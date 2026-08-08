// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AlesisMultiMixProfile.cpp
// Alesis MultiMix 8/12/16 FireWire profile (DICE/TCAT).
//
// The MultiMix 8, 12 and 16 FireWire share ONE vendor/model pair
// (0x000595 / 0x000000) — the model ID does not distinguish them. libffado
// records the same collision (configuration:622-628, "The MultiMix-16 and
// MultiMix-12 share the same vendor/model IDs"), and the ALSA userspace TCAT
// tree labels the pair "Alesis MultiMix 8/12/16 FireWire"
// (snd-firewire-ctl-services runtime/dice/src/model.rs:140).
//
// Consequence: the channel counts below are only a pre-caps seed. The real
// per-variant geometry is read back from the device's TX/RX stream-format
// registers and overwrites inputChannelCount/outputChannelCount via
// ApplyDiceRuntimeCapsToDeviceConfig (DiceRuntimeDeviceConfig.hpp:26-27).
// Both reference stacks do the same thing — neither hardcodes MultiMix
// geometry: Linux has no dice.c match entry for model 0x000000 at all (only
// iO14/iO26 at 0x000001 and MasterControl at 0x000002 in dice-alesis.c), so it
// takes the generic DICE format-detection path; libffado reads NB_TX/NB_RX from
// the device (dice_avdevice.cpp:1661-1680).
//
// UNVERIFIED ON HARDWARE: the seed counts (16 in / 2 out) are the MultiMix-16
// front-panel geometry and are NOT sourced from any reference stack — no
// reference publishes MultiMix channel counts because they all read them from
// the device. They exist so the nub can be published before runtime caps load;
// confirm against a TX/RX register dump when hardware is available.

#include "AlesisMultiMixProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

namespace {

constexpr uint32_t kAlesisVendorId        = 0x000595;
constexpr uint32_t kAlesisMultiMixModelId = 0x000000;

// Seed geometry only — see the UNVERIFIED note above.
//
// Direction naming here is ASFW's, which is the INVERSE of the DICE register
// map's: ASFW Tx = host-to-device = playback = the device's DICE "RX" section;
// ASFW Rx = device-to-host = capture = the device's DICE "TX" section
// (DiceAudioBackend.cpp:594-595 maps RxChannelCount to inputChannelCount).
// Do not transcribe DICE register names into these fields without flipping.
constexpr uint32_t kRxPcmChannels = 16; // capture, device -> host
constexpr uint32_t kTxPcmChannels = 2;  // playback return, host -> device
constexpr uint32_t kMidiSlots     = 0;
constexpr uint32_t kRxDbs         = kRxPcmChannels + kMidiSlots;
constexpr uint32_t kTxDbs         = kTxPcmChannels + kMidiSlots;

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

const char* AlesisMultiMixProfile::Name() const noexcept {
    return "Alesis MultiMix FireWire (DICE)";
}

bool AlesisMultiMixProfile::Matches(const DiceDeviceIdentity& identity) const noexcept {
    // Exact vendor+model. The Alesis OUI also covers the iO14/iO26 (0x000001)
    // and MasterControl (0x000002), which have different stream geometry and
    // their own format detection in Linux (dice-alesis.c) — they must not fall
    // into this profile.
    return identity.vendorId == kAlesisVendorId &&
           identity.modelId == kAlesisMultiMixModelId;
}

DiceDeviceQuirks AlesisMultiMixProfile::Quirks() const noexcept {
    DiceDeviceQuirks quirks{};
    // Same TCAT DICE chip family as Focusrite Saffire / Midas Venice / PreSonus
    // StudioLive — same wire encoding. Confirmed from the vendor kext, which is
    // TC Applied Technologies' DICE reference driver shipped under the Alesis
    // name (CFBundleIdentifier tc.tctechnologies.driver.AlesisFirewire; the
    // binary carries tcat::dice symbols and the same FillFirewireBuffers /
    // ReadFirewireBuffers isoch core as Focusrite's Saffire.kext).
    quirks.tx.hostToDevicePcmEncoding  = Encoding::AudioWireFormat::kRawPcm24In32;
    quirks.tx.dbsPolicy                = DbsPolicy::Constant;
    // The MultiMix has no MIDI I/O, so its host-to-device data block is pure PCM
    // (DBS == pcmChannels) and there is no non-audio slot to seed. The 0x80000000
    // MIDI-slot fill the other TCAT profiles carry is therefore left OFF rather
    // than set-but-inert: AmdtpTxPacketizer.cpp:293 gates the fill on
    // `dbs > pcmChannels`, and because the TX wire config is built from this
    // profile at StartIO (ASFWAudioDevice.cpp:167) — runtime caps correct only the
    // HAL channel counts, not the wire geometry — that gate can never open here.
    //
    // If hardware verification shows a non-audio slot after all, BOTH must change
    // together: raise midiSlots (so dbs > pcmChannels) AND set
    // initializeNonAudioSlots = true with defaultNonAudioSlotWord = 0x80000000,
    // which is the TCAT-family behaviour the Saffire requires.
    quirks.tx.initializeNonAudioSlots  = false;
    quirks.tx.preserveFdfInNoDataPackets = true;
    quirks.rx.deviceToHostPcmEncoding  = Encoding::AudioWireFormat::kAM824;
    quirks.rx.dbsPolicy                = DbsPolicy::Constant;
    return quirks;
}

bool AlesisMultiMixProfile::BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::HostToDevice);
    return true;
}

bool AlesisMultiMixProfile::BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, DiceStreamDirection::DeviceToHost);
    return true;
}

// Safety offsets follow the Focusrite Saffire baseline (the tested TCAT ladder):
// Tx 6 packets, Rx 16 packets, scaled by frames-per-packet per rate mode.
uint32_t AlesisMultiMixProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
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

uint32_t AlesisMultiMixProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
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

uint32_t AlesisMultiMixProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    if (sampleRate > 96000.0) return 119;
    if (sampleRate > 48000.0) return 59;
    return 29;
}

uint32_t AlesisMultiMixProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    if (sampleRate > 96000.0) return 119;
    if (sampleRate > 48000.0) return 59;
    return 29;
}

} // namespace ASFW::Isoch::Audio::DICE::Profiles
