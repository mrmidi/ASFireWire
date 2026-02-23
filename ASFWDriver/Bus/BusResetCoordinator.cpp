#include "BusResetCoordinator.hpp"

#ifdef ASFW_HOST_TEST
#include <chrono>
#include <thread>
#else
#include <DriverKit/IOLib.h>
#endif

#include "HardwareInterface.hpp"
#include "SelfIDCapture.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "InterruptManager.hpp"
#include "TopologyManager.hpp"
#include "Logging.hpp"
#include "../Hardware/OHCIConstants.hpp"
#include "../Async/AsyncSubsystem.hpp"
#include "../ConfigROM/ROMScanner.hpp"
// NEW: BusManager.hpp
#include "BusManager.hpp"

namespace ASFW::Driver {

BusResetCoordinator::BusResetCoordinator() = default;
BusResetCoordinator::~BusResetCoordinator() = default;

void BusResetCoordinator::Initialize(HardwareInterface* hw,
                                    OSSharedPtr<IODispatchQueue> workQueue,
                                    Async::AsyncSubsystem* asyncSys,
                                    SelfIDCapture* selfIdCapture,
                                    ConfigROMStager* configRom,
                                    InterruptManager* interrupts,
                                    TopologyManager* topology,
                                    BusManager* busManager,
                                    Discovery::ROMScanner* romScanner) {
    hardware_ = hw;
    workQueue_ = std::move(workQueue);
    asyncSubsystem_ = asyncSys;
    selfIdCapture_ = selfIdCapture;
    configRomStager_ = configRom;
    interruptManager_ = interrupts;
    topologyManager_ = topology;
    busManager_ = busManager;
    romScanner_ = romScanner;
    pendingPhyCommand_.reset();
    pendingPhyReason_.clear();
    pendingManagedReset_ = false;

    if (!hardware_ || !workQueue_ || !asyncSubsystem_ || !selfIdCapture_ || !configRomStager_ || !interruptManager_ || !topologyManager_) {
        ASFW_LOG(BusReset, "ERROR: BusResetCoordinator initialized with null dependencies!");
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
    if (event == Event::IrqBusReset) {
        const uint64_t now = MonotonicNow();
        const uint64_t sinceLastMs = (lastResetNs_ > 0 && now > lastResetNs_)
            ? (now - lastResetNs_) / 1'000'000 : 0;
        ASFW_LOG(BusReset,
                 "══ BUS RESET ══ gen=%u state=%{public}s sinceLastReset=%llums "
                 "prevScanBusy=%d filtersEnabled=%d atArmed=%d",
                 lastGeneration_, StateString(), sinceLastMs,
                 previousScanHadBusyNodes_, filtersEnabled_, atArmed_);

        if (romScanner_ && lastGeneration_ > 0) {
            ASFW_LOG(BusReset, "  Aborting ROM scan for gen=%u", lastGeneration_);
            romScanner_->Abort(lastGeneration_);
        }

        filtersEnabled_ = false;
        atArmed_ = false;

        TransitionTo(State::Detecting, "busReset edge detected");
        A_MaskBusReset();
        A_ClearSelfID2Stale();
        selfIDComplete1_ = false;
        selfIDComplete2_ = false;
        return;
    }

    if (event == Event::IrqSelfIDComplete) {
        selfIDComplete1_ = true;
        selfIDComplete1Time_ = MonotonicNow();
        ASFW_LOG(BusReset, "[FSM] Self-ID phase 1 complete");
        
        if (state_ == State::Idle || state_ == State::Complete) {
            if (workQueue_) {
                workQueue_->DispatchAsync(^{
                    HandleStraySelfID();
                });
            }
        }
    }
    
    if (event == Event::IrqSelfIDComplete2) {
        selfIDComplete2_ = true;
        selfIDComplete2Time_ = MonotonicNow();
        ASFW_LOG(BusReset, "[FSM] Self-ID phase 2 complete");
        
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
            ASFW_LOG(BusReset, "[FSM] Error state - ignoring events");
            break;

        default:
            break;
    }
}

void BusResetCoordinator::RunStateMachine() {
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

    constexpr int kMaxIterations = 10;
    int iteration = 0;
    
    while (iteration++ < kMaxIterations) {
        ASFW_LOG_BUSRESET_DETAIL("[FSM] RunStateMachine iteration %d: state=%{public}s selfID1=%d selfID2=%d",
                                 iteration, StateString(), selfIDComplete1_, selfIDComplete2_);

        switch (state_) {
            case State::Idle:
                if (selfIDComplete1_ || selfIDComplete2_) {
                    ASFW_LOG(BusReset, "[FSM] Idle state - draining stray Self-ID bits");
                    if (G_NodeIDValid()) {
                        A_DecodeSelfID();
                    }
                    A_AckSelfIDPair();
                }
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Idle state - no action");
                ForceUnmaskBusResetIfNeeded();
                workInProgress_.store(false, std::memory_order_release);
                return;

            case State::Detecting:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Detecting state - arming Self-ID buffer");
                if (selfIdCapture_) {
                    A_ArmSelfIDBuffer();
                }
                TransitionTo(State::WaitingSelfID, "Self-ID buffer armed");
                continue;

            case State::WaitingSelfID:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] WaitingSelfID state - checking guards: selfID1=%d selfID2=%d",
                                         selfIDComplete1_, selfIDComplete2_);
                
                if (G_HaveSelfIDPair()) {
                    if (!selfIDComplete1Time_) {
                        selfIDComplete1Time_ = MonotonicNow();
                    }
                    A_DecodeSelfID();
                    A_AckSelfIDPair();
                    TransitionTo(State::QuiescingAT, "Self-ID pair received + acked");
                    continue;
                }
                
                if (G_NodeIDValid()) {
                    if (!selfIDComplete2_) {
                        selfIDComplete2_ = true;
                        selfIDComplete2Time_ = MonotonicNow();
                        ASFW_LOG_BUSRESET_DETAIL("[FSM] Self-ID phase 2 synthesized via NodeID valid");
                    }
                    if (!selfIDComplete1Time_) {
                        selfIDComplete1Time_ = MonotonicNow();
                    }
                    A_DecodeSelfID();
                    A_AckSelfIDPair();
                    TransitionTo(State::QuiescingAT, "NodeID valid + acked — proceed");
                    continue;
                }
                
                if ((selfIDComplete1_ || selfIDComplete2_) &&
                    (MonotonicNow() - stateEntryTime_) > 2'000'000) {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] Single-bit grace path: complete1=%d complete2=%d",
                                             selfIDComplete1_, selfIDComplete2_);
                    A_AckSelfIDPair();
                    TransitionTo(State::QuiescingAT, "Self-ID single-bit grace path + acked");
                    continue;
                } else {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] WaitingSelfID - no guard satisfied, waiting...");
                    workInProgress_.store(false, std::memory_order_release);
                    return;
                }

        case State::QuiescingAT:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] QuiescingAT state - stopping AT contexts");
                
                A_StopFlushAT();
                
                if (G_ATInactive()) {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] AT contexts inactive - continuing to ConfigROM restore");
                    TransitionTo(State::RestoringConfigROM, "AT contexts quiesced");
                    continue;
                } else {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] AT contexts still active - rescheduling");
                    ScheduleDeferredRun(1, "AT contexts active during QuiescingAT");
                    workInProgress_.store(false, std::memory_order_release);
                    return;
                }

            case State::RestoringConfigROM:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] RestoringConfigROM state");

                if (configRomStager_) {
                    A_RestoreConfigROM();
                }
                A_BuildTopology();
                if (lastTopology_.has_value()) {
                    EvaluateRootDelegation(*lastTopology_);
                }

                ASFW_LOG(BusReset, "🔍 BusManager check: busManager_=%p lastTopology_=%d (gen=%u)",
                         busManager_, lastTopology_.has_value(),
                         lastTopology_.has_value() ? lastTopology_->generation : 0xFF);
                if (busManager_ && lastTopology_.has_value()) {
                    if (auto phyCmd = busManager_->AssignCycleMaster(*lastTopology_,
                                                                     topologyManager_->GetBadIRMFlags())) {
                        StageDelayedPhyPacket(*phyCmd, "AssignCycleMaster");
                    }

                    if (pendingManagedReset_) {
                        ASFW_LOG(BusReset, "[FSM] BusManager staged PHY packet; will trigger reset after completion");
                    }
                }

                TransitionTo(State::ClearingBusReset, "Config ROM restored + topology built + bus managed");
                continue;

            case State::ClearingBusReset:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] ClearingBusReset state - checking AT inactive");
                
                if (G_ATInactive()) {
                    A_ClearBusReset();
                    A_UnmaskBusReset();
                    
                    TransitionTo(State::Rearming, "busReset cleared & re-enabled");
                    continue;
                } else {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] ClearingBusReset - AT still active, waiting");
                    ScheduleDeferredRun(1, "AT contexts active during ClearingBusReset");
                    workInProgress_.store(false, std::memory_order_release);
                    return;
                }

            case State::Rearming:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Rearming state - verifying NodeID valid before AT.run");
                
                if (!G_NodeIDValid()) {
                    ASFW_LOG_BUSRESET_DETAIL("[FSM] Rearming - NodeID not valid yet, rescheduling");
                    ScheduleDeferredRun(1, "Waiting for NodeID valid");
                    workInProgress_.store(false, std::memory_order_release);
                    return;
                }
                
                A_EnableFilters();
                A_RearmAT();
                
                if (asyncSubsystem_ && lastGeneration_ != 0xFF) {
                    asyncSubsystem_->OnBusResetComplete(lastGeneration_);
                }
                
                TransitionTo(State::Complete, "AT contexts re-armed (NodeID valid)");
                continue;

            case State::Complete:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Complete state - finalizing bus reset cycle");
                
                A_MetricsLog();

                if (pendingManagedReset_ && pendingPhyCommand_.has_value()) {
                    ASFW_LOG(BusReset, "Dispatching staged PHY packet (reason=%s)",
                             pendingPhyReason_.c_str());
                    if (DispatchPendingPhyPacket()) {
                        workInProgress_.store(false, std::memory_order_release);
                        return;
                    } else {
                        ASFW_LOG(BusReset, "⚠️  Failed to dispatch staged PHY packet - clearing request");
                        pendingManagedReset_ = false;
                        pendingPhyCommand_.reset();
                        pendingPhyReason_.clear();
                    }
                }

                if (!pendingManagedReset_) {
                    A_SendGlobalResumeIfNeeded();
                }
                
                TransitionTo(State::Idle, "bus reset cycle complete");

                if (topologyCallback_ && lastTopology_.has_value() && workQueue_) {
                    auto topo = *lastTopology_;
                    const auto gen = topo.generation;

                    if (previousScanHadBusyNodes_ && currentDiscoveryDelayMs_ > 0) {
                        // DICE/Saffire-class devices: delay discovery to let firmware
                        // finish booting before we start a new scan.  The generation
                        // staleness check inside the callback prevents acting on a
                        // stale topology if another bus reset occurs during the delay.
                        // Delay escalates with consecutive failures (2s→4s→6s→8s→10s).
                        const uint32_t delayMs = currentDiscoveryDelayMs_;
                        ASFW_LOG(BusReset, "Discovery delayed %ums for gen=%u (ack_busy in prev scan)",
                                 delayMs, gen);
                        workQueue_->DispatchAsync(^{
#ifdef ASFW_HOST_TEST
                            std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
#else
                            IOSleep(delayMs);
#endif
                            if (ReadyForDiscovery(gen)) {
                                uint8_t localNode = topo.localNodeId.value_or(0xFF);
                                ASFW_LOG(BusReset, "Discovery start gen=%u local=%u (after %ums delay)",
                                         gen, localNode, delayMs);
                                topologyCallback_(topo);
                            } else {
                                ASFW_LOG(BusReset, "Discovery cancelled gen=%u (stale after delay)", gen);
                            }
                        });
                    } else {
                        ASFW_LOG(BusReset, "Post-reset hooks scheduled for gen=%u", gen);
                        workQueue_->DispatchAsync(^{
                            if (ReadyForDiscovery(gen)) {
                                uint8_t localNode = topo.localNodeId.value_or(0xFF);
                                ASFW_LOG(BusReset, "Discovery start gen=%u local=%u", gen, localNode);
                                topologyCallback_(topo);
                            } else {
                                ASFW_LOG(BusReset, "Discovery deferred gen=%u", gen);
                            }
                        });
                    }
                }
                
                continue;

            case State::Error:
                ASFW_LOG_BUSRESET_DETAIL("[FSM] Error state - terminal");
                ForceUnmaskBusResetIfNeeded();
                workInProgress_.store(false, std::memory_order_release);
                return;
        }
        
        ASFW_LOG(BusReset, "[FSM] WARNING: State %d fell through", static_cast<int>(state_));
        ForceUnmaskBusResetIfNeeded();
        workInProgress_.store(false, std::memory_order_release);
        return;
    }
    
    ASFW_LOG(BusReset, "[FSM] Max iterations (%u) reached in state %d - rescheduling", kMaxIterations, static_cast<int>(state_));
    ForceUnmaskBusResetIfNeeded();
    ScheduleDeferredRun(1, "max iteration guard");
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
    
    const uint8_t nextGen = (lastGeneration_ == 0xFF) ? 0 : static_cast<uint8_t>(lastGeneration_ + 1);
    asyncSubsystem_->OnBusResetBegin(nextGen);
    
    ASFW_LOG(BusReset, "[Action] Stopping AT contexts");
    asyncSubsystem_->StopATContextsOnly();
    
    ASFW_LOG(BusReset, "[Action] Flushing AT context descriptors");
    asyncSubsystem_->FlushATContexts();
    
    ASFW_LOG(BusReset, "[Action] AT contexts stop+flush complete");
}

void BusResetCoordinator::A_DecodeSelfID() {
    if (!selfIdCapture_ || !hardware_) return;
    
    const uint32_t countReg = hardware_->Read(Register32::kSelfIDCount);
    pendingSelfIDCountReg_ = countReg;
    
    const uint32_t nodeIDReg = hardware_->Read(Register32::kNodeID);
    const bool iDValid = (nodeIDReg & 0x80000000) != 0;
    
    if (!iDValid) {
        ASFW_LOG(BusReset, "  ⚠️ iDValid=0 indicates Self-ID phase not complete");
    }
    
    auto result = selfIdCapture_->Decode(countReg, *hardware_);
    lastSelfId_ = result;
    
    if (result && result->valid) {
        lastGeneration_ = result->generation;
        ASFW_LOG(BusReset, "[Action] Self-ID decoded: gen=%u, %zu quads",
               result->generation, result->quads.size());
        if (asyncSubsystem_) {
            asyncSubsystem_->ConfirmBusGeneration(static_cast<uint8_t>(result->generation & 0xFF));
        }

        if (hardware_ && result->quads.size() > 1) {
            constexpr uint32_t kGapMask = 0x003F0000u;
            const uint32_t localSelfId = result->quads[1];
            const uint8_t gapCount = static_cast<uint8_t>((localSelfId & kGapMask) >> 16);

            if (gapCount == 0) {
                ASFW_LOG(BusReset,
                         "⚠️ Local gap count zero – sending reactive PHY fix");

                BusManager::PhyConfigCommand fix{};
                fix.gapCount = std::uint8_t{0x3F};

                if (hardware_->SendPhyConfig(fix.gapCount,
                                             fix.forceRootNodeID,
                                             "GapCountZeroFix")) {
                    ASFW_LOG(BusReset, "Reactive gap fix PHY packet sent; initiating short bus reset");
                    const bool resetOk = hardware_->InitiateBusReset(true);
                    if (!resetOk) {
                        ASFW_LOG_ERROR(BusReset, "Reactive short reset failed to start");
                    }
                    return;
                } else {
                    ASFW_LOG_ERROR(BusReset, "Reactive gap fix PHY send failed");
                }
            }
        }
    } else {
        ASFW_LOG(BusReset, "[Action] Self-ID decode failed");
        if (result && !result->crcError && !result->timedOut) {
            metrics_.lastFailureReason = "Self-ID generation mismatch";
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
        return;
    }

    if (!lastSelfId_ || !lastSelfId_->valid) {
        return;
    }
    
    const uint32_t nodeIDReg = hardware_->Read(Register32::kNodeID);
    const uint64_t timestamp = MonotonicNow();
    
    auto snapshot = topologyManager_->UpdateFromSelfID(*lastSelfId_, timestamp, nodeIDReg);
    
    if (snapshot.has_value()) {
        ASFW_LOG(BusReset, "[Action] Topology built: gen=%u nodes=%u root=%{public}s IRM=%{public}s local=%{public}s",
                 snapshot->generation,
                 snapshot->nodeCount,
                 snapshot->rootNodeId.has_value() ? std::to_string(*snapshot->rootNodeId).c_str() : "none",
                 snapshot->irmNodeId.has_value() ? std::to_string(*snapshot->irmNodeId).c_str() : "none",
                 snapshot->localNodeId.has_value() ? std::to_string(*snapshot->localNodeId).c_str() : "none");
        
        lastTopology_ = *snapshot;
    } else {
        ASFW_LOG(BusReset, "[Action] Topology build returned nullopt");
        lastTopology_ = std::nullopt;
    }
}

void BusResetCoordinator::A_RestoreConfigROM() {
    if (!configRomStager_ || !hardware_) return;
    
    configRomStager_->RestoreHeaderAfterBusReset();
    
    const uint32_t busOpts = configRomStager_->ExpectedBusOptions();
    hardware_->WriteAndFlush(Register32::kBusOptions, busOpts);
    
    const uint32_t romHeader = configRomStager_->ExpectedHeader();
    hardware_->WriteAndFlush(Register32::kConfigROMHeader, romHeader);
    ASFW_LOG(BusReset, "[Action] ConfigROMheader written: 0x%08x", romHeader);
}

void BusResetCoordinator::A_ClearBusReset() {
    if (!hardware_) return;
    hardware_->WriteAndFlush(Register32::kIntEventClear, IntEventBits::kBusReset);
    busResetClearTime_ = MonotonicNow();
    
    ASFW_LOG(BusReset, "[Action] busReset interrupt event cleared");
}

void BusResetCoordinator::A_EnableFilters() {
    if (!hardware_) return;
    
    hardware_->Write(Register32::kAsReqFilterHiSet, kAsReqAcceptAllMask);
    filtersEnabled_ = true;
    ASFW_LOG(BusReset, "[Action] AsynchronousRequestFilter enabled");
}

void BusResetCoordinator::A_RearmAT() {
    if (!asyncSubsystem_) return;
    asyncSubsystem_->RearmATContexts();
    atArmed_ = true;
    ASFW_LOG(BusReset, "[Action] AT contexts re-armed");
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

void BusResetCoordinator::A_SendGlobalResumeIfNeeded() {
    if (!hardware_) {
        return;
    }
    if (!lastTopology_.has_value() || !lastTopology_->localNodeId.has_value()) {
        return;
    }

    const uint32_t generation = lastTopology_->generation;
    if (lastResumeGeneration_ == generation) {
        return;
    }

    const uint8_t localNode = *lastTopology_->localNodeId;
    if (hardware_->SendPhyGlobalResume(localNode)) {
        lastResumeGeneration_ = generation;
        ASFW_LOG(BusReset,
                 "PHY Global Resume dispatched (node=%u generation=%u)",
                 localNode,
                 generation);
    } else {
        ASFW_LOG_ERROR(BusReset,
                       "PHY Global Resume failed to send (node=%u generation=%u)",
                       localNode,
                       generation);
    }
}

void BusResetCoordinator::StageDelayedPhyPacket(const BusManager::PhyConfigCommand& command,
                                                const char* reason) {
    const bool isDelegate = (reason && std::strcmp(reason, "AssignCycleMaster") == 0);

    // Check persistent suppression first (Linux pattern: prevents infinite loops)
    if (isDelegate && delegateSuppressed_) {
        ASFW_LOG(BusReset, "Root delegation suppressed (exceeded retry limit of %u)", kMaxDelegateRetries);
        return;
    }

    if (pendingPhyCommand_.has_value()) {
        ASFW_LOG(BusReset,
                 "Deferred PHY packet already staged (existing reason=%{public}s) - ignoring new request",
                 pendingPhyReason_.c_str());
        return;
    }

    if (isDelegate && delegateAttemptActive_) {
        ASFW_LOG(BusReset,
                 "Skipping new AssignCycleMaster request - previous delegation still in flight (target=%u)",
                 delegateTarget_);
        return;
    }

    if (isDelegate && command.forceRootNodeID.has_value()) {
        // Check if target changed (topology change)
        const uint8_t newTarget = *command.forceRootNodeID;
        if (newTarget != delegateTarget_) {
            // Target changed - reset retry counter
            delegateRetryCount_ = 0;
            delegateTarget_ = newTarget;
            ASFW_LOG(BusReset, "Delegation target changed to node %u - retry counter reset", newTarget);
        }

        // Increment retry counter (Linux pattern)
        delegateRetryCount_++;

        // Check retry limit (Linux: max 5 attempts)
        if (delegateRetryCount_ > kMaxDelegateRetries) {
            delegateSuppressed_ = true;
            ASFW_LOG(BusReset,
                     "Root delegation to node %u failed after %u attempts - suppressing further attempts",
                     delegateTarget_, kMaxDelegateRetries);
            return;
        }

        delegateAttemptActive_ = true;
        ASFW_LOG(BusReset, "Root delegation attempt %u/%u to node %u",
                 delegateRetryCount_, kMaxDelegateRetries, delegateTarget_);
    }

    pendingPhyCommand_ = command;
    pendingPhyReason_ = reason ? reason : "unspecified";
    pendingManagedReset_ = true;

    const std::string rootStr = command.forceRootNodeID.has_value()
                                    ? std::to_string(*command.forceRootNodeID)
                                    : "none";
    const std::string gapStr = command.gapCount.has_value()
                                   ? std::to_string(*command.gapCount)
                                   : "none";
    ASFW_LOG(BusReset, "Staged PHY packet (reason=%{public}s root=%{public}s gap=%{public}s)",
             pendingPhyReason_.c_str(), rootStr.c_str(), gapStr.c_str());
}

bool BusResetCoordinator::DispatchPendingPhyPacket() {
    if (!pendingPhyCommand_.has_value() || !hardware_) {
        return false;
    }

    const auto cmd = *pendingPhyCommand_;
    const std::string rootStr = cmd.forceRootNodeID.has_value()
                                    ? std::to_string(*cmd.forceRootNodeID)
                                    : "none";
    const std::string gapStr = cmd.gapCount.has_value()
                                   ? std::to_string(*cmd.gapCount)
                                   : "none";

    ASFW_LOG(BusReset,
             "Dispatching delayed PHY packet (reason=%{public}s root=%{public}s gap=%{public}s)",
             pendingPhyReason_.c_str(), rootStr.c_str(), gapStr.c_str());

    if (cmd.setContender.has_value()) {
        ASFW_LOG(BusReset, "Applying Contender bit update: %d", *cmd.setContender);
        hardware_->SetContender(*cmd.setContender);
    }

    if (!hardware_->SendPhyConfig(cmd.gapCount, cmd.forceRootNodeID, "BusResetCoordinator::DispatchPendingPhyPacket")) {
        ASFW_LOG(BusReset, "⚠️  Failed to send staged PHY packet");
        if (std::strcmp(pendingPhyReason_.c_str(), "AssignCycleMaster") == 0) {
            delegateAttemptActive_ = false;
            delegateTarget_ = 0xFF;
            delegateRetryCount_ = 0;
            delegateSuppressed_ = false;
        }
        pendingManagedReset_ = false;
        pendingPhyCommand_.reset();
        pendingPhyReason_.clear();
        return false;
    }

    const bool resetOk = hardware_->InitiateBusReset(/*shortReset=*/false);
    if (!resetOk) {
        ASFW_LOG(BusReset, "⚠️  Failed to initiate bus reset after staged PHY packet");
        if (std::strcmp(pendingPhyReason_.c_str(), "AssignCycleMaster") == 0) {
            delegateAttemptActive_ = false;
            delegateTarget_ = 0xFF;
            delegateRetryCount_ = 0;
            delegateSuppressed_ = false;
        }
        pendingManagedReset_ = false;
        pendingPhyCommand_.reset();
        pendingPhyReason_.clear();
        return false;
    }

    pendingPhyCommand_.reset();
    pendingPhyReason_.clear();
    pendingManagedReset_ = false;
    return true;
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
    const bool nodeValid = G_NodeIDValid();
    const bool genMatch = (gen == lastGeneration_);
    const bool hasTopo = lastTopology_.has_value();
    const bool ready = nodeValid && filtersEnabled_ && atArmed_ && hasTopo && genMatch;

    if (!ready) {
        ASFW_LOG(BusReset,
                 "ReadyForDiscovery(gen=%u): NOT READY — nodeValid=%d filters=%d at=%d "
                 "topo=%d genMatch=%d(last=%u)",
                 gen, nodeValid, filtersEnabled_, atArmed_,
                 hasTopo, genMatch, lastGeneration_);
    }
    return ready;
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

void BusResetCoordinator::EvaluateRootDelegation(const TopologySnapshot& topo) {
    if (!delegateAttemptActive_) {
        if (delegateSuppressed_ && topo.rootNodeId.has_value() && topo.localNodeId.has_value()) {
            const uint8_t currentRoot = *topo.rootNodeId;
            const uint8_t local = *topo.localNodeId;
            if (currentRoot != local) {
                delegateSuppressed_ = false;
                ASFW_LOG(BusReset,
                         "Root delegation suppression cleared (local=%u currentRoot=%u)",
                         local, currentRoot);
            }
        }
        return;
    }

    if (!topo.rootNodeId.has_value()) {
        return;
    }

    const uint8_t currentRoot = *topo.rootNodeId;
    const uint8_t localNode = topo.localNodeId.value_or(0xFF);

    if ((delegateTarget_ != 0xFF && currentRoot == delegateTarget_) ||
        (localNode != 0xFF && currentRoot != localNode)) {
        ASFW_LOG(BusReset,
                 "Root delegation succeeded (root=%u target=%u local=%u)",
                 currentRoot, delegateTarget_, localNode);
        delegateAttemptActive_ = false;
        delegateSuppressed_ = false;
        delegateTarget_ = 0xFF;
        delegateRetryCount_ = 0;
        return;
    }

    // Delegation attempt failed - let retry counter decide if we should suppress
    // Don't immediately suppress on first failure (Linux pattern: allows retries)
    ASFW_LOG(BusReset,
             "Root delegation to node %u failed (current root=%u local=%u) - retry attempt %u/%u",
             delegateTarget_, currentRoot, localNode, delegateRetryCount_, kMaxDelegateRetries);
    delegateAttemptActive_ = false;
    // Note: Do NOT set delegateSuppressed_ here - let StageDelayedPhyPacket handle retry limit
    // Note: Do NOT reset delegateTarget_ or delegateRetryCount_ - preserve for retry logic
}

void BusResetCoordinator::ResetDelegationRetryCounter() {
    if (delegateRetryCount_ > 0 || delegateSuppressed_) {
        ASFW_LOG(BusReset,
                 "Resetting delegation retry counter (was %u, suppressed=%d) - topology change or gap=0 bypass",
                 delegateRetryCount_, delegateSuppressed_);
    }
    delegateRetryCount_ = 0;
    delegateSuppressed_ = false;
    // Note: Keep delegateTarget_ to detect target changes
}

void BusResetCoordinator::SetPreviousScanHadBusyNodes(bool busy) {
    if (busy) {
        // Escalate: increase delay each consecutive busy scan
        if (currentDiscoveryDelayMs_ < kMaxDiscoveryDelayMs) {
            currentDiscoveryDelayMs_ = std::min(
                currentDiscoveryDelayMs_ + kDiscoveryDelayStepMs,
                kMaxDiscoveryDelayMs);
        }
        if (!previousScanHadBusyNodes_) {
            ASFW_LOG(BusReset, "previousScanHadBusyNodes: false → true, delay=%ums",
                     currentDiscoveryDelayMs_);
        } else {
            ASFW_LOG(BusReset, "previousScanHadBusyNodes: still true, delay escalated to %ums",
                     currentDiscoveryDelayMs_);
        }
    } else {
        // Device recovered — reset delay
        if (previousScanHadBusyNodes_ || currentDiscoveryDelayMs_ > 0) {
            ASFW_LOG(BusReset, "previousScanHadBusyNodes: %d → false, delay reset (was %ums)",
                     previousScanHadBusyNodes_, currentDiscoveryDelayMs_);
        }
        currentDiscoveryDelayMs_ = 0;
    }
    previousScanHadBusyNodes_ = busy;
}

void BusResetCoordinator::EscalateDiscoveryDelay() {
    // Called when scan produced 0 ROMs — we learned nothing, increase delay
    if (previousScanHadBusyNodes_ && currentDiscoveryDelayMs_ < kMaxDiscoveryDelayMs) {
        const uint32_t prev = currentDiscoveryDelayMs_;
        currentDiscoveryDelayMs_ = std::min(
            currentDiscoveryDelayMs_ + kDiscoveryDelayStepMs,
            kMaxDiscoveryDelayMs);
        ASFW_LOG(BusReset, "Discovery delay escalated %ums → %ums (0 ROMs, device still booting)",
                 prev, currentDiscoveryDelayMs_);
    }
}

} // namespace ASFW::Driver
