// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuProfile.cpp - see MotuProfile.hpp.

#include "MotuProfile.hpp"

#include "../../../../DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "../../../Wire/MOTU/MotuModel.hpp"
#include "../../../Wire/MOTU/MotuPortLayout.hpp"

namespace ASFW::Isoch::Audio::MOTU::Profiles {

namespace {

constexpr uint32_t kFramesPerDataPacket = 8; // 48 kHz / 8000 cycles

void FillStreamConfig(AudioStreamConfig& outConfig, AudioStreamDirection direction, uint32_t chunks, uint32_t version) noexcept {
    outConfig = AudioStreamConfig{};
    outConfig.direction = direction;
    outConfig.sampleRate = 48000;
    outConfig.streamMode = Encoding::StreamMode::kBlocking;
    outConfig.sid = 0;
    outConfig.pcmChannels = static_cast<uint8_t>(chunks);
    // MIDI rides in the data block's second-quadlet message slot rather than an AM824
    // MIDI conformant block, so it costs no PCM chunk here (motu-stream.c:117-131).
    outConfig.midiSlots = 0;
    outConfig.dbs = static_cast<uint8_t>(::ASFW::Encoding::Motu::DataBlockQuadlets(chunks, ::ASFW::Encoding::Motu::MessageChunks(version, direction == AudioStreamDirection::DeviceToHost)));
    outConfig.framesPerDataPacket = static_cast<uint8_t>(kFramesPerDataPacket);
    // MOTU sets the CIP SPH bit and uses fmt 0x02 / FDF 0x22, not AM824's 0x10 / 0x02
    // (amdtp-motu.c:19-25).
    outConfig.fdf = 0x22;
    outConfig.fmt = 0x02;
    outConfig.cipSph = true;
    outConfig.motuMessageChunks = ::ASFW::Encoding::Motu::MessageChunks(version, direction == AudioStreamDirection::DeviceToHost);
    outConfig.motuPcmByteOffset = ::ASFW::Encoding::Motu::PcmByteOffset(version);
}

} // namespace

const char* MotuProfile::Name() const noexcept {
    const char* const model =
        DeviceProfiles::Audio::AudioDeviceCatalog::MotuModelNameForSwVersion(unitSwVersion_);
    return model != nullptr ? model : "MOTU (protocol v2)";
}

Encoding::AudioWireFormat MotuProfile::TxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kMotuPacked;
}

Encoding::AudioWireFormat MotuProfile::RxWireFormat() const noexcept {
    return Encoding::AudioWireFormat::kMotuPacked;
}

bool MotuProfile::BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    const auto* model = ::ASFW::Encoding::Motu::FindModel(unitSwVersion_);
    if (!model || !::ASFW::Encoding::Motu::FireWireOnly(unitSwVersion_)) return false;
    FillStreamConfig(outConfig, AudioStreamDirection::HostToDevice, model->playbackChunks[0], unitSwVersion_);
    return true;
}

bool MotuProfile::BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept {
    const auto* model = ::ASFW::Encoding::Motu::FindModel(unitSwVersion_);
    if (!model || !::ASFW::Encoding::Motu::FireWireOnly(unitSwVersion_)) return false;
    FillStreamConfig(outConfig, AudioStreamDirection::DeviceToHost, model->captureChunks[0], unitSwVersion_);
    return true;
}

AudioStreamTxPolicy MotuProfile::TxStreamPolicy() const noexcept {
    AudioStreamTxPolicy policy{};
    // Selects MotuPayloadWriter in DiceTxStreamEngine; the AM824 slot writer must not
    // also run or it would overwrite the chunk layout with slot-shaped samples.
    policy.hostToDevicePcmEncoding = Encoding::AudioWireFormat::kMotuPacked;
    policy.variableDbs = false;
    // The non-audio slot word is an AM824 concept (a labelled quadlet). MOTU has no such
    // slots -- its message chunks are plain bytes -- so nothing should be pre-filled.
    policy.initializeNonAudioSlots = false;
    policy.defaultNonAudioSlotWord = 0;
    // Wire order puts the headphone pair first; the map moves Main to host channels 1-2.
    policy.motuPlaybackPorts = PlaybackPortsForSwVersion(unitSwVersion_);
    policy.dbcIsEndEvent = true;
    policy.preserveFdfInNoDataPackets = true;
    return policy;
}

std::vector<uint32_t> MotuProfile::SupportedSampleRates() const {
    // Best-effort FireWire rates follow each model's wire geometry table.
    std::vector<uint32_t> rates;
    if (const auto* model = ::ASFW::Encoding::Motu::FindModel(unitSwVersion_))
        for (const auto rate : ::ASFW::Encoding::Motu::kClockRates)
            if (::ASFW::Encoding::Motu::SupportsRate(*model, rate) &&
                ::ASFW::Encoding::Motu::FireWireOnly(unitSwVersion_)) rates.push_back(rate);
    return rates;
}

uint32_t MotuProfile::TxMidiSlots() const noexcept {
    // MIDI rides the data block's message slot, not a dedicated AM824 block.
    return 0;
}

uint32_t MotuProfile::RxMidiSlots() const noexcept {
    return 0;
}

uint32_t MotuProfile::TxDbs() const noexcept {
    const auto* model = ::ASFW::Encoding::Motu::FindModel(unitSwVersion_);
    return model ? ::ASFW::Encoding::Motu::DataBlockQuadlets(model->playbackChunks[0], ::ASFW::Encoding::Motu::MessageChunks(unitSwVersion_, false)) : 0;
}

uint32_t MotuProfile::RxDbs() const noexcept {
    const auto* model = ::ASFW::Encoding::Motu::FindModel(unitSwVersion_);
    return model ? ::ASFW::Encoding::Motu::DataBlockQuadlets(model->captureChunks[0], ::ASFW::Encoding::Motu::MessageChunks(unitSwVersion_, true)) : 0;
}

uint32_t MotuProfile::TxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    // MOTU is duplex-always and the transmit cadence is replayed from the capture
    // stream, so playback trails capture by at least the replay depth. Match the
    // conservative offset the other non-DICE profiles use until hardware says otherwise.
    return 64;
}

uint32_t MotuProfile::RxSafetyOffsetFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 64;
}

uint32_t MotuProfile::TxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

uint32_t MotuProfile::RxReportedLatencyFrames(double sampleRate) const noexcept {
    (void)sampleRate;
    return 128;
}

} // namespace ASFW::Isoch::Audio::MOTU::Profiles
