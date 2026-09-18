// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// StreamGeometryResolverTests.cpp
//
// These cover the cases no bench device can produce. The asymmetric
// multi-stream shapes below (16+10) exist on a PreSonus StudioLive 24.4.2 that
// nobody working on this driver owns, so the host suite is the only place the
// resolution rule can be held to anything.

#include "Audio/Protocols/StreamGeometryResolver.hpp"

#include <gtest/gtest.h>

namespace {

using ASFW::Audio::ResolveStreamCount;
using ASFW::Audio::ResolveStreamGeometry;
using ASFW::Audio::StreamGeometrySource;
using ASFW::Audio::WireStreamGeometry;

constexpr WireStreamGeometry kNothing{};

// ---------------------------------------------------------------------------
// Per-stream geometry
// ---------------------------------------------------------------------------

TEST(StreamGeometryResolver, DeviceWinsWhenProfileIsSilent) {
    const auto d = ResolveStreamGeometry(WireStreamGeometry{.pcmChannels = 10, .am824Slots = 10},
                                         kNothing);
    EXPECT_EQ(d.source, StreamGeometrySource::kDevice);
    EXPECT_EQ(d.geometry.pcmChannels, 10);
    EXPECT_FALSE(d.disagrees);
    EXPECT_TRUE(d.Usable());
}

TEST(StreamGeometryResolver, ProfileIsUsedWhenTheDeviceStatesNothing) {
    // Families whose protocols publish only aggregate caps (BeBoB, Fireworks,
    // Mackie) legitimately land here. It must not be an error.
    const auto d = ResolveStreamGeometry(kNothing,
                                         WireStreamGeometry{.pcmChannels = 20, .am824Slots = 20});
    EXPECT_EQ(d.source, StreamGeometrySource::kProfile);
    EXPECT_EQ(d.geometry.pcmChannels, 20);
    EXPECT_FALSE(d.disagrees);
    EXPECT_TRUE(d.Usable());
}

TEST(StreamGeometryResolver, AgreementIsNotADisagreement) {
    const WireStreamGeometry same{.pcmChannels = 16, .am824Slots = 16, .midiPorts = 0};
    const auto d = ResolveStreamGeometry(same, same);
    EXPECT_EQ(d.source, StreamGeometrySource::kDevice);
    EXPECT_FALSE(d.disagrees);
    EXPECT_TRUE(d.Usable());
}

TEST(StreamGeometryResolver, ChannelCountDisagreementIsNotUsable) {
    const auto d = ResolveStreamGeometry(WireStreamGeometry{.pcmChannels = 10, .am824Slots = 10},
                                         WireStreamGeometry{.pcmChannels = 16, .am824Slots = 16});
    EXPECT_TRUE(d.disagrees);
    EXPECT_FALSE(d.Usable());
    // The device's value is still reported, so the caller can log both sides.
    EXPECT_EQ(d.geometry.pcmChannels, 10);
    EXPECT_EQ(d.deviceStated.pcmChannels, 10);
    EXPECT_EQ(d.profileStated.pcmChannels, 16);
}

TEST(StreamGeometryResolver, SlotCountAloneCanDisagree) {
    // Same PCM channels but a different data-block size: the device would
    // receive packets of the wrong length. Must not pass.
    const auto d = ResolveStreamGeometry(WireStreamGeometry{.pcmChannels = 16, .am824Slots = 17},
                                         WireStreamGeometry{.pcmChannels = 16, .am824Slots = 16});
    EXPECT_TRUE(d.disagrees);
    EXPECT_FALSE(d.Usable());
}

TEST(StreamGeometryResolver, AnUnstatedSlotCountIsNotADisagreement) {
    // am824Slots == 0 means "not stated", not "zero slots".
    const auto d = ResolveStreamGeometry(WireStreamGeometry{.pcmChannels = 16, .am824Slots = 0},
                                         WireStreamGeometry{.pcmChannels = 16, .am824Slots = 16});
    EXPECT_FALSE(d.disagrees);
    EXPECT_TRUE(d.Usable());
}

TEST(StreamGeometryResolver, NeitherSideStatingAnythingIsUnusableRatherThanInvented) {
    // The old DuplexStreamProfile fallback substituted the aggregate channel
    // count here, which produced plausible-but-wrong geometry instead of an
    // error. Nothing may be invented.
    const auto d = ResolveStreamGeometry(kNothing, kNothing);
    EXPECT_FALSE(d.Usable());
    EXPECT_EQ(d.geometry.pcmChannels, 0);
}

// The regression this whole header exists for: issue #115's StudioLive 24.4.2
// is the first device whose playback streams are unequal (16 + 10). The
// IAudioStreamProfile default replicates stream 0's shape at successive channel
// offsets, so it describes stream 1 as 16 channels. Framing 16 AM824 slots into
// a device RX stream that has 10 is silence with no error.
TEST(StreamGeometryResolver, AsymmetricPlaybackIsCaughtOnTheSecondStream) {
    const WireStreamGeometry deviceStream0{.pcmChannels = 16, .am824Slots = 16};
    const WireStreamGeometry deviceStream1{.pcmChannels = 10, .am824Slots = 10};
    const WireStreamGeometry uniformProfile{.pcmChannels = 16, .am824Slots = 16};

    const auto s0 = ResolveStreamGeometry(deviceStream0, uniformProfile);
    EXPECT_TRUE(s0.Usable());
    EXPECT_FALSE(s0.disagrees);

    const auto s1 = ResolveStreamGeometry(deviceStream1, uniformProfile);
    EXPECT_TRUE(s1.disagrees);
    EXPECT_FALSE(s1.Usable());
    EXPECT_EQ(s1.deviceStated.pcmChannels, 10);
    EXPECT_EQ(s1.profileStated.pcmChannels, 16);
}

// With a profile that states the asymmetry correctly, the same device is clean.
TEST(StreamGeometryResolver, AsymmetricPlaybackPassesWhenTheProfileMatches) {
    const auto s1 = ResolveStreamGeometry(WireStreamGeometry{.pcmChannels = 10, .am824Slots = 10},
                                          WireStreamGeometry{.pcmChannels = 10, .am824Slots = 10});
    EXPECT_TRUE(s1.Usable());
    EXPECT_FALSE(s1.disagrees);
}

// ---------------------------------------------------------------------------
// Stream count
// ---------------------------------------------------------------------------

constexpr uint32_t kMaxStreams = 4;

TEST(StreamCountResolver, DeviceCountWinsOverTheProfileDefault) {
    // The live shape on a Saffire whose profile never overrode TxStreamCount():
    // the HAL armed one stream while the transport armed what the device said.
    const auto d = ResolveStreamCount(2, 1, kMaxStreams);
    EXPECT_EQ(d.count, 2u);
    EXPECT_EQ(d.source, StreamGeometrySource::kDevice);
    EXPECT_TRUE(d.disagrees);
}

TEST(StreamCountResolver, AgreementIsClean) {
    const auto d = ResolveStreamCount(2, 2, kMaxStreams);
    EXPECT_EQ(d.count, 2u);
    EXPECT_FALSE(d.disagrees);
}

TEST(StreamCountResolver, ProfileIsUsedWhenTheDeviceStatesNothing) {
    const auto d = ResolveStreamCount(0, 2, kMaxStreams);
    EXPECT_EQ(d.count, 2u);
    EXPECT_EQ(d.source, StreamGeometrySource::kProfile);
    EXPECT_FALSE(d.disagrees);
}

TEST(StreamCountResolver, MoreStreamsThanTheHostSupportsIsRefusedNotTruncated) {
    // Both vendor drivers reject RX_NUMBER > 4 outright. Truncating would drop
    // a stream the device still transmits on.
    const auto d = ResolveStreamCount(5, 0, kMaxStreams);
    EXPECT_EQ(d.count, 0u);
    EXPECT_TRUE(d.disagrees);
}

TEST(StreamCountResolver, ExactlyTheHostBoundIsAccepted) {
    const auto d = ResolveStreamCount(kMaxStreams, 0, kMaxStreams);
    EXPECT_EQ(d.count, kMaxStreams);
    EXPECT_FALSE(d.disagrees);
}

TEST(StreamCountResolver, AProfileOverTheBoundIsClamped) {
    // No device opinion, so there is nothing to refuse — just do not walk off
    // the end of the fixed-size array.
    const auto d = ResolveStreamCount(0, 9, kMaxStreams);
    EXPECT_EQ(d.count, kMaxStreams);
}

} // namespace
