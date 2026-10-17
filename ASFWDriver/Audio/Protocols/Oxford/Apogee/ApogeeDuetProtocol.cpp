// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetProtocol.cpp - Apogee Duet composition point.
//
// The duplex lifecycle and clock-transition FSM moved to ApogeeDuetDuplex
// (FW-127); what remains is construction plus the Duet's control surface -
// parameters, meters and the Oxford ID registers - each delegating to the piece
// that owns it.

#include "ApogeeDuetProtocol.hpp"

#include "ApogeeParamsSerdes.hpp"
#include "ApogeeTransport.hpp"
#include "../../../Controls/ControlNotifications.hpp"
#include "../../../Model/Topology/Validate.hpp"
#include <algorithm>
#include <bit>

#include "../../../../Common/CallbackUtils.hpp"
#include "../../../../Protocols/AVC/CMP/CMPClient.hpp"
#include "../../../../Protocols/AVC/FCPTransport.hpp"

#include <memory>
#include <vector>

namespace ASFW::Audio::Oxford::Apogee {


ApogeeDuetProtocol::ApogeeDuetProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                       Protocols::Ports::FireWireBusInfo& busInfo,
                                       Discovery::DeviceRouteToken route,
                                       Discovery::DeviceRegistry* routeRegistry,
                                       IRM::IRMClient* irmClient,
                                       CMP::CMPClient* cmpClient,
                                       uint32_t formatSettleDelayMs,
                                       Scheduling::ITimerScheduler* timerScheduler)
    : runtime_{
          .busOps = busOps,
          .busInfo = busInfo,
          .route = route,
          .routeRegistry = routeRegistry,
          .irmClient = irmClient,
          .cmpClient = cmpClient,
          .timerScheduler = timerScheduler,
          .formatSettleDelayMs = formatSettleDelayMs,
      }
    , duplex_(runtime_) {
}

IOReturn ApogeeDuetProtocol::Initialize() {
    return kIOReturnSuccess;
}

IOReturn ApogeeDuetProtocol::Shutdown() {
    StopControlPoll(); controlsReady_ = false; ++controlsEpoch_; ResetControlQueue();
    duplex_.Shutdown();
    return kIOReturnSuccess;
}


// Dispatch lives in ApogeeTransport (FW-129); these forward the protocol's
// current transport into it.
void ApogeeDuetProtocol::SendVendorCommand(const VendorCommand& command,
                                           bool isStatus,
                                           VendorResultCallback callback) {
    VendorFcp::Send(runtime_.avcUnit, command, isStatus, std::move(callback));
}

void ApogeeDuetProtocol::ExecuteVendorSequence(const std::vector<VendorCommand>& commands,
                                               bool isStatus,
                                               VendorSequenceCallback callback) {
    const auto unit = runtime_.avcUnit;
    const auto route = runtime_.route;
    VendorFcp::ExecuteSequence(unit, commands, isStatus, std::move(callback),
        [unit, route] { return unit && unit->IsCurrentRoute(route); });
}

void ApogeeDuetProtocol::GetKnobState(ResultCallback<KnobState> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildKnobStateQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess || responses.empty()) {
                Common::InvokeSharedCallback(callbackState,
                                             status != kIOReturnSuccess ? status : kIOReturnError,
                                             KnobState{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseKnobState(responses[0]));
        });
}

void ApogeeDuetProtocol::SetKnobState(const KnobState& state, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        {ParamsSerdes::BuildKnobStateControl(state)},
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetOutputParams(ResultCallback<OutputParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildOutputParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, OutputParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseOutputParams(responses));
        });
}

void ApogeeDuetProtocol::SetOutputParams(const OutputParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildOutputParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetInputParams(ResultCallback<InputParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildInputParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, InputParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseInputParams(responses));
        });
}

void ApogeeDuetProtocol::SetInputParams(const InputParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildInputParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetMixerParams(ResultCallback<MixerParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildMixerParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, MixerParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseMixerParams(responses));
        });
}

void ApogeeDuetProtocol::SetMixerParams(const MixerParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildMixerParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetDisplayParams(ResultCallback<DisplayParams> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildDisplayParamsQuery(),
        true,
        [callbackState](IOReturn status, const std::vector<VendorCommand>& responses) {
            if (status != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, status, DisplayParams{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, ParamsSerdes::ParseDisplayParams(responses));
        });
}

void ApogeeDuetProtocol::SetDisplayParams(const DisplayParams& params, VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        ParamsSerdes::BuildDisplayParamsControl(params),
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::ClearDisplay(VoidCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    ExecuteVendorSequence(
        {VendorCommand::Make(VendorCommand::Code::DisplayClear)},
        false,
        [callbackState](IOReturn status, const std::vector<VendorCommand>&) {
            Common::InvokeSharedCallback(callbackState, status);
        });
}

void ApogeeDuetProtocol::GetInputMeter(ResultCallback<InputMeterState> callback) {
    MeterRegisters::ReadInput(runtime_.busOps, MakeRouteProvider(), std::move(callback));
}

void ApogeeDuetProtocol::GetMixerMeter(ResultCallback<MixerMeterState> callback) {
    MeterRegisters::ReadMixer(runtime_.busOps, MakeRouteProvider(), std::move(callback));
}

// The Oxford ASIC registers are chip-common, not Duet-specific (FW-137), so
// the register map and decode live in Oxford/OxfordCsr. What stays here is the
// Duet's own route policy, expressed as the provider that layer resolves through.
//
// FW-142: resolve the route per use, straight from the registry.
//
// This originally validated `runtime_.route` — a token snapshotted at
// construction — through `runtime_.cmpClient`. That failed for a blunter reason
// than a stale epoch: the two construction sites bind *different* optional
// clients. The discovery prefetch path, which is where the CSR and meter reads
// run, passes a real FCP transport and **null** IRM/CMP clients
// (AVCDiscovery.cpp), so the validator short-circuited on the null check and
// reported "route not current" without ever comparing a route. Node and
// generation matched in the log because nothing examined them.
//
// The registry is the only route authority both sites can supply, which is why
// it is now its own constructor parameter rather than something borrowed from a
// client that may not exist. FCPTransport re-resolves the same way per
// submission (FCPTransport.cpp:206), which is why vendor commands never failed.
//
// The lambda captures the registry pointer and GUID by value rather than `this`,
// so an in-flight read cannot outlive the protocol and dereference it.
Oxford::RouteProvider ApogeeDuetProtocol::MakeRouteProvider() const {
    if (runtime_.routeRegistry == nullptr) {
        // Distinct from "the device has no live route": this is a wiring bug,
        // and returning an empty provider is what makes the reads say so.
        return {};
    }
    return [registry = runtime_.routeRegistry,
            guid = runtime_.route.guid]() -> std::optional<Discovery::DeviceRouteToken> {
        return registry->CurrentRoute(guid);
    };
}

void ApogeeDuetProtocol::GetFirmwareId(ResultCallback<uint32_t> callback) {
    Oxford::ReadFirmwareId(runtime_.busOps, MakeRouteProvider(), std::move(callback));
}

void ApogeeDuetProtocol::GetHardwareId(ResultCallback<uint32_t> callback) {
    Oxford::ReadHardwareId(runtime_.busOps, MakeRouteProvider(), std::move(callback));
}


std::optional<ASFW::Audio::Controls::ControlSnapshot> ApogeeDuetProtocol::CopySnapshot() const {
    return controlsReady_ ? controlSnapshot_ : std::nullopt;
}

void ApogeeDuetProtocol::ReadControlGroup(DuetControlGroup group, VoidCallback done) {
    auto self = std::static_pointer_cast<ApogeeDuetProtocol>(weak_from_this().lock());
    if (!self || !runtime_.avcUnit) { done(kIOReturnNotReady); return; }
    const auto epoch = controlsEpoch_;
    auto finish = [self, epoch, done](IOReturn status, auto apply) {
        if (epoch != self->controlsEpoch_) { done(kIOReturnAborted); return; }
        if (status == kIOReturnSuccess) apply();
        done(status);
    };
    switch (group) {
        case DuetControlGroup::Input:
            GetInputParams([self, finish](auto status, auto input) { finish(status, [self, input] { self->controls_.input = input; }); }); break;
        case DuetControlGroup::Output:
            GetOutputParams([self, finish](auto status, auto output) { finish(status, [self, output] { self->controls_.output = output; }); }); break;
        case DuetControlGroup::Mixer:
            GetMixerParams([self, finish](auto status, auto mixer) { finish(status, [self, mixer] { self->controls_.mixer = mixer; }); }); break;
        case DuetControlGroup::Display:
            GetDisplayParams([self, finish](auto status, auto display) { finish(status, [self, display] { self->controls_.display = display; }); }); break;
        case DuetControlGroup::Knob:
            GetKnobState([self, finish](auto status, auto knob) { finish(status, [self, knob] {
                self->controls_.knob = knob;
                self->controls_.input.gains = knob.inputGains;
                // Same register as OutVolume: back to raw attenuation.
                self->controls_.output.volume = static_cast<uint8_t>(KnobState::kOutputVolMax - knob.outputVolume);
                self->controls_.output.mute = knob.outputMute;
            }); }); break;
        case DuetControlGroup::Action: done(kIOReturnSuccess); break;
    }
}
void ApogeeDuetProtocol::Prepare(std::function<void(IOReturn)> done) {
    if (controlsReady_) { done(kIOReturnSuccess); return; }
    if (controlBusy_ || !runtime_.avcUnit) { done(kIOReturnNotReady); return; }
    auto self = std::static_pointer_cast<ApogeeDuetProtocol>(weak_from_this().lock());
    if (!self) { done(kIOReturnNotReady); return; }
    controlBusy_ = true;
    const auto epoch = controlsEpoch_;
    // Each continuation owns the protocol. No queue waits while FCP is pending.
    auto finish = [self, epoch, done](IOReturn status) {
        if (epoch != self->controlsEpoch_) { done(kIOReturnAborted); return; }
        if (status == kIOReturnSuccess) {
            self->controlsReady_ = true;
            if (!self->CommitControls()) { self->controlsReady_ = false; status = kIOReturnError; }
        }
        self->ReleaseControl();
        if (status == kIOReturnSuccess) self->ScheduleControlPoll();
        done(status);
    };
    ReadControlGroup(DuetControlGroup::Input, [self, finish](auto status) {
        if (status != kIOReturnSuccess) { finish(status); return; }
        self->ReadControlGroup(DuetControlGroup::Output, [self, finish](auto status) {
            if (status != kIOReturnSuccess) { finish(status); return; }
            self->ReadControlGroup(DuetControlGroup::Mixer, [self, finish](auto status) {
                if (status != kIOReturnSuccess) { finish(status); return; }
                self->ReadControlGroup(DuetControlGroup::Display, [self, finish](auto status) {
                    if (status != kIOReturnSuccess) { finish(status); return; }
                    self->ReadControlGroup(DuetControlGroup::Knob, finish);
                });
            });
        });
    });
}
bool ApogeeDuetProtocol::CommitControls() {
    namespace AM = ASFW::AudioModel;
    namespace C = ASFW::Audio::Controls;
    auto next = BuildDuetControls(controls_, runtime_.route.routeEpoch,
        controlSnapshot_ ? controlSnapshot_->sequence : 0);
    if (!AM::validate(next.topology) || !AM::validateState(next.topology, next.state)) return false;
    if (!controlSnapshot_ || controlSnapshot_->topology.revision != next.topology.revision) {
        const bool rebound = controlSnapshot_.has_value();
        if (rebound) next.sequence = C::ControlNotifications::Shared().NextSequence();
        controlSnapshot_ = std::move(next);
        if (rebound) C::ControlNotifications::Shared().Publish({runtime_.route.guid,
            controlSnapshot_->topology.revision, controlSnapshot_->sequence, 0, 4, 0});
        return true;
    }
    bool metadata = false;
    for (const auto& [id, state] : next.state.routers) {
        const auto old = controlSnapshot_->state.routers.find(id);
        if (old == controlSnapshot_->state.routers.end() || old->second.activeBundles != state.activeBundles) metadata = true;
    }
    std::vector<C::ParameterChanged> events;
    for (const auto& [id, state] : next.state.parameters) {
        const auto old = controlSnapshot_->state.parameters.find(id);
        if (old == controlSnapshot_->state.parameters.end()) { metadata = true; continue; }
        if (old->second.domainOverride != state.domainOverride || old->second.applicable != state.applicable) metadata = true;
        if (old->second.value == state.value) continue;
        C::ParameterChanged event{runtime_.route.guid, next.topology.revision, 0, id.value, 0, 0};
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, bool>) { event.kind = 0; event.bits = value; }
            else if constexpr (std::is_same_v<T, double>) { event.kind = 1; event.bits = std::bit_cast<uint64_t>(value); }
            else if constexpr (std::is_same_v<T, int64_t>) { event.kind = 2; event.bits = static_cast<uint64_t>(value); }
        }, state.value);
        events.push_back(event);
    }
    if (metadata) events = {{runtime_.route.guid, next.topology.revision, 0, 0, 4, 0}};
    for (auto& event : events) event.sequence = next.sequence = C::ControlNotifications::Shared().NextSequence();
    controlSnapshot_ = std::move(next); // exact values/watermark before fan-out
    for (const auto& event : events) C::ControlNotifications::Shared().Publish(event);
    return true;
}
void ApogeeDuetProtocol::QueueControl(std::function<void(ApogeeDuetProtocol&)> op, Completion refuse) {
    if (controlWaiters_.size() >= kMaxControlWaiters) { refuse(kIOReturnBusy, false); return; }
    controlWaiters_.push_back([weak = weak_from_this(), op = std::move(op), refuse = std::move(refuse)] {
        const auto owner = weak.lock();
        if (!owner) { refuse(kIOReturnAborted, false); return; }
        op(*static_cast<ApogeeDuetProtocol*>(owner.get()));
    });
}
void ApogeeDuetProtocol::ReleaseControl() {
    controlBusy_ = false;
    // A waiter can finish without starting I/O (a refused value): keep going
    // until one holds the device or none is left.
    while (!controlBusy_ && !controlWaiters_.empty()) {
        auto next = std::move(controlWaiters_.front());
        controlWaiters_.pop_front();
        next();
    }
}
void ApogeeDuetProtocol::ResetControlQueue() {
    // Every waiter gets an answer: re-entered after controlsReady_ was cleared
    // it is refused NotReady; once the protocol is gone, Aborted.
    controlBusy_ = false;
    auto waiters = std::move(controlWaiters_);
    controlWaiters_.clear();
    for (auto& waiter : waiters) waiter();
}
void ApogeeDuetProtocol::StopControlPoll() {
    if (runtime_.timerScheduler && controlPoll_ != Scheduling::kInvalidTimerToken)
        runtime_.timerScheduler->Cancel(controlPoll_);
    controlPoll_ = Scheduling::kInvalidTimerToken;
}
void ApogeeDuetProtocol::ScheduleControlPoll() {
    StopControlPoll();
    if (!controlsReady_ || !runtime_.timerScheduler) return;
    const auto weak = weak_from_this();
    controlPoll_ = runtime_.timerScheduler->ScheduleAfter(500'000'000, [weak] {
        const auto owner = weak.lock();
        if (!owner) return;
        auto* self = static_cast<ApogeeDuetProtocol*>(owner.get());
        self->controlPoll_ = Scheduling::kInvalidTimerToken;
        self->PollControls();
    });
}
void ApogeeDuetProtocol::PollControls() {
    if (!controlsReady_) return;
    if (controlBusy_) { ScheduleControlPoll(); return; }
    auto self = std::static_pointer_cast<ApogeeDuetProtocol>(weak_from_this().lock());
    if (!self) return;
    controlBusy_ = true;
    const auto epoch = controlsEpoch_;
    ReadControlGroup(DuetControlGroup::Knob, [self, epoch](auto status) {
        if (epoch != self->controlsEpoch_) return;
        if (status == kIOReturnSuccess) (void)self->CommitControls();
        self->ReleaseControl();
        self->ScheduleControlPoll();
    });
}
void ApogeeDuetProtocol::FinishControlWrite(AudioModel::ParameterId id, Completion done) {
    if (!CommitControls()) { ReleaseControl(); done(kIOReturnError, false); return; }
    ReleaseControl();
    const auto found = controlSnapshot_->state.parameters.find(id);
    if (found == controlSnapshot_->state.parameters.end()) { done(kIOReturnError, false); return; }
    done(kIOReturnSuccess, found->second.value);
}
void ApogeeDuetProtocol::SetParameter(AudioModel::ParameterId id, const AudioModel::ParameterValue& value, Completion done) {
    if (!controlsReady_) { done(kIOReturnNotReady, false); return; }
    if (controlBusy_) {
        QueueControl([id, value, done](ApogeeDuetProtocol& self) { self.SetParameter(id, value, done); }, done);
        return;
    }
    auto self = std::static_pointer_cast<ApogeeDuetProtocol>(weak_from_this().lock());
    if (!self) { done(kIOReturnNotReady, false); return; }
    const auto write = BuildDuetParameterWrite(controls_, id, value);
    if (!write) { done(kIOReturnBadArgument, false); return; }
    controlBusy_ = true;
    const auto epoch = controlsEpoch_;
    if (write->group == DuetControlGroup::Action) {
        ClearDisplay([self, epoch, done](auto status) {
            if (epoch == self->controlsEpoch_) self->ReleaseControl();
            done(epoch == self->controlsEpoch_ ? status : kIOReturnAborted, AudioModel::ActionTrigger{});
        });
        return;
    }
    // The HW_STATE setter contains gain/volume as well as target: refresh it
    // immediately before changing target so a knob movement is not overwritten.
    auto send = [self, epoch, id, value, done](IOReturn status) {
        if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
        if (status != kIOReturnSuccess) { self->ReleaseControl(); done(status, false); return; }
        const auto plan = BuildDuetParameterWrite(self->controls_, id, value);
        if (!plan) { self->ReleaseControl(); done(kIOReturnBadArgument, false); return; }
        self->ExecuteVendorSequence(plan->commands, false, [self, epoch, id, done, group = plan->group](auto status, const auto&) {
            if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
            if (status != kIOReturnSuccess) {
                // A multi-command group can partially apply. Re-read it before
                // accepting any later write; never publish the intended image.
                self->ReadControlGroup(group, [self, epoch, done, status](auto readStatus) {
                    if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
                    if (readStatus == kIOReturnSuccess) (void)self->CommitControls();
                    self->ReleaseControl(); done(status, false);
                });
                return;
            }
            self->ReadControlGroup(group, [self, epoch, id, done](auto status) {
                if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
                if (status != kIOReturnSuccess) { self->ReleaseControl(); done(status, false); return; }
                self->FinishControlWrite(id, done);
            });
        });
    };
    if (write->group == DuetControlGroup::Knob) ReadControlGroup(DuetControlGroup::Knob, std::move(send));
    else send(kIOReturnSuccess);
}
void ApogeeDuetProtocol::SetRoute(AudioModel::NodeId router, AudioModel::PortId output,
    std::optional<AudioModel::PortId> input, Completion done) {
    if (!controlsReady_ || !input) { done(kIOReturnNotReady, false); return; }
    if (controlBusy_) {
        QueueControl([router, output, input, done](ApogeeDuetProtocol& self) {
            self.SetRoute(router, output, input, done);
        }, done);
        return;
    }
    if (router.value < DuetControl::InputRouter || router.value >= DuetControl::InputRouter + 2) {
        done(kIOReturnUnsupported, false); return;
    }
    const auto ch = router.value - DuetControl::InputRouter;
    const auto node = std::ranges::find(controlSnapshot_->topology.nodes, router, &AudioModel::Node::id);
    if (node == controlSnapshot_->topology.nodes.end()) { done(kIOReturnNotFound, false); return; }
    const auto& routes = std::get<AudioModel::RouterNode>(node->body);
    const auto source = std::ranges::find(routes.inputs, *input);
    if (routes.outputs != std::vector{output} || source == routes.inputs.end()) { done(kIOReturnBadArgument, false); return; }
    auto self = std::static_pointer_cast<ApogeeDuetProtocol>(weak_from_this().lock());
    if (!self) { done(kIOReturnNotReady, false); return; }
    const bool phone = source - routes.inputs.begin() == 1;
    controlBusy_ = true; const auto epoch = controlsEpoch_;
    // XLR level is set separately, before this source switch (generic composite).
    ExecuteVendorSequence({VendorCommand::IndexedBool(VendorCommand::Code::InputSourceIsPhone,
        static_cast<uint8_t>(ch), phone)}, false, [self, epoch, router, output, input, done](auto status, const auto&) {
        if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
        self->ReadControlGroup(DuetControlGroup::Input, [self, epoch, router, input, done, status](auto readStatus) {
            if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
            if (readStatus == kIOReturnSuccess && !self->CommitControls()) readStatus = kIOReturnError;
            self->ReleaseControl();
            if (status != kIOReturnSuccess || readStatus != kIOReturnSuccess) {
                done(status != kIOReturnSuccess ? status : readStatus, false); return;
            }
            const auto& active = self->controlSnapshot_->state.routers.at(router).activeBundles;
            const uint32_t selected = self->controls_.input.sources[router.value - DuetControl::InputRouter] == InputSource::Phone ? 2 : 1;
            if (active != std::vector{AudioModel::RouteBundleId{selected}}) { done(kIOReturnError, false); return; }
            const auto& actualRouter = std::get<AudioModel::RouterNode>(std::ranges::find(
                self->controlSnapshot_->topology.nodes, router, &AudioModel::Node::id)->body);
            done(kIOReturnSuccess, int64_t(actualRouter.inputs[selected - 1].value));
        });
    });
}

} // namespace ASFW::Audio::Oxford::Apogee
