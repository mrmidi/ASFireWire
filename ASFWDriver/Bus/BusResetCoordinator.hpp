#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <utility>
#include <string>
#include <cstring>

#include "../Controller/ControllerTypes.hpp"
#include "../Hardware/RegisterMap.hpp"
#include "SelfIDCapture.hpp"
#include "BusManager.hpp"
#include "../Discovery/DiscoveryTypes.hpp"  // For Discovery::Generation

#ifdef ASFW_HOST_TEST
#include "../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/OSSharedPtr.h>
#endif

namespace ASFW::Driver {

class HardwareInterface;
class SelfIDCapture;
class ConfigROMStager;
class InterruptManager;
class TopologyManager;
class BusManager;
}

namespace ASFW::Async {
class AsyncSubsystem;
}

namespace ASFW::Discovery {
class ROMScanner;
}

namespace ASFW::Driver {

// Coordinates the staged workflow for handling OHCI bus resets.
// Implements a deterministic FSM that enforces spec-ordered steps
// (OHCI 1.1 §§6.1.1, 7.2.3.2, 11).
class BusResetCoordinator {
public:
    using TopologyReadyCallback = std::function<void(const TopologySnapshot&)>;

    enum class State : uint8_t {
        Idle,               // Normal operation, no reset in progress
        Detecting,          // busReset observed, mask interrupt, prime context
        WaitingSelfID,      // Awaiting selfIDComplete AND selfIDComplete2
        QuiescingAT,        // Stop and flush AT contexts (AR continues)
        RestoringConfigROM, // 3-step ROM restoration sequence
        ClearingBusReset,   // Preconditions satisfied, clear busReset bit
        Rearming,           // Re-enable filters, re-arm AT contexts
        Complete,           // Publish metrics, unmask busReset, go Idle
        Error               // Unrecoverable error path
    };

    enum class Event : uint8_t {
        IrqBusReset,        // IntEvent.busReset asserted
        IrqSelfIDComplete,  // IntEvent.selfIDComplete observed
        IrqSelfIDComplete2, // IntEvent.selfIDComplete2 observed
        AsyncSynthReset,    // Observed PHY packet in AR/RQ (optional)
        TimeoutGuard,       // Safety timeout
        Unrecoverable,      // Unrecoverable error
        RegFail             // Register access failure
    };

    BusResetCoordinator();
    ~BusResetCoordinator();

    void Initialize(HardwareInterface* hw,
                    OSSharedPtr<IODispatchQueue> workQueue,
                    Async::AsyncSubsystem* asyncSys,
                    SelfIDCapture* selfIdCapture,
                    ConfigROMStager* configRom,
                    InterruptManager* interrupts,
                    TopologyManager* topology,
                    BusManager* busManager = nullptr,
                    Discovery::ROMScanner* romScanner = nullptr);

    void OnIrq(uint32_t intEvent, uint64_t timestamp);

    void BindCallbacks(TopologyReadyCallback onTopology);

    const BusResetMetrics& Metrics() const { return metrics_; }
    State GetState() const { return state_; }
    const char* StateString() const;
    static const char* StateString(State s);

    /**
     * Reset delegation retry counter (Linux pattern for emergency bypass).
     *
     * Call this when:
     * 1. Gap=0 detected (critical error, bypass retry limit)
     * 2. Topology actually changes (device added/removed)
     */
    void ResetDelegationRetryCounter();

private:
    void TransitionTo(State newState, const char* reason);
    void ProcessEvent(Event event);
    void RunStateMachine();

    void A_MaskBusReset();
    void A_UnmaskBusReset();
    void ForceUnmaskBusResetIfNeeded();
    void HandleStraySelfID();
    void A_ClearSelfID2Stale();
    void A_ArmSelfIDBuffer();
    void A_AckSelfIDPair();
    void A_StopFlushAT();
    void A_DecodeSelfID();
    void A_BuildTopology();
    void A_RestoreConfigROM();
    void A_ClearBusReset();
    void A_EnableFilters();
    void A_RearmAT();
    void A_MetricsLog();
    void A_SendGlobalResumeIfNeeded();
    void StageDelayedPhyPacket(const BusManager::PhyConfigCommand& command,
                               const char* reason);
    bool DispatchPendingPhyPacket();
    void EvaluateRootDelegation(const TopologySnapshot& topo);
    void ScheduleDeferredRun(uint32_t delayMs, const char* reason);

    bool G_ATInactive();
    bool G_HaveSelfIDPair();
    bool G_ROMImageReady();
    bool G_NodeIDValid() const;
    
    bool ReadyForDiscovery(Discovery::Generation gen) const;

    static uint64_t MonotonicNow();

    State state_{State::Idle};
    uint64_t stateEntryTime_{0};
    bool selfIDComplete1_{false};
    bool selfIDComplete2_{false};
    uint32_t pendingSelfIDCountReg_{0};

    std::atomic<bool> workInProgress_{false};
    
    bool firstBusResetSeen_{false};
    uint64_t lastResetTimestamp_{0};

    BusResetMetrics metrics_{};
    
    uint64_t firstIrqTime_{0};
    uint64_t selfIDComplete1Time_{0};
    uint64_t selfIDComplete2Time_{0};
    uint64_t busResetClearTime_{0};
    std::optional<SelfIDCapture::Result> lastSelfId_;
    std::optional<TopologySnapshot> lastTopology_;
    TopologyReadyCallback topologyCallback_;

    std::atomic<bool> deferredRunScheduled_{false};
    HardwareInterface* hardware_{nullptr};
    Async::AsyncSubsystem* asyncSubsystem_{nullptr};
    SelfIDCapture* selfIdCapture_{nullptr};
    ConfigROMStager* configRomStager_{nullptr};
    InterruptManager* interruptManager_{nullptr};
    TopologyManager* topologyManager_{nullptr};
    BusManager* busManager_{nullptr};
    Discovery::ROMScanner* romScanner_{nullptr};

    OSSharedPtr<IODispatchQueue> workQueue_;

    uint64_t lastResetNs_{0};
    uint64_t lastSelfIdNs_{0};
    bool busResetMasked_{false};
    Discovery::Generation lastGeneration_{0};
    
    bool filtersEnabled_{false};
    bool atArmed_{false};

    std::optional<BusManager::PhyConfigCommand> pendingPhyCommand_;
    std::string pendingPhyReason_;
    bool pendingManagedReset_{false};
    bool delegateAttemptActive_{false};
    uint8_t delegateTarget_{0xFF};
    uint32_t delegateRetryCount_{0};
    static constexpr uint32_t kMaxDelegateRetries = 5;
    bool delegateSuppressed_{false};
    uint32_t lastResumeGeneration_{0xFFFFFFFFu};
};

} // namespace ASFW::Driver
