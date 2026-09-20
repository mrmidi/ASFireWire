// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// PreSonusStudioLive2442Profile.hpp
// PreSonus StudioLive 24.4.2 FireWire profile (DICE/TCAT).

#pragma once

#include "../../DiceDeviceProfile.hpp"

namespace ASFW::Isoch::Audio::DICE::Profiles {

class PreSonusStudioLive2442Profile final : public IDiceDeviceProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override;


    [[nodiscard]] DiceDeviceQuirks Quirks() const noexcept override;

    [[nodiscard]] bool BuildDefaultTxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(DiceStreamConfig& outConfig) const noexcept override;

    // Two isochronous streams per direction. Capture is uniform (16 + 16) so it
    // inherits the default indexed behaviour; playback is 16 + 10 and therefore
    // overrides BuildTxStreamConfig below.
    [[nodiscard]] uint32_t TxStreamCount() const noexcept override { return 2; }
    [[nodiscard]] uint32_t RxStreamCount() const noexcept override { return 2; }

    // The base implementation assumes every playback stream has the shape of
    // stream 0 at successive offsets, which would describe this device as 16+16
    // and write 16-slot CIP into a device RX stream that carries 10.
    [[nodiscard]] bool BuildTxStreamConfig(uint32_t streamIndex,
                                           AudioStreamConfig& outConfig) const noexcept override;

    // TxChannelCount() is deliberately NOT overridden any more: the base now
    // sums BuildTxStreamConfig() across the streams, which gives this device's
    // 16 + 10 = 26 without a per-model override.

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;

    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::DICE::Profiles
