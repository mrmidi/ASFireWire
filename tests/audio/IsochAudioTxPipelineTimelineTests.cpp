#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "AudioEngine/DirectIsoch/IsochAudioTxPipeline.hpp"
#include "AudioWire/AM824/AM824Encoder.hpp"
#include "AudioWire/RawPcm24In32/RawPcm24In32Encoder.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Isoch/Memory/IsochDMAMemoryManager.hpp"
#include "Isoch/Transmit/IsochTxDmaRing.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>

namespace {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::FatalStreamReason;
using ASFW::Encoding::AudioWireFormat;
using ASFW::Isoch::Core::IsochEventDirection;
using ASFW::Isoch::Core::IsochEventGroup;
using ASFW::Isoch::IsochAudioTxPipeline;
using ASFW::Isoch::Tx::IsochTxPacket;
using ASFW::Isoch::Tx::PreparedTxAction;
using ASFW::Isoch::Tx::PreparedTxPayloadRequest;
using ASFW::Isoch::Tx::PreparedTxSlotMetadata;
using ASFW::Isoch::Tx::PreparedTxSlotState;
using ASFW::Isoch::Tx::TxPacketRequest;
using ASFW::Isoch::Tx::IsochTxDmaRing;
using ASFW::Isoch::Tx::Layout;

constexpr uint32_t kChannels = 2;
constexpr uint32_t kFrames = 1024;

void PublishRange(AudioTransportControlBlock& control,
                  uint64_t oldest,
                  uint64_t writtenEnd) {
    control.playbackRingOldestValidFrame.store(oldest, std::memory_order_relaxed);
    control.playbackRingWriteFrame.store(writtenEnd, std::memory_order_release);
}

void ConfigurePipeline(IsochAudioTxPipeline& pipeline,
                       AudioTransportControlBlock& control,
                       std::array<int32_t, kFrames * kChannels>& output,
                       AudioWireFormat format = AudioWireFormat::kRawPcm24In32) {
    EXPECT_EQ(pipeline.Configure(
                  0,
                  static_cast<uint32_t>(ASFW::Encoding::StreamMode::kBlocking),
                  kChannels,
                  kChannels,
                  format),
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
}

PreparedTxSlotMetadata SlotMetadata(const IsochTxPacket& packet,
                                    uint64_t generation) {
    return PreparedTxSlotMetadata{
        .generation = generation,
        .timelineFirstFrame = packet.timelineFirstFrame,
        .sizeBytes = packet.sizeBytes,
        .framesPerPacket = packet.framesPerPacket,
        .dbc = packet.dbc,
        .syt = packet.syt,
        .valid = true,
        .isData = packet.isData,
        .state = PreparedTxSlotState::InitialSilence,
    };
}

PreparedTxPayloadRequest WritableRequest(uint32_t packetIndex,
                                         uint64_t generation,
                                         const IsochTxPacket& packet,
                                         std::array<uint8_t, 4096>& payload,
                                         uint32_t distance = 5) {
    std::memcpy(payload.data(), packet.words, packet.sizeBytes);
    return PreparedTxPayloadRequest{
        .packetIndex = packetIndex,
        .hwPacketIndex = 0,
        .distanceToHardware = distance,
        .writable = true,
        .deadline = false,
        .metadata = SlotMetadata(packet, generation),
        .payloadBytes = payload.data(),
        .payloadCapacityBytes = payload.size(),
    };
}

std::shared_ptr<ASFW::Isoch::Memory::IIsochDMAMemory> MakeIsochMemory(
    ASFW::Driver::HardwareInterface& hardware) {
    ASFW::Isoch::Memory::IsochMemoryConfig config;
    config.numDescriptors = Layout::kNumPackets * Layout::kBlocksPerPacket;
    config.packetSizeBytes = Layout::kMaxPacketSize;
    config.descriptorAlignment = Layout::kOHCIPageSize;
    config.payloadPageAlignment = Layout::kOHCIPageSize;
    auto memory = ASFW::Isoch::Memory::IsochDMAMemoryManager::Create(config);
    EXPECT_TRUE(memory);
    EXPECT_TRUE(memory->Initialize(hardware));
    return memory;
}

TEST(IsochAudioTxPipelineTimelineTests, PacketProductionIsTimedSilenceOnly) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    output[0] = 0x123400;
    PublishRange(control, 0, 128);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    const auto noData = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});

    ASSERT_FALSE(noData.isData);
    ASSERT_TRUE(data.isData);
    EXPECT_EQ(data.framesPerPacket, 8U);
    EXPECT_EQ(data.timelineFirstFrame, 0U);
    EXPECT_EQ(data.words[2], 0U);
    EXPECT_EQ(control.txScheduledSampleFrame.load(std::memory_order_acquire), 8U);
    EXPECT_EQ(control.txPreparedSourceEndFrame.load(std::memory_order_acquire), 0U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     PreparationBeforeDirectBindingRemainsStartupSilence) {
    IsochAudioTxPipeline pipeline;
    ASSERT_EQ(pipeline.Configure(
                  0,
                  static_cast<uint32_t>(ASFW::Encoding::StreamMode::kBlocking),
                  kChannels,
                  kChannels,
                  AudioWireFormat::kRawPcm24In32),
              kIOReturnSuccess);
    pipeline.ResetForStart();

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    ASSERT_TRUE(data.isData);

    std::array<uint8_t, 4096> payload{};
    const auto result =
        pipeline.PreparePayload(WritableRequest(1, 1, data, payload));

    EXPECT_EQ(result.action, PreparedTxAction::NoChange);
    EXPECT_FALSE(pipeline.HasFatalFault());
}

TEST(IsochAudioTxPipelineTimelineTests,
     PreparationAnchorsEarliestWritableSlotAndAcceptsExactWrittenBoundary) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        output[frame * kChannels] = static_cast<int32_t>((frame + 1) << 8);
        output[frame * kChannels + 1] = -static_cast<int32_t>((frame + 1) << 8);
    }
    PublishRange(control, 100, 108);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 7});
    std::array<uint8_t, 4096> payload{};
    const auto result =
        pipeline.PreparePayload(WritableRequest(1, 7, data, payload));

    ASSERT_EQ(result.action, PreparedTxAction::Prepared);
    EXPECT_EQ(result.sourceFirstFrame, 100U);
    EXPECT_EQ(result.sourceEndFrame, 108U);
    const auto* encoded = reinterpret_cast<const uint32_t*>(payload.data()) + 2;
    EXPECT_EQ(encoded[0],
              ASFW::Encoding::RawPcm24In32::Encode(
                  output[(100 % kFrames) * kChannels]));
    EXPECT_EQ(control.txPreparedSourceEndFrame.load(std::memory_order_acquire), 108U);
    EXPECT_EQ(control.playbackRingReadFrame.load(std::memory_order_acquire), 0U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     DelayedFirstWriteLeavesStartupSilenceThenAnchorsWithoutReplay) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    output[200 * kChannels] = 0x123400;
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> payload{};
    auto request = WritableRequest(1, 1, data, payload);
    EXPECT_EQ(pipeline.PreparePayload(request).action,
              PreparedTxAction::NoChange);

    PublishRange(control, 200, 208);
    const auto prepared = pipeline.PreparePayload(request);
    EXPECT_EQ(prepared.action, PreparedTxAction::Prepared);
    EXPECT_EQ(prepared.sourceFirstFrame, 200U);
    EXPECT_EQ(prepared.sourceEndFrame, 208U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     RepeatedWriteBurstsProduceSequentialSourceRangesAcrossBlockingCadence) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 512);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    uint64_t expectedSource = 0;
    uint32_t preparedPackets = 0;
    for (uint32_t packetIndex = 0; packetIndex < 64; ++packetIndex) {
        const auto packet = pipeline.NextTransmitPacket(TxPacketRequest{
            .transmitCycle = 1000 + packetIndex,
            .packetIndex = packetIndex,
            .slotGeneration = 1,
        });
        if (!packet.isData) {
            continue;
        }
        std::array<uint8_t, 4096> payload{};
        const auto result = pipeline.PreparePayload(
            WritableRequest(packetIndex, 1, packet, payload, 8));
        ASSERT_EQ(result.action, PreparedTxAction::Prepared);
        EXPECT_EQ(result.sourceFirstFrame, expectedSource);
        EXPECT_EQ(result.sourceEndFrame, expectedSource + 8);
        expectedSource += 8;
        ++preparedPackets;
    }

    EXPECT_EQ(preparedPackets, 48U);
    EXPECT_EQ(expectedSource, 384U);
    EXPECT_EQ(control.counters.txPreparedPcmSlots.load(std::memory_order_relaxed),
              48U);
    EXPECT_EQ(control.counters.txReadAheadFaults.load(std::memory_order_relaxed),
              0U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     PendingFutureRangeBecomesFatalReadAheadAtDeadline) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 8);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto first = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> firstPayload{};
    ASSERT_EQ(pipeline.PreparePayload(
                  WritableRequest(1, 1, first, firstPayload)).action,
              PreparedTxAction::Prepared);

    const auto second = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1002, .packetIndex = 2, .slotGeneration = 1});
    std::array<uint8_t, 4096> secondPayload{};
    auto pendingRequest = WritableRequest(2, 1, second, secondPayload, 6);
    const auto pending = pipeline.PreparePayload(pendingRequest);
    ASSERT_EQ(pending.action, PreparedTxAction::Pending);
    EXPECT_EQ(pending.sourceFirstFrame, 8U);
    EXPECT_EQ(pending.sourceEndFrame, 16U);

    pendingRequest.writable = false;
    pendingRequest.deadline = true;
    pendingRequest.distanceToHardware = 4;
    pendingRequest.payloadBytes = nullptr;
    pendingRequest.payloadCapacityBytes = 0;
    pendingRequest.metadata.state = PreparedTxSlotState::PendingSource;
    pendingRequest.metadata.epoch = pending.epoch;
    pendingRequest.metadata.sourceFirstFrame = pending.sourceFirstFrame;
    pendingRequest.metadata.sourceEndFrame = pending.sourceEndFrame;
    const auto fatal = pipeline.PreparePayload(pendingRequest);
    const auto repeatedFatal = pipeline.PreparePayload(pendingRequest);

    EXPECT_EQ(fatal.action, PreparedTxAction::Fatal);
    EXPECT_EQ(repeatedFatal.action, PreparedTxAction::Fatal);
    EXPECT_EQ(control.fatalReason.load(std::memory_order_acquire),
              FatalStreamReason::TxReadAhead);
    EXPECT_EQ(control.fatalGeneration.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(control.counters.txReadAheadFaults.load(std::memory_order_relaxed), 1U);
    EXPECT_EQ(control.txFatalSnapshot.packetIndex.load(std::memory_order_relaxed), 2U);
    EXPECT_EQ(control.txFatalSnapshot.sourceEndFrame.load(std::memory_order_relaxed), 16U);
    EXPECT_EQ(control.fatalGeneration.load(std::memory_order_acquire), 1U);
    EXPECT_EQ(control.counters.txReadAheadFaults.load(std::memory_order_relaxed), 1U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     WatchdogCompletionWithoutHostTimeAdvancesOnlyPreparedPcm) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 40, 48);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> payload{};
    ASSERT_EQ(pipeline.PreparePayload(
                  WritableRequest(1, 1, data, payload)).action,
              PreparedTxAction::Prepared);

    pipeline.OnIsochEventGroup(IsochEventGroup{
        .direction = IsochEventDirection::kTransmit,
        .hostTicks = 0,
        .hwPacketIndex = 2,
        .completedPacketIndex = 1,
        .completedPacketCount = 2,
        .firstRefillPacket = 2,
        .refillPacketCount = 0,
        .outputLastTimestamp = 1001,
    });

    EXPECT_EQ(control.txCompletedSampleFrame.load(std::memory_order_acquire), 8U);
    EXPECT_EQ(control.playbackRingReadFrame.load(std::memory_order_acquire), 48U);
    EXPECT_EQ(control.outputConsumedEndFrame.load(std::memory_order_acquire), 48U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     DiscontinuityRetiresOldPendingEpochAndReanchorsNewSlots) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 8);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto first = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> firstPayload{};
    ASSERT_EQ(pipeline.PreparePayload(
                  WritableRequest(1, 1, first, firstPayload)).action,
              PreparedTxAction::Prepared);

    const auto pendingPacket = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1002, .packetIndex = 2, .slotGeneration = 1});
    std::array<uint8_t, 4096> pendingPayload{};
    auto pendingRequest = WritableRequest(2, 1, pendingPacket, pendingPayload, 6);
    const auto pending = pipeline.PreparePayload(pendingRequest);
    ASSERT_EQ(pending.action, PreparedTxAction::Pending);
    pendingRequest.metadata.epoch = pending.epoch;
    pendingRequest.metadata.sourceFirstFrame = pending.sourceFirstFrame;
    pendingRequest.metadata.sourceEndFrame = pending.sourceEndFrame;

    PublishRange(control, 500, 508);
    control.playbackRingDiscontinuityGeneration.fetch_add(1, std::memory_order_release);
    const auto retired = pipeline.PreparePayload(pendingRequest);
    EXPECT_EQ(retired.action, PreparedTxAction::RetiredSilence);

    const auto newPacket = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1003, .packetIndex = 3, .slotGeneration = 1});
    std::array<uint8_t, 4096> newPayload{};
    const auto reanchored =
        pipeline.PreparePayload(WritableRequest(3, 1, newPacket, newPayload, 7));
    EXPECT_EQ(reanchored.action, PreparedTxAction::Prepared);
    EXPECT_EQ(reanchored.sourceFirstFrame, 500U);
    EXPECT_EQ(reanchored.sourceEndFrame, 508U);
    EXPECT_GT(reanchored.epoch, pending.epoch);
}

TEST(IsochAudioTxPipelineTimelineTests, OverwrittenMappedSourceIsImmediatelyFatal) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 8);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto first = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> firstPayload{};
    ASSERT_EQ(pipeline.PreparePayload(
                  WritableRequest(1, 1, first, firstPayload)).action,
              PreparedTxAction::Prepared);

    const auto second = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1002, .packetIndex = 2, .slotGeneration = 1});
    PublishRange(control, 9, 16);
    std::array<uint8_t, 4096> secondPayload{};
    const auto fatal =
        pipeline.PreparePayload(WritableRequest(2, 1, second, secondPayload, 6));

    EXPECT_EQ(fatal.action, PreparedTxAction::Fatal);
    EXPECT_EQ(control.fatalReason.load(std::memory_order_acquire),
              FatalStreamReason::TxSourceOverwritten);
    EXPECT_EQ(control.counters.txSourceOverwrittenFaults.load(
                  std::memory_order_relaxed),
              1U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     AvailableButUnpreparedDeadlineIsFatal) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 16);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto first = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> firstPayload{};
    ASSERT_EQ(pipeline.PreparePayload(
                  WritableRequest(1, 1, first, firstPayload)).action,
              PreparedTxAction::Prepared);

    const auto second = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1002, .packetIndex = 2, .slotGeneration = 1});
    PreparedTxPayloadRequest deadline{
        .packetIndex = 2,
        .hwPacketIndex = 2,
        .distanceToHardware = 0,
        .writable = false,
        .deadline = true,
        .metadata = SlotMetadata(second, 1),
    };
    const auto fatal = pipeline.PreparePayload(deadline);

    EXPECT_EQ(fatal.action, PreparedTxAction::Fatal);
    EXPECT_EQ(control.fatalReason.load(std::memory_order_acquire),
              FatalStreamReason::TxPreparationMissedDeadline);
    EXPECT_EQ(control.counters.txPreparationDeadlineFaults.load(
                  std::memory_order_relaxed),
              1U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     StaleGenerationAndSourceOverflowAreSlotInvariantFaults) {
    {
        AudioTransportControlBlock control{};
        std::array<int32_t, kFrames * kChannels> output{};
        PublishRange(control, 0, 8);
        IsochAudioTxPipeline pipeline;
        ConfigurePipeline(pipeline, control, output);
        (void)pipeline.NextTransmitPacket(TxPacketRequest{
            .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
        const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
            .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 4});
        std::array<uint8_t, 4096> payload{};
        const auto fatal =
            pipeline.PreparePayload(WritableRequest(1, 3, data, payload));
        EXPECT_EQ(fatal.action, PreparedTxAction::Fatal);
        EXPECT_EQ(control.fatalReason.load(std::memory_order_acquire),
                  FatalStreamReason::TxSlotInvariant);
    }

    {
        AudioTransportControlBlock control{};
        std::array<int32_t, kFrames * kChannels> output{};
        PublishRange(control,
                     std::numeric_limits<uint64_t>::max() - 4,
                     std::numeric_limits<uint64_t>::max());
        IsochAudioTxPipeline pipeline;
        ConfigurePipeline(pipeline, control, output);
        (void)pipeline.NextTransmitPacket(TxPacketRequest{
            .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
        const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
            .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
        std::array<uint8_t, 4096> payload{};
        const auto fatal =
            pipeline.PreparePayload(WritableRequest(1, 1, data, payload));
        EXPECT_EQ(fatal.action, PreparedTxAction::Fatal);
        EXPECT_EQ(control.fatalReason.load(std::memory_order_acquire),
                  FatalStreamReason::TxSlotInvariant);
    }
}

TEST(IsochAudioTxPipelineTimelineTests, PreparedAm824PayloadMatchesSourceMarkers) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    output[0] = 0x00123456;
    output[1] = static_cast<int32_t>(0x00FEDCBA);
    PublishRange(control, 0, 8);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output, AudioWireFormat::kAM824);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> payload{};
    ASSERT_EQ(pipeline.PreparePayload(
                  WritableRequest(1, 1, data, payload)).action,
              PreparedTxAction::Prepared);

    const auto* encoded = reinterpret_cast<const uint32_t*>(payload.data()) + 2;
    EXPECT_EQ(encoded[0], ASFW::Encoding::AM824Encoder::encode(output[0]));
    EXPECT_EQ(encoded[1], ASFW::Encoding::AM824Encoder::encode(output[1]));
}

TEST(IsochAudioTxPipelineTimelineTests,
     PrimedDmaRingPatchesEarliestWritableDataSlotWithCoreAudioPcm) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        output[frame * kChannels] = static_cast<int32_t>((frame + 1) << 8);
        output[frame * kChannels + 1] = -static_cast<int32_t>((frame + 1) << 8);
    }
    PublishRange(control, 0, 128);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    ASFW::Driver::HardwareInterface hardware;
    hardware.SetTestRegister(ASFW::Driver::Register32::kCycleTimer,
                             1000u << 12);
    auto memory = MakeIsochMemory(hardware);
    ASSERT_TRUE(memory);
    IsochTxDmaRing ring;
    ASSERT_EQ(ring.SetupRings(*memory), kIOReturnSuccess);
    ring.ResetForStart();
    ring.SeedCycleTracking(hardware);
    ASSERT_EQ(ring.Prime(pipeline).packetsAssembled, Layout::kNumPackets);
    hardware.SetTestRegister(
        static_cast<ASFW::Driver::Register32>(
            DMAContextHelpers::IsoXmitCommandPtr(0)),
        ring.Slab().GetDescriptorIOVA(0) | Layout::kBlocksPerPacket);

    const auto prepared = ring.PreparePayloads(hardware, 0, pipeline);
    ASSERT_TRUE(prepared.ok);
    ASSERT_GT(prepared.preparedCount, 0U);

    for (uint32_t packet = 0; packet <= Layout::kGuardBandPackets; ++packet) {
        const auto* payload =
            reinterpret_cast<const uint32_t*>(ring.Slab().PayloadPtr(packet));
        EXPECT_EQ(payload[2], 0U);
    }

    constexpr uint32_t kFirstWritableDataPacket = 5;
    ASSERT_EQ(ring.SlotMetadata(kFirstWritableDataPacket).state,
              PreparedTxSlotState::PcmPrepared);
    EXPECT_EQ(ring.SlotMetadata(kFirstWritableDataPacket).sourceFirstFrame, 0U);
    const auto* payload = reinterpret_cast<const uint32_t*>(
        ring.Slab().PayloadPtr(kFirstWritableDataPacket));
    EXPECT_EQ(payload[2],
              ASFW::Encoding::RawPcm24In32::Encode(output[0]));
    EXPECT_EQ(payload[3],
              ASFW::Encoding::RawPcm24In32::Encode(output[1]));
    EXPECT_EQ(control.playbackRingReadFrame.load(std::memory_order_acquire), 0U);
}

} // namespace
