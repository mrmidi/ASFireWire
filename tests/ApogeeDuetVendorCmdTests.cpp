// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// ApogeeDuetVendorCmdTests.cpp - Unit tests for Apogee Duet vendor command encoding/decoding

#include <gtest/gtest.h>
#include "Protocols/Audio/OXFW/Apogee/ApogeeDuetVendorCmd.hpp"
#include "Protocols/Audio/OXFW/Apogee/ApogeeDuetTypes.hpp"

using namespace ASFW::Audio::OXFW::Apogee;

// ============================================================================
// VendorCmd Operand Building Tests
// ============================================================================

TEST(ApogeeDuetVendorCmd, BuildOperands_OutSourceIsMixer) {
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer, .boolValue = true};
    auto operands = cmd.BuildOperands();
    
    // Expected: PCM(3) + code + padding(2)
    ASSERT_EQ(operands.size(), 6u);
    EXPECT_EQ(operands[0], 'P');
    EXPECT_EQ(operands[1], 'C');
    EXPECT_EQ(operands[2], 'M');
    EXPECT_EQ(operands[3], static_cast<uint8_t>(VendorCmdCode::OutSourceIsMixer));
}

TEST(ApogeeDuetVendorCmd, BuildOperands_XlrIsConsumerLevel_WithIndex) {
    VendorCmd cmd{.code = VendorCmdCode::XlrIsConsumerLevel, .index = 1, .boolValue = true};
    auto operands = cmd.BuildOperands();
    
    ASSERT_EQ(operands.size(), 6u);
    EXPECT_EQ(operands[3], static_cast<uint8_t>(VendorCmdCode::XlrIsConsumerLevel));
    EXPECT_EQ(operands[4], 0x80);  // Channel specifier marker
    EXPECT_EQ(operands[5], 1);     // Channel index
}

TEST(ApogeeDuetVendorCmd, BuildOperands_MixerSrc_SourceEncoding) {
    // Source index encoding: ((src / 2) << 4) | (src % 2)
    VendorCmd cmd{.code = VendorCmdCode::MixerSrc, .index = 2, .index2 = 1, .u16Value = 0x1234};
    auto operands = cmd.BuildOperands();
    
    ASSERT_EQ(operands.size(), 6u);
    EXPECT_EQ(operands[3], static_cast<uint8_t>(VendorCmdCode::MixerSrc));
    // Source 2: ((2/2) << 4) | (2%2) = (1 << 4) | 0 = 0x10
    EXPECT_EQ(operands[4], 0x10);
    EXPECT_EQ(operands[5], 1);  // Destination
}

TEST(ApogeeDuetVendorCmd, BuildOperands_MixerSrc_Source3) {
    // Source 3: ((3/2) << 4) | (3%2) = (1 << 4) | 1 = 0x11
    VendorCmd cmd{.code = VendorCmdCode::MixerSrc, .index = 3, .index2 = 0};
    auto operands = cmd.BuildOperands();
    
    EXPECT_EQ(operands[4], 0x11);
}

// ============================================================================
// VendorCmd AppendVariable Tests
// ============================================================================

TEST(ApogeeDuetVendorCmd, AppendVariable_Bool_On) {
    VendorCmd cmd{.code = VendorCmdCode::OutMute, .boolValue = true};
    std::vector<uint8_t> data;
    cmd.AppendVariable(data);
    
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0], kBoolOn);
}

TEST(ApogeeDuetVendorCmd, AppendVariable_Bool_Off) {
    VendorCmd cmd{.code = VendorCmdCode::OutMute, .boolValue = false};
    std::vector<uint8_t> data;
    cmd.AppendVariable(data);
    
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0], kBoolOff);
}

TEST(ApogeeDuetVendorCmd, AppendVariable_U8_InGain) {
    VendorCmd cmd{.code = VendorCmdCode::InGain, .index = 0, .u8Value = 45};
    std::vector<uint8_t> data;
    cmd.AppendVariable(data);
    
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0], 45);
}

TEST(ApogeeDuetVendorCmd, AppendVariable_U16_MixerSrc) {
    VendorCmd cmd{.code = VendorCmdCode::MixerSrc, .u16Value = 0xABCD};
    std::vector<uint8_t> data;
    cmd.AppendVariable(data);
    
    ASSERT_EQ(data.size(), 2u);
    EXPECT_EQ(data[0], 0xAB);  // High byte (big-endian)
    EXPECT_EQ(data[1], 0xCD);  // Low byte
}

TEST(ApogeeDuetVendorCmd, AppendVariable_HwState) {
    VendorCmd cmd{.code = VendorCmdCode::HwState};
    cmd.hwStateValue = {0x01, 0x02, 0x00, 0x3F, 0x4E, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00};
    
    std::vector<uint8_t> data;
    cmd.AppendVariable(data);
    
    ASSERT_EQ(data.size(), 11u);
    EXPECT_EQ(data[0], 0x01);
    EXPECT_EQ(data[3], 0x3F);
    EXPECT_EQ(data[4], 0x4E);
    EXPECT_EQ(data[5], 0x1C);
}

// ============================================================================
// VendorCmd ParseVariable Tests
// ============================================================================

TEST(ApogeeDuetVendorCmd, ParseVariable_OutSourceIsMixer_On) {
    // Response: PCM + code + padding + value
    uint8_t response[] = {'P', 'C', 'M', 0x11, 0xff, 0xff, 0x70};
    
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer};
    bool result = cmd.ParseVariable(response, sizeof(response));
    
    EXPECT_TRUE(result);
    EXPECT_TRUE(cmd.boolValue);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_OutSourceIsMixer_Off) {
    uint8_t response[] = {'P', 'C', 'M', 0x11, 0xff, 0xff, 0x60};
    
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer};
    bool result = cmd.ParseVariable(response, sizeof(response));
    
    EXPECT_TRUE(result);
    EXPECT_FALSE(cmd.boolValue);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_XlrIsConsumerLevel_IndexMatch) {
    uint8_t response[] = {'P', 'C', 'M', 0x02, 0x80, 0x01, 0x70};
    
    VendorCmd cmd{.code = VendorCmdCode::XlrIsConsumerLevel, .index = 1};
    bool result = cmd.ParseVariable(response, sizeof(response));
    
    EXPECT_TRUE(result);
    EXPECT_TRUE(cmd.boolValue);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_XlrIsConsumerLevel_IndexMismatch) {
    uint8_t response[] = {'P', 'C', 'M', 0x02, 0x80, 0x00, 0x70};
    
    // Expecting index 1, but response has index 0
    VendorCmd cmd{.code = VendorCmdCode::XlrIsConsumerLevel, .index = 1};
    bool result = cmd.ParseVariable(response, sizeof(response));
    
    EXPECT_FALSE(result);  // Should fail on index mismatch
}

TEST(ApogeeDuetVendorCmd, ParseVariable_MixerSrc) {
    // Response with gain value 0xDE00
    uint8_t response[] = {'P', 'C', 'M', 0x10, 0x01, 0x00, 0xDE, 0x00};
    
    VendorCmd cmd{.code = VendorCmdCode::MixerSrc, .index = 1, .index2 = 0};
    bool result = cmd.ParseVariable(response, sizeof(response));
    
    EXPECT_TRUE(result);
    EXPECT_EQ(cmd.u16Value, 0xDE00);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_HwState) {
    uint8_t response[] = {
        'P', 'C', 'M', 0x07, 0xff, 0xff,
        0x01, 0x01, 0x00, 0x25, 0x4E, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    VendorCmd cmd{.code = VendorCmdCode::HwState};
    bool result = cmd.ParseVariable(response, sizeof(response));
    
    EXPECT_TRUE(result);
    EXPECT_EQ(cmd.hwStateValue[0], 0x01);  // outputMute = true
    EXPECT_EQ(cmd.hwStateValue[1], 0x01);  // target = InputPair0
    EXPECT_EQ(cmd.hwStateValue[3], 0x25);  // volume (inverted)
    EXPECT_EQ(cmd.hwStateValue[4], 0x4E);  // input gain L
    EXPECT_EQ(cmd.hwStateValue[5], 0x1C);  // input gain R
}

TEST(ApogeeDuetVendorCmd, ParseVariable_InvalidPrefix) {
    uint8_t response[] = {'X', 'Y', 'Z', 0x11, 0xff, 0xff, 0x70};
    
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer};
    bool result = cmd.ParseVariable(response, sizeof(response));
    
    EXPECT_FALSE(result);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_WrongCode) {
    uint8_t response[] = {'P', 'C', 'M', 0x09, 0xff, 0xff, 0x70};
    
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer};  // Expecting 0x11, got 0x09
    bool result = cmd.ParseVariable(response, sizeof(response));
    
    EXPECT_FALSE(result);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_TooShort) {
    uint8_t response[] = {'P', 'C', 'M', 0x11, 0xff};  // Only 5 bytes, need 7
    
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer};
    bool result = cmd.ParseVariable(response, sizeof(response));
    
    EXPECT_FALSE(result);
}

// ============================================================================
// Knob State Serialization Tests
// ============================================================================

TEST(ApogeeDuetVendorCmd, KnobState_RoundTrip) {
    DuetKnobState original{
        .outputMute = true,
        .target = DuetKnobTarget::InputPair0,
        .outputVolume = 0x3F,
        .inputGains = {0x4E, 0x1C}
    };
    
    VendorCmd cmd = BuildKnobStateControl(original);
    
    // Simulate response parsing
    VendorCmd response{.code = VendorCmdCode::HwState};
    response.hwStateValue = cmd.hwStateValue;
    
    DuetKnobState parsed = ParseKnobState(response);
    
    EXPECT_EQ(parsed.outputMute, original.outputMute);
    EXPECT_EQ(parsed.target, original.target);
    EXPECT_EQ(parsed.outputVolume, original.outputVolume);
    EXPECT_EQ(parsed.inputGains[0], original.inputGains[0]);
    EXPECT_EQ(parsed.inputGains[1], original.inputGains[1]);
}

TEST(ApogeeDuetVendorCmd, KnobState_VolumeInversion) {
    // Volume is stored as (MAX - value)
    DuetKnobState state{.outputVolume = 10};
    VendorCmd cmd = BuildKnobStateControl(state);
    
    // Expected stored value: 64 - 10 = 54 at index 3
    EXPECT_EQ(cmd.hwStateValue[3], 54);
}

// ============================================================================
// Mute Mode Helper Tests
// ============================================================================

TEST(ApogeeDuetVendorCmd, MuteMode_Parse_Never) {
    EXPECT_EQ(ParseMuteMode(true, true), DuetOutputMuteMode::Never);
    EXPECT_EQ(ParseMuteMode(false, false), DuetOutputMuteMode::Never);
}

TEST(ApogeeDuetVendorCmd, MuteMode_Parse_Normal) {
    EXPECT_EQ(ParseMuteMode(false, true), DuetOutputMuteMode::Normal);
}

TEST(ApogeeDuetVendorCmd, MuteMode_Parse_Swapped) {
    EXPECT_EQ(ParseMuteMode(true, false), DuetOutputMuteMode::Swapped);
}

TEST(ApogeeDuetVendorCmd, MuteMode_Build_RoundTrip) {
    for (auto mode : {DuetOutputMuteMode::Never, DuetOutputMuteMode::Normal, DuetOutputMuteMode::Swapped}) {
        bool mute, unmute;
        BuildMuteMode(mode, mute, unmute);
        EXPECT_EQ(ParseMuteMode(mute, unmute), mode);
    }
}

// ============================================================================
// Query Builder Tests
// ============================================================================

TEST(ApogeeDuetVendorCmd, BuildKnobStateQuery) {
    auto cmds = BuildKnobStateQuery();
    ASSERT_EQ(cmds.size(), 1u);
    EXPECT_EQ(cmds[0].code, VendorCmdCode::HwState);
}

TEST(ApogeeDuetVendorCmd, BuildOutputParamsQuery) {
    auto cmds = BuildOutputParamsQuery();
    EXPECT_EQ(cmds.size(), 8u);  // OutMute, OutVolume, OutSourceIsMixer, OutIsConsumerLevel, + 4 mute modes
}

TEST(ApogeeDuetVendorCmd, BuildInputParamsQuery) {
    auto cmds = BuildInputParamsQuery();
    EXPECT_EQ(cmds.size(), 13u);  // 2×gain, 2×polarity, 2×mic, 2×consumer, 2×phantom, 2×source, clickless
}

TEST(ApogeeDuetVendorCmd, BuildMixerParamsQuery) {
    auto cmds = BuildMixerParamsQuery();
    EXPECT_EQ(cmds.size(), 8u);  // 4 sources × 2 destinations
}

TEST(ApogeeDuetVendorCmd, BuildDisplayParamsQuery) {
    auto cmds = BuildDisplayParamsQuery();
    EXPECT_EQ(cmds.size(), 3u);  // isInput, followKnob, overhold
}
