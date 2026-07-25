// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeParamsSerdesTests.cpp - FW-128 round-trip coverage for the Duet params
// serialization. Every function under test is pure and static, so these need no
// bus, no transport and no device.
//
// Round-trip here means params -> commands -> params, asserting field by field.
// A "did not crash" assertion would pass on a serdes that dropped every field.

#include <gtest/gtest.h>

#include "Audio/Protocols/Oxford/Apogee/ApogeeParamsSerdes.hpp"

using namespace ASFW::Audio::Oxford::Apogee;
namespace Serdes = ASFW::Audio::Oxford::Apogee::ParamsSerdes;

namespace {

/// The device answers a status query by filling the value fields of the very
/// commands the query sent, so a control list is the right stand-in for a
/// response list when exercising Parse.
template <typename Params, typename SerdesT>
[[nodiscard]] Params RoundTrip(const Params& in) {
    return SerdesT::Parse(SerdesT::BuildControl(in));
}

//==============================================================================
// Knob
//==============================================================================

TEST(ApogeeParamsSerdes, KnobRoundTripsEveryField) {
    KnobState in{};
    in.outputMute = true;
    in.target = KnobTarget::InputPair1;
    in.outputVolume = 42;
    in.inputGains[0] = 17;
    in.inputGains[1] = 63;

    const auto out = RoundTrip<KnobState, Serdes::KnobSerdes>(in);

    EXPECT_EQ(out.outputMute, in.outputMute);
    EXPECT_EQ(out.target, in.target);
    EXPECT_EQ(out.outputVolume, in.outputVolume);
    EXPECT_EQ(out.inputGains[0], in.inputGains[0]);
    EXPECT_EQ(out.inputGains[1], in.inputGains[1]);
}

TEST(ApogeeParamsSerdes, KnobTargetRoundTripsAllThreeValues) {
    for (const auto target : {KnobTarget::OutputPair0, KnobTarget::InputPair0,
                              KnobTarget::InputPair1}) {
        KnobState in{};
        in.target = target;
        EXPECT_EQ((RoundTrip<KnobState, Serdes::KnobSerdes>(in).target), target);
    }
}

TEST(ApogeeParamsSerdes, KnobTargetUnknownByteFallsBackToOutputPair) {
    // The device is free to report a target byte we do not model; that must land
    // on the documented default rather than a reinterpreted enumerator.
    auto command = Serdes::BuildKnobStateControl(KnobState{});
    command.hwState[1] = 0x7F;
    EXPECT_EQ(Serdes::ParseKnobState(command).target, KnobTarget::OutputPair0);
}

TEST(ApogeeParamsSerdes, KnobOutputVolumeIsStoredInverted) {
    // Encoded as kOutputVolMax - volume, so a round trip alone would hide a
    // double inversion. Pin the on-the-wire byte too.
    KnobState in{};
    in.outputVolume = 10;
    const auto command = Serdes::BuildKnobStateControl(in);
    EXPECT_EQ(command.hwState[3], KnobState::kOutputVolMax - 10);
    EXPECT_EQ(Serdes::ParseKnobState(command).outputVolume, 10);
}

TEST(ApogeeParamsSerdes, KnobParseRejectsForeignCode) {
    const auto foreign = ApogeeVendorCommand::Bool(ApogeeVendorCommand::Code::OutMute, true);
    EXPECT_EQ(Serdes::ParseKnobState(foreign).outputVolume, KnobState{}.outputVolume);
}

TEST(ApogeeParamsSerdes, KnobGroupIsASingleCommand) {
    EXPECT_EQ(Serdes::KnobSerdes::BuildControl(KnobState{}).size(), 1U);
    EXPECT_EQ(Serdes::KnobSerdes::Parse({}).target, KnobState{}.target);
}

//==============================================================================
// Output
//==============================================================================

TEST(ApogeeParamsSerdes, OutputRoundTripsEveryField) {
    OutputParams in{};
    in.mute = true;
    in.volume = 99;
    in.source = OutputSource::MixerOutputPair0;
    in.nominalLevel = OutputNominalLevel::Consumer;
    in.lineMuteMode = OutputMuteMode::Swapped;
    in.hpMuteMode = OutputMuteMode::Normal;

    const auto out = RoundTrip<OutputParams, Serdes::OutputSerdes>(in);

    EXPECT_EQ(out.mute, in.mute);
    EXPECT_EQ(out.volume, in.volume);
    EXPECT_EQ(out.source, in.source);
    EXPECT_EQ(out.nominalLevel, in.nominalLevel);
    EXPECT_EQ(out.lineMuteMode, in.lineMuteMode);
    EXPECT_EQ(out.hpMuteMode, in.hpMuteMode);
}

TEST(ApogeeParamsSerdes, OutputMuteModeRoundTripsAllThreeValues) {
    for (const auto mode : {OutputMuteMode::Never, OutputMuteMode::Normal,
                            OutputMuteMode::Swapped}) {
        OutputParams in{};
        in.lineMuteMode = mode;
        in.hpMuteMode = mode;
        const auto out = RoundTrip<OutputParams, Serdes::OutputSerdes>(in);
        EXPECT_EQ(out.lineMuteMode, mode);
        EXPECT_EQ(out.hpMuteMode, mode);
    }
}

TEST(ApogeeParamsSerdes, OutputNominalLevelDefaultsToInstrumentWhenNotConsumer) {
    OutputParams in{};
    in.nominalLevel = OutputNominalLevel::Instrument;
    EXPECT_EQ((RoundTrip<OutputParams, Serdes::OutputSerdes>(in).nominalLevel),
              OutputNominalLevel::Instrument);
}

//==============================================================================
// Input
//==============================================================================

TEST(ApogeeParamsSerdes, InputRoundTripsEveryField) {
    InputParams in{};
    in.gains[0] = 12;
    in.gains[1] = 34;
    in.polarities[0] = true;
    in.polarities[1] = false;
    in.phantomPowerings[0] = false;
    in.phantomPowerings[1] = true;
    in.sources[0] = InputSource::Phone;
    in.sources[1] = InputSource::Xlr;
    in.xlrNominalLevels[0] = InputXlrNominalLevel::Microphone;
    in.xlrNominalLevels[1] = InputXlrNominalLevel::Consumer;
    in.clickless = true;

    const auto out = RoundTrip<InputParams, Serdes::InputSerdes>(in);

    EXPECT_EQ(out.gains[0], in.gains[0]);
    EXPECT_EQ(out.gains[1], in.gains[1]);
    EXPECT_EQ(out.polarities[0], in.polarities[0]);
    EXPECT_EQ(out.polarities[1], in.polarities[1]);
    EXPECT_EQ(out.phantomPowerings[0], in.phantomPowerings[0]);
    EXPECT_EQ(out.phantomPowerings[1], in.phantomPowerings[1]);
    EXPECT_EQ(out.sources[0], in.sources[0]);
    EXPECT_EQ(out.sources[1], in.sources[1]);
    EXPECT_EQ(out.xlrNominalLevels[0], in.xlrNominalLevels[0]);
    EXPECT_EQ(out.xlrNominalLevels[1], in.xlrNominalLevels[1]);
    EXPECT_EQ(out.clickless, in.clickless);
}

TEST(ApogeeParamsSerdes, InputXlrNominalLevelRoundTripsAllThreeValues) {
    // Professional is the "neither flag set" case, which is the one a naive
    // two-boolean encoding gets wrong.
    for (const auto level : {InputXlrNominalLevel::Microphone, InputXlrNominalLevel::Consumer,
                             InputXlrNominalLevel::Professional}) {
        InputParams in{};
        in.xlrNominalLevels[0] = level;
        in.xlrNominalLevels[1] = level;
        const auto out = RoundTrip<InputParams, Serdes::InputSerdes>(in);
        EXPECT_EQ(out.xlrNominalLevels[0], level);
        EXPECT_EQ(out.xlrNominalLevels[1], level);
    }
}

TEST(ApogeeParamsSerdes, InputParseIgnoresOutOfRangeChannelIndex) {
    // A device answering with a channel we do not have must not write past the
    // fixed-size arrays.
    auto commands = Serdes::InputSerdes::BuildControl(InputParams{});
    for (auto& command : commands) {
        command.index = 9;
    }
    const auto out = Serdes::ParseInputParams(commands);
    EXPECT_EQ(out.gains[0], InputParams{}.gains[0]);
    EXPECT_EQ(out.gains[1], InputParams{}.gains[1]);
}

//==============================================================================
// Mixer
//==============================================================================

TEST(ApogeeParamsSerdes, MixerRoundTripsEveryCoefficient) {
    MixerParams in{};
    uint16_t next = 100;
    for (auto& output : in.outputs) {
        output.analogInputs[0] = next++;
        output.analogInputs[1] = next++;
        output.streamInputs[0] = next++;
        output.streamInputs[1] = next++;
    }

    const auto out = RoundTrip<MixerParams, Serdes::MixerSerdes>(in);

    for (size_t dst = 0; dst < in.outputs.size(); ++dst) {
        EXPECT_EQ(out.outputs[dst].analogInputs[0], in.outputs[dst].analogInputs[0]) << "dst " << dst;
        EXPECT_EQ(out.outputs[dst].analogInputs[1], in.outputs[dst].analogInputs[1]) << "dst " << dst;
        EXPECT_EQ(out.outputs[dst].streamInputs[0], in.outputs[dst].streamInputs[0]) << "dst " << dst;
        EXPECT_EQ(out.outputs[dst].streamInputs[1], in.outputs[dst].streamInputs[1]) << "dst " << dst;
    }
}

TEST(ApogeeParamsSerdes, MixerParseIgnoresOutOfRangeDestination) {
    auto commands = Serdes::MixerSerdes::BuildControl(MixerParams{});
    for (auto& command : commands) {
        command.index2 = 7;
    }
    const auto out = Serdes::ParseMixerParams(commands);
    EXPECT_EQ(out.outputs[0].analogInputs[0], MixerParams{}.outputs[0].analogInputs[0]);
}

//==============================================================================
// Display
//==============================================================================

TEST(ApogeeParamsSerdes, DisplayRoundTripsEveryField) {
    DisplayParams in{};
    in.target = DisplayTarget::Input;
    in.mode = DisplayMode::FollowingToKnobTarget;
    in.overhold = DisplayOverhold::TwoSeconds;

    const auto out = RoundTrip<DisplayParams, Serdes::DisplaySerdes>(in);

    EXPECT_EQ(out.target, in.target);
    EXPECT_EQ(out.mode, in.mode);
    EXPECT_EQ(out.overhold, in.overhold);
}

TEST(ApogeeParamsSerdes, DisplayRoundTripsTheOppositeOfEveryField) {
    DisplayParams in{};
    in.target = DisplayTarget::Output;
    in.mode = DisplayMode::Independent;
    in.overhold = DisplayOverhold::Infinite;

    const auto out = RoundTrip<DisplayParams, Serdes::DisplaySerdes>(in);

    EXPECT_EQ(out.target, DisplayTarget::Output);
    EXPECT_EQ(out.mode, DisplayMode::Independent);
    EXPECT_EQ(out.overhold, DisplayOverhold::Infinite);
}

//==============================================================================
// Shape contract the diff engine relies on
//==============================================================================

TEST(ApogeeParamsSerdes, ControlCommandShapeIsIndependentOfValues) {
    // The engine diffs positionally, so a group must emit the same commands in
    // the same order whatever its values are. If this breaks, the diff silently
    // sends the wrong subset.
    const auto compareShape = [](const auto& a, const auto& b) {
        ASSERT_EQ(a.size(), b.size());
        for (size_t i = 0; i < a.size(); ++i) {
            EXPECT_EQ(a[i].code, b[i].code) << "position " << i;
            EXPECT_EQ(a[i].index, b[i].index) << "position " << i;
            EXPECT_EQ(a[i].index2, b[i].index2) << "position " << i;
        }
    };

    OutputParams outputA{};
    OutputParams outputB{};
    outputB.mute = true;
    outputB.volume = 55;
    outputB.lineMuteMode = OutputMuteMode::Swapped;
    compareShape(Serdes::OutputSerdes::BuildControl(outputA),
                 Serdes::OutputSerdes::BuildControl(outputB));

    InputParams inputA{};
    InputParams inputB{};
    inputB.gains[0] = 7;
    inputB.clickless = true;
    inputB.xlrNominalLevels[0] = InputXlrNominalLevel::Consumer;
    compareShape(Serdes::InputSerdes::BuildControl(inputA),
                 Serdes::InputSerdes::BuildControl(inputB));

    MixerParams mixerA{};
    MixerParams mixerB{};
    mixerB.outputs[0].analogInputs[0] = 500;
    compareShape(Serdes::MixerSerdes::BuildControl(mixerA),
                 Serdes::MixerSerdes::BuildControl(mixerB));
}

TEST(ApogeeParamsSerdes, QueryAndControlAgreeOnCommandCount) {
    // Cache reads the group with BuildQuery and writes it with BuildControl; a
    // count mismatch means one of them forgot a field.
    EXPECT_EQ(Serdes::OutputSerdes::BuildQuery().size(),
              Serdes::OutputSerdes::BuildControl(OutputParams{}).size());
    EXPECT_EQ(Serdes::InputSerdes::BuildQuery().size(),
              Serdes::InputSerdes::BuildControl(InputParams{}).size());
    EXPECT_EQ(Serdes::MixerSerdes::BuildQuery().size(),
              Serdes::MixerSerdes::BuildControl(MixerParams{}).size());
    EXPECT_EQ(Serdes::DisplaySerdes::BuildQuery().size(),
              Serdes::DisplaySerdes::BuildControl(DisplayParams{}).size());
    EXPECT_EQ(Serdes::KnobSerdes::BuildQuery().size(),
              Serdes::KnobSerdes::BuildControl(KnobState{}).size());
}

} // namespace
