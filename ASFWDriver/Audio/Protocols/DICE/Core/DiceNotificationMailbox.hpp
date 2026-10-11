// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceNotificationMailbox.hpp - One DICE device's notification bits.
//
// A DICE device reports clock, lock and stream-configuration changes by
// writing a quadlet to the address its owner registered (GLOBAL_OWNER). Each
// device's bits land in its own mailbox; DiceNotificationRouter attributes a
// write to its device by source node.

#pragma once

#include <atomic>
#include <cstdint>

namespace ASFW::Audio::DICE {

/// The host address our owner value names, as saffire.kext uses it.
inline constexpr uint64_t kNotificationHandlerOffset = 0x000100000000ULL;

[[nodiscard]] constexpr bool IsNotificationAddress(uint64_t destOffset) noexcept {
    return destOffset == kNotificationHandlerOffset;
}

[[nodiscard]] inline uint32_t DecodeNotificationQuadlet(const uint8_t* data) noexcept {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

class DiceNotificationMailbox final {
public:
    DiceNotificationMailbox() noexcept = default;
    DiceNotificationMailbox(const DiceNotificationMailbox&) = delete;
    DiceNotificationMailbox& operator=(const DiceNotificationMailbox&) = delete;

    void Reset() noexcept { bits_.store(0, std::memory_order_release); }
    void Publish(uint32_t bits) noexcept { bits_.fetch_or(bits, std::memory_order_acq_rel); }
    // Take every bit latched so far.
    [[nodiscard]] uint32_t Consume() noexcept { return bits_.exchange(0, std::memory_order_acq_rel); }
    [[nodiscard]] uint32_t Snapshot() const noexcept { return bits_.load(std::memory_order_acquire); }

private:
    std::atomic<uint32_t> bits_{0};
};

} // namespace ASFW::Audio::DICE
