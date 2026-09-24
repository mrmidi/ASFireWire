// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Wire choreography cross-checked with Linux bebob_maudio.c and bebob_stream.c.

#include "MAudioSpecialProtocol.hpp"

#include "MAudioSpecialFormation.hpp"
#include "MAudioSpecialRouting.hpp"
#include "MAudioSpecialStartPolicy.hpp"
#include "../../../Protocols/AVC/AVCCommand.hpp"
#include "../../../Protocols/AVC/MAudioSpecialCommand.hpp"
#include "../../../Protocols/AVC/StreamFormats/AVCUnitPlugSignalFormatCommand.hpp"

#include <DriverKit/IOLib.h>

#include <algorithm>
#include <memory>

namespace ASFW::Audio::BeBoB {
namespace {

using SignalCommand = Protocols::AVC::StreamFormats::AVCUnitPlugSignalFormatCommand;
using SignalRate = Protocols::AVC::StreamFormats::SampleRate;

[[nodiscard]] IOReturn ToIOReturn(Protocols::AVC::AVCResult result) noexcept {
    using Protocols::AVC::AVCResult;
    switch (result) {
        case AVCResult::kAccepted:
        case AVCResult::kImplementedStable:
        case AVCResult::kChanged:
            return kIOReturnSuccess;
        case AVCResult::kNotImplemented:
            return kIOReturnUnsupported;
        case AVCResult::kInTransition:
        case AVCResult::kInterim:
        case AVCResult::kBusy:
            return kIOReturnBusy;
        case AVCResult::kTimeout:
            return kIOReturnTimeout;
        case AVCResult::kBusReset:
            return kIOReturnNotResponding;
        default:
            return kIOReturnError;
    }
}

[[nodiscard]] SignalRate ToSignalRate(uint32_t hz) noexcept {
    switch (hz) {
        case 44100: return SignalRate::k44100Hz;
        case 48000: return SignalRate::k48000Hz;
        case 88200: return SignalRate::k88200Hz;
        case 96000: return SignalRate::k96000Hz;
        case 176400: return SignalRate::k176400Hz;
        case 192000: return SignalRate::k192000Hz;
        default: return SignalRate::kUnknown;
    }
}

[[nodiscard]] AudioStreamRuntimeCaps MakeCaps(uint32_t rateHz) noexcept {
    const auto formation = MAudioFormationFor(MAudioDigitalFormat::SPDIF,
                                               MAudioDigitalFormat::SPDIF, rateHz);
    if (!formation) return {};
    AudioStreamRuntimeCaps caps{
        .hostInputPcmChannels = formation->capturePcmChannels,
        .hostOutputPcmChannels = formation->playbackPcmChannels,
        .deviceToHostAm824Slots = formation->capturePcmChannels + formation->midiDataBlocks,
        .hostToDeviceAm824Slots = formation->playbackPcmChannels + formation->midiDataBlocks,
        .sampleRateHz = rateHz,
        .deviceToHostIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .hostToDeviceIsoChannel = AudioStreamRuntimeCaps::kInvalidIsoChannel,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };
    // The MIDI ports are multiplexed in one AM824 data block in each direction.
    caps.deviceToHostStreams[0] = {
        .pcmChannels = static_cast<uint16_t>(formation->capturePcmChannels),
        .am824Slots = static_cast<uint16_t>(formation->capturePcmChannels +
                                            formation->midiDataBlocks),
    };
    caps.hostToDeviceStreams[0] = {
        .pcmChannels = static_cast<uint16_t>(formation->playbackPcmChannels),
        .am824Slots = static_cast<uint16_t>(formation->playbackPcmChannels +
                                            formation->midiDataBlocks),
    };
    return caps;
}

} // namespace

MAudioSpecialProtocol::MAudioSpecialProtocol(
    Protocols::Ports::FireWireBusOps& busOps,
    Protocols::Ports::FireWireBusInfo& busInfo,
    Discovery::DeviceRouteToken route,
    IRM::IRMClient* irmClient,
    CMP::CMPClient* cmpClient,
    Scheduling::ITimerScheduler* timerScheduler,
    bool isFireWire1814) noexcept
    : BeBoBProtocol(busOps, busInfo, route, irmClient, cmpClient, timerScheduler),
      isFireWire1814_(isFireWire1814),
      busOps_(busOps),
      alive_(std::make_shared<std::atomic<bool>>(true)),
      runtimeContextEpoch_(std::make_shared<std::atomic<uint64_t>>(1)),
      routingAppliedGeneration_(std::make_shared<std::atomic<uint32_t>>(0)) {}

MAudioSpecialProtocol::~MAudioSpecialProtocol() {
    alive_->store(false);
    runtimeContextEpoch_->fetch_add(1, std::memory_order_acq_rel);
    routingAppliedGeneration_->store(0, std::memory_order_release);
    CancelPostStartTimer();
}

IOReturn MAudioSpecialProtocol::Shutdown() {
    alive_->store(false);
    runtimeContextEpoch_->fetch_add(1, std::memory_order_acq_rel);
    routingAppliedGeneration_->store(0, std::memory_order_release);
    CancelPostStartTimer();
    return BeBoBProtocol::Shutdown();
}

void MAudioSpecialProtocol::UpdateRuntimeContext(
    const Discovery::DeviceRouteToken& route,
    Protocols::AVC::FCPTransport* transport) {
    if (route_ != route || fcpTransport_ != transport) {
        runtimeContextEpoch_->fetch_add(1, std::memory_order_acq_rel);
        routingAppliedGeneration_->store(0, std::memory_order_release);
        CancelPostStartTimer();
    }
    BeBoBProtocol::UpdateRuntimeContext(route, transport);
}

void MAudioSpecialProtocol::ConfigureMixer(MixerFailurePolicy,
                                            MixerCompletion completion) {
    const Discovery::DeviceRouteToken route = route_;
    const uint32_t generation = route.generation.value;
    if (generation == 0 || busInfo_.GetGeneration() != route.generation) {
        completion(kIOReturnNotReady);
        return;
    }
    if (routingAppliedGeneration_->load(std::memory_order_acquire) == generation) {
        completion(kIOReturnSuccess);
        return;
    }

    const auto operationalNode = Discovery::TryOperationalNodeId(route.nodeId);
    if (!operationalNode) {
        completion(kIOReturnNoDevice);
        return;
    }

    const auto write = BuildMAudioSpecialRoutingWrite();
    const Async::FWAddress address{Async::FWAddress::QualifiedAddressParts{
        .addressHi = write.addressHi,
        .addressLo = write.addressLo,
        .nodeID = *operationalNode,
    }};
    const uint64_t contextEpoch =
        runtimeContextEpoch_->load(std::memory_order_acquire);
    const auto alive = alive_;
    const auto contextState = runtimeContextEpoch_;
    const auto appliedGeneration = routingAppliedGeneration_;
    auto completed = std::make_shared<std::atomic<bool>>(false);
    auto finish = [alive, contextState, contextEpoch, appliedGeneration,
                   generation, completed,
                   completion = std::move(completion)](IOReturn status) mutable {
        if (completed->exchange(true, std::memory_order_acq_rel)) return;
        // The base clock continuation captures this protocol. Once shutdown
        // begins, it must not run from a late bus completion.
        if (!alive->load(std::memory_order_acquire)) return;
        if (contextState->load(std::memory_order_acquire) != contextEpoch) {
            completion(kIOReturnAborted);
            return;
        }
        if (status == kIOReturnSuccess) {
            appliedGeneration->store(generation, std::memory_order_release);
        }
        completion(status);
    };

    const Async::AsyncHandle handle = busOps_.WriteBlock(
        route.generation, FW::NodeId{*operationalNode}, address, write.bytes,
        FW::FwSpeed::S100,
        [finish](Async::AsyncStatus status,
                 std::span<const uint8_t>) mutable {
            finish(status == Async::AsyncStatus::kSuccess
                       ? kIOReturnSuccess
                       : kIOReturnIOError);
        });
    if (handle.value == 0) {
        finish(kIOReturnNoResources);
    }
}

void MAudioSpecialProtocol::CancelPostStartTimer() {
    if (timerScheduler_ && postStartTimer_ != Scheduling::kInvalidTimerToken) {
        timerScheduler_->Cancel(postStartTimer_);
        postStartTimer_ = Scheduling::kInvalidTimerToken;
    }
    if (postStartCompletion_) {
        auto pending = std::move(postStartCompletion_);
        CompletePostStart(pending, kIOReturnAborted, {});
    }
}

void MAudioSpecialProtocol::FinishPostStart(
    const std::shared_ptr<PendingPostStart>& pending, IOReturn status,
    DuplexConfirmResult result) {
    if (postStartCompletion_ == pending) {
        postStartCompletion_.reset();
    }
    CompletePostStart(pending, status, result);
}

void MAudioSpecialProtocol::CompletePostStart(
    const std::shared_ptr<PendingPostStart>& pending, IOReturn status,
    DuplexConfirmResult result) {
    if (!pending || pending->completed.exchange(true)) return;
    auto callback = std::move(pending->callback);
    if (callback) callback(status, result);
}

const char* MAudioSpecialProtocol::GetName() const {
    return isFireWire1814_ ? "M-Audio FireWire 1814" : "M-Audio ProjectMix I/O";
}

bool MAudioSpecialProtocol::GetRuntimeAudioStreamCaps(AudioStreamRuntimeCaps& outCaps) const {
    outCaps = DeviceCaps();
    return true;
}

AudioStreamRuntimeCaps MAudioSpecialProtocol::DeviceCaps() const {
    return MakeCaps(appliedRateHz_);
}

std::vector<uint32_t> MAudioSpecialProtocol::SupportedRates() const {
    const size_t count = isFireWire1814_ ? kMAudioFireWire1814RateCount
                                         : kMAudioProjectMixRateCount;
    return std::vector<uint32_t>(kMAudioSpecialRatesHz,
                                 kMAudioSpecialRatesHz + count);
}

void MAudioSpecialProtocol::ApplyClockConfig(const AudioClockConfig& desiredClock,
                                             ClockApplyCallback callback) {
    CancelClockApply();
    CancelPostStartTimer();
    if (!fcpTransport_) {
        callback(kIOReturnNotReady, {});
        return;
    }
    const auto rates = SupportedRates();
    if (std::find(rates.begin(), rates.end(), desiredClock.sampleRateHz) == rates.end()) {
        callback(kIOReturnUnsupported, {});
        return;
    }

    const auto clockCdb = Protocols::AVC::BuildMAudioSpecialInitialClockCommand();
    if (!clockCdb) {
        callback(kIOReturnBadArgument, {});
        return;
    }
    auto command = std::make_shared<Protocols::AVC::AVCCommand>(*fcpTransport_, *clockCdb);
    command->Submit([this, alive = alive_, desiredClock,
                     callback = std::move(callback), command](
                        Protocols::AVC::AVCResult result,
                        const Protocols::AVC::AVCCdb&) mutable {
        if (!alive->load()) {
            callback(kIOReturnAborted, {});
            return;
        }
        const IOReturn status = ToIOReturn(result);
        if (status != kIOReturnSuccess) {
            callback(status, {});
            return;
        }
        BeBoBProtocol::ApplyClockConfig(
            desiredClock,
            [this, desiredClock, callback = std::move(callback)](
                IOReturn applyStatus, DuplexClockApplyResult applyResult) mutable {
                if (applyStatus == kIOReturnSuccess) {
                    appliedRateHz_ = desiredClock.sampleRateHz;
                }
                callback(applyStatus, applyResult);
            });
    });
}

void MAudioSpecialProtocol::ConfirmDuplexStart(ConfirmCallback callback) {
    CancelPostStartTimer();
    const uint32_t rateHz = appliedRateHz_;
    auto pending = std::make_shared<PendingPostStart>();
    pending->callback = std::move(callback);
    postStartCompletion_ = pending;
    BeBoBProtocol::ConfirmDuplexStart(
        [this, alive = alive_, rateHz, pending](
            IOReturn status, DuplexConfirmResult result) mutable {
            if (pending->completed.load() || !alive->load()) {
                CompletePostStart(pending, kIOReturnAborted, {});
                return;
            }
            if (status != kIOReturnSuccess) {
                FinishPostStart(pending, status, result);
                return;
            }
            if (!timerScheduler_) {
                FinishPostStart(pending, kIOReturnAborted, {});
                return;
            }
            SetPostStartOutput(rateHz, [this, alive, rateHz, result, pending](
                                           IOReturn outputStatus) mutable {
                if (pending->completed.load() || !alive->load()) {
                    CompletePostStart(pending, kIOReturnAborted, {});
                    return;
                }
                if (outputStatus != kIOReturnSuccess) {
                    FinishPostStart(pending, outputStatus, result);
                    return;
                }
                if (!timerScheduler_) {
                    FinishPostStart(pending, kIOReturnAborted, {});
                    return;
                }
                postStartTimer_ = timerScheduler_->ScheduleAfter(
                    static_cast<uint64_t>(kMAudioPostStartInputDelayMs) *
                        1000ULL * 1000ULL,
                    [this, alive, rateHz, result, pending]() mutable {
                        if (pending->completed.load() || !alive->load()) {
                            CompletePostStart(pending, kIOReturnAborted, {});
                            return;
                        }
                        postStartTimer_ = Scheduling::kInvalidTimerToken;
                        SetPostStartInput(rateHz, [this, alive, pending, result](
                                                   IOReturn inputStatus) mutable {
                            if (pending->completed.load() || !alive->load()) {
                                CompletePostStart(pending, kIOReturnAborted, {});
                                return;
                            }
                            FinishPostStart(pending, inputStatus, result);
                        });
                    });
                if (postStartTimer_ == Scheduling::kInvalidTimerToken) {
                    FinishPostStart(pending, kIOReturnNoResources, result);
                }
            });
        });
}

void MAudioSpecialProtocol::SetSignalFormat(uint32_t rateHz, bool input,
                                             std::function<void(IOReturn)> completion) {
    if (!fcpTransport_) {
        completion(kIOReturnNotReady);
        return;
    }
    const SignalRate rate = ToSignalRate(rateHz);
    if (rate == SignalRate::kUnknown) {
        completion(kIOReturnUnsupported);
        return;
    }
    auto command = std::make_shared<SignalCommand>(*fcpTransport_, 0, input, rate);
    command->Submit([completion = std::move(completion), command](
                        Protocols::AVC::AVCResult result,
                        const SignalCommand::SignalFormat&) mutable {
        completion(ToIOReturn(result));
    });
}

void MAudioSpecialProtocol::SetPostStartOutput(
    uint32_t rateHz, std::function<void(IOReturn)> completion) {
    SetSignalFormat(rateHz, false, std::move(completion));
}

void MAudioSpecialProtocol::SetPostStartInput(
    uint32_t rateHz, std::function<void(IOReturn)> completion) {
    SetSignalFormat(rateHz, true, std::move(completion));
}

} // namespace ASFW::Audio::BeBoB
