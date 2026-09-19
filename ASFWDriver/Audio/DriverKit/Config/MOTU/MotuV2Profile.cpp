// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuV2Profile.cpp - see MotuV2Profile.hpp.

#include "MotuV2Profile.hpp"

#include "../../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../../Wire/MOTU/MotuBlockLayout.hpp"
#include "../../../Wire/MOTU/MotuPortLayout.hpp"

namespace ASFW::Isoch::Audio::MOTU::Profiles {

namespace {

/// PCM chunks per data block at 44.1/48 kHz for the v2 fixed-chunk models
/// (motu-protocol-v2.c:274-282).
constexpr uint32_t kPcmChunks = ::ASFW::Encoding::Motu::k828mk2FixedPcmChunks[0];

/// Data block size in QUADLETS: SPH quadlet + message and PCM chunks, quadlet-padded.
/// 14 chunks give 13 quadlets, which is *smaller* than the channel count -- MOTU packs
/// 3-byte chunks, so the AM824 assumption of one quadlet per channel does not hold.
constexpr uint32_t kDbs = ::ASFW::Encoding::Motu::DataBlockQuadlets(kPcmChunks);

constexpr uint32_t kFramesPerDataPacket = 8; // 48 kHz / 8000 cycles

void FillStreamConfig(AudioStreamConfig& outConfig, AudioStreamDirection direction) noexcept {
    outConfig = AudioStreamConfig{};
    outConfig.direction = direction;
    outConfig.sampleRate = 48000;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.sid = 0;
    outConfig.pcmChannels = static_cast<uint8_t>(kPcmChunks);
    // MIDI rides in the data block's second-quadlet message slot rather than an AM824
    // MIDI conformant block, so it costs no PCM chunk here (motu-stream.c:117-131).
    outConfig.midiSlots = 0;
    outConfig.dbs = static_cast<uint8_t>(kDbs);
    outConfig.framesPerDataPacket = static_cast<uint8_t>(kFramesPerDataPacket);
    // MOTU sets the CIP SPH bit and uses fmt 0x02 / FDF 0x22, not AM824's 0x10 / 0x02
    // (amdtp-motu.c:19-25).
    outConfig.fdf = 0x22;
    outConfig.fmt = 0x02;
}

} // namespace

const char* MotuV2Profile::Name() const noexcept {
    const char* const model =
        DeviceProfiles::Audio::AudioDeviceCatalog::MotuModelNameForSwVersion(unitSwVersion_);
    return model != nullptr ? model : "MOTU (protocol v2)";
}

Encoding::AudioWireFormat MotuV2Profile::TxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kMotuV2;
}

Encoding::AudioWireFormat MotuV2Profile::RxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kMotuV2;
}

bool MotuV2Profile::BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, AudioStreamDirection::HostToDevice);
    return true;
}

bool MotuV2Profile::BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    FillStreamConfig(outConfig, AudioStreamDirection::DeviceToHost);
    return true;
}

AudioStreamTxPolicy MotuV2Profile::TxStreamPolicy() const noexcept {
    AudioStreamTxPolicy policy{};
    // Selects MotuPayloadWriter in DiceTxStreamEngine; the AM824 slot writer must not
    // also run or it would overwrite the chunk layout with slot-shaped samples.
    policy.hostToDevicePcmEncoding = Encoding::AudioWireFormat::kMotuV2;
    policy.variableDbs = false;
    // The non-audio slot word is an AM824 concept (a labelled quadlet). MOTU has no such
    // slots -- its message chunks are plain bytes -- so nothing should be pre-filled.
    policy.initializeNonAudioSlots = false;
    policy.defaultNonAudioSlotWord = 0;
    // Wire order puts the headphone pair first; the map moves Main to host channels 1-2.
    policy.motuPlaybackPorts = ::ASFW::Encoding::Motu::PlaybackPortsForSwVersion(unitSwVersion_);
    return policy;
}

std::vector<uint32_t> MotuV2Profile::SupportedSampleRates() const {
    // Mode 0 only for now: mode 1 (88.2/96k) halves the ADAT extras and mode 2
    // (176.4/192k) is unsupported by these models. Publishing a rate the chunk table
    // cannot describe would let CoreAudio select a geometry the device never accepts.
    return {44100u, 48000u};
}

uint32_t MotuV2Profile::TxMidiSlots() const noexcept {
    // MIDI rides the data block's message slot, not a dedicated AM824 block.
    return 0;
}

uint32_t MotuV2Profile::RxMidiSlots() const noexcept {
    return 0;
}

uint32_t MotuV2Profile::TxDbs() const noexcept {
    return kDbs;
}

uint32_t MotuV2Profile::RxDbs() const noexcept {
    return kDbs;
}

uint32_t MotuV2Profile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    // MOTU is duplex-always and the transmit cadence is replayed from the capture
    // stream, so playback trails capture by at least the replay depth. Match the
    // conservative offset the other non-DICE profiles use until hardware says otherwise.
    return 64;
}

uint32_t MotuV2Profile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 64;
}

uint32_t MotuV2Profile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

uint32_t MotuV2Profile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

} // namespace ASFW::Isoch::Audio::MOTU::Profiles
