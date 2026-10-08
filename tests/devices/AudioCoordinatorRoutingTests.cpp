// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioCoordinatorRoutingTests.cpp
// Which family serves a device event: the RME Fireface (E2) and the AV/C
// family (E4) go to the audio device host (documentation/AUDIO_DEVICE_HOST.md
// §6), DICE stays on its backend.
// The coordinator is built for real; only the device manager is a fake, and
// the assertions read the host's per-outcome counters.

#include <gtest/gtest.h>

#include "Audio/Core/AudioCoordinator.hpp"
#include "Audio/Core/AudioEndpointRuntime.hpp"
#include "Audio/Core/AudioRuntimeRegistry.hpp"
#include "Audio/Host/AudioDeviceHost.hpp"
#include "Audio/Protocols/DICE/Core/DiceNotificationRouter.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Discovery/FWDevice.hpp"
#include "Discovery/IDeviceManager.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Isoch/IsochService.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace {

using ASFW::Audio::AudioCoordinator;
using ASFW::Audio::AudioRuntimeRegistry;
using ASFW::Audio::Host::HostEvent;
using ASFW::Audio::Host::HostOutcome;
using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::FWDevice;
using ASFW::Discovery::IDeviceObserver;
using ASFW::Discovery::RomEntry;
using ASFW::FW::Generation;
using ASFW::Audio::DICE::DiceNotificationRouter;
using ASFW::Driver::HardwareInterface;
using ASFW::Driver::IsochService;

// Records observer registration; everything the coordinator asks of the bus
// manager that these tests do not exercise is a no-op.
class FakeDeviceManager final : public ASFW::Discovery::IDeviceManager {
public:
    std::shared_ptr<FWDevice> GetDeviceByGUID(uint64_t) const override { return nullptr; }
    std::shared_ptr<FWDevice> GetDeviceByNode(Generation, uint8_t) const override { return nullptr; }
    std::vector<std::shared_ptr<FWDevice>> GetDevicesByGeneration(Generation) const override { return {}; }
    std::vector<std::shared_ptr<FWDevice>> GetAllDevices() const override { return {}; }
    std::vector<std::shared_ptr<FWDevice>> GetReadyDevices() const override { return {}; }
    void RegisterDeviceObserver(IDeviceObserver* observer) override {
        ++registerCalls;
        observers.insert(observer);
    }
    void UnregisterDeviceObserver(IDeviceObserver* observer) override {
        ++unregisterCalls;
        observers.erase(observer);
    }
    std::shared_ptr<FWDevice> UpsertDevice(const DeviceRecord&, const ConfigROM&) override { return nullptr; }
    void MarkDeviceLost(uint64_t) override {}
    void TerminateDevice(uint64_t) override {}

    std::vector<std::shared_ptr<ASFW::Discovery::FWUnit>> FindUnitsBySpec(
        uint32_t, std::optional<uint32_t>) const override {
        return {};
    }
    std::vector<std::shared_ptr<ASFW::Discovery::FWUnit>> GetAllUnits() const override { return {}; }
    std::vector<std::shared_ptr<ASFW::Discovery::FWUnit>> GetReadyUnits() const override { return {}; }
    void RegisterUnitObserver(ASFW::Discovery::IUnitObserver*) override {}
    void UnregisterUnitObserver(ASFW::Discovery::IUnitObserver*) override {}
    CallbackHandle RegisterUnitCallback(uint32_t, std::optional<uint32_t>, UnitCallback) override { return 0; }
    void UnregisterCallback(CallbackHandle) override {}

    std::set<IDeviceObserver*> observers{};
    int registerCalls{0};
    int unregisterCalls{0};
};

constexpr uint64_t kRmeGuid = 0x000a3501000000a1ULL;
constexpr uint64_t kDiceGuid = 0x00130e0400000001ULL;
constexpr uint64_t kAvcGuid = 0x000aac0300b1d1f7ULL;
constexpr uint32_t kTa1394Specifier = 0x00A02D;
constexpr uint32_t kAvcVersion = 0x010001;

struct SeededDevice {
    ConfigROM rom{};
};

SeededDevice MakeRom(uint64_t guid, uint32_t vendorId, uint32_t modelId, uint32_t unitSpecId,
                     uint32_t unitSwVersion) {
    SeededDevice seed{};
    seed.rom.gen = Generation{1};
    seed.rom.firstSeen = Generation{1};
    seed.rom.lastValidated = Generation{1};
    seed.rom.nodeId = 2;
    seed.rom.bib.guid = guid;
    seed.rom.bib.maxRec = 8;
    seed.rom.rootDirMinimal = {
        RomEntry{.key = CfgKey::VendorId, .value = vendorId},
        RomEntry{.key = CfgKey::ModelId, .value = modelId},
    };
    ASFW::Discovery::UnitDirectory unit{};
    unit.offsetQuadlets = 5;
    unit.unitSpecId = unitSpecId;
    unit.unitSwVersion = unitSwVersion;
    seed.rom.unitDirectories.push_back(unit);
    return seed;
}

SeededDevice MakeRmeRom() {
    return MakeRom(kRmeGuid, ASFW::DeviceProfiles::Audio::kRmeVendorId,
                   ASFW::DeviceProfiles::Audio::kRmeRootModelId,
                   ASFW::DeviceProfiles::Audio::kRmeUnitSpecifierId,
                   ASFW::DeviceProfiles::Audio::kRmeFireface800UnitVersion);
}

SeededDevice MakeDiceRom() {
    return MakeRom(kDiceGuid, ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
                   ASFW::DeviceProfiles::Audio::kSPro24DspModelId,
                   ASFW::DeviceProfiles::Audio::kFocusriteVendorId, 0x000001);
}

// A TerraTec Phase 88: BeBoB, served by the AV/C adapter.
SeededDevice MakeAvcRom() {
    return MakeRom(kAvcGuid, ASFW::DeviceProfiles::Audio::kTerraTecVendorId,
                   ASFW::DeviceProfiles::Audio::kPhase88RackFwModelId, kTa1394Specifier,
                   kAvcVersion);
}

// What AV/C discovery would push for the Phase 88.
ASFW::Audio::Model::ASFWAudioDevice MakeDiscoveredConfig(uint64_t guid) {
    ASFW::Audio::Model::ASFWAudioDevice config{};
    config.guid = guid;
    config.deviceName = "Discovered Phase 88";
    config.inputChannelCount = 8;
    config.outputChannelCount = 8;
    config.channelCount = 8;
    config.sampleRates = {44100U, 48000U};
    config.currentSampleRate = 48000U;
    return config;
}

// Everything the coordinator is built over. Declared in dependency order; the
// coordinator is last so it is destroyed first.
struct CoordinatorHarness {
    FakeDeviceManager deviceManager{};
    DeviceRegistry registry{};
    AudioRuntimeRegistry runtime{};
    IsochService isoch{};
    HardwareInterface hardware{};
    DiceNotificationRouter diceNotifications{registry};
    AudioCoordinator coordinator{nullptr, deviceManager, registry, runtime, isoch, hardware,
                                 diceNotifications};

    // Put the device into the registry (so the catalog resolves its policy) and
    // return the FWDevice the bus would hand to the observer.
    std::shared_ptr<FWDevice> Seed(const SeededDevice& seed) {
        (void)registry.UpsertFromROM(seed.rom, ASFW::Discovery::LinkPolicy{});
        const auto record = registry.SnapshotByGuid(seed.rom.bib.guid);
        if (!record) {
            return nullptr;
        }
        return FWDevice::Create(*record, seed.rom);
    }

    ASFW::Audio::Host::AudioDeviceHost& Host() { return coordinator.HostForTesting(); }
};

uint64_t PublishOutcomes(ASFW::Audio::Host::AudioDeviceHost& host) {
    uint64_t total = 0;
    for (uint8_t i = 0; i < static_cast<uint8_t>(HostOutcome::kCount); ++i) {
        total += host.OutcomeCount(HostEvent::Publish, static_cast<HostOutcome>(i));
    }
    return total;
}

uint64_t RuntimeFaultOutcomes(ASFW::Audio::Host::AudioDeviceHost& host) {
    uint64_t total = 0;
    for (uint8_t i = 0; i < static_cast<uint8_t>(HostOutcome::kCount); ++i) {
        total += host.OutcomeCount(HostEvent::RuntimeFault, static_cast<HostOutcome>(i));
    }
    return total;
}

uint64_t RebindOutcomes(ASFW::Audio::Host::AudioDeviceHost& host) {
    uint64_t total = 0;
    for (uint8_t i = 0; i < static_cast<uint8_t>(HostOutcome::kCount); ++i) {
        total += host.OutcomeCount(HostEvent::Rebind, static_cast<HostOutcome>(i));
    }
    return total;
}

uint64_t RestartLikeOutcomes(ASFW::Audio::Host::AudioDeviceHost& host) {
    return host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::RestartRequested) +
           host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Declined) +
           host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Failed);
}

// ---------------------------------------------------------------------------
// Observer registration
// ---------------------------------------------------------------------------

TEST(AudioCoordinatorRoutingTests, ConstructionRegistersObserverAndDestructionUnregistersIt) {
    FakeDeviceManager deviceManager;
    DeviceRegistry registry;
    AudioRuntimeRegistry runtime;
    IsochService isoch;
    HardwareInterface hardware;
    DiceNotificationRouter diceNotifications{registry};
    {
        AudioCoordinator coordinator{nullptr, deviceManager, registry, runtime, isoch, hardware,
                                     diceNotifications};
        EXPECT_EQ(deviceManager.registerCalls, 1);
        EXPECT_EQ(deviceManager.observers.size(), 1U);
        EXPECT_EQ(deviceManager.unregisterCalls, 0);
    }
    EXPECT_EQ(deviceManager.unregisterCalls, 1);
    EXPECT_TRUE(deviceManager.observers.empty());
}

// ---------------------------------------------------------------------------
// Routing
// ---------------------------------------------------------------------------

TEST(AudioCoordinatorRoutingTests, RmeDeviceAddedIsPublishedThroughTheHostOnce) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeRmeRom());
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(h.Host().KindForGuid(kRmeGuid), ASFW::Audio::AudioBackendKind::RmeRegister);

    h.coordinator.OnDeviceAdded(device);

    // No protocol is registered in the runtime registry, so the adapter refuses.
    EXPECT_EQ(PublishOutcomes(h.Host()), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 1U);
}

TEST(AudioCoordinatorRoutingTests, DiceDeviceAddedDoesNotReachTheHost) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeDiceRom());
    ASSERT_NE(device, nullptr);
    ASSERT_EQ(h.Host().KindForGuid(kDiceGuid), ASFW::Audio::AudioBackendKind::Dice);

    h.coordinator.OnDeviceAdded(device);

    EXPECT_EQ(PublishOutcomes(h.Host()), 0U);
}

TEST(AudioCoordinatorRoutingTests, RmeDeviceResumedWhileIdleIsPublishedOnceWithoutRebind) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeRmeRom());
    ASSERT_NE(device, nullptr);

    h.coordinator.OnDeviceResumed(device);

    EXPECT_EQ(PublishOutcomes(h.Host()), 1U);
    EXPECT_EQ(RebindOutcomes(h.Host()), 0U);
}

TEST(AudioCoordinatorRoutingTests, TimingLossOnRmeIsQueuedAndDecidedOnce) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeRmeRom());
    ASSERT_NE(device, nullptr);
    auto* queue = h.Host().WorkQueueForTesting();
    ASSERT_NE(queue, nullptr);
    queue->SetManualDispatchForTesting(true);

    h.coordinator.HandleHostTimingLoss(kRmeGuid);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Queued), 1U);
    queue->DrainAllForTesting();

    // Exactly one decision: the session is idle, so the restart is declined or
    // the host records it as requested. Either way it is recorded once.
    EXPECT_EQ(RestartLikeOutcomes(h.Host()), 1U);
}

TEST(AudioCoordinatorRoutingTests, TimingLossOnDiceNeverReachesHostRuntimeFault) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeDiceRom());
    ASSERT_NE(device, nullptr);

    h.coordinator.HandleHostTimingLoss(kDiceGuid);

    EXPECT_EQ(RuntimeFaultOutcomes(h.Host()), 0U);
}

TEST(AudioCoordinatorRoutingTests, RemovedRmeDeviceDropsLaterTimingLossSilently) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeRmeRom());
    ASSERT_NE(device, nullptr);
    h.coordinator.OnDeviceAdded(device);
    const uint64_t publishBefore = PublishOutcomes(h.Host());

    h.coordinator.OnDeviceRemoved(kRmeGuid);
    h.coordinator.HandleHostTimingLoss(kRmeGuid);

    // Removal does not record on the host; a late timing loss for the removed
    // GUID is dropped before it reaches the host.
    EXPECT_EQ(RuntimeFaultOutcomes(h.Host()), 0U);
    EXPECT_EQ(PublishOutcomes(h.Host()), publishBefore);
}

TEST(AudioCoordinatorRoutingTests, BeginTeardownDrainsTheHostOnceAndIsIdempotent) {
    CoordinatorHarness h;
    h.coordinator.BeginTeardown();
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Teardown, HostOutcome::Drained), 1U);

    h.coordinator.BeginTeardown();
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Teardown, HostOutcome::Drained), 1U);
}

// ---------------------------------------------------------------------------
// AV/C (E4): discovery pushes the description, the host publishes it
// ---------------------------------------------------------------------------

TEST(AudioCoordinatorRoutingTests, AvcDeviceIsServedByTheHost) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeAvcRom());
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(h.Host().KindForGuid(kAvcGuid), ASFW::Audio::AudioBackendKind::Avc);
}

TEST(AudioCoordinatorRoutingTests, AvcDeviceAddedBeforeDiscoveryIsRefusedAsAwaitingDiscovery) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeAvcRom());
    ASSERT_NE(device, nullptr);

    h.coordinator.OnDeviceAdded(device);

    EXPECT_EQ(PublishOutcomes(h.Host()), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 1U);
    EXPECT_EQ(h.runtime.FindEndpointRuntime(kAvcGuid), nullptr);
}

TEST(AudioCoordinatorRoutingTests, AvcDeviceResumedWhileIdleIsRefreshedOnceWithoutRebind) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeAvcRom());
    ASSERT_NE(device, nullptr);

    h.coordinator.OnDeviceResumed(device);

    EXPECT_EQ(PublishOutcomes(h.Host()), 1U);
    EXPECT_EQ(RebindOutcomes(h.Host()), 0U);
}

TEST(AudioCoordinatorRoutingTests, AvcConfigurationReadyIsPublishedThroughTheHostOnce) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeAvcRom());
    ASSERT_NE(device, nullptr);
    const auto offered = MakeDiscoveredConfig(kAvcGuid);

    h.coordinator.OnAVCAudioConfigurationReady(kAvcGuid, offered);

    // One publication, decided once. The test publisher has no driver, so the
    // nub cannot be created: PublishFailed. The decision is still recorded.
    EXPECT_EQ(PublishOutcomes(h.Host()), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 1U);

    // The endpoint runtime carries exactly the description discovery offered.
    const auto endpoint = h.runtime.FindEndpointRuntime(kAvcGuid);
    ASSERT_NE(endpoint, nullptr);
    ASFW::Audio::Model::ASFWAudioDevice committed{};
    ASSERT_TRUE(endpoint->CopyConfig(committed));
    EXPECT_EQ(committed.guid, offered.guid);
    EXPECT_EQ(committed.deviceName, offered.deviceName);
    EXPECT_EQ(committed.inputChannelCount, offered.inputChannelCount);
    EXPECT_EQ(committed.outputChannelCount, offered.outputChannelCount);
    EXPECT_EQ(committed.sampleRates, offered.sampleRates);
    EXPECT_EQ(committed.currentSampleRate, offered.currentSampleRate);
}

TEST(AudioCoordinatorRoutingTests, AvcDiscoveryAfterAddedPublishesWhatAddedCouldNot) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeAvcRom());
    ASSERT_NE(device, nullptr);

    h.coordinator.OnDeviceAdded(device);
    h.coordinator.OnAVCAudioConfigurationReady(kAvcGuid, MakeDiscoveredConfig(kAvcGuid));

    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 1U);
    EXPECT_EQ(PublishOutcomes(h.Host()), 2U);
}

TEST(AudioCoordinatorRoutingTests, TimingLossOnAvcIsQueuedAndJudgedOnTheHost) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeAvcRom());
    ASSERT_NE(device, nullptr);
    uint32_t sleptMs = 0;
    h.Host().SetSleepForTesting([&sleptMs](uint32_t ms) { sleptMs += ms; });
    auto* queue = h.Host().WorkQueueForTesting();
    ASSERT_NE(queue, nullptr);
    queue->SetManualDispatchForTesting(true);

    h.coordinator.HandleHostTimingLoss(kAvcGuid);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Queued), 1U);
    EXPECT_EQ(RuntimeFaultOutcomes(h.Host()), 1U);
    queue->DrainAllForTesting();

    // The session is not streaming: the settle sees the device gone on its first
    // check, so no restart is requested and nothing sleeps.
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::RuntimeFault, HostOutcome::DeviceLeft), 1U);
    EXPECT_EQ(RestartLikeOutcomes(h.Host()), 0U);
    EXPECT_EQ(sleptMs, 0U);
}

TEST(AudioCoordinatorRoutingTests, RemovedAvcDeviceDropsLaterTimingLossSilently) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeAvcRom());
    ASSERT_NE(device, nullptr);
    h.coordinator.OnDeviceAdded(device);
    h.coordinator.OnAVCAudioConfigurationReady(kAvcGuid, MakeDiscoveredConfig(kAvcGuid));
    const uint64_t publishBefore = PublishOutcomes(h.Host());

    h.coordinator.OnDeviceRemoved(kAvcGuid);
    h.coordinator.HandleHostTimingLoss(kAvcGuid);

    EXPECT_EQ(RuntimeFaultOutcomes(h.Host()), 0U);
    EXPECT_EQ(PublishOutcomes(h.Host()), publishBefore);
}

TEST(AudioCoordinatorRoutingTests, RemovalClearsTheStoredAvcDescription) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeAvcRom());
    ASSERT_NE(device, nullptr);
    h.coordinator.OnAVCAudioConfigurationReady(kAvcGuid, MakeDiscoveredConfig(kAvcGuid));
    ASSERT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 1U);
    ASSERT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 0U);

    // Still stored: a refresh describes from it again and tries to publish.
    h.Host().RefreshPublication(kAvcGuid);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 0U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 2U);

    h.coordinator.OnDeviceRemoved(kAvcGuid);

    // Gone with the device: a later refresh waits for discovery again.
    h.Host().RefreshPublication(kAvcGuid);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 2U);
}

TEST(AudioCoordinatorRoutingTests, AvcTeardownDrainsOnceAndRefusesLaterDiscovery) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeAvcRom());
    ASSERT_NE(device, nullptr);

    h.coordinator.BeginTeardown();
    h.coordinator.BeginTeardown();
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Teardown, HostOutcome::Drained), 1U);

    h.coordinator.OnAVCAudioConfigurationReady(kAvcGuid, MakeDiscoveredConfig(kAvcGuid));
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::RefusedTeardown), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 0U);
}

TEST(AudioCoordinatorRoutingTests, AvcIsAudioActiveIsFalseWhenIdle) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeAvcRom());
    ASSERT_NE(device, nullptr);
    EXPECT_FALSE(h.coordinator.IsAudioActive(kAvcGuid));
}

} // namespace
