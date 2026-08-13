// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "MAudioDuplexStartAdapter.hpp"

#include "../../../../Common/TimingUtils.hpp"
#include "../../../../Logging/Logging.hpp"

#include <utility>
#include <variant>

namespace ASFW::Audio::Families::BeBoB::MAudio {

void DuplexStartAdapter::Begin(StartEpoch epoch) noexcept {
    epoch_ = epoch;
    ChoreographyStep step =
        ::ASFW::Audio::Families::BeBoB::MAudio::Begin(epoch_);
    state_ = std::move(step.state);
    if (!std::holds_alternative<ApplyPreStreamConfiguration>(step.action)) {
        (void)UnexpectedAction("begin");
    }
}

ChoreographyStep DuplexStartAdapter::Advance(ChoreographyEvent event) noexcept {
    ChoreographyStep step =
        ::ASFW::Audio::Families::BeBoB::MAudio::Advance(state_, event);
    state_ = std::move(step.state);
    return step;
}

kern_return_t DuplexStartAdapter::UnexpectedAction(const char* stage) const noexcept {
    ASFW_LOG_ERROR(Audio,
                   "[MAudioDuplex] unexpected choreography action stage=%{public}s "
                   "epoch=%llu state=%{public}s",
                   stage ? stage : "unknown", epoch_.value, StateName(state_));
    return kIOReturnInternalError;
}

kern_return_t DuplexStartAdapter::OnPreStreamConfigurationSucceeded() noexcept {
    const ChoreographyStep step =
        Advance(PreStreamConfigurationSucceeded{.epoch = epoch_});
    return std::holds_alternative<ConnectDeviceStreams>(step.action)
        ? kIOReturnSuccess
        : UnexpectedAction("pre-stream-complete");
}

void DuplexStartAdapter::OnPreStreamConfigurationFailed() noexcept {
    (void)Advance(PreStreamConfigurationFailed{.epoch = epoch_});
}

HostTransportArmResult
DuplexStartAdapter::OnDeviceStreamsConnectedAndArmHost() noexcept {
    HostTransportArmResult result{};
    const ChoreographyStep beginArm =
        Advance(DeviceStreamsConnected{.epoch = epoch_});
    const auto* action =
        std::get_if<StartAsymmetricHostTransport>(&beginArm.action);
    if (!action) {
        result.status = UnexpectedAction("device-streams-connected");
        return result;
    }

    // Sample once before either RUN write. The vendor uses this same bus-time
    // origin for immediate TX and delayed RX; a wall-clock sleep here would
    // introduce a topology- and scheduler-dependent phase error.
    const uint32_t now = hardware_.ReadCycleTime();
    result.receiveStartCycleTimer =
        ::ASFW::Timing::AddCyclesToCycleTimer(
            now, action->receiveStartLeadCycles);
    ASFW_LOG(Audio,
             "[MAudioDuplex] arm epoch=%llu txLead=%u rxLead=%u prefill=%u "
             "now=0x%08x rxAt=0x%08x",
             epoch_.value, action->transmitStartLeadCycles,
             action->receiveStartLeadCycles, action->transmitPrefillSegments,
             now, result.receiveStartCycleTimer);

    // IsochTransmitContext::Start validates and primes the opaque descriptor
    // ring before RUN. AudioDriverKit has already committed its full startup
    // NODATA lap, which is stronger than the vendor's four-segment prefill.
    result.status = hostTransport_.StartPreparedTransmit();
    if (result.status != kIOReturnSuccess) {
        (void)Advance(HostTransportArmFailed{.epoch = epoch_});
        return result;
    }
    result.transmitStarted = true;

    result.status =
        hostTransport_.StartPreparedReceiveAtCycle(result.receiveStartCycleTimer);
    if (result.status != kIOReturnSuccess) {
        (void)Advance(HostTransportArmFailed{.epoch = epoch_});
        return result;
    }
    result.receiveStarted = true;

    const ChoreographyStep armed = Advance(HostTransportArmed{.epoch = epoch_});
    if (!std::holds_alternative<ConfirmDeviceStart>(armed.action)) {
        result.status = UnexpectedAction("host-transport-armed");
    }
    return result;
}

void DuplexStartAdapter::OnDeviceStreamsConnectionFailed() noexcept {
    (void)Advance(DeviceStreamsConnectionFailed{.epoch = epoch_});
}

kern_return_t DuplexStartAdapter::OnDeviceStartConfirmed() noexcept {
    const ChoreographyStep step = Advance(DeviceStartConfirmed{.epoch = epoch_});
    return std::holds_alternative<Done>(step.action)
        ? kIOReturnSuccess
        : UnexpectedAction("device-start-confirmed");
}

void DuplexStartAdapter::OnDeviceStartRejected() noexcept {
    (void)Advance(DeviceStartRejected{.epoch = epoch_});
}

} // namespace ASFW::Audio::Families::BeBoB::MAudio
