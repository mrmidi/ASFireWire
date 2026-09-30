// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../../../Isoch/Core/IsochTypes.hpp"
#include "../../../../Isoch/Receive/IsochRxTiming.hpp"
#include "../../../../Isoch/Receive/ZtsTelemetry.hpp"
#include "../../../DriverKit/Runtime/AudioGraphBinding.hpp"
#include "../../../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../AudioClockPublisher.hpp"
#include "../DirectInputWriter.hpp"
#include "RxAudioPacketProcessor.hpp"
#include "../../../Wire/AM824/Am824PayloadCodec.hpp"
#include "../../../Wire/RawPcm24In32/RawPcm24In32PayloadCodec.hpp"
#include "../../../Wire/RawPcm24In32/RawPcm24Upper24In32LEPayloadCodec.hpp"
#include "../../../Ports/IWirePayloadCodec.hpp"

#include <functional>

namespace ASFW::AudioEngine::Direct::Rx {

// Owns all content interpretation for one IR stream. Isoch supplies only an
// opaque payload and its controller-time correlation; this class owns audio
// decode, replay, ZTS and device-policy callbacks.
// Device-specific presentation timing (e.g. per-block SPH) is observed via
// IRxDeviceTimingObserver.
class DirectAudioReceiveConsumer final : public ::ASFW::Isoch::IIsochReceiveConsumer {
  public:
    struct Configuration final {
        ::ASFW::Encoding::AudioWireFormat wireFormat{
            ::ASFW::Encoding::AudioWireFormat::kAM824};
        ::ASFW::Encoding::AudioPacketFraming framing{
            ::ASFW::Encoding::AudioPacketFraming::kCip};
        uint32_t am824Slots{0};
        uint32_t channelOffset{0};
        uint32_t streamChannels{0};
        bool isSecondary{false};
        // Loud OXFW quirk: take the RX stride from the configured slot count,
        // not the packet's CIP dbs field (snd-oxfw SND_OXFW_QUIRK_WRONG_DBS).
        bool trustConfiguredStride{false};
        RxCaptureChannelMap captureChannelMap{};
    };

    using TimingLossCallback = std::function<void()>;
    using ZtsAnchorReadyCallback = std::function<void(uint64_t)>;
    using ReplayReadyCallback = std::function<void()>;

    DirectAudioReceiveConsumer(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
        Configuration configuration) noexcept;

    void SetBindingSource(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept;
    void SetTimingLossCallback(TimingLossCallback callback) noexcept;
    void SetZtsAnchorReadyCallback(ZtsAnchorReadyCallback callback) noexcept;
    void SetReplayReadyCallback(ReplayReadyCallback callback) noexcept;
    void SetPayloadCodec(const ::ASFW::Audio::IRxPayloadCodec* codec) noexcept {
        payloadCodec_ = codec;
    }
    void SetTimingObserver(::ASFW::Audio::IRxDeviceTimingObserver* observer) noexcept {
        timingObserver_ = observer;
    }
    [[nodiscard]] bool IsReplayEstablished() const noexcept;

    void OnReceiveActivated() noexcept override;
    void OnReceiveQuiesced() noexcept override;
    void BeginReceiveBatch(const ::ASFW::Isoch::IsochReceiveBatch& batch) noexcept override;
    void ConsumePacket(const ::ASFW::Isoch::IsochReceiveBatch& batch,
                       const ::ASFW::Isoch::IsochReceivePacket& packet) noexcept override;

    void DrainReceiveTelemetry(uint32_t maxRecords) override;
    // Reports IO-callback errors recorded by the real-time path.
    void ServiceConsumerDiagnostics() override;

  private:
    enum class ReplayResetReason : uint8_t {
        kPacketProcessorStatus,
        kInvalidReceiveTimestamp,
        kReceiveCycleGap,
        kSytCadenceRejected,
        kClockAnchorRejected,
        kTransmitClockRebase,
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
    // reportTimingLoss=false rebases the numbering without telling the session
    // the stream lost timing: a rebase is not a loss, and restarting on it
    // would only repeat it.
    void ResetReplayEpochForDiscontinuity(ReplayResetReason reason,
                                          const ReplayResetContext& context,
                                          bool reportTimingLoss = true) noexcept;

    // When a Transmit epoch owns the device's clock (M-Audio special firmware),
    // RX publishes no anchor, so nothing ties this cursor to the frame numbering
    // the HAL reads at. Give it that origin, once per start, from the anchor TX
    // published. Returns true if it moved the cursor.
    bool AnchorCursorToTransmitClock(const ::ASFW::Isoch::IsochReceivePacket& packet,
                                     const RxAudioPacketProcessorResult& result,
                                     uint64_t packetHostTicks) noexcept;

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
    bool cursorInitialized_{false};
    // The first frames after a rebase have no history to delay from.
    bool primeCaptureDelayLine_{false};
    // Drain cycle timers unwrapped across the 128 s cycle-timer wrap, so the
    // hardware timeline sees monotonic bus time for the whole activation.
    [[nodiscard]] uint64_t UnwrapDrainBusTicks(uint32_t drainCycleTimer) noexcept;
    uint64_t drainBusWraps_{0};
    int64_t lastDrainOffsets_{-1};
    uint64_t ztsPublishCount_{0};
    uint64_t timestampValidCount_{0};
    uint64_t timestampInvalidCount_{0};
    uint64_t negativeAgeCount_{0};
    uint64_t largeNegativeAgeCount_{0};
    bool cadenceEstablishedLogged_{false};
    ::ASFW::Isoch::Rx::ZtsTelemetryRing ztsTelemetry_{};
    TimingLossCallback timingLossCallback_{};
    ZtsAnchorReadyCallback ztsAnchorReadyCallback_{};
    ReplayReadyCallback replayReadyCallback_{};
    bool replayReadyNotified_{false};
    ::ASFW::Audio::IRxDeviceTimingObserver* timingObserver_{nullptr};
    bool replayResetForStart_{false};
    // Bounded [RxReplayReset] records for a stream that has not established yet.
    // Re-armed at each bring-up; without a budget a permanently-rejected stream
    // would log at the isochronous packet rate.
    static constexpr uint32_t kBootstrapResetLogBudget = 8;
    uint32_t bootstrapResetLogBudget_{kBootstrapResetLogBudget};
    bool replayCycleInitialized_{false};
    uint32_t lastReplayCycleOrdinal_{0};
    uint32_t headerlessContinuousCycles_{0};
    ::ASFW::Isoch::Rx::ZtsTelemetryLogGate ztsTelemetryLogGate_{};
    uint64_t prevLoggedAnchorFrame_{0};
    uint64_t prevLoggedAnchorHostTicks_{0};
    uint32_t prevLoggedAnchorRate_{0};
    bool prevLoggedAnchorValid_{false};
    ::ASFW::Audio::Wire::Am824RxPayloadCodec am824Codec_{};
    ::ASFW::Audio::Wire::RawPcm24In32RxPayloadCodec rawPcmCodec_{};
    ::ASFW::Audio::Wire::RawPcm24Upper24In32LEPayloadCodec rawUpper24LeCodec_{};
    const ::ASFW::Audio::IRxPayloadCodec* payloadCodec_{nullptr};

};

} // namespace ASFW::AudioEngine::Direct::Rx
