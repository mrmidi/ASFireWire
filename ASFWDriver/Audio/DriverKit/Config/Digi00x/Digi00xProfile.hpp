// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xProfile.hpp - stream profile for the Digidesign Digi 002 / 003
// (issue #249).
//
// CIP AM824, non-blocking, one stream per direction: 19 data channels, the
// MIDI data channel first and 18 PCM after it (Wire/Digi00x/Digi00xSlotLayout).
// The device ignores SYT and recovers its clock from the data-block count of
// the host's packets, and transmits nothing until it has received packets, so
// transmit starts from a nominal cadence and then replays capture's sequence
// (Linux digi00x-stream.c:375-386: amdtp_domain_start(..., replay_seq = true)).
// Outgoing packets carry SYT 0xFFFF (amdtp-dot.c:393 CIP_UNAWARE_SYT) and
// scrambled PCM (Wire/Digi00x/DotScrambler.hpp).

#pragma once

#include "../AudioStreamProfile.hpp"
#include "../../../Wire/AMDTP/AmdtpTypes.hpp"
#include "../../../Wire/Digi00x/Digi00xSlotLayout.hpp"
#include "../../../Protocols/Digi00x/Digi00xRegisters.hpp"

#include <vector>

namespace ASFW::Isoch::Audio::Digi00x::Profiles {

class Digi00xProfile final : public IAudioStreamProfile {
public:
    explicit constexpr Digi00xProfile(const char* name) noexcept : name_(name) {}

    [[nodiscard]] const char* Name() const noexcept override { return name_; }
    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override {
        return Encoding::AudioWireFormat::kAM824;
    }
    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override {
        return Encoding::AudioWireFormat::kAM824;
    }
    [[nodiscard]] bool BuildDefaultTxStreamConfig(AudioStreamConfig& out) const noexcept override {
        return Build(AudioStreamDirection::HostToDevice, out);
    }
    [[nodiscard]] bool BuildDefaultRxStreamConfig(AudioStreamConfig& out) const noexcept override {
        return Build(AudioStreamDirection::DeviceToHost, out);
    }
    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override {
        AudioStreamTxPolicy policy{};
        policy.hostToDevicePcmEncoding = Encoding::AudioWireFormat::kAM824;
        // The MIDI data channel idles as 0x80 followed by zeros.
        policy.defaultNonAudioSlotWord = Encoding::Digi00x::kIdleMidiWord;
        policy.initializeNonAudioSlots = true;
        policy.playbackChannelMap = PcmMap();
        policy.sytUnaware = true;      // SYT 0xFFFF (Linux CIP_UNAWARE_SYT)
        policy.dotScramblePcm = true;  // playback PCM is scrambled
        return policy;
    }
    [[nodiscard]] TxClockSource TransmitClockSource() const noexcept override {
        return TxClockSource::kRxReplayAfterBootstrap;
    }
    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override {
        return {44100U, 48000U};
    }
    // Untuned: the conservative values the other replayed register families
    // (RME, MOTU) declare, until hardware says otherwise.
    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 64U; }
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 64U; }
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double) const noexcept override { return 128U; }
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double) const noexcept override { return 128U; }
    // The device answers only once it receives; Linux waits 200 ms
    // (digi00x-stream.c:10). Generous until measured on hardware.
    [[nodiscard]] uint32_t InitialClockAnchorTimeoutMs() const noexcept override { return 2000U; }

private:
    static constexpr uint32_t kRateHz = 48000U;

    [[nodiscard]] static ::ASFW::Audio::Wire::PcmSlotMap PcmMap() noexcept {
        return Encoding::Digi00x::PcmAfterMidiSlotMap(
            ::ASFW::Audio::Digi00x::PcmChannelsForRate(kRateHz).value_or(0));
    }

    [[nodiscard]] bool Build(AudioStreamDirection direction, AudioStreamConfig& out) const noexcept {
        const auto pcm = ::ASFW::Audio::Digi00x::PcmChannelsForRate(kRateHz);
        const auto dbs = ::ASFW::Audio::Digi00x::DataBlockQuadletsForRate(kRateHz);
        if (!pcm || !dbs) return false;
        out = AudioStreamConfig{};
        out.direction = direction;
        out.sampleRate = kRateHz;
        out.streamMode = Encoding::StreamMode::kNonBlocking;
        out.pcmChannels = static_cast<uint8_t>(*pcm);
        out.dbs = static_cast<uint8_t>(*dbs);
        out.midiSlots = 0;  // the MIDI data channel is not an IEC 61883-6 MIDI slot
        out.fmt = 0x10;     // AM824; fdf follows the rate (AmdtpTxPacketizer)
        out.fdf = 0x02;
        out.packetFraming = Encoding::AudioPacketFraming::kCip;
        out.pcmSlotMap = PcmMap();
        out.hasPcmSlotMap = true;
        return true;
    }

    const char* name_;
};

} // namespace ASFW::Isoch::Audio::Digi00x::Profiles
