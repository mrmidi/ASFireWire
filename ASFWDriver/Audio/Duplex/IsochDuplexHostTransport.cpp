// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "IsochDuplexHostTransport.hpp"

#include "../../Common/DriverKitOwnership.hpp"
#include "../../Logging/Logging.hpp"
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <new>
#include <utility>

namespace ASFW::Audio {

kern_return_t IsochDuplexHostTransport::AttachReceiveConsumer(
    uint32_t streamIndex, ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    Encoding::AudioWireFormat wireFormat, uint32_t am824Slots, uint32_t channelOffset,
    uint32_t streamChannels, bool isSecondary,
    bool acceptHeaderOnlyNoDataTransition,
    bool useTxDerivedPlaybackClock) noexcept {
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
        .acceptHeaderOnlyNoDataTransition = acceptHeaderOnlyNoDataTransition,
        .useTxDerivedPlaybackClock = useTxDerivedPlaybackClock,
    };
    // This is a DriverKit `noexcept` boundary: report allocation failure instead
    // of allowing std::make_unique to terminate the driver process.
    auto consumer = std::unique_ptr<Consumer>(new (std::nothrow) Consumer(bindingSource, configuration));
    if (!consumer) {
        return kIOReturnNoMemory;
    }
    consumer->SetTimingLossCallback([this] {
        if (timingLossCallback_ && activeEndpoint_) {
            timingLossCallback_(activeEndpoint_);
        }
    });
    consumer->SetZtsAnchorReadyCallback(
        [this](uint64_t generation) {
            if (clockAnchorReadyCallback_) {
                clockAnchorReadyCallback_(generation);
            }
        });
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
    TimingLossCallback callback) noexcept {
    timingLossCallback_ = std::move(callback);
}

void IsochDuplexHostTransport::SetTxPreparationCallback(
    Driver::IsochService::TxPreparationCallback callback) noexcept {
    isoch_.SetTxPreparationCallback(std::move(callback));
}

void IsochDuplexHostTransport::SetClockAnchorReadyCallback(
    ClockAnchorReadyCallback callback) noexcept {
    clockAnchorReadyCallback_ = std::move(callback);
}

kern_return_t IsochDuplexHostTransport::BeginSplitDuplex(EndpointId endpointId) noexcept {
    if (activeEndpoint_ && activeEndpoint_ != endpointId) {
        return kIOReturnBusy;
    }
    reservations_.ReleaseAll();
    activeEndpoint_ = endpointId;
    return kIOReturnSuccess;
}

kern_return_t IsochDuplexHostTransport::ReservePlaybackResources(EndpointId endpointId,
                                                               ::ASFW::IRM::IRMClient& irmClient,
                                                               uint64_t allowedChannels,
                                                               uint32_t bandwidthUnits,
                                                               uint8_t& outChannel) noexcept {
    if (activeEndpoint_ != endpointId) {
        return kIOReturnNotPrivileged;
    }
    // The IRM, not the device profile, chooses the live channel. A one-bit
    // mask preserves DICE's device-assigned channels; OXFW supplies all usable
    // channels and consumes the returned value for CMP + OHCI programming.
    const Duplex::IRMReservationResult reservation =
        reservations_.ReserveAnyPlayback(irmClient, allowedChannels, bandwidthUnits);
    if (reservation.status != kIOReturnSuccess) {
        return reservation.status;
    }
    outChannel = reservation.channel;
    return kIOReturnSuccess;
}

kern_return_t IsochDuplexHostTransport::ReserveCaptureResources(EndpointId endpointId,
                                                              ::ASFW::IRM::IRMClient& irmClient,
                                                              uint64_t allowedChannels,
                                                              uint32_t bandwidthUnits,
                                                              uint8_t& outChannel) noexcept {
    if (activeEndpoint_ != endpointId) {
        return kIOReturnNotPrivileged;
    }
    const Duplex::IRMReservationResult reservation =
        reservations_.ReserveAnyCapture(irmClient, allowedChannels, bandwidthUnits);
    if (reservation.status != kIOReturnSuccess) {
        return reservation.status;
    }
    outChannel = reservation.channel;
    return kIOReturnSuccess;
}

kern_return_t IsochDuplexHostTransport::PrepareReceive(
    uint8_t channel, Driver::HardwareInterface& hardware,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    Encoding::AudioWireFormat wireFormat, uint32_t am824Slots, uint32_t streamChannels,
    bool acceptHeaderOnlyNoDataTransition,
    bool useTxDerivedPlaybackClock) noexcept {
    const kern_return_t attached =
        AttachReceiveConsumer(/*streamIndex=*/0, bindingSource, wireFormat, am824Slots,
                              /*channelOffset=*/0, streamChannels, /*isSecondary=*/false,
                              acceptHeaderOnlyNoDataTransition,
                              useTxDerivedPlaybackClock);
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
                              streamChannels, /*isSecondary=*/true,
                              /*acceptHeaderOnlyNoDataTransition=*/false,
                              /*useTxDerivedPlaybackClock=*/false);
    if (attached != kIOReturnSuccess) {
        return attached;
    }
    const kern_return_t status =
        isoch_.PrepareReceiveStream(streamIndex, channel, hardware);
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

kern_return_t IsochDuplexHostTransport::StartPreparedReceiveAtCycle(
    uint32_t cycleTimer) noexcept {
    return isoch_.StartPreparedReceiveAtCycle(cycleTimer);
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
    if (isoch_.HardwareGone()) {
        // Provider removal invalidates the old bus generation. Do not queue
        // IRM release transactions after the async subsystem has quiesced.
        reservations_.InvalidateAfterGenerationChange();
        ASFW_LOG(Isoch,
                 "[Lifecycle] IsochDuplexHostTransport StopAll hardware-gone "
                 "action=invalidate-irm-reservations");
    } else {
        reservations_.ReleaseAll();
    }
    activeEndpoint_ = {};
    return kIOReturnSuccess;
}

kern_return_t IsochDuplexHostTransport::StopAllAfterBusReset() noexcept {
    const kern_return_t status = isoch_.StopAll();
    if (status != kIOReturnSuccess) {
        return status;
    }

    DetachReceiveConsumers();
    // A reset is the allocation invalidation boundary: retain no local lease
    // and do not attempt an IRM release in the new generation. Cross-validated
    // with IOFireWireFamily/IOFWIsochChannel.cpp:1243-1269.
    reservations_.InvalidateAfterGenerationChange();
    activeEndpoint_ = {};
    ASFW_LOG(Isoch,
             "[Lifecycle] IsochDuplexHostTransport StopAll generation-invalidated "
             "action=invalidate-irm-reservations");
    return kIOReturnSuccess;
}

bool IsochDuplexHostTransport::IsReceiveReplayEstablished() const noexcept {
    // Replay cadence is content policy. The transport owns no audio state;
    // this audio-side adapter owns the master receive consumer that does.
    const auto& consumer = receiveConsumers_[0];
    return consumer && consumer->IsReplayEstablished();
}

} // namespace ASFW::Audio
