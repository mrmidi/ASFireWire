// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "CMPClient.hpp"

#include "../../../Logging/Logging.hpp"

#include <array>
#include <cstring>
#include <utility>

// !!! DIAGNOSTIC: CMP connection-path tracing. Remove after FW-94 resolution.
// Every async stage logs SUBMIT (request issued to bus ops) / DONE (callback fired)
// with hex wire values so the trace aligns 1:1 with a FireBug capture.
#define CMPTRACE(fmt, ...) \
    ASFW_LOG(Audio, "!!! [CMP] " fmt, ##__VA_ARGS__)

namespace ASFW::CMP {

namespace {

[[nodiscard]] FW::FwSpeed MinSpeed(FW::FwSpeed lhs, FW::FwSpeed rhs) noexcept {
    return static_cast<uint8_t>(lhs) < static_cast<uint8_t>(rhs) ? lhs : rhs;
}

} // namespace

CMPClient::CMPClient(Async::IFireWireBusOps& busOps, Async::IFireWireBusInfo& busInfo)
    : busOps_(busOps), busInfo_(busInfo), lock_(IOLockAlloc()) {}

CMPClient::~CMPClient() {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void CMPClient::ReadOPCR(const CMPDevice& device, uint8_t plugNum, PCRReadCallback callback) {
    if (!device.IsValid() || plugNum > kMaxPlugNumber) {
        callback(false, 0);
        return;
    }
    ReadQuadlet(device, PCRAddress(PCRDirection::kOutput, plugNum),
                busInfo_.GetSpeed(device.nodeId), std::move(callback));
}

void CMPClient::ReadIPCR(const CMPDevice& device, uint8_t plugNum, PCRReadCallback callback) {
    if (!device.IsValid() || plugNum > kMaxPlugNumber) {
        callback(false, 0);
        return;
    }
    ReadQuadlet(device, PCRAddress(PCRDirection::kInput, plugNum),
                busInfo_.GetSpeed(device.nodeId), std::move(callback));
}

void CMPClient::ConnectOPCR(const CMPDevice& device, uint8_t plugNum, uint8_t channel,
                            CMPCallback callback) {
    const LeaseKey key{device.guid, PCRDirection::kOutput, plugNum};
    CMPTRACE("ConnectOPCR entry: guid=0x%016llx node=%u gen=%u plug=%u ch=%u",
             device.guid, device.nodeId.value, device.generation.value, plugNum, channel);
    if (!device.IsValid() || plugNum > kMaxPlugNumber || channel > 63 || !BeginConnect(key, device, channel)) {
        CMPTRACE("ConnectOPCR: rejected invalid args or BeginConnect failed");
        callback(CMPStatus::Failed);
        return;
    }
    ReadMPR(device, PCRDirection::kOutput, plugNum,
            [this, key, device, channel, callback = std::move(callback)](CMPStatus status, FW::FwSpeed speed) mutable {
                CMPTRACE("ConnectOPCR ReadMPR done: status=%u speed=%u", status, speed);
                if (status != CMPStatus::Success) {
                    CompleteConnect(key, device, channel, status, std::move(callback));
                    return;
                }
                AttemptConnect(key, device, channel, speed, 0, std::move(callback));
            });
}

void CMPClient::ConnectIPCR(const CMPDevice& device, uint8_t plugNum, uint8_t channel,
                            CMPCallback callback) {
    const LeaseKey key{device.guid, PCRDirection::kInput, plugNum};
    CMPTRACE("ConnectIPCR entry: guid=0x%016llx node=%u gen=%u plug=%u ch=%u",
             device.guid, device.nodeId.value, device.generation.value, plugNum, channel);
    if (!device.IsValid() || plugNum > kMaxPlugNumber || channel > 63 || !BeginConnect(key, device, channel)) {
        CMPTRACE("ConnectIPCR: rejected invalid args or BeginConnect failed");
        callback(CMPStatus::Failed);
        return;
    }
    ReadMPR(device, PCRDirection::kInput, plugNum,
            [this, key, device, channel, callback = std::move(callback)](CMPStatus status, FW::FwSpeed speed) mutable {
                CMPTRACE("ConnectIPCR ReadMPR done: status=%u speed=%u", status, speed);
                if (status != CMPStatus::Success) {
                    CompleteConnect(key, device, channel, status, std::move(callback));
                    return;
                }
                AttemptConnect(key, device, channel, speed, 0, std::move(callback));
            });
}

void CMPClient::DisconnectOPCR(const CMPDevice& device, uint8_t plugNum, CMPCallback callback) {
    const LeaseKey key{device.guid, PCRDirection::kOutput, plugNum};
    Lease lease{};
    CMPTRACE("DisconnectOPCR entry: guid=0x%016llx node=%u gen=%u plug=%u",
             device.guid, device.nodeId.value, device.generation.value, plugNum);
    if (!device.IsValid() || plugNum > kMaxPlugNumber || !BeginDisconnect(key, device, lease)) {
        // No local lease means this client never established the remote p2p
        // count. Treat BREAK as idempotent without touching a foreign stream.
        CMPTRACE("DisconnectOPCR: no local lease, returning Success");
        callback(CMPStatus::Success);
        return;
    }
    AttemptDisconnect(key, lease, 0, std::move(callback));
}

void CMPClient::DisconnectIPCR(const CMPDevice& device, uint8_t plugNum, CMPCallback callback) {
    const LeaseKey key{device.guid, PCRDirection::kInput, plugNum};
    Lease lease{};
    CMPTRACE("DisconnectIPCR entry: guid=0x%016llx node=%u gen=%u plug=%u",
             device.guid, device.nodeId.value, device.generation.value, plugNum);
    if (!device.IsValid() || plugNum > kMaxPlugNumber || !BeginDisconnect(key, device, lease)) {
        CMPTRACE("DisconnectIPCR: no local lease, returning Success");
        callback(CMPStatus::Success);
        return;
    }
    AttemptDisconnect(key, lease, 0, std::move(callback));
}

void CMPClient::InvalidateDevice(uint64_t guid) {
    if (!lock_ || guid == 0) {
        return;
    }
    IOLockLock(lock_);
    for (auto it = leases_.begin(); it != leases_.end();) {
        if (it->first.guid == guid) {
            it = leases_.erase(it);
        } else {
            ++it;
        }
    }
    IOLockUnlock(lock_);
}

void CMPClient::ReadQuadlet(const CMPDevice& device, uint32_t address, FW::FwSpeed speed,
                            PCRReadCallback callback) {
    const Async::FWAddress target{Async::FWAddress::AddressParts{
        .addressHi = PCRRegisters::kAddressHi,
        .addressLo = address,
    }};
    CMPTRACE("ReadQuadlet SUBMIT: gen=%u node=%u addr=0x%08x (%{public}s)",
             device.generation.value, device.nodeId.value, address,
             address == PCRAddress(PCRDirection::kInput, 0) ? "iPCR0" :
             address == PCRAddress(PCRDirection::kOutput, 0) ? "oPCR0" : "other");
    busOps_.ReadQuad(device.generation, device.nodeId, target, speed,
                      [callback = std::move(callback), address](Async::AsyncStatus status,
                                                       std::span<const uint8_t> payload) mutable {
        if (status != Async::AsyncStatus::kSuccess || payload.size() != sizeof(uint32_t)) {
            CMPTRACE("ReadQuadlet DONE: addr=0x%08x status=%u (FAIL, payload=%zu)",
                     address, status, payload.size());
            callback(false, 0);
            return;
        }
        uint32_t raw = 0;
        __builtin_memcpy(&raw, payload.data(), sizeof(raw));
        const uint32_t value = OSSwapBigToHostInt32(raw);
        CMPTRACE("ReadQuadlet DONE: addr=0x%08x value=0x%08x", address, value);
        callback(true, value);
    });
}

void CMPClient::CompareSwap(const CMPDevice& device, uint32_t address, uint32_t expected,
                            uint32_t desired, FW::FwSpeed speed, CompareSwapCallback callback) {
    const Async::FWAddress target{Async::FWAddress::AddressParts{
        .addressHi = PCRRegisters::kAddressHi,
        .addressLo = address,
    }};
    alignas(uint32_t) std::array<uint8_t, 8> operand{};
    const uint32_t expectedBE = OSSwapHostToBigInt32(expected);
    const uint32_t desiredBE = OSSwapHostToBigInt32(desired);
    __builtin_memcpy(operand.data(), &expectedBE, sizeof(expectedBE));
    __builtin_memcpy(operand.data() + sizeof(expectedBE), &desiredBE, sizeof(desiredBE));

    CMPTRACE("CompareSwap SUBMIT: gen=%u node=%u addr=0x%08x expected=0x%08x desired=0x%08x",
             device.generation.value, device.nodeId.value, address, expected, desired);
    busOps_.Lock(device.generation, device.nodeId, target, FW::LockOp::kCompareSwap,
                 operand, sizeof(uint32_t), speed,
                 [callback = std::move(callback), address, expected, desired](Async::AsyncStatus status,
                                                  std::span<const uint8_t> payload) mutable {
        if (status != Async::AsyncStatus::kSuccess) {
            CMPTRACE("CompareSwap DONE: addr=0x%08x status=%u (async FAIL)", address, status);
            callback(MapAsyncStatus(status), 0);
            return;
        }
        if (payload.size() != sizeof(uint32_t)) {
            CMPTRACE("CompareSwap DONE: addr=0x%08x payload=%zu (SIZE FAIL)", address, payload.size());
            callback(CMPStatus::Failed, 0);
            return;
        }
        uint32_t raw = 0;
        std::memcpy(&raw, payload.data(), sizeof(raw));
        const uint32_t observed = OSSwapBigToHostInt32(raw);
        CMPTRACE("CompareSwap DONE: addr=0x%08x expected=0x%08x desired=0x%08x observed=0x%08x (%{public}s)",
                 address, expected, desired, observed,
                 observed == expected ? "MATCH" : "MISMATCH");
        callback(CMPStatus::Success, observed);
    });
}

void CMPClient::ReadMPR(const CMPDevice& device, PCRDirection direction, uint8_t plugNum,
                        std::function<void(CMPStatus, FW::FwSpeed)> callback) {
    const FW::FwSpeed routeSpeed = busInfo_.GetSpeed(device.nodeId);
    const uint32_t mprAddr = MPRAddress(direction);
    CMPTRACE("ReadMPR SUBMIT: dir=%{public}s plug=%u addr=0x%08x",
             direction == PCRDirection::kInput ? "Input" : "Output", plugNum, mprAddr);
    ReadQuadlet(device, mprAddr, routeSpeed,
                [routeSpeed, plugNum, direction, callback = std::move(callback)](bool success, uint32_t mpr) mutable {
        if (!success) {
            CMPTRACE("ReadMPR DONE: dir=%{public}s plug=%u FAIL",
                     direction == PCRDirection::kInput ? "Input" : "Output", plugNum);
            callback(CMPStatus::Failed, FW::FwSpeed::S100);
            return;
        }
        const uint8_t plugCount = static_cast<uint8_t>(mpr & 0x1FU);
        if (plugNum >= plugCount) {
            CMPTRACE("ReadMPR DONE: dir=%{public}s plug=%u NotFound (count=%u)",
                     direction == PCRDirection::kInput ? "Input" : "Output", plugNum, plugCount);
            callback(CMPStatus::NotFound, FW::FwSpeed::S100);
            return;
        }
        const auto mprSpeed = static_cast<FW::FwSpeed>((mpr >> 30U) & 0x03U);
        CMPTRACE("ReadMPR DONE: dir=%{public}s plug=%u OK speed=%u plugCount=%u",
                 direction == PCRDirection::kInput ? "Input" : "Output", plugNum, mprSpeed, plugCount);
        callback(CMPStatus::Success, MinSpeed(routeSpeed, mprSpeed));
    });
}

void CMPClient::AttemptConnect(const LeaseKey& key, const CMPDevice& device, uint8_t channel,
                               FW::FwSpeed speed, uint8_t attempt, CMPCallback callback) {
    CMPTRACE("AttemptConnect entry: dir=%{public}s plug=%u ch=%u attempt=%u",
             key.direction == PCRDirection::kInput ? "Input" : "Output",
             key.plugNum, channel, attempt);
    ReadQuadlet(device, PCRAddress(key.direction, key.plugNum), speed,
                [this, key, device, channel, speed, attempt, callback = std::move(callback)]
                (bool success, uint32_t current) mutable {
        if (!success) {
            CMPTRACE("AttemptConnect: readPCR failed");
            CompleteConnect(key, device, channel, CMPStatus::Failed, std::move(callback));
            return;
        }
        if (!PCRBits::IsOnline(current)) {
            CMPTRACE("AttemptConnect: PCR not online (0x%08x)", current);
            CompleteConnect(key, device, channel, CMPStatus::NotFound, std::move(callback));
            return;
        }
        // Establish is exclusive. Sharing a p2p count is not safe without a
        // common lease owner and would let BREAK tear down another host stream.
        if (PCRBits::IsInUse(current)) {
            CMPTRACE("AttemptConnect: PCR in use (0x%08x, p2p=%u bcast=%u)", current,
                     PCRBits::GetP2P(current), (current & PCRBits::kBcastMask) != 0);
            CompleteConnect(key, device, channel, CMPStatus::NoResources, std::move(callback));
            return;
        }

        uint32_t desired = PCRBits::SetChannel(PCRBits::SetP2P(current, 1), channel);
        if (key.direction == PCRDirection::kOutput) {
            // Cross-validated with Linux sound/firewire/cmp.c:231-273 and
            // iso-resources.c:64-79: oPCR carries speed and overhead ID.
            desired = PCRBits::SetDataRate(desired, speed);
            desired = PCRBits::SetOverheadId(desired, OverheadIdForGapCount(busInfo_.GetGapCount()));
        }
        CMPTRACE("AttemptConnect: PCR=0x%08x → desired=0x%08x (p2p=1, ch=%u)", current, desired, channel);
        CompareSwap(device, PCRAddress(key.direction, key.plugNum), current, desired, speed,
                    [this, key, device, channel, speed, attempt, current, callback = std::move(callback)]
                    (CMPStatus status, uint32_t observed) mutable {
            if (status != CMPStatus::Success) {
                CMPTRACE("AttemptConnect: CAS failed status=%u", status);
                CompleteConnect(key, device, channel, status, std::move(callback));
                return;
            }
            if (observed == current) {
                CMPTRACE("AttemptConnect: CAS matched, connect SUCCESS");
                CompleteConnect(key, device, channel, CMPStatus::Success, std::move(callback));
                return;
            }
            if (attempt + 1U >= kMaxCompareSwapAttempts) {
                CMPTRACE("AttemptConnect: CAS mismatch, max retries (%u) reached", attempt);
                CompleteConnect(key, device, channel, CMPStatus::NoResources, std::move(callback));
                return;
            }
            CMPTRACE("AttemptConnect: CAS mismatch (0x%08x vs 0x%08x), retry %u",
                     observed, current, attempt + 1U);
            AttemptConnect(key, device, channel, speed, attempt + 1U, std::move(callback));
        });
    });
}

void CMPClient::AttemptDisconnect(const LeaseKey& key, const Lease& lease, uint8_t attempt,
                                  CMPCallback callback) {
    const FW::FwSpeed speed = busInfo_.GetSpeed(lease.device.nodeId);
    ReadQuadlet(lease.device, PCRAddress(key.direction, key.plugNum), speed,
                [this, key, lease, speed, attempt, callback = std::move(callback)]
                (bool success, uint32_t current) mutable {
        if (!success) {
            CompleteDisconnect(key, CMPStatus::Failed, std::move(callback));
            return;
        }
        // Never decrement a PCR unless it still describes the exclusive lease
        // that this client established in this generation.
        if (PCRBits::GetP2P(current) != 1 || PCRBits::GetChannel(current) != lease.channel ||
            (current & PCRBits::kBcastMask) != 0) {
            CompleteDisconnect(key, CMPStatus::Failed, std::move(callback));
            return;
        }
        const uint32_t desired = PCRBits::SetP2P(current, 0);
        CompareSwap(lease.device, PCRAddress(key.direction, key.plugNum), current, desired, speed,
                    [this, key, lease, attempt, current, callback = std::move(callback)]
                    (CMPStatus status, uint32_t observed) mutable {
            if (status != CMPStatus::Success) {
                CompleteDisconnect(key, status, std::move(callback));
                return;
            }
            if (observed == current) {
                CompleteDisconnect(key, CMPStatus::Success, std::move(callback));
                return;
            }
            if (attempt + 1U >= kMaxCompareSwapAttempts) {
                CompleteDisconnect(key, CMPStatus::Failed, std::move(callback));
                return;
            }
            AttemptDisconnect(key, lease, attempt + 1U, std::move(callback));
        });
    });
}

bool CMPClient::BeginConnect(const LeaseKey& key, const CMPDevice& device, uint8_t channel) {
    if (!lock_) {
        return false;
    }
    IOLockLock(lock_);
    const auto it = leases_.find(key);
    if (it != leases_.end()) {
        if (it->second.device.generation != device.generation || it->second.device.nodeId != device.nodeId) {
            leases_.erase(it);
        } else {
            IOLockUnlock(lock_);
            return false;
        }
    }
    leases_.emplace(key, Lease{device, channel, LeaseState::kConnecting});
    IOLockUnlock(lock_);
    return true;
}

bool CMPClient::BeginDisconnect(const LeaseKey& key, const CMPDevice& device, Lease& outLease) {
    if (!lock_) {
        return false;
    }
    IOLockLock(lock_);
    const auto it = leases_.find(key);
    if (it == leases_.end() || it->second.state != LeaseState::kConnected) {
        IOLockUnlock(lock_);
        return false;
    }
    if (it->second.device.generation != device.generation || it->second.device.nodeId != device.nodeId) {
        leases_.erase(it);
        IOLockUnlock(lock_);
        return false;
    }
    it->second.state = LeaseState::kDisconnecting;
    outLease = it->second;
    IOLockUnlock(lock_);
    return true;
}

void CMPClient::CompleteConnect(const LeaseKey& key, const CMPDevice& device, uint8_t channel,
                                CMPStatus status, CMPCallback callback) {
    if (lock_) {
        IOLockLock(lock_);
        const auto it = leases_.find(key);
        if (it != leases_.end() && it->second.device.generation == device.generation &&
            it->second.device.nodeId == device.nodeId && it->second.channel == channel) {
            if (status == CMPStatus::Success) {
                it->second.state = LeaseState::kConnected;
            } else {
                leases_.erase(it);
            }
        }
        IOLockUnlock(lock_);
    }
    CMPTRACE("CompleteConnect: status=%u lease=%{public}s dir=%{public}s plug=%u ch=%u",
             status, lock_ ? "found" : "no-lock",
             key.direction == PCRDirection::kInput ? "Input" : "Output",
             key.plugNum, channel);
    callback(status);
}

void CMPClient::CompleteDisconnect(const LeaseKey& key, CMPStatus status, CMPCallback callback) {
    if (lock_) {
        IOLockLock(lock_);
        const auto it = leases_.find(key);
        if (it != leases_.end()) {
            if (status == CMPStatus::Success) {
                leases_.erase(it);
            } else {
                it->second.state = LeaseState::kConnected;
            }
        }
        IOLockUnlock(lock_);
    }
    callback(status);
}

void CMPClient::BreakBothConnections(const CMPDevice& device, uint8_t plugNum, CMPCallback callback) {
    if (!device.IsValid() || plugNum > kMaxPlugNumber) {
        callback(CMPStatus::Failed);
        return;
    }
    CMPTRACE("BreakBothConnections SUBMIT: plug=%u (will clear iPCR then oPCR)", plugNum);
    DisconnectIPCR(device, plugNum, [this, device, plugNum, callback](CMPStatus ipcrStatus) {
        CMPTRACE("BreakBothConnections: iPCR clear status=%u", ipcrStatus);
        DisconnectOPCR(device, plugNum, [ipcrStatus, callback](CMPStatus opcrStatus) {
            CMPTRACE("BreakBothConnections: oPCR clear status=%u", opcrStatus);
            if (ipcrStatus == CMPStatus::Success && opcrStatus == CMPStatus::Success) {
                callback(CMPStatus::Success);
            } else if (ipcrStatus != CMPStatus::Success) {
                callback(ipcrStatus);
            } else {
                callback(opcrStatus);
            }
        });
    });
}

void CMPClient::CheckPlugUsed(const CMPDevice& device, PCRDirection dir, uint8_t plugNum,
                             PCRBoolCallback callback) {
    if (!device.IsValid() || plugNum > kMaxPlugNumber) {
        callback(false, false);
        return;
    }
    const FW::FwSpeed speed = busInfo_.GetSpeed(device.nodeId);
    CMPTRACE("CheckPlugUsed SUBMIT: dir=%{public}s plug=%u",
             dir == PCRDirection::kInput ? "Input" : "Output", plugNum);
    ReadQuadlet(device, PCRAddress(dir, plugNum), speed,
                [callback = std::move(callback), dir, plugNum](bool success, uint32_t current) {
        if (!success) {
            CMPTRACE("CheckPlugUsed DONE: dir=%{public}s plug=%u FAIL", dir == PCRDirection::kInput ? "Input" : "Output", plugNum);
            callback(false, false);
        } else {
            CMPTRACE("CheckPlugUsed DONE: dir=%{public}s plug=%u value=0x%08x used=%u",
                     dir == PCRDirection::kInput ? "Input" : "Output", plugNum, current,
                     PCRBits::IsInUse(current));
            callback(true, PCRBits::IsInUse(current));
        }
    });
}

CMPStatus CMPClient::MapAsyncStatus(Async::AsyncStatus status) noexcept {
    switch (status) {
        case Async::AsyncStatus::kSuccess:
            return CMPStatus::Success;
        case Async::AsyncStatus::kTimeout:
            return CMPStatus::Timeout;
        case Async::AsyncStatus::kStaleGeneration:
            return CMPStatus::GenerationMismatch;
        default:
            return CMPStatus::Failed;
    }
}

uint32_t CMPClient::PCRAddress(PCRDirection direction, uint8_t plugNum) noexcept {
    return direction == PCRDirection::kOutput ? PCRRegisters::GetOPCRAddress(plugNum)
                                              : PCRRegisters::GetIPCRAddress(plugNum);
}

uint32_t CMPClient::MPRAddress(PCRDirection direction) noexcept {
    return direction == PCRDirection::kOutput ? PCRRegisters::kOMPR : PCRRegisters::kIMPR;
}

uint8_t CMPClient::OverheadIdForGapCount(uint8_t gapCount) noexcept {
    // Linux derives allocation overhead from the live gap count. The
    // unoptimised fallback (63) maps to 512 units and thus overhead ID 0.
    const uint32_t overhead = gapCount < 63U ? (static_cast<uint32_t>(gapCount) * 97U) / 10U + 89U
                                             : 512U;
    for (uint8_t id = 1; id < 16U; ++id) {
        if (overhead < (static_cast<uint32_t>(id) << 5U)) {
            return id;
        }
    }
    return 0;
}

} // namespace ASFW::CMP
