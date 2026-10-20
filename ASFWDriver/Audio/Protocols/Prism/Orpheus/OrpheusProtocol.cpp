// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "OrpheusProtocol.hpp"
#include "../../../Controls/ControlNotifications.hpp"
#include "../../../Model/Topology/Validate.hpp"
#include "../../../../Protocols/AVC/Core/AvcFrame.hpp"
#include "../../../../Protocols/AVC/Core/IAvcUnit.hpp"
#include "../../../../Protocols/AVC/Core/AvcError.hpp"
#include "../../../../Common/CallbackUtils.hpp"

#include <algorithm>
#include <bit>
#include <ranges>

namespace ASFW::Audio::Prism::Orpheus {

OrpheusProtocol::OrpheusProtocol(Protocols::Ports::FireWireBusOps& busOps,
                                 Protocols::Ports::FireWireBusInfo& busInfo,
                                 Discovery::DeviceRouteToken route,
                                 IRM::IRMClient* irmClient,
                                 CMP::CMPClient* cmpClient,
                                 Scheduling::ITimerScheduler* timerScheduler) noexcept
    : GenericAvcProtocol(busOps, busInfo, route, irmClient, cmpClient, timerScheduler, nullptr, "Prism Sound Orpheus") {
}

OrpheusProtocol::~OrpheusProtocol() {
    controlsReady_ = false;
    ++controlsEpoch_;
    ResetControlQueue();
}

IOReturn OrpheusProtocol::Shutdown() {
    controlsReady_ = false;
    ++controlsEpoch_;
    ResetControlQueue();
    return GenericAvcProtocol::Shutdown();
}

void OrpheusProtocol::UpdateRuntimeContext(const Discovery::DeviceRouteToken& route,
                                          std::shared_ptr<ASFW::AVC::IAvcUnit> avcUnit) {
    if (route_ != route || avcUnit_ != avcUnit) {
        controlsReady_ = false;
        ++controlsEpoch_;
        ResetControlQueue();
    }
    GenericAvcProtocol::UpdateRuntimeContext(route, std::move(avcUnit));
}

std::optional<ASFW::Audio::Controls::ControlSnapshot> OrpheusProtocol::CopySnapshot() const {
    return controlsReady_ ? controlSnapshot_ : std::nullopt;
}

void OrpheusProtocol::SendOrpheusFrame(std::span<const uint8_t> frameBytes,
                                       std::function<void(IOReturn, std::vector<uint8_t>)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    if (!avcUnit_) {
        Common::InvokeSharedCallback(callbackState, kIOReturnNotReady, std::vector<uint8_t>{});
        return;
    }
    if (frameBytes.size() < 3) {
        Common::InvokeSharedCallback(callbackState, kIOReturnBadArgument, std::vector<uint8_t>{});
        return;
    }

    auto cmdFrame = AVC::CommandFrame::Make(
        static_cast<AVC::CommandType>(frameBytes[0]),
        AVC::SubunitAddress::FromByte(frameBytes[1]),
        static_cast<AVC::Opcode>(frameBytes[2]),
        std::span<const uint8_t>(frameBytes.data() + 3, frameBytes.size() - 3));

    if (!cmdFrame) {
        Common::InvokeSharedCallback(callbackState, kIOReturnBadArgument, std::vector<uint8_t>{});
        return;
    }

    const auto ctype = static_cast<AVC::CommandType>(frameBytes[0]);
    avcUnit_->Submit(*cmdFrame, avcUnit_->CurrentGeneration(), [callbackState, ctype](AVC::Expected<AVC::Response> res) {
        if (!res) {
            Common::InvokeSharedCallback(callbackState, AVC::ToIOReturn(res.error()), std::vector<uint8_t>{});
            return;
        }
        if (!AVC::IsResponseCodeAccepted(res->code, ctype)) {
            Common::InvokeSharedCallback(callbackState, kIOReturnError, std::vector<uint8_t>{});
            return;
        }
        std::vector<uint8_t> fullResponse;
        fullResponse.reserve(3 + res->operands.size());
        fullResponse.push_back(static_cast<uint8_t>(res->code));
        fullResponse.push_back(res->address.Byte());
        fullResponse.push_back(static_cast<uint8_t>(res->opcode));
        fullResponse.insert(fullResponse.end(), res->operands.begin(), res->operands.end());
        Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, std::move(fullResponse));
    });
}

void OrpheusProtocol::ExecuteOrpheusSequence(const std::vector<std::vector<uint8_t>>& frames,
                                             std::function<void(IOReturn, std::vector<std::vector<uint8_t>>)> callback) {
    if (frames.empty()) {
        callback(kIOReturnSuccess, {});
        return;
    }
    struct SequenceState {
        size_t index{0};
        std::vector<std::vector<uint8_t>> responses;
        std::function<void(IOReturn, std::vector<std::vector<uint8_t>>)> done;
    };
    auto state = std::make_shared<SequenceState>();
    state->responses.reserve(frames.size());
    state->done = std::move(callback);

    auto self = std::static_pointer_cast<OrpheusProtocol>(weak_from_this().lock());
    auto step = std::make_shared<std::function<void()>>();
    *step = [self, frames, state, step]() {
        if (!self) {
            state->done(kIOReturnAborted, {});
            return;
        }
        if (state->index >= frames.size()) {
            state->done(kIOReturnSuccess, std::move(state->responses));
            return;
        }
        self->SendOrpheusFrame(frames[state->index], [self, frames, state, step](IOReturn status, std::vector<uint8_t> response) {
            if (status != kIOReturnSuccess) {
                state->done(status, std::move(state->responses));
                return;
            }
            state->responses.push_back(std::move(response));
            state->index++;
            (*step)();
        });
    };
    (*step)();
}

void OrpheusProtocol::ReadControlGroup(OrpheusControlGroup group, uint8_t index, VoidCallback done) {
    auto self = std::static_pointer_cast<OrpheusProtocol>(weak_from_this().lock());
    if (!self || !avcUnit_) { done(kIOReturnNotReady); return; }
    const auto epoch = controlsEpoch_;
    auto finish = [self, epoch, done = std::move(done)](IOReturn status, auto apply) {
        if (epoch != self->controlsEpoch_) { done(kIOReturnAborted); return; }
        if (status == kIOReturnSuccess) apply();
        done(status);
    };

    switch (group) {
        case OrpheusControlGroup::Input: {
            if (index >= 8) { finish(kIOReturnBadArgument, []{}); return; }
            const auto cmd = OrpheusVendorCodec::BuildAnalogChannelStatus(index);
            if (!cmd) { finish(kIOReturnError, []{}); return; }
            SendOrpheusFrame(*cmd, [self, finish, index](IOReturn status, std::vector<uint8_t> resp) {
                if (status != kIOReturnSuccess) { finish(status, []{}); return; }
                const auto parsed = OrpheusVendorCodec::ParseAnalogChannelState(resp, index);
                if (!parsed) { finish(kIOReturnError, []{}); return; }
                finish(kIOReturnSuccess, [self, parsed, index] {
                    self->controls_.analog[index] = *parsed;
                });
            });
            break;
        }
        case OrpheusControlGroup::Output:
        case OrpheusControlGroup::Settings: {
            const auto cmd = OrpheusVendorCodec::BuildUnitStateStatus();
            SendOrpheusFrame(cmd, [self, finish](IOReturn status, std::vector<uint8_t> resp) {
                if (status != kIOReturnSuccess) { finish(status, []{}); return; }
                const auto parsed = OrpheusVendorCodec::ParseUnitState(resp);
                if (!parsed) { finish(kIOReturnError, []{}); return; }
                finish(kIOReturnSuccess, [self, parsed] {
                    self->controls_.unit = *parsed;
                });
            });
            break;
        }
        case OrpheusControlGroup::Mixer: {
            if (index >= 6) { finish(kIOReturnBadArgument, []{}); return; }
            const auto cmd = OrpheusVendorCodec::BuildMixBusStatus(index);
            if (!cmd) { finish(kIOReturnError, []{}); return; }
            SendOrpheusFrame(*cmd, [self, finish, index](IOReturn status, std::vector<uint8_t> resp) {
                if (status != kIOReturnSuccess) { finish(status, []{}); return; }
                const auto parsed = OrpheusVendorCodec::ParseMixBusState(resp, index);
                if (!parsed) { finish(kIOReturnError, []{}); return; }
                finish(kIOReturnSuccess, [self, parsed, index] {
                    self->controls_.mixer[index] = *parsed;
                });
            });
            break;
        }
        case OrpheusControlGroup::ClockSource: {
            const bool hasAdat = self->controls_.unit.HasAdatInput();
            const auto cmd = OrpheusVendorCodec::BuildSignalSourceStatus(hasAdat);
            SendOrpheusFrame(cmd, [self, finish, hasAdat](IOReturn status, std::vector<uint8_t> resp) {
                if (status != kIOReturnSuccess) { finish(status, []{}); return; }
                const auto parsed = OrpheusVendorCodec::ParseSignalSourceStatus(resp, hasAdat);
                if (!parsed) { finish(kIOReturnError, []{}); return; }
                finish(kIOReturnSuccess, [self, parsed] {
                    self->controls_.syncSource = *parsed;
                });
            });
            break;
        }
    }
}

void OrpheusProtocol::Prepare(std::function<void(IOReturn)> done) {
    if (controlsReady_) { done(kIOReturnSuccess); return; }
    if (controlBusy_ || !avcUnit_) { done(kIOReturnNotReady); return; }
    auto self = std::static_pointer_cast<OrpheusProtocol>(weak_from_this().lock());
    if (!self) { done(kIOReturnNotReady); return; }

    controlBusy_ = true;
    const auto epoch = controlsEpoch_;

    auto finish = [self, epoch, done = std::move(done)](IOReturn status) {
        if (epoch != self->controlsEpoch_) { done(kIOReturnAborted); return; }
        if (status == kIOReturnSuccess) {
            self->controlsReady_ = true;
            if (!self->CommitControls()) {
                self->controlsReady_ = false;
                status = kIOReturnError;
            }
        }
        self->ReleaseControl();
        done(status);
    };

    // 1. Read UnitState
    self->ReadControlGroup(OrpheusControlGroup::Output, 0, [self, finish](IOReturn status) {
        if (status != kIOReturnSuccess) { finish(status); return; }

        // 2. Read 8 Analog Channels sequentially
        auto readAnalog = std::make_shared<std::function<void(uint8_t)>>();
        *readAnalog = [self, finish, readAnalog](uint8_t ch) {
            if (ch >= 8) {
                // 3. Read 6 Mix Buses sequentially
                auto readMix = std::make_shared<std::function<void(uint8_t)>>();
                *readMix = [self, finish, readMix](uint8_t bus) {
                    if (bus >= 6) {
                        // 4. Read SignalSource
                        self->ReadControlGroup(OrpheusControlGroup::ClockSource, 0, finish);
                        return;
                    }
                    self->ReadControlGroup(OrpheusControlGroup::Mixer, bus, [bus, finish, readMix](IOReturn status) {
                        if (status != kIOReturnSuccess) { finish(status); return; }
                        (*readMix)(static_cast<uint8_t>(bus + 1));
                    });
                };
                (*readMix)(0);
                return;
            }
            self->ReadControlGroup(OrpheusControlGroup::Input, ch, [ch, finish, readAnalog](IOReturn status) {
                if (status != kIOReturnSuccess) { finish(status); return; }
                (*readAnalog)(static_cast<uint8_t>(ch + 1));
            });
        };
        (*readAnalog)(0);
    });
}

void OrpheusProtocol::SetParameter(AudioModel::ParameterId id,
                                   const AudioModel::ParameterValue& value,
                                   Completion done) {
    if (!controlsReady_) { done(kIOReturnNotReady, false); return; }
    if (controlBusy_) {
        QueueControl([id, value, done](OrpheusProtocol& self) { self.SetParameter(id, value, done); }, done);
        return;
    }
    auto self = std::static_pointer_cast<OrpheusProtocol>(weak_from_this().lock());
    if (!self) { done(kIOReturnNotReady, false); return; }

    const auto write = BuildOrpheusParameterWrite(controls_, id, value);
    if (!write) { done(kIOReturnBadArgument, false); return; }

    controlBusy_ = true;
    const auto epoch = controlsEpoch_;

    ExecuteOrpheusSequence(write->frames, [self, epoch, id, write, done](IOReturn status, const auto&) {
        if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
        if (status != kIOReturnSuccess) {
            self->ReadControlGroup(write->group, write->index, [self, epoch, done, status](auto readStatus) {
                if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
                if (readStatus == kIOReturnSuccess) (void)self->CommitControls();
                self->ReleaseControl();
                done(status, false);
            });
            return;
        }

        self->ReadControlGroup(write->group, write->index, [self, epoch, id, done](auto status) {
            if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
            if (status != kIOReturnSuccess) {
                self->ReleaseControl();
                done(status, false);
                return;
            }
            self->FinishControlWrite(id, done);
        });
    });
}

void OrpheusProtocol::FinishControlWrite(AudioModel::ParameterId id, Completion done) {
    if (!CommitControls()) {
        ReleaseControl();
        done(kIOReturnError, false);
        return;
    }
    ReleaseControl();
    const auto found = controlSnapshot_->state.parameters.find(id);
    if (found == controlSnapshot_->state.parameters.end()) {
        done(kIOReturnError, false);
        return;
    }
    done(kIOReturnSuccess, found->second.value);
}

void OrpheusProtocol::SetRoute(AudioModel::NodeId router,
                               AudioModel::PortId output,
                               std::optional<AudioModel::PortId> input,
                               Completion done) {
    if (!controlsReady_ || !input) { done(kIOReturnNotReady, false); return; }
    if (controlBusy_) {
        QueueControl([router, output, input, done](OrpheusProtocol& self) {
            self.SetRoute(router, output, input, done);
        }, done);
        return;
    }
    if (router.value < OrpheusControl::InputRouter || router.value >= OrpheusControl::InputRouter + 4) {
        done(kIOReturnUnsupported, false);
        return;
    }
    const auto ch = router.value - OrpheusControl::InputRouter;
    const auto node = std::ranges::find(controlSnapshot_->topology.nodes, router, &AudioModel::Node::id);
    if (node == controlSnapshot_->topology.nodes.end()) { done(kIOReturnNotFound, false); return; }
    const auto& routes = std::get<AudioModel::RouterNode>(node->body);
    const auto source = std::ranges::find(routes.inputs, *input);
    if (routes.outputs != std::vector{output} || source == routes.inputs.end()) {
        done(kIOReturnBadArgument, false);
        return;
    }
    auto self = std::static_pointer_cast<OrpheusProtocol>(weak_from_this().lock());
    if (!self) { done(kIOReturnNotReady, false); return; }

    controlBusy_ = true;
    const auto epoch = controlsEpoch_;
    const size_t inIndex = static_cast<size_t>(source - routes.inputs.begin()); // 0 = inA, 1 = inB

    uint8_t inputTypeVal = 0;
    if (ch < 2) {
        // Ch 1-2: inA = XLR, inB = Jack (Instrument = 2)
        if (inIndex == 1) {
            inputTypeVal = 2; // Instrument
        } else {
            inputTypeVal = (self->controls_.analog[ch].lineInputLevel != 0) ? 1 : 0;
        }
    } else {
        // Ch 3-4: inA = XLR, inB = Line (Line TRS = 1)
        inputTypeVal = (inIndex == 1) ? 1 : 0;
    }

    const auto frame = OrpheusVendorCodec::BuildAnalogControl(OrpheusCommand::InputType, static_cast<uint8_t>(ch), inputTypeVal);
    if (!frame) {
        ReleaseControl();
        done(kIOReturnError, false);
        return;
    }

    SendOrpheusFrame(*frame, [self, epoch, ch, router, done](IOReturn status, const auto&) {
        if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
        self->ReadControlGroup(OrpheusControlGroup::Input, static_cast<uint8_t>(ch), [self, epoch, router, done, status](auto readStatus) {
            if (epoch != self->controlsEpoch_) { done(kIOReturnAborted, false); return; }
            if (readStatus == kIOReturnSuccess && !self->CommitControls()) {
                readStatus = kIOReturnError;
            }
            self->ReleaseControl();
            if (status != kIOReturnSuccess || readStatus != kIOReturnSuccess) {
                done(status != kIOReturnSuccess ? status : readStatus, false);
                return;
            }
            const auto& active = self->controlSnapshot_->state.routers.at(router).activeBundles;
            const uint32_t selected = active.empty() ? 1u : active.front().value;
            const auto& actualRouter = std::get<AudioModel::RouterNode>(std::ranges::find(
                self->controlSnapshot_->topology.nodes, router, &AudioModel::Node::id)->body);
            done(kIOReturnSuccess, int64_t(actualRouter.inputs[selected - 1].value));
        });
    });
}

bool OrpheusProtocol::CommitControls() {
    namespace AM = ASFW::AudioModel;
    namespace C = ASFW::Audio::Controls;
    auto next = BuildOrpheusControls(controls_, route_.routeEpoch,
        controlSnapshot_ ? controlSnapshot_->sequence : 0);
    if (!AM::validate(next.topology) || !AM::validateState(next.topology, next.state)) return false;
    if (!controlSnapshot_ || controlSnapshot_->topology.revision != next.topology.revision) {
        const bool rebound = controlSnapshot_.has_value();
        if (rebound) next.sequence = C::ControlNotifications::Shared().NextSequence();
        controlSnapshot_ = std::move(next);
        if (rebound) C::ControlNotifications::Shared().Publish({route_.guid,
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
        C::ParameterChanged event{route_.guid, next.topology.revision, 0, id.value, 0, 0};
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, bool>) { event.kind = 0; event.bits = value; }
            else if constexpr (std::is_same_v<T, double>) { event.kind = 1; event.bits = std::bit_cast<uint64_t>(value); }
            else if constexpr (std::is_same_v<T, int64_t>) { event.kind = 2; event.bits = static_cast<uint64_t>(value); }
        }, state.value);
        events.push_back(event);
    }
    if (metadata) events = {{route_.guid, next.topology.revision, 0, 0, 4, 0}};
    for (auto& event : events) event.sequence = next.sequence = C::ControlNotifications::Shared().NextSequence();
    controlSnapshot_ = std::move(next); // exact values/watermark before fan-out
    for (const auto& event : events) C::ControlNotifications::Shared().Publish(event);
    return true;
}

void OrpheusProtocol::QueueControl(std::function<void(OrpheusProtocol&)> op, Completion refuse) {
    if (controlWaiters_.size() >= kMaxControlWaiters) { refuse(kIOReturnBusy, false); return; }
    controlWaiters_.push_back([weak = weak_from_this(), op = std::move(op), refuse = std::move(refuse)] {
        const auto owner = weak.lock();
        if (!owner) { refuse(kIOReturnAborted, false); return; }
        op(*static_cast<OrpheusProtocol*>(owner.get()));
    });
}

void OrpheusProtocol::ReleaseControl() {
    controlBusy_ = false;
    while (!controlBusy_ && !controlWaiters_.empty()) {
        auto next = std::move(controlWaiters_.front());
        controlWaiters_.pop_front();
        next();
    }
}

void OrpheusProtocol::ResetControlQueue() {
    controlBusy_ = false;
    auto waiters = std::move(controlWaiters_);
    controlWaiters_.clear();
    for (auto& waiter : waiters) waiter();
}

} // namespace ASFW::Audio::Prism::Orpheus
