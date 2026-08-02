// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// GenericBeBoBProtocol.cpp — Concrete BeBoB fallback for known-but-untested devices.
//
// Fresh implementation. Wire choreography is cross-validated with
// Linux sound/firewire/bebob/bebob_stream.c; no reference source is copied.

#include "GenericBeBoBProtocol.hpp"

#include "../../../DeviceProfiles/Audio/Vendors/BeBoBDeviceProfiles.hpp"
#include "../../../Logging/Logging.hpp"

namespace ASFW::Audio::BeBoB {
namespace {

// BridgeCo rate codes → Hz. Cross-validated with Linux
// sound/firewire/bebob/bebob_stream.c:24-30 (snd_bebob_rate_table).
uint32_t RateCodeToHz(uint8_t code) noexcept {
    switch (code) {
        case 0x00: return 32000U;
        case 0x01: return 44100U;
        case 0x02: return 48000U;
        case 0x03: return 88200U;
        case 0x04: return 96000U;
        case 0x05: return 176400U;
        case 0x06: return 192000U;
        case 0x0A: return 88200U;  // Some devices use 0x0A for 88.2k.
        default: return 0U;
    }
}

template<typename F>
void ForEachFormationRate(const DeviceModel& model, F&& fn) noexcept {
    for (const auto& formation : model.input.supportedFormations) {
        fn(formation.rateCode);
    }
    for (const auto& formation : model.output.supportedFormations) {
        fn(formation.rateCode);
    }
}

} // namespace

GenericBeBoBProtocol::GenericBeBoBProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                           Protocols::Ports::FireWireBusInfo& busInfo,
                                           uint16_t nodeId,
                                           IRM::IRMClient* irmClient,
                                           CMP::CMPClient* cmpClient,
                                           uint64_t deviceGuid,
                                           Scheduling::ITimerScheduler* timerScheduler,
                                           const DeviceModel& discoveryModel) noexcept
    : BeBoBProtocol(busOps, busInfo, nodeId, irmClient, cmpClient, deviceGuid, timerScheduler),
      supportedRates_(MakeSupportedRates(discoveryModel)) {

    deviceName_ = "Unknown BeBoB Device";

    // Conservative single-stream geometry from the first duplex formation.
    uint16_t pcmChannels = 0;
    uint16_t midiSlots = 0;
    if (!discoveryModel.input.supportedFormations.empty()) {
        pcmChannels = discoveryModel.input.supportedFormations[0].pcmChannels;
        midiSlots = discoveryModel.input.supportedFormations[0].midiSlots;
    }

    caps_.hostInputPcmChannels = pcmChannels;
    caps_.hostOutputPcmChannels = pcmChannels;
    caps_.deviceToHostAm824Slots = pcmChannels + midiSlots;
    caps_.hostToDeviceAm824Slots = pcmChannels + midiSlots;
    caps_.sampleRateHz = supportedRates_.empty() ? 48000U : supportedRates_[0];
    caps_.deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps_.hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps_.deviceToHostStreamCount = 1;
    caps_.hostToDeviceStreamCount = 1;
    caps_.deviceToHostStreams[0] = {.pcmChannels = pcmChannels,
                                    .am824Slots = static_cast<uint16_t>(pcmChannels + midiSlots)};
    caps_.hostToDeviceStreams[0] = {.pcmChannels = pcmChannels,
                                    .am824Slots = static_cast<uint16_t>(pcmChannels + midiSlots)};
}

std::vector<uint32_t>
GenericBeBoBProtocol::MakeSupportedRates(const DeviceModel& model) noexcept {
    bool seen[128] = {};
    std::vector<uint32_t> rates;
    ForEachFormationRate(model, [&seen, &rates](uint8_t code) {
        const uint32_t hz = RateCodeToHz(code);
        if (hz > 0 && !seen[code]) {
            seen[code] = true;
            rates.push_back(hz);
        }
    });
    return rates;
}

void GenericBeBoBProtocol::ReadClockHealth(HealthCallback callback) {
    callback(kIOReturnSuccess, DuplexHealthResult{.generation = busInfo_.GetGeneration(),
                                                   .appliedClock = appliedClock_,
                                                   .runtimeCaps = caps_,
                                                   .sourceLocked = inputConnected_ && outputConnected_,
                                                   .clockReferenceHealthy = true,
                                                   .nominalRateHz = caps_.sampleRateHz});
}

} // namespace ASFW::Audio::BeBoB
