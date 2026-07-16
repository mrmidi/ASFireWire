// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "CMPClient.hpp"

#include "../../../Logging/Logging.hpp"

#include <array>
#include <cstring>
#include <utility>

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
    if (!device.IsValid() || plugNum > kMaxPlugNumber || channel > 63 || !BeginConnect(key, device, channel)) {
        callback(CMPStatus::Failed);
        return;
    }
    ReadMPR(device, PCRDirection::kOutput, plugNum,
            [this, key, device, channel, callback = std::move(callback)](CMPStatus status, FW::FwSpeed speed) mutable {
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
    if (!device.IsValid() || plugNum > kMaxPlugNumber || channel > 63 || !BeginConnect(key, device, channel)) {
        callback(CMPStatus::Failed);
        return;
    }
    ReadMPR(device, PCRDirection::kInput, plugNum,
            [this, key, device, channel, callback = std::move(callback)](CMPStatus status, FW::FwSpeed speed) mutable {
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
    if (!device.IsValid() || plugNum > kMaxPlugNumber || !BeginDisconnect(key, device, lease)) {
        // No local lease means this client never established the remote p2p
        // count. Treat BREAK as idempotent without touching a foreign stream.
        callback(CMPStatus::Success);
        return;
    }
    AttemptDisconnect(key, lease, 0, std::move(callback));
}

void CMPClient::DisconnectIPCR(const CMPDevice& device, uint8_t plugNum, CMPCallback callback) {
    const LeaseKey key{device.guid, PCRDirection::kInput, plugNum};
    Lease lease{};
    if (!device.IsValid() || plugNum > kMaxPlugNumber || !BeginDisconnect(key, device, lease)) {
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
    busOps_.ReadQuad(device.generation, device.nodeId, target, speed,
                      [callback = std::move(callback)](Async::AsyncStatus status,
                                                       std::span<const uint8_t> payload) mutable {
        if (status != Async::AsyncStatus::kSuccess || payload.size() != sizeof(uint32_t)) {
            callback(false, 0);
            return;
        }
        uint32_t raw = 0;
        std::memcpy(&raw, payload.data(), sizeof(raw));
        callback(true, OSSwapBigToHostInt32(raw));
    });
}

void CMPClient::CompareSwap(const CMPDevice& device, uint32_t address, uint32_t expected,
                            uint32_t desired, FW::FwSpeed speed, CompareSwapCallback callback) {
    const Async::FWAddress target{Async::FWAddress::AddressParts{
        .addressHi = PCRRegisters::kAddressHi,
        .addressLo = address,
    }};
    std::array<uint8_t, 8> operand{};
    const uint32_t expectedBE = OSSwapHostToBigInt32(expected);
    const uint32_t desiredBE = OSSwapHostToBigInt32(desired);
    std::memcpy(operand.data(), &expectedBE, sizeof(expectedBE));
    std::memcpy(operand.data() + sizeof(expectedBE), &desiredBE, sizeof(desiredBE));

    busOps_.Lock(device.generation, device.nodeId, target, FW::LockOp::kCompareSwap,
                 operand, sizeof(uint32_t), speed,
                 [callback = std::move(callback)](Async::AsyncStatus status,
                                                  std::span<const uint8_t> payload) mutable {
        if (status != Async::AsyncStatus::kSuccess) {
            callback(MapAsyncStatus(status), 0);
            return;
        }
        if (payload.size() != sizeof(uint32_t)) {
            callback(CMPStatus::Failed, 0);
            return;
        }
        uint32_t raw = 0;
        std::memcpy(&raw, payload.data(), sizeof(raw));
        callback(CMPStatus::Success, OSSwapBigToHostInt32(raw));
    });
}

void CMPClient::ReadMPR(const CMPDevice& device, PCRDirection direction, uint8_t plugNum,
                        std::function<void(CMPStatus, FW::FwSpeed)> callback) {
    const FW::FwSpeed routeSpeed = busInfo_.GetSpeed(device.nodeId);
    ReadQuadlet(device, MPRAddress(direction), routeSpeed,
                [routeSpeed, plugNum, callback = std::move(callback)](bool success, uint32_t mpr) mutable {
        if (!success) {
            callback(CMPStatus::Failed, FW::FwSpeed::S100);
            return;
        }
        const uint8_t plugCount = static_cast<uint8_t>(mpr & 0x1FU);
        if (plugNum >= plugCount) {
            callback(CMPStatus::NotFound, FW::FwSpeed::S100);
            return;
        }
        const auto mprSpeed = static_cast<FW::FwSpeed>((mpr >> 30U) & 0x03U);
        callback(CMPStatus::Success, MinSpeed(routeSpeed, mprSpeed));
    });
}

void CMPClient::AttemptConnect(const LeaseKey& key, const CMPDevice& device, uint8_t channel,
                               FW::FwSpeed speed, uint8_t attempt, CMPCallback callback) {
    ReadQuadlet(device, PCRAddress(key.direction, key.plugNum), speed,
                [this, key, device, channel, speed, attempt, callback = std::move(callback)]
                (bool success, uint32_t current) mutable {
        if (!success) {
            CompleteConnect(key, device, channel, CMPStatus::Failed, std::move(callback));
            return;
        }
        if (!PCRBits::IsOnline(current)) {
            CompleteConnect(key, device, channel, CMPStatus::NotFound, std::move(callback));
            return;
        }
        // Establish is exclusive. Sharing a p2p count is not safe without a
        // common lease owner and would let BREAK tear down another host stream.
        if (PCRBits::IsInUse(current)) {
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
        CompareSwap(device, PCRAddress(key.direction, key.plugNum), current, desired, speed,
                    [this, key, device, channel, speed, attempt, current, callback = std::move(callback)]
                    (CMPStatus status, uint32_t observed) mutable {
            if (status != CMPStatus::Success) {
                CompleteConnect(key, device, channel, status, std::move(callback));
                return;
            }
            if (observed == current) {
                CompleteConnect(key, device, channel, CMPStatus::Success, std::move(callback));
                return;
            }
            if (attempt + 1U >= kMaxCompareSwapAttempts) {
                CompleteConnect(key, device, channel, CMPStatus::NoResources, std::move(callback));
                return;
            }
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
