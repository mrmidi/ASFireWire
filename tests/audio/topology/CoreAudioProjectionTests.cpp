// SPDX-License-Identifier: Apache-2.0
#include "Audio/Controls/CoreAudioProjection.hpp"
#include "Audio/Model/Topology/Validate.hpp"
#include <gtest/gtest.h>
#include <limits>
#include <algorithm>
using namespace ASFW::Audio::Controls;
namespace AM = ASFW::AudioModel;
namespace {
struct Fixture {
    AM::Topology topology;
    AM::DeviceState state;
    Fixture() {
        topology.revision = state.topologyRevision = 42;
        topology.nodes = {
            {AM::NodeId{1}, "Input", AM::EndpointNode{AM::EndpointKind::Physical}},
            {AM::NodeId{2}, "Gain", AM::ProcessorNode{{AM::PortId{2}}, {AM::PortId{3}}, true}},
            {AM::NodeId{3}, "Capture", AM::EndpointNode{AM::EndpointKind::Host}}};
        topology.ports = {
            {AM::PortId{1}, AM::NodeId{1}, AM::PortDirection::Output, 1, "Mic", {AM::SignalKind::AnalogMicXlr, 1}},
            {AM::PortId{2}, AM::NodeId{2}, AM::PortDirection::Input, 1, "In"},
            {AM::PortId{3}, AM::NodeId{2}, AM::PortDirection::Output, 1, "Out"},
            {AM::PortId{4}, AM::NodeId{3}, AM::PortDirection::Input, 1, "Capture 2", {AM::SignalKind::HostStream, 2}}};
        topology.fixedLinks = {{AM::PortId{1}, AM::PortId{2}}, {AM::PortId{3}, AM::PortId{4}}};
        topology.parameters = {{AM::ParameterId{10}, AM::PortId{3}, AM::ParameterSemantic::Level,
                               AM::ScalarDomain{10, 75, 1, AM::ScalarUnit::Decibels}, "Gain"}};
        state.parameters.emplace(AM::ParameterId{10}, AM::ParameterState{20.0});
    }
};
TEST(CoreAudioProjection, UsesHostChannelIdentityThroughSignalPath) {
    Fixture f;
    ASSERT_TRUE(AM::validate(f.topology));
    auto controls = ProjectCoreAudioControls(f.topology, f.state);
    ASSERT_EQ(controls.size(), 1);
    EXPECT_EQ(controls[0].parameter.value, 10);
    EXPECT_EQ(controls[0].scope, uint32_t{'inpt'});
    EXPECT_EQ(controls[0].element, 2);
    EXPECT_EQ(controls[0].classId, uint32_t{'vlme'});
}
TEST(CoreAudioProjection, UsesCurrentRangeAndSettableState) {
    Fixture f;
    auto& s = f.state.parameters.at(AM::ParameterId{10});
    s.domainOverride = AM::ScalarDomain{0, 65, 1, AM::ScalarUnit::Decibels};
    s.applicable = false;
    auto controls = ProjectCoreAudioControls(f.topology, f.state);
    ASSERT_EQ(controls.size(), 1);
    EXPECT_TRUE(controls[0].settable); // applicability is checked on each write, not frozen at publication
    EXPECT_TRUE(controls[0].normalized);
    EXPECT_DOUBLE_EQ(std::get<AM::ScalarDomain>(controls[0].domain).min, 0);
}
TEST(CoreAudioProjection, DoesNotGuessAcrossMixerOrUnknownProcessor) {
    Fixture f;
    std::get<AM::ProcessorNode>(f.topology.nodes[1].body).channelPreserving = false;
    f.topology.parameters[0].target = AM::PortId{2};
    EXPECT_TRUE(ProjectCoreAudioControls(f.topology, f.state).empty());
}
TEST(CoreAudioProjection, AmbiguousHostFanoutAndDuplicateClassesAreOmitted) {
    Fixture f;
    f.topology.ports.push_back({AM::PortId{5}, AM::NodeId{3}, AM::PortDirection::Input, 1, "Capture 3",
                               {AM::SignalKind::HostStream, 3}});
    f.topology.fixedLinks.push_back({AM::PortId{3}, AM::PortId{5}});
    EXPECT_TRUE(ProjectCoreAudioControls(f.topology, f.state).empty());
    f.topology.fixedLinks.pop_back();
    auto duplicate = f.topology.parameters[0]; duplicate.id = AM::ParameterId{11};
    f.topology.parameters.push_back(duplicate);
    f.state.parameters.emplace(duplicate.id, AM::ParameterState{20.0});
    EXPECT_TRUE(ProjectCoreAudioControls(f.topology, f.state).empty());
}
TEST(CoreAudioProjection, SemanticClassesHaveOneMappingTable) {
    Fixture f;
    const std::array semantics{AM::ParameterSemantic::Mute, AM::ParameterSemantic::PhantomPower,
        AM::ParameterSemantic::PhaseInvert, AM::ParameterSemantic::NominalLevel, AM::ParameterSemantic::Source,
        AM::ParameterSemantic::ClockSource, AM::ParameterSemantic::HighPassFilter};
    const std::array<uint32_t, 7> classes{'mute', 'phan', 'phsi', 'nlvl', 'dsrc', 'clck', 'hipf'};
    for (size_t k = 0; k < semantics.size(); ++k) {
        auto& p = f.topology.parameters[0]; p.semantic = semantics[k];
        p.domain = k < 3 ? AM::ParameterDomain{AM::BooleanDomain{}} :
            AM::ParameterDomain{AM::EnumDomain{{{0, "A"}, {1, "B"}}}};
        f.state.parameters.at(p.id).value = k < 3 ? AM::ParameterValue{false} : AM::ParameterValue{int64_t{0}};
        const auto controls = ProjectCoreAudioControls(f.topology, f.state);
        ASSERT_EQ(controls.size(), 1);
        EXPECT_EQ(controls[0].classId, classes[k]);
    }
}

TEST(CoreAudioProjection, CompositeInputTypeUsesConnectorAndNominalLevelAndInactivePath) {
    Fixture f;
    f.topology.nodes.push_back({AM::NodeId{4}, "Jack", AM::EndpointNode{AM::EndpointKind::Physical}});
    f.topology.nodes.push_back({AM::NodeId{5}, "Input selector", AM::RouterNode{
        {AM::PortId{6}, AM::PortId{7}}, {AM::PortId{8}},
        {{AM::RouteBundleId{1}, {{AM::PortId{6}, AM::PortId{8}}}},
         {AM::RouteBundleId{2}, {{AM::PortId{7}, AM::PortId{8}}}}},
        {.maxActiveBundles = 1, .maxSourcesPerOutput = 1}}});
    f.topology.ports.push_back({AM::PortId{5}, AM::NodeId{4}, AM::PortDirection::Output, 1, "Jack",
                               {AM::SignalKind::AnalogInstrument, 1}});
    f.topology.ports.push_back({AM::PortId{6}, AM::NodeId{5}, AM::PortDirection::Input, 1, "XLR"});
    f.topology.ports.push_back({AM::PortId{7}, AM::NodeId{5}, AM::PortDirection::Input, 1, "Jack"});
    f.topology.ports.push_back({AM::PortId{8}, AM::NodeId{5}, AM::PortDirection::Output, 1, "Selected"});
    f.topology.fixedLinks = {{AM::PortId{1}, AM::PortId{6}}, {AM::PortId{5}, AM::PortId{7}},
                            {AM::PortId{8}, AM::PortId{2}}, {AM::PortId{3}, AM::PortId{4}}};
    f.topology.parameters.push_back({AM::ParameterId{11}, AM::PortId{1}, AM::ParameterSemantic::NominalLevel,
                                     AM::EnumDomain{{{0, "Mic"}, {1, "+4 dBu"}, {2, "-10 dBV"}}}, "XLR level"});
    f.state.parameters.emplace(AM::ParameterId{11}, AM::ParameterState{int64_t{1}});
    f.state.routers.emplace(AM::NodeId{5}, AM::RouterState{{AM::RouteBundleId{2}}});
    auto controls = ProjectInputTypes(f.topology, f.state);
    ASSERT_EQ(controls.size(), 1);
    EXPECT_EQ(controls[0].element, 2);
    ASSERT_EQ(controls[0].choices.size(), 4);
    EXPECT_EQ(controls[0].choices[0].name, "Mic");
    EXPECT_EQ(controls[0].choices[3].name, "Inst");
    EXPECT_EQ(controls[0].selected, 3);
    // A level choice contains the two ordered operations the caller needs.
    EXPECT_EQ(controls[0].choices[1].levelParameter, AM::ParameterId{11});
    EXPECT_EQ(controls[0].choices[1].levelValue, 1);
    EXPECT_EQ(controls[0].choices[1].bundle, AM::RouteBundleId{1});
    f.state.routers.at(AM::NodeId{5}).activeBundles = {AM::RouteBundleId{1}};
    controls = ProjectInputTypes(f.topology, f.state);
    EXPECT_EQ(controls[0].selected, 1);
    f.topology.parameters.push_back({AM::ParameterId{12}, AM::PortId{1}, AM::ParameterSemantic::PhantomPower,
                                     AM::BooleanDomain{}, "Phantom"});
    f.state.parameters.emplace(AM::ParameterId{12}, AM::ParameterState{false});
    f.state.routers.at(AM::NodeId{5}).activeBundles = {AM::RouteBundleId{2}};
    const auto projected = ProjectCoreAudioControls(f.topology, f.state);
    const auto phantom = std::ranges::find(projected, AM::ParameterId{12}, &CoreAudioControl::parameter);
    ASSERT_NE(phantom, projected.end());
    EXPECT_EQ(phantom->element, 2); // inactive XLR still belongs to this input
}

TEST(CoreAudioProjection, PositionTranslatorUsesRangeAndGridWithoutAmplitudeLaw) {
    const AM::ScalarDomain d{10, 75, 1, AM::ScalarUnit::Decibels};
    EXPECT_EQ(ScalarToPosition(d, 10), 0);
    EXPECT_EQ(ScalarToPosition(d, 75), 1);
    EXPECT_EQ(PositionToScalar(d, 0.5), 43);
    EXPECT_FALSE(PositionToScalar(d, -0.1));
    EXPECT_FALSE(PositionToScalar(d, std::numeric_limits<double>::quiet_NaN()));
    const AM::ScalarDomain uneven{0, 1, 0.3};
    EXPECT_NEAR(*PositionToScalar(uneven, 1), 0.9, 1e-12);
}
} // namespace
// A device-wide clock source sits on a node with no ports. The HAL reads the
// device's clock source list from a global-scope 'clck' selector, so that is
// where it goes. A clock parameter on a node with a host path is not device-wide.
TEST(CoreAudioProjection, PlacesADeviceWideClockSourceOnTheGlobalScope) {
    Fixture f;
    f.topology.nodes.push_back({AM::NodeId{9}, "Clock", AM::EndpointNode{AM::EndpointKind::Host}});
    f.topology.parameters.push_back({AM::ParameterId{20}, AM::NodeId{9}, AM::ParameterSemantic::ClockSource,
        AM::EnumDomain{{{0, "Internal"}, {16, "S/PDIF"}}}, "Clock Source"});
    f.state.parameters.emplace(AM::ParameterId{20}, AM::ParameterState{int64_t{16}});
    ASSERT_TRUE(AM::validate(f.topology));
    const auto controls = ProjectCoreAudioControls(f.topology, f.state);
    const auto clock = std::ranges::find(controls, AM::ParameterId{20}, &CoreAudioControl::parameter);
    ASSERT_NE(clock, controls.end());
    EXPECT_EQ(clock->classId, uint32_t{'clck'});
    EXPECT_EQ(clock->scope, uint32_t{'glob'});
    EXPECT_EQ(clock->element, 0U);
    EXPECT_EQ(clock->kind, CoreAudioControlKind::Selector);
}
TEST(CoreAudioProjection, KeepsAClockSourceOnAPortedNodeOffTheGlobalScope) {
    Fixture f;
    // Node 1 owns the microphone port, so a clock parameter there is not device-wide.
    f.topology.parameters.push_back({AM::ParameterId{20}, AM::NodeId{1}, AM::ParameterSemantic::ClockSource,
        AM::EnumDomain{{{0, "Internal"}, {16, "S/PDIF"}}}, "Clock Source"});
    f.state.parameters.emplace(AM::ParameterId{20}, AM::ParameterState{int64_t{0}});
    const auto controls = ProjectCoreAudioControls(f.topology, f.state);
    EXPECT_TRUE(std::ranges::none_of(controls, [](const auto& c) { return c.scope == uint32_t{'glob'}; }));
}
