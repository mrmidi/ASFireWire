// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "DiceHostTransport.hpp"

#include "../../../Common/DriverKitOwnership.hpp"

#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <utility>

namespace ASFW::Audio {

void DiceIsochHostTransport::SetTimingLossCallback(
    Driver::IsochService::TimingLossCallback callback) noexcept {
    isoch_.SetTimingLossCallback(std::move(callback));
}

kern_return_t DiceIsochHostTransport::BeginSplitDuplex(uint64_t guid) noexcept {
    return isoch_.BeginSplitDuplex(guid);
}

kern_return_t DiceIsochHostTransport::ReservePlaybackResources(uint64_t guid,
                                                               ::ASFW::IRM::IRMClient& irmClient,
                                                               uint8_t channel,
                                                               uint32_t bandwidthUnits) noexcept {
    return isoch_.ReservePlaybackResources(guid, irmClient, channel, bandwidthUnits);
}

kern_return_t DiceIsochHostTransport::ReserveCaptureResources(uint64_t guid,
                                                              ::ASFW::IRM::IRMClient& irmClient,
                                                              uint8_t channel,
                                                              uint32_t bandwidthUnits) noexcept {
    return isoch_.ReserveCaptureResources(guid, irmClient, channel, bandwidthUnits);
}

kern_return_t DiceIsochHostTransport::StartReceive(
    uint8_t channel,
    Driver::HardwareInterface& hardware,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    Encoding::AudioWireFormat wireFormat,
    uint32_t am824Slots) noexcept {
    return isoch_.StartReceive(channel, hardware, bindingSource, wireFormat, am824Slots);
}

kern_return_t DiceIsochHostTransport::StartTransmit(
    uint8_t channel,
    Driver::HardwareInterface& hardware,
    uint8_t sourceId,
    uint32_t streamModeRaw,
    uint32_t pcmChannels,
    uint32_t dataBlockSize,
    Encoding::AudioWireFormat wireFormat,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept {
    return isoch_.StartTransmit(channel,
                                hardware,
                                sourceId,
                                streamModeRaw,
                                pcmChannels,
                                dataBlockSize,
                                wireFormat,
                                bindingSource);
}

kern_return_t DiceIsochHostTransport::StopDuplex(
    uint64_t guid,
    ::ASFW::IRM::IRMClient* irmClient) noexcept {
    return isoch_.StopDuplex(guid, irmClient);
}

} // namespace ASFW::Audio
