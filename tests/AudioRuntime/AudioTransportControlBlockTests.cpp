#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>

namespace {

using ASFW::Audio::Runtime::AudioTransportControlBlock;

TEST(AudioTransportControlBlockTests, ResetForStartClearsNestedStateAndIncrementsGeneration) {
    AudioTransportControlBlock control{};

    control.generation.store(41, std::memory_order_relaxed);
    control.client.PublishBeginRead(2000, 456, 64);
    control.client.PublishWriteEnd(1000, 123, 128);
    control.device.Publish(3000, 789, 55);
    control.counters.CountBeginRead();
    control.counters.CountWriteEnd();
    control.counters.CountZtsPublished();
    control.counters.txPackets.store(9, std::memory_order_relaxed);
    control.counters.rxPackets.store(7, std::memory_order_relaxed);
    control.inputProducedEndFrame.store(111, std::memory_order_relaxed);
    control.outputConsumedEndFrame.store(222, std::memory_order_relaxed);
    control.inputOverruns.store(3, std::memory_order_relaxed);
    control.outputUnderruns.store(4, std::memory_order_relaxed);
    control.discontinuities.store(5, std::memory_order_relaxed);

    control.ResetForStart();

    EXPECT_EQ(control.generation.load(std::memory_order_acquire), 42U);

    EXPECT_EQ(control.client.inputBeginReadSampleFrame.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.inputBeginReadHostTicks.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.inputBeginReadFrames.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.InputReadEndFrame(), 0U);
    EXPECT_EQ(control.client.outputWriteEndSampleFrame.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.outputWriteEndHostTicks.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.outputWriteEndFrames.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.client.OutputWrittenEndFrame(), 0U);

    EXPECT_EQ(control.device.sampleFrame.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.device.hostTicks.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.device.hostNanosPerSampleQ8.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.device.generation.load(std::memory_order_acquire), 0U);

    EXPECT_EQ(control.counters.ioBeginReadCount.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.ioWriteEndCount.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txPackets.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.rxPackets.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.ztsPublished.load(std::memory_order_relaxed), 0U);

    EXPECT_EQ(control.inputProducedEndFrame.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.outputConsumedEndFrame.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.inputOverruns.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.outputUnderruns.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.discontinuities.load(std::memory_order_acquire), 0U);
}

} // namespace
