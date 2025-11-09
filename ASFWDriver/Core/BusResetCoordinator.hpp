#pragma once

#include <atomic>
#include <functional>
#include <optional>
#include <utility>

#include "ControllerTypes.hpp"
#include "RegisterMap.hpp"
#include "SelfIDCapture.hpp"
#include "../Discovery/DiscoveryTypes.hpp"  // For Discovery::Generation

#ifdef ASFW_HOST_TEST
#include "HostDriverKitStubs.hpp"
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
}

namespace ASFW::Async {
class AsyncSubsystem;
}

namespace ASFW::Discovery {
class ROMScanner;
}

namespace ASFW::Driver {

// Coordinates the staged workflow for handling OHCI bus resets as outlined in
// DRAFT.md §6.8 and ASFW_BusReset_Refactor_Guide.md. Implements a deterministic
// FSM that enforces spec-ordered steps (OHCI 1.1 §§6.1.1, 7.2.3.2, 11).
class BusResetCoordinator {
public:
    using TopologyReadyCallback = std::function<void(const TopologySnapshot&)>;

    // FSM States (§3 of refactor guide)
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

    // FSM Events (inputs to transitions)
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

    // Initialize with all dependencies (not just hardware + queue)
    // FSM actions require asyncSubsystem, selfIdCapture, configRomStager to function
    // Add InterruptManager for mask synchronization
    // Add TopologyManager for building topology snapshot after Self-ID decode
    // Add ROMScanner for aborting in-flight discovery on bus reset
    void Initialize(HardwareInterface* hw,
                    OSSharedPtr<IODispatchQueue> workQueue,
                    Async::AsyncSubsystem* asyncSys,
                    SelfIDCapture* selfIdCapture,
                    ConfigROMStager* configRom,
                    InterruptManager* interrupts,
                    TopologyManager* topology,
                    Discovery::ROMScanner* romScanner = nullptr);

    // ISR-safe, non-blocking event dispatcher
    void OnIrq(uint32_t intEvent, uint64_t timestamp);

    void BindCallbacks(TopologyReadyCallback onTopology);

    const BusResetMetrics& Metrics() const { return metrics_; }
    State GetState() const { return state_; }
    const char* StateString() const;
    static const char* StateString(State s);

private:
    // FSM transition engine
    void TransitionTo(State newState, const char* reason);
    void ProcessEvent(Event event);
    void RunStateMachine();

    // FSM Actions (side effects)
    void A_MaskBusReset();
    void A_UnmaskBusReset();
    void ForceUnmaskBusResetIfNeeded();
    void HandleStraySelfID();  // Drain stray Self-ID when FSM is Idle/Complete
    void A_ClearSelfID2Stale();
    void A_ArmSelfIDBuffer();
    void A_AckSelfIDPair();       // Clear sticky Self-ID interrupt bits after decode
    void A_StopFlushAT();
    void A_DecodeSelfID();
    void A_BuildTopology();  // NEW: Build topology snapshot from Self-ID data
    void A_RestoreConfigROM();
    void A_ClearBusReset();
    void A_EnableFilters();
    void A_RearmAT();
    void A_MetricsLog();
    void ScheduleDeferredRun(uint32_t delayMs, const char* reason);

    // FSM Guards (preconditions)
    bool G_ATInactive();
    bool G_HaveSelfIDPair();
    bool G_ROMImageReady();
    bool G_NodeIDValid() const;
    
    // Discovery readiness check (comprehensive invariants)
    bool ReadyForDiscovery(Discovery::Generation gen) const;

    static uint64_t MonotonicNow();

    // FSM state
    State state_{State::Idle};
    uint64_t stateEntryTime_{0};
    bool selfIDComplete1_{false};
    bool selfIDComplete2_{false};
    uint32_t pendingSelfIDCountReg_{0};

    // Reentrancy protection (atomic for thread-safety)
    std::atomic<bool> workInProgress_{false};
    
    bool firstBusResetSeen_{false};
    uint64_t lastResetTimestamp_{0};

    BusResetMetrics metrics_{};
    
    // Reset Capsule timestamps for structured logging
    uint64_t firstIrqTime_{0};
    uint64_t selfIDComplete1Time_{0};
    uint64_t selfIDComplete2Time_{0};
    uint64_t busResetClearTime_{0};
    std::optional<SelfIDCapture::Result> lastSelfId_;  // Cached decode result (avoid double decode)
    std::optional<TopologySnapshot> lastTopology_;     // Cached topology snapshot (for Discovery callback)
    TopologyReadyCallback topologyCallback_;

    std::atomic<bool> deferredRunScheduled_{false};
    HardwareInterface* hardware_{nullptr};
    Async::AsyncSubsystem* asyncSubsystem_{nullptr};
    SelfIDCapture* selfIdCapture_{nullptr};
    ConfigROMStager* configRomStager_{nullptr};
    InterruptManager* interruptManager_{nullptr};
    TopologyManager* topologyManager_{nullptr};
    Discovery::ROMScanner* romScanner_{nullptr};

    OSSharedPtr<IODispatchQueue> workQueue_;

    uint64_t lastResetNs_{0};
    uint64_t lastSelfIdNs_{0};
    bool busResetMasked_{false};
    Discovery::Generation lastGeneration_{0};
    
    // Software latches for discovery readiness checks
    bool filtersEnabled_{false};
    bool atArmed_{false};
};

} // namespace ASFW::Driver
