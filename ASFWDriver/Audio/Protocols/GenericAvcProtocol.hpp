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
    void AdoptDiscoveredRates(std::span<const uint32_t> rates) override { rates_.assign(rates.begin(), rates.end()); }
    void AdoptDiscoveredFormations(std::span<const Runtime::RateFormation> formations) override {
        formations_.assign(formations.begin(), formations.end());
    }
    [[nodiscard]] std::span<const Runtime::RateFormation> DiscoveredFormations() const noexcept {
        return formations_;
    }
    bool GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& caps) const override {
        if (caps_.sampleRateHz == 0 || caps_.hostInputPcmChannels == 0 ||
            caps_.hostOutputPcmChannels == 0) return false;
        caps = DeviceCaps();
        return caps.sampleRateHz != 0 && caps.hostInputPcmChannels != 0 && caps.hostOutputPcmChannels != 0;
    }
protected:
    const char* DeviceName() const override { return name_; }
    AudioStreamRuntimeCaps DeviceCaps() const override {
        auto caps = caps_;
        if (appliedClock_.sampleRateHz) caps.sampleRateHz = appliedClock_.sampleRateHz;
        if (!formations_.empty()) {
            const Runtime::RateFormation* selected = nullptr;
            for (const auto& formation : formations_) {
                if (formation.sampleRateHz != caps.sampleRateHz) continue;
                if (selected) return {};
                selected = &formation;
            }
            if (!selected || !selected->protocolSupported || selected->capture.size() > 4 || selected->playback.size() > 4)
                return {};
            caps.hostInputPcmChannels = caps.hostOutputPcmChannels = 0;
            caps.deviceToHostStreamCount = static_cast<uint32_t>(selected->capture.size());
            caps.hostToDeviceStreamCount = static_cast<uint32_t>(selected->playback.size());
            for (uint32_t i = 0; i < selected->capture.size(); ++i) {
                const auto& stream = selected->capture[i];
                caps.deviceToHostStreams[i].pcmChannels = stream.pcmChannels;
                caps.deviceToHostStreams[i].am824Slots = stream.dataBlockSize;
                caps.deviceToHostStreams[i].midiPorts = stream.midiSlots;
                caps.hostInputPcmChannels += stream.pcmChannels;
            }
            for (uint32_t i = 0; i < selected->playback.size(); ++i) {
                const auto& stream = selected->playback[i];
                caps.hostToDeviceStreams[i].pcmChannels = stream.pcmChannels;
                caps.hostToDeviceStreams[i].am824Slots = stream.dataBlockSize;
                caps.hostToDeviceStreams[i].midiPorts = stream.midiSlots;
                caps.hostOutputPcmChannels += stream.pcmChannels;
            }
            caps.deviceToHostAm824Slots = selected->capture.empty() ? 0 : selected->capture[0].dataBlockSize;
            caps.hostToDeviceAm824Slots = selected->playback.empty() ? 0 : selected->playback[0].dataBlockSize;
        }
        return caps;
    }
    // Only formations with the same PCM/slot geometry are offered by discovery.
    std::vector<uint32_t> SupportedRates() const override { return rates_.empty() ? std::vector<uint32_t>{caps_.sampleRateHz} : rates_; }
    void ConfigureMixer(MixerFailurePolicy policy, MixerCompletion completion) override {
        if (!startupMixer_) { BeBoBProtocol::ConfigureMixer(policy, std::move(completion)); return; }
        RunMixerMap(*startupMixer_, policy, std::move(completion));
    }
private:
    AudioStreamRuntimeCaps caps_{};
    std::vector<uint32_t> rates_;
    std::vector<Runtime::RateFormation> formations_;
    const BeBoB::MixerMap* startupMixer_;
    const char* name_;
};

} // namespace ASFW::Audio
