#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

#include <DriverKit/IOReturn.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSObject.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/IODispatchQueue.h>

#include "AsyncTypes.hpp"
#include "Bus/GenerationTracker.hpp"
#include "Track/CompletionQueue.hpp"
#include "Track/Tracking.hpp"
#include "Rx/RxPath.hpp"

namespace ASFW::Driver {
class HardwareInterface;
}

namespace ASFW::Debug {
class BusResetPacketCapture;
}

namespace ASFW::Async {
// Forward declarations
class ResetHook;
class AsyncMetricsSink;
class DescriptorBuilder;
class PacketBuilder;
class PacketRouter;

// Forward declaration for Tx Submitter (two-path TX FSM)
namespace Tx { class Submitter; }

// Forward declarations - new architecture
class DMAMemoryManager;
class DescriptorRing;
class BufferRing;
class ATRequestContext;
class ATResponseContext;
class ARRequestContext;
class ARResponseContext;

// Forward declaration for ContextManager (incremental wiring)
namespace Engine { class ContextManager; }

// Forward declarations - Tracking actor
template <typename TCompletionQueue> class Track_Tracking;

// Forward declaration for CRTP command access
template <typename Derived> class AsyncCommand;

class AsyncSubsystem {
public:
    AsyncSubsystem();
    ~AsyncSubsystem();
    
    // Friend declaration: Allow CRTP commands to access private helpers
    template <typename Derived> friend class AsyncCommand;

    enum class ARContextType {
        Request,
        Response,
    };

    kern_return_t Start(Driver::HardwareInterface& hw,
                        OSObject* owner,
                        ::IODispatchQueue* workloopQueue,
                        ::OSAction* completionAction,
                        size_t completionQueueCapacityBytes = 64 * 1024);
    
    // CRITICAL: Must be called AFTER HCControl.linkEnable is set (OHCI §5.5.6, §7.2.1)
    // Arms all DMA contexts (AT Request/Response, AR Request/Response) by writing CommandPtr
    // Calling before linkEnable may cause UnrecoverableError interrupt (descriptor fetch fails)
    kern_return_t ArmDMAContexts();
    
    // Phase 2B: Arm ONLY AR contexts (receive), leaving AT contexts (transmit) disabled
    // Allows testing receive pipeline independently before enabling transmission
    kern_return_t ArmARContextsOnly();
    
    void Stop();

    /// Basic read without retry (single attempt) (Phase 2.3: std::function, no void* context)
    AsyncHandle Read(const ReadParams& params, CompletionCallback callback);

    /// Read with automatic retry on transient errors (BUSY_X, timeout).
    /// Follows Apple's IOFWAsyncCommand retry pattern (fCurRetries/fMaxRetries) but
    /// implemented at transaction level with configurable retry policy.
    ///
    /// @param params      Read parameters (address, size, speed, etc.)
    /// @param retryPolicy Retry configuration (max attempts, backoff strategy)
    /// @param callback    Completion callback (invoked after final attempt)
    /// @return AsyncHandle for tracking/cancellation
    ///
    /// Phase 2.3: Removed void* context (captured in callback lambda)
    ///
    /// Reference: IOFWAsyncCommand.cpp complete() method implements retry logic
    ///            at command level; we implement at transaction level for DriverKit.
    AsyncHandle ReadWithRetry(const ReadParams& params,
                             const RetryPolicy& retryPolicy,
                             CompletionCallback callback);

    AsyncHandle Write(const WriteParams& params, CompletionCallback callback);
    AsyncHandle Lock(const LockParams& params, uint16_t extendedTCode, CompletionCallback callback);
    AsyncHandle Stream(const StreamParams& params);
    AsyncHandle PhyRequest(const PhyParams& params, CompletionCallback callback);
    bool Cancel(AsyncHandle handle);

    /// Post a block to the workloop queue for deferred execution
    /// Used to avoid inline re-entry during completion callbacks
    /// @param block Block to execute on workloop queue
    void PostToWorkloop(void (^block)()) {
        if (workloopQueue_) {
            workloopQueue_->DispatchAsync(block);
        }
    }

    void OnTxInterrupt();
    void OnRxInterrupt(ARContextType contextType);
    void OnBusReset();
    // Bus reset lifecycle hooks (called by BusResetCoordinator FSM)
    void OnBusResetBegin(uint8_t nextGen);
    void OnBusResetComplete(uint8_t stableGen);
    // Public entry to confirm a new bus generation (called after Self-ID decoding)
    // This will update the generation via GenerationTracker and perform subsystem-level
    // coordination (invalidate outstanding transactions, reset response matcher, etc.).
    void ConfirmBusGeneration(uint8_t confirmedGeneration);
    void OnTimeoutTick();

    struct WatchdogStats {
        uint64_t tickCount{0};
        uint64_t expiredTransactions{0};
        uint64_t drainedTxCompletions{0};
        uint64_t contextsRearmed{0};
        uint64_t lastTickUsec{0};
    };

    [[nodiscard]] WatchdogStats GetWatchdogStats() const;

    // Bus reset recovery (OHCI §7.2.3.2): Must be called in this exact sequence:
    // 1. StopATContextsOnly() - halts AT contexts (clears .run, polls .active)
    // 2. FlushATContexts() - processes pending descriptors before busReset clear
    // 3. RearmATContexts() - re-arms AT contexts AFTER busReset cleared
    
    // Internal: Stop AT contexts (clear .run, poll .active until stopped)
    // Per Linux context_stop(): Synchronous blocking call, max 100ms timeout
    void StopATContextsOnly();
    
    // Internal: Flush AT contexts before clearing busReset (Phase 2C-2E)
    void FlushATContexts();

    // Bus reset recovery: Re-arm AT contexts after busReset cleared (OHCI §7.2.3.2 step 7)
    // CRITICAL: Must be called AFTER IntEvent.busReset is cleared and AT contexts are inactive
    // Called by ControllerCore after verifying contexts stopped and clearing busReset interrupt
    void RearmATContexts();

    void DumpState();

    // Access to bus reset packet capture for debugging/metrics
    Debug::BusResetPacketCapture* GetBusResetCapture() const {
        return busResetCapture_.get();
    }

    [[nodiscard]] std::optional<AsyncStatusSnapshot> GetStatusSnapshot() const;
    
    // ========================================================================
    // Helper Accessors for CRTP Commands (friend access only)
    // ========================================================================
    
    /// Prepare transaction context - validates bus state, reads NodeID, queries generation.
    /// Matches Apple's executeCommandElement() gate check pattern (DECOMPILATION.md §Command Execution).
    /// @return TransactionContext with validated bus state, or nullopt on failure
    [[nodiscard]] std::optional<TransactionContext> PrepareTransactionContext();
    
    /// Get current monotonic time in microseconds (for timeout scheduling)
    [[nodiscard]] uint64_t GetCurrentTimeUsec() const;
    
    // Subsystem component accessors for command submission
    [[nodiscard]] Track_Tracking<CompletionQueue>* GetTracking() { return tracking_.get(); }
    [[nodiscard]] DescriptorBuilder* GetDescriptorBuilder() { return descriptorBuilder_.get(); }
    [[nodiscard]] Tx::Submitter* GetSubmitter() { return submitter_.get(); }
    [[nodiscard]] Driver::HardwareInterface* GetHardware() { return hardware_; }

private:
    std::atomic<uint32_t> is_bus_reset_in_progress_{0};

    Driver::HardwareInterface* hardware_{nullptr};
    ::OSObject* owner_{nullptr};
    ::IODispatchQueue* workloopQueue_{nullptr};
    ::IOLock* sharedLock_{nullptr};

    std::unique_ptr<ASFW::Async::Bus::GenerationTracker> generationTracker_;
    std::unique_ptr<DescriptorBuilder> descriptorBuilder_;
    std::unique_ptr<PacketBuilder> packetBuilder_;

    // Phase 2.0: Transaction infrastructure (owned by AsyncSubsystem)
    std::unique_ptr<TransactionManager> txnMgr_;

    // Tracking Actor (Phase 5)
    std::unique_ptr<Track_Tracking<CompletionQueue>> tracking_;

    // New Context Architecture is now fully owned by ContextManager.
    // AsyncSubsystem no longer directly owns DMA slabs, rings, or context objects.
    // Packet routing and RxPath remain top-level helpers created from ContextManager.
    std::unique_ptr<PacketRouter> packetRouter_;         ///< Packet dispatcher
    std::unique_ptr<Rx::RxPath> rxPath_;                 ///< Receive path actor

    std::unique_ptr<CompletionQueue> completionQueue_{};
    OSSharedPtr<::OSAction> completionAction_{};
    ResetHook* resetHook_{nullptr};
    AsyncMetricsSink* metricsSink_{nullptr};
    std::unique_ptr<Debug::BusResetPacketCapture> busResetCapture_{};
    bool isRunning_{false};
    
    // Context manager (exclusive owner of DMA/rings/contexts)
    // Forward-declared type in Engine namespace
    std::unique_ptr<ASFW::Async::Engine::ContextManager> contextManager_;

    // Transmit submitter: encapsulates two-path TX FSM (first-arm vs link+wake)
    std::unique_ptr<ASFW::Async::Tx::Submitter> submitter_;

    // NOTE: bus generation and local NodeID are now owned by GenerationTracker

    // Bus-Reset packet processing (OHCI §8.4.2.3, Linux handle_ar_packet)
    // Parses synthetic Bus-Reset packet injected by controller into AR Request
    void HandleSyntheticBusResetPacket(const uint32_t* quadlets, uint8_t newGeneration);
    void Teardown(bool disableHardware);

    [[nodiscard]] bool EnsureATContextsRunning(const char* reason);

    uint32_t DrainTxCompletions(const char* reason);

    // Resolver helpers — prefer ContextManager when present, else fall back to
    // previously-owned context pointers. These return non-owning raw pointers.
    ATRequestContext* ResolveAtRequestContext() noexcept;
    ATResponseContext* ResolveAtResponseContext() noexcept;
    ARRequestContext* ResolveArRequestContext() noexcept;
    ARResponseContext* ResolveArResponseContext() noexcept;

    std::atomic<uint64_t> watchdogTickCount_{0};
    std::atomic<uint64_t> watchdogExpiredCount_{0};
    std::atomic<uint64_t> watchdogDrainedCompletions_{0};
    std::atomic<uint64_t> watchdogContextsRearmed_{0};
    std::atomic<uint64_t> watchdogLastTickUsec_{0};
    
    // ========================================================================
    // Command Queue Architecture (Apple IOFWCmdQ pattern)
    // ========================================================================
    // Serializes async transactions to prevent concurrent RUN/WAKE strobing.
    // Commands are queued and executed sequentially with completion-driven
    // advancement (similar to Apple's IOFWCmdQ::executeQueue).
    //
    // Reference: IOFWCmdQ.cpp executeQueue() - processes one command at a time,
    //            removing from queue before startExecution().
    
    /// Pending command structure (queued for sequential execution)
    struct PendingCommand {
        ReadParams params;
        RetryPolicy retryPolicy;
        CompletionCallback userCallback;
        uint8_t retriesRemaining;
        AsyncHandle handle;  // Pre-allocated handle for tracking
        AsyncSubsystem* subsystem;  // Back-pointer for retry/queue advancement
        
        PendingCommand(const ReadParams& p, const RetryPolicy& pol,
                      CompletionCallback cb, AsyncHandle h, AsyncSubsystem* sub)
            : params(p), retryPolicy(pol), userCallback(cb)
            , retriesRemaining(pol.maxRetries), handle(h), subsystem(sub) {}
    };
    
    std::unique_ptr<std::deque<PendingCommand>> commandQueue_{};  // FIFO command queue
    IOLock* commandQueueLock_{nullptr};                           // Protects queue access
    std::atomic<bool> commandInFlight_{false};                    // True when command executing
    
    /// Execute next queued command (called after completion or on first submit)
    /// Follows Apple's executeQueue pattern: dequeue → execute → await completion
    void ExecuteNextCommand();
    
    /// Internal completion wrapper for retry logic and queue advancement
    /// Replaces user callback to handle retries before invoking user code
    void OnCommandCompleteInternal(AsyncHandle handle, AsyncStatus status,
                                   const void* payload, uint32_t payloadSize,
                                   PendingCommand* cmd);
};

} // namespace ASFW::Async
