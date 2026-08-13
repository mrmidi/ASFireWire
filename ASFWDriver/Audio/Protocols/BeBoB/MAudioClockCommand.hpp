// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MAudioClockCommand.hpp — the vendor command that tells M-Audio "special"
// firmware which clock to run on and which digital formats are selected.
//
// Nothing else can configure these devices. Linux calls it the very first thing
// in discovery and treats failure as fatal, under the comment "initialize these
// parameters because driver is not allowed to ask"
// (bebob_maudio.c:273-281). Without it the device is never told to clock
// internally, and a device that is not clocked does not transmit — which is
// exactly what a silent IR context looks like.
//
// Closed by construction, like BeBoBBootloaderCue and AVCSignalFormatProbe: the
// company ID has no parameter. That is the whole safety boundary here. BridgeCo's
// own extensions are also vendor-dependent, also opcode 0x00, and freeze this
// firmware; only bytes 3..5 separate this command from the ones that hang it.
//
//   references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:171-198
//     avc_maudio_set_special_clk() — byte-identical frame
//   vendor kext com_m_audio_FW1814Device::SetClockSourceInternal (0xe25c)
//     builds the same 6-byte prefix

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ASFW::Audio::BeBoB {

inline constexpr size_t kMAudioClockCommandBytes = 12;

/// Clock source selector. These are indices into the device's own list, not a
/// bitfield — Linux exposes them as an enum whose labels name each one
/// (bebob_maudio.c:343-364).
enum class MAudioClockSource : uint8_t {
    /// Internal, with the digital outputs muted. What the vendor kext selects
    /// in SetBlankSlateClockSource.
    InternalDigitalMute = 0,
    /// S/PDIF or ADAT, whichever dig_in_fmt selects.
    Digital = 1,
    WordClock = 2,
    /// Plain internal. What Linux selects at discovery, and what a host-clocked
    /// playback path wants.
    Internal = 3,
};

/// Digital interface format for one direction. Same wire values as
/// MAudioDigitalFormat in MAudioSpecialFormation.hpp; kept separate here so this
/// header stays a pure frame builder.
enum class MAudioClockDigitalFormat : uint8_t {
    SPDIF = 0,
    ADAT = 1,
};

/// Builds the CONTROL frame. Every caller-supplied value is an operand — none of
/// them can become an opcode or a company ID.
[[nodiscard]] constexpr std::array<uint8_t, kMAudioClockCommandBytes>
BuildMAudioClockCommand(MAudioClockSource source,
                        MAudioClockDigitalFormat captureFormat,
                        MAudioClockDigitalFormat playbackFormat,
                        bool lockSettings) noexcept {
    return {
        0x00,  // AV/C CONTROL
        0xFF,  // unit
        0x00,  // VENDOR DEPENDENT
        0x04,  // company ID high   — pinned; this is the safety boundary
        0x00,  // company ID middle
        0x04,  // company ID low
        static_cast<uint8_t>(source),
        static_cast<uint8_t>(captureFormat),   // dig_in_fmt  -> selects capture geometry
        static_cast<uint8_t>(playbackFormat),  // dig_out_fmt -> selects playback geometry
        lockSettings ? uint8_t{0x01} : uint8_t{0x00},
        0x00,  // padding, zeroed by Linux and pinned by the command filter
        0x00,
    };
}

/// How long to wait after the command before doing anything else with the device.
///
/// 2500 ms, from the vendor kext's SetBlankSlateClockSource, which issues the
/// clock command and then IOSleep(0x9C4). This is *not* the 300 ms that appears
/// around SetClockSourceInternal — that is the user-driven clock-change path,
/// and initialisation waits over eight times longer.
///
/// The size of this number is why the command belongs at install time and not in
/// the stream-start path: 2500 ms plus the signal-format interlock plus the
/// device's own ~1 s transmission delay would exceed the 4 s initial-ZTS budget
/// on its own, and a working device would look like a hung one.
inline constexpr uint32_t kMAudioClockSettleMs = 2500;

} // namespace ASFW::Audio::BeBoB
