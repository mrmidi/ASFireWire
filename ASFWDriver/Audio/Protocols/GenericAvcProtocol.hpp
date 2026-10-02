// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include "BeBoB/BeBoBProtocol.hpp"

namespace ASFW::Audio {

// Standard plug-0 AV/C/CMP lifecycle, shared with BeBoB and Oxford.
// Cross-validated with Linux sound/firewire/oxfw/oxfw-stream.c:105-120,188
// and sound/firewire/bebob/bebob_stream.c. Stream geometry is the discovered
// one (AdoptDiscoveredGeometry); a device row may add a startup mixer map,
// its only device-specific behaviour.
class GenericAvcProtocol final : public BeBoB::BeBoBProtocol {
public:
    GenericAvcProtocol(Protocols::Ports::FireWireBusOps& busOps,
                       Protocols::Ports::FireWireBusInfo& busInfo,
                       Discovery::DeviceRouteToken route,
                       IRM::IRMClient* irmClient,
                       CMP::CMPClient* cmpClient,
                       Scheduling::ITimerScheduler* timerScheduler,
                       const BeBoB::MixerMap* startupMixer = nullptr,
                       const char* name = "Generic AV/C Audio") noexcept
        : BeBoBProtocol(busOps, busInfo, route, irmClient, cmpClient, timerScheduler),
          startupMixer_(startupMixer), name_(name) {}

    const char* GetName() const override { return name_; }
    void AdoptDiscoveredGeometry(const AudioStreamRuntimeCaps& caps) noexcept override { caps_ = caps; }
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& caps) const override {
        if (caps_.sampleRateHz == 0 || caps_.hostInputPcmChannels == 0 ||
            caps_.hostOutputPcmChannels == 0) return false;
        caps = caps_;
        return true;
    }
protected:
    const char* DeviceName() const override { return name_; }
    AudioStreamRuntimeCaps DeviceCaps() const override { return caps_; }
    // Start at the observed rate. A different rate requires fresh geometry.
    std::vector<uint32_t> SupportedRates() const override { return {caps_.sampleRateHz}; }
    void ConfigureMixer(MixerFailurePolicy policy, MixerCompletion completion) override {
        if (!startupMixer_) { BeBoBProtocol::ConfigureMixer(policy, std::move(completion)); return; }
        RunMixerMap(*startupMixer_, policy, std::move(completion));
    }
private:
    AudioStreamRuntimeCaps caps_{};
    const BeBoB::MixerMap* startupMixer_;
    const char* name_;
};

} // namespace ASFW::Audio
