// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeTransport.cpp - Duet FCP dispatch and meter register reads.

#include "ApogeeTransport.hpp"

#include "../../../../Common/CallbackUtils.hpp"
#include "../../../../Logging/Logging.hpp"
#include "../../../../Protocols/AVC/Commands/GeneralCommands.hpp"
#include "../../../../Protocols/AVC/Core/AvcError.hpp"
#include "../../../../Protocols/AVC/Core/IAvcUnit.hpp"

#include <algorithm>
#include <memory>

namespace ASFW::Audio::Oxford::Apogee {

//==============================================================================
// Transport A - AV/C vendor commands over FCP
//==============================================================================

namespace VendorFcp {

void Send(AVC::IAvcUnit* transport,
          const ApogeeVendorCommand& command,
          bool isStatus,
          ResultCallback callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));
    if (!transport) {
        Common::InvokeSharedCallback(callbackState, kIOReturnNotReady, command);
        return;
    }

    std::vector<uint8_t> operands = command.BuildOperandBase();
    if (!isStatus) {
        command.AppendControlValue(operands);
    }

    if (operands.size() < 3) {
        Common::InvokeSharedCallback(callbackState, kIOReturnBadArgument, command);
        return;
    }

    AVC::CompanyId oui{operands[0], operands[1], operands[2]};
    std::span<const uint8_t> payload{operands.data() + 3, operands.size() - 3};
    AVC::Cmd::RawVendorDependentCommand cmd{
        .operands = AVC::Cmd::RawVendorDependentOperands(oui, payload)};

    ASFW_LOG_INFO(Oxfw, "vendor %{public}s code=0x%02x",
                  isStatus ? "STATUS" : "CONTROL",
                  static_cast<unsigned>(command.code));

    auto completion = [callbackState, command, isStatus](
        AVC::Expected<AVC::Cmd::RawVendorDependentReply> res) {
        if (!res) {
            ASFW_LOG_ERROR(Oxfw, "vendor code=0x%02x: AV/C command failed error=%u",
                           static_cast<unsigned>(command.code), static_cast<unsigned>(res.error().kind));
            Common::InvokeSharedCallback(callbackState, AVC::ToIOReturn(res.error()), command);
            return;
        }

        ApogeeVendorCommand parsed = command;
        if (isStatus) {
            if (!parsed.ParseStatusPayload(res->companyId, res->payload)) {
                ASFW_LOG_ERROR(Oxfw, "vendor code=0x%02x: status parse failed",
                               static_cast<unsigned>(command.code));
                Common::InvokeSharedCallback(
                    callbackState,
                    AVC::ToIOReturn(AVC::AvcError::Of(AVC::AvcErrorKind::kMalformedOperands)),
                    command);
                return;
            }
        }

        Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, parsed);
    };

    if (isStatus) {
        transport->Status(cmd, std::move(completion));
    } else {
        transport->Control(cmd, std::move(completion));
    }
}

void ExecuteSequence(AVC::IAvcUnit* transport,
                     const std::vector<ApogeeVendorCommand>& commands,
                     bool isStatus,
                     SequenceCallback callback) {
    if (commands.empty()) {
        callback(kIOReturnSuccess, {});
        return;
    }

    struct SequenceState {
        std::vector<ApogeeVendorCommand> commands;
        std::vector<ApogeeVendorCommand> responses;
        size_t index{0};
        bool isStatus{false};
        AVC::IAvcUnit* transport{nullptr};
        SequenceCallback completion;
    };

    auto state = std::make_shared<SequenceState>();
    state->commands = commands;
    state->responses.reserve(commands.size());
    state->isStatus = isStatus;
    state->transport = transport;
    state->completion = std::move(callback);

    auto step = std::make_shared<std::function<void()>>();
    *step = [state, step]() {
        if (state->index >= state->commands.size()) {
            state->completion(kIOReturnSuccess, state->responses);
            return;
        }

        const ApogeeVendorCommand command = state->commands[state->index];
        Send(state->transport, command, state->isStatus,
             [state, step](IOReturn status, const ApogeeVendorCommand& response) {
                 if (status != kIOReturnSuccess) {
                     // Abort the rest: a half-applied params group is worse
                     // than a failed one, and the caller retries the whole set.
                     // Send hands the originating command back on every failure
                     // path, so `response` names the step that aborted.
                     ASFW_LOG_ERROR(Oxfw,
                                    "vendor sequence aborted at %zu/%zu code=0x%02x status=0x%08x",
                                    state->index, state->commands.size(),
                                    static_cast<unsigned>(response.code),
                                    static_cast<unsigned>(status));
                     state->completion(status, {});
                     return;
                 }
                 state->responses.push_back(response);
                 ++state->index;
                 (*step)();
             });
    };

    (*step)();
}

} // namespace VendorFcp

//==============================================================================
// Transport B - plain async block reads to Duet meter registers
//==============================================================================

namespace MeterRegisters {

namespace {

/// Shared shape of both meter reads: refuse a dead route before touching the
/// bus, then re-check on completion because a reset in between means the
/// payload may have come from whatever now occupies that node.
template <typename State, typename Decoder>
void ReadMeterBlock(Async::IFireWireBusOps& busOps,
                    Async::FWAddress address,
                    uint32_t blockBytes,
                    const char* meterName,
                    RouteProvider currentRoute,
                    Decoder decode,
                    std::function<void(IOReturn, State)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));

    // A provider the caller never wired is a programming error; a device with
    // no live route is ordinary bus state. Separate branches, separate messages.
    if (!currentRoute) {
        ASFW_LOG_ERROR(Oxfw, "meter %{public}s: no route provider wired", meterName);
        Common::InvokeSharedCallback(callbackState, kIOReturnNotReady, State{});
        return;
    }

    const auto issued = currentRoute();
    if (!issued.has_value()) {
        ASFW_LOG_ERROR(Oxfw, "meter %{public}s: device has no live route", meterName);
        Common::InvokeSharedCallback(callbackState, kIOReturnNotReady, State{});
        return;
    }

    busOps.ReadBlock(
        issued->generation,
        FW::NodeId{static_cast<uint8_t>(issued->nodeId)},
        address,
        blockBytes,
        FW::FwSpeed::S100,
        [callbackState, currentRoute = std::move(currentRoute), decode, meterName,
         issued = *issued](Async::AsyncStatus status, std::span<const uint8_t> payload) {
            // One kIOReturnError covers a retired route, a bus error, and a
            // malformed payload. Name which, or a meter that stops updating is
            // indistinguishable from a device that never answered.
            State state{};
            const auto now = currentRoute();
            if (!now.has_value() || *now != issued) {
                ASFW_LOG_ERROR(Oxfw,
                               "meter %{public}s: route retired during read "
                               "(issued epoch=%llu, now epoch=%llu)",
                               meterName,
                               static_cast<unsigned long long>(issued.routeEpoch),
                               static_cast<unsigned long long>(
                                   now.has_value() ? now->routeEpoch : 0ULL));
                Common::InvokeSharedCallback(callbackState, kIOReturnError, State{});
                return;
            }
            if (status != Async::AsyncStatus::kSuccess) {
                ASFW_LOG_ERROR(Oxfw, "meter %{public}s: read failed status=%d", meterName,
                               static_cast<int>(status));
                Common::InvokeSharedCallback(callbackState, kIOReturnError, State{});
                return;
            }
            if (!decode(payload, state)) {
                ASFW_LOG_ERROR(Oxfw, "meter %{public}s: decode rejected %zu bytes", meterName,
                               payload.size());
                Common::InvokeSharedCallback(callbackState, kIOReturnError, State{});
                return;
            }
            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, state);
        });
}

} // namespace

bool DecodeInput(std::span<const uint8_t> payload, InputMeterState& out) noexcept {
    if (payload.size() < kInputBlockBytes) {
        return false;
    }
    out.levels[0] = DecodeLevel(payload, 0);
    out.levels[1] = DecodeLevel(payload, 4);
    return true;
}

bool DecodeMixer(std::span<const uint8_t> payload, MixerMeterState& out) noexcept {
    if (payload.size() < kMixerBlockBytes) {
        return false;
    }
    out.streamInputs[0] = DecodeLevel(payload, 0);
    out.streamInputs[1] = DecodeLevel(payload, 4);
    out.mixerOutputs[0] = DecodeLevel(payload, 8);
    out.mixerOutputs[1] = DecodeLevel(payload, 12);
    return true;
}

void ReadInput(Async::IFireWireBusOps& busOps,
               RouteProvider currentRoute,
               std::function<void(IOReturn, InputMeterState)> callback) {
    ReadMeterBlock<InputMeterState>(busOps, InputAddress(), kInputBlockBytes, "input",
                                    std::move(currentRoute), &DecodeInput, std::move(callback));
}

void ReadMixer(Async::IFireWireBusOps& busOps,
               RouteProvider currentRoute,
               std::function<void(IOReturn, MixerMeterState)> callback) {
    ReadMeterBlock<MixerMeterState>(busOps, MixerAddress(), kMixerBlockBytes, "mixer",
                                    std::move(currentRoute), &DecodeMixer, std::move(callback));
}

} // namespace MeterRegisters

} // namespace ASFW::Audio::Oxford::Apogee
