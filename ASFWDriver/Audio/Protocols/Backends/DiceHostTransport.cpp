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

kern_return_t DiceIsochHostTransport::PrepareReceive(
    uint8_t channel,
    Driver::HardwareInterface& hardware,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    Encoding::AudioWireFormat wireFormat,
    uint32_t am824Slots) noexcept {
    return isoch_.PrepareReceive(
        channel, hardware, bindingSource, wireFormat, am824Slots);
}

kern_return_t DiceIsochHostTransport::PrepareTransmit(
    uint8_t channel,
    Driver::HardwareInterface& hardware,
    uint8_t sourceId) noexcept {
    return isoch_.PrepareTransmit(channel, hardware, sourceId);
}

kern_return_t DiceIsochHostTransport::StartPreparedReceive() noexcept {
    return isoch_.StartPreparedReceive();
}

kern_return_t DiceIsochHostTransport::StartPreparedTransmit() noexcept {
    return isoch_.StartPreparedTransmit();
}

kern_return_t DiceIsochHostTransport::StopAll() noexcept {
    isoch_.StopAll();
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio
