// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MAudioSpecialFormation.hpp — stream geometry for M-Audio "special" firmware.
//
// The FireWire 1814 and ProjectMix I/O cannot be asked how many channels they
// carry: PLUG_INFO and the BridgeCo extensions that would answer are exactly the
// commands that hang this firmware (AVC_DEVICE_HAZARDS.md H1, H5). Linux states
// the consequence plainly — *"initialize these parameters because driver is not
// allowed to ask"* (bebob_maudio.c:273).
//
// So this table is not a cache of anything observed. It is the geometry, and if
// it is wrong the stream shape is silently wrong on the wire with no error
// anywhere. Transcribed from:
//
//   references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:227-254
//     special_stream_formation_set(), ch_table[direction][dig_fmt][rate_band]
//
// and independently confirmed against the vendor kext's twelve ch_table entries
// during the 1814 RE pass.
//
// Two things about the indexing are easy to get wrong:
//
//  - **Capture and playback are selected independently.** Capture uses
//    dig_in_fmt, playback uses dig_out_fmt, so S/PDIF-in with ADAT-out is a
//    legal combination and this is not a single "mode" per device.
//  - **The third index is a rate *band*, not a rate.** Linux writes
//    `ch_table[...][i / 2]` over formation entries 1..6, which pairs
//    44.1/48, 88.2/96, and 176.4/192.

#pragma once

#include <cstdint>
#include <optional>

namespace ASFW::Audio::BeBoB {

/// Digital interface selection. These values are the wire encoding of
/// dig_in_fmt / dig_out_fmt in the M-Audio vendor clock command — never reorder.
enum class MAudioDigitalFormat : uint8_t {
    SPDIF = 0,
    ADAT = 1,
};

/// PCM channel counts for one rate band, plus the MIDI data blocks that ride
/// alongside them. DBS is PCM + MIDI, not PCM alone.
struct MAudioStreamFormation final {
    uint32_t capturePcmChannels{0};   ///< device -> host
    uint32_t playbackPcmChannels{0};  ///< host -> device
    uint32_t midiDataBlocks{0};
};

/// Every rate the 1814 supports, in the order Linux's formation entries 1..6
/// use. ProjectMix I/O stops after 96 kHz — Linux expresses that as `max -= 2`
/// on the same table rather than a different table.
///   protocols/bebob/src/maudio/special.rs:73 (Fw1814ClkProtocol::FREQ_LIST)
///   protocols/bebob/src/maudio/special.rs:89 (ProjectMixClkProtocol::FREQ_LIST)
inline constexpr uint32_t kMAudioSpecialRatesHz[] = {
    44100, 48000, 88200, 96000, 176400, 192000,
};
inline constexpr size_t kMAudioFireWire1814RateCount = 6;
inline constexpr size_t kMAudioProjectMixRateCount = 4;

/// Maps a rate onto its band index. Bands pair adjacent rates exactly as
/// Linux's `i / 2` does.
[[nodiscard]] constexpr std::optional<size_t> MAudioRateBand(uint32_t rateHz) noexcept {
    switch (rateHz) {
        case 44100:
        case 48000:
            return 0;
        case 88200:
        case 96000:
            return 1;
        case 176400:
        case 192000:
            return 2;
        default:
            return std::nullopt;
    }
}

/// Resolves the stream formation for a rate and the two independently selected
/// digital formats. Returns nullopt for a rate this firmware family never runs.
[[nodiscard]] constexpr std::optional<MAudioStreamFormation> MAudioFormationFor(
    MAudioDigitalFormat captureFormat,
    MAudioDigitalFormat playbackFormat,
    uint32_t rateHz) noexcept {
    // ch_table[AMDTP_IN_STREAM][dig_in_fmt][band] — device -> host.
    constexpr uint32_t kCapture[2][3] = {
        {10, 10, 2},  // S/PDIF
        {16, 12, 2},  // ADAT
    };
    // ch_table[AMDTP_OUT_STREAM][dig_out_fmt][band] — host -> device.
    constexpr uint32_t kPlayback[2][3] = {
        {6, 6, 4},   // S/PDIF
        {12, 8, 4},  // ADAT
    };
    // `bebob->tx_stream_formations[i + 1].midi = 1` and likewise for rx: one
    // AM824 conformant-data block per direction at every rate, which multiplexes
    // the unit's MIDI ports. bebob_maudio.c:249,252.
    constexpr uint32_t kMidiDataBlocks = 1;

    const auto band = MAudioRateBand(rateHz);
    if (!band) {
        return std::nullopt;
    }
    return MAudioStreamFormation{
        .capturePcmChannels = kCapture[static_cast<size_t>(captureFormat)][*band],
        .playbackPcmChannels = kPlayback[static_cast<size_t>(playbackFormat)][*band],
        .midiDataBlocks = kMidiDataBlocks,
    };
}

} // namespace ASFW::Audio::BeBoB
