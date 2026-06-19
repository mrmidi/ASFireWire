// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// BroadcastChannelCSR.hpp — Local software-owned BROADCAST_CHANNEL register (FW-19).

#pragma once

#include <atomic>
#include <cstdint>
#include "../IRM/IRMCSRConstants.hpp"

namespace ASFW::Bus {

/**
 * @brief Manages the local software-owned BROADCAST_CHANNEL CSR (offset 0x234).
 * This class handles thread-safe access and V0 sanitization policy.
 */
class BroadcastChannelCSR {
public:
    BroadcastChannelCSR() noexcept = default;
    ~BroadcastChannelCSR() = default;

    // Disable copy/move
    BroadcastChannelCSR(const BroadcastChannelCSR&) = delete;
    BroadcastChannelCSR& operator=(const BroadcastChannelCSR&) = delete;

    /**
     * @brief Resets the register to the "implemented but invalid" initial state (0x8000001F).
     */
    void ResetImplementedInvalid() noexcept {
        value_.store(Driver::IRMCSR::kBroadcastChannelImplementedInvalid, std::memory_order_release);
    }

    /**
     * @brief Marks the register as "valid" (0xC000001F). Usually called when local wins IRM.
     */
    void MarkValidChannel31() noexcept {
        value_.store(Driver::IRMCSR::kBroadcastChannelImplementedValid, std::memory_order_release);
    }

    /**
     * @brief Reads the current quadlet value.
     */
    [[nodiscard]] uint32_t Read() const noexcept {
        return value_.load(std::memory_order_acquire);
    }

    /**
     * @brief Writes a new quadlet value with V0 sanitization.
     * V0 Policy: Force implemented=1, preserve valid, force channel=31, zero reserved.
     */
    void Write(uint32_t incoming) noexcept {
        value_.store(Sanitize(incoming), std::memory_order_release);
    }

private:
    /**
     * @brief Applies V0 sanitization: 0x80000000 | (valid bit) | 31.
     */
    [[nodiscard]] static uint32_t Sanitize(uint32_t incoming) noexcept {
        // bit 31 = implemented (force 1)
        // bit 30 = valid (preserve from incoming)
        // bit 0..5 = channel (force 31)
        return 0x80000000U | (incoming & 0x40000000U) | 0x0000001FU;
    }

    std::atomic<uint32_t> value_{Driver::IRMCSR::kBroadcastChannelImplementedInvalid};
};

} // namespace ASFW::Bus
