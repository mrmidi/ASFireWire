#pragma once

#include "ControllerConfig.hpp"
#include "../Bus/BusManager.hpp"

namespace ASFW::Driver {

// Host cycle-master bring-up configuration. Linux firewire_ohci and Apple
// IOFireWireController both make the local PHY contender-capable during init,
// while root delegation remains policy-controlled by BusManager.
inline void ApplyBringupOverrides(ControllerConfig& config, BusManager* busManager) {
    config.allowCycleMasterEligibility = true;

    if (busManager != nullptr) {
        // When experimental flag is set, disable delegation so host keeps
        // root/cycle-master. Otherwise use default delegation policy.
        busManager->SetDelegateMode(!config.experimentalHostCycleMasterBringup);
    }
}

} // namespace ASFW::Driver
