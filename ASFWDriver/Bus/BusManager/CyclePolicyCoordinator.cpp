// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CyclePolicyCoordinator.cpp — see CyclePolicyCoordinator.hpp

#include "CyclePolicyCoordinator.hpp"

namespace ASFW::Bus {

using namespace ASFW::FW;

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
    snapshot_ = {};
    snapshot_.generation = generation;
}

} // namespace ASFW::Bus
