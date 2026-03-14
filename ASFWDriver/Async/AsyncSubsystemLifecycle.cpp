#include "AsyncSubsystem.hpp"
#include "Engine/ContextManager.hpp"

// Command architecture
#include "Commands/LockCommand.hpp"
#include "Commands/PhyCommand.hpp"
#include "Commands/ReadCommand.hpp"
#include "Commands/WriteCommand.hpp"

#include "../Debug/BusResetPacketCapture.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Logging/Logging.hpp"
#include "Core/TransactionManager.hpp"
#include "Rx/ARPacketParser.hpp"
#include "Track/CompletionQueue.hpp"
#include "Track/LabelAllocator.hpp"
#include "Track/Tracking.hpp"
#include "Tx/DescriptorBuilder.hpp"
#include "Tx/PacketBuilder.hpp"
#include "Tx/Submitter.hpp"

// New context architecture
#include "../Shared/Memory/DMAMemoryManager.hpp"
#include "../Shared/Rings/BufferRing.hpp"
#include "../Shared/Rings/DescriptorRing.hpp"
#include "Contexts/ARRequestContext.hpp"
#include "Contexts/ARResponseContext.hpp"
#include "Contexts/ATRequestContext.hpp"
#include "Contexts/ATResponseContext.hpp"
#include "Rx/PacketRouter.hpp"
#include "Tx/ResponseSender.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>

#include <DriverKit/IOLib.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSString.h>

namespace ASFW::Async {

AsyncSubsystem::AsyncSubsystem() = default;
AsyncSubsystem::~AsyncSubsystem() = default;

namespace {
uint64_t GetCurrentMonotonicTimeUsec() {
    static mach_timebase_info_data_t timebase{};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    const uint64_t ticks = mach_absolute_time();
    return (ticks * timebase.numer) / timebase.denom / 1000;
}

constexpr uint32_t kAsyncInterruptMask = 0x0000000Du;
constexpr uint32_t kLinkControlRcvPhyPktBit = 1u << 12;
// TODO(ASFW-Topology): Replace the compatibility S100 default with topology-driven speed
// selection once the async startup path can query the negotiated link speed.
// Replace with topology-based speed queries when TopologyManager is available.
// NOTE: Apple's IOFireWireFamily uses speed downgrade strategy for discovery:
//   - Start at S400 for initial Config ROM read (faster discovery)
//   - Downgrade to S100 after first successful transaction (reliability)
//   - See packet trace: 060:3279:1136 (s400) → 060:3291:0385 (s100)
// FIXED: Speed encoding bug corrected in PacketBuilder.cpp (shift 16→24 per OHCI spec)
constexpr uint8_t kDefaultAsyncSpeed = 0; // S100 (98.304 Mbps)
constexpr size_t kDefaultCompletionQueueCapacity = size_t{64} * 1024u;

bool ParseBooleanLikeProperty(OSObject* property) {
    if (auto booleanProp = OSDynamicCast(OSBoolean, property)) {
        return booleanProp == kOSBooleanTrue;
    }
    if (auto numberProp = OSDynamicCast(OSNumber, property)) {
        return numberProp->unsigned32BitValue() != 0;
    }
    if (auto stringProp = OSDynamicCast(OSString, property)) {
        return stringProp->isEqualTo("1") || stringProp->isEqualTo("true") ||
               stringProp->isEqualTo("TRUE");
    }
    return false;
}

bool ShouldEnableCoherencyTrace(OSObject* owner) {
    auto service = OSDynamicCast(IOService, owner);
    if (!service) {
        return false;
    }

    OSDictionary* properties = nullptr;
    const kern_return_t kr = service->CopyProperties(&properties);
    if (kr != kIOReturnSuccess || properties == nullptr) {
        return false;
    }

    const bool enabled = ParseBooleanLikeProperty(properties->getObject("ASFWTraceDMACoherency"));
    properties->release();
    return enabled;
}

// Retry state structure (heap-allocated, freed after final completion)
// Similar to Apple's command object pattern but lighter-weight
struct RetryState {
    ReadParams params;
    RetryPolicy policy;
    CompletionCallback userCallback;
    uint8_t attemptsRemaining;
    AsyncHandle currentHandle{};
    AsyncSubsystem* subsystem; // Back-pointer for re-submission

    RetryState(const ReadParams& p, const RetryPolicy& pol, CompletionCallback cb,
               AsyncSubsystem* sub)
        : params(p), policy(pol), userCallback(cb), attemptsRemaining(pol.maxRetries),
          subsystem(sub) {}
};

using RetryStatePtr = std::shared_ptr<RetryState>;

// Static callback function that implements retry logic
// This matches Apple's IOFWAsyncCommand::complete() pattern where retries
// are decremented and execute() is called again on transient failures
// Signature matches CompletionCallback: (AsyncHandle, AsyncStatus, responseCode, std::span<const
// uint8_t>)
void ReadWithRetryCallback(AsyncHandle handle, AsyncStatus status, uint8_t responseCode,
                           std::span<const uint8_t> responsePayload,
                           const RetryStatePtr& state) {
    if (!state) {
        return;
    }

    // Check if retry is needed (Apple's pattern: retry on timeout/busy)
    bool shouldRetry = false;
    const char* retryReason = "";

    if (state->attemptsRemaining > 0 && status != AsyncStatus::kSuccess) {
        // Apple pattern: IOFWAsyncCommand::complete() checks rcode and decrements fCurRetries
        if (status == AsyncStatus::kTimeout && state->policy.retryOnTimeout) {
            shouldRetry = true;
            retryReason = "timeout";
        } else if (status == AsyncStatus::kBusyRetryExhausted && state->policy.retryOnBusy) {
            shouldRetry = true;
            retryReason = "busy";
        }
        // TODO(ASFW-Async): Add speed fallback on type error (Apple's
        // IOFWReadCommand::gotPacket pattern).
        // if (status == AsyncStatus::kTypeError && state->policy.speedFallback) {
        //     state->params.speedCode = 0;  // Downgrade to S100
        //     shouldRetry = true;
        //     retryReason = "type error, downgrading speed";
        // }
    }

    if (shouldRetry) {
        const uint8_t attemptNumber = state->policy.maxRetries - state->attemptsRemaining + 1;
        state->attemptsRemaining--;

        ASFW_LOG(Async, "ReadWithRetry: %{public}s on attempt %u, %u retries remaining",
                 retryReason, attemptNumber, state->attemptsRemaining);

        // Apple pattern: Delay before retry (simple blocking sleep)
        // In production, this could be improved with async timer dispatch
        if (state->policy.retryDelayUsec > 0) {
            const uint32_t delayMs = static_cast<uint32_t>(state->policy.retryDelayUsec / 1000);
            if (delayMs > 0) {
                IOSleep(delayMs); // Convert µs to ms
            }
        }

        // Re-submit transaction (Apple pattern: call execute() again)
        // Note: This creates a new handle - cancellation of original handle won't
        // affect retries. This matches Apple's behavior where commands are atomic.
        state->currentHandle =
            state->subsystem->Read(state->params, [state](AsyncHandle h, AsyncStatus s, uint8_t rc,
                                                          std::span<const uint8_t> payload) {
                ReadWithRetryCallback(h, s, rc, payload, state);
            });

        if (!state->currentHandle) {
            // Retry submission failed - invoke user callback with error
            ASFW_LOG_ERROR(Async, "ReadWithRetry: Re-submission failed after %{public}s",
                           retryReason);
            if (state->userCallback) {
                state->userCallback(handle, AsyncStatus::kHardwareError, 0xFF,
                                    std::span<const uint8_t>{});
            }
        }
    } else {
        // Final completion - no more retries available or success achieved
        // Invoke user callback with actual result
        if (status != AsyncStatus::kSuccess) {
            ASFW_LOG(Async, "ReadWithRetry: Final completion after %u attempts: status=%u",
                     state->policy.maxRetries - state->attemptsRemaining + 1,
                     static_cast<unsigned>(status));
        }

        if (state->userCallback) {
            state->userCallback(handle, status, responseCode, responsePayload);
        }
    }
}

struct PayloadContext {
    PayloadContext() = default;
    ~PayloadContext() { Reset(); }

    PayloadContext(const PayloadContext&) = delete;
    PayloadContext& operator=(const PayloadContext&) = delete;

    bool Initialize(Driver::HardwareInterface& hw, const uint8_t* logicalData, std::size_t length,
                    uint64_t options) {
        Reset();

        auto dmaOpt = hw.AllocateDMA(length, options, 16);
        if (!dmaOpt.has_value()) {
            return false;
        }

        dmaBuffer_ = std::move(dmaOpt.value());

        IOMemoryMap* map = nullptr;
        kern_return_t kr = dmaBuffer_.descriptor->CreateMapping(0, 0, 0, 0, 0, &map);
        if (kr != kIOReturnSuccess || map == nullptr) {
            Reset();
            return false;
        }

        mapping_ = map;
        virtualAddress_ = reinterpret_cast<uint8_t*>(map->GetAddress());
        if (virtualAddress_ == nullptr) {
            Reset();
            return false;
        }

        if (logicalData != nullptr && length > 0) {
            std::memcpy(virtualAddress_, logicalData, length);
            std::atomic_thread_fence(std::memory_order_release);
#if defined(IODMACommand_Synchronize_ID)
            if (dmaBuffer_.dmaCommand) {
                const kern_return_t syncKr = dmaBuffer_.dmaCommand->Synchronize(
                    /*options*/ 0,
                    /*offset*/ 0, static_cast<uint64_t>(length));
                if (syncKr != kIOReturnSuccess) {
                    ASFW_LOG(Async, "PayloadContext(Stream): Synchronize failed kr=0x%x len=%zu",
                             syncKr, length);
                    OSSynchronizeIO();
                }
            } else {
                ASFW_LOG(Async, "PayloadContext(Stream): Missing DMA command for cache sync");
                OSSynchronizeIO();
            }
#else
            OSSynchronizeIO();
#endif
        }

        logicalAddress_ = logicalData;
        length_ = length;
        return true;
    }

    void Reset() {
        if (mapping_ != nullptr) {
            mapping_->release();
            mapping_ = nullptr;
        }
        if (dmaBuffer_.dmaCommand) {
            dmaBuffer_.dmaCommand->CompleteDMA(kIODMACommandCompleteDMANoOptions);
            dmaBuffer_.dmaCommand.reset();
        }
        dmaBuffer_.descriptor.reset();
        dmaBuffer_.deviceAddress = 0;
        dmaBuffer_.length = 0;
        virtualAddress_ = nullptr;
        logicalAddress_ = nullptr;
        length_ = 0;
    }

    [[nodiscard]] uint64_t DeviceAddress() const noexcept { return dmaBuffer_.deviceAddress; }

    [[nodiscard]] uint8_t* VirtualAddress() const noexcept { return virtualAddress_; }

    [[nodiscard]] const uint8_t* LogicalAddress() const noexcept { return logicalAddress_; }

    [[nodiscard]] std::size_t Length() const noexcept { return length_; }

  private:
    Driver::HardwareInterface::DMABuffer dmaBuffer_{};
    IOMemoryMap* mapping_{nullptr};
    uint8_t* virtualAddress_{nullptr};
    const uint8_t* logicalAddress_{nullptr};
    std::size_t length_{0};
};

} // namespace

kern_return_t AsyncSubsystem::InitializeCoreStartState(size_t completionQueueCapacityBytes,
                                                       const char*& failureStage) {
    if (!labelAllocator_) {
        labelAllocator_ = std::make_unique<LabelAllocator>();
    }
    labelAllocator_->Reset();

    sharedLock_ = ::IOLockAlloc();
    if (!sharedLock_) {
        failureStage = "AllocSharedLock";
        return kIOReturnNoMemory;
    }

    commandQueue_ = std::make_unique<std::deque<PendingCommandPtr>>();
    commandQueueLock_ = ::IOLockAlloc();
    if (!commandQueueLock_) {
        failureStage = "AllocCommandQueueLock";
        return kIOReturnNoMemory;
    }
    commandInFlight_.store(false, std::memory_order_release);

    txnMgr_ = std::make_unique<TransactionManager>();
    Result<void> txnMgrResult = txnMgr_->Initialize();
    if (!txnMgrResult) {
        txnMgrResult.error().Log();
        failureStage = "TransactionManager";
        return txnMgrResult.error().BoundaryStatus();
    }

    if (!generationTracker_) {
        generationTracker_ = std::make_unique<ASFW::Async::Bus::GenerationTracker>(*labelAllocator_);
    }
    generationTracker_->Reset();

    packetBuilder_ = std::make_unique<PacketBuilder>();

    std::unique_ptr<CompletionQueue> completionQueue;
    const kern_return_t kr = CompletionQueue::Create(workloopQueue_, completionQueueCapacityBytes,
                                                     completionAction_.get(), completionQueue);
    if (kr != kIOReturnSuccess || !completionQueue) {
        ASFW_LOG(Async, "FAILED: CompletionQueue::Create returned 0x%08x", kr);
        failureStage = "CompletionQueue";
        return kr != kIOReturnSuccess ? kr : kIOReturnNoMemory;
    }

    completionQueue_ = std::move(completionQueue);
    completionQueue_->SetClientBound();
    completionQueue_->Activate();

    tracking_ = std::make_unique<Track_Tracking<CompletionQueue>>(
        labelAllocator_.get(), txnMgr_.get(), *completionQueue_, nullptr);
    return kIOReturnSuccess;
}

kern_return_t AsyncSubsystem::ProvisionAsyncDataPath(const char*& failureStage) {
    constexpr size_t kATReqDescCount = 256;
    constexpr size_t kATRespDescCount = 64;
    constexpr size_t kARReqBufferCount = 128;
    constexpr size_t kARReqBufferSize = 4096 + 64;
    constexpr size_t kARRespBufferCount = 256;
    constexpr size_t kARRespBufferSize = 4096 + 64;

    contextManager_ = std::make_unique<Engine::ContextManager>();

    Engine::ProvisionSpec spec{};
    spec.atReqDescCount = kATReqDescCount;
    spec.atRespDescCount = kATRespDescCount;
    spec.arReqBufCount = kARReqBufferCount;
    spec.arReqBufSize = kARReqBufferSize;
    spec.arRespBufCount = kARRespBufferCount;
    spec.arRespBufSize = kARRespBufferSize;

    const kern_return_t provisionKr = contextManager_->provision(*hardware_, spec);
    if (provisionKr != kIOReturnSuccess) {
        ASFW_LOG(Async, "FAILED: ContextManager::provision (kr=0x%08x)", provisionKr);
        failureStage = "ContextManagerProvision";
        return provisionKr;
    }

    DMAMemoryManager::SetTracingEnabled(ShouldEnableCoherencyTrace(owner_));
    if (DMAMemoryManager::IsTracingEnabled()) {
        ASFW_LOG(Async, "AsyncSubsystem: coherency tracing enabled (ASFWTraceDMACoherency)");
    }

    descriptorBuilder_ = contextManager_->GetDescriptorBuilderRequest();
    descriptorBuilderResponse_ = contextManager_->GetDescriptorBuilderResponse();

    if (!descriptorBuilder_) {
        ASFW_LOG_ERROR(Async, "AsyncSubsystem: descriptorBuilder (request) unavailable");
        failureStage = "DescriptorBuilderReq";
        return kIOReturnNoResources;
    }
    if (!descriptorBuilderResponse_) {
        ASFW_LOG_ERROR(Async, "AsyncSubsystem: descriptorBuilder (response) unavailable");
        failureStage = "DescriptorBuilderRsp";
        return kIOReturnNoResources;
    }

    submitter_ = std::make_unique<Tx::Submitter>(*contextManager_, *descriptorBuilder_);
    if (tracking_ && contextManager_) {
        contextManager_->SetPayloads(tracking_->Payloads());
        if (submitter_) {
            submitter_->SetPayloads(tracking_->Payloads());
        }

        tracking_->SetContextManager(contextManager_.get());
    }

    packetRouter_ = std::make_unique<PacketRouter>();
    rxPath_ = std::make_unique<Rx::RxPath>(*contextManager_->GetArRequestContext(),
                                           *contextManager_->GetArResponseContext(), *tracking_,
                                           *generationTracker_, *packetRouter_);

    responseSender_ = std::make_unique<ResponseSender>(*descriptorBuilderResponse_, *submitter_,
                                                       *contextManager_, *generationTracker_);
    packetRouter_->SetResponseSender(responseSender_.get());

    ASFW_LOG(Async, "✓ ContextManager provisioned");
    return kIOReturnSuccess;
}

void AsyncSubsystem::ResetWatchdogCounters() noexcept {
    watchdogTickCount_.store(0, std::memory_order_relaxed);
    watchdogExpiredCount_.store(0, std::memory_order_relaxed);
    watchdogDrainedCompletions_.store(0, std::memory_order_relaxed);
    watchdogContextsRearmed_.store(0, std::memory_order_relaxed);
    watchdogLastTickUsec_.store(0, std::memory_order_relaxed);
}

void AsyncSubsystem::FinalizeStart() {
    busResetCapture_ = std::make_unique<Debug::BusResetPacketCapture>();

    hardware_->SetInterruptMask(kAsyncInterruptMask, true);
    hardware_->SetLinkControlBits(kLinkControlRcvPhyPktBit);

    // Report DMA cache mode (always uncached since kIOMemoryMapCacheModeInhibit works reliably)
    ASFW_LOG_TYPE(Async, OS_LOG_TYPE_INFO, "AsyncSubsystem::Start complete (DMA always uncached)");

    ResetWatchdogCounters();
    is_bus_reset_in_progress_.store(0, std::memory_order_release);
    isRunning_ = true;
}

kern_return_t AsyncSubsystem::FailStart(const char* failureStage, kern_return_t kr) {
    ASFW_LOG_TYPE(Async, OS_LOG_TYPE_ERROR,
                  "AsyncSubsystem::Start failed at stage %{public}s (kr=0x%08x)",
                  failureStage ? failureStage : "unknown", kr);
    Teardown(false);
    return kr == kIOReturnSuccess ? kIOReturnError : kr;
}

kern_return_t AsyncSubsystem::Start(Driver::HardwareInterface& hw, OSObject* owner,
                                    IODispatchQueue* workloopQueue, OSAction* completionAction,
                                    size_t completionQueueCapacityBytes) {
    if (isRunning_) {
        ASFW_LOG(Async, "Already running, returning success");
        return kIOReturnSuccess;
    }

    if (!owner || !workloopQueue || !completionAction) {
        ASFW_LOG(Async, "Bad arguments: owner=%p queue=%p action=%p", owner, workloopQueue,
                 completionAction);
        return kIOReturnBadArgument;
    }

    if (completionQueueCapacityBytes == 0) {
        completionQueueCapacityBytes = kDefaultCompletionQueueCapacity;
    }

    // Initial bus state is managed by GenerationTracker. Reset tracker state now.
    hardware_ = &hw;
    owner_ = owner;
    workloopQueue_ = workloopQueue;
    completionAction_ = OSSharedPtr(completionAction, OSRetain);

    const char* failureStage = nullptr;
    const kern_return_t coreKr =
        InitializeCoreStartState(completionQueueCapacityBytes, failureStage);
    if (coreKr != kIOReturnSuccess) {
        return FailStart(failureStage, coreKr);
    }

    const kern_return_t provisionKr = ProvisionAsyncDataPath(failureStage);
    if (provisionKr != kIOReturnSuccess) {
        return FailStart(failureStage, provisionKr);
    }

    FinalizeStart();
    return kIOReturnSuccess;
}

kern_return_t AsyncSubsystem::ArmDMAContexts() {
    if (!isRunning_) {
        ASFW_LOG(Async, "ArmDMAContexts() called but AsyncSubsystem not running");
        return kIOReturnNotReady;
    }

    if (!contextManager_) {
        ASFW_LOG(Async, "ArmDMAContexts() called but ContextManager not initialized");
        return kIOReturnNoResources;
    }

    ASFW_LOG(Async, "Arming DMA contexts (AFTER LinkEnable)...");

    // ContextManager is the single authority for DMA contexts.
    ASFW_LOG(Async, "Arming DMA contexts via ContextManager (exclusive)...");

    // Arm AR contexts (receive) immediately
    kern_return_t kr = contextManager_->armAR();
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Async, "FAILED: ContextManager::armAR (kr=0x%08x)", kr);
        return kr;
    }

    // AT contexts are initialized to IDLE state by ATManager and will be armed
    // via PATH 1 (direct arming) on first submission - no sentinel setup needed.

    ASFW_LOG(Async, "ArmDMAContexts: completed via ContextManager");
    return kIOReturnSuccess;
}

kern_return_t AsyncSubsystem::ArmARContextsOnly() {
    if (!isRunning_) {
        ASFW_LOG(Async, "ArmARContextsOnly() called but AsyncSubsystem not running");
        return kIOReturnNotReady;
    }

    if (!contextManager_) {
        ASFW_LOG(Async, "ArmARContextsOnly() called but ContextManager not initialized");
        return kIOReturnNoResources;
    }

    ASFW_LOG(Async, "Arming AR contexts via ContextManager (receive path)");
    kern_return_t kr = contextManager_->armAR();
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Async, "FAILED: ContextManager::armAR (kr=0x%08x)", kr);
        return kr;
    }

    ASFW_LOG(Async, "AR contexts armed via ContextManager");
    return kIOReturnSuccess;
}

void AsyncSubsystem::Stop() {
    const bool disableHardware = isRunning_ && hardware_ != nullptr;
    Teardown(disableHardware);
}

void AsyncSubsystem::Teardown(bool disableHardware) {
    if (disableHardware && hardware_) {
        hardware_->SetInterruptMask(0xFFFFFFFFu, false);
        hardware_->ClearLinkControlBits(kLinkControlRcvPhyPktBit);
    }

    // CRITICAL: Deactivate completion queue BEFORE stopping contexts
    // This prevents new enqueues while we're tearing down, but allows
    // in-flight completions to be processed
    if (completionQueue_) {
        completionQueue_->Deactivate();
        completionQueue_->SetClientUnbound();
    }

    // Delegate teardown to ContextManager (it owns DMA mappings/rings/contexts)
    if (contextManager_) {
        contextManager_->teardown(disableHardware);
    } else {
        ASFW_LOG(Async, "Teardown: ContextManager not present");
    }

    completionQueue_.reset();
    completionAction_.reset();

    if (txnMgr_) {
        txnMgr_->CancelAll();
        txnMgr_.reset();
    }

    responseSender_.reset();
    descriptorBuilder_ = nullptr;
    descriptorBuilderResponse_ = nullptr;
    packetBuilder_.reset();

    generationTracker_.reset();

    if (sharedLock_) {
        ::IOLockFree(sharedLock_);
        sharedLock_ = nullptr;
    }

    // Clean up command queue
    if (commandQueueLock_) {
        ::IOLockLock(commandQueueLock_);
        if (commandQueue_) {
            commandQueue_->clear();
        }
        ::IOLockUnlock(commandQueueLock_);
        ::IOLockFree(commandQueueLock_);
        commandQueueLock_ = nullptr;
    }
    commandQueue_.reset();
    commandInFlight_.store(false, std::memory_order_release);

    owner_ = nullptr;
    workloopQueue_ = nullptr;
    hardware_ = nullptr;

    is_bus_reset_in_progress_.store(0, std::memory_order_release);
    isRunning_ = false;
}

// ============================================================================
// Helper Methods for CRTP Commands
// ============================================================================

std::optional<TransactionContext> AsyncSubsystem::PrepareTransactionContext() {
    // Step 1: Bus reset gate check
    if (is_bus_reset_in_progress_.load(std::memory_order_acquire)) {
        ASFW_LOG_ERROR(Async, "PrepareTransactionContext: Bus reset in progress");
        return std::nullopt;
    }

    // Step 2: Validate subsystem components initialized
    if (!packetBuilder_ || !descriptorBuilder_ || !ResolveAtRequestContext()) {
        ASFW_LOG_ERROR(Async, "PrepareTransactionContext: Subsystem not initialized");
        return std::nullopt;
    }

    // Step 3: Read NodeID register with valid bit check (OHCI §5.10, bit 31)
    const uint32_t nodeIdReg = hardware_->ReadNodeID();
    constexpr uint32_t kNodeIDValidBit = 0x80000000u;
    if ((nodeIdReg & kNodeIDValidBit) == 0) {
        ASFW_LOG_ERROR(Async, "PrepareTransactionContext: NodeID valid bit not set (reg=0x%08x)",
                       nodeIdReg);
        return std::nullopt;
    }
    const uint16_t sourceNodeID = static_cast<uint16_t>(nodeIdReg & 0xFFFFu);

    // Step 4: Query current generation from GenerationTracker
    const auto busState = generationTracker_->GetCurrentState();
    const uint8_t currentGeneration = busState.generation8;

    // Step 5: Resolve speed code. TODO(ASFW-Topology): query TopologyManager instead of using the
    // compatibility S100 default.
    const uint8_t speedCode = kDefaultAsyncSpeed; // S100 (98.304 Mbps)

    // Step 6: Build TransactionContext with PacketContext
    TransactionContext txCtx{};
    txCtx.sourceNodeID = sourceNodeID;
    txCtx.generation = currentGeneration;
    txCtx.speedCode = speedCode;
    txCtx.packetContext = PacketContext{sourceNodeID, currentGeneration, speedCode};

    return txCtx;
}

uint64_t AsyncSubsystem::GetCurrentTimeUsec() const { return GetCurrentMonotonicTimeUsec(); }

void AsyncSubsystem::OnTimeoutTick() {
    if (!isRunning_) {
        return;
    }
    if (is_bus_reset_in_progress_.load(std::memory_order_acquire)) {
        return;
    }

    const uint64_t nowUsec = GetCurrentMonotonicTimeUsec();

    // Delegate timeout processing to Tracking actor
    if (tracking_) {
        tracking_->OnTimeoutTick(nowUsec);
    }

    const uint32_t drainedByWatchdog = DrainTxCompletions("watchdog");
    const bool contextsRearmed = EnsureATContextsRunning("timeout-watchdog");

    watchdogTickCount_.fetch_add(1, std::memory_order_relaxed);
    watchdogLastTickUsec_.store(nowUsec, std::memory_order_relaxed);

    if (drainedByWatchdog > 0) {
        watchdogDrainedCompletions_.fetch_add(drainedByWatchdog, std::memory_order_relaxed);
    }

    if (contextsRearmed) {
        watchdogContextsRearmed_.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace ASFW::Async
