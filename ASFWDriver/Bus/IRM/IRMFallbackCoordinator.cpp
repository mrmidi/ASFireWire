// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// IRMFallbackCoordinator.cpp — see IRMFallbackCoordinator.hpp

#include "IRMFallbackCoordinator.hpp"
#include "../Timing/PostResetTimingCoordinator.hpp"
#include "../../Logging/Logging.hpp"
#include "../BusResetCoordinator.hpp"
#include "IRMCSRConstants.hpp"

namespace ASFW::Bus {

IRMFallbackCoordinator::IRMFallbackCoordinator(Deps deps) noexcept
    : deps_(deps), csr_(*deps.hardware) {}

void IRMFallbackCoordinator::OnBusResetStarted(uint32_t generation) noexcept {
    snapshot_ = {};
    snapshot_.generation = generation;
    snapshot_.state = IRMFallbackState::WaitingForTopology;
}

void IRMFallbackCoordinator::OnTopologyReady(const Driver::TopologySnapshot& topology,
                                             const Driver::RolePolicy& rolePolicy,
                                             const BusManagerRuntimeState& bmState,
                                             uint64_t nowNs) noexcept {
    snapshot_.generation = topology.generation;
    snapshot_.localNodeId = topology.localNodeId;
    snapshot_.irmNodeId = topology.irmNodeId;
    snapshot_.rootNodeId = topology.rootNodeId;
    snapshot_.localIsRoot = (topology.localNodeId == topology.rootNodeId);
    snapshot_.topologyValid = (topology.graphStatus == Driver::TopologyGraphStatus::Valid);

    snapshot_.roleAllowsIRMHost = RoleAllowsFallbackCheck(rolePolicy);
    snapshot_.localIsIRM = (topology.localNodeId == topology.irmNodeId);

    snapshot_.cycleStartObserved = bmState.cycleStartObserved;
    snapshot_.cycleStartSourceNode = bmState.cycleStartSourceNode;
    snapshot_.rootCmcKnown = bmState.rootCmcKnown;
    snapshot_.rootCmcCapable = bmState.rootCmcCapable;

    if (!snapshot_.roleAllowsIRMHost) {
        snapshot_.state = IRMFallbackState::Disabled;
        return;
    }

    if (!snapshot_.topologyValid) {
        snapshot_.state = IRMFallbackState::SuppressedByTopology;
        return;
    }

    if (!snapshot_.localIsIRM) {
        snapshot_.state = IRMFallbackState::NotLocalIRM;
        return;
    }

    snapshot_.state = IRMFallbackState::WaitingForAnnexHGate;
    MaybeEvaluate(nowNs);
}

void IRMFallbackCoordinator::MaybeEvaluate(uint64_t nowNs) noexcept {
    if (snapshot_.state != IRMFallbackState::WaitingForAnnexHGate) {
        return;
    }

    if (!deps_.timing) {
        ASFW_LOG(Controller, "⚠️ [IRM Fallback] Timing coordinator missing; evaluation suppressed");
        return;
    }

    const uint32_t generation = snapshot_.generation;
    const auto gate = deps_.timing->CheckGate(generation, Timing::TimingGate::IRMFallbackCheck, nowNs);

    snapshot_.annexHGateOpen = gate.allowed;
    snapshot_.allowedAtNs = gate.allowedAtNs;
    snapshot_.remainingNs = gate.remainingNs;
    snapshot_.checkedAtNs = nowNs;

    if (gate.state == Timing::TimingGateState::ExpiredGeneration) {
        snapshot_.state = IRMFallbackState::StaleGeneration;
        snapshot_.staleGenerationDrops++;
        if (deps_.timing) {
            deps_.timing->RecordGenerationSuppression();
        }
        return;
    }

    if (!gate.allowed) {
        // Gate still closed. Schedule deferred check if we have a scheduler.
        if (deps_.scheduler) {
            std::weak_ptr<IRMFallbackCoordinator> weakThis = shared_from_this();
            deps_.scheduler->DispatchAsyncAfter(gate.remainingNs, [weakThis, generation]() {
                auto self = weakThis.lock();
                if (!self) return;
                
                // Ensure we are still in the same generation
                if (self->snapshot_.generation != generation) {
                    self->snapshot_.staleGenerationDrops++;
                    if (self->deps_.timing) {
                        self->deps_.timing->RecordStaleTimerFiring();
                    }
                    return;
                }
                
                self->MaybeEvaluate(Driver::BusResetCoordinator::MonotonicNow());
            });
        }
        return;
    }

    // Gate is OPEN! Proceed to probe BUS_MANAGER_ID.
    snapshot_.state = IRMFallbackState::ProbingBusManagerId;
    
    uint32_t bmidValue = 0;
    snapshot_.probeStatus = ProbeBusManagerId(&bmidValue);
    snapshot_.busManagerIdRaw = bmidValue;

    if (snapshot_.probeStatus != BMIDProbeStatus::Success) {
        snapshot_.state = IRMFallbackState::ProbeFailed;
        snapshot_.probeFailures++;
        return;
    }

    // Interpret probe results
    if (bmidValue == Driver::IRMCSR::kNoBusManagerId) {
        snapshot_.noBusManagerDetected = true;
        snapshot_.busManagerExists = false;
        snapshot_.bmNodeId = 0x3F;
        snapshot_.state = IRMFallbackState::NoBMDetected;
        snapshot_.plannedAction = PlanFallbackAction();
    } else {
        snapshot_.noBusManagerDetected = false;
        snapshot_.busManagerExists = true;
        snapshot_.bmNodeId = static_cast<uint8_t>(bmidValue & 0x3FU);
        snapshot_.state = IRMFallbackState::BMExists;
        snapshot_.plannedAction = IRMFallbackAction::BMAlreadyExists;
    }
}

void IRMFallbackCoordinator::Disable() noexcept {
    snapshot_.state = IRMFallbackState::Disabled;
}

bool IRMFallbackCoordinator::RoleAllowsFallbackCheck(const Driver::RolePolicy& policy) const noexcept {
    return policy.roleMode == FW::RoleMode::IRMResourceHost ||
           policy.roleMode == FW::RoleMode::FullBusManager;
}

BMIDProbeStatus IRMFallbackCoordinator::ProbeBusManagerId(uint32_t* outValue) noexcept {
    // Milestone 4: Use local CSRControl path as we only run this when localIsIRM.
    auto result = csr_.ReadBusManagerId();

    if (outValue) {
        *outValue = result.value;
    }

    if (result.status == Driver::LocalCSRLockResult::Status::Timeout) {
        return BMIDProbeStatus::Timeout;
    }
    if (result.status != Driver::LocalCSRLockResult::Status::Success) {
        return BMIDProbeStatus::HardwareUnavailable;
    }

    // Validate upper bits (BUS_MANAGER_ID is 6 bits)
    if ((result.value & ~0x3Fu) != 0) {
        ASFW_LOG(Controller, "[IRM Fallback] ERROR: Invalid BUS_MANAGER_ID old value: 0x%08X (upper bits set)", result.value);
        return BMIDProbeStatus::InvalidUpperBits;
    }

    return BMIDProbeStatus::Success;
}

IRMFallbackAction IRMFallbackCoordinator::PlanFallbackAction() const noexcept {
    if (snapshot_.cycleStartObserved) {
        return IRMFallbackAction::CycleStartAlreadyObserved;
    }

    if (snapshot_.localIsRoot) {
        return IRMFallbackAction::LocalRootEnableCycleMasterRequired;
    }

    if (snapshot_.rootCmcKnown && snapshot_.rootCmcCapable) {
        return IRMFallbackAction::RemoteRootCmstrRequired;
    }

    return IRMFallbackAction::RootSelectionRequired;
}

} // namespace ASFW::Bus
