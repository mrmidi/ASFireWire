// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetVendorCmdTests.cpp - Unit tests for Apogee Duet vendor command encoding/decoding

#include <gtest/gtest.h>
#include <algorithm>
#include <span>
#include <type_traits>
#include "Async/Interfaces/IFireWireBus.hpp"
#include "Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.hpp"
#include "Audio/Protocols/Oxford/Apogee/ApogeeTypes.hpp"
#include "Bus/IRM/IRMClient.hpp"
#include "Protocols/AVC/CMP/CMPClient.hpp"
#include "Protocols/AVC/FCPTransport.hpp"

using namespace ASFW::Audio::Oxford::Apogee;
using VendorCmd = ApogeeDuetProtocol::VendorCommand;
using VendorCmdCode = ApogeeDuetProtocol::VendorCommand::Code;
using DuetKnobState = KnobState;
using DuetKnobTarget = KnobTarget;
using DuetOutputMuteMode = OutputMuteMode;

static_assert(!std::is_base_of_v<OSObject, ASFW::Protocols::AVC::FCPTransport>,
              "FCPTransport is shared C++ state, not a DriverKit OSObject");

// Constants from ApogeeDuetProtocol
constexpr uint8_t kBoolOn = 0x70;
constexpr uint8_t kBoolOff = 0x60;

namespace {

std::vector<ASFW::Protocols::AVC::FCPFrame> gSubmittedFCPFrames;
uint8_t gDuetInputFrequency{0x02U};
uint8_t gDuetOutputFrequency{0x02U};
bool gFailNextOutputFormatControl{false};

void ResetDuetFormatFixture(uint8_t inputFrequency = 0x02U,
                            uint8_t outputFrequency = 0x02U) {
    gSubmittedFCPFrames.clear();
    gDuetInputFrequency = inputFrequency;
    gDuetOutputFrequency = outputFrequency;
    gFailNextOutputFormatControl = false;
}

class RecordingAVCBus final : public ASFW::Async::IFireWireBus {
  public:
    ASFW::Async::AsyncHandle ReadBlock(
        ASFW::FW::Generation, ASFW::FW::NodeId, ASFW::Async::FWAddress address, uint32_t,
        ASFW::FW::FwSpeed, ASFW::Async::InterfaceCompletionCallback callback) override {
        const uint32_t value = (address.addressLo == ASFW::CMP::PCRRegisters::kOMPR ||
                                address.addressLo == ASFW::CMP::PCRRegisters::kIMPR)
                                   ? mprValue
                                   : pcrValue;
        const uint32_t pcrBE = OSSwapHostToBigInt32(value);
        std::array<uint8_t, 4> payload{};
        std::memcpy(payload.data(), &pcrBE, sizeof(pcrBE));
        callback(ASFW::Async::AsyncStatus::kSuccess, payload);
        return {.value = ++handle_};
    }

    ASFW::Async::AsyncHandle WriteBlock(
        ASFW::FW::Generation, ASFW::FW::NodeId, ASFW::Async::FWAddress,
        std::span<const uint8_t>, ASFW::FW::FwSpeed,
        ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(ASFW::Async::AsyncStatus::kSuccess, {});
        return {.value = ++handle_};
    }

    ASFW::Async::AsyncHandle Lock(
        ASFW::FW::Generation, ASFW::FW::NodeId, ASFW::Async::FWAddress,
        ASFW::FW::LockOp, std::span<const uint8_t> operand, uint32_t,
        ASFW::FW::FwSpeed, ASFW::Async::InterfaceCompletionCallback callback) override {
        lastLockOperand.assign(operand.begin(), operand.end());
        std::array<uint8_t, 4> oldValue{};
        std::copy_n(operand.begin(), oldValue.size(), oldValue.begin());
        if (failCompareSwap) {
            oldValue[3] ^= 0x01U;
        }
        callback(ASFW::Async::AsyncStatus::kSuccess, oldValue);
        return {.value = ++handle_};
    }

    bool Cancel(ASFW::Async::AsyncHandle) override { return false; }
    ASFW::FW::FwSpeed GetSpeed(ASFW::FW::NodeId) const override {
        return ASFW::FW::FwSpeed::S400;
    }
    uint32_t HopCount(ASFW::FW::NodeId, ASFW::FW::NodeId) const override { return 0; }
    ASFW::FW::Generation GetGeneration() const override { return ASFW::FW::Generation{1}; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return ASFW::FW::NodeId{0}; }

    bool failCompareSwap{false};
    uint32_t pcrValue{0x80000000U};
    uint32_t mprValue{0x80000001U}; // S400, one plug
    std::vector<uint8_t> lastLockOperand;

  private:
    uint32_t handle_{0};
};

} // namespace

// Helper to match old BuildOperands API
inline std::vector<uint8_t> BuildOperands(const VendorCmd& cmd) {
    auto operands = cmd.BuildOperandBase();
    cmd.AppendControlValue(operands);
    return operands;
}

// Helper to match old AppendVariable API
inline void AppendVariable(const VendorCmd& cmd, std::vector<uint8_t>& data) {
    cmd.AppendControlValue(data);
}

// Helper to match old ParseVariable API
inline bool ParseVariable(VendorCmd& cmd, const uint8_t* data, size_t size) {
    return cmd.ParseStatusPayload(std::span<const uint8_t>{data, size});
}

// Anonymous namespace functions from ApogeeDuetProtocol
inline OutputMuteMode ParseMuteMode(bool mute, bool unmute) noexcept {
    if (mute && unmute) {
        return OutputMuteMode::Never;
    }
    if (mute && !unmute) {
        return OutputMuteMode::Swapped;
    }
    if (!mute && unmute) {
        return OutputMuteMode::Normal;
    }
    return OutputMuteMode::Never;
}

inline void BuildMuteMode(OutputMuteMode mode, bool& mute, bool& unmute) noexcept {
    switch (mode) {
        case OutputMuteMode::Never:
            mute = true;
            unmute = true;
            break;
        case OutputMuteMode::Normal:
            mute = false;
            unmute = true;
            break;
        case OutputMuteMode::Swapped:
            mute = true;
            unmute = false;
            break;
    }
}

// Static member function aliases
inline VendorCmd BuildKnobStateControl(const KnobState& state) {
    return ApogeeDuetProtocol::BuildKnobStateControl(state);
}
inline KnobState ParseKnobState(const VendorCmd& cmd) {
    return ApogeeDuetProtocol::ParseKnobState(cmd);
}
inline std::vector<VendorCmd> BuildKnobStateQuery() {
    return ApogeeDuetProtocol::BuildKnobStateQuery();
}
inline std::vector<VendorCmd> BuildOutputParamsQuery() {
    return ApogeeDuetProtocol::BuildOutputParamsQuery();
}
inline std::vector<VendorCmd> BuildInputParamsQuery() {
    return ApogeeDuetProtocol::BuildInputParamsQuery();
}
inline std::vector<VendorCmd> BuildMixerParamsQuery() {
    return ApogeeDuetProtocol::BuildMixerParamsQuery();
}
inline std::vector<VendorCmd> BuildDisplayParamsQuery() {
    return ApogeeDuetProtocol::BuildDisplayParamsQuery();
}

// ============================================================================
// VendorCmd Operand Building Tests
// ============================================================================

TEST(ApogeeDuetVendorCmd, BuildOperands_OutSourceIsMixer) {
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer, .boolValue = true};
    auto operands = BuildOperands(cmd);
    
    // Expected: OUI(3) + Prefix(3) + code + Arg1 + Arg2 + value
    ASSERT_EQ(operands.size(), 10u);
    EXPECT_EQ(operands[3], 'P');
    EXPECT_EQ(operands[4], 'C');
    EXPECT_EQ(operands[5], 'M');
    EXPECT_EQ(operands[6], static_cast<uint8_t>(VendorCmdCode::OutSourceIsMixer));
}

TEST(ApogeeDuetVendorCmd, BuildOperands_XlrIsConsumerLevel_WithIndex) {
    VendorCmd cmd{.code = VendorCmdCode::XlrIsConsumerLevel, .index = 1, .boolValue = true};
    auto operands = BuildOperands(cmd);
    
    ASSERT_EQ(operands.size(), 10u);
    EXPECT_EQ(operands[6], static_cast<uint8_t>(VendorCmdCode::XlrIsConsumerLevel));
    EXPECT_EQ(operands[7], 0x80);  // Channel specifier marker
    EXPECT_EQ(operands[8], 1);     // Channel index
}

TEST(ApogeeDuetVendorCmd, BuildOperands_MixerSrc_SourceEncoding) {
    VendorCmd cmd{.code = VendorCmdCode::MixerSrc, .index = 2, .index2 = 1, .u16Value = 0x1234};
    auto operands = BuildOperands(cmd);
    
    ASSERT_EQ(operands.size(), 11u);
    EXPECT_EQ(operands[6], static_cast<uint8_t>(VendorCmdCode::MixerSrc));
    // Source 2: ((2/2) << 4) | (2%2) = (1 << 4) | 0 = 0x10
    EXPECT_EQ(operands[7], 0x10);
    EXPECT_EQ(operands[8], 1);  // Destination
}

TEST(ApogeeDuetVendorCmd, BuildOperands_MixerSrc_Source3) {
    VendorCmd cmd{.code = VendorCmdCode::MixerSrc, .index = 3, .index2 = 0};
    auto operands = BuildOperands(cmd);
    
    EXPECT_EQ(operands[7], 0x11);
}

// ============================================================================
// VendorCmd AppendVariable Tests
// ============================================================================

TEST(ApogeeDuetVendorCmd, AppendVariable_Bool_On) {
    VendorCmd cmd{.code = VendorCmdCode::OutMute, .boolValue = true};
    std::vector<uint8_t> data;
    AppendVariable(cmd, data);
    
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0], kBoolOn);
}

TEST(ApogeeDuetVendorCmd, AppendVariable_Bool_Off) {
    VendorCmd cmd{.code = VendorCmdCode::OutMute, .boolValue = false};
    std::vector<uint8_t> data;
    AppendVariable(cmd, data);
    
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0], kBoolOff);
}

TEST(ApogeeDuetVendorCmd, AppendVariable_U8_InGain) {
    VendorCmd cmd{.code = VendorCmdCode::InGain, .index = 0, .u8Value = 45};
    std::vector<uint8_t> data;
    AppendVariable(cmd, data);
    
    ASSERT_EQ(data.size(), 1u);
    EXPECT_EQ(data[0], 45);
}

TEST(ApogeeDuetVendorCmd, AppendVariable_U16_MixerSrc) {
    VendorCmd cmd{.code = VendorCmdCode::MixerSrc, .u16Value = 0xABCD};
    std::vector<uint8_t> data;
    AppendVariable(cmd, data);
    
    ASSERT_EQ(data.size(), 2u);
    EXPECT_EQ(data[0], 0xAB);  // High byte (big-endian)
    EXPECT_EQ(data[1], 0xCD);  // Low byte
}

TEST(ApogeeDuetVendorCmd, AppendVariable_HwState) {
    VendorCmd cmd{.code = VendorCmdCode::HwState};
    cmd.hwState = {0x01, 0x02, 0x00, 0x3F, 0x4E, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00};
    
    std::vector<uint8_t> data;
    AppendVariable(cmd, data);
    
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
    // Response: OUI + PCM + code + Arg1 + Arg2 + value
    uint8_t response[] = {0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x11, 0xff, 0xff, 0x70};
    
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer};
    bool result = ParseVariable(cmd, response, sizeof(response));
    
    EXPECT_TRUE(result);
    EXPECT_TRUE(cmd.boolValue);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_OutSourceIsMixer_Off) {
    uint8_t response[] = {0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x11, 0xff, 0xff, 0x60};
    
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer};
    bool result = ParseVariable(cmd, response, sizeof(response));
    
    EXPECT_TRUE(result);
    EXPECT_FALSE(cmd.boolValue);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_XlrIsConsumerLevel_IndexMatch) {
    uint8_t response[] = {0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x02, 0x80, 0x01, 0x70};
    
    VendorCmd cmd{.code = VendorCmdCode::XlrIsConsumerLevel, .index = 1};
    bool result = ParseVariable(cmd, response, sizeof(response));
    
    EXPECT_TRUE(result);
    EXPECT_TRUE(cmd.boolValue);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_XlrIsConsumerLevel_IndexMismatch) {
    uint8_t response[] = {0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x02, 0x80, 0x00, 0x70};
    
    // Expecting index 1, but response has index 0
    VendorCmd cmd{.code = VendorCmdCode::XlrIsConsumerLevel, .index = 1};
    bool result = ParseVariable(cmd, response, sizeof(response));
    
    EXPECT_FALSE(result);  // Should fail on index mismatch
}

TEST(ApogeeDuetVendorCmd, ParseVariable_MixerSrc) {
    // Response with gain value 0xDE00
    uint8_t response[] = {0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x10, 0x10, 0x01, 0xDE, 0x00};
    
    VendorCmd cmd{.code = VendorCmdCode::MixerSrc, .index = 2, .index2 = 1};
    bool result = ParseVariable(cmd, response, sizeof(response));
    
    EXPECT_TRUE(result);
    EXPECT_EQ(cmd.u16Value, 0xDE00);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_HwState) {
    uint8_t response[] = {
        0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x07, 0xff, 0xff,
        0x01, 0x01, 0x00, 0x25, 0x4E, 0x1C, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    
    VendorCmd cmd{.code = VendorCmdCode::HwState};
    bool result = ParseVariable(cmd, response, sizeof(response));
    
    EXPECT_TRUE(result);
    EXPECT_EQ(cmd.hwState[0], 0x01);  // outputMute = true
    EXPECT_EQ(cmd.hwState[1], 0x01);  // target = InputPair0
    EXPECT_EQ(cmd.hwState[3], 0x25);  // volume (inverted)
    EXPECT_EQ(cmd.hwState[4], 0x4E);  // input gain L
    EXPECT_EQ(cmd.hwState[5], 0x1C);  // input gain R
}

TEST(ApogeeDuetVendorCmd, ParseVariable_InvalidPrefix) {
    uint8_t response[] = {0x00, 0x03, 0xDB, 'X', 'Y', 'Z', 0x11, 0xff, 0xff, 0x70};
    
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer};
    bool result = ParseVariable(cmd, response, sizeof(response));
    
    EXPECT_FALSE(result);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_WrongCode) {
    uint8_t response[] = {0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x09, 0xff, 0xff, 0x70};
    
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer};  // Expecting 0x11, got 0x09
    bool result = ParseVariable(cmd, response, sizeof(response));
    
    EXPECT_FALSE(result);
}

TEST(ApogeeDuetVendorCmd, ParseVariable_TooShort) {
    uint8_t response[] = {0x00, 0x03, 0xDB, 'P', 'C', 'M', 0x11, 0xff};  // Only 8 bytes, need 10
    
    VendorCmd cmd{.code = VendorCmdCode::OutSourceIsMixer};
    bool result = ParseVariable(cmd, response, sizeof(response));
    
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
    response.hwState = cmd.hwState;
    
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
    EXPECT_EQ(cmd.hwState[3], 54);
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

TEST(ApogeeDuetDuplexAdapter, Applies48kToInputThenOutputUnitPlugsOnlyOnce) {
    using namespace ASFW::Protocols::AVC;
    RecordingAVCBus bus;
    auto transport = std::make_shared<FCPTransport>();
    ApogeeDuetProtocol protocol(bus, bus, 2, transport.get(), nullptr, nullptr, 0, 0);
    ResetDuetFormatFixture(0x01U, 0x01U);

    IOReturn completionStatus = kIOReturnNotReady;
    protocol.ApplyClockConfig(
        ASFW::Audio::AudioClockConfig{.sampleRateHz = 48000U},
        [&completionStatus](IOReturn status, ASFW::Audio::ClockApplyResult) {
            completionStatus = status;
        });

    ASSERT_EQ(completionStatus, kIOReturnSuccess);
    ASSERT_EQ(gSubmittedFCPFrames.size(), 6U);
    // Read both initial formations, then apply the OXFW input/output order,
    // and finally re-query both plugs before declaring the clock applied.
    EXPECT_EQ(gSubmittedFCPFrames[0].Payload()[0], 0x01U);
    EXPECT_EQ(gSubmittedFCPFrames[0].Payload()[2], 0x19U);
    EXPECT_EQ(gSubmittedFCPFrames[1].Payload()[0], 0x01U);
    EXPECT_EQ(gSubmittedFCPFrames[1].Payload()[2], 0x18U);
    constexpr std::array<uint8_t, 8> expectedInput{
        0x00, 0xFF, 0x19, 0x00, 0x90, 0x02, 0xFF, 0xFF};
    constexpr std::array<uint8_t, 8> expectedOutput{
        0x00, 0xFF, 0x18, 0x00, 0x90, 0x02, 0xFF, 0xFF};
    EXPECT_TRUE(std::ranges::equal(gSubmittedFCPFrames[2].Payload(), expectedInput));
    EXPECT_TRUE(std::ranges::equal(gSubmittedFCPFrames[3].Payload(), expectedOutput));
    EXPECT_EQ(gSubmittedFCPFrames[4].Payload()[2], 0x19U);
    EXPECT_EQ(gSubmittedFCPFrames[5].Payload()[2], 0x18U);
    EXPECT_EQ(gDuetInputFrequency, 0x02U);
    EXPECT_EQ(gDuetOutputFrequency, 0x02U);

    // The same restart request must use the cached formation instead of
    // perturbing the OXFW device with another pair of AV/C controls.
    protocol.ApplyClockConfig(
        ASFW::Audio::AudioClockConfig{.sampleRateHz = 48000U},
        [&completionStatus](IOReturn status, ASFW::Audio::ClockApplyResult) {
            completionStatus = status;
        });
    EXPECT_EQ(completionStatus, kIOReturnSuccess);
    EXPECT_EQ(gSubmittedFCPFrames.size(), 6U);
}

TEST(ApogeeDuetDuplexAdapter, PrepareDuplexRevalidates48kBeforeResourceAllocation) {
    using namespace ASFW::Protocols::AVC;
    RecordingAVCBus bus;
    ASFW::IRM::IRMClient irm(bus);
    ASFW::CMP::CMPClient cmp(bus, bus);
    auto transport = std::make_shared<FCPTransport>();
    ApogeeDuetProtocol protocol(bus, bus, 2, transport.get(), &irm, &cmp, 1, 0);
    ASFW::Audio::AudioDuplexChannels channels{};
    IOReturn completionStatus = kIOReturnNotReady;

    ResetDuetFormatFixture(0x01U, 0x01U);
    protocol.PrepareDuplex(
        channels, ASFW::Audio::AudioClockConfig{.sampleRateHz = 48000U},
        [&completionStatus](IOReturn status, ASFW::Audio::DuplexPrepareResult) {
            completionStatus = status;
        });
    ASSERT_EQ(completionStatus, kIOReturnSuccess);
    ASSERT_EQ(gSubmittedFCPFrames.size(), 6U);

    // Simulate an external format change while the driver is still in the
    // same bus generation.  The next Start must not trust its old cache.
    ResetDuetFormatFixture(0x01U, 0x01U);
    completionStatus = kIOReturnNotReady;
    protocol.PrepareDuplex(
        channels, ASFW::Audio::AudioClockConfig{.sampleRateHz = 48000U},
        [&completionStatus](IOReturn status, ASFW::Audio::DuplexPrepareResult) {
            completionStatus = status;
        });
    EXPECT_EQ(completionStatus, kIOReturnSuccess);
    ASSERT_EQ(gSubmittedFCPFrames.size(), 6U);
    EXPECT_EQ(gDuetInputFrequency, 0x02U);
    EXPECT_EQ(gDuetOutputFrequency, 0x02U);
}

TEST(ApogeeDuetDuplexAdapter, MapsRequested44100RateIntoUnitPlugSignalFormat) {
    using namespace ASFW::Protocols::AVC;
    RecordingAVCBus bus;
    auto transport = std::make_shared<FCPTransport>();
    ApogeeDuetProtocol protocol(bus, bus, 2, transport.get(), nullptr, nullptr, 0, 0);
    ResetDuetFormatFixture();

    IOReturn completionStatus = kIOReturnNotReady;
    protocol.ApplyClockConfig(
        ASFW::Audio::AudioClockConfig{.sampleRateHz = 44100U},
        [&completionStatus](IOReturn status, ASFW::Audio::ClockApplyResult) {
            completionStatus = status;
        });

    ASSERT_EQ(completionStatus, kIOReturnSuccess);
    ASSERT_EQ(gSubmittedFCPFrames.size(), 6U);
    EXPECT_EQ(gSubmittedFCPFrames[2].Payload()[5], 0x01U);
    EXPECT_EQ(gSubmittedFCPFrames[3].Payload()[5], 0x01U);
}

TEST(ApogeeDuetDuplexAdapter, RestoresInputFormationWhenOutputFormatControlFails) {
    using namespace ASFW::Protocols::AVC;
    RecordingAVCBus bus;
    auto transport = std::make_shared<FCPTransport>();
    ApogeeDuetProtocol protocol(bus, bus, 2, transport.get(), nullptr, nullptr, 0, 0);
    ResetDuetFormatFixture(0x01U, 0x01U);
    gFailNextOutputFormatControl = true;

    IOReturn completionStatus = kIOReturnSuccess;
    protocol.ApplyClockConfig(
        ASFW::Audio::AudioClockConfig{.sampleRateHz = 48000U},
        [&completionStatus](IOReturn status, ASFW::Audio::ClockApplyResult) {
            completionStatus = status;
        });

    EXPECT_EQ(completionStatus, kIOReturnError);
    ASSERT_EQ(gSubmittedFCPFrames.size(), 5U);
    EXPECT_EQ(gSubmittedFCPFrames[2].Payload()[2], 0x19U);
    EXPECT_EQ(gSubmittedFCPFrames[3].Payload()[2], 0x18U);
    EXPECT_EQ(gSubmittedFCPFrames[4].Payload()[2], 0x19U);
    EXPECT_EQ(gSubmittedFCPFrames[4].Payload()[5], 0x01U);
    EXPECT_EQ(gDuetInputFrequency, 0x01U);
    EXPECT_EQ(gDuetOutputFrequency, 0x01U);
}

TEST(ApogeeDuetDuplexAdapter, ProgramsIRMChannelIntoOutputPCR) {
    RecordingAVCBus bus;
    ASFW::IRM::IRMClient irm(bus);
    ASFW::CMP::CMPClient cmp(bus, bus);
    ApogeeDuetProtocol protocol(bus, bus, 2, nullptr, &irm, &cmp, 1);
    ASFW::Audio::AudioDuplexChannels channels{};
    channels.deviceToHostIsoChannel = 5;
    protocol.SetAssignedChannels(channels);

    IOReturn status = kIOReturnNotReady;
    protocol.ProgramRx([&status](IOReturn result, ASFW::Audio::DuplexStageResult) {
        status = result;
    });

    ASSERT_EQ(status, kIOReturnSuccess);
    ASSERT_EQ(bus.lastLockOperand.size(), 8U);
    uint32_t desiredBE = 0;
    std::memcpy(&desiredBE, bus.lastLockOperand.data() + 4, sizeof(desiredBE));
    // oPCR carries the negotiated S400 rate in bits 15:14.
    EXPECT_EQ(OSSwapBigToHostInt32(desiredBE), 0x81058000U);
}

TEST(CMPClientPCRBits, UsesSixBitPointToPointConnectionCount) {
    EXPECT_EQ(ASFW::CMP::PCRBits::GetP2P(0xBF000000U), 63U);
    EXPECT_EQ(ASFW::CMP::PCRBits::SetP2P(0x80000000U, 63U), 0xBF000000U);
}

TEST(CMPClientPCRBits, RejectsChangingChannelOfExistingPointToPointConnection) {
    RecordingAVCBus bus;
    bus.pcrValue = 0x81030000U; // online, one connection, channel 3
    ASFW::CMP::CMPClient cmp(bus, bus);

    ASFW::CMP::CMPStatus status = ASFW::CMP::CMPStatus::Success;
    cmp.ConnectOPCR({.guid = 1, .nodeId = ASFW::FW::NodeId{2}, .generation = ASFW::FW::Generation{1}},
                    0, 5, [&status](ASFW::CMP::CMPStatus result) {
        status = result;
    });

    EXPECT_EQ(status, ASFW::CMP::CMPStatus::NoResources);
    EXPECT_TRUE(bus.lastLockOperand.empty());
}

TEST(ApogeeDuetDuplexAdapter, MapsCompletedCmpFailureToErrorRatherThanTimeout) {
    RecordingAVCBus bus;
    ASFW::IRM::IRMClient irm(bus);
    ASFW::CMP::CMPClient cmp(bus, bus);
    ApogeeDuetProtocol protocol(bus, bus, 2, nullptr, &irm, &cmp, 1);

    IOReturn rxStatus = kIOReturnNotReady;
    protocol.ProgramRx([&rxStatus](IOReturn status, ASFW::Audio::DuplexStageResult) {
        rxStatus = status;
    });
    EXPECT_EQ(rxStatus, kIOReturnSuccess);

    bus.failCompareSwap = true;
    IOReturn txStatus = kIOReturnNotReady;
    protocol.ProgramTxAndEnableDuplex(
        [&txStatus](IOReturn status, ASFW::Audio::DuplexStageResult) { txStatus = status; });
    EXPECT_EQ(txStatus, kIOReturnError);
}

// Linker stub for the profile's FCP transition tests.
namespace ASFW::Protocols::AVC {
bool FCPTransport::init(Protocols::Ports::FireWireBusOps*,
                        Protocols::Ports::FireWireBusInfo*,
                        Discovery::FWDevice*,
                        Scheduling::ITimerScheduler&,
                        const FCPTransportConfig&) {
    return true;
}

FCPTransport::~FCPTransport() = default;

void FCPTransport::Shutdown() {}

FCPHandle FCPTransport::SubmitCommand(const FCPFrame& command, FCPCompletion completion) {
    return SubmitCommand(command, std::move(completion), {});
}

FCPHandle FCPTransport::SubmitCommand(const FCPFrame& command,
                                      FCPCompletion completion,
                                      FCPCommandPolicy) {
    gSubmittedFCPFrames.push_back(command);
    FCPFrame response = command;
    response.data[0] = static_cast<uint8_t>(AVCResponseType::kAccepted);
    const auto payload = command.Payload();
    if (payload.size() >= 6U && payload[1] == 0xFFU &&
        (payload[2] == 0x18U || payload[2] == 0x19U)) {
        const bool isInput = payload[2] == 0x19U;
        if (payload[0] == static_cast<uint8_t>(AVCCommandType::kStatus)) {
            response.data[4] = 0x90U;
            response.data[5] = isInput ? gDuetInputFrequency : gDuetOutputFrequency;
        } else if (payload[0] == static_cast<uint8_t>(AVCCommandType::kControl)) {
            if (!isInput && gFailNextOutputFormatControl) {
                gFailNextOutputFormatControl = false;
                completion(FCPStatus::kTransportError, FCPFrame{});
                return FCPHandle{.transactionID = static_cast<uint32_t>(gSubmittedFCPFrames.size())};
            }
            if (isInput) {
                gDuetInputFrequency = payload[5];
            } else {
                gDuetOutputFrequency = payload[5];
            }
        }
    }
    completion(FCPStatus::kOk, response);
    return FCPHandle{.transactionID = static_cast<uint32_t>(gSubmittedFCPFrames.size())};
}
}
