#include <gtest/gtest.h>

#include "../ASFWDriver/Isoch/Encoding/SYTGenerator.hpp"

namespace {
constexpr int32_t kTickDomain = 16 * 3072;

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

TEST(SYTGenerator, NudgePositiveAndNegativeTicks) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);

    gen.reset();
    const int32_t base = TickIndex(gen.computeDataSYT(0));

    gen.reset();
    gen.nudgeOffsetTicks(+1);
    const int32_t plusOne = TickIndex(gen.computeDataSYT(0));
    EXPECT_EQ(WrapSigned(plusOne - base), +1);

    gen.reset();
    gen.nudgeOffsetTicks(-1);
    const int32_t minusOne = TickIndex(gen.computeDataSYT(0));
    EXPECT_EQ(WrapSigned(minusOne - base), -1);
}

TEST(SYTGenerator, NudgeWrapBehaviorAcrossDomain) {
    ASFW::Encoding::SYTGenerator gen;
    gen.initialize(48000.0);

    gen.reset();
    const int32_t base = TickIndex(gen.computeDataSYT(0));

    gen.reset();
    gen.nudgeOffsetTicks(kTickDomain + 3);
    const int32_t plusWrapped = TickIndex(gen.computeDataSYT(0));
    EXPECT_EQ(WrapSigned(plusWrapped - base), +3);

    gen.reset();
    gen.nudgeOffsetTicks(-(kTickDomain + 5));
    const int32_t minusWrapped = TickIndex(gen.computeDataSYT(0));
    EXPECT_EQ(WrapSigned(minusWrapped - base), -5);
}
