// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuCommandDspTests.cpp - the MOTU mk3 command DSP (MotuCommandDsp.hpp):
// command bytes, framing, reassembly and the mixer state.
//
// Layouts cross-checked with snd-firewire-ctl-services protocols/motu/src/
// command_dsp.rs (DspCmd::parse, append_u8, append_f32, send_message,
// cache_dsp_messages). Not yet observed on the wire.

#include <gtest/gtest.h>

#include "Audio/Protocols/MOTU/MotuCommandDsp.hpp"
#include "Audio/Protocols/MOTU/MotuDspParameters.hpp"

#include <vector>

namespace {

using namespace ASFW::Audio::Motu::CommandDsp;

std::vector<uint8_t> Bytes(const Command& c) {
    std::vector<uint8_t> out;
    Encode(c, out);
    return out;
}

} // namespace

TEST(MotuCommandDsp, AByteCommandIsOpValueThenIdentifier) {
    // Mix 1, source 1 (Analog 1) mute on: second level 2 + 0, third 0.
    EXPECT_EQ(Bytes(Set::SourceMute(0, 0, true)),
              (std::vector<uint8_t>{0x69, 0x01, 0x00, 0x00, 0x02, 0x02}));
    EXPECT_EQ(Bytes(Set::OutputAssign(3, 6)),
              (std::vector<uint8_t>{0x69, 0x06, 0x03, 0x00, 0x00, 0x02}));
}

TEST(MotuCommandDsp, AFloatCommandIsOpIdentifierThenLittleEndianFloat) {
    // Mix 2, source 4 gain 0.5: 0.5f is 0x3f000000.
    EXPECT_EQ(Bytes(Set::SourceGain(1, 3, 0.5f)),
              (std::vector<uint8_t>{0x66, 0x01, 0x03, 0x05, 0x02, 0x00, 0x00, 0x00, 0x3f}));
    EXPECT_EQ(Bytes(Set::MainVolume(1.0f)),
              (std::vector<uint8_t>{0x66, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f}));
}

TEST(MotuCommandDsp, DecodeReadsWhatEncodeWrites) {
    const std::vector<Command> sent{Set::SourceMute(7, 9, true), Set::SourceGain(2, 4, 0.25f),
                                    Set::OutputVolume(5, 0.75f), Set::SourcePan(0, 1, -1.0f)};
    std::vector<uint8_t> message;
    for (const auto& c : sent) Encode(c, message);
    std::vector<Command> got;
    ASSERT_TRUE(Decode(message, got));
    EXPECT_EQ(got, sent);
}

TEST(MotuCommandDsp, AMultipleCommandExpandsOnePerChannel) {
    // Byte multiple: three mixers' output mute, then a quadlet multiple: two
    // mixers' output volume.
    const std::vector<uint8_t> message{
        0x49, 0x03, 0xff, 0x01, 0x00, 0x02, 0x01, 0x00, 0x01,
        0x46, 0x02, 0xff, 0x02, 0x00, 0x02, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x3f};
    std::vector<Command> got;
    ASSERT_TRUE(Decode(message, got));
    ASSERT_EQ(got.size(), 5u);
    EXPECT_EQ(got[0], Set::OutputMute(0, true));
    EXPECT_EQ(got[1], Set::OutputMute(1, false));
    EXPECT_EQ(got[2], Set::OutputMute(2, true));
    EXPECT_EQ(got[3], Set::OutputVolume(0, 1.0f));
    EXPECT_EQ(got[4], Set::OutputVolume(1, 0.5f));
}

TEST(MotuCommandDsp, ResourceAndDrainCarryNoParameterAndPaddingEndsTheMessage) {
    std::vector<uint8_t> message{0x23, 0x00, 0x00, 0x20, 0x41, 0x11, 0x62};
    Encode(Set::OutputMute(0, true), message);
    message.insert(message.end(), {0x00, 0x00, 0x00});
    std::vector<Command> got;
    ASSERT_TRUE(Decode(message, got));
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0], Set::OutputMute(0, true));
}

TEST(MotuCommandDsp, AnUnknownOpOrATruncatedCommandIsRefused) {
    std::vector<Command> got;
    EXPECT_FALSE(Decode(std::vector<uint8_t>{0x77, 0x01}, got));
    EXPECT_FALSE(Decode(std::vector<uint8_t>{0x66, 0x00, 0x00, 0x00, 0x02, 0x00}, got));
    EXPECT_FALSE(Decode(std::vector<uint8_t>{0x49, 0x05, 0x00, 0x00, 0x00, 0x02, 0x01}, got));
    EXPECT_TRUE(got.empty());
}

TEST(MotuCommandDsp, AShortMessageIsOneQuadletAlignedFrame) {
    std::vector<uint8_t> message;
    Encode(Set::SourceMute(0, 0, true), message);  // 6 bytes
    uint8_t sequence = 7;
    const auto frames = BuildFrames(message, kTagCommands, sequence);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0], (std::vector<uint8_t>{0x02, 0x07, 0x69, 0x01, 0x00, 0x00, 0x02, 0x02}));
    EXPECT_EQ(sequence, 8);
}

TEST(MotuCommandDsp, ALongMessageContinuesInTheNextFrame) {
    std::vector<uint8_t> message;
    for (int i = 0; i < 40; ++i) Encode(Set::SourceGain(0, static_cast<uint8_t>(i % 10), 0.5f), message);
    ASSERT_EQ(message.size(), 360u);
    uint8_t sequence = 0;
    const auto frames = BuildFrames(message, kTagCommands, sequence);
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0].size(), kMaxFrameBytes);
    EXPECT_EQ(frames[1][1], 1);
    EXPECT_EQ(frames[1].size() % 4, 0u);
    EXPECT_EQ(sequence, 2);
}

TEST(MotuCommandDsp, TheSendSequenceWrapsFrom254ToZero) {
    const std::vector<uint8_t> message{0x62};
    uint8_t sequence = 0xfe;
    (void)BuildFrames(message, kTagCommands, sequence);
    EXPECT_EQ(sequence, 0);
}

TEST(MotuCommandDsp, BeginningMessagingIsTwoEmptyFrames) {
    const std::vector<uint8_t> empty{0x00, 0x00};
    uint8_t sequence = 0;
    const auto begin = BuildFrames(empty, kTagBegin, sequence);
    const auto commands = BuildFrames(empty, kTagCommands, sequence);
    EXPECT_EQ(begin[0], (std::vector<uint8_t>{0x01, 0x00, 0x00, 0x00}));
    EXPECT_EQ(commands[0], (std::vector<uint8_t>{0x02, 0x01, 0x00, 0x00}));
}

TEST(MotuCommandDsp, ReassemblyUndoesFramingAcrossFrames) {
    std::vector<uint8_t> message;
    for (int i = 0; i < 40; ++i) Encode(Set::SourceGain(1, static_cast<uint8_t>(i % 10), 0.125f), message);
    uint8_t sequence = 200;
    const auto frames = BuildFrames(message, kTagCommands, sequence);
    MessageReassembler reassembler;
    EXPECT_FALSE(reassembler.Feed(frames[0]).has_value());
    const auto got = reassembler.Feed(frames[1]);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, message) << "padding removed, frames joined";
}

TEST(MotuCommandDsp, KeepAlivesCarryNothing) {
    MessageReassembler reassembler;
    EXPECT_FALSE(reassembler.Feed(std::vector<uint8_t>{0x02, 0x00, 0x00, 0x00}).has_value());
    EXPECT_FALSE(reassembler.Feed(std::vector<uint8_t>{0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}).has_value());
    std::vector<uint8_t> message;
    Encode(Set::OutputMute(0, true), message);
    uint8_t sequence = 2;
    const auto frames = BuildFrames(message, kTagCommands, sequence);
    EXPECT_TRUE(reassembler.Feed(frames[0]).has_value());
}

TEST(MotuCommandDsp, ALostFrameDropsThePartialMessage) {
    std::vector<uint8_t> message;
    for (int i = 0; i < 40; ++i) Encode(Set::SourceGain(0, 0, 0.5f), message);
    uint8_t sequence = 10;
    auto frames = BuildFrames(message, kTagCommands, sequence);
    MessageReassembler reassembler;
    EXPECT_FALSE(reassembler.Feed(frames[0]).has_value());
    frames[1][1] = 12;  // frame 11 lost
    EXPECT_FALSE(reassembler.Feed(frames[1]).has_value());
    EXPECT_EQ(reassembler.DroppedFrames(), 1u);
    // The next whole message is read normally.
    std::vector<uint8_t> next;
    Encode(Set::OutputMute(4, true), next);
    sequence = 13;
    const auto after = BuildFrames(next, kTagCommands, sequence);
    const auto got = reassembler.Feed(after[0]);
    ASSERT_TRUE(got.has_value());
    EXPECT_EQ(*got, next);
}

TEST(MotuCommandDsp, TheReceiveSequenceWrapsFrom255ToZero) {
    MessageReassembler reassembler;
    std::vector<uint8_t> message;
    Encode(Set::OutputMute(0, true), message);
    for (const uint8_t seq : {uint8_t{0xfe}, uint8_t{0xff}, uint8_t{0x00}}) {
        uint8_t sequence = seq;
        const auto frame = BuildFrames(message, kTagCommands, sequence)[0];
        EXPECT_TRUE(reassembler.Feed(frame).has_value()) << int{seq};
    }
    EXPECT_EQ(reassembler.DroppedFrames(), 0u);
}

// The parameter table and state (MotuDspParameters.hpp).

TEST(MotuCommandDsp, TheStateFollowsTheUnitsMessages) {
    const auto ports = DspPortsFor(0x19);
    ASSERT_TRUE(ports.has_value());
    EXPECT_EQ(ports->sources.size(), 10u);
    EXPECT_EQ(ports->outputs.size(), 7u);
    EXPECT_EQ(ports->inputs.size(), 10u);
    DspState state(*ports);
    for (const auto& c : {Set::OutputAssign(1, 6), Set::OutputMute(1, true), Set::OutputVolume(1, 0.8f),
                          Set::SourceGain(1, 9, 0.3f), Set::SourcePan(1, 9, 0.5f),
                          Set::SourceSolo(1, 2, true), Set::MainVolume(0.6f)})
        EXPECT_TRUE(state.Apply(c));
    EXPECT_EQ(state.Get(Id(Block::Mixer, 0, MixerThird::kOutputAssign, 1))->byte, 6);
    EXPECT_FLOAT_EQ(state.Get(Id(Block::Mixer, 11, MixerThird::kSourceGain, 1))->value, 0.3f);
    EXPECT_FLOAT_EQ(state.Get(Id(Block::Monitor, 0, MonitorThird::kMainVolume, 0))->value, 0.6f);
    EXPECT_EQ(state.Size(), 7u);
}

TEST(MotuCommandDsp, EveryBlockOfTheTableIsAddressable) {
    const auto ports = *DspPortsFor(0x19);
    DspState state(ports);
    // Input 3 LF gain (input EQ starts at second level 1: LF is 3), output 2
    // compressor threshold (output dynamics: compressor is 9), reverb decay,
    // a mixer source's stereo width.
    EXPECT_TRUE(state.Apply(Command::Float(Block::Input, 3, 3, 2, -6.0f)));
    EXPECT_TRUE(state.Apply(Command::Float(Block::Output, 9, 1, 1, -12.0f)));
    EXPECT_TRUE(state.Apply(Command::Float(Block::Reverb, 0, 5, 0, 2500.0f)));
    EXPECT_TRUE(state.Apply(Command::Float(Block::Mixer, 4, 6, 0, 0.5f)));
    EXPECT_TRUE(state.Apply(Command::Byte(Block::Input, 7, 1, 9, 4)));  // input 10 HF type Shelf
    EXPECT_EQ(state.Size(), 5u);
}

TEST(MotuCommandDsp, SteppedValuesAreTruncatedAndClampedAsTheUnitSendsThemOffStep) {
    DspState state(*DspPortsFor(0x19));
    const auto trim = [&](uint8_t channel) { return state.Get(Id(Block::Input, 0, 0x02, channel))->value; };
    // An UltraLite mk3 reported Analog 1 Trim as 0.12 (hardware, 2026-10-28);
    // the reference truncates to i32.
    EXPECT_TRUE(state.Apply(Command::Float(Block::Input, 0, 0x02, 0, 0.12f)));
    EXPECT_FLOAT_EQ(trim(0), 0.0f);
    EXPECT_TRUE(state.Apply(Command::Float(Block::Input, 0, 0x02, 1, -3.7f)));
    EXPECT_FLOAT_EQ(trim(1), -3.0f);
    EXPECT_TRUE(state.Apply(Command::Float(Block::Input, 0, 0x02, 2, 30.0f)));
    EXPECT_FLOAT_EQ(trim(2), 22.0f);
    // Continuous parameters keep their value inside the range.
    EXPECT_TRUE(state.Apply(Set::MainVolume(0.33f)));
    EXPECT_FLOAT_EQ(state.Get(Id(Block::Monitor, 0, MonitorThird::kMainVolume, 0))->value, 0.33f);
}

TEST(MotuCommandDsp, CommandsOutsideTheTableOrOfTheWrongWidthAreIgnored) {
    DspState state(*DspPortsFor(0x19));
    EXPECT_FALSE(state.Apply(Set::SourceGain(0, 10, 1.0f)));  // no 11th source
    EXPECT_FALSE(state.Apply(Set::SourceMute(8, 0, true)));   // no 9th mixer
    EXPECT_FALSE(state.Apply(Command::Byte(Block::Mixer, 0, MixerThird::kOutputVolume, 0, 1)));
    EXPECT_FALSE(state.Apply(Command::Float(Block::Input, 0, 0x09, 0, 1.0f)));  // pad: not on this model
    EXPECT_FALSE(state.Apply(Command::Float(Block::Output, 0, 0, 7, 1.0f)));    // no 8th output
    EXPECT_FALSE(state.Apply(Command::Byte(Block::Monitor, 0, 0x07, 0, 1)));    // focus: not a parameter
    EXPECT_EQ(state.Size(), 0u);
}

TEST(MotuCommandDsp, OnlyTheUltraLiteMk3HasKnownDspPorts) {
    for (const uint32_t version : {0x15U, 0x17U, 0x1bU, 0x30U, 0x39U, 0x03U})
        EXPECT_FALSE(DspPortsFor(version).has_value()) << version;
}

TEST(MotuCommandDsp, EveryParameterIdRoundTripsToItsAddress) {
    const auto ports = *DspPortsFor(0x19);
    size_t checked = 0;
    for (const auto& spec : kParams) {
        const size_t sources = spec.scope == Scope::MixerSource ? ports.sources.size() : 1;
        for (size_t ch = 0; ch < ports.Count(spec.scope); ++ch)
            for (size_t src = 0; src < sources; ++src) {
                const uint32_t id = ParameterId(spec, static_cast<uint8_t>(ch), static_cast<uint8_t>(src));
                ASSERT_TRUE(IsDspParameterId(id)) << std::hex << id;
                ASSERT_EQ(id & 0x80000000U, 0u);
                auto c = AddressOf(id);
                c.quadlet = spec.kind == Kind::Float;
                const auto at = Lookup(ports, c);
                ASSERT_TRUE(at.has_value()) << spec.name << " " << ch;
                EXPECT_EQ(at->spec, &spec) << spec.name;
                ++checked;
            }
    }
    EXPECT_GT(checked, 1400u);
}

// ---- Mailbox: what the unit sends, as control listeners see it ----

#include "Audio/Protocols/MOTU/MotuDspMessages.hpp"

namespace {

std::vector<ASFW::Audio::Controls::ParameterChanged> gEvents;
int gListener = 0;

ASFW::Discovery::DeviceRouteToken UltraLiteRoute() {
    ASFW::Discovery::DeviceRouteToken route{};
    route.guid = 0x0001f2000008be94ULL;
    route.deviceIncarnation = 1;
    route.routeEpoch = 7;
    route.generation = decltype(route.generation){3};
    route.nodeId = 0;  // the unit sends from 0xffc0: bus 0x3ff, node 0
    return route;
}

// The unit's frames: tag 2, a running sequence number, then the message.
void Send(MessageMailbox& mailbox, const std::vector<Command>& commands, uint8_t& sequence) {
    std::vector<uint8_t> message;
    for (const auto& c : commands) Encode(c, message);
    for (const auto& frame : BuildFrames(message, 0x02, sequence))
        ASSERT_TRUE(mailbox.Deliver(3, 0xffc0, frame).accepted);
}

struct Listening {
    Listening() {
        gEvents.clear();
        EXPECT_TRUE(ASFW::Audio::Controls::ControlNotifications::Shared().Register(
            &gListener, [](void*, const auto& event) { gEvents.push_back(event); }));
    }
    ~Listening() { ASFW::Audio::Controls::ControlNotifications::Shared().Unregister(&gListener); }
};

} // namespace

TEST(MotuDspMailbox, AFrontPanelChangeAnnouncesTheValueNotAReread) {
    Listening listening;
    MessageMailbox mailbox(UltraLiteRoute(), *DspPortsFor(0x19));
    uint8_t sequence = 0;
    // The main volume knob, as an UltraLite mk3 sent it (hardware, 2026-10-28).
    Send(mailbox, {Set::MainVolume(0.2304f)}, sequence);
    ASSERT_EQ(gEvents.size(), 1u);
    const auto& event = gEvents.front();
    EXPECT_EQ(event.guid, 0x0001f2000008be94ULL);
    EXPECT_EQ(event.revision, 7u);
    EXPECT_EQ(event.parameterId, Id(Block::Monitor, 0, MonitorThird::kMainVolume, 0));
    EXPECT_EQ(event.kind, 1u);
    EXPECT_FLOAT_EQ(static_cast<float>(std::bit_cast<double>(event.bits)), 0.2304f);
    // The same value again changes nothing and announces nothing.
    Send(mailbox, {Set::MainVolume(0.2304f)}, sequence);
    EXPECT_EQ(gEvents.size(), 1u);
}

TEST(MotuDspMailbox, TheStateDumpAsksForOneRereadOfEverything) {
    Listening listening;
    MessageMailbox mailbox(UltraLiteRoute(), *DspPortsFor(0x19));
    uint8_t sequence = 0;
    std::vector<Command> dump;
    for (uint8_t mix = 0; mix < 8; ++mix) {
        dump.push_back(Set::OutputVolume(mix, 0.5f));
        dump.push_back(Set::OutputMute(mix, true));
        dump.push_back(Set::SourceGain(mix, 0, 0.25f));
    }
    Send(mailbox, dump, sequence);
    ASSERT_EQ(gEvents.size(), 1u);
    EXPECT_EQ(gEvents.front().kind, 4u);
    EXPECT_EQ(gEvents.front().parameterId, 0u);
}
