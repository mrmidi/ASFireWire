// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DotScrambler.hpp - the "double-oh-three" transform a Digidesign Digi 002 /
// 003 applies to host-to-device PCM (issue #249).
//
// The device expects every playback PCM quadlet's middle sample byte (wire
// byte 2 of the AM824 quadlet [label][hi][mid][lo]) XORed with a running salt.
// Capture is not scrambled, and the MIDI data channel is not scrambled.
//
// The salt sequence was reverse-engineered by Robin Gareus and Damien Zammit
// (2012, on a Digi 003 Rack). The tables below are that wire contract; their
// values are cross-validated against Linux sound/firewire/digi00x/amdtp-dot.c
// (dot_scrt :62-99, dot_encode_step :101-110, use in write_pcm_s32 :145-171)
// as a behavioral reference. No code is copied from it.
//
// State runs across every scrambled quadlet in transmission order (channel by
// channel, block by block, packet by packet). Reset it when a stream starts.

#pragma once

#include <array>
#include <cstdint>

namespace ASFW::Encoding::Digi00x {

/// Wire byte, within one AM824 quadlet in bus order, that carries the salt.
inline constexpr uint32_t kDotScrambledByte = 2;

class DotScrambler final {
public:
    /// Salt for a key byte and the distance (in scrambled quadlets) since that
    /// key byte. Zero once the key's pattern is exhausted.
    [[nodiscard]] static constexpr uint8_t Salt(uint8_t key, uint32_t distance) noexcept {
        const uint8_t low = key & 0x0FU;
        const uint8_t high = key >> 4U;
        const uint8_t length = kPatternLength[low];
        if (distance > length) {
            return 0;
        }
        // The low nibble walks backwards through kLowNibbles and ends on its
        // last entry when distance == length.
        const uint8_t lowSalt = kLowNibbles[14U + distance - length];
        // A key high nibble of 9 always yields 9; every other one steps
        // through the high-nibble cycle from its own start.
        const uint8_t highSalt =
            high == 0x9U ? uint8_t{0x9U}
                         : kHighNibbleCycle[(kHighNibbleStart[high] + distance) % 15U];
        return static_cast<uint8_t>((highSalt << 4U) | lowSalt);
    }

    constexpr void Reset() noexcept { *this = DotScrambler{}; }

    /// Scramble one byte (the middle sample byte of the next PCM quadlet) and
    /// advance the state.
    [[nodiscard]] constexpr uint8_t Next(uint8_t byte) noexcept {
        if (byte != 0) {
            // A non-zero byte starts a new pattern, keyed on its scrambled value.
            key_ = static_cast<uint8_t>(byte ^ carry_);
            distance_ = 0;
        }
        const auto out = static_cast<uint8_t>(byte ^ carry_);
        // Every distance beyond the longest pattern gives a zero salt, so
        // saturating keeps the arithmetic bounded on long silences.
        if (distance_ <= kMaxPatternLength) {
            ++distance_;
        }
        carry_ = Salt(key_, distance_);
        return out;
    }

    /// Scramble one AM824 quadlet held in bus (big-endian) byte order.
    constexpr void ScrambleQuadlet(std::array<uint8_t, 4>& quadletBE) noexcept {
        quadletBE[kDotScrambledByte] = Next(quadletBE[kDotScrambledByte]);
    }

private:
    // Indexed by the key's low nibble: how many quadlets its pattern lasts.
    static constexpr std::array<uint8_t, 16> kPatternLength{
        0, 1, 3, 5, 7, 9, 11, 13, 14, 12, 10, 8, 6, 4, 2, 0};
    static constexpr uint8_t kMaxPatternLength = 14;
    // Low salt nibbles, read backwards from the end by kPatternLength.
    static constexpr std::array<uint8_t, 15> kLowNibbles{
        0x8, 0x7, 0x9, 0x6, 0xA, 0x5, 0xB, 0x4, 0xC, 0x3, 0xD, 0x2, 0xE, 0x1, 0xF};
    // High salt nibbles, a cycle of 15 entered at kHighNibbleStart[key high].
    static constexpr std::array<uint8_t, 15> kHighNibbleCycle{
        0x0, 0x6, 0xF, 0x8, 0x7, 0x5, 0x3, 0x4, 0xC, 0xD, 0xE, 0x1, 0x2, 0xB, 0xA};
    // Entry 9 is unused: a key high nibble of 9 bypasses the cycle.
    static constexpr std::array<uint8_t, 16> kHighNibbleStart{
        0, 11, 12, 6, 7, 5, 1, 4, 3, 0, 14, 13, 8, 9, 10, 2};

    uint8_t carry_{0};
    uint8_t key_{0};
    uint32_t distance_{0};
};

} // namespace ASFW::Encoding::Digi00x
