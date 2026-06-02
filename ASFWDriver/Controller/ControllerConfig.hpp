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

// Immutable identity/static configuration supplied at construction. Values here
// are populated by the DriverKit service before Start() and never change at
// runtime. Mutable role/bus-management policy lives in RolePolicy (below), not
// here, so the role mode can be switched at runtime via ControllerCore.
struct ControllerConfig {
    VendorInfo vendor;
    uint64_t localGuid{0};
    bool enableVerboseLogging{false};
    bool experimentalHostCycleMasterBringup{false};
    bool allowCycleMasterEligibility{false};

    static ControllerConfig MakeDefault();
};

// Runtime-mutable bus-management policy. Owned by ControllerCore and changed
// only through ControllerCore::ApplyRolePolicy(), which re-stages the local
// Config ROM (BIB capabilities) and triggers a bus reset so peers re-read it.
// Kept out of the constructor (and out of immutable ControllerConfig) precisely
// so role mode can be flipped while the driver is running.

/**
 * @brief Activity tier for power management and Link-On policy (Milestone 8).
 *
 * This level is a separate axis from the main bus-management activity ladder
 * because Link-On is an explicit wakeup command, not a topology mutation.
 * Cross-validated with linux: core-device.c:1314, core-topology.c:377.
 */
enum class PowerPolicyLevel : uint8_t {
    ObserveOnly = 0,   ///< Identify link-inactive nodes but do not wake them.
    LinkOnAllowed = 1, ///< Send Link-On packets to eligible nodes when BM/fallback IRM.
};

// FW-22: roleMode selects which capabilities the local Config ROM advertises.
//   The value-initialized policy remains ClientOnly for unit tests and explicit
//   passive construction. The live driver seeds ServiceContext with the hardware
//   validation profile below so ASFW advertises BM/IRM capability and can be
//   observed against Linux/Apple-like behavior on a real bus.
struct RolePolicy {
    ASFW::FW::RoleMode roleMode{ASFW::FW::RoleMode::ClientOnly};
    ASFW::FW::FullBMActivityLevel fullBMActivityLevel{ASFW::FW::FullBMActivityLevel::ObserveOnly};
    PowerPolicyLevel powerPolicyLevel{PowerPolicyLevel::ObserveOnly};

    // EXPERIMENTAL (FW-21): Linux-shaped self-promotion on a verified CMC=0 root.
    // Apple never does this, so it is OFF by default and only takes effect when the
    // activity ladder is also at ForceRootAllowed or higher and local == IRM.
    bool linuxStyleCmcForceRoot{false};

    [[nodiscard]] static constexpr RolePolicy MakeHardwareValidationDefault() noexcept {
        RolePolicy policy{};
        // cross-validated with Linux: core-card.c:425-428 Apple: IOFireWireController.cpp:3258-3367
        policy.roleMode = ASFW::FW::RoleMode::FullBusManager;
        // Hardware validation needs the complete BM mutation envelope except the
        // legacy remote STATE_SET.cmstr path. ForceRootAllowed unlocks M6 root
        // selection and M7 gap optimization; RemoteCmstrAllowed remains opt-in.
        policy.fullBMActivityLevel = ASFW::FW::FullBMActivityLevel::ForceRootAllowed;
        policy.powerPolicyLevel = PowerPolicyLevel::LinkOnAllowed;
        return policy;
    }
};

} // namespace ASFW::Driver
