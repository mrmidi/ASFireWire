// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// The AUAA semantic core as ported to main (FW-202): naming, structural
// validation and state validation including provenance.

#include "Audio/Model/Topology/Naming.hpp"
#include "Audio/Model/Topology/Validate.hpp"
#include "UserClient/WireFormats/ControlSnapshotDocument.hpp"
#include <limits>

#include <gtest/gtest.h>

#include <algorithm>

namespace {
using namespace ASFW::AudioModel;

// A processor with one channel and a level/mute pair, plus a two-input router.
Topology SmallTopology() {
    Topology t;
    t.revision = 7;
    const NodeId gain{1}, router{2};
    t.ports = {
        Port{.id = PortId{1}, .owner = gain, .direction = PortDirection::Input, .channels = 1, .name = "In"},
        Port{.id = PortId{2}, .owner = gain, .direction = PortDirection::Output, .channels = 1, .name = "Out"},
        Port{.id = PortId{3}, .owner = router, .direction = PortDirection::Input, .channels = 1, .name = "A"},
        Port{.id = PortId{4}, .owner = router, .direction = PortDirection::Input, .channels = 1, .name = "B"},
        Port{.id = PortId{5}, .owner = router, .direction = PortDirection::Output, .channels = 1, .name = "Out"},
    };
    t.nodes = {
        Node{.id = gain, .name = "Gain", .body = ProcessorNode{.inputs = {PortId{1}}, .outputs = {PortId{2}}}},
        Node{.id = router, .name = "Select", .body = RouterNode{
            .inputs = {PortId{3}, PortId{4}}, .outputs = {PortId{5}},
            .legalBundles = {RouteBundle{RouteBundleId{1}, {Route{PortId{3}, PortId{5}}}},
                             RouteBundle{RouteBundleId{2}, {Route{PortId{4}, PortId{5}}}}},
            .constraints = RouterConstraints{.maxActiveBundles = 1}}},
    };
    t.parameters = {
        Parameter{.id = ParameterId{10}, .target = gain, .semantic = ParameterSemantic::Level,
                  .domain = ScalarDomain{.min = -60, .max = 0, .step = 0.5, .unit = ScalarUnit::Decibels},
                  .name = "Volume"},
        Parameter{.id = ParameterId{11}, .target = gain, .semantic = ParameterSemantic::Mute,
                  .domain = BooleanDomain{}, .name = "Mute"},
    };
    return t;
}

DeviceState StateFor(const Topology& t) {
    DeviceState s;
    s.topologyRevision = t.revision;
    s.parameters.emplace(ParameterId{10}, ParameterState{.value = -6.5, .origin = StateOrigin::Observed});
    s.parameters.emplace(ParameterId{11}, ParameterState{.value = false, .origin = StateOrigin::Cached});
    s.routers.emplace(NodeId{2}, RouterState{.activeBundles = {RouteBundleId{2}}});
    return s;
}

TEST(TopologyModel, CanonicalNamesAreTheSameForEveryDevice) {
    EXPECT_EQ(canonicalName({SignalKind::Adat, 3}, PortDirection::Output, 2), "ADAT In 3/4");
    EXPECT_EQ(canonicalName({SignalKind::Adat, 3}, PortDirection::Input, 1), "ADAT Out 3");
    EXPECT_EQ(canonicalName({SignalKind::AnalogMicXlr, 1}, PortDirection::Output, 1), "XLR In 1");
    EXPECT_EQ(canonicalName({SignalKind::SpdifCoaxial, 1}, PortDirection::Input, 2), "S/PDIF Out 1/2");
    EXPECT_EQ(canonicalName({SignalKind::HostStream, 5}, PortDirection::Output, 2), "Playback 5/6");
    EXPECT_EQ(canonicalName({SignalKind::HostStream, 5}, PortDirection::Input, 1), "Capture 5");
    EXPECT_EQ(canonicalName({SignalKind::Adat, 1}, PortDirection::Output, 8), "ADAT In 1-8");
}

TEST(TopologyModel, ValidTopologyAndStateValidate) {
    const auto t = SmallTopology();
    ASSERT_TRUE(validate(t).has_value()) << validate(t).error().message;
    const auto s = StateFor(t);
    const auto checked = validateState(t, s);
    EXPECT_TRUE(checked.has_value()) << (checked ? "" : checked.error().message);
}

TEST(TopologyModel, RouterPortOwnedByAnotherNodeIsForeign) {
    auto t = SmallTopology();
    std::get<RouterNode>(t.nodes[1].body).inputs.push_back(PortId{1}); // the gain's input
    const auto errors = validateAll(t);
    ASSERT_FALSE(errors.empty());
    EXPECT_TRUE(std::ranges::any_of(errors, [](const auto& e) { return e.kind == TopologyErrorKind::ForeignPortReference; }));
}

TEST(TopologyModel, DuplicateParameterIdIsRejected) {
    auto t = SmallTopology();
    t.parameters.push_back(t.parameters.front());
    const auto errors = validateAll(t);
    EXPECT_TRUE(std::ranges::any_of(errors, [](const auto& e) { return e.kind == TopologyErrorKind::DuplicateId; }));
}

TEST(TopologyModel, ScalarValueMustLieOnTheStepGridInsideTheRange) {
    const auto t = SmallTopology();
    EXPECT_TRUE(validateParameterValue(t, ParameterId{10}, -6.5).has_value());
    EXPECT_FALSE(validateParameterValue(t, ParameterId{10}, -6.3).has_value()); // off grid
    EXPECT_FALSE(validateParameterValue(t, ParameterId{10}, 3.0).has_value());  // above max
    EXPECT_FALSE(validateParameterValue(t, ParameterId{10}, true).has_value()); // wrong type
    EXPECT_FALSE(validateParameterValue(t, ParameterId{99}, 0.0).has_value());  // unknown id
}

TEST(TopologyModel, StateFromAnotherRevisionIsRefused) {
    const auto t = SmallTopology();
    auto s = StateFor(t);
    s.topologyRevision = t.revision + 1;
    const auto checked = validateState(t, s);
    ASSERT_FALSE(checked.has_value());
    EXPECT_EQ(checked.error().kind, StateErrorKind::TopologyRevisionMismatch);
}

TEST(TopologyModel, StateMustCoverEveryParameterAndNoOthers) {
    const auto t = SmallTopology();
    auto missing = StateFor(t);
    missing.parameters.erase(ParameterId{11});
    ASSERT_FALSE(validateState(t, missing).has_value());
    EXPECT_EQ(validateState(t, missing).error().kind, StateErrorKind::MissingParameter);

    auto extra = StateFor(t);
    extra.parameters.emplace(ParameterId{12}, ParameterState{.value = true});
    ASSERT_FALSE(validateState(t, extra).has_value());
    EXPECT_EQ(validateState(t, extra).error().kind, StateErrorKind::NonexistentParameter);
}

TEST(TopologyModel, ProvenanceTravelsWithTheValueAndDoesNotAffectLegality) {
    const auto t = SmallTopology();
    auto s = StateFor(t);
    EXPECT_EQ(s.parameters.at(ParameterId{11}).origin, StateOrigin::Cached);
    s.parameters.at(ParameterId{10}).origin = StateOrigin::Derived;
    EXPECT_TRUE(validateState(t, s).has_value());
    s.parameters.at(ParameterId{10}).value = 1.0; // illegal value, any origin
    EXPECT_FALSE(validateState(t, s).has_value());
}

TEST(TopologyModel, ActiveBundleMustBelongToTheRouter) {
    const auto t = SmallTopology();
    auto s = StateFor(t);
    s.routers.at(NodeId{2}).activeBundles = {RouteBundleId{9}};
    ASSERT_FALSE(validateState(t, s).has_value());
    EXPECT_EQ(validateState(t, s).error().kind, StateErrorKind::NonexistentRouteBundle);
}


TEST(TopologyModel, ReadOnlyValuesRemainObservableButCannotBeWritten) {
    auto t = SmallTopology();
    t.parameters[0].access = ParameterAccess::ReadOnly;
    auto s = StateFor(t);
    EXPECT_TRUE(validateState(t, s));
    const auto result = validateParameterWrite(t, s, ParameterId{10}, -10.0);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().kind, StateErrorKind::ReadOnlyParameter);
}

TEST(TopologyModel, DynamicRangeChangesWithoutChangingIdentityOrRevision) {
    auto t = SmallTopology();
    t.parameters[0].domain = ScalarDomain{10, 75, 1, ScalarUnit::Decibels};
    auto s = StateFor(t);
    auto& gain = s.parameters.at(ParameterId{10});
    gain.value = 20.0;
    EXPECT_TRUE(validateParameterWrite(t, s, ParameterId{10}, 75.0));
    gain.domainOverride = ScalarDomain{0, 65, 1, ScalarUnit::Decibels};
    EXPECT_TRUE(validateState(t, s));
    EXPECT_TRUE(validateParameterWrite(t, s, ParameterId{10}, 0.0));
    EXPECT_FALSE(validateParameterWrite(t, s, ParameterId{10}, 75.0));
    EXPECT_EQ(s.topologyRevision, 7);
    gain.applicable = false;
    auto refused = validateParameterWrite(t, s, ParameterId{10}, 20.0);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().kind, StateErrorKind::ParameterNotApplicable);
}

TEST(TopologyModel, MalformedDynamicRangesAreRejected) {
    const auto t = SmallTopology();
    auto s = StateFor(t);
    auto& gain = s.parameters.at(ParameterId{10});
    for (const auto domain : {
            ScalarDomain{-60, 0, 0.5, ScalarUnit::Hertz},
            ScalarDomain{0, -60, 0.5, ScalarUnit::Decibels},
            ScalarDomain{-60, 0, 0, ScalarUnit::Decibels},
            ScalarDomain{-60, 0, std::numeric_limits<double>::infinity(), ScalarUnit::Decibels},
            ScalarDomain{std::numeric_limits<double>::quiet_NaN(), 0, 1, ScalarUnit::Decibels}}) {
        gain.domainOverride = domain;
        EXPECT_FALSE(validateState(t, s));
        EXPECT_FALSE(validateParameterWrite(t, s, ParameterId{10}, -6.5));
    }
    gain.domainOverride.reset();
    s.parameters.at(ParameterId{11}).domainOverride = ScalarDomain{0, 1};
    EXPECT_FALSE(validateState(t, s));
}

TEST(TopologyModel, ActionsNeedNoRememberedResultAndOnlyAcceptTriggers) {
    auto t = SmallTopology();
    t.parameters.push_back(Parameter{ParameterId{12}, NodeId{1}, ParameterSemantic::Unknown,
                                     ActionDomain{}, "Clear display"});
    auto s = StateFor(t);
    EXPECT_TRUE(validate(t));
    EXPECT_TRUE(validateState(t, s));
    EXPECT_TRUE(validateParameterWrite(t, s, ParameterId{12}, ActionTrigger{}));
    EXPECT_FALSE(validateParameterWrite(t, s, ParameterId{12}, true));
    EXPECT_FALSE(validateParameterWrite(t, s, ParameterId{12}, int64_t{1}));
    const auto doc = ASFW::UserClient::Wire::BuildControlSnapshotDocument(1, "test", t, s, 0);
    EXPECT_NE(doc.find("\"kind\":\"action\""), std::string::npos);
    EXPECT_NE(doc.find("\"value\":null,\"origin\":null"), std::string::npos);
    s.parameters.emplace(ParameterId{12}, ParameterState{ActionTrigger{}, StateOrigin::Observed});
    const auto withMetadata = ASFW::UserClient::Wire::BuildControlSnapshotDocument(1, "test", t, s, 0);
    EXPECT_EQ(doc, withMetadata);
}

TEST(TopologyModel, SnapshotCarriesAccessAndDynamicStateMetadata) {
    auto t = SmallTopology();
    t.parameters[0].access = ParameterAccess::ReadOnly;
    auto s = StateFor(t);
    s.parameters.at(ParameterId{10}).domainOverride = ScalarDomain{-20, 0, 0.5, ScalarUnit::Decibels};
    s.parameters.at(ParameterId{10}).applicable = false;
    const auto doc = ASFW::UserClient::Wire::BuildControlSnapshotDocument(1, "test", t, s, 0);
    EXPECT_NE(doc.find("\"access\":\"readOnly\""), std::string::npos);
    EXPECT_NE(doc.find("\"applicable\":false"), std::string::npos);
    EXPECT_NE(doc.find("\"domainOverride\":{\"kind\":\"scalar\",\"min\":-20"), std::string::npos);
}

} // namespace
