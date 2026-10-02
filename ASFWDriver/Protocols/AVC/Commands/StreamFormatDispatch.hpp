// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// StreamFormatDispatch.hpp - Send a stream-format command with the opcode the
// unit answers.
//
// A unit is asked with EXTENDED STREAM FORMAT (0xBF) first. If it answers NOT
// IMPLEMENTED, the same command is resent once as STREAM FORMAT SUPPORT (0x2F);
// when that succeeds, the unit is remembered as 0x2F-only and is never asked
// 0xBF again. Apple AVCVideoServices keeps the same per-unit opcode
// (MusicSubunitController.cpp:150, 1784-1789). The Phase 88 is 0x2F-only and
// otherwise pays one refused 0xBF for every query.
//
// The opcode is learned only from a 0x2F success: a 0xBF unit may also answer
// NOT IMPLEMENTED at the end of a format list, and that must not flip it.

#pragma once

#include "StreamFormatCommand.hpp"
#include "../Core/IAvcUnit.hpp"

#include <utility>

namespace ASFW::AVC::Cmd {

template <typename Callback>
void SendStreamFormat(IAvcUnit& unit, StreamFormatCommand cmd, CommandType type, Callback&& completion) {
    if (cmd.operands.opcode == StreamFormatOpcode::kExtendedStreamFormat && unit.UsesStreamFormatSupportOpcode()) {
        cmd.operands.opcode = StreamFormatOpcode::kStreamFormatSupport;
    }
    const auto generation = unit.CurrentGeneration();
    SendCommand(unit, cmd, type, generation,
        [&unit, cmd, type, generation, cb = std::forward<Callback>(completion)](
            Expected<StreamFormatReply> reply) mutable {
        if (reply || reply.error().response != ResponseCode::kNotImplemented ||
            cmd.operands.opcode != StreamFormatOpcode::kExtendedStreamFormat ||
            !unit.MayLearnStreamFormatOpcode()) {
            cb(std::move(reply));
            return;
        }
        cmd.operands.opcode = StreamFormatOpcode::kStreamFormatSupport;
        SendCommand(unit, cmd, type, generation,
            [&unit, cb = std::move(cb)](Expected<StreamFormatReply> fallback) mutable {
            if (fallback) {
                unit.LearnStreamFormatSupportOpcode();
            }
            cb(std::move(fallback));
        });
    });
}

} // namespace ASFW::AVC::Cmd
