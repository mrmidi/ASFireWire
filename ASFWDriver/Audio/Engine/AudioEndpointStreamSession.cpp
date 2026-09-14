// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "AudioEndpointStreamSession.hpp"
#include "../Core/AudioEndpointRuntime.hpp"
#include "../Duplex/AudioDuplexCoordinator.hpp"
#include "../Duplex/IsochDuplexHostTransport.hpp"
#include "../Wire/IEC61883/Syt.hpp"
#include "../Runtime/TxCompletionStampDrain.hpp"
#include "../../Common/TimingUtils.hpp"
#include "../../Logging/Logging.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace ASFW::Audio {
namespace {

using AmdtpDisposition = Protocols::Audio::AMDTP::AmdtpPacketDisposition;
using TxPlan = Protocols::Audio::AMDTP::TxPresentationPlan;
using PrepareResult = Protocols::Audio::DICE::TxSlotPrepareResult;
using FillResult = Protocols::Audio::DICE::TxSlotFillResult;

constexpr uint64_t kBusWrapTicks =
    static_cast<uint64_t>(Timing::kFWTimeWrapSeconds) * Timing::kTicksPerSecond;

[[nodiscard]] bool IsRecoverableReplayDesync(
    Runtime::RxSequenceReplayReadFailure failure) noexcept {
    using Failure = Runtime::RxSequenceReplayReadFailure;
    switch (failure) {
    case Failure::kEpochChanged:
    case Failure::kHistoryOverwritten:
    case Failure::kSlotSequenceMismatch:
    case Failure::kSlotEpochMismatch:
    case Failure::kSlotChanged:
        return true;
    case Failure::kNone:
    case Failure::kReaderInactive:
    case Failure::kAheadOfProducer:
        return false;
    }
    return false;
}

[[nodiscard]] bool IsPowerOfTwo(uint64_t value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

void UpdateMaximum(std::atomic<uint64_t>& target, uint64_t value) noexcept {
    uint64_t previous = target.load(std::memory_order_relaxed);
    while (value > previous &&
           !target.compare_exchange_weak(previous, value,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
    }
}

[[nodiscard]] uint32_t HeadroomBucket(uint64_t cycles) noexcept {
    return cycles <= 6 ? 0 : cycles <= 12 ? 1 : cycles <= 24 ? 2
        : cycles <= 48 ? 3 : 4;
}

void TraceCycle(Runtime::AudioTransportControlBlock& control,
                const TxPlan& plan,
                PrepareResult pcmResult,
                uint64_t completionCursor) noexcept {
    const uint64_t headroom = plan.cycleOrdinal > completionCursor
        ? plan.cycleOrdinal - completionCursor : 0;
    control.txDeadlineHeadroomHistogram[HeadroomBucket(headroom)].fetch_add(
        1, std::memory_order_relaxed);
    control.txCycleTrace.Publish({
        .epoch = plan.epoch,
        .cycleOrdinal = plan.cycleOrdinal,
        .firstAudioFrame = plan.firstAudioFrame,
        .presentationBusTicks = plan.presentationBusTicks,
        .prepareCycle = completionCursor,
        .publishCycle = completionCursor,
        .ownershipCycle = plan.cycleOrdinal >
                Shared::AudioTimingGeometry::kTxOwnershipGuardCycleSlots
            ? plan.cycleOrdinal - Shared::AudioTimingGeometry::kTxOwnershipGuardCycleSlots
            : 0,
        .completionCycle = 0,
        .frameCount = plan.frameCount,
        .pcmResult = static_cast<uint32_t>(pcmResult),
        .disposition = static_cast<uint32_t>(plan.disposition),
        .deadlineHeadroomCycles = headroom > UINT32_MAX
            ? UINT32_MAX : static_cast<uint32_t>(headroom),
    });
}

[[nodiscard]] bool UnwrapBusTicks(uint64_t rawTicks,
                                  bool& initialized,
                                  uint64_t& lastTicks,
                                  uint64_t& outTicks) noexcept {
    rawTicks %= kBusWrapTicks;
    uint64_t unwrapped = rawTicks;
    if (initialized) {
        unwrapped = (lastTicks / kBusWrapTicks) * kBusWrapTicks + rawTicks;
        if (unwrapped + kBusWrapTicks / 2 < lastTicks) {
            unwrapped += kBusWrapTicks;
        }
        if (unwrapped < lastTicks) return false;
    }
    initialized = true;
    lastTicks = unwrapped;
    outTicks = unwrapped;
    return true;
}

[[nodiscard]] uint16_t SytForPresentation(uint64_t busTicks) noexcept {
    const uint32_t cycle = static_cast<uint32_t>(
        (busTicks / Timing::kTicksPerCycle) % Timing::kCyclesPerSecond);
    const uint32_t offset = static_cast<uint32_t>(
        busTicks % Timing::kTicksPerCycle);
    return Protocols::Audio::IEC61883::SytFormatter::EncodeCycleOffset(cycle, offset);
}

[[nodiscard]] constexpr uint64_t BusTicksToMicros(uint64_t ticks) noexcept {
    return (ticks * 125ULL) / 3072ULL;
}

} // namespace

AudioEndpointStreamSession::AudioEndpointStreamSession(
    Devices::AudioEndpointId endpointId,
    Runtime::IDirectAudioBindingSource& endpointRuntime,
    Driver::IsochService& isoch,
    Driver::HardwareInterface& hardware,
    IIsochDuplexHostTransport& hostTransport,
    IAudioDuplexStreamControl& duplexCoordinator,
    const Devices::ResolvedAudioEndpointProfile& profile) noexcept
    : endpointId_(endpointId)
    , endpointRuntime_(endpointRuntime)
    , isoch_(isoch)
    , hardware_(hardware)
    , hostTransport_(hostTransport)
    , duplexCoordinator_(duplexCoordinator) {
    resolvedProfile_.Load(profile);
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AudioEndpointStreamSession: lock alloc failed");
    }
}

AudioEndpointStreamSession::~AudioEndpointStreamSession() noexcept {
    // 1. Mark destroyed so no new callback enters.
    destroyed_.store(true, std::memory_order_release);

    // 2. Quiesce the callback in IsochService and its transmit contexts.
    // This clears the callback and drains any active DoRefillOnce invocation.
    isoch_.QuiesceTxPreparation();
    mAudioPresentationObserver_.Disarm();
    mAudioInternalTxTiming_.Disarm();

    // 3. Drain any in-flight OnTxPreparation invocation before destroying session state.
    while (inFlightCallbacks_.load(std::memory_order_acquire) != 0) {
        IODelay(5);
    }

    // 4. Quiesce transport and release resources.
    if (streaming_) {
        const IOReturn status = StopSessionLocked();
        if (status != kIOReturnSuccess) {
            ASFW_LOG_ERROR(
                Audio,
                "AudioEndpointStreamSession: destructor StopSessionLocked failed 0x%x; forcing transport halt",
                status);
            const kern_return_t stopTxKr = isoch_.StopTransmit();
            if (stopTxKr == kIOReturnSuccess || hardware_.HardwareGone()) {
                streaming_ = false;
                Runtime::DirectAudioBindingSnapshot bindingSnapshot{};
                if (endpointRuntime_.CopyDirectAudioBinding(bindingSnapshot) && bindingSnapshot.control) {
                    bindingSnapshot.control->isSessionStreaming.store(false, std::memory_order_release);
                    bindingSnapshot.control->hardwareTimeline.Reset();
                }
                FreeTxMemoryLocked();
            } else {
                ASFW_LOG_ERROR(
                    Audio,
                    "AudioEndpointStreamSession: destructor StopTransmit failed 0x%x; retaining TX resources to protect DMA",
                    stopTxKr);
                for (size_t i = 0; i < 2; ++i) {
                    if (txPayloadBuffer_[i]) (void)txPayloadBuffer_[i].detach();
                    if (txMetadataBuffer_[i]) (void)txMetadataBuffer_[i].detach();
                    if (txControlBuffer_[i]) (void)txControlBuffer_[i].detach();
                    if (txPayloadMap_[i]) (void)txPayloadMap_[i].detach();
                    if (txMetadataMap_[i]) (void)txMetadataMap_[i].detach();
                    if (txControlMap_[i]) (void)txControlMap_[i].detach();
                }
            }
        }
    }
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

IOReturn AudioEndpointStreamSession::AcquireAudioLease(Ports::ITxPcmSource* pcmSource) noexcept {
    if (!lock_) return kIOReturnNotReady;
    IOLockLock(lock_);
    pcmSource_ = pcmSource;
    txStreamEngine_.BindPcmSource(pcmSource_);
    if (txSecondaryActive_) {
        txStreamEngineSecondary_.BindPcmSource(pcmSource_);
    }
    audioLease_ = true;

    if (!streaming_) {
        const IOReturn status = StartSessionLocked();
        if (status != kIOReturnSuccess) {
            audioLease_ = false;
            pcmSource_ = nullptr;
            txStreamEngine_.BindPcmSource(nullptr);
            if (txSecondaryActive_) {
                txStreamEngineSecondary_.BindPcmSource(nullptr);
            }
            IOLockUnlock(lock_);
            return status;
        }
    }
    IOLockUnlock(lock_);
    return kIOReturnSuccess;
}

IOReturn AudioEndpointStreamSession::ReleaseAudioLease() noexcept {
    if (!lock_) return kIOReturnNotReady;
    IOLockLock(lock_);
    if (!audioLease_) {
        IOLockUnlock(lock_);
        return kIOReturnSuccess;
    }
    pcmSource_ = nullptr;
    txStreamEngine_.BindPcmSource(nullptr);
    if (txSecondaryActive_) {
        txStreamEngineSecondary_.BindPcmSource(nullptr);
    }

    if (!midiLease_ && streaming_) {
        const IOReturn status = StopSessionLocked();
        if (status != kIOReturnSuccess) {
            IOLockUnlock(lock_);
            return status;
        }
    }
    audioLease_ = false;
    IOLockUnlock(lock_);
    return kIOReturnSuccess;
}

IOReturn AudioEndpointStreamSession::AcquireMidiLease(
    Midi::MidiTransportBlock* block, uint64_t streamEpoch,
    const Encoding::MpxMidiGeometry& geometry,
    uint32_t sampleRateHz, uint32_t sytIntervalFrames) noexcept {
    if (!lock_) return kIOReturnNotReady;
    IOLockLock(lock_);
    midiBlock_ = block;
    midiEpoch_ = streamEpoch;
    midiGeometry_ = geometry;
    midiSampleRateHz_ = sampleRateHz;
    midiSytIntervalFrames_ = sytIntervalFrames;

    txStreamEngine_.SetMidiTransport(
        midiBlock_, midiEpoch_, midiGeometry_, midiSampleRateHz_, midiSytIntervalFrames_);
    midiLease_ = true;

    if (!streaming_) {
        const IOReturn status = StartSessionLocked();
        if (status != kIOReturnSuccess) {
            midiLease_ = false;
            midiBlock_ = nullptr;
            txStreamEngine_.SetMidiTransport(nullptr, 0, {}, 0, 0);
            IOLockUnlock(lock_);
            return status;
        }
    }
    IOLockUnlock(lock_);
    return kIOReturnSuccess;
}

IOReturn AudioEndpointStreamSession::ReleaseMidiLease() noexcept {
    if (!lock_) return kIOReturnNotReady;
    IOLockLock(lock_);
    if (!midiLease_) {
        IOLockUnlock(lock_);
        return kIOReturnSuccess;
    }
    midiBlock_ = nullptr;
    txStreamEngine_.SetMidiTransport(nullptr, 0, {}, 0, 0);

    if (!audioLease_ && streaming_) {
        const IOReturn status = StopSessionLocked();
        if (status != kIOReturnSuccess) {
            IOLockUnlock(lock_);
            return status;
        }
    }
    midiLease_ = false;
    IOLockUnlock(lock_);
    return kIOReturnSuccess;
}

IOReturn AudioEndpointStreamSession::StartSessionLocked() noexcept {
    const kern_return_t allocKr = AllocateTxMemoryLocked();
    if (allocKr != kIOReturnSuccess) {
        return allocKr;
    }

    // Prefill TX ring with NO-DATA packets
    txFillCursor_ = 0;
    const uint32_t slots = txSlotProvider_.numSlots;
    Runtime::DirectAudioBindingSnapshot bindingSnapshot{};
    (void)endpointRuntime_.CopyDirectAudioBinding(bindingSnapshot);
    auto* control = bindingSnapshot.control;
    uint64_t epoch = control ? control->hardwareTimeline.Epoch() : 0;
    const bool useMAudio = Families::BeBoB::MAudio::UsesSpecialDuplexPolicy(
        resolvedProfile_.Value().profileBuilder);

    if (control && epoch == 0) {
        const auto timelineSource =
            (useMAudio || resolvedProfile_.RxChannelCount() == 0)
                ? ASFW::Audio::Runtime::HardwareTimelineSource::Transmit
                : ASFW::Audio::Runtime::HardwareTimelineSource::Receive;
        const uint32_t sampleRate = txStreamEngine_.StreamConfig().sampleRate != 0
            ? txStreamEngine_.StreamConfig().sampleRate
            : resolvedProfile_.Value().currentSampleRateHz;
        epoch = control->hardwareTimeline.BeginEpoch(
            timelineSource,
            ASFW::Audio::Runtime::HardwareTimelineDiscontinuity::StartIO,
            sampleRate,
            0);
        if (epoch == 0) {
            ASFW_LOG_ERROR(Audio, "AudioEndpointStreamSession: BeginEpoch failed");
            FreeTxMemoryLocked();
            return kIOReturnUnsupported;
        }
        control->rxTransferDelayTicks.store(
            resolvedProfile_.RxTransferDelayTicks(sampleRate),
            std::memory_order_relaxed);
        control->txTransferDelayTicks.store(
            resolvedProfile_.TxTransferDelayTicks(sampleRate),
            std::memory_order_relaxed);
    }

    if (useMAudio) {
        const auto startEpoch = Families::BeBoB::MAudio::StartEpoch{
            .value = ++mAudioTxClockEpoch_};
        const auto& txStreamConfig = txStreamEngine_.StreamConfig();
        if (!mAudioPresentationObserver_.Arm(
                startEpoch,
                epoch,
                txStreamConfig.sampleRate,
                Families::BeBoB::MAudio::InternalTxTransferDelayTicks(
                    txStreamConfig.sampleRate,
                    txStreamConfig.framesPerDataPacket))) {
            ASFW_LOG_ERROR(
                Audio,
                "AudioEndpointStreamSession: StartSessionLocked failed - M-Audio presentation observer arm failed rate=%u sytInterval=%u",
                txStreamConfig.sampleRate,
                txStreamConfig.framesPerDataPacket);
            FreeTxMemoryLocked();
            return kIOReturnNotReady;
        }
        if (!mAudioInternalTxTiming_.Arm(
                startEpoch,
                txStreamConfig.sampleRate,
                txStreamConfig.framesPerDataPacket)) {
            ASFW_LOG_ERROR(
                Audio,
                "AudioEndpointStreamSession: StartSessionLocked failed - M-Audio internal TX timing arm failed rate=%u frames=%u",
                txStreamConfig.sampleRate,
                txStreamConfig.framesPerDataPacket);
            mAudioPresentationObserver_.Disarm();
            FreeTxMemoryLocked();
            return kIOReturnUnsupported;
        }
    }

    const TxPlan noData{
        .epoch = epoch,
        .cycleOrdinal = 0,
        .firstAudioFrame = 0,
        .frameCount = 0,
        .presentationBusTicks = 0,
        .disposition = AmdtpDisposition::NoData,
    };
    for (uint32_t packet = 0; packet < slots; ++packet) {
        TxPlan plan = noData;
        plan.cycleOrdinal = packet;
        if (txStreamEngine_.PrepareTransmitSlot(packet, plan, 0, 0xFFFF) !=
            PrepareResult::Prepared) {
            break;
        }
        if (txSecondaryActive_ &&
            txStreamEngineSecondary_.PrepareTransmitSlot(packet, plan, 0, 0xFFFF) !=
            PrepareResult::Prepared) {
            break;
        }
    }

    // Register TX preparation callback on IsochService
    isoch_.SetTxPreparationCallback([this](uint64_t generation) {
        OnTxPreparation(generation);
    });

    const IOReturn status = duplexCoordinator_.StartStreaming(endpointId_);
    if (status != kIOReturnSuccess) {
        if (useMAudio) {
            mAudioPresentationObserver_.Disarm();
            mAudioInternalTxTiming_.Disarm();
        }
        isoch_.SetTxPreparationCallback({});
        FreeTxMemoryLocked();
        return status;
    }

    streaming_ = true;
    if (control) {
        control->isSessionStreaming.store(true, std::memory_order_release);
    }
    return kIOReturnSuccess;
}

IOReturn AudioEndpointStreamSession::StopSessionLocked() noexcept {
    const IOReturn status = duplexCoordinator_.StopStreaming(endpointId_);
    if (status != kIOReturnSuccess) {
        ASFW_LOG_ERROR(
            Audio,
            "AudioEndpointStreamSession: StopStreaming failed 0x%x; preserving TX resources",
            status);
        return status;
    }
    streaming_ = false;
    isoch_.SetTxPreparationCallback({});
    mAudioPresentationObserver_.Disarm();
    mAudioInternalTxTiming_.Disarm();
    Runtime::DirectAudioBindingSnapshot bindingSnapshot{};
    if (endpointRuntime_.CopyDirectAudioBinding(bindingSnapshot) && bindingSnapshot.control) {
        bindingSnapshot.control->isSessionStreaming.store(false, std::memory_order_release);
        bindingSnapshot.control->hardwareTimeline.Reset();
    }
    FreeTxMemoryLocked();
    return kIOReturnSuccess;
}

kern_return_t AudioEndpointStreamSession::AllocateTxMemoryLocked() noexcept {
    ASFW::Isoch::Audio::AudioStreamConfig txConfig{};
    if (!resolvedProfile_.BuildDefaultTxStreamConfig(txConfig)) {
        ASFW_LOG_ERROR(Audio, "AudioEndpointStreamSession: BuildDefaultTxStreamConfig failed");
        return kIOReturnError;
    }
    const uint32_t numSlots = Shared::AudioTimingGeometry::kTxSharedSlotPackets;
    const uint32_t maxPacketBytes = 8u + static_cast<uint32_t>(txConfig.framesPerDataPacket) * txConfig.dbs * 4u;
    const uint32_t interruptInterval = Shared::AudioTimingGeometry::kTimingGroupPackets;

    IOMemoryDescriptor* rawPayload = nullptr;
    IOMemoryDescriptor* rawMetadata = nullptr;
    IOMemoryDescriptor* rawControl = nullptr;

    kern_return_t kr = isoch_.AllocateTxIsochResources(
        0, numSlots, maxPacketBytes, interruptInterval,
        &rawPayload, &rawMetadata, &rawControl);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio, "AudioEndpointStreamSession: AllocateTxIsochResources(0) failed 0x%x", kr);
        return kr;
    }

    txPayloadBuffer_[0] = ASFW::Common::AdoptRetained(rawPayload);
    txMetadataBuffer_[0] = ASFW::Common::AdoptRetained(rawMetadata);
    txControlBuffer_[0] = ASFW::Common::AdoptRetained(rawControl);

    kr = ASFW::Common::CreateSharedMapping(txPayloadBuffer_[0], txPayloadMap_[0]);
    if (kr != kIOReturnSuccess) { FreeTxMemoryLocked(); return kr; }
    kr = ASFW::Common::CreateSharedMapping(txMetadataBuffer_[0], txMetadataMap_[0]);
    if (kr != kIOReturnSuccess) { FreeTxMemoryLocked(); return kr; }
    kr = ASFW::Common::CreateSharedMapping(txControlBuffer_[0], txControlMap_[0]);
    if (kr != kIOReturnSuccess) { FreeTxMemoryLocked(); return kr; }

    uint8_t* payloadBase = reinterpret_cast<uint8_t*>(txPayloadMap_[0]->GetAddress());
    auto* metadataRing = reinterpret_cast<ASFW::Isoch::IsochTxPacketMeta*>(txMetadataMap_[0]->GetAddress());
    auto* queueControl = reinterpret_cast<ASFW::Isoch::IsochTxQueueControl*>(txControlMap_[0]->GetAddress());

    queueControl->ResetProducerForStart();

    Runtime::DirectAudioBindingSnapshot bindingSnapshot{};
    (void)endpointRuntime_.CopyDirectAudioBinding(bindingSnapshot);
    auto* control = bindingSnapshot.control;

    txSlotProvider_.payloadBase = payloadBase;
    txSlotProvider_.metadataRing = metadataRing;
    txSlotProvider_.queueControl = queueControl;
    txSlotProvider_.audioControl = control;
    txSlotProvider_.numSlots = numSlots;
    txSlotProvider_.slotStrideBytes = maxPacketBytes;

    txExecutionTimeline_.queueControl = queueControl;

    if (!txStreamEngine_.Configure(resolvedProfile_, txConfig)) {
        ASFW_LOG_ERROR(Audio, "AudioEndpointStreamSession: txStreamEngine Configure failed");
        FreeTxMemoryLocked();
        return kIOReturnError;
    }
    txStreamEngine_.BindSlotProvider(&txSlotProvider_);
    if (pcmSource_) {
        txStreamEngine_.BindPcmSource(pcmSource_);
    }
    if (midiBlock_) {
        txStreamEngine_.SetMidiTransport(
            midiBlock_, midiEpoch_, midiGeometry_, midiSampleRateHz_, midiSytIntervalFrames_);
    }
    txStreamEngine_.ResetForStart(0);

    // Optional secondary transmit stream
    txSecondaryActive_ = false;
    if (resolvedProfile_.TxStreamCount() > 1) {
        ASFW::Isoch::Audio::AudioStreamConfig txConfig2{};
        if (resolvedProfile_.BuildDefaultTxStreamConfig(txConfig2)) {
            txConfig2.sourceChannelOffset = txConfig2.pcmChannels;
            const uint32_t numSlots2 = Shared::AudioTimingGeometry::kTxSharedSlotPackets;
            const uint32_t maxPacketBytes2 = 8u + static_cast<uint32_t>(txConfig2.framesPerDataPacket) * txConfig2.dbs * 4u;
            const uint32_t interruptInterval2 = Shared::AudioTimingGeometry::kTimingGroupPackets;

            IOMemoryDescriptor* rawPayload2 = nullptr;
            IOMemoryDescriptor* rawMetadata2 = nullptr;
            IOMemoryDescriptor* rawControl2 = nullptr;
            kr = isoch_.AllocateTxIsochResources(
                1, numSlots2, maxPacketBytes2, interruptInterval2,
                &rawPayload2, &rawMetadata2, &rawControl2);
            if (kr == kIOReturnSuccess) {
                txPayloadBuffer_[1] = ASFW::Common::AdoptRetained(rawPayload2);
                txMetadataBuffer_[1] = ASFW::Common::AdoptRetained(rawMetadata2);
                txControlBuffer_[1] = ASFW::Common::AdoptRetained(rawControl2);

                if (ASFW::Common::CreateSharedMapping(txPayloadBuffer_[1], txPayloadMap_[1]) == kIOReturnSuccess &&
                    ASFW::Common::CreateSharedMapping(txMetadataBuffer_[1], txMetadataMap_[1]) == kIOReturnSuccess &&
                    ASFW::Common::CreateSharedMapping(txControlBuffer_[1], txControlMap_[1]) == kIOReturnSuccess) {
                    uint8_t* payloadBase2 = reinterpret_cast<uint8_t*>(txPayloadMap_[1]->GetAddress());
                    auto* metadataRing2 = reinterpret_cast<ASFW::Isoch::IsochTxPacketMeta*>(txMetadataMap_[1]->GetAddress());
                    auto* queueControl2 = reinterpret_cast<ASFW::Isoch::IsochTxQueueControl*>(txControlMap_[1]->GetAddress());

                    queueControl2->ResetProducerForStart();

                    txSlotProviderSecondary_.payloadBase = payloadBase2;
                    txSlotProviderSecondary_.metadataRing = metadataRing2;
                    txSlotProviderSecondary_.queueControl = queueControl2;
                    txSlotProviderSecondary_.audioControl = control;
                    txSlotProviderSecondary_.numSlots = numSlots2;
                    txSlotProviderSecondary_.slotStrideBytes = maxPacketBytes2;

                    if (txStreamEngineSecondary_.Configure(resolvedProfile_, txConfig2)) {
                        txStreamEngineSecondary_.BindSlotProvider(&txSlotProviderSecondary_);
                        if (pcmSource_) {
                            txStreamEngineSecondary_.BindPcmSource(pcmSource_);
                        }
                        txStreamEngineSecondary_.ResetForStart(0);
                        txSecondaryActive_ = true;
                    }
                }
            }
        }
    }

    return kIOReturnSuccess;
}

void AudioEndpointStreamSession::FreeTxMemoryLocked() noexcept {
    txSecondaryActive_ = false;
    txStreamEngineSecondary_.BindSlotProvider(nullptr);
    txStreamEngineSecondary_.BindPcmSource(nullptr);
    txStreamEngine_.BindSlotProvider(nullptr);
    txStreamEngine_.BindPcmSource(nullptr);

    for (size_t i = 0; i < 2; ++i) {
        txPayloadMap_[i].reset();
        txMetadataMap_[i].reset();
        txControlMap_[i].reset();
        txPayloadBuffer_[i].reset();
        txMetadataBuffer_[i].reset();
        txControlBuffer_[i].reset();
    }

    txSlotProvider_ = {};
    txSlotProviderSecondary_ = {};
    txExecutionTimeline_ = {};
    (void)isoch_.FreeTxIsochResources();
}

void AudioEndpointStreamSession::HandlePendingTimelineEpoch(
    Runtime::AudioTransportControlBlock* control) noexcept {
    if (!control) return;
    Runtime::HardwareTimelineDiscontinuity reason{};
    if (!control->ConsumeTimelineEpochRequest(reason)) return;

    const uint64_t lastBoundary = control->hardwareTimeline.LastPublishedBoundary();
    const uint64_t baseFrame = control->hardwareTimeline.NextBoundary(lastBoundary);
    const uint32_t sampleRate = control->hardwareTimeline.SampleRateHz();
    const uint64_t epoch = control->hardwareTimeline.BeginEpoch(
        Runtime::HardwareTimelineSource::Transmit,
        reason, sampleRate, baseFrame);
    if (epoch == 0) {
        control->backendObservationConversionFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    control->txScheduledSampleFrame.store(baseFrame, std::memory_order_release);
    const bool useMAudio = Families::BeBoB::MAudio::UsesSpecialDuplexPolicy(
        resolvedProfile_.Value().profileBuilder);
    if (useMAudio) {
        const auto startEpoch = Families::BeBoB::MAudio::StartEpoch{
            .value = ++mAudioTxClockEpoch_};
        const auto& txStreamConfig = txStreamEngine_.StreamConfig();
        (void)mAudioPresentationObserver_.Arm(
            startEpoch,
            epoch,
            txStreamConfig.sampleRate,
            Families::BeBoB::MAudio::InternalTxTransferDelayTicks(
                txStreamConfig.sampleRate,
                txStreamConfig.framesPerDataPacket));
        (void)mAudioInternalTxTiming_.Arm(
            startEpoch,
            txStreamConfig.sampleRate,
            txStreamConfig.framesPerDataPacket);
    }
    ASFW_LOG_ERROR(
        DirectAudio,
        "[TimelineEpoch] epoch=%llu reason=%u source=tx base=%llu lastBoundary=%llu rate=%u",
        epoch, static_cast<uint32_t>(reason), baseFrame, lastBoundary,
        sampleRate);
}

void AudioEndpointStreamSession::ObserveTxHardware(
    Isoch::IsochTxQueueControl* queue,
    Runtime::AudioTransportControlBlock* control,
    uint64_t requested, bool useMAudio) noexcept {
    if (!queue || !control) return;

    const bool txDrivesTimeline =
        control->hardwareTimeline.Source() == Runtime::HardwareTimelineSource::Transmit;
    const uint64_t stampCount = queue->completionStampCount.load(std::memory_order_acquire);
    if (stampCount == 0) {
        txCompletionStampCursor_ = 0;
        return;
    }
    const auto drain = Runtime::PlanTxCompletionStampDrain(
        txCompletionStampCursor_, stampCount, ASFW::Isoch::kIsochTxCompletionStampSlots);
    if (drain.missed != 0) {
        control->backendCompletionStampsMissed.fetch_add(drain.missed, std::memory_order_relaxed);
    }
    if (drain.Empty()) {
        txCompletionStampCursor_ = stampCount;
        return;
    }
    const uint64_t cursor = drain.first;

    Isoch::IsochTxClockPairSample pair{};
    if (!queue->clockPair.TryRead(pair) || pair.hostTimeMid == 0) return;

    const auto& timeline = txStreamEngine_.Timeline();
    const uint64_t timelineEpoch = control->hardwareTimeline.Epoch();

    // Record ledger finality
    const uint64_t finalizedEnd = queue->finalizedEnd.load(std::memory_order_acquire);
    const uint64_t alreadySeen = control->ledgerObservedFinalizedEnd.load(std::memory_order_relaxed);
    if (finalizedEnd > alreadySeen) {
        uint64_t first = alreadySeen;
        if (alreadySeen == 0 || finalizedEnd - alreadySeen > Runtime::kLedgerStampSlots) {
            first = finalizedEnd > Runtime::kLedgerStampSlots
                        ? finalizedEnd - Runtime::kLedgerStampSlots
                        : 0;
        }
        for (uint64_t packet = first; packet < finalizedEnd; ++packet) {
            const auto* slot = timeline.SlotByIndex(static_cast<uint32_t>(packet));
            if (!slot || !slot->isData || slot->framesInPacket == 0 || slot->epoch != timelineEpoch) {
                continue;
            }
            uint64_t publishedAt = 0;
            const auto lookup = control->ledgerOutputPublication.Lookup(
                slot->firstAudioFrame, publishedAt);
            if (lookup != Runtime::LedgerLookup::Resolved || pair.hostTimeMid < publishedAt) {
                control->ledgerI1WriteToFinality.Count(lookup);
                continue;
            }
            control->ledgerI1WriteToFinality.Record(
                Timing::hostTicksToNanos(pair.hostTimeMid - publishedAt) / 1000ULL);
        }
        control->ledgerObservedFinalizedEnd.store(finalizedEnd, std::memory_order_relaxed);
    }

    const uint64_t finalizedEndAtWake = finalizedEnd;
    bool finalityStamped = false;

    if (useMAudio && txDrivesTimeline) {
        constexpr uint32_t kMaxWakeDataPackets =
            4 * ::ASFW::Shared::Isoch::IsochQueueGeometry::kPacketsPerCompletionGroup;
        struct WakeDataPacket final {
            uint64_t completionBusTicks{0};
            uint64_t correlationBusTicks{0};
            uint64_t firstAudioFrame{0};
            uint32_t frameCount{0};
        };
        std::array<WakeDataPacket, kMaxWakeDataPackets> dataPackets{};
        uint32_t dataPacketCount = 0;

        uint64_t completionBusTicks = 0;
        uint64_t correlationBusTicks = 0;
        uint64_t sampleFrame = 0;
        uint32_t frameCount = 0;
        bool haveStamp = false;
        bool haveData = false;

        for (uint64_t stampIndex = cursor; stampIndex < stampCount; ++stampIndex) {
            uint64_t packetIndex = 0;
            uint32_t compCycleTimer = 0;
            uint32_t compMetadata = 0;
            if (!queue->ReadCompletionStamp(stampIndex, packetIndex, compCycleTimer, compMetadata)) {
                continue;
            }
            uint64_t stampCompletion = 0;
            uint64_t stampCorrelation = 0;
            if (!Shared::ExpandCompletionAgainstCorrelation(
                    txCorrelationUnwrap_, compCycleTimer, pair.cycleTimer32,
                    stampCompletion, stampCorrelation)) {
                control->backendObservationConversionFailures.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (!haveData) {
                completionBusTicks = stampCompletion;
                correlationBusTicks = stampCorrelation;
            }
            haveStamp = true;
            const auto* slot = timeline.SlotByIndex(static_cast<uint32_t>(packetIndex));
            if (!slot || !slot->isData || slot->framesInPacket == 0 || slot->epoch != timelineEpoch) {
                continue;
            }
            completionBusTicks = stampCompletion;
            correlationBusTicks = stampCorrelation;
            sampleFrame = slot->firstAudioFrame;
            frameCount = slot->framesInPacket;
            haveData = true;
            if (dataPacketCount < kMaxWakeDataPackets) {
                dataPackets[dataPacketCount++] = {
                    .completionBusTicks = stampCompletion,
                    .correlationBusTicks = stampCorrelation,
                    .firstAudioFrame = slot->firstAudioFrame,
                    .frameCount = slot->framesInPacket,
                };
            }
        }
        txCompletionStampCursor_ = stampCount;
        if (!haveStamp) return;

        const auto converted = mAudioPresentationObserver_.ObserveHardwareWake(
            requested, completionBusTicks, correlationBusTicks,
            {.cycleTime = pair.cycleTimer32, .hostTicks = pair.hostTimeMid},
            sampleFrame, frameCount);
        control->mAudioWarmupGroups.store(converted.groupCount, std::memory_order_relaxed);
        if (converted.observationReady) {
            const uint64_t presentationOffset =
                converted.observation.presentationBusTicks - completionBusTicks;
            for (uint32_t i = 0; i < dataPacketCount; ++i) {
                const auto& entry = dataPackets[i];
                control->mAudioTxDerivedObservations.fetch_add(1, std::memory_order_relaxed);
                Runtime::HardwarePresentationObservation obs{
                    .epoch = converted.observation.epoch,
                    .source = Runtime::HardwareTimelineSource::Transmit,
                    .sampleFrame = entry.firstAudioFrame,
                    .frameCount = entry.frameCount,
                    .presentationBusTicks = entry.completionBusTicks + presentationOffset,
                    .correlationBusTicks = entry.correlationBusTicks,
                    .correlationHostTicks = converted.observation.correlationHostTicks,
                };
                Runtime::HardwareZeroTimestamp boundary{};
                if (control->hardwareTimeline.Observe(obs, &boundary) ==
                    Runtime::HardwareObservationResult::BoundaryReady) {
                    if (control->PublishHostClockAnchor(
                            boundary.sampleFrame, boundary.hostTicks,
                            boundary.hostNanosPerSampleQ8).accepted) {
                        control->counters.CountZtsPublished();
                        control->hardwareTimeline.CountZtsPublication();
                        hostTransport_.NotifyClockAnchorReady(boundary.sampleFrame);
                    }
                }
            }
        }
        return;
    }

    // Generic TX-derived clock
    const uint32_t transfer = control->txTransferDelayTicks.load(std::memory_order_relaxed);
    const uint64_t completionCursor = queue->completionCursor.load(std::memory_order_relaxed);
    for (uint64_t stampIndex = cursor; stampIndex < stampCount; ++stampIndex) {
        uint64_t packetIndex = 0;
        uint32_t compCycleTimer = 0;
        uint32_t compMetadata = 0;
        if (!queue->ReadCompletionStamp(stampIndex, packetIndex, compCycleTimer, compMetadata)) {
            continue;
        }
        uint64_t completionBusTicks = 0;
        uint64_t correlationBusTicks = 0;
        if (!Shared::ExpandCompletionAgainstCorrelation(
                txCorrelationUnwrap_, compCycleTimer, pair.cycleTimer32,
                completionBusTicks, correlationBusTicks)) {
            control->backendObservationConversionFailures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!finalityStamped) {
            uint64_t sealFrontier = 0;
            uint32_t sealCycleTimer = 0;
            uint64_t sealBusTicks = 0;
            uint64_t ignoredCorrelation = 0;
            if (queue->ReadFinalitySeal(sealFrontier, sealCycleTimer) &&
                sealCycleTimer != 0 &&
                Shared::ExpandCompletionAgainstCorrelation(
                    txCorrelationUnwrap_, sealCycleTimer, pair.cycleTimer32,
                    sealBusTicks, ignoredCorrelation)) {
                control->ledgerTxFinality.Record(sealFrontier, sealBusTicks);
            } else {
                control->ledgerTxFinality.Record(finalizedEndAtWake, correlationBusTicks);
            }
            finalityStamped = true;
        }

        const auto* slot = timeline.SlotByIndex(static_cast<uint32_t>(packetIndex));
        if (!slot || !slot->isData || slot->framesInPacket == 0 || slot->epoch != timelineEpoch) {
            continue;
        }

        uint64_t finalityBusTicks = 0;
        const auto finalityLookup = control->ledgerTxFinality.Lookup(packetIndex, finalityBusTicks);
        if (finalityLookup == Runtime::LedgerLookup::Resolved && completionBusTicks >= finalityBusTicks) {
            control->ledgerI2FinalityToTransmit.Record(
                BusTicksToMicros(completionBusTicks - finalityBusTicks));
        } else {
            control->ledgerI2FinalityToTransmit.Count(finalityLookup);
        }

        control->txCycleTrace.Complete(slot->epoch, slot->cycleOrdinal, completionCursor);
        const uint64_t completionLatency =
            completionCursor > packetIndex ? completionCursor - packetIndex : 0;
        UpdateMaximum(control->txCompletionLatencyMaxCycles, completionLatency);
        control->txCompletionLatencyHistogram[HeadroomBucket(completionLatency)].fetch_add(
            1, std::memory_order_relaxed);

        if (txDrivesTimeline) {
            Runtime::HardwarePresentationObservation observation{
                .epoch = timelineEpoch,
                .source = Runtime::HardwareTimelineSource::Transmit,
                .sampleFrame = slot->firstAudioFrame,
                .frameCount = slot->framesInPacket,
                .presentationBusTicks = completionBusTicks + transfer,
                .correlationBusTicks = correlationBusTicks,
                .correlationHostTicks = pair.hostTimeMid,
            };
            Runtime::HardwareZeroTimestamp boundary{};
            if (control->hardwareTimeline.Observe(observation, &boundary) ==
                Runtime::HardwareObservationResult::BoundaryReady) {
                if (control->PublishHostClockAnchor(
                        boundary.sampleFrame, boundary.hostTicks,
                        boundary.hostNanosPerSampleQ8).accepted) {
                    control->counters.CountZtsPublished();
                    control->hardwareTimeline.CountZtsPublication();
                    hostTransport_.NotifyClockAnchorReady(boundary.sampleFrame);
                }
            }
        }
    }
    txCompletionStampCursor_ = stampCount;
}

uint32_t AudioEndpointStreamSession::PrepareTransmitSlots(
    Isoch::IsochTxQueueControl* queue,
    Runtime::AudioTransportControlBlock* control,
    uint64_t startPacketIndex,
    uint64_t requiredPacketIndex,
    bool useMAudioInternalTiming) noexcept {
    if (!control || !queue || txSlotProvider_.numSlots == 0) {
        return 0;
    }
    const uint64_t epoch = control->hardwareTimeline.Epoch();
    uint64_t packetIndex = startPacketIndex;
    uint32_t prepared = 0;
    const uint32_t preparedTarget = preparedTargetPackets_.load(std::memory_order_acquire);

    while (packetIndex < requiredPacketIndex && prepared < preparedTarget) {
        int64_t normalizedTransmitTicks = 0;
        uint64_t transmitBusTicks = 0;
        const bool anchored = txExecutionTimeline_.AnchorForPacket(
            packetIndex, normalizedTransmitTicks);
        const bool unwrapped = anchored && normalizedTransmitTicks >= 0 &&
            UnwrapBusTicks(
                static_cast<uint64_t>(normalizedTransmitTicks),
                txPlanBusTicksValid_,
                lastTxPlanBusTicks_,
                transmitBusTicks);
        const bool haveCycle = unwrapped;

        if (!haveCycle) {
            const uint64_t events = ++txNoCycleAnchorEvents_;
            if (IsPowerOfTwo(events)) {
                ASFW_LOG_ERROR(
                    DirectAudio,
                    "[BackendTiming] noCycleAnchor=%llu packet=%llu anchored=%u raw=%lld lastPlanBus=%llu",
                    events, packetIndex, anchored ? 1u : 0u,
                    normalizedTransmitTicks, lastTxPlanBusTicks_);
            }
        }

        Protocols::Audio::AMDTP::AmdtpTimingState timing{};
        timing.disposition = AmdtpDisposition::NoData;
        uint64_t presentationBusTicks = haveCycle ? transmitBusTicks : 0;
        bool replayPeeked = false;
        Runtime::RxSequenceEntry replayEntry{};
        bool mAudioPlanActive = false;
        Families::BeBoB::MAudio::InternalTxPacketPlan mAudioPlan{};

        if (useMAudioInternalTiming) {
            if (!mAudioInternalTxTiming_.PreviewNextPacket(mAudioPlan)) {
                break;
            }
            mAudioPlanActive = true;
            timing.hasExplicitPacketSchedule = true;
            timing.explicitDataBlocks = haveCycle && mAudioPlan.isData ? mAudioPlan.dataBlocks : 0;
            timing.disposition = timing.explicitDataBlocks != 0
                ? AmdtpDisposition::Data : AmdtpDisposition::NoData;
            if (timing.disposition == AmdtpDisposition::Data) {
                presentationBusTicks = transmitBusTicks +
                    mAudioPlan.sytOffsetTicks +
                    mAudioInternalTxTiming_.TransferDelayTicks();
                timing.txClockValid = true;
                timing.nextDataSyt = SytForPresentation(presentationBusTicks);
            }
        } else {
            if (!txReplayReader_.IsActive() && control->rxSequenceReplay.IsEstablished()) {
                (void)txReplayReader_.Begin(control->rxSequenceReplay);
            }
            Runtime::RxSequenceReplayReadDiagnostic replayDiag{};
            const bool peeked = txReplayReader_.TryPeek(
                control->rxSequenceReplay, replayEntry, &replayDiag);
            if (!peeked && IsRecoverableReplayDesync(replayDiag.failure) &&
                control->rxSequenceReplay.IsEstablished() &&
                txReplayReader_.Begin(control->rxSequenceReplay)) {
                const uint64_t resyncs = ++txReplayResyncs_;
                if (IsPowerOfTwo(resyncs)) {
                    ASFW_LOG_ERROR(
                        DirectAudio,
                        "[BackendTiming] replayResync=%llu packet=%llu failure=%{public}s",
                        resyncs, packetIndex,
                        Runtime::RxSequenceReplayReadFailureName(replayDiag.failure));
                }
            }
            if (peeked) {
                replayPeeked = true;
                control->txReplayEntries.fetch_add(1, std::memory_order_relaxed);
                timing.replayValid = true;
                timing.replayDataBlocks = replayEntry.dataBlocks;
                const bool replayData = haveCycle &&
                    replayEntry.dataBlocks != 0 &&
                    (replayEntry.flags & Runtime::RxSequenceFlags::kValidSyt) != 0 &&
                    replayEntry.sytOffset != Runtime::RxSequenceReplayState::kNoInfo;
                if (replayEntry.dataBlocks != 0 && !replayData) {
                    control->txReplayInvalidSyt.fetch_add(1, std::memory_order_relaxed);
                    control->backendSytDiscontinuities.fetch_add(1, std::memory_order_relaxed);
                }
                timing.disposition = replayData ? AmdtpDisposition::Data : AmdtpDisposition::NoData;
                if (replayData) {
                    presentationBusTicks = transmitBusTicks +
                        Runtime::ComputePresentationLeadTicks(
                            replayEntry.sytOffset,
                            control->txTransferDelayTicks.load(std::memory_order_relaxed));
                    timing.txClockValid = true;
                    timing.nextDataSyt = SytForPresentation(presentationBusTicks);
                }
            } else if (control->hardwareTimeline.Source() ==
                           Runtime::HardwareTimelineSource::Transmit && haveCycle) {
                timing.disposition = AmdtpDisposition::Data;
                presentationBusTicks = transmitBusTicks +
                    control->txTransferDelayTicks.load(std::memory_order_relaxed);
                timing.txClockValid = true;
                timing.nextDataSyt = SytForPresentation(presentationBusTicks);
            } else if (control->rxSequenceReplay.IsEstablished()) {
                control->txReplayUnderflows.fetch_add(1, std::memory_order_relaxed);
            }
        }

        TxPlan plan{};
        uint8_t wireBlocks = 0;
        uint16_t syt = 0xFFFF;
        if (!txStreamEngine_.PreviewPresentationPlan(
                epoch, packetIndex, control->hardwareTimeline.NextTxFrame(),
                presentationBusTicks, timing, plan, wireBlocks, syt)) {
            break;
        }

        Runtime::TxPresentationRange range{};
        if (plan.disposition == AmdtpDisposition::Data) {
            if (!control->hardwareTimeline.PreviewTxRange(
                    epoch, presentationBusTicks, plan.frameCount, range)) {
                const uint64_t events = ++txNoPresentationOriginEvents_;
                if (IsPowerOfTwo(events)) {
                    ASFW_LOG_ERROR(
                        DirectAudio,
                        "[BackendTiming] noPresentationOrigin=%llu packet=%llu presentBus=%llu",
                        events, packetIndex, presentationBusTicks);
                }
                plan.disposition = AmdtpDisposition::NoData;
                plan.frameCount = 0;
                wireBlocks = 0;
                syt = 0xFFFF;
            } else {
                plan.firstAudioFrame = range.firstAudioFrame;
            }
        }

        const uint64_t completionCursor = queue->completionCursor.load(std::memory_order_acquire);
        const PrepareResult result = txStreamEngine_.PrepareTransmitSlot(
            static_cast<uint32_t>(packetIndex), plan, wireBlocks, syt);
        if (result != PrepareResult::Prepared) {
            break;
        } else if (txSecondaryActive_ &&
                   txStreamEngineSecondary_.PrepareTransmitSlot(
                       static_cast<uint32_t>(packetIndex), plan, wireBlocks, syt) !=
                       PrepareResult::Prepared) {
            break;
        }

        if (plan.frameCount != 0) {
            if (haveCycle && plan.presentationBusTicks >= transmitBusTicks) {
                const int64_t leadTicks =
                    static_cast<int64_t>(plan.presentationBusTicks) -
                    static_cast<int64_t>(transmitBusTicks);
                control->txLastLeadTicks.store(leadTicks, std::memory_order_relaxed);
            }
            range = {
                .epoch = plan.epoch,
                .firstAudioFrame = plan.firstAudioFrame,
                .frameCount = plan.frameCount,
                .presentationBusTicks = plan.presentationBusTicks,
            };
            if (!control->hardwareTimeline.CommitTxRange(range)) break;
            const uint64_t nextTxFrame = control->hardwareTimeline.NextTxFrame();
            control->txScheduledSampleFrame.store(nextTxFrame, std::memory_order_release);
        }

        if (mAudioPlanActive &&
            !mAudioInternalTxTiming_.CommitPacket(mAudioPlan, plan.disposition == AmdtpDisposition::Data)) {
            break;
        }
        if (replayPeeked) txReplayReader_.Advance();

        TraceCycle(*control, plan, result, completionCursor);

        control->counters.txPackets.fetch_add(1, std::memory_order_relaxed);
        if (plan.disposition == AmdtpDisposition::Data) {
            control->counters.txDataPackets.fetch_add(1, std::memory_order_relaxed);
            control->counters.txValidSytPackets.fetch_add(1, std::memory_order_relaxed);
            control->counters.txPcmFramesEncoded.fetch_add(plan.frameCount, std::memory_order_relaxed);
        } else {
            control->counters.txNoDataPackets.fetch_add(1, std::memory_order_relaxed);
            control->counters.txSytFfffPackets.fetch_add(1, std::memory_order_relaxed);
        }
        ++packetIndex;
        ++prepared;
    }
    return prepared;
}

void AudioEndpointStreamSession::NotifyLatePayloadOffers(
    Isoch::IsochTxQueueControl* queue, uint32_t streamIndex, bool offered) noexcept {
    if (!offered || !queue) return;
    if (!queue->PublishOfferBatch()) return;
    auto* txContext = isoch_.TransmitContext(streamIndex);
    if (txContext) {
        txContext->ServiceLatePayloadOffers();
    } else {
        queue->AbandonOfferNotification();
    }
}

void AudioEndpointStreamSession::SetRuntimeTuning(
    const Shared::AudioRuntimeTuning& tuning) noexcept {
    tuning_ = tuning;
    preparedTargetPackets_.store(tuning.PreparedTargetPackets(),
                                 std::memory_order_release);
}

void AudioEndpointStreamSession::OnTxPreparation(uint64_t generation) noexcept {
    (void)generation;
    // Every refusal below is counted separately. A single collapsed "the pump
    // did not run" is what left an earlier TX stall with no evidence at all:
    // the callback can decline for four unrelated reasons and they need
    // different fixes. Counters only -- this is the completion path.
    if (destroyed_.load(std::memory_order_acquire)) {
        ++txPumpDeclinedDestroyed_;
        return;
    }
    if (!streaming_) {
        // Expected briefly at start: the transmit context begins completing
        // inside StartStreaming(), before StartSessionLocked sets streaming_.
        // A count that keeps climbing after start is not that.
        //
        // Acknowledge anyway. Declining is fine; leaving the request
        // outstanding is not, because transport would then coalesce every
        // later completion and never call back again -- a transient decline
        // during start would silence the pump permanently.
        ++txPumpDeclinedNotStreaming_;
        if (auto* pending = txSlotProvider_.queueControl) {
            pending->MarkRefillHandled(
                pending->refillRequestGeneration.load(std::memory_order_acquire));
        }
        return;
    }

    inFlightCallbacks_.fetch_add(1, std::memory_order_acq_rel);
    if (destroyed_.load(std::memory_order_acquire) || !streaming_) {
        inFlightCallbacks_.fetch_sub(1, std::memory_order_release);
        ++txPumpDeclinedRaced_;
        return;
    }

    struct CallbackGuard {
        std::atomic<uint32_t>& count;
        ~CallbackGuard() { count.fetch_sub(1, std::memory_order_release); }
    } guard{inFlightCallbacks_};

    auto* queue = txSlotProvider_.queueControl;
    Runtime::DirectAudioBindingSnapshot bindingSnapshot{};
    if (!endpointRuntime_.CopyDirectAudioBinding(bindingSnapshot)) {
        ++txPumpDeclinedNoBinding_;
        return;
    }
    auto* control = bindingSnapshot.control;
    if (!queue || !control) {
        ++txPumpDeclinedNoQueue_;
        return;
    }

    txSlotProvider_.audioControl = control;
    if (txSecondaryActive_) {
        txSlotProviderSecondary_.audioControl = control;
    }

    const uint64_t requested = queue->refillRequestGeneration.load(std::memory_order_acquire);
    const uint64_t handled = queue->refillHandledGeneration.load(std::memory_order_acquire);
    const bool hardwareWake = requested != handled;
    const uint64_t completion = queue->completionCursor.load(std::memory_order_acquire);
    const uint64_t committedBefore = queue->committedEnd.load(std::memory_order_acquire);
    const uint64_t target =
        completion + preparedTargetPackets_.load(std::memory_order_acquire);
    const bool useMAudio = Families::BeBoB::MAudio::UsesSpecialDuplexPolicy(
        resolvedProfile_.Value().profileBuilder);

    HandlePendingTimelineEpoch(control);

    if (hardwareWake) {
        ObserveTxHardware(queue, control, requested, useMAudio);
    }

    const uint32_t prepared = PrepareTransmitSlots(
        queue, control, committedBefore, target,
        useMAudio && mAudioInternalTxTiming_.IsArmed());
    const uint64_t committedAfter = queue->committedEnd.load(std::memory_order_acquire);

    // Restored from the pre-WP-6 pump (ASFWAudioDriverZts.cpp:1569). This fires
    // exactly when the producer failed to reach the horizon transport asked
    // for, which is the condition that precedes every content-starvation fatal
    // stop -- and it was the one diagnostic the move across the seam dropped,
    // leaving committedEnd frozen at the prefill with nothing recorded.
    //
    // Note this is silent by construction whenever target <= committedBefore:
    // the prefill commits numSlots packets while the horizon is only
    // completion + PreparedTargetPackets(), so the pump legitimately has no
    // work until completion climbs. Both figures are printed so that window is
    // readable rather than inferred.
    // The dead zone: the prefill commits numSlots packets while the horizon is
    // only completion + PreparedTargetPackets(), so until completion climbs
    // past (numSlots - depth) the loop in PrepareTransmitSlots cannot run at
    // all and [TxOwnership] below stays silent by construction. That window is
    // exactly where an observed TX stall died, with committedEnd frozen at the
    // prefill and nothing recorded anywhere. Count it and print the three
    // figures that decide it, so the window is measured rather than inferred.
    if (target <= committedBefore) {
        const uint64_t idle = ++txPumpHorizonBehindEvents_;
        if (IsPowerOfTwo(idle)) {
            ASFW_LOG_LEVELED(
                DirectAudio, ::ASFW::Logging::LogLevel::Debug,
                "[TxHorizon] idle=%llu completion=%llu committed=%llu "
                "target=%llu depth=%u slots=%u",
                idle, completion, committedBefore, target,
                preparedTargetPackets_.load(std::memory_order_relaxed),
                txSlotProvider_.numSlots);
        }
    }

    if (committedAfter < target) {
        const uint64_t events = ++txOwnershipShortEvents_;
        if (IsPowerOfTwo(events)) {
            ASFW_LOG_ERROR(
                DirectAudio,
                "[TxOwnership] short=%llu completion=%llu committedBefore=%llu "
                "committed=%llu target=%llu prepared=%u depth=%u wake=%u",
                events, completion, committedBefore, committedAfter, target,
                prepared, preparedTargetPackets_.load(std::memory_order_relaxed),
                hardwareWake ? 1u : 0u);
        }
    }

    // Content fill pass
    {
        const uint64_t frozen = queue->finalizedEnd.load(std::memory_order_acquire);
        if (txFillCursor_ > committedAfter) {
            const uint64_t events = ++txFillCursorAheadEvents_;
            if (IsPowerOfTwo(events)) {
                ASFW_LOG_ERROR(
                    DirectAudio,
                    "[TxFill] stale content cursor=%llu ahead of committed=%llu events=%llu",
                    txFillCursor_, committedAfter, events);
            }
        }
        while (txFillCursor_ < frozen) {
            const auto packet = static_cast<uint32_t>(txFillCursor_);
            txStreamEngine_.NoteFrozenWithoutContent(packet);
            if (txSecondaryActive_) {
                txStreamEngineSecondary_.NoteFrozenWithoutContent(packet);
            }
            ++txFillCursor_;
        }

        // For standalone MIDI (or whenever PCM source is unbound), bounding the content
        // fill horizon prevents pre-filling the entire committed DMA runway (~1008 packets / 126 ms)
        // with silence, which would lock out subsequently arriving MIDI events.
        constexpr uint64_t kMidiOnlyContentFillLeadPackets =
            2 * Shared::AudioTimingGeometry::kTimingGroupPackets;
        const uint64_t fillLimit = (pcmSource_ != nullptr)
            ? committedAfter
            : std::min(committedAfter, frozen + kMidiOnlyContentFillLeadPackets);

        bool offeredPrimary = false;
        bool offeredSecondary = false;
        while (txFillCursor_ < fillLimit) {
            const auto packet = static_cast<uint32_t>(txFillCursor_);
            const auto primary = txStreamEngine_.FillTransmitSlot(packet);
            if (primary == FillResult::ContentUnavailable) break;
            if (primary == FillResult::Filled) {
                const bool secondaryReady =
                    !txSecondaryActive_ ||
                    txStreamEngineSecondary_.FillTransmitSlot(packet) == FillResult::Filled;
                if (secondaryReady) {
                    if (txStreamEngine_.CommitFill(packet)) {
                        offeredPrimary = true;
                        if (txSecondaryActive_ && txStreamEngineSecondary_.CommitFill(packet)) {
                            offeredSecondary = true;
                        }
                    }
                }
            }
            ++txFillCursor_;
        }

        NotifyLatePayloadOffers(queue, 0, offeredPrimary);
        if (txSecondaryActive_) {
            NotifyLatePayloadOffers(txSlotProviderSecondary_.queueControl, 1, offeredSecondary);
        }
    }

    const uint64_t margin = committedAfter > completion ? committedAfter - completion : 0;
    UpdateMaximum(control->txPacketStoreHighWaterPackets, margin);
    control->txTransportCompletionCursor.store(completion, std::memory_order_relaxed);
    control->txTransportCommittedEnd.store(committedAfter, std::memory_order_relaxed);
    control->txTransportStatus.store(
        static_cast<uint32_t>(queue->statusWord.load(std::memory_order_acquire)),
        std::memory_order_relaxed);
    control->txCurrentCommittedMarginPackets.store(
        margin > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(margin),
        std::memory_order_relaxed);
    control->RecordCommittedMargin(margin);
    control->counters.txPreparationWakeDispatches.fetch_add(1, std::memory_order_relaxed);
    control->counters.txPreparationDrainPasses.fetch_add(1, std::memory_order_relaxed);

    // TX liveness heartbeat. The five-second block that used to carry this
    // ([TxV3], [TxLead], [TxFill], [TxPrep], [Ledger]) still sits in
    // ASFWAudioDriverZts.cpp's TxPreparationReady, which WP-6 replaced -- so
    // since that move the driver has emitted no TX heartbeat at all and every
    // one of these counters was computed and discarded. CLAUDE.md requires one
    // coarse liveness/margin record to survive; this is it.
    //
    // `filled` is the load-bearing field. A stream can transmit a full packet
    // count with healthy transport telemetry and still put pure silence on the
    // wire -- that is c6803886's failure shape -- and `filled` is the only
    // figure that separates the two. Do not remove it.
    {
        const uint64_t nowTicks = mach_absolute_time();
        const uint64_t lastBeat =
            control->txHeartbeatLastHostTicks.load(std::memory_order_relaxed);
        if (lastBeat == 0 || nowTicks <= lastBeat ||
            ASFW::Timing::hostTicksToNanos(nowTicks - lastBeat) >=
                5'000'000'000ULL) {
            control->txHeartbeatLastHostTicks.store(nowTicks,
                                                    std::memory_order_relaxed);
            const auto& fill = txStreamEngine_.Counters();
            // `filled` is the producer's OPTIMISTIC count: it rises when a
            // publication is accepted, which is not the same as the wire
            // carrying it. `lost` is transport's count of publications it then
            // sealed on the armed image -- IsochTxQueue.hpp:615 calls it "the
            // authoritative count of content the producer believed it placed
            // and the wire never carried". filled-minus-lost is the truthful
            // content figure, so the two must always be read as a pair. A
            // heartbeat carrying `filled` alone reports a healthy encoder
            // during total silence.
            ASFW_LOG(DirectAudio,
                     "[TxPrep] filled=%llu lost=%llu silent=%llu tooLate=%llu "
                     "unavailable=%llu rebound=%llu rejected=%llu "
                     "missedDeadline=%llu cursor=%llu completion=%llu "
                     "committed=%llu margin=%llu bound=%u",
                     fill.lateFillsPublished.load(std::memory_order_relaxed),
                     queue->latePayloadLostPublicationCount.load(
                         std::memory_order_relaxed),
                     fill.pcmSilenceSubstitutions.load(std::memory_order_relaxed),
                     fill.lateFillsTooLate.load(std::memory_order_relaxed),
                     fill.lateFillsUnavailable.load(std::memory_order_relaxed),
                     queue->latePayloadRebindCount.load(std::memory_order_relaxed),
                     queue->latePayloadRebindRejectedCount.load(
                         std::memory_order_relaxed),
                     queue->latePayloadRebindMissedDeadlineCount.load(
                         std::memory_order_relaxed),
                     txFillCursor_, completion, committedAfter, margin,
                     pcmSource_ != nullptr ? 1u : 0u);

            // What actually went onto the wire, as opposed to what the producer
            // believed it published. `dataPackets` counts payloads the content
            // inspector examined; `zeroPcm` counts those whose every quad was
            // zero or the idle-slot word. zeroPcm == dataPackets is silence on
            // the wire regardless of how healthy `filled` looks, and
            // dataPackets == 0 means the inspector never ran at all (a null
            // audioControl), which is a different fault with the same symptom.
            const auto& wire = control->txWirePayloadTelemetry;
            ASFW_LOG(DirectAudio,
                     "[TxWireSum] dataPackets=%llu zeroPcm=%llu dropouts=%llu "
                     "infoQuads=%llu maxAbs24=%u",
                     wire.dataPackets.load(std::memory_order_relaxed),
                     wire.zeroPcmPackets.load(std::memory_order_relaxed),
                     wire.pcmDropouts.load(std::memory_order_relaxed),
                     wire.infoQuads.load(std::memory_order_relaxed),
                     wire.maxAbs24.load(std::memory_order_relaxed));
        }
    }

    // Acknowledge the request that woke us. Transport raises a new refill
    // request only while requested == handled (IsochTxDmaRing.cpp:1258); an
    // unacknowledged request makes every later completion take the coalesce
    // branch, leave out.refillRequestGeneration at zero, and never call this
    // back. One missed acknowledgement therefore silences the pump for the rest
    // of the stream, which is exactly what happened: one pump call at
    // completion=7, committedEnd frozen at the prefill, then the hardware
    // lapped the ring into an uncommitted slot and fatally stopped.
    queue->MarkRefillHandled(requested);
}

} // namespace ASFW::Audio
