// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcAudioConfigTests.cpp - The endpoint an AV/C unit publishes, from its graph
// or from its catalog profile.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/AVC/AvcAudioConfig.hpp"

namespace {

using ASFW::DeviceProfiles::Audio::ForcedStreamMode;
using ASFW::DeviceProfiles::Audio::StaticAudioEndpointPlan;
using ASFW::Protocols::AVC::BuildGraphAudioConfig;
using ASFW::Protocols::AVC::BuildProfileOwnedAudioConfig;
using ASFW::Protocols::AVC::Graph::DeviceGraph;
using ASFW::Protocols::AVC::Graph::StreamGraph;

StreamGraph Stream(uint32_t pcm, uint32_t dbs, std::vector<uint32_t> rates, uint32_t current) {
    StreamGraph stream;
    stream.channelCount = pcm;
    stream.dataBlockSize = dbs;
    stream.currentSampleRate = current;
    stream.supportedSampleRates = std::move(rates);
    return stream;
}

// The AV/C runtime runs the rate it was published at and nothing else, so the
// graph offers exactly the rate it starts at.
TEST(AvcAudioConfig, GraphOffersOnlyTheRateItStartsAt) {
    DeviceGraph graph;
    graph.playback = Stream(2, 3, {44100, 48000, 96000}, 44100);
    graph.capture = Stream(2, 3, {44100, 48000, 96000}, 44100);
    StaticAudioEndpointPlan plan{};

    // Found at 44.1, but every device starts at 48 kHz when it can.
    const auto open = BuildGraphAudioConfig({.guid = 1, .modelName = "Device"}, plan, graph);
    ASSERT_TRUE(open);
    EXPECT_EQ(open->sampleRates, std::vector<uint32_t>{48000});
    EXPECT_EQ(open->currentSampleRate, 48000U);
    EXPECT_EQ(open->inputPlugName, "Device Inputs");

    // Without 48 kHz the device keeps the rate it reported.
    DeviceGraph no48;
    no48.playback = Stream(2, 3, {44100, 88200}, 88200);
    no48.capture = Stream(2, 3, {44100, 88200}, 88200);
    const auto kept = BuildGraphAudioConfig({.guid = 1}, plan, no48);
    ASSERT_TRUE(kept);
    EXPECT_EQ(kept->sampleRates, std::vector<uint32_t>{88200});
    EXPECT_EQ(kept->currentSampleRate, 88200U);

    // No rate both directions can run: nothing to publish.
    DeviceGraph disjoint;
    disjoint.playback = Stream(2, 3, {44100}, 44100);
    disjoint.capture = Stream(2, 3, {48000}, 44100);
    EXPECT_FALSE(BuildGraphAudioConfig({.guid = 1}, plan, disjoint));

    plan.streamTraits.start.startRatePinHz = 96000;
    const auto pinned = BuildGraphAudioConfig({.guid = 1}, plan, graph);
    ASSERT_TRUE(pinned);
    EXPECT_EQ(pinned->sampleRates, std::vector<uint32_t>{96000});
    EXPECT_EQ(pinned->currentSampleRate, 96000U);
}

TEST(AvcAudioConfig, GraphWithMismatchedOrMissingGeometryIsNotPublished) {
    DeviceGraph graph;
    graph.playback = Stream(2, 3, {48000}, 48000);
    graph.capture = Stream(2, 3, {44100}, 44100);
    EXPECT_FALSE(BuildGraphAudioConfig({}, StaticAudioEndpointPlan{}, graph));
    graph.capture = Stream(2, 0, {48000}, 48000);
    EXPECT_FALSE(BuildGraphAudioConfig({}, StaticAudioEndpointPlan{}, graph));
}

class FixedProfile final : public ASFW::Isoch::Audio::IAudioDeviceProfile {
public:
    std::vector<uint32_t> rates{48000};
    std::vector<uint32_t> SupportedSampleRates() const override { return rates; }
    const char* Name() const noexcept override { return "M-Audio ProjectMix I/O"; }
    ASFW::Encoding::AudioWireFormat TxWireFormat() const noexcept override {
        return ASFW::Encoding::AudioWireFormat::kAM824;
    }
    ASFW::Encoding::AudioWireFormat RxWireFormat() const noexcept override {
        return ASFW::Encoding::AudioWireFormat::kAM824;
    }
    uint32_t TxChannelCount() const noexcept override { return 6; }
    uint32_t RxChannelCount() const noexcept override { return 10; }
    uint32_t TxMidiSlots() const noexcept override { return 1; }
    uint32_t RxMidiSlots() const noexcept override { return 1; }
    uint32_t TxMidiPorts() const noexcept override { return 2; }
    uint32_t RxMidiPorts() const noexcept override { return 2; }
    uint32_t TxDbs() const noexcept override { return 7; }
    uint32_t RxDbs() const noexcept override { return 11; }
    uint32_t TxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t RxSafetyOffsetFrames(double) const noexcept override { return 0; }
    uint32_t TxReportedLatencyFrames(double) const noexcept override { return 0; }
    uint32_t RxReportedLatencyFrames(double) const noexcept override { return 0; }
};

TEST(AvcAudioConfig, ProfileOwnedEndpointCarriesTheProfilesGeometry) {
    StaticAudioEndpointPlan plan{};
    plan.streamTraits.wire.forcedStreamMode = ForcedStreamMode::Blocking;
    plan.streamTraits.start.startRatePinHz = 48000;
    const auto config = BuildProfileOwnedAudioConfig({.guid = 7, .vendorId = 0x000D6C}, plan, FixedProfile{});
    ASSERT_TRUE(config);
    EXPECT_EQ(config->deviceName, "M-Audio ProjectMix I/O");
    EXPECT_EQ(config->inputChannelCount, 10U);
    EXPECT_EQ(config->outputChannelCount, 6U);
    EXPECT_EQ(config->sampleRates, std::vector<uint32_t>{48000});
    ASSERT_EQ(config->captureStreams.size(), 1U);
    EXPECT_EQ(config->captureStreams[0].am824Slots, 11U);
    // Two MIDI ports ride the one MIDI slot.
    EXPECT_EQ(config->captureStreams[0].midiPorts, 2U);
    EXPECT_EQ(config->playbackStreams[0].am824Slots, 7U);
    EXPECT_TRUE(config->resolvedGeometryRequired);
    EXPECT_FALSE(config->graphResolved);
    EXPECT_EQ(config->streamMode, ASFW::Audio::Model::StreamMode::kBlocking);
}

TEST(AvcAudioConfig, ProfileOwnedEndpointStartsAt48kWhenOffered) {
    FixedProfile profile;
    profile.rates = {44100, 48000};
    const auto config = BuildProfileOwnedAudioConfig({.guid = 7}, StaticAudioEndpointPlan{}, profile);
    ASSERT_TRUE(config);
    EXPECT_EQ(config->sampleRates, (std::vector<uint32_t>{44100, 48000}));
    EXPECT_EQ(config->currentSampleRate, 48000U);

    profile.rates = {44100, 88200};
    const auto without48 = BuildProfileOwnedAudioConfig({.guid = 7}, StaticAudioEndpointPlan{}, profile);
    ASSERT_TRUE(without48);
    EXPECT_EQ(without48->currentSampleRate, 44100U);
}

} // namespace
