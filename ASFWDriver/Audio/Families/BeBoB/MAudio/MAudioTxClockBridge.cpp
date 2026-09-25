// SPDX-License-Identifier: Apache-2.0
#include "MAudioTxClockBridge.hpp"
#include "../../../Protocols/BeBoB/MAudioInternalTxTiming.hpp"

#include <limits>

namespace ASFW::Audio::Families::BeBoB::MAudio {

bool TxClockBridge::Arm(const uint64_t startEpoch,
                        const uint32_t sampleRateHz,
                        const uint32_t zeroTimestampPeriodFrames,
                        const uint32_t presentationOffsetTicks) noexcept {
    Disarm();
    if (startEpoch == 0 ||
        sampleRateHz !=
            ASFW::Audio::BeBoB::kMAudioInternalTxSampleRateHz ||
        zeroTimestampPeriodFrames !=
            ASFW::Audio::Runtime::HardwareSampleTimeline::
                ZeroTimestampPeriodForRate(sampleRateHz)) {
        return false;
    }
    epoch_ = timeline_.BeginEpoch(
        ASFW::Audio::Runtime::HardwareTimelineSource::Transmit,
        ASFW::Audio::Runtime::HardwareTimelineDiscontinuity::StartIO,
        sampleRateHz, 0);
    if (epoch_ == 0 ||
        !observer_.Arm(StartEpoch{startEpoch}, epoch_, sampleRateHz,
                       presentationOffsetTicks)) {
        Disarm();
        return false;
    }
    return true;
}

void TxClockBridge::Disarm() noexcept {
    observer_.Disarm();
    epoch_ = 0;
    timeline_.Reset();
}

TxClockBoundaryResult TxClockBridge::ObserveWake(
    const uint64_t transportGeneration,
    const uint32_t correlationCycleTime,
    const uint64_t correlationBusTicks,
    const uint64_t correlationHostTicks,
    const uint64_t newestCompletionBusTicks,
    const std::span<const TxDataClockObservation> dataPackets) noexcept {
    TxClockBoundaryResult result{};
    if (epoch_ == 0 || correlationBusTicks == 0 || correlationHostTicks == 0 ||
        newestCompletionBusTicks == 0) {
        return result;
    }

    const TxDataClockObservation* newestData =
        dataPackets.empty() ? nullptr : &dataPackets.back();
    const uint64_t captureBusTicks = newestData
        ? newestData->completionBusTicks : newestCompletionBusTicks;
    const uint64_t captureCorrelationBusTicks = newestData
        ? newestData->correlationBusTicks : correlationBusTicks;
    const auto qualified = observer_.ObserveHardwareWake(
        transportGeneration, captureBusTicks, captureCorrelationBusTicks,
        {.cycleTime = correlationCycleTime,
         .hostTicks = correlationHostTicks},
        newestData ? newestData->sampleFrame : 0,
        newestData ? newestData->frameCount : 0);
    if (!qualified.observationReady || newestData == nullptr ||
        qualified.observation.presentationBusTicks <
            newestData->completionBusTicks) {
        return result;
    }

    const uint64_t presentationOffset =
        qualified.observation.presentationBusTicks -
        newestData->completionBusTicks;
    for (const auto& packet : dataPackets) {
        const uint64_t packetPresentationOffset =
            presentationOffset + packet.sytOffsetTicks;
        if (packet.frameCount == 0 || packet.completionBusTicks == 0 ||
            packet.correlationBusTicks == 0 ||
            packet.completionBusTicks > std::numeric_limits<uint64_t>::max() -
                                            packetPresentationOffset) {
            continue;
        }
        ASFW::Audio::Runtime::HardwareZeroTimestamp boundary{};
        const auto observed = timeline_.Observe({
            .epoch = epoch_,
            .source = ASFW::Audio::Runtime::HardwareTimelineSource::Transmit,
            .sampleFrame = packet.sampleFrame,
            .frameCount = packet.frameCount,
            .presentationBusTicks = packet.completionBusTicks +
                                    packetPresentationOffset,
            .correlationBusTicks = packet.correlationBusTicks,
            .correlationHostTicks = correlationHostTicks,
        }, &boundary);
        if (observed ==
            ASFW::Audio::Runtime::HardwareObservationResult::BoundaryReady) {
            result.ready = true;
            result.boundary = boundary;
        }
    }
    return result;
}

} // namespace ASFW::Audio::Families::BeBoB::MAudio
