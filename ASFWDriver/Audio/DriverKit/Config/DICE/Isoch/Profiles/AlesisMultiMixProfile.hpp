// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AlesisMultiMixProfile.hpp
// Alesis MultiMix 8/12/16 FireWire profile (DICE/TCAT).

#pragma once

#include "../../DiceDeviceProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

class AlesisMultiMixProfile final : public IDiceDeviceProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;


    [[nodiscard]] DiceDeviceQuirks Quirks() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;

    // ONE host-to-device (playback) stream. This is a deliberate override of
    // what the device announces, and it is the one geometry fact here with a
    // reference behind it: libffado forces nb_rx to 1 for Alesis model 0x000000
    // and 0x000001 because "[it] announces two receive transmitters, but only
    // has one" (dice_avdevice.cpp:1686-1700). Do not raise it to match the
    // register.
    [[nodiscard]] uint32_t TxStreamCount() const noexcept override { return 1; }

    // Capture (the device's DICE TX section) is NOT covered by that quirk --
    // libffado clamps nb_rx only and never touches nb_tx. This value was a
    // transcription of the playback clamp onto the other direction, and a
    // contributed MultiMix dump reports two capture streams (12 + 2), so it is
    // a seed and nothing more. See CaptureGeometryAuthority below.
    [[nodiscard]] uint32_t RxStreamCount() const noexcept override { return 1; }

    // Playback is asserted on libffado's authority. Capture is a seed: the
    // MultiMix 8, 12 and 16 all publish vendor 0x000595 / model 0x000000
    // (ALSA-userspace names the single row "Alesis MultiMix 8/12/16",
    // model.rs:140) and no reference stack hardcodes their channel counts --
    // Linux has no dice.c row for this model at all, so it takes the generic
    // register-detection path. A constant here could only ever be right for one
    // of the three variants.
    [[nodiscard]] Isoch::Audio::StreamGeometryAuthority
    PlaybackGeometryAuthority() const noexcept override {
        return Isoch::Audio::StreamGeometryAuthority::kAsserted;
    }
    [[nodiscard]] Isoch::Audio::StreamGeometryAuthority
    CaptureGeometryAuthority() const noexcept override {
        return Isoch::Audio::StreamGeometryAuthority::kSeed;
    }

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;

    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::DICE::Profiles
