// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AV/C discovery graph -> semantic topology (FW-204): edge cases on a
// hand-built graph. Real discovery graphs are covered in AvcGoldenTests.

#include "Audio/Controls/AvcControlProjection.hpp"
#include "Audio/Model/Topology/Validate.hpp"

#include <gtest/gtest.h>

#include <algorithm>

namespace {
using namespace ASFW::Audio::Controls;
namespace AM = ASFW::AudioModel;
namespace G = ASFW::Protocols::AVC::Graph;
using BlockType = ASFW::Protocols::AVC::Descriptors::AudioFunctionBlockType;
using Source = ASFW::Protocols::AVC::Descriptors::AudioSourceId;

constexpr uint8_t kSubunit = 0;

G::DeviceGraph Graph() {
    G::DeviceGraph g;
    g.controls.push_back(G::ControlBlockInfo{.type = BlockType::kFeature, .id = 1, .name = "Out Master",
                                             .channelCount = 2, .audioSubunitId = kSubunit});
    g.controls.push_back(G::ControlBlockInfo{.type = BlockType::kSelector, .id = 2, .name = "",
                                             .channelCount = 2, .audioSubunitId = kSubunit});
    g.controls.push_back(G::ControlBlockInfo{.type = BlockType::kProcessing, .id = 3, .audioSubunitId = kSubunit});
    // Master: volume -6 dB in [-80, 0] step 1 dB, unmuted.
    g.featureChannels.push_back(G::FeatureChannelState{.subunit = kSubunit, .block = 1, .channel = 0,
        .mute = false, .volume = int16_t(-6 * 256), .minimum = int16_t(-80 * 256), .maximum = 0, .resolution = 256});
    // Channel 1: volume outside its own range -> no level published; mute muted.
    g.featureChannels.push_back(G::FeatureChannelState{.subunit = kSubunit, .block = 1, .channel = 1,
        .mute = true, .volume = int16_t(6 * 256), .minimum = int16_t(-80 * 256), .maximum = 0, .resolution = 256});
    // Channel 2: volume is -inf (0x8000): no level.
    g.featureChannels.push_back(G::FeatureChannelState{.subunit = kSubunit, .block = 1, .channel = 2,
        .volume = INT16_MIN, .minimum = int16_t(-80 * 256), .maximum = 0, .resolution = 256});
    // Channel 5 exceeds the block's width: ignored.
    g.featureChannels.push_back(G::FeatureChannelState{.subunit = kSubunit, .block = 1, .channel = 5, .mute = true});
    g.selectors.push_back(G::AudioSelectorInfo{.audioSubunitId = kSubunit, .functionBlockId = 2, .name = "Monitor",
        .declaredInputs = {Source{0xF0, 0}, Source{0x81, 1}, Source{0xF0, 1}}, .currentInput = uint8_t{2}});
    return g;
}

const AM::Parameter* FindParam(const AvcControlModel& m, AM::ParameterId id) {
    for (const auto& p : m.topology.parameters) if (p.id == id) return &p;
    return nullptr;
}

TEST(AvcControlProjection, ProjectionValidatesStructurallyAndItsStateIsLegal) {
    const auto model = ProjectAvcControls(Graph(), 42);
    const auto structural = AM::validate(model.topology);
    ASSERT_TRUE(structural.has_value()) << structural.error().message;
    const auto state = AM::validateState(model.topology, model.state);
    EXPECT_TRUE(state.has_value()) << (state ? "" : state.error().message);
    EXPECT_EQ(model.topology.revision, 42U);
    EXPECT_EQ(model.state.topologyRevision, 42U);
}

TEST(AvcControlProjection, MasterVolumeAndMuteTargetTheBlockWithTheGraphRange) {
    const auto model = ProjectAvcControls(Graph(), 1);
    const auto volumeId = AvcParameterId(kSubunit, 1, 0, AvcBindingKind::kFeatureVolume);
    const auto* volume = FindParam(model, volumeId);
    ASSERT_NE(volume, nullptr);
    EXPECT_EQ(volume->semantic, AM::ParameterSemantic::Level);
    ASSERT_TRUE(std::holds_alternative<AM::NodeId>(volume->target));
    const auto& domain = std::get<AM::ScalarDomain>(volume->domain);
    EXPECT_DOUBLE_EQ(domain.min, -80.0);
    EXPECT_DOUBLE_EQ(domain.max, 0.0);
    EXPECT_DOUBLE_EQ(*domain.step, 1.0);
    EXPECT_EQ(domain.unit, AM::ScalarUnit::Decibels);
    const auto& state = model.state.parameters.at(volumeId);
    EXPECT_DOUBLE_EQ(std::get<double>(state.value), -6.0);
    EXPECT_EQ(state.origin, AM::StateOrigin::Observed);

    const auto* binding = model.FindBinding(volumeId);
    ASSERT_NE(binding, nullptr);
    EXPECT_EQ(binding->Token(), ASFW::Audio::Model::AvcControlToken(kSubunit, 1, 0));
    EXPECT_EQ(binding->range.resolution, 256);
}

TEST(AvcControlProjection, OutOfRangeAndSilentLevelsAreNotPublishedButMuteIs) {
    const auto model = ProjectAvcControls(Graph(), 1);
    EXPECT_EQ(FindParam(model, AvcParameterId(kSubunit, 1, 1, AvcBindingKind::kFeatureVolume)), nullptr);
    EXPECT_EQ(FindParam(model, AvcParameterId(kSubunit, 1, 2, AvcBindingKind::kFeatureVolume)), nullptr);
    const auto muteId = AvcParameterId(kSubunit, 1, 1, AvcBindingKind::kFeatureMute);
    const auto* mute = FindParam(model, muteId);
    ASSERT_NE(mute, nullptr);
    ASSERT_TRUE(std::holds_alternative<AM::PortId>(mute->target)); // channel parameter -> its output port
    EXPECT_EQ(std::get<bool>(model.state.parameters.at(muteId).value), true);
}

TEST(AvcControlProjection, ChannelBeyondTheBlockWidthIsIgnored) {
    const auto model = ProjectAvcControls(Graph(), 1);
    EXPECT_EQ(FindParam(model, AvcParameterId(kSubunit, 1, 5, AvcBindingKind::kFeatureMute)), nullptr);
}

TEST(AvcControlProjection, SelectorBecomesARouterWithTheConfirmedInputActive) {
    const auto model = ProjectAvcControls(Graph(), 1);
    const auto router = std::ranges::find_if(model.topology.nodes, [](const auto& n) {
        return std::holds_alternative<AM::RouterNode>(n.body);
    });
    ASSERT_NE(router, model.topology.nodes.end());
    EXPECT_EQ(router->name, "Selector 2"); // unnamed block gets a stable fallback
    const auto& body = std::get<AM::RouterNode>(router->body);
    EXPECT_EQ(body.inputs.size(), 6U);
    EXPECT_EQ(body.outputs.size(), 2U);
    EXPECT_EQ(body.legalBundles.front().routes.size(), 2U);
    EXPECT_EQ(body.legalBundles.size(), 3U);
    const auto& active = model.state.routers.at(router->id).activeBundles;
    ASSERT_EQ(active.size(), 1U);
    EXPECT_EQ(active.front(), AM::RouteBundleId{3}); // input plug 2, 0-based
}

TEST(AvcControlProjection, UnconfirmedSelectorHasNoActiveBundle) {
    auto graph = Graph();
    graph.selectors.front().currentInput.reset();
    const auto model = ProjectAvcControls(graph, 1);
    for (const auto& [id, state] : model.state.routers) EXPECT_TRUE(state.activeBundles.empty());
}

TEST(AvcControlProjection, ProcessingBlocksAreNotInventedIntoNodes) {
    const auto model = ProjectAvcControls(Graph(), 1);
    EXPECT_EQ(model.topology.nodes.size(), 2U); // feature + selector
}

TEST(AvcControlProjection, IdsAreStableAcrossProjectionsOfTheSameGraph) {
    const auto a = ProjectAvcControls(Graph(), 1);
    const auto b = ProjectAvcControls(Graph(), 2);
    ASSERT_EQ(a.topology.parameters.size(), b.topology.parameters.size());
    for (size_t i = 0; i < a.topology.parameters.size(); ++i)
        EXPECT_EQ(a.topology.parameters[i].id, b.topology.parameters[i].id);
}


TEST(AvcControlProjection, DescriptorPathsConnectHostChannelsWithoutGuessing) {
    auto graph = Graph();
    graph.controls.front().inputSources = {Source{0xF0, 0}};
    graph.playback.channelCount = graph.capture.channelCount = 2;
    graph.playbackAudioChannels = {{0, 0, 0, 0}, {1, 0, 0, 1}};
    graph.captureAudioChannels = {{0, 0, 1, 0}, {1, 0, 1, 1}};
    graph.audioSourcePlugs = {{0, 1, Source{static_cast<uint8_t>(BlockType::kFeature), 1}}};
    auto model = ProjectAvcControls(graph, 7);
    ASSERT_TRUE(AM::validate(model.topology)) << AM::validate(model.topology).error().message;
    EXPECT_EQ(model.topology.fixedLinks.size(), 8U); // both selector sources, feature input, capture
    EXPECT_EQ(std::ranges::count_if(model.topology.nodes, [](const auto& n) {
        return std::holds_alternative<AM::EndpointNode>(n.body);
    }), 2);
    graph.playbackAudioChannels[1].position = 0; // malformed evidence cannot become an endpoint
    model = ProjectAvcControls(graph, 7);
    EXPECT_EQ(std::ranges::count_if(model.topology.ports, [](const auto& p) {
        return p.signal.kind == AM::SignalKind::HostStream && p.direction == AM::PortDirection::Output;
    }), 0);
}

TEST(AvcControlProjection, ConfirmedWriteShowsUpInTheNextProjection) {
    auto graph = Graph();
    const auto id = AvcParameterId(kSubunit, 1, 0, AvcBindingKind::kFeatureVolume);
    graph.featureChannels.front().volume = int16_t(-20 * 256); // as RememberConfirmedFeature does
    const auto model = ProjectAvcControls(graph, 1);
    EXPECT_DOUBLE_EQ(std::get<double>(model.state.parameters.at(id).value), -20.0);
}

} // namespace
