// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MidasVeniceProfile.hpp
// Midas Venice F32 FireWire profile (DICE/TCAT).

#pragma once

#include "../../DiceDeviceProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

class MidasVeniceProfile final : public IDiceDeviceProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;


    [[nodiscard]] DiceDeviceQuirks Quirks() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;

    // TWO isoch streams per direction. This is the one geometry fact that holds
    // across the whole Venice F range: the variants differ in how wide the
    // streams are, not how many there are (the recorded F24 carries 16 + 8 and
    // still reports NUMBER = 2 in both sections).
    [[nodiscard]] uint32_t TxStreamCount() const noexcept override { return 2; }
    [[nodiscard]] uint32_t RxStreamCount() const noexcept override { return 2; }

    // The per-stream channel counts are a seed, in both directions.
    //
    // The F16, F24 and F32 share one identity -- vendor 0x0010C73F, TCAT
    // product 0x001 -- and the recorded F24 dump even reports its model string
    // as "Venice F32" (documentation/fixtures/DICE/midasF24.txt), so identity
    // cannot tell the variants apart at all. The 16-per-stream constants below
    // describe the F32 and are simply wrong for the other two; asserting them
    // would refuse to publish every Venice that is not an F32.
    [[nodiscard]] Isoch::Audio::StreamGeometryAuthority
    PlaybackGeometryAuthority() const noexcept override {
        return Isoch::Audio::StreamGeometryAuthority::kSeed;
    }
    [[nodiscard]] Isoch::Audio::StreamGeometryAuthority
    CaptureGeometryAuthority() const noexcept override {
        return Isoch::Audio::StreamGeometryAuthority::kSeed;
    }

    // Which Venice this is, named from what the device turned out to carry.
    [[nodiscard]] const char* NameForGeometry(
        uint32_t hostInputPcmChannels,
        uint32_t hostOutputPcmChannels) const noexcept override;

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;

    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::DICE::Profiles
