// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// LocalCSRAccessor.hpp — Thin wrapper for local autonomous CSR access.

#pragma once

#include "../../Hardware/HardwareInterface.hpp"
#include "IRMCSRConstants.hpp"

namespace ASFW::Bus {

/**
 * @brief Provides semantic access to local OHCI autonomous CSR resources.
 * This class is a thin wrapper over HardwareInterface to provide named helpers
 * and "no-change" probing functionality for IRM CSRs.
 */
class LocalCSRAccessor {
public:
    explicit LocalCSRAccessor(ASFW::Driver::HardwareInterface& hw) noexcept : hw_(hw) {}

    // Read helpers
    [[nodiscard]] ASFW::Driver::LocalCSRReadResult ReadBusManagerId() noexcept;
    [[nodiscard]] ASFW::Driver::LocalCSRReadResult ReadBandwidthAvailable() noexcept;
    [[nodiscard]] ASFW::Driver::LocalCSRReadResult ReadChannelsAvailableHi() noexcept;
    [[nodiscard]] ASFW::Driver::LocalCSRReadResult ReadChannelsAvailableLo() noexcept;

    // No-change probe helpers (Compare-Swap with same value)
    [[nodiscard]] ASFW::Driver::LocalCSRLockResult ProbeBusManagerIdNoChange(uint32_t expected) noexcept;
    [[nodiscard]] ASFW::Driver::LocalCSRLockResult ProbeBandwidthNoChange(uint32_t expected) noexcept;
    [[nodiscard]] ASFW::Driver::LocalCSRLockResult ProbeChannelsHiNoChange(uint32_t expected) noexcept;
    [[nodiscard]] ASFW::Driver::LocalCSRLockResult ProbeChannelsLoNoChange(uint32_t expected) noexcept;

    // Real Compare-Swap for election
    [[nodiscard]] ASFW::Driver::LocalCSRLockResult CompareSwapBusManagerId(uint32_t compareValue,
                                                                         uint32_t newValue) noexcept;

private:
    ASFW::Driver::HardwareInterface& hw_;
};

} // namespace ASFW::Bus
