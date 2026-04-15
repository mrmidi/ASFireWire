#pragma once

#include "ControllerConfig.hpp"
#include "../Bus/BusManager.hpp"

namespace ASFW::Driver {

// Experimental Nikon 8000 ED bring-up path:
// - enable local contender / cycle-master eligibility
// - disable delegation so the host keeps root/cycle-master responsibility
//
// The shipped default remains the conservative delegated path used by the rest
// of the driver. This helper centralizes the override so ControllerCore and
// tests apply the same behavior.
inline void ApplyBringupOverrides(ControllerConfig& config, BusManager* busManager) {
    config.allowCycleMasterEligibility = config.experimentalHostCycleMasterBringup;

    if (busManager != nullptr) {
        busManager->SetDelegateMode(!config.experimentalHostCycleMasterBringup);
    }
}

} // namespace ASFW::Driver
