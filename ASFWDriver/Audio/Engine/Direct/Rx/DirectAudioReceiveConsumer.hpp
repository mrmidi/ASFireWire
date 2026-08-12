// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../../../../Isoch/Core/IsochTypes.hpp"
#include "../../../../Isoch/Receive/IsochRxTiming.hpp"
#include "../../../Runtime/ZtsTelemetry.hpp"
#include "../../../DriverKit/Runtime/AudioGraphBinding.hpp"
#include "../../../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../AudioClockPublisher.hpp"
#include "../DirectInputWriter.hpp"
#include "RxAudioPacketProcessor.hpp"

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
    };

    using TimingLossCallback = std::function<void()>;
    using ZtsAnchorReadyCallback = std::function<void(uint64_t)>;

    DirectAudioReceiveConsumer(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
        Configuration configuration) noexcept;

    void SetBindingSource(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept;
    void SetTimingLossCallback(TimingLossCallback callback) noexcept;
    void SetZtsAnchorReadyCallback(ZtsAnchorReadyCallback callback) noexcept;
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
    void DrainReceiveTelemetry(uint32_t maxRecords);
    void LogTransmitTimingTrace();

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
    uint64_t ztsPublishCount_{0};
    uint64_t timestampValidCount_{0};
    uint64_t timestampInvalidCount_{0};
    uint64_t negativeAgeCount_{0};
    uint64_t largeNegativeAgeCount_{0};
    bool cadenceEstablishedLogged_{false};
    ::ASFW::Audio::Runtime::ZtsTelemetryRing ztsTelemetry_{};
    TimingLossCallback timingLossCallback_{};
    ZtsAnchorReadyCallback ztsAnchorReadyCallback_{};
    bool replayResetForStart_{false};
    // Bounded [RxReplayReset] records for a stream that has not established yet.
    // Re-armed at each bring-up; without a budget a permanently-rejected stream
    // would log at the isochronous packet rate.
    static constexpr uint32_t kBootstrapResetLogBudget = 8;
    uint32_t bootstrapResetLogBudget_{kBootstrapResetLogBudget};
    bool replayCycleInitialized_{false};
    uint32_t lastReplayCycleOrdinal_{0};
    uint8_t lastDbc_{0};
    bool dbcInitialized_{false};
    ::ASFW::Audio::Runtime::ZtsTelemetryLogGate ztsTelemetryLogGate_{};
    uint64_t prevLoggedAnchorFrame_{0};
    uint64_t prevLoggedAnchorHostTicks_{0};
    uint32_t prevLoggedAnchorRate_{0};
    bool prevLoggedAnchorValid_{false};
};

} // namespace ASFW::AudioEngine::Direct::Rx
