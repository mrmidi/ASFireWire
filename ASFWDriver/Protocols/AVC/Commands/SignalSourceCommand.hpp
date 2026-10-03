// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SignalSourceCommand.hpp - SIGNAL SOURCE (0x1A) operand codec.
//
// Source: TA 2002010 (CCM 1.1) §7.1; frame layouts Figures 7.1, 7.6, 7.7, 7.8; worked
// examples Annex C Tables C.1, C.2, C.6. Apple AM824AVC::GetSignalSourceInfo (STATUS,
// operand[0] FF), QuerySyncPlugReconnect and SyncPlugReconnect (SPECIFIC INQUIRY and
// CONTROL, operand[0] 0F); FFADO libavc/ccm/avc_signal_source.cpp. ta1394 ccm/src/lib.rs:161-212
// writes FF for CONTROL too, which departs from Figure 7.1.
//
// Named requests (build these, do not assemble bytes):
//   QuerySignalSource(destination)                 STATUS            §7.1.4
//   ConnectSignalSource(source, destination)       CONTROL           §7.1.1
//   CanConnectSignalSource(source, destination)    SPECIFIC INQUIRY  (control format)
//   WatchSignalSource(destination)                 NOTIFY            §7.1.6
// Encode each with the matching CommandType; the codec writes the operand[0] that
// type needs from the named defaults in CcmTypes.hpp.

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

/// A SIGNAL SOURCE response. `first` is operand[0] as sent: read it as a STATUS reply
/// (output_status, conv, signal_status) or a CONTROL reply (result_status) by the
/// command type that was answered.
struct SignalSource {
    SignalSourceFirstOperand first{SignalSourceFirstOperand::FromRaw(0)};
    SignalAddress source{};
    SignalAddress destination{};

    /// Reading of a STATUS or NOTIFY reply (Figure 7.8).
    [[nodiscard]] constexpr SignalSourceStatusField Status() const noexcept { return first.AsStatus(); }
    /// Reading of a CONTROL reply (Figure 7.6).
    [[nodiscard]] constexpr SignalSourceControlField Control() const noexcept { return first.AsControl(); }

    /// A STATUS reply whose output_status is "virtual output" carries the repeated stream in
    /// signal_source instead of a plug address (Figure 7.11): the isochronous channel the
    /// target receives and the iPCR it receives it on.
    struct VirtualOutputSource {
        uint8_t isochronousChannel;  ///< Channel number, 0..63.
        UnitPlugId inputPlug;        ///< The iPCR (Table 7.11).
    };
    [[nodiscard]] constexpr std::optional<VirtualOutputSource> AsVirtualOutput() const noexcept {
        if (Status().Status() != OutputStatus::kVirtualOutput) return std::nullopt;
        if ((source.bytes[0] & kVirtualOutputPrefixMask) != kVirtualOutputPrefix) return std::nullopt;
        return VirtualOutputSource{static_cast<uint8_t>(source.bytes[0] & kChannelMask),
                                   UnitPlugId::FromRaw(source.bytes[1])};
    }

    static constexpr uint8_t kVirtualOutputPrefixMask = 0xC0;  ///< Top two bits of the first signal_source byte.
    static constexpr uint8_t kVirtualOutputPrefix = 0xC0;      ///< "11" (Figure 7.11).
    static constexpr uint8_t kChannelMask = 0x3F;              ///< isochronous_channel, 6 bits.
};

struct SignalSourceOperands {
    static constexpr Opcode kOpcode = Opcode::kSignalSource;
    static constexpr bool kRequiresUnitAddress = true;

    /// The plug whose signal path is asked about (STATUS, NOTIFY) or set up (CONTROL, SPECIFIC INQUIRY).
    SignalAddress destination{};
    /// The signal source to connect. Required for CONTROL and SPECIFIC INQUIRY; STATUS and NOTIFY ask for it.
    std::optional<SignalAddress> source{std::nullopt};

    using Reply = SignalSource;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        switch (t) {
            case CommandType::kStatus:
            case CommandType::kNotify: {
                // §7.1.4, §7.1.6: the NOTIFY command has the STATUS command's syntax. The
                // source field carries the "invalid" placeholder, the destination is the plug asked about.
                return Append(w, SignalSourceStatusField::Request().Raw(), SignalAddress::NoSignal(), destination);
            }
            case CommandType::kControl:
            case CommandType::kSpecificInquiry: {
                if (!source.has_value()) {
                    return Fail(AvcErrorKind::kInvalidArgument);
                }
                return Append(w, SignalSourceControlField::Request().Raw(), *source, destination);
            }
            default:
                return Fail(AvcErrorKind::kInvalidArgument);
        }
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < kOperandBytes) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        return SignalSource{
            .first = SignalSourceFirstOperand::FromRaw(in[0]),
            .source = SignalAddress{{in[1], in[2]}},
            .destination = SignalAddress{{in[3], in[4]}},
        };
    }

private:
    static constexpr size_t kOperandBytes = 5;  // operand[0] + signal_source (2) + signal_destination (2)

    [[nodiscard]] static Expected<void> Append(OperandWriter& w, uint8_t first, const SignalAddress& source,
                                               const SignalAddress& destination) noexcept {
        const std::array<uint8_t, kOperandBytes> ops = {first,
                                                        source.bytes[0], source.bytes[1],
                                                        destination.bytes[0], destination.bytes[1]};
        return w.Append(ops);
    }
};

using SignalSourceCommand = Command<SignalSourceOperands>;

/// STATUS: which source feeds `destination` (a unit output plug or a subunit destination
/// plug), and the state of that plug (§7.1.4, §7.1.5).
[[nodiscard]] constexpr SignalSourceCommand QuerySignalSource(SignalAddress destination) noexcept {
    return SignalSourceCommand{.operands = {.destination = destination}};
}

/// CONTROL: set up the unit-internal path from `source` to `destination` (§7.1.1).
/// Changes device state. Send only to a device whose frames are proven.
[[nodiscard]] constexpr SignalSourceCommand ConnectSignalSource(SignalAddress source,
                                                                SignalAddress destination) noexcept {
    return SignalSourceCommand{.operands = {.destination = destination, .source = source}};
}

/// SPECIFIC INQUIRY: would the device accept ConnectSignalSource(source, destination)? It
/// changes nothing. Answered 0C (STABLE) if supported, 08 (NOT IMPLEMENTED) if not.
[[nodiscard]] constexpr SignalSourceCommand CanConnectSignalSource(SignalAddress source,
                                                                   SignalAddress destination) noexcept {
    return SignalSourceCommand{.operands = {.destination = destination, .source = source}};
}

/// NOTIFY: tell me when the source or state of `destination` changes (§7.1.6).
[[nodiscard]] constexpr SignalSourceCommand WatchSignalSource(SignalAddress destination) noexcept {
    return QuerySignalSource(destination);
}

} // namespace ASFW::AVC::Cmd
