// SPDX-License-Identifier: Apache-2.0
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
    snapshot_.localCycleMasterBefore = inputs.localCycleMasterEnabled;
    snapshot_.localCycleMasterAfter = inputs.localCycleMasterEnabled;

    switch (snapshot_.lastDecision) {
    case CyclePolicyDecision::LocalCycleMasterClearNotRoot: {
        snapshot_.lastAction = CyclePolicyAction::ClearLocalCycleMaster;
        if (executor.ClearLocalCycleMasterMutation(inputs.generation)) {
            snapshot_.localCycleMasterAfter = false;
            snapshot_.localCycleMasterClearCount++;
        } else {
            snapshot_.lastDecision = CyclePolicyDecision::FailedHardwareUnavailable;
        }
        break;
    }

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

    // OHCI cycleMaster is meaningful only for the root node. Clear stale local
    // state before authority checks so a host that won root in one generation
    // does not keep emitting/advertising cycle-master state after another node
    // becomes root. cross-validated with Linux: ohci.c:2760-2765,2805-2819 Apple: IOFireWireController.cpp:3366-3367
    if (!inputs.localIsRoot && inputs.localCycleMasterEnabled) {
        return CyclePolicyDecision::LocalCycleMasterClearNotRoot;
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

    if (!isBM && inputs.cycleStartObserved) {
        return CyclePolicyDecision::AlreadySatisfiedCycleStartObserved;
    }

    // If local is root, always use local enable path. The active decision is
    // based on Self-ID link state, not BIB CMC.
    if (inputs.localIsRoot) {
        if (!inputs.localSelfIdKnown) {
            return CyclePolicyDecision::DeferLocalSelfIDUnknown;
        }
        if (!inputs.localSelfIdLinkActive) {
            return CyclePolicyDecision::RootSelectionRequired;
        }
        if (inputs.localCycleMasterEnabled) {
            return CyclePolicyDecision::AlreadySatisfiedLocalCycleMasterEnabled;
        }
        return CyclePolicyDecision::LocalRootEnableCycleMaster;
    }

    // Remote root path: root suitability is based on Self-ID contender+link bits.
    // The remote STATE_SET.cmstr write is separately gated by BIB CMC below.
    // cross-validated with Linux: core-card.c:448-473 Apple: IOFireWireController.cpp:2364-2404
    if (!inputs.rootSelfIdKnown) {
        return CyclePolicyDecision::DeferRootSelfIDUnknown;
    }

    if (!inputs.rootSelfIdLinkActive || !inputs.rootSelfIdContender) {
        return CyclePolicyDecision::RootSelectionRequired;
    }

    if (!inputs.rootCmcKnown) {
        return CyclePolicyDecision::DeferRootBibCmcUnknown;
    }

    if (!inputs.rootCmcCapable) {
        if (inputs.cycleStartObserved) {
            return CyclePolicyDecision::AlreadySatisfiedCycleStartObserved;
        }
        return CyclePolicyDecision::RootSelectionRequired;
    }

    // Elected BM duty: make sure a BIB-CMC-qualified root generates cycle starts.
    // Linux writes remote STATE_SET.cmstr only when root_device_is_cmc; Apple
    // instead forces/keeps itself root before enabling the local cycle master.
    // cross-validated with Linux: core-card.c:520-528 Apple: IOFireWireController.cpp:3366-3367
    if (isBM) {
        return CyclePolicyDecision::RemoteRootSetCmstr;
    }

    // IRM fallback path (no BM exists):
    // For Milestone 5, we only allow local root enable in IRM fallback.
    // Remote CMSTR is reserved for Full Bus Manager.
    return CyclePolicyDecision::RootSelectionRequired;
}

void CyclePolicyCoordinator::OnBusResetStarted(uint32_t generation) noexcept {
    // Preserve cumulative counters
    const uint32_t localCount = snapshot_.localCycleMasterEnableCount;
    const uint32_t localClearCount = snapshot_.localCycleMasterClearCount;
    const uint32_t remoteCount = snapshot_.remoteCmstrSubmitCount;
    const uint32_t suppressedCount = snapshot_.suppressedCount;
    uint32_t staleCount = snapshot_.staleGenerationDrops;

    if (remoteCmstrHandle_.IsValid()) {
        staleCount++;
    }

    snapshot_ = {};
    snapshot_.generation = generation;
    snapshot_.localCycleMasterEnableCount = localCount;
    snapshot_.localCycleMasterClearCount = localClearCount;
    snapshot_.remoteCmstrSubmitCount = remoteCount;
    snapshot_.suppressedCount = suppressedCount;
    snapshot_.staleGenerationDrops = staleCount;

    remoteCmstrHandle_.Invalidate();
    lastRemoteCmstrGeneration_ = 0;
    lastRemoteCmstrTargetNode_ = 0x3F;
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
