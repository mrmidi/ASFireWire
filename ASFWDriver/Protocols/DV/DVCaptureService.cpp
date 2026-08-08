#include "DVCaptureService.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#include "../../Bus/IRM/IRMClient.hpp"
#include "../../Discovery/DeviceRegistry.hpp"
#include "../../Isoch/IsochService.hpp"
#include "../../Logging/Logging.hpp"
#include "../AVC/CMP/CMPClient.hpp"

namespace ASFW::Protocols::DV {
namespace {

constexpr uint32_t kOperationTimeoutMs = 750;

class LockGuard final {
public:
    explicit LockGuard(IOLock* lock) noexcept : lock_(lock) {
        if (lock_) IOLockLock(lock_);
    }
    ~LockGuard() {
        if (lock_) IOLockUnlock(lock_);
    }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    IOLock* lock_{nullptr};
};

template <typename State>
[[nodiscard]] bool WaitUntilDone(const std::shared_ptr<State>& state,
                                 uint32_t timeoutMs = kOperationTimeoutMs) noexcept {
    constexpr uint32_t kPollMs = 5;
    for (uint32_t waited = 0; waited < timeoutMs; waited += kPollMs) {
        if (state->done.load(std::memory_order_acquire)) return true;
        IOSleep(kPollMs);
    }
    return state->done.load(std::memory_order_acquire);
}

template <typename Start>
[[nodiscard]] std::optional<uint32_t> AwaitPCRRead(Start&& start) {
    struct State {
        std::atomic<bool> done{false};
        std::atomic<bool> success{false};
        std::atomic<uint32_t> value{0};
    };
    auto state = std::make_shared<State>();
    start([state](bool success, uint32_t value) {
        state->value.store(value, std::memory_order_relaxed);
        state->success.store(success, std::memory_order_relaxed);
        state->done.store(true, std::memory_order_release);
    });
    if (!WaitUntilDone(state) ||
        !state->success.load(std::memory_order_relaxed)) {
        return std::nullopt;
    }
    return state->value.load(std::memory_order_relaxed);
}

template <typename Start>
[[nodiscard]] IRM::AllocationStatus AwaitAllocation(Start&& start) {
    struct State {
        std::atomic<bool> done{false};
        std::atomic<IRM::AllocationStatus> status{IRM::AllocationStatus::Failed};
    };
    auto state = std::make_shared<State>();
    start([state](IRM::AllocationStatus status) {
        state->status.store(status, std::memory_order_relaxed);
        state->done.store(true, std::memory_order_release);
    });
    if (!WaitUntilDone(state)) return IRM::AllocationStatus::Timeout;
    return state->status.load(std::memory_order_relaxed);
}

[[nodiscard]] std::optional<IRM::ResourceSnapshot>
AwaitResourceSnapshot(IRM::IRMClient& irm) {
    struct State {
        std::atomic<bool> done{false};
        std::atomic<IRM::AllocationStatus> status{IRM::AllocationStatus::Failed};
        IRM::ResourceSnapshot snapshot{};
    };
    auto state = std::make_shared<State>();
    irm.ReadResourcesSnapshot(
        [state](IRM::AllocationStatus status, IRM::ResourceSnapshot snapshot) {
            state->snapshot = snapshot;
            state->status.store(status, std::memory_order_relaxed);
            state->done.store(true, std::memory_order_release);
        });
    if (!WaitUntilDone(state) ||
        state->status.load(std::memory_order_relaxed) !=
            IRM::AllocationStatus::Success) {
        return std::nullopt;
    }
    return state->snapshot;
}

[[nodiscard]] kern_return_t ToIOReturn(IRM::AllocationStatus status) noexcept {
    switch (status) {
    case IRM::AllocationStatus::Success: return kIOReturnSuccess;
    case IRM::AllocationStatus::NoResources: return kIOReturnNoResources;
    case IRM::AllocationStatus::GenerationMismatch: return kIOReturnAborted;
    case IRM::AllocationStatus::Timeout: return kIOReturnTimeout;
    case IRM::AllocationStatus::NotFound: return kIOReturnNotFound;
    case IRM::AllocationStatus::Failed: return kIOReturnError;
    }
    return kIOReturnError;
}

struct PlugSelection {
    uint8_t plug{0xFF};
    uint32_t pcr{0};
};

} // namespace

DVCaptureService::DVCaptureService() : lock_(IOLockAlloc()) {}

DVCaptureService::~DVCaptureService() {
    // ServiceContext quiesces the receive context before destruction.
    ResetState();
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

kern_return_t DVCaptureService::Start(
    uint64_t deviceGuid,
    uint64_t ownerToken,
    Driver::IsochService& isoch,
    Driver::HardwareInterface& hardware,
    Discovery::DeviceRegistry& registry,
    IRM::IRMClient& irm,
    CMP::CMPClient& cmp) {
    if (!lock_) return kIOReturnNoResources;
    if (ownerToken == 0 || deviceGuid == 0) return kIOReturnBadArgument;

    LockGuard guard(lock_);
    if (active_) {
        return ownerToken_ == ownerToken && route_.guid == deviceGuid
                   ? kIOReturnSuccess
                   : kIOReturnExclusiveAccess;
    }

    if (isoch.ReceiveContext() &&
        isoch.ReceiveContext()->GetState() !=
            ASFW::Isoch::IRPolicy::State::Stopped) {
        return kIOReturnBusy;
    }

    const auto record = registry.SnapshotByGuid(deviceGuid);
    if (!record || record->state == Discovery::LifeState::Lost ||
        record->state == Discovery::LifeState::Quarantined) {
        return kIOReturnNotFound;
    }
    const auto node = Discovery::TryOperationalNodeId(record->nodeId);
    if (!node) return kIOReturnNotReady;
    const auto route = registry.CurrentRoute(deviceGuid);
    if (!route || !*route) return kIOReturnNotReady;
    const CMP::CMPDevice cmpDevice{.route = *route};

    ConnectionMode mode = ConnectionMode::BroadcastFallback;
    uint8_t channel = 63;
    uint8_t outputPlug = 0xFF;
    uint32_t bandwidthUnits = 0;
    bool allocatedResources = false;
    bool shouldConnectPCR = false;

    const auto ompr = AwaitPCRRead(
        [&cmp, cmpDevice](CMP::PCRReadCallback callback) {
            cmp.ReadOMPR(cmpDevice, std::move(callback));
        });
    if (ompr) {
        channel = CMP::MPRBits::GetBroadcastChannel(*ompr);
        std::optional<PlugSelection> firstOnline;
        std::optional<PlugSelection> firstFree;
        const uint8_t plugCount = CMP::MPRBits::GetPlugCount(*ompr);
        for (uint8_t plug = 0; plug < plugCount; ++plug) {
            const auto pcr = AwaitPCRRead(
                [&cmp, cmpDevice, plug](CMP::PCRReadCallback callback) {
                    cmp.ReadOPCR(cmpDevice, plug, std::move(callback));
                });
            if (!pcr || !CMP::PCRBits::IsOnline(*pcr)) continue;
            if (!firstOnline) firstOnline = PlugSelection{plug, *pcr};
            if (CMP::PCRBits::GetP2P(*pcr) == 0) {
                firstFree = PlugSelection{plug, *pcr};
                break;
            }
        }

        const auto selected = firstFree ? firstFree : firstOnline;
        if (selected) {
            outputPlug = selected->plug;
            if (CMP::PCRBits::IsBroadcast(selected->pcr)) {
                mode = ConnectionMode::Broadcast;
                channel = CMP::PCRBits::GetChannel(selected->pcr);
            } else if (CMP::PCRBits::GetP2P(selected->pcr) > 0) {
                mode = ConnectionMode::Overlay;
                channel = CMP::PCRBits::GetChannel(selected->pcr);
                // Observe an existing stream without modifying its connection
                // count. We do not own that lease and therefore must never
                // issue a matching BREAK on disconnect.
                shouldConnectPCR = false;
            } else if (const auto snapshot = AwaitResourceSnapshot(irm)) {
                if (const auto availableChannel = FirstAvailableChannel(*snapshot)) {
                    const uint8_t effectiveSpeed = static_cast<uint8_t>(
                        std::min({CMP::MPRBits::GetDataRate(*ompr),
                                  CMP::PCRBits::GetDataRate(selected->pcr),
                                  static_cast<uint8_t>(record->link.localToNode),
                                  uint8_t{2}}));
                    if (const auto units =
                            CalculateBandwidthUnits(selected->pcr,
                                                    effectiveSpeed)) {
                        const auto status = AwaitAllocation(
                            [&irm, availableChannel, units](IRM::AllocationCallback callback) {
                                irm.AllocateResources(*availableChannel, *units,
                                                      std::move(callback));
                            });
                        if (status == IRM::AllocationStatus::Success) {
                            mode = ConnectionMode::PointToPoint;
                            channel = *availableChannel;
                            bandwidthUnits = *units;
                            allocatedResources = true;
                            shouldConnectPCR = true;
                        } else {
                            ASFW_LOG_WARNING(
                                Isoch,
                                "[DV] IRM allocation failed status=%{public}s; using broadcast channel %u",
                                IRM::ToString(status), channel);
                        }
                    }
                }
            }
        }
    } else {
        ASFW_LOG_WARNING(Isoch,
                         "[DV] oMPR unavailable for GUID=0x%llx; using broadcast channel 63",
                         deviceGuid);
    }

    irm_ = &irm;
    cmp_ = &cmp;
    route_ = *route;
    channel_ = channel;
    outputPlug_ = outputPlug;
    bandwidthUnits_ = bandwidthUnits;
    connectionMode_ = mode;
    resourcesAllocated_ = allocatedResources;

    constexpr uint32_t kRingRecords = 8192;
    const uint64_t ringBytes = DVCaptureSink::RequiredBytes(kRingRecords);

    IOBufferMemoryDescriptor* rawMemory = nullptr;
    kern_return_t kr = IOBufferMemoryDescriptor::Create(
        kIOMemoryDirectionOutIn, ringBytes, 64, &rawMemory);
    if (kr != kIOReturnSuccess || !rawMemory) {
        (void)TeardownConnection(false);
        ResetState();
        return kr != kIOReturnSuccess ? kr : kIOReturnNoMemory;
    }
    kr = rawMemory->SetLength(ringBytes);
    if (kr != kIOReturnSuccess) {
        rawMemory->release();
        (void)TeardownConnection(false);
        ResetState();
        return kr;
    }

    ring_.memory = Common::AdoptRetained(rawMemory);
    ring_.bytes = ringBytes;
    kr = Common::CreateSharedMapping(ring_.memory, ring_.map);
    if (kr != kIOReturnSuccess) {
        (void)TeardownConnection(false);
        ResetState();
        return kr;
    }
    if (!sink_.InitializeAndAttach(
            ring_.BaseAddress(), ringBytes, kRingRecords)) {
        (void)TeardownConnection(false);
        ResetState();
        return kIOReturnInternalError;
    }

    kr = isoch.StartPacketReceive(channel, hardware, &sink_);
    if (kr != kIOReturnSuccess) {
        sink_.MarkTerminal(CaptureState::Failed);
        (void)TeardownConnection(false);
        ResetState();
        return kr;
    }

    if (shouldConnectPCR) {
        const auto status = AwaitAllocation(
            [&cmp, cmpDevice, outputPlug, channel](CMP::CMPCallback callback) {
                cmp.ConnectOPCR(cmpDevice, outputPlug, channel,
                                std::move(callback));
            });
        if (status != IRM::AllocationStatus::Success) {
            const kern_return_t stopStatus =
                isoch.StopPacketReceive(&sink_);
            sink_.MarkTerminal(CaptureState::Failed);
            if (stopStatus != kIOReturnSuccess) {
                ownerToken_ = ownerToken;
                active_ = true;
                return stopStatus;
            }
            const kern_return_t result = ToIOReturn(status);
            (void)TeardownConnection(false);
            ResetState();
            return result;
        }
        pcrConnected_ = true;
    }

    ownerToken_ = ownerToken;
    active_ = true;
    sink_.MarkActive(channel, static_cast<uint8_t>(mode));
    ASFW_LOG(Isoch,
             "[DV] capture started GUID=0x%llx channel=%u mode=%u plug=%u bandwidth=%u owner=0x%llx",
             deviceGuid, channel, static_cast<unsigned>(mode), outputPlug,
             bandwidthUnits, ownerToken);
    return kIOReturnSuccess;
}

kern_return_t DVCaptureService::Stop(uint64_t ownerToken,
                                     Driver::IsochService& isoch) {
    if (!lock_) return kIOReturnNotReady;
    LockGuard guard(lock_);
    if (!active_) return kIOReturnSuccess;
    if (ownerToken == 0 || ownerToken != ownerToken_) {
        return kIOReturnNotPrivileged;
    }

    const kern_return_t stopStatus = isoch.StopPacketReceive(&sink_);
    if (stopStatus != kIOReturnSuccess) {
        return stopStatus;
    }
    sink_.MarkTerminal(CaptureState::Stopped);
    const kern_return_t kr = TeardownConnection(false);
    ASFW_LOG(Isoch, "[DV] capture stopped owner=0x%llx kr=0x%x",
             ownerToken, kr);
    ResetState();
    return kr;
}

void DVCaptureService::StopAll(Driver::IsochService& isoch) noexcept {
    if (!lock_) return;
    LockGuard guard(lock_);
    if (active_) {
        const kern_return_t stopStatus = isoch.StopPacketReceive(&sink_);
        if (stopStatus != kIOReturnSuccess) {
            ASFW_LOG_ERROR(
                Isoch,
                "[DV] capture stop-all could not quiesce IR kr=0x%x; retaining session",
                stopStatus);
            return;
        }
        sink_.MarkTerminal(CaptureState::Stopped);
        (void)TeardownConnection(false);
    }
    ResetState();
}

void DVCaptureService::HandleBusReset(
    Driver::IsochService& isoch) noexcept {
    if (!lock_) return;
    LockGuard guard(lock_);
    if (!active_) return;

    // A reset invalidates both the generation-pinned PCR transaction and all
    // IRM reservations. Do not write stale PCR/IRM state during the reset.
    sink_.MarkTerminal(CaptureState::BusReset);
    const kern_return_t stopStatus = isoch.StopPacketReceive(&sink_);
    if (stopStatus != kIOReturnSuccess) {
        ASFW_LOG_ERROR(
            Isoch,
            "[DV] bus-reset stop could not quiesce IR kr=0x%x; retaining session",
            stopStatus);
        return;
    }
    (void)TeardownConnection(true);
    ASFW_LOG_WARNING(Isoch,
                     "[DV] capture terminated by bus reset GUID=0x%llx",
                     route_.guid);
    ResetState();
}

kern_return_t DVCaptureService::TeardownConnection(
    bool generationInvalid) noexcept {
    kern_return_t result = kIOReturnSuccess;
    bool mayReleaseResources = !pcrConnected_;

    const CMP::CMPDevice cmpDevice{.route = route_};

    if (generationInvalid && cmp_ && route_) {
        cmp_->InvalidateRoute(route_);
    }

    if (!generationInvalid && pcrConnected_ && cmp_ && outputPlug_ <= 30) {
        const auto status = AwaitAllocation(
            [this, cmpDevice](CMP::CMPCallback callback) {
                cmp_->DisconnectOPCR(cmpDevice, outputPlug_,
                                     std::move(callback));
            });
        mayReleaseResources = status == IRM::AllocationStatus::Success;
        if (!mayReleaseResources) result = ToIOReturn(status);
    }
    pcrConnected_ = false;

    if (!generationInvalid && resourcesAllocated_ && mayReleaseResources &&
        irm_ && channel_ <= 63) {
        const auto status = AwaitAllocation(
            [this](IRM::AllocationCallback callback) {
                irm_->ReleaseResources(channel_, bandwidthUnits_,
                                       std::move(callback));
            });
        if (status != IRM::AllocationStatus::Success &&
            result == kIOReturnSuccess) {
            result = ToIOReturn(status);
        }
    }

    resourcesAllocated_ = false;
    bandwidthUnits_ = 0;
    irm_ = nullptr;
    cmp_ = nullptr;
    return result;
}

kern_return_t DVCaptureService::CopyMemory(
    uint64_t ownerToken,
    uint64_t* options,
    IOMemoryDescriptor** memory) const {
    if (!memory) return kIOReturnBadArgument;
    if (!lock_) return kIOReturnNotReady;

    LockGuard guard(lock_);
    if (!active_ || !ring_.memory) return kIOReturnNotReady;
    if (ownerToken == 0 || ownerToken != ownerToken_) {
        return kIOReturnNotPrivileged;
    }
    if (options) *options = 0;
    ring_.memory->retain();
    *memory = ring_.memory.get();
    return kIOReturnSuccess;
}

bool DVCaptureService::IsActive() const noexcept {
    if (!lock_) return false;
    LockGuard guard(lock_);
    return active_;
}

void DVCaptureService::ResetState() noexcept {
    sink_.Detach();
    ring_.Reset();
    ownerToken_ = 0;
    route_ = {};
    channel_ = 0xFF;
    outputPlug_ = 0xFF;
    bandwidthUnits_ = 0;
    connectionMode_ = ConnectionMode::Initializing;
    irm_ = nullptr;
    cmp_ = nullptr;
    pcrConnected_ = false;
    resourcesAllocated_ = false;
    active_ = false;
}

} // namespace ASFW::Protocols::DV
