// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProfile.hpp - Generic AV/C framing and policy, with the Duet's own
// timing declarations.
//
// The generic AV/C safety offsets (192/256 frames at 48 kHz) were widened for
// the Onyx-i and serve every AV/C device; they put the Duet's declared round
// trip at 21 ms for a 32-frame buffer. Apogee ships its own table for this
// device (DuetFWOverideDriver.kext Info.plist, IOKitPersonalities ->
// DeviceSpecific "000003DB.0001DDDD" -> Latency), which AppleFWAudio applied:
//
//   rate    InputLatency  InputSafety  OutputLatency  OutputSafety
//   44100        46           46            55             46
//   48000        40           50            67             50
//   88200       108           92            57             92
//   96000       119          100            59            100
//
// The latencies are the device path (converters, transport) and are declared
// as Apogee wrote them. The safety offsets are promises OUR engine must keep,
// so they come from the ladder the DICE profiles run on the same TX/RX engine
// (6 / 10 delay packets + 2 per rate tier: 48 out / 80 in at 1x), not from
// Apogee's values for AppleFWAudio's engine.

#pragma once

#include "GenericAvcProfile.hpp"

namespace ASFW::Isoch::Audio::AVC::Profiles {

class ApogeeDuetProfile final : public GenericAvcProfile {
public:
    [[nodiscard]] const char* Name() const noexcept override { return "Apogee Duet"; }

    [[nodiscard]] uint32_t TxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxSafetyOffsetFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t TxReportedLatencyFrames(double sampleRate) const noexcept override;
    [[nodiscard]] uint32_t RxReportedLatencyFrames(double sampleRate) const noexcept override;
};

} // namespace ASFW::Isoch::Audio::AVC::Profiles
