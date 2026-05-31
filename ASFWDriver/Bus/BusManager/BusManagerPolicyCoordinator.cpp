// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerPolicyCoordinator.cpp — see BusManagerPolicyCoordinator.hpp

#include "BusManagerPolicyCoordinator.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Hardware/RegisterMap.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Common/CSRSpace.hpp"

namespace ASFW::Bus {

BusManagerPolicyCoordinator::BusManagerPolicyCoordinator(Deps deps) noexcept
    : deps_(deps) {}

void BusManagerPolicyCoordinator::Evaluate(BusManagerRuntimeState& state) noexcept {
    if (state.generation != generation_) {
        generation_ = state.generation;
    }

    if (!state.localIsBM) {
        // We are not the Bus Manager. Do not run any active policy decisions.
        state.bmPolicyVerdict = static_cast<uint8_t>(BMPolicyVerdict::ObserveOnly);
        return;
    }

    if (state.localIsRoot) {
        // If local node is BM and also root: local cycleMaster should be set.
        state.bmPolicyVerdict = static_cast<uint8_t>(BMPolicyVerdict::LocalRootCycleMaster);
        
        // Diagnostics only: report suppression if below CyclePolicyAllowed.
        if (state.fullBMActivityLevel < static_cast<uint8_t>(ASFW::FW::FullBMActivityLevel::CyclePolicyAllowed)) {
            ASFW_LOG(Controller, "[BM Policy] Local node is BM and root; but cycleMaster activation is suppressed (level=%u)", state.fullBMActivityLevel);
        }
    } else {
        // We are BM but NOT root.
        if (!state.rootCmcKnown) {
            state.bmPolicyVerdict = static_cast<uint8_t>(BMPolicyVerdict::ObserveOnly);
            state.remoteCmstrNeeded = false;
            state.remoteCmstrAlreadySatisfied = false;
            ASFW_LOG(Controller, "[BM Policy] We are BM, remote root (id=%u). Root CMC status unknown. ObserveOnly.", state.rootNodeId);
        } else if (!state.rootCmcCapable) {
            state.bmPolicyVerdict = static_cast<uint8_t>(BMPolicyVerdict::ObserveOnly);
            state.remoteCmstrNeeded = false;
            state.remoteCmstrAlreadySatisfied = false;
            ASFW_LOG(Controller, "[BM Policy] We are BM, remote root (id=%u). Root CMC is false (unsuitable). ObserveOnly.", state.rootNodeId);
        } else {
            // Root is CMC capable
            if (state.cycleStartObserved) {
                state.bmPolicyVerdict = static_cast<uint8_t>(BMPolicyVerdict::RemoteRootAlreadyCycling);
                state.remoteCmstrAlreadySatisfied = true;
                state.remoteCmstrNeeded = false;
                ASFW_LOG(Controller, "[BM Policy] We are BM, remote root (id=%u). Root already cycling. Satisfied.", state.rootNodeId);
            } else {
                state.bmPolicyVerdict = static_cast<uint8_t>(BMPolicyVerdict::RemoteCMSTRNeeded);
                state.remoteCmstrAlreadySatisfied = false;
                state.remoteCmstrNeeded = true;
                ASFW_LOG(Controller, "[BM Policy] We are BM, remote root (id=%u). Root CMC capable but not cycling. Remote CMSTR needed.", state.rootNodeId);
            }
        }
    }
}

} // namespace ASFW::Bus
