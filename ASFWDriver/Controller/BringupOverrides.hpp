#pragma once

#include "ControllerConfig.hpp"
#include "../Bus/BusManager.hpp"

namespace ASFW::Driver {

// Host cycle-master bring-up configuration:
// - enable local contender / cycle-master eligibility (matches Linux/Apple default)
// - delegation controlled by experimentalHostCycleMasterBringup property
//
// Per Linux firewire_ohci: PHY contender bit is always set during init.
// Per Apple IOFireWireController: contender is set for most configurations.
// The host MUST be contender-capable for proper bus management (IRM election,
// cycle-start generation), especially in 2-node topologies with SBP-2 devices.
inline void ApplyBringupOverrides(ControllerConfig& config, BusManager* busManager) {
    // Always enable contender — matches Linux/Apple behavior
    config.allowCycleMasterEligibility = true;

    if (busManager != nullptr) {
        // When experimental flag is set, disable delegation so host keeps
        // root/cycle-master. Otherwise use default delegation policy.
        busManager->SetDelegateMode(!config.experimentalHostCycleMasterBringup);
    }
}

} // namespace ASFW::Driver
