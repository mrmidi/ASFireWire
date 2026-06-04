#include <gtest/gtest.h>

#include "../ASFWDriver/AudioWire/AMDTP/SYTGenerator.hpp"

namespace {
constexpr int32_t kTickDomain = 16 * 3072;
constexpr uint16_t kSeedSyt = 0xD8B0;

int32_t TickIndex(uint16_t syt) {
    constexpr int32_t kTicksPerCycle = 3072;
    return (static_cast<int32_t>((syt >> 12) & 0x0F) * kTicksPerCycle) +
           static_cast<int32_t>(syt & 0x0FFF);
}

int32_t WrapSigned(int32_t ticks) {
    constexpr int32_t half = kTickDomain / 2;
    int32_t wrapped = ticks % kTickDomain;
    if (wrapped >= half) {
        wrapped -= kTickDomain;
    } else if (wrapped < -half) {
        wrapped += kTickDomain;
    }
    return wrapped;
}
} // namespace

TEST(SYTGenerator, AnchorUsesAcceptedPresentationLeadCycle978And979) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);
    gen.armTransmitCycleAnchor();

    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/978, /*samplesInPacket=*/8), 0x3400);
    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/979, /*samplesInPacket=*/8), 0x4800);
}

TEST(SYTGenerator, AnchorIsConsumedByFirstDataPacketOnly) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);
    gen.armTransmitCycleAnchor();

    const uint16_t first = gen.computeDataSYT(/*transmitCycle=*/978, /*samplesInPacket=*/8);
    EXPECT_EQ(first, 0x3400);

    // Subsequent DATA packets free-run in sample time; arbitrary bus-cycle jumps do not re-anchor.
    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/4321, /*samplesInPacket=*/8), 0x4800);
}

TEST(SYTGenerator, AnchorOverridesPriorRxSeed) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);
    gen.seedFromRxSyt(kSeedSyt);
    gen.armTransmitCycleAnchor();

    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/978, /*samplesInPacket=*/8), 0x3400);
}

TEST(SYTGenerator, LowFourCycleBitsDetermineEncodedAnchorPhase) {
    ASFW::Encoding::SYTGenerator a;
    a.initialize(48000.0);
    a.armTransmitCycleAnchor();

    ASFW::Encoding::SYTGenerator b;
    b.initialize(48000.0);
    b.armTransmitCycleAnchor();

    EXPECT_EQ(a.computeDataSYT(/*transmitCycle=*/978, /*samplesInPacket=*/8),
              b.computeDataSYT(/*transmitCycle=*/978 + 16 * 50, /*samplesInPacket=*/8));
}

TEST(SYTGenerator, ReturnsNoInfoUntilSeededOrAnchored) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);

    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/0, /*samplesInPacket=*/8),
              ASFW::Encoding::SYTGenerator::kNoInfo);
}

TEST(SYTGenerator, RxSeedPathStillAdvancesByPacketStep) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);
    gen.seedFromRxSyt(kSeedSyt);

    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/5151, /*samplesInPacket=*/8), 0xD8B0);
    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/5152, /*samplesInPacket=*/8), 0xF0B0);
}

TEST(SYTGenerator, RxSeedOverridesPriorTransmitCycleAnchor) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);
    gen.armTransmitCycleAnchor();
    gen.seedFromRxSyt(kSeedSyt);

    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/978, /*samplesInPacket=*/8), 0xD8B0);
    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/979, /*samplesInPacket=*/8), 0xF0B0);
}

TEST(SYTGenerator, NudgePositiveAndNegativeTicks) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);

    gen.reset();
    gen.armTransmitCycleAnchor();
    const int32_t base = TickIndex(gen.computeDataSYT(978, 8));

    gen.reset();
    gen.armTransmitCycleAnchor();
    (void)gen.computeDataSYT(978, 8);
    gen.nudgeOffsetTicks(+1);
    const int32_t plusOne = TickIndex(gen.computeDataSYT(979, 8));

    gen.reset();
    gen.armTransmitCycleAnchor();
    (void)gen.computeDataSYT(978, 8);
    gen.nudgeOffsetTicks(-1);
    const int32_t minusOne = TickIndex(gen.computeDataSYT(979, 8));

    EXPECT_EQ(WrapSigned(plusOne - (base + 4096)), +1);
    EXPECT_EQ(WrapSigned(minusOne - (base + 4096)), -1);
}
