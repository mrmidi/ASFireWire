// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioStreamProfile.hpp - Protocol-neutral ADK stream geometry contract.

#pragma once

#include "IAudioDeviceProfile.hpp"

#include <cstdint>

namespace ASFW::Isoch::Audio {

enum class AudioStreamDirection : uint8_t {
    HostToDevice,
    DeviceToHost,
};

struct AudioStreamConfig final {
    AudioStreamDirection direction{AudioStreamDirection::HostToDevice};
    uint32_t sampleRate{48000};
    Encoding::StreamMode streamMode{Encoding::StreamMode::kBlocking};
    uint8_t sid{0};
    uint8_t pcmChannels{2};
    uint8_t dbs{2};
    uint8_t midiSlots{0};
    uint8_t framesPerDataPacket{8};
    uint8_t fdf{0x02};
    uint8_t fmt{0x10};
    uint8_t sourceChannelOffset{0};
};

struct AudioStreamTxPolicy final {
    Encoding::AudioWireFormat hostToDevicePcmEncoding{Encoding::AudioWireFormat::kAM824};
    bool variableDbs{false};
    uint32_t defaultNonAudioSlotWord{0x80000000};
    bool initializeNonAudioSlots{true};
    bool preserveFdfInNoDataPackets{false};
    bool emptyPacketsDuringIdle{false};
};

// ADK packet allocation and AMDTP encoding are shared by multiple protocol
// families. Identity matching and control-plane quirks intentionally do not
// belong here.
class IAudioStreamProfile : public IAudioDeviceProfile {
public:
    virtual ~IAudioStreamProfile() override = default;

    [[nodiscard]] virtual bool BuildDefaultTxStreamConfig(AudioStreamConfig& outConfig) const noexcept = 0;
    [[nodiscard]] virtual bool BuildDefaultRxStreamConfig(AudioStreamConfig& outConfig) const noexcept = 0;
    [[nodiscard]] virtual uint32_t TxStreamCount() const noexcept { return 1; }
    [[nodiscard]] virtual uint32_t RxStreamCount() const noexcept { return 1; }
    [[nodiscard]] virtual AudioStreamTxPolicy TxStreamPolicy() const noexcept { return {}; }

    // Budget StartIO grants the device-to-host stream to deliver the first
    // data-bearing packet (which seeds the HAL zero-timestamp anchor) before
    // the start attempt is failed. DICE devices stream data within a few
    // milliseconds of the isoch start, so the default stays tight. BeBoB
    // devices transmit only CIP NO-DATA until roughly one second after they
    // begin receiving host packets, so their profiles must widen this budget
    // (Linux waits 4 s; cross-validated with Linux bebob_stream.c:10,636-666).
    [[nodiscard]] virtual uint32_t InitialClockAnchorTimeoutMs() const noexcept { return 500; }

    [[nodiscard]] uint32_t TxChannelCount() const noexcept override {
        AudioStreamConfig config{};
        return BuildDefaultTxStreamConfig(config) ? config.pcmChannels * TxStreamCount() : 0;
    }

    [[nodiscard]] uint32_t RxChannelCount() const noexcept override {
        AudioStreamConfig config{};
        return BuildDefaultRxStreamConfig(config) ? config.pcmChannels * RxStreamCount() : 0;
    }

    [[nodiscard]] uint32_t TxMidiSlots() const noexcept override {
        AudioStreamConfig config{};
        return BuildDefaultTxStreamConfig(config) ? config.midiSlots : 0;
    }

    [[nodiscard]] uint32_t RxMidiSlots() const noexcept override {
        AudioStreamConfig config{};
        return BuildDefaultRxStreamConfig(config) ? config.midiSlots : 0;
    }

    [[nodiscard]] uint32_t TxDbs() const noexcept override {
        AudioStreamConfig config{};
        return BuildDefaultTxStreamConfig(config) ? config.dbs : 0;
    }

    [[nodiscard]] uint32_t RxDbs() const noexcept override {
        AudioStreamConfig config{};
        return BuildDefaultRxStreamConfig(config) ? config.dbs : 0;
    }
};

} // namespace ASFW::Isoch::Audio
