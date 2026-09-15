// SPDX-License-Identifier: Apache-2.0
// Modified in 2026 by Rafal Zalech to add original MOTU UltraLite support.
// Copyright (c) 2026 ASFireWire Project
//
// RxAudioPacketProcessorTests.cpp
// Pins the RX decode outcome split.
//
// These four faults used to share one status value (kInvalidRange), which made a
// stalled bring-up unattributable: a device sending only CIP NO-DATA and a host
// rejecting every packet the device sent looked identical from outside. Each
// case below must keep its own status, and a NO-DATA packet must stay
// distinguishable from a rejected one.

#include "Audio/DriverKit/Runtime/AudioGraphBinding.hpp"
#include "Audio/Engine/Direct/DirectInputWriter.hpp"
#include "Audio/Engine/Direct/Rx/RxAudioPacketProcessor.hpp"
#include "Audio/Wire/MOTU/MotuBlockCodec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ASFW::Tests::AudioEngineDirect {

using ASFW::Audio::Runtime::AudioGraphBinding;
using ASFW::Audio::Runtime::AudioStreamMemory;
using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::AudioEngine::Direct::DirectInputWriter;
using ASFW::AudioEngine::Direct::Rx::DirectRxWriteStatus;
using ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessor;

namespace {

constexpr size_t kIsochHeaderBytes = 8;
constexpr uint32_t kSlots = 4; // PCM channels == AM824 slots for these fixtures

void StoreBigEndian(uint8_t* dst, uint32_t value) noexcept {
    dst[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(value & 0xFF);
}

/// CIP quadlet 0: EOH_0 must be 0. DBS at [23:16]. (IEC 61883-1 6.2.1)
constexpr uint32_t MakeQuadlet0(uint8_t dbs, uint8_t dbc = 0) noexcept {
    return (static_cast<uint32_t>(dbs) << 16) | dbc;
}

/// CIP quadlet 1: EOH_1 must be 1. FMT 0x10 = AM824. SYT at [15:0].
constexpr uint32_t MakeQuadlet1(uint16_t syt, uint8_t fdf = 0x02,
                                uint8_t fmt = 0x10) noexcept {
    return (1U << 31) | (static_cast<uint32_t>(fmt) << 24) |
           (static_cast<uint32_t>(fdf) << 16) | syt;
}

/// Build one isoch packet: 8-byte isoch header, two CIP quadlets, then
/// `dataBlocks` blocks of `dbs` quadlets each.
std::vector<uint8_t> MakePacket(uint32_t quadlet0,
                                uint32_t quadlet1,
                                uint32_t dbs,
                                uint32_t dataBlocks) {
    std::vector<uint8_t> packet(kIsochHeaderBytes + 8 + (dbs * dataBlocks * 4), 0);
    StoreBigEndian(packet.data() + kIsochHeaderBytes, quadlet0);
    StoreBigEndian(packet.data() + kIsochHeaderBytes + 4, quadlet1);
    return packet;
}

struct Fixture {
    AudioTransportControlBlock control{};
    std::array<float, 4096> inputBuffer{};
    AudioGraphBinding binding{};
    DirectInputWriter writer{};

    Fixture() {
        binding = AudioGraphBinding{
            .guid = 0x0011223344556677ULL,
            .sampleRateHz = 48000,
            .memory =
                AudioStreamMemory{
                    .inputBase = inputBuffer.data(),
                    .inputFrameCapacity = 512,
                    .inputChannels = kSlots,
                },
            .control = &control,
            .deviceToHostAm824Slots = kSlots,
        };
        writer.Bind(&binding);
    }
};

ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessorResult
Process(RxAudioPacketProcessor& processor,
        const std::vector<uint8_t>& packet,
        uint32_t channels = kSlots,
        uint32_t am824Slots = kSlots) {
    return processor.ProcessPacket(packet.data(), packet.size(), /*absoluteFrame=*/0,
                                   channels, am824Slots,
                                   ASFW::Encoding::AudioWireFormat::kAM824,
                                   /*channelOffset=*/0, /*publishTimeline=*/true);
}

} // namespace

TEST(RxAudioPacketProcessorTests, ShortPacketIsNotConfusedWithABadHeader) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    // Too small to even hold the isoch header plus the two CIP quadlets.
    std::vector<uint8_t> runt(kIsochHeaderBytes + 4, 0);
    const auto result = Process(processor, runt);

    EXPECT_EQ(result.status, DirectRxWriteStatus::kShortPacket);
    EXPECT_FALSE(result.hasValidCip);
}

TEST(RxAudioPacketProcessorTests, UndecodableCipHeaderReportsInvalidCipHeader) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    // EOH_0 set on the first quadlet is illegal, so Decode rejects it.
    const auto packet = MakePacket(MakeQuadlet0(kSlots) | (1U << 31),
                                   MakeQuadlet1(0x1000), kSlots, 1);
    const auto result = Process(processor, packet);

    EXPECT_EQ(result.status, DirectRxWriteStatus::kInvalidCipHeader);
    EXPECT_FALSE(result.hasValidCip);
}

TEST(RxAudioPacketProcessorTests, ZeroDataBlockSizeIsItsOwnStatus) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    // Header decodes, but DBS == 0 leaves no event size to derive.
    const auto packet = MakePacket(MakeQuadlet0(0), MakeQuadlet1(0x1000), kSlots, 1);
    const auto result = Process(processor, packet);

    EXPECT_EQ(result.status, DirectRxWriteStatus::kZeroDataBlockSize);
    EXPECT_TRUE(result.hasValidCip);
}

TEST(RxAudioPacketProcessorTests, GeometryDisagreementReportsGeometryMismatch) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    // The device's data block is 8 quadlets wide; our stream expects 4. This is
    // the profile-vs-device disagreement, and it must not look like a device
    // that went quiet.
    constexpr uint32_t kDeviceDbs = 8;
    const auto packet =
        MakePacket(MakeQuadlet0(kDeviceDbs), MakeQuadlet1(0x1000), kDeviceDbs, 2);
    const auto result = Process(processor, packet, kSlots, kSlots);

    EXPECT_EQ(result.status, DirectRxWriteStatus::kGeometryMismatch);
    EXPECT_TRUE(result.hasValidCip);
    EXPECT_EQ(result.dbs, kDeviceDbs);
}

TEST(RxAudioPacketProcessorTests, NoDataPacketIsAcceptedAndCarriesSytNoInfo) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    // A genuine CIP NO-DATA packet: valid header, SYT 0xFFFF, zero data blocks.
    // This is the shape a device sends while it has nothing to transmit, and the
    // consumer must be able to tell it apart from every reject above -- it is
    // accepted, not rejected, and the SYT is what identifies it.
    const auto packet = MakePacket(MakeQuadlet0(kSlots), MakeQuadlet1(0xFFFF), kSlots, 0);
    const auto result = Process(processor, packet);

    EXPECT_EQ(result.status, DirectRxWriteStatus::kAvailable);
    EXPECT_TRUE(result.hasValidCip);
    EXPECT_EQ(result.syt, 0xFFFF);
    EXPECT_EQ(result.framesDecoded, 0u);
}

TEST(RxAudioPacketProcessorTests, DataPacketDecodesAndReportsItsSyt) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    const auto packet = MakePacket(MakeQuadlet0(kSlots), MakeQuadlet1(0x1234), kSlots, 3);
    const auto result = Process(processor, packet);

    EXPECT_EQ(result.status, DirectRxWriteStatus::kAvailable);
    EXPECT_TRUE(result.hasValidCip);
    EXPECT_EQ(result.syt, 0x1234);
    EXPECT_EQ(result.framesDecoded, 3u);
}

TEST(RxAudioPacketProcessorTests, MotuPacketUsesConfiguredWidthAndDecodesPackedPcmAndSph) {
    AudioTransportControlBlock control{};
    std::array<float, 14 * 32> inputBuffer{};
    AudioGraphBinding binding{
        .guid = 0x0001f20000085f25ULL,
        .sampleRateHz = 48000,
        .memory = AudioStreamMemory{
            .inputBase = inputBuffer.data(),
            .inputFrameCapacity = 32,
            .inputChannels = 14,
        },
        .control = &control,
        .deviceToHostAm824Slots = 13,
    };
    DirectInputWriter writer{};
    writer.Bind(&binding);
    RxAudioPacketProcessor processor(writer);

    constexpr uint32_t kDbs = 13;
    constexpr uint32_t kFrames = 6;
    // Real MOTU v2 capture can report zero in CIP DBS. The profile's fixed
    // 13-quadlet geometry must remain authoritative.
    auto packet = MakePacket(MakeQuadlet0(0),
                             MakeQuadlet1(0xFFFF, 0x22, 0x02),
                             kDbs, kFrames);
    for (uint32_t frame = 0; frame < kFrames; ++frame) {
        auto* bytes = packet.data() + kIsochHeaderBytes + 8 + frame * kDbs * 4;
        std::span<uint8_t> block(bytes, kDbs * 4);
        ASFW::Encoding::Motu::WriteSph(block, (100U + frame) << 12);
        ASFW::Encoding::Motu::WritePcmSample(
            block.subspan(ASFW::Encoding::Motu::kPcmByteOffset, 3),
            frame == 0 ? 0x40000000 : 0);
    }

    const auto result = processor.ProcessPacket(
        packet.data(), packet.size(), 0, 14, 13,
        ASFW::Encoding::AudioWireFormat::kMotuV2, 0, true);
    EXPECT_EQ(result.status, DirectRxWriteStatus::kAvailable);
    EXPECT_EQ(result.framesDecoded, kFrames);
    EXPECT_EQ(result.motuSphCount, kFrames);
    EXPECT_EQ(result.motuSph[0], 100U << 12);
    EXPECT_NEAR(inputBuffer[0], 0.5F, 0.000001F);
    EXPECT_EQ(inputBuffer[1], 0.0F);
}

TEST(RxAudioPacketProcessorTests, MotuUltraLiteMapsPhysicalInputsAheadOfMixReturn) {
    AudioTransportControlBlock control{};
    std::array<float, 14 * 8> inputBuffer{};
    AudioGraphBinding binding{
        .guid = 0x0001f20000085f25ULL,
        .sampleRateHz = 48000,
        .memory = AudioStreamMemory{
            .inputBase = inputBuffer.data(),
            .inputFrameCapacity = 8,
            .inputChannels = 14,
        },
        .control = &control,
        .deviceToHostAm824Slots = 13,
    };
    DirectInputWriter writer{};
    writer.Bind(&binding);
    RxAudioPacketProcessor processor(writer);

    constexpr uint32_t kDbs = 13;
    auto packet = MakePacket(MakeQuadlet0(0),
                             MakeQuadlet1(0xFFFF, 0x22, 0x02),
                             kDbs, 1);
    auto* bytes = packet.data() + kIsochHeaderBytes + 8;
    std::span<uint8_t> block(bytes, kDbs * 4);
    ASFW::Encoding::Motu::WritePcmSample(
        block.subspan(ASFW::Encoding::Motu::kPcmByteOffset, 3),
        0x20000000); // raw channel 1: Mix1 return
    ASFW::Encoding::Motu::WritePcmSample(
        block.subspan(ASFW::Encoding::Motu::kPcmByteOffset + 2 * 3, 3),
        0x40000000); // raw channel 3: physical analog input 1

    const auto result = processor.ProcessPacket(
        packet.data(), packet.size(), 0, 14, 13,
        ASFW::Encoding::AudioWireFormat::kMotuV2, 0, true,
        &ASFW::Encoding::Motu::kUltraLiteInputWireChannelForHostChannel);

    ASSERT_EQ(result.status, DirectRxWriteStatus::kAvailable);
    EXPECT_NEAR(inputBuffer[0], 0.5F, 0.000001F);
    EXPECT_NEAR(inputBuffer[10], 0.25F, 0.000001F);
}

TEST(RxAudioPacketProcessorTests, EveryRejectStatusIsDistinct) {
    // The whole point of the split: no two faults share a value. If someone
    // collapses these again, the bring-up diagnosis stops working.
    const std::array<DirectRxWriteStatus, 5> statuses{
        DirectRxWriteStatus::kInvalidRange,
        DirectRxWriteStatus::kShortPacket,
        DirectRxWriteStatus::kInvalidCipHeader,
        DirectRxWriteStatus::kZeroDataBlockSize,
        DirectRxWriteStatus::kGeometryMismatch,
    };
    for (size_t i = 0; i < statuses.size(); ++i) {
        for (size_t j = i + 1; j < statuses.size(); ++j) {
            EXPECT_NE(statuses[i], statuses[j]);
        }
    }
}

} // namespace ASFW::Tests::AudioEngineDirect
