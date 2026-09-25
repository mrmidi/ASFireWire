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

#include <initializer_list>

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

// ---------------------------------------------------------------------------
// Resolved per-direction geometry (Stage 4)
//
// Built from the recorded register dumps in documentation/fixtures/DICE/, not
// from any profile constant: asserting a profile against itself proves
// nothing. Both devices are asymmetric per stream, which is what makes them
// discriminating -- an F32 (16+16 both ways) would let a stream-0 reader pass.
// ---------------------------------------------------------------------------

namespace {

using ASFW::Audio::ResolvedDirectionGeometry;
using ASFW::Audio::ResolvedDeviceGeometry;
using ASFW::Audio::kMaxResolvedStreams;

// Build a direction from per-stream PCM counts, device-stated with no profile
// opinion -- the shape the dumps describe.
ResolvedDirectionGeometry DeviceStated(std::initializer_list<uint16_t> pcmPerStream) {
    ResolvedDirectionGeometry dir{};
    dir.count = ASFW::Audio::ResolveStreamCount(
        static_cast<uint32_t>(pcmPerStream.size()), 0, kMaxResolvedStreams);
    uint32_t i = 0;
    for (const uint16_t pcm : pcmPerStream) {
        dir.streams[i] = ASFW::Audio::ResolveStreamGeometry(
            WireStreamGeometry{.pcmChannels = pcm, .am824Slots = pcm}, kNothing);
        ++i;
    }
    return dir;
}

} // namespace

// documentation/fixtures/DICE/midasF24.txt: TX [0]=16 [1]=8, RX [0]=16 [1]=8.
TEST(ResolvedStreamGeometry, VeniceF24AggregateIsNotStreamZeroTimesCount) {
    const auto capture = DeviceStated({16, 8});

    EXPECT_EQ(capture.StreamCount(), 2u);
    EXPECT_TRUE(capture.Usable());

    // What the device means.
    EXPECT_EQ(capture.TotalPcmChannels(), 24u);

    // What AudioStreamProfile::TxChannelCount() computes today: stream 0 times
    // the stream count. This is the Stage 4 defect, stated as an inequality so
    // the test fails if the aggregate is ever derived that way again.
    const uint32_t streamZeroTimesCount =
        capture.streams[0].geometry.pcmChannels * capture.StreamCount();
    EXPECT_EQ(streamZeroTimesCount, 32u);
    EXPECT_NE(capture.TotalPcmChannels(), streamZeroTimesCount);
}

// documentation/fixtures/DICE/presonus2442.txt: TX 16+16 = 32, RX 16+10 = 26.
TEST(ResolvedStreamGeometry, StudioLive2442DirectionsDoNotShareAShape) {
    ResolvedDeviceGeometry resolved{};
    resolved.capture = DeviceStated({16, 16});
    resolved.playback = DeviceStated({16, 10});

    EXPECT_TRUE(resolved.Usable());
    EXPECT_EQ(resolved.capture.TotalPcmChannels(), 32u);
    EXPECT_EQ(resolved.playback.TotalPcmChannels(), 26u);

    // A consumer that resolves one direction and reuses it for the other is
    // wrong by six channels on this device.
    EXPECT_NE(resolved.capture.TotalPcmChannels(),
              resolved.playback.TotalPcmChannels());
}

// An F32 is 16+16 both ways, so it cannot catch either defect above. Recorded
// so the fixture choice is not mistaken for arbitrary.
TEST(ResolvedStreamGeometry, VeniceF32WouldNotDiscriminate) {
    const auto capture = DeviceStated({16, 16});
    EXPECT_EQ(capture.TotalPcmChannels(),
              capture.streams[0].geometry.pcmChannels * capture.StreamCount());
}

TEST(ResolvedStreamGeometry, DisagreementMakesTheDirectionUnusableAndNamesTheStream) {
    ResolvedDirectionGeometry dir{};
    dir.count = ASFW::Audio::ResolveStreamCount(2, 0, kMaxResolvedStreams);
    dir.streams[0] = ASFW::Audio::ResolveStreamGeometry(
        WireStreamGeometry{.pcmChannels = 16, .am824Slots = 16},
        WireStreamGeometry{.pcmChannels = 16, .am824Slots = 16});
    // Stream 1: the device says 8, the profile insists on 16.
    dir.streams[1] = ASFW::Audio::ResolveStreamGeometry(
        WireStreamGeometry{.pcmChannels = 8, .am824Slots = 8},
        WireStreamGeometry{.pcmChannels = 16, .am824Slots = 16});

    EXPECT_FALSE(dir.Usable());
    EXPECT_EQ(dir.FirstDisagreeingStream(), 1u);

    ResolvedDeviceGeometry resolved{};
    resolved.capture = dir;
    EXPECT_FALSE(resolved.Usable());
}

// Weiss is TX-only: a direction carrying no streams must stay usable, or
// half-duplex devices regress.
TEST(ResolvedStreamGeometry, EmptyDirectionIsUsable) {
    ResolvedDeviceGeometry resolved{};
    resolved.capture = DeviceStated({16});
    EXPECT_EQ(resolved.playback.StreamCount(), 0u);
    EXPECT_TRUE(resolved.playback.Usable());
    EXPECT_EQ(resolved.playback.TotalPcmChannels(), 0u);
    EXPECT_TRUE(resolved.Usable());
}

// ---------------------------------------------------------------------------
// ResolveDirectionGeometry: the routine the backend actually calls. These feed
// it caps-shaped and profile-shaped accessors, so a change that resolves one
// direction from the other's inputs, or that stops walking the union of the
// two descriptions, fails here.
// ---------------------------------------------------------------------------

TEST(ResolveDirectionGeometryFn, DeviceStatedAsymmetricStreamsSurviveResolution) {
    // Venice F24 capture: device says 16 + 8, profile says nothing.
    const uint16_t devicePcm[] = {16, 8};
    const auto resolved = ASFW::Audio::ResolveDirectionGeometry(
        2, 0,
        [&](uint32_t i) {
            return WireStreamGeometry{.pcmChannels = devicePcm[i], .am824Slots = devicePcm[i]};
        },
        [](uint32_t) { return kNothing; });

    EXPECT_EQ(resolved.StreamCount(), 2u);
    EXPECT_TRUE(resolved.Usable());
    EXPECT_EQ(resolved.TotalPcmChannels(), 24u);
    EXPECT_EQ(resolved.streams[1].geometry.pcmChannels, 8u);
    EXPECT_EQ(resolved.streams[1].source, StreamGeometrySource::kDevice);
}

TEST(ResolveDirectionGeometryFn, ProfileDisagreementOnASecondStreamIsCaught) {
    // The profile replicates stream 0 (16) while the device states 16 + 8:
    // exactly what a non-indexed profile accessor produces.
    const uint16_t devicePcm[] = {16, 8};
    const auto resolved = ASFW::Audio::ResolveDirectionGeometry(
        2, 2,
        [&](uint32_t i) {
            return WireStreamGeometry{.pcmChannels = devicePcm[i], .am824Slots = devicePcm[i]};
        },
        [](uint32_t) { return WireStreamGeometry{.pcmChannels = 16, .am824Slots = 16}; });

    EXPECT_FALSE(resolved.Usable());
    EXPECT_EQ(resolved.FirstDisagreeingStream(), 1u);
}

TEST(ResolveDirectionGeometryFn, WalksTheUnionWhenOnlyTheProfileDescribesAStream) {
    // Device states one stream, profile describes two. The second must still be
    // resolved (from the profile) rather than dropped.
    const auto resolved = ASFW::Audio::ResolveDirectionGeometry(
        1, 2,
        [](uint32_t i) {
            return i == 0 ? WireStreamGeometry{.pcmChannels = 16, .am824Slots = 16} : kNothing;
        },
        [](uint32_t) { return WireStreamGeometry{.pcmChannels = 10, .am824Slots = 10}; });

    EXPECT_EQ(resolved.streams[1].geometry.pcmChannels, 10u);
    EXPECT_EQ(resolved.streams[1].source, StreamGeometrySource::kProfile);
    EXPECT_EQ(resolved.count.deviceStated, 1u);
}

TEST(ResolveDirectionGeometryFn, StreamCountBeyondTheHostBoundIsRefusedNotTruncated) {
    const auto resolved = ASFW::Audio::ResolveDirectionGeometry(
        kMaxResolvedStreams + 3, 0,
        [](uint32_t) { return WireStreamGeometry{.pcmChannels = 8, .am824Slots = 8}; },
        [](uint32_t) { return kNothing; });
    EXPECT_TRUE(resolved.count.disagrees);
    EXPECT_FALSE(resolved.Usable());
}

