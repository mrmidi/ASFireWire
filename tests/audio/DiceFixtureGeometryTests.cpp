// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceFixtureGeometryTests.cpp
//
// Three REAL DICE devices, from three vendors, transcribed from register dumps
// in documentation/fixtures/. Nothing here comes from a profile constant: each
// row is what the device's own TX_/RX_ sections reported, so a profile that
// disagrees fails against the hardware rather than against itself.
//
//   Midas Venice F24      capture 16+8  = 24   playback 16+8  = 24
//   PreSonus StudioLive   capture 16+16 = 32   playback 16+10 = 26
//   Alesis MultiMix       capture 12+2  = 14   playback 2          (ONE stream)
//
// Together they discriminate more than a synthetic pair could:
//
//   - asymmetric streams inside a direction        (all three)
//   - asymmetric totals between directions         (StudioLive, MultiMix)
//   - different STREAM COUNTS per direction        (MultiMix, 2 vs 1)
//   - a second stream far smaller than the first   (MultiMix 12+2)
//
// That last one is why the MultiMix matters most: stream0 x count gives 24
// against a true 14, so a regression to multiplication is off by ten channels
// rather than by a plausible-looking few.
//
// This file is also the equivalence harness for collapsing the seven DICE
// profile classes into one builder. These resolved answers are what must not
// change. Only two of the catalog's rows are hardware-verified, so "the
// numbers still match the dumps" is the migration invariant, not a nicety.

#include "Audio/Protocols/AudioTypes.hpp"
#include "Audio/Protocols/StreamGeometryResolver.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::AudioStreamWireInfo;
using ASFW::Audio::ResolveDirectionGeometry;
using ASFW::Audio::StreamGeometrySource;
using ASFW::Audio::WireStreamGeometry;

struct StreamShape {
    uint16_t pcmChannels;
    uint16_t midiPorts;
};

struct DiceFixture {
    const char* name;
    const char* dump;
    uint64_t guid;
    uint32_t clockCaps;

    uint32_t captureStreamCount;         // DICE TX_NUMBER
    StreamShape capture[2];
    uint32_t expectedCapturePcm;

    uint32_t playbackStreamCount;        // DICE RX_NUMBER
    StreamShape playback[2];
    uint32_t expectedPlaybackPcm;
};

// MIDI is 0 on all three, so the AM824 slot count equals the PCM channel count.
// Transcribed, not computed: if a dump is ever re-read and disagrees, this
// table is what gets corrected.
constexpr DiceFixture kFixtures[] = {
    {
        .name = "Midas Venice F24",
        .dump = "documentation/fixtures/DICE/midasF24.txt",
        .guid = 0x10C73F040040C7B6ULL,
        .clockCaps = 0x13000006,
        .captureStreamCount = 2,
        .capture = {{16, 0}, {8, 0}},
        .expectedCapturePcm = 24,
        .playbackStreamCount = 2,
        .playback = {{16, 0}, {8, 0}},
        .expectedPlaybackPcm = 24,
    },
    {
        .name = "PreSonus StudioLive 24.4.2",
        .dump = "documentation/fixtures/DICE/presonus2442.txt",
        .guid = 0x000A9204049204CBULL,
        .clockCaps = 0x13000006,
        .captureStreamCount = 2,
        .capture = {{16, 0}, {16, 0}},
        .expectedCapturePcm = 32,
        .playbackStreamCount = 2,
        .playback = {{16, 0}, {10, 0}},
        .expectedPlaybackPcm = 26,
    },
    {
        .name = "Alesis MultiMix",
        .dump = "documentation/fixtures/alesismultimix.txt",
        .guid = 0x00059504000005FEULL,
        .clockCaps = 0x11000006,
        .captureStreamCount = 2,
        .capture = {{12, 0}, {2, 0}},
        .expectedCapturePcm = 14,
        .playbackStreamCount = 1,
        .playback = {{2, 0}, {0, 0}},
        .expectedPlaybackPcm = 2,
    },
};

// Build the caps a protocol would publish after reading this device.
AudioStreamRuntimeCaps CapsFrom(const DiceFixture& fixture) noexcept {
    AudioStreamRuntimeCaps caps{};
    caps.sampleRateHz = 48000;
    caps.deviceToHostStreamCount = fixture.captureStreamCount;
    caps.hostToDeviceStreamCount = fixture.playbackStreamCount;
    for (uint32_t i = 0; i < fixture.captureStreamCount; ++i) {
        caps.deviceToHostStreams[i] = AudioStreamWireInfo{
            .isoChannel = static_cast<uint8_t>(i),
            .pcmChannels = fixture.capture[i].pcmChannels,
            .am824Slots = static_cast<uint16_t>(fixture.capture[i].pcmChannels +
                                                fixture.capture[i].midiPorts),
            .midiPorts = fixture.capture[i].midiPorts};
    }
    for (uint32_t i = 0; i < fixture.playbackStreamCount; ++i) {
        caps.hostToDeviceStreams[i] = AudioStreamWireInfo{
            .isoChannel = static_cast<uint8_t>(i + 2),
            .pcmChannels = fixture.playback[i].pcmChannels,
            .am824Slots = static_cast<uint16_t>(fixture.playback[i].pcmChannels +
                                                fixture.playback[i].midiPorts),
            .midiPorts = fixture.playback[i].midiPorts};
    }
    return caps;
}

WireStreamGeometry CaptureFromDevice(const AudioStreamRuntimeCaps& caps, uint32_t i) noexcept {
    if (i >= caps.deviceToHostStreamCount) {
        return {};
    }
    const auto& w = caps.deviceToHostStreams[i];
    return {.pcmChannels = w.pcmChannels, .am824Slots = w.am824Slots, .midiPorts = w.midiPorts};
}

WireStreamGeometry PlaybackFromDevice(const AudioStreamRuntimeCaps& caps, uint32_t i) noexcept {
    if (i >= caps.hostToDeviceStreamCount) {
        return {};
    }
    const auto& w = caps.hostToDeviceStreams[i];
    return {.pcmChannels = w.pcmChannels, .am824Slots = w.am824Slots, .midiPorts = w.midiPorts};
}

class DiceFixtureGeometry : public ::testing::TestWithParam<DiceFixture> {};

// The device describes itself and no profile contradicts it -- the state the
// resolver is meant to reach, and the state a collapsed single-profile build
// must still reach for every row.
TEST_P(DiceFixtureGeometry, ResolvesTheGeometryTheDeviceReported) {
    const auto& fixture = GetParam();
    const auto caps = CapsFrom(fixture);

    const auto capture = ResolveDirectionGeometry(
        caps.deviceToHostStreamCount, 0,
        [&](uint32_t i) { return CaptureFromDevice(caps, i); },
        [](uint32_t) { return WireStreamGeometry{}; });
    const auto playback = ResolveDirectionGeometry(
        caps.hostToDeviceStreamCount, 0,
        [&](uint32_t i) { return PlaybackFromDevice(caps, i); },
        [](uint32_t) { return WireStreamGeometry{}; });

    EXPECT_TRUE(capture.Usable()) << fixture.name << " (" << fixture.dump << ")";
    EXPECT_TRUE(playback.Usable()) << fixture.name;

    EXPECT_EQ(capture.StreamCount(), fixture.captureStreamCount) << fixture.name;
    EXPECT_EQ(playback.StreamCount(), fixture.playbackStreamCount) << fixture.name;

    EXPECT_EQ(capture.TotalPcmChannels(), fixture.expectedCapturePcm) << fixture.name;
    EXPECT_EQ(playback.TotalPcmChannels(), fixture.expectedPlaybackPcm) << fixture.name;

    EXPECT_EQ(capture.count.source, StreamGeometrySource::kDevice) << fixture.name;
    for (uint32_t i = 0; i < fixture.captureStreamCount; ++i) {
        EXPECT_EQ(capture.streams[i].geometry.pcmChannels, fixture.capture[i].pcmChannels)
            << fixture.name << " capture stream " << i;
    }
    for (uint32_t i = 0; i < fixture.playbackStreamCount; ++i) {
        EXPECT_EQ(playback.streams[i].geometry.pcmChannels, fixture.playback[i].pcmChannels)
            << fixture.name << " playback stream " << i;
    }
}

// Every fixture must be one that stream0-times-count gets WRONG. A fixture set
// that a uniform device would also satisfy proves nothing, and an F32 (16+16
// both ways) is exactly such a device -- which is why the F24 dump is the one
// worth having.
TEST_P(DiceFixtureGeometry, DiscriminatesAgainstStreamZeroTimesCount) {
    const auto& fixture = GetParam();

    const uint32_t captureByMultiplication =
        fixture.capture[0].pcmChannels * fixture.captureStreamCount;
    const uint32_t playbackByMultiplication =
        fixture.playback[0].pcmChannels * fixture.playbackStreamCount;

    const bool captureDiscriminates = captureByMultiplication != fixture.expectedCapturePcm;
    const bool playbackDiscriminates = playbackByMultiplication != fixture.expectedPlaybackPcm;

    EXPECT_TRUE(captureDiscriminates || playbackDiscriminates)
        << fixture.name << " cannot tell summing from multiplication; it is not "
        << "evidence for the geometry fix and should not be counted as such";
}

// Direction independence. The MultiMix is the row that enforces it: two capture
// streams against one playback stream, so any code path that resolves one
// direction and reuses the answer for the other reports a playback stream the
// device does not have.
TEST_P(DiceFixtureGeometry, ResolvesDirectionsIndependently) {
    const auto& fixture = GetParam();
    const auto caps = CapsFrom(fixture);

    const auto capture = ResolveDirectionGeometry(
        caps.deviceToHostStreamCount, 0,
        [&](uint32_t i) { return CaptureFromDevice(caps, i); },
        [](uint32_t) { return WireStreamGeometry{}; });
    const auto playback = ResolveDirectionGeometry(
        caps.hostToDeviceStreamCount, 0,
        [&](uint32_t i) { return PlaybackFromDevice(caps, i); },
        [](uint32_t) { return WireStreamGeometry{}; });

    EXPECT_EQ(capture.TotalPcmChannels(), fixture.expectedCapturePcm);
    EXPECT_EQ(playback.TotalPcmChannels(), fixture.expectedPlaybackPcm);
    if (fixture.captureStreamCount != fixture.playbackStreamCount) {
        EXPECT_NE(capture.StreamCount(), playback.StreamCount()) << fixture.name;
    }
}

INSTANTIATE_TEST_SUITE_P(
    RecordedDevices, DiceFixtureGeometry, ::testing::ValuesIn(kFixtures),
    [](const ::testing::TestParamInfo<DiceFixture>& info) {
        switch (info.index) {
            case 0: return "MidasVeniceF24";
            case 1: return "PreSonusStudioLive2442";
            default: return "AlesisMultiMix";
        }
    });

// The set as a whole. At least one fixture must exercise unequal stream counts
// between directions, or the harness cannot catch a regression that assumes one
// count serves both.
TEST(DiceFixtureSet, CoversUnequalStreamCountsBetweenDirections) {
    bool sawUnequal = false;
    for (const auto& fixture : kFixtures) {
        sawUnequal = sawUnequal || fixture.captureStreamCount != fixture.playbackStreamCount;
    }
    EXPECT_TRUE(sawUnequal)
        << "no recorded device has different capture and playback stream counts; "
        << "the 2-vs-1 MultiMix case is the only such row";
}

// Three vendors, not three rows from one. Geometry conventions are a vendor
// trait, so a set drawn from a single vendor would agree for reasons that do
// not generalise.
TEST(DiceFixtureSet, DrawsFromThreeDistinctVendors) {
    const uint32_t vendors[] = {
        static_cast<uint32_t>((kFixtures[0].guid >> 40) & 0xFFFFFF),
        static_cast<uint32_t>((kFixtures[1].guid >> 40) & 0xFFFFFF),
        static_cast<uint32_t>((kFixtures[2].guid >> 40) & 0xFFFFFF),
    };
    EXPECT_NE(vendors[0], vendors[1]);
    EXPECT_NE(vendors[1], vendors[2]);
    EXPECT_NE(vendors[0], vendors[2]);
}

} // namespace
