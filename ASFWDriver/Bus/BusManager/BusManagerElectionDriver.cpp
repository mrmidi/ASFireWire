// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerElectionDriver.cpp — see BusManagerElectionDriver.hpp

#include "BusManagerElectionDriver.hpp"
#include "../IRM/LocalIRMResourceController.hpp"
#include "../IRM/IRMCSRConstants.hpp"
#include "../Timing/PostResetTimingCoordinator.hpp"
#include "../Timing/PostResetTiming.hpp"
#include "../BusResetCoordinator.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Controller/ControllerTypes.hpp"
#include "../../Hardware/HardwareInterface.hpp"

namespace ASFW::Bus {

BusManagerElectionDriver::BusManagerElectionDriver(Deps deps, ASFW::Driver::RolePolicy rolePolicy) noexcept
    : deps_(deps), rolePolicy_(rolePolicy) {}

bool BusManagerElectionDriver::ElectionStillAllowed() const noexcept {
    return active_ &&
           rolePolicy_.roleMode == ASFW::FW::RoleMode::FullBusManager &&
           rolePolicy_.fullBMActivityLevel >= ASFW::FW::FullBMActivityLevel::ElectionOnly;
}

bool BusManagerElectionDriver::ShouldYieldForStableRemoteIRM(const ASFW::Driver::TopologySnapshot& snap) noexcept {
    if (stormYieldPending_) {
        stormYieldPending_ = false;
        // IEEE 1394-2008 H.4 / 8.5.4 leave "most suitable" BM choice and
        // abdication heuristics implementation-defined. If a remote root/IRM
        // immediately resets after ASFW wins BM, treat that as bus-specific
        // evidence to yield instead of fighting the incumbent device.
        stormYieldActive_ =
            snap.localNodeId != Driver::kInvalidPhysicalId &&
            snap.rootNodeId != Driver::kInvalidPhysicalId &&
            snap.irmNodeId != Driver::kInvalidPhysicalId &&
            snap.localNodeId != snap.rootNodeId &&
            snap.rootNodeId == snap.irmNodeId;
        if (stormYieldActive_) {
            stormYieldKey_ = YieldTopologyKey{
                .localNodeId = snap.localNodeId,
                .rootNodeId = snap.rootNodeId,
                .irmNodeId = snap.irmNodeId,
                .nodeCount = snap.nodeCount
            };
            ASFW_LOG(Controller,
                     "[BM Election] Fast reset after local BM win; yielding BM contention while "
                     "remote root/IRM topology stays stable (local=%u root=%u irm=%u nodes=%u)",
                     static_cast<unsigned>(stormYieldKey_.localNodeId),
                     static_cast<unsigned>(stormYieldKey_.rootNodeId),
                     static_cast<unsigned>(stormYieldKey_.irmNodeId),
                     static_cast<unsigned>(stormYieldKey_.nodeCount));
        }
    }

    if (!stormYieldActive_) {
        return false;
    }

    // IEEE 1394-2008 Q.8 explicitly permits a cable bus with an IRM and no BM.
    // Maintaining that state is preferable to repeatedly destabilizing a bus
    // that just rejected our BM ownership.
    const bool sameTopology =
        snap.localNodeId == stormYieldKey_.localNodeId &&
        snap.rootNodeId == stormYieldKey_.rootNodeId &&
        snap.irmNodeId == stormYieldKey_.irmNodeId &&
        snap.nodeCount == stormYieldKey_.nodeCount;
    if (!sameTopology) {
        ASFW_LOG(Controller,
                 "[BM Election] Clearing fast-reset BM yield: topology changed "
                 "(local=%u root=%u irm=%u nodes=%u)",
                 static_cast<unsigned>(snap.localNodeId),
                 static_cast<unsigned>(snap.rootNodeId),
                 static_cast<unsigned>(snap.irmNodeId),
                 static_cast<unsigned>(snap.nodeCount));
        stormYieldActive_ = false;
        return false;
    }

    // cross-validated with Linux: core-topology.c:483-485 Apple: IOFireWireController.cpp:3258-3263
    ASFW_LOG(Controller,
             "[BM Election] Yielding BM contention for gen=%u after fast reset storm evidence "
             "(stable remote root/IRM=%u, local=%u)",
             snap.generation, static_cast<unsigned>(snap.irmNodeId),
             static_cast<unsigned>(snap.localNodeId));
    lastAction_ = 3;
    lastElectionPath_ = 0;
    inFlight_ = false;
    return true;
}

void BusManagerElectionDriver::OnTopologyReady(const ASFW::Driver::TopologySnapshot& snap, uint64_t nowNs) noexcept {
    if (!active_) {
        return;
    }

    if (!ElectionStillAllowed()) {
        return;
    }

    if (snap.localNodeId == Driver::kInvalidPhysicalId || snap.irmNodeId == Driver::kInvalidPhysicalId) {
        ASFW_LOG(Controller, "[BM Election] Skipping election: missing local ID or IRM ID");
        return;
    }

    const uint8_t localNodeId = snap.localNodeId;
    const uint8_t irmNodeId = snap.irmNodeId;
    const uint32_t generation = snap.generation;

    localNodeId_ = localNodeId;
    irmNodeId_ = irmNodeId;

    if (ShouldYieldForStableRemoteIRM(snap)) {
        return;
    }

    // Milestone 3: Max one election attempt per generation
    if (attemptedGeneration_ == generation && attemptsThisGeneration_ >= 1) {
        return;
    }

    // Check if we are already contending for this or a newer generation
    if (inFlight_ && inFlightGen_ >= generation) {
        return;
    }

    bool abdicateObserved = false;
    if (deps_.csrResponder) {
        abdicateObserved = deps_.csrResponder->ConsumeAbdicate();
    }

    BmElectionInputs inputs{
        .mode = rolePolicy_.roleMode,
        .activityLevel = rolePolicy_.fullBMActivityLevel,
        .generation = generation,
        .localId = localNodeId,
        .irmId = irmNodeId,
        .wasIncumbent = wasIncumbent_,
        .abdicateObserved = abdicateObserved,
    };

    DecisionAction action = fsm_.Decide(inputs);
    if (action == DecisionAction::DoNotContend) {
        lastAction_ = 0;
        return;
    }

    lastAction_ = (action == DecisionAction::ContendImmediately) ? 1 : 2;

    // Map DecisionAction to Annex H candidate class
    using namespace Timing;
    BMCandidateClass candidateClass = (action == DecisionAction::ContendImmediately) ? BMCandidateClass::Incumbent : BMCandidateClass::NonIncumbent;

    if (!deps_.timing) {
        ASFW_LOG(Controller, "⚠️ [BM Election] Cannot check Annex H gate: timing coordinator missing");
        return;
    }

    const TimingGateResult gate = deps_.timing->CheckBMGate(generation, candidateClass, nowNs);
    if (gate.state == TimingGateState::ExpiredGeneration) {
        ASFW_LOG(Controller, "[BM Election] Aborting: timing gate expired for generation %u", generation);
        return;
    }

    if (gate.allowed) {
        ASFW_LOG(Controller, "[BM Election] Gate OPEN for gen=%u (class=%u); contending now", generation, static_cast<int>(candidateClass));
        inFlight_ = true;
        inFlightGen_ = generation;
        attemptedGeneration_ = generation;
        attemptsThisGeneration_++;
        Contend(generation, localNodeId, irmNodeId, snap.busBase16);
    } else if (gate.state == TimingGateState::Closed) {
        ASFW_LOG(Controller, "[BM Election] Gate CLOSED for gen=%u (class=%u); scheduling for +%llu ms", 
                 generation, static_cast<int>(candidateClass), gate.remainingNs / 1000000ULL);
        
        if (deps_.scheduler) {
            inFlight_ = true;
            inFlightGen_ = generation;
            std::weak_ptr<BusManagerElectionDriver> weakSelf = shared_from_this();
            deps_.scheduler->DispatchAsyncAfter(gate.remainingNs, [weakSelf, generation, localNodeId, irmNodeId, busBase16 = snap.busBase16, candidateClass]() {
                auto self = weakSelf.lock();
                if (!self || !self->active_) return;
                
                // Final check before contending: Role/Activity policy
                if (!self->ElectionStillAllowed()) {
                    ASFW_LOG(Controller, "[BM Election] Deferred contention suppressed: role/activity changed");
                    self->inFlight_ = false;
                    if (self->deps_.timing) self->deps_.timing->RecordRoleSuppression();
                    return;
                }

                // Final check before contending: Generation
                if (self->deps_.asyncController) {
                    const auto state = self->deps_.asyncController->GetBusStateSnapshot();
                    if (state.generation16 != generation) {
                        ASFW_LOG(Controller, "[BM Election] Aborting deferred contention: generation changed from %u to %u",
                                 generation, state.generation16);
                        self->inFlight_ = false;
                        if (self->deps_.timing) self->deps_.timing->RecordStaleTimerFiring();
                        return;
                    }
                }
                
                // Final check before contending: Re-validate timing gate
                if (self->deps_.timing && self->deps_.monotonicNowNs) {
                    const uint64_t now = self->deps_.monotonicNowNs();
                    const auto finalGate = self->deps_.timing->CheckBMGate(generation, candidateClass, now);
                    if (!finalGate.allowed) {
                        ASFW_LOG(Controller, "[BM Election] Deferred contention aborted: gate still closed (state=%d)", static_cast<int>(finalGate.state));
                        self->inFlight_ = false;
                        return;
                    }
                }

                self->attemptedGeneration_ = generation;
                self->attemptsThisGeneration_++;
                self->Contend(generation, localNodeId, irmNodeId, busBase16);
            });
        }
    }
}

void BusManagerElectionDriver::OnBusReset() noexcept {
    const bool localWasBM = fsm_.Owner() == BmOwner::Local;
    if (localWasBM && deps_.monotonicNowNs && lastLocalBMWinNs_ != 0) {
        const uint64_t nowNs = deps_.monotonicNowNs();
        if (nowNs >= lastLocalBMWinNs_ && nowNs - lastLocalBMWinNs_ <= kFastResetAfterBMWinNs) {
            // IEEE 1394-2008 H.4 models BM abdication as a valid path when a
            // better-suited manager is detected; the standard deliberately does
            // not define that suitability heuristic.
            stormYieldPending_ = true;
            ASFW_LOG(Controller,
                     "[BM Election] Reset arrived %llu ms after local BM win; next stable "
                     "remote-root topology may suppress BM re-contention",
                     (nowNs - lastLocalBMWinNs_) / 1000000ULL);
        }
    }

    if (localWasBM && !stormYieldPending_) {
        wasIncumbent_ = true;
    } else {
        wasIncumbent_ = false;
    }
    fsm_.Reset();
    inFlight_ = false;
    inFlightGen_ = 0;
    attemptsThisGeneration_ = 0;
    lastElectionPath_ = 0;
    lastAction_ = 0;
    if (inFlightHandle_) {
        if (deps_.asyncController) {
            deps_.asyncController->Cancel(inFlightHandle_);
        }
        inFlightHandle_ = {};
    }
}

void BusManagerElectionDriver::Stop() noexcept {
    active_ = false;
    inFlight_ = false;
    if (inFlightHandle_) {
        if (deps_.asyncController) {
            deps_.asyncController->Cancel(inFlightHandle_);
        }
        inFlightHandle_ = {};
    }
}

void BusManagerElectionDriver::Contend(uint32_t generation, uint8_t localNodeId, uint8_t irmNodeId, uint16_t busBase16) noexcept {
    if (!ElectionStillAllowed() || !deps_.asyncController) {
        inFlight_ = false;
        return;
    }

    // Verify generation is still current
    const auto state = deps_.asyncController->GetBusStateSnapshot();
    if (state.generation16 != generation) {
        ASFW_LOG(Controller, "[BM Election] Contention aborted: generation is stale (expected %u, current %u)",
                 generation, state.generation16);
        inFlight_ = false;
        return;
    }
    // Local Loopback Compare-Swap Branching (FW-14)
    if (irmNodeId == localNodeId) {
        ASFW_LOG(Controller, "[BM Election] Local node is IRM; routing CompareSwap through local CSRControl loopback");
        lastElectionPath_ = 1; // Local

        ASFW::Driver::LocalCSRLockResult result;
        if (deps_.localIrmController) {
            result = deps_.localIrmController->CompareSwapBusManagerId(Driver::IRMCSR::kNoBusManagerId, localNodeId);
        } else {
            if (deps_.hardware == nullptr) {
                ASFW_LOG(Controller, "[BM Election] Cannot perform local CompareSwap: hardware interface is null");
                inFlight_ = false;
                if (observer_) {
                    observer_->OnBMElectionFailed(generation, ASFW::Async::AsyncStatus::kHardwareError);
                }
                return;
            }
            result = deps_.hardware->CompareSwapLocalIRMResource(
                static_cast<uint32_t>(Driver::IRMCSR::CSRSelector::BusManagerId),
                Driver::IRMCSR::kNoBusManagerId, 
                localNodeId);
        }
        
        if (result.status != ASFW::Driver::LocalCSRLockResult::Status::Success) {
            ASFW_LOG(Controller, "[BM Election] Local CompareSwap failed (status=%d)",
                     static_cast<int>(result.status));
            inFlight_ = false;
            if (observer_) {
                observer_->OnBMElectionFailed(generation, ASFW::Async::AsyncStatus::kHardwareError);
            }
            return;
        }

        // Invoke HandleCompareSwapResult synchronously since it's a local hardware lock sequence
        HandleCompareSwapResult(generation, localNodeId, ASFW::Async::AsyncStatus::kSuccess, result.oldValue, result.compareMatched);
        return;
    }

    lastElectionPath_ = 2; // Remote

    ASFW::Async::CompareSwapParams params{
        .destinationID = ASFW::Driver::ComposeNodeID(busBase16, irmNodeId),
        .addressHigh = ASFW::FW::kCSRRegSpaceHi,
        .addressLow = ASFW::FW::kCSR_BusManagerID,
        .compareValue = Driver::IRMCSR::kNoBusManagerId,
        .swapValue = localNodeId,
        .speedCode = static_cast<uint8_t>(ASFW::FW::Speed::S100)
    };

    ASFW_LOG(Controller, "[BM Election] Issuing CompareSwap to IRM node=0x%04X (phys=%u) for gen=%u",
             params.destinationID, irmNodeId, generation);

    std::weak_ptr<BusManagerElectionDriver> weakSelf = shared_from_this();
    inFlightHandle_ = deps_.asyncController->CompareSwap(params, [weakSelf, generation, localNodeId](ASFW::Async::AsyncStatus status, uint32_t oldValue, bool compareMatched) {
        auto self = weakSelf.lock();
        if (!self) {
            return;
        }
        self->inFlightHandle_ = {}; // clear in-flight handle
        self->HandleCompareSwapResult(generation, localNodeId, status, oldValue, compareMatched);
    });
}

void BusManagerElectionDriver::HandleCompareSwapResult(uint32_t generation, uint8_t localNodeId, ASFW::Async::AsyncStatus status, uint32_t oldValue, bool compareMatched) noexcept {
    if (!active_) {
        return;
    }

    inFlight_ = false;

    // Verify generation is still current
    if (deps_.asyncController) {
        const auto state = deps_.asyncController->GetBusStateSnapshot();
        if (state.generation16 != generation) {
            ASFW_LOG(Controller, "[BM Election] CompareSwap callback ignored: generation is stale (callback gen=%u, current gen=%u)",
                     generation, state.generation16);
            fsm_.IncrementStaleAbortCount();
            return;
        }
    }

    if (status != ASFW::Async::AsyncStatus::kSuccess) {
        ASFW_LOG(Controller, "[BM Election] CompareSwap failed with status %d (%s)",
                 static_cast<int>(status), ASFW::Async::ToString(status));
        if (observer_) {
            observer_->OnBMElectionFailed(generation, status);
        }
        return;
    }

    ElectionOutcome outcome = fsm_.InterpretOldValue(oldValue, localNodeId);
    switch (outcome) {
    case ElectionOutcome::WonBM:
        ASFW_LOG(Controller, "[BM Election] WON Bus Manager election! (oldValue=0x%X, compareMatched=%d)", oldValue, compareMatched);
        if (deps_.monotonicNowNs) {
            lastLocalBMWinNs_ = deps_.monotonicNowNs();
        }
        if (observer_) {
            observer_->OnLocalWonBM(generation, localNodeId);
        }
        break;
    case ElectionOutcome::IncumbentReestablished:
        ASFW_LOG(Controller, "[BM Election] Re-established BM incumbency.");
        if (deps_.monotonicNowNs) {
            lastLocalBMWinNs_ = deps_.monotonicNowNs();
        }
        if (observer_) {
            observer_->OnLocalWonBM(generation, localNodeId);
        }
        break;
    case ElectionOutcome::RemoteBM:
        ASFW_LOG(Controller, "[BM Election] Remote node 0x%02X won Bus Manager election (oldValue=0x%X).",
                 fsm_.OwnerId().value_or(0xFF), oldValue);
        if (observer_) {
            observer_->OnRemoteBM(generation, fsm_.OwnerId().value_or(0xFF));
        }
        break;
    default:
        if (observer_) {
            observer_->OnBMElectionFailed(generation, ASFW::Async::AsyncStatus::kLockCompareFail);
        }
        break;
    }
}

} // namespace ASFW::Bus
