#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "AudioEngine/DirectIsoch/IsochAudioTxPipeline.hpp"
#include "AudioWire/RawPcm24In32/RawPcm24In32Encoder.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Encoding::AudioWireFormat;
using ASFW::Isoch::Core::IsochEventDirection;
using ASFW::Isoch::Core::IsochEventGroup;
using ASFW::Isoch::IsochAudioTxPipeline;
using ASFW::Isoch::Tx::IsochTxPacket;
using ASFW::Isoch::Tx::TxPacketRequest;

constexpr uint32_t kChannels = 2;
constexpr uint32_t kFrames = 128;

void PublishRange(AudioTransportControlBlock& control,
                  uint64_t oldest,
                  uint64_t writtenEnd) {
    control.playbackRingOldestValidFrame.store(oldest, std::memory_order_relaxed);
    control.playbackRingWriteFrame.store(writtenEnd, std::memory_order_release);
}

const uint32_t* Payload(const IsochTxPacket& packet) {
    return packet.words + 2;
}

TEST(IsochAudioTxPipelineTimelineTests,
     UnderrunAdvancesHardwareTimelineAndRecoveryDoesNotReplay) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        output[frame * kChannels] = static_cast<int32_t>((frame + 1) << 8);
        output[frame * kChannels + 1] = -static_cast<int32_t>((frame + 1) << 8);
    }
    PublishRange(control, 0, 16);

    IsochAudioTxPipeline pipeline;
    ASSERT_EQ(pipeline.Configure(0,
                                 static_cast<uint32_t>(ASFW::Encoding::StreamMode::kBlocking),
                                 kChannels,
                                 kChannels,
                                 AudioWireFormat::kRawPcm24In32),
              kIOReturnSuccess);
    pipeline.SetDirectTxRuntimeBinding(IsochAudioTxPipeline::DirectTxRuntimeBinding{
        .outputBase = output.data(),
        .outputBytes = output.size() * sizeof(int32_t),
        .outputFrames = kFrames,
        .control = &control,
        .enabled = true,
        .sampleRateHz = 48000,
        .streamModeRaw =
            static_cast<uint32_t>(ASFW::Encoding::StreamMode::kBlocking),
        .outputChannels = kChannels,
        .am824Slots = kChannels,
    });
    pipeline.ResetForStart();
    pipeline.SetCycleTrackingValid(true);

    uint32_t dataPacketsBeforePhase = 0;
    for (uint32_t packetIndex = 0; packetIndex < 8; ++packetIndex) {
        const auto packet = pipeline.NextTransmitPacket(TxPacketRequest{
            .transmitCycle = 1000 + packetIndex,
            .packetIndex = packetIndex,
        });
        dataPacketsBeforePhase += packet.isData ? 1U : 0U;
    }
    EXPECT_EQ(dataPacketsBeforePhase, 6U);
    EXPECT_EQ(control.txScheduledSampleFrame.load(std::memory_order_acquire), 48U);

    pipeline.OnIsochEventGroup(IsochEventGroup{
        .direction = IsochEventDirection::kTransmit,
        .hostTicks = 1,
        .hwPacketIndex = 8,
        .completedPacketIndex = 7,
        .completedPacketCount = 8,
        .firstRefillPacket = 8,
        .refillPacketCount = 8,
        .outputLastTimestamp = 1007,
    });
    EXPECT_EQ(control.txCompletedSampleFrame.load(std::memory_order_acquire), 48U);

    const auto noData = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1008,
        .packetIndex = 8,
    });
    EXPECT_FALSE(noData.isData);
    EXPECT_EQ(control.txScheduledSampleFrame.load(std::memory_order_acquire), 48U);

    const auto firstPcm = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1009,
        .packetIndex = 9,
    });
    ASSERT_TRUE(firstPcm.isData);
    EXPECT_EQ(Payload(firstPcm)[0], ASFW::Encoding::RawPcm24In32::Encode(output[0]));
    EXPECT_EQ(Payload(firstPcm)[1], ASFW::Encoding::RawPcm24In32::Encode(output[1]));
    EXPECT_EQ(control.txLastSourceFrame.load(std::memory_order_acquire), 0U);

    const auto secondPcm = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1010,
        .packetIndex = 10,
    });
    ASSERT_TRUE(secondPcm.isData);
    EXPECT_EQ(Payload(secondPcm)[0],
              ASFW::Encoding::RawPcm24In32::Encode(output[8 * kChannels]));
    EXPECT_EQ(control.txLastSourceFrame.load(std::memory_order_acquire), 8U);

    const auto underrun = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1011,
        .packetIndex = 11,
    });
    ASSERT_TRUE(underrun.isData);
    EXPECT_EQ(Payload(underrun)[0], 0U);
    EXPECT_EQ(control.txLastSourceFrame.load(std::memory_order_acquire), 16U);
    EXPECT_EQ(control.txScheduledSampleFrame.load(std::memory_order_acquire), 72U);
    EXPECT_EQ(control.counters.txProducerAheadUnderruns.load(std::memory_order_relaxed), 1U);

    PublishRange(control, 0, 32);
    const auto recoveryNoData = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1012,
        .packetIndex = 12,
    });
    EXPECT_FALSE(recoveryNoData.isData);
    EXPECT_EQ(control.txScheduledSampleFrame.load(std::memory_order_acquire), 72U);

    const auto recovered = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1013,
        .packetIndex = 13,
    });
    ASSERT_TRUE(recovered.isData);
    EXPECT_EQ(Payload(recovered)[0],
              ASFW::Encoding::RawPcm24In32::Encode(output[24 * kChannels]));
    EXPECT_EQ(control.txLastSourceFrame.load(std::memory_order_acquire), 24U);
    EXPECT_EQ(control.counters.txForwardCursorCorrections.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txPreventedBackwardCorrections.load(std::memory_order_relaxed), 0U);
    EXPECT_EQ(control.counters.txTimelineInvariantFailures.load(std::memory_order_relaxed), 0U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     CompletionUsesPriorMetadataWhenRefillOverwritesSameSlot) {
    AudioTransportControlBlock control{};
    IsochAudioTxPipeline pipeline;
    ASSERT_EQ(pipeline.Configure(0,
                                 static_cast<uint32_t>(ASFW::Encoding::StreamMode::kBlocking),
                                 kChannels,
                                 kChannels,
                                 AudioWireFormat::kRawPcm24In32),
              kIOReturnSuccess);
    pipeline.SetDirectTxRuntimeBinding(IsochAudioTxPipeline::DirectTxRuntimeBinding{
        .control = &control,
    });
    pipeline.ResetForStart();

    const auto completedNoData = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000,
        .packetIndex = 0,
    });
    ASSERT_FALSE(completedNoData.isData);

    const auto refilledData = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001,
        .packetIndex = 0,
    });
    ASSERT_TRUE(refilledData.isData);
    ASSERT_EQ(control.txScheduledSampleFrame.load(std::memory_order_acquire), 8U);

    pipeline.OnIsochEventGroup(IsochEventGroup{
        .direction = IsochEventDirection::kTransmit,
        .hostTicks = 1,
        .hwPacketIndex = 1,
        .completedPacketIndex = 0,
        .completedPacketCount = 1,
        .firstRefillPacket = 0,
        .refillPacketCount = 1,
        .outputLastTimestamp = 1000,
    });

    EXPECT_EQ(control.txCompletedSampleFrame.load(std::memory_order_acquire), 0U);
}

} // namespace
