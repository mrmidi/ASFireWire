#include <gtest/gtest.h>

#include "Testing/HostDriverKitStubs.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Isoch/Encoding/TimingUtils.hpp"
#include "Isoch/Receive/IsochAudioRxPipeline.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using ASFW::Driver::HardwareInterface;
using ASFW::Isoch::Core::ExternalSyncBridge;
using ASFW::Isoch::Rx::IsochAudioRxPipeline;
using ASFW::Shared::TxSharedQueueSPSC;

class IsochAudioRxPipelineTests : public ::testing::Test {
};

struct QueueFixture {
    std::vector<std::byte> backing;
    TxSharedQueueSPSC queue;
    bool ok{false};

    explicit QueueFixture(uint32_t capacityFrames, uint32_t channels) {
        const uint64_t bytes = TxSharedQueueSPSC::RequiredBytes(capacityFrames, channels);
        backing.resize(bytes);
        auto* queueBytes = reinterpret_cast<uint8_t*>(backing.data());
        const bool initialized = TxSharedQueueSPSC::InitializeInPlace(queueBytes,
                                                                      backing.size(),
                                                                      capacityFrames,
                                                                      channels);
        ok = initialized && queue.Attach(queueBytes, backing.size());
    }
};

uint32_t Be32(uint32_t host) {
    return OSSwapHostToBigInt32(host);
}

std::vector<uint32_t> MakeAmdtpPacket(uint8_t dbs,
                                      uint8_t dbc,
                                      uint32_t eventCount,
                                      uint8_t audioLabel = 0x40,
                                      uint16_t isochDataLength = 0) {
    std::vector<uint32_t> words;
    words.reserve(4 + (static_cast<size_t>(dbs) * eventCount));
    words.push_back(0);
    if (isochDataLength == 0) {
        words.push_back(0);
    } else {
        words.push_back(Be32((static_cast<uint32_t>(isochDataLength) << 16) |
                             (uint32_t{1} << 14) |
                             (uint32_t{0x3F} << 8) |
                             (uint32_t{0x0A} << 4)));
    }
    words.push_back(Be32((uint32_t{0x02} << 24) | (uint32_t{dbs} << 16) | dbc));
    words.push_back(Be32(0x90020000u | 0x1234u));
    for (uint32_t event = 0; event < eventCount; ++event) {
        for (uint8_t ch = 0; ch < dbs; ++ch) {
            const uint32_t sample = (uint32_t{audioLabel} << 24) | ((event + 1u) << 8) | ch;
            words.push_back(Be32(sample));
        }
    }
    return words;
}

TEST_F(IsochAudioRxPipelineTests, TimingLossCallbackFiresOncePerEstablishedToStaleTransition) {
    IsochAudioRxPipeline pipeline;
    ExternalSyncBridge bridge;
    HardwareInterface hardware;
    int callbackCount = 0;

    pipeline.ConfigureFor48k();
    pipeline.SetExternalSyncBridge(&bridge);
    pipeline.SetTimingLossCallback([&callbackCount] { ++callbackCount; });
    pipeline.OnStart();

    const uint64_t nowTicks = mach_absolute_time();
    bridge.clockEstablished.store(true, std::memory_order_release);
    bridge.startupQualified.store(true, std::memory_order_release);
    bridge.lastUpdateHostTicks.store(
        nowTicks - ASFW::Timing::nanosToHostTicks(150'000'000ULL),
        std::memory_order_release);

    pipeline.OnPollEnd(hardware, /*packetsProcessed=*/0, /*pollStartMachTicks=*/nowTicks);
    EXPECT_EQ(callbackCount, 1);
    EXPECT_FALSE(bridge.clockEstablished.load(std::memory_order_acquire));

    pipeline.OnPollEnd(hardware, /*packetsProcessed=*/0, /*pollStartMachTicks=*/nowTicks);
    EXPECT_EQ(callbackCount, 1);
}

TEST_F(IsochAudioRxPipelineTests, StreamProcessorCountsRxQueueProducerDrops) {
    QueueFixture rxQueue(1, 2);
    ASSERT_TRUE(rxQueue.ok);

    ASFW::Isoch::StreamProcessor processor;
    processor.SetOutputSharedQueue(&rxQueue.queue);

    const auto packet = MakeAmdtpPacket(/*dbs=*/2, /*dbc=*/0, /*eventCount=*/2);
    const auto summary = processor.ProcessPacket(
        reinterpret_cast<const uint8_t*>(packet.data()),
        packet.size() * sizeof(uint32_t));

    EXPECT_TRUE(summary.hasValidCip);
    EXPECT_EQ(summary.decodedFrames, 2u);
    EXPECT_EQ(rxQueue.queue.FillLevelFrames(), 1u);
    EXPECT_EQ(processor.RxQueueProducerDropEvents(), 1u);
    EXPECT_EQ(processor.RxQueueProducerDropFrames(), 1u);
}

TEST_F(IsochAudioRxPipelineTests, StreamProcessorAcceptsFullMblaLabelRange) {
    QueueFixture rxQueue(8, 2);
    ASSERT_TRUE(rxQueue.ok);

    ASFW::Isoch::StreamProcessor processor;
    processor.SetOutputSharedQueue(&rxQueue.queue);

    const auto packet = MakeAmdtpPacket(/*dbs=*/2,
                                        /*dbc=*/0,
                                        /*eventCount=*/1,
                                        /*audioLabel=*/0x4f);
    const auto summary = processor.ProcessPacket(
        reinterpret_cast<const uint8_t*>(packet.data()),
        packet.size() * sizeof(uint32_t));

    int32_t readback[2]{};
    EXPECT_TRUE(summary.hasValidCip);
    EXPECT_EQ(summary.decodedFrames, 1u);
    EXPECT_EQ(rxQueue.queue.Read(readback, 1), 1u);
    EXPECT_EQ(readback[0], 0x00000100);
    EXPECT_EQ(readback[1], 0x00000101);
}

TEST_F(IsochAudioRxPipelineTests, StreamProcessorDecodesConfiguredPcmPayloadWithNonMblaLabel) {
    QueueFixture rxQueue(8, 2);
    ASSERT_TRUE(rxQueue.ok);

    ASFW::Isoch::StreamProcessor processor;
    processor.SetOutputSharedQueue(&rxQueue.queue);

    const auto packet = MakeAmdtpPacket(/*dbs=*/2,
                                        /*dbc=*/0,
                                        /*eventCount=*/1,
                                        /*audioLabel=*/0x00);
    const auto summary = processor.ProcessPacket(
        reinterpret_cast<const uint8_t*>(packet.data()),
        packet.size() * sizeof(uint32_t));

    int32_t readback[2]{};
    EXPECT_TRUE(summary.hasValidCip);
    EXPECT_EQ(summary.decodedFrames, 1u);
    EXPECT_EQ(rxQueue.queue.Read(readback, 1), 1u);
    EXPECT_EQ(readback[0], 0x00000100);
    EXPECT_EQ(readback[1], 0x00000101);
    EXPECT_EQ(processor.PcmPayloadFallbackSamples(), 2u);
}

TEST_F(IsochAudioRxPipelineTests, StreamProcessorKeepsMidiLabelledSlotsSilent) {
    QueueFixture rxQueue(8, 2);
    ASSERT_TRUE(rxQueue.ok);

    ASFW::Isoch::StreamProcessor processor;
    processor.SetOutputSharedQueue(&rxQueue.queue);

    const auto packet = MakeAmdtpPacket(/*dbs=*/2,
                                        /*dbc=*/0,
                                        /*eventCount=*/1,
                                        /*audioLabel=*/0x80);
    const auto summary = processor.ProcessPacket(
        reinterpret_cast<const uint8_t*>(packet.data()),
        packet.size() * sizeof(uint32_t));

    int32_t readback[2]{};
    EXPECT_TRUE(summary.hasValidCip);
    EXPECT_EQ(summary.decodedFrames, 1u);
    EXPECT_EQ(rxQueue.queue.Read(readback, 1), 1u);
    EXPECT_EQ(readback[0], 0);
    EXPECT_EQ(readback[1], 0);
    EXPECT_EQ(processor.PcmPayloadFallbackSamples(), 0u);
}

TEST_F(IsochAudioRxPipelineTests, StreamProcessorClampsDecodeToIsochHeaderDataLength) {
    QueueFixture rxQueue(8, 2);
    ASSERT_TRUE(rxQueue.ok);

    ASFW::Isoch::StreamProcessor processor;
    processor.SetOutputSharedQueue(&rxQueue.queue);

    constexpr uint8_t kDbs = 2;
    constexpr uint16_t kDeclaredDataLength = 8 + (kDbs * 4); // CIP header + one event.
    auto packet = MakeAmdtpPacket(kDbs,
                                  /*dbc=*/0,
                                  /*eventCount=*/2,
                                  /*audioLabel=*/0x40,
                                  kDeclaredDataLength);
    packet[6] = Be32(0x40000000u);
    packet[7] = Be32(0x40000000u);

    const auto summary = processor.ProcessPacket(
        reinterpret_cast<const uint8_t*>(packet.data()),
        packet.size() * sizeof(uint32_t));

    int32_t readback[4]{};
    EXPECT_TRUE(summary.hasValidCip);
    EXPECT_EQ(summary.decodedFrames, 1u);
    EXPECT_EQ(rxQueue.queue.FillLevelFrames(), 1u);
    EXPECT_EQ(rxQueue.queue.Read(readback, 2), 1u);
    EXPECT_EQ(readback[0], 0x00000100);
    EXPECT_EQ(readback[1], 0x00000101);
}

} // namespace
