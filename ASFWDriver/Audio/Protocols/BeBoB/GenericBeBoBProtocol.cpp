// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// GenericBeBoBProtocol.cpp — Concrete BeBoB fallback for known-but-untested devices.
//
// Fresh implementation. Wire choreography is cross-validated with
// Linux sound/firewire/bebob/bebob_stream.c; no reference source is copied.

#include "GenericBeBoBProtocol.hpp"

#include "../../../Logging/Logging.hpp"
#include "../../../Protocols/AVC/Core/RateCodes.hpp"

namespace ASFW::Audio::BeBoB {

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
    caps_.sampleRateHz = discoveryModel.CurrentRateHz().value_or(
        supportedRates_.empty() ? 48000U : supportedRates_[0]);
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
    return model.SupportedRatesHz();
}

} // namespace ASFW::Audio::BeBoB
