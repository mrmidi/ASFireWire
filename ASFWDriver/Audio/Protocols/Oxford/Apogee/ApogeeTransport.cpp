// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeTransport.cpp - Duet FCP dispatch and meter register reads.

#include "ApogeeTransport.hpp"

#include "../../../../Common/CallbackUtils.hpp"
#include "../../../../Logging/Logging.hpp"
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

/// Renders the framed operand bytes for a log line. The framing is the whole
/// point of the OXFW-common template (FW-126), and a one-byte error in the OUI
/// or magic prefix is invisible to host tests - they check our framing against
/// our own expectations, not against the device. This is what makes it visible.
void FormatFrameBytes(const FCPFrame& frame, char* out, size_t outSize) noexcept {
    size_t written = 0;
    for (size_t index = 0; index < frame.length && written + 3 < outSize; ++index) {
        static constexpr char kHex[] = "0123456789abcdef";
        out[written++] = kHex[(frame.data[index] >> 4U) & 0x0FU];
        out[written++] = kHex[frame.data[index] & 0x0FU];
        out[written++] = ' ';
    }
    out[written > 0 ? written - 1 : 0] = '\0';
}

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
        ASFW_LOG_ERROR(Oxfw, "vendor code=0x%02x: framed length %zu out of AV/C bounds",
                       static_cast<unsigned>(command.code), paddedLength);
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

    {
        char bytes[(3 * 24) + 1];
        FormatFrameBytes(frame, bytes, sizeof(bytes));
        ASFW_LOG_INFO(Oxfw, "vendor %{public}s code=0x%02x tx=[%{public}s]",
                      isStatus ? "STATUS" : "CONTROL",
                      static_cast<unsigned>(command.code), bytes);
    }

    const auto handle = transport->SubmitCommand(
        frame,
        [callbackState, command, isStatus](FCPStatus status, const FCPFrame& response) {
            const IOReturn transportStatus = MapFCPStatusToIOReturn(status);
            if (transportStatus != kIOReturnSuccess) {
                ASFW_LOG_ERROR(Oxfw, "vendor code=0x%02x: FCP transport failed status=%d",
                               static_cast<unsigned>(command.code), static_cast<int>(status));
                Common::InvokeSharedCallback(callbackState, transportStatus, command);
                return;
            }

            if (response.length < Protocols::AVC::kAVCFrameMinSize) {
                ASFW_LOG_ERROR(Oxfw, "vendor code=0x%02x: runt response (%zu bytes)",
                               static_cast<unsigned>(command.code),
                               static_cast<size_t>(response.length));
                Common::InvokeSharedCallback(callbackState, kIOReturnBadMessageID, command);
                return;
            }

            const AVCResult avcResult = CTypeToResult(response.data[0]);
            const IOReturn avcStatus = MapAVCResultToIOReturn(avcResult);
            if (avcStatus != kIOReturnSuccess) {
                // REJECTED vs NOT IMPLEMENTED is the difference between a wrong
                // operand and an unsupported code - do not collapse them.
                ASFW_LOG_ERROR(Oxfw, "vendor code=0x%02x: AV/C refused ctype=0x%02x result=%d",
                               static_cast<unsigned>(command.code),
                               static_cast<unsigned>(response.data[0]),
                               static_cast<int>(avcResult));
                Common::InvokeSharedCallback(callbackState, avcStatus, command);
                return;
            }

            ApogeeVendorCommand parsed = command;
            if (isStatus) {
                const size_t operandLength = response.length - kAvcHeaderBytes;
                std::span<const uint8_t> payload{response.data.data() + kAvcHeaderBytes,
                                                 operandLength};
                if (!parsed.ParseStatusPayload(payload)) {
                    char bytes[(3 * 24) + 1];
                    FormatFrameBytes(response, bytes, sizeof(bytes));
                    ASFW_LOG_ERROR(Oxfw, "vendor code=0x%02x: status parse failed rx=[%{public}s]",
                                   static_cast<unsigned>(command.code), bytes);
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
