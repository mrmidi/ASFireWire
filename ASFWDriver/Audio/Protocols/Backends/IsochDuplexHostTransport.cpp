// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "IsochDuplexHostTransport.hpp"

#include "../../../Common/DriverKitOwnership.hpp"
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <utility>

namespace ASFW::Audio {

void IsochDuplexHostTransport::SetTimingLossCallback(
    Driver::IsochService::TimingLossCallback callback) noexcept {
    isoch_.SetTimingLossCallback(std::move(callback));
}

kern_return_t IsochDuplexHostTransport::BeginSplitDuplex(uint64_t guid) noexcept {
    reservations_.ReleaseAll();
    return isoch_.BeginSplitDuplex(guid);
}

kern_return_t IsochDuplexHostTransport::ReservePlaybackResources(uint64_t guid,
                                                               ::ASFW::IRM::IRMClient& irmClient,
                                                               uint64_t allowedChannels,
                                                               uint32_t bandwidthUnits,
                                                               uint8_t& outChannel) noexcept {
    // The IRM, not the device profile, chooses the live channel. A one-bit
    // mask preserves DICE's device-assigned channels; OXFW supplies all usable
    // channels and consumes the returned value for CMP + OHCI programming.
    const Backends::IRMReservationResult reservation =
        reservations_.ReserveAnyPlayback(irmClient, allowedChannels, bandwidthUnits);
    if (reservation.status != kIOReturnSuccess) {
        return reservation.status;
    }
    outChannel = reservation.channel;
    const kern_return_t bookkeeping =
        isoch_.ReservePlaybackResources(guid, irmClient, outChannel, bandwidthUnits);
    if (bookkeeping != kIOReturnSuccess) {
        reservations_.ReleaseAll();
    }
    return bookkeeping;
}

kern_return_t IsochDuplexHostTransport::ReserveCaptureResources(uint64_t guid,
                                                              ::ASFW::IRM::IRMClient& irmClient,
                                                              uint64_t allowedChannels,
                                                              uint32_t bandwidthUnits,
                                                              uint8_t& outChannel) noexcept {
    const Backends::IRMReservationResult reservation =
        reservations_.ReserveAnyCapture(irmClient, allowedChannels, bandwidthUnits);
    if (reservation.status != kIOReturnSuccess) {
        return reservation.status;
    }
    outChannel = reservation.channel;
    const kern_return_t bookkeeping =
        isoch_.ReserveCaptureResources(guid, irmClient, outChannel, bandwidthUnits);
    if (bookkeeping != kIOReturnSuccess) {
        reservations_.ReleaseAll();
    }
    return bookkeeping;
}

kern_return_t IsochDuplexHostTransport::PrepareReceive(
    uint8_t channel, Driver::HardwareInterface& hardware,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    Encoding::AudioWireFormat wireFormat, uint32_t am824Slots, uint32_t streamChannels) noexcept {
    return isoch_.PrepareReceive(channel, hardware, bindingSource, wireFormat, am824Slots,
                                 /*packetCallback=*/nullptr, streamChannels);
}

kern_return_t IsochDuplexHostTransport::PrepareTransmit(uint8_t channel,
                                                      Driver::HardwareInterface& hardware,
                                                      uint8_t sourceId) noexcept {
    return isoch_.PrepareTransmit(channel, hardware, sourceId);
}

kern_return_t IsochDuplexHostTransport::PrepareReceiveStream(
    uint32_t streamIndex, uint8_t channel, Driver::HardwareInterface& hardware,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource, uint32_t channelOffset,
    uint32_t streamChannels, Encoding::AudioWireFormat wireFormat, uint32_t am824Slots) noexcept {
    return isoch_.PrepareReceiveStream(streamIndex, channel, hardware, bindingSource, channelOffset,
                                       streamChannels, wireFormat, am824Slots);
}

kern_return_t IsochDuplexHostTransport::PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                                            Driver::HardwareInterface& hardware,
                                                            uint8_t sourceId) noexcept {
    return isoch_.PrepareTransmitStream(streamIndex, channel, hardware, sourceId);
}

kern_return_t IsochDuplexHostTransport::StartPreparedReceive() noexcept {
    return isoch_.StartPreparedReceive();
}

kern_return_t IsochDuplexHostTransport::StartPreparedTransmit() noexcept {
    return isoch_.StartPreparedTransmit();
}

kern_return_t IsochDuplexHostTransport::StopPreparedReceive() noexcept {
    return isoch_.StopReceive();
}

kern_return_t IsochDuplexHostTransport::StopPreparedTransmit() noexcept {
    return isoch_.StopTransmit();
}

kern_return_t IsochDuplexHostTransport::StopAll() noexcept {
    const kern_return_t status = isoch_.StopAll();
    if (status != kIOReturnSuccess) {
        return status;
    }
    reservations_.ReleaseAll();
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio
