#include <gtest/gtest.h>

#include "Isoch/Audio/AudioIOPath.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using ASFW::Isoch::Audio::AudioIOPathState;
using ASFW::Isoch::Audio::HandleIOOperation;
using ASFW::Isoch::Audio::ZeroCopyTimelineState;
using ASFW::Shared::TxSharedQueueSPSC;

struct QueueFixture {
    std::vector<std::byte> backing;
    TxSharedQueueSPSC queue;
    bool ok{false};

    explicit QueueFixture(uint32_t capacityFrames, uint32_t channels) {
        const uint64_t bytes = TxSharedQueueSPSC::RequiredBytes(capacityFrames, channels);
        backing.resize(bytes);
        const bool initialized = TxSharedQueueSPSC::InitializeInPlace(backing.data(),
                                                                      backing.size(),
                                                                      capacityFrames,
                                                                      channels);
        const bool attached = initialized && queue.Attach(backing.data(), backing.size());
        ok = initialized && attached;
    }
};

IOBufferMemoryDescriptor* CreateAudioBuffer(uint32_t frames, uint32_t channels) {
    IOBufferMemoryDescriptor* buffer = nullptr;
    const uint64_t bytes = static_cast<uint64_t>(frames) * channels * sizeof(int32_t);
    if (IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, bytes, 16, &buffer) != kIOReturnSuccess) {
        return nullptr;
    }
    return buffer;
}

int32_t* BufferPtr(IOBufferMemoryDescriptor* buffer) {
    IOAddressSegment range{};
    if (!buffer || buffer->GetAddressRange(&range) != kIOReturnSuccess || range.address == 0) {
        return nullptr;
    }
    return reinterpret_cast<int32_t*>(range.address);
}

TEST(AudioIOPathTests, BeginReadWithoutRxQueueWritesSilenceToWindow) {
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kPeriodFrames = 8;
    constexpr uint32_t kReadFrames = 4;

    IOBufferMemoryDescriptor* inputBuffer = CreateAudioBuffer(kPeriodFrames, kChannels);
    ASSERT_NE(inputBuffer, nullptr);

    int32_t* samples = BufferPtr(inputBuffer);
    ASSERT_NE(samples, nullptr);
    std::memset(samples, 0x5A, kPeriodFrames * kChannels * sizeof(int32_t));

    bool startupDrained = false;
    AudioIOPathState state{
        .inputBuffer = inputBuffer,
        .inputChannelCount = kChannels,
        .ioBufferPeriodFrames = kPeriodFrames,
        .rxStartupDrained = &startupDrained,
        .rxQueueValid = false,
        .rxQueueReader = nullptr,
    };

    ASSERT_EQ(HandleIOOperation(state, IOUserAudioIOOperationBeginRead, kReadFrames, 2), kIOReturnSuccess);

    for (uint32_t frame = 0; frame < kPeriodFrames; ++frame) {
        const bool touched = (frame >= 2 && frame < 6);
        for (uint32_t ch = 0; ch < kChannels; ++ch) {
            const int32_t value = samples[frame * kChannels + ch];
            if (touched) {
                EXPECT_EQ(value, 0);
            }
        }
    }
}

TEST(AudioIOPathTests, BeginReadWrapsAndZeroPadsOnPartialQueueRead) {
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kPeriodFrames = 8;

    QueueFixture rxQueue(32, kChannels);
    ASSERT_TRUE(rxQueue.ok);
    const std::array<int32_t, 4> twoFrames = {101, 102, 201, 202};
    ASSERT_EQ(rxQueue.queue.Write(twoFrames.data(), 2), 2u);

    IOBufferMemoryDescriptor* inputBuffer = CreateAudioBuffer(kPeriodFrames, kChannels);
    ASSERT_NE(inputBuffer, nullptr);
    int32_t* samples = BufferPtr(inputBuffer);
    ASSERT_NE(samples, nullptr);
    std::memset(samples, 0x11, kPeriodFrames * kChannels * sizeof(int32_t));

    bool startupDrained = false;
    AudioIOPathState state{
        .inputBuffer = inputBuffer,
        .inputChannelCount = kChannels,
        .ioBufferPeriodFrames = kPeriodFrames,
        .rxStartupDrained = &startupDrained,
        .rxQueueValid = true,
        .rxQueueReader = &rxQueue.queue,
    };

    ASSERT_EQ(HandleIOOperation(state, IOUserAudioIOOperationBeginRead, 4, 6), kIOReturnSuccess);
    EXPECT_TRUE(startupDrained);

    EXPECT_EQ(samples[6 * kChannels + 0], 101);
    EXPECT_EQ(samples[6 * kChannels + 1], 102);
    EXPECT_EQ(samples[7 * kChannels + 0], 201);
    EXPECT_EQ(samples[7 * kChannels + 1], 202);

    EXPECT_EQ(samples[0], 0);
    EXPECT_EQ(samples[1], 0);
    EXPECT_EQ(samples[2], 0);
    EXPECT_EQ(samples[3], 0);
}

TEST(AudioIOPathTests, WriteEndUsesPacketAssemblerWhenTxQueueUnavailable) {
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kPeriodFrames = 8;

    IOBufferMemoryDescriptor* outputBuffer = CreateAudioBuffer(kPeriodFrames, kChannels);
    ASSERT_NE(outputBuffer, nullptr);
    int32_t* samples = BufferPtr(outputBuffer);
    ASSERT_NE(samples, nullptr);

    const std::array<int32_t, 8> fourFrames = {
        11, 12, 21, 22, 31, 32, 41, 42
    };
    std::memcpy(samples, fourFrames.data(), fourFrames.size() * sizeof(int32_t));

    ASFW::Encoding::PacketAssembler assembler(kChannels, 0);
    uint64_t overruns = 0;
    AudioIOPathState state{
        .outputBuffer = outputBuffer,
        .outputChannelCount = kChannels,
        .ioBufferPeriodFrames = kPeriodFrames,
        .txQueueValid = false,
        .packetAssembler = &assembler,
        .encodingOverruns = &overruns,
    };

    ASSERT_EQ(HandleIOOperation(state, IOUserAudioIOOperationWriteEnd, 4, 0), kIOReturnSuccess);
    EXPECT_EQ(assembler.bufferFillLevel(), 4u);
    EXPECT_EQ(overruns, 0u);

    std::array<int32_t, 8> readBack{};
    ASSERT_EQ(assembler.ringBuffer().read(readBack.data(), 4), 4u);
    EXPECT_EQ(readBack, fourFrames);
}

TEST(AudioIOPathTests, WriteEndWithTxQueueWrapWritesFirstThenSecondSpan) {
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kPeriodFrames = 8;

    QueueFixture txQueue(32, kChannels);
    ASSERT_TRUE(txQueue.ok);

    IOBufferMemoryDescriptor* outputBuffer = CreateAudioBuffer(kPeriodFrames, kChannels);
    ASSERT_NE(outputBuffer, nullptr);
    int32_t* samples = BufferPtr(outputBuffer);
    ASSERT_NE(samples, nullptr);

    for (uint32_t frame = 0; frame < kPeriodFrames; ++frame) {
        samples[frame * kChannels + 0] = static_cast<int32_t>(frame * 10 + 1);
        samples[frame * kChannels + 1] = static_cast<int32_t>(frame * 10 + 2);
    }

    uint64_t overruns = 0;
    AudioIOPathState state{
        .outputBuffer = outputBuffer,
        .outputChannelCount = kChannels,
        .ioBufferPeriodFrames = kPeriodFrames,
        .txQueueValid = true,
        .txQueueWriter = &txQueue.queue,
        .zeroCopyEnabled = false,
        .encodingOverruns = &overruns,
    };

    ASSERT_EQ(HandleIOOperation(state, IOUserAudioIOOperationWriteEnd, 4, 6), kIOReturnSuccess);
    EXPECT_EQ(overruns, 0u);

    std::array<int32_t, 8> readBack{};
    ASSERT_EQ(txQueue.queue.Read(readBack.data(), 4), 4u);

    const std::array<int32_t, 8> expected = {
        61, 62, 71, 72, 1, 2, 11, 12
    };
    EXPECT_EQ(readBack, expected);
}

TEST(AudioIOPathTests, ZeroCopyPublishTracksDiscontinuityAndPhaseRebase) {
    constexpr uint32_t kChannels = 2;
    constexpr uint32_t kPeriodFrames = 8;

    QueueFixture txQueue(16, kChannels);
    ASSERT_TRUE(txQueue.ok);

    IOBufferMemoryDescriptor* outputBuffer = CreateAudioBuffer(kPeriodFrames, kChannels);
    ASSERT_NE(outputBuffer, nullptr);

    ZeroCopyTimelineState timeline{};
    uint64_t overruns = 0;
    AudioIOPathState state{
        .outputBuffer = outputBuffer,
        .outputChannelCount = kChannels,
        .ioBufferPeriodFrames = kPeriodFrames,
        .txQueueValid = true,
        .txQueueWriter = &txQueue.queue,
        .zeroCopyEnabled = true,
        .zeroCopyFrameCapacity = 8,
        .zeroCopyTimeline = &timeline,
        .encodingOverruns = &overruns,
    };

    ASSERT_EQ(HandleIOOperation(state, IOUserAudioIOOperationWriteEnd, 4, 4), kIOReturnSuccess);
    EXPECT_TRUE(timeline.valid);
    EXPECT_EQ(timeline.discontinuities, 0u);
    EXPECT_EQ(timeline.phaseFrames, 4u);
    EXPECT_EQ(timeline.publishedSampleTime, 8u);

    ASSERT_EQ(HandleIOOperation(state, IOUserAudioIOOperationWriteEnd, 4, 2), kIOReturnSuccess);
    EXPECT_EQ(timeline.discontinuities, 1u);
    EXPECT_EQ(timeline.phaseFrames, 6u);
    EXPECT_EQ(timeline.publishedSampleTime, 6u);
    EXPECT_EQ(overruns, 0u);
}

} // namespace
