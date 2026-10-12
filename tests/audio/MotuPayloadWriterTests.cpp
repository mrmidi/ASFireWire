// SPDX-License-Identifier: Apache-2.0
//
// MotuPayloadWriter tests.
//
// These pin MOTU's data-block layout on the wire: 3-byte big-endian PCM chunks starting
// at byte offset 10, past the SPH quadlet and the two message chunks. Layout
// cross-validated with Linux amdtp-motu.c:93-187.

#include "Audio/Wire/MOTU/MotuPayloadWriter.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using ASFW::Encoding::Motu::kBytesPerChunk;
using ASFW::Encoding::Motu::kPcmByteOffset;
using ASFW::Encoding::Motu::kV2Playback;
using ASFW::Encoding::Motu::DataBlockQuadlets;
using ASFW::Encoding::Motu::MotuPayloadStreamConfig;
using ASFW::Encoding::Motu::MotuPayloadWriter;
using ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline;
using ASFW::Protocols::Audio::AMDTP::HostAudioBufferView;
using ASFW::Protocols::Audio::AMDTP::PacketTimelineSlot;
using ASFW::Protocols::Audio::AMDTP::PcmSlotCodec;
using ASFW::Protocols::Audio::AMDTP::PreparedTxPacket;

constexpr uint32_t kCipHeaderBytes = 8;

/// Two PCM chunks, so a data block is: SPH quadlet + 2 message chunks + 2 PCM chunks
/// = 4 + 6 + 6 = 16 bytes, already quadlet aligned -> dbs 4.
constexpr uint32_t kPcmChunks = 2;
constexpr uint32_t kDbs = 4;
constexpr uint32_t kBlockBytes = kDbs * 4;

/// Exposes one data packet of `frames` blocks and hands back its byte buffer.
struct TimelineHarness {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    std::vector<uint8_t> bytes;

    explicit TimelineHarness(uint32_t frames, uint64_t firstAudioFrame = 0,
                             uint32_t dbs = kDbs) {
        bytes.assign(kCipHeaderBytes + frames * dbs * 4U, 0xEEu); // poison
        EXPECT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));

        PreparedTxPacket packet{};
        packet.packetIndex = 0;
        packet.isData = true;
        packet.firstAudioFrame = firstAudioFrame;
        packet.framesInPacket = frames;
        packet.dbs = dbs;
        packet.byteCount = static_cast<uint32_t>(bytes.size());
        EXPECT_TRUE(timeline.ExposeDataPacket(packet, bytes.data(),
                                              static_cast<uint32_t>(bytes.size())));
    }

    [[nodiscard]] const uint8_t* Block(uint32_t index) const {
        return bytes.data() + kCipHeaderBytes + index * kBlockBytes;
    }
};

[[nodiscard]] int32_t ReadChunk(const uint8_t* block, uint32_t chunk) {
    const uint8_t* p = block + kPcmByteOffset + chunk * kBytesPerChunk;
    return static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 24) |
                                (static_cast<uint32_t>(p[1]) << 16) |
                                (static_cast<uint32_t>(p[2]) << 8));
}

/// Configures in place: the writer owns atomic counters, so it is neither copyable nor
/// movable and cannot be returned by value.
void SetUpWriter(MotuPayloadWriter& writer, AmdtpPacketTimeline* timeline,
                 uint32_t chunks = kPcmChunks, uint32_t sourceChannelOffset = 0) {
    writer.Configure(MotuPayloadStreamConfig{.pcmChunks = chunks,
                                             .sourceChannelOffset = sourceChannelOffset});
    writer.BindTimeline(timeline);
}

//==============================================================================

TEST(MotuPayloadWriterTests, PlacesPcmAtTheMotuChunkOffsets) {
    TimelineHarness h{2};
    MotuPayloadWriter writer{};
    SetUpWriter(writer, &h.timeline);

    const std::array<float, 4> samples{1.0f, -1.0f, 0.5f, -0.5f};
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.firstFrame = 0;
    view.frameCount = 2;
    view.frameCapacity = 2;
    view.channels = 2;

    writer.WriteFloat32Interleaved(view, 0);

    EXPECT_EQ(writer.Counters().framesWritten.load(), 2U);

    // Full scale is 2^23-1 in the low 24 bits, shifted into the top 24.
    const int32_t expectFullPos = static_cast<int32_t>(static_cast<uint32_t>(8388607) << 8);
    EXPECT_EQ(ReadChunk(h.Block(0), 0), expectFullPos);
    EXPECT_EQ(ReadChunk(h.Block(0), 1),
              static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(-1.0f))
                                   << 8));
    EXPECT_EQ(ReadChunk(h.Block(1), 0),
              static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(0.5f))
                                   << 8));
}

TEST(MotuPayloadWriterTests, PortMapSendsEachHostChannelToItsPhysicalPort) {
    // Full 14-chunk v2 playback: host channels 1-2 must land on the Main chunks (10-11),
    // not the headphone pair that wire order puts first.
    constexpr uint32_t chunks = 14;
    constexpr uint32_t dbs = DataBlockQuadlets(chunks);
    TimelineHarness h{1, 0, dbs};
    MotuPayloadWriter writer{};
    writer.Configure(MotuPayloadStreamConfig{.pcmChunks = chunks,
                                             .sourceChannelOffset = 0,
                                             .ports = kV2Playback});
    writer.BindTimeline(&h.timeline);

    std::array<float, chunks> samples{};
    for (uint32_t ch = 0; ch < chunks; ++ch) {
        samples[ch] = static_cast<float>(ch + 1) / 32.0f;
    }
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.frameCount = 1;
    view.frameCapacity = 1;
    view.channels = chunks;

    writer.WriteFloat32Interleaved(view, 0);

    const uint8_t* block = h.bytes.data() + kCipHeaderBytes;
    for (uint32_t ch = 0; ch < chunks; ++ch) {
        const int32_t expected = static_cast<int32_t>(
            static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(samples[ch])) << 8);
        EXPECT_EQ(ReadChunk(block, kV2Playback[ch].chunk), expected) << kV2Playback[ch].name;
    }
    EXPECT_EQ(ReadChunk(block, 10), static_cast<int32_t>(static_cast<uint32_t>(
                                        PcmSlotCodec::Float32ToSigned24(samples[0])) << 8));
}

TEST(MotuPayloadWriterTests, LeavesTheSphQuadletAndMessageChunksUntouched) {
    TimelineHarness h{1};
    MotuPayloadWriter writer{};
    SetUpWriter(writer, &h.timeline);

    const std::array<float, 2> samples{1.0f, 1.0f};
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.frameCount = 1;
    view.frameCapacity = 1;
    view.channels = 2;

    writer.WriteFloat32Interleaved(view, 0);

    // SPH carries replayed presentation time and is owned by the packetizer; the
    // message chunks (offsets 4..9) carry MIDI. Neither belongs to this writer, so the
    // poison must survive.
    const uint8_t* block = h.Block(0);
    for (uint32_t byte = 0; byte < kPcmByteOffset; ++byte) {
        EXPECT_EQ(block[byte], 0xEEu) << "clobbered block byte " << byte;
    }
}

TEST(MotuPayloadWriterTests, ChannelsBeyondTheHostBufferEncodeSilence) {
    // Four chunks configured but only two host channels: the physical ports CoreAudio is
    // not driving must carry silence, not stale bytes.
    TimelineHarness h{1, 0, /*dbs=*/6}; // 4 + 6 + 12 = 22 -> 6 quadlets
    MotuPayloadWriter writer{};
    writer.Configure(MotuPayloadStreamConfig{.pcmChunks = 4, .sourceChannelOffset = 0});
    writer.BindTimeline(&h.timeline);

    const std::array<float, 2> samples{1.0f, 1.0f};
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.frameCount = 1;
    view.frameCapacity = 1;
    view.channels = 2;

    writer.WriteFloat32Interleaved(view, 0);

    const uint8_t* block = h.bytes.data() + kCipHeaderBytes;
    EXPECT_NE(ReadChunk(block, 0), 0);
    EXPECT_NE(ReadChunk(block, 1), 0);
    EXPECT_EQ(ReadChunk(block, 2), 0);
    EXPECT_EQ(ReadChunk(block, 3), 0);
}

TEST(MotuPayloadWriterTests, HonoursSourceChannelOffsetForMultiStreamSplits) {
    TimelineHarness h{1};
    MotuPayloadWriter writer{};
    SetUpWriter(writer, &h.timeline, kPcmChunks, /*sourceChannelOffset=*/2);

    // A 4-channel host buffer split across two streams; this one owns channels [2,4).
    const std::array<float, 4> samples{0.0f, 0.0f, 1.0f, -1.0f};
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.frameCount = 1;
    view.frameCapacity = 1;
    view.channels = 4;

    writer.WriteFloat32Interleaved(view, 0);

    EXPECT_EQ(ReadChunk(h.Block(0), 0),
              static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(1.0f))
                                   << 8));
    EXPECT_EQ(ReadChunk(h.Block(0), 1),
              static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(-1.0f))
                                   << 8));
}

TEST(MotuPayloadWriterTests, CountsFramesWithNoOwningPacketWithoutWriting) {
    TimelineHarness h{1};
    MotuPayloadWriter writer{};
    SetUpWriter(writer, &h.timeline);

    const std::array<float, 2> samples{1.0f, 1.0f};
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.firstFrame = 100; // far past the single exposed packet
    view.frameCount = 1;
    view.frameCapacity = 1;
    view.channels = 2;

    writer.WriteFloat32Interleaved(view, 0);

    EXPECT_EQ(writer.Counters().framesWritten.load(), 0U);
    EXPECT_EQ(writer.Counters().framesWithoutPacket.load(), 1U);
    EXPECT_EQ(writer.Counters().framesVisited.load(), 1U);
}

TEST(MotuPayloadWriterTests, RefusesToWritePastAShortDataBlock) {
    // dbs 4 holds 2 chunks; claiming 4 would run into the next block's SPH.
    TimelineHarness h{1};
    MotuPayloadWriter writer{};
    writer.Configure(MotuPayloadStreamConfig{.pcmChunks = 4, .sourceChannelOffset = 0});
    writer.BindTimeline(&h.timeline);

    const std::array<float, 4> samples{1.0f, 1.0f, 1.0f, 1.0f};
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.frameCount = 1;
    view.frameCapacity = 1;
    view.channels = 4;

    writer.WriteFloat32Interleaved(view, 0);

    EXPECT_EQ(writer.Counters().framesTruncated.load(), 1U);
    EXPECT_EQ(writer.Counters().framesWritten.load(), 0U);
    // Nothing was written, so the poison is intact right up to the block end.
    for (uint32_t byte = 0; byte < kBlockBytes; ++byte) {
        EXPECT_EQ(h.Block(0)[byte], 0xEEu) << "wrote past a short block at " << byte;
    }
}

TEST(MotuPayloadWriterTests, DoesNothingWhenUnconfiguredOrUnbound) {
    const std::array<float, 2> samples{1.0f, 1.0f};
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.frameCount = 1;
    view.frameCapacity = 1;
    view.channels = 2;

    // No timeline bound.
    MotuPayloadWriter unbound{};
    unbound.Configure(MotuPayloadStreamConfig{.pcmChunks = kPcmChunks});
    unbound.WriteFloat32Interleaved(view, 0);
    EXPECT_EQ(unbound.Counters().framesVisited.load(), 0U);

    // Bound but zero chunk geometry.
    TimelineHarness h{1};
    MotuPayloadWriter unconfigured{};
    unconfigured.BindTimeline(&h.timeline);
    unconfigured.WriteFloat32Interleaved(view, 0);
    EXPECT_EQ(unconfigured.Counters().framesVisited.load(), 0U);
}

// The regulator's mute: the same packets, the same frames, silence instead of
// the host's samples. SPH and message chunks stay untouched either way.
TEST(MotuPayloadWriterTests, MuteWritesSilenceIntoTheSamePacketsAndCountsIt) {
    TimelineHarness h{2};
    MotuPayloadWriter writer{};
    SetUpWriter(writer, &h.timeline);
    EXPECT_FALSE(writer.IsPcmMuted());

    const std::array<float, 4> samples{0.5f, -0.5f, 0.25f, -0.25f};
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.frameCount = 2;
    view.frameCapacity = 2;
    view.channels = 2;

    writer.WriteFloat32Interleaved(view, 0);
    const std::vector<uint8_t> audible = h.bytes;
    ASSERT_NE(ReadChunk(h.Block(0), 0), 0);

    writer.SetPcmMuted(true);
    EXPECT_TRUE(writer.IsPcmMuted());
    writer.WriteFloat32Interleaved(view, 0);
    EXPECT_EQ(writer.Counters().framesWritten.load(), 4U);
    EXPECT_EQ(writer.Counters().framesMuted.load(), 2U);
    for (uint32_t block = 0; block < 2; ++block) {
        for (uint32_t chunk = 0; chunk < kPcmChunks; ++chunk) {
            EXPECT_EQ(ReadChunk(h.Block(block), chunk), 0) << "block " << block;
        }
        for (uint32_t byte = 0; byte < kPcmByteOffset; ++byte) {
            EXPECT_EQ(h.Block(block)[byte], 0xEEu) << "block " << block << " byte " << byte;
        }
    }

    writer.SetPcmMuted(false);
    writer.WriteFloat32Interleaved(view, 0);
    EXPECT_EQ(h.bytes, audible);
    EXPECT_EQ(writer.Counters().framesMuted.load(), 2U);
}

// Muting changes the source only: frames behind the finality frontier and
// frames no packet owns yet are counted as before, and not as muted.
TEST(MotuPayloadWriterTests, MuteKeepsTheFinalityFrontierAndTheMissingPacketCounts) {
    TimelineHarness h{1};
    MotuPayloadWriter writer{};
    SetUpWriter(writer, &h.timeline);
    writer.SetPcmMuted(true);

    const std::array<float, 4> samples{1.0f, 1.0f, 1.0f, 1.0f};
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.frameCount = 2;
    view.frameCapacity = 2;
    view.channels = 2;

    // Packet 0 is behind the frontier; frame 1 has no packet yet.
    writer.WriteFloat32Interleaved(view, 1);
    EXPECT_EQ(writer.Counters().framesMissedFinality.load(), 1U);
    EXPECT_EQ(writer.Counters().framesWithoutPacket.load(), 1U);
    EXPECT_EQ(writer.Counters().framesWritten.load(), 0U);
    EXPECT_EQ(writer.Counters().framesMuted.load(), 0U);
    for (uint32_t byte = 0; byte < kBlockBytes; ++byte) {
        EXPECT_EQ(h.Block(0)[byte], 0xEEu) << "byte " << byte;
    }
}

} // namespace

TEST(MotuPayloadWriterTests, OriginalProtocolWritesImmediatelyAfterSphWithoutMessageGap) {
    TimelineHarness harness{1,0,3}; // SPH + two packed PCM chunks + padding
    MotuPayloadWriter writer{};
    writer.Configure({.pcmChunks=2,.pcmByteOffset=4});
    writer.BindTimeline(&harness.timeline);
    const std::array<float,2> samples{1.0f,-1.0f};
    HostAudioBufferView view{};
    view.interleavedFloat32=samples.data(); view.frameCount=1; view.channels=2;
    writer.WriteFloat32Interleaved(view,0);
    EXPECT_EQ(writer.Counters().framesWritten.load(),1U);
    const auto* block = harness.bytes.data()+8;
    EXPECT_EQ(block[0],0xee); EXPECT_EQ(block[3],0xee); // SPH remains owned by timing
    EXPECT_EQ(block[4],0x7f); EXPECT_EQ(block[5],0xff); EXPECT_EQ(block[6],0xff);
    EXPECT_EQ(block[7],0x80); EXPECT_EQ(block[8],0x00); EXPECT_EQ(block[9],0x01); // symmetric full-scale quantization
    EXPECT_EQ(block[10],0xee); EXPECT_EQ(block[11],0xee); // padding untouched
}

TEST(MotuPayloadWriterTests, EightPreWritesOpticalPcmAndZerosInteriorPadding) {
    const auto* layout = ASFW::Encoding::Motu::PortLayoutFor(0xf,48000,14,false);
    ASSERT_NE(layout,nullptr);
    TimelineHarness harness{1,0,DataBlockQuadlets(14)};
    MotuPayloadWriter writer{};
    writer.Configure({.pcmChunks=14,.ports=layout->Map()});
    writer.BindTimeline(&harness.timeline);
    std::array<float,12> samples{};
    for (uint32_t i=0;i<samples.size();++i) samples[i]=float(i+1)/32;
    HostAudioBufferView view{};
    view.interleavedFloat32=samples.data(); view.frameCount=1; view.frameCapacity=1; view.channels=12;
    writer.WriteFloat32Interleaved(view,0);
    const auto* block=harness.bytes.data()+8;
    EXPECT_EQ(ReadChunk(block,4),0); EXPECT_EQ(ReadChunk(block,5),0);
    EXPECT_EQ(ReadChunk(block,6),static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(samples[4]))<<8));
    EXPECT_EQ(ReadChunk(block,13),static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(samples[11]))<<8));
    EXPECT_EQ(block[0],0xee); EXPECT_EQ(block[4],0xee); // SPH/status preserved.
}

TEST(MotuPayloadWriterTests, UltraLiteMk3VendorOrderReachesSpdifAndPhones) {
    const auto* layout=ASFW::Encoding::Motu::PortLayoutFor(0x19,48000,14,false);
    TimelineHarness harness{1,0,DataBlockQuadlets(14)};
    MotuPayloadWriter writer{};
    writer.Configure({.pcmChunks=14,.ports=layout->Map()}); writer.BindTimeline(&harness.timeline);
    std::array<float,14> samples{}; samples[10]=0.25f; samples[12]=0.5f;
    HostAudioBufferView view{};
    view.interleavedFloat32=samples.data(); view.frameCount=1;view.frameCapacity=1;view.channels=14;
    writer.WriteFloat32Interleaved(view,0);
    const auto* block=harness.bytes.data()+8;
    EXPECT_EQ(ReadChunk(block,12),static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(0.25f))<<8));
    EXPECT_EQ(ReadChunk(block,10),static_cast<int32_t>(static_cast<uint32_t>(PcmSlotCodec::Float32ToSigned24(0.5f))<<8));
}
