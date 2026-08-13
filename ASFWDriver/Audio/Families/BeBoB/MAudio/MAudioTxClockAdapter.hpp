// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Converts the M-Audio vendor TX callback warm-up into AudioDriverKit clock
// anchors. It owns no mapped audio memory and performs no controller access;
// the AudioDriverKit side publishes its returned value through the existing
// HostClockAnchor mailbox.

#pragma once

#include "MAudioDuplexChoreography.hpp"

#include <cstdint>

namespace ASFW::Audio::Families::BeBoB::MAudio {

struct TxClockPublication final {
    bool captureReferencePlanted{false};
    TxClockAnchor captureReference{};

    bool playbackAnchorReady{false};
    uint64_t sampleFrame{0};
    uint64_t hostTicks{0};
    uint32_t hostNanosPerSampleQ8{0};
    TxClockAnchor source{};
};

// One serial AudioDriverKit TX-preparation queue owns this adapter between
// Arm() and Disarm(). `transportGeneration` de-duplicates coalesced delivery;
// `observedGroupCount_` intentionally counts callbacks, rather than assuming
// a particular OHCI interrupt-generation sequence.
class TxClockAdapter final {
  public:
    [[nodiscard]] bool Arm(StartEpoch epoch, uint32_t sampleRateHz,
                           uint32_t zeroTimestampPeriodFrames) noexcept;
    void Disarm() noexcept;

    [[nodiscard]] TxClockPublication ObserveHardwareWake(
        uint64_t transportGeneration, TxClockAnchor anchor) noexcept;

    [[nodiscard]] bool IsArmed() const noexcept { return armed_; }
    [[nodiscard]] const ChoreographyState& State() const noexcept {
        return state_;
    }

  private:
    [[nodiscard]] ChoreographyStep Advance(ChoreographyEvent event) noexcept;
    [[nodiscard]] TxClockPublication PublishPlaybackAnchor(
        TxClockAnchor source) noexcept;

    StartEpoch epoch_{};
    ChoreographyState state_{Stopped{}};
    uint64_t lastTransportGeneration_{0};
    uint32_t observedGroupCount_{0};
    uint32_t sampleRateHz_{0};
    uint32_t zeroTimestampPeriodFrames_{0};
    uint32_t hostNanosPerSampleQ8_{0};
    bool armed_{false};
    bool playbackOriginValid_{false};
    TxClockAnchor playbackOrigin_{};
    uint64_t lastPublishedSampleFrame_{0};
};

} // namespace ASFW::Audio::Families::BeBoB::MAudio
