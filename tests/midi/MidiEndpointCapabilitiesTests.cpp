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

//==============================================================================
// Entity layout published to CoreMIDI
//==============================================================================

#include "Midi/Core/MidiNubProperties.hpp"

namespace {

MidiEndpointCapabilities Caps(uint32_t sources, uint32_t destinations) {
    return ProjectMidiCapabilities(MakeCaps(8, sources, 6, destinations),
                                   kGuid, 1);
}

} // namespace

TEST(MidiEntityLayout, SymmetricPortsGiveOneEntityPerJackPair) {
    const auto caps = Caps(2, 2);
    ASSERT_EQ(ASFW::Midi::NubKeys::EntityCount(caps), 2u);
    for (uint32_t i = 0; i < 2; ++i) {
        EXPECT_TRUE(ASFW::Midi::NubKeys::EntityHasSource(caps, i));
        EXPECT_TRUE(ASFW::Midi::NubKeys::EntityHasDestination(caps, i));
    }
}

TEST(MidiEntityLayout, AsymmetricPortsOmitTheSideThatDoesNotExist) {
    // Two in, one out. The second entity must have a source and no
    // destination, not a phantom destination whose bytes go nowhere.
    const auto caps = Caps(2, 1);
    ASSERT_EQ(ASFW::Midi::NubKeys::EntityCount(caps), 2u);
    EXPECT_TRUE(ASFW::Midi::NubKeys::EntityHasSource(caps, 0));
    EXPECT_TRUE(ASFW::Midi::NubKeys::EntityHasDestination(caps, 0));
    EXPECT_TRUE(ASFW::Midi::NubKeys::EntityHasSource(caps, 1));
    EXPECT_FALSE(ASFW::Midi::NubKeys::EntityHasDestination(caps, 1));
}

TEST(MidiEntityLayout, OutputOnlyPublishesDestinationsOnly) {
    const auto caps = Caps(0, 2);
    ASSERT_EQ(ASFW::Midi::NubKeys::EntityCount(caps), 2u);
    EXPECT_FALSE(ASFW::Midi::NubKeys::EntityHasSource(caps, 0));
    EXPECT_TRUE(ASFW::Midi::NubKeys::EntityHasDestination(caps, 0));
}

TEST(MidiEntityLayout, NoMidiPublishesNoEntities) {
    EXPECT_EQ(ASFW::Midi::NubKeys::EntityCount(Caps(0, 0)), 0u);
}

TEST(MidiEntityLayout, ARejectedDirectionContributesNoEntities) {
    // 9 ports exceeds one MPX slot, so that direction is rejected and must not
    // be counted even though the raw port count is nonzero.
    auto raw = MakeCaps(8, 9, 6, 1);
    raw.deviceToHostStreams[0].am824Slots = 10;
    const auto caps = ProjectMidiCapabilities(raw, kGuid, 1);
    ASSERT_FALSE(caps.deviceToHost.Usable());
    EXPECT_EQ(ASFW::Midi::NubKeys::EntityCount(caps), 1u) << "only the usable destination remains";
    EXPECT_FALSE(ASFW::Midi::NubKeys::EntityHasSource(caps, 0));
    EXPECT_TRUE(ASFW::Midi::NubKeys::EntityHasDestination(caps, 0));
}

// A family that declares its own slot (the Digi 002 / 003: MIDI in slot 0,
// PCM in slots 1-18) is checked the same way a projected geometry is.
namespace {
constexpr uint64_t kPcmAfterSlot0 = ((uint64_t{1} << 18) - 1U) << 1;
using ASFW::Encoding::MidiWireFormat;
} // namespace

TEST(DeclareMidiDirection, ASlotOutsideThePcmIsAccepted) {
    const auto d = DeclareMidiDirection(MidiWireFormat::kDigi00x, 2, 0, 19, 18, kPcmAfterSlot0);
    EXPECT_EQ(d.status, MidiCapabilityStatus::kOk);
    EXPECT_TRUE(d.Usable());
    EXPECT_EQ(d.portCount, 2U);
    EXPECT_EQ(d.midiSlotIndex, 0U);
    EXPECT_EQ(d.dbs, 19U);
    EXPECT_EQ(d.pcmChannels, 18U);
    EXPECT_EQ(d.format, MidiWireFormat::kDigi00x);
}

TEST(DeclareMidiDirection, ASlotOnAPcmChannelIsRefused) {
    // Where the trailing-slot projection would put it: PCM channel 18.
    const auto d = DeclareMidiDirection(MidiWireFormat::kDigi00x, 1, 18, 19, 18, kPcmAfterSlot0);
    EXPECT_EQ(d.status, MidiCapabilityStatus::kSlotOverlapsPcm);
    EXPECT_FALSE(d.Usable());
}

TEST(DeclareMidiDirection, ASlotOutsideTheBlockIsRefused) {
    const auto d = DeclareMidiDirection(MidiWireFormat::kDigi00x, 1, 19, 19, 18, kPcmAfterSlot0);
    EXPECT_EQ(d.status, MidiCapabilityStatus::kSlotOutOfRange);
}

TEST(DeclareMidiDirection, AnInconsistentGeometryIsRefused) {
    // DBS must be the PCM plus the one MIDI slot, and the PCM mask must name
    // exactly the PCM channels.
    EXPECT_EQ(DeclareMidiDirection(MidiWireFormat::kDigi00x, 1, 0, 20, 18, kPcmAfterSlot0).status,
              MidiCapabilityStatus::kGeometryInconsistent);
    EXPECT_EQ(DeclareMidiDirection(MidiWireFormat::kDigi00x, 1, 0, 19, 18, (kPcmAfterSlot0 & ~(uint64_t{1} << 18))).status,
              MidiCapabilityStatus::kGeometryInconsistent);
}

TEST(DeclareMidiDirection, NoPortsIsNoMidiAndTooManyIsRefused) {
    EXPECT_EQ(DeclareMidiDirection(MidiWireFormat::kDigi00x, 0, 0, 19, 18, kPcmAfterSlot0).status,
              MidiCapabilityStatus::kNoMidi);
    EXPECT_EQ(DeclareMidiDirection(MidiWireFormat::kDigi00x, 9, 0, 19, 18, kPcmAfterSlot0).status,
              MidiCapabilityStatus::kPortCountUnsupported);
}

// The trailing-slot projection keeps the IEC 61883-6 format.
TEST(DeclareMidiDirection, AProjectedDirectionIsMpx) {
    AudioStreamRuntimeCaps caps{};
    caps.hostToDeviceStreamCount = 1;
    caps.hostToDeviceStreams[0] = AudioStreamWireInfo{.pcmChannels = 8, .am824Slots = 9, .midiPorts = 1};
    const auto projected = ProjectMidiCapabilities(caps, 1, 0);
    EXPECT_EQ(projected.hostToDevice.format, MidiWireFormat::kAm824Mpx);
}
