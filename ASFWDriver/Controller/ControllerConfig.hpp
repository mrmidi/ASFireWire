#pragma once

#include "../Common/CSRSpace.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ASFW::Driver {

struct VendorInfo {
    uint32_t vendorId{0};
    uint32_t deviceId{0};
    std::string vendorName;
};

// Immutable configuration describing how the controller core should initialise
// hardware and logging surfaces. Values here are populated by the DriverKit
// service before Start() so helpers remain pure C++.
struct ControllerConfig {
    VendorInfo vendor;
    uint64_t localGuid{0};
    bool enableVerboseLogging{false};
    bool experimentalHostCycleMasterBringup{false};
    bool allowCycleMasterEligibility{false};
    std::vector<uint32_t> supportedSpeeds;

    // FW-22: which capabilities the local Config ROM advertises.
    // Conservative Apple-style mode: advertise neither BM nor IRM capability.
    // Use LegacyBmcCleared only for backwards-compatible behavior verification.
    ASFW::FW::RoleMode roleMode{ASFW::FW::RoleMode::AppleAvoidManager};
    ASFW::FW::FullBMActivityLevel fullBMActivityLevel{ASFW::FW::FullBMActivityLevel::ObserveOnly};

    static ControllerConfig MakeDefault();
};

} // namespace ASFW::Driver
