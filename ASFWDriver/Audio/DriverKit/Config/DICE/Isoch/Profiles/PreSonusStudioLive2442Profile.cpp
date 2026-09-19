// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// PreSonusStudioLive2442Profile.cpp
// PreSonus StudioLive 24.4.2 FireWire profile (DICE/TCAT).
//
// Stream geometry captured live from hardware and contributed in issue #115
// (2026-09-14): Mac mini M1 on macOS 26.6.2, device locked at 48 kHz, GUID
// 0x000A9204049204CB, TCAT vendor 0x000A92 / category 0x04 (standard DICE) /
// product 0x012, firmware 1.0.4.0. The GUID is per-unit (it carries the serial),
// so Matches() keys on the Config ROM model_id, which the device populates
// correctly — no Focusrite-style GUID decoding is needed here.
//
// TWO isochronous streams per direction, and unlike the Venice F32 the playback
// side is ASYMMETRIC:
//
//   capture  (device -> host, DICE TX): 16 ch ['Ch 1'..'Ch 16']
//                                     + 16 ch ['Ch 17'..'Ch 24',
//                                              'Auxiliary Ch 25'..'Ch 32']  = 32
//   playback (host -> device, DICE RX): 16 ch ['Ch 1'..'Ch 16']
//                                     + 10 ch ['Ch 17'..'Ch 24',
//                                              '2TrackIn L', '2TrackIn R']  = 26
//
// MIDI ports are 0 in every stream, so DBS equals the PCM channel count. That
// differs from the 16.0.2, which muxes one MIDI slot and therefore runs DBS 17.
//
// The asymmetry is why this profile overrides BuildTxStreamConfig and
// TxChannelCount: the uniform defaults would describe playback as 16+16 and emit
// 16-slot CIP into a device RX stream that only has 10 slots, which the device
// rejects — the failure mode is silence with no error, since the transport
// reserves bandwidth from the device-reported caps while the packetizer frames
// from this profile, and nothing cross-checks the two.
//
// No extension (EAP) space at 0xFFFFE0200000, corroborated by libffado 2.5.0,
// whose StudioLive entries carry a plain `driver = "DICE"` with no `mixer` field
// (unlike the FireStudios' "Generic_Dice_EAP"). Linux snd-dice has PreSonus
// handling only for model 0x000008 (dice-presonus.c), so every StudioLive takes
// the fully generic DICE path. Wire encoding therefore matches the 16.0.2,
// Saffire and Venice — same TCAT chip family.
//
// CLOCK_CAPABILITIES (0x13000006) advertises 44.1/48 kHz only, so the higher
// rate branches below are defensive.
//
// NOT hardware-verified: this profile has never been run against the device.
// The capture is a register dump, not a streaming test.

#include "PreSonusStudioLive2442Profile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

namespace {


// Per wire stream. MIDI is 0 in every stream, so DBS == PCM channels.
constexpr uint8_t kCapturePcmChannels     = 16;  // both capture streams
constexpr uint8_t kPlaybackPcmChannels0   = 16;  // playback stream 0
constexpr uint8_t kPlaybackPcmChannels1   = 10;  // playback stream 1
constexpr uint8_t kMidiSlots              = 0;

void FillStreamConfig(DiceStreamConfig& out,
                      DiceStreamDirection direction,
                      uint8_t pcmChannels) noexcept {
    out = DiceStreamConfig{};
    out.direction           = direction;
    out.sampleRate          = 48000;
    out.streamMode          = Encoding::StreamMode::kBlocking;
    out.sid                 = 0;
    out.framesPerDataPacket = 8;
    out.fdf                 = 0x02;
    out.fmt                 = 0x10;
    out.pcmChannels         = pcmChannels;
    out.midiSlots           = kMidiSlots;
    out.dbs                 = static_cast<uint8_t>(pcmChannels + kMidiSlots);
}

} // namespace

const char* PreSonusStudioLive2442Profile::Name() const noexcept {
    return "PreSonus StudioLive 24.4.2 (DICE)";
}

DiceDeviceQuirks PreSonusStudioLive2442Profile::Quirks() const noexcept {
    DiceDeviceQuirks quirks{};
    // Same TCAT DICE chip family as the StudioLive 16.0.2, Focusrite Saffire and
    // Midas Venice — same wire encoding, and no model-specific quirk in either
    // reference stack.
    quirks.tx.hostToDevicePcmEncoding    = Encoding::AudioWireFormat::kRawPcm24In32;
    quirks.tx.dbsPolicy                  = DbsPolicy::Constant;
    quirks.tx.defaultNonAudioSlotWord    = 0x80000000;
    quirks.tx.initializeNonAudioSlots    = true;
    quirks.tx.preserveFdfInNoDataPackets = true;
    quirks.rx.deviceToHostPcmEncoding    = Encoding::AudioWireFormat::kAM824;
    quirks.rx.dbsPolicy                  = DbsPolicy::Constant;
    return quirks;
}

bool PreSonusStudioLive2442Profile::BuildDefaultTxStreamConfig(
    DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::HostToDevice, kPlaybackPcmChannels0);
    return true;
}

bool PreSonusStudioLive2442Profile::BuildDefaultRxStreamConfig(
    DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::DeviceToHost, kCapturePcmChannels);
    return true;
}

bool PreSonusStudioLive2442Profile::BuildTxStreamConfig(
    uint32_t streamIndex, AudioStreamConfig& outConfig) const noexcept {
    if (streamIndex >= TxStreamCount()) {
        return false;
    }
    const uint8_t pcmChannels =
        (streamIndex == 0) ? kPlaybackPcmChannels0 : kPlaybackPcmChannels1;
    FillStreamConfig(outConfig, DiceStreamDirection::HostToDevice, pcmChannels);
    // Stream 1 carries the host channels that follow stream 0's block.
    outConfig.sourceChannelOffset =
        (streamIndex == 0) ? 0 : kPlaybackPcmChannels0;
    return true;
}

uint32_t PreSonusStudioLive2442Profile::TxChannelCount() const noexcept {
    return static_cast<uint32_t>(kPlaybackPcmChannels0) +
           static_cast<uint32_t>(kPlaybackPcmChannels1);
}

// Safety offsets follow the Focusrite Saffire baseline (the tested TCAT ladder),
// matching the StudioLive 16.0.2: Tx 6 packets, Rx 16 packets, scaled by
// frames-per-packet per rate mode. The device only advertises the low rate mode
// (8 frames per packet), so the higher branches are defensive.
uint32_t PreSonusStudioLive2442Profile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    uint32_t framesPerPacket = 8;
    uint32_t rateAddend = 0;
    if (sampleRate > 96000.0) {
        framesPerPacket = 32;
        rateAddend = 4;
    } else if (sampleRate > 48000.0) {
        framesPerPacket = 16;
        rateAddend = 2;
    }
    return (6 + rateAddend) * framesPerPacket;
}

uint32_t PreSonusStudioLive2442Profile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    uint32_t framesPerPacket = 8;
    uint32_t rateAddend = 0;
    if (sampleRate > 96000.0) {
        framesPerPacket = 32;
        rateAddend = 4;
    } else if (sampleRate > 48000.0) {
        framesPerPacket = 16;
        rateAddend = 2;
    }
    return (16 + rateAddend) * framesPerPacket;
}

uint32_t PreSonusStudioLive2442Profile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    if (sampleRate > 96000.0) {
        return 119;
    }
    if (sampleRate > 48000.0) {
        return 59;
    }
    return 29;
}

uint32_t PreSonusStudioLive2442Profile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    if (sampleRate > 96000.0) {
        return 119;
    }
    if (sampleRate > 48000.0) {
        return 59;
    }
    return 29;
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
