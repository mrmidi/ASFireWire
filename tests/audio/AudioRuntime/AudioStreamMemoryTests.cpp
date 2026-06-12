#include "Audio/DriverKit/Runtime/AudioStreamMemory.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

using ASFW::Audio::Runtime::AudioStreamMemory;

TEST(AudioStreamMemoryTests, InputFrameWrapsByFrameCapacityAndUsesChannelStride) {
    std::array<int32_t, 8> input{};

    const AudioStreamMemory memory{
        .inputBase = input.data(),
        .inputFrameCapacity = 4,
        .inputChannels = 2,
    };

    EXPECT_EQ(memory.InputFrame(0), input.data());
    EXPECT_EQ(memory.InputFrame(1), input.data() + 2);
    EXPECT_EQ(memory.InputFrame(3), input.data() + 6);
    EXPECT_EQ(memory.InputFrame(4), input.data());
}

TEST(AudioStreamMemoryTests, OutputFrameWrapsByFrameCapacityAndUsesChannelStride) {
    std::array<float, 12> output{};

    const AudioStreamMemory memory{
        .outputBase = output.data(),
        .outputFrameCapacity = 4,
        .outputChannels = 3,
    };

    EXPECT_EQ(memory.OutputFrame(0), output.data());
    EXPECT_EQ(memory.OutputFrame(1), output.data() + 3);
    EXPECT_EQ(memory.OutputFrame(3), output.data() + 9);
    EXPECT_EQ(memory.OutputFrame(4), output.data());
}

TEST(AudioStreamMemoryTests, MissingBuffersReturnNullAndReportInvalidDirections) {
    const AudioStreamMemory memory{};

    EXPECT_FALSE(memory.HasInput());
    EXPECT_FALSE(memory.HasOutput());
    EXPECT_FALSE(memory.IsValid());
    EXPECT_EQ(memory.InputFrame(0), nullptr);
    EXPECT_EQ(memory.OutputFrame(0), nullptr);
}

TEST(AudioStreamMemoryTests, MemoryIsValidWithEitherDirection) {
    std::array<int32_t, 2> input{};
    std::array<float, 2> output{};

    const AudioStreamMemory inputOnly{
        .inputBase = input.data(),
        .inputFrameCapacity = 1,
        .inputChannels = 2,
    };
    const AudioStreamMemory outputOnly{
        .outputBase = output.data(),
        .outputFrameCapacity = 1,
        .outputChannels = 2,
    };

    EXPECT_TRUE(inputOnly.IsValid());
    EXPECT_TRUE(outputOnly.IsValid());
}

} // namespace
