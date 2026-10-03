// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Pure MOTU 828 Mk3 V3 register vocabulary and control-word construction.
// The live protocol adapter owns asynchronous I/O and lifecycle state; this
// model only preserves hardware-observed values and testable bit arithmetic.

#pragma once

#include "../AudioTypes.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace ASFW::Audio::MOTU {

inline constexpr uint16_t kRegisterAddressHi = 0xffffu;
inline constexpr uint32_t kRegisterAddressBase = 0xf0000000u;

// Cross-validated with Linux sound/firewire/motu/motu-stream.c:12-25 and
// motu-protocol-v3.c:11-32. The additional prepare registers below come from
// captured Apple-driver transactions preserved by the migration source commit.
inline constexpr uint32_t kIsocControlOffset = 0x0b00u;
inline constexpr uint32_t kStreamControlOffset = 0x0b04u;
inline constexpr uint32_t kDoorbellOffset = 0x0b08u;
inline constexpr uint32_t kPacketFormatOffset = 0x0b10u;
inline constexpr uint32_t kClockStatusOffset = 0x0b14u;
inline constexpr uint32_t kStreamConfigOffset = 0x0b1cu;
inline constexpr uint32_t kStream2ControlOffset = 0x0b38u;
inline constexpr uint32_t kRoutePortConfigOffset = 0x0c04u;
inline constexpr uint32_t kAudioBankControlOffset = 0x0c94u;

inline constexpr uint32_t kStreamControlInit = 0xffc10001u;
inline constexpr uint32_t kDoorbellArm = 0xffffffffu;
inline constexpr uint32_t kDoorbellClear = 0x00000000u;
inline constexpr uint32_t kPacketFormatS400 = 0x00000002u;
inline constexpr uint32_t kStream2ControlQ0 = 0xffc20002u;
inline constexpr uint32_t kStream2ControlQ1 = 0x00000000u;
inline constexpr uint32_t kStreamConfig48k = 0x00120000u;

inline constexpr uint32_t kFetchPcmFrames = 0x02000000u;
inline constexpr uint32_t kClockRateMask = 0x0000ff00u;
inline constexpr uint8_t kClockRateShift = 8u;

inline constexpr uint32_t kIsocStateMask = 0xffff0000u;
inline constexpr uint32_t kChangeRxIsocState = 0x80000000u;
inline constexpr uint32_t kRxIsocActivated = 0x40000000u;
inline constexpr uint8_t kRxChannelShift = 24u;
inline constexpr uint32_t kRxChannelMask = 0x3f000000u;
inline constexpr uint32_t kChangeTxIsocState = 0x00800000u;
inline constexpr uint32_t kTxIsocActivated = 0x00400000u;
inline constexpr uint8_t kTxChannelShift = 16u;
inline constexpr uint32_t kTxChannelMask = 0x003f0000u;

struct RegisterWrite final {
    uint32_t offset{0};
    std::array<uint32_t, 2> quadlets{};
    uint8_t quadletCount{0};
    bool fatalOnFailure{false};
};

// Hardware-observed cold-start writes before ISOC_COMM_CONTROL. The two state
// reads (AudioBankControl and RoutePortConfig) are diagnostics and deliberately
// absent from this write-only recipe. In particular, RoutePortConfig must not
// be overwritten during bring-up.
inline constexpr std::array<RegisterWrite, 5> kPrepareWrites{{
    {kStream2ControlOffset, {kStream2ControlQ0, kStream2ControlQ1}, 2, false},
    {kDoorbellOffset, {kDoorbellArm, 0}, 1, false},
    {kStreamControlOffset, {kStreamControlInit, 0}, 1, false},
    {kDoorbellOffset, {kDoorbellClear, 0}, 1, false},
    {kPacketFormatOffset, {kPacketFormatS400, 0}, 1, true},
}};

struct IsocControlWords final {
    uint32_t deactivate{0};
    uint32_t activate{0};
};

[[nodiscard]] constexpr std::optional<IsocControlWords>
BuildIsocControlWords(uint32_t currentControl,
                      const AudioDuplexChannels& channels) noexcept {
    if (channels.hostToDeviceIsoChannel > 0x3fu ||
        channels.deviceToHostIsoChannel > 0x3fu) {
        return std::nullopt;
    }

    // The official MOTU driver (com.motu.driver.FireWireAudio) always writes
    // the low 16 bits of ISOC_COMM_CONTROL as zero: it emits pure state+channel
    // words (e.g. activate 0xc0c00000 for channels 0/0), ignoring whatever the
    // register currently reports back. The official driver's behaviour takes
    // precedence over Linux motu-stream.c, which an earlier revision followed
    // by preserving `currentControl & ~kIsocStateMask`.
    // That divergence was invisible while devices reported a zero low half, but
    // hardware was observed reporting deviceLowHalf=0x5b59 (2026-07-23), so the
    // preserved path would have put non-zero low bits on the wire. currentControl
    // is retained in the signature (ProgramRx still reads 0x0b00) but no longer
    // feeds the written word.
    (void)currentControl;
    const uint32_t deactivate = kChangeRxIsocState | kChangeTxIsocState;
    const uint32_t activate =
        kChangeRxIsocState | kRxIsocActivated |
        ((static_cast<uint32_t>(channels.hostToDeviceIsoChannel) << kRxChannelShift) &
         kRxChannelMask) |
        kChangeTxIsocState | kTxIsocActivated |
        ((static_cast<uint32_t>(channels.deviceToHostIsoChannel) << kTxChannelShift) &
         kTxChannelMask);
    return IsocControlWords{.deactivate = deactivate, .activate = activate};
}

[[nodiscard]] constexpr uint32_t BuildStopIsocControl(uint32_t currentControl) noexcept {
    // Linux sound/firewire/motu/motu-stream.c:85-106 keeps the assigned
    // channels while clearing only both activation bits. This intentionally
    // differs from the captured pre-start deactivate word, which clears the
    // channel fields before assigning fresh IRM channels.
    return (currentControl & ~(kRxIsocActivated | kTxIsocActivated)) |
        kChangeRxIsocState | kChangeTxIsocState;
}

[[nodiscard]] constexpr uint32_t SetFetchPcmFrames(uint32_t clockStatus) noexcept {
    return clockStatus | kFetchPcmFrames;
}

[[nodiscard]] constexpr uint32_t ClearFetchPcmFrames(uint32_t clockStatus) noexcept {
    return clockStatus & ~kFetchPcmFrames;
}

[[nodiscard]] constexpr std::optional<uint32_t>
SampleRateFromClockStatus(uint32_t clockStatus) noexcept {
    // Linux sound/firewire/motu/motu.c:16-26 and
    // motu-protocol-v3.c:37-56 use this same six-entry index.
    switch ((clockStatus & kClockRateMask) >> kClockRateShift) {
        case 0: return 44100u;
        case 1: return 48000u;
        case 2: return 88200u;
        case 3: return 96000u;
        case 4: return 176400u;
        case 5: return 192000u;
        default: return std::nullopt;
    }
}

// Only the 48 kHz value is currently backed by a captured official-driver
// transaction. Other rates remain unavailable until their register values are
// promoted with equivalent evidence.
[[nodiscard]] constexpr std::optional<uint32_t>
StreamConfigForSampleRate(uint32_t sampleRateHz) noexcept {
    return sampleRateHz == 48000u
        ? std::optional<uint32_t>{kStreamConfig48k}
        : std::nullopt;
}

} // namespace ASFW::Audio::MOTU
