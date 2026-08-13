// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MAudioTxClockAdapter.hpp"

#include "../../../../Common/TimingUtils.hpp"

#include <limits>
#include <utility>
#include <variant>

namespace ASFW::Audio::Families::BeBoB::MAudio {

ChoreographyStep TxClockAdapter::Advance(ChoreographyEvent event) noexcept {
    ChoreographyStep step =
        ::ASFW::Audio::Families::BeBoB::MAudio::Advance(state_, event);
    state_ = std::move(step.state);
    return step;
}

bool TxClockAdapter::Arm(StartEpoch epoch, uint32_t sampleRateHz,
                         uint32_t zeroTimestampPeriodFrames) noexcept {
    Disarm();
    epoch_ = epoch;
    sampleRateHz_ = sampleRateHz;
    zeroTimestampPeriodFrames_ = zeroTimestampPeriodFrames;
    hostNanosPerSampleQ8_ = sampleRateHz == 0
        ? 0
        : static_cast<uint32_t>((1'000'000'000ULL << 8) / sampleRateHz);
    if (sampleRateHz_ == 0 || zeroTimestampPeriodFrames_ == 0 ||
        hostNanosPerSampleQ8_ == 0 || !::ASFW::Timing::initializeHostTimebase()) {
        state_ = Failed{epoch_};
        return false;
    }

    ChoreographyStep step =
        ::ASFW::Audio::Families::BeBoB::MAudio::Begin(epoch_);
    state_ = std::move(step.state);
    if (!std::holds_alternative<ApplyPreStreamConfiguration>(step.action)) {
        state_ = Failed{epoch_};
        return false;
    }

    step = Advance(PreStreamConfigurationSucceeded{.epoch = epoch_});
    if (!std::holds_alternative<ConnectDeviceStreams>(step.action)) {
        state_ = Failed{epoch_};
        return false;
    }

    step = Advance(DeviceStreamsConnected{.epoch = epoch_});
    if (!std::holds_alternative<StartAsymmetricHostTransport>(step.action)) {
        state_ = Failed{epoch_};
        return false;
    }

    // The coordinator executes this action on the transport service. Here it
    // merely establishes the callback policy before TX can issue its first
    // timing wake. ConfirmDeviceStart remains device-side and never gates the
    // vendor's TX-derived playback clock.
    step = Advance(HostTransportArmed{.epoch = epoch_});
    if (!std::holds_alternative<ConfirmDeviceStart>(step.action)) {
        state_ = Failed{epoch_};
        return false;
    }

    armed_ = true;
    return true;
}

void TxClockAdapter::Disarm() noexcept {
    armed_ = false;
    lastTransportGeneration_ = 0;
    observedGroupCount_ = 0;
    sampleRateHz_ = 0;
    zeroTimestampPeriodFrames_ = 0;
    hostNanosPerSampleQ8_ = 0;
    playbackOriginValid_ = false;
    playbackOrigin_ = {};
    lastPublishedSampleFrame_ = 0;
    state_ = Stopped{epoch_};
}

TxClockPublication TxClockAdapter::ObserveHardwareWake(
    uint64_t transportGeneration, TxClockAnchor anchor) noexcept {
    TxClockPublication publication{};
    if (!armed_ || transportGeneration == 0 ||
        transportGeneration <= lastTransportGeneration_ ||
        anchor.hostTicks == 0 || observedGroupCount_ ==
            std::numeric_limits<uint32_t>::max()) {
        return publication;
    }

    lastTransportGeneration_ = transportGeneration;
    ++observedGroupCount_;
    const ChoreographyStep step = Advance(TxGroupTimestampObserved{
        .epoch = epoch_,
        .groupCounter = observedGroupCount_,
        .anchor = anchor,
    });

    if (const auto* plant = std::get_if<PlantCaptureReference>(&step.action)) {
        publication.captureReferencePlanted = true;
        publication.captureReference = plant->anchor;
        return publication;
    }
    if (const auto* publish = std::get_if<PublishPlaybackClock>(&step.action)) {
        return PublishPlaybackAnchor(publish->anchor);
    }
    return publication;
}

TxClockPublication TxClockAdapter::PublishPlaybackAnchor(
    TxClockAnchor source) noexcept {
    TxClockPublication publication{};
    publication.source = source;

    if (!playbackOriginValid_) {
        playbackOriginValid_ = true;
        playbackOrigin_ = source;
        lastPublishedSampleFrame_ = 0;
        publication.playbackAnchorReady = true;
        publication.sampleFrame = 0;
        publication.hostTicks = source.hostTicks;
        publication.hostNanosPerSampleQ8 = hostNanosPerSampleQ8_;
        return publication;
    }

    if (source.hostTicks < playbackOrigin_.hostTicks) {
        return publication;
    }

    const uint64_t elapsedNanos = ::ASFW::Timing::hostTicksToNanos(
        source.hostTicks - playbackOrigin_.hostTicks);
    const unsigned __int128 elapsedFramesWide =
        static_cast<unsigned __int128>(elapsedNanos) * sampleRateHz_ /
        ::ASFW::Timing::kNanosPerSecond;
    const uint64_t elapsedFrames = elapsedFramesWide >
            std::numeric_limits<uint64_t>::max()
        ? std::numeric_limits<uint64_t>::max()
        : static_cast<uint64_t>(elapsedFramesWide);
    const uint64_t boundaryFrame =
        (elapsedFrames / zeroTimestampPeriodFrames_) *
        zeroTimestampPeriodFrames_;
    if (boundaryFrame <= lastPublishedSampleFrame_) {
        return publication;
    }

    const unsigned __int128 boundaryNanosWide =
        static_cast<unsigned __int128>(boundaryFrame) *
        ::ASFW::Timing::kNanosPerSecond / sampleRateHz_;
    if (boundaryNanosWide > std::numeric_limits<uint64_t>::max()) {
        return publication;
    }
    const uint64_t boundaryTicks = ::ASFW::Timing::nanosToHostTicks(
        static_cast<uint64_t>(boundaryNanosWide));
    if (boundaryTicks == 0 || playbackOrigin_.hostTicks >
            std::numeric_limits<uint64_t>::max() - boundaryTicks) {
        return publication;
    }

    lastPublishedSampleFrame_ = boundaryFrame;
    publication.playbackAnchorReady = true;
    publication.sampleFrame = boundaryFrame;
    publication.hostTicks = playbackOrigin_.hostTicks + boundaryTicks;
    publication.hostNanosPerSampleQ8 = hostNanosPerSampleQ8_;
    return publication;
}

} // namespace ASFW::Audio::Families::BeBoB::MAudio
