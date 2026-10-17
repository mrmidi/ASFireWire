// SPDX-License-Identifier: Apache-2.0
#include "Audio/Protocols/Oxford/Apogee/DuetControlModel.hpp"
#include "Audio/Controls/CoreAudioProjection.hpp"
#include "Audio/Model/Topology/Validate.hpp"
#include <gtest/gtest.h>
#include <algorithm>
namespace {
using namespace ASFW::Audio::Oxford::Apogee;
namespace C = ASFW::Audio::Controls;
namespace AM = ASFW::AudioModel;
DuetControlsState State() {
    DuetControlsState state;
    state.input.gains = {10, 75};
    state.output.volume = 64;
    return state;
}
TEST(DuetControlModel, TopologyAndObservedStateValidate) {
    const auto model = BuildDuetControls(State(), 7, 10);
    ASSERT_TRUE(AM::validate(model.topology)) << AM::validate(model.topology).error().message;
    ASSERT_TRUE(AM::validateState(model.topology, model.state)) << AM::validateState(model.topology, model.state).error().message;
    EXPECT_EQ(model.family, "duet");
    EXPECT_EQ(model.sequence, 10);
    EXPECT_FALSE(model.state.parameters.contains(AM::ParameterId{DuetControl::ClearDisplay}));
}
TEST(DuetControlModel, GainUsesTheSameRawStepsWithModeDependentDecibels) {
    auto state = State();
    const auto mic = BuildDuetControls(state, 7, 0);
    state.input.sources[0] = InputSource::Phone;
    const auto inst = BuildDuetControls(state, 7, 0);
    const auto id = AM::ParameterId{DuetControl::Gain};
    EXPECT_EQ(mic.topology.revision, inst.topology.revision);
    EXPECT_DOUBLE_EQ(std::get<double>(mic.state.parameters.at(id).value), 10);
    EXPECT_DOUBLE_EQ(std::get<double>(inst.state.parameters.at(id).value), 0);
    EXPECT_DOUBLE_EQ(inst.state.parameters.at(id).domainOverride->max, 65);
    state.input.sources[0] = InputSource::Xlr;
    state.input.xlrNominalLevels[0] = InputXlrNominalLevel::Professional;
    const auto line = BuildDuetControls(state, 7, 0);
    EXPECT_FALSE(line.state.parameters.at(id).applicable);
    EXPECT_FALSE(AM::validateParameterWrite(line.topology, line.state, id, 20.0));
}
TEST(DuetControlModel, CompositeInputTypeHasFourDistinctStatesAndTwoElements) {
    auto state = State(); state.input.sources[1] = InputSource::Phone;
    auto model = BuildDuetControls(state, 7, 0);
    const auto types = C::ProjectInputTypes(model.topology, model.state);
    ASSERT_EQ(types.size(), 2);
    EXPECT_EQ(types[0].element, 1);
    EXPECT_EQ(types[1].element, 2);
    EXPECT_EQ(types[0].choices.size(), 4);
    EXPECT_EQ(types[0].choices[0].name, "Mic");
    EXPECT_EQ(types[0].choices[1].name, "+4 dBu");
    EXPECT_EQ(types[0].choices[2].name, "-10 dBV");
    EXPECT_EQ(types[0].choices[3].name, "Inst");
    EXPECT_EQ(types[0].selected, 0);
    EXPECT_EQ(types[1].selected, 3);
    const auto controls = C::ProjectCoreAudioControls(model.topology, model.state);
    EXPECT_EQ(std::ranges::count_if(controls, [](const auto& c) { return c.classId == uint32_t{'nlvl'} && c.scope == uint32_t{'inpt'}; }), 2);
}
TEST(DuetControlModel, OutputMasterRemainsAvailableForStreamAndMixerSources) {
    for (const auto source : {OutputSource::StreamInputPair0, OutputSource::MixerOutputPair0}) {
        auto state = State(); state.output.source = source;
        const auto model = BuildDuetControls(state, 7, 0);
        const auto controls = C::ProjectCoreAudioControls(model.topology, model.state);
        for (const auto id : {DuetControl::OutputVolume, DuetControl::OutputMute, DuetControl::OutputSource, DuetControl::OutputLevel}) {
            const auto found = std::ranges::find(controls, AM::ParameterId{id}, &C::CoreAudioControl::parameter);
            ASSERT_NE(found, controls.end()) << id;
            EXPECT_EQ(found->scope, uint32_t{'outp'});
            EXPECT_EQ(found->element, 0);
        }
    }
}
TEST(DuetControlModel, MixerDisplayAndKnobControlsAreAppOnly) {
    const auto model = BuildDuetControls(State(), 7, 0);
    const auto controls = C::ProjectCoreAudioControls(model.topology, model.state);
    for (const auto& c : controls) {
        EXPECT_FALSE(c.parameter.value >= DuetControl::Mixer && c.parameter.value < DuetControl::Mixer + 8);
        EXPECT_NE(c.parameter.value, DuetControl::DisplayMode);
        EXPECT_NE(c.parameter.value, DuetControl::KnobTarget);
    }
    const auto& volume = model.state.parameters.at(AM::ParameterId{DuetControl::OutputVolume});
    EXPECT_DOUBLE_EQ(std::get<double>(volume.value), 0);
}
}
