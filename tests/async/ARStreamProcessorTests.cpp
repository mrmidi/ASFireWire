// ARStreamProcessorTests.cpp
//
// Host tests for the shared AR bufferFill stream walker (ARStreamProcessor.hpp)
// against a real BufferRing. Regression coverage for the CoolScan wedge root
// cause: inbound packets straddling a buffer boundary were silently destroyed
// by the AR request path (whole-buffer commit without stitching), so the
// target's status-block write vanished and its split transaction dangled.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <vector>

#include "ASFWDriver/Async/Rx/ARStreamProcessor.hpp"
#include "ASFWDriver/Hardware/OHCIDescriptors.hpp"
#include "ASFWDriver/Shared/Rings/BufferRing.hpp"
#include "ASFWDriver/Testing/FakeDMAMemory.hpp"

namespace ASFW::Testing {

namespace {

using Async::ARPacketParser;
using Async::Rx::ProcessARStream;

// Thin adapter exposing the ring surface the walker needs (mirrors the
// delegation in ARContextBase).
struct RingContext {
    Shared::BufferRing& ring;

    std::optional<Shared::FilledBufferInfo> Dequeue() { return ring.Dequeue(); }
    kern_return_t Recycle(size_t index) { return ring.Recycle(index); }
    kern_return_t CommitConsumed(size_t index, size_t bytes) {
        return ring.CommitConsumed(index, bytes);
    }
    size_t CopyReadableBytes(std::span<uint8_t> destination) {
        return ring.CopyReadableBytes(destination);
    }
    kern_return_t ConsumeReadableBytes(size_t bytes) {
        return ring.ConsumeReadableBytes(bytes);
    }
};

struct DispatchedPacket {
    uint8_t tCode;
    size_t headerLength;
    size_t dataLength;
    size_t totalLength;
    std::vector<uint8_t> payload;
};

void AppendHostWordLE(std::vector<uint8_t>& bytes, uint32_t word) {
    bytes.push_back(static_cast<uint8_t>(word & 0xFF));
    bytes.push_back(static_cast<uint8_t>((word >> 8) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((word >> 16) & 0xFF));
    bytes.push_back(static_cast<uint8_t>((word >> 24) & 0xFF));
}

// Block write request (tCode 0x1) as OHCI AR DMA memory: 16-byte header,
// quadlet-padded payload, 4-byte trailer. Mirrors an SBP-2 status-block write.
std::vector<uint8_t> MakeWriteBlockPacket(uint8_t tLabel,
                                          std::initializer_list<uint32_t> payloadWords,
                                          uint32_t trailer = 0x00001100u) {
    std::vector<uint8_t> bytes;
    const uint32_t dataLength = static_cast<uint32_t>(payloadWords.size() * 4);
    AppendHostWordLE(bytes, (0xFFC1u << 16) | (static_cast<uint32_t>(tLabel) << 10) | (0x1u << 4));
    AppendHostWordLE(bytes, (0xFFC0u << 16) | 0x0010u);
    AppendHostWordLE(bytes, 0x00100090u);
    AppendHostWordLE(bytes, dataLength << 16);
    for (uint32_t word : payloadWords) {
        AppendHostWordLE(bytes, word);
    }
    AppendHostWordLE(bytes, trailer);
    return bytes;
}

class ARStreamProcessorTest : public ::testing::Test {
protected:
    static constexpr size_t kNumBuffers = 8;
    static constexpr size_t kBufferSize = 200;

    FakeDMAMemory dma_{256 * 1024};
    Shared::BufferRing ring_{};
    std::vector<DispatchedPacket> dispatched_;
    uint64_t heartbeat_ = 0;

    void SetUp() override {
        auto descRegion = dma_.AllocateRegion(kNumBuffers * sizeof(Async::HW::OHCIDescriptor));
        ASSERT_TRUE(descRegion.has_value());
        auto bufRegion = dma_.AllocateRegion(kNumBuffers * kBufferSize);
        ASSERT_TRUE(bufRegion.has_value());

        auto* descs = reinterpret_cast<Async::HW::OHCIDescriptor*>(descRegion->virtualBase);
        ASSERT_TRUE(ring_.Initialize({descs, kNumBuffers},
                                     {bufRegion->virtualBase, kNumBuffers * kBufferSize},
                                     kNumBuffers, kBufferSize));
        ring_.BindDma(&dma_);
        ASSERT_TRUE(ring_.Finalize(descRegion->deviceBase, bufRegion->deviceBase));
    }

    // Write bytes into one buffer and mark its filled count (simulates DMA
    // completing into that descriptor).
    void FillBuffer(size_t index, std::span<const uint8_t> bytes) {
        ASSERT_LE(bytes.size(), kBufferSize);
        std::memcpy(ring_.GetBufferAddress(index), bytes.data(), bytes.size());
        auto* desc = ring_.GetDescriptor(index);
        ASSERT_NE(desc, nullptr);
        Async::HW::AR_init_status(*desc, static_cast<uint16_t>(kBufferSize - bytes.size()));
    }

    // Lay a contiguous byte stream into the ring the way bufferFill DMA would:
    // fill each buffer to the brim before moving to the next, and mark filled
    // byte counts in the descriptors' resCount.
    void FillStream(std::span<const uint8_t> stream) {
        ASSERT_LE(stream.size(), kNumBuffers * kBufferSize);
        size_t remaining = stream.size();
        size_t offset = 0;
        for (size_t i = 0; i < kNumBuffers && remaining > 0; ++i) {
            const size_t chunk = std::min(remaining, kBufferSize);
            std::memcpy(ring_.GetBufferAddress(i), stream.data() + offset, chunk);
            auto* desc = ring_.GetDescriptor(i);
            ASSERT_NE(desc, nullptr);
            Async::HW::AR_init_status(*desc, static_cast<uint16_t>(kBufferSize - chunk));
            offset += chunk;
            remaining -= chunk;
        }
    }

    Async::Rx::ARStreamStats Run() {
        RingContext ctx{ring_};
        return ProcessARStream(
            ctx,
            "ARTest",
            &heartbeat_,
            [](uint32_t, const Shared::FilledBufferInfo&) {},
            [this](const ARPacketParser::PacketInfo& info) {
                DispatchedPacket packet{};
                packet.tCode = info.tCode;
                packet.headerLength = info.headerLength;
                packet.dataLength = info.dataLength;
                packet.totalLength = info.totalLength;
                packet.payload.assign(info.packetStart + info.headerLength,
                                      info.packetStart + info.headerLength + info.dataLength);
                dispatched_.push_back(std::move(packet));
            });
    }
};

TEST_F(ARStreamProcessorTest, DispatchesMultiplePacketsWithinOneBuffer) {
    std::vector<uint8_t> stream;
    const auto first = MakeWriteBlockPacket(1, {0x11111111u, 0x22222222u});
    const auto second = MakeWriteBlockPacket(2, {0x33333333u});
    stream.insert(stream.end(), first.begin(), first.end());
    stream.insert(stream.end(), second.begin(), second.end());
    FillStream(stream);

    const auto stats = Run();

    EXPECT_EQ(stats.packetsFound, 2u);
    ASSERT_EQ(dispatched_.size(), 2u);
    EXPECT_EQ(dispatched_[0].dataLength, 8u);
    EXPECT_EQ(dispatched_[1].dataLength, 4u);
}

TEST_F(ARStreamProcessorTest, StitchesPacketStraddlingBufferBoundary) {
    // Fill buffer 0 up to 8 bytes before the end, then append a 28-byte packet:
    // 8 bytes land in buffer 0 (making it full), 20 bytes in buffer 1.
    std::vector<uint8_t> stream;
    while (stream.size() + 28 <= kBufferSize - 8) {
        const auto filler = MakeWriteBlockPacket(1, {0xAAAAAAAAu, 0xBBBBBBBBu});
        ASSERT_EQ(filler.size(), 28u);
        stream.insert(stream.end(), filler.begin(), filler.end());
    }
    const size_t fillerCount = stream.size() / 28;
    ASSERT_EQ(kBufferSize - stream.size(), 32u);
    const auto pad = MakeWriteBlockPacket(2, {0xCCCCCCCCu});  // 24 bytes
    stream.insert(stream.end(), pad.begin(), pad.end());

    const auto straddler = MakeWriteBlockPacket(3, {0xDEADBEEFu, 0xFEEDFACEu});  // 28 bytes
    stream.insert(stream.end(), straddler.begin(), straddler.end());
    const auto after = MakeWriteBlockPacket(4, {0x44444444u});
    stream.insert(stream.end(), after.begin(), after.end());
    FillStream(stream);

    const auto stats = Run();

    // Every packet must survive the boundary: fillers + pad + straddler + after.
    EXPECT_EQ(stats.packetsFound, fillerCount + 3);
    ASSERT_EQ(dispatched_.size(), fillerCount + 3);
    const auto& stitched = dispatched_[fillerCount + 1];
    EXPECT_EQ(stitched.dataLength, 8u);
    ASSERT_EQ(stitched.payload.size(), 8u);
    EXPECT_EQ(stitched.payload[0], 0xEFu);  // 0xDEADBEEF little-endian in DMA memory
    EXPECT_EQ(stitched.payload[3], 0xDEu);
    EXPECT_EQ(dispatched_[fillerCount + 2].dataLength, 4u);
}

TEST_F(ARStreamProcessorTest, StitchesWhenOnlyTrailerCrossesBoundary) {
    // Header+payload of the straddler end exactly at the buffer boundary; only
    // the 4-byte trailer lands in the next buffer. The old parser framed this
    // as a complete trailer-less packet, desyncing the stream at the orphan
    // trailer — the wedge signature.
    std::vector<uint8_t> stream;
    constexpr size_t kFillerCount = 5;
    for (size_t i = 0; i < kFillerCount; ++i) {
        const auto filler = MakeWriteBlockPacket(1, {0xAAAAAAAAu, 0xBBBBBBBBu});
        stream.insert(stream.end(), filler.begin(), filler.end());
    }
    ASSERT_EQ(stream.size(), 140u);
    // 16-byte header + 44-byte payload + 4-byte trailer = 64 bytes; header+payload
    // (60 bytes) end exactly at the 200-byte buffer boundary.
    const auto straddler = MakeWriteBlockPacket(5, {0x11111111u, 0x22222222u,
                                                    0x33333333u, 0x44444444u,
                                                    0x55555555u, 0x66666666u,
                                                    0x77777777u, 0x88888888u,
                                                    0x12121212u, 0x34343434u,
                                                    0x56565656u});
    ASSERT_EQ(straddler.size(), 64u);
    stream.insert(stream.end(), straddler.begin(), straddler.end() - 4);
    ASSERT_EQ(stream.size(), kBufferSize);
    // Trailer goes into the next buffer, followed by one more packet.
    stream.insert(stream.end(), straddler.end() - 4, straddler.end());
    const auto after = MakeWriteBlockPacket(6, {0x99999999u});
    stream.insert(stream.end(), after.begin(), after.end());
    FillStream(stream);

    const auto stats = Run();

    EXPECT_EQ(stats.packetsFound, kFillerCount + 2);
    ASSERT_EQ(dispatched_.size(), kFillerCount + 2);
    EXPECT_EQ(dispatched_[kFillerCount].dataLength, 44u);
    EXPECT_EQ(dispatched_[kFillerCount].totalLength, 64u);
    EXPECT_EQ(dispatched_[kFillerCount + 1].dataLength, 4u);
}

TEST_F(ARStreamProcessorTest, FragmentWithoutTailIsPreservedNotDispatched) {
    // Buffer 0 completely full, ending in a fragment whose tail has not been
    // DMA'd yet. Nothing may be dispatched for the fragment and the parsed
    // prefix must be committed so the fragment stays readable for stitching.
    std::vector<uint8_t> stream;
    while (stream.size() + 28 <= kBufferSize - 8) {
        const auto filler = MakeWriteBlockPacket(1, {0xAAAAAAAAu, 0xBBBBBBBBu});
        stream.insert(stream.end(), filler.begin(), filler.end());
    }
    const size_t fillerCount = stream.size() / 28;
    const auto pad = MakeWriteBlockPacket(2, {0xCCCCCCCCu});
    stream.insert(stream.end(), pad.begin(), pad.end());
    const auto straddler = MakeWriteBlockPacket(3, {0xDEADBEEFu, 0xFEEDFACEu});
    // Only the first 8 bytes of the straddler arrive (buffer 0 becomes full).
    stream.insert(stream.end(), straddler.begin(), straddler.begin() + 8);
    ASSERT_EQ(stream.size(), kBufferSize);
    FillStream(stream);

    const auto stats = Run();

    EXPECT_EQ(stats.packetsFound, fillerCount + 1);
    EXPECT_EQ(dispatched_.size(), fillerCount + 1);
}

TEST_F(ARStreamProcessorTest, StitchesAcrossInterruptPassesWhenTailArrivesLater) {
    // The cross-pass straddle: buffer 0 completes FULL, ending in a fragment,
    // but the continuation has not been DMA'd into buffer 1 yet (OHCI may
    // complete the two descriptors of a straddling packet in either order —
    // Linux ar_search_last_active_buffer makes the same allowance). The first
    // pass must preserve the fragment; once buffer 1 gains the tail, the next
    // pass must re-present the full head, stitch, dispatch exactly once, and
    // advance the ring past the straddler.
    std::vector<uint8_t> phase1;
    constexpr size_t kFillerCount = 5;
    for (size_t i = 0; i < kFillerCount; ++i) {
        const auto filler = MakeWriteBlockPacket(1, {0xAAAAAAAAu, 0xBBBBBBBBu});
        phase1.insert(phase1.end(), filler.begin(), filler.end());
    }
    const auto pad = MakeWriteBlockPacket(2, {0xCCCCCCCCu});  // 24 bytes → 164 total
    phase1.insert(phase1.end(), pad.begin(), pad.end());
    const auto straddler = MakeWriteBlockPacket(3, {0xDEADBEEFu, 0xFEEDFACEu,
                                                    0x01020304u, 0x05060708u,
                                                    0x090A0B0Cu});  // 16+20+4 = 40 bytes
    ASSERT_EQ(straddler.size(), 40u);
    // First 36 straddler bytes make buffer 0 exactly full (164 + 36 = 200).
    phase1.insert(phase1.end(), straddler.begin(), straddler.begin() + 36);
    ASSERT_EQ(phase1.size(), kBufferSize);
    FillBuffer(0, phase1);

    const auto pass1 = Run();
    EXPECT_EQ(pass1.packetsFound, kFillerCount + 1);
    EXPECT_EQ(dispatched_.size(), kFillerCount + 1);

    // Tail (4 bytes) plus a following packet arrive in buffer 1.
    std::vector<uint8_t> phase2(straddler.begin() + 36, straddler.end());
    const auto after = MakeWriteBlockPacket(4, {0x44444444u});
    phase2.insert(phase2.end(), after.begin(), after.end());
    FillBuffer(1, phase2);

    const auto pass2 = Run();
    EXPECT_EQ(pass2.packetsFound, 2u);
    ASSERT_EQ(dispatched_.size(), kFillerCount + 3);
    const auto& stitched = dispatched_[kFillerCount + 1];
    EXPECT_EQ(stitched.totalLength, 40u);
    ASSERT_EQ(stitched.payload.size(), 20u);
    EXPECT_EQ(stitched.payload[0], 0xEFu);
    EXPECT_EQ(dispatched_[kFillerCount + 2].dataLength, 4u);

    // The ring must have advanced past the straddler: a third pass sees nothing.
    const auto pass3 = Run();
    EXPECT_EQ(pass3.packetsFound, 0u);
    EXPECT_EQ(dispatched_.size(), kFillerCount + 3);
}

TEST_F(ARStreamProcessorTest, OversizedDataLengthIsDroppedNotTreatedAsFragment) {
    // A corrupt block header declaring data_length > 4096 can never complete —
    // it must take the garbage path (drop remainder, realign at next buffer),
    // not be preserved as a stitchable fragment (which would stall the ring).
    // Mirrors Linux ohci.c: payload_length > MAX_ASYNC_PAYLOAD → context abort.
    std::vector<uint8_t> stream;
    const auto good = MakeWriteBlockPacket(1, {0x11111111u});
    stream.insert(stream.end(), good.begin(), good.end());
    AppendHostWordLE(stream, (0xFFC1u << 16) | (0x7u << 10) | (0x1u << 4));  // write block
    AppendHostWordLE(stream, (0xFFC0u << 16) | 0x0010u);
    AppendHostWordLE(stream, 0x00100090u);
    AppendHostWordLE(stream, 0x2000u << 16);  // data_length = 8192 > max 4096
    FillStream(stream);

    const auto pass1 = Run();
    EXPECT_EQ(pass1.packetsFound, 1u);

    // Stream continues normally in the next buffer.
    std::vector<uint8_t> next;
    const auto after = MakeWriteBlockPacket(2, {0x22222222u});
    next.insert(next.end(), after.begin(), after.end());
    FillBuffer(1, next);

    // Buffer 0 was only partially filled and then dropped-to-end; hardware
    // moving on to buffer 1 is only visible to Dequeue() once buffer 0 is
    // drained — which the garbage-drop commit guarantees.
    const auto pass2 = Run();
    EXPECT_EQ(pass2.packetsFound, 1u);
    ASSERT_EQ(dispatched_.size(), 2u);
    EXPECT_EQ(dispatched_[1].dataLength, 4u);
}

TEST_F(ARStreamProcessorTest, GarbageHeadDropsBufferRemainderAndRealignsAtNextBuffer) {
    // Good packet, then a head with invalid tCode 0xC — framing is lost, the
    // remainder of buffer 0 must be dropped loudly, and the stream must realign
    // at buffer 1 where a fresh packet begins.
    std::vector<uint8_t> stream;
    const auto good = MakeWriteBlockPacket(1, {0x11111111u});
    stream.insert(stream.end(), good.begin(), good.end());
    while (stream.size() < kBufferSize) {
        AppendHostWordLE(stream, 0x000000C0u);  // tCode nibble = 0xC
    }
    const auto next = MakeWriteBlockPacket(2, {0x22222222u});
    stream.insert(stream.end(), next.begin(), next.end());
    FillStream(stream);

    const auto stats = Run();

    EXPECT_EQ(stats.packetsFound, 2u);
    ASSERT_EQ(dispatched_.size(), 2u);
    EXPECT_EQ(dispatched_[1].dataLength, 4u);
}

TEST_F(ARStreamProcessorTest, AllZeroHeadIsDroppedAsGarbage) {
    std::vector<uint8_t> stream;
    const auto good = MakeWriteBlockPacket(1, {0x11111111u});
    stream.insert(stream.end(), good.begin(), good.end());
    for (int i = 0; i < 8; ++i) {
        AppendHostWordLE(stream, 0x00000000u);
    }
    FillStream(stream);

    const auto stats = Run();

    EXPECT_EQ(stats.packetsFound, 1u);
    EXPECT_EQ(dispatched_.size(), 1u);
}

TEST_F(ARStreamProcessorTest, WedgeRegressionSustainedCommandStreamLosesNoPackets) {
    // The CoolScan failure shape: a long run of small status-write-sized
    // packets crossing several buffer boundaries. Before the stitching fix,
    // exactly one packet vanished per boundary crossing (v46 run: command #83).
    std::vector<uint8_t> stream;
    size_t packetCount = 0;
    while (true) {
        const auto packet = MakeWriteBlockPacket(static_cast<uint8_t>(packetCount & 0x3F),
                                                 {static_cast<uint32_t>(packetCount),
                                                  0x5B500000u});
        if (stream.size() + packet.size() > (kNumBuffers - 1) * kBufferSize) {
            break;
        }
        stream.insert(stream.end(), packet.begin(), packet.end());
        ++packetCount;
    }
    ASSERT_GT(packetCount, 40u);  // spans several 200-byte buffers
    FillStream(stream);

    const auto stats = Run();

    EXPECT_EQ(stats.packetsFound, packetCount);
    ASSERT_EQ(dispatched_.size(), packetCount);
    for (size_t i = 0; i < packetCount; ++i) {
        ASSERT_EQ(dispatched_[i].payload.size(), 8u);
        uint32_t sequence = 0;
        std::memcpy(&sequence, dispatched_[i].payload.data(), 4);
        EXPECT_EQ(sequence, static_cast<uint32_t>(i)) << "packet " << i << " lost or reordered";
    }
}

} // namespace

} // namespace ASFW::Testing
