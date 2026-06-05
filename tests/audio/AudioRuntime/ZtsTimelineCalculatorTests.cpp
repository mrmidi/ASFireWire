#include "Audio/Runtime/ZtsTimelineCalculator.hpp"
#include <gtest/gtest.h>

namespace ASFW::Tests::AudioRuntime {

using ASFW::Audio::Runtime::ZtsTimelineCalculator;

TEST(ZtsTimelineCalculatorTests, ZtsMirror_FirstPublication_UsesCeilAlignment_NotPastFrame) {
    ASSERT_TRUE(ASFW::Timing::initializeHostTimebase());

    const uint64_t eventSampleFrame = 152;
    const uint64_t eventHostTicks = 5528585408482ULL;
    const uint32_t period = 512;
    const uint32_t eventHostNanosPerSampleQ8 = 256; // 1 sample = 1 ns

    const uint64_t firstPublishedFrame = ZtsTimelineCalculator::AlignZtsStart(eventSampleFrame, period);
    
    // Assert ceil alignment
    EXPECT_EQ(firstPublishedFrame, 512U);
    EXPECT_GE(firstPublishedFrame, eventSampleFrame);

    const uint64_t firstPublishedHostTicks = ZtsTimelineCalculator::CalculateTargetHostTicks(
        firstPublishedFrame,
        eventSampleFrame,
        eventHostTicks,
        eventHostNanosPerSampleQ8
    );

    EXPECT_GT(firstPublishedHostTicks, eventHostTicks);
}

TEST(ZtsTimelineCalculatorTests, ZtsMirror_RejectsNonMonotonicPublishedHostTicks) {
    ASSERT_TRUE(ASFW::Timing::initializeHostTimebase());

    const uint64_t eventSampleFrame = 152;
    const uint64_t eventHostTicks = 5528585408482ULL;
    const uint32_t eventHostNanosPerSampleQ8 = 256;

    const uint64_t host512 = ZtsTimelineCalculator::CalculateTargetHostTicks(
        512, eventSampleFrame, eventHostTicks, eventHostNanosPerSampleQ8
    );
    const uint64_t host1024 = ZtsTimelineCalculator::CalculateTargetHostTicks(
        1024, eventSampleFrame, eventHostTicks, eventHostNanosPerSampleQ8
    );

    EXPECT_GT(host1024, host512);
}

} // namespace ASFW::Tests::AudioRuntime
