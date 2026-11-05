// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xSlotLayout.hpp - where PCM sits in a Digi 002 / 003 data block
// (issue #249).
//
// Data channel 0 of every data block is the MIDI data channel; PCM channel N
// is data channel N + 1, in both directions (Linux
// sound/firewire/digi00x/amdtp-dot.c: PCM is written and read from buffer[1],
// MIDI in buffer[0]; used as a behavioral reference). An idle MIDI data channel
// carries 0x80 in its first byte and zeros after it (write_midi_messages).

#pragma once

#include "../AMDTP/PcmSlotMap.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace ASFW::Encoding::Digi00x {

inline constexpr uint32_t kMidiDataChannel = 0;
inline constexpr uint32_t kFirstPcmDataChannel = 1;
/// An idle MIDI data channel, host order.
inline constexpr uint32_t kIdleMidiWord = 0x80000000U;

/// Logical PCM channel N -> data channel N + 1, for `pcmChannels` channels.
[[nodiscard]] inline ASFW::Audio::Wire::PcmSlotMap PcmAfterMidiSlotMap(uint32_t pcmChannels) noexcept {
    ASFW::Audio::Wire::PcmSlotMap map{};
    if (pcmChannels + kFirstPcmDataChannel > ASFW::Common::kMaxPcmSlots) return map;
    std::array<uint8_t, ASFW::Common::kMaxPcmSlots> order{};
    for (uint32_t channel = 0; channel < pcmChannels; ++channel) {
        order[channel] = static_cast<uint8_t>(channel + kFirstPcmDataChannel);
    }
    (void)map.SetSlots(std::span<const uint8_t>(order.data(), pcmChannels));
    return map;
}

} // namespace ASFW::Encoding::Digi00x
