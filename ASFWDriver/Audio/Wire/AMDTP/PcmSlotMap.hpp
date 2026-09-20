// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// PcmSlotMap.hpp — logical PCM channel to AM824 data-block slot permutation.

#pragma once

#include "AmdtpRateGeometry.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace ASFW::Audio::Wire {

/// Empty (`slotCount == 0`) means logical PCM channel N uses AM824 slot N.
/// A populated map must cover the active PCM width exactly and only address
/// slots carried by the active data block. It has no timing semantics, so it
/// can be used symmetrically by both capture decoding and playback encoding.
struct PcmSlotMap {
    std::array<uint8_t, ASFW::Encoding::kMaxPcmChannels> slotForChannel{};
    uint32_t slotCount{0};
    uint32_t channelCount{0};

    template <size_t Count>
    [[nodiscard]] constexpr bool SetSlots(
        const std::array<uint8_t, Count>& slots) noexcept {
        static_assert(Count <= ASFW::Encoding::kMaxPcmChannels);
        slotCount = static_cast<uint32_t>(Count);
        channelCount = static_cast<uint32_t>(Count);
        for (size_t index = 0; index < Count; ++index) {
            slotForChannel[index] = slots[index];
        }
        return true;
    }

    [[nodiscard]] bool SetSlots(std::span<const uint8_t> slots) noexcept {
        if (slots.size() > slotForChannel.size()) return false;
        slotCount = static_cast<uint32_t>(slots.size());
        channelCount = static_cast<uint32_t>(slots.size());
        for (size_t index = 0; index < slots.size(); ++index) {
            slotForChannel[index] = slots[index];
        }
        return true;
    }

    [[nodiscard]] constexpr bool IsIdentity() const noexcept {
        return slotCount == 0;
    }

    [[nodiscard]] constexpr bool FitsWithin(uint32_t channels,
                                            uint32_t dataBlockSize) const noexcept {
        if (slotCount == 0) return true;
        if (channelCount != channels || slotCount < channels) return false;
        for (uint32_t channel = 0; channel < channels; ++channel) {
            if (slotForChannel[channel] >= dataBlockSize) return false;
        }
        return true;
    }

    [[nodiscard]] constexpr uint32_t SlotFor(uint32_t channel) const noexcept {
        return slotCount == 0 ? channel : slotForChannel[channel];
    }
};

} // namespace ASFW::Audio::Wire
