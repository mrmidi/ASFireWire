#include "Audio/Runtime/AudioSampleRing.hpp"

#include <gtest/gtest.h>

#include <array>

using ASFW::Audio::Runtime::AudioSampleRing;

TEST(AudioSampleRingTests, WritesAndReadsAbsoluteFrames) {
    AudioSampleRing ring;
    ASSERT_EQ(ring.Configure(8, 2), kIOReturnSuccess);

    const std::array<int32_t, 4> in{10, 11, 20, 21};
    const auto write = ring.WriteFrames(100, in.data(), 2, 2);
    EXPECT_FALSE(write.invalid);
    EXPECT_EQ(write.copiedFrames, 2U);
    EXPECT_EQ(ring.WriteFrame(), 102U);

    std::array<int32_t, 4> out{};
    const auto read = ring.ReadFrames(100, out.data(), 2, 2);
    EXPECT_FALSE(read.invalid);
    EXPECT_FALSE(read.underrun);
    EXPECT_EQ(read.copiedFrames, 2U);
    EXPECT_EQ(out, in);
    EXPECT_EQ(ring.ReadFrame(), 102U);
}

TEST(AudioSampleRingTests, WrapsByAbsoluteFrame) {
    AudioSampleRing ring;
    ASSERT_EQ(ring.Configure(4, 2), kIOReturnSuccess);

    const std::array<int32_t, 8> in{1, 2, 3, 4, 5, 6, 7, 8};
    EXPECT_EQ(ring.WriteFrames(6, in.data(), 4, 2).copiedFrames, 4U);

    std::array<int32_t, 8> out{};
    const auto read = ring.ReadFrames(6, out.data(), 4, 2);
    EXPECT_FALSE(read.underrun);
    EXPECT_EQ(out, in);
}

TEST(AudioSampleRingTests, MissingFramesZeroFillAndCountUnderrun) {
    AudioSampleRing ring;
    ASSERT_EQ(ring.Configure(8, 2), kIOReturnSuccess);

    const std::array<int32_t, 4> in{10, 11, 20, 21};
    EXPECT_EQ(ring.WriteFrames(4, in.data(), 2, 2).copiedFrames, 2U);

    std::array<int32_t, 8> out;
    out.fill(-1);
    const auto read = ring.ReadFrames(4, out.data(), 4, 2);
    EXPECT_TRUE(read.underrun);
    EXPECT_TRUE(read.starvation);
    EXPECT_EQ(read.copiedFrames, 2U);
    EXPECT_EQ(out[0], 10);
    EXPECT_EQ(out[1], 11);
    EXPECT_EQ(out[2], 20);
    EXPECT_EQ(out[3], 21);
    EXPECT_EQ(out[4], 0);
    EXPECT_EQ(out[5], 0);
    EXPECT_EQ(out[6], 0);
    EXPECT_EQ(out[7], 0);
}

TEST(AudioSampleRingTests, OverrunAdvancesReadCursor) {
    AudioSampleRing ring;
    ASSERT_EQ(ring.Configure(4, 1), kIOReturnSuccess);

    std::array<int32_t, 6> in{1, 2, 3, 4, 5, 6};
    const auto write = ring.WriteFrames(0, in.data(), 6, 1);
    EXPECT_TRUE(write.overrun);
    EXPECT_EQ(write.copiedFrames, 6U);
    EXPECT_EQ(ring.WriteFrame(), 6U);
    EXPECT_EQ(ring.ReadFrame(), 2U);

    std::array<int32_t, 4> out{};
    const auto read = ring.ReadFrames(2, out.data(), 4, 1);
    EXPECT_FALSE(read.underrun);
    EXPECT_EQ(out[0], 3);
    EXPECT_EQ(out[1], 4);
    EXPECT_EQ(out[2], 5);
    EXPECT_EQ(out[3], 6);
}

TEST(AudioSampleRingTests, ExternalStorageUsesExternalCursors) {
    std::array<int32_t, 8> storage{};
    std::atomic<uint64_t> write{0};
    std::atomic<uint64_t> read{0};

    AudioSampleRing ring;
    ASSERT_EQ(ring.BindExternal(storage.data(), 4, 2, &write, &read), kIOReturnSuccess);

    const std::array<int32_t, 4> in{7, 8, 9, 10};
    EXPECT_EQ(ring.WriteFrames(10, in.data(), 2, 2).copiedFrames, 2U);
    EXPECT_EQ(write.load(std::memory_order_acquire), 12U);

    std::array<int32_t, 4> out{};
    EXPECT_EQ(ring.ReadFrames(10, out.data(), 2, 2).copiedFrames, 2U);
    EXPECT_EQ(read.load(std::memory_order_acquire), 12U);
    EXPECT_EQ(out, in);
}

TEST(AudioSampleRingTests, ResetClearsStorageAndCursors) {
    AudioSampleRing ring;
    ASSERT_EQ(ring.Configure(4, 1), kIOReturnSuccess);

    std::array<int32_t, 2> in{42, 43};
    EXPECT_EQ(ring.WriteFrames(20, in.data(), 2, 1).copiedFrames, 2U);
    ring.Reset(100);

    EXPECT_EQ(ring.WriteFrame(), 100U);
    EXPECT_EQ(ring.ReadFrame(), 100U);
    std::array<int32_t, 1> out{-1};
    const auto read = ring.ReadFrames(100, out.data(), 1, 1);
    EXPECT_TRUE(read.underrun);
    EXPECT_EQ(out[0], 0);
}
