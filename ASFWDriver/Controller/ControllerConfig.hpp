#pragma once

#include "../Common/CSRSpace.hpp"

#include <cstdint>
#include <string>

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

    // FW-22: which capabilities the local Config ROM advertises.
    // Default ClientOnly: advertise neither BM nor IRM capability (conservative,
    // and intentionally MORE conservative than Apple/Linux, which advertise bmc=1).
    // Use LegacyBmcCleared only for backwards-compatible behavior verification.
    ASFW::FW::RoleMode roleMode{ASFW::FW::RoleMode::ClientOnly};
    ASFW::FW::FullBMActivityLevel fullBMActivityLevel{ASFW::FW::FullBMActivityLevel::ObserveOnly};

    // EXPERIMENTAL (FW-21): Linux-shaped self-promotion on a verified CMC=0 root.
    // Apple never does this, so it is OFF by default and only takes effect when the
    // activity ladder is also at ForceRootAllowed or higher and local == IRM.
    bool linuxStyleCmcForceRoot{false};

    static ControllerConfig MakeDefault();
};

} // namespace ASFW::Driver
