// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <DriverKit/IOReturn.h>
#include <atomic>
#include <cstdint>
#include <optional>

namespace ASFW::Audio::Runtime {
// A slot belongs to one nub incarnation. Zero means unpublished; bit zero
// marks a result and the remaining bits preserve all 32 IOReturn bits.
// Publish before termination, after transport cleanup has returned.
struct RemoteDeviceStopResult final {
    static void Publish(std::atomic<uint64_t>& slot, IOReturn status) noexcept {
        slot.store((static_cast<uint64_t>(static_cast<uint32_t>(status)) << 1) | 1U,
                   std::memory_order_release);
    }
    [[nodiscard]] static std::optional<IOReturn> Read(const std::atomic<uint64_t>& slot) noexcept {
        const auto result = slot.load(std::memory_order_acquire);
        if ((result & 1U) == 0) return std::nullopt;
        return static_cast<IOReturn>(static_cast<uint32_t>(result >> 1));
    }
};
} // namespace ASFW::Audio::Runtime
