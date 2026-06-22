// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// MidasVeniceProfile.cpp
// Midas Venice F32 FireWire profile (DICE/TCAT).
//
// Venice F32: 32 analog inputs, 32 playback returns over FireWire.
// Channel counts need hardware verification — these are initial values derived
// from FFADO 2.4.9 TCAT device listings (vendor=0x0010c73f, model=0x000001).

#include "MidasVeniceProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

namespace {

constexpr uint32_t kMidasVendorId      = 0x10c73f;
constexpr uint32_t kMidasVeniceModelId = 0x000001;

// Venice F32 stream geometry verified at runtime from the TCAT TX/RX STREAM
// FORMAT registers at 48 kHz: a SINGLE isochronous stream per direction carrying
// 32 PCM channels, 0 MIDI (DBS = 32). The device reports RX_NUMBER/TX_NUMBER = 1
// with am824Slots = 32 (DICE DUPLEX START: inCh=32 inSlots=32, streams=1). Earlier
// values (33 = +MIDI, 16 = half) did not match the device's RX format, so the
// host CIP never locked the device and no hardware ZTS was produced.
// (At 2x sample rates DICE may split into multiple streams; that path is handled
// generically by the per-stream bringup/transport but is untested on this device.)
constexpr uint32_t kRxPcmChannels  = 32;
constexpr uint32_t kTxPcmChannels  = 32;
constexpr uint32_t kMidiSlots      = 0;
constexpr uint32_t kRxDbs          = kRxPcmChannels + kMidiSlots;
constexpr uint32_t kTxDbs          = kTxPcmChannels + kMidiSlots;

void FillStreamConfig(DiceStreamConfig& out, DiceStreamDirection direction) noexcept {
    out = DiceStreamConfig{};
    out.direction    = direction;
    out.sampleRate   = 48000;
    out.streamMode   = Encoding::StreamMode::kBlocking;
    out.sid          = 0;
    out.framesPerDataPacket = 8;
    out.fdf          = 0x02;
    out.fmt          = 0x10;

    if (direction == DiceStreamDirection::HostToDevice) {
        out.pcmChannels = kTxPcmChannels;
        out.midiSlots   = kMidiSlots;
        out.dbs         = kTxDbs;
    } else {
        out.pcmChannels = kRxPcmChannels;
        out.midiSlots   = kMidiSlots;
        out.dbs         = kRxDbs;
    }
}

} // namespace

const char* MidasVeniceProfile::Name() const noexcept {
    return "Midas Venice F32 (DICE)";
}

bool MidasVeniceProfile::Matches(const DiceDeviceIdentity& identity) const noexcept {
    return identity.vendorId == kMidasVendorId && identity.modelId == kMidasVeniceModelId;
}

DiceDeviceQuirks MidasVeniceProfile::Quirks() const noexcept {
    DiceDeviceQuirks quirks{};
    // Same TCAT DICE chip family as Focusrite Saffire — same wire encoding.
    quirks.tx.hostToDevicePcmEncoding  = Encoding::AudioWireFormat::kRawPcm24In32;
    quirks.tx.dbsPolicy                = DbsPolicy::Constant;
    quirks.tx.defaultNonAudioSlotWord  = 0x80000000;
    quirks.tx.initializeNonAudioSlots  = true;
    quirks.tx.preserveFdfInNoDataPackets = true;
    quirks.rx.deviceToHostPcmEncoding  = Encoding::AudioWireFormat::kAM824;
    quirks.rx.dbsPolicy                = DbsPolicy::Constant;
    return quirks;
}

bool MidasVeniceProfile::BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::HostToDevice);
    return true;
}

bool MidasVeniceProfile::BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::DeviceToHost);
    return true;
}

uint32_t MidasVeniceProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
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

uint32_t MidasVeniceProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
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

uint32_t MidasVeniceProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    if (sampleRate > 96000.0) return 119;
    if (sampleRate > 48000.0) return 59;
    return 29;
}

uint32_t MidasVeniceProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    if (sampleRate > 96000.0) return 119;
    if (sampleRate > 48000.0) return 59;
    return 29;
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
