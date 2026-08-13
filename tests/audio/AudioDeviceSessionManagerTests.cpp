// SPDX-License-Identifier: Apache-2.0

#include "ASFWDriver/Audio/Devices/AudioDeviceSessionManager.hpp"
#include "ASFWDriver/Discovery/DeviceManager.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Testing/Audio/FakeExistingFamilyDevice.hpp"
#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace ASFW;
using namespace ASFW::Audio::Devices;
namespace Template = ASFW::Testing::AudioTemplate;

class RecordingSink final : public IAudioSessionSink {
public:
    void EndpointReady(std::shared_ptr<const ResolvedAudioEndpointProfile> value,
                       std::shared_ptr<Audio::IDeviceProtocol>) noexcept override {
        profile = std::move(value);
        events.emplace_back("ready");
    }

    void QuiesceEndpoint(AudioEndpointId) noexcept override {
        events.emplace_back("quiesce");
    }

    void InvalidateEndpointBindings(AudioEndpointId) noexcept override {
        events.emplace_back("invalidate");
    }

    void TerminateEndpoint(AudioEndpointId) noexcept override {
        events.emplace_back("terminate");
    }

    std::shared_ptr<const ResolvedAudioEndpointProfile> profile;
    std::vector<std::string> events;
};

class ImmediateAdapter final : public IAudioDeviceAdapter {
public:
    explicit ImmediateAdapter(uint32_t& probeCount) : probeCount_(probeCount) {}

    [[nodiscard]] bool Probe(ProbeCompletion completion) noexcept override {
        if (!completion) return false;
        ++probeCount_;
        Audio::AudioStreamRuntimeCaps caps{};
        caps.hostInputPcmChannels = 2;
        caps.hostOutputPcmChannels = 2;
        caps.deviceToHostAm824Slots = 2;
        caps.hostToDeviceAm824Slots = 2;
        caps.sampleRateHz = 48000;
        caps.deviceToHostStreamCount = 1;
        caps.hostToDeviceStreamCount = 1;
        completion(GenericAvcProbeFacts{.streams = caps,
                                        .supportedRates = {48000},
                                        .hasAudioSubunit = true});
        return true;
    }
    void CancelProbe() noexcept override {}
    [[nodiscard]] IOReturn Shutdown() noexcept override { return kIOReturnSuccess; }
    [[nodiscard]] Audio::IDuplexDeviceControl* DuplexControl() noexcept override {
        return nullptr;
    }
    [[nodiscard]] std::shared_ptr<Audio::IDeviceProtocol> ProtocolHold() noexcept override {
        return nullptr;
    }
    [[nodiscard]] std::vector<std::shared_ptr<IAudioDeviceFacet>> Facets() const override {
        return {};
    }

private:
    uint32_t& probeCount_;
};

class ImmediateProvider final : public IAudioFamilyProvider {
public:
    explicit ImmediateProvider(uint32_t& probeCount) : probeCount_(probeCount) {}
    [[nodiscard]] DeviceProfiles::Audio::AudioFamilyProviderId Id() const noexcept override {
        return DeviceProfiles::Audio::AudioFamilyProviderId::GenericAvc;
    }
    [[nodiscard]] std::unique_ptr<IAudioDeviceAdapter>
    CreateAdapter(const AudioFamilyProviderContext&) noexcept override {
        return std::make_unique<ImmediateAdapter>(probeCount_);
    }
private:
    uint32_t& probeCount_;
};

struct Fixture final {
    Fixture() {
        rom.gen = Discovery::Generation{1};
        rom.nodeId = 2;
        rom.bib.guid = 0x00F1A00000000001ULL;
        rom.bib.maxRec = 8;
        rom.rootDirMinimal = {
            {Discovery::CfgKey::VendorId, Template::kVendorId},
            {Discovery::CfgKey::ModelId, Template::kModelId},
        };
        rom.vendorName = "ASFW Fixture Vendor";
        rom.modelName = "Existing-Family Fixture";
        rom.unitDirectories = {Discovery::UnitDirectory{
            .offsetQuadlets = 0x20,
            .unitSpecId = Template::kAvcSpecifierId,
            .unitSwVersion = 0x010001,
        }};
        record = routes.UpsertFromROM(rom, Discovery::LinkPolicy{});
        device = devices.UpsertDevice(record, rom);
    }

    static AudioDeviceSessionManager::CatalogResolver Catalog() {
        return [](const Discovery::DeviceRecord& device,
                  const Discovery::UnitIdentityEvidence& unit) {
            const std::array definitions{Template::kDefinition};
            return DeviceProfiles::Audio::AudioDeviceCatalog::ResolveWithDefinitions(
                device, unit, definitions,
                std::span<const DeviceProfiles::Audio::AudioSafetyRule>{}, false);
        };
    }

    void RebindAfterReset() {
        routes.InvalidateLiveMappingsForBusReset();
        devices.SuspendAllForBusReset();
        rom.gen = Discovery::Generation{2};
        rom.nodeId = 4;
        record = routes.UpsertFromROM(rom, Discovery::LinkPolicy{});
        device = devices.UpsertDevice(record, rom);
    }

    Discovery::DeviceRegistry routes;
    Discovery::DeviceManager devices;
    Discovery::ConfigROM rom{};
    Discovery::DeviceRecord record{};
    std::shared_ptr<Discovery::FWDevice> device;
};

TEST(AudioDeviceSessionManager, ResolvesAndPublishesOneEndpointPerUnit) {
    Fixture fixture;
    RecordingSink sink;
    auto harness = std::make_shared<Template::ProbeHarness>();
    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      Fixture::Catalog());
    ASSERT_TRUE(manager.RegisterProvider(std::make_unique<Template::Provider>(harness)));

    manager.Start();
    auto sessions = manager.SnapshotAll();
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].state, AudioSessionState::Probing);
    EXPECT_EQ(sessions[0].unitId.unitDirectoryOffset, 0x20U);

    harness->CompleteSuccess();
    sessions = manager.SnapshotAll();
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].state, AudioSessionState::Ready);
    ASSERT_NE(sink.profile, nullptr);
    EXPECT_EQ(sink.profile->deviceInstanceId, fixture.record.instanceId);
    EXPECT_EQ(sink.profile->definitionId,
              Template::kDefinitionId);
    EXPECT_TRUE(std::ranges::any_of(
        sink.profile->facets, [](const FacetDescriptor& facet) {
            return facet.kind == FacetKind::VendorControl &&
                   facet.schemaId == 0x46585452;
        }));
    EXPECT_EQ(sink.events, std::vector<std::string>{"ready"});
    EXPECT_EQ(harness->acceptedProbeCount, 1U);
}

TEST(AudioDeviceSessionManager, ProbeFailureIsTerminalAndPublishesNothing) {
    Fixture fixture;
    RecordingSink sink;
    auto harness = std::make_shared<Template::ProbeHarness>();
    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      Fixture::Catalog());
    ASSERT_TRUE(manager.RegisterProvider(std::make_unique<Template::Provider>(harness)));
    manager.Start();

    harness->Complete(std::unexpected(ProbeError::Transport));
    const auto sessions = manager.SnapshotAll();
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].state, AudioSessionState::Failed);
    EXPECT_TRUE(sink.events.empty());
}

TEST(AudioDeviceSessionManager, StreamingTransitionsRemainSessionOwned) {
    Fixture fixture;
    RecordingSink sink;
    auto harness = std::make_shared<Template::ProbeHarness>();
    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      Fixture::Catalog());
    ASSERT_TRUE(manager.RegisterProvider(std::make_unique<Template::Provider>(harness)));
    manager.Start();
    harness->CompleteSuccess();

    const auto endpointId = manager.SnapshotAll().front().endpointId;
    ASSERT_TRUE(manager.UpdateStreamingState(endpointId, true));
    EXPECT_EQ(manager.Snapshot(endpointId)->state, AudioSessionState::Streaming);
    EXPECT_TRUE(manager.UpdateStreamingState(endpointId, true));

    ASSERT_TRUE(manager.UpdateStreamingState(endpointId, false));
    EXPECT_EQ(manager.Snapshot(endpointId)->state, AudioSessionState::Ready);
    EXPECT_FALSE(manager.UpdateStreamingState(AudioEndpointId{999}, true));
}

TEST(AudioDeviceSessionManager, ResetCancelsStaleProbeAndReprobesCurrentRoute) {
    Fixture fixture;
    RecordingSink sink;
    auto harness = std::make_shared<Template::ProbeHarness>();
    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      Fixture::Catalog());
    ASSERT_TRUE(manager.RegisterProvider(std::make_unique<Template::Provider>(harness)));
    manager.Start();
    ASSERT_EQ(harness->acceptedProbeCount, 1U);

    fixture.RebindAfterReset();
    EXPECT_EQ(harness->cancellationCount, 1U);
    EXPECT_EQ(harness->acceptedProbeCount, 2U);
    EXPECT_EQ(sink.events,
              (std::vector<std::string>{"quiesce", "invalidate", "terminate"}));

    harness->CompleteSuccess(96000);
    const auto sessions = manager.SnapshotAll();
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].state, AudioSessionState::Ready);
    ASSERT_NE(sink.profile, nullptr);
    EXPECT_EQ(sink.profile->currentSampleRateHz, 96000U);
    EXPECT_EQ(sessions[0].probeEpoch, 3U);
    EXPECT_EQ(sink.events,
              (std::vector<std::string>{"quiesce", "invalidate", "terminate",
                                        "ready"}));
}

TEST(AudioDeviceSessionManager, ReadyEndpointIsRepublishedWithFreshProfileAfterReset) {
    Fixture fixture;
    RecordingSink sink;
    auto harness = std::make_shared<Template::ProbeHarness>();
    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      Fixture::Catalog());
    ASSERT_TRUE(manager.RegisterProvider(std::make_unique<Template::Provider>(harness)));
    manager.Start();
    harness->CompleteSuccess(48000);
    ASSERT_EQ(manager.SnapshotAll().size(), 1U);
    const auto endpointId = manager.SnapshotAll().front().endpointId;

    fixture.RebindAfterReset();
    EXPECT_EQ(manager.SnapshotAll().front().endpointId, endpointId);
    EXPECT_EQ(sink.events,
              (std::vector<std::string>{"ready", "quiesce", "invalidate",
                                        "terminate"}));
    EXPECT_EQ(harness->acceptedProbeCount, 2U);

    harness->CompleteSuccess(96000);
    ASSERT_NE(sink.profile, nullptr);
    EXPECT_EQ(sink.profile->endpointId, endpointId);
    EXPECT_EQ(sink.profile->currentSampleRateHz, 96000U);
    EXPECT_EQ(sink.events,
              (std::vector<std::string>{"ready", "quiesce", "invalidate",
                                        "terminate", "ready"}));
}

TEST(AudioDeviceSessionManager, HotUnplugUsesStrictTeardownOrderAndCompletesOnce) {
    Fixture fixture;
    RecordingSink sink;
    auto harness = std::make_shared<Template::ProbeHarness>();
    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      Fixture::Catalog());
    ASSERT_TRUE(manager.RegisterProvider(std::make_unique<Template::Provider>(harness)));
    manager.Start();

    fixture.devices.TerminateDevice(fixture.record.instanceId);
    EXPECT_EQ(harness->cancellationCount, 1U);
    EXPECT_EQ(harness->shutdownCount, 1U);
    EXPECT_EQ(sink.events,
              (std::vector<std::string>{"quiesce", "invalidate", "terminate"}));
    EXPECT_TRUE(manager.SnapshotAll().empty());

    harness->CompleteSuccess();
    EXPECT_EQ(sink.events,
              (std::vector<std::string>{"quiesce", "invalidate", "terminate"}));
}

TEST(AudioDeviceSessionManager, MultipleAudioUnitsProduceDistinctEndpoints) {
    Discovery::DeviceRegistry routes;
    Discovery::DeviceManager devices;
    Discovery::ConfigROM rom{};
    rom.gen = Discovery::Generation{1};
    rom.nodeId = 2;
    rom.bib.guid = 0x00F1A00000000002ULL;
    rom.rootDirMinimal = {
        {Discovery::CfgKey::VendorId, Template::kVendorId},
        {Discovery::CfgKey::ModelId, Template::kModelId},
    };
    rom.unitDirectories = {
        Discovery::UnitDirectory{.offsetQuadlets = 0x20,
                                 .unitSpecId = Template::kAvcSpecifierId,
                                 .unitSwVersion = 0x010001},
        Discovery::UnitDirectory{.offsetQuadlets = 0x40,
                                 .unitSpecId = Template::kAvcSpecifierId,
                                 .unitSwVersion = 0x010001},
    };
    const auto record = routes.UpsertFromROM(rom, {});
    (void)devices.UpsertDevice(record, rom);

    RecordingSink sink;
    uint32_t probeCount = 0;
    AudioDeviceSessionManager manager(devices, routes, sink, Fixture::Catalog());
    ASSERT_TRUE(manager.RegisterProvider(std::make_unique<ImmediateProvider>(probeCount)));
    manager.Start();

    const auto sessions = manager.SnapshotAll();
    ASSERT_EQ(sessions.size(), 2U);
    EXPECT_NE(sessions[0].endpointId, sessions[1].endpointId);
    EXPECT_NE(sessions[0].unitId, sessions[1].unitId);
    EXPECT_EQ(sessions[0].state, AudioSessionState::Ready);
    EXPECT_EQ(sessions[1].state, AudioSessionState::Ready);
    EXPECT_EQ(probeCount, 2U);
}

TEST(AudioDeviceSessionManager, FilteredMAudioCreatesNoAdapterProbeOrNub) {
    Discovery::DeviceRegistry routes;
    Discovery::DeviceManager devices;
    Discovery::ConfigROM rom{};
    rom.gen = Discovery::Generation{1};
    rom.nodeId = 2;
    rom.bib.guid = 0x000D6C0000000001ULL;
    rom.rootDirMinimal = {
        {Discovery::CfgKey::VendorId, 0x000D6C},
        {Discovery::CfgKey::ModelId, 0x00010071},
    };
    rom.unitDirectories = {Discovery::UnitDirectory{
        .offsetQuadlets = 0x20,
        .unitSpecId = 0x00A02D,
        .unitSwVersion = 0x010001,
    }};
    const auto record = routes.UpsertFromROM(rom, {});
    (void)devices.UpsertDevice(record, rom);

    RecordingSink sink;
    auto harness = std::make_shared<Template::ProbeHarness>();
    AudioDeviceSessionManager manager(devices, routes, sink);
    ASSERT_TRUE(manager.RegisterProvider(std::make_unique<Template::Provider>(harness)));
    manager.Start();

    // The 1814 now resolves as Supported with AudioFamilyProviderId::BeBoB, so
    // the session manager does create a session for it. This fixture registers
    // only a GenericAvc provider, so no adapter is built and the session stops
    // at UnsupportedFamily — which is what makes this test still meaningful:
    // it pins that the session manager itself never probes and never publishes,
    // independent of whether a provider happens to be present.
    //
    // The real BeBoB path is exercised by the family provider, not here; this
    // fixture deliberately has no way to instantiate MAudioSpecialProtocol.
    const auto sessions = manager.SnapshotAll();
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions.front().state, AudioSessionState::Quarantined);
    EXPECT_EQ(sessions.front().quarantineReason,
              Discovery::QuarantineReason::UnsupportedFamily);

    // Unchanged and the point of the test: no probe, no nub.
    EXPECT_EQ(harness->acceptedProbeCount, 0U);
    EXPECT_TRUE(sink.events.empty());
    EXPECT_EQ(sink.profile, nullptr);
}

TEST(AudioDeviceTemplate, SafetyPrecedesProbeAndTypedFactsNarrowEquivalence) {
    Fixture fixture;
    const auto& unit = fixture.record.identity.units.front();
    const auto common = DeviceProfiles::Audio::AudioDeviceCatalog::ResolveWithDefinitions(
        fixture.record, unit, Template::kEquivalentDefinitions,
        std::span<const DeviceProfiles::Audio::AudioSafetyRule>{}, false);
    ASSERT_TRUE(common.has_value());
    EXPECT_EQ(common->candidates.size(), 2U);
    EXPECT_EQ(common->equivalenceClassId, Template::kEquivalenceClassId);

    GenericAvcProbeFacts facts{};
    facts.streams.hostInputPcmChannels = 4;
    const auto narrowed = ResolvedProfileBuilder::ApplyProbeEvidence(*common, facts);
    ASSERT_TRUE(narrowed.has_value());
    EXPECT_EQ(narrowed->candidates,
              (std::vector<DeviceProfiles::Audio::DeviceDefinitionId>{
                  Template::kEquivalentDefinitionBId}));
    EXPECT_EQ(narrowed->exactVariantId, 4U);

    Discovery::DeviceRegistry unsafeRoutes;
    auto unsafeRom = fixture.rom;
    unsafeRom.bib.guid = 0x00F1A000000000FEULL;
    unsafeRom.rootDirMinimal[1].value = Template::kUnsafeModelId;
    const auto unsafe = unsafeRoutes.UpsertFromROM(unsafeRom, {});
    const auto denied = DeviceProfiles::Audio::AudioDeviceCatalog::ResolveWithDefinitions(
        unsafe, unsafe.identity.units.front(), Template::kEquivalentDefinitions,
        Template::kSafetyRules, true);
    ASSERT_FALSE(denied.has_value());
    EXPECT_EQ(denied.error(),
              DeviceProfiles::Audio::CatalogResolutionError::HazardousIdentity);
}

TEST(ResolvedProfileBuilder, SafeProbeGeometryNarrowsEquivalenceOrUsesCommonSubset) {
    // Capability-first disambiguation mirrors the safe Alesis iO14/iO26 stream
    // geometry check in
    // references/alsa-userspace-control-protocols-impl/runtime/dice/src/io_fw_model.rs:920-949.
    using namespace DeviceProfiles::Audio;
    StaticAudioEndpointPlan plan{};
    plan.unit = Discovery::UnitInstanceId{Discovery::DeviceInstanceId{7}, 0x20};
    plan.family = AudioFamilyProviderId::DICE;
    plan.probePolicy = ProbePolicyId::DiceTcat;
    plan.equivalenceClassId = 77;
    plan.profileBuilder = ProfileBuilderId::AlesisMultiMix;
    plan.commonEquivalenceProfileBuilder = ProfileBuilderId::AlesisMultiMix;
    plan.candidates = {static_cast<DeviceDefinitionId>(5001),
                       static_cast<DeviceDefinitionId>(5002)};
    plan.candidatePlans = {
        CandidateEndpointPlan{static_cast<DeviceDefinitionId>(5001), 14,
                              ProfileBuilderId::FocusriteSPro24Dsp,
                              SafeProbeConstraint{.hostInputPcmChannels = 14},
                              "Alesis", "iO14"},
        CandidateEndpointPlan{static_cast<DeviceDefinitionId>(5002), 26,
                              ProfileBuilderId::FocusriteSPro24Dsp,
                              SafeProbeConstraint{.hostInputPcmChannels = 26},
                              "Alesis", "iO26"},
    };

    DiceProbeFacts exactFacts{};
    exactFacts.streams.hostInputPcmChannels = 26;
    auto exact = ResolvedProfileBuilder::ApplyProbeEvidence(plan, exactFacts);
    ASSERT_TRUE(exact.has_value());
    ASSERT_EQ(exact->candidates.size(), 1U);
    EXPECT_EQ(exact->exactVariantId, 26U);
    EXPECT_EQ(exact->modelName, "iO26");

    for (auto& candidate : plan.candidatePlans) candidate.probeConstraint = {};
    DiceProbeFacts commonFacts{};
    commonFacts.streams = {
        .hostInputPcmChannels = 14,
        .hostOutputPcmChannels = 6,
        .deviceToHostAm824Slots = 16,
        .hostToDeviceAm824Slots = 8,
        .sampleRateHz = 48000,
        .deviceToHostStreamCount = 1,
        .hostToDeviceStreamCount = 1,
    };
    commonFacts.supportedRates = {48000};
    auto common = ResolvedProfileBuilder::ApplyProbeEvidence(plan, commonFacts);
    ASSERT_TRUE(common.has_value());
    EXPECT_FALSE(common->exactVariantId.has_value());
    EXPECT_EQ(common->profileBuilder, ProfileBuilderId::AlesisMultiMix);

    Discovery::DeviceRecord record{};
    record.instanceId = Discovery::DeviceInstanceId{7};
    record.identity.observedGuid = 0x0005950000000001ULL;
    auto profile = ResolvedProfileBuilder::Build(
        ProfileBuildContext{AudioEndpointId{9}, record, *common, commonFacts});
    ASSERT_TRUE(profile.has_value());
    EXPECT_FALSE(std::ranges::any_of(profile->facets, [](const FacetDescriptor& facet) {
        return facet.kind == FacetKind::Mixer;
    }));
    EXPECT_FALSE(profile->exactVariantId.has_value());
    EXPECT_EQ(profile->definitionId, DeviceDefinitionId::Unknown);
}

// --- Bootloader preparation wiring -------------------------------------------
//
// The cue machine and its transport are covered in BeBoBBootloaderCueTests.
// What is asserted here is only the wiring: that a plan carrying a cue policy
// reaches the machine at all, and that it does so without an adapter, a probe or
// a published endpoint. The persona is RecognizedUnsupported, so before this
// wiring existed the session manager skipped it entirely and the cue was inert.

namespace Boot = ASFW::Audio::Families::BeBoB::Bootloader;

/// Answers the bootloader register window and records what reached the bus.
class BootloaderBus final : public ASFW::Async::IFireWireBusOps {
public:
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> writes;
    std::vector<uint32_t> reads;
    std::vector<uint8_t> infoBlock =
        std::vector<uint8_t>(Boot::kInfoBlockBytes, 0);

    ASFW::Async::AsyncHandle ReadBlock(
        ASFW::FW::Generation, ASFW::FW::NodeId, ASFW::Async::FWAddress address,
        uint32_t, ASFW::FW::FwSpeed,
        ASFW::Async::InterfaceCompletionCallback callback) override {
        reads.push_back(address.addressLo);
        callback(ASFW::Async::AsyncStatus::kSuccess, infoBlock);
        return ASFW::Async::AsyncHandle{1};
    }

    ASFW::Async::AsyncHandle WriteBlock(
        ASFW::FW::Generation, ASFW::FW::NodeId, ASFW::Async::FWAddress address,
        std::span<const uint8_t> data, ASFW::FW::FwSpeed,
        ASFW::Async::InterfaceCompletionCallback callback) override {
        writes.emplace_back(address.addressLo,
                            std::vector<uint8_t>(data.begin(), data.end()));
        callback(ASFW::Async::AsyncStatus::kSuccess, std::span<const uint8_t>{});
        return ASFW::Async::AsyncHandle{1};
    }

    ASFW::Async::AsyncHandle Lock(
        ASFW::FW::Generation, ASFW::FW::NodeId, ASFW::Async::FWAddress,
        ASFW::FW::LockOp, std::span<const uint8_t>, uint32_t, ASFW::FW::FwSpeed,
        ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(ASFW::Async::AsyncStatus::kSuccess, std::span<const uint8_t>{});
        return ASFW::Async::AsyncHandle{1};
    }

    bool Cancel(ASFW::Async::AsyncHandle) override { return true; }
};

/// A plan shaped like the 1814's bootloader persona: no family, no probe, not an
/// audio endpoint, carrying only the cue policy.
[[nodiscard]] AudioDeviceSessionManager::CatalogResolver BootloaderCatalog() {
    return [](const Discovery::DeviceRecord& device,
              const Discovery::UnitIdentityEvidence& unit)
               -> std::expected<DeviceProfiles::Audio::StaticAudioEndpointPlan,
                                DeviceProfiles::Audio::CatalogResolutionError> {
        DeviceProfiles::Audio::StaticAudioEndpointPlan plan{};
        plan.unit = Discovery::UnitInstanceId{device.instanceId,
                                              unit.unitDirectoryOffset};
        plan.family = DeviceProfiles::Audio::AudioFamilyProviderId::None;
        plan.probePolicy = DeviceProfiles::Audio::ProbePolicyId::NoAutomaticTraffic;
        plan.support =
            DeviceProfiles::Audio::SupportDisposition::RecognizedUnsupported;
        plan.bootloaderCue =
            DeviceProfiles::Audio::BootloaderCuePolicy::BeBoBStartFirmware;
        return plan;
    };
}

TEST(AudioDeviceSessionManagerBootloader, ActiveBootloaderIsCuedExactlyOnce) {
    Fixture fixture;
    RecordingSink sink;
    BootloaderBus bus;
    // Non-zero bootloader "version" is the state flag meaning the bootloader is
    // the running image, so the cue is warranted.
    bus.infoBlock[Boot::kInfoOffsetBootloaderVersion] = 1;
    bus.infoBlock[Boot::kInfoOffsetProtocolVersion] = 1;

    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      BootloaderCatalog(), &bus);
    manager.Start();

    ASSERT_EQ(bus.writes.size(), 1U);
    // Exactly one info read decides it. The normal cue path deliberately does
    // not poll the response register in this generation.
    EXPECT_EQ(bus.reads.size(), 1U);
    EXPECT_EQ(bus.reads[0], Boot::kInfoAddressLo);
    EXPECT_EQ(bus.writes[0].first, Boot::kRequestAddressLo);
    EXPECT_TRUE(Boot::IsPermittedBootloaderWrite(
        Boot::kBootloaderAddressHi, bus.writes[0].first, bus.writes[0].second));
    // Preparation is terminal and never publishes an endpoint.
    EXPECT_TRUE(sink.events.empty());
    EXPECT_EQ(sink.profile, nullptr);
}

TEST(AudioDeviceSessionManagerBootloader,
     ResetRetiresAwaitingReenumerationBeforeTheCurrentPersonaIsResolved) {
    // A real 1814 returns as 0x00010071. This fixture intentionally keeps the
    // bootloader catalog result after reset so that a second preparation proves
    // the old endpoint-by-unit mapping was removed; the production catalog then
    // resolves the current operational model instead of sending this second cue.
    Fixture fixture;
    RecordingSink sink;
    BootloaderBus bus;
    bus.infoBlock[Boot::kInfoOffsetBootloaderVersion] = 1;

    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      BootloaderCatalog(), &bus);
    manager.Start();
    ASSERT_EQ(bus.writes.size(), 1U);
    ASSERT_EQ(bus.reads, std::vector<uint32_t>{Boot::kInfoAddressLo});

    fixture.RebindAfterReset();
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.reads,
              (std::vector<uint32_t>{Boot::kInfoAddressLo, Boot::kInfoAddressLo}));
    ASSERT_EQ(manager.SnapshotAll().size(), 1U);
    EXPECT_EQ(manager.SnapshotAll()[0].state, AudioSessionState::Preparing);
}

TEST(AudioDeviceSessionManagerBootloader, InactiveBootloaderPutsNoWriteOnTheBus) {
    Fixture fixture;
    RecordingSink sink;
    BootloaderBus bus;
    // Zero means the device is already running application firmware.
    bus.infoBlock[Boot::kInfoOffsetBootloaderVersion] = 0;

    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      BootloaderCatalog(), &bus);
    manager.Start();

    EXPECT_FALSE(bus.reads.empty());
    EXPECT_TRUE(bus.writes.empty());
    EXPECT_TRUE(sink.events.empty());
}

TEST(AudioDeviceSessionManagerBootloader, WithoutTransportNothingIsPrepared) {
    Fixture fixture;
    RecordingSink sink;
    // No bus: the session is recorded and left alone rather than driven down a
    // second, degraded firmware path.
    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      BootloaderCatalog());
    manager.Start();

    const auto sessions = manager.SnapshotAll();
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions[0].state, AudioSessionState::Preparing);
    EXPECT_TRUE(sink.events.empty());
}

TEST(AudioDeviceSessionManagerBootloader, CuePolicyNeverCreatesAnAdapterOrProbes) {
    Fixture fixture;
    RecordingSink sink;
    BootloaderBus bus;
    bus.infoBlock[Boot::kInfoOffsetBootloaderVersion] = 1;

    auto harness = std::make_shared<Template::ProbeHarness>();
    AudioDeviceSessionManager manager(fixture.devices, fixture.routes, sink,
                                      BootloaderCatalog(), &bus);
    ASSERT_TRUE(manager.RegisterProvider(std::make_unique<Template::Provider>(harness)));
    manager.Start();

    // Assert the session was actually taken through preparation first. Without
    // this the probe-count check below passes vacuously: an unwired build skips
    // a RecognizedUnsupported plan entirely, so it also probes nothing, and the
    // test would report success for the opposite reason.
    ASSERT_FALSE(bus.reads.empty());
    ASSERT_EQ(bus.writes.size(), 1U);

    // A registered provider must not be consulted for a preparation session: the
    // hazard this whole subsystem exists to avoid is FCP reaching the device.
    EXPECT_EQ(harness->acceptedProbeCount, 0U);
}

} // namespace
