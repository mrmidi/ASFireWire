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
                                         uint32_t distance = 65) {
    std::memcpy(payload.data(), packet.words, packet.sizeBytes);
    const bool deadline = distance <= Layout::kPreparationDeadlinePackets;
    return PreparedTxPayloadRequest{
        .packetIndex = packetIndex,
        .hwPacketIndex = 0,
        .distanceToHardware = distance,
        .writable = !deadline,
        .deadline = deadline,
        .hardwareOwned = distance <= Layout::kHardwareOwnedGuardPackets,
        .metadata = SlotMetadata(packet, generation),
        .payloadBytes = deadline ? nullptr : payload.data(),
        .payloadCapacityBytes =
            deadline ? 0U : static_cast<uint32_t>(payload.size()),
    };
}

ASFW::Isoch::Tx::PreparedTxPayloadResult PreparePopulated(
    IsochAudioTxPipeline& pipeline,
    const PreparedTxPayloadRequest& request) {
    pipeline.PopulateClipStyleTxRingFromWrittenRange();
    return pipeline.PreparePayload(request);
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
    EXPECT_EQ(data.timelineFirstFrame, 768U);
    EXPECT_EQ(data.words[2], 0U);
    EXPECT_EQ(control.txScheduledSampleFrame.load(std::memory_order_acquire), 776U);
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
        PreparePopulated(pipeline, WritableRequest(1, 1, data, payload));

    EXPECT_EQ(result.action, PreparedTxAction::NoChange);
    EXPECT_FALSE(pipeline.HasFatalFault());
}

TEST(IsochAudioTxPipelineTimelineTests,
     SlotPrimedBeforeLateBindingDrainsAsStartupSilence) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 128);

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
    const auto primedBeforeBinding = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    ASSERT_TRUE(primedBeforeBinding.isData);
    ASSERT_LT(primedBeforeBinding.timelineFirstFrame, 768U);

    pipeline.SetDirectTxRuntimeBinding(
        IsochAudioTxPipeline::DirectTxRuntimeBinding{
            .outputBase = output.data(),
            .outputBytes = output.size() * sizeof(int32_t),
            .outputFrames = kFrames,
            .control = &control,
            .enabled = true,
            .sampleRateHz = 48000,
            .streamModeRaw = static_cast<uint32_t>(
                ASFW::Encoding::StreamMode::kBlocking),
            .outputChannels = kChannels,
            .am824Slots = kChannels,
        });

    std::array<uint8_t, 4096> oldPayload{};
    const auto oldResult = PreparePopulated(
        pipeline,
        WritableRequest(1, 1, primedBeforeBinding, oldPayload));
    EXPECT_EQ(oldResult.action, PreparedTxAction::NoChange);
    EXPECT_FALSE(pipeline.HasFatalFault());

    const auto packetAfterBinding = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1002, .packetIndex = 2, .slotGeneration = 1});
    ASSERT_TRUE(packetAfterBinding.isData);
    EXPECT_EQ(packetAfterBinding.timelineFirstFrame, 768U);

    std::array<uint8_t, 4096> newPayload{};
    const auto newResult = PreparePopulated(
        pipeline,
        WritableRequest(2, 1, packetAfterBinding, newPayload));
    EXPECT_EQ(newResult.action, PreparedTxAction::Prepared);
    EXPECT_EQ(newResult.sourceFirstFrame, 0U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     PartialLeadingPacketUsesEncodedSilenceAtPacketAlignedOffset) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        output[frame * kChannels] = static_cast<int32_t>((frame + 1) << 8);
        output[frame * kChannels + 1] = -static_cast<int32_t>((frame + 1) << 8);
    }
    PublishRange(control, 100, 868);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 7});
    std::array<uint8_t, 4096> payload{};
    const auto result =
        PreparePopulated(pipeline, WritableRequest(1, 7, data, payload));

    ASSERT_EQ(result.action, PreparedTxAction::Prepared);
    EXPECT_EQ(result.sourceFirstFrame, 96U);
    EXPECT_EQ(result.sourceEndFrame, 104U);
    const auto* encoded = reinterpret_cast<const uint32_t*>(payload.data()) + 2;
    EXPECT_EQ(encoded[0], 0U);
    EXPECT_EQ(encoded[1], 0U);
    EXPECT_EQ(control.playbackRingReadFrame.load(std::memory_order_acquire), 0U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     DelayedFirstWriteDoesNotReanchorAlreadyScheduledPacket) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    output[200 * kChannels] = 0x123400;
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    const auto noData = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    EXPECT_TRUE(pipeline.OnTransmitSlotCompleted(
        ASFW::Isoch::Tx::CompletedTxSlot{
            .metadata = SlotMetadata(noData, 1),
            .packetIndex = 0,
            .hwPacketIndex = 1,
        }));
    EXPECT_EQ(control.txCompletedSampleFrame.load(std::memory_order_acquire), 768U);
    EXPECT_EQ(control.counters.txCompletedStartupSilenceSlots.load(
                  std::memory_order_relaxed),
              0U);

    std::array<uint8_t, 4096> payload{};
    auto request = WritableRequest(1, 1, data, payload);
    auto initial = PreparePopulated(pipeline, request);
    ASSERT_EQ(initial.action, PreparedTxAction::NoChange);
    EXPECT_EQ(initial.sourceFirstFrame, 0U);

    PublishRange(control, 200, 968);
    const auto prepared = PreparePopulated(pipeline, request);
    EXPECT_EQ(prepared.action, PreparedTxAction::Prepared);
    EXPECT_EQ(prepared.sourceFirstFrame, 0U);
    EXPECT_EQ(prepared.sourceEndFrame, 8U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     FirstWrittenBurstIsAvailableWithoutStartupGate) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> payload{};
    auto request = WritableRequest(1, 1, data, payload);

    PublishRange(control, 0, 192);
    const auto prepared = PreparePopulated(pipeline, request);
    ASSERT_EQ(prepared.action, PreparedTxAction::Prepared);
    EXPECT_EQ(prepared.sourceFirstFrame, 0U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     RepeatedWriteBurstsProduceSequentialSourceRangesAcrossBlockingCadence) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 768);
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
        const auto result = PreparePopulated(pipeline,
            WritableRequest(packetIndex, 1, packet, payload, 65));
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
     UncoveredFutureRangeDefersUntilWrittenAndDeadlineLeavesSlotUnchanged) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 768);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto first = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> firstPayload{};
    ASSERT_EQ(PreparePopulated(
                  pipeline, WritableRequest(1, 1, first, firstPayload)).action,
              PreparedTxAction::Prepared);

    IsochTxPacket second{};
    uint32_t secondPacketIndex = 0;
    for (uint32_t packetIndex = 2; packetIndex < 140; ++packetIndex) {
        const auto candidate = pipeline.NextTransmitPacket(TxPacketRequest{
            .transmitCycle = 1000 + packetIndex,
            .packetIndex = packetIndex,
            .slotGeneration = 1,
        });
        if (candidate.isData && candidate.timelineFirstFrame == 1536) {
            second = candidate;
            secondPacketIndex = packetIndex;
            break;
        }
    }
    ASSERT_NE(secondPacketIndex, 0U);
    std::array<uint8_t, 4096> secondPayload{};
    auto pendingRequest =
        WritableRequest(secondPacketIndex, 1, second, secondPayload, 65);
    const auto uncovered = PreparePopulated(pipeline, pendingRequest);
    ASSERT_EQ(uncovered.action, PreparedTxAction::NoChange);
    EXPECT_EQ(uncovered.sourceFirstFrame, 768U);
    EXPECT_EQ(uncovered.sourceEndFrame, 776U);

    pendingRequest.writable = false;
    pendingRequest.deadline = true;
    pendingRequest.distanceToHardware = 64;
    pendingRequest.hardwareOwned = false;
    pendingRequest.payloadBytes = nullptr;
    pendingRequest.payloadCapacityBytes = 0;
    const auto deadline = PreparePopulated(pipeline, pendingRequest);
    EXPECT_EQ(deadline.action, PreparedTxAction::NoChange);
    EXPECT_FALSE(pipeline.HasFatalFault());

    output[(768 % kFrames) * kChannels] = 0x123400;
    output[(768 % kFrames) * kChannels + 1] = -0x123400;
    PublishRange(control, 0, 776);
    pendingRequest.writable = true;
    pendingRequest.deadline = false;
    pendingRequest.distanceToHardware = 65;
    pendingRequest.hardwareOwned = false;
    pendingRequest.payloadBytes = secondPayload.data();
    pendingRequest.payloadCapacityBytes =
        static_cast<uint32_t>(secondPayload.size());
    const auto populated = PreparePopulated(pipeline, pendingRequest);
    ASSERT_EQ(populated.action, PreparedTxAction::Prepared);
    EXPECT_EQ(populated.sourceFirstFrame, 768U);
    EXPECT_EQ(populated.sourceEndFrame, 776U);
    const auto* encoded =
        reinterpret_cast<const uint32_t*>(secondPayload.data()) + 2;
    EXPECT_EQ(encoded[0],
              ASFW::Encoding::RawPcm24In32::Encode(
                  output[(768 % kFrames) * kChannels]));
    EXPECT_EQ(encoded[1],
              ASFW::Encoding::RawPcm24In32::Encode(
                  output[(768 % kFrames) * kChannels + 1]));
}

TEST(IsochAudioTxPipelineTimelineTests,
     WatchdogCompletionWithoutHostTimeAdvancesOnlyPreparedPcm) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 40, 808);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> payload{};
    ASSERT_EQ(PreparePopulated(
                  pipeline, WritableRequest(1, 1, data, payload)).action,
              PreparedTxAction::Prepared);

    auto completedMetadata = SlotMetadata(data, 1);
    completedMetadata.state = PreparedTxSlotState::PcmPrepared;
    completedMetadata.sourceFirstFrame = 40;
    completedMetadata.sourceEndFrame = 48;
    completedMetadata.preparedPayloadHash =
        ASFW::Isoch::Tx::HashTxPayload(payload.data(), data.sizeBytes);
    EXPECT_TRUE(pipeline.OnTransmitSlotCompleted(ASFW::Isoch::Tx::CompletedTxSlot{
        .metadata = completedMetadata,
        .completedPayloadHash = completedMetadata.preparedPayloadHash,
        .packetIndex = 1,
        .hwPacketIndex = 2,
        .payloadHashMatches = true,
    }));

    EXPECT_EQ(control.txCompletedSampleFrame.load(std::memory_order_acquire), 816U);
    EXPECT_EQ(control.playbackRingReadFrame.load(std::memory_order_acquire), 48U);
    EXPECT_EQ(control.outputConsumedEndFrame.load(std::memory_order_acquire), 48U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     DiscontinuityIsDiagnosticAndDoesNotRetirePreparedSlots) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 768);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto first = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> firstPayload{};
    ASSERT_EQ(PreparePopulated(
                  pipeline, WritableRequest(1, 1, first, firstPayload)).action,
              PreparedTxAction::Prepared);
    control.playbackRingDiscontinuityGeneration.fetch_add(1, std::memory_order_release);
    const auto repeated = PreparePopulated(
        pipeline, WritableRequest(1, 1, first, firstPayload));
    EXPECT_EQ(repeated.action, PreparedTxAction::Prepared);
    EXPECT_EQ(repeated.sourceFirstFrame, 0U);
    EXPECT_EQ(control.counters.txTimelineDiscontinuities.load(
                  std::memory_order_relaxed), 1U);
}

TEST(IsochAudioTxPipelineTimelineTests, SoftwareRingRemainsSourceAfterCoreAudioOverwrite) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 768);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto first = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> firstPayload{};
    ASSERT_EQ(PreparePopulated(
                  pipeline, WritableRequest(1, 1, first, firstPayload)).action,
              PreparedTxAction::Prepared);

    const auto second = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1002, .packetIndex = 2, .slotGeneration = 1});
    PublishRange(control, 9, 777);
    std::array<uint8_t, 4096> secondPayload{};
    const auto prepared = PreparePopulated(
        pipeline, WritableRequest(2, 1, second, secondPayload, 65));

    EXPECT_EQ(prepared.action, PreparedTxAction::Prepared);
    EXPECT_FALSE(pipeline.HasFatalFault());
}

TEST(IsochAudioTxPipelineTimelineTests,
     AvailableButNonWritableDeadlineRemainsInitialSilence) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 768);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto first = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> firstPayload{};
    ASSERT_EQ(PreparePopulated(
                  pipeline, WritableRequest(1, 1, first, firstPayload)).action,
              PreparedTxAction::Prepared);

    const auto second = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1002, .packetIndex = 2, .slotGeneration = 1});
    PreparedTxPayloadRequest deadline{
        .packetIndex = 2,
        .hwPacketIndex = 2,
        .distanceToHardware = 0,
        .writable = false,
        .deadline = true,
        .hardwareOwned = true,
        .metadata = SlotMetadata(second, 1),
    };
    const auto unchanged = PreparePopulated(pipeline, deadline);

    EXPECT_EQ(unchanged.action, PreparedTxAction::NoChange);
    EXPECT_FALSE(pipeline.HasFatalFault());
}

TEST(IsochAudioTxPipelineTimelineTests,
     StaleGenerationIsSlotInvariantFault) {
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
        PreparePopulated(pipeline, WritableRequest(1, 3, data, payload));
    EXPECT_EQ(fatal.action, PreparedTxAction::Fatal);
    EXPECT_EQ(control.fatalReason.load(std::memory_order_acquire),
              FatalStreamReason::TxSlotInvariant);
}

TEST(IsochAudioTxPipelineTimelineTests, PreparedAm824PayloadMatchesSourceMarkers) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    output[0] = 0x00123456;
    output[1] = static_cast<int32_t>(0x00FEDCBA);
    PublishRange(control, 0, 768);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output, AudioWireFormat::kAM824);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> payload{};
    ASSERT_EQ(PreparePopulated(
                  pipeline, WritableRequest(1, 1, data, payload)).action,
              PreparedTxAction::Prepared);

    const auto* encoded = reinterpret_cast<const uint32_t*>(payload.data()) + 2;
    EXPECT_EQ(encoded[0], ASFW::Encoding::AM824Encoder::encode(output[0]));
    EXPECT_EQ(encoded[1], ASFW::Encoding::AM824Encoder::encode(output[1]));
}

TEST(IsochAudioTxPipelineTimelineTests,
     CompletedPayloadMismatchIsFatalAndDoesNotAdvanceConsumption) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 768);
    IsochAudioTxPipeline pipeline;
    ConfigurePipeline(pipeline, control, output);

    (void)pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1000, .packetIndex = 0, .slotGeneration = 1});
    const auto data = pipeline.NextTransmitPacket(TxPacketRequest{
        .transmitCycle = 1001, .packetIndex = 1, .slotGeneration = 1});
    std::array<uint8_t, 4096> payload{};
    const auto prepared =
        PreparePopulated(pipeline, WritableRequest(1, 1, data, payload));
    ASSERT_EQ(prepared.action, PreparedTxAction::Prepared);

    auto metadata = SlotMetadata(data, 1);
    metadata.state = PreparedTxSlotState::PcmPrepared;
    metadata.sourceFirstFrame = prepared.sourceFirstFrame;
    metadata.sourceEndFrame = prepared.sourceEndFrame;
    metadata.preparedPayloadHash =
        ASFW::Isoch::Tx::HashTxPayload(payload.data(), data.sizeBytes);

    EXPECT_FALSE(pipeline.OnTransmitSlotCompleted(
        ASFW::Isoch::Tx::CompletedTxSlot{
            .metadata = metadata,
            .completedPayloadHash = metadata.preparedPayloadHash ^ 1U,
            .packetIndex = 1,
            .hwPacketIndex = 2,
            .payloadHashMatches = false,
        }));
    EXPECT_EQ(control.fatalReason.load(std::memory_order_acquire),
              FatalStreamReason::TxPayloadMismatch);
    EXPECT_EQ(control.playbackRingReadFrame.load(std::memory_order_acquire), 0U);
    EXPECT_EQ(control.counters.txCompletedPayloadHashMismatches.load(
                  std::memory_order_relaxed),
              1U);
    pipeline.RecordImmediateStop();
    EXPECT_EQ(control.counters.txImmediateStops.load(std::memory_order_relaxed),
              1U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     PrimedDmaRingPatchesEarliestWritableDataSlotWithCoreAudioPcm) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        output[frame * kChannels] = static_cast<int32_t>((frame + 1) << 8);
        output[frame * kChannels + 1] = -static_cast<int32_t>((frame + 1) << 8);
    }
    PublishRange(control, 0, 768);
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

    pipeline.PopulateClipStyleTxRingFromWrittenRange();
    const auto prepared = ring.PreparePayloads(hardware, 0, pipeline);
    ASSERT_TRUE(prepared.ok);
    ASSERT_GT(prepared.preparedCount, 0U);

    for (uint32_t packet = 0;
         packet <= Layout::kPreparationDeadlinePackets;
         ++packet) {
        const auto* payload =
            reinterpret_cast<const uint32_t*>(ring.Slab().PayloadPtr(packet));
        EXPECT_EQ(payload[2], 0U);
    }

    constexpr uint32_t kFirstWritableDataPacket = 65;
    ASSERT_EQ(ring.SlotMetadata(kFirstWritableDataPacket).state,
              PreparedTxSlotState::PcmPrepared);
    const uint64_t sourceFirst =
        ring.SlotMetadata(kFirstWritableDataPacket).sourceFirstFrame;
    const auto* payload = reinterpret_cast<const uint32_t*>(
        ring.Slab().PayloadPtr(kFirstWritableDataPacket));
    EXPECT_EQ(payload[2],
              ASFW::Encoding::RawPcm24In32::Encode(
                  output[(sourceFirst % kFrames) * kChannels]));
    EXPECT_EQ(payload[3],
              ASFW::Encoding::RawPcm24In32::Encode(
                  output[(sourceFirst % kFrames) * kChannels + 1]));
    EXPECT_EQ(control.playbackRingReadFrame.load(std::memory_order_acquire), 0U);
}

TEST(IsochAudioTxPipelineTimelineTests,
     Repeated192FrameWritesSurviveMultipleDmaRingRotations) {
    AudioTransportControlBlock control{};
    std::array<int32_t, kFrames * kChannels> output{};
    PublishRange(control, 0, 768);
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

    uint32_t hwPacket = 0;
    hardware.SetTestRegister(
        static_cast<ASFW::Driver::Register32>(
            DMAContextHelpers::IsoXmitCommandPtr(0)),
        ring.Slab().GetDescriptorIOVA(0) | Layout::kBlocksPerPacket);
    pipeline.PopulateClipStyleTxRingFromWrittenRange();
    ASSERT_TRUE(ring.PreparePayloads(hardware, 0, pipeline).ok);

    uint64_t writtenEnd = 768;
    for (uint32_t callback = 0; callback < 18; ++callback) {
        writtenEnd += 192;
        const uint64_t oldest =
            writtenEnd > kFrames ? writtenEnd - kFrames : 0;
        PublishRange(control, oldest, writtenEnd);
        hwPacket = (hwPacket + 32) % Layout::kNumPackets;
        hardware.SetTestRegister(
            static_cast<ASFW::Driver::Register32>(
                DMAContextHelpers::IsoXmitCommandPtr(0)),
            ring.Slab().GetDescriptorIOVA(
                hwPacket * Layout::kBlocksPerPacket) |
                Layout::kBlocksPerPacket);
        const auto refill =
            ring.Refill(hardware, 0, pipeline, nullptr, nullptr, &pipeline);
        ASSERT_TRUE(refill.ok);
        pipeline.PopulateClipStyleTxRingFromWrittenRange();
        ASSERT_TRUE(ring.PreparePayloads(hardware, 0, pipeline).ok);
        ASSERT_FALSE(pipeline.HasFatalFault());
    }

    EXPECT_EQ(control.counters.txPreparationDeadlineFaults.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_EQ(control.counters.txReadAheadFaults.load(std::memory_order_relaxed),
              0U);
    EXPECT_GE(control.counters.txCompletedPcmSlots.load(
                  std::memory_order_relaxed),
              200U);
    EXPECT_EQ(control.counters.txCompletedPayloadHashMismatches.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_GT(control.counters.txCompletedPayloadHashMatches.load(
                  std::memory_order_relaxed),
              0U);
    EXPECT_GT(control.txMinimumPreparationDistance.load(
                  std::memory_order_relaxed),
              Layout::kPreparationDeadlinePackets);
}

} // namespace
