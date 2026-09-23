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
#include "Audio/Wire/AM824/Am824PayloadCodec.hpp"
#include "Audio/Wire/MOTU/MotuPayloadCodec.hpp"

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
    ASFW::Audio::Wire::Am824RxPayloadCodec codec(am824Slots);
    return processor.ProcessPacket(packet.data(), packet.size(), /*absoluteFrame=*/0,
                                   channels, codec,
                                   /*channelOffset=*/0, /*publishTimeline=*/true);
}


//==============================================================================
// MOTU protocol-v2 fixtures.
//
// MOTU sets the CIP SPH bit and packs 3-byte PCM chunks from byte offset 10 of each
// data block. dbs is therefore SMALLER than the channel count for any stream wider than
// about four channels -- 14 chunks give dbs 13 -- which is exactly what the AM824
// geometry rule rejects.
//==============================================================================

/// CIP quadlet 1 for MOTU: FMT 0x02, FDF 0x22, SPH bit set (amdtp-motu.c:19-25).
constexpr uint32_t MakeMotuQuadlet1() noexcept {
    return (1U << 31) | (0x02U << 24) | (0x22U << 16) | (1U << 30);
}

/// Data block quadlets for `chunks` PCM chunks: SPH quadlet + 2 message chunks + PCM,
/// padded to quadlet alignment.
constexpr uint32_t MotuDbs(uint32_t chunks) noexcept {
    return 1U + (((2U + chunks) * 3U) + 3U) / 4U;
}

/// One MOTU packet whose every block carries `chunkValue` in each PCM chunk.
std::vector<uint8_t> MakeMotuPacket(uint32_t chunks, uint32_t dataBlocks,
                                    int32_t sampleTop24 = 0) {
    const uint32_t dbs = MotuDbs(chunks);
    std::vector<uint8_t> packet(kIsochHeaderBytes + 8 + (dbs * dataBlocks * 4), 0);
    StoreBigEndian(packet.data() + kIsochHeaderBytes, MakeQuadlet0(static_cast<uint8_t>(dbs)));
    StoreBigEndian(packet.data() + kIsochHeaderBytes + 4, MakeMotuQuadlet1());

    uint8_t* blocks = packet.data() + kIsochHeaderBytes + 8;
    for (uint32_t b = 0; b < dataBlocks; ++b) {
        uint8_t* block = blocks + b * dbs * 4U;
        for (uint32_t c = 0; c < chunks; ++c) {
            uint8_t* chunk = block + 10U + c * 3U;
            const auto v = static_cast<uint32_t>(sampleTop24);
            chunk[0] = static_cast<uint8_t>(v >> 24);
            chunk[1] = static_cast<uint8_t>(v >> 16);
            chunk[2] = static_cast<uint8_t>(v >> 8);
        }
    }
    return packet;
}

ASFW::AudioEngine::Direct::Rx::RxAudioPacketProcessorResult
ProcessMotu(RxAudioPacketProcessor& processor,
            const std::vector<uint8_t>& packet,
            uint32_t channels,
            uint32_t motuPcmChunks,
            uint32_t channelOffset = 0) {
    ASFW::Audio::Wire::MotuRxPayloadCodec codec(motuPcmChunks);
    return processor.ProcessPacket(packet.data(), packet.size(), /*absoluteFrame=*/0,
                                   channels, codec,
                                   channelOffset, /*publishTimeline=*/true);
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

TEST(RxAudioPacketProcessorTests, WrongDbsQuirkTakesStrideFromConfiguration) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    // Loud OXFW case (snd-oxfw SND_OXFW_QUIRK_WRONG_DBS): the device lays out
    // data blocks at the negotiated width (kSlots) but stamps a wrong dbs in
    // the CIP header. With the quirk, the configured stride is the authority
    // and the packet decodes; the lying header value stays visible in
    // result.dbs for telemetry.
    constexpr uint8_t kLyingHeaderDbs = 16;
    constexpr uint32_t kDataBlocks = 2;
    const auto packet = MakePacket(MakeQuadlet0(kLyingHeaderDbs), MakeQuadlet1(0x1000),
                                   kSlots, kDataBlocks);

    ASFW::Audio::Wire::Am824RxPayloadCodec trustedCodec(kSlots, /*trustConfiguredStride=*/true);
    const auto trusted = processor.ProcessPacket(
        packet.data(), packet.size(), /*absoluteFrame=*/0, kSlots, trustedCodec,
        /*channelOffset=*/0, /*publishTimeline=*/true);
    EXPECT_EQ(trusted.status, DirectRxWriteStatus::kAvailable);
    EXPECT_EQ(trusted.framesDecoded, kDataBlocks);
    EXPECT_EQ(trusted.dbs, kLyingHeaderDbs);

    // Without the quirk, the same packet is the stalled-capture illusion this
    // quirk exists to fix: the oversized header stride swallows the payload and
    // the packet decodes as zero events — indistinguishable from a device that
    // went quiet. Pin that symptom so the quirk's value stays visible.
    ASFW::Audio::Wire::Am824RxPayloadCodec untrustedCodec(kSlots, /*trustConfiguredStride=*/false);
    const auto untrusted = processor.ProcessPacket(
        packet.data(), packet.size(), /*absoluteFrame=*/0, kSlots, untrustedCodec,
        /*channelOffset=*/0, /*publishTimeline=*/true);
    EXPECT_EQ(untrusted.status, DirectRxWriteStatus::kAvailable);
    EXPECT_EQ(untrusted.framesDecoded, 0U);
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


//==============================================================================
// MOTU protocol-v2
//==============================================================================

TEST(RxAudioPacketProcessorTests, MotuAcceptsABlockNarrowerThanItsChannelCount) {
    // The case this branch exists for. A 14-chunk MOTU stream has dbs 13, so the AM824
    // rule (dbs must be >= channels, and must equal the negotiated slot count) rejects
    // every packet the device sends.
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    constexpr uint32_t kChunks = 14;
    ASSERT_LT(MotuDbs(kChunks), kChunks) << "fixture must exercise dbs < channels";

    const auto packet = MakeMotuPacket(kChunks, /*dataBlocks=*/2);
    const auto result = ProcessMotu(processor, packet, /*channels=*/4, kChunks);

    EXPECT_EQ(result.status, DirectRxWriteStatus::kAvailable);
    EXPECT_TRUE(result.hasValidCip);
    EXPECT_EQ(result.framesDecoded, 2u);
}

TEST(RxAudioPacketProcessorTests, MotuDecodesChunkValuesIntoHostFrames) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    constexpr uint32_t kChunks = 4;
    // Full positive scale in the top 24 bits.
    const int32_t sample = static_cast<int32_t>(static_cast<uint32_t>(8388607) << 8);
    const auto packet = MakeMotuPacket(kChunks, /*dataBlocks=*/1, sample);

    const auto result = ProcessMotu(processor, packet, /*channels=*/kChunks, kChunks);
    ASSERT_EQ(result.status, DirectRxWriteStatus::kAvailable);

    for (uint32_t ch = 0; ch < kChunks; ++ch) {
        EXPECT_NEAR(fixture.inputBuffer[ch], 1.0f, 1e-6f) << "channel " << ch;
    }
}

TEST(RxAudioPacketProcessorTests, MotuNeverReadsPastAPayloadTooShortForOneBlock) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    // Packet built for 4 chunks (one 24-byte block), processed as a 14-chunk stream whose
    // blocks are 52 bytes. MOTU blocks are sized from the configured geometry now, not
    // the header, so the hazard this guarded -- reading past a block into the next -- is
    // closed by construction: only whole configured blocks inside the payload decode.
    const auto packet = MakeMotuPacket(/*chunks=*/4, /*dataBlocks=*/1);
    const auto result = ProcessMotu(processor, packet, /*channels=*/4, /*motuPcmChunks=*/14);

    EXPECT_EQ(result.framesDecoded, 0u);
    EXPECT_EQ(result.strideQuadlets, MotuDbs(14));
}

TEST(RxAudioPacketProcessorTests, MotuIgnoresTheWrongDbsInTheUltraLiteHeader) {
    // The regression this pins. The UltraLite puts a wrong DBS in its CIP header; Linux
    // sets CIP_WRONG_DBS for it (amdtp-motu.c:458-463) and divides the payload by the
    // configured block size. Trusting the header turned each 8-block packet into 5 read
    // at the wrong stride, so capture ran at 5/8 speed and the device crackled.
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    constexpr uint32_t kChunks = 14;
    constexpr uint32_t kBlocks = 8;
    const int32_t sample = static_cast<int32_t>(static_cast<uint32_t>(4194304) << 8); // 0.5
    auto packet = MakeMotuPacket(kChunks, kBlocks, sample);

    // Replace the header's DBS with a wrong value of the kind that yields 5 blocks.
    constexpr uint8_t kWrongDbs = 19;
    ASSERT_EQ((kBlocks * MotuDbs(kChunks)) / kWrongDbs, 5u) << "fixture must reproduce 5";
    StoreBigEndian(packet.data() + kIsochHeaderBytes, MakeQuadlet0(kWrongDbs));

    const auto result = ProcessMotu(processor, packet, /*channels=*/4, kChunks);
    ASSERT_EQ(result.status, DirectRxWriteStatus::kAvailable);
    EXPECT_EQ(result.framesDecoded, kBlocks);
    // Both halves: we reported the lie the device told, and we did not decode with it.
    EXPECT_EQ(result.dbs, kWrongDbs);
    EXPECT_EQ(result.strideQuadlets, MotuDbs(kChunks));

    // Every block decodes at the configured stride -- a wrong stride reads the SPH and
    // message bytes of later blocks as samples, so the last frame would be garbage.
    for (uint32_t frame = 0; frame < kBlocks; ++frame) {
        for (uint32_t ch = 0; ch < 4; ++ch) {
            EXPECT_NEAR(fixture.inputBuffer[frame * kSlots + ch], 0.5f, 1e-6f)
                << "frame " << frame << " channel " << ch;
        }
    }
}

TEST(RxAudioPacketProcessorTests, MotuRejectsZeroChunkCount) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    const auto packet = MakeMotuPacket(/*chunks=*/4, /*dataBlocks=*/1);
    const auto result = ProcessMotu(processor, packet, /*channels=*/4, /*motuPcmChunks=*/0);

    EXPECT_EQ(result.status, DirectRxWriteStatus::kGeometryMismatch);
}

TEST(RxAudioPacketProcessorTests, MotuRejectsASliceRunningPastTheChunkCount) {
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    // channelOffset 2 + 4 channels needs 6 chunks; the device carries 4.
    const auto packet = MakeMotuPacket(/*chunks=*/4, /*dataBlocks=*/1);
    const auto result =
        ProcessMotu(processor, packet, /*channels=*/4, /*motuPcmChunks=*/4, /*channelOffset=*/2);

    EXPECT_EQ(result.status, DirectRxWriteStatus::kGeometryMismatch);
}

TEST(RxAudioPacketProcessorTests, Am824PathIsUnaffectedByTheMotuBranch) {
    // Guards the shared file: the AM824 geometry rule must still reject a slot-count
    // disagreement exactly as before, and a well-formed AM824 packet must still decode.
    Fixture fixture;
    RxAudioPacketProcessor processor(fixture.writer);

    const auto good = MakePacket(MakeQuadlet0(kSlots), MakeQuadlet1(0x1000), kSlots, 1);
    EXPECT_EQ(Process(processor, good).status, DirectRxWriteStatus::kAvailable);

    // am824Slots disagreeing with dbs is still a geometry mismatch.
    EXPECT_EQ(Process(processor, good, kSlots, kSlots + 1).status,
              DirectRxWriteStatus::kGeometryMismatch);
}


} // namespace ASFW::Tests::AudioEngineDirect
