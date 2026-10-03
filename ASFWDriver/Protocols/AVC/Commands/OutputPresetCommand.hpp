// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// OutputPresetCommand.hpp - OUTPUT PRESET (0x1C) operand codec.
//
// Source: TA 2002010 (CCM 1.1) §7.3; layouts Figures 7.28 (control command), 7.31 (control
// response), 7.32 (status command), 7.33 (status response); fields Tables 7.24, 7.25.
//
// OUTPUT PRESET stores, in a source device, the destination node that device should send an
// INPUT SELECT to when it wants to be input. It changes the target's stored presets.
// This codec has no caller: no supported device is known to implement it. Do not send it
// without a capture.
//
// Named requests:
//   QueryPresetCount()            STATUS   §7.3.4  (entry 7F asks how many entries exist)
//   QueryPreset(entry)            STATUS   §7.3.4
//   AddPreset(node, destination)  CONTROL  §7.3.1  (new entry; the target assigns the number)
//   CancelPreset(entry)           CONTROL  §7.3.1

#pragma once

#include "CcmTypes.hpp"

#include "../Core/AvcCommand.hpp"
#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ASFW::AVC::Cmd {

/// An OUTPUT PRESET response, operands as sent.
struct OutputPreset {
    uint8_t first{0};  ///< operand[0]: self (bit 7, status only) | preset_entry_number (bits 6..0).
    BusNodeId destinationNode{BusNodeId::Unspecified()};
    SignalAddress destination{};

    static constexpr uint8_t kSelfBit = 0x80;
    static constexpr uint8_t kEntryNumberMask = 0x7F;

    [[nodiscard]] constexpr uint8_t EntryNumber() const noexcept { return first & kEntryNumberMask; }
    /// STATUS reply only: the entry was set locally in the target (Figure 7.33).
    [[nodiscard]] constexpr bool IsSelf() const noexcept { return (first & kSelfBit) != 0; }
    /// A status reply for an unoccupied entry leaves node and destination at FF FF (§7.3.5).
    [[nodiscard]] constexpr bool IsUnoccupied() const noexcept {
        return destinationNode.IsUnspecified() && destination.bytes[0] == kUnoccupiedByte &&
               destination.bytes[1] == kUnoccupiedByte;
    }

    static constexpr uint8_t kUnoccupiedByte = 0xFF;
};

struct OutputPresetOperands {
    static constexpr Opcode kOpcode = Opcode::kOutputPreset;
    static constexpr bool kRequiresUnitAddress = true;

    /// "Invalid" entry number: a controller sends it to ask for a new entry (§7.3.1), and in
    /// a STATUS command to ask how many entries the target supports (§7.3.4).
    static constexpr uint8_t kNewEntry = 0x7F;

    uint8_t entry{kNewEntry};
    BusNodeId destinationNode{BusNodeId::Unspecified()};
    /// Where the signal goes in the destination device; NoSignal() when unspecified (Figure 7.29).
    SignalAddress signalDestination{SignalAddress::NoSignal()};
    /// A CONTROL that cancels `entry`: node and destination are all ones (§7.3.1).
    bool cancel{false};

    using Reply = OutputPreset;

    [[nodiscard]] Expected<void> Write(OperandWriter& w, CommandType t) const noexcept {
        const uint8_t entryByte = static_cast<uint8_t>(entry & OutputPreset::kEntryNumberMask);  // bit 7 is 0
        std::array<uint8_t, kOperandBytes> ops{};
        switch (t) {
            case CommandType::kStatus:
                // Figure 7.32: operand[1..4] are ones.
                ops = {entryByte, kFill, kFill, kFill, kFill};
                break;
            case CommandType::kControl:
            case CommandType::kSpecificInquiry:
                if (cancel) {
                    ops = {entryByte, kFill, kFill, kFill, kFill};
                } else {
                    ops = {entryByte, destinationNode.HighByte(), destinationNode.LowByte(),
                           signalDestination.bytes[0], signalDestination.bytes[1]};
                }
                break;
            default:
                // NOTIFY is not defined for OUTPUT PRESET (Table 7.1).
                return Fail(AvcErrorKind::kInvalidArgument);
        }
        return w.Append(ops);
    }

    [[nodiscard]] static Expected<Reply> Read(std::span<const uint8_t> in) noexcept {
        if (in.size() < kOperandBytes) {
            return FailAt(AvcErrorKind::kOperandsTooShort, static_cast<uint16_t>(in.size()));
        }
        return OutputPreset{
            .first = in[0],
            .destinationNode = BusNodeId::FromRaw(static_cast<uint16_t>((in[1] << 8) | in[2])),
            .destination = SignalAddress{{in[3], in[4]}},
        };
    }

private:
    static constexpr size_t kOperandBytes = 5;
    static constexpr uint8_t kFill = 0xFF;
};

using OutputPresetCommand = Command<OutputPresetOperands>;

/// STATUS: how many preset entries the target supports (the reply's entry number is the
/// highest entry plus one; §7.3.5).
[[nodiscard]] constexpr OutputPresetCommand QueryPresetCount() noexcept {
    return OutputPresetCommand{.operands = {.entry = OutputPresetOperands::kNewEntry}};
}

/// STATUS: the contents of preset entry `entry`.
[[nodiscard]] constexpr OutputPresetCommand QueryPreset(uint8_t entry) noexcept {
    return OutputPresetCommand{.operands = {.entry = entry}};
}

/// CONTROL: ask for a new preset entry for `destinationNode`. The reply carries the number the target assigned.
[[nodiscard]] constexpr OutputPresetCommand AddPreset(BusNodeId destinationNode,
                                                      SignalAddress signalDestination = SignalAddress::NoSignal()) noexcept {
    return OutputPresetCommand{.operands = {.entry = OutputPresetOperands::kNewEntry,
                                            .destinationNode = destinationNode,
                                            .signalDestination = signalDestination}};
}

/// CONTROL: cancel preset entry `entry`. Only the controller that made the entry can cancel it (§7.3.1).
[[nodiscard]] constexpr OutputPresetCommand CancelPreset(uint8_t entry) noexcept {
    return OutputPresetCommand{.operands = {.entry = entry, .cancel = true}};
}

} // namespace ASFW::AVC::Cmd
