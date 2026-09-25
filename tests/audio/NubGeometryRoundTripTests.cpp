// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// NubGeometryRoundTripTests.cpp
//
// The nub boundary, end to end: Model::ASFWAudioDevice -> property dictionary
// -> ParsedAudioDriverConfig. Two IOService objects with a serialized contract
// between them, so an encode that does not match its decode loses the geometry
// silently and the audio side falls back to profile constants -- which is the
// mismatch the whole resolution path exists to remove.
//
// This became testable when the DriverKit container mocks were made to carry
// values (tests/mocks/DriverKit/OS{Number,String,Array,Dictionary}.h). Before
// that they were no-ops that read back zero, so the encode side could have been
// deleted entirely without a test noticing.

#include "Audio/DriverKit/Config/AudioDriverConfig.hpp"
#include "Audio/Model/ASFWAudioDevice.hpp"

#include <DriverKit/OSDictionary.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using ASFW::Audio::Model::ASFWAudioDevice;
using ASFW::Audio::Model::ASFWAudioWireStream;
using ASFW::Isoch::Audio::ParsedAudioDriverConfig;

ASFWAudioDevice MakeVeniceF24() {
    ASFWAudioDevice device{};
    device.guid = 0x10C73F040040C7B6ULL;
    device.deviceName = "Midas Venice F24";
    device.inputChannelCount = 24;
    device.outputChannelCount = 24;
    device.channelCount = 24;
    device.sampleRates = {44100u, 48000u};
    device.currentSampleRate = 48000u;
    // Recorded playback (DICE RX): 16 then 8, second starting after the first.
    device.playbackStreams = {
        {.pcmChannels = 16, .am824Slots = 16, .midiPorts = 0, .channelOffset = 0},
        {.pcmChannels = 8, .am824Slots = 8, .midiPorts = 0, .channelOffset = 16},
    };
    device.captureStreams = device.playbackStreams;
    device.resolvedGeometryRequired = true;
    return device;
}

ParsedAudioDriverConfig RoundTrip(const ASFWAudioDevice& device, bool& outPublished) {
    auto* properties = OSDictionary::withCapacity(32);
    outPublished = device.PopulateNubProperties(properties);
    ParsedAudioDriverConfig parsed{};
    ASFW::Isoch::Audio::ParseAudioDriverConfigFromProperties(properties, parsed);
    properties->release();
    return parsed;
}

} // namespace

// The headline: the F24's 16 + 8 survives the crossing intact, offsets included.
TEST(NubGeometryRoundTrip, VeniceF24PlaybackGeometrySurvivesTheCrossing) {
    bool published = false;
    const auto parsed = RoundTrip(MakeVeniceF24(), published);
    ASSERT_TRUE(published);

    ASSERT_EQ(parsed.playbackStreamCount, 2u);
    EXPECT_EQ(parsed.playbackStreams[0].pcmChannels, 16u);
    EXPECT_EQ(parsed.playbackStreams[0].am824Slots, 16u);
    EXPECT_EQ(parsed.playbackStreams[0].channelOffset, 0u);
    EXPECT_EQ(parsed.playbackStreams[1].pcmChannels, 8u);
    EXPECT_EQ(parsed.playbackStreams[1].am824Slots, 8u);
    EXPECT_EQ(parsed.playbackStreams[1].channelOffset, 16u);

    // Both directions cross, and the aggregate the HAL sees still agrees with
    // the sum of the streams.
    ASSERT_EQ(parsed.captureStreamCount, 2u);
    EXPECT_EQ(parsed.inputChannelCount, 24u);
    EXPECT_EQ(parsed.outputChannelCount, 24u);
    EXPECT_EQ(parsed.playbackStreams[0].pcmChannels + parsed.playbackStreams[1].pcmChannels,
              parsed.outputChannelCount);
}

// The flag that forbids falling back to profile constants must itself survive:
// if it is lost, losing the arrays becomes indistinguishable from a family that
// never published any.
TEST(NubGeometryRoundTrip, ResolvedGeometryRequiredSurvives) {
    bool published = false;
    const auto parsed = RoundTrip(MakeVeniceF24(), published);
    ASSERT_TRUE(published);
    EXPECT_TRUE(parsed.resolvedGeometryRequired);
}

// The device's own rate list must reach the audio side marked as such, or the
// audio side replaces it with a profile's.
TEST(NubGeometryRoundTrip, DeviceSampleRatesSurvive) {
    ASFWAudioDevice device = MakeVeniceF24();
    device.sampleRates = {32000u, 48000u};
    device.deviceSampleRates = true;
    bool published = false;
    const auto parsed = RoundTrip(device, published);
    ASSERT_TRUE(published);
    EXPECT_TRUE(parsed.deviceSampleRates);
    ASSERT_EQ(parsed.sampleRateCount, 2u);
    EXPECT_EQ(parsed.sampleRates[0], 32000.0);
    EXPECT_EQ(parsed.sampleRates[1], 48000.0);
}

// A family that resolves nothing publishes nothing and forbids nothing, so the
// profile remains its only description -- the pre-resolution behaviour, still
// reachable and still correct for a uniform device.
TEST(NubGeometryRoundTrip, DeviceWithoutResolvedGeometryCarriesNoneAndForbidsNothing) {
    ASFWAudioDevice device{};
    device.guid = 0x1122334455667788ULL;
    device.inputChannelCount = 2;
    device.outputChannelCount = 2;
    device.sampleRates = {48000u};

    bool published = false;
    const auto parsed = RoundTrip(device, published);
    ASSERT_TRUE(published);

    EXPECT_EQ(parsed.playbackStreamCount, 0u);
    EXPECT_EQ(parsed.captureStreamCount, 0u);
    EXPECT_FALSE(parsed.resolvedGeometryRequired);
}

// MIDI rides in the slot count, so a stream carrying it must arrive with slots
// exceeding channels. Saffire Pro 24 DSP: 8 PCM + 1 MIDI playback, DBS 9.
TEST(NubGeometryRoundTrip, MidiBearingStreamKeepsItsSlotCount) {
    ASFWAudioDevice device{};
    device.guid = 0x00130E0402004713ULL;
    device.inputChannelCount = 16;
    device.outputChannelCount = 8;
    device.sampleRates = {48000u};
    device.playbackStreams = {
        {.pcmChannels = 8, .am824Slots = 9, .midiPorts = 1, .channelOffset = 0}};
    device.captureStreams = {
        {.pcmChannels = 16, .am824Slots = 17, .midiPorts = 1, .channelOffset = 0}};
    device.resolvedGeometryRequired = true;

    bool published = false;
    const auto parsed = RoundTrip(device, published);
    ASSERT_TRUE(published);

    ASSERT_EQ(parsed.playbackStreamCount, 1u);
    EXPECT_EQ(parsed.playbackStreams[0].pcmChannels, 8u);
    EXPECT_EQ(parsed.playbackStreams[0].midiPorts, 1u);
    EXPECT_EQ(parsed.playbackStreams[0].am824Slots, 9u);
    ASSERT_EQ(parsed.captureStreamCount, 1u);
    EXPECT_EQ(parsed.captureStreams[0].am824Slots, 17u);
}

// Four streams is the array bound on both sides of the crossing.
TEST(NubGeometryRoundTrip, CarriesTheMaximumStreamCount) {
    ASFWAudioDevice device{};
    device.guid = 0x1ULL;
    device.inputChannelCount = 8;
    device.outputChannelCount = 8;
    device.sampleRates = {48000u};
    uint32_t offset = 0;
    for (uint32_t i = 0; i < ASFW::Isoch::Audio::kMaxConfiguredStreams; ++i) {
        device.playbackStreams.push_back({.pcmChannels = 2,
                                          .am824Slots = 2,
                                          .midiPorts = 0,
                                          .channelOffset = offset});
        offset += 2;
    }

    bool published = false;
    const auto parsed = RoundTrip(device, published);
    ASSERT_TRUE(published);
    ASSERT_EQ(parsed.playbackStreamCount, ASFW::Isoch::Audio::kMaxConfiguredStreams);
    EXPECT_EQ(parsed.playbackStreams[3].channelOffset, 6u);
}

// More streams than either side can hold is rejected whole, not truncated:
// arming a subset of what the device transmits on is the silent-wrong-geometry
// class this path exists to end.
TEST(NubGeometryRoundTrip, RejectsMoreStreamsThanTheArrayBoundRatherThanTruncating) {
    ASFWAudioDevice device{};
    device.guid = 0x2ULL;
    device.inputChannelCount = 10;
    device.outputChannelCount = 10;
    device.sampleRates = {48000u};
    for (uint32_t i = 0; i < ASFW::Isoch::Audio::kMaxConfiguredStreams + 1; ++i) {
        device.playbackStreams.push_back(
            {.pcmChannels = 2, .am824Slots = 2, .midiPorts = 0, .channelOffset = i * 2});
    }

    bool published = false;
    const auto parsed = RoundTrip(device, published);
    ASSERT_TRUE(published);
    EXPECT_EQ(parsed.playbackStreamCount, 0u)
        << "a truncated array would describe a device that does not exist";
}


#include "Audio/Model/NubGeometryRefresh.hpp"

TEST(NubGeometryRefresh, UnchangedF24AndF32RetainTheirContract) {
    using namespace ASFW::Audio::Model;
    for (uint32_t secondWidth : {8U, 16U}) {
        ASFWAudioDevice config{};
        config.playbackStreams = {{16, 16, 0, 0}, {secondWidth, secondWidth, 0, 16}};
        config.captureStreams = config.playbackStreams;
        config.inputChannelCount = config.outputChannelCount = config.channelCount = 16 + secondWidth;
        config.resolvedGeometryRequired = true;
        NubGeometryRefreshState state(config);
        EXPECT_TRUE(state.Accept(config));
        EXPECT_FALSE(state.IsBlocked());
    }
}

TEST(NubGeometryRefresh, ChangedGeometryStaysBlockedUntilRecreation) {
    using namespace ASFW::Audio::Model;
    ASFWAudioDevice original{};
    original.playbackStreams = {{16, 16, 0, 0}, {8, 8, 0, 16}};
    auto changed = original;
    changed.playbackStreams[1] = {16, 16, 0, 16};
    NubGeometryRefreshState state(original);
    EXPECT_FALSE(state.Accept(changed));
    EXPECT_FALSE(state.Accept(original));
    NubGeometryRefreshState recreated(changed);
    EXPECT_TRUE(recreated.Accept(changed));
}

TEST(NubGeometryRefresh, CaptureVisibilityRatesAndOffsetsArePartOfContract) {
    using namespace ASFW::Audio::Model;
    ASFWAudioDevice original{};
    original.captureStreams = {{2, 2, 0, 0}};
    original.playbackStreams = {{2, 2, 0, 0}};
    original.sampleRates = {44100, 48000};
    original.inputChannelCount = 0;
    auto check = [&](const ASFWAudioDevice& changed) {
        EXPECT_EQ(ClassifyGeometryRefresh(original, changed),
                  GeometryRefreshDecision::kGeometryChanged);
    };
    auto changed = original;
    changed.inputChannelCount = 2; // Weiss must stay hidden.
    check(changed);
    changed = original;
    changed.playbackStreams[0].channelOffset = 1;
    check(changed);
    changed = original;
    changed.captureStreams[0].am824Slots = 3;
    check(changed);
    changed = original;
    changed.sampleRates = {48000};
    check(changed);
}
