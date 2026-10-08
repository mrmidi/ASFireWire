// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioCoordinatorRoutingTests.cpp
// Which family serves a device event: the RME Fireface (E2), the AV/C family
// (E4) and MOTU (E3) go to the audio device host (documentation/AUDIO_DEVICE_HOST.md
// §6), DICE stays on its backend.
// The coordinator is built for real; only the device manager is a fake, and
// the assertions read the host's per-outcome counters.

#include <gtest/gtest.h>

#include "Audio/Core/AudioCoordinator.hpp"
#include "Audio/Core/AudioEndpointRuntime.hpp"
#include "Audio/Core/AudioRuntimeRegistry.hpp"
#include "Audio/Host/AudioDeviceHost.hpp"
#include "Audio/Protocols/MOTU/MotuV2Protocol.hpp"
#include "Audio/Protocols/DICE/Core/DiceNotificationRouter.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Discovery/FWDevice.hpp"
#include "Discovery/IDeviceManager.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Isoch/IsochService.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
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
constexpr uint64_t kMotuGuid = 0x0001f20000000001ULL;
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

// A MOTU 828mkII: the root directory publishes model_id 0, the model is in Unit_Sw_Version.
SeededDevice MakeMotuRom() {
    return MakeRom(kMotuGuid, ASFW::DeviceProfiles::Audio::kMotuVendorId, 0U,
                   ASFW::DeviceProfiles::Audio::kMotuVendorId,
                   ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion);
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

// Answers quadlet reads from a table; everything else is accepted. Lets the real
// MotuV2Protocol run under the coordinator.
class MotuRecordingBus final : public ASFW::Async::IFireWireBusOps,
                               public ASFW::Async::IFireWireBusInfo {
public:
    std::vector<uint32_t> reads;
    std::map<uint32_t, uint32_t> readValues;
    ASFW::Async::AsyncStatus readStatus{ASFW::Async::AsyncStatus::kSuccess};

    ASFW::Async::AsyncHandle ReadBlock(ASFW::FW::Generation, ASFW::FW::NodeId,
                                       ASFW::Async::FWAddress address, uint32_t, ASFW::FW::FwSpeed,
                                       ASFW::Async::InterfaceCompletionCallback callback) override {
        reads.push_back(address.addressLo);
        const auto found = readValues.find(address.addressLo);
        if (readStatus != ASFW::Async::AsyncStatus::kSuccess || found == readValues.end()) {
            callback(readStatus, {});
            return ASFW::Async::AsyncHandle{1};
        }
        const uint32_t value = found->second;
        const uint8_t payload[4] = {static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16),
                                    static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value)};
        callback(ASFW::Async::AsyncStatus::kSuccess, std::span<const uint8_t>(payload, 4));
        return ASFW::Async::AsyncHandle{1};
    }
    ASFW::Async::AsyncHandle WriteBlock(ASFW::FW::Generation, ASFW::FW::NodeId,
                                        ASFW::Async::FWAddress, std::span<const uint8_t>,
                                        ASFW::FW::FwSpeed,
                                        ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(ASFW::Async::AsyncStatus::kSuccess, {});
        return ASFW::Async::AsyncHandle{1};
    }
    ASFW::Async::AsyncHandle Lock(ASFW::FW::Generation, ASFW::FW::NodeId, ASFW::Async::FWAddress,
                                  ASFW::FW::LockOp, std::span<const uint8_t>, uint32_t,
                                  ASFW::FW::FwSpeed,
                                  ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(ASFW::Async::AsyncStatus::kSuccess, {});
        return ASFW::Async::AsyncHandle{1};
    }
    bool Cancel(ASFW::Async::AsyncHandle) override { return false; }
    ASFW::FW::FwSpeed GetSpeed(ASFW::FW::NodeId) const override { return ASFW::FW::FwSpeed::S400; }
    uint32_t HopCount(ASFW::FW::NodeId, ASFW::FW::NodeId) const override { return 1; }
    ASFW::FW::Generation GetGeneration() const override { return ASFW::FW::Generation{1}; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return ASFW::FW::NodeId{0}; }
};

constexpr uint32_t kInOutConfLow =
    static_cast<uint32_t>(ASFW::Audio::Motu::kAddrBase) +
    static_cast<uint32_t>(ASFW::Audio::Motu::Reg::InOutConfV2);

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

    // Give the seeded MOTU its real protocol over `bus`, as the driver's device
    // factory would, with the optical config word the device reports.
    void AttachMotuProtocol(MotuRecordingBus& bus, uint32_t opticalWord) {
        bus.readValues[kInOutConfLow] = opticalWord;
        const auto route = registry.CurrentRoute(kMotuGuid);
        ASSERT_TRUE(route.has_value());
        runtime.Insert(kMotuGuid, std::make_shared<ASFW::Audio::Motu::MotuV2Protocol>(
                                      bus, bus, registry, *route,
                                      ASFW::DeviceProfiles::Audio::kMotu828mk2SwVersion));
    }
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

// ---------------------------------------------------------------------------
// MOTU (E3): described from the optical register, served by the host
// ---------------------------------------------------------------------------

constexpr uint32_t OpticalWord(uint32_t inMode, uint32_t outMode) {
    return ((inMode & 0x3U) << 8) | ((outMode & 0x3U) << 10);
}

TEST(AudioCoordinatorRoutingTests, MotuDeviceIsServedByTheHost) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeMotuRom());
    ASSERT_NE(device, nullptr);
    EXPECT_EQ(h.Host().KindForGuid(kMotuGuid), ASFW::Audio::AudioBackendKind::MotuRegister);
}

TEST(AudioCoordinatorRoutingTests, MotuDeviceAddedWithoutProtocolIsRefusedAsNoProtocol) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeMotuRom());
    ASSERT_NE(device, nullptr);

    h.coordinator.OnDeviceAdded(device);

    EXPECT_EQ(PublishOutcomes(h.Host()), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 1U);
}

TEST(AudioCoordinatorRoutingTests, MotuAddedIsDescribedFromTheOpticalRegisterAndPublishedOnce) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeMotuRom());
    ASSERT_NE(device, nullptr);
    MotuRecordingBus bus;
    h.AttachMotuProtocol(bus, OpticalWord(1U /*ADAT in*/, 2U /*S/PDIF out*/));

    h.coordinator.OnDeviceAdded(device);

    // One decision, after exactly one register read. The test publisher has no
    // driver, so the nub cannot be created (PublishFailed) -- but the endpoint
    // config carries the description the host built.
    EXPECT_EQ(PublishOutcomes(h.Host()), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 1U);
    ASSERT_EQ(bus.reads.size(), 1U);
    EXPECT_EQ(bus.reads[0], kInOutConfLow);
    const auto endpoint = h.runtime.FindEndpointRuntime(kMotuGuid);
    ASSERT_NE(endpoint, nullptr);
    ASFW::Audio::Model::ASFWAudioDevice committed{};
    ASSERT_TRUE(endpoint->CopyConfig(committed));
    EXPECT_EQ(committed.inputChannelCount, 22U);   // ADAT input: 14 + 8
    EXPECT_EQ(committed.outputChannelCount, 14U);  // S/PDIF output: fixed
    EXPECT_EQ(committed.sampleRates, (std::vector<uint32_t>{44100U, 48000U}));
}

TEST(AudioCoordinatorRoutingTests, MotuFailedRegisterReadStillPublishesTheFixedGeometry) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeMotuRom());
    ASSERT_NE(device, nullptr);
    MotuRecordingBus bus;
    h.AttachMotuProtocol(bus, OpticalWord(1U, 1U));
    bus.readStatus = ASFW::Async::AsyncStatus::kTimeout;

    h.coordinator.OnDeviceAdded(device);

    // The device must still appear: published (here: PublishFailed for want of a
    // driver), never refused, with the model's fixed 14 x 14.
    EXPECT_EQ(PublishOutcomes(h.Host()), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 0U);
    const auto endpoint = h.runtime.FindEndpointRuntime(kMotuGuid);
    ASSERT_NE(endpoint, nullptr);
    ASFW::Audio::Model::ASFWAudioDevice committed{};
    ASSERT_TRUE(endpoint->CopyConfig(committed));
    EXPECT_EQ(committed.inputChannelCount, 14U);
    EXPECT_EQ(committed.outputChannelCount, 14U);
}

TEST(AudioCoordinatorRoutingTests, MotuResumedWhileIdleIsRefreshedOnceWithoutRebind) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeMotuRom());
    ASSERT_NE(device, nullptr);

    h.coordinator.OnDeviceResumed(device);

    EXPECT_EQ(PublishOutcomes(h.Host()), 1U);
    EXPECT_EQ(RebindOutcomes(h.Host()), 0U);
}

TEST(AudioCoordinatorRoutingTests, TimingLossOnMotuIsQueuedAndRestartedOnTheHost) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeMotuRom());
    ASSERT_NE(device, nullptr);
    auto* queue = h.Host().WorkQueueForTesting();
    ASSERT_NE(queue, nullptr);
    queue->SetManualDispatchForTesting(true);

    h.coordinator.HandleHostTimingLoss(kMotuGuid);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Queued), 1U);
    queue->DrainAllForTesting();

    // MOTU reads no health evidence: the fault is never SelfHealed or DeviceLeft,
    // it goes straight to the session, which is idle here and so declines.
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::RuntimeFault, HostOutcome::SelfHealed), 0U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::RuntimeFault, HostOutcome::DeviceLeft), 0U);
    EXPECT_EQ(RestartLikeOutcomes(h.Host()), 1U);
}

TEST(AudioCoordinatorRoutingTests, RemovedMotuDeviceDropsLaterTimingLossSilently) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeMotuRom());
    ASSERT_NE(device, nullptr);
    h.coordinator.OnDeviceAdded(device);
    const uint64_t publishBefore = PublishOutcomes(h.Host());

    h.coordinator.OnDeviceRemoved(kMotuGuid);
    h.coordinator.HandleHostTimingLoss(kMotuGuid);

    EXPECT_EQ(RuntimeFaultOutcomes(h.Host()), 0U);
    EXPECT_EQ(PublishOutcomes(h.Host()), publishBefore);
}

TEST(AudioCoordinatorRoutingTests, MotuTeardownDrainsOnceAndRefusesLaterPublication) {
    CoordinatorHarness h;
    auto device = h.Seed(MakeMotuRom());
    ASSERT_NE(device, nullptr);

    h.coordinator.BeginTeardown();
    h.coordinator.BeginTeardown();
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Teardown, HostOutcome::Drained), 1U);

    h.coordinator.OnDeviceAdded(device);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::RefusedTeardown), 1U);
    EXPECT_EQ(h.Host().OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 0U);
}

// ---------------------------------------------------------------------------
// MOTU capture diagnostics keep working through the host's kind lookup
// ---------------------------------------------------------------------------

TEST(AudioCoordinatorRoutingTests, MotuCaptureArmsOnlyAMotuEndpoint) {
    CoordinatorHarness h;
    auto motu = h.Seed(MakeMotuRom());
    auto rme = h.Seed(MakeRmeRom());
    auto dice = h.Seed(MakeDiceRom());
    ASSERT_NE(motu, nullptr);
    ASSERT_NE(rme, nullptr);
    ASSERT_NE(dice, nullptr);
    MotuRecordingBus bus;
    h.AttachMotuProtocol(bus, OpticalWord(0U, 0U));
    std::string output;

    // Never label another family's packets as MOTU.
    EXPECT_EQ(h.coordinator.MotuCaptureCommand(kRmeGuid, 0, 0, output), kIOReturnUnsupported);
    EXPECT_EQ(h.coordinator.MotuCaptureCommand(kDiceGuid, 0, 0, output), kIOReturnUnsupported);
    EXPECT_EQ(h.coordinator.MotuCaptureCommand(0x1234ULL, 0, 0, output), kIOReturnUnsupported);

    EXPECT_EQ(h.coordinator.MotuCaptureCommand(kMotuGuid, 0, 0, output), kIOReturnSuccess);
    // Armed: a second arm is busy, and a disarm releases it.
    EXPECT_EQ(h.coordinator.MotuCaptureCommand(kMotuGuid, 0, 0, output), kIOReturnBusy);
    EXPECT_EQ(h.coordinator.MotuCaptureCommand(kMotuGuid, 0, 1, output), kIOReturnSuccess);
    EXPECT_EQ(h.coordinator.MotuCaptureCommand(kMotuGuid, 0, 0, output), kIOReturnSuccess);
}

TEST(AudioCoordinatorRoutingTests, MotuCaptureNeedsAProtocolAndRejectsBadArguments) {
    CoordinatorHarness h;
    auto motu = h.Seed(MakeMotuRom());
    ASSERT_NE(motu, nullptr);
    std::string output;

    EXPECT_EQ(h.coordinator.MotuCaptureCommand(kMotuGuid, 0, 0, output), kIOReturnNotReady);
    EXPECT_EQ(h.coordinator.MotuCaptureCommand(kMotuGuid, 0, 3, output), kIOReturnBadArgument);
    EXPECT_EQ(h.coordinator.MotuCaptureCommand(0, 0, 0, output), kIOReturnBadArgument);
}

TEST(AudioCoordinatorRoutingTests, MotuCaptureIsNotReadyAfterTeardown) {
    CoordinatorHarness h;
    auto motu = h.Seed(MakeMotuRom());
    ASSERT_NE(motu, nullptr);
    MotuRecordingBus bus;
    h.AttachMotuProtocol(bus, OpticalWord(0U, 0U));
    std::string output;

    h.coordinator.BeginTeardown();

    EXPECT_EQ(h.coordinator.MotuCaptureCommand(kMotuGuid, 0, 0, output), kIOReturnNotReady);
}

} // namespace
