// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IsochDuplexHostTransport.hpp - Host-side isoch orchestration seam

#pragma once

#include "../Engine/Direct/Rx/RxCaptureChannelMap.hpp"

#include "../../Common/WireFormat.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Isoch/IsochService.hpp"
#include "../Devices/AudioIdentity.hpp"
#include "../Engine/Direct/Rx/DirectAudioReceiveConsumer.hpp"
#include "DuplexIRMReservations.hpp"

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSSharedPtr.h>
#include <functional>
#include <memory>
#include "../../Midi/Transport/MidiRingByteSink.hpp"
#include "../Wire/AM824/MpxMidiDemux.hpp"


namespace ASFW::IRM {
class IRMClient;
}

class ASFWAudioNub;

namespace ASFW::Audio {

class IIsochDuplexHostTransport {
  public:
    using EndpointId = Devices::AudioEndpointId;

    virtual ~IIsochDuplexHostTransport() = default;

    [[nodiscard]] virtual kern_return_t BeginSplitDuplex(EndpointId endpointId) noexcept = 0;
    [[nodiscard]] virtual kern_return_t
    ReservePlaybackResources(EndpointId endpointId, ::ASFW::IRM::IRMClient& irmClient,
                             uint64_t allowedChannels, uint32_t bandwidthUnits,
                             uint8_t& outChannel) noexcept = 0;
    [[nodiscard]] virtual kern_return_t
    ReserveCaptureResources(EndpointId endpointId, ::ASFW::IRM::IRMClient& irmClient,
                            uint64_t allowedChannels, uint32_t bandwidthUnits,
                            uint8_t& outChannel) noexcept = 0;
    [[nodiscard]] virtual kern_return_t
    PrepareReceive(uint8_t channel, Driver::HardwareInterface& hardware,
                   ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                   Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
                   uint32_t am824Slots = 0, uint32_t streamChannels = 0,
                   bool acceptHeaderOnlyNoDataTransition = false,
                   bool useTxDerivedPlaybackClock = false,
                   const AudioEngine::Direct::Rx::RxCaptureChannelMap& captureChannelMap =
                       {}) noexcept = 0;
    [[nodiscard]] virtual kern_return_t PrepareTransmit(uint8_t channel,
                                                        Driver::HardwareInterface& hardware,
                                                        uint8_t sourceId) noexcept = 0;
    // Secondary streams (streamIndex >= 1) for multi-stream DICE devices; the
    // master stream uses PrepareReceive/PrepareTransmit above.
    [[nodiscard]] virtual kern_return_t
    PrepareReceiveStream(uint32_t streamIndex, uint8_t channel, Driver::HardwareInterface& hardware,
                         ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                         uint32_t channelOffset, uint32_t streamChannels,
                         Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
                         uint32_t am824Slots = 0) noexcept = 0;
    [[nodiscard]] virtual kern_return_t PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                                              Driver::HardwareInterface& hardware,
                                                              uint8_t sourceId) noexcept = 0;
    [[nodiscard]] virtual kern_return_t StartPreparedReceive() noexcept = 0;
    [[nodiscard]] virtual kern_return_t
    StartPreparedReceiveAtCycle(uint32_t cycleTimer) noexcept = 0;
    [[nodiscard]] virtual kern_return_t StartPreparedTransmit() noexcept = 0;
    [[nodiscard]] virtual kern_return_t StopPreparedReceive() noexcept {
        return kIOReturnUnsupported;
    }
    [[nodiscard]] virtual kern_return_t StopPreparedTransmit() noexcept {
        return kIOReturnUnsupported;
    }
    [[nodiscard]] virtual kern_return_t StopAll() noexcept = 0;
    // A completed bus reset has already invalidated every IRM allocation.  The
    // host still needs to quiesce DMA, but must not issue a release transaction
    // against the newly elected (or absent) IRM.
    [[nodiscard]] virtual kern_return_t StopAllAfterBusReset() noexcept {
        return StopAll();
    }

    // AV/C stream-health signal: is the master RX replay cadence established?
    // Default false for mocks that don't model the RX layer.
    [[nodiscard]] virtual bool IsReceiveReplayEstablished() const noexcept { return false; }
    virtual void NotifyClockAnchorReady(uint64_t generation) noexcept { (void)generation; }
};

class IsochDuplexHostTransport final : public IIsochDuplexHostTransport {
  public:
    using TimingLossCallback = std::function<void(EndpointId endpointId)>;
    using ClockAnchorReadyCallback = std::function<void(uint64_t generation)>;
    // Terminal TX transport fault, tagged with the active endpoint. Delivered
    // on the isoch watchdog/poll thread — the callee must not block.
    using TxTransportFaultCallback =
        std::function<void(EndpointId endpointId, uint32_t statusRaw,
                           uint64_t streamGeneration)>;

    explicit IsochDuplexHostTransport(Driver::IsochService& isoch) noexcept : isoch_(isoch) {}

    void SetTimingLossCallback(TimingLossCallback callback) noexcept;
    void SetTxPreparationCallback(
        Driver::IsochService::TxPreparationCallback callback) noexcept;
    void SetClockAnchorReadyCallback(ClockAnchorReadyCallback callback) noexcept;
    void NotifyClockAnchorReady(uint64_t generation) noexcept override;
    void SetTxTransportFaultCallback(TxTransportFaultCallback callback) noexcept;

    [[nodiscard]] kern_return_t BeginSplitDuplex(EndpointId endpointId) noexcept override;
    [[nodiscard]] kern_return_t ReservePlaybackResources(EndpointId endpointId,
                                                         ::ASFW::IRM::IRMClient& irmClient,
                                                         uint64_t allowedChannels,
                                                         uint32_t bandwidthUnits,
                                                         uint8_t& outChannel) noexcept override;
    [[nodiscard]] kern_return_t ReserveCaptureResources(EndpointId endpointId,
                                                        ::ASFW::IRM::IRMClient& irmClient,
                                                        uint64_t allowedChannels,
                                                        uint32_t bandwidthUnits,
                                                        uint8_t& outChannel) noexcept override;
    [[nodiscard]] kern_return_t
    PrepareReceive(uint8_t channel, Driver::HardwareInterface& hardware,
                   ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                   Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
                   uint32_t am824Slots = 0, uint32_t streamChannels = 0,
                   bool acceptHeaderOnlyNoDataTransition = false,
                   bool useTxDerivedPlaybackClock = false,
                   const AudioEngine::Direct::Rx::RxCaptureChannelMap& captureChannelMap =
                       {}) noexcept override;
    [[nodiscard]] kern_return_t PrepareTransmit(uint8_t channel,
                                                Driver::HardwareInterface& hardware,
                                                uint8_t sourceId) noexcept override;
    [[nodiscard]] kern_return_t
    PrepareReceiveStream(uint32_t streamIndex, uint8_t channel, Driver::HardwareInterface& hardware,
                         ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                         uint32_t channelOffset, uint32_t streamChannels,
                         Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
                         uint32_t am824Slots = 0) noexcept override;
    [[nodiscard]] kern_return_t PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                                      Driver::HardwareInterface& hardware,
                                                      uint8_t sourceId) noexcept override;
    [[nodiscard]] kern_return_t StartPreparedReceive() noexcept override;
    [[nodiscard]] kern_return_t
    StartPreparedReceiveAtCycle(uint32_t cycleTimer) noexcept override;
    [[nodiscard]] kern_return_t StartPreparedTransmit() noexcept override;
    [[nodiscard]] kern_return_t StopPreparedReceive() noexcept override;
    [[nodiscard]] kern_return_t StopPreparedTransmit() noexcept override;
    [[nodiscard]] kern_return_t StopAll() noexcept override;
    [[nodiscard]] kern_return_t StopAllAfterBusReset() noexcept override;
    [[nodiscard]] bool IsReceiveReplayEstablished() const noexcept override;

    /// Point receive-side MIDI extraction at an endpoint's byte rings.
    ///
    /// `block` is the mapped MidiTransportBlock the MIDI nub owns; passing
    /// nullptr detaches. Only stream 0 carries MIDI in this milestone, matching
    /// Linux's dice-midi, so only that consumer is given the extraction.
    /// `wake` is invoked once per receive batch that produced bytes.
    void SetMidiReceiveTransport(
        ASFW::Midi::MidiTransportBlock* block, uint64_t streamEpoch,
        const ASFW::Encoding::MpxMidiGeometry& geometry,
        std::function<void()> wake) noexcept;

  private:
    [[nodiscard]] kern_return_t AttachReceiveConsumer(
        uint32_t streamIndex,
        ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
        Encoding::AudioWireFormat wireFormat, uint32_t am824Slots,
        uint32_t channelOffset, uint32_t streamChannels, bool isSecondary,
        bool acceptHeaderOnlyNoDataTransition,
        bool useTxDerivedPlaybackClock,
        const AudioEngine::Direct::Rx::RxCaptureChannelMap& captureChannelMap) noexcept;
    void DetachReceiveConsumers() noexcept;

    Driver::IsochService& isoch_;
    // Receive-side MIDI. The sink writes into rings the MIDI nub owns; this
    // object only borrows them, and DetachReceiveConsumers unbinds before any
    // consumer that could still call into it is destroyed.
    ASFW::Midi::MidiRingByteSink midiSink_{};
    ASFW::Encoding::MpxMidiGeometry midiGeometry_{};
    ASFW::Encoding::MpxMidiDemuxCounters midiCounters_{};
    std::function<void()> midiWake_{};
    std::unique_ptr<ASFW::AudioEngine::Direct::Rx::DirectAudioReceiveConsumer>
        receiveConsumers_[Driver::IsochService::kMaxStreamsPerDirection]{};
    Duplex::DuplexIRMReservationPair reservations_{};
    EndpointId activeEndpoint_{};
    TimingLossCallback timingLossCallback_{};
    ClockAnchorReadyCallback clockAnchorReadyCallback_{};
    TxTransportFaultCallback txTransportFaultCallback_{};
};

} // namespace ASFW::Audio
