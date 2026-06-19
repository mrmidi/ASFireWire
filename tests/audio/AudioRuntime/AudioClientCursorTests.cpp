#include "Audio/DriverKit/Runtime/AudioClientCursor.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

namespace {

using ASFW::Audio::Runtime::AudioClientCursor;

TEST(AudioClientCursorTests, PublishWriteEndStoresOutputEndFrame) {
    AudioClientCursor cursor{};

    cursor.PublishWriteEnd(1000, 123, 128);

    EXPECT_EQ(cursor.outputWriteEndSampleFrame.load(std::memory_order_relaxed), 1000U);
    EXPECT_EQ(cursor.outputWriteEndHostTicks.load(std::memory_order_relaxed), 123U);
    EXPECT_EQ(cursor.outputWriteEndFrames.load(std::memory_order_relaxed), 128U);
    EXPECT_EQ(cursor.OutputWrittenEndFrame(), 1128U);
}

TEST(AudioClientCursorTests, PublishBeginReadStoresInputEndFrame) {
    AudioClientCursor cursor{};

    cursor.PublishBeginRead(2000, 456, 64);

    EXPECT_EQ(cursor.inputBeginReadSampleFrame.load(std::memory_order_relaxed), 2000U);
    EXPECT_EQ(cursor.inputBeginReadHostTicks.load(std::memory_order_relaxed), 456U);
    EXPECT_EQ(cursor.inputBeginReadFrames.load(std::memory_order_relaxed), 64U);
    EXPECT_EQ(cursor.InputReadEndFrame(), 2064U);
}

} // namespace
