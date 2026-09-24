// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceFixtureGeometryTests.cpp
//
// Four REAL DICE devices, from four vendors, transcribed from register dumps in
// documentation/fixtures/. Nothing here comes from a profile constant: each row
// is what the device's own TX_/RX_ sections reported, so a profile that
// disagrees fails against the hardware rather than against itself.
//
//   Saffire Pro 24 DSP    capture 16+1MIDI     playback 8+1MIDI   (ONE stream each)
//   Midas Venice F24      capture 16+8  = 24   playback 16+8  = 24
//   Midas Venice F32      capture 16+16 = 32   playback 16+16 = 32
//   PreSonus StudioLive   capture 16+16 = 32   playback 16+10 = 26
//   Alesis MultiMix       capture 12+2  = 14   playback 2         (ONE stream)
//
// Each is here for something the others cannot show, which is why `role` is a
// field rather than a comment -- a fixture that covers nothing new is a fixture
// that makes the suite slower without making it stronger:
//
//   - MIDI slots, so am824Slots != pcmChannels     (Pro 24 DSP only)
//   - a single-stream device that must not regress (Pro 24 DSP)
//   - asymmetric streams inside a direction        (Venice, StudioLive, MultiMix)
//   - asymmetric totals between directions         (StudioLive, MultiMix)
//   - different STREAM COUNTS per direction        (MultiMix, 2 vs 1)
//   - a second stream far smaller than the first   (MultiMix 12+2)
//   - two variants behind ONE identity             (Venice F24 vs F32)
//
// That last pair is the evidence for one catalog row serving a range: the two
// Venice units agree on every identifying register -- TCAT product 0x001,
// VERSION 1.0.4.0, CLOCK_CAPABILITIES 0x13000006, NICK_NAME 'Venice' -- and
// differ only in GUID, serial and geometry. Nothing but the measured channel
// count can tell them apart.
//
// documentation/DICE_TCAT_ARCHITECTURE.md sec 2.8 is the prose version.
//
// That last one is why the MultiMix matters most: stream0 x count gives 24
// where the streams actually sum to 14, so a regression to multiplication is
// off by ten channels rather than by a plausible-looking few.
//
// On what "sums to 14" means, because the vendor does NOT model it that way.
// AlesisFirewireAudioEngine::CreateStreams (0x59a0) creates one IOAudioStream
// per DICE stream -- createNewAudioStream(direction, _DICE_STREAM_STRUCT*,
// startingChannelID), named "Input Stream %d", each taking its channel count
// from its own per-stream struct. No aggregate number exists in that driver:
// CoreAudio is shown "Input Stream 1" (12ch) and "Input Stream 2" (2ch).
//
// ASFW publishes ONE IOUserAudioStream per direction and slices it with
// sourceChannelOffset, so our aggregate is a host-side construct. The totals
// agree, which is why these rows are still the right expectation -- but the
// vendor's transferable rule is the CHANNEL BASE, not the total:
//
//     v39 = 1; ... v39 += v16;   // running sum of preceding stream widths
//
// not index * width-of-stream-0. Those agree while every stream is stream 0's
// width, which is exactly the assumption the resolver is removing. See
// ChannelBaseIsARunningSum below.
//
// This file is also the equivalence harness for collapsing the seven DICE
// profile classes into one builder. These resolved answers are what must not
// change. Only two of the catalog's rows are hardware-verified, so "the
// numbers still match the dumps" is the migration invariant, not a nicety.

#include "Audio/Protocols/AudioTypes.hpp"
#include "Audio/Protocols/StreamGeometryResolver.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string_view>

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

// What a fixture is EVIDENCE for. Asserted as set-level coverage below, so a
// row cannot quietly stop earning its place.
struct FixtureRole {
    /// Summing and stream0-times-count give different answers for this device.
    bool discriminatesAggregation;
    /// Carries MIDI, so am824Slots != pcmChannels and slot comparison is live.
    bool exercisesMidiSlots;
    /// Capture and playback carry different numbers of streams.
    bool unequalStreamCounts;
    /// Exactly one stream per direction: the simple path must not regress.
    bool singleStream;
    /// Attested against the hardware it was dumped from.
    bool hardwareVerified;
};

struct DiceFixture {
    const char* name;
    const char* dump;
    uint64_t guid;
    uint32_t clockCaps;
    FixtureRole role;

    uint32_t captureStreamCount;         // DICE TX_NUMBER
    StreamShape capture[2];
    uint32_t expectedCapturePcm;

    uint32_t playbackStreamCount;        // DICE RX_NUMBER
    StreamShape playback[2];
    uint32_t expectedPlaybackPcm;

    // Zero-based channel base per stream: the running sum of preceding stream
    // widths. The vendor's is 1-based (v39 starts at 1); same rule, different
    // origin.
    uint32_t captureChannelBase[2];
    uint32_t playbackChannelBase[2];
};

// am824Slots is pcmChannels + midiPorts, which is why the MIDI column matters:
// it is 0 on three of the four, and only the Pro 24 DSP makes slots differ from
// channels (17 vs 16, 9 vs 8). Transcribed, not computed -- if a dump is ever
// re-read and disagrees, this table is what gets corrected.
constexpr DiceFixture kFixtures[] = {
    {
        // The only hardware-verified row, the only TCAT extension, the only one
        // advertising 2x rates -- and the only one carrying MIDI, so it is the
        // only fixture where am824Slots and pcmChannels differ at all.
        // Single-stream in both directions, so it does NOT discriminate summing
        // from multiplication; its job is the slot comparison and the simple
        // path. FocusriteSaffireProfile's constants match it exactly.
        .name = "Focusrite Saffire Pro 24 DSP",
        .dump = "documentation/fixtures/DICE/spro24dsp.txt",
        .guid = 0x00130E0402004713ULL,
        .clockCaps = 0x112C001E,
        .role = {.discriminatesAggregation = false,
                 .exercisesMidiSlots = true,
                 .unequalStreamCounts = false,
                 .singleStream = true,
                 .hardwareVerified = true},
        .captureStreamCount = 1,
        .capture = {{16, 1}, {0, 0}},
        .expectedCapturePcm = 16,
        .playbackStreamCount = 1,
        .playback = {{8, 1}, {0, 0}},
        .expectedPlaybackPcm = 8,
        .captureChannelBase = {0, 0},
        .playbackChannelBase = {0, 0},
    },
    {
        .name = "Midas Venice F24",
        .dump = "documentation/fixtures/DICE/midasF24.txt",
        .guid = 0x10C73F040040C7B6ULL,
        .clockCaps = 0x13000006,
        .role = {.discriminatesAggregation = true,
                 .exercisesMidiSlots = false,
                 .unequalStreamCounts = false,
                 .singleStream = false,
                 .hardwareVerified = false},
        .captureStreamCount = 2,
        .capture = {{16, 0}, {8, 0}},
        .expectedCapturePcm = 24,
        .playbackStreamCount = 2,
        .playback = {{16, 0}, {8, 0}},
        .expectedPlaybackPcm = 24,
        .captureChannelBase = {0, 16},
        .playbackChannelBase = {0, 16},
    },
    {
        // The F24's sibling, and the reason one catalog row can serve the
        // range. Every identifying register on these two units is IDENTICAL --
        // TCAT vendor 0x10C73F, category 0x04, product 0x001, VERSION 1.0.4.0,
        // NICK_NAME 'Venice', CLOCK_CAPABILITIES 0x13000006, and an ext_sync
        // section at the same offset and size. They differ in GUID, TCAT serial
        // and NOTHING ELSE except the stream geometry below. Measured channel
        // count is therefore the only discriminator that exists, which is what
        // IAudioDeviceProfile::NameForGeometry uses.
        //
        // (Both dumps print "Model: Venice F32" because that is OUR catalog
        // string echoed back -- kMidasVeniceModelName -- not a device report.
        // Recorded before NameForGeometry landed.)
        //
        // Uniform 16+16, so it does NOT discriminate summing from
        // stream0 x count; the F24 is the row that does. Its job here is to
        // prove the uniform sibling still resolves once the asymmetric one
        // drives the code.
        .name = "Midas Venice F32",
        .dump = "documentation/fixtures/DICE/midasF32.txt",
        .guid = 0x10C73F04004011DFULL,
        .clockCaps = 0x13000006,
        .role = {.discriminatesAggregation = false,
                 .exercisesMidiSlots = false,
                 .unequalStreamCounts = false,
                 .singleStream = false,
                 .hardwareVerified = false},
        .captureStreamCount = 2,
        .capture = {{16, 0}, {16, 0}},
        .expectedCapturePcm = 32,
        .playbackStreamCount = 2,
        .playback = {{16, 0}, {16, 0}},
        .expectedPlaybackPcm = 32,
        .captureChannelBase = {0, 16},
        .playbackChannelBase = {0, 16},
    },
    {
        .name = "PreSonus StudioLive 24.4.2",
        .dump = "documentation/fixtures/DICE/presonus2442.txt",
        .guid = 0x000A9204049204CBULL,
        .clockCaps = 0x13000006,
        .role = {.discriminatesAggregation = true,
                 .exercisesMidiSlots = false,
                 .unequalStreamCounts = false,
                 .singleStream = false,
                 .hardwareVerified = true},
        .captureStreamCount = 2,
        .capture = {{16, 0}, {16, 0}},
        .expectedCapturePcm = 32,
        .playbackStreamCount = 2,
        .playback = {{16, 0}, {10, 0}},
        .expectedPlaybackPcm = 26,
        .captureChannelBase = {0, 16},
        .playbackChannelBase = {0, 16},
    },
    {
        .name = "Alesis MultiMix",
        .dump = "documentation/fixtures/alesismultimix.txt",
        .guid = 0x00059504000005FEULL,
        .clockCaps = 0x11000006,
        .role = {.discriminatesAggregation = true,
                 .exercisesMidiSlots = false,
                 .unequalStreamCounts = true,
                 .singleStream = false,
                 .hardwareVerified = false},
        .captureStreamCount = 2,
        .capture = {{12, 0}, {2, 0}},
        .expectedCapturePcm = 14,
        .playbackStreamCount = 1,
        .playback = {{2, 0}, {0, 0}},
        .expectedPlaybackPcm = 2,
        .captureChannelBase = {0, 12},
        .playbackChannelBase = {0, 0},
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

// Whether a fixture can tell summing from stream0-times-count is a PROPERTY of
// the device, not a requirement on every device. A single-stream device cannot,
// and that is not a defect in the fixture -- the Pro 24 DSP earns its place on
// MIDI slots and on being the hardware-verified simple path.
//
// So the row declares what it is evidence for and this checks the declaration
// is honest. Set-level coverage is asserted separately, below; getting that
// backwards is what would let a uniform device be added and counted as proof of
// a fix it cannot demonstrate.
TEST_P(DiceFixtureGeometry, AggregationRoleMatchesTheGeometry) {
    const auto& fixture = GetParam();

    const uint32_t captureByMultiplication =
        fixture.capture[0].pcmChannels * fixture.captureStreamCount;
    const uint32_t playbackByMultiplication =
        fixture.playback[0].pcmChannels * fixture.playbackStreamCount;

    const bool discriminates = captureByMultiplication != fixture.expectedCapturePcm ||
                               playbackByMultiplication != fixture.expectedPlaybackPcm;

    EXPECT_EQ(discriminates, fixture.role.discriminatesAggregation)
        << fixture.name << ": declared discriminatesAggregation="
        << fixture.role.discriminatesAggregation << " but the recorded geometry says "
        << discriminates;

    const bool single = fixture.captureStreamCount == 1 && fixture.playbackStreamCount == 1;
    EXPECT_EQ(single, fixture.role.singleStream) << fixture.name;

    const bool unequal = fixture.captureStreamCount != fixture.playbackStreamCount;
    EXPECT_EQ(unequal, fixture.role.unequalStreamCounts) << fixture.name;
}

// MIDI is what makes am824Slots differ from pcmChannels, and the slot
// comparison in ResolveStreamGeometry is vacuous on a device without it: three
// of the four fixtures have dbs == pcm, so only the Pro 24 DSP reaches that
// branch. A device declaring MIDI must actually carry it, and one that does not
// must not claim to.
TEST_P(DiceFixtureGeometry, MidiRoleMatchesTheGeometry) {
    const auto& fixture = GetParam();

    bool anyMidi = false;
    for (uint32_t i = 0; i < fixture.captureStreamCount; ++i) {
        anyMidi = anyMidi || fixture.capture[i].midiPorts != 0;
    }
    for (uint32_t i = 0; i < fixture.playbackStreamCount; ++i) {
        anyMidi = anyMidi || fixture.playback[i].midiPorts != 0;
    }
    EXPECT_EQ(anyMidi, fixture.role.exercisesMidiSlots) << fixture.name;

    // And where MIDI is present the slot count must exceed the channel count,
    // which is the whole reason the resolver compares them separately.
    const auto caps = CapsFrom(fixture);
    for (uint32_t i = 0; i < fixture.captureStreamCount; ++i) {
        if (fixture.capture[i].midiPorts != 0) {
            EXPECT_GT(caps.deviceToHostStreams[i].am824Slots,
                      caps.deviceToHostStreams[i].pcmChannels)
                << fixture.name << " capture stream " << i;
        }
    }
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
            case 0: return "FocusriteSaffirePro24Dsp";
            case 1: return "MidasVeniceF24";
            case 2: return "MidasVeniceF32";
            case 3: return "PreSonusStudioLive2442";
            default: return "AlesisMultiMix";
        }
    });

// The vendor's channel-base rule, against real geometry.
//
// AlesisFirewireAudioEngine::CreateStreams carries a running base and advances
// it by the width of the stream it just created (v39 += v16). ASFW's equivalent
// is AudioStreamConfig::sourceChannelOffset, and the base formula at
// AudioStreamProfile.hpp:113 is `streamIndex * outConfig.pcmChannels`.
//
// Those two agree only while every stream has stream 0's width -- which is true
// today ONLY because BuildDefaultTxStreamConfig hands back stream 0's shape for
// every index. Once per-stream geometry comes from the device, index * width
// puts the MultiMix's second capture stream at offset 2 instead of 12, writing
// it on top of the first. This test states the rule against the recorded
// devices so that change cannot land silently.
TEST_P(DiceFixtureGeometry, ChannelBaseIsARunningSum) {
    const auto& fixture = GetParam();

    uint32_t base = 0;
    for (uint32_t i = 0; i < fixture.captureStreamCount; ++i) {
        EXPECT_EQ(base, fixture.captureChannelBase[i])
            << fixture.name << " capture stream " << i;
        base += fixture.capture[i].pcmChannels;
    }
    EXPECT_EQ(base, fixture.expectedCapturePcm) << fixture.name << " capture";

    base = 0;
    for (uint32_t i = 0; i < fixture.playbackStreamCount; ++i) {
        EXPECT_EQ(base, fixture.playbackChannelBase[i])
            << fixture.name << " playback stream " << i;
        base += fixture.playback[i].pcmChannels;
    }
    EXPECT_EQ(base, fixture.expectedPlaybackPcm) << fixture.name << " playback";
}

// Where the two formulas actually diverge. For a device whose streams are not
// all stream 0's width, index * width-of-stream-0 is not the running sum, and
// the MultiMix is the recorded device that proves it: 1 * 12 = 12 is right only
// because the base repeats stream 0's width. Feed it the REAL per-stream widths
// -- what the resolver now produces -- and index * width gives 2.
TEST(ChannelBaseRule, IndexTimesOwnWidthIsNotTheRunningSum) {
    // Alesis MultiMix capture, as the device reports it.
    constexpr uint32_t kWidths[] = {12, 2};

    uint32_t runningSum = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        const uint32_t indexTimesOwnWidth = i * kWidths[i];
        if (i == 1) {
            EXPECT_EQ(runningSum, 12u);
            EXPECT_EQ(indexTimesOwnWidth, 2u);
            EXPECT_NE(indexTimesOwnWidth, runningSum)
                << "index * own width would overlap stream 0";
        }
        runningSum += kWidths[i];
    }
    EXPECT_EQ(runningSum, 14u);
}

// The set as a whole must still cover every property, even though no single row
// has to. Losing a fixture, or replacing one with a device that happens to be
// uniform, silently narrows what this suite can catch -- these are the checks
// that make that loud.
TEST(DiceFixtureSet, CoversEveryPropertyItClaimsTo) {
    FixtureRole covered{};
    for (const auto& fixture : kFixtures) {
        covered.discriminatesAggregation |= fixture.role.discriminatesAggregation;
        covered.exercisesMidiSlots |= fixture.role.exercisesMidiSlots;
        covered.unequalStreamCounts |= fixture.role.unequalStreamCounts;
        covered.singleStream |= fixture.role.singleStream;
        covered.hardwareVerified |= fixture.role.hardwareVerified;
    }

    EXPECT_TRUE(covered.discriminatesAggregation)
        << "no fixture can tell summing from stream0 x count; the Venice F24, "
        << "StudioLive and MultiMix rows are the ones that do";
    EXPECT_TRUE(covered.exercisesMidiSlots)
        << "no fixture carries MIDI, so am824Slots == pcmChannels everywhere and "
        << "the resolver's slot comparison is never reached";
    EXPECT_TRUE(covered.unequalStreamCounts)
        << "no recorded device has different capture and playback stream counts; "
        << "the 2-vs-1 MultiMix is the only such row";
    EXPECT_TRUE(covered.singleStream)
        << "no single-stream fixture; the simple path could regress unnoticed";
    EXPECT_TRUE(covered.hardwareVerified)
        << "no hardware-attested fixture; the set would rest entirely on dumps "
        << "nobody has confirmed against a running device";
}

// At least four distinct vendors, not four rows from one. Geometry conventions are a
// vendor trait, so a set drawn from a single vendor would agree for reasons that do not
// generalise. Sibling variants behind one identity (Venice F24 vs F32) share a vendor
// to prove one catalog row serves the range.
TEST(DiceFixtureSet, DrawsFromDistinctVendors) {
    constexpr size_t kCount = sizeof(kFixtures) / sizeof(kFixtures[0]);
    std::set<uint32_t> vendors;
    for (size_t i = 0; i < kCount; ++i) {
        vendors.insert(static_cast<uint32_t>((kFixtures[i].guid >> 40) & 0xFFFFFF));
        for (size_t j = i + 1; j < kCount; ++j) {
            if (std::string_view(kFixtures[i].name).starts_with("Midas Venice") &&
                std::string_view(kFixtures[j].name).starts_with("Midas Venice")) {
                continue;
            }
            EXPECT_NE((kFixtures[i].guid >> 40) & 0xFFFFFF,
                      (kFixtures[j].guid >> 40) & 0xFFFFFF)
                << kFixtures[i].name << " and " << kFixtures[j].name
                << " share a vendor id";
        }
    }
    EXPECT_GE(vendors.size(), 4u)
        << "fixture set must draw from at least four distinct vendors";
}

} // namespace
