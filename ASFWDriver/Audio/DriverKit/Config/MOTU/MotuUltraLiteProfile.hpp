// SPDX-License-Identifier: Apache-2.0
// Modified in 2026 by Rafal Zalech to add original MOTU UltraLite support.
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../AudioStreamProfile.hpp"
#include "../../../Wire/MOTU/MotuBlockLayout.hpp"

namespace ASFW::Isoch::Audio::MOTU {

class MotuUltraLiteProfile final : public IAudioStreamProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override { return "MOTU UltraLite"; }
    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override {
        return Encoding::AudioWireFormat::kMotuV2;
    }
    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override {
        return Encoding::AudioWireFormat::kMotuV2;
    }

    [[nodiscard]] bool BuildDefaultTxStreamConfig(
        AudioStreamConfig& out) const noexcept override {
        Fill(out, AudioStreamDirection::HostToDevice);
        return true;
    }
    [[nodiscard]] bool BuildDefaultRxStreamConfig(
        AudioStreamConfig& out) const noexcept override {
        Fill(out, AudioStreamDirection::DeviceToHost);
        return true;
    }

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 256; }
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 256; }
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double) const noexcept override { return 512; }
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double) const noexcept override { return 512; }
    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override {
        return {48000u};
    }

    [[nodiscard]] PreferredStereoChannels PreferredOutputStereoChannels() const noexcept override {
        return {.left = 1, .right = 2};
    }

    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override {
        return AudioStreamTxPolicy{
            .hostToDevicePcmEncoding = Encoding::AudioWireFormat::kMotuV2,
            .variableDbs = false,
            .defaultNonAudioSlotWord = 0,
            .initializeNonAudioSlots = false,
            .preserveFdfInNoDataPackets = true,
            .emptyPacketsDuringIdle = false,
            // Match the stream order created by MOTU's original kext: Main,
            // Analog 1-8, S/PDIF, Phones.
            .sourceChannelForWireSlot =
                Encoding::Motu::kUltraLiteOutputSourceForWireChannel,
            .sourceChannelMapEnabled = true,
        };
    }

private:
    static void Fill(AudioStreamConfig& out, AudioStreamDirection direction) noexcept {
        out = {};
        out.direction = direction;
        out.sampleRate = 48000;
        out.streamMode = Encoding::StreamMode::kBlocking;
        out.sid = 0;
        out.pcmChannels = 14;
        // MOTU uses 13 quadlets per data block: one SPH quadlet followed by
        // two 3-byte message chunks and fourteen packed 24-bit PCM chunks.
        out.dbs = 13;
        out.midiSlots = 0;
        // The UltraLite's 48 kHz receive stream uses the 8/8/8/0 blocking
        // cadence. TX replays that physical cadence, so the slot geometry must
        // accept the largest observed packet rather than the six-frame average.
        out.framesPerDataPacket = 8;
        out.fdf = 0x22;
        out.fmt = 0x02;
        out.sph = true;
    }
};

} // namespace ASFW::Isoch::Audio::MOTU
