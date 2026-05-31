// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CyclePolicyCoordinator.cpp — see CyclePolicyCoordinator.hpp

#include "CyclePolicyCoordinator.hpp"

namespace ASFW::Bus {

using namespace ASFW::FW;

void CyclePolicyCoordinator::Evaluate(const CyclePolicyInputs& inputs, ICyclePolicyExecutor& executor) noexcept {
    snapshot_.generation = inputs.generation;
    snapshot_.lastDecision = Plan(inputs);
    snapshot_.lastAction = CyclePolicyAction::None;
    snapshot_.targetNode = 0x3F;

    switch (snapshot_.lastDecision) {
    case CyclePolicyDecision::LocalRootEnableCycleMaster: {
        if (lastLocalCycleMasterGeneration_ == inputs.generation) {
            snapshot_.lastDecision = CyclePolicyDecision::AlreadySatisfiedLocalCycleMasterEnabled;
            return;
        }

        snapshot_.lastAction = CyclePolicyAction::EnableLocalCycleMaster;
        if (executor.EnableLocalCycleMasterMutation(inputs.generation)) {
            lastLocalCycleMasterGeneration_ = inputs.generation;
            snapshot_.localCycleMasterEnableCount++;
            snapshot_.localCycleMasterAfter = true;
        } else {
            snapshot_.lastDecision = CyclePolicyDecision::FailedHardwareUnavailable;
        }
        break;
    }

    case CyclePolicyDecision::RemoteRootSetCmstr: {
        // Throttling: one remote CMSTR per generation/target
        if (lastRemoteCmstrGeneration_ == inputs.generation && 
            lastRemoteCmstrTargetNode_ == inputs.rootNodeId) {
            return;
        }

        if (remoteCmstrHandle_.IsValid()) {
            // Already in flight for this or previous generation
            return;
        }

        snapshot_.lastAction = CyclePolicyAction::WriteRemoteStateSetCmstr;
        snapshot_.targetNode = inputs.rootNodeId;
        
        auto handle = executor.WriteRemoteStateSetCmstr(inputs.generation, inputs.busBase16, inputs.rootNodeId);
        if (handle.IsValid()) {
            remoteCmstrHandle_ = handle;
            lastRemoteCmstrGeneration_ = inputs.generation;
            lastRemoteCmstrTargetNode_ = inputs.rootNodeId;
            snapshot_.remoteCmstrSubmitCount++;
            snapshot_.remoteCmstrInFlight = true;
        } else {
            snapshot_.lastDecision = CyclePolicyDecision::FailedAsyncSubmit;
        }
        break;
    }

    case CyclePolicyDecision::RootSelectionRequired:
        snapshot_.lastAction = CyclePolicyAction::ReportRootSelectionRequired;
        break;

    default:
        // No execution action for other decisions
        if (snapshot_.lastDecision != CyclePolicyDecision::None &&
            snapshot_.lastDecision < CyclePolicyDecision::AlreadySatisfiedCycleStartObserved) {
            snapshot_.suppressedCount++;
        }
        break;
    }
}

CyclePolicyDecision CyclePolicyCoordinator::Plan(const CyclePolicyInputs& inputs) const noexcept {
    if (!inputs.topologyValid) {
        return CyclePolicyDecision::SuppressedByTopology;
    }

    if (inputs.cycleStartObserved) {
        return CyclePolicyDecision::AlreadySatisfiedCycleStartObserved;
    }

    // Two paths to cycle repair: 
    // A. We are the elected Bus Manager.
    // B. We are the IRM and the fallback gate is open without a detected BM.
    const bool isBM = inputs.localIsBM;
    const bool isFallbackIRM = inputs.localIsIRM && inputs.irmFallbackGateOpen && inputs.irmFallbackNoBMDetected;

    if (!isBM && !isFallbackIRM) {
        return CyclePolicyDecision::SuppressedNotBMOrFallbackIRM;
    }

    // Role check: IRM fallback requires IRMResourceHost or FullBusManager.
    // BM path requires FullBusManager.
    if (inputs.roleMode == RoleMode::ClientOnly) {
        return CyclePolicyDecision::SuppressedByRoleMode;
    }

    // Activity level check: Cycle mutation requires CyclePolicyAllowed or higher.
    if (inputs.activityLevel < FullBMActivityLevel::CyclePolicyAllowed) {
        return CyclePolicyDecision::SuppressedByActivityLevel;
    }

    // If local is root, always use local enable path (safest and most spec-compliant).
    if (inputs.localIsRoot) {
        return CyclePolicyDecision::LocalRootEnableCycleMaster;
    }

    // Remote Root Path:
    // This requires root CMC evidence.
    if (!inputs.rootCmcKnown) {
        return CyclePolicyDecision::DeferRootCmcUnknown;
    }

    if (!inputs.rootCmcCapable) {
        // Root cannot cycle; active root selection required (Milestone 6).
        return CyclePolicyDecision::RootSelectionRequired;
    }

    // If we are BM, we can send remote CMSTR if activity level allows.
    if (isBM) {
        if (inputs.activityLevel >= FullBMActivityLevel::RemoteCmstrAllowed) {
            return CyclePolicyDecision::RemoteRootSetCmstr;
        } else {
            return CyclePolicyDecision::SuppressedByActivityLevel;
        }
    }

    // IRM fallback path (no BM exists):
    // For Milestone 5, we only allow local root enable in IRM fallback.
    // Remote CMSTR is reserved for Full Bus Manager.
    return CyclePolicyDecision::RootSelectionRequired;
}

void CyclePolicyCoordinator::OnBusResetStarted(uint32_t generation) noexcept {
    // Preserve cumulative counters
    const uint32_t localCount = snapshot_.localCycleMasterEnableCount;
    const uint32_t remoteCount = snapshot_.remoteCmstrSubmitCount;
    const uint32_t suppressedCount = snapshot_.suppressedCount;
    const uint32_t staleCount = snapshot_.staleGenerationDrops;

    snapshot_ = {};
    snapshot_.generation = generation;
    snapshot_.localCycleMasterEnableCount = localCount;
    snapshot_.remoteCmstrSubmitCount = remoteCount;
    snapshot_.suppressedCount = suppressedCount;
    snapshot_.staleGenerationDrops = staleCount;
}

void CyclePolicyCoordinator::OnRemoteCmstrComplete(uint32_t generation, uint8_t targetNode,
                                                   Async::AsyncStatus status) noexcept {
    if (!remoteCmstrHandle_.IsValid()) {
        return;
    }

    if (snapshot_.generation != generation) {
        snapshot_.staleGenerationDrops++;
        remoteCmstrHandle_.Invalidate();
        return;
    }

    snapshot_.remoteCmstrInFlight = false;
    snapshot_.remoteCmstrStatus = static_cast<uint8_t>(status);
    snapshot_.remoteCmstrGeneration = generation;
    snapshot_.remoteCmstrTargetNode = targetNode;
    
    remoteCmstrHandle_.Invalidate();
}

} // namespace ASFW::Bus
