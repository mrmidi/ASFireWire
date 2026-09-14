// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuRegisterDsp.hpp - Output-volume encoding, and the DSP state messages that v2
// "register DSP" models interleave into their capture stream.
//
// Output volume. Registers 0x0c0c (main) and 0x0c10 (phones) hold 0..0x80, and that value
// is **linear in amplitude, not in dB**: gain = raw / 0x80, so
//
//     dB = 20 * log10(raw / 128)        0x80 = 0 dB, 0x40 = -6 dB, 0x08 = -24 dB, 0 = off
//
// snd-firewire-ctl-services publishes it as an ALSA DB_LINEAR interval
// (runtime/motu/src/register_dsp_ctls.rs:841-846), and DB_LINEAR is defined as "the value
// increases linearly, convert with 20 * log10(current / (maximum - minimum))"
// (protocols/alsa-ctl-tlv-codec/src/items.rs:123-128). Reading it as 0.5 dB per step --
// which the -6400..0 endpoints invite -- makes every level far louder than asked for:
// raw 8 is -24 dB, not -60 dB. The register takes the bare value, as ctl-services writes
// it; FFADO additionally sets bit 24 (motu_controls.cpp:476-481).
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

namespace detail {

/// 2^y. DriverKit ships no libm, so the integer part comes from the exponent field and the
/// fraction from a Taylor series for e^u over u in [0, ln 2).
[[nodiscard]] constexpr float Exp2(float y) noexcept {
    if (!(y > -126.0f)) { // also catches NaN
        return 0.0f;
    }
    if (y >= 127.0f) {
        return 3.4028235e38f;
    }
    auto n = static_cast<int32_t>(y);
    float fraction = y - static_cast<float>(n);
    if (fraction < 0.0f) { // truncation is not floor for negatives
        fraction += 1.0f;
        --n;
    }
    const float u = fraction * 0.6931472f; // ln 2
    float term = 1.0f;
    float sum = 1.0f;
    for (int i = 1; i <= 8; ++i) {
        term *= u / static_cast<float>(i);
        sum += term;
    }
    const auto exponentBits = static_cast<uint32_t>(127 + n) << 23U;
    return sum * __builtin_bit_cast(float, exponentBits);
}

/// log2(x) for x > 0: the exponent comes from the bits, the mantissa from the atanh series
/// log2(m) = (2 / ln 2) * (t + t^3/3 + t^5/5 + ...), t = (m - 1) / (m + 1), |t| <= 1/3.
[[nodiscard]] constexpr float Log2(float x) noexcept {
    if (!(x > 0.0f)) {
        return -126.0f;
    }
    const uint32_t bits = __builtin_bit_cast(uint32_t, x);
    const auto exponent = static_cast<int32_t>((bits >> 23U) & 0xFFU) - 127;
    const uint32_t mantissaBits = (bits & 0x807FFFFFU) | (127U << 23U);
    const float m = __builtin_bit_cast(float, mantissaBits);
    const float t = (m - 1.0f) / (m + 1.0f);
    const float t2 = t * t;
    const float series =
        t * (1.0f + t2 * (0.33333333f + t2 * (0.2f + t2 * (0.14285714f + t2 * 0.11111111f))));
    return static_cast<float>(exponent) + series * 2.8853901f; // 2 / ln 2
}

inline constexpr float kDbPerOctave = 6.0205999f;  // 20 * log10(2)

} // namespace detail

/// dB of the quietest audible step (raw 1). Raw 0 is off; the control publishes this as its
/// floor because a level control cannot express -inf.
inline constexpr float kOutputVolumeMinDb = -42.1442f;
inline constexpr float kOutputVolumeMaxDb = 0.0f;

[[nodiscard]] constexpr float OutputVolumeToDb(uint8_t raw) noexcept {
    const uint8_t clamped = raw > kOutputVolumeMaxRaw ? kOutputVolumeMaxRaw : raw;
    if (clamped == 0) {
        return kOutputVolumeMinDb;
    }
    return detail::kDbPerOctave *
           detail::Log2(static_cast<float>(clamped) / static_cast<float>(kOutputVolumeMaxRaw));
}

/// Anything quieter than half a step below raw 1 turns the register off, which is how mute
/// reaches true silence.
[[nodiscard]] constexpr uint8_t OutputVolumeFromDb(float decibels) noexcept {
    if (!(decibels > -1000.0f)) { // also catches NaN
        return 0;
    }
    if (decibels >= kOutputVolumeMaxDb) {
        return kOutputVolumeMaxRaw;
    }
    const float scaled = static_cast<float>(kOutputVolumeMaxRaw) *
                             detail::Exp2(decibels / detail::kDbPerOctave) +
                         0.5f;
    if (scaled < 1.0f) {
        return 0;
    }
    if (scaled >= static_cast<float>(kOutputVolumeMaxRaw)) {
        return kOutputVolumeMaxRaw;
    }
    return static_cast<uint8_t>(scaled);
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
