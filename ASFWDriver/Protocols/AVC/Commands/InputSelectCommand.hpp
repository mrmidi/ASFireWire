// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// InputSelectCommand.hpp - INPUT SELECT (0x1B) operand codec.
//
// Source: TA 2002010 (CCM 1.1) §7.2; layouts Figures 7.17 (control command), 7.20 (control
// response), 7.23 (status command), 7.24 (status response); fields Tables 7.14-7.23;
// decision flow Figure 7.21; plug state model Annex A.1.
//
// INPUT SELECT tells a destination device to select, connect to, or disconnect from a
// unit output plug of a source node. It changes the target's bus connections.
// This codec has no caller: no supported device is known to implement it, and an
// unproven frame on the wire has frozen firmware before. Do not send it without a capture.
//
// Named requests:
//   QueryInputPlug(inputPlug)          STATUS   §7.2.4
//   SelectInput(...), ConnectInput(...), ChangeInputPath(...), DisconnectInput(...)
//                                      CONTROL  §7.2.1, one per subfunction (Table 7.14)

#pragma once

#include "CcmTypes.hpp"

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace ASFW::AVC::Cmd {

/// subfunction (Table 7.14).
enum class InputSelectSubfunction : uint8_t {
    kConnect = 0x00,     ///< Connect the destination's input to the source's output plug now.
    kPathChange = 0x01,  ///< As CONNECT, but the target may reject it if it did not select that source.
    kSelect = 0x02,      ///< Select the source; the isochronous connection is made when the target needs it.
    kDisconnect = 0x03,  ///< Disconnect the specified peripheral connection. Needs an input plug.
};

/// result_status of an INPUT SELECT control response (Table 7.18).
enum class InputSelectResult : uint8_t {
    kNoError = 0x0,               ///< ACCEPTED.
    kDisabled = 0x1,              ///< REJECTED: the target's own setting prohibits external selection.
    kLocked = 0x2,                ///< REJECTED: the target's state prohibits changing source (e.g. recording).
    kPointToPointNotOwner = 0x3,  ///< REJECTED: every iPCR is held by another node's point-to-point connection.
    kInsufficientResource = 0x4,  ///< REJECTED: bandwidth or channel could not be allocated.
    kSourceNotFound = 0x5,        ///< REJECTED: the node or plug does not exist.
    kNotSelected = 0x6,           ///< REJECTED: PATH CHANGE for a source the target did not select.
    kNotRegistered = 0x7,         ///< REJECTED: external plug source not registered in the target.
    kAnyOtherReason = 0xE,        ///< REJECTED: e.g. the input format does not match.
    kNoInformation = 0xF,         ///< INTERIM (the controller's request value).
};

/// status of an INPUT SELECT status response (Table 7.21).
enum class InputPlugStatus : uint8_t {
    kActive = 0x0,                ///< A connection exists with the source in node_ID.
    kReady = 0x1,                 ///< The source is selected but not connected.
    kNoSelection = 0x2,           ///< No source is selected.
    kCannotInput = 0x3,           ///< Selected, connection failed, target still trying.
    kInsufficientResource = 0x4,  ///< Selected, bus resources short, target still trying.
};

/// An INPUT SELECT response, operands as sent. The second field means different things
/// for the two command types, so read it with AsControl() or AsStatus().
struct InputSelect {
    uint8_t first{0};   ///< operand[0]: the subfunction (control) or the FF fill (status).
    uint8_t second{0};  ///< operand[1]: reserved | result_status (control), status | F (status).
    BusNodeId node{BusNodeId::Unspecified()};
    UnitPlugId outputPlug{UnitPlugId::Invalid()};
    UnitPlugId inputPlug{UnitPlugId::Invalid()};
    SignalAddress destination{};

    struct ControlReading {
        std::optional<InputSelectSubfunction> subfunction;  ///< nullopt for a value Table 7.14 does not define.
        uint8_t resultCode;                                 ///< result_status as sent.
        std::optional<InputSelectResult> result;            ///< nullopt for a reserved value.
    };
    struct StatusReading {
        uint8_t statusCode;                         ///< status as sent.
        std::optional<InputPlugStatus> status;      ///< nullopt for a reserved value.
    };

    [[nodiscard]] constexpr ControlReading AsControl() const noexcept {
        std::optional<InputSelectSubfunction> sub;
        if (first <= static_cast<uint8_t>(InputSelectSubfunction::kDisconnect)) {
            sub = static_cast<InputSelectSubfunction>(first);
        }
        const uint8_t code = static_cast<uint8_t>(second & kLowNibble);
        std::optional<InputSelectResult> result;
        switch (static_cast<InputSelectResult>(code)) {  // Table 7.18; 8..D are reserved
            case InputSelectResult::kNoError:
            case InputSelectResult::kDisabled:
            case InputSelectResult::kLocked:
            case InputSelectResult::kPointToPointNotOwner:
            case InputSelectResult::kInsufficientResource:
            case InputSelectResult::kSourceNotFound:
            case InputSelectResult::kNotSelected:
            case InputSelectResult::kNotRegistered:
            case InputSelectResult::kAnyOtherReason:
            case InputSelectResult::kNoInformation:
                result = static_cast<InputSelectResult>(code);
                break;
            default:
                break;
        }
        return ControlReading{sub, code, result};
    }
    [[nodiscard]] constexpr StatusReading AsStatus() const noexcept {
        const uint8_t code = static_cast<uint8_t>((second >> 4) & kLowNibble);
        std::optional<InputPlugStatus> status;
        if (code <= static_cast<uint8_t>(InputPlugStatus::kInsufficientResource)) {
            status = static_cast<InputPlugStatus>(code);
        }
        return StatusReading{code, status};
    }

private:
    static constexpr uint8_t kLowNibble = 0x0F;
};

struct InputSelectOperands {
    static constexpr Opcode kOpcode = Opcode::kInputSelect;
    static constexpr bool kRequiresUnitAddress = true;

    /// Used by CONTROL and SPECIFIC INQUIRY only.
    InputSelectSubfunction subfunction{InputSelectSubfunction::kSelect};
    BusNodeId sourceNode{BusNodeId::Unspecified()};
    /// The source node's unit output plug (Table 7.15).
    UnitPlugId outputPlug{UnitPlugId::NotApplicable()};
    /// The target's unit input plug. STATUS names it; CONTROL leaves the choice to the target
    /// (Table 7.19: any, except DISCONNECT, which names the plug to release).
    UnitPlugId inputPlug{UnitPlugId::AnyAvailableExternal()};
    /// Where the signal goes in the target; NoSignal() lets the target pick a default (Figure 7.18).
    SignalAddress signalDestination{SignalAddress::NoSignal()};

    using Reply = InputSelect;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        std::array<uint8_t, kOperandBytes> ops{};
        switch (t) {
            case CommandType::kStatus:
                // Figure 7.23: everything but input_plug and the destination placeholder is filled with ones.
                ops = {kUnspecifiedFill, kUnspecifiedFill, kUnspecifiedFill, kUnspecifiedFill, kUnspecifiedFill,
                       inputPlug.Raw(),
                       SignalAddress::NoSignal().bytes[0], SignalAddress::NoSignal().bytes[1],
                       kReservedZero};
                break;
            case CommandType::kControl:
            case CommandType::kSpecificInquiry:
                // Figure 7.17: reserved 0 | result_status F in operand[1].
                ops = {static_cast<uint8_t>(subfunction),
                       static_cast<uint8_t>(InputSelectResult::kNoInformation),
                       sourceNode.HighByte(), sourceNode.LowByte(),
                       outputPlug.Raw(), inputPlug.Raw(),
                       signalDestination.bytes[0], signalDestination.bytes[1],
                       kReservedZero};
                break;
            default:
                // NOTIFY is not defined for INPUT SELECT (Table 7.1).
                return Fail(AvcErrorKind::kInvalidArgument);
        }
        return w.Append(ops);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < kOperandBytes) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        return InputSelect{
            .first = in[0],
            .second = in[1],
            .node = BusNodeId::FromRaw(static_cast<uint16_t>((in[2] << 8) | in[3])),
            .outputPlug = UnitPlugId::FromRaw(in[4]),
            .inputPlug = UnitPlugId::FromRaw(in[5]),
            .destination = SignalAddress{{in[6], in[7]}},
        };
    }

private:
    static constexpr size_t kOperandBytes = 9;      // operand[0..8], Figures 7.17, 7.20, 7.23, 7.24
    static constexpr uint8_t kUnspecifiedFill = 0xFF;  // "FF16" in Figure 7.23
    static constexpr uint8_t kReservedZero = 0x00;     // operand[8], "shall be set to 0"
};

using InputSelectCommand = Command<InputSelectOperands>;

/// STATUS: which source node and output plug feed `inputPlug`, and the state of that plug (§7.2.4, §7.2.5).
[[nodiscard]] constexpr InputSelectCommand QueryInputPlug(UnitPlugId inputPlug) noexcept {
    return InputSelectCommand{.operands = {.inputPlug = inputPlug}};
}

namespace detail {
[[nodiscard]] constexpr InputSelectCommand InputSelectControl(InputSelectSubfunction subfunction, BusNodeId sourceNode,
                                                              UnitPlugId outputPlug, UnitPlugId inputPlug,
                                                              SignalAddress signalDestination) noexcept {
    return InputSelectCommand{.operands = {.subfunction = subfunction,
                                           .sourceNode = sourceNode,
                                           .outputPlug = outputPlug,
                                           .inputPlug = inputPlug,
                                           .signalDestination = signalDestination}};
}
} // namespace detail

/// CONTROL, CONNECT: make the target connect to `outputPlug` of `sourceNode` now (§7.2.1 1). The
/// target picks its input plug.
[[nodiscard]] constexpr InputSelectCommand ConnectInput(BusNodeId sourceNode, UnitPlugId outputPlug,
                                                        SignalAddress signalDestination = SignalAddress::NoSignal()) noexcept {
    return detail::InputSelectControl(InputSelectSubfunction::kConnect, sourceNode, outputPlug,
                                      UnitPlugId::AnyAvailableExternal(), signalDestination);
}

/// CONTROL, PATH CHANGE: as ConnectInput, but the target may reject it if it did not select that source (§7.2.1 2).
[[nodiscard]] constexpr InputSelectCommand ChangeInputPath(BusNodeId sourceNode, UnitPlugId outputPlug,
                                                           SignalAddress signalDestination = SignalAddress::NoSignal()) noexcept {
    return detail::InputSelectControl(InputSelectSubfunction::kPathChange, sourceNode, outputPlug,
                                      UnitPlugId::AnyAvailableExternal(), signalDestination);
}

/// CONTROL, SELECT: select the source; the connection is made when the target needs the stream (§7.2.1 3).
[[nodiscard]] constexpr InputSelectCommand SelectInput(BusNodeId sourceNode, UnitPlugId outputPlug,
                                                       SignalAddress signalDestination = SignalAddress::NoSignal()) noexcept {
    return detail::InputSelectControl(InputSelectSubfunction::kSelect, sourceNode, outputPlug,
                                      UnitPlugId::AnyAvailableExternal(), signalDestination);
}

/// CONTROL, DISCONNECT: release the peripheral connection on `inputPlug` (§7.2.1 4). The one subfunction
/// that names an input plug.
[[nodiscard]] constexpr InputSelectCommand DisconnectInput(BusNodeId sourceNode, UnitPlugId outputPlug,
                                                           UnitPlugId inputPlug) noexcept {
    return detail::InputSelectControl(InputSelectSubfunction::kDisconnect, sourceNode, outputPlug, inputPlug,
                                      SignalAddress::NoSignal());
}

} // namespace ASFW::AVC::Cmd
