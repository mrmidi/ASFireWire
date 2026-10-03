// SPDX-License-Identifier: Apache-2.0
//
// MotuV3PayloadWriter tests.
//
// The v3 writer is the shared v2 placement under the Mk3 port map plus the acquisition
// mute. The first test is the claim that makes that possible: on every Mk3 geometry
// frame, the placement is byte-for-byte what an independent V3 encoder writes.
// The silence diagnostics close the file.

#include "Audio/Wire/MOTU/MotuV3PayloadWriter.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "Audio/Wire/AMDTP/MotuV3WireFormat.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"
#include "Audio/Protocols/MOTU/MOTU828Mk3Geometry.hpp"
#include "Audio/DriverKit/Config/MOTU/MOTU828Mk3Profile.hpp"
#include "Logging/LogRing.hpp"
#include "TxPacketizerTestSupport.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace {

namespace MotuV3Wire = ASFW::Protocols::Audio::AMDTP::MotuV3Wire;
using ASFW::Audio::Wire::MotuV3PayloadWriter;
using ASFW::Audio::Wire::MotuV3PlacementDelta;
using ASFW::Audio::Wire::MotuV3TxSilenceDiagnostics;
using ASFW::Encoding::Motu::kMk3Playback;
using ASFW::Encoding::Motu::MotuPayloadStreamConfig;
using ASFW::Protocols::Audio::AMDTP::AmdtpPacketTimeline;
using ASFW::Protocols::Audio::AMDTP::HostAudioBufferView;
using ASFW::Protocols::Audio::AMDTP::PacketTimelineSlot;
using ASFW::Protocols::Audio::AMDTP::PcmSlotCodec;
using ASFW::Protocols::Audio::AMDTP::PreparedTxPacket;

constexpr uint32_t kCipHeaderBytes = 8;

struct Mk3TxGeometry {
    uint32_t pcm;
    uint32_t dbs;
    uint32_t frames;
};

Mk3TxGeometry Mk3Tx48k() {
    const auto geometry = ASFW::Audio::MOTU::Build828Mk3Geometry(48000);
    EXPECT_TRUE(geometry.has_value());
    return {geometry->hostToDevicePcm, geometry->hostToDeviceDbs,
            geometry->framesPerDataPacket};
}

// `packets` consecutive DATA packets of `frames` frames each, from audio frame 0.
// `bytes` is packet 0's image; `images` holds every packet's.
struct TimelineHarness {
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    std::vector<std::vector<uint8_t>> images;
    std::vector<uint8_t>& bytes;

    TimelineHarness(uint32_t frames, uint32_t dbs, uint32_t packets = 1)
        : images(packets, std::vector<uint8_t>(kCipHeaderBytes + frames * dbs * 4U, 0)),
          bytes(images[0]) {
        EXPECT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
        for (uint32_t i = 0; i < packets; ++i) {
            PreparedTxPacket packet{};
            packet.packetIndex = i;
            packet.isData = true;
            packet.firstAudioFrame = static_cast<uint64_t>(i) * frames;
            packet.framesInPacket = frames;
            packet.dbs = dbs;
            packet.byteCount = static_cast<uint32_t>(images[i].size());
            EXPECT_TRUE(timeline.ExposeDataPacket(packet, images[i].data(),
                                                  static_cast<uint32_t>(images[i].size())));
        }
    }
};

// An independent V3 encoder, addressing samples by wire chunk from the SPH quadlet
// (`kSphBytes + wireChunk * 3`): placement only, no counters, no diagnostics.
void ReferenceEncodeV3(uint8_t* packet, uint32_t dbs, uint32_t frames, uint32_t pcmChannels,
                      uint32_t sourceChannelOffset, const float* source, uint32_t channels) {
    const uint32_t availableChunks = MotuV3Wire::AvailableChunks(static_cast<uint8_t>(dbs));
    const uint32_t mapped = pcmChannels < MotuV3Wire::kCoreOutputChannels
                                ? pcmChannels
                                : static_cast<uint32_t>(MotuV3Wire::kCoreOutputChannels);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        uint8_t* dest = packet + kCipHeaderBytes + frame * dbs * 4U;
        const float* in = source + frame * channels;
        for (uint32_t ch = 0; ch < mapped; ++ch) {
            const uint32_t wireChunk = MotuV3Wire::kCoreToWireChunk[ch];
            if (wireChunk >= availableChunks) {
                continue;
            }
            const uint32_t srcCh = sourceChannelOffset + ch;
            const float sample = srcCh < channels ? in[srcCh] : 0.0f;
            const int32_t encoded = PcmSlotCodec::Float32ToSigned24(sample);
            uint8_t* bytes = dest + MotuV3Wire::kSphBytes + wireChunk * MotuV3Wire::kBytesPerChunk;
            bytes[0] = static_cast<uint8_t>(encoded >> 16);
            bytes[1] = static_cast<uint8_t>(encoded >> 8);
            bytes[2] = static_cast<uint8_t>(encoded);
        }
    }
}

void Configure(MotuV3PayloadWriter& writer, AmdtpPacketTimeline* timeline, uint32_t pcm) {
    writer.Configure(MotuPayloadStreamConfig{.pcmChunks = pcm, .ports = kMk3Playback});
    writer.BindTimeline(timeline);
}

HostAudioBufferView View(const std::vector<float>& samples, uint32_t frames, uint32_t channels) {
    HostAudioBufferView view{};
    view.interleavedFloat32 = samples.data();
    view.firstFrame = 0;
    view.frameCount = frames;
    view.frameCapacity = frames;
    view.channels = channels;
    return view;
}

//==============================================================================

TEST(MotuV3PayloadWriterTests, PlacesPcmBitForBitLikeTheReferenceEncoder) {
    const Mk3TxGeometry g = Mk3Tx48k();
    ASSERT_EQ(g.pcm, 14U);
    std::mt19937 rng(0x5eed);
    std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
    // Host buffers narrower than, equal to and wider than the 14 playback ports.
    for (const uint32_t channels : {2U, 14U, 16U}) {
        for (int trial = 0; trial < 64; ++trial) {
            std::vector<float> samples(g.frames * channels);
            for (float& s : samples) {
                s = dist(rng);
            }
            TimelineHarness h{g.frames, g.dbs};
            MotuV3PayloadWriter writer{};
            Configure(writer, &h.timeline, g.pcm);
            writer.WriteFloat32Interleaved(View(samples, g.frames, channels), 0);
            ASSERT_EQ(writer.Counters().framesWritten.load(), g.frames);

            std::vector<uint8_t> expected(h.bytes.size(), 0);
            ReferenceEncodeV3(expected.data(), g.dbs, g.frames, g.pcm, 0, samples.data(),
                             channels);
            ASSERT_EQ(0, std::memcmp(expected.data(), h.bytes.data(), expected.size()))
                << "channels=" << channels << " trial=" << trial;
        }
    }
}

TEST(MotuV3PayloadWriterTests, MainPairLandsOnTheCapturedMainChunks) {
    const Mk3TxGeometry g = Mk3Tx48k();
    TimelineHarness h{1, g.dbs};
    MotuV3PayloadWriter writer{};
    Configure(writer, &h.timeline, g.pcm);
    std::vector<float> samples(2, 0.0f);
    samples[0] = 1.0f;   // host channel 1
    samples[1] = -1.0f;  // host channel 2
    writer.WriteFloat32Interleaved(View(samples, 1, 2), 0);

    // Main L/R are wire chunks 12/13 (PCM positions 10/11) in the El Capitan capture.
    const uint8_t* block = h.bytes.data() + kCipHeaderBytes;
    const uint8_t* mainL = block + MotuV3Wire::kSphBytes + 12 * MotuV3Wire::kBytesPerChunk;
    const uint8_t* mainR = block + MotuV3Wire::kSphBytes + 13 * MotuV3Wire::kBytesPerChunk;
    EXPECT_EQ(mainL[0], 0x7f);
    EXPECT_EQ(mainR[0], 0x80);
    // SPH and both message chunks belong to the stamper and the device: untouched.
    for (uint32_t i = 0; i < MotuV3Wire::kSphBytes + 2 * MotuV3Wire::kBytesPerChunk; ++i) {
        EXPECT_EQ(block[i], 0) << "byte " << i;
    }
}

TEST(MotuV3PayloadWriterTests, MuteWritesSilenceIntoTheSamePacketsAndCountsIt) {
    const Mk3TxGeometry g = Mk3Tx48k();
    TimelineHarness h{g.frames, g.dbs};
    MotuV3PayloadWriter writer{};
    Configure(writer, &h.timeline, g.pcm);
    std::vector<float> samples(g.frames * g.pcm, 0.5f);

    // Audible first, so the mute has something to overwrite.
    writer.WriteFloat32Interleaved(View(samples, g.frames, g.pcm), 0);
    std::vector<uint8_t> audible = h.bytes;
    ASSERT_NE(audible, std::vector<uint8_t>(audible.size(), 0));

    writer.SetPcmMuted(true);
    EXPECT_TRUE(writer.IsPcmMuted());
    writer.WriteFloat32Interleaved(View(samples, g.frames, g.pcm), 0);
    // DATA with silence: every frame still written, every PCM byte zero.
    EXPECT_EQ(writer.Counters().framesWritten.load(), 2U * g.frames);
    EXPECT_EQ(writer.FramesIntentionallyMuted(), g.frames);
    EXPECT_EQ(h.bytes, std::vector<uint8_t>(h.bytes.size(), 0));

    writer.SetPcmMuted(false);
    writer.WriteFloat32Interleaved(View(samples, g.frames, g.pcm), 0);
    EXPECT_EQ(h.bytes, audible);
    EXPECT_EQ(writer.FramesIntentionallyMuted(), g.frames);
}

TEST(MotuV3PayloadWriterTests, MuteKeepsTheFinalityFrontierAndTheMissingPacketCounts) {
    const Mk3TxGeometry g = Mk3Tx48k();
    TimelineHarness h{g.frames, g.dbs};
    MotuV3PayloadWriter writer{};
    Configure(writer, &h.timeline, g.pcm);
    writer.SetPcmMuted(true);
    std::vector<float> samples(2 * g.frames * g.pcm, 0.5f);
    // Packet 0 is behind the frontier; the second packet's frames have no owner yet.
    writer.WriteFloat32Interleaved(View(samples, 2 * g.frames, g.pcm), 1);
    EXPECT_EQ(writer.Counters().framesMissedFinality.load(), g.frames);
    EXPECT_EQ(writer.Counters().framesWithoutPacket.load(), g.frames);
    EXPECT_EQ(writer.FramesIntentionallyMuted(), 0U);
}

//==============================================================================
// Silence diagnostics. The counters split silence on the 828 by
// layer: host source, servo mute, placement, packet.

// Host channel 1 carries `level` on every frame of `frames`, the rest silence.
std::vector<float> Channel1(uint32_t frames, uint32_t channels, float level) {
    std::vector<float> samples(static_cast<size_t>(frames) * channels, 0.0f);
    for (uint32_t f = 0; f < frames; ++f) {
        samples[static_cast<size_t>(f) * channels] = level;
    }
    return samples;
}

TEST(MotuV3PayloadWriterTests, SilenceCountersSplitSourceFromWire) {
    const Mk3TxGeometry g = Mk3Tx48k();
    TimelineHarness h{g.frames, g.dbs};
    MotuV3PayloadWriter writer{};
    Configure(writer, &h.timeline, g.pcm);

    const auto music = Channel1(g.frames, 2, 0.5f);
    writer.WriteFloat32Interleaved(View(music, g.frames, 2), 0);
    const auto& c = writer.SilenceCounters();
    EXPECT_EQ(c.framesSourceNonZero.load(), g.frames);
    EXPECT_EQ(c.framesSourceZero.load(), 0U);
    EXPECT_EQ(c.payloadZeroPackets.load(), 0U);

    // An idle host: a zero packet, but nothing was lost and it is no hole in music.
    const std::vector<float> idle(g.frames * 2U, 0.0f);
    writer.WriteFloat32Interleaved(View(idle, g.frames, 2), 0);
    EXPECT_EQ(c.framesSourceZero.load(), g.frames);
    EXPECT_EQ(c.payloadZeroPackets.load(), 1U);
    EXPECT_EQ(c.payloadZeroPacketsWithSignal.load(), 0U);
    EXPECT_EQ(c.inputZeroRuns.load(), 0U);
}

TEST(MotuV3PayloadWriterTests, BelowTwentyFourBitsIsSilence) {
    const Mk3TxGeometry g = Mk3Tx48k();
    TimelineHarness h{g.frames, g.dbs};
    MotuV3PayloadWriter writer{};
    Configure(writer, &h.timeline, g.pcm);
    // Encodes to 0 at 24 bits: on the wire it is silence, so it must not count as
    // signal that a zero packet then lost.
    const auto whisper = Channel1(g.frames, 2, 1.0e-9f);
    writer.WriteFloat32Interleaved(View(whisper, g.frames, 2), 0);
    const auto& c = writer.SilenceCounters();
    EXPECT_EQ(c.framesSourceNonZero.load(), 0U);
    EXPECT_EQ(c.payloadZeroPackets.load(), 1U);
    EXPECT_EQ(c.payloadZeroPacketsWithSignal.load(), 0U);
}

TEST(MotuV3PayloadWriterTests, MutedMusicIsAZeroPacketButNotLostPcm) {
    const Mk3TxGeometry g = Mk3Tx48k();
    TimelineHarness h{g.frames, g.dbs};
    MotuV3PayloadWriter writer{};
    Configure(writer, &h.timeline, g.pcm);
    writer.SetPcmMuted(true);
    const auto music = Channel1(g.frames, 2, 0.5f);
    writer.WriteFloat32Interleaved(View(music, g.frames, 2), 0);
    const auto& c = writer.SilenceCounters();
    // The source is measured before the mute, so "host sent music, servo held it"
    // reads apart from "host sent nothing".
    EXPECT_EQ(c.framesSourceNonZero.load(), g.frames);
    EXPECT_EQ(c.payloadZeroPackets.load(), 1U);
    EXPECT_EQ(c.payloadZeroPacketsWithSignal.load(), 0U);
    EXPECT_EQ(writer.FramesIntentionallyMuted(), g.frames);
}

TEST(MotuV3PayloadWriterTests, HoleInMusicCountsOneInputZeroRun) {
    const Mk3TxGeometry g = Mk3Tx48k();
    ASSERT_GE(g.frames, MotuV3TxSilenceDiagnostics::kZeroRunFrames);
    TimelineHarness h{g.frames, g.dbs, 2};
    MotuV3PayloadWriter writer{};
    Configure(writer, &h.timeline, g.pcm);
    // Packet 0 silent, packet 1 music: one hole of a whole packet.
    auto samples = Channel1(2 * g.frames, 2, 0.5f);
    for (uint32_t f = 0; f < g.frames; ++f) {
        samples[static_cast<size_t>(f) * 2U] = 0.0f;
    }
    writer.WriteFloat32Interleaved(View(samples, 2 * g.frames, 2), 0);
    const auto& c = writer.SilenceCounters();
    EXPECT_EQ(c.inputZeroRuns.load(), 1U);
    EXPECT_EQ(c.framesSourceZero.load(), g.frames);
    EXPECT_EQ(c.payloadZeroPackets.load(), 1U);
    EXPECT_EQ(c.payloadZeroPacketsWithSignal.load(), 0U);
}

TEST(MotuV3PayloadWriterTests, OnlyPacketsPlacedWhollyByTheCallAreJudged) {
    const Mk3TxGeometry g = Mk3Tx48k();
    ASSERT_GE(g.frames, 2U);
    TimelineHarness h{g.frames, g.dbs};
    MotuV3PayloadWriter writer{};
    Configure(writer, &h.timeline, g.pcm);
    // Half a packet of silence: its other half is another call's, so the packet's PCM
    // says nothing about this one.
    const std::vector<float> idle(g.frames * 2U, 0.0f);
    writer.WriteFloat32Interleaved(View(idle, g.frames / 2, 2), 0);
    EXPECT_EQ(writer.SilenceCounters().payloadZeroPackets.load(), 0U);
    // Behind the finality frontier the placement skips it, so it is not judged either.
    writer.WriteFloat32Interleaved(View(idle, g.frames, 2), 1);
    EXPECT_EQ(writer.SilenceCounters().payloadZeroPackets.load(), 0U);
}

// PCM lost between the host buffer and the packet cannot come from a correct
// placement, so the detector is driven directly: a packet that stayed zero while its
// source carried music.
TEST(MotuV3PayloadWriterTests, ZeroPacketUnderMusicIsLostPcmAndLogged) {
    auto& ring = ASFW::Logging::LogRing::Shared();
    ring.Initialize();
    ASSERT_TRUE(ring.IsInitialized());
    const uint64_t cursor = ring.Stats().latestSequence;

    const Mk3TxGeometry g = Mk3Tx48k();
    TimelineHarness h{g.frames, g.dbs};
    MotuV3TxSilenceDiagnostics diagnostics{};
    diagnostics.Configure(MotuPayloadStreamConfig{.pcmChunks = g.pcm, .ports = kMk3Playback});
    diagnostics.BindTimeline(&h.timeline);
    const auto music = Channel1(g.frames, 2, 0.5f);
    diagnostics.Observe(View(music, g.frames, 2), 0, /*muted=*/false,
                        MotuV3PlacementDelta{.written = g.frames});
    EXPECT_EQ(diagnostics.Counters().payloadZeroPackets.load(), 1U);
    EXPECT_EQ(diagnostics.Counters().payloadZeroPacketsWithSignal.load(), 1U);

    ASFW::Logging::LogRingQuery query{};
    query.afterSequence = cursor;
    std::array<ASFW::Logging::LogRecord, 16> records{};
    const auto result = ring.Query(query, records.data(), records.size());
    bool sawFreshness = false;
    bool sawLoss = false;
    for (uint32_t i = 0; i < result.recordCount; ++i) {
        const std::string message = records[i].message;
        sawFreshness |= message.find("[TxZeroTrace] v3 pcmCh=14 srcOffset=0") != std::string::npos;
        sawLoss |= message.find("[TxPayloadZero] packets=1 withSignal=1 firstPkt=0") !=
                   std::string::npos;
    }
    EXPECT_TRUE(sawFreshness);
    EXPECT_TRUE(sawLoss);
}

} // namespace

//==============================================================================
// Message chunks (U3 checklist §6): nothing in ASFW sends V3 messages, so both chunks
// between the SPH quadlet and the PCM must reach the wire as zero.

TEST(MotuV3PayloadWriterTests, NonZeroMessageByteIsCountedAsMsgNZ) {
    const Mk3TxGeometry g = Mk3Tx48k();
    TimelineHarness h{g.frames, g.dbs, 2};
    // Packet 1, last block, last message byte: the far corner of the range.
    h.images[1][kCipHeaderBytes + (g.frames - 1) * g.dbs * 4U +
                MotuV3Wire::kSphBytes + 2 * MotuV3Wire::kBytesPerChunk - 1] = 0x01;
    MotuV3PayloadWriter writer{};
    Configure(writer, &h.timeline, g.pcm);
    const auto music = Channel1(g.frames * 2, 2, 0.5f);
    writer.WriteFloat32Interleaved(View(music, g.frames * 2, 2), 0);
    EXPECT_EQ(writer.SilenceCounters().payloadMessageNonZeroPackets.load(), 1U);
}

// The whole host path for one DATA packet, packetizer then writer, on a slot that
// still holds a previous lap's bytes. Only the packetizer's clear stands between those
// bytes and the wire, and its AM824 silence (0x40000000) must not be armed for V3.
TEST(MotuV3PayloadWriterTests, PacketizerAndWriterLeaveMessageChunksZero) {
    using namespace ASFW::Protocols::Audio::AMDTP;
    const Mk3TxGeometry g = Mk3Tx48k();
    ASFW::Isoch::Audio::MOTU::Profiles::MOTU828Mk3Profile profile;
    const auto streamPolicy = profile.TxStreamPolicy();

    AmdtpStreamConfig config{};
    config.streamMode = StreamMode::Blocking;
    config.dbs = static_cast<uint8_t>(g.dbs);
    config.pcmChannels = static_cast<uint8_t>(g.pcm);
    config.framesPerDataPacket = static_cast<uint8_t>(g.frames);
    config.maxPacketBytes = kCipHeaderBytes + g.frames * g.dbs * 4U;

    // DiceTxStreamEngine::BuildTxPolicy for this profile: V3 falls to the AM824
    // encoding branch, and only the layout keeps that silence out.
    AmdtpTxPolicy policy{};
    policy.payloadLayout = TxPayloadLayout::MotuV3Packed;
    policy.defaultNonAudioSlotWord = streamPolicy.defaultNonAudioSlotWord;
    policy.initializeNonAudioSlots = streamPolicy.initializeNonAudioSlots;
    policy.preserveFdfInNoDataPackets = streamPolicy.preserveFdfInNoDataPackets;
    policy.clearPayloadBeforeExposure = true;
    ASSERT_EQ(policy.hostToDevicePcmEncoding, PcmSlotEncoding::Am824MBLA);

    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    ASSERT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
    AmdtpTxPacketizer packetizer{};
    packetizer.BindTimeline(&timeline);
    ASSERT_TRUE(packetizer.Configure(config, policy));
    MotuV3PayloadWriter writer{};
    Configure(writer, &timeline, g.pcm);

    AmdtpTimingState timing{};
    timing.txClockValid = true;
    timing.disposition = AmdtpPacketDisposition::Data;
    timing.replayValid = true;
    timing.replayDataBlocks = static_cast<uint8_t>(g.frames);

    std::vector<uint8_t> bytes(config.maxPacketBytes, 0xA5); // a previous lap
    uint64_t nextFrame = 0;
    PreparedTxPacket prepared{};
    ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(
        packetizer, {0, bytes.data(), static_cast<uint32_t>(bytes.size())}, timing,
        nextFrame, prepared));
    ASSERT_TRUE(prepared.isData);
    ASSERT_EQ(prepared.framesInPacket, g.frames);

    std::vector<float> samples(g.frames * g.pcm, 0.5f);
    writer.WriteFloat32Interleaved(View(samples, g.frames, g.pcm), 0);
    ASSERT_EQ(writer.Counters().framesWritten.load(), g.frames);

    for (uint32_t frame = 0; frame < g.frames; ++frame) {
        const uint8_t* block = bytes.data() + kCipHeaderBytes + frame * g.dbs * 4U;
        for (uint32_t i = MotuV3Wire::kSphBytes;
             i < MotuV3Wire::kSphBytes + 2 * MotuV3Wire::kBytesPerChunk; ++i) {
            EXPECT_EQ(block[i], 0) << "frame " << frame << " byte " << i;
        }
    }
    EXPECT_EQ(writer.SilenceCounters().payloadMessageNonZeroPackets.load(), 0U);
}
