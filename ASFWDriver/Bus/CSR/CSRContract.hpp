// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CSRContract.hpp — IEEE 1212 / IEEE 1394 Bus Manager CSR ownership contract.

#pragma once

#include <cstdint>

namespace ASFW::Bus {

/**
 * @brief Authoritative owner of a CSR register.
 */
enum class CSRRegisterOwner : uint8_t {
    Unknown = 0,
    HardwareOHCI,    ///< Handled autonomously by the OHCI chip.
    SoftwareASFW,    ///< Handled in software by ASFW driver (CSRResponder).
    Unsupported,     ///< Not implemented in either HW or SW.
    Reserved,        ///< Reserved by spec; should return error or NotMine.
};

/**
 * @brief Access permissions for a CSR register.
 */
enum class CSRAccessPolicy : uint8_t {
    ReadOnly = 0,
    WriteOnly,
    ReadWrite,
    LockOnly,
    ReadLock,
    WriteLock,
};

/**
 * @brief Definitive contract for a single CSR register range.
 */
struct CSRRegisterContract {
    uint32_t offsetLo{0};
    uint32_t sizeBytes{4};

    CSRRegisterOwner owner{CSRRegisterOwner::Unknown};
    CSRAccessPolicy access{CSRAccessPolicy::ReadOnly};

    const char* name{nullptr};
};

/**
 * @brief Global CSR ownership contract table (Milestone 9).
 */
namespace CSRContract {

// Base for all registers
static constexpr uint32_t kCoreBase = 0xF0000000u;

// STATE_CLEAR (0x00) - SW
static constexpr uint32_t kStateClear = kCoreBase + 0x000;
// STATE_SET (0x04) - SW
static constexpr uint32_t kStateSet = kCoreBase + 0x004;
// NODE_IDS (0x08) - HW (OHCI derived)
static constexpr uint32_t kNodeIds = kCoreBase + 0x008;
// RESET_START (0x0C) - SW
static constexpr uint32_t kResetStart = kCoreBase + 0x00C;
// CYCLE_TIME (0x200) - HW (OHCI derived)
static constexpr uint32_t kCycleTime = kCoreBase + 0x200;
// BUS_MANAGER_ID (0x21C) - HW
static constexpr uint32_t kBusManagerId = kCoreBase + 0x21C;
// BANDWIDTH_AVAILABLE (0x220) - HW
static constexpr uint32_t kBandwidthAvailable = kCoreBase + 0x220;
// CHANNELS_AVAILABLE_HI (0x224) - HW
static constexpr uint32_t kChannelsAvailableHi = kCoreBase + 0x224;
// CHANNELS_AVAILABLE_LO (0x228) - HW
static constexpr uint32_t kChannelsAvailableLo = kCoreBase + 0x228;
// BROADCAST_CHANNEL (0x234) - SW
static constexpr uint32_t kBroadcastChannel = kCoreBase + 0x234;
// TOPOLOGY_MAP (0x1000) - SW
static constexpr uint32_t kTopologyMapBase = kCoreBase + 0x1000;
static constexpr uint32_t kTopologyMapEnd = kCoreBase + 0x13FF;
// SPEED_MAP (0x2000) - SW
static constexpr uint32_t kSpeedMapBase = kCoreBase + 0x2000;
static constexpr uint32_t kSpeedMapEnd = kCoreBase + 0x23FF;

} // namespace CSRContract

/**
 * @brief Authoritative table of Bus Manager CSRs.
 */
inline constexpr CSRRegisterContract kBusManagerCSRContract[] = {
    {CSRContract::kStateClear, 4, CSRRegisterOwner::SoftwareASFW, CSRAccessPolicy::ReadWrite,
     "STATE_CLEAR"},
    {CSRContract::kStateSet, 4, CSRRegisterOwner::SoftwareASFW, CSRAccessPolicy::ReadWrite,
     "STATE_SET"},
    {CSRContract::kNodeIds, 4, CSRRegisterOwner::HardwareOHCI, CSRAccessPolicy::ReadOnly,
     "NODE_IDS"},
    {CSRContract::kResetStart, 4, CSRRegisterOwner::SoftwareASFW, CSRAccessPolicy::WriteOnly,
     "RESET_START"},
    {CSRContract::kCycleTime, 4, CSRRegisterOwner::HardwareOHCI, CSRAccessPolicy::ReadWrite,
     "CYCLE_TIME"},
    {CSRContract::kBusManagerId, 4, CSRRegisterOwner::HardwareOHCI, CSRAccessPolicy::ReadLock,
     "BUS_MANAGER_ID"},
    {CSRContract::kBandwidthAvailable, 4, CSRRegisterOwner::HardwareOHCI,
     CSRAccessPolicy::ReadLock, "BANDWIDTH_AVAILABLE"},
    {CSRContract::kChannelsAvailableHi, 4, CSRRegisterOwner::HardwareOHCI,
     CSRAccessPolicy::ReadLock, "CHANNELS_AVAILABLE_HI"},
    {CSRContract::kChannelsAvailableLo, 4, CSRRegisterOwner::HardwareOHCI,
     CSRAccessPolicy::ReadLock, "CHANNELS_AVAILABLE_LO"},
    {CSRContract::kBroadcastChannel, 4, CSRRegisterOwner::SoftwareASFW, CSRAccessPolicy::ReadWrite,
     "BROADCAST_CHANNEL"},
    {CSRContract::kTopologyMapBase, 0x400, CSRRegisterOwner::SoftwareASFW,
     CSRAccessPolicy::ReadOnly, "TOPOLOGY_MAP"},
    {CSRContract::kSpeedMapBase, 0x400, CSRRegisterOwner::SoftwareASFW, CSRAccessPolicy::ReadOnly,
     "SPEED_MAP"},
};

} // namespace ASFW::Bus
