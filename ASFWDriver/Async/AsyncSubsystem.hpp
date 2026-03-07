#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "Interfaces/IAsyncControllerPort.hpp"

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSObject.h>
#include <DriverKit/OSSharedPtr.h>
#endif

#include "../Bus/GenerationTracker.hpp"
#include "AsyncTypes.hpp"
#include "Rx/RxPath.hpp"
#include "Track/CompletionQueue.hpp"
#include "Track/Tracking.hpp"

namespace ASFW::Driver {
class HardwareInterface;
}

namespace ASFW::Debug {
class BusResetPacketCapture;
}

namespace ASFW::Async {

// Import Shared types used by AsyncSubsystem
using ASFW::Shared::BufferRing;
using ASFW::Shared::DescriptorRing;
using ASFW::Shared::DMAMemoryManager;

// Forward declarations
class ResetHook;
class AsyncMetricsSink;
class DescriptorBuilder;
class PacketBuilder;
class PacketRouter;
class ResponseSender;

// Forward declaration for Tx Submitter (two-path TX FSM)
namespace Tx {
class Submitter;
}

// Forward declarations - new architecture
#include "../Shared/Memory/DMAMemoryManager.hpp"
#include "../Shared/Rings/BufferRing.hpp"
#include "../Shared/Rings/DescriptorRing.hpp"
class ATRequestContext;
class ATResponseContext;
class ARRequestContext;
class ARResponseContext;

namespace Engine {
class ContextManager;
}

template <typename TCompletionQueue> class Track_Tracking;

template <typename Derived> class AsyncCommand;

class AsyncSubsystem : public IAsyncControllerPort {
  public:
    AsyncSubsystem();
    ~AsyncSubsystem() override;

    template <typename Derived> friend class AsyncCommand;

    enum class ARContextType {
        Request,
        Response,
    };

    kern_return_t Start(Driver::HardwareInterface& hw, OSObject* owner,
                        ::IODispatchQueue* workloopQueue, ::OSAction* completionAction,
                        size_t completionQueueCapacityBytes = 64 * 1024);

    kern_return_t ArmDMAContexts();

    kern_return_t ArmARContextsOnly() override;

    void Stop();

    AsyncHandle Read(const ReadParams& params, CompletionCallback callback) override;

    AsyncHandle ReadWithRetry(const ReadParams& params, const RetryPolicy& retryPolicy,
                              CompletionCallback callback) override;

    AsyncHandle Write(const WriteParams& params, CompletionCallback callback) override;
    AsyncHandle Lock(const LockParams& params, uint16_t extendedTCode,
                     CompletionCallback callback) override;
    AsyncHandle CompareSwap(const CompareSwapParams& params, CompareSwapCallback callback) override;
    AsyncHandle Stream(const StreamParams& params);
    AsyncHandle PhyRequest(const PhyParams& params, CompletionCallback callback) override;
    bool Cancel(AsyncHandle handle) override;

    void PostToWorkloop(void (^block)()) override {
#ifdef ASFW_HOST_TEST
        if (hostDeferPostedWork_.load(std::memory_order_acquire)) {
            std::function<void()> work = block;
            {
                std::lock_guard lock(hostPostedWorkLock_);
                hostPostedWork_.push_back(std::move(work));
            }
            return;
        }
#endif
        if (workloopQueue_) {
            workloopQueue_->DispatchAsync(block);
            return;
        }

        // Fallback: If the workloop queue isn't available (mis-wired early init or host test),
        // do not silently drop work that may carry required completions (e.g. Cancel()).
        if (block) {
            block();
        }
    }

#ifdef ASFW_HOST_TEST
    // Host-only hooks for deterministic testing of "no inline completion" guarantees.
    void HostTest_SetDeferPostedWork(bool enabled) {
        hostDeferPostedWork_.store(enabled, std::memory_order_release);
    }

    void HostTest_DrainPostedWork() {
        std::deque<std::function<void()>> work;
        {
            std::lock_guard lock(hostPostedWorkLock_);
            work.swap(hostPostedWork_);
        }
        for (auto& fn : work) {
            if (fn) {
                fn();
            }
        }
    }
#endif

    void OnTxInterrupt() override;
    void OnRxInterrupt(ARContextType contextType);
    void OnRxRequestInterrupt() override { OnRxInterrupt(ARContextType::Request); }
    void OnRxResponseInterrupt() override { OnRxInterrupt(ARContextType::Response); }
    void OnBusReset();
    void OnBusResetBegin(uint8_t nextGen) override;
    void OnBusResetComplete(uint8_t stableGen) override;
    void ConfirmBusGeneration(uint8_t confirmedGeneration) override;
    void OnTimeoutTick() override;

    [[nodiscard]] AsyncWatchdogStats GetWatchdogStats() const override;

    void StopATContextsOnly() override;

    void FlushATContexts() override;

    void RearmATContexts() override;

    void DumpState();

    Debug::BusResetPacketCapture* GetBusResetCapture() const override {
        return busResetCapture_.get();
    }

    [[nodiscard]] std::optional<AsyncStatusSnapshot> GetStatusSnapshot() const override;

    [[nodiscard]] std::optional<TransactionContext> PrepareTransactionContext();

    [[nodiscard]] uint64_t GetCurrentTimeUsec() const;

    [[nodiscard]] Bus::GenerationTracker::BusState GetBusState() const {
        return generationTracker_->GetCurrentState();
    }

    [[nodiscard]] AsyncBusStateSnapshot GetBusStateSnapshot() const override {
        const auto state = generationTracker_ ? generationTracker_->GetCurrentState()
                                              : Bus::GenerationTracker::BusState{};
        return AsyncBusStateSnapshot{
            .generation16 = state.generation16,
            .generation8 = state.generation8,
            .localNodeID = state.localNodeID,
        };
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

    [[nodiscard]] DMAMemoryManager* GetDMAManager() override;

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

    struct PendingCommandState {
        ReadParams params{};
        RetryPolicy retryPolicy{};
        CompletionCallback userCallback{nullptr};
        uint8_t retriesRemaining{0};
        AsyncHandle publicHandle{};  // Stable handle returned to caller (ReadWithRetry)
        AsyncHandle currentHandle{}; // Underlying transaction handle (Read)
        std::atomic<bool> cancelRequested{false};
        AsyncSubsystem* subsystem{nullptr};

        PendingCommandState(const ReadParams& p, const RetryPolicy& pol, CompletionCallback cb,
                            AsyncHandle publicHandleIn, AsyncSubsystem* sub)
            : params(p), retryPolicy(pol), userCallback(std::move(cb)),
              retriesRemaining(pol.maxRetries), publicHandle(publicHandleIn), currentHandle{},
              subsystem(sub) {}
    };

    using PendingCommandPtr = std::shared_ptr<PendingCommandState>;

    std::unique_ptr<std::deque<PendingCommandPtr>> commandQueue_{};
    IOLock* commandQueueLock_{nullptr};
    std::atomic<bool> commandInFlight_{false};
    PendingCommandPtr inFlightCommand_{};

#ifdef ASFW_HOST_TEST
    std::atomic<bool> hostDeferPostedWork_{false};
    mutable std::mutex hostPostedWorkLock_{};
    std::deque<std::function<void()>> hostPostedWork_{};
#endif

    void ExecuteNextCommand();

    void OnCommandCompleteInternal(AsyncHandle handle, AsyncStatus status, const void* payload,
                                   uint32_t payloadSize, PendingCommandState* cmd);
};

} // namespace ASFW::Async
