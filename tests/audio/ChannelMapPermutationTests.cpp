// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ChannelMapPermutationTests.cpp
//
// Exit Criterion verification:
// An injected capture and playback channel map must demonstrably change RX and TX
// sample placement in buffer memory and wire packets.

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "Audio/Engine/Direct/DirectInputWriter.hpp"
#include "Audio/Engine/Direct/Rx/RxAudioPacketProcessor.hpp"
#include "Audio/Engine/Direct/Rx/RxCaptureChannelMap.hpp"
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Ports/IAmdtpTxSlotProvider.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"
#include "Audio/Wire/AMDTP/PcmSlotMap.hpp"
#include "Audio/Wire/AM824/Am824PayloadCodec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ASFW::Audio::Tests {

using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::Audio::Runtime::AudioGraphBinding;
using ASFW::Audio::Runtime::AudioStreamMemory;
using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::AudioEngine::Direct::DirectInputWriter;
using ASFW::AudioEngine::Direct::Rx::DirectRxWriteStatus;
using ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor;
using ASFW::AudioEngine::Direct::Rx::RxCaptureChannelMap;
using ASFW::Audio::Wire::PcmSlotMap;

namespace {

constexpr size_t kIsochHeaderBytes = 8;
constexpr uint32_t kChannels = 4;
constexpr uint32_t kDataBlockSize = 4;

void StoreBigEndian(uint8_t* dst, uint32_t value) noexcept {
    dst[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(value & 0xFF);
}

constexpr uint32_t MakeQuadlet0(uint8_t dbs, uint8_t dbc = 0) noexcept {
    return (static_cast<uint32_t>(dbs) << 16) | dbc;
}

constexpr uint32_t MakeQuadlet1(uint16_t syt, uint8_t fdf = 0x02) noexcept {
    return (1U << 31) | (0x10U << 24) | (static_cast<uint32_t>(fdf) << 16) | syt;
}

std::vector<uint8_t> MakeRxPacket(uint32_t dbs, const std::vector<uint32_t>& quadletData) {
    const size_t totalBytes = kIsochHeaderBytes + 8 + (quadletData.size() * 4);
    std::vector<uint8_t> packet(totalBytes, 0);
    StoreBigEndian(packet.data() + kIsochHeaderBytes, MakeQuadlet0(dbs, 0));
    StoreBigEndian(packet.data() + kIsochHeaderBytes + 4, MakeQuadlet1(0x1234, 0x02));
    for (size_t i = 0; i < quadletData.size(); ++i) {
        StoreBigEndian(packet.data() + kIsochHeaderBytes + 8 + (i * 4), quadletData[i]);
    }
    return packet;
}

struct RxTestFixture {
    AudioTransportControlBlock control{};
    std::array<float, 1024> inputBuffer{};
    AudioGraphBinding binding{};
    DirectInputWriter writer{};
    RxAudioPacketProcessor processor;

    RxTestFixture()
        : binding{
            .guid = 0x0011223344556677ULL,
            .sampleRateHz = 48000,
            .memory =
                AudioStreamMemory{
                    .inputBase = inputBuffer.data(),
                    .inputFrameCapacity = 256,
                    .inputChannels = kChannels,
                },
            .control = &control,
            .deviceToHostAm824Slots = kDataBlockSize,
        },
        writer{},
        processor{writer} {
        inputBuffer.fill(0.0f);
        writer.Bind(&binding);
    }
};

} // namespace

TEST(ChannelMapPermutationTests, RxInjectedMapDemonstrablyChangesSamplePlacement) {
    // Quadlet payload with 4 distinct recognizable samples in AM824 format
    // Slot 0: 0x40100000 -> Float ~ +0.125
    // Slot 1: 0x40200000 -> Float ~ +0.250
    // Slot 2: 0x40300000 -> Float ~ +0.375
    // Slot 3: 0x40400000 -> Float ~ +0.500
    const std::vector<uint32_t> wireQuadlets = {
        0x40100000, 0x40200000, 0x40300000, 0x40400000
    };
    const auto packet = MakeRxPacket(kDataBlockSize, wireQuadlets);

    // 1. Identity Map: Ch 0 -> Slot 0, Ch 1 -> Slot 1, Ch 2 -> Slot 2, Ch 3 -> Slot 3
    RxTestFixture identityFixture;
    RxCaptureChannelMap identityMap{};
    ASFW::Audio::Wire::Am824RxPayloadCodec codec(kDataBlockSize);
    auto resIdentity = identityFixture.processor.ProcessPacket(
        packet.data(), packet.size(), /*absoluteFrame=*/0,
        kChannels, codec,
        /*channelOffset=*/0, /*publishTimeline=*/false, identityMap);

    EXPECT_EQ(resIdentity.status, DirectRxWriteStatus::kAvailable);
    EXPECT_GT(resIdentity.framesDecoded, 0u);
    EXPECT_FALSE(resIdentity.mapRejected);
    const float ch0_identity = identityFixture.inputBuffer[0];
    const float ch1_identity = identityFixture.inputBuffer[1];
    const float ch2_identity = identityFixture.inputBuffer[2];
    const float ch3_identity = identityFixture.inputBuffer[3];

    // 2. Injected Permuted Map: Ch 0 -> Slot 3, Ch 1 -> Slot 2, Ch 2 -> Slot 1, Ch 3 -> Slot 0 (reverse)
    RxTestFixture permutedFixture;
    RxCaptureChannelMap permutedMap{};
    std::array<uint8_t, 4> reversedSlots = {3, 2, 1, 0};
    ASSERT_TRUE(permutedMap.SetSlots(reversedSlots));

    auto resPermuted = permutedFixture.processor.ProcessPacket(
        packet.data(), packet.size(), /*absoluteFrame=*/0,
        kChannels, codec,
        /*channelOffset=*/0, /*publishTimeline=*/false, permutedMap);

    EXPECT_EQ(resPermuted.status, DirectRxWriteStatus::kAvailable);
    EXPECT_GT(resPermuted.framesDecoded, 0u);
    EXPECT_FALSE(resPermuted.mapRejected);
    const float ch0_permuted = permutedFixture.inputBuffer[0];
    const float ch1_permuted = permutedFixture.inputBuffer[1];
    const float ch2_permuted = permutedFixture.inputBuffer[2];
    const float ch3_permuted = permutedFixture.inputBuffer[3];

    // Assert that sample placement has demonstrably changed:
    // ch0 received Slot 3 instead of Slot 0
    EXPECT_NE(ch0_permuted, ch0_identity);
    EXPECT_FLOAT_EQ(ch0_permuted, ch3_identity);

    // ch1 received Slot 2 instead of Slot 1
    EXPECT_NE(ch1_permuted, ch1_identity);
    EXPECT_FLOAT_EQ(ch1_permuted, ch2_identity);

    // ch2 received Slot 1 instead of Slot 2
    EXPECT_NE(ch2_permuted, ch2_identity);
    EXPECT_FLOAT_EQ(ch2_permuted, ch1_identity);

    // ch3 received Slot 0 instead of Slot 3
    EXPECT_NE(ch3_permuted, ch3_identity);
    EXPECT_FLOAT_EQ(ch3_permuted, ch0_identity);
}

TEST(ChannelMapPermutationTests, RxOutOfBoundsMapFailsClosedToIdentity) {
    const std::vector<uint32_t> wireQuadlets = {
        0x40100000, 0x40200000, 0x40300000, 0x40400000
    };
    const auto packet = MakeRxPacket(kDataBlockSize, wireQuadlets);

    RxTestFixture fixture;
    RxCaptureChannelMap invalidMap{};
    // Map with a slot 7 which exceeds kDataBlockSize (4)
    std::array<uint8_t, 4> badSlots = {0, 1, 7, 3};
    ASSERT_TRUE(invalidMap.SetSlots(badSlots));
    EXPECT_FALSE(invalidMap.FitsWithin(kChannels, kDataBlockSize));

    ASFW::Audio::Wire::Am824RxPayloadCodec codec(kDataBlockSize);
    auto res = fixture.processor.ProcessPacket(
        packet.data(), packet.size(), /*absoluteFrame=*/0,
        kChannels, codec,
        /*channelOffset=*/0, /*publishTimeline=*/false, invalidMap);

    EXPECT_EQ(res.status, DirectRxWriteStatus::kAvailable);
    EXPECT_GT(res.framesDecoded, 0u);
    // Verified: processor detected invalid map, set mapRejected and decoded via identity fast path
    EXPECT_TRUE(res.mapRejected);
    EXPECT_NEAR(fixture.inputBuffer[0], 0.125f, 0.01f);
    EXPECT_NEAR(fixture.inputBuffer[1], 0.250f, 0.01f);
    EXPECT_NEAR(fixture.inputBuffer[2], 0.375f, 0.01f);
    EXPECT_NEAR(fixture.inputBuffer[3], 0.500f, 0.01f);
}

TEST(ChannelMapPermutationTests, TxInjectedMapDemonstrablyChangesWirePlacement) {
    // 4-channel host buffer with distinct non-zero float values
    // Ch 0 = 0.25f, Ch 1 = 0.50f, Ch 2 = 0.75f, Ch 3 = 1.00f
    std::array<float, 4> hostSamples = {0.25f, 0.50f, 0.75f, 1.00f};
    HostAudioBufferView hostBuffer{
        .interleavedFloat32 = hostSamples.data(),
        .firstFrame = 0,
        .frameCount = 1,
        .frameCapacity = 1,
        .channels = kChannels,
    };

    AmdtpStreamConfig config{};
    config.streamMode = StreamMode::Blocking;
    config.dbs = kChannels;
    config.pcmChannels = kChannels;
    config.framesPerDataPacket = 1;
    config.maxPacketBytes = 128;

    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.nextDataSyt = 0x1234;
    timing.replayValid = true;
    timing.replayDataBlocks = 1;

    // 1. Identity Playback Map
    AmdtpPacketTimeline timelineIdentity{};
    std::array<PacketTimelineSlot, 8> slotsIdentity{};
    ASSERT_TRUE(timelineIdentity.AttachSlots(slotsIdentity.data(), slotsIdentity.size()));

    AmdtpTxPolicy policyIdentity{};
    AmdtpTxPacketizer packetizerIdentity{};
    AmdtpPayloadWriter writerIdentity{};
    packetizerIdentity.BindTimeline(&timelineIdentity);
    ASSERT_TRUE(packetizerIdentity.Configure(config, policyIdentity));
    writerIdentity.Configure(config, policyIdentity);
    writerIdentity.BindTimeline(&timelineIdentity);

    std::array<uint8_t, 128> bytesIdentity{};
    PreparedTxPacket preparedIdentity{};
    ASSERT_TRUE(packetizerIdentity.PrepareNextPacket(
        {0, bytesIdentity.data(), bytesIdentity.size()}, timing, preparedIdentity));
    writerIdentity.WriteFloat32Interleaved(hostBuffer, /*completionCursor=*/1);

    // Read back the AM824 slots from the identity packet (after 8 bytes CIP header)
    const uint8_t* payloadIdentity = bytesIdentity.data() + 8;
    const uint32_t slot0_identity = (static_cast<uint32_t>(payloadIdentity[0]) << 24) |
                                    (static_cast<uint32_t>(payloadIdentity[1]) << 16) |
                                    (static_cast<uint32_t>(payloadIdentity[2]) << 8) |
                                    payloadIdentity[3];
    const uint32_t slot1_identity = (static_cast<uint32_t>(payloadIdentity[4]) << 24) |
                                    (static_cast<uint32_t>(payloadIdentity[5]) << 16) |
                                    (static_cast<uint32_t>(payloadIdentity[6]) << 8) |
                                    payloadIdentity[7];
    const uint32_t slot2_identity = (static_cast<uint32_t>(payloadIdentity[8]) << 24) |
                                    (static_cast<uint32_t>(payloadIdentity[9]) << 16) |
                                    (static_cast<uint32_t>(payloadIdentity[10]) << 8) |
                                    payloadIdentity[11];
    const uint32_t slot3_identity = (static_cast<uint32_t>(payloadIdentity[12]) << 24) |
                                    (static_cast<uint32_t>(payloadIdentity[13]) << 16) |
                                    (static_cast<uint32_t>(payloadIdentity[14]) << 8) |
                                    payloadIdentity[15];

    // 2. Permuted Playback Map: reverse order (Ch 0 -> Slot 3, Ch 1 -> Slot 2, Ch 2 -> Slot 1, Ch 3 -> Slot 0)
    AmdtpPacketTimeline timelinePermuted{};
    std::array<PacketTimelineSlot, 8> slotsPermuted{};
    ASSERT_TRUE(timelinePermuted.AttachSlots(slotsPermuted.data(), slotsPermuted.size()));

    AmdtpTxPolicy policyPermuted{};
    std::array<uint8_t, 4> reversedSlots = {3, 2, 1, 0};
    ASSERT_TRUE(policyPermuted.playbackChannelMap.SetSlots(reversedSlots));

    AmdtpTxPacketizer packetizerPermuted{};
    AmdtpPayloadWriter writerPermuted{};
    packetizerPermuted.BindTimeline(&timelinePermuted);
    ASSERT_TRUE(packetizerPermuted.Configure(config, policyPermuted));
    writerPermuted.Configure(config, policyPermuted);
    writerPermuted.BindTimeline(&timelinePermuted);

    std::array<uint8_t, 128> bytesPermuted{};
    PreparedTxPacket preparedPermuted{};
    ASSERT_TRUE(packetizerPermuted.PrepareNextPacket(
        {0, bytesPermuted.data(), bytesPermuted.size()}, timing, preparedPermuted));
    writerPermuted.WriteFloat32Interleaved(hostBuffer, /*completionCursor=*/1);

    const uint8_t* payloadPermuted = bytesPermuted.data() + 8;
    const uint32_t slot0_permuted = (static_cast<uint32_t>(payloadPermuted[0]) << 24) |
                                    (static_cast<uint32_t>(payloadPermuted[1]) << 16) |
                                    (static_cast<uint32_t>(payloadPermuted[2]) << 8) |
                                    payloadPermuted[3];
    const uint32_t slot1_permuted = (static_cast<uint32_t>(payloadPermuted[4]) << 24) |
                                    (static_cast<uint32_t>(payloadPermuted[5]) << 16) |
                                    (static_cast<uint32_t>(payloadPermuted[6]) << 8) |
                                    payloadPermuted[7];
    const uint32_t slot2_permuted = (static_cast<uint32_t>(payloadPermuted[8]) << 24) |
                                    (static_cast<uint32_t>(payloadPermuted[9]) << 16) |
                                    (static_cast<uint32_t>(payloadPermuted[10]) << 8) |
                                    payloadPermuted[11];
    const uint32_t slot3_permuted = (static_cast<uint32_t>(payloadPermuted[12]) << 24) |
                                    (static_cast<uint32_t>(payloadPermuted[13]) << 16) |
                                    (static_cast<uint32_t>(payloadPermuted[14]) << 8) |
                                    payloadPermuted[15];

    // Assert that sample placement in wire packet bytes has demonstrably changed:
    // Slot 0 has Ch 3 sample instead of Ch 0 sample
    EXPECT_NE(slot0_permuted, slot0_identity);
    EXPECT_EQ(slot0_permuted, slot3_identity);

    // Slot 1 has Ch 2 sample instead of Ch 1 sample
    EXPECT_NE(slot1_permuted, slot1_identity);
    EXPECT_EQ(slot1_permuted, slot2_identity);

    // Slot 2 has Ch 1 sample instead of Ch 2 sample
    EXPECT_NE(slot2_permuted, slot2_identity);
    EXPECT_EQ(slot2_permuted, slot1_identity);

    // Slot 3 has Ch 0 sample instead of Ch 3 sample
    EXPECT_NE(slot3_permuted, slot3_identity);
    EXPECT_EQ(slot3_permuted, slot0_identity);
}

} // namespace ASFW::Audio::Tests
