// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceDeviceProfile.hpp
// DICE-specific profile interface.

#pragma once

#include "../AudioStreamProfile.hpp"
#include "Async/DiceQuirks.hpp"
#include "Isoch/DiceStreamConfig.hpp"

#include <cstdint>
#include <vector>

namespace ASFW::Isoch::Audio::DICE {

struct DiceDeviceIdentity final {
    uint64_t guid{0};
    uint32_t vendorId{0};
    uint32_t modelId{0};
};

class IDiceDeviceProfile : public IAudioStreamProfile {
public:
    virtual ~IDiceDeviceProfile() override = default;

    /// Returns true if this profile matches the given hardware vendor, model, or GUID.
    [[nodiscard]] virtual bool Matches(const DiceDeviceIdentity& identity) const noexcept = 0;

    /// Returns the DICE specific hardware/software quirks (e.g. PCM format, DBS policy).
    [[nodiscard]] virtual DiceDeviceQuirks Quirks() const noexcept = 0;

    /// Sample rates advertised to CoreAudio. Default is the universal DICE 1x
    /// baseline (44.1/48 kHz); 2x/4x are omitted until their stream geometry is
    /// verified end-to-end (see kDiceMaxSupportedRateHz). Profiles for devices
    /// with a different 1x set may override.
    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override {
        return {44100u, 48000u};
    }

    // DICE control profiles retain their wire-format and quirk policy here;
    // neutral stream geometry lives in IAudioStreamProfile.
    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override {
        return Quirks().tx.hostToDevicePcmEncoding;
    }

    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override {
        return Quirks().rx.deviceToHostPcmEncoding;
    }

    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override {
        const DiceDeviceQuirks quirks = Quirks();
        const DiceTxQuirks& tx = quirks.tx;
        return AudioStreamTxPolicy{
            .hostToDevicePcmEncoding = tx.hostToDevicePcmEncoding,
            .variableDbs = tx.dbsPolicy == DbsPolicy::VariablePerPacket,
            .defaultNonAudioSlotWord = tx.defaultNonAudioSlotWord,
            .initializeNonAudioSlots = tx.initializeNonAudioSlots,
            .preserveFdfInNoDataPackets = tx.preserveFdfInNoDataPackets,
        };
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
