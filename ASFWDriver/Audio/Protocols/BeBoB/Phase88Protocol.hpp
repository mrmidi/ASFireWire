// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Exact TerraTec PHASE 88 Rack FW BeBoB/CMP adapter.

#pragma once

#include "BeBoBProtocol.hpp"

#include <cstdint>
#include <functional>

namespace ASFW::Audio::BeBoB {

struct MixerMap;

class Phase88Protocol final : public BeBoBProtocol {
public:
    using BeBoBProtocol::BeBoBProtocol;

    const char* GetName() const override { return "TerraTec PHASE 88 Rack FW"; }
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const override;

protected:
    // IDeviceProtocol hooks
    const char* DeviceName() const override { return "TerraTec PHASE 88 Rack FW"; }
    [[nodiscard]] AudioStreamRuntimeCaps DeviceCaps() const override;
    [[nodiscard]] std::vector<uint32_t> SupportedRates() const override;
    void ReadClockHealth(HealthCallback callback) override;

    // Async mixer configuration — Phase88 ships muted; unmute + max-vol on start.
    void ConfigureMixer(MixerFailurePolicy policy, MixerCompletion completion) override;

private:
    [[nodiscard]] static AudioStreamRuntimeCaps Phase88Caps() noexcept;

    void RunMixerSteps(const MixerMap& map, MixerFailurePolicy policy, MixerCompletion finalCompletion);

    static constexpr uint32_t kPhase88PcmChannels = 10;
    static constexpr uint32_t kPhase88MidiDataBlocks = 1;
    static constexpr uint32_t kPhase88SampleRateHz = 48000;
};

} // namespace ASFW::Audio::BeBoB
