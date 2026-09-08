// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "../../DiceDeviceProfile.hpp"

#include <array>

namespace ASFW::Isoch::Audio::DICE::Profiles {

// Experimental 44.1/48 kHz profile using geometry read from a real Project.
// Build 9 raw-PCM trials verified bounded playback and rate switching on one
// unit; earlier labelled-AM824 trials also covered capture. Adapter-reconnect
// timing remains unfixed; calibrated latency and long-run stability are unvalidated.
// See captures/presonus-firestudio-project/.
class PreSonusFireStudioProjectProfile final : public IDiceDeviceProfile {
public:
    static constexpr uint32_t kSampleRateHz = 48000;
    static constexpr std::array<uint32_t, 2> kSupportedSampleRatesHz{44100, 48000};
    static constexpr uint16_t kPcmChannels = 10;
    static constexpr uint16_t kMidiPorts = 1;
    static constexpr uint8_t kMidiSlots = 1;
    static constexpr uint8_t kDbs = 11;
    static constexpr uint32_t kStreamCount = 1;

    [[nodiscard]] const char* Name() const noexcept override;
    [[nodiscard]] bool Matches(const DiceDeviceIdentity& identity) const noexcept override;
    [[nodiscard]] DiceDeviceQuirks Quirks() const noexcept override;
    [[nodiscard]] std::vector<uint32_t> SupportedSampleRates() const override;
    [[nodiscard]] bool BuildDefaultTxStreamConfig(DiceStreamConfig& out) const noexcept override;
    [[nodiscard]] bool BuildDefaultRxStreamConfig(DiceStreamConfig& out) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::DICE::Profiles
