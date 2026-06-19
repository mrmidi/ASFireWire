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
    // Role/BM policy is no longer part of ControllerConfig — it lives in the
    // separately-owned, runtime-mutable RolePolicy (see ControllerConfig.hpp).
    return config;
}

} // namespace ASFW::Driver
