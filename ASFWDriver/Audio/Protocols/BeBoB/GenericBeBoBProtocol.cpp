// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// GenericBeBoBProtocol.cpp — Concrete BeBoB fallback for known-but-untested devices.
//
// Fresh implementation. Wire choreography is cross-validated with
// Linux sound/firewire/bebob/bebob_stream.c; no reference source is copied.

#include "GenericBeBoBProtocol.hpp"

#include "../../../Logging/Logging.hpp"

#include <algorithm>

namespace ASFW::Audio::BeBoB {
namespace {

[[nodiscard]] const Protocols::AVC::Probe::StreamFormation*
FindFormation(const Protocols::AVC::Probe::IsochronousPlugModel& plug,
              uint8_t rateCode) noexcept {
    const auto it = std::find_if(
        plug.supportedFormations.begin(), plug.supportedFormations.end(),
        [rateCode](const auto& formation) { return formation.rateCode == rateCode; });
    return it != plug.supportedFormations.end() ? &*it : nullptr;
}

} // namespace

GenericBeBoBProtocol::GenericBeBoBProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                           Protocols::Ports::FireWireBusInfo& busInfo,
                                           Discovery::DeviceRouteToken route,
                                           IRM::IRMClient* irmClient,
                                           CMP::CMPClient* cmpClient,
                                           Scheduling::ITimerScheduler* timerScheduler,
                                           const DeviceModel& discoveryModel) noexcept
    : BeBoBProtocol(busOps, busInfo, route, irmClient, cmpClient, timerScheduler),
      supportedRates_(MakeSupportedRates(discoveryModel)) {

    deviceName_ = "Unknown BeBoB Device";

    // Keep both directions independent. AV/C input is host->device playback;
    // AV/C output is device->host capture. A common rate is selected, but
    // channel/slot geometry is never copied from one direction to the other.
    uint8_t selectedRateCode = discoveryModel.CurrentRateCode().value_or(0xFFU);
    if (FindFormation(discoveryModel.input, selectedRateCode) == nullptr ||
        FindFormation(discoveryModel.output, selectedRateCode) == nullptr) {
        selectedRateCode = 0xFFU;
        for (const auto& input : discoveryModel.input.supportedFormations) {
            if (FindFormation(discoveryModel.output, input.rateCode) != nullptr) {
                selectedRateCode = input.rateCode;
                break;
            }
        }
    }
    const auto* playback = FindFormation(discoveryModel.input, selectedRateCode);
    const auto* capture = FindFormation(discoveryModel.output, selectedRateCode);
    const uint16_t capturePcm = capture ? capture->pcmChannels : 0;
    const uint16_t captureMidi = capture ? capture->midiSlots : 0;
    const uint16_t playbackPcm = playback ? playback->pcmChannels : 0;
    const uint16_t playbackMidi = playback ? playback->midiSlots : 0;

    caps_.hostInputPcmChannels = capturePcm;
    caps_.hostOutputPcmChannels = playbackPcm;
    caps_.deviceToHostAm824Slots = capturePcm + captureMidi;
    caps_.hostToDeviceAm824Slots = playbackPcm + playbackMidi;
    caps_.sampleRateHz = Protocols::AVC::Probe::RateCodeToHz(selectedRateCode);
    caps_.deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps_.hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps_.deviceToHostStreamCount = capture ? 1U : 0U;
    caps_.hostToDeviceStreamCount = playback ? 1U : 0U;
    caps_.deviceToHostStreams[0] = {
        .pcmChannels = capturePcm,
        .am824Slots = static_cast<uint16_t>(capturePcm + captureMidi)};
    caps_.hostToDeviceStreams[0] = {
        .pcmChannels = playbackPcm,
        .am824Slots = static_cast<uint16_t>(playbackPcm + playbackMidi)};
}

std::vector<uint32_t>
GenericBeBoBProtocol::MakeSupportedRates(const DeviceModel& model) noexcept {
    return model.CommonDuplexRatesHz();
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
