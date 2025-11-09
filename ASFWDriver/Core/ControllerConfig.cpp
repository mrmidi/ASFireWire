#include "ControllerConfig.hpp"

namespace ASFW::Driver {

ControllerConfig ControllerConfig::MakeDefault() {
    ControllerConfig config;
    config.vendor.vendorId = 0;
    config.vendor.deviceId = 0;
    config.vendor.vendorName = "Unknown";
    config.localGuid = 0;
    config.enableVerboseLogging = false;
    config.allowCycleMasterEligibility = false;
    config.supportedSpeeds = {100, 200, 400};
    return config;
}

} // namespace ASFW::Driver

