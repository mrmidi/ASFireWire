// MidiEndpointCapabilitiesTests.cpp
// ASFW - WP-3/WP-1 capability projection tests
//
// The projection reads wire geometry the audio discovery path already parsed
// and decides what MIDI, if any, can be published. Every rejection here is a
// case where guessing would publish an endpoint that silently drops bytes.

#include <gtest/gtest.h>

#include "Midi/Capabilities/MidiEndpointCapabilities.hpp"

using namespace ASFW::Midi;
using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::AudioStreamWireInfo;

namespace {

constexpr uint64_t kGuid = 0x0011'2233'4455'6677ULL;

/// A Saffire-shaped duplex formation: one stream each way, trailing MIDI slot.
AudioStreamRuntimeCaps MakeCaps(uint32_t d2hPcm, uint32_t d2hMidi,
                                uint32_t h2dPcm, uint32_t h2dMidi) {
    AudioStreamRuntimeCaps caps{};
    caps.sampleRateHz = 48000;

    caps.deviceToHostStreamCount = 1;
    caps.deviceToHostStreams[0].pcmChannels = static_cast<uint16_t>(d2hPcm);
    caps.deviceToHostStreams[0].midiPorts = static_cast<uint16_t>(d2hMidi);
    caps.deviceToHostStreams[0].am824Slots =
        static_cast<uint16_t>(d2hPcm + (d2hMidi + 7u) / 8u);

    caps.hostToDeviceStreamCount = 1;
    caps.hostToDeviceStreams[0].pcmChannels = static_cast<uint16_t>(h2dPcm);
    caps.hostToDeviceStreams[0].midiPorts = static_cast<uint16_t>(h2dMidi);
    caps.hostToDeviceStreams[0].am824Slots =
        static_cast<uint16_t>(h2dPcm + (h2dMidi + 7u) / 8u);
    return caps;
}

} // namespace

//==============================================================================
// The ordinary case
//==============================================================================

TEST(MidiEndpointCapabilities, OnePortEachWayProjectsBothDirections) {
    const auto caps = MakeCaps(8, 1, 6, 1);
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);

    ASSERT_EQ(midi.deviceToHost.status, MidiCapabilityStatus::kOk);
    EXPECT_EQ(midi.deviceToHost.portCount, 1);
    EXPECT_EQ(midi.deviceToHost.pcmChannels, 8);
    EXPECT_EQ(midi.deviceToHost.midiSlotIndex, 8) << "trailing slot";
    EXPECT_EQ(midi.deviceToHost.dbs, 9);
    EXPECT_TRUE(midi.deviceToHost.dbcAligned);
    EXPECT_EQ(midi.deviceToHost.streamIndex, 0);

    ASSERT_EQ(midi.hostToDevice.status, MidiCapabilityStatus::kOk);
    EXPECT_EQ(midi.hostToDevice.portCount, 1);
    EXPECT_EQ(midi.hostToDevice.midiSlotIndex, 6);
    EXPECT_EQ(midi.hostToDevice.dbs, 7);

    EXPECT_TRUE(midi.AnyUsable());
    EXPECT_EQ(midi.guid, kGuid);
    EXPECT_EQ(midi.streamEpoch, 1u);
}

TEST(MidiEndpointCapabilities, EightPortsFitOneMpxSlot) {
    const auto caps = MakeCaps(2, 8, 2, 8);
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    ASSERT_EQ(midi.deviceToHost.status, MidiCapabilityStatus::kOk);
    EXPECT_EQ(midi.deviceToHost.portCount, 8);
    EXPECT_EQ(midi.deviceToHost.dbs, 3);
}

//==============================================================================
// Absent and asymmetric MIDI
//==============================================================================

TEST(MidiEndpointCapabilities, NoMidiIsNotAnError) {
    const auto caps = MakeCaps(8, 0, 6, 0);
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    EXPECT_EQ(midi.deviceToHost.status, MidiCapabilityStatus::kNoMidi);
    EXPECT_EQ(midi.hostToDevice.status, MidiCapabilityStatus::kNoMidi);
    EXPECT_FALSE(midi.AnyUsable());
    EXPECT_EQ(midi.deviceToHost.portCount, 0);
}

TEST(MidiEndpointCapabilities, AsymmetricCountsProjectIndependently) {
    // Input only. Publishing a destination here would expose a port with no
    // wire slot behind it.
    const auto caps = MakeCaps(8, 1, 6, 0);
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    EXPECT_EQ(midi.deviceToHost.status, MidiCapabilityStatus::kOk);
    EXPECT_EQ(midi.deviceToHost.portCount, 1);
    EXPECT_EQ(midi.hostToDevice.status, MidiCapabilityStatus::kNoMidi);
    EXPECT_TRUE(midi.AnyUsable());
}

TEST(MidiEndpointCapabilities, OneDirectionRejectedLeavesTheOtherUsable) {
    auto caps = MakeCaps(8, 1, 6, 1);
    caps.hostToDeviceStreams[0].midiPorts = 9;   // over one MPX slot
    caps.hostToDeviceStreams[0].am824Slots = 8;
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    EXPECT_EQ(midi.deviceToHost.status, MidiCapabilityStatus::kOk);
    EXPECT_EQ(midi.hostToDevice.status,
              MidiCapabilityStatus::kPortCountUnsupported);
    EXPECT_TRUE(midi.AnyUsable());
}

TEST(MidiEndpointCapabilities, NoStreamAtAllIsDistinctFromNoMidi) {
    AudioStreamRuntimeCaps caps{};
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    EXPECT_EQ(midi.deviceToHost.status, MidiCapabilityStatus::kNoStream);
    EXPECT_EQ(midi.hostToDevice.status, MidiCapabilityStatus::kNoStream);
}

//==============================================================================
// Rejections
//==============================================================================

TEST(MidiEndpointCapabilities, MoreThanEightPortsIsRejected) {
    auto caps = MakeCaps(8, 16, 6, 1);
    caps.deviceToHostStreams[0].am824Slots = 10;   // 8 PCM + 2 MIDI slots
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    EXPECT_EQ(midi.deviceToHost.status,
              MidiCapabilityStatus::kPortCountUnsupported);
    EXPECT_EQ(midi.deviceToHost.portCount, 0) << "a rejected direction publishes nothing";
}

TEST(MidiEndpointCapabilities, MidiOnAStreamWeDoNotRouteIsRejected) {
    // Linux takes the maximum across streams but drives stream 0 only. Taking
    // that maximum without routing stream 1 would publish a port that can
    // never carry a byte.
    auto caps = MakeCaps(8, 1, 6, 1);
    caps.deviceToHostStreamCount = 2;
    caps.deviceToHostStreams[1].pcmChannels = 8;
    caps.deviceToHostStreams[1].midiPorts = 1;
    caps.deviceToHostStreams[1].am824Slots = 9;
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    EXPECT_EQ(midi.deviceToHost.status,
              MidiCapabilityStatus::kMidiOnUnroutedStream);
}

TEST(MidiEndpointCapabilities, ASecondStreamWithoutMidiIsFine) {
    auto caps = MakeCaps(8, 1, 6, 1);
    caps.deviceToHostStreamCount = 2;
    caps.deviceToHostStreams[1].pcmChannels = 16;
    caps.deviceToHostStreams[1].midiPorts = 0;
    caps.deviceToHostStreams[1].am824Slots = 16;
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    EXPECT_EQ(midi.deviceToHost.status, MidiCapabilityStatus::kOk);
}

TEST(MidiEndpointCapabilities, MidiOnStreamZeroOnlyIsRoutable) {
    auto caps = MakeCaps(8, 2, 6, 2);
    caps.hostToDeviceStreamCount = 3;
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    EXPECT_EQ(midi.hostToDevice.status, MidiCapabilityStatus::kOk);
    EXPECT_EQ(midi.hostToDevice.portCount, 2);
}

TEST(MidiEndpointCapabilities, DbsThatDoesNotAccountForTheMidiSlotIsRejected) {
    // 8 PCM + 1 MIDI port reported in only 8 slots: the device is telling us
    // MIDI shares a PCM slot. With a trailing slot this is exactly the
    // PCM-overlap case, and writing MIDI would overwrite an audio channel in
    // every data block.
    auto caps = MakeCaps(8, 1, 6, 1);
    caps.deviceToHostStreams[0].am824Slots = 8;
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    EXPECT_EQ(midi.deviceToHost.status,
              MidiCapabilityStatus::kGeometryInconsistent);
}

TEST(MidiEndpointCapabilities, DbsLargerThanTheReportedContentsIsRejected) {
    auto caps = MakeCaps(8, 1, 6, 1);
    caps.deviceToHostStreams[0].am824Slots = 12;
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    EXPECT_EQ(midi.deviceToHost.status,
              MidiCapabilityStatus::kGeometryInconsistent);
}

TEST(MidiEndpointCapabilities, ZeroPcmWithMidiStillProjects) {
    // A MIDI-only formation is legal geometry: DBS 1, MIDI in slot 0.
    auto caps = MakeCaps(0, 1, 0, 1);
    const auto midi = ProjectMidiCapabilities(caps, kGuid, 1);
    ASSERT_EQ(midi.deviceToHost.status, MidiCapabilityStatus::kOk);
    EXPECT_EQ(midi.deviceToHost.midiSlotIndex, 0);
    EXPECT_EQ(midi.deviceToHost.dbs, 1);
}

//==============================================================================
// Identity
//==============================================================================

TEST(MidiPortKeyTest, IsStableAcrossEpochsAndProjections) {
    const auto first = ProjectMidiCapabilities(MakeCaps(8, 1, 6, 1), kGuid, 1);
    const auto afterReset =
        ProjectMidiCapabilities(MakeCaps(8, 1, 6, 1), kGuid, 77);

    EXPECT_NE(first.streamEpoch, afterReset.streamEpoch);
    EXPECT_EQ(MidiPortKey(first.guid, MidiDirection::kDeviceToHost, 0),
              MidiPortKey(afterReset.guid, MidiDirection::kDeviceToHost, 0))
        << "a replug must not create new logical ports";
}

TEST(MidiPortKeyTest, DirectionsAndPortsDoNotCollide) {
    const uint64_t source = MidiPortKey(kGuid, MidiDirection::kDeviceToHost, 0);
    const uint64_t dest = MidiPortKey(kGuid, MidiDirection::kHostToDevice, 0);
    const uint64_t secondPort =
        MidiPortKey(kGuid, MidiDirection::kDeviceToHost, 1);
    EXPECT_NE(source, dest);
    EXPECT_NE(source, secondPort);
    EXPECT_NE(dest, secondPort);
}

TEST(MidiPortKeyTest, NeighbouringGuidsDoNotCollideAcrossPorts) {
    // Sequential GUIDs are the realistic case -- two units off one production
    // run. A key that merely added the port index would alias them.
    for (uint8_t port = 0; port < 8; ++port) {
        for (uint64_t delta = 1; delta <= 4; ++delta) {
            EXPECT_NE(MidiPortKey(kGuid, MidiDirection::kDeviceToHost, port),
                      MidiPortKey(kGuid + delta, MidiDirection::kDeviceToHost, 0))
                << "port " << int(port) << " delta " << delta;
        }
    }
}

TEST(MidiPortKeyTest, DifferentDevicesDifferentKeys) {
    EXPECT_NE(MidiPortKey(kGuid, MidiDirection::kDeviceToHost, 0),
              MidiPortKey(kGuid ^ 0x1ULL, MidiDirection::kDeviceToHost, 0));
}
