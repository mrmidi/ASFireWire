// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// LocalIRMResourceController.cpp — see LocalIRMResourceController.hpp

#include "LocalIRMResourceController.hpp"
#include "../CSR/BroadcastChannelCSR.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Bus {

using namespace ASFW::Driver::IRMCSR;

LocalIRMResourceController::LocalIRMResourceController(ASFW::Driver::HardwareInterface& hw,
                                                       BroadcastChannelCSR& broadcastChannel) noexcept
    : hw_(hw), broadcastChannel_(broadcastChannel), csr_(hw) {}

void LocalIRMResourceController::OnBusResetStarted(uint32_t generation) noexcept {
    snapshot_.generation = generation;
    snapshot_.state = LocalIRMResourceState::InitialRegistersProgrammed;
    snapshot_.localIsIRM = false;
    snapshot_.activeProbeAttempted = false;
    snapshot_.activeProbeSucceeded = false;
    
    // BROADCAST_CHANNEL resets to unimplemented/invalid on bus reset
    broadcastChannel_.ResetImplementedInvalid();
}

void LocalIRMResourceController::OnTopologyReady(uint32_t generation,
                                                 uint8_t localNodeId,
                                                 uint8_t irmNodeId,
                                                 bool roleAllowsIRMHost) noexcept {
    snapshot_.generation = generation;
    snapshot_.localNodeId = localNodeId;
    snapshot_.irmNodeId = irmNodeId;

    if (!roleAllowsIRMHost) {
        snapshot_.state = LocalIRMResourceState::Disabled;
        broadcastChannel_.ResetImplementedInvalid();
        return;
    }

    if (localNodeId != irmNodeId) {
        snapshot_.state = LocalIRMResourceState::NotLocalIRM;
        snapshot_.localIsIRM = false;
        broadcastChannel_.ResetImplementedInvalid();
        return;
    }

    // Local node is IRM!
    snapshot_.localIsIRM = true;
    snapshot_.state = LocalIRMResourceState::InitializingActiveResources;
    
    // Mark BROADCAST_CHANNEL as valid (0xC000001F)
    broadcastChannel_.MarkValidChannel31();

    // Probe active OHCI autonomous resources
    ProbeActiveResources();
}

void LocalIRMResourceController::OnTopologyInvalid(uint32_t generation) noexcept {
    snapshot_.generation = generation;
    snapshot_.state = LocalIRMResourceState::Disabled;
    snapshot_.localIsIRM = false;
    broadcastChannel_.ResetImplementedInvalid();
}

bool LocalIRMResourceController::ProbeActiveResources() noexcept {
    snapshot_.activeProbeAttempted = true;
    
    // Probe all four core IRM resources using "no-change" Compare-Swap.
    // This allows us to discover the current state without blindly overwriting
    // potentially allocated resources from remote nodes.
    auto rBM = csr_.ProbeBusManagerIdNoChange(kNoBusManagerId);
    auto rBW = csr_.ProbeBandwidthNoChange(kInitialBandwidthAvailable);
    auto rCHi = csr_.ProbeChannelsHiNoChange(kInitialChannelsAvailableHi);
    auto rCLo = csr_.ProbeChannelsLoNoChange(kInitialChannelsAvailableLo);

    if (rBM.status != ASFW::Driver::LocalCSRLockResult::Status::Success ||
        rBW.status != ASFW::Driver::LocalCSRLockResult::Status::Success ||
        rCHi.status != ASFW::Driver::LocalCSRLockResult::Status::Success ||
        rCLo.status != ASFW::Driver::LocalCSRLockResult::Status::Success) {
        
        snapshot_.state = LocalIRMResourceState::ProbeFailed;
        snapshot_.activeProbeSucceeded = false;
        
        if (rBM.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout ||
            rBW.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout ||
            rCHi.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout ||
            rCLo.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout) {
            snapshot_.lastCsrStatus = 1; // Timeout
        } else {
            snapshot_.lastCsrStatus = 2; // HardwareUnavailable
        }
        return false;
    }

    // Store read/old values for diagnostics
    snapshot_.busManagerId = rBM.oldValue;
    snapshot_.bandwidthAvailable = rBW.oldValue;
    snapshot_.channelsAvailableHi = rCHi.oldValue;
    snapshot_.channelsAvailableLo = rCLo.oldValue;
    
    snapshot_.activeProbeSucceeded = true;
    snapshot_.lastCsrStatus = 0; // OK

    // Decide between ReadyDefaults or ReadyChanged
    if (rBM.compareMatched && rBW.compareMatched && rCHi.compareMatched && rCLo.compareMatched) {
        snapshot_.state = LocalIRMResourceState::ReadyDefaults;
    } else {
        snapshot_.state = LocalIRMResourceState::ReadyChanged;
    }

    return true;
}

void LocalIRMResourceController::Disable() noexcept {
    snapshot_.state = LocalIRMResourceState::Disabled;
    snapshot_.localIsIRM = false;
    snapshot_.activeProbeAttempted = false;
    snapshot_.activeProbeSucceeded = false;
    broadcastChannel_.ResetImplementedInvalid();
}

} // namespace ASFW::Bus
