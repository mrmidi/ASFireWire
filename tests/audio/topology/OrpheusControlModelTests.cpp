// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include "Audio/Protocols/Prism/Orpheus/OrpheusControlModel.hpp"
#include "Audio/Controls/CoreAudioProjection.hpp"
#include "Audio/Model/Topology/Validate.hpp"
#include <gtest/gtest.h>
#include <algorithm>

namespace {

using namespace ASFW::Audio::Prism::Orpheus;
namespace C = ASFW::Audio::Controls;
namespace AM = ASFW::AudioModel;

OrpheusControlsState MakeDefaultState() {
    OrpheusControlsState state;
    state.unit.masterVolume = -2560; // -10 dB in 1/256 dB
    state.unit.adatMode = 3;         // Send/Return
    state.syncSource = OrpheusSyncSource::Local;

    // Channels 0..1: Mic (XLR) default
    state.analog[0].inputType = 0;
    state.analog[0].micGain = 45;
    state.analog[0].phantom = true;

    state.analog[1].inputType = 2; // Instrument
    state.analog[1].instrumentGain = 20;

    // Channels 2..3: Mic (XLR) default
    state.analog[2].inputType = 0;
    state.analog[2].micGain = 30;
    state.analog[2].phantom = true;

    state.analog[3].inputType = 0;
    state.analog[3].micGain = 25;

    // Channels 4..7: Line
    for (size_t ch = 4; ch < 8; ++ch) {
        state.analog[ch].lineInputLevel = 1; // +4 dBu
    }

    return state;
}

TEST(OrpheusControlModelTests, TopologyAndObservedStateValidate) {
    const auto model = BuildOrpheusControls(MakeDefaultState(), 1, 100);
    const auto topValidation = AM::validate(model.topology);
    ASSERT_TRUE(topValidation) << topValidation.error().message;

    const auto stateValidation = AM::validateState(model.topology, model.state);
    ASSERT_TRUE(stateValidation) << stateValidation.error().message;

    EXPECT_EQ(model.family, "orpheus");
    EXPECT_EQ(model.sequence, 100U);
}

TEST(OrpheusControlModelTests, CoreAudioProjectionPublishesExactlyFourPhantomControls) {
    const auto model = BuildOrpheusControls(MakeDefaultState(), 1, 0);
    const auto controls = C::ProjectCoreAudioControls(model.topology, model.state);

    std::vector<C::CoreAudioControl> phantoms;
    for (const auto& c : controls) {
        if (c.classId == uint32_t{'phan'} && c.scope == uint32_t{'inpt'}) {
            phantoms.push_back(c);
        }
    }

    ASSERT_EQ(phantoms.size(), 4U);
    EXPECT_EQ(phantoms[0].element, 1U);
    EXPECT_EQ(phantoms[1].element, 2U);
    EXPECT_EQ(phantoms[2].element, 3U);
    EXPECT_EQ(phantoms[3].element, 4U);

    // Channels 5..8 do not have phantom power
    for (const auto& p : phantoms) {
        EXPECT_LE(p.element, 4U);
    }
}

TEST(OrpheusControlModelTests, CoreAudioProjectionMasterVolumeLandsOnElementZero) {
    const auto model = BuildOrpheusControls(MakeDefaultState(), 1, 0);
    const auto controls = C::ProjectCoreAudioControls(model.topology, model.state);

    const auto vol = std::ranges::find_if(controls, [](const auto& c) {
        return c.parameter == AM::ParameterId{OrpheusControl::OutputVolume};
    });

    ASSERT_NE(vol, controls.end());
    EXPECT_EQ(vol->scope, uint32_t{'outp'});
    EXPECT_EQ(vol->element, 0U); // Promoted to Master!
    EXPECT_EQ(vol->classId, uint32_t{'vlme'});
}

TEST(OrpheusControlModelTests, GainApplicabilityFollowsInputType) {
    auto state = MakeDefaultState();
    // Ch 0 is Mic (inputType 0)
    auto model = BuildOrpheusControls(state, 1, 0);
    const auto gain0 = AM::ParameterId{OrpheusControl::Gain + 0};
    EXPECT_TRUE(model.state.parameters.at(gain0).applicable);

    // Set Ch 2 to Line (inputType 1)
    state.analog[2].inputType = 1;
    model = BuildOrpheusControls(state, 1, 0);
    const auto gain2 = AM::ParameterId{OrpheusControl::Gain + 2};
    EXPECT_FALSE(model.state.parameters.at(gain2).applicable);
}

TEST(OrpheusControlModelTests, CompositeInputTypesArePublishedForPreampChannels) {
    const auto model = BuildOrpheusControls(MakeDefaultState(), 1, 0);
    const auto types = C::ProjectInputTypes(model.topology, model.state);

    ASSERT_EQ(types.size(), 4U); // Ch 1..4 have composite routers
    EXPECT_EQ(types[0].element, 1U);
    EXPECT_EQ(types[1].element, 2U);
    EXPECT_EQ(types[2].element, 3U);
    EXPECT_EQ(types[3].element, 4U);

    // Ch 1 (elements 1 & 2 have XLR vs Instrument)
    EXPECT_GE(types[0].choices.size(), 4U); // Mic, +4, -10, Inst
}

TEST(OrpheusControlModelTests, MixerCrosspointCountMatchesSixBusesByTwelveInputs) {
    const auto model = BuildOrpheusControls(MakeDefaultState(), 1, 0);
    const auto node = std::ranges::find_if(model.topology.nodes, [](const auto& n) {
        return n.name == "Hardware Mixer";
    });

    ASSERT_NE(node, model.topology.nodes.end());
    const auto* mixer = std::get_if<AM::MixerNode>(&node->body);
    ASSERT_NE(mixer, nullptr);
    EXPECT_EQ(mixer->inputs.size(), 12U);
    EXPECT_EQ(mixer->outputs.size(), 6U);
    EXPECT_EQ(mixer->crosspoints.size(), 72U);
}

} // namespace
