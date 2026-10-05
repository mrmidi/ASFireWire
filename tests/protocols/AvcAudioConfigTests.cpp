// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcAudioConfigTests.cpp - The endpoint an AV/C unit publishes, from its graph
// or from its catalog profile.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/AVC/AvcAudioConfig.hpp"
#include "ASFWDriver/Audio/Protocols/AVC/AvcControlMapping.hpp"

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

TEST(AvcAudioConfig, AvcDevicesPublishOnlyStartupRateUntilTransactionalMultiRate) {
    DeviceGraph graph;
    graph.playback = Stream(2, 2, {32000, 44100, 48000, 96000}, 96000);
    graph.capture = Stream(2, 2, {44100, 48000, 96000}, 96000);
    for (const auto implementation : {
             ASFW::DeviceProfiles::Audio::ProtocolImplementationId::ApogeeDuet,
             ASFW::DeviceProfiles::Audio::ProtocolImplementationId::BeBoBPhase88}) {
        StaticAudioEndpointPlan plan;
        plan.protocolImplementation = implementation;
        plan.streamTraits.start.startAtObservedRate = true;
        const auto config = BuildGraphAudioConfig({}, plan, graph);
        ASSERT_TRUE(config);
        EXPECT_EQ(config->sampleRates, (std::vector<uint32_t>{48000}));
        EXPECT_EQ(config->currentSampleRate, 48000U);
    }
}

TEST(AvcAudioConfig, DifferentInitialPlugRatesPublishACommonStartupRate) {
    DeviceGraph graph;
    graph.playback = Stream(10, 11, {32000, 44100, 48000, 88200, 96000}, 32000);
    graph.capture = Stream(10, 11, {32000, 44100, 48000, 88200, 96000}, 48000);
    for (const auto implementation : {
             ASFW::DeviceProfiles::Audio::ProtocolImplementationId::BeBoBPhase88,
             ASFW::DeviceProfiles::Audio::ProtocolImplementationId::ApogeeDuet}) {
        StaticAudioEndpointPlan plan{};
        plan.protocolImplementation = implementation;
        auto config = BuildGraphAudioConfig({}, plan, graph);
        ASSERT_TRUE(config);
        EXPECT_EQ(config->currentSampleRate, 48000U);
        EXPECT_EQ(config->inputChannelCount, 10U);
        EXPECT_EQ(config->outputChannelCount, 10U);
        EXPECT_EQ(config->sampleRates, (std::vector<uint32_t>{48000}));
    }
    // The generic startup also sets both plug formats, with one published rate.
    auto generic = BuildGraphAudioConfig({}, StaticAudioEndpointPlan{}, graph);
    ASSERT_TRUE(generic);
    EXPECT_EQ(generic->sampleRates, (std::vector<uint32_t>{48000}));
    graph.capture.currentSampleRate = 0;
    EXPECT_FALSE(BuildGraphAudioConfig({}, StaticAudioEndpointPlan{}, graph));
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


TEST(AvcAudioConfig, PublishesOnlyConfirmedControlsWithDeviceReportedRanges) {
    DeviceGraph graph;
    graph.playback = Stream(2, 2, {44100, 48000}, 48000);
    graph.capture = graph.playback;
    ASFW::Protocols::AVC::Graph::ControlBlockInfo block;
    block.id = 1; block.channelCount = 2; block.inputSources = {{0xf0, 0}}; block.name = "Output";
    graph.controls.push_back(block);
    graph.playbackAudioChannels = {{0, 0, 0, 0}, {1, 0, 0, 1}};
    graph.featureChannels.push_back({.subunit = 0, .block = 1, .channel = 0, .mute = false,
        .volume = 0, .minimum = -16384, .maximum = 0, .resolution = 1});
    graph.featureChannels.push_back({.subunit = 0, .block = 1, .channel = 1, .mute = true, .volume = 0});
    StaticAudioEndpointPlan plan;
    plan.protocolImplementation = ASFW::DeviceProfiles::Audio::ProtocolImplementationId::ApogeeDuet;
    auto config = BuildGraphAudioConfig({.guid = 42}, plan, graph);
    ASSERT_TRUE(config); ASSERT_EQ(config->avcControls.size(), 1);
    EXPECT_EQ(config->sampleRates, (std::vector<uint32_t>{48000}));
    EXPECT_EQ(config->avcControls[0].scope, static_cast<uint32_t>('outp'));
    EXPECT_EQ(config->avcControls[0].element, 0);
    EXPECT_TRUE(config->avcControls[0].hasVolume); EXPECT_TRUE(config->avcControls[0].hasMute);
    EXPECT_STREQ(config->avcControls[0].name, "Output Master 0");
    graph.featureChannels[0].resolution = 0;
    config = BuildGraphAudioConfig({.guid = 42}, plan, graph);
    ASSERT_TRUE(config); EXPECT_FALSE(config->avcControls[0].hasVolume);
}
TEST(AvcAudioConfig, MixerElementsAreDistinctAndDoNotBecomeSystemVolume) {
    DeviceGraph graph;
    graph.playback = Stream(10, 10, {48000}, 48000); graph.capture = graph.playback;
    for (uint8_t block = 1; block <= 2; ++block) {
        ASFW::Protocols::AVC::Graph::ControlBlockInfo info; info.id = block; graph.controls.push_back(info);
        graph.featureChannels.push_back({.subunit = 0, .block = block, .channel = 0, .mute = true,
            .volume = -256, .minimum = -16384, .maximum = 0, .resolution = 256});
    }
    const auto config = BuildGraphAudioConfig({}, StaticAudioEndpointPlan{}, graph);
    ASSERT_TRUE(config); EXPECT_TRUE(config->avcControls.empty());
}
namespace G = ASFW::Protocols::AVC::Graph;
namespace D = ASFW::Protocols::AVC::Descriptors;
using ASFW::Protocols::AVC::PlaceAvcControl;
G::ControlBlockInfo Feature(uint8_t id, uint8_t channels, D::AudioSourceId source, uint8_t subunit = 0) {
    G::ControlBlockInfo result;
    result.id = id; result.channelCount = channels; result.inputSources = {source}; result.audioSubunitId = subunit;
    return result;
}
TEST(AvcControlMapping, OutputMappingNeedsNoDeviceIdentityAndPreservesChannelOrder) {
    DeviceGraph graph; graph.playback.channelCount = 4;
    graph.controls = {Feature(9, 2, {0xf0, 3})};
    graph.playbackAudioChannels = {{2, 0, 3, 1}, {3, 0, 3, 0}};
    auto placement = PlaceAvcControl(graph, graph.controls[0], 1);
    EXPECT_EQ(placement.scope, static_cast<uint32_t>('outp')); EXPECT_EQ(placement.element, 4);
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 2).element, 3);
    // A two-channel master is not a master for all four output channels.
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 0).scope, static_cast<uint32_t>('ptru'));
}
TEST(AvcControlMapping, FullStreamMasterMapsToMainElement) {
    DeviceGraph graph; graph.playback.channelCount = 2;
    graph.controls = {Feature(9, 2, {0xf0, 3})};
    graph.playbackAudioChannels = {{0, 0, 3, 1}, {1, 0, 3, 0}};
    const auto placement = PlaceAvcControl(graph, graph.controls[0], 0);
    EXPECT_EQ(placement.scope, static_cast<uint32_t>('outp')); EXPECT_EQ(placement.element, 0);
}
TEST(AvcControlMapping, CaptureMappingFollowsSourcePlugFeatureChain) {
    DeviceGraph graph; graph.capture.channelCount = 4;
    graph.controls = {Feature(4, 2, {0xf0, 8}), Feature(7, 2, {0x81, 4})};
    graph.audioSourcePlugs = {{0, 6, {0x81, 7}}};
    graph.captureAudioChannels = {{2, 0, 6, 0}, {3, 0, 6, 1}};
    const auto placement = PlaceAvcControl(graph, graph.controls[0], 2);
    EXPECT_EQ(placement.scope, static_cast<uint32_t>('inpt')); EXPECT_EQ(placement.element, 4);
}
TEST(AvcControlMapping, MonitorInputBranchDoesNotBecomeRecordedInputGain) {
    DeviceGraph graph; graph.capture.channelCount = 2;
    graph.controls = {Feature(4, 2, {0xf0, 8})};
    graph.audioSourcePlugs = {{0, 6, {0xf0, 8}}};
    graph.captureAudioChannels = {{0, 0, 6, 0}, {1, 0, 6, 1}};
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 1).scope, static_cast<uint32_t>('ptru'));
}
TEST(AvcControlMapping, CyclesMixersAndSelectableRoutesStayInternal) {
    DeviceGraph graph; graph.playback.channelCount = 2;
    graph.playbackAudioChannels = {{0, 0, 3, 0}, {1, 0, 3, 1}};
    graph.controls = {Feature(1, 2, {0x81, 2}), Feature(2, 2, {0x81, 1})};
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 1).scope, static_cast<uint32_t>('ptru'));
    graph.controls[1].type = D::AudioFunctionBlockType::kProcessing;
    graph.controls[0].inputSources = {{0x82, 2}}; graph.controls[1].inputSources = {{0xf0, 3}};
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 1).scope, static_cast<uint32_t>('ptru'));
    graph.controls[1].type = D::AudioFunctionBlockType::kSelector;
    graph.controls[0].inputSources = {{0x80, 2}}; graph.controls[1].inputSources = {{0xf0, 3}, {0xf0, 4}};
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 1).scope, static_cast<uint32_t>('ptru'));
}
TEST(AvcControlMapping, MissingDuplicateAndAmbiguousChannelEvidenceStayInternal) {
    DeviceGraph graph; graph.playback.channelCount = 2;
    graph.controls = {Feature(1, 2, {0xf0, 3})};
    graph.playbackAudioChannels = {{0, 0, 3, 0}};
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 1).scope, static_cast<uint32_t>('ptru'));
    graph.playbackAudioChannels.push_back({1, 0, 3, 0});
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 1).scope, static_cast<uint32_t>('ptru'));
    graph.playbackAudioChannels[1].position = 1; graph.playback.routeAmbiguous = true;
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 1).scope, static_cast<uint32_t>('ptru'));
}
TEST(AvcControlMapping, SharedDirectionsAndSubunitIdentitiesAreNotConflated) {
    DeviceGraph graph; graph.playback.channelCount = graph.capture.channelCount = 2;
    graph.controls = {Feature(1, 2, {0xf0, 3}, 1)};
    graph.playbackAudioChannels = {{0, 0, 3, 0}, {1, 0, 3, 1}};
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 1).scope, static_cast<uint32_t>('ptru'));
    graph.playbackAudioChannels = {{0, 1, 3, 0}, {1, 1, 3, 1}};
    graph.audioSourcePlugs = {{1, 6, {0x81, 1}}};
    graph.captureAudioChannels = {{0, 1, 6, 0}, {1, 1, 6, 1}};
    EXPECT_EQ(PlaceAvcControl(graph, graph.controls[0], 1).scope, static_cast<uint32_t>('ptru'));
}
TEST(AvcControlMapping, CompetingOutputMastersAreNotPublished) {
    DeviceGraph graph; graph.playback = graph.capture = Stream(2, 2, {48000}, 48000);
    graph.controls = {Feature(1, 2, {0xf0, 3}), Feature(2, 2, {0x81, 1})};
    graph.playbackAudioChannels = {{0, 0, 3, 0}, {1, 0, 3, 1}};
    for (uint8_t block : {1, 2}) graph.featureChannels.push_back({.subunit=0, .block=block, .channel=0, .mute=false});
    const auto config = BuildGraphAudioConfig({}, {}, graph);
    ASSERT_TRUE(config); EXPECT_TRUE(config->avcControls.empty());
}
TEST(AvcAudioConfig, GenericOutputSelectorFrontierPublishesOnlyMixerMaster) {
    DeviceGraph graph; graph.playback = graph.capture = Stream(10, 10, {48000}, 48000);
    graph.controls = {Feature(1, 8, {0x82, 1}), Feature(2, 2, {0xf0, 2})};
    G::ControlBlockInfo selector; selector.type=D::AudioFunctionBlockType::kSelector; selector.id=6;
    selector.inputSources={{0x81,1}, {0xf0,0}};
    graph.controls.push_back(selector);
    graph.audioSourcePlugs={{0,0,{0xf0,2}}, {0,6,{0x80,6}}};
    for (uint32_t i=0; i<10; ++i) graph.captureAudioChannels.push_back({i,0,0,static_cast<uint8_t>(i)});
    for (uint8_t block : {1, 2}) for (uint8_t channel : {0, 1})
        graph.featureChannels.push_back({.subunit=0, .block=block, .channel=channel, .mute=false,
            .volume=-256, .minimum=-25600, .maximum=0, .resolution=256});
    StaticAudioEndpointPlan plan;
    auto config = BuildGraphAudioConfig({}, plan, graph);
    ASSERT_TRUE(config); ASSERT_EQ(config->avcControls.size(), 1U);
    EXPECT_EQ(config->avcControls[0].token, 0x100U);
    EXPECT_EQ(config->avcControls[0].scope, static_cast<uint32_t>('outp'));
    EXPECT_EQ(config->avcControls[0].element, 0U);
    EXPECT_TRUE(config->avcControls[0].hasVolume); EXPECT_TRUE(config->avcControls[0].hasMute);
    graph.featureChannels[0].resolution.reset();
    config = BuildGraphAudioConfig({}, plan, graph);
    ASSERT_TRUE(config); ASSERT_EQ(config->avcControls.size(), 1U);
    EXPECT_FALSE(config->avcControls[0].hasVolume); EXPECT_TRUE(config->avcControls[0].hasMute);
    graph.featureChannels[0].resolution=256; graph.featureChannels[0].mute.reset();
    config = BuildGraphAudioConfig({}, plan, graph);
    ASSERT_TRUE(config); ASSERT_EQ(config->avcControls.size(), 1U);
    EXPECT_TRUE(config->avcControls[0].hasVolume); EXPECT_FALSE(config->avcControls[0].hasMute);
}
TEST(AvcControlMapping, OutputFrontierNeedsCompleteCaptureIdentityAndRejectsSharedGain) {
    DeviceGraph graph; graph.playback=graph.capture=Stream(2,2,{48000},48000);
    graph.controls={Feature(9,2,{0x82,1}), Feature(10,2,{0xf0,2})};
    G::ControlBlockInfo selector; selector.type=D::AudioFunctionBlockType::kSelector; selector.id=4;
    selector.inputSources={{0x81,9},{0x80,4}}; graph.controls.push_back(selector);
    graph.audioSourcePlugs={{0,3,{0x80,4}}, {0,1,{0xf0,2}}};
    EXPECT_FALSE(ASFW::Protocols::AVC::IsAvcOutputMaster(graph, graph.controls[0]));
    graph.captureAudioChannels={{0,0,1,0},{1,0,1,1}};
    EXPECT_TRUE(ASFW::Protocols::AVC::IsAvcOutputMaster(graph, graph.controls[0]));
    graph.controls[0].volumePurpose=2;
    EXPECT_FALSE(ASFW::Protocols::AVC::IsAvcOutputMaster(graph,graph.controls[0]));
    graph.controls[0].volumePurpose=3;
    EXPECT_FALSE(ASFW::Protocols::AVC::IsAvcOutputMaster(graph,graph.controls[0]));
    graph.controls[0].volumePurpose=0;
    EXPECT_FALSE(ASFW::Protocols::AVC::IsAvcOutputMaster(graph, graph.controls[1]));
    graph.captureAudioChannels={{0,0,3,0},{1,0,3,1}};
    EXPECT_FALSE(ASFW::Protocols::AVC::IsAvcOutputMaster(graph, graph.controls[0]));
}
TEST(AvcAudioConfig, DescriptorMasterPurposeResolvesCompetingOutputStages) {
    DeviceGraph graph; graph.playback=graph.capture=Stream(2,2,{48000},48000);
    graph.controls={Feature(5,2,{0xf0,0}),Feature(7,2,{0x81,5})};
    graph.controls[1].volumePurpose=1;
    graph.playbackAudioChannels={{0,0,0,0},{1,0,0,1}};
    for (uint8_t block : {5,7}) graph.featureChannels.push_back({.subunit=0,.block=block,.channel=0,.mute=false,
        .volume=0,.minimum=-16384,.maximum=0,.resolution=1});
    const auto config=BuildGraphAudioConfig({}, {}, graph);
    ASSERT_TRUE(config); ASSERT_EQ(config->avcControls.size(),1U);
    EXPECT_EQ(config->avcControls[0].token,0x700U);
}
TEST(AvcAudioConfig, PrefersBlockingFromBothDirectionsAndHonorsValidatedOverrides) {
    DeviceGraph graph; graph.playback = Stream(2, 2, {48000}, 48000); graph.capture = graph.playback;
    StaticAudioEndpointPlan plan;
    graph.transmitModes = 3; graph.receiveModes = 3;
    auto config = BuildGraphAudioConfig({}, plan, graph);
    ASSERT_TRUE(config); EXPECT_EQ(config->streamMode, ASFW::Audio::Model::StreamMode::kBlocking);
    graph.receiveModes = 1;
    config = BuildGraphAudioConfig({}, plan, graph);
    ASSERT_TRUE(config); EXPECT_EQ(config->streamMode, ASFW::Audio::Model::StreamMode::kNonBlocking);
    plan.streamTraits.wire.forcedStreamMode = ForcedStreamMode::Blocking;
    config = BuildGraphAudioConfig({}, plan, graph);
    ASSERT_TRUE(config); EXPECT_EQ(config->streamMode, ASFW::Audio::Model::StreamMode::kBlocking);
    plan.streamTraits.wire.forcedStreamMode = ForcedStreamMode::Unspecified;
    graph.transmitModes = 2;
    EXPECT_FALSE(BuildGraphAudioConfig({}, plan, graph));
}

} // namespace
