// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../Common/DriverKitOwnership.hpp"
#include "../../Isoch/IsochService.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Midi/Transport/MidiTransportBlock.hpp"
#include "../Devices/AudioIdentity.hpp"
#include "../Devices/ResolvedAudioEndpointProfile.hpp"
#include "../DriverKit/Config/ResolvedAudioStreamProfile.hpp"
#include "../DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "../Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "../Engine/DextTxSlotProvider.hpp"
#include "../Families/BeBoB/MAudio/MAudioPresentationObserver.hpp"
#include "../Families/BeBoB/MAudio/MAudioInternalTxTiming.hpp"
#include "../Families/BeBoB/MAudio/MAudioDuplexPolicy.hpp"
#include "../Ports/ITxPcmSource.hpp"
#include "../Wire/AMDTP/RxSequenceReplay.hpp"
#include "../Shared/TxCycleAnchor.hpp"

#include <DriverKit/IOLib.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>

namespace ASFW::Audio {

namespace Runtime {
class IDirectAudioBindingSource;
}
class IAudioDuplexStreamControl;
class IIsochDuplexHostTransport;

/// Dedicated owner of an endpoint's isochronous stream transport and TX content pump.
///
/// Holds the single TX consumer and content pump per endpoint. Audio and MIDI become
/// independent leases on this shared session:
/// - Standalone MIDI transmits silence for PCM slots and multiplexed MIDI into the MIDI slot.
/// - CoreAudio binds/unbinds ITxPcmSource dynamically without stopping hardware transport.
/// - Hardware stops only when the last lease is released.
class AudioEndpointStreamSession final {
public:
    AudioEndpointStreamSession(
        Devices::AudioEndpointId endpointId,
        Runtime::IDirectAudioBindingSource& endpointRuntime,
        Driver::IsochService& isoch,
        Driver::HardwareInterface& hardware,
        IIsochDuplexHostTransport& hostTransport,
        IAudioDuplexStreamControl& duplexCoordinator,
        const Devices::ResolvedAudioEndpointProfile& profile) noexcept;
    ~AudioEndpointStreamSession() noexcept;

    AudioEndpointStreamSession(const AudioEndpointStreamSession&) = delete;
    AudioEndpointStreamSession& operator=(const AudioEndpointStreamSession&) = delete;

    [[nodiscard]] Devices::AudioEndpointId EndpointId() const noexcept { return endpointId_; }
    [[nodiscard]] bool HasLease() const noexcept { return audioLease_ || midiLease_; }
    [[nodiscard]] bool AudioLeaseActive() const noexcept { return audioLease_; }
    [[nodiscard]] bool MidiLeaseActive() const noexcept { return midiLease_; }
    [[nodiscard]] bool IsStreaming() const noexcept { return streaming_; }

    // Leases
    [[nodiscard]] IOReturn AcquireAudioLease(Ports::ITxPcmSource* pcmSource) noexcept;
    [[nodiscard]] IOReturn ReleaseAudioLease() noexcept;

    [[nodiscard]] IOReturn AcquireMidiLease(
        Midi::MidiTransportBlock* block, uint64_t streamEpoch,
        const Encoding::MpxMidiGeometry& geometry,
        uint32_t sampleRateHz, uint32_t sytIntervalFrames) noexcept;
    [[nodiscard]] IOReturn ReleaseMidiLease() noexcept;

    // Pump callback from IsochService / IsochTransmitContext
    void OnTxPreparation(uint64_t generation) noexcept;

    // Direct access to the stream engines for diagnostics/testing
    [[nodiscard]] Protocols::Audio::DICE::DiceTxStreamEngine& TxStreamEngine() noexcept {
        return txStreamEngine_;
    }
    [[nodiscard]] const Protocols::Audio::DICE::DiceTxStreamEngine& TxStreamEngine() const noexcept {
        return txStreamEngine_;
    }

    [[nodiscard]] DextTxSlotProvider& TxSlotProvider() noexcept {
        return txSlotProvider_;
    }

    [[nodiscard]] uint64_t TxFillCursor() const noexcept {
        return txFillCursor_;
    }

    [[nodiscard]] bool MAudioInternalTimingArmed() const noexcept {
        return mAudioInternalTxTiming_.IsArmed();
    }

private:
    [[nodiscard]] IOReturn StartSessionLocked() noexcept;
    [[nodiscard]] IOReturn StopSessionLocked() noexcept;
    [[nodiscard]] kern_return_t AllocateTxMemoryLocked() noexcept;
    void FreeTxMemoryLocked() noexcept;

    void HandlePendingTimelineEpoch(Runtime::AudioTransportControlBlock* control) noexcept;
    void ObserveTxHardware(Isoch::IsochTxQueueControl* queue,
                           Runtime::AudioTransportControlBlock* control,
                           uint64_t requested, bool useMAudio) noexcept;
    uint32_t PrepareTransmitSlots(Isoch::IsochTxQueueControl* queue,
                                  Runtime::AudioTransportControlBlock* control,
                                  uint64_t startPacketIndex,
                                  uint64_t requiredPacketIndex,
                                  bool useMAudioInternalTiming) noexcept;
    void NotifyLatePayloadOffers(Isoch::IsochTxQueueControl* queue,
                                 uint32_t streamIndex,
                                 bool offered) noexcept;

    Devices::AudioEndpointId endpointId_{};
    Runtime::IDirectAudioBindingSource& endpointRuntime_;
    Driver::IsochService& isoch_;
    Driver::HardwareInterface& hardware_;
    IIsochDuplexHostTransport& hostTransport_;
    IAudioDuplexStreamControl& duplexCoordinator_;
    DriverKit::ResolvedAudioStreamProfile resolvedProfile_{};

    IOLock* lock_{nullptr};
    bool audioLease_{false};
    bool midiLease_{false};
    bool streaming_{false};
    std::atomic<bool> destroyed_{false};
    std::atomic<uint32_t> inFlightCallbacks_{0};

    // Shared descriptors and mapped views
    OSSharedPtr<IOMemoryDescriptor> txPayloadBuffer_[2]{};
    OSSharedPtr<IOMemoryDescriptor> txMetadataBuffer_[2]{};
    OSSharedPtr<IOMemoryDescriptor> txControlBuffer_[2]{};
    OSSharedPtr<IOMemoryMap> txPayloadMap_[2]{};
    OSSharedPtr<IOMemoryMap> txMetadataMap_[2]{};
    OSSharedPtr<IOMemoryMap> txControlMap_[2]{};

    DextTxSlotProvider txSlotProvider_{};
    DextTxSlotProvider txSlotProviderSecondary_{};
    DextTxExecutionTimeline txExecutionTimeline_{};

    Protocols::Audio::DICE::DiceTxStreamEngine txStreamEngine_{};
    Protocols::Audio::DICE::DiceTxStreamEngine txStreamEngineSecondary_{};
    bool txSecondaryActive_{false};

    Runtime::RxSequenceReplayReader txReplayReader_{};
    Families::BeBoB::MAudio::PresentationObserver mAudioPresentationObserver_{};
    Families::BeBoB::MAudio::InternalTxTiming mAudioInternalTxTiming_{};
    uint64_t mAudioTxClockEpoch_{0};
    Shared::TxCorrelationUnwrapState txCorrelationUnwrap_{};

    uint64_t txFillCursor_{0};
    uint64_t txFillCursorAheadEvents_{0};
    uint64_t txNoCycleAnchorEvents_{0};
    uint64_t txNoPresentationOriginEvents_{0};
    uint64_t txReplayResyncs_{0};
    uint64_t txCompletionStampCursor_{0};
    bool txPlanBusTicksValid_{false};
    uint64_t lastTxPlanBusTicks_{0};

    // MIDI lease state
    Midi::MidiTransportBlock* midiBlock_{nullptr};
    uint64_t midiEpoch_{0};
    Encoding::MpxMidiGeometry midiGeometry_{};
    uint32_t midiSampleRateHz_{0};
    uint32_t midiSytIntervalFrames_{0};

    // Audio lease state
    Ports::ITxPcmSource* pcmSource_{nullptr};
};

} // namespace ASFW::Audio
