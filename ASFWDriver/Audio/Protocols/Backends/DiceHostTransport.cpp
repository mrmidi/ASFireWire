// SPDX-License-Identifier: Apache-2.0
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
    reservations_.ReleaseAll();
    return isoch_.BeginSplitDuplex(guid);
}

kern_return_t DiceIsochHostTransport::ReservePlaybackResources(uint64_t guid,
                                                               ::ASFW::IRM::IRMClient& irmClient,
                                                               uint8_t channel,
                                                               uint32_t bandwidthUnits) noexcept {
    const kern_return_t status =
        reservations_.ReservePlayback(irmClient, channel, bandwidthUnits);
    if (status != kIOReturnSuccess) {
        return status;
    }
    const kern_return_t bookkeeping =
        isoch_.ReservePlaybackResources(guid, irmClient, channel, bandwidthUnits);
    if (bookkeeping != kIOReturnSuccess) {
        reservations_.ReleaseAll();
    }
    return bookkeeping;
}

kern_return_t DiceIsochHostTransport::ReserveCaptureResources(uint64_t guid,
                                                              ::ASFW::IRM::IRMClient& irmClient,
                                                              uint8_t channel,
                                                              uint32_t bandwidthUnits) noexcept {
    const kern_return_t status =
        reservations_.ReserveCapture(irmClient, channel, bandwidthUnits);
    if (status != kIOReturnSuccess) {
        return status;
    }
    const kern_return_t bookkeeping =
        isoch_.ReserveCaptureResources(guid, irmClient, channel, bandwidthUnits);
    if (bookkeeping != kIOReturnSuccess) {
        reservations_.ReleaseAll();
    }
    return bookkeeping;
}

kern_return_t DiceIsochHostTransport::PrepareReceive(
    uint8_t channel, Driver::HardwareInterface& hardware,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    Encoding::AudioWireFormat wireFormat, uint32_t am824Slots, uint32_t streamChannels) noexcept {
    return isoch_.PrepareReceive(channel, hardware, bindingSource, wireFormat, am824Slots,
                                 /*packetCallback=*/nullptr, streamChannels);
}

kern_return_t DiceIsochHostTransport::PrepareTransmit(uint8_t channel,
                                                      Driver::HardwareInterface& hardware,
                                                      uint8_t sourceId) noexcept {
    return isoch_.PrepareTransmit(channel, hardware, sourceId);
}

kern_return_t DiceIsochHostTransport::PrepareReceiveStream(
    uint32_t streamIndex, uint8_t channel, Driver::HardwareInterface& hardware,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource, uint32_t channelOffset,
    uint32_t streamChannels, Encoding::AudioWireFormat wireFormat, uint32_t am824Slots) noexcept {
    return isoch_.PrepareReceiveStream(streamIndex, channel, hardware, bindingSource, channelOffset,
                                       streamChannels, wireFormat, am824Slots);
}

kern_return_t DiceIsochHostTransport::PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                                            Driver::HardwareInterface& hardware,
                                                            uint8_t sourceId) noexcept {
    return isoch_.PrepareTransmitStream(streamIndex, channel, hardware, sourceId);
}

kern_return_t DiceIsochHostTransport::StartPreparedReceive() noexcept {
    return isoch_.StartPreparedReceive();
}

kern_return_t DiceIsochHostTransport::StartPreparedTransmit() noexcept {
    return isoch_.StartPreparedTransmit();
}

kern_return_t DiceIsochHostTransport::StopPreparedReceive() noexcept {
    return isoch_.StopReceive();
}

kern_return_t DiceIsochHostTransport::StopPreparedTransmit() noexcept {
    return isoch_.StopTransmit();
}

kern_return_t DiceIsochHostTransport::StopAll() noexcept {
    isoch_.StopAll();
    reservations_.ReleaseAll();
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio
