// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// LocalIRMResourceController.hpp — Manages local IRM CSR registers (FW-13/19).

#pragma once

#include "../../Hardware/HardwareInterface.hpp"
#include "LocalCSRAccessor.hpp"
#include <cstdint>

namespace ASFW::Bus {

class BroadcastChannelCSR;

enum class LocalIRMResourceState : uint8_t {
    Disabled = 0,
    NotLocalIRM = 1,
    InitialRegistersProgrammed = 2,
    InitializingActiveResources = 3,
    ReadyDefaults = 4,
    ReadyChanged = 5,
    ProbeFailed = 6,
};

struct LocalIRMResourceSnapshot {
    LocalIRMResourceState state{LocalIRMResourceState::Disabled};
    uint32_t generation{0};
    uint8_t localNodeId{0x3F};
    uint8_t irmNodeId{0x3F};
    
    bool initialRegistersProgrammed{false};
    bool localIsIRM{false};
    bool activeProbeAttempted{false};
    bool activeProbeSucceeded{false};

    uint32_t busManagerId{0x3F};
    uint32_t bandwidthAvailable{0};
    uint32_t channelsAvailableHi{0};
    uint32_t channelsAvailableLo{0};
    
    uint8_t lastCsrStatus{0}; // Success = 0, Timeout = 1, HardwareUnavailable = 2, AccessFailed = 3
};

/**
 * @brief Coordinates local IRM resource hosting based on role policy and topology.
 * This class handles the two-phase IRM initialization: initial registers during bring-up
 * and active CSR probing when the local node wins the IRM election.
 */
class LocalIRMResourceController {
public:
    explicit LocalIRMResourceController(ASFW::Driver::HardwareInterface& hw,
                                        BroadcastChannelCSR& broadcastChannel) noexcept;
    ~LocalIRMResourceController() = default;

    // Disable copy/move
    LocalIRMResourceController(const LocalIRMResourceController&) = delete;
    LocalIRMResourceController& operator=(const LocalIRMResourceController&) = delete;

    /**
     * @brief Called when a bus reset sequence begins.
     * Resets BROADCAST_CHANNEL to unimplemented and marks state as InitialRegistersProgrammed.
     */
    void OnBusResetStarted(uint32_t generation) noexcept;

    /**
     * @brief Called when topology is accepted and the local node's role is determined.
     */
    void OnTopologyReady(uint32_t generation,
                         uint8_t localNodeId,
                         uint8_t irmNodeId,
                         bool roleAllowsIRMHost) noexcept;

    /**
     * @brief Called when topology is invalid.
     */
    void OnTopologyInvalid(uint32_t generation) noexcept;

    /**
     * @brief Probes active OHCI autonomous CSRs via CSRControl.
     * Internal helper used by OnTopologyReady when local node is IRM.
     */
    bool ProbeActiveResources() noexcept;

    /**
     * @brief Disables the controller state.
     */
    void Disable() noexcept;

    /**
     * @brief Performs an atomic compare-swap on the local BUS_MANAGER_ID register.
     * Used by BM election when local node is the IRM.
     */
    [[nodiscard]] ASFW::Driver::LocalCSRLockResult CompareSwapBusManagerId(uint32_t compareValue,
                                                                         uint32_t newValue) noexcept;

    [[nodiscard]] LocalIRMResourceSnapshot Snapshot() const noexcept { return snapshot_; }

private:
    ASFW::Driver::HardwareInterface& hw_;
    BroadcastChannelCSR& broadcastChannel_;
    LocalCSRAccessor csr_;
    LocalIRMResourceSnapshot snapshot_;
};

} // namespace ASFW::Bus
