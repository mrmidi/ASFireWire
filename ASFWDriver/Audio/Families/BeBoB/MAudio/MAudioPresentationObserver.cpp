// SPDX-License-Identifier: Apache-2.0
#include "MAudioPresentationObserver.hpp"

#include "../../../../Common/TimingUtils.hpp"

#include <limits>
#include <utility>
#include <variant>

namespace ASFW::Audio::Families::BeBoB::MAudio {

ChoreographyStep PresentationObserver::Advance(
    ChoreographyEvent event) noexcept {
    auto step = ::ASFW::Audio::Families::BeBoB::MAudio::Advance(state_, event);
    state_ = std::move(step.state);
    return step;
}

bool PresentationObserver::Arm(StartEpoch startEpoch,
                               uint64_t timelineEpoch,
                               uint32_t sampleRateHz,
                               uint32_t presentationOffsetTicks) noexcept {
    Disarm();
    if (startEpoch.value == 0 || timelineEpoch == 0 ||
        ASFW::Audio::Runtime::HardwareSampleTimeline::
            NominalBusTicksPerFrame(sampleRateHz) == 0 ||
        !ASFW::Timing::initializeHostTimebase()) {
        state_ = Failed{startEpoch};
        return false;
    }
    startEpoch_ = startEpoch;
    timelineEpoch_ = timelineEpoch;
    presentationOffsetTicks_ = presentationOffsetTicks;
    auto step = Begin(startEpoch_);
    state_ = std::move(step.state);
    if (!std::holds_alternative<ApplyPreStreamConfiguration>(step.action)) {
        state_ = Failed{startEpoch_};
        return false;
    }
    step = Advance(PreStreamConfigurationSucceeded{startEpoch_});
    if (!std::holds_alternative<ConnectDeviceStreams>(step.action)) return false;
    step = Advance(DeviceStreamsConnected{startEpoch_});
    if (!std::holds_alternative<StartAsymmetricHostTransport>(step.action)) {
        return false;
    }
    step = Advance(HostTransportArmed{startEpoch_});
    if (!std::holds_alternative<ConfirmDeviceStart>(step.action)) return false;
    armed_ = true;
    return true;
}

void PresentationObserver::Disarm() noexcept {
    armed_ = false;
    timelineEpoch_ = 0;
    lastTransportGeneration_ = 0;
    observedGroupCount_ = 0;
    presentationOffsetTicks_ = 0;
    state_ = Stopped{startEpoch_};
}

PresentationObservationResult PresentationObserver::ObserveHardwareWake(
    uint64_t transportGeneration,
    uint64_t completionBusTicks,
    uint64_t correlationBusTicks,
    TxClockAnchor correlation,
    uint64_t sampleFrame,
    uint32_t frameCount) noexcept {
    PresentationObservationResult result{};
    if (!armed_ || transportGeneration == 0 ||
        transportGeneration <= lastTransportGeneration_ ||
        correlation.hostTicks == 0 || completionBusTicks == 0 ||
        correlationBusTicks == 0 ||
        observedGroupCount_ == std::numeric_limits<uint32_t>::max()) {
        return result;
    }
    lastTransportGeneration_ = transportGeneration;
    result.groupCount = ++observedGroupCount_;
    const auto step = Advance(TxGroupTimestampObserved{
        .epoch = startEpoch_,
        .groupCounter = observedGroupCount_,
        .anchor = correlation,
    });
    if (const auto* capture = std::get_if<PlantCaptureReference>(&step.action)) {
        result.captureReferencePlanted = true;
        result.captureReference = capture->anchor;
        return result;
    }
    if (!std::holds_alternative<PublishPlaybackClock>(step.action) ||
        frameCount == 0 || completionBusTicks >
            std::numeric_limits<uint64_t>::max() - presentationOffsetTicks_) {
        return result;
    }
    result.observationReady = true;
    result.observation = {
        .epoch = timelineEpoch_,
        .source = ASFW::Audio::Runtime::HardwareTimelineSource::Transmit,
        .sampleFrame = sampleFrame,
        .frameCount = frameCount,
        .presentationBusTicks = completionBusTicks + presentationOffsetTicks_,
        .correlationBusTicks = correlationBusTicks,
        .correlationHostTicks = correlation.hostTicks,
    };
    return result;
}

} // namespace ASFW::Audio::Families::BeBoB::MAudio
