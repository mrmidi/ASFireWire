// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MidiNubPublisherTests.cpp - The MIDI nub is configured explicitly, in order.
//
// IOService::Create runs the nub's Start before the publisher can set anything.
// On the midi branch the nub read its endpoint id from its own properties in
// Start, cached 0, and the MIDI service then started against endpoint 0 and
// crashed the dext. The publisher must hand the id over with SetEndpointId
// after Create, set the properties, and only then Publish (register for
// matching), the way AudioNubPublisher hands over SetGuid.

#include <gtest/gtest.h>

#include <net.asfw.driver/ASFWMidiNub.h>

#include "Midi/Core/MidiNubProperties.hpp"
#include "Midi/Core/MidiNubPublisher.hpp"

#include <DriverKit/OSNumber.h>

#include <string>
#include <vector>

namespace {

using ASFW::Midi::MidiEndpointCapabilities;
using ASFW::Midi::MidiNubPublisher;

class NubProvider final : public IOService {
public:
    NubProvider() : nub(new ASFWMidiNub) {}
    ~NubProvider() override { nub->release(); }
    kern_return_t Create(IOService*, const char* key, IOService** out) override {
        createdWith = key ? key : "";
        ++creates;
        nub->retain();
        *out = nub;
        return kIOReturnSuccess;
    }
    ASFWMidiNub* nub;
    std::string createdWith;
    int creates{0};
};

MidiEndpointCapabilities OnePortEachWay(uint64_t guid) {
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    caps.deviceToHostStreamCount = 1;
    caps.deviceToHostStreams[0] = {.pcmChannels = 8, .am824Slots = 9, .midiPorts = 1};
    caps.hostToDeviceStreamCount = 1;
    caps.hostToDeviceStreams[0] = {.pcmChannels = 8, .am824Slots = 9, .midiPorts = 1};
    return ASFW::Midi::ProjectMidiCapabilities(caps, guid, 0);
}

uint64_t NumberIn(OSDictionary* dict, const char* key) {
    auto* number = OSDynamicCast(OSNumber, dict->getObject(key));
    return number ? number->unsigned64BitValue() : ~0ULL;
}

constexpr uint64_t kGuid = 0x00130e0400000001ULL;

} // namespace

TEST(MidiNubPublisherTests, EndpointIdIsSetAfterCreateAndBeforeRegistration) {
    NubProvider provider;
    MidiNubPublisher publisher(&provider);
    ASSERT_TRUE(publisher.EnsureNub(OnePortEachWay(kGuid), kGuid, "Dev", "Model", "Maker"));

    EXPECT_EQ(provider.createdWith, "ASFWMidiNubProperties");
    const std::vector<std::string> expected{"SetEndpointId", "CopyProperties", "SetProperties",
                                            "Publish"};
    EXPECT_EQ(provider.nub->calls, expected);
    EXPECT_EQ(provider.nub->endpointIdAtPublish, kGuid);
    EXPECT_TRUE(provider.nub->registered);
    EXPECT_TRUE(publisher.HasNub(kGuid));

    ASSERT_NE(provider.nub->published, nullptr);
    namespace Keys = ASFW::Midi::NubKeys;
    EXPECT_EQ(NumberIn(provider.nub->published, Keys::kEndpointId), kGuid);
    EXPECT_EQ(NumberIn(provider.nub->published, Keys::kSourcePorts), 1U);
    EXPECT_EQ(NumberIn(provider.nub->published, Keys::kDestinationPorts), 1U);
    EXPECT_EQ(NumberIn(provider.nub->published, Keys::kDestinationSlotIndex), 8U);
}

TEST(MidiNubPublisherTests, AnEndpointIdOfZeroIsRefusedBeforeAnythingIsCreated) {
    NubProvider provider;
    MidiNubPublisher publisher(&provider);
    EXPECT_FALSE(publisher.EnsureNub(OnePortEachWay(kGuid), 0, "Dev", "Model", "Maker"));
    EXPECT_EQ(provider.creates, 0);
}

TEST(MidiNubPublisherTests, NoUsableMidiPublishesNothing) {
    NubProvider provider;
    MidiNubPublisher publisher(&provider);
    EXPECT_TRUE(publisher.EnsureNub(MidiEndpointCapabilities{.guid = kGuid}, kGuid, "Dev", "M",
                                    "Maker"));
    EXPECT_EQ(provider.creates, 0);
    EXPECT_FALSE(publisher.HasNub(kGuid));
}

TEST(MidiNubPublisherTests, OneNubPerEndpointAndTerminateRetiresIt) {
    NubProvider provider;
    MidiNubPublisher publisher(&provider);
    ASSERT_TRUE(publisher.EnsureNub(OnePortEachWay(kGuid), kGuid, "Dev", "Model", "Maker"));
    ASSERT_TRUE(publisher.EnsureNub(OnePortEachWay(kGuid), kGuid, "Dev", "Model", "Maker"));
    EXPECT_EQ(provider.creates, 1);

    publisher.MidiReceived(kGuid);
    EXPECT_EQ(provider.nub->notifications, 1U);

    publisher.TerminateNub(kGuid);
    EXPECT_EQ(provider.nub->terminations, 1U);
    EXPECT_FALSE(publisher.HasNub(kGuid));
    // A wake for an endpoint that was unpublished meanwhile does nothing.
    publisher.MidiReceived(kGuid);
    EXPECT_EQ(provider.nub->notifications, 1U);
}

// A full stop of ASFWDriver: the kernel has already terminated the nubs, and
// Terminate() on one aborts the dext (adapter unplug, 2026-10-28). Release
// forgets them without terminating; a suspend still terminates (TerminateAll).
TEST(MidiNubPublisherTests, ReleaseAllForgetsWithoutTerminating) {
    NubProvider provider;
    MidiNubPublisher publisher(&provider);
    ASSERT_TRUE(publisher.EnsureNub(OnePortEachWay(kGuid), kGuid, "Dev", "Model", "Maker"));

    publisher.ReleaseAll();
    EXPECT_EQ(provider.nub->terminations, 0U);
    EXPECT_FALSE(publisher.HasNub(kGuid));
    publisher.MidiReceived(kGuid);
    EXPECT_EQ(provider.nub->notifications, 0U);
}
