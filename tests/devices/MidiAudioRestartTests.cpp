// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MidiAudioRestartTests.cpp - Audio stop/start/restart is the same with or
// without a MIDI client.
//
// The defect this guards (midi branch, bisect-confirmed on a Saffire Pro 24
// DSP 2026-09-14): with the MIDI nub published, a CoreAudio stop/start did not
// recover audio. The isoch transmit context started once across several
// StartIO/StopIO cycles, rebound rose from 366 to ~29,000 and missedDeadline
// from 0 to 806. The suspect was a MIDI lease that kept the shared session
// "streaming" across the stop, so the start path never re-ran.
//
// On main MIDI holds nothing of the session. These tests drive the real
// session scheduler and a real DICE protocol on the simulated Pro 24 DSP
// through exactly the bracket the coordinator uses (MidiRunBracket.hpp), with
// a MIDI client holding the seam open the way ASFWMidiDevice does (the
// host -> device ingress and the device -> host drain), and compare every host
// transport call against the same sequence with no MIDI at all.

#include <gtest/gtest.h>

#include "SessionTestSupport.hpp"

#include "Audio/Core/AudioEndpointRuntime.hpp"
#include "Audio/Core/MidiRunBracket.hpp"
#include "Common/DriverKitOwnership.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "Midi/Capabilities/MidiEndpointCapabilities.hpp"
#include "Midi/Transport/MidiRxDrain.hpp"
#include "Midi/Transport/MidiTxIngress.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace ASFW::Testing::Session;
using ASFW::Audio::AudioEndpointRuntime;
using ASFW::Audio::StartWithMidiRun;
using ASFW::Audio::StopWithMidiRun;
using ASFW::Midi::MidiTransportBlock;
using ASFW::Midi::MidiTxStatus;
namespace Ids = ASFW::DeviceProfiles::Audio;

constexpr SessionShape kPro24Dsp{"pro24dsp", Ids::kFocusriteVendorId, Ids::kSPro24DspModelId,
                                 Ids::kFocusriteVendorId, kDiceUnitVersion,
                                 &ASFW::Testing::DICE::DiceDeviceImages::kSaffirePro24Dsp, true};

// One MIDI port each way, after 8 PCM channels: the shape a DICE stream table
// with midiPorts = 1 projects to.
ASFW::Midi::MidiEndpointCapabilities OnePortEachWay(uint64_t guid) {
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    caps.deviceToHostStreamCount = 1;
    caps.deviceToHostStreams[0] = {.pcmChannels = 8, .am824Slots = 9, .midiPorts = 1};
    caps.hostToDeviceStreamCount = 1;
    caps.hostToDeviceStreams[0] = {.pcmChannels = 8, .am824Slots = 9, .midiPorts = 1};
    return ASFW::Midi::ProjectMidiCapabilities(caps, guid, 0);
}

/// What the MIDI service holds while CoreMIDI has the device open: its own
/// mapping of the seam, the real-time ingress and the receive drain. Nothing
/// else -- in particular no call into the session.
struct OpenMidiClient {
    OSSharedPtr<IOMemoryDescriptor> memory;
    OSSharedPtr<IOMemoryMap> map;
    MidiTransportBlock* block{nullptr};
    ASFW::Midi::MidiTxIngress ingress;
    ASFW::Midi::MidiRxDrain drain;

    explicit OpenMidiClient(AudioEndpointRuntime& endpoint) {
        IOMemoryDescriptor* raw = nullptr;
        ASFW::Audio::MidiEndpointAttachment attachment{};
        EXPECT_EQ(endpoint.CopyMidiTransportMemory(&raw, attachment), kIOReturnSuccess);
        memory = ASFW::Common::AdoptRetained(raw);
        EXPECT_EQ(ASFW::Common::CreateSharedMapping(memory, map), kIOReturnSuccess);
        block = reinterpret_cast<MidiTransportBlock*>(static_cast<uintptr_t>(map->GetAddress()));
    }

    /// A Note On sent to destination 0.
    MidiTxStatus SendNoteOn() {
        const std::array<uint32_t, 1> ump{0x20903C40u};  // MIDI 1.0 channel voice, group 0
        return ingress.Submit(*block, 0, ump, 1);
    }

    /// The service's receive wake.
    void Drain() {
        drain.ObserveEpoch(*block);
        drain.DrainPort(*block, 0, [](std::span<const uint32_t>) {});
    }
};

struct Rig {
    SessionRig session{kPro24Dsp};
    std::shared_ptr<AudioEndpointRuntime> endpoint;

    explicit Rig(bool withMidi) {
        if (!withMidi) return;
        endpoint = session.runtime.EnsureEndpointRuntime(session.guid);
        EXPECT_EQ(endpoint->ConfigureMidi(OnePortEachWay(session.guid)), kIOReturnSuccess);
    }

    // The coordinator's StartStreaming / StopStreaming, minus its locking.
    IOReturn Start() {
        return StartWithMidiRun(endpoint.get(), session.IsStreaming(),
                                [&] { return session.Start(); });
    }
    IOReturn Stop() {
        return StopWithMidiRun(endpoint.get(), [&] { return session.Stop(); });
    }

    [[nodiscard]] std::vector<std::string> HostCalls() const {
        std::vector<std::string> out;
        for (const auto& line : session.bus.Trace().Lines()) {
            if (line.rfind("H ", 0) == 0) out.push_back(line);
        }
        return out;
    }
    [[nodiscard]] size_t Count(const std::string& call) const {
        size_t n = 0;
        for (const auto& line : HostCalls()) n += line == call ? 1 : 0;
        return n;
    }
};

} // namespace

TEST(MidiAudioRestartTests, AudioStopThenStartReArmsTransportWhileAMidiClientIsOpen) {
    Rig rig(/*withMidi=*/true);
    OpenMidiClient client(*rig.endpoint);

    ASSERT_EQ(rig.Start(), kIOReturnSuccess);
    ASSERT_TRUE(rig.session.IsStreaming());
    const uint64_t firstEpoch = rig.endpoint->MidiEpoch();
    EXPECT_TRUE(client.block->Usable(firstEpoch));
    EXPECT_EQ(client.SendNoteOn(), MidiTxStatus::kOk);
    client.Drain();

    // CoreAudio stops. The MIDI client stays open and keeps sending.
    ASSERT_EQ(rig.Stop(), kIOReturnSuccess);
    EXPECT_FALSE(rig.session.IsStreaming()) << "MIDI must not keep the session streaming";
    EXPECT_FALSE(client.block->Usable(firstEpoch));
    // Refused at the producer, never queued for a later stream.
    EXPECT_EQ(client.SendNoteOn(), MidiTxStatus::kNotActive);
    client.Drain();

    // CoreAudio starts again: the whole start runs, both contexts start again.
    ASSERT_EQ(rig.Start(), kIOReturnSuccess);
    EXPECT_TRUE(rig.session.IsStreaming());
    EXPECT_EQ(rig.Count("H start rx"), 2u);
    EXPECT_EQ(rig.Count("H start tx"), 2u);
    EXPECT_EQ(rig.Count("H stop all"), 1u);

    // MIDI follows the new run rather than holding the old one.
    const uint64_t secondEpoch = rig.endpoint->MidiEpoch();
    EXPECT_GT(secondEpoch, firstEpoch);
    EXPECT_TRUE(client.block->Usable(secondEpoch));
    EXPECT_EQ(client.SendNoteOn(), MidiTxStatus::kOk);

    // A third cycle, to rule out "only the first restart works".
    ASSERT_EQ(rig.Stop(), kIOReturnSuccess);
    ASSERT_EQ(rig.Start(), kIOReturnSuccess);
    EXPECT_EQ(rig.Count("H start tx"), 3u);
}

TEST(MidiAudioRestartTests, HostTransportCallsAreIdenticalWithAndWithoutMidi) {
    Rig plain(/*withMidi=*/false);
    Rig midi(/*withMidi=*/true);
    OpenMidiClient client(*midi.endpoint);

    for (Rig* rig : {&plain, &midi}) {
        ASSERT_EQ(rig->Start(), kIOReturnSuccess);
        ASSERT_EQ(rig->Stop(), kIOReturnSuccess);
        ASSERT_EQ(rig->Start(), kIOReturnSuccess);
        ASSERT_EQ(rig->Stop(), kIOReturnSuccess);
        ASSERT_EQ(rig->Start(), kIOReturnSuccess);
        if (rig == &midi) {
            (void)client.SendNoteOn();
            client.Drain();
        }
    }
    EXPECT_FALSE(plain.HostCalls().empty());
    EXPECT_EQ(plain.HostCalls(), midi.HostCalls());
    // And the whole trace, device bus traffic included.
    EXPECT_EQ(plain.session.bus.Trace().Lines(), midi.session.bus.Trace().Lines());
}

TEST(MidiAudioRestartTests, AFailedStartLeavesMidiOffAndTheNextStartWorks) {
    Rig rig(/*withMidi=*/true);
    OpenMidiClient client(*rig.endpoint);

    rig.session.failures["host.start_receive"] = kIOReturnError;
    EXPECT_NE(rig.Start(), kIOReturnSuccess);
    EXPECT_FALSE(rig.session.IsStreaming());
    EXPECT_FALSE(client.block->Usable(rig.endpoint->MidiEpoch()));
    EXPECT_EQ(client.SendNoteOn(), MidiTxStatus::kNotActive);

    ASSERT_EQ(rig.Start(), kIOReturnSuccess);
    EXPECT_TRUE(client.block->Usable(rig.endpoint->MidiEpoch()));
    EXPECT_EQ(client.SendNoteOn(), MidiTxStatus::kOk);
}

TEST(MidiAudioRestartTests, ASecondStartAgainstRunningStreamsKeepsTheMidiRun) {
    Rig rig(/*withMidi=*/true);
    OpenMidiClient client(*rig.endpoint);

    ASSERT_EQ(rig.Start(), kIOReturnSuccess);
    const uint64_t epoch = rig.endpoint->MidiEpoch();
    EXPECT_EQ(client.SendNoteOn(), MidiTxStatus::kOk);
    const uint32_t queued = client.block->hostToDevice[0].Available();
    ASSERT_GT(queued, 0u);

    // A start while running must not move the epoch: every consumer would drop
    // its backlog mid-stream.
    ASSERT_EQ(rig.Start(), kIOReturnSuccess);
    EXPECT_EQ(rig.endpoint->MidiEpoch(), epoch);
    EXPECT_EQ(client.block->hostToDevice[0].Available(), queued);
}

TEST(MidiAudioRestartTests, AnEndpointWithoutMidiIsUntouched) {
    Rig rig(/*withMidi=*/false);
    auto endpoint = rig.session.runtime.EnsureEndpointRuntime(rig.session.guid);
    ASSERT_EQ(StartWithMidiRun(endpoint.get(), false, [&] { return rig.session.Start(); }),
              kIOReturnSuccess);
    EXPECT_FALSE(endpoint->HasMidi());
    EXPECT_EQ(endpoint->MidiEpoch(), 0u);
}
