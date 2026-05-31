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
        // If local node is BM and also root: set local cycleMaster in LinkControlSet
        state.bmPolicyVerdict = static_cast<uint8_t>(BMPolicyVerdict::LocalRootCycleMaster);
        
        // Milestone 3: ElectionOnly must not enable local cycleMaster
        // For now, we use RemoteCmstrAllowed as the gate for ANY active BM behavior
        // beyond election itself (maximum conservatism). Later milestones may
        // introduce intermediate levels like LocalCyclePolicyAllowed.
        if (state.fullBMActivityLevel >= static_cast<uint8_t>(ASFW::FW::FullBMActivityLevel::RemoteCmstrAllowed)) {
            if (deps_.hardware) {
                const uint32_t linkCtrl = deps_.hardware->ReadLinkControl();
                if ((linkCtrl & ASFW::Driver::LinkControlBits::kCycleMaster) == 0) {
                    ASFW_LOG(Controller, "[BM Policy] Local node is BM and root; enabling local cycleMaster (level=%u)", state.fullBMActivityLevel);
                    deps_.hardware->SetLinkControlBits(ASFW::Driver::LinkControlBits::kCycleMaster);
                }
            }
        } else {
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

                // Pass 3: remote CMSTR submission gate
                if (state.fullBMActivityLevel >= static_cast<uint8_t>(ASFW::FW::FullBMActivityLevel::RemoteCmstrAllowed)) {
                    if (!state.remoteCmstrAllowed) {
                        state.remoteCmstrAllowed = true;
                        state.lastRemoteCmstrGeneration = state.generation;
                        state.lastRemoteCmstrTargetNode = state.rootNodeId;
                        ASFW_LOG(Controller, "[BM Policy] Submitting remote CMSTR write to root=%u for gen=%u",
                                 state.rootNodeId, state.generation);
                        if (deps_.executor) {
                            deps_.executor->SendRemoteCmstr(state.rootNodeId, state.generation);
                        }
                    }
                } else {
                    ASFW_LOG(Controller, "[BM Policy] Observe-only: Remote CMSTR not sent (activity level = %u)", state.fullBMActivityLevel);
                }
            }
        }
    }
}

} // namespace ASFW::Bus
