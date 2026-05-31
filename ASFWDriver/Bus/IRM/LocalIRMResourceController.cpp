// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// LocalIRMResourceController.cpp — see LocalIRMResourceController.hpp

#include "LocalIRMResourceController.hpp"
#include "IRMCSRConstants.hpp"
#include "../../Logging/Logging.hpp"

namespace ASFW::Bus {

LocalIRMResourceController::LocalIRMResourceController(ASFW::Driver::HardwareInterface* hw) noexcept
    : hw_(hw) {}

bool LocalIRMResourceController::InitializeDefaults() noexcept {
    if (!hw_) {
        snapshot_.state = LocalIRMResourceState::Failed;
        snapshot_.lastCsrStatus = 2; // HardwareUnavailable
        return false;
    }

    ASFW_LOG(Controller, "[IRM] Initializing local IRM resource registers");

    using namespace ASFW::Driver::IRMCSR;

    auto r0 = hw_->WriteLocalIRMResource(static_cast<uint32_t>(CSRSelector::BusManagerId), 
                                         kNoBusManagerId);
    auto r1 = hw_->WriteLocalIRMResource(static_cast<uint32_t>(CSRSelector::BandwidthAvailable), 
                                         kInitialBandwidthAvailable);
    auto r2 = hw_->WriteLocalIRMResource(static_cast<uint32_t>(CSRSelector::ChannelsAvailableHi), 
                                         kInitialChannelsAvailableHi);
    auto r3 = hw_->WriteLocalIRMResource(static_cast<uint32_t>(CSRSelector::ChannelsAvailableLo), 
                                         kInitialChannelsAvailableLo);

    if (r0.status != ASFW::Driver::LocalCSRLockResult::Status::Success ||
        r1.status != ASFW::Driver::LocalCSRLockResult::Status::Success ||
        r2.status != ASFW::Driver::LocalCSRLockResult::Status::Success ||
        r3.status != ASFW::Driver::LocalCSRLockResult::Status::Success) {
        
        snapshot_.state = LocalIRMResourceState::Failed;
        if (r0.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout ||
            r1.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout ||
            r2.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout ||
            r3.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout) {
            snapshot_.lastCsrStatus = 1; // Timeout
        } else {
            snapshot_.lastCsrStatus = 2; // HardwareUnavailable
        }
        ASFW_LOG(Controller, "❌ [IRM] Failed to write local IRM resources: status r0=%d r1=%d r2=%d r3=%d",
                 static_cast<int>(r0.status), static_cast<int>(r1.status),
                 static_cast<int>(r2.status), static_cast<int>(r3.status));
        return false;
    }

    // Now validate via readback
    bool valid = ProbeReadback();
    if (valid) {
        snapshot_.state = LocalIRMResourceState::Initialized;
    } else {
        snapshot_.state = LocalIRMResourceState::Failed;
        snapshot_.lastCsrStatus = 3; // AccessFailed
    }
    return valid;
}

bool LocalIRMResourceController::ProbeReadback() noexcept {
    if (!hw_) {
        snapshot_.readbackValid = false;
        snapshot_.lastCsrStatus = 2; // HardwareUnavailable
        return false;
    }

    auto r0 = hw_->ReadLocalIRMResource(0);
    auto r1 = hw_->ReadLocalIRMResource(1);
    auto r2 = hw_->ReadLocalIRMResource(2);
    auto r3 = hw_->ReadLocalIRMResource(3);

    if (r0.status != ASFW::Driver::LocalCSRLockResult::Status::Success ||
        r1.status != ASFW::Driver::LocalCSRLockResult::Status::Success ||
        r2.status != ASFW::Driver::LocalCSRLockResult::Status::Success ||
        r3.status != ASFW::Driver::LocalCSRLockResult::Status::Success) {
        
        snapshot_.readbackValid = false;
        if (r0.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout ||
            r1.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout ||
            r2.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout ||
            r3.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout) {
            snapshot_.lastCsrStatus = 1; // Timeout
        } else {
            snapshot_.lastCsrStatus = 2; // HardwareUnavailable
        }
        return false;
    }

    snapshot_.busManagerId = r0.value;
    snapshot_.bandwidthAvailable = r1.value;
    snapshot_.channelsAvailableHi = r2.value;
    snapshot_.channelsAvailableLo = r3.value;

    snapshot_.readbackValid = true;
    snapshot_.lastCsrStatus = 0; // OK
    return true;
}

ASFW::Driver::LocalCSRLockResult LocalIRMResourceController::CompareSwapBusManagerId(uint32_t compare, uint32_t swap) noexcept {
    if (!hw_) {
        return {ASFW::Driver::LocalCSRLockResult::Status::HardwareUnavailable, 0, false};
    }
    auto result = hw_->CompareSwapLocalIRMResource(0, compare, swap);
    if (result.status == ASFW::Driver::LocalCSRLockResult::Status::Success) {
        snapshot_.busManagerId = result.compareMatched ? swap : result.oldValue;
        snapshot_.lastCsrStatus = 0; // OK
    } else if (result.status == ASFW::Driver::LocalCSRLockResult::Status::Timeout) {
        snapshot_.lastCsrStatus = 1; // Timeout
    } else {
        snapshot_.lastCsrStatus = 2; // HardwareUnavailable
    }
    return result;
}

} // namespace ASFW::Bus
