// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// LocalIRMResourceController.hpp — Manages local IRM CSR registers (FW-13/19).

#pragma once

#include "../../Hardware/HardwareInterface.hpp"
#include <cstdint>

namespace ASFW::Bus {

enum class LocalIRMResourceState : uint8_t {
    Disabled = 0,
    Initialized = 1,
    Failed = 2,
};

struct LocalIRMResourceSnapshot {
    uint32_t busManagerId{0x3F};
    uint32_t bandwidthAvailable{0};
    uint32_t channelsAvailableHi{0};
    uint32_t channelsAvailableLo{0};
    LocalIRMResourceState state{LocalIRMResourceState::Disabled};
    bool readbackValid{false};
    uint8_t lastCsrStatus{0}; // Success = 0, Timeout = 1, HardwareUnavailable = 2, AccessFailed = 3
};

class LocalIRMResourceController {
public:
    explicit LocalIRMResourceController(ASFW::Driver::HardwareInterface* hw) noexcept;
    ~LocalIRMResourceController() = default;

    // Disable copy/move
    LocalIRMResourceController(const LocalIRMResourceController&) = delete;
    LocalIRMResourceController& operator=(const LocalIRMResourceController&) = delete;

    /**
     * @brief Writes default IRM values to local CSRs and validates them via readback.
     * @return true if all writes succeeded and readback matches, false otherwise.
     */
    bool InitializeDefaults() noexcept;

    /**
     * @brief Probes/reads back all local IRM registers to update our diagnostics cache.
     * @return true if reads completed successfully, false if any read failed/timed out.
     */
    bool ProbeReadback() noexcept;

    /**
     * @brief Performs a Compare-Swap on the local BUS_MANAGER_ID register.
     */
    ASFW::Driver::LocalCSRLockResult CompareSwapBusManagerId(uint32_t compare, uint32_t swap) noexcept;

    /**
     * @brief Disables the controller state.
     */
    void Disable() noexcept {
        snapshot_.state = LocalIRMResourceState::Disabled;
        snapshot_.readbackValid = false;
        snapshot_.busManagerId = 0x3F;
        snapshot_.bandwidthAvailable = 0;
        snapshot_.channelsAvailableHi = 0;
        snapshot_.channelsAvailableLo = 0;
    }

    [[nodiscard]] LocalIRMResourceSnapshot Snapshot() const noexcept { return snapshot_; }

private:
    ASFW::Driver::HardwareInterface* hw_;
    LocalIRMResourceSnapshot snapshot_;
};

} // namespace ASFW::Bus
