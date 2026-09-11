// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuRegisterDsp.hpp - Output-volume encoding, and the DSP state messages that v2
// "register DSP" models interleave into their capture stream.
//
// Output volume. Registers 0x0c0c (main) and 0x0c10 (phones) hold 0..0x80, linear in dB
// from -64 dB to 0 dB, so one step is 0.5 dB. Cross-validated with snd-firewire-ctl-services
// protocols/motu/src/register_dsp.rs:886-887 and runtime/motu/src/register_dsp_ctls.rs:841-846
// (DbInterval -6400..0 linear), and FFADO motu_mixerdefs.cpp:126-127. The register takes the
// bare value, as ctl-services writes it; FFADO also sets bit 24 (motu_controls.cpp:476-481).
//
// DSP messages. Each capture data block carries one message in its first message chunk:
// byte 4 holds the type in bits 7..3 (bit 0 is the MIDI flag), byte 5 the value. The device
// cycles through its state, so a knob turn arrives as a new value within a few blocks with
// no bus transaction at all (Linux motu-register-dsp-message-parser.c:24-38, :160-170,
// :266-277). The UltraLite is flagged SND_MOTU_SPEC_REGISTER_DSP (motu-protocol-v2.c:302-307).
//
// (Behavioural sources only; no code copied.)

#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::Encoding::Motu {

inline constexpr uint8_t kOutputVolumeMaxRaw = 0x80;
inline constexpr float kOutputVolumeMinDb = -64.0f;
inline constexpr float kOutputVolumeMaxDb = 0.0f;

[[nodiscard]] constexpr float OutputVolumeToDb(uint8_t raw) noexcept {
    const uint8_t clamped = raw > kOutputVolumeMaxRaw ? kOutputVolumeMaxRaw : raw;
    return (static_cast<float>(clamped) - static_cast<float>(kOutputVolumeMaxRaw)) * 0.5f;
}

[[nodiscard]] constexpr uint8_t OutputVolumeFromDb(float decibels) noexcept {
    if (!(decibels > kOutputVolumeMinDb)) { // also catches NaN
        return 0;
    }
    if (decibels >= kOutputVolumeMaxDb) {
        return kOutputVolumeMaxRaw;
    }
    // (dB + 64) * 2 is non-negative here, so adding 0.5 and truncating rounds to nearest.
    return static_cast<uint8_t>((decibels - kOutputVolumeMinDb) * 2.0f + 0.5f);
}

/// Message types the parser needs (motu-register-dsp-message-parser.c:46-57).
enum class RegisterDspMessage : uint8_t {
    kMainOutputVolume = 0x07,
    kPhonesVolume = 0x08,
};

inline constexpr uint32_t kDspMessageFlagByte = 4;
inline constexpr uint32_t kDspMessageValueByte = 5;
inline constexpr uint8_t kDspMessageTypeMask = 0xF8;
inline constexpr uint32_t kDspMessageTypeShift = 3;

/// The last value `payload` reports for `type`, or nullopt when no block carries it.
/// `prefixBytes` is where the first data block starts; `dbs` is the block size in quadlets.
[[nodiscard]] inline std::optional<uint8_t> LatestDspMessageValue(
    std::span<const uint8_t> payload,
    uint32_t dbs,
    uint32_t dataBlocks,
    uint32_t prefixBytes,
    RegisterDspMessage type) noexcept {
    const size_t blockBytes = static_cast<size_t>(dbs) * 4U;
    if (blockBytes <= kDspMessageValueByte) {
        return std::nullopt;
    }
    std::optional<uint8_t> latest{};
    for (uint32_t block = 0; block < dataBlocks; ++block) {
        const size_t start = static_cast<size_t>(prefixBytes) + block * blockBytes;
        if (start + blockBytes > payload.size()) {
            break;
        }
        const uint8_t flag = payload[start + kDspMessageFlagByte];
        if (static_cast<uint8_t>((flag & kDspMessageTypeMask) >> kDspMessageTypeShift) ==
            static_cast<uint8_t>(type)) {
            latest = payload[start + kDspMessageValueByte];
        }
    }
    return latest;
}

} // namespace ASFW::Encoding::Motu
