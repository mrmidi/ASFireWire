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

void BusManagerPolicyCoordinator::Evaluate(const BusManagerRuntimeState& state) noexcept {
    if (state.generation != generation_) {
        generation_ = state.generation;
    }

    if (!state.localIsBM) {
        // We are not the Bus Manager. Do not run any active policy decisions.
        return;
    }

    if (state.localIsRoot) {
        // If local node is BM and also root: set local cycleMaster in LinkControlSet
        if (deps_.hardware) {
            const uint32_t linkCtrl = deps_.hardware->ReadLinkControl();
            if ((linkCtrl & ASFW::Driver::LinkControlBits::kCycleMaster) == 0) {
                ASFW_LOG(Controller, "[BM Policy] Local node is BM and root; enabling local cycleMaster");
                deps_.hardware->SetLinkControlBits(ASFW::Driver::LinkControlBits::kCycleMaster);
            }
        }
    } else {
        // We are BM but NOT root. Remote root should be cycle master if it is
        // CMC-capable. Phase 1 (observe-only): log what we would do, but do not
        // send remote STATE_SET.cmstr yet. This avoids:
        //   - async callback lifetime issues (raw this capture)
        //   - sending CMSTR without root CMC capability evidence
        // Wire active CMSTR sending in a future pass once root CMC / CycleStart
        // evidence is available from the RoleCoordinator pipeline.
        ASFW_LOG(Controller, "[BM Policy] Observe-only: we are BM, remote root (id=%u). "
                 "Remote CMSTR not sent (awaiting root CMC evidence pipeline)", state.rootNodeId);
    }
}

} // namespace ASFW::Bus
