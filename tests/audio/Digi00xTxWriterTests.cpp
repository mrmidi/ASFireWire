// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xTxWriterTests.cpp - Digi 002/003 playback on the wire: PCM after the
// MIDI data channel, the double-oh-three scrambler applied in wire order and
// carried across writes, the idle MIDI word, and SYT-unaware transmit.
// Cross-validated with Linux sound/firewire/digi00x/amdtp-dot.c
// (write_pcm_s32, write_midi_messages) as a behavioral reference.

#include "Audio/DriverKit/Config/Digi00x/Digi00xProfile.hpp"
#include "Audio/Engine/Direct/Tx/DiceTxStreamEngine.hpp"
#include "Audio/Wire/AMDTP/AmdtpPacketTimeline.hpp"
#include "Audio/Wire/AMDTP/AmdtpPayloadWriter.hpp"
#include "Audio/Wire/AMDTP/AmdtpTxPacketizer.hpp"
#include "Audio/Wire/AMDTP/PcmSlotCodec.hpp"
#include "Audio/Wire/Digi00x/DotScrambler.hpp"
#include "Audio/Wire/Digi00x/Digi00xSlotLayout.hpp"

#include "TxPacketizerTestSupport.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

namespace {

using namespace ASFW::Protocols::Audio::AMDTP;
using ASFW::Encoding::Digi00x::DotScrambler;

constexpr uint32_t kPcm = 18;
constexpr uint32_t kDbs = 19;
constexpr uint32_t kFrames = 4;
constexpr uint32_t kCipBytes = 8;

[[nodiscard]] uint32_t ReadBE32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | uint32_t{p[3]};
}

// Distinct, non-trivial samples whose middle bytes vary (and include zeros).
[[nodiscard]] std::vector<float> HostFrames(uint32_t frames) {
    std::vector<float> host(frames * kPcm);
    for (uint32_t f = 0; f < frames; ++f)
        for (uint32_t c = 0; c < kPcm; ++c)
            host[f * kPcm + c] = ((c % 3 == 0) ? 0.0f : 0.013f * static_cast<float>(c + 1)) *
                                 ((f % 2 == 0) ? 1.0f : -0.7f);
    return host;
}

struct Rig {
    AmdtpStreamConfig config{};
    AmdtpTxPolicy policy{};
    AmdtpPacketTimeline timeline{};
    std::array<PacketTimelineSlot, 8> slots{};
    AmdtpTxPacketizer packetizer{};
    AmdtpPayloadWriter writer{};
    std::array<std::array<uint8_t, 1024>, 4> packets{};
    uint64_t nextFrame{0};

    explicit Rig(bool scramble) {
        config.sampleRate = 48000;
        config.streamMode = StreamMode::Blocking;
        config.dbs = kDbs;
        config.pcmChannels = kPcm;
        config.framesPerDataPacket = 8;
        config.maxPacketBytes = 1024;
        policy.playbackChannelMap = ASFW::Encoding::Digi00x::PcmAfterMidiSlotMap(kPcm);
        policy.defaultNonAudioSlotWord = ASFW::Encoding::Digi00x::kIdleMidiWord;
        policy.initializeNonAudioSlots = true;
        policy.dotScramblePcm = scramble;
        EXPECT_TRUE(timeline.AttachSlots(slots.data(), slots.size()));
        packetizer.BindTimeline(&timeline);
        EXPECT_TRUE(packetizer.Configure(config, policy));
        writer.Configure(config, policy);
        writer.BindTimeline(&timeline);
    }
    // Arm one DATA packet of kFrames frames into packets[index].
    void Arm(uint64_t index) {
        AmdtpTimingState timing{};
        timing.txClockValid = false;  // SYT-unaware
        timing.disposition = AmdtpPacketDisposition::Data;
        timing.replayValid = true;
        timing.replayDataBlocks = kFrames;
        PreparedTxPacket prepared{};
        ASSERT_TRUE(ASFW::Testing::PrepareCadencePacket(
            packetizer, {index, packets[index].data(), static_cast<uint32_t>(packets[index].size())}, timing, nextFrame,
            prepared));
    }
    void Write(const std::vector<float>& host, uint64_t firstFrame, uint32_t frames) {
        const HostAudioBufferView view{.interleavedFloat32 = host.data() + firstFrame * kPcm,
                                       .firstFrame = firstFrame, .frameCount = frames,
                                       .frameCapacity = 0, .channels = kPcm};
        writer.WriteFloat32Interleaved(view, /*firstWritablePacket=*/0);
    }
    [[nodiscard]] uint32_t Slot(uint64_t packet, uint32_t frame, uint32_t slot) const {
        return ReadBE32(packets[packet].data() + kCipBytes + (frame * kDbs + slot) * 4U);
    }
};

// What the wire must carry: AM824 words in wire order, the middle byte of each
// PCM word run through one scrambler.
[[nodiscard]] std::vector<uint32_t> Expected(const std::vector<float>& host, uint32_t frames,
                                             bool scramble) {
    DotScrambler dot;
    std::vector<uint32_t> words;
    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t c = 0; c < kPcm; ++c) {
            uint32_t w = PcmSlotCodec::EncodeFloat32(host[f * kPcm + c], PcmSlotEncoding::Am824MBLA);
            if (scramble) {
                w = (w & ~0xFF00U) | (uint32_t{dot.Next(static_cast<uint8_t>(w >> 8))} << 8);
            }
            words.push_back(w);
        }
    }
    return words;
}

TEST(Digi00xTxWriter, PcmFollowsTheMidiSlotAndIsScrambledInWireOrder) {
    Rig rig(/*scramble=*/true);
    rig.Arm(0);
    const auto host = HostFrames(kFrames);
    rig.Write(host, 0, kFrames);

    const auto expected = Expected(host, kFrames, true);
    for (uint32_t f = 0; f < kFrames; ++f) {
        EXPECT_EQ(rig.Slot(0, f, 0), 0x80000000U) << "frame " << f << ": idle MIDI word";
        for (uint32_t c = 0; c < kPcm; ++c)
            EXPECT_EQ(rig.Slot(0, f, c + 1), expected[f * kPcm + c]) << "frame " << f << " ch " << c;
    }
    // The scrambler changed something: the stream is not plain AM824.
    EXPECT_NE(Expected(host, kFrames, false), expected);
}

TEST(Digi00xTxWriter, TheScramblerCarriesAcrossWritesAndPackets) {
    Rig rig(true);
    rig.Arm(0);
    rig.Arm(1);
    const auto host = HostFrames(2 * kFrames);
    // Three calls with boundaries that split a packet.
    rig.Write(host, 0, 3);
    rig.Write(host, 3, 2);
    rig.Write(host, 5, 2 * kFrames - 5);

    const auto expected = Expected(host, 2 * kFrames, true);
    for (uint32_t f = 0; f < 2 * kFrames; ++f)
        for (uint32_t c = 0; c < kPcm; ++c)
            EXPECT_EQ(rig.Slot(f / kFrames, f % kFrames, c + 1), expected[f * kPcm + c])
                << "frame " << f << " ch " << c;
}

TEST(Digi00xTxWriter, ResetStartsTheSequenceAgain) {
    Rig rig(true);
    rig.Arm(0);
    rig.Arm(1);
    const auto host = HostFrames(2 * kFrames);
    rig.Write(host, 0, kFrames);
    rig.writer.ResetStreamState();
    rig.Write(host, kFrames, kFrames);
    // The second packet scrambles as if it were the first of a stream.
    const std::vector<float> second(host.begin() + kFrames * kPcm, host.end());
    const auto expected = Expected(second, kFrames, true);
    for (uint32_t f = 0; f < kFrames; ++f)
        for (uint32_t c = 0; c < kPcm; ++c)
            EXPECT_EQ(rig.Slot(1, f, c + 1), expected[f * kPcm + c]);
}

TEST(Digi00xTxWriter, WithoutTheFlagPcmIsPlainAm824) {
    Rig rig(/*scramble=*/false);
    rig.Arm(0);
    const auto host = HostFrames(kFrames);
    rig.Write(host, 0, kFrames);
    const auto expected = Expected(host, kFrames, false);
    for (uint32_t f = 0; f < kFrames; ++f)
        for (uint32_t c = 0; c < kPcm; ++c)
            EXPECT_EQ(rig.Slot(0, f, c + 1), expected[f * kPcm + c]);
}

TEST(Digi00xTxWriter, AnUnwrittenPacketCarriesSilenceAndTheIdleMidiWord) {
    // A frame that misses its deadline keeps what the packetizer armed.
    Rig rig(true);
    rig.Arm(0);
    for (uint32_t f = 0; f < kFrames; ++f) {
        EXPECT_EQ(rig.Slot(0, f, 0), 0x80000000U) << "frame " << f;
        for (uint32_t c = 0; c < kPcm; ++c)
            EXPECT_EQ(rig.Slot(0, f, c + 1), 0x40000000U) << "frame " << f << " ch " << c;
    }
}

TEST(Digi00xTxWriter, TheSytFieldCarriesNoInfo) {
    Rig rig(true);
    rig.Arm(0);
    // CIP quadlet 1: [fmt:8 (with EOH bits)][fdf:8][syt:16].
    EXPECT_EQ(ReadBE32(rig.packets[0].data() + 4) & 0xFFFFU, 0xFFFFU);
}

TEST(Digi00xTxWriter, TheProfileMakesTheEngineSytUnawareAndScrambling) {
    const ASFW::Isoch::Audio::Digi00x::Profiles::Digi00xProfile profile{"Digi 003"};
    ASFW::Isoch::Audio::AudioStreamConfig tx{};
    ASSERT_TRUE(profile.BuildDefaultTxStreamConfig(tx));
    ASFW::Protocols::Audio::DICE::DiceTxStreamEngine engine;
    ASSERT_TRUE(engine.Configure(profile, tx));
    EXPECT_TRUE(engine.IsSytUnaware());
    EXPECT_TRUE(profile.TxStreamPolicy().dotScramblePcm);
}

} // namespace
