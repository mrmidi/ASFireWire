// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceDeviceProfile.hpp
// DICE-specific profile interface.

#pragma once

#include "../IAudioDeviceProfile.hpp"
#include "Async/DiceQuirks.hpp"
#include "Isoch/DiceStreamConfig.hpp"

#include <cstdint>

namespace ASFW::Isoch::Audio::DICE {

struct DiceDeviceIdentity final {
    uint64_t guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
};

class IDiceDeviceProfile : public IAudioDeviceProfile {
public:
    virtual ~IDiceDeviceProfile() override = default;

    /// Returns true if this profile matches the given hardware vendor, model, or GUID.
    [[nodiscard]] virtual bool Matches(const DiceDeviceIdentity& identity) const noexcept = 0;

    /// Returns the DICE specific hardware/software quirks (e.g. PCM format, DBS policy).
    [[nodiscard]] virtual DiceDeviceQuirks Quirks() const noexcept = 0;

    /// Builds the default transmit (host-to-device) stream configuration.
    [[nodiscard]] virtual bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept = 0;

    /// Builds the default receive (device-to-host) stream configuration.
    [[nodiscard]] virtual bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept = 0;

    // Default implementations mapping DICE structures to the unified IAudioDeviceProfile:
    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override {
        return Quirks().tx.hostToDevicePcmEncoding;
    }

    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override {
        return Quirks().rx.deviceToHostPcmEncoding;
    }

    [[nodiscard]] uint32_t TxChannelCount() const noexcept override {
        DiceStreamConfig config{};
        if (BuildDefaultTxStreamConfig(config)) {
            return config.pcmChannels;
        }
        return 0;
    }

    [[nodiscard]] uint32_t RxChannelCount() const noexcept override {
        DiceStreamConfig config{};
        if (BuildDefaultRxStreamConfig(config)) {
            return config.pcmChannels;
        }
        return 0;
    }

    [[nodiscard]] uint32_t TxMidiSlots() const noexcept override {
        DiceStreamConfig config{};
        if (BuildDefaultTxStreamConfig(config)) {
            return config.midiSlots;
        }
        return 0;
    }

    [[nodiscard]] uint32_t RxMidiSlots() const noexcept override {
        DiceStreamConfig config{};
        if (BuildDefaultRxStreamConfig(config)) {
            return config.midiSlots;
        }
        return 0;
    }

    [[nodiscard]] uint32_t TxDbs() const noexcept override {
        DiceStreamConfig config{};
        if (BuildDefaultTxStreamConfig(config)) {
            return config.dbs;
        }
        return 0;
    }

    [[nodiscard]] uint32_t RxDbs() const noexcept override {
        DiceStreamConfig config{};
        if (BuildDefaultRxStreamConfig(config)) {
            return config.dbs;
        }
        return 0;
    }

    [[nodiscard]] virtual uint32_t SafetyOffsetFrames(double sampleRate) const noexcept {
        (void)sampleRate;
        return 64;
    }
    [[nodiscard]] virtual uint32_t ReportedLatencyFrames(double sampleRate) const noexcept {
        (void)sampleRate;
        return 128;
    }

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override {
        return SafetyOffsetFrames(sampleRate);
    }

    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override {
        return SafetyOffsetFrames(sampleRate);
    }

    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override {
        return ReportedLatencyFrames(sampleRate);
    }

    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override {
        return ReportedLatencyFrames(sampleRate);
    }
};

} // namespace ASFW::Isoch::Audio::DICE
