// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MidasVeniceProfile.cpp
// Midas Venice F16/F24/F32 FireWire profile (DICE/TCAT).
//
// One profile for the whole Venice F range (F16 / F24 / F32). They publish one
// identity, so the variant is resolved from the device's own TX/RX sections and
// only the NAME is derived from the result (NameForGeometry).
//
// The channel constants below are a SEED, not an assertion -- they describe the
// F32 and were derived from FFADO 2.4.9 TCAT device listings
// (vendor=0x0010c73f, model=0x000001), never verified on hardware. The recorded
// F24 dump contradicts them (16 + 8 per direction, not 16 + 16), which is
// exactly the case Tx/CaptureGeometryAuthority exists to keep out of the
// conflict path.

#include "MidasVeniceProfile.hpp"
#include "../../../TimingLadder.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

namespace {


// F32 stream geometry: TWO isochronous streams per direction, each carrying 16
// PCM channels and 0 MIDI (DBS = 16) — 32×32 total. Each config below describes
// ONE wire stream, never the aggregate: the host transmit engine must match the
// device's per-stream DBS, not the sum, or the device's 16-slot RX rejects the
// CIP.
//
// These are F32 numbers and the range is not all F32s. The resolved geometry is
// what the HAL is told and what names the device; these seed the streams until
// the device has been read.
constexpr uint32_t kRxPcmChannels = 16; // per wire stream (F32: ×2 = 32 HAL in)
constexpr uint32_t kTxPcmChannels = 16; // per wire stream (F32: ×2 = 32 HAL out)
constexpr uint32_t kMidiSlots = 0;
constexpr uint32_t kRxDbs = kRxPcmChannels + kMidiSlots; // 16
constexpr uint32_t kTxDbs = kTxPcmChannels + kMidiSlots; // 16

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
    // Pre-geometry name. Identity cannot narrow this further -- see
    // NameForGeometry -- so it names the range, not a member of it. Calling
    // every Venice an F32 was how the F24 came to report itself as one.
    return "Midas Venice F (DICE)";
}

const char* MidasVeniceProfile::NameForGeometry(
    uint32_t hostInputPcmChannels,
    uint32_t /*hostOutputPcmChannels*/) const noexcept {
    // The model number IS the capture width, which is why this works at all:
    // the recorded F24 carries 16 + 8 = 24 device->host PCM channels
    // (documentation/fixtures/DICE/midasF24.txt), and an F32 carries 16 + 16.
    // Capture is the discriminator rather than playback because it is the count
    // that tracks the console's input strip count across the range.
    //
    // Anything else is left at the range name rather than rounded to the
    // nearest known variant: naming a device we cannot identify is worse than
    // admitting we cannot, and a Venice at a higher rate mode legitimately
    // carries fewer channels than its model number.
    switch (hostInputPcmChannels) {
        case 16: return "Midas Venice F16";
        case 24: return "Midas Venice F24";
        case 32: return "Midas Venice F32";
        default: return Name();
    }
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
    return TimingLadder::SafetyOffsetFrames(TimingLadder::kTxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kPerTier);
}

uint32_t MidasVeniceProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    return TimingLadder::SafetyOffsetFrames(TimingLadder::kRxDelayPackets, sampleRate,
                                            TimingLadder::RateAddend::kPerTier);
}

uint32_t MidasVeniceProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    return TimingLadder::ReportedLatencyFrames(sampleRate);
}

uint32_t MidasVeniceProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    return TimingLadder::ReportedLatencyFrames(sampleRate);
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
