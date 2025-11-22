#include "AsyncSubsystem.hpp"
#include "Engine/ContextManager.hpp"
#include "../Common/FWCommon.hpp"  // For FW::Ack, FW::AckName, FW::AckFromByte

// Command architecture
#include "Commands/ReadCommand.hpp"
#include "Commands/WriteCommand.hpp"
#include "Commands/LockCommand.hpp"
#include "Commands/PhyCommand.hpp"

#include "Rx/ARPacketParser.hpp"
#include "Tx/DescriptorBuilder.hpp"
#include "Tx/Submitter.hpp"
#include "Track/LabelAllocator.hpp"
#include "Tx/PacketBuilder.hpp"
#include "Track/CompletionQueue.hpp"
#include "Core/TransactionManager.hpp"  // Phase 2.0
#include "../Hardware/OHCIEventCodes.hpp"
#include "../Hardware/OHCIDescriptors.hpp"
#include "../Hardware/HardwareInterface.hpp"
#include "../Logging/Logging.hpp"
#include "../Debug/BusResetPacketCapture.hpp"
#include "Track/Tracking.hpp"

// New context architecture
#include "../Shared/Memory/DMAMemoryManager.hpp"
#include "../Shared/Rings/DescriptorRing.hpp"
#include "../Shared/Rings/BufferRing.hpp"
#include "Contexts/ATRequestContext.hpp"
#include "Contexts/ATResponseContext.hpp"
#include "Contexts/ARRequestContext.hpp"
#include "Contexts/ARResponseContext.hpp"
#include "Rx/PacketRouter.hpp"
// Context manager (optional incremental wiring)
#include "Engine/ContextManager.hpp"

#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <optional>
#include <cassert>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSObject.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOMemoryMap.h>
#include <DriverKit/IODMACommand.h>
#include <DriverKit/OSBoolean.h>
#include <DriverKit/OSNumber.h>
#include <DriverKit/OSString.h>
#include <DriverKit/IOService.h>
#include <DriverKit/OSDictionary.h>
#include "../Logging/Logging.hpp"
#include <deque>

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
// TODO: S100 hardcoded for maximum hardware compatibility (especially Agere/LSI FW643E).
// Replace with topology-based speed queries when TopologyManager is available.
// NOTE: Apple's IOFireWireFamily uses speed downgrade strategy for discovery:
//   - Start at S400 for initial Config ROM read (faster discovery)
//   - Downgrade to S100 after first successful transaction (reliability)
//   - See packet trace: 060:3279:1136 (s400) → 060:3291:0385 (s100)
// FIXED: Speed encoding bug corrected in PacketBuilder.cpp (shift 16→24 per OHCI spec)
constexpr uint8_t kDefaultAsyncSpeed = 0;  // S100 (98.304 Mbps)
// DMA bases are now dynamically allocated via HardwareInterface::AllocateDMA()
// with 16-byte alignment per OHCI §1.7, Table 7-3
// Phase 2.0: Legacy constants removed (kOutstandingSlotCapacity, kTimeoutWheelBuckets, kTimeoutQuantumUsec, kSlotFlagHasRequestPayloadDMA)
constexpr size_t kDefaultCompletionQueueCapacity = 64 * 1024;

bool ShouldEnableCoherencyTrace(OSObject* owner) {
    bool enabled = false;
    if (auto service = OSDynamicCast(IOService, owner)) {
        OSDictionary* properties = nullptr;
        const kern_return_t kr = service->CopyProperties(&properties);
        if (kr == kIOReturnSuccess && properties != nullptr) {
            if (auto property = properties->getObject("ASFWTraceDMACoherency")) {
                if (auto booleanProp = OSDynamicCast(OSBoolean, property)) {
                    enabled = (booleanProp == kOSBooleanTrue);
                } else if (auto numberProp = OSDynamicCast(OSNumber, property)) {
                    enabled = numberProp->unsigned32BitValue() != 0;
                } else if (auto stringProp = OSDynamicCast(OSString, property)) {
                    enabled = stringProp->isEqualTo("1") ||
                              stringProp->isEqualTo("true") ||
                              stringProp->isEqualTo("TRUE");
                }
            }
            properties->release();
        }
    }
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
    AsyncSubsystem* subsystem;  // Back-pointer for re-submission
    
    RetryState(const ReadParams& p, const RetryPolicy& pol, 
               CompletionCallback cb, AsyncSubsystem* sub)
        : params(p), policy(pol), userCallback(cb)
        , attemptsRemaining(pol.maxRetries), subsystem(sub) {}
};

// Static callback function that implements retry logic
// This matches Apple's IOFWAsyncCommand::complete() pattern where retries
// are decremented and execute() is called again on transient failures
// Signature matches CompletionCallback: (AsyncHandle, AsyncStatus, std::span<const uint8_t>)
void ReadWithRetryCallback(AsyncHandle handle, AsyncStatus status, std::span<const uint8_t> responsePayload, RetryState* state) {
    
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
        // TODO: Add speed fallback on type error (Apple's pattern in IOFWReadCommand::gotPacket)
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
                IOSleep(delayMs);  // Convert µs to ms
            }
        }
        
        // Re-submit transaction (Apple pattern: call execute() again)
        // Note: This creates a new handle - cancellation of original handle won't
        // affect retries. This matches Apple's behavior where commands are atomic.
        state->currentHandle = state->subsystem->Read(
            state->params,
            [state](AsyncHandle h, AsyncStatus s, std::span<const uint8_t> payload) {
                ReadWithRetryCallback(h, s, payload, state);
            }
        );
        
        if (!state->currentHandle) {
            // Retry submission failed - invoke user callback with error
            ASFW_LOG_ERROR(Async, "ReadWithRetry: Re-submission failed after %{public}s", 
                          retryReason);
            if (state->userCallback) {
                state->userCallback(handle, AsyncStatus::kHardwareError, std::span<const uint8_t>{});
            }
            delete state;
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
            state->userCallback(handle, status, responsePayload);
        }
        delete state;  // Free retry state
    }
}

struct PayloadContext {
    PayloadContext() = default;
    ~PayloadContext() {
        Reset();
    }

    PayloadContext(const PayloadContext&) = delete;
    PayloadContext& operator=(const PayloadContext&) = delete;

    bool Initialize(Driver::HardwareInterface& hw,
                    const void* logicalData,
                    std::size_t length,
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
                    /*options*/0,
                    /*offset*/0,
                    static_cast<uint64_t>(length));
                if (syncKr != kIOReturnSuccess) {
                    ASFW_LOG(Async,
                             "PayloadContext(Stream): Synchronize failed kr=0x%x len=%zu",
                             syncKr,
                             length);
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

    [[nodiscard]] uint64_t DeviceAddress() const noexcept {
        return dmaBuffer_.deviceAddress;
    }

    [[nodiscard]] uint8_t* VirtualAddress() const noexcept {
        return virtualAddress_;
    }

    [[nodiscard]] const void* LogicalAddress() const noexcept {
        return logicalAddress_;
    }

    [[nodiscard]] std::size_t Length() const noexcept {
        return length_;
    }

private:
    Driver::HardwareInterface::DMABuffer dmaBuffer_{};
    IOMemoryMap* mapping_{nullptr};
    uint8_t* virtualAddress_{nullptr};
    const void* logicalAddress_{nullptr};
    std::size_t length_{0};
};

// Phase 2.0: CleanupPayloadContext and AttachPayloadContext removed (replaced by PayloadHandle RAII)
}

kern_return_t AsyncSubsystem::Start(Driver::HardwareInterface& hw,
                                    OSObject* owner,
                                    IODispatchQueue* workloopQueue,
                                    OSAction* completionAction,
                                    size_t completionQueueCapacityBytes) {
    if (isRunning_) {
        ASFW_LOG(Async, "Already running, returning success");
        return kIOReturnSuccess;
    }

    if (!owner || !workloopQueue || !completionAction) {
        ASFW_LOG(Async, "Bad arguments: owner=%p queue=%p action=%p",
               owner, workloopQueue, completionAction);
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

    kern_return_t kr = kIOReturnSuccess;
    const char* failureStage = nullptr;

    // Legacy per-ring physical/virtual bookkeeping removed: ContextManager owns DMA

    // Create components for Tracking actor (must be before goto labels)
    auto labelAllocator = std::make_unique<LabelAllocator>();
    labelAllocator->Reset();

    // Pre-declare variables that might be jumped over by goto
    Result<void> txnMgrResult = {};
    std::unique_ptr<CompletionQueue> completionQueue;

    sharedLock_ = ::IOLockAlloc();
    if (!sharedLock_) {
        kr = kIOReturnNoMemory;
        failureStage = "AllocSharedLock";
        goto fail;
    }
    
    // Initialize command queue for serialized execution (Apple IOFWCmdQ pattern)
    commandQueue_ = std::make_unique<std::deque<PendingCommand>>();
    commandQueueLock_ = ::IOLockAlloc();
    if (!commandQueueLock_) {
        kr = kIOReturnNoMemory;
        failureStage = "AllocCommandQueueLock";
        goto fail;
    }
    commandInFlight_.store(false, std::memory_order_release);

    // Phase 2.0: Initialize TransactionManager (replaces OutstandingTable, ResponseMatcher, TimeoutEngine)
    txnMgr_ = std::make_unique<TransactionManager>();
    txnMgrResult = txnMgr_->Initialize();
    if (!txnMgrResult) {
        txnMgrResult.error().Log();  // Phase 2.1: Rich error context with source location
        kr = txnMgrResult.error().kr;
        failureStage = "TransactionManager";
        goto fail;
    }

    // Create GenerationTracker after labelAllocator per dependency ordering
    generationTracker_ = std::make_unique<ASFW::Async::Bus::GenerationTracker>(*labelAllocator);
    // Ensure internal tracker state initialized (clears local NodeID and 8-bit generation)
    if (generationTracker_) {
        generationTracker_->Reset();
    }

    packetBuilder_ = std::make_unique<PacketBuilder>();

    {
        kr = CompletionQueue::Create(workloopQueue_,
                                     completionQueueCapacityBytes,
                                     completionAction_.get(),
                                     completionQueue);
        if (kr != kIOReturnSuccess || !completionQueue) {
            ASFW_LOG(Async, "FAILED: CompletionQueue::Create returned 0x%08x", kr);
            failureStage = "CompletionQueue";
            goto fail;
        }
        completionQueue_ = std::move(completionQueue);

        // CRITICAL: Activate queue and mark client as bound BEFORE starting producers
        // This prevents crashes from enqueueing to an unactivated queue
        completionQueue_->SetClientBound();
        completionQueue_->Activate();
    }

    // Initialize Tracking actor
    // Pass raw pointers since we still need these components in AsyncSubsystem
    // Phase 2.0: Create Tracking actor with TransactionManager
    // NOTE: contextManager_ will be initialized later, so we pass nullptr now
    // and update it in Start() after contextManager_ is created
    tracking_ = std::make_unique<Track_Tracking<CompletionQueue>>(
        labelAllocator.get(),
        txnMgr_.get(),
        *completionQueue_,
        nullptr  // contextManager_ not yet created
    );

    // NEW: Context Architecture (Phase 4) — owned by ContextManager
    {
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
        spec.arReqBufCount  = kARReqBufferCount;
        spec.arReqBufSize   = kARReqBufferSize;
        spec.arRespBufCount = kARRespBufferCount;
        spec.arRespBufSize  = kARRespBufferSize;

        kern_return_t pkr = contextManager_->provision(*hardware_, spec);
        if (pkr != kIOReturnSuccess) {
            ASFW_LOG(Async, "FAILED: ContextManager::provision (kr=0x%08x)", pkr);
            kr = pkr;
            failureStage = "ContextManagerProvision";
            goto fail;
        }

        DMAMemoryManager::SetTracingEnabled(ShouldEnableCoherencyTrace(owner_));
        if (DMAMemoryManager::IsTracingEnabled()) {
            ASFW_LOG(Async, "AsyncSubsystem: coherency tracing enabled (ASFWTraceDMACoherency)");
        }

        // Use ContextManager-owned contexts/rings through resolver helpers.
        // Build descriptor builder using ContextManager resources.
        descriptorBuilder_ = std::make_unique<DescriptorBuilder>(*contextManager_->AtRequestRing(), *contextManager_->DmaManager());

        // Construct Submitter (two-path TX FSM) using ContextManager-owned resources
        submitter_ = std::make_unique<Tx::Submitter>(*contextManager_, *descriptorBuilder_);
        // Wire payload registry (owned by Tracking actor) into ContextManager and Submitter
        if (tracking_ && contextManager_) {
            contextManager_->SetPayloads(tracking_->Payloads());
            if (submitter_) submitter_->SetPayloads(tracking_->Payloads());
            
            // Wire ContextManager into Tracking so response processing can reuse
            // shared helpers (label matcher, outstanding table, payload registry)
            tracking_->SetContextManager(contextManager_.get());
        }

        // Initialize packet router and RxPath (uses contexts from ContextManager)
        packetRouter_ = std::make_unique<PacketRouter>();
        rxPath_ = std::make_unique<Rx::RxPath>(*contextManager_->GetArRequestContext(),
                                               *contextManager_->GetArResponseContext(),
                                               *tracking_,
                                               *generationTracker_,
                                               *packetRouter_);

        ASFW_LOG(Async, "✓ ContextManager provisioned and Rx/Tx helpers initialized");
    }

    busResetCapture_ = std::make_unique<Debug::BusResetPacketCapture>();

    hardware_->SetInterruptMask(kAsyncInterruptMask, true);
    hardware_->SetLinkControlBits(kLinkControlRcvPhyPktBit);

    // Report DMA cache mode (always uncached since kIOMemoryMapCacheModeInhibit works reliably)
    ASFW_LOG_TYPE(Async, OS_LOG_TYPE_INFO,
                  "AsyncSubsystem::Start complete (DMA always uncached)");

    watchdogTickCount_.store(0, std::memory_order_relaxed);
    watchdogExpiredCount_.store(0, std::memory_order_relaxed);
    watchdogDrainedCompletions_.store(0, std::memory_order_relaxed);
    watchdogContextsRearmed_.store(0, std::memory_order_relaxed);
    watchdogLastTickUsec_.store(0, std::memory_order_relaxed);

    is_bus_reset_in_progress_.store(0, std::memory_order_release);
    isRunning_ = true;
    return kIOReturnSuccess;

fail:
    ASFW_LOG_TYPE(Async, OS_LOG_TYPE_ERROR,
                  "AsyncSubsystem::Start failed at stage %{public}s (kr=0x%08x)",
                  failureStage ? failureStage : "unknown", kr);
    Teardown(false);
    if (kr == kIOReturnSuccess) {
        kr = kIOReturnError;
    }
    return kr;
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

    ASFW_LOG(Async, "Phase 2B: Arming AR contexts only via ContextManager (receive)");
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
        ASFW_LOG(Async, "Teardown: ContextManager not present - nothing to teardown (legacy owners removed)");
    }

    completionQueue_.reset();
    completionAction_.reset();

    // Phase 2.0: Clean up TransactionManager (replaces timeoutEngine_, responseMatcher_, outstanding_)
    if (txnMgr_) {
        txnMgr_->CancelAll();  // Cancel all in-flight transactions
        txnMgr_.reset();
    }

    descriptorBuilder_.reset();
    packetBuilder_.reset();

    // Destroy GenerationTracker
    generationTracker_.reset();

    // Legacy per-ring bookkeeping removed - nothing to reset here

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
        ASFW_LOG_ERROR(Async, "PrepareTransactionContext: NodeID valid bit not set (reg=0x%08x)", nodeIdReg);
        return std::nullopt;
    }
    const uint16_t sourceNodeID = static_cast<uint16_t>(nodeIdReg & 0xFFFFu);
    
    // Step 4: Query current generation from GenerationTracker
    const auto busState = generationTracker_->GetCurrentState();
    const uint8_t currentGeneration = busState.generation8;
    
    // Step 5: Resolve speed code (TODO: query TopologyManager, default S100 for compatibility)
    const uint8_t speedCode = kDefaultAsyncSpeed;  // S100 (98.304 Mbps)
    
    // Step 6: Build TransactionContext with PacketContext
    TransactionContext txCtx{};
    txCtx.sourceNodeID = sourceNodeID;
    txCtx.generation = currentGeneration;
    txCtx.speedCode = speedCode;
    txCtx.packetContext = PacketContext{sourceNodeID, currentGeneration, speedCode};
    
    return txCtx;
}

uint64_t AsyncSubsystem::GetCurrentTimeUsec() const {
    return GetCurrentMonotonicTimeUsec();
}

// ============================================================================
// Transaction APIs (CRTP Command Dispatch - Phase 2.3: std::function)
// ============================================================================

AsyncHandle AsyncSubsystem::Read(const ReadParams& params, CompletionCallback callback) {
    return ReadCommand{params, std::move(callback)}.Submit(*this);
}

AsyncHandle AsyncSubsystem::Write(const WriteParams& params, CompletionCallback callback) {
    return WriteCommand{params, std::move(callback)}.Submit(*this);
}

AsyncHandle AsyncSubsystem::Lock(const LockParams& params,
                                 uint16_t extendedTCode,
                                 CompletionCallback callback) {
    return LockCommand{params, extendedTCode, std::move(callback)}.Submit(*this);
}

namespace {
struct CompareSwapOperandStorage {
    std::array<uint32_t, 2> beOperands{};
    uint32_t compareHost{0};
};
} // namespace

AsyncHandle AsyncSubsystem::CompareSwap(const CompareSwapParams& params,
                                        CompareSwapCallback callback) {
    auto storage = std::make_shared<CompareSwapOperandStorage>();
    storage->compareHost      = params.compareValue;
    storage->beOperands[0]    = OSSwapHostToBigInt32(params.compareValue);
    storage->beOperands[1]    = OSSwapHostToBigInt32(params.swapValue);

    LockParams lockParams{};
    lockParams.destinationID   = params.destinationID;
    lockParams.addressHigh     = params.addressHigh;
    lockParams.addressLow      = params.addressLow;
    lockParams.operand         = storage->beOperands.data();
    lockParams.operandLength   = static_cast<uint32_t>(storage->beOperands.size() * sizeof(uint32_t));
    lockParams.responseLength  = sizeof(uint32_t);
    lockParams.speedCode       = params.speedCode;

    const uint16_t kExtendedTCodeCompareSwap = 0x02;

    CompletionCallback internalCallback = [callback, storage](AsyncHandle,
                                                             AsyncStatus status,
                                                             std::span<const uint8_t> payload) {
        if (status != AsyncStatus::kSuccess) {
            callback(status, 0u, false);
            return;
        }

        if (payload.size() != sizeof(uint32_t)) {
            callback(AsyncStatus::kHardwareError, 0u, false);
            return;
        }

        uint32_t raw = 0;
        std::memcpy(&raw, payload.data(), sizeof(uint32_t));
        uint32_t oldValueHost = OSSwapBigToHostInt32(raw);
        const bool matched = (oldValueHost == storage->compareHost);
        callback(AsyncStatus::kSuccess, oldValueHost, matched);
    };

    return Lock(lockParams, kExtendedTCodeCompareSwap, std::move(internalCallback));
}

AsyncHandle AsyncSubsystem::PhyRequest(const PhyParams& params,
                                      CompletionCallback callback) {
    return PhyCommand{params, std::move(callback)}.Submit(*this);
}

AsyncHandle AsyncSubsystem::Stream(const StreamParams& /* params */) {
    // TODO: Implement stream packet support
    ASFW_LOG_ERROR(Async, "Stream packets not yet implemented");
    return AsyncHandle{0};
}

// OLD Read() implementation below - will be removed after validation

// ReadWithRetry() - Queue-based retry wrapper following Apple's IOFWCmdQ pattern
// Reference: IOFWCmdQ::executeQueue() - enqueues command and triggers sequential execution
AsyncHandle AsyncSubsystem::ReadWithRetry(const ReadParams& params,
                                         const RetryPolicy& retryPolicy,
                                         CompletionCallback callback) {
    if (!commandQueue_ || !commandQueueLock_) {
        ASFW_LOG_ERROR(Async, "ReadWithRetry: Queue not initialized");
        return AsyncHandle{0};  // Invalid handle
    }
    
    // Allocate placeholder handle (will be assigned when command executes)
    static std::atomic<uint32_t> sNextQueuedHandle{0x80000000};  // High bit = queued
    AsyncHandle placeholderHandle{sNextQueuedHandle.fetch_add(1, std::memory_order_relaxed)};
    
    ::IOLockLock(commandQueueLock_);
    
    // Create pending command with subsystem back-pointer for callback access
    commandQueue_->emplace_back(params, retryPolicy, callback, 
                               placeholderHandle, this);
    const size_t queueDepth = commandQueue_->size();
    const bool wasIdle = !commandInFlight_.load(std::memory_order_acquire);
    
    ::IOLockUnlock(commandQueueLock_);
    
    ASFW_LOG(Async, "📥 Queued read request: dest=%04x addr=%08x:%08x len=%u handle=0x%x (queue depth=%zu)",
             params.destinationID, params.addressHigh, params.addressLow,
             params.length, placeholderHandle.value, queueDepth);
    
    // If queue was idle, kick off execution
    if (wasIdle) {
        ASFW_LOG(Async, "🚀 Queue was idle, starting execution");
        ExecuteNextCommand();
    }
    
    return placeholderHandle;
}

bool AsyncSubsystem::Cancel(AsyncHandle /*handle*/) {
    // TODO: locate outstanding request and issue cancel workflow.
    return false;
}

// Phase 2.0: ProcessTxCompletion removed (replaced by TransactionCompletionHandler)

uint32_t AsyncSubsystem::DrainTxCompletions(const char* reason) {
    if (!tracking_) {
        return 0;
    }

    uint32_t drained = 0;
    // CRITICAL: Only call ScanCompletion() - it properly rejects evt_no_status
    // Never bypass ScanCompletion() checks or advance ring head directly.
    // ScanCompletion() will return nullopt for evt_no_status without advancing head.
    auto scanContext = [&](auto* ctx) {
        if (!ctx) {
            return;
        }
        while (auto completion = ctx->ScanCompletion()) {
            tracking_->OnTxCompletion(*completion);
            ++drained;
        }
    };

    scanContext(ResolveAtRequestContext());
    scanContext(ResolveAtResponseContext());

    if (drained > 0 && reason) {
        ASFW_LOG(Async,
                 "DrainTxCompletions: reason=%{public}s drained=%u",
                 reason,
                 drained);
    }

    return drained;
}

ATRequestContext* AsyncSubsystem::ResolveAtRequestContext() noexcept {
    if (contextManager_) return contextManager_->GetAtRequestContext();
    return nullptr;
}

ATResponseContext* AsyncSubsystem::ResolveAtResponseContext() noexcept {
    if (contextManager_) return contextManager_->GetAtResponseContext();
    return nullptr;
}

ARRequestContext* AsyncSubsystem::ResolveArRequestContext() noexcept {
    if (contextManager_) return contextManager_->GetArRequestContext();
    return nullptr;
}

ARResponseContext* AsyncSubsystem::ResolveArResponseContext() noexcept {
    if (contextManager_) return contextManager_->GetArResponseContext();
    return nullptr;
}

void AsyncSubsystem::OnTxInterrupt() {
    if (!isRunning_ || is_bus_reset_in_progress_.load(std::memory_order_acquire)) {
        return;  // Ignore completions during bus reset
    }

    (void)DrainTxCompletions("irq");
}

void AsyncSubsystem::OnRxInterrupt(ARContextType /*contextType*/) {
    if (rxPath_) {
        rxPath_->ProcessARInterrupts(is_bus_reset_in_progress_, isRunning_, busResetCapture_.get());
    }
    
    // No bus-reset work here. AR IRQ ≠ bus reset.
}

void AsyncSubsystem::OnBusResetBegin(uint8_t nextGen) {
    // CRITICAL: Follow Linux core-transaction.c:fw_core_handle_bus_reset() ordering
    // 1. Gate new submissions FIRST
    // 2. Cancel OLD generation transactions SECOND
    // 3. Let HARDWARE set generation via synthetic bus reset packet
    // This ordering prevents race: generation comes from hardware, not manual increment
    
    // Step 1: Gate new submissions
    // Any new RegisterTx() calls will be blocked until bus reset completes
    is_bus_reset_in_progress_.store(1, std::memory_order_release);
    
    // NOTE: Generation will be updated by hardware via synthetic bus reset packet
    // in RxPath, NOT manually here. This prevents race between OnBusResetBegin
    // and the AR synthetic packet handler.
    
    // Step 2: Cancel transactions from OLD generation only
    // Read current generation from tracker (set by previous bus reset)
    const uint8_t oldGen = generationTracker_ ? generationTracker_->GetCurrentState().generation8 : 0;

    if (tracking_) {
        // Cancel any lingering transactions (all generations) to guarantee label bitmap is clean.
        tracking_->CancelAllAndFreeLabels();
        // Cancel transactions belonging to oldGen (precise, not ~0u!)
        tracking_->CancelByGeneration(oldGen);

        // Hard-clear bitmap to evict any leaked bits that lack corresponding transactions
        if (auto* alloc = tracking_->GetLabelAllocator()) {
            alloc->ClearBitmap();
        }
    }
    
    // Step 3: Bump payload epoch for deferred cleanup (to nextGen)
    if (tracking_ && tracking_->Payloads()) {
        tracking_->Payloads()->SetEpoch(nextGen);
    }
    
    ASFW_LOG(Async, "OnBusResetBegin: cancelled oldGen=%u transactions, payload epoch→%u (hw will set gen)",
             oldGen, nextGen);
}

void AsyncSubsystem::OnBusResetComplete(uint8_t stableGen) {
    is_bus_reset_in_progress_.store(0, std::memory_order_release);
    ASFW_LOG(Async, "OnBusResetComplete: gen=%u", stableGen);
}

void AsyncSubsystem::RearmATContexts() {
    // OHCI §7.2.3.2 Step 7: Re-arm AT contexts after busReset cleared
    // CRITICAL: This is called by ControllerCore AFTER:
    //   1. AT contexts stopped (active=0)
    //   2. IntEvent.busReset cleared
    //   3. Self-ID complete
    //   4. Config ROM restored
    //   5. AsynchronousRequestFilter re-enabled
    //
    // Calling this earlier (e.g., in OnBusReset) prevents busReset clearing because
    // ControllerCore checks AT contexts are inactive before clearing the interrupt.

    ASFW_LOG(Async, "Re-arming AT contexts for new generation (OHCI §7.2.3.2 step 7)");

    // Step 6 from §7.2.3.2: Read NodeID (should be valid - Self-ID already completed)
    // CRITICAL: No polling here! This is still called from interrupt context.
    // Since we only call RearmATContexts() AFTER Self-ID complete, NodeID should be valid.
    if (hardware_) {
        constexpr uint32_t kNodeIDValidBit = 0x80000000u;
        constexpr uint16_t kUnassignedBus = 0x03FFu;

        uint32_t nodeIdReg = 0;

        // Poll briefly for NodeID valid. Keep the wait bounded (~10 ms) since we're
        // still on the interrupt workloop.
        for (uint32_t attempt = 0; attempt < 100; ++attempt) {
            nodeIdReg = hardware_->ReadNodeID();
            if ((nodeIdReg & kNodeIDValidBit) != 0) {
                break;
            }
            IODelay(100); // 100 µs
        }

        const bool idValid = (nodeIdReg & kNodeIDValidBit) != 0;
        if (!idValid) {
            if (generationTracker_) {
                generationTracker_->OnSelfIDComplete(0);
            }
            ASFW_LOG(Async,
                     "WARNING: NodeID never reported valid state (reg=0x%08x). "
                     "Async transmit remains gated.",
                     nodeIdReg);
        } else {
            const uint16_t rawBus = static_cast<uint16_t>((nodeIdReg >> 6) & 0x03FFu);
            const uint8_t nodeNumber = static_cast<uint8_t>(nodeIdReg & 0x3F);
            // Per IEEE 1394-1995 §8.3.2.3.2: source_ID uses broadcast bus (0x3ff) if unassigned
            // NEVER substitute bus=0, as it's semantically different from unassigned broadcast bus
            const uint16_t nodeID = static_cast<uint16_t>((rawBus << 6) | nodeNumber);

            if (generationTracker_) {
                generationTracker_->OnSelfIDComplete(nodeID);
            }

            if (rawBus == kUnassignedBus) {
                ASFW_LOG(Async,
                         "NodeID valid: using broadcast bus (0x3ff) for source field (raw=0x%08x node=%u)",
                         nodeIdReg,
                         nodeNumber);
            } else {
                ASFW_LOG(Async,
                         "NodeID locked: bus=%u node=%u (raw=0x%08x)",
                         rawBus,
                         nodeNumber,
                         nodeIdReg);
            }
        }
    }

    // Re-arm AT contexts: ContextManager is the authoritative owner.
    if (!contextManager_) {
        ASFW_LOG(Async, "RearmATContexts: ContextManager unavailable - cannot rearm");
        return;
    }

    // ContextManager owns contexts and manages the DMA; AT contexts remain
    // idle until the first SubmitChain() per Apple's implementation.
    ASFW_LOG(Async, "RearmATContexts: handled by ContextManager (AT contexts remain idle)");
    return;
}

bool AsyncSubsystem::EnsureATContextsRunning(const char* reason) {
    // Per Apple's implementation: AT contexts are NOT pre-armed.
    // They arm themselves during SubmitChain() when transitioning from idle→active.
    // This function is retained for API compatibility but no longer attempts re-arming.
    (void)reason;  // Unused
    return false;
}

std::optional<AsyncStatusSnapshot> AsyncSubsystem::GetStatusSnapshot() const {
    if (!contextManager_) {
        return std::nullopt;
    }
    AsyncStatusSnapshot snapshot{};
    if (contextManager_) {
        auto* dm = contextManager_->DmaManager();
        if (dm) {
            snapshot.dmaSlabVirt = reinterpret_cast<uint64_t>(dm->BaseVirtual());
            snapshot.dmaSlabIOVA = dm->BaseIOVA();
            snapshot.dmaSlabSize = static_cast<uint32_t>(dm->TotalSize());
        }
    }

    auto populateDescriptor = [](AsyncDescriptorStatus& out,
                                 const DescriptorRing* ring,
                                 const uint8_t* virt,
                                 uint64_t iova,
                                 uint32_t commandPtr,
                                 uint32_t count,
                                 uint32_t stride,
                                 uint32_t strideFallback) {
        out.descriptorVirt = reinterpret_cast<uint64_t>(virt);
        out.descriptorIOVA = iova;
        if (count == 0 && ring != nullptr) {
            count = static_cast<uint32_t>(ring->Capacity() + 1); // include sentinel slot
        }
        out.descriptorCount = count;
        out.descriptorStride = stride != 0 ? stride : strideFallback;
        out.commandPtr = commandPtr;
    };

    auto populateBuffers = [](AsyncBufferStatus& out,
                              const BufferRing* ring,
                              const uint8_t* virt,
                              uint64_t iova) {
        out.bufferVirt = reinterpret_cast<uint64_t>(virt);
        out.bufferIOVA = iova;
        if (ring) {
            out.bufferCount = static_cast<uint32_t>(ring->BufferCount());
            out.bufferSize = static_cast<uint32_t>(ring->BufferSize());
        }
    };

    // Populate descriptor info from ContextManager when present
    // Populate descriptor info and buffer rings from ContextManager
    {
        auto* atReqRing = contextManager_->AtRequestRing();
        populateDescriptor(snapshot.atRequest,
                           atReqRing,
                           nullptr,
                           0,
                           0,
                           0,
                           0,
                           static_cast<uint32_t>(sizeof(HW::OHCIDescriptorImmediate)));
    }
    {
        auto* atRspRing = contextManager_->AtResponseRing();
        populateDescriptor(snapshot.atResponse,
                           atRspRing,
                           nullptr,
                           0,
                           0,
                           0,
                           0,
                           static_cast<uint32_t>(sizeof(HW::OHCIDescriptorImmediate)));
    }
    {
        auto* arReqRing = contextManager_->ArRequestRing();
        populateDescriptor(snapshot.arRequest,
                           nullptr,
                           nullptr,
                           0,
                           0,
                           0,
                           0,
                           static_cast<uint32_t>(sizeof(HW::OHCIDescriptor)));
        populateBuffers(snapshot.arRequestBuffers,
                        arReqRing,
                        nullptr,
                        0);
    }
    {
        auto* arRspRing = contextManager_->ArResponseRing();
        populateDescriptor(snapshot.arResponse,
                           nullptr,
                           nullptr,
                           0,
                           0,
                           0,
                           0,
                           static_cast<uint32_t>(sizeof(HW::OHCIDescriptor)));
        populateBuffers(snapshot.arResponseBuffers,
                        arRspRing,
                        nullptr,
                        0);
    }

    return snapshot;
}

AsyncSubsystem::WatchdogStats AsyncSubsystem::GetWatchdogStats() const {
    WatchdogStats stats{};
    stats.tickCount = watchdogTickCount_.load(std::memory_order_relaxed);
    stats.expiredTransactions = watchdogExpiredCount_.load(std::memory_order_relaxed);
    stats.drainedTxCompletions = watchdogDrainedCompletions_.load(std::memory_order_relaxed);
    stats.contextsRearmed = watchdogContextsRearmed_.load(std::memory_order_relaxed);
    stats.lastTickUsec = watchdogLastTickUsec_.load(std::memory_order_relaxed);
    return stats;
}

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

void AsyncSubsystem::StopATContextsOnly() {
    // Bus Reset Recovery per OHCI §7.2.3.2, §C.2
    // CRITICAL: Only stop AT contexts - AR contexts continue processing
    // Called by BusResetCoordinator during QuiescingAT state
    if (contextManager_) {
        const kern_return_t stopKr = contextManager_->stopAT();
        if (stopKr != kIOReturnSuccess) {
            ASFW_LOG(Async, "StopATContextsOnly: ContextManager::stopAT failed (kr=0x%08x)", stopKr);
        }
    } else {
        ASFW_LOG(Async, "StopATContextsOnly: ContextManager not present - nothing to stop");
    }
    // Notify Submitter that AT contexts have been stopped so it can reset internal state
    if (submitter_) {
        submitter_->OnATContextsStopped();
    }
    // DO NOT stop AR contexts - they continue per §C.3
}

void AsyncSubsystem::FlushATContexts() {
    // Phase 2C-2E: Flush AT contexts to process pending descriptors
    // CRITICAL: Must be called BEFORE clearing busReset interrupt
    // Process any completed descriptors in AT rings
    if (!txnMgr_) {
        return;
    }
    (void)DrainTxCompletions(nullptr);
}

void AsyncSubsystem::ConfirmBusGeneration(uint8_t confirmedGeneration) {
    // Coordinate bus reset based on synthetic packet from controller
    // CRITICAL: This is called when AR Request receives the Bus-Reset packet
    // BEFORE the main interrupt handler sees IntEvent.busReset
    //
    // Linux equivalent: handle_ar_packet() evt_bus_reset → fw_core_handle_bus_reset()
    // Updates generation, gates AT contexts, keeps AR running

    // ConfirmBusGeneration: called when a new generation is confirmed (e.g. after
    // Self-ID decoding). This is the AUTHORITATIVE generation from SelfIDCount register.
    // This is the ONLY place where generation should be set.
    ASFW_LOG(Async, "ConfirmBusGeneration: Confirmed generation %u (from SelfIDCount register)", confirmedGeneration);
    
    const auto currentState = generationTracker_ ? generationTracker_->GetCurrentState() : Bus::GenerationTracker::BusState{};
    
    // Set the generation from hardware (SelfIDCount register is authoritative per OHCI §11.2)
    if (generationTracker_) {
        generationTracker_->OnSyntheticBusReset(confirmedGeneration);
        ASFW_LOG(Async, "GenerationTracker updated: %u→%u", currentState.generation8, confirmedGeneration);
    }
    
    // No redundant cancel here - already done in OnBusResetBegin
    if (tracking_) {
        ASFW_LOG(Async, "Generation confirmed via Tracking actor (no redundant cancel)");
    }

    // Annexe C behavior: cancel request payloads belonging to the old generation
    // but keep AR contexts running. Use PayloadRegistry to cancel payloads
    // from the previous 8-bit generation. We treat generation numbers as 8-bit
    // and cancel payloads with epoch <= oldGen.
    if (contextManager_) {
        auto* pr = contextManager_->Payloads();
        if (pr) {
            // Compute previous generation (wrap-around aware)
            const uint32_t oldGen = (confirmedGeneration == 0) ? 0xFFu : (static_cast<uint32_t>(confirmedGeneration) - 1u);
            pr->CancelByEpoch(oldGen, PayloadRegistry::CancelMode::Deferred);
            // Advance registry epoch to the confirmed generation so new submissions
            // are tagged with the new epoch.
            pr->SetEpoch(static_cast<uint32_t>(confirmedGeneration));
            ASFW_LOG(Async, "PayloadRegistry: canceled epoch <= %u and set epoch=%u", oldGen, confirmedGeneration);
        }
    }

    ASFW_LOG(Async, "ConfirmBusGeneration complete - async subsystem coordinated for new generation");
}

// ApplyBusGeneration has been moved to GenerationTracker. AsyncSubsystem delegates
// generation updates to generationTracker_ to centralize generation logic and
// preserve interrupt-safe semantics.

void AsyncSubsystem::DumpState() {
    // TODO: emit structured diagnostics for debugging.
}

// ============================================================================
// Command Queue Implementation (Apple IOFWCmdQ pattern)
// ============================================================================

void AsyncSubsystem::ExecuteNextCommand() {
    if (!commandQueueLock_ || !commandQueue_) {
        return;
    }
    
    ::IOLockLock(commandQueueLock_);
    
    if (commandQueue_->empty()) {
        commandInFlight_.store(false, std::memory_order_release);
        ::IOLockUnlock(commandQueueLock_);
        ASFW_LOG(Async, "📭 Command queue empty - going idle");
        return;
    }
    
    // Dequeue next command (Apple IOFWCmdQ pattern: remove from queue before execution)
    PendingCommand cmd = std::move(commandQueue_->front());
    commandQueue_->pop_front();
    const size_t remainingCommands = commandQueue_->size();
    commandInFlight_.store(true, std::memory_order_release);
    
    ::IOLockUnlock(commandQueueLock_);
    
    ASFW_LOG(Async, "📤 Executing queued command to %04x addr=%08x:%08x len=%u retries=%u (queue depth=%zu)",
             cmd.params.destinationID, cmd.params.addressHigh, cmd.params.addressLow,
             cmd.params.length, cmd.retriesRemaining, remainingCommands);
    
    // Allocate heap copy with subsystem back-pointer for callback access
    auto* cmdCopy = new PendingCommand(cmd);
    
    // Static wrapper for internal callback (handles retry + queue advancement)
    struct InternalCallbackContext {
        static void HandleCompletion(AsyncHandle handle, AsyncStatus status, std::span<const uint8_t> responsePayload, PendingCommand* cmdPtr) {
            AsyncSubsystem* subsystem = cmdPtr->subsystem;
            
            if (status == AsyncStatus::kSuccess) {
                ASFW_LOG(Async, "✅ Command completed successfully: handle=0x%x", handle.value);
                
                // Invoke user callback with success
                if (cmdPtr->userCallback) {
                    cmdPtr->userCallback(handle, status, responsePayload);
                }
                
                delete cmdPtr;
                subsystem->ExecuteNextCommand();  // Advance to next command
                return;  // CRITICAL: prevent fall-through to failure path
                
            } else if (cmdPtr->retriesRemaining > 0) {
                // Check if we should retry based on policy
                bool shouldRetry = false;
                if (status == AsyncStatus::kTimeout && cmdPtr->retryPolicy.retryOnTimeout) {
                    shouldRetry = true;
                } else if (status == AsyncStatus::kBusyRetryExhausted && cmdPtr->retryPolicy.retryOnBusy) {
                    shouldRetry = true;
                }
                
                if (shouldRetry) {
                    cmdPtr->retriesRemaining--;
                    ASFW_LOG(Async, "🔄 Command failed (status=%u), retrying (%u attempts left)",
                             static_cast<unsigned>(status), cmdPtr->retriesRemaining);
                    
                    // Re-submit immediately (already dequeued, so no queue push)
                    AsyncHandle retryHandle = subsystem->Read(
                        cmdPtr->params,
                        [cmdPtr](AsyncHandle h, AsyncStatus s, std::span<const uint8_t> payload) {
                            HandleCompletion(h, s, payload, cmdPtr);
                        }
                    );
                    
                    cmdPtr->handle = retryHandle;  // Update handle for tracking
                    return;  // Don't delete cmdPtr or advance queue yet
                }
            }
            
            // No retry or retries exhausted - final failure
            ASFW_LOG(Async, "❌ Command failed permanently: handle=0x%x status=%u",
                     handle.value, static_cast<unsigned>(status));
            
            if (cmdPtr->userCallback) {
                cmdPtr->userCallback(handle, status, responsePayload);
            }
            
            delete cmdPtr;
            subsystem->ExecuteNextCommand();  // Move to next command
        }
    };
    
    // Submit command to hardware layer
    AsyncHandle handle = Read(cmd.params, [cmdCopy](AsyncHandle h, AsyncStatus s, std::span<const uint8_t> payload) {
        InternalCallbackContext::HandleCompletion(h, s, payload, cmdCopy);
    });
    cmdCopy->handle = handle;
    
    ASFW_LOG(Async, "📮 Command submitted: handle=0x%x", handle.value);
}

} // namespace ASFW::Async
