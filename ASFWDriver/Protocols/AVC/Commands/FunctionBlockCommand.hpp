// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// FunctionBlockCommand.hpp - FUNCTION BLOCK (0xB8) on the audio subunit:
// selector blocks, and the mute and volume controls of feature blocks.
// Phase 1 covers what today's callers use (the Phase 88 mixer map: selectors,
// mutes, volumes). Other feature controls and processing blocks come later.
//
// Layout: ta1394 audio/src/lib.rs (MIT): AudioFuncBlkType :35-46,
// CtlAttr :54-100, AudioFuncBlk :180-260 (OPCODE :256), AudioSelector :283-350,
// FeatureCtl :773-816, AudioCh :943-963, AudioFeature :993-1066.
// Linux bebob_command.c:10-86 (selector CONTROL/STATUS). Fresh implementation.
//
// Operands: [fb type][fb id][control attribute][selector length][selector data][control]
//   Selector: selector data = [input plug]; control = [0x01]. Linux:
//     buf[3]=0x80 buf[4]=fb id buf[5]=0x10 buf[6]=0x02 buf[7]=plug (0xFF for STATUS) buf[8]=0x01
//   Feature:  selector data = [audio channel]; control = [control selector][length][data].
//
// BYTE-EXACT RULE: today's AudioFunctionBlockCommand.{hpp,cpp} set the Phase 88
// mixer on hardware. The new codec must produce the same bytes for the same
// calls (differential test). If ta1394 and the old code disagree, the old code
// wins and the disagreement is reported in the phase-1 review.
//
// Implementation: FunctionBlockCommand.cpp (phase 1).

#pragma once

#include "../Core/AvcError.hpp"
#include "../Core/AvcFrame.hpp"
#include "../Core/AvcTypes.hpp"

#include <cstdint>

namespace ASFW::AVC::Cmd {

enum class FunctionBlockType : uint8_t {
    kSelector = 0x80,
    kFeature = 0x81,
    kProcessing = 0x82,
};

enum class ControlAttribute : uint8_t {
    kResolution = 0x01,
    kMinimum = 0x02,
    kMaximum = 0x03,
    kDefault = 0x04,
    kDuration = 0x08,
    kCurrent = 0x10,
    kMove = 0x18,
    kDelta = 0x19,
};

/// Feature block control selectors. ta1394 audio lib.rs:802-813.
enum class FeatureControl : uint8_t {
    kMute = 0x01,
    kVolume = 0x02,
    kLrBalance = 0x03,
    kFrBalance = 0x04,
    kBass = 0x05,
    kMid = 0x06,
    kTreble = 0x07,
    kGraphicEqualizer = 0x08,
    kAutomaticGain = 0x09,
    kDelay = 0x0A,
    kBassBoost = 0x0B,
    kLoudness = 0x0C,
};

inline constexpr uint8_t kSelectorControl = 0x01;   ///< ta1394 lib.rs:289; Linux bebob_command.c:28
inline constexpr uint8_t kMasterChannel = 0x00;     ///< ta1394 lib.rs:961
inline constexpr uint8_t kBooleanTrue = 0x70;       ///< ta1394 lib.rs:815 (mute on)
inline constexpr uint8_t kBooleanFalse = 0x60;      ///< ta1394 lib.rs:816 (mute off)
inline constexpr int16_t kVolumeInvalid = 0x7FFF;   ///< ta1394 lib.rs:359

// ---- Selector ----------------------------------------------------------------

struct SelectorValue {
    uint8_t functionBlockId{0};
    uint8_t inputPlug{0xFF};
};

/// `subunit`: normally the audio subunit (kAudioSubunit0).
[[nodiscard]] Expected<CommandFrame> BuildSelectorStatus(SubunitAddress subunit, uint8_t functionBlockId) noexcept;
[[nodiscard]] Expected<CommandFrame> BuildSelectorControl(SubunitAddress subunit, uint8_t functionBlockId,
                                                          uint8_t inputPlug) noexcept;
[[nodiscard]] Expected<SelectorValue> ParseSelector(const Response& response, ResponseCode expected) noexcept;

// ---- Feature: mute -----------------------------------------------------------

[[nodiscard]] Expected<CommandFrame> BuildFeatureMuteStatus(SubunitAddress subunit, uint8_t functionBlockId,
                                                            uint8_t channel) noexcept;
[[nodiscard]] Expected<CommandFrame> BuildFeatureMuteControl(SubunitAddress subunit, uint8_t functionBlockId,
                                                             uint8_t channel, bool muted) noexcept;
/// true = muted (kBooleanTrue). A value that is neither 0x70 nor 0x60 is kMalformedOperands.
[[nodiscard]] Expected<bool> ParseFeatureMute(const Response& response, ResponseCode expected) noexcept;

// ---- Feature: volume ---------------------------------------------------------
// Volume data is a big-endian int16 (ta1394 VolumeData, lib.rs:355-395).
// STATUS can ask for current, minimum, maximum, resolution or default.

[[nodiscard]] Expected<CommandFrame> BuildFeatureVolumeStatus(SubunitAddress subunit, uint8_t functionBlockId,
                                                              uint8_t channel,
                                                              ControlAttribute attribute = ControlAttribute::kCurrent) noexcept;
[[nodiscard]] Expected<CommandFrame> BuildFeatureVolumeControl(SubunitAddress subunit, uint8_t functionBlockId,
                                                               uint8_t channel, int16_t value) noexcept;
[[nodiscard]] Expected<int16_t> ParseFeatureVolume(const Response& response, ResponseCode expected) noexcept;

} // namespace ASFW::AVC::Cmd
