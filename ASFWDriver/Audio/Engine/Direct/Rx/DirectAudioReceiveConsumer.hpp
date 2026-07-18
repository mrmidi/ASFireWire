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
    using ReplayReadyCallback = std::function<void()>;

    DirectAudioReceiveConsumer(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
        Configuration configuration) noexcept;

    void SetBindingSource(
        ::ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept;
    void SetTimingLossCallback(TimingLossCallback callback) noexcept;
    void SetZtsAnchorReadyCallback(ZtsAnchorReadyCallback callback) noexcept;
    void SetReplayReadyCallback(ReplayReadyCallback callback) noexcept;
    [[nodiscard]] bool IsReplayEstablished() const noexcept;

    void OnReceiveActivated() noexcept override;
    void OnReceiveQuiesced() noexcept override;
    void BeginReceiveBatch(const ::ASFW::Isoch::IsochReceiveBatch& batch) noexcept override;
    void ConsumePacket(const ::ASFW::Isoch::IsochReceiveBatch& batch,
                       const ::ASFW::Isoch::IsochReceivePacket& packet) noexcept override;

    void DrainReceiveTelemetry(uint32_t maxRecords) override;
    void DrainPayloadTelemetry() override;
    void LogTransmitTimingTrace() override;

  private:
    void ResetReplayEpochForDiscontinuity() noexcept;

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
    ::ASFW::Isoch::Rx::ZtsTelemetryRing ztsTelemetry_{};
    TimingLossCallback timingLossCallback_{};
    ZtsAnchorReadyCallback ztsAnchorReadyCallback_{};
    ReplayReadyCallback replayReadyCallback_{};
    bool replayReadyNotified_{false};
    bool replayResetForStart_{false};
    bool replayCycleInitialized_{false};
    uint32_t lastReplayCycleOrdinal_{0};
    ::ASFW::Audio::Runtime::PayloadWriterTelemetryAnomalyAggregator
        payloadWriterTelemetryAggregator_{};
    uint8_t lastDbc_{0};
    bool dbcInitialized_{false};
    ::ASFW::Isoch::Rx::ZtsTelemetryLogGate ztsTelemetryLogGate_{};
    uint64_t prevLoggedAnchorFrame_{0};
    uint64_t prevLoggedAnchorHostTicks_{0};
    uint32_t prevLoggedAnchorRate_{0};
    bool prevLoggedAnchorValid_{false};
};

} // namespace ASFW::AudioEngine::Direct::Rx
