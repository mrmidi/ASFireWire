// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Wire/MOTU/MotuPayloadCodec.hpp"
#include "Audio/Wire/MOTU/MotuDeviceTiming.hpp"
#include "Audio/Wire/MOTU/MotuBlockLayout.hpp"
#include "Audio/Wire/MOTU/MotuSph.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <vector>

using namespace ASFW::Audio::Wire;
using namespace ASFW::AudioEngine::Direct::Rx;
using namespace ASFW::Encoding::Motu;
using namespace ASFW::Protocols::Audio::AMDTP;

namespace {

void StoreBigEndian(uint8_t* dst, uint32_t value) noexcept {
    dst[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    dst[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    dst[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    dst[3] = static_cast<uint8_t>(value & 0xFF);
}

} // namespace

TEST(MotuPayloadCodecTests, StrideAndGeometryValidation) {
    // 14 PCM chunks -> DataBlockQuadlets(14) = 1 + (((2 + 14)*3 + 3) / 4) = 13 quadlets.
    MotuRxPayloadCodec codec(14);

    EXPECT_EQ(codec.StrideQuadlets(8), 13U);
    EXPECT_EQ(codec.StrideQuadlets(19), 13U);

    // 0 chunks falls back to CIP DBS
    MotuRxPayloadCodec fallbackCodec(0);
    EXPECT_EQ(fallbackCodec.StrideQuadlets(8), 8U);

    // Geometry validation:
    // Valid: 4 channels starting at offset 0, 14 chunks, stride 13
    EXPECT_TRUE(codec.ValidateGeometry(4, 0, 13, 8));
    // Invalid: 0 channels
    EXPECT_FALSE(codec.ValidateGeometry(0, 0, 13, 8));
    // Invalid: 0 chunks
    EXPECT_FALSE(fallbackCodec.ValidateGeometry(4, 0, 13, 8));
    // Invalid: offset + channels > pcmChunks (offset 12 + 4 = 16 > 14)
    EXPECT_FALSE(codec.ValidateGeometry(4, 12, 13, 8));
    // Invalid: stride too small for required bytes (14 chunks need 10 + 14*3 = 52 bytes = 13 quadlets)
    EXPECT_FALSE(codec.ValidateGeometry(4, 0, 12, 8));
}

TEST(MotuPayloadCodecTests, DecodeBlockExtractsFloatSamplesAndHandlesDelay) {
    constexpr uint32_t kChunks = 4;
    MotuRxPayloadCodec codec(kChunks);

    // Build one 24-byte block (4 chunks: SPH 4B + Msg 6B + 4*3B = 22B, padded to 24B = 6 quadlets)
    std::vector<uint8_t> block(DataBlockQuadlets(kChunks) * 4, 0);

    // Sample values in 24-bit top: 0.5f and -0.5f
    // 0.5f in signed 24-bit: 0x400000 -> sampleTop24: 0x40000000
    const int32_t sample0 = static_cast<int32_t>(static_cast<uint32_t>(4194304) << 8);
    // -0.5f in signed 24-bit: -4194304 -> sampleTop24: -4194304 << 8
    const int32_t sample1 = static_cast<int32_t>(static_cast<uint32_t>(-4194304) << 8);

    // Write chunk 0 at offset 10
    block[10] = static_cast<uint8_t>((sample0 >> 24) & 0xFF);
    block[11] = static_cast<uint8_t>((sample0 >> 16) & 0xFF);
    block[12] = static_cast<uint8_t>((sample0 >> 8) & 0xFF);

    // Write chunk 1 at offset 13
    block[13] = static_cast<uint8_t>((sample1 >> 24) & 0xFF);
    block[14] = static_cast<uint8_t>((sample1 >> 16) & 0xFF);
    block[15] = static_cast<uint8_t>((sample1 >> 8) & 0xFF);

    std::array<float, 2> frameOut{};
    std::array<float, 2> delayedOut{};
    RxCaptureChannelMap map{};
    map.delayedChannelMask = (1U << 1); // Channel 1 is delayed
    map.delayFrames = 1;

    codec.DecodeBlock(block, 2, 0, map, frameOut.data(), delayedOut.data());

    // Channel 0 is not delayed: frameOut has ~0.5f
    EXPECT_NEAR(frameOut[0], 0.5f, 1e-5f);
    // Channel 1 is delayed: frameOut is zeroed, delayedOut has ~-0.5f
    EXPECT_EQ(frameOut[1], 0.0f);
    EXPECT_NEAR(delayedOut[1], -0.5f, 1e-5f);
}

TEST(MotuPayloadCodecTests, RxTimingObserverCapturesOffsetsAndEstablishesTiming) {
    MotuEventOffsetCache cache;
    MotuRxTimingObserver observer(&cache);

    EXPECT_FALSE(observer.IsTimingEstablished());

    // Build packet payload with 2 blocks (kDbs = 4 quadlets = 16 bytes per block).
    // Total size = 16 bytes prefix (8 isoch + 8 CIP) + 2 * 16 = 48 bytes.
    constexpr uint32_t kDbs = 4;
    constexpr uint32_t kDataBlocks = 2;
    std::vector<uint8_t> payload(16 + kDataBlocks * kDbs * 4, 0);

    // Write SPH to block 0 and block 1
    // Tick at cycle 100, tick 500: SPH = (100 << 12) | 500
    uint32_t sph0 = (100U << 12) | 500U;
    uint32_t sph1 = (100U << 12) | 1500U;
    StoreBigEndian(payload.data() + 16, sph0);
    StoreBigEndian(payload.data() + 16 + 16, sph1);

    // Feed 32 runs of packets to seed cache and pass min contiguous runs threshold
    for (uint32_t i = 0; i < 35; ++i) {
        observer.ObservePacket(payload, kDbs, kDataBlocks, /*cycle=*/100);
    }

    EXPECT_TRUE(observer.IsTimingEstablished());

    observer.Reset();
    EXPECT_FALSE(observer.IsTimingEstablished());
}

TEST(MotuPayloadCodecTests, TxTimingStamperStampsSphAndFallsBackToZero) {
    MotuEventOffsetCache cache;
    constexpr uint32_t kDbs = 4;
    MotuTxTimingStamper stamper(&cache, kDbs);

    EXPECT_TRUE(stamper.IsSytUnaware());

    // Allocate buffer for 1 slot: 8 bytes CIP + 2 blocks * 16 bytes = 40 bytes.
    std::vector<uint8_t> slotBytes(40, 0xFF); // Initialize with dirty memory
    TxPacketSlotView slot{
        .packetIndex = 0,
        .bytes = slotBytes.data(),
        .capacityBytes = static_cast<uint32_t>(slotBytes.size()),
    };

    PreparedTxPacket packet{
        .packetIndex = 0,
        .byteCount = static_cast<uint32_t>(slotBytes.size()),
        .isData = true,
        .framesInPacket = 2,
        .dbs = kDbs,
    };

    AmdtpTimingState timing{
        .transmitCycle = 10,
        .transmitCycleValid = false, // Invalid cycle -> should fall back to zeroing SPH
    };

    stamper.StampPacket(slot, packet, timing);

    // SPH at offset 8 and offset 24 should be zeroed
    uint32_t sph0 = (static_cast<uint32_t>(slotBytes[8]) << 24) |
                    (static_cast<uint32_t>(slotBytes[9]) << 16) |
                    (static_cast<uint32_t>(slotBytes[10]) << 8) |
                    static_cast<uint32_t>(slotBytes[11]);
    uint32_t sph1 = (static_cast<uint32_t>(slotBytes[24]) << 24) |
                    (static_cast<uint32_t>(slotBytes[25]) << 16) |
                    (static_cast<uint32_t>(slotBytes[26]) << 8) |
                    static_cast<uint32_t>(slotBytes[27]);
    EXPECT_EQ(sph0, 0U);
    EXPECT_EQ(sph1, 0U);

    // Now seed the cache with known offsets
    // Feed RX packets with known SPH so cache is established
    std::vector<uint8_t> rxPayload(16 + 2 * kDbs * 4, 0);
    StoreBigEndian(rxPayload.data() + 16, (100U << 12) | 200U);
    StoreBigEndian(rxPayload.data() + 16 + 16, (100U << 12) | 1200U);
    for (uint32_t i = 0; i < 40; ++i) {
        cache.Capture(rxPayload, kDbs, 2, 100, 16);
    }
    ASSERT_TRUE(cache.IsEstablished());

    // Transmit with valid cycle
    timing.transmitCycleValid = true;
    timing.transmitCycle = 20;

    stamper.StampPacket(slot, packet, timing);

    sph0 = (static_cast<uint32_t>(slotBytes[8]) << 24) |
           (static_cast<uint32_t>(slotBytes[9]) << 16) |
           (static_cast<uint32_t>(slotBytes[10]) << 8) |
           static_cast<uint32_t>(slotBytes[11]);
    sph1 = (static_cast<uint32_t>(slotBytes[24]) << 24) |
           (static_cast<uint32_t>(slotBytes[25]) << 16) |
           (static_cast<uint32_t>(slotBytes[26]) << 8) |
           static_cast<uint32_t>(slotBytes[27]);

    // Both SPH words must be non-zero and stamped with cycle 20 base
    EXPECT_NE(sph0, 0U);
    EXPECT_NE(sph1, 0U);
}
