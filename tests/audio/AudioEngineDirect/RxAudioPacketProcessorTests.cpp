// SPDX-License-Identifier: Apache-2.0
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
constexpr uint32_t MakeQuadlet1(uint16_t syt, uint8_t fdf = 0x02) noexcept {
    return (1U << 31) | (0x10U << 24) | (static_cast<uint32_t>(fdf) << 16) | syt;
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
            .endpointId = ASFW::Audio::Devices::AudioEndpointId{1},
            .sampleRateHz = 48000,
            .memory =
                AudioStreamMemory{
                    .inputBase = inputBuffer.data(),
                    .activeInputRingFrames = 512,
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

TEST(RxAudioPacketProcessorTests, PayloadBearingNoDataWithFdf0xFFIsTreatedAsZeroDataBlocks) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    // FMT 0x10, FDF 0xFF, SYT 0xFFFF. Even if trailing payload bytes are present,
    // AM824 FDF 0xFF means NO-DATA (IEC 61883-6 §5.2 / §5.3).
    // framesDecoded must be 0 and no MIDI must be extracted.
    const auto packet = MakePacket(MakeQuadlet0(kSlots), MakeQuadlet1(0xFFFF, 0xFF), kSlots, 2);

    struct TestSink : ASFW::Audio::Ports::IMidiByteSink {
        uint32_t delivered{0};
        void DeliverMidiBytes(uint8_t, const uint8_t*, uint8_t n, uint64_t) noexcept override { delivered += n; }
        void MarkMidiDiscontinuity(uint8_t) noexcept override {}
    } sink;

    ASFW::AudioEngine::Direct::Rx::RxMidiExtraction extraction{
        .sink = &sink,
        .geometry = {
            .dbs = kSlots,
            .midiSlotIndex = 1,
            .portCount = 1,
            .dbcAligned = true,
        },
    };

    const auto result = processor.ProcessPacket(
        packet.data(), packet.size(), 0, 1, kSlots,
        ASFW::Encoding::AudioWireFormat::kAM824, 0, false, {}, false, extraction);

    EXPECT_EQ(result.status, DirectRxWriteStatus::kAvailable);
    EXPECT_TRUE(result.hasValidCip);
    EXPECT_EQ(result.fdf, 0xFF);
    EXPECT_EQ(result.framesDecoded, 0u);
    EXPECT_EQ(result.midiBytesDelivered, 0u);
    EXPECT_EQ(sink.delivered, 0u);
}

} // namespace ASFW::Tests::AudioEngineDirect
