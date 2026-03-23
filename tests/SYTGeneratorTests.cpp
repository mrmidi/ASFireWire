#include <gtest/gtest.h>

#include "../ASFWDriver/Isoch/Encoding/SYTGenerator.hpp"

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

TEST(SYTGenerator, FirstDataPacketUsesSeededRxSytExactly) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);
    gen.seedFromRxSyt(kSeedSyt);

    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/481, /*samplesInPacket=*/8), kSeedSyt);
}

TEST(SYTGenerator, AdvancesBy4096PerDataPacketIndependentOfBusCycleGaps) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);
    gen.seedFromRxSyt(kSeedSyt);

    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/5151, /*samplesInPacket=*/8), 0xD8B0);
    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/5152, /*samplesInPacket=*/8), 0xF0B0);
    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/5154, /*samplesInPacket=*/8), 0x04B0);
    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/5155, /*samplesInPacket=*/8), 0x18B0);
}

TEST(SYTGenerator, ReturnsNoInfoUntilSeeded) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);

    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/0, /*samplesInPacket=*/8),
              ASFW::Encoding::SYTGenerator::kNoInfo);
}

TEST(SYTGenerator, NudgePositiveAndNegativeTicks) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);

    gen.reset();
    gen.seedFromRxSyt(kSeedSyt);
    const int32_t base = TickIndex(gen.computeDataSYT(0, 8));

    gen.reset();
    gen.seedFromRxSyt(kSeedSyt);
    gen.nudgeOffsetTicks(+1);
    const int32_t plusOne = TickIndex(gen.computeDataSYT(0, 8));
    EXPECT_EQ(WrapSigned(plusOne - base), +1);

    gen.reset();
    gen.seedFromRxSyt(kSeedSyt);
    gen.nudgeOffsetTicks(-1);
    const int32_t minusOne = TickIndex(gen.computeDataSYT(0, 8));
    EXPECT_EQ(WrapSigned(minusOne - base), -1);
}

TEST(SYTGenerator, WrapsAcrossSixteenCycleDomainByPacketStep) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);
    gen.seedFromRxSyt(0xFB00);

    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/0, /*samplesInPacket=*/8), 0xFB00);
    EXPECT_EQ(gen.computeDataSYT(/*transmitCycle=*/7, /*samplesInPacket=*/8), 0x1300);
}
