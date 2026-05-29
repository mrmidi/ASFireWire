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
        remoteCmstrInFlight_ = false;
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
        // We are BM but NOT root. We must ensure the root node is behaving as cycle master
        // (i.e. generating CycleStart packets) if it is capable.
        // For this minimal phase, if the remote root is cycle master capable, we write
        // STATE_SET.cmstr to the root node.
        if (deps_.busImpl && state.rootNodeId != 0x3F && !remoteCmstrInFlight_) {
            ASFW_LOG(Controller, "[BM Policy] We are BM; remote root (id=%u) is cycle master. Sending STATE_SET.cmstr write request", state.rootNodeId);

            const Async::FWAddress address{Async::FWAddress::QualifiedAddressParts{
                .addressHi = ASFW::FW::kCSRRegSpaceHi,
                .addressLo = ASFW::FW::kCSRRemoteStateSet,
                .nodeID = static_cast<uint16_t>(0xFFC0u | (state.rootNodeId & 0x3Fu)),
            }};

            remoteCmstrInFlight_ = true;
            deps_.busImpl->WriteQuad(ASFW::FW::Generation{state.generation},
                                     ASFW::FW::NodeId{state.rootNodeId},
                                     address,
                                     ASFW::FW::kCSRStateBitCMSTR,
                                     ASFW::FW::FwSpeed::S100,
                                     [this, gen = state.generation, rootId = state.rootNodeId](Async::AsyncStatus status, std::span<const uint8_t>) {
                remoteCmstrInFlight_ = false;
                ASFW_LOG(Controller, "[BM Policy] Remote root (id=%u) STATE_SET.cmstr write finished with status %d", rootId, static_cast<int>(status));
            });
        }
    }
}

} // namespace ASFW::Bus
