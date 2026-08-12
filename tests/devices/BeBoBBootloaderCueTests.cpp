// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBBootloaderCueTests.cpp — the cue is the one write this driver may make to
// a BeBoB bootloader, and its neighbours in the same byte field permanently
// reprogram device identity in flash. These tests pin the exact bytes, the write
// choke point, and the state machine rules that keep the cue single-shot.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Families/BeBoB/Bootloader/BeBoBBootloaderPreparation.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace {

using namespace ASFW::Audio::Families::BeBoB::Bootloader;

[[nodiscard]] BootRomInfo MakeInfo(uint32_t protocolVersion,
                                   uint32_t bootloaderVersion) {
    BootRomInfo info{};
    const auto store = [&info](size_t offset, uint32_t value) {
        info.raw[offset] = static_cast<uint8_t>(value & 0xFFU);
        info.raw[offset + 1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
        info.raw[offset + 2] = static_cast<uint8_t>((value >> 16U) & 0xFFU);
        info.raw[offset + 3] = static_cast<uint8_t>((value >> 24U) & 0xFFU);
    };
    store(kInfoOffsetProtocolVersion, protocolVersion);
    store(kInfoOffsetBootloaderVersion, bootloaderVersion);
    return info;
}

// --- The bytes --------------------------------------------------------------

TEST(BeBoBBootloaderCueTests, EmitsExactlyTheLinuxCueQuadlets) {
    // Linux MAUDIO_BOOTLOADER_CUE1/2/3 written with cpu_to_le32:
    // bebob_maudio.c:41-49,119-121. CUE1 is the protocol version, which Linux
    // hardcodes to 1 and we read live from the info block.
    const BeBoBBootloaderCue cue{MakeInfo(1, 0x0000'0001)};
    const std::array<uint8_t, 12> expected{
        0x01, 0x00, 0x00, 0x00,  // CUE1: protocol version 1
        0x00, 0x00, 0x11, 0x01,  // CUE2: 0x01110000, command 0x11
        0x00, 0x00, 0x00, 0x00,  // CUE3: start mode 0 = application
    };
    const auto bytes = cue.Bytes();
    EXPECT_TRUE(std::equal(expected.begin(), expected.end(), bytes.begin()));
}

TEST(BeBoBBootloaderCueTests, CarriesTheDeviceProtocolVersionNotAConstant) {
    // The vendor kext reads info+0x08 rather than assuming 1, which is the safer
    // behaviour on firmware we have not seen.
    const BeBoBBootloaderCue cue{MakeInfo(3, 1)};
    EXPECT_EQ(cue.ProtocolVersion(), 3U);
    EXPECT_EQ(cue.Bytes()[0], 0x03);
    // The command and start mode are unaffected by the version.
    EXPECT_EQ(cue.Bytes()[6], 0x11);
    EXPECT_EQ(cue.Bytes()[8], 0x00);
}

TEST(BeBoBBootloaderCueTests, InfoBlockDecodesLittleEndianNotBusOrder) {
    // This register window is little-endian, unlike ordinary 1394 payloads.
    const auto info = MakeInfo(0x0000'0002, 0x1234'5678);
    EXPECT_EQ(info.ProtocolVersion(), 0x0000'0002U);
    EXPECT_EQ(info.BootloaderVersion(), 0x1234'5678U);
    EXPECT_EQ(info.raw[kInfoOffsetBootloaderVersion], 0x78);  // LSB first
}

TEST(BeBoBBootloaderCueTests, BootloaderVersionIsAStateFlagNotAVersion) {
    EXPECT_FALSE(MakeInfo(1, 0).BootloaderActive());
    EXPECT_TRUE(MakeInfo(1, 1).BootloaderActive());
    EXPECT_TRUE(MakeInfo(1, 0x0001'0000).BootloaderActive());
}

// --- The choke point --------------------------------------------------------

TEST(BeBoBBootloaderCueTests, ChokePointAcceptsOnlyTheExactCue) {
    const BeBoBBootloaderCue cue{MakeInfo(1, 1)};
    const auto bytes = cue.Bytes();
    EXPECT_TRUE(IsPermittedBootloaderWrite(kBootloaderAddressHi,
                                           kRequestAddressLo, bytes));
}

TEST(BeBoBBootloaderCueTests, ChokePointRejectsEveryOtherAddressInTheWindow) {
    const BeBoBBootloaderCue cue{MakeInfo(1, 1)};
    const auto bytes = cue.Bytes();
    // A correct payload at the wrong register is still refused.
    EXPECT_FALSE(IsPermittedBootloaderWrite(kBootloaderAddressHi,
                                            kInfoAddressLo, bytes));
    EXPECT_FALSE(IsPermittedBootloaderWrite(kBootloaderAddressHi,
                                            kResponseAddressLo, bytes));
    EXPECT_FALSE(IsPermittedBootloaderWrite(kBootloaderAddressHi,
                                            kRequestAddressLo + 4, bytes));
    EXPECT_FALSE(IsPermittedBootloaderWrite(0xFFFEU, kRequestAddressLo, bytes));
}

TEST(BeBoBBootloaderCueTests, ChokePointRejectsEveryDestructiveCommandCode) {
    // The whole point of the guard: 0x0a rewrites the GUID, 0x10 the hardware
    // ID, 0x04-0x06 the firmware image, 0x0d the persistent config. Each is one
    // byte away from the cue in the same field.
    const BeBoBBootloaderCue cue{MakeInfo(1, 1)};
    for (const uint8_t destructive : {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0f,
                                      0x10, 0x12}) {
        std::vector<uint8_t> payload(cue.Bytes().begin(), cue.Bytes().end());
        payload[6] = destructive;  // the command-code byte
        EXPECT_FALSE(IsPermittedBootloaderWrite(kBootloaderAddressHi,
                                                kRequestAddressLo, payload))
            << "command code 0x" << std::hex << static_cast<unsigned>(destructive)
            << " must never reach the bus";
    }
}

TEST(BeBoBBootloaderCueTests, ChokePointRejectsDebuggerStartModeAndBadLengths) {
    const BeBoBBootloaderCue cue{MakeInfo(1, 1)};
    std::vector<uint8_t> debugger(cue.Bytes().begin(), cue.Bytes().end());
    debugger[8] = 0x02;  // eSM_Debugger: boots the debugger, never streams
    EXPECT_FALSE(IsPermittedBootloaderWrite(kBootloaderAddressHi,
                                            kRequestAddressLo, debugger));

    std::vector<uint8_t> shortPayload(cue.Bytes().begin(),
                                      cue.Bytes().begin() + 8);
    EXPECT_FALSE(IsPermittedBootloaderWrite(kBootloaderAddressHi,
                                            kRequestAddressLo, shortPayload));

    std::vector<uint8_t> longPayload(cue.Bytes().begin(), cue.Bytes().end());
    longPayload.push_back(0);
    EXPECT_FALSE(IsPermittedBootloaderWrite(kBootloaderAddressHi,
                                            kRequestAddressLo, longPayload));
}

TEST(BeBoBBootloaderCueTests, ChokePointAcceptsAnyProtocolVersionInQuadletZero) {
    // Quadlet 0 is the one field the device supplies, so it must stay free.
    for (const uint32_t version : {0U, 1U, 2U, 0xFFFF'FFFFU}) {
        const BeBoBBootloaderCue cue{MakeInfo(version, 1)};
        EXPECT_TRUE(IsPermittedBootloaderWrite(kBootloaderAddressHi,
                                               kRequestAddressLo, cue.Bytes()));
    }
}

// --- The state machine ------------------------------------------------------

TEST(BeBoBBootloaderPreparationTests, InactiveBootloaderWritesNothing) {
    auto step = BeginPreparation();
    EXPECT_TRUE(std::holds_alternative<ReadInfoBlock>(step.action));

    step = AdvancePreparation(step.state, InfoReadSucceeded{MakeInfo(1, 0)});
    step = AdvancePreparation(step.state, InfoReadSucceeded{MakeInfo(1, 0)});

    ASSERT_TRUE(IsRetired(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::NoCueNeeded);
    EXPECT_FALSE(std::holds_alternative<WriteCue>(step.action));
}

TEST(BeBoBBootloaderPreparationTests, ActiveBootloaderProducesExactlyOneCue) {
    auto step = BeginPreparation();
    step = AdvancePreparation(step.state, InfoReadSucceeded{MakeInfo(2, 1)});
    step = AdvancePreparation(step.state, InfoReadSucceeded{MakeInfo(2, 1)});

    ASSERT_TRUE(std::holds_alternative<WriteCue>(step.action));
    // Built from the info block that was actually read.
    EXPECT_EQ(std::get<WriteCue>(step.action).cue.ProtocolVersion(), 2U);
    EXPECT_TRUE(std::holds_alternative<AwaitingResponse>(step.state));
}

TEST(BeBoBBootloaderPreparationTests, ResponseTimeoutNeverReWritesTheCue) {
    auto step = BeginPreparation();
    step = AdvancePreparation(step.state, InfoReadSucceeded{MakeInfo(1, 1)});
    step = AdvancePreparation(step.state, InfoReadSucceeded{MakeInfo(1, 1)});
    ASSERT_TRUE(std::holds_alternative<WriteCue>(step.action));
    step = AdvancePreparation(step.state, CueWriteSucceeded{});

    // Exhaust the read budget. The cue must not be emitted again at any point:
    // the first write may have landed and the device may be mid-boot.
    for (int attempt = 0; attempt < 10; ++attempt) {
        step = AdvancePreparation(step.state, ResponseReadFailed{});
        EXPECT_FALSE(std::holds_alternative<WriteCue>(step.action))
            << "re-sent the cue on response attempt " << attempt;
        if (IsRetired(step.state)) break;
    }
    ASSERT_TRUE(IsRetired(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::ResponseTimeout);
}

TEST(BeBoBBootloaderPreparationTests, ResponseReadRetriesAreBounded) {
    auto step = BeginPreparation();
    step = AdvancePreparation(step.state, InfoReadSucceeded{MakeInfo(1, 1)});
    step = AdvancePreparation(step.state, InfoReadSucceeded{MakeInfo(1, 1)});
    step = AdvancePreparation(step.state, CueWriteSucceeded{});

    int reads = 0;
    while (!IsRetired(step.state)) {
        step = AdvancePreparation(step.state, ResponseReadFailed{});
        if (std::holds_alternative<ReadResponse>(step.action)) ++reads;
    }
    EXPECT_EQ(reads, kMaxResponseReadAttempts - 1);
}

TEST(BeBoBBootloaderPreparationTests, InfoReadRetriesAreBoundedAndSendNoCue) {
    auto step = BeginPreparation();
    for (int attempt = 0; attempt < 10; ++attempt) {
        step = AdvancePreparation(step.state, InfoReadFailed{});
        EXPECT_FALSE(std::holds_alternative<WriteCue>(step.action));
        if (IsRetired(step.state)) break;
    }
    ASSERT_TRUE(IsRetired(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::InfoUnavailable);
}

TEST(BeBoBBootloaderPreparationTests, GenerationChangeRetiresWithoutWriting) {
    // From every non-terminal state, an invalidated generation must stop the
    // machine and must never be answered with a write.
    const std::vector<PreparationState> states{
        ReadingInfo{0},
        EvaluatingInfo{MakeInfo(1, 1)},
        AwaitingResponse{1, 0},
    };
    for (const auto& state : states) {
        const auto step = AdvancePreparation(state, GenerationInvalidated{});
        EXPECT_FALSE(std::holds_alternative<WriteCue>(step.action));
        ASSERT_TRUE(IsRetired(step.state));
        EXPECT_EQ(std::get<Retired>(step.state).reason,
                  RetireReason::GenerationChanged);
    }
}

TEST(BeBoBBootloaderPreparationTests, FailedWriteRetiresRatherThanRetrying) {
    auto step = BeginPreparation();
    step = AdvancePreparation(step.state, InfoReadSucceeded{MakeInfo(1, 1)});
    step = AdvancePreparation(step.state, InfoReadSucceeded{MakeInfo(1, 1)});
    step = AdvancePreparation(step.state, CueWriteFailed{});

    ASSERT_TRUE(IsRetired(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::CueWriteFailed);
    EXPECT_FALSE(std::holds_alternative<WriteCue>(step.action));
}

TEST(BeBoBBootloaderPreparationTests, RetiredMachineIsInert) {
    const PreparationState retired = Retired{RetireReason::Cued};
    for (const PreparationEvent& event :
         std::vector<PreparationEvent>{InfoReadSucceeded{MakeInfo(1, 1)},
                                       CueWriteSucceeded{}, ResponseReadFailed{},
                                       GenerationInvalidated{}}) {
        const auto step = AdvancePreparation(retired, event);
        EXPECT_FALSE(std::holds_alternative<WriteCue>(step.action));
        EXPECT_TRUE(IsRetired(step.state));
    }
}

} // namespace
