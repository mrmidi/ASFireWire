// SPDX-License-Identifier: Apache-2.0
//
// MotuPayloadReader tests.
//
// Capture-side counterpart to MotuPayloadWriterTests: 3-byte big-endian chunks from byte
// offset 10, normalised to float on the same scale the AMDTP path uses so MOTU and DICE
// produce identically scaled host audio. Cross-validated with amdtp-motu.c:93-187.

#include "Audio/Wire/MOTU/MotuPayloadReader.hpp"
#include "Audio/Wire/MOTU/MotuPayloadWriter.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using ASFW::Encoding::Motu::BlockAt;
using ASFW::Encoding::Motu::DataBlocksInPayload;
using ASFW::Encoding::Motu::DecodeMotuBlock;
using ASFW::Encoding::Motu::kBytesPerChunk;
using ASFW::Encoding::Motu::kPcmByteOffset;
using ASFW::Encoding::Motu::kUltraLiteCapture;
using ASFW::Encoding::Motu::DataBlockBytes;
using ASFW::Encoding::Motu::Signed24ToFloat32;
using ASFW::Encoding::Motu::WritePcmSample;
using ASFW::Protocols::Audio::AMDTP::PcmSlotCodec;

constexpr uint32_t kCipHeaderBytes = 8;
constexpr uint32_t kDbs = 4;
constexpr uint32_t kBlockBytes = kDbs * 4;

/// A single data block with `chunks` PCM chunks written from `samples` (float).
[[nodiscard]] std::vector<uint8_t> MakeBlock(const std::vector<float>& samples,
                                             uint32_t blockBytes = kBlockBytes) {
    std::vector<uint8_t> block(blockBytes, 0u);
    for (size_t i = 0; i < samples.size(); ++i) {
        const int32_t raw =
            static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(samples[i]))
                                 << 8);
        WritePcmSample(std::span<uint8_t>(block.data() + kPcmByteOffset + i * kBytesPerChunk,
                                          kBytesPerChunk),
                       raw);
    }
    return block;
}

//==============================================================================

TEST(MotuPayloadReaderTests, DecodesChunksFromTheMotuOffsets) {
    const auto block = MakeBlock({1.0f, -1.0f});
    std::array<float, 2> out{};
    DecodeMotuBlock(block, 2, 0, out.data(), 2);

    EXPECT_NEAR(out[0], 1.0f, 1e-6f);
    EXPECT_NEAR(out[1], -1.0f, 1e-6f);
}

TEST(MotuPayloadReaderTests, PortMapReadsEachHostChannelFromItsPhysicalPort) {
    // UltraLite capture: host input 1-2 must be the mic chunks (2-3), not the CueMix
    // return that wire order puts first.
    constexpr uint32_t chunks = 14;
    std::vector<float> wire(chunks);
    for (uint32_t c = 0; c < chunks; ++c) {
        wire[c] = static_cast<float>(c + 1) / 32.0f;
    }
    const auto block = MakeBlock(wire, DataBlockBytes(chunks));
    std::array<float, chunks> out{};
    DecodeMotuBlock(block, chunks, 0, out.data(), chunks, kUltraLiteCapture);

    for (uint32_t ch = 0; ch < chunks; ++ch) {
        EXPECT_NEAR(out[ch], wire[kUltraLiteCapture[ch].chunk], 1e-6f)
            << kUltraLiteCapture[ch].name;
    }
    EXPECT_NEAR(out[0], wire[2], 1e-6f); // Mic 1
}

TEST(MotuPayloadReaderTests, RoundTripsThroughTheWriterScale) {
    // Writer and reader must agree: a value written then read back lands where it
    // started, so MOTU capture and playback share one normalisation.
    const std::vector<float> inputs{0.0f, 0.25f, -0.25f, 0.5f};
    const auto block = MakeBlock(inputs, /*blockBytes=*/kPcmByteOffset + 4 * kBytesPerChunk + 2);

    std::array<float, 4> out{};
    DecodeMotuBlock(block, 4, 0, out.data(), 4);

    for (size_t i = 0; i < inputs.size(); ++i) {
        EXPECT_NEAR(out[i], inputs[i], 1e-6f) << "chunk " << i;
    }
}

TEST(MotuPayloadReaderTests, ClampsFullNegativeScale) {
    // The 24-bit minimum has no positive counterpart; it must clamp to -1.0 rather than
    // overshooting, matching DirectRxPacketDecoder's Signed24ToFloat32.
    EXPECT_NEAR(Signed24ToFloat32(-8388608), -1.0f, 1e-6f);
    EXPECT_NEAR(Signed24ToFloat32(-8388607), -1.0f, 1e-6f);
    EXPECT_NEAR(Signed24ToFloat32(0), 0.0f, 1e-6f);
    EXPECT_NEAR(Signed24ToFloat32(8388607), 1.0f, 1e-6f);
}

TEST(MotuPayloadReaderTests, ChunksBeyondTheDeviceCountDecodeAsSilence) {
    const auto block = MakeBlock({1.0f, 1.0f});
    std::array<float, 4> out{9.0f, 9.0f, 9.0f, 9.0f};
    // Device carries 2 chunks; the caller asked for 4.
    DecodeMotuBlock(block, 2, 0, out.data(), 4);

    EXPECT_NEAR(out[0], 1.0f, 1e-6f);
    EXPECT_NEAR(out[1], 1.0f, 1e-6f);
    EXPECT_EQ(out[2], 0.0f);
    EXPECT_EQ(out[3], 0.0f);
}

TEST(MotuPayloadReaderTests, DoesNotReadPastAShortBlock) {
    // Block truncated mid-chunk: the partial chunk must decode as silence, not as
    // whatever bytes follow the buffer.
    std::vector<uint8_t> shortBlock(kPcmByteOffset + kBytesPerChunk + 1, 0xFFu);
    std::array<float, 2> out{9.0f, 9.0f};
    DecodeMotuBlock(shortBlock, 2, 0, out.data(), 2);

    EXPECT_EQ(out[1], 0.0f);
}

TEST(MotuPayloadReaderTests, HonoursChannelOffsetForMultiStreamSplits) {
    const auto block = MakeBlock({0.0f, 0.0f, 1.0f, -1.0f},
                                 /*blockBytes=*/kPcmByteOffset + 4 * kBytesPerChunk + 2);
    std::array<float, 2> out{};
    DecodeMotuBlock(block, 4, /*channelOffset=*/2, out.data(), 2);

    EXPECT_NEAR(out[0], 1.0f, 1e-6f);
    EXPECT_NEAR(out[1], -1.0f, 1e-6f);
}

TEST(MotuPayloadReaderTests, CountsWholeDataBlocksOnly) {
    EXPECT_EQ(DataBlocksInPayload(kCipHeaderBytes + 3 * kBlockBytes, kDbs), 3u);
    // A trailing partial block does not count.
    EXPECT_EQ(DataBlocksInPayload(kCipHeaderBytes + 3 * kBlockBytes + 5, kDbs), 3u);
    EXPECT_EQ(DataBlocksInPayload(kCipHeaderBytes, kDbs), 0u);
    EXPECT_EQ(DataBlocksInPayload(4, kDbs), 0u);
    EXPECT_EQ(DataBlocksInPayload(1024, 0), 0u);
}

TEST(MotuPayloadReaderTests, AddressesEachBlockInAPayload) {
    std::vector<uint8_t> payload(kCipHeaderBytes + 2 * kBlockBytes, 0u);
    payload[kCipHeaderBytes + kBlockBytes + kPcmByteOffset] = 0x7Fu; // mark block 1

    const auto block0 = BlockAt(payload, kDbs, 0);
    const auto block1 = BlockAt(payload, kDbs, 1);
    ASSERT_EQ(block0.size(), kBlockBytes);
    ASSERT_EQ(block1.size(), kBlockBytes);
    EXPECT_EQ(block0[kPcmByteOffset], 0x00u);
    EXPECT_EQ(block1[kPcmByteOffset], 0x7Fu);

    // Out of range yields an empty span rather than a wild pointer.
    EXPECT_TRUE(BlockAt(payload, kDbs, 2).empty());
    EXPECT_TRUE(BlockAt(payload, 0, 0).empty());
}

TEST(MotuPayloadReaderTests, NullOutputIsIgnored) {
    const auto block = MakeBlock({1.0f});
    DecodeMotuBlock(block, 1, 0, nullptr, 1); // must not crash
    SUCCEED();
}

} // namespace
