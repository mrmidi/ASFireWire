// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "../AudioStreamProfile.hpp"
#include "../../../Wire/AMDTP/AmdtpTypes.hpp"
#include "../../../../DeviceProfiles/Audio/AudioDeviceIds.hpp"

namespace ASFW::Isoch::Audio::RME::Profiles {

class FirefaceProfile final : public IAudioStreamProfile {
public:
    explicit constexpr FirefaceProfile(uint32_t channels, const char* name) noexcept
        : channels_(channels), name_(name) {}

    [[nodiscard]] const char* Name() const noexcept override { return name_; }
    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override {
        return Encoding::AudioWireFormat::kRawPcm24Upper24In32LE;
    }
    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override {
        return Encoding::AudioWireFormat::kRawPcm24Upper24In32LE;
    }
    [[nodiscard]] bool BuildDefaultTxStreamConfig(AudioStreamConfig& out) const noexcept override {
        return Build(AudioStreamDirection::HostToDevice, out);
    }
    [[nodiscard]] bool BuildDefaultRxStreamConfig(AudioStreamConfig& out) const noexcept override {
        return Build(AudioStreamDirection::DeviceToHost, out);
    }
    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override {
        AudioStreamTxPolicy policy{};
        policy.hostToDevicePcmEncoding = Encoding::AudioWireFormat::kRawPcm24Upper24In32LE;
        return policy;
    }
    [[nodiscard]] TxClockSource TransmitClockSource() const noexcept override {
        return TxClockSource::kRxReplayAfterBootstrap;
    }
    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override { return {48000U}; }
    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 16U; }
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 16U; }
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double) const noexcept override { return 0U; }
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double) const noexcept override { return 0U; }
    [[nodiscard]] uint32_t InitialClockAnchorTimeoutMs() const noexcept override { return 2000U; }

private:
    [[nodiscard]] bool Build(AudioStreamDirection direction, AudioStreamConfig& out) const noexcept {
        out = AudioStreamConfig{};
        out.direction = direction;
        out.sampleRate = 48000U;
        out.streamMode = Encoding::StreamMode::kBlocking;
        out.pcmChannels = static_cast<uint8_t>(channels_);
        out.dbs = static_cast<uint8_t>(channels_);
        out.midiSlots = 0;
        out.framesPerDataPacket = 8;
        out.packetFraming = Encoding::AudioPacketFraming::kHeaderless;
        return channels_ == 18U || channels_ == 28U;
    }

    uint32_t channels_;
    const char* name_;
};

} // namespace ASFW::Isoch::Audio::RME::Profiles
