// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../../../Isoch/Core/IsochTypes.hpp"
#include "../../../../Isoch/Receive/IsochRxTiming.hpp"
#include "../../../Runtime/ZtsTelemetry.hpp"
#include "../../../Wire/AMDTP/RxSytCadence.hpp"
#include "../../../DriverKit/Runtime/AudioGraphBinding.hpp"
#include "../../../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../AudioClockPublisher.hpp"
#include "../DirectInputWriter.hpp"
#include "RxAudioPacketProcessor.hpp"
#include "RxCaptureChannelMap.hpp"

#include <functional>

namespace ASFW::AudioEngine::Direct::Rx {

// Owns all content interpretation for one IR stream. Isoch supplies only an
// opaque payload and its controller-time correlation; this class owns audio
// decode, replay, ZTS and device-policy callbacks.
class DirectAudioReceiveConsumer final : public ::ASFW::Isoch::IIsochReceiveConsumer {
  public:
    struct Configuration final {
        ::ASFW::Encoding::AudioWireFormat wireFormat{
            ::ASFW::Encoding::AudioWireFormat::kAM824};
        uint32_t am824Slots{0};
        uint32_t channelOffset{0};
        uint32_t streamChannels{0};
        bool isSecondary{false};
        // M-Audio 1814 / ProjectMix can legitimately emit exactly an OHCI
        // receive prefix plus a DBS=0/SYT=ffff CIP header while capture comes
        // out of its special-firmware transition. This is opt-in per profile;
        // generic AM824 receive remains strict.
        bool acceptHeaderOnlyNoDataTransition{false};
        // M-Audio's vendor transport derives playback time from its TX DMA
        // callback. RX remains available for capture/replay, but it must not
        // overwrite that playback clock with a late or header-only transition.
        bool useTxDerivedPlaybackClock{false};
        // How this device's AM824 capture slots land on CoreAudio channels, and
        // any per-channel skew to undo. Identity for every device that reports
        // an honest channel order; chosen by the family that knows otherwise.
        RxCaptureChannelMap captureChannelMap{};
        // MPX-MIDI lifted from the same packets. Disabled unless the endpoint
        // published usable MIDI capability, and only ever on stream 0.
        RxMidiExtraction midi{};
    };

    using TimingLossCallback = std::function<void()>;
    /// Raised after a batch delivered MIDI bytes, so the MIDI service can be
    /// woken to drain them. Never called from inside the per-packet loop: one
    /// wake per batch, on the receive queue, doing nothing but signalling.
    using MidiReceivedCallback = std::function<void()>;
    using ZtsAnchorReadyCallback = std::function<void(uint64_t)>;

    DirectAudioReceiveConsumer(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
        Configuration configuration) noexcept;

    void SetBindingSource(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept;
    void SetTimingLossCallback(TimingLossCallback callback) noexcept;
    void SetZtsAnchorReadyCallback(ZtsAnchorReadyCallback callback) noexcept;
    void SetMidiReceivedCallback(MidiReceivedCallback callback) noexcept;
    [[nodiscard]] bool IsReplayEstablished() const noexcept;

    void OnReceiveActivated() noexcept override;
    void OnReceiveQuiesced() noexcept override;
    void BeginReceiveBatch(const ::ASFW::Isoch::IsochReceiveBatch& batch) noexcept override;
    void ConsumePacket(const ::ASFW::Isoch::IsochReceiveBatch& batch,
                       const ::ASFW::Isoch::IsochReceivePacket& packet) noexcept override;

    void PerformMaintenance(
        ::ASFW::Isoch::IsochConsumerMaintenanceKind kind,
        uint32_t budget) override;

  private:
    enum class ReplayResetReason : uint8_t {
        kEmptyCompletion,
        kPacketProcessorStatus,
        kInvalidReceiveTimestamp,
        kReceiveCycleGap,
        kSytCadenceRejected,
        kClockAnchorRejected,
    };

    struct ReplayResetContext final {
        uint32_t descriptorIndex{0};
        uint32_t payloadBytes{0};
        uint32_t drainCycleTimer{0};
        uint16_t receiveCycleTimestamp{0};
        uint16_t syt{0xffff};
        uint32_t expectedCycleOrdinal{0};
        uint32_t observedCycleOrdinal{0};
        uint32_t packetStatus{0};
        uint64_t sampleFrame{0};
    };

    [[nodiscard]] static const char* ReplayResetReasonName(ReplayResetReason reason) noexcept;
    void ResetReplayEpochForDiscontinuity(ReplayResetReason reason,
                                          const ReplayResetContext& context) noexcept;
    [[nodiscard]] bool IsAcceptedHeaderOnlyNoDataTransition(
        const ::ASFW::Isoch::IsochReceivePacket& packet,
        const RxAudioPacketProcessorResult& result) const noexcept;
    void ObserveAcceptedHeaderOnlyNoDataTransition(
        const ::ASFW::Isoch::IsochReceiveBatch& batch,
        const ::ASFW::Isoch::IsochReceivePacket& packet,
        const RxAudioPacketProcessorResult& result) noexcept;
    void LogZeroDataBlockSizeEvidence(
        const ::ASFW::Isoch::IsochReceiveBatch& batch,
        const ::ASFW::Isoch::IsochReceivePacket& packet,
        const RxAudioPacketProcessorResult& result) noexcept;
    void LogReceivedWirePayload(
        const ::ASFW::Isoch::IsochReceivePacket& packet,
        const RxAudioPacketProcessorResult& result) noexcept;
    void DrainReceiveTelemetry(uint32_t maxRecords);
    void LogTransmitTimingTrace();
    void MeasureCursorAgainstPhase(
        const ::ASFW::Driver::RxSytCadence::Snapshot& cadence,
        int64_t currentPhaseTicks,
        uint64_t packetFirstFrame,
        uint32_t framesDecoded) noexcept;

    ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource_{nullptr};
    uint64_t lastBindingGeneration_{0};
    Configuration configuration_{};
    ::ASFW::AudioEngine::Direct::DirectInputWriter inputWriter_{};
    ::ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor processor_{inputWriter_};
    ::ASFW::Audio::Runtime::AudioGraphBinding inputView_{};
    ::ASFW::AudioEngine::Direct::AudioClockPublisher clockPublisher_{};

    bool secondaryAnchored_{false};
    uint64_t secondaryAnchorEpoch_{0};
    uint64_t absoluteFrameCursor_{0};
    // Reference pair for the cursor correction: the recovered SYT phase of a
    // past packet and the frame the cursor stood at when it arrived. Held for
    // one ZTS period at a time, because extOffsetDiff resolves an eight-second
    // domain and only a delta well inside half of that is unambiguous.
    int64_t cursorPhaseAnchorTicks_{0};
    uint64_t cursorPhaseAnchorFrame_{0};
    bool cursorPhaseAnchorValid_{false};
    // Deferred by one packet on purpose. The packet that reveals the gap has
    // already been decoded at the old position, so applying the correction to it
    // would move its anchor away from where its audio actually landed. Applied
    // before the next decode instead, which leaves anchor and buffer agreeing on
    // every packet and puts the hole between them.
    int64_t pendingCursorCorrectionFrames_{0};
    uint32_t cursorCorrectionLogBudget_{16};
    // Set whenever a new stream/timeline epoch begins, so the next decoded
    // packet primes the head of the capture delay line instead of inheriting
    // the previous epoch's audio.
    bool primeCaptureDelayLine_{true};
    uint64_t ztsPublishCount_{0};
    uint64_t timestampValidCount_{0};
    uint64_t timestampInvalidCount_{0};
    uint64_t negativeAgeCount_{0};
    uint64_t largeNegativeAgeCount_{0};
    bool cadenceEstablishedLogged_{false};
    ::ASFW::Audio::Runtime::ZtsTelemetryRing ztsTelemetry_{};
    TimingLossCallback timingLossCallback_{};
    ZtsAnchorReadyCallback ztsAnchorReadyCallback_{};
    MidiReceivedCallback midiReceivedCallback_{};
    /// MIDI bytes seen during the batch currently being drained.
    uint32_t midiBytesThisBatch_{0};
    bool replayResetForStart_{false};
    // Bounded [RxReplayReset] records for a stream that has not established yet.
    // Re-armed at each bring-up; without a budget a permanently-rejected stream
    // would log at the isochronous packet rate.
    static constexpr uint32_t kBootstrapResetLogBudget = 8;
    uint32_t bootstrapResetLogBudget_{kBootstrapResetLogBudget};
    // A zero DBS conflicts with the configured stream geometry. It could be a
    // device-specific representation or an RX offset/descriptor problem, so
    // preserve evidence for a few occurrences without producing an 8 kHz trace.
    static constexpr uint32_t kZeroDataBlockSizeCaptureLogBudget = 4;
    uint32_t zeroDataBlockSizeCaptureLogBudget_{
        kZeroDataBlockSizeCaptureLogBudget};
    uint32_t zeroDataBlockSizeCaptureCount_{0};
    // Bring-up evidence for "every channel decodes to exact silence". The AM824
    // reader accepts only the 0x40 MBLA label and yields 0.0f for anything else,
    // while Linux's reader ignores the label entirely (amdtp-am824.c:203). Those
    // two failures — the device really sending silence, and us discarding real
    // audio carrying an unexpected label — are indistinguishable downstream, so
    // record the labels actually on the wire alongside a label-independent peak.
    // Bounded per bring-up: a healthy stream logs this a few times and stops.
    static constexpr uint32_t kReceivedWirePayloadLogBudget = 4;
    uint32_t receivedWirePayloadLogBudget_{kReceivedWirePayloadLogBudget};
    // A packet that decoded fine but wrote no PCM. Bounded per start so a
    // permanently unbound writer names itself once instead of flooding at the
    // 8 kHz packet rate.
    static constexpr uint32_t kCaptureMapRejectedLogBudget = 2;
    uint32_t captureMapRejectedLogBudget_{kCaptureMapRejectedLogBudget};
    static constexpr uint32_t kUnwrittenStatusLogBudget = 4;
    uint32_t unwrittenStatusLogBudget_{kUnwrittenStatusLogBudget};
    static constexpr uint32_t kHeaderOnlyNoDataTransitionLogBudget = 4;
    uint32_t headerOnlyNoDataTransitionLogBudget_{
        kHeaderOnlyNoDataTransitionLogBudget};
    bool replayCycleInitialized_{false};
    uint32_t lastReplayCycleOrdinal_{0};
    bool busTicksInitialized_{false};
    uint64_t lastUnwrappedBusTicks_{0};
    uint8_t lastDbc_{0};
    bool dbcInitialized_{false};
    ::ASFW::Audio::Runtime::ZtsTelemetryLogGate ztsTelemetryLogGate_{};
    uint64_t prevLoggedAnchorFrame_{0};
    uint64_t prevLoggedAnchorHostTicks_{0};
    uint32_t prevLoggedAnchorRate_{0};
    bool prevLoggedAnchorValid_{false};
};

} // namespace ASFW::AudioEngine::Direct::Rx
