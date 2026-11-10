// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xWiringTests.cpp - the Digi 002/003 family as the rest of the stack
// sees it: backend and probe selection, the AV/C bound, catalog validation of a
// supported row, the protocol object, the published nub description and the
// stream profile (issue #249). The catalog rows themselves are pinned in
// AudioDeviceCatalogTests and CatalogMatcherAgreementTests; these tests build
// their plans directly.

#include "DICEDuplexTestSupport.hpp"
#include "Audio/Protocols/Digi00x/Digi00xDeviceProtocol.hpp"
#include "Audio/Host/Digi00xFamilyAdapter.hpp"
#include "Audio/DriverKit/Config/AudioProfileRegistry.hpp"
#include "Audio/DriverKit/Config/Digi00x/Digi00xProfile.hpp"
#include "Audio/Protocols/DeviceProtocolChoice.hpp"
#include "Audio/Protocols/SelectProbeBootstrap.hpp"
#include "DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <array>

namespace {
using namespace ASFW::Testing::DICE;
using namespace ASFW::DeviceProfiles::Audio;
using ::ASFW::Async::InterfaceCompletionCallback;
namespace Audio = ASFW::Audio;

[[nodiscard]] StaticAudioEndpointPlan SupportedPlan() {
    StaticAudioEndpointPlan plan{};
    plan.family = AudioFamilyProviderId::Digi00xRegister;
    plan.probePolicy = ProbePolicyId::Digi00xRegister;
    plan.support = SupportDisposition::Supported;
    plan.profileBuilder = ProfileBuilderId::Digi00x;
    plan.protocolImplementation = ProtocolImplementationId::Digi00x;
    plan.modelName = kDigi003ConsoleModelName;
    plan.vendorName = kDigidesignVendorName;
    return plan;
}

TEST(Digi00xWiring, ASupportedPlanSelectsTheRegisterBackendAndProbe) {
    const auto plan = SupportedPlan();
    EXPECT_EQ(Audio::ChooseAudioBackend(plan), Audio::AudioBackendKind::Digi00xRegister);
    EXPECT_EQ(Audio::SelectProbeBootstrap(plan), Audio::ProbeBootstrap::Digi00xRegister);
    // Not an AV/C device: no FCP frame may be sent.
    EXPECT_EQ(AudioDeviceCatalog::CommandFilterFor(plan), ASFW::Discovery::AvcCommandFilterId::BlockAll);
}

TEST(Digi00xWiring, TheProbePolicyBelongsToTheFamilyOnly) {
    EXPECT_EQ(Audio::SelectProbeBootstrap(AudioFamilyProviderId::Digi00xRegister, ProbePolicyId::RmeRegister),
              Audio::ProbeBootstrap::Unsupported);
    EXPECT_EQ(Audio::SelectProbeBootstrap(AudioFamilyProviderId::RmeRegister, ProbePolicyId::Digi00xRegister),
              Audio::ProbeBootstrap::Unsupported);
}

TEST(Digi00xWiring, ASupportedRowValidates) {
    // The shape the catalog rows take once the family streams.
    auto row = AudioDeviceCatalog::Definitions()[0];
    for (const auto& definition : AudioDeviceCatalog::Definitions()) {
        if (definition.id == DeviceDefinitionId::Digi00xConsole) row = definition;
    }
    ASSERT_EQ(row.id, DeviceDefinitionId::Digi00xConsole);
    row.family = AudioFamilyProviderId::Digi00xRegister;
    row.probePolicy = ProbePolicyId::Digi00xRegister;
    row.profileBuilder = ProfileBuilderId::Digi00x;
    row.protocolImplementation = ProtocolImplementationId::Digi00x;
    row.support = SupportDisposition::Supported;
    const std::array rows{row};
    const auto issues = AudioDeviceCatalog::ValidateDefinitions(rows);
    for (const auto& issue : issues) ADD_FAILURE() << issue.reason;

    auto wrongProtocol = row;
    wrongProtocol.protocolImplementation = ProtocolImplementationId::RmeFireface;
    EXPECT_FALSE(AudioDeviceCatalog::ValidateDefinitions(std::array{wrongProtocol}).empty());
}

class SilentBus final : public IFireWireBus {
public:
    AsyncHandle ReadBlock(Generation, NodeId, FWAddress, uint32_t, FwSpeed,
                          InterfaceCompletionCallback callback) override {
        ++traffic;
        callback(AsyncStatus::kHardwareError, {});
        return {};
    }
    AsyncHandle WriteBlock(Generation, NodeId, FWAddress, std::span<const uint8_t>, FwSpeed,
                           InterfaceCompletionCallback callback) override {
        ++traffic;
        callback(AsyncStatus::kHardwareError, {});
        return {};
    }
    AsyncHandle Lock(Generation, NodeId, FWAddress, LockOp, std::span<const uint8_t>, uint32_t, FwSpeed,
                     InterfaceCompletionCallback callback) override {
        ++traffic;
        callback(AsyncStatus::kHardwareError, {});
        return {};
    }
    bool Cancel(AsyncHandle) override { return false; }
    FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S400; }
    uint32_t HopCount(NodeId, NodeId) const override { return 1; }
    Generation GetGeneration() const override { return Generation{1}; }
    NodeId GetLocalNodeID() const override { return NodeId{0}; }
    int traffic{0};
};

TEST(Digi00xWiring, TheProtocolPublishesGeometryWithoutBusTraffic) {
    SilentBus bus;
    RouteState route;
    Audio::Digi00x::Digi00xDeviceProtocol protocol{bus, bus, route.registry, route.route, "Digi 003"};
    EXPECT_EQ(protocol.Initialize(), kIOReturnSuccess);
    EXPECT_STREQ(protocol.GetName(), "Digi 003");
    ASSERT_NE(protocol.AsFamilyDriver(), nullptr);
    Audio::AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 18U);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, 19U);
    EXPECT_EQ(bus.traffic, 0);
}

TEST(Digi00xWiring, TheNubDescriptionIsNonBlockingWithPcmAfterTheMidiSlot) {
    ASFW::Discovery::DeviceRecord record{};
    record.guid = 0x00a07e0000aa0000ULL;
    record.vendorId = kAvidVendorId;
    const auto config = Audio::Host::Digi00xFamilyAdapter::BuildNubConfig(record, ProfileBuilderId::Digi00x,
                                                                          "Digi 003");
    EXPECT_EQ(config.deviceName, "Digi 003");
    EXPECT_EQ(config.sampleRates, (std::vector<uint32_t>{44100U, 48000U}));
    EXPECT_EQ(config.currentSampleRate, 48000U);
    EXPECT_EQ(config.streamMode, ASFW::Audio::Model::StreamMode::kNonBlocking);
    EXPECT_EQ(config.inputChannelCount, 18U);
    EXPECT_EQ(config.outputChannelCount, 18U);
    for (const auto* streams : {&config.captureStreams, &config.playbackStreams}) {
        ASSERT_EQ(streams->size(), 1U);
        const auto& s = streams->front();
        EXPECT_EQ(s.pcmChannels, 18U);
        EXPECT_EQ(s.am824Slots, 19U);
        EXPECT_EQ(s.channelOffset, 0U);
        ASSERT_FALSE(s.pcmSlotMap.IsIdentity());
        EXPECT_TRUE(s.pcmSlotMap.FitsWithin(18U, 19U));
        for (uint32_t channel = 0; channel < 18U; ++channel)
            EXPECT_EQ(s.pcmSlotMap.SlotFor(channel), channel + 1U);
    }
}

TEST(Digi00xWiring, TheAdapterRefusesOtherFamilies) {
    Audio::Host::Digi00xFamilyAdapter adapter;
    EXPECT_FALSE(adapter.ActsOn(Audio::DuplexRestartReason::kRecoverAfterCycleInconsistent));
    EXPECT_STREQ(adapter.Name(), "Digi00x");
}

TEST(Digi00xWiring, TheProfileIsAm824NonBlockingAndMatchesTheNub) {
    const auto* profile = ASFW::Isoch::Audio::AudioProfileRegistry::ProfileForBuilderId(
        static_cast<uint32_t>(ProfileBuilderId::Digi00x));
    ASSERT_NE(profile, nullptr);
    const auto* stream = dynamic_cast<const ASFW::Isoch::Audio::IAudioStreamProfile*>(profile);
    ASSERT_NE(stream, nullptr);
    EXPECT_EQ(stream->SupportedSampleRates(), (std::vector<uint32_t>{44100U, 48000U}));
    EXPECT_EQ(stream->TransmitClockSource(), ASFW::Isoch::Audio::TxClockSource::kRxReplayAfterBootstrap);

    for (const bool transmit : {true, false}) {
        ASFW::Isoch::Audio::AudioStreamConfig config{};
        ASSERT_TRUE(transmit ? stream->BuildDefaultTxStreamConfig(config)
                             : stream->BuildDefaultRxStreamConfig(config));
        EXPECT_EQ(config.streamMode, ASFW::Encoding::StreamMode::kNonBlocking);
        EXPECT_EQ(config.pcmChannels, 18U);
        EXPECT_EQ(config.dbs, 19U);
        EXPECT_EQ(config.midiSlots, 0U);
        EXPECT_EQ(config.fmt, 0x10U);
        EXPECT_EQ(config.packetFraming, ASFW::Encoding::AudioPacketFraming::kCip);
        ASSERT_TRUE(config.hasPcmSlotMap);
        EXPECT_EQ(config.pcmSlotMap, ASFW::Encoding::Digi00x::PcmAfterMidiSlotMap(18U));
    }
    const auto policy = stream->TxStreamPolicy();
    EXPECT_EQ(policy.defaultNonAudioSlotWord, 0x80000000U);
    EXPECT_TRUE(policy.initializeNonAudioSlots);
    EXPECT_EQ(policy.playbackChannelMap, ASFW::Encoding::Digi00x::PcmAfterMidiSlotMap(18U));
}

} // namespace
