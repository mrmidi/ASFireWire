// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "IsochDuplexHostTransport.hpp"

#include "../../../Common/DriverKitOwnership.hpp"
#include "../../../Logging/Logging.hpp"
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <new>
#include <utility>

namespace ASFW::Audio {

kern_return_t IsochDuplexHostTransport::AttachReceiveConsumer(
    uint32_t streamIndex, ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    uint32_t channelOffset, bool isSecondary,
    const DirectRxFormatDescriptor& format) noexcept {
    if (streamIndex >= Driver::IsochService::kMaxStreamsPerDirection) {
        return kIOReturnBadArgument;
    }

    using Consumer = ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer;
    Consumer::Configuration configuration{
        .wireFormat = format.wireFormat,
        .am824Slots = format.am824Slots,
        .channelOffset = channelOffset,
        .streamChannels = format.streamChannels,
        .isSecondary = isSecondary,
        .trustConfiguredStride = format.trustConfiguredStride,
        .captureChannelMap = format.captureChannelMap,
    };
    // This is a DriverKit `noexcept` boundary: report allocation failure instead
    // of allowing std::make_unique to terminate the driver process.
    auto consumer = std::unique_ptr<Consumer>(new (std::nothrow) Consumer(bindingSource, configuration));
    if (!consumer) {
        return kIOReturnNoMemory;
    }
    if (format.wireFormat == ::ASFW::Encoding::AudioWireFormat::kMotuV2) {
        motuRxCodecs_[streamIndex] = std::make_unique<::ASFW::Audio::Wire::MotuRxPayloadCodec>(
            format.motuPcmChunks, format.motuPorts);
        consumer->SetPayloadCodec(motuRxCodecs_[streamIndex].get());

        motuRxTimingObservers_[streamIndex] = std::make_unique<::ASFW::Audio::Wire::MotuRxTimingObserver>(
            bindingSource);

        motuRxTimingObservers_[streamIndex]->BindDiagnosticCapture(
            &motuRxDiagnosticCaptures_[streamIndex],
            motuRxCodecs_[streamIndex]->StrideQuadlets(0), diagnosticGuid_);

        consumer->SetTimingObserver(motuRxTimingObservers_[streamIndex].get());
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
        motuRxCodecs_[streamIndex].reset();
        motuRxTimingObservers_[streamIndex].reset();
    }
}

void IsochDuplexHostTransport::SetTimingLossCallback(
    Driver::IsochService::TimingLossCallback callback) noexcept {
    isoch_.SetTimingLossCallback(std::move(callback));
}

kern_return_t IsochDuplexHostTransport::BeginSplitDuplex(uint64_t guid) noexcept {
    reservations_.ReleaseAll();
    diagnosticGuid_ = guid;
    return isoch_.BeginSplitDuplex(guid);
}

kern_return_t IsochDuplexHostTransport::ReservePlaybackResources(
    uint64_t guid, ::ASFW::IRM::IRMClient& irmClient, uint64_t allowedChannels,
    uint32_t packetBandwidthUnits, Backends::IRMReservationResult& outResult) noexcept {
    // The IRM, not the device profile, chooses the live channel. A one-bit
    // mask preserves DICE's device-assigned channels; OXFW supplies all usable
    // channels and consumes the returned value for CMP + OHCI programming.
    outResult = reservations_.ReserveAnyPlayback(irmClient, allowedChannels,
                                                 packetBandwidthUnits);
    if (outResult.status != kIOReturnSuccess) {
        return outResult.status;
    }
    const kern_return_t bookkeeping =
        isoch_.ReservePlaybackResources(guid, irmClient, outResult.channel, outResult.charge.Total());
    if (bookkeeping != kIOReturnSuccess) {
        reservations_.ReleaseAll();
        outResult.status = bookkeeping;
    }
    return bookkeeping;
}

kern_return_t IsochDuplexHostTransport::ReserveCaptureResources(
    uint64_t guid, ::ASFW::IRM::IRMClient& irmClient, uint64_t allowedChannels,
    uint32_t packetBandwidthUnits, Backends::IRMReservationResult& outResult) noexcept {
    outResult = reservations_.ReserveAnyCapture(irmClient, allowedChannels,
                                                packetBandwidthUnits);
    if (outResult.status != kIOReturnSuccess) {
        return outResult.status;
    }
    const kern_return_t bookkeeping =
        isoch_.ReserveCaptureResources(guid, irmClient, outResult.channel, outResult.charge.Total());
    if (bookkeeping != kIOReturnSuccess) {
        reservations_.ReleaseAll();
        outResult.status = bookkeeping;
    }
    return bookkeeping;
}

kern_return_t IsochDuplexHostTransport::PrepareReceive(
    uint8_t channel, Driver::HardwareInterface& hardware,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
    const DirectRxFormatDescriptor& format) noexcept {
    const kern_return_t attached =
        AttachReceiveConsumer(/*streamIndex=*/0, bindingSource, /*channelOffset=*/0,
                              /*isSecondary=*/false, format);
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
                                                      uint8_t sourceId,
                                                      FW::FwSpeed speed) noexcept {
    return isoch_.PrepareTransmit(channel, hardware, sourceId, speed);
}

kern_return_t IsochDuplexHostTransport::PrepareReceiveStream(
    uint32_t streamIndex, uint8_t channel, Driver::HardwareInterface& hardware,
    ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource, uint32_t channelOffset,
    const DirectRxFormatDescriptor& format) noexcept {
    const kern_return_t attached =
        AttachReceiveConsumer(streamIndex, bindingSource, channelOffset,
                              /*isSecondary=*/true, format);
    if (attached != kIOReturnSuccess) {
        return attached;
    }
    const kern_return_t status = isoch_.PrepareReceiveStream(
        streamIndex, channel, hardware, channelOffset, format.streamChannels);
    if (status != kIOReturnSuccess) {
        isoch_.SetReceiveConsumer(streamIndex, nullptr);
        receiveConsumers_[streamIndex].reset();
    }
    return status;
}

kern_return_t IsochDuplexHostTransport::PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                                            Driver::HardwareInterface& hardware,
                                                            uint8_t sourceId,
                                                            FW::FwSpeed speed) noexcept {
    return isoch_.PrepareTransmitStream(streamIndex, channel, hardware, sourceId, speed);
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
