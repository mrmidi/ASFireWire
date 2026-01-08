#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOReturn.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSObject.h>
#include <DriverKit/OSSharedPtr.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/IODispatchQueue.h>
#endif

#include "AsyncTypes.hpp"
#include "../Bus/GenerationTracker.hpp"
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

// Import Shared types used by AsyncSubsystem
using ASFW::Shared::DescriptorRing;
using ASFW::Shared::BufferRing;
using ASFW::Shared::DMAMemoryManager;

// Forward declarations
class ResetHook;
class AsyncMetricsSink;
class DescriptorBuilder;
class PacketBuilder;
class PacketRouter;
class ResponseSender;

// Forward declaration for Tx Submitter (two-path TX FSM)
namespace Tx { class Submitter; }

// Forward declarations - new architecture
#include "../Shared/Rings/DescriptorRing.hpp"
#include "../Shared/Rings/BufferRing.hpp"
#include "../Shared/Memory/DMAMemoryManager.hpp"
class ATRequestContext;
class ATResponseContext;
class ARRequestContext;
class ARResponseContext;

namespace Engine { class ContextManager; }

template <typename TCompletionQueue> class Track_Tracking;

template <typename Derived> class AsyncCommand;

class AsyncSubsystem {
public:
    AsyncSubsystem();
    ~AsyncSubsystem();
    
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
    
    kern_return_t ArmDMAContexts();
    
    kern_return_t ArmARContextsOnly();
    
    void Stop();

    AsyncHandle Read(const ReadParams& params, CompletionCallback callback);

    AsyncHandle ReadWithRetry(const ReadParams& params,
                             const RetryPolicy& retryPolicy,
                             CompletionCallback callback);

    AsyncHandle Write(const WriteParams& params, CompletionCallback callback);
    AsyncHandle Lock(const LockParams& params, uint16_t extendedTCode, CompletionCallback callback);
    AsyncHandle CompareSwap(const CompareSwapParams& params, CompareSwapCallback callback);
    AsyncHandle Stream(const StreamParams& params);
    AsyncHandle PhyRequest(const PhyParams& params, CompletionCallback callback);
    bool Cancel(AsyncHandle handle);

    void PostToWorkloop(void (^block)()) {
        if (workloopQueue_) {
            workloopQueue_->DispatchAsync(block);
        }
    }

    void OnTxInterrupt();
    void OnRxInterrupt(ARContextType contextType);
    void OnBusReset();
    void OnBusResetBegin(uint8_t nextGen);
    void OnBusResetComplete(uint8_t stableGen);
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

    void StopATContextsOnly();
    
    void FlushATContexts();

    void RearmATContexts();

    void DumpState();

    Debug::BusResetPacketCapture* GetBusResetCapture() const {
        return busResetCapture_.get();
    }

    [[nodiscard]] std::optional<AsyncStatusSnapshot> GetStatusSnapshot() const;
    
    [[nodiscard]] std::optional<TransactionContext> PrepareTransactionContext();
    
    [[nodiscard]] uint64_t GetCurrentTimeUsec() const;

    [[nodiscard]] Bus::GenerationTracker::BusState GetBusState() const {
        return generationTracker_->GetCurrentState();
    }

    [[nodiscard]] Bus::GenerationTracker& GetGenerationTracker() {
        if (!labelAllocator_) {
            labelAllocator_ = std::make_unique<LabelAllocator>();
            labelAllocator_->Reset();
        }
        if (!generationTracker_) {
            generationTracker_ = std::make_unique<Bus::GenerationTracker>(*labelAllocator_);
            generationTracker_->Reset();
        }
        return *generationTracker_;
    }

    [[nodiscard]] Track_Tracking<CompletionQueue>* GetTracking() { return tracking_.get(); }
    [[nodiscard]] DescriptorBuilder* GetDescriptorBuilder() { return descriptorBuilder_; }
    [[nodiscard]] PacketBuilder* GetPacketBuilder() { return packetBuilder_.get(); }
    [[nodiscard]] Tx::Submitter* GetSubmitter() { return submitter_.get(); }
    [[nodiscard]] Driver::HardwareInterface* GetHardware() { return hardware_; }
    [[nodiscard]] PacketRouter* GetPacketRouter() { return packetRouter_.get(); }

    [[nodiscard]] DMAMemoryManager* GetDMAManager() {
        return contextManager_ ? contextManager_->DmaManager() : nullptr;
    }

private:
    std::atomic<uint32_t> is_bus_reset_in_progress_{0};

    Driver::HardwareInterface* hardware_{nullptr};
    ::OSObject* owner_{nullptr};
    ::IODispatchQueue* workloopQueue_{nullptr};
    ::IOLock* sharedLock_{nullptr};

    std::unique_ptr<LabelAllocator> labelAllocator_;
    std::unique_ptr<ASFW::Async::Bus::GenerationTracker> generationTracker_;
    DescriptorBuilder* descriptorBuilder_{nullptr};
    DescriptorBuilder* descriptorBuilderResponse_{nullptr};
    std::unique_ptr<PacketBuilder> packetBuilder_;

    std::unique_ptr<TransactionManager> txnMgr_;

    std::unique_ptr<Track_Tracking<CompletionQueue>> tracking_;

    std::unique_ptr<PacketRouter> packetRouter_;
    std::unique_ptr<Rx::RxPath> rxPath_;
    std::unique_ptr<ResponseSender> responseSender_;

    std::unique_ptr<CompletionQueue> completionQueue_{};
    OSSharedPtr<::OSAction> completionAction_{};
    ResetHook* resetHook_{nullptr};
    AsyncMetricsSink* metricsSink_{nullptr};
    std::unique_ptr<Debug::BusResetPacketCapture> busResetCapture_{};
    bool isRunning_{false};
    
    std::unique_ptr<ASFW::Async::Engine::ContextManager> contextManager_;

    std::unique_ptr<ASFW::Async::Tx::Submitter> submitter_;

    void HandleSyntheticBusResetPacket(const uint32_t* quadlets, uint8_t newGeneration);
    void Teardown(bool disableHardware);

    [[nodiscard]] bool EnsureATContextsRunning(const char* reason);

    uint32_t DrainTxCompletions(const char* reason);

    ATRequestContext* ResolveAtRequestContext() noexcept;
    ATResponseContext* ResolveAtResponseContext() noexcept;
    ARRequestContext* ResolveArRequestContext() noexcept;
    ARResponseContext* ResolveArResponseContext() noexcept;

    std::atomic<uint64_t> watchdogTickCount_{0};
    std::atomic<uint64_t> watchdogExpiredCount_{0};
    std::atomic<uint64_t> watchdogDrainedCompletions_{0};
    std::atomic<uint64_t> watchdogContextsRearmed_{0};
    std::atomic<uint64_t> watchdogLastTickUsec_{0};
    
    struct PendingCommand {
        ReadParams params;
        RetryPolicy retryPolicy;
        CompletionCallback userCallback;
        uint8_t retriesRemaining;
        AsyncHandle handle;
        AsyncSubsystem* subsystem;
        
        PendingCommand(const ReadParams& p, const RetryPolicy& pol,
                      CompletionCallback cb, AsyncHandle h, AsyncSubsystem* sub)
            : params(p), retryPolicy(pol), userCallback(cb)
            , retriesRemaining(pol.maxRetries), handle(h), subsystem(sub) {}
    };
    
    std::unique_ptr<std::deque<PendingCommand>> commandQueue_{};
    IOLock* commandQueueLock_{nullptr};
    std::atomic<bool> commandInFlight_{false};
    
    void ExecuteNextCommand();
    
    void OnCommandCompleteInternal(AsyncHandle handle, AsyncStatus status,
                                   const void* payload, uint32_t payloadSize,
                                   PendingCommand* cmd);
};

} // namespace ASFW::Async
