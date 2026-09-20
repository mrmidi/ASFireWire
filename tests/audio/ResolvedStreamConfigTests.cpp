// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ResolvedStreamConfigTests.cpp
//
// The step that closes the loop: resolved per-stream geometry, carried across
// the nub, becoming the AudioStreamConfig ASFWAudioDevice::StartIO hands to the
// packetizer. Before it, StartIO built playback streams from the profile's
// compiled-in constants while the transport reserved bandwidth from the device.
//
// Geometry here comes from the recorded dumps in documentation/fixtures/, never
// from a profile. The Venice F24 is the case that matters: its profile
// describes an F32 (16 + 16) and the device carries 16 + 8.

#include "Audio/DriverKit/Config/ResolvedStreamConfig.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using ASFW::Isoch::Audio::AudioStreamConfig;
using ASFW::Isoch::Audio::BuildResolvedTxStreamConfig;
using ASFW::Isoch::Audio::IAudioStreamProfile;
using ASFW::Isoch::Audio::ParsedWireStream;
using ASFW::Isoch::Audio::ResolvedPlaybackStreamCount;
using ASFW::Isoch::Audio::TxPacketBytesForStreamConfig;

// Stands in for MidasVeniceProfile: two playback streams of 16, which is the
// F32's shape and the seed every Venice carries regardless of variant. The
// framing constants are the ones the DICE registers do not hold.
class F32ShapedProfile : public IAudioStreamProfile {
public:
    const char* Name() const noexcept override { return "F32ShapedProfile"; }
    ASFW::Encoding::AudioWireFormat TxWireFormat() const noexcept override { return {}; }
    ASFW::Encoding::AudioWireFormat RxWireFormat() const noexcept override { return {}; }
    uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t TxReportedLatencyFrames(double) const noexcept override { return 0; }
    uint32_t RxReportedLatencyFrames(double) const noexcept override { return 0; }

    uint32_t TxStreamCount() const noexcept override { return 2; }
    uint32_t RxStreamCount() const noexcept override { return 2; }

    bool BuildDefaultTxStreamConfig(AudioStreamConfig& c) const noexcept override {
        c = {};
        c.pcmChannels = 16;
        c.dbs = 16;
        c.midiSlots = 0;
        c.framesPerDataPacket = 8;
        c.fdf = 0x02;
        c.fmt = 0x10;
        c.sid = 0;
        return true;
    }
    bool BuildDefaultRxStreamConfig(AudioStreamConfig& c) const noexcept override {
        return BuildDefaultTxStreamConfig(c);
    }
};

// One playback stream of 16 + 1 MIDI: the Saffire base shape. Used to prove the
// resolved count is the DEVICE's, so a profile is never asked about a stream it
// does not know exists.
class SingleStreamProfile : public F32ShapedProfile {
public:
    uint32_t TxStreamCount() const noexcept override { return 1; }
    bool BuildDefaultTxStreamConfig(AudioStreamConfig& c) const noexcept override {
        c = {};
        c.pcmChannels = 16;
        c.midiSlots = 1;
        c.dbs = 17;
        c.framesPerDataPacket = 8;
        c.fdf = 0x02;
        c.fmt = 0x10;
        return true;
    }
};

// documentation/fixtures/DICE/midasF24.txt, playback (DICE RX): 16 then 8, with
// the second stream starting after the first.
constexpr ParsedWireStream kVeniceF24Playback[] = {
    {.pcmChannels = 16, .am824Slots = 16, .midiPorts = 0, .channelOffset = 0},
    {.pcmChannels = 8, .am824Slots = 8, .midiPorts = 0, .channelOffset = 16},
};

// A Venice F32 as the device reports it: two streams of 16, which happens to
// match the profile's seed. Kept explicit so "the F32 still works" is an
// assertion rather than an inference from the F24 case.
constexpr ParsedWireStream kVeniceF32Playback[] = {
    {.pcmChannels = 16, .am824Slots = 16, .midiPorts = 0, .channelOffset = 0},
    {.pcmChannels = 16, .am824Slots = 16, .midiPorts = 0, .channelOffset = 16},
};

} // namespace

// The variant that already worked must keep working, driven through the same
// path rather than assumed. Measured 16 + 16 has to produce what the profile
// used to: equal widths, offsets 0 and 16, equal packet sizes.
TEST(ResolvedStreamConfig, VeniceF32KeepsSixteenPlusSixteenThroughTheResolvedPath) {
    const F32ShapedProfile profile;

    EXPECT_EQ(ResolvedPlaybackStreamCount(profile.TxStreamCount(), 2), 2u);

    AudioStreamConfig first{};
    AudioStreamConfig second{};
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kVeniceF32Playback, 2, 0, first));
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kVeniceF32Playback, 2, 1, second));

    EXPECT_EQ(first.pcmChannels, 16);
    EXPECT_EQ(first.dbs, 16);
    EXPECT_EQ(first.sourceChannelOffset, 0);
    EXPECT_EQ(second.pcmChannels, 16);
    EXPECT_EQ(second.dbs, 16);
    EXPECT_EQ(second.sourceChannelOffset, 16);

    EXPECT_EQ(TxPacketBytesForStreamConfig(first), TxPacketBytesForStreamConfig(second));
    EXPECT_EQ(second.sourceChannelOffset + second.pcmChannels, 32);
}

// And the resolved path must agree with the profile-only path for the device
// whose profile was always right. If these ever diverge, the F32 regressed.
TEST(ResolvedStreamConfig, F32ResolvedMatchesWhatTheProfileAloneWouldHaveBuilt) {
    const F32ShapedProfile profile;

    for (uint32_t index = 0; index < 2; ++index) {
        AudioStreamConfig resolved{};
        AudioStreamConfig profileOnly{};
        ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kVeniceF32Playback, 2, index, resolved));
        ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, nullptr, 0, index, profileOnly));

        EXPECT_EQ(resolved.pcmChannels, profileOnly.pcmChannels) << "stream " << index;
        EXPECT_EQ(resolved.dbs, profileOnly.dbs) << "stream " << index;
        EXPECT_EQ(resolved.sourceChannelOffset, profileOnly.sourceChannelOffset)
            << "stream " << index;
        EXPECT_EQ(TxPacketBytesForStreamConfig(resolved),
                  TxPacketBytesForStreamConfig(profileOnly)) << "stream " << index;
    }
}

// The headline case. Two engines, 16 at offset 0 and 8 at offset 16, with the
// DBS each stream actually frames with -- not the profile's 16 / 16.
TEST(ResolvedStreamConfig, VeniceF24BuildsSixteenThenEightFromTheDevice) {
    const F32ShapedProfile profile;

    EXPECT_EQ(ResolvedPlaybackStreamCount(profile.TxStreamCount(), 2), 2u);

    AudioStreamConfig first{};
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kVeniceF24Playback, 2, 0, first));
    EXPECT_EQ(first.pcmChannels, 16);
    EXPECT_EQ(first.dbs, 16);
    EXPECT_EQ(first.sourceChannelOffset, 0);

    AudioStreamConfig second{};
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kVeniceF24Playback, 2, 1, second));
    EXPECT_EQ(second.pcmChannels, 8) << "the F32 profile would have said 16";
    EXPECT_EQ(second.dbs, 8) << "framing an 8-slot stream with DBS 16 is the defect";
    EXPECT_EQ(second.sourceChannelOffset, 16) << "running sum, not index * width";

    // The two streams tile the 24 channels the endpoint publishes, with no gap
    // and no overlap.
    EXPECT_EQ(first.sourceChannelOffset + first.pcmChannels, second.sourceChannelOffset);
    EXPECT_EQ(second.sourceChannelOffset + second.pcmChannels, 24);
}

// Packet allocation follows the same config, so the buffer StartIO allocates
// and the geometry it frames from cannot disagree.
TEST(ResolvedStreamConfig, PacketSizeFollowsTheResolvedDbs) {
    const F32ShapedProfile profile;

    AudioStreamConfig first{};
    AudioStreamConfig second{};
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kVeniceF24Playback, 2, 0, first));
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kVeniceF24Playback, 2, 1, second));

    // 8 frames per data packet, 4 bytes per slot, plus the CIP header.
    EXPECT_EQ(TxPacketBytesForStreamConfig(first), 8u + 8u * 16u * 4u);
    EXPECT_EQ(TxPacketBytesForStreamConfig(second), 8u + 8u * 8u * 4u);
    EXPECT_LT(TxPacketBytesForStreamConfig(second), TxPacketBytesForStreamConfig(first))
        << "the narrower stream must allocate a smaller packet";
}

// Framing constants the DICE registers do not carry stay the profile's. The
// device owns the shape; the profile owns the encoding.
TEST(ResolvedStreamConfig, ProfileKeepsTheConstantsTheRegistersDoNotHold) {
    const F32ShapedProfile profile;

    AudioStreamConfig second{};
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kVeniceF24Playback, 2, 1, second));
    EXPECT_EQ(second.framesPerDataPacket, 8);
    EXPECT_EQ(second.fdf, 0x02);
    EXPECT_EQ(second.fmt, 0x10);
}

// The resolved count belongs to the device. A profile declaring one playback
// stream must still yield a config for a second one the device reports, rather
// than failing because its own accessor rejects the index.
TEST(ResolvedStreamConfig, DeviceMayCarryMoreStreamsThanTheProfileDeclares) {
    const SingleStreamProfile profile;
    ASSERT_EQ(profile.TxStreamCount(), 1u);
    EXPECT_EQ(ResolvedPlaybackStreamCount(profile.TxStreamCount(), 2), 2u);

    AudioStreamConfig second{};
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kVeniceF24Playback, 2, 1, second))
        << "asking the profile about stream 1 would have failed";
    EXPECT_EQ(second.pcmChannels, 8);
    EXPECT_EQ(second.sourceChannelOffset, 16);
    // Stream 0's MIDI slot is the profile's, and the device's slot count wins.
    EXPECT_EQ(second.midiSlots, 0);
}

// MIDI rides in the slot count, so a device carrying it must not have its DBS
// reduced to the PCM count. Saffire Pro 24 DSP: 16 PCM + 1 MIDI, DBS 17.
TEST(ResolvedStreamConfig, MidiSlotsSurviveIntoTheDataBlockSize) {
    const SingleStreamProfile profile;
    constexpr ParsedWireStream kPro24Playback[] = {
        {.pcmChannels = 8, .am824Slots = 9, .midiPorts = 1, .channelOffset = 0},
    };

    AudioStreamConfig only{};
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kPro24Playback, 1, 0, only));
    EXPECT_EQ(only.pcmChannels, 8);
    EXPECT_EQ(only.midiSlots, 1);
    EXPECT_EQ(only.dbs, 9) << "DBS must count the MIDI slot, not just PCM";
    EXPECT_EQ(TxPacketBytesForStreamConfig(only), 8u + 8u * 9u * 4u);
}

// With nothing carried, behaviour is exactly what it was before the nub carried
// geometry: the profile answers. Correct for a uniform device, and the reason
// the caller logs when it happens.
TEST(ResolvedStreamConfig, FallsBackToTheProfileWhenNothingWasResolved) {
    const F32ShapedProfile profile;
    EXPECT_EQ(ResolvedPlaybackStreamCount(profile.TxStreamCount(), 0), 2u);

    AudioStreamConfig second{};
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, nullptr, 0, 1, second));
    EXPECT_EQ(second.pcmChannels, 16) << "the profile's own answer";
    EXPECT_EQ(second.dbs, 16);

    AudioStreamConfig alsoSecond{};
    ASSERT_TRUE(BuildResolvedTxStreamConfig(profile, kVeniceF24Playback, 1, 1, alsoSecond));
    EXPECT_EQ(alsoSecond.pcmChannels, 16)
        << "index beyond the resolved count falls back too";
}
