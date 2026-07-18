// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "IsochDuplexHostTransport.hpp"

#include "../../../Common/DriverKitOwnership.hpp"
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <new>
#include <utility>

namespace ASFW::Audio {

kern_return_t IsochDuplexHostTransport::AttachReceiveConsumer(
    uint32_t streamIndex, ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    Encoding::AudioWireFormat wireFormat, uint32_t am824Slots, uint32_t channelOffset,
    uint32_t streamChannels, bool isSecondary) noexcept {
    if (streamIndex >= Driver::IsochService::kMaxStreamsPerDirection) {
        return kIOReturnBadArgument;
    }

    using Consumer = ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer;
    Consumer::Configuration configuration{
        .wireFormat = wireFormat,
        .am824Slots = am824Slots,
        .channelOffset = channelOffset,
        .streamChannels = streamChannels,
        .isSecondary = isSecondary,
    };
    // This is a DriverKit `noexcept` boundary: report allocation failure instead
    // of allowing std::make_unique to terminate the driver process.
    auto consumer = std::unique_ptr<Consumer>(new (std::nothrow) Consumer(bindingSource, configuration));
    if (!consumer) {
        return kIOReturnNoMemory;
    }
    consumer->SetTimingLossCallback([this] { isoch_.NotifyReceiveTimingLoss(); });
    consumer->SetReplayReadyCallback([this] { isoch_.NotifyReceiveReplayEstablished(); });
    consumer->SetZtsAnchorReadyCallback(
        [this](uint64_t generation) { isoch_.NotifyReceiveZtsAnchor(generation); });
    receiveConsumers_[streamIndex] = std::move(consumer);
    isoch_.SetReceiveConsumer(streamIndex, receiveConsumers_[streamIndex].get());
    return kIOReturnSuccess;
}

void IsochDuplexHostTransport::DetachReceiveConsumers() noexcept {
    for (uint32_t streamIndex = 0;
         streamIndex < Driver::IsochService::kMaxStreamsPerDirection; ++streamIndex) {
        isoch_.SetReceiveConsumer(streamIndex, nullptr);
        receiveConsumers_[streamIndex].reset();
    }
}

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
    const kern_return_t attached =
        AttachReceiveConsumer(/*streamIndex=*/0, bindingSource, wireFormat, am824Slots,
                              /*channelOffset=*/0, streamChannels, /*isSecondary=*/false);
    if (attached != kIOReturnSuccess) {
        return attached;
    }
    const kern_return_t status = isoch_.PrepareReceive(channel, hardware);
    if (status != kIOReturnSuccess) {
        DetachReceiveConsumers();
    }
    return status;
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
    const kern_return_t attached =
        AttachReceiveConsumer(streamIndex, bindingSource, wireFormat, am824Slots, channelOffset,
                              streamChannels, /*isSecondary=*/true);
    if (attached != kIOReturnSuccess) {
        return attached;
    }
    const kern_return_t status = isoch_.PrepareReceiveStream(
        streamIndex, channel, hardware, channelOffset, streamChannels);
    if (status != kIOReturnSuccess) {
        isoch_.SetReceiveConsumer(streamIndex, nullptr);
        receiveConsumers_[streamIndex].reset();
    }
    return status;
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
    const kern_return_t status = isoch_.StopReceive();
    if (status == kIOReturnSuccess) {
        DetachReceiveConsumers();
    }
    return status;
}

kern_return_t IsochDuplexHostTransport::StopPreparedTransmit() noexcept {
    return isoch_.StopTransmit();
}

kern_return_t IsochDuplexHostTransport::StopAll() noexcept {
    const kern_return_t status = isoch_.StopAll();
    if (status != kIOReturnSuccess) {
        return status;
    }
    DetachReceiveConsumers();
    reservations_.ReleaseAll();
    return kIOReturnSuccess;
}

bool IsochDuplexHostTransport::IsReceiveReplayEstablished() const noexcept {
    // Replay cadence is content policy. The transport owns no audio state;
    // this audio-side adapter owns the master receive consumer that does.
    const auto& consumer = receiveConsumers_[0];
    return consumer && consumer->IsReplayEstablished();
}

} // namespace ASFW::Audio
