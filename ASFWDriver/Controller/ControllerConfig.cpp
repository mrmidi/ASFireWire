#include "ControllerConfig.hpp"

namespace ASFW::Driver {

ControllerConfig ControllerConfig::MakeDefault() {
    ControllerConfig config;
    config.vendor.vendorId = 0;
    config.vendor.deviceId = 0;
    config.vendor.vendorName = "Unknown";
    config.localGuid = 0;
    config.enableVerboseLogging = false;
    config.experimentalHostCycleMasterBringup = false;
    config.allowCycleMasterEligibility = false;
    config.supportedSpeeds = {100, 200, 400};
    // Role defaults are also expressed as member initializers in ControllerConfig.
    // The live Start() path builds a default-constructed ControllerConfig (so the
    // member initializers are what ship), while tests use MakeDefault(); set the
    // role fields explicitly here so the two sources can never silently diverge.
    config.roleMode = ASFW::FW::RoleMode::AppleAvoidManager;
    config.fullBMActivityLevel = ASFW::FW::FullBMActivityLevel::ObserveOnly;
    config.linuxStyleCmcForceRoot = false;
    return config;
}

} // namespace ASFW::Driver
