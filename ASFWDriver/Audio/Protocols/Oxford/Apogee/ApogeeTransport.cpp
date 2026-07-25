// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeTransport.cpp - Duet FCP dispatch and meter register reads.

#include "ApogeeTransport.hpp"

#include "../../../../Common/CallbackUtils.hpp"
#include "../../../../Protocols/AVC/AVCDefs.hpp"
#include "../../../../Protocols/AVC/FCPTransport.hpp"

#include <algorithm>
#include <memory>

namespace ASFW::Audio::Oxford::Apogee {

namespace {

using Protocols::AVC::AVCResult;
using Protocols::AVC::CTypeToResult;
using Protocols::AVC::FCPFrame;
using Protocols::AVC::FCPStatus;

constexpr uint8_t kCTypeControl = 0x00;
constexpr uint8_t kCTypeStatus = 0x01;
constexpr uint8_t kSubunitUnit = 0xFF;
constexpr uint8_t kOpcodeVendorDependent = 0x00;

/// ctype + subunit + opcode precede the vendor operands in every AV/C frame.
constexpr size_t kAvcHeaderBytes = 3;

[[nodiscard]] IOReturn MapFCPStatusToIOReturn(FCPStatus status) noexcept {
    switch (status) {
        case FCPStatus::kOk:
            return kIOReturnSuccess;
        case FCPStatus::kTimeout:
            return kIOReturnTimeout;
        case FCPStatus::kBusReset:
            return kIOReturnNotResponding;
        case FCPStatus::kBusy:
            return kIOReturnBusy;
        case FCPStatus::kInvalidPayload:
            return kIOReturnBadArgument;
        default:
            return kIOReturnError;
    }
}

[[nodiscard]] IOReturn MapAVCResultToIOReturn(AVCResult result) noexcept {
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

} // namespace

//==============================================================================
// Transport A - AV/C vendor commands over FCP
//==============================================================================

namespace VendorFcp {

void Send(Protocols::AVC::FCPTransport* transport,
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

    // AV/C frames are quadlet-aligned, so the operand tail is zero-padded up.
    const size_t unpaddedLength = kAvcHeaderBytes + operands.size();
    const size_t paddedLength = (unpaddedLength + 3U) & ~3U;

    if (paddedLength < Protocols::AVC::kAVCFrameMinSize ||
        paddedLength > Protocols::AVC::kAVCFrameMaxSize) {
        Common::InvokeSharedCallback(callbackState, kIOReturnBadArgument, command);
        return;
    }

    FCPFrame frame{};
    frame.data[0] = isStatus ? kCTypeStatus : kCTypeControl;
    frame.data[1] = kSubunitUnit;
    frame.data[2] = kOpcodeVendorDependent;

    if (!operands.empty()) {
        std::copy(operands.begin(), operands.end(), frame.data.begin() + kAvcHeaderBytes);
    }

    if (paddedLength > unpaddedLength) {
        std::fill(frame.data.begin() + unpaddedLength, frame.data.begin() + paddedLength, 0);
    }

    frame.length = paddedLength;

    const auto handle = transport->SubmitCommand(
        frame,
        [callbackState, command, isStatus](FCPStatus status, const FCPFrame& response) {
            const IOReturn transportStatus = MapFCPStatusToIOReturn(status);
            if (transportStatus != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, transportStatus, command);
                return;
            }

            if (response.length < Protocols::AVC::kAVCFrameMinSize) {
                Common::InvokeSharedCallback(callbackState, kIOReturnBadMessageID, command);
                return;
            }

            const AVCResult avcResult = CTypeToResult(response.data[0]);
            const IOReturn avcStatus = MapAVCResultToIOReturn(avcResult);
            if (avcStatus != kIOReturnSuccess) {
                Common::InvokeSharedCallback(callbackState, avcStatus, command);
                return;
            }

            ApogeeVendorCommand parsed = command;
            if (isStatus) {
                const size_t operandLength = response.length - kAvcHeaderBytes;
                std::span<const uint8_t> payload{response.data.data() + kAvcHeaderBytes,
                                                 operandLength};
                if (!parsed.ParseStatusPayload(payload)) {
                    Common::InvokeSharedCallback(callbackState, kIOReturnBadMessageID, command);
                    return;
                }
            }

            Common::InvokeSharedCallback(callbackState, kIOReturnSuccess, parsed);
        });
    // The handle is deliberately dropped: these commands are never cancelled
    // individually, and a host transport may complete synchronously, in which
    // case the returned handle is already stale.
    (void)handle;
}

void ExecuteSequence(Protocols::AVC::FCPTransport* transport,
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
        Protocols::AVC::FCPTransport* transport{nullptr};
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
                    const Discovery::DeviceRouteToken& route,
                    Async::FWAddress address,
                    uint32_t blockBytes,
                    RouteValidator isRouteCurrent,
                    Decoder decode,
                    std::function<void(IOReturn, State)> callback) {
    auto callbackState = Common::ShareCallback(std::move(callback));

    if (!isRouteCurrent || !isRouteCurrent()) {
        Common::InvokeSharedCallback(callbackState, kIOReturnNotReady, State{});
        return;
    }

    busOps.ReadBlock(
        route.generation,
        FW::NodeId{static_cast<uint8_t>(route.nodeId)},
        address,
        blockBytes,
        FW::FwSpeed::S100,
        [callbackState, isRouteCurrent, decode](Async::AsyncStatus status,
                                                std::span<const uint8_t> payload) {
            State state{};
            if (!isRouteCurrent() || status != Async::AsyncStatus::kSuccess ||
                !decode(payload, state)) {
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
               const Discovery::DeviceRouteToken& route,
               RouteValidator isRouteCurrent,
               std::function<void(IOReturn, InputMeterState)> callback) {
    ReadMeterBlock<InputMeterState>(busOps, route, InputAddress(), kInputBlockBytes,
                                    std::move(isRouteCurrent), &DecodeInput, std::move(callback));
}

void ReadMixer(Async::IFireWireBusOps& busOps,
               const Discovery::DeviceRouteToken& route,
               RouteValidator isRouteCurrent,
               std::function<void(IOReturn, MixerMeterState)> callback) {
    ReadMeterBlock<MixerMeterState>(busOps, route, MixerAddress(), kMixerBlockBytes,
                                    std::move(isRouteCurrent), &DecodeMixer, std::move(callback));
}

} // namespace MeterRegisters

} // namespace ASFW::Audio::Oxford::Apogee
