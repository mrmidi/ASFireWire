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

    const auto selectedRate = discoveryModel.SelectDuplexRateHz();
    if (!selectedRate) {
        ASFW_LOG(AVC, "GenericBeBoB: no usable duplex rate/formation; leaving stream caps empty");
        return;
    }
    const auto playback = discoveryModel.InputFormationAtRate(*selectedRate);
    const auto capture = discoveryModel.OutputFormationAtRate(*selectedRate);
    if (!playback || !capture) {
        ASFW_LOG(AVC, "GenericBeBoB: selected rate %u has incomplete duplex geometry; leaving caps empty",
                 *selectedRate);
        return;
    }

    // ISO input is host-to-device playback; ISO output is device-to-host capture.
    caps_.hostInputPcmChannels = capture->pcmChannels;
    caps_.hostOutputPcmChannels = playback->pcmChannels;
    caps_.deviceToHostAm824Slots = static_cast<uint16_t>(capture->pcmChannels + capture->midiSlots);
    caps_.hostToDeviceAm824Slots = static_cast<uint16_t>(playback->pcmChannels + playback->midiSlots);
    caps_.sampleRateHz = *selectedRate;
    caps_.deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps_.hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel;
    caps_.deviceToHostStreamCount = 1;
    caps_.hostToDeviceStreamCount = 1;
    caps_.deviceToHostStreams[0] = {
        .pcmChannels = capture->pcmChannels,
        .am824Slots = static_cast<uint16_t>(capture->pcmChannels + capture->midiSlots),
    };
    caps_.hostToDeviceStreams[0] = {
        .pcmChannels = playback->pcmChannels,
        .am824Slots = static_cast<uint16_t>(playback->pcmChannels + playback->midiSlots),
    };
    geometryAvailable_ = true;
}

std::vector<uint32_t>
GenericBeBoBProtocol::MakeSupportedRates(const DeviceModel& model) noexcept {
    return model.SupportedRatesHz();
}

} // namespace ASFW::Audio::BeBoB
