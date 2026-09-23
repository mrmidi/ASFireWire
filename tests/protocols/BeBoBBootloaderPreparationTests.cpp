#include <gtest/gtest.h>
#include "ASFWDriver/Protocols/BeBoB/Bootloader/BeBoBBootloaderPreparation.hpp"

#include <algorithm>
#include <array>

namespace {
using namespace ASFW::Protocols::BeBoB::Bootloader;

void StoreLE32(std::array<uint8_t, kInfoBlockBytes>& b, size_t p, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) b[p + i] = static_cast<uint8_t>(v >> (8U * i));
}
BootRomInfo Info(uint32_t protocol, uint32_t loader, uint64_t date) {
    BootRomInfo info{};
    StoreLE32(info.raw, kProtocolVersionOffset, protocol);
    StoreLE32(info.raw, kBootloaderVersionOffset, loader);
    for (size_t i = 0; i < 8; ++i)
        info.raw[kSoftwareDateOffset + i] = static_cast<uint8_t>(date >> (8U * i));
    return info;
}

TEST(BeBoBBootloaderPreparation, EncodesTheSinglePermittedCueLittleEndian) {
    const BeBoBBootloaderCue cue{Info(0x01020304, 1, 0x3230303730343031ULL)};
    const std::array<uint8_t, 12> expected{
        0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x11, 0x01, 0, 0, 0, 0};
    EXPECT_TRUE(std::equal(cue.Bytes().begin(), cue.Bytes().end(), expected.begin()));
    EXPECT_TRUE(IsPermittedBootloaderWrite(kAddressHi, kRequestAddressLo, cue.Bytes()));
    EXPECT_FALSE(IsPermittedBootloaderWrite(kAddressHi, kInfoAddressLo, cue.Bytes()));
}

TEST(BeBoBBootloaderPreparation, RequiresActiveBootloaderAndSupportedBuildDate) {
    auto state = BeginPreparation().state;
    auto step = AdvancePreparation(state, InfoReadSucceeded{Info(1, 0, 0x3230303730343031ULL)});
    ASSERT_TRUE(std::holds_alternative<Retired>(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::FirmwareAlreadyRunning);

    step = AdvancePreparation(ReadingInfo{}, InfoReadSucceeded{Info(1, 1, 0x3230303730343030ULL)});
    ASSERT_TRUE(std::holds_alternative<Retired>(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::UnsupportedBuild);
}

TEST(BeBoBBootloaderPreparation, ReadsOnceThenCuesOnceAndWaitsForReenumeration) {
    auto step = BeginPreparation();
    ASSERT_TRUE(std::holds_alternative<ReadInfoBlock>(step.action));
    step = AdvancePreparation(step.state,
        InfoReadSucceeded{Info(0x1234, 1, 0x3230303730343031ULL)});
    ASSERT_TRUE(std::holds_alternative<WriteCue>(step.action));
    ASSERT_TRUE(std::holds_alternative<AwaitingReenumeration>(step.state));
    EXPECT_EQ(std::get<WriteCue>(step.action).cue.ProtocolVersion(), 0x1234U);
    step = AdvancePreparation(step.state, CueWriteSucceeded{});
    ASSERT_TRUE(std::holds_alternative<Done>(step.action));
    EXPECT_TRUE(std::holds_alternative<AwaitingReenumeration>(step.state));
    step = AdvancePreparation(step.state, CueWriteSucceeded{});
    EXPECT_TRUE(std::holds_alternative<Done>(step.action));
    EXPECT_TRUE(std::holds_alternative<AwaitingReenumeration>(step.state));
    step = AdvancePreparation(step.state, GenerationInvalidated{});
    ASSERT_TRUE(std::holds_alternative<Retired>(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::GenerationChanged);
}

TEST(BeBoBBootloaderPreparation, FailedInfoReadsAreBoundedAndGenerationChangeStopsWork) {
    auto state = BeginPreparation().state;
    for (uint8_t i = 1; i < kMaxInfoReadAttempts; ++i) {
        auto step = AdvancePreparation(state, InfoReadFailed{});
        EXPECT_TRUE(std::holds_alternative<ReadInfoBlock>(step.action));
        state = step.state;
    }
    auto step = AdvancePreparation(state, InfoReadFailed{});
    ASSERT_TRUE(std::holds_alternative<Retired>(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::InfoUnavailable);

    step = AdvancePreparation(BeginPreparation().state, GenerationInvalidated{});
    ASSERT_TRUE(std::holds_alternative<Retired>(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::GenerationChanged);
}
} // namespace
