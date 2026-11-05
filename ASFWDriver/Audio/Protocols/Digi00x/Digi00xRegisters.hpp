// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xRegisters.hpp - Digidesign Digi 002 / 003 register addresses, codes
// and stream geometry (issue #249). Definitions only; the sequencing belongs to
// the family driver.
//
// Behavior cross-validated with Linux sound/firewire/digi00x as a reference
// (digi00x.h:62-85 addresses; digi00x-stream.c rates :12-27, rate/clock reads
// :30-122, finish_session :125-143, begin_session :145-186;
// digi00x-transaction.c:36-46 message address). No code is copied from it.
// Every register is one big-endian quadlet unless noted.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::Audio::Digi00x {

namespace Register {
inline constexpr uint64_t kBase = 0xffffe0000000ULL;
// Read: the streaming state. Write: one step of the start/stop handshake.
inline constexpr uint64_t kStreamingState = kBase + 0x0000;
inline constexpr uint64_t kStreamingSet = kBase + 0x0004;
// Two quadlets, written together as one block: where the device sends its
// asynchronous messages (MessageAddressWords).
inline constexpr uint64_t kMessageAddress = kBase + 0x0014;
// (capture channel << 16) | playback channel; 0 releases both (ChannelWord).
inline constexpr uint64_t kIsochChannels = kBase + 0x0100;
inline constexpr uint64_t kLocalRate = kBase + 0x0110;
inline constexpr uint64_t kExternalRate = kBase + 0x0114;
inline constexpr uint64_t kClockSource = kBase + 0x0118;
inline constexpr uint64_t kOpticalMode = kBase + 0x011c;
// Non-zero while an external clock signal is detected.
inline constexpr uint64_t kDetectExternal = kBase + 0x012c;
} // namespace Register

// Rate, clock and optical codes live in the low nibble of their registers.
inline constexpr uint32_t kCodeMask = 0x0fU;

enum class RateCode : uint32_t { k44100 = 0, k48000 = 1, k88200 = 2, k96000 = 3 };
enum class ClockSource : uint32_t { kInternal = 0, kSpdif = 1, kAdat = 2, kWordClock = 3 };
enum class OpticalMode : uint32_t { kAdat = 0, kSpdif = 1 };

inline constexpr std::array<uint32_t, 4> kSupportedRatesHz{44100, 48000, 88200, 96000};

[[nodiscard]] constexpr std::optional<RateCode> RateCodeForHz(uint32_t hz) noexcept {
    for (uint32_t code = 0; code < kSupportedRatesHz.size(); ++code) {
        if (kSupportedRatesHz[code] == hz) {
            return static_cast<RateCode>(code);
        }
    }
    return std::nullopt;
}

/// Decode a local or external rate register. For the external rate, an
/// out-of-range code means the device is not locked to the external source.
[[nodiscard]] constexpr std::optional<uint32_t> DecodeRateHz(uint32_t registerValue) noexcept {
    const uint32_t code = registerValue & kCodeMask;
    if (code >= kSupportedRatesHz.size()) {
        return std::nullopt;
    }
    return kSupportedRatesHz[code];
}

[[nodiscard]] constexpr std::optional<ClockSource> DecodeClockSource(uint32_t registerValue) noexcept {
    const uint32_t code = registerValue & kCodeMask;
    if (code > static_cast<uint32_t>(ClockSource::kWordClock)) {
        return std::nullopt;
    }
    return static_cast<ClockSource>(code);
}

[[nodiscard]] constexpr bool DecodeExternalDetected(uint32_t registerValue) noexcept {
    return registerValue != 0;
}

// ---- Stream geometry ----

/// PCM channels per direction: 8 analog + 8 ADAT + 2 S/PDIF at single rate;
/// at double rate the ADAT channels are not carried.
[[nodiscard]] constexpr std::optional<uint32_t> PcmChannelsForRate(uint32_t hz) noexcept {
    switch (hz) {
        case 44100:
        case 48000:
            return 8U + 8U + 2U;
        case 88200:
        case 96000:
            return 8U + 2U;
        default:
            return std::nullopt;
    }
}

/// Data channel 0 of every data block carries MIDI; PCM follows it.
inline constexpr uint32_t kMidiDataChannels = 1;

[[nodiscard]] constexpr std::optional<uint32_t> DataBlockQuadletsForRate(uint32_t hz) noexcept {
    const auto pcm = PcmChannelsForRate(hz);
    if (!pcm) {
        return std::nullopt;
    }
    return *pcm + kMidiDataChannels;
}

// ---- Isochronous channels ----

[[nodiscard]] constexpr uint32_t ChannelWord(uint8_t captureChannel, uint8_t playbackChannel) noexcept {
    return (static_cast<uint32_t>(captureChannel & 0x3fU) << 16U) | (playbackChannel & 0x3fU);
}
inline constexpr uint32_t kReleaseChannels = 0;

// ---- Start/stop handshake ----

/// Written to kStreamingSet to stop streaming.
inline constexpr uint32_t kStreamingStop = 3;
/// After a stop the device may not transmit for a short while.
inline constexpr uint32_t kStopSettleMs = 50;
/// Delay after each start step written to kStreamingSet.
inline constexpr uint32_t kStartStepDelayMs = 20;
/// Longest start sequence StartSteps() can return.
inline constexpr uint32_t kMaxStartSteps = 3;

struct StartSteps {
    std::array<uint32_t, kMaxStartSteps> values{};
    uint32_t count{0};
    [[nodiscard]] constexpr std::span<const uint32_t> Values() const noexcept {
        return {values.data(), count};
    }
};

/// The values to write to kStreamingSet, in order, given the value read from
/// kStreamingState: count down from one below the current state to 1, where a
/// state of 0 counts as 2. A state of 1 needs no write.
///
/// Deliberate difference from Linux, which counts down from any value: states
/// above kMaxStartSteps + 1 are refused, so a garbage read cannot become
/// billions of 20 ms writes. Which states the device really reports is still
/// to be confirmed on hardware (issue #249).
[[nodiscard]] constexpr std::optional<StartSteps> StartStepsFor(uint32_t streamingState) noexcept {
    const uint32_t current = streamingState == 0 ? 2U : streamingState;
    if (current - 1U > kMaxStartSteps) {
        return std::nullopt;
    }
    StartSteps steps{};
    for (uint32_t value = current - 1U; value > 0; --value) {
        steps.values[steps.count++] = value;
    }
    return steps;
}

// ---- Device messages ----

/// The two quadlets for kMessageAddress: the host node id and the high 16 bits
/// of the 48-bit host address, then its low 32 bits.
[[nodiscard]] constexpr std::array<uint32_t, 2> MessageAddressWords(uint16_t hostNodeId,
                                                                    uint64_t hostAddress) noexcept {
    return {(static_cast<uint32_t>(hostNodeId) << 16U) |
                static_cast<uint32_t>((hostAddress >> 32U) & 0xffffU),
            static_cast<uint32_t>(hostAddress & 0xffffffffU)};
}

} // namespace ASFW::Audio::Digi00x
