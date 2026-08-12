// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ApogeeDuetStreamCapsTests.cpp - Regression tests for the Duet's runtime
// stream capabilities.
//
// AudioStreamRuntimeCaps carries two views of the same streams: an aggregate
// view (what CoreAudio publishes) and a per-stream wire view (what the audio
// stack turns into an AudioStreamConfig). A protocol that fills only the
// aggregate view publishes a plausible-looking 2-in/2-out device that then
// fails StartIO at BuildDefaultTxStreamConfig, because
// ResolvedAudioStreamProfile::BuildStream reads hostToDeviceStreams[0] and
// rejects a zero pcmChannels. These tests pin both views together.

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/Oxford/Apogee/ApogeeDuetProtocol.hpp"

#include "AvcTestRig.hpp"

namespace {

using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol;
using ASFW::Testing::AvcTestRig;

[[nodiscard]] AudioStreamRuntimeCaps QueryCaps(AvcTestRig& rig) {
    ApogeeDuetProtocol protocol(rig.Bus(), rig.Bus(), rig.Route(), &rig.Routes(),
                                nullptr, nullptr, nullptr, 100U, &rig.Timers());
    AudioStreamRuntimeCaps caps{};
    EXPECT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    return caps;
}

TEST(ApogeeDuetStreamCapsTests, PublishesOneDuplexStreamPair) {
    AvcTestRig rig;
    const auto caps = QueryCaps(rig);

    EXPECT_EQ(caps.deviceToHostStreamCount, 1U);
    EXPECT_EQ(caps.hostToDeviceStreamCount, 1U);
    EXPECT_EQ(caps.hostInputPcmChannels, 2U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 2U);
}

TEST(ApogeeDuetStreamCapsTests, FillsPerStreamWireViewNotOnlyTheAggregate) {
    AvcTestRig rig;
    const auto caps = QueryCaps(rig);

    // Plug-0 inventory observes pcm=2 midiSlots=0 dbs=2 in both directions.
    EXPECT_EQ(caps.deviceToHostStreams[0].pcmChannels, 2U);
    EXPECT_EQ(caps.deviceToHostStreams[0].am824Slots, 2U);
    EXPECT_EQ(caps.hostToDeviceStreams[0].pcmChannels, 2U);
    EXPECT_EQ(caps.hostToDeviceStreams[0].am824Slots, 2U);
}

TEST(ApogeeDuetStreamCapsTests, AggregateAndPerStreamViewsAgree) {
    AvcTestRig rig;
    const auto caps = QueryCaps(rig);

    // With a single stream per direction the aggregate is exactly stream[0].
    EXPECT_EQ(caps.hostInputPcmChannels, caps.deviceToHostStreams[0].pcmChannels);
    EXPECT_EQ(caps.hostOutputPcmChannels, caps.hostToDeviceStreams[0].pcmChannels);
    EXPECT_EQ(caps.deviceToHostAm824Slots, caps.deviceToHostStreams[0].am824Slots);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, caps.hostToDeviceStreams[0].am824Slots);
}

TEST(ApogeeDuetStreamCapsTests, SatisfiesTheStreamConfigPreconditions) {
    AvcTestRig rig;
    const auto caps = QueryCaps(rig);

    // The exact gates ResolvedAudioStreamProfile::BuildStream applies before it
    // will produce a TX/RX AudioStreamConfig.
    for (const auto& stream : {caps.hostToDeviceStreams[0], caps.deviceToHostStreams[0]}) {
        EXPECT_NE(stream.pcmChannels, 0U);
        EXPECT_GE(stream.am824Slots, stream.pcmChannels);
        EXPECT_LE(stream.pcmChannels, UINT8_MAX);
        EXPECT_LE(stream.am824Slots, UINT8_MAX);
    }
    EXPECT_NE(caps.sampleRateHz, 0U);
}

} // namespace
