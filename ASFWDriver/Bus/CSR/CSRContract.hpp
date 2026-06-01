// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CSRContract.hpp — IEEE 1212 / IEEE 1394 Bus Manager CSR ownership contract.

#pragma once

#include "../../Common/CSRSpace.hpp"
#include <cstdint>
#include <optional>

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
 * Distinguishes between quadlet and block access as requested.
 */
enum class CSRAccessPolicy : uint8_t {
    ReadOnly = 0,       ///< Quadlet read only.
    WriteOnly,          ///< Quadlet write only.
    ReadWrite,          ///< Quadlet read/write.
    BlockReadOnly,      ///< Block read supported.
    ReadWriteBlock,     ///< Quadlet R/W + Block read supported.
    LockOnly,           ///< Lock only (e.g. BANDWIDTH).
    ReadLock,           ///< Read and Lock.
    WriteLock,          ///< Write and Lock.
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
 * @brief Authoritative table of Bus Manager CSRs.
 */
inline constexpr CSRRegisterContract kBusManagerCSRContract[] = {
    {FW::kCSR_StateClear, 4, CSRRegisterOwner::SoftwareASFW, CSRAccessPolicy::ReadWrite,
     "STATE_CLEAR"},
    {FW::kCSR_StateSet, 4, CSRRegisterOwner::SoftwareASFW, CSRAccessPolicy::ReadWrite,
     "STATE_SET"},
    {FW::kCSR_NodeIDs, 4, CSRRegisterOwner::HardwareOHCI, CSRAccessPolicy::ReadOnly,
     "NODE_IDS"},
    {FW::kCSR_ResetStart, 4, CSRRegisterOwner::SoftwareASFW, CSRAccessPolicy::WriteOnly,
     "RESET_START"},
    {FW::kCSR_BusManagerID, 4, CSRRegisterOwner::HardwareOHCI, CSRAccessPolicy::ReadLock,
     "BUS_MANAGER_ID"},
    {FW::kCSR_BandwidthAvailable, 4, CSRRegisterOwner::HardwareOHCI,
     CSRAccessPolicy::ReadLock, "BANDWIDTH_AVAILABLE"},
    {FW::kCSR_ChannelsAvailableHi, 4, CSRRegisterOwner::HardwareOHCI,
     CSRAccessPolicy::ReadLock, "CHANNELS_AVAILABLE_HI"},
    {FW::kCSR_ChannelsAvailableLo, 4, CSRRegisterOwner::HardwareOHCI,
     CSRAccessPolicy::ReadLock, "CHANNELS_AVAILABLE_LO"},
    {FW::kCSR_BroadcastChannel, 4, CSRRegisterOwner::SoftwareASFW, CSRAccessPolicy::ReadWrite,
     "BROADCAST_CHANNEL"},
    {FW::kCSR_TopologyMapBase, 0x400, CSRRegisterOwner::SoftwareASFW,
     CSRAccessPolicy::BlockReadOnly, "TOPOLOGY_MAP"},
    {FW::kCSR_SpeedMapBase, 0x400, CSRRegisterOwner::SoftwareASFW, CSRAccessPolicy::ReadOnly,
     "SPEED_MAP"},
};

/**
 * @brief Finds the contract for a given CSR offset.
 */
[[nodiscard]] inline std::optional<CSRRegisterContract> FindCSRContract(uint32_t offsetLo) noexcept {
    for (const auto& entry : kBusManagerCSRContract) {
        if (offsetLo >= entry.offsetLo && offsetLo < (entry.offsetLo + entry.sizeBytes)) {
            return entry;
        }
    }
    return std::nullopt;
}

} // namespace ASFW::Bus
