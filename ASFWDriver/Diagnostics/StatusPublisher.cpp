#include "StatusPublisher.hpp"

#include <string>

#include "../Controller/ControllerCore.hpp"
#include "../Controller/ControllerStateMachine.hpp"
#include "../Diagnostics/MetricsSink.hpp"
#include "../Async/AsyncSubsystem.hpp"
#include <net.mrmidi.ASFW.ASFWDriver/ASFWDriverUserClient.h>

namespace ASFW::Driver {

kern_return_t StatusPublisher::Prepare() {
    if (statusBlock_ != nullptr) {
        return kIOReturnSuccess;
    }

    IOBufferMemoryDescriptor* rawBuffer = nullptr;
    auto kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                               sizeof(SharedStatusBlock),
                                               64,
                                               &rawBuffer);
    if (kr != kIOReturnSuccess || rawBuffer == nullptr) {
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }

    rawBuffer->SetLength(sizeof(SharedStatusBlock));
    statusMemory_ = OSSharedPtr(rawBuffer, OSNoRetain);

    IOMemoryMap* rawMap = nullptr;
    kr = rawBuffer->CreateMapping(0, 0, 0, 0, 0, &rawMap);
    if (kr != kIOReturnSuccess || rawMap == nullptr) {
        statusMemory_.reset();
        return (kr != kIOReturnSuccess) ? kr : kIOReturnNoMemory;
    }

    statusMap_ = OSSharedPtr(rawMap, OSNoRetain);
    statusBlock_ = reinterpret_cast<SharedStatusBlock*>(rawMap->GetAddress());
    if (!statusBlock_) {
        statusMap_.reset();
        statusMemory_.reset();
        return kIOReturnNoMemory;
    }

    std::memset(statusBlock_, 0, sizeof(SharedStatusBlock));
    statusBlock_->version = SharedStatusBlock::kVersion;
    statusBlock_->length = sizeof(SharedStatusBlock);
    statusBlock_->sequence = 0;
    statusBlock_->reason = static_cast<uint32_t>(SharedStatusReason::Boot);
    statusBlock_->updateTimestamp = mach_absolute_time();
    return kIOReturnSuccess;
}

void StatusPublisher::Reset() {
    statusListener_.reset();
    statusBlock_ = nullptr;
    statusMemory_.reset();
    statusMap_.reset();
    statusSequence_.store(0, std::memory_order_release);
    lastAsyncCompletionMach_.store(0, std::memory_order_release);
    asyncTimeoutCount_.store(0, std::memory_order_release);
    watchdogTickCount_.store(0, std::memory_order_release);
    watchdogLastTickUsec_.store(0, std::memory_order_release);
}

void StatusPublisher::Publish(ControllerCore* controller,
                              const ASFW::Async::AsyncSubsystem* asyncSubsystem,
                              SharedStatusReason reason,
                              uint32_t detailMask) {
    if (!statusBlock_) {
        return;
    }

    SharedStatusBlock snapshot{};
    snapshot.version = SharedStatusBlock::kVersion;
    snapshot.length = sizeof(SharedStatusBlock);
    snapshot.reason = static_cast<uint32_t>(reason);
    snapshot.detailMask = detailMask;
    snapshot.updateTimestamp = mach_absolute_time();
    snapshot.sequence = statusSequence_.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (controller) {
        const auto state = controller->StateMachine().CurrentState();
        snapshot.controllerState = static_cast<uint32_t>(state);
        auto stateName = std::string(ToString(state));
        std::strncpy(snapshot.controllerStateName,
                     stateName.c_str(),
                     sizeof(snapshot.controllerStateName) - 1);

        const auto& busMetrics = controller->Metrics().BusReset();
        snapshot.busResetCount = busMetrics.resetCount;
        snapshot.lastBusResetStart = busMetrics.lastResetStart;
        snapshot.lastBusResetCompletion = busMetrics.lastResetCompletion;

        if (auto topo = controller->LatestTopology()) {
            snapshot.busGeneration = topo->generation;
            snapshot.nodeCount = topo->nodeCount;
            if (topo->localNodeId.has_value()) {
                snapshot.localNodeID = static_cast<uint32_t>(*topo->localNodeId);
            }
            if (topo->rootNodeId.has_value()) {
                snapshot.rootNodeID = static_cast<uint32_t>(*topo->rootNodeId);
            }
            if (topo->irmNodeId.has_value()) {
                snapshot.irmNodeID = static_cast<uint32_t>(*topo->irmNodeId);
            }
            if (topo->irmNodeId.has_value() && topo->localNodeId.has_value() &&
                topo->irmNodeId == topo->localNodeId) {
                snapshot.flags |= SharedStatusBlock::kFlagIsIRM;
            }
        }
    }

    if (asyncSubsystem) {
        const auto stats = asyncSubsystem->GetWatchdogStats();
        snapshot.watchdogTickCount = stats.tickCount;
        snapshot.watchdogLastTickUsec = stats.lastTickUsec;
        snapshot.asyncTimeouts = static_cast<uint32_t>(stats.expiredTransactions);
        snapshot.asyncPending = 0; // Placeholder until OutstandingTable exposes count
    }

    snapshot.asyncLastCompletion = lastAsyncCompletionMach_.load(std::memory_order_acquire);
    snapshot.asyncTimeouts = asyncTimeoutCount_.load(std::memory_order_acquire);
    snapshot.watchdogTickCount = watchdogTickCount_.load(std::memory_order_acquire);
    snapshot.watchdogLastTickUsec = watchdogLastTickUsec_.load(std::memory_order_acquire);

    if (snapshot.localNodeID != 0xFFFFFFFFu) {
        snapshot.flags |= SharedStatusBlock::kFlagLinkActive;
    }

    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(statusBlock_, &snapshot, sizeof(SharedStatusBlock));
    std::atomic_thread_fence(std::memory_order_release);

    if (statusListener_) {
        if (auto* client = OSDynamicCast(ASFWDriverUserClient, statusListener_.get())) {
            client->NotifyStatus(snapshot.sequence, snapshot.reason);
        }
    }
}

void StatusPublisher::BindListener(::ASFWDriverUserClient* client) {
    if (client) {
        statusListener_.reset(static_cast<OSObject*>(client), OSRetain);
    } else {
        statusListener_.reset();
    }
}

void StatusPublisher::UnbindListener(::ASFWDriverUserClient* client) {
    if (statusListener_ && statusListener_.get() == static_cast<OSObject*>(client)) {
        statusListener_.reset();
    }
}

kern_return_t StatusPublisher::CopySharedMemory(uint64_t* options, IOMemoryDescriptor** memory) const {
    if (!memory) {
        return kIOReturnBadArgument;
    }
    if (!statusMemory_) {
        return kIOReturnNotReady;
    }

    auto descriptor = statusMemory_.get();
    descriptor->retain();
    *memory = descriptor;
    if (options) {
        *options = kIOUserClientMemoryReadOnly;
    }
    return kIOReturnSuccess;
}

void StatusPublisher::SetLastAsyncCompletion(uint64_t machTime) {
    lastAsyncCompletionMach_.store(machTime, std::memory_order_release);
}

void StatusPublisher::UpdateAsyncWatchdog(uint32_t asyncTimeoutCount,
                                          uint64_t watchdogTickCount,
                                          uint64_t watchdogLastTickUsec) {
    asyncTimeoutCount_.store(asyncTimeoutCount, std::memory_order_release);
    watchdogTickCount_.store(watchdogTickCount, std::memory_order_release);
    watchdogLastTickUsec_.store(watchdogLastTickUsec, std::memory_order_release);
}

} // namespace ASFW::Driver
