#pragma once

#include <atomic>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "../Controller/ControllerTypes.hpp"
#include "../Discovery/DiscoveryTypes.hpp" // For Discovery::Generation
#include "../Hardware/RegisterMap.hpp"
#include "BusManager.hpp"
#include "SelfIDCapture.hpp"

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
} // namespace ASFW::Driver

namespace ASFW::Async {
class IAsyncControllerPort;
}

namespace ASFW::Discovery {
class ROMScanner;
}

namespace ASFW::Driver {

#ifdef ASFW_HOST_TEST
class BusResetCoordinatorTestPeer;
#endif

/**
 * @class BusResetCoordinator
 * @brief Orchestrates bus-reset recovery as a staged, deterministic FSM.
 *
 * The coordinator owns the sequencing constraints around Self-ID capture,
 * async transmit quiescence, Config ROM restoration, interrupt ownership, and
 * post-reset discovery handoff. Heavy work stays off the IRQ path; `OnIrq()`
 * only latches reset-related bits and schedules deferred processing.
 *
 * Key spec constraints preserved here:
 * - OHCI 1.1 §6.1 / Table 6-1 and §11.5 for `selfIDComplete2` sticky semantics
 * - OHCI 1.1 §7.2.3.2 for AT inactivity before clearing `IntEvent.busReset`
 * - IEEE 1394-2008 §8.2.1 for the 2 s software-reset holdoff after Self-ID
 *   completion, with conservative handling of the §8.4.5.2 gap-count flow
 */
class BusResetCoordinator {
  public:
    using TopologyReadyCallback = std::function<void(const TopologySnapshot&)>;

    enum class State : uint8_t {
        Idle,               // Normal operation, no reset in progress
        Detecting,          // busReset observed, mask interrupt, prime context
        WaitingSelfID,      // Awaiting a stable Self-ID completion indication
        QuiescingAT,        // Stop and flush AT contexts (AR continues)
        RestoringConfigROM, // 3-step ROM restoration sequence
        ClearingBusReset,   // Preconditions satisfied, clear busReset bit
        Rearming,           // Re-enable filters, re-arm AT contexts
        Complete,           // Publish metrics, unmask busReset, go Idle
    };

    BusResetCoordinator();
    ~BusResetCoordinator();

    /**
     * @brief Bind the coordinator to controller-owned infrastructure.
     *
     * The coordinator does not take ownership of the supplied objects. Callers
     * must keep them alive for at least as long as the coordinator itself.
     */
    void Initialize(HardwareInterface* hw, OSSharedPtr<IODispatchQueue> workQueue,
                    Async::IAsyncControllerPort* asyncSys, SelfIDCapture* selfIdCapture,
                    ConfigROMStager* configRom, InterruptManager* interrupts,
                    TopologyManager* topology, BusManager* busManager = nullptr,
                    Discovery::ROMScanner* romScanner = nullptr);

    /**
     * Latch bus-reset related interrupt bits and schedule deferred recovery work.
     *
     * OHCI 1.1 §6.1 / Table 6-1 defines `selfIDComplete2` as a sticky companion
     * to `selfIDComplete`, and §11.5 states it is cleared only via
     * `IntEventClear`. This ingress path records the bits but leaves the
     * ordering-sensitive clear/consume policy to the coordinator FSM.
     */
    void OnIrq(uint32_t intEvent, uint64_t timestamp);

    /// Register the topology publication callback used after a stable reset completes.
    void BindCallbacks(TopologyReadyCallback onTopology);

    /// Return the most recent reset metrics snapshot.
    const BusResetMetrics& Metrics() const { return metrics_; }
    /// Return the current FSM state.
    State GetState() const { return state_; }
    const char* StateString() const;
    static const char* StateString(State state);

    /**
     * Reset delegation retry counter (Linux pattern for emergency bypass).
     *
     * Call this when:
     * 1. Gap=0 detected (critical error, bypass retry limit)
     * 2. Topology actually changes (device added/removed)
     */
    void ResetDelegationRetryCounter();

    /**
     * Inform coordinator that the most recently completed ROM scan had
     * nodes returning ack_busy_X (device still booting).  When true,
     * the next post-reset discovery dispatch will be delayed to give
     * slow-booting firmware (e.g. DICE) time to finish initialization
     * before we scan again.
     *
     * The delay escalates with consecutive busy/empty scans:
     *   2s → 4s → 6s → 8s → 10s (capped at kMaxDiscoveryDelayMs).
     * Resets to 0 when a scan succeeds with actual ROMs.
     */
    void SetPreviousScanHadBusyNodes(bool busy);

    /**
     * Escalate the discovery delay without changing the busy-node flag.
     * Call when a scan completes with 0 ROMs (no scannable nodes) — we
     * learned nothing about whether the device recovered, so increase
     * the delay for the next attempt.
     */
    void EscalateDiscoveryDelay();

  private:
#ifdef ASFW_HOST_TEST
    friend class BusResetCoordinatorTestPeer;
#endif

    enum class StepResult : uint8_t { Continue, Yield, Finish };

    enum class ResetRequestKind : uint8_t { Recovery, GapCorrection, Delegation, ManualBusManager };

    enum class ResetFlavor : uint8_t { Short, Long };

    struct SelfIDLatchState {
        bool complete{false};
        bool stickyComplete{false};
        uint64_t completeTimeNs{0};
        uint64_t stickyCompleteTimeNs{0};

        void Reset() noexcept {
            complete = false;
            stickyComplete = false;
            completeTimeNs = 0;
            stickyCompleteTimeNs = 0;
        }
    };

    struct ResetTimingState {
        uint64_t lastBusResetEdgeNs{0};
        uint64_t lastSelfIdCompletionNs{0};
        uint64_t softwareResetBlockedUntilNs{0};
    };

    struct ResetRequest {
        ResetRequestKind kind{ResetRequestKind::Recovery};
        ResetFlavor flavor{ResetFlavor::Short};
        std::optional<BusManager::PhyConfigCommand> phyConfig;
        std::string reason;
        std::optional<BusManager::GapDecisionReason> gapDecisionReason;
    };

    struct ResetCycleState {
        ResetTimingState timing{};
        std::optional<SelfIDCapture::Result> acceptedSelfId;
        std::optional<TopologySnapshot> acceptedTopology;
        std::optional<ResetRequest> pendingReset;
        std::optional<std::string> recoveryReason;

        void ResetForNewEdge() noexcept {
            acceptedSelfId.reset();
            acceptedTopology.reset();
            pendingReset.reset();
            recoveryReason.reset();
        }
    };

    void TransitionTo(State newState, const char* reason);
    void RunStateMachine();
    void BeginNewResetCycle();
    void CompleteCurrentRun();
    void YieldAndReschedule(uint32_t delayMs, const char* reason);

    StepResult StepIdle();
    StepResult StepDetecting();
    StepResult StepWaitingSelfID();
    StepResult StepQuiescingAT();
    StepResult StepRestoringConfigROM();
    StepResult StepClearingBusReset();
    StepResult StepRearming();
    StepResult StepComplete();

    void MaskBusReset();
    void UnmaskBusReset();
    void ForceUnmaskBusResetIfNeeded();
    void HandleStraySelfID();
    void ClearStaleSelfIDComplete2();
    void ClearConsumedSelfIDInterrupts();
    void ArmSelfIDBuffer();
    void StopFlushAT();
    bool DecodeSelfID();
    bool BuildTopology();
    void RestoreConfigROM();
    void ClearBusReset();
    void EnableFilters();
    void RearmAT();
    void LogMetrics();
    void SendGlobalResumeIfNeeded();
    void MaybeRequestTopologyDrivenReset();
    void EvaluateRootDelegation(const TopologySnapshot& topo);
    void RequestSoftwareReset(ResetRequest request);
    [[nodiscard]] ResetRequest MergeResetRequests(const ResetRequest& current,
                                                  const ResetRequest& incoming) const;
    bool MaybeDispatchPendingSoftwareReset();
    void ClearSoftwareResetTracking(const ResetRequest& request, bool carriesDelegation);
    [[nodiscard]] bool ApplySoftwareResetPhyConfig(const ResetRequest& request,
                                                   bool carriesDelegation);
    void NoteIssuedGapReset(const ResetRequest& request);
    bool DispatchSoftwareReset(const ResetRequest& request);
    void ClearDelegationAttempt();
    void RecordRecoveryReason(std::string reason);

    bool G_ATInactive();
    bool HasSelfIDCompletion() const;
    bool CanAttemptSelfIDDecode() const;
    bool G_NodeIDValid() const;

    bool ReadyForDiscovery(Discovery::Generation gen) const;

    static uint64_t MonotonicNow();

    State state_{State::Idle};
    uint64_t stateEntryTime_{0};
    std::atomic<bool> workInProgress_{false};
    bool pendingBusResetEdge_{false};
    bool stopFlushIssued_{false};

    BusResetMetrics metrics_{};

    uint64_t firstIrqTime_{0};
    uint64_t busResetClearTime_{0};
    TopologyReadyCallback topologyCallback_;

    std::atomic<bool> deferredRunScheduled_{false};
    HardwareInterface* hardware_{nullptr};
    Async::IAsyncControllerPort* asyncSubsystem_{nullptr};
    SelfIDCapture* selfIdCapture_{nullptr};
    ConfigROMStager* configRomStager_{nullptr};
    InterruptManager* interruptManager_{nullptr};
    TopologyManager* topologyManager_{nullptr};
    BusManager* busManager_{nullptr};
    Discovery::ROMScanner* romScanner_{nullptr};

    OSSharedPtr<IODispatchQueue> workQueue_;

    SelfIDLatchState selfIdLatch_{};
    ResetCycleState cycle_{};
    bool busResetMasked_{false};
    Discovery::Generation lastGeneration_{0};

    bool filtersEnabled_{false};
    bool atArmed_{false};
    bool delegateAttemptActive_{false};
    uint8_t delegateTarget_{0xFF};
    uint32_t delegateRetryCount_{0};
    static constexpr uint32_t kMaxDelegateRetries = 5;
    bool delegateSuppressed_{false};
    uint32_t lastResumeGeneration_{0xFFFFFFFFU};

    // Discovery delay for slow-booting devices (DICE/Saffire).
    // Escalates with consecutive failed scans: 2s → 4s → 6s → 8s → 10s.
    static constexpr uint32_t kDiscoveryDelayStepMs = 2000; // escalation step
    static constexpr uint32_t kMaxDiscoveryDelayMs = 10000; // 10s cap
    uint32_t currentDiscoveryDelayMs_{0};
    bool previousScanHadBusyNodes_{false};
};

} // namespace ASFW::Driver
