#include "BusResetCoordinator.hpp"

#ifdef ASFW_HOST_TEST
#include <chrono>
#include <thread>
#else
#include <DriverKit/IOLib.h>
#endif

#include "HardwareInterface.hpp"
#include "SelfIDCapture.hpp"
#include "ConfigROMStager.hpp"
#include "InterruptManager.hpp"
#include "TopologyManager.hpp"
#include "Logging.hpp"
#include "OHCIConstants.hpp"
#include "../Async/AsyncSubsystem.hpp"
#include "../Discovery/ROMScanner.hpp"

namespace ASFW::Driver {

BusResetCoordinator::BusResetCoordinator() = default;
BusResetCoordinator::~BusResetCoordinator() = default;

// Add TopologyManager for building topology snapshot after Self-ID decode
// Add ROMScanner for aborting in-flight discovery on bus reset
void BusResetCoordinator::Initialize(HardwareInterface* hw,
                                    OSSharedPtr<IODispatchQueue> workQueue,
                                    Async::AsyncSubsystem* asyncSys,
                                    SelfIDCapture* selfIdCapture,
                                    ConfigROMStager* configRom,
                                    InterruptManager* interrupts,
                                    TopologyManager* topology,
                                    Discovery::ROMScanner* romScanner) {
    hardware_ = hw;
    workQueue_ = std::move(workQueue);
    asyncSubsystem_ = asyncSys;
    selfIdCapture_ = selfIdCapture;
    configRomStager_ = configRom;
    interruptManager_ = interrupts;
    topologyManager_ = topology;
    romScanner_ = romScanner;
    
    // Validate critical dependencies (romScanner is optional for now)
    if (!hardware_ || !workQueue_ || !asyncSubsystem_ || !selfIdCapture_ || !configRomStager_ || !interruptManager_ || !topologyManager_) {
        ASFW_LOG(BusReset, "ERROR: BusResetCoordinator initialized with null dependencies!");
        ASFW_LOG(BusReset, "  hardware=%p workQueue=%p async=%p selfId=%p configRom=%p interrupts=%p topology=%p romScanner=%p",
                 hardware_, workQueue_.get(), asyncSubsystem_, selfIdCapture_, configRomStager_, interruptManager_, topologyManager_, romScanner_);
    }
    
    state_ = State::Idle;
    selfIDComplete1_ = false;
    selfIDComplete2_ = false;
}

// ISR-safe event dispatcher - just posts events to FSM
void BusResetCoordinator::OnIrq(uint32_t intEvent, uint64_t timestamp) {
    bool relevant = false;
    
    if (intEvent & IntEventBits::kBusReset) {
        relevant = true;
        lastResetNs_ = timestamp;
        ProcessEvent(Event::IrqBusReset);
    }
    
    if (intEvent & IntEventBits::kSelfIDComplete) {
        relevant = true;
        lastSelfIdNs_ = timestamp;
        ProcessEvent(Event::IrqSelfIDComplete);
    }
    
    if (intEvent & IntEventBits::kSelfIDComplete2) {
        relevant = true;
        ProcessEvent(Event::IrqSelfIDComplete2);
    }
    
    if (intEvent & IntEventBits::kUnrecoverableError) {
        relevant = true;
        ProcessEvent(Event::Unrecoverable);
    }
    
    if (intEvent & IntEventBits::kRegAccessFail) {
        relevant = true;
        ProcessEvent(Event::RegFail);
    }
    
    // Only schedule FSM if relevant bits were present
    if (relevant && workQueue_) {
        ASFW_LOG(BusReset, "OnIrq: Scheduling RunStateMachine on workQueue (state=%{public}s)", StateString());
        workQueue_->DispatchAsync(^{
            RunStateMachine();
        });
    }
}

void BusResetCoordinator::BindCallbacks(TopologyReadyCallback onTopology) {
    topologyCallback_ = std::move(onTopology);
}

uint64_t BusResetCoordinator::MonotonicNow() {
#ifdef ASFW_HOST_TEST
    using namespace std::chrono;
    return duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count();
#else
    static mach_timebase_info_data_t info = {0, 0};
    if (info.denom == 0) {
        mach_timebase_info(&info);
    }
    const uint64_t ticks = mach_absolute_time();
    return ticks * info.numer / info.denom;
#endif
}

// ============================================================================
// FSM Implementation
// ============================================================================

void BusResetCoordinator::TransitionTo(State newState, const char* reason) {
    if (state_ == newState) return;
    
    const State previous = state_;
    const uint64_t now = MonotonicNow();
    
    // Increment resetCount when entering Detecting state
    if (newState == State::Detecting && previous == State::Idle) {
        metrics_.resetCount++;
        ASFW_LOG(BusReset, "Reset count: %u", metrics_.resetCount);
    }
    
    // Capture Reset Capsule timestamps for structured metrics logging
    if (newState == State::Detecting && previous == State::Idle) {
        firstIrqTime_ = now;
    } else if (newState == State::RestoringConfigROM) {
        busResetClearTime_ = now;  // Bus reset cleared before restoration
    }

    ASFW_LOG(BusReset, "[FSM] %{public}s -> %{public}s: %{public}s",
           StateString(), StateString(newState), reason);
    
    state_ = newState;
    stateEntryTime_ = now;
}

const char* BusResetCoordinator::StateString() const {
    return StateString(state_);
}

const char* BusResetCoordinator::StateString(State s) {
    switch (s) {
        case State::Idle: return "Idle";
        case State::Detecting: return "Detecting";
        case State::WaitingSelfID: return "WaitingSelfID";
        case State::QuiescingAT: return "QuiescingAT";
        case State::RestoringConfigROM: return "RestoringConfigROM";
        case State::ClearingBusReset: return "ClearingBusReset";
        case State::Rearming: return "Rearming";
        case State::Complete: return "Complete";
        case State::Error: return "Error";
    }
    return "Unknown";
}

void BusResetCoordinator::ProcessEvent(Event event) {
    // ProcessEvent is called from OnIrq (ISR context) and should NOT
    // manipulate workInProgress_ - that's RunStateMachine's job!
    // Old code cleared workInProgress_ here, racing with RunStateMachine's lock acquisition
    
    // Global re-entrancy rule: busReset event at any time restarts the flow
    if (event == Event::IrqBusReset) {
        // Abort in-flight ROM scanning from previous generation before starting new reset
        if (romScanner_ && lastGeneration_ > 0) {
            ASFW_LOG(BusReset, "Aborting ROM scan for gen=%u (new bus reset detected)", lastGeneration_);
            romScanner_->Abort(lastGeneration_);
        }
        
        // Clear software latches for discovery readiness (will be set again during reset flow)
        filtersEnabled_ = false;
        atArmed_ = false;
        
        TransitionTo(State::Detecting, "busReset edge detected");
        A_MaskBusReset();
        A_ClearSelfID2Stale();
        selfIDComplete1_ = false;
        selfIDComplete2_ = false;
        return;
    }

    // CRITICAL: Record Self-ID complete events REGARDLESS of current state
    // They may arrive before FSM transitions to WaitingSelfID (simultaneous interrupt delivery)
    // Per OHCI §6.1.1: selfIDComplete and selfIDComplete2 set SIMULTANEOUSLY by hardware
    if (event == Event::IrqSelfIDComplete) {
        selfIDComplete1_ = true;
        selfIDComplete1Time_ = MonotonicNow();
        ASFW_LOG(BusReset, "[FSM] Self-ID phase 1 complete (event recorded)");
        
        // Drain stray Self-ID when not in reset flow (prevents infinite IRQ loop)
        if (state_ == State::Idle || state_ == State::Complete) {
            if (workQueue_) {
                workQueue_->DispatchAsync(^{
                    HandleStraySelfID();
                });
            }
        }
    }
    // TODO: we should read this bit instead of reading interrupt(?)
    if (event == Event::IrqSelfIDComplete2) {
        selfIDComplete2_ = true;
        selfIDComplete2Time_ = MonotonicNow();
        ASFW_LOG(BusReset, "[FSM] Self-ID phase 2 complete (event recorded)");
        
        // Drain stray Self-ID when not in reset flow (prevents infinite IRQ loop)
        if (state_ == State::Idle || state_ == State::Complete) {
            if (workQueue_) {
                workQueue_->DispatchAsync(^{
                    HandleStraySelfID();
                });
            }
        }
    }

    switch (state_) {
        case State::Error:
            ASFW_LOG(BusReset, "[FSM] Error state - ignoring events until reset");
            break;

        default:
            // All other events are handled by guards in RunStateMachine
            break;
    }
}

void BusResetCoordinator::RunStateMachine() {
    // Reentrancy protection with atomic exchange
    if (workInProgress_.exchange(true, std::memory_order_acq_rel)) {
        ASFW_LOG(BusReset, "FSM already running, ignoring reentrant call");
        return;
    }
    
    if (!hardware_) {
        ASFW_LOG(BusReset, "RunStateMachine: hardware_ is NULL!");
        ForceUnmaskBusResetIfNeeded();
        workInProgress_.store(false, std::memory_order_release);
        return;
    }

    // CRITICAL: FSM may transition states multiple times in one call
    // Loop until we reach a stable state (waiting for external event)
    // This prevents needing to reschedule after every transition
    constexpr int kMaxIterations = 10;  // Prevent infinite loops
    int iteration = 0;
    
    while (iteration++ < kMaxIterations) {
        ASFW_LOG_BUSRESET_DETAIL("[FSM] RunStateMachine iteration %d: state=%{public}s selfID1=%d selfID2=%d",
                                 iteration, StateString(), selfIDComplete1_, selfIDComplete2_);

        switch (state_) {
            case State::Idle:
                // Drain stray Self-ID bits to prevent infinite IRQ loop
                // If sticky Self-ID bits are set while Idle, ACK them to clear interrupt source
                if (selfIDComplete1_ || selfIDComplete2_) {
                    ASFW_LOG(BusReset, "[FSM] Idle state - draining stray Self-ID bits (complete1=%d complete2=%d)",
                             selfIDComplete1_, selfIDComplete2_);
                    if (G_NodeIDValid()) {
                        A_DecodeSelfID();  // Optionally decode if NodeID is valid
                    }
                    A_AckSelfIDPair();  // Clear sticky bits to stop IRQ re-assertion
                }
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Idle state - no action");
                ForceUnmaskBusResetIfNeeded();
                workInProgress_.store(false, std::memory_order_release);
                return;  // Exit loop - stable state

            case State::Detecting:
                // Entry actions: mask busReset, clear stale selfIDComplete2
                // Transition to WaitingSelfID after arming Self-ID buffer
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Detecting state - arming Self-ID buffer");
                if (selfIdCapture_) {
                    A_ArmSelfIDBuffer();
                }
                TransitionTo(State::WaitingSelfID, "Self-ID buffer armed");
                // Continue loop - process WaitingSelfID immediately
                continue;

            case State::WaitingSelfID:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] WaitingSelfID state - checking guards: selfID1=%d selfID2=%d",
                                         selfIDComplete1_, selfIDComplete2_);
                
                // Normal path: both bits (OHCI §6.1.1)
                if (G_HaveSelfIDPair()) {
                    // Decode Self-ID data BEFORE transitioning
                    if (!selfIDComplete1Time_) {
                        selfIDComplete1Time_ = MonotonicNow();
                    }
                    A_DecodeSelfID();
                    A_AckSelfIDPair();  // Clear sticky interrupt bits after decode
                    TransitionTo(State::QuiescingAT, "Self-ID pair received + acked");
                    continue;  // Continue loop - process QuiescingAT immediately
                }
                
                // Poll NodeID.iDValid as implicit phase-2 completion (§7.2.3.2)
                // Per OHCI §7.2.3.2: NodeID.iDValid=1 marks completion of entire Self-ID phase
                // This handles controllers where selfIDComplete2 interrupt is dropped/masked
                if (G_NodeIDValid()) {
                    if (!selfIDComplete2_) {
                        selfIDComplete2_ = true;
                        selfIDComplete2Time_ = MonotonicNow();
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] Self-ID phase 2 synthesized via NodeID valid");
                    }
                    if (!selfIDComplete1Time_) {
                        selfIDComplete1Time_ = MonotonicNow();
                    }
                    // Decode Self-ID data BEFORE transitioning
                    A_DecodeSelfID();
                    A_AckSelfIDPair();  // Clear sticky interrupt bits after decode
                    TransitionTo(State::QuiescingAT, "NodeID valid + acked — proceed");
                    continue;  // Continue loop
                }
                
                // Failsafe: if one bit arrived and the other is masked/absent for >2 ms,
                // proceed (some HCs are quirky). This does NOT violate §6.1.1; phase 2
                // (`selfIDComplete2`) is sticky, and HW already clears phase 1 on reset.
                if ((selfIDComplete1_ || selfIDComplete2_) &&
                    (MonotonicNow() - stateEntryTime_) > 2'000'000) {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] Single-bit grace path: complete1=%d complete2=%d",
                                             selfIDComplete1_, selfIDComplete2_);
                    A_AckSelfIDPair();  // Clear sticky interrupt bits (grace path)
                    TransitionTo(State::QuiescingAT, "Self-ID single-bit grace path + acked");
                    continue;  // Continue loop
                } else {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] WaitingSelfID - no guard satisfied, waiting...");
                    workInProgress_.store(false, std::memory_order_release);
                    return;  // Exit - waiting for interrupt
                }

        case State::QuiescingAT:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] QuiescingAT state - stopping AT contexts");
                
                // Stop AT contexts and flush pending descriptors (Linux: context_stop + at_context_flush)
                A_StopFlushAT();
                
                // Poll AT .active bits per OHCI §7.2.3.1 (hardware clears on reset/stop)
                if (G_ATInactive()) {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] AT contexts inactive - continuing to ConfigROM restore");
                    TransitionTo(State::RestoringConfigROM, "AT contexts quiesced");
                    continue;  // Continue loop - process RestoringConfigROM immediately
                } else {
                    // Hardware still active - reschedule and wait
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] AT contexts still active - rescheduling");
                    ScheduleDeferredRun(/*delayMs=*/1, "AT contexts active during QuiescingAT");
                    workInProgress_.store(false, std::memory_order_release);
                    return;  // Exit - waiting for AT contexts to quiesce
                }

            case State::RestoringConfigROM:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] RestoringConfigROM state");
                
                // Restore Config ROM (Self-ID already decoded in WaitingSelfID)
                if (configRomStager_) {
                    A_RestoreConfigROM();
                }
                // Build topology from Self-ID data after decode completes
                A_BuildTopology();
                
                TransitionTo(State::ClearingBusReset, "Config ROM restored + topology built");
                continue;  // Continue loop - process ClearingBusReset immediately

            case State::ClearingBusReset:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] ClearingBusReset state - checking AT inactive");
                
                // Guard: Ensure AT contexts inactive before clearing busReset flag
                if (G_ATInactive()) {
                    A_ClearBusReset();
                    
                    // Re-enable busReset detection ASAP to not miss a subsequent reset edge.
                    // unmask busReset immediately after you clear the event
                    A_UnmaskBusReset();
                    
                    TransitionTo(State::Rearming, "busReset cleared & re-enabled");
                    continue;  // Continue loop - process Rearming immediately
                } else {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] ClearingBusReset - AT still active, waiting");
                    ScheduleDeferredRun(/*delayMs=*/1, "AT contexts active during ClearingBusReset");
                    workInProgress_.store(false, std::memory_order_release);
                    return;  // Exit - waiting for AT contexts
                }

            case State::Rearming:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Rearming state - verifying NodeID valid before AT.run");
                
                // OHCI §7.2.3.2: NodeID.iDValid MUST be set before setting ContextControl.run
                // This ensures Self-ID phase is fully complete and node addressing is stable
                if (!G_NodeIDValid()) {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] Rearming - NodeID not valid yet, rescheduling");
                    // Reschedule and wait; do NOT re-arm AT contexts yet
                    ScheduleDeferredRun(/*delayMs=*/1, "Waiting for NodeID valid");
                    workInProgress_.store(false, std::memory_order_release);
                    return;  // Exit - waiting for NodeID.iDValid
                }
                
                // Re-enable filters and re-arm AT contexts (NodeID now valid)
                A_EnableFilters();
                A_RearmAT();
                
                // Notify AsyncSubsystem that bus reset is complete
                if (asyncSubsystem_ && lastGeneration_ != 0xFF) {
                    asyncSubsystem_->OnBusResetComplete(lastGeneration_);
                }
                
                TransitionTo(State::Complete, "AT contexts re-armed (NodeID valid)");
                continue;  // Continue loop - process Complete immediately

            case State::Complete:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Complete state - finalizing bus reset cycle");
                
                // Log metrics (busReset already unmasked in ClearingBusReset)
                A_MetricsLog();
                
                TransitionTo(State::Idle, "bus reset cycle complete");
                
                ASFW_LOG(BusReset, "Config ROM reading should start here... TODO");
                
#if 0
                // Post discovery kickoff to workloop (deferred from FSM to avoid reentrancy)
                // Bus is ready: NodeID is valid (set in Rearming state), interrupts unmasked, AT contexts armed
                if (topologyCallback_ && lastTopology_.has_value() && workQueue_) {
                    auto topo = *lastTopology_;  // Copy snapshot
                    const auto gen = topo.generation;  // Capture generation for guard
                    ASFW_LOG(BusReset, "Post-reset hooks scheduled for gen=%u", gen);
                    workQueue_->DispatchAsync(^{
                        if (ReadyForDiscovery(gen)) {
                            uint8_t localNode = topo.localNodeId.value_or(0xFF);
                            ASFW_LOG(BusReset, "Discovery start gen=%u local=%u", gen, localNode);
                            topologyCallback_(topo);
                        } else {
                            ASFW_LOG(BusReset, "Discovery deferred gen=%u (invariants: NodeID=%d filters=%d at=%d gen_match=%d)",
                                     gen, G_NodeIDValid(), filtersEnabled_, atArmed_, (gen == lastGeneration_));
                        }
                    });
                } else if (topologyCallback_ && lastTopology_.has_value() && !workQueue_) {
                    ASFW_LOG(BusReset, "WARNING: Cannot schedule discovery - workQueue_ is null");
                }
#endif
                
                continue;  // Continue loop - Idle will return and exit

            case State::Error:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Error state - terminal, requires external recovery");
                ForceUnmaskBusResetIfNeeded();
                workInProgress_.store(false, std::memory_order_release);
                return;  // Exit - terminal state
        }
        
        // Safeguard: If we reach here without continue/return, something is wrong
        ASFW_LOG(BusReset, "[FSM] WARNING: State %d fell through without explicit control flow", static_cast<int>(state_));
        ForceUnmaskBusResetIfNeeded();  // Ensure busReset unmasked on abnormal exit
        workInProgress_.store(false, std::memory_order_release);
        return;
    }
    
    // Max iterations reached
    ASFW_LOG(BusReset, "[FSM] Max iterations (%u) reached in state %d - rescheduling", kMaxIterations, static_cast<int>(state_));
    ForceUnmaskBusResetIfNeeded();  // Ensure busReset unmasked on abnormal exit
    ScheduleDeferredRun(/*delayMs=*/1, "max iteration guard");
    workInProgress_.store(false, std::memory_order_release);
}

// ============================================================================
// FSM Actions
// ============================================================================

void BusResetCoordinator::A_MaskBusReset() {
    // Route through InterruptManager to keep software shadow in sync
    if (!interruptManager_ || !hardware_) return;
    interruptManager_->MaskInterrupts(hardware_, IntEventBits::kBusReset);
    ASFW_LOG(BusReset, "[Action] Masked busReset interrupt");
    busResetMasked_ = true;

    // OHCI §3.1.1.3 + §7.2.3.1:
    // Hardware automatically clears ContextControl.active for AT contexts
    // when a bus reset occurs. This temporary software mask only prevents
    // overlapping busReset edges during our FSM-controlled cleanup.
    // Not required by spec but aligns with Linux post-reset delay behavior.
    //
    // IMPORTANT: Do not mask other interrupt bits here — hardware guarantees
    // isolation between busReset and unrelated DMA contexts.
}

void BusResetCoordinator::A_UnmaskBusReset() {
    // Route through InterruptManager to keep software shadow in sync
    if (!interruptManager_ || !hardware_) return;
    interruptManager_->UnmaskInterrupts(hardware_, IntEventBits::kBusReset);
    ASFW_LOG(BusReset, "[Action] Unmasked busReset (with masterIntEnable ensured)");
    busResetMasked_ = false;
}

void BusResetCoordinator::ForceUnmaskBusResetIfNeeded() {
    if (!busResetMasked_) {
        return;
    }

    if (!interruptManager_ || !hardware_) {
        ASFW_LOG(BusReset,
                 "⚠️  busReset interrupt remained masked but cannot unmask (interruptMgr=%p hardware=%p)",
                 interruptManager_, hardware_);
        return;
    }

    ASFW_LOG(BusReset, "[Action] Forcing busReset interrupt unmask to re-enable future bus reset detection");
    interruptManager_->UnmaskInterrupts(hardware_, IntEventBits::kBusReset);
    busResetMasked_ = false;
}

void BusResetCoordinator::A_ClearSelfID2Stale() {
    if (!hardware_) return;
    hardware_->Write(Register32::kIntEventClear, IntEventBits::kSelfIDComplete2);
    ASFW_LOG(BusReset, "[Action] Cleared stale selfIDComplete2");
}

void BusResetCoordinator::A_ArmSelfIDBuffer() {
    if (!selfIdCapture_ || !hardware_) return;
    kern_return_t ret = selfIdCapture_->Arm(*hardware_);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(BusReset, "[Action] Failed to arm Self-ID buffer: 0x%x", ret);
    }
}

void BusResetCoordinator::A_AckSelfIDPair() {
    if (!hardware_) return;

    // Clear sticky Self-ID interrupt bits now that we've consumed the buffer
    // Per OHCI §6.1.1: selfIDComplete and selfIDComplete2 are sticky status bits
    // that must be explicitly cleared to prevent continuous IRQ assertion
    uint32_t toClear = 0;
    if (selfIDComplete1_) toClear |= IntEventBits::kSelfIDComplete;
    if (selfIDComplete2_) toClear |= IntEventBits::kSelfIDComplete2;

    if (toClear) {
        hardware_->WriteAndFlush(Register32::kIntEventClear, toClear);
        ASFW_LOG(BusReset, "[Action] Acked Self-ID interrupts: clear=0x%08x", toClear);
    } else {
        ASFW_LOG(BusReset, "[Action] AckSelfIDPair skipped (no bits set)");
    }

    // Reset latched flags so next cycle can detect fresh Self-ID pair
    selfIDComplete1_ = false;
    selfIDComplete2_ = false;
}

void BusResetCoordinator::A_StopFlushAT() {
    if (!asyncSubsystem_) return;
    
    // Notify AsyncSubsystem that bus reset is beginning
    // Track next generation (will be confirmed after Self-ID decode)
    const uint8_t nextGen = (lastGeneration_ == 0xFF) ? 0 : static_cast<uint8_t>(lastGeneration_ + 1);
    asyncSubsystem_->OnBusResetBegin(nextGen);
    
    // Per Linux ohci.c bus_reset_work() and OHCI §7.2.3.2:
    // 1. StopATContextsOnly() - clears .run bit, polls .active bit until stopped (synchronous)
    // 2. FlushATContexts() - processes pending descriptors before clearing busReset
    // This matches Linux context_stop() + at_context_flush() sequence
    ASFW_LOG(BusReset, "[Action] Stopping AT contexts (clearing .run, polling .active)");
    asyncSubsystem_->StopATContextsOnly();
    
    ASFW_LOG(BusReset, "[Action] Flushing AT context descriptors");
    asyncSubsystem_->FlushATContexts();
    
    ASFW_LOG(BusReset, "[Action] AT contexts stop+flush complete");
}

void BusResetCoordinator::A_DecodeSelfID() {
    if (!selfIdCapture_ || !hardware_) return;
    
    const uint32_t countReg = hardware_->Read(Register32::kSelfIDCount);
    pendingSelfIDCountReg_ = countReg;
    
    // EXPERIMENTAL: Read NodeID register to test FW642E chip compatibility
    // Per OHCI 1.1 §5.11: NodeID register contains iDValid, root, CPS, busNumber, nodeNumber
    // Testing hypothesis: Newer chips (FW642E) still implement standard OHCI NodeID register
    // OHCI §5.11 Table 47: bit 31=iDValid, bit 30=root, bits 15:6=busNumber, bits 5:0=nodeNumber
    const uint32_t nodeIDReg = hardware_->Read(Register32::kNodeID);
    const bool iDValid = (nodeIDReg & 0x80000000) != 0;
    const bool isRoot = (nodeIDReg & 0x40000000) != 0;
    const uint8_t busNumber = static_cast<uint8_t>((nodeIDReg >> 6) & 0x3FF);
    const uint8_t nodeNumber = static_cast<uint8_t>(nodeIDReg & 0x3F);
    
    ASFW_LOG(BusReset, "🧪 EXPERIMENTAL NodeID read (testing FW642E): raw=0x%08x iDValid=%d root=%d bus=%u node=%u",
             nodeIDReg, iDValid, isRoot, busNumber, nodeNumber);
    if (nodeNumber == 63) {
        ASFW_LOG(BusReset, "  ⚠️ nodeNumber=63 indicates invalid/unset node ID");
    }
    if (!iDValid) {
        ASFW_LOG(BusReset, "  ⚠️ iDValid=0 indicates Self-ID phase not complete (unexpected at this point!)");
    }
    
    auto result = selfIdCapture_->Decode(countReg, *hardware_);
    lastSelfId_ = result;  // Cache for A_BuildTopology() and A_MetricsLog()
    
    if (result && result->valid) {
        lastGeneration_ = result->generation;  // Track for ROMScanner abort
        ASFW_LOG(BusReset, "[Action] Self-ID decoded: gen=%u, %zu quads",
               result->generation, result->quads.size());
        if (asyncSubsystem_) {
            // Confirm generation with AsyncSubsystem's coordinator path which delegates
            // to GenerationTracker.
            asyncSubsystem_->ConfirmBusGeneration(static_cast<uint8_t>(result->generation & 0xFF));
        }
    } else {
        ASFW_LOG(BusReset, "[Action] Self-ID decode failed");
        if (result && !result->crcError && !result->timedOut) {
            metrics_.lastFailureReason = "Self-ID generation mismatch (racing bus reset)";
        } else if (result && result->crcError) {
            metrics_.lastFailureReason = "Self-ID CRC error";
        } else if (result && result->timedOut) {
            metrics_.lastFailureReason = "Self-ID timeout";
        } else {
            metrics_.lastFailureReason = "Self-ID decode failed";
        }
    }
}

void BusResetCoordinator::A_BuildTopology() {
    if (!topologyManager_ || !selfIdCapture_ || !hardware_) {
        ASFW_LOG(Topology,
                 "⚠️  A_BuildTopology skipped: topology=%p selfId=%p hardware=%p",
                 topologyManager_, selfIdCapture_, hardware_);
        return;
    }

    ASFW_LOG(Topology, "📡 A_BuildTopology invoked (cached lastSelfId valid=%d)",
             lastSelfId_.has_value() && lastSelfId_->valid);

    // Use cached Self-ID decode result (already decoded in A_DecodeSelfID)
    if (!lastSelfId_ || !lastSelfId_->valid) {
        ASFW_LOG(BusReset, "[Action] Topology build skipped - no valid cached Self-ID data");
        return;
    }
    
    // Read NodeID register to pass local node information to topology builder
    const uint32_t nodeIDReg = hardware_->Read(Register32::kNodeID);
    const uint64_t timestamp = MonotonicNow();
    
    // Build topology snapshot from cached Self-ID data
    auto snapshot = topologyManager_->UpdateFromSelfID(*lastSelfId_, timestamp, nodeIDReg);
    
    if (snapshot.has_value()) {
        ASFW_LOG(BusReset, "[Action] Topology built: gen=%u nodes=%u root=%{public}s IRM=%{public}s local=%{public}s",
                 snapshot->generation,
                 snapshot->nodeCount,
                 snapshot->rootNodeId.has_value() ? std::to_string(*snapshot->rootNodeId).c_str() : "none",
                 snapshot->irmNodeId.has_value() ? std::to_string(*snapshot->irmNodeId).c_str() : "none",
                 snapshot->localNodeId.has_value() ? std::to_string(*snapshot->localNodeId).c_str() : "none");
        
        // Cache topology snapshot for callback invocation after bus reset completes
        // Callback will fire in Idle state when bus is fully operational (NodeID valid, interrupts unmasked)
        lastTopology_ = *snapshot;
    } else {
        ASFW_LOG(BusReset, "[Action] Topology build returned nullopt - invalid Self-ID data");
        // Clear lastTopology_ on invalid build
        lastTopology_ = std::nullopt;
    }
}

// Single-point ConfigROM restoration with strict ordering
// Per OHCI §5.5.6: ConfigROMheader must be written LAST to atomically publish ROM
// Per Linux bus_reset_work (ohci.c:2168-2184): 3-step sequence prevents races
void BusResetCoordinator::A_RestoreConfigROM() {
    if (!configRomStager_ || !hardware_) return;
    
    // Step 1: Restore header quadlet in DMA buffer (host memory only, no register writes)
    // This ensures subsequent hardware DMA reads get correct header value
    configRomStager_->RestoreHeaderAfterBusReset();
    ASFW_LOG(BusReset, "[Action] Config ROM DMA buffer header restored (step 1/3)");
    
    // Step 2: Write BusOptions register
    // Per OHCI §5.5.6: BusOptions must be written before ConfigROMheader
    const uint32_t busOpts = configRomStager_->ExpectedBusOptions();
    hardware_->WriteAndFlush(Register32::kBusOptions, busOpts);
    ASFW_LOG(BusReset, "[Action] BusOptions register written: 0x%08x (step 2/3)", busOpts);
    
    // Step 3: Write ConfigROMheader register LAST (atomic publish)
    // Per OHCI §5.5.6: Writing header signals Config ROM is ready for serving
    // Per Linux: reg_write(ohci, OHCI1394_ConfigROMhdr, be32_to_cpu(ohci->next_header));
    const uint32_t romHeader = configRomStager_->ExpectedHeader();
    hardware_->WriteAndFlush(Register32::kConfigROMHeader, romHeader);
    ASFW_LOG(BusReset, "[Action] ConfigROMheader written: 0x%08x (step 3/3 - ROM ready)", romHeader);

    // OHCI §5.5.6 — ConfigROM restoration rules:
    // 1. BusOptions must be written BEFORE ConfigROMheader
    // 2. ConfigROMheader write acts as an atomic publish operation
    // 3. Shadow update must complete before linkEnable resumes requests
    // These ordering rules prevent invalid BIBimage reads during recovery.
}

// Per OHCI §6.1.1: Can only clear after Self-ID complete and AT contexts inactive
void BusResetCoordinator::A_ClearBusReset() {
    if (!hardware_) return;
    hardware_->WriteAndFlush(Register32::kIntEventClear, IntEventBits::kBusReset);
    busResetClearTime_ = MonotonicNow();
    
    // Read-back to prove event is cleared (diagnostic)
    const uint32_t evt = hardware_->Read(Register32::kIntEvent);
    ASFW_LOG(BusReset, "[Action] busReset interrupt event cleared (IntEvent post-clear=0x%08x)", evt);
}

// Re-enable AsynchronousRequestFilter after busReset cleared
// Per OHCI §C.3: Prevents async requests arriving during bus reset state
void BusResetCoordinator::A_EnableFilters() {
    if (!hardware_) return;
    
    // Use shared constant from OHCIConstants.hpp
    hardware_->Write(Register32::kAsReqFilterHiSet, kAsReqAcceptAllMask);
    filtersEnabled_ = true;  // Set software latch for discovery readiness
    ASFW_LOG(BusReset, "[Action] AsynchronousRequestFilter enabled (accept all) - filters enabled latch set");
}

// Per OHCI §7.2.3.2 step 7: Re-arm must happen AFTER busReset cleared
void BusResetCoordinator::A_RearmAT() {
    if (!asyncSubsystem_) return;
    asyncSubsystem_->RearmATContexts();
    atArmed_ = true;  // Set software latch for discovery readiness
    ASFW_LOG(BusReset, "[Action] AT contexts re-armed - AT armed latch set");

    // OHCI §7.2.3.2 — Re-arm transmit contexts:
    // Re-arming (writing CommandPtr and setting ContextControl.run)
    // must occur *after* busReset is cleared and ConfigROM restored.
    // At this stage ContextControl.active == 0 (hardware cleared it)
    // and CommandPtr descriptors are valid again. Safe restart point.
}

void BusResetCoordinator::A_MetricsLog() {
    const uint64_t now = MonotonicNow();
    const uint64_t completionTime = now;
    const uint64_t durationNs = completionTime - firstIrqTime_;
    // Read final NodeID to capture our bus position
    uint32_t finalNodeID = 0;
    bool nodeIDValid = false;
    if (hardware_) {
        finalNodeID = hardware_->Read(Register32::kNodeID);
        nodeIDValid = (finalNodeID & 0x80000000) != 0;  // iDValid bit
    }
    
    // Extract generation from cached Self-ID decode result
    uint32_t generation = 0;
    if (lastSelfId_ && lastSelfId_->valid) {
        generation = lastSelfId_->generation;
    }
    
    const uint32_t nodeNumber = finalNodeID & 0x3Fu;
    const uint32_t busNumber = (finalNodeID >> 6) & 0x3FFu;  // Fixed: bits[15:6] per OHCI §5.11 Table 47
    const double durationMsDouble = static_cast<double>(durationNs) / 1'000'000.0;

    ASFW_LOG(BusReset,
             "Bus reset #%u complete: duration=%.2f ms gen=%u nodeID=0x%08x(bus=%u node=%u valid=%d) aborts=%u",
             metrics_.resetCount,
             durationMsDouble,
             generation,
             finalNodeID,
             busNumber,
             nodeNumber,
             nodeIDValid,
             metrics_.abortCount);

#if ASFW_DEBUG_BUS_RESET
    ASFW_LOG_BUSRESET_DETAIL("  first_irq=%llu selfid1=%llu selfid2=%llu cleared=%llu completed=%llu",
                             firstIrqTime_,
                             selfIDComplete1Time_,
                             selfIDComplete2Time_,
                             busResetClearTime_,
                             completionTime);
#endif

    if (metrics_.lastFailureReason.has_value()) {
        ASFW_LOG(BusReset, "  Last failure cleared: %{public}s", metrics_.lastFailureReason->c_str());
    }
    
    // Update BusResetMetrics structure for DriverKit status queries
    metrics_.lastResetStart = firstIrqTime_;
    metrics_.lastResetCompletion = completionTime;
    metrics_.lastFailureReason.reset();  // Clear stale failure text on success
}

void BusResetCoordinator::ScheduleDeferredRun(uint32_t delayMs, const char* reason) {
    if (!workQueue_) {
        return;
    }

    bool expected = false;
    if (!deferredRunScheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ASFW_LOG_BUSRESET_DETAIL("[FSM] Deferred run already scheduled (reason=%{public}s)",
                                 reason ? reason : "unknown");
        return;
    }

    workQueue_->DispatchAsync(^{
#ifdef ASFW_HOST_TEST
        if (delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
        }
#else
        if (delayMs > 0) {
            IOSleep(delayMs);
        }
#endif
        deferredRunScheduled_.store(false, std::memory_order_release);
        this->RunStateMachine();
    });
}

// ============================================================================
// FSM Guards
// ============================================================================

bool BusResetCoordinator::G_ATInactive() {
    // Per Linux ohci.c context_stop(): Poll CONTEXT_ACTIVE bit with timeout
    // Linux polls up to 1000 times with 10μs delay (max 10ms total)
    // DriverKit can't block that long, so we do a few quick polls and reschedule if needed
    
    if (!hardware_) return false;
    
    // OHCI §3.1: ContextControl is read/write; *Set/*Clear are write-only strobes
    // Read from ControlSet offset (same as Base for AT contexts) to get current .active/.run state
    const uint32_t atReqControl = hardware_->Read(static_cast<Register32>(DMAContextHelpers::AsReqTrContextControlSet));
    const uint32_t atRspControl = hardware_->Read(static_cast<Register32>(DMAContextHelpers::AsRspTrContextControlSet));
    
    const bool atReqActive = (atReqControl & kContextControlActiveBit) != 0;
    const bool atRspActive = (atRspControl & kContextControlActiveBit) != 0;

    // OHCI §3.1.1.3 — ContextControl.active:
    // Hardware clears this bit after bus reset when DMA controller reaches safe stop point
    // Per §7.2.3.2: Software must wait for .active==0 before clearing busReset interrupt
    
    const bool inactive = !atReqActive && !atRspActive;
    
    if (!inactive) {
        ASFW_LOG_BUSRESET_DETAIL("[Guard] AT still active: Req=%d Rsp=%d (will retry)", atReqActive, atRspActive);
    } else {
        ASFW_LOG_BUSRESET_DETAIL("[Guard] AT contexts now INACTIVE - safe to proceed");
    }
    
    return inactive;
}

bool BusResetCoordinator::G_HaveSelfIDPair() {
    return selfIDComplete1_ && selfIDComplete2_;
}

bool BusResetCoordinator::G_ROMImageReady() {
    // NOTE: Simple null-check validates ConfigROMStager is initialized and ready.
    // ConfigROMStager::StageImage() must be called during ControllerCore::Start()
    // before any bus reset occurs. Non-null pointer indicates successful staging.
    // Future enhancement: Add explicit ConfigROMStager::IsReady() status method.
    return configRomStager_ != nullptr;
}

bool BusResetCoordinator::G_NodeIDValid() const {
    if (!hardware_) return false;
    uint32_t nodeId = hardware_->Read(Register32::kNodeID);
    // Check iDValid bit and nodeNumber != 63
    return (nodeId & 0x80000000) && ((nodeId & 0x3F) != 63);
}

bool BusResetCoordinator::ReadyForDiscovery(Discovery::Generation gen) const {
    if (!G_NodeIDValid()) return false;
    if (!filtersEnabled_ || !atArmed_) return false;
    if (!lastTopology_.has_value()) return false;
    if (gen != lastGeneration_) return false;  // stale kickoff guard
    return true;
}

// Handle stray Self-ID interrupts that arrive outside normal reset flow
// This prevents infinite IRQ loops from sticky selfIDComplete/selfIDComplete2 bits
void BusResetCoordinator::HandleStraySelfID() {
    if (!hardware_ || !selfIdCapture_) {
        ASFW_LOG(BusReset, "[FSM] HandleStraySelfID: missing dependencies (hw=%p selfId=%p)",
                 hardware_, selfIdCapture_);
        return;
    }
    
    // If NodeID.iDValid=1, treat as late completion and synthesize normal path
    if (G_NodeIDValid()) {
        ASFW_LOG(BusReset, "[FSM] Stray Self-ID while Idle, NodeID valid → synthesize reset completion");
        A_DecodeSelfID();
        A_AckSelfIDPair();  // Clear sticky bits 15/16
        TransitionTo(State::QuiescingAT, "SYNTH: Self-ID complete while Idle");
        RunStateMachine();  // Continue FSM processing
        return;
    }
    
    // NodeID invalid - just ACK and ignore (late/spurious interrupt)
    ASFW_LOG(BusReset, "[FSM] Stray Self-ID while Idle, NodeID invalid → ack & ignore");
    A_AckSelfIDPair();  // Clear sticky bits 15/16, remain Idle
}

} // namespace ASFW::Driver
