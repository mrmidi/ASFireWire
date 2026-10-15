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

class IDiceDeviceProfile : public IAudioStreamProfile {
public:
    virtual ~IDiceDeviceProfile() override = default;



    /// Returns the DICE specific hardware/software quirks (e.g. PCM format, DBS policy).
    [[nodiscard]] virtual DiceDeviceQuirks Quirks() const noexcept = 0;

    // Every DICE device is sent one wire format, as the TCAT kexts send it: raw
    // 24-in-32 PCM (WeissFirewire.kext 4.3.1 clipOutputSamples @0x17e10 ->
    // Float32ToSwapInt24_In_32, no AM824 label), and NO-DATA packets that keep
    // the stream's FDF with only SYT = 0xFFFF (FillFirewireBuffers in
    // PaeFireStudio.kext @0x14290 and WeissFirewire.kext @0x1503c; Linux
    // amdtp-stream.c:692-702 also writes the rate FDF in every packet). The
    // DICE III receiver checks the incoming FDF against the rate it was given
    // (Pro40 DICE III firmware, ARX CFG0 bits 15:8), so a NO-DATA FDF of 0xFF
    // is not a harmless variant. This is not a per-profile choice.
    static constexpr Encoding::AudioWireFormat kTxPcmEncoding =
        Encoding::AudioWireFormat::kRawPcm24In32;
    static constexpr bool kTxPreservesFdfInNoDataPackets = true;

    // DICE control profiles retain their wire-format and quirk policy here;
    // neutral stream geometry lives in IAudioStreamProfile.
    [[nodiscard]] Encoding::AudioWireFormat TxWireFormat() const noexcept override {
        return kTxPcmEncoding;
    }

    [[nodiscard]] Encoding::AudioWireFormat RxWireFormat() const noexcept override {
        return Quirks().rx.deviceToHostPcmEncoding;
    }

    [[nodiscard]] AudioStreamTxPolicy TxStreamPolicy() const noexcept override {
        const DiceDeviceQuirks quirks = Quirks();
        const DiceTxQuirks& tx = quirks.tx;
        return AudioStreamTxPolicy{
            .hostToDevicePcmEncoding = kTxPcmEncoding,
            .variableDbs = tx.dbsPolicy == DbsPolicy::VariablePerPacket,
            .defaultNonAudioSlotWord = tx.defaultNonAudioSlotWord,
            .initializeNonAudioSlots = tx.initializeNonAudioSlots,
            .preserveFdfInNoDataPackets = kTxPreservesFdfInNoDataPackets,
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
