#include <gtest/gtest.h>
#include "ASFWDriver/Protocols/BeBoB/Bootloader/BeBoBBootloaderPreparation.hpp"
#include "ASFWDriver/Protocols/BeBoB/Bootloader/BeBoBBootloaderPreparationCoordinator.hpp"
#include "ASFWDriver/Discovery/DiscoveryTypes.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/DeviceProfiles/Audio/AudioDeviceCatalog.hpp"
#include "ASFWDriver/Audio/Protocols/SelectProbeBootstrap.hpp"
#include "tests/mocks/DeferredFireWireBus.hpp"

#include <algorithm>
#include <array>

namespace {
using namespace ASFW::Protocols::BeBoB::Bootloader;

void StoreLE32(std::array<uint8_t, kInfoBlockBytes>& b, size_t p, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) b[p + i] = static_cast<uint8_t>(v >> (8U * i));
}
BootRomInfo Info(uint32_t protocol, uint32_t loader, uint64_t date) {
    BootRomInfo info{};
    StoreLE32(info.raw, kProtocolVersionOffset, protocol);
    StoreLE32(info.raw, kBootloaderVersionOffset, loader);
    for (size_t i = 0; i < 8; ++i)
        info.raw[kSoftwareDateOffset + i] = static_cast<uint8_t>(date >> (8U * i));
    return info;
}

ASFW::Discovery::DeviceIdentityEvidence BootloaderIdentity() {
    ASFW::Discovery::DeviceIdentityEvidence identity{};
    identity.observedGuid = 0x0011223344556677ULL;
    identity.rootVendorId = kMAudioVendorId;
    identity.rootModelId = kFireWire1814BootloaderModelId;
    identity.units.push_back(ASFW::Discovery::UnitIdentityEvidence{
        .specifierId = 0x00A02D,
        .version = 0x00010070,
    });
    return identity;
}

// Production hands preparation the registry's resolved plan; the tests resolve
// the same identity once to stand in for it.
ASFW::DeviceProfiles::Audio::StaticAudioEndpointPlan PlanFor(
    const ASFW::Discovery::DeviceIdentityEvidence& identity) {
    return ASFW::DeviceProfiles::Audio::AudioDeviceCatalog::Resolve(identity).value_or(
        ASFW::DeviceProfiles::Audio::StaticAudioEndpointPlan{});
}

ASFW::Discovery::DeviceRouteToken BindRoute(ASFW::Discovery::DeviceRegistry& registry) {
    ASFW::Discovery::ConfigROM rom{};
    rom.bib.guid = 0x0011223344556677ULL;
    rom.gen = ASFW::Discovery::Generation{1};
    rom.nodeId = 0x21;
    (void)registry.UpsertFromROM(rom, {});
    return *registry.CurrentRoute(rom.bib.guid);
}

void MapInfo(ASFW::Async::Testing::DeferredFireWireBus& bus, uint32_t loader,
             uint64_t date) {
    const auto info = Info(1, loader, date);
    const uint64_t address = (static_cast<uint64_t>(kAddressHi) << 32U) | kInfoAddressLo;
    bus.MapRead(address, std::vector<uint8_t>(info.raw.begin(), info.raw.end()));
}

TEST(BeBoBBootloaderPreparation, EncodesTheSinglePermittedCueLittleEndian) {
    const BeBoBBootloaderCue cue{Info(0x01020304, 1, 0x3230303730343031ULL)};
    const std::array<uint8_t, 12> expected{
        0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x11, 0x01, 0, 0, 0, 0};
    EXPECT_TRUE(std::equal(cue.Bytes().begin(), cue.Bytes().end(), expected.begin()));
    EXPECT_TRUE(IsPermittedBootloaderWrite(kAddressHi, kRequestAddressLo, cue.Bytes()));
    EXPECT_FALSE(IsPermittedBootloaderWrite(kAddressHi, kInfoAddressLo, cue.Bytes()));
}

TEST(BeBoBBootloaderPreparation, AutomaticCuePersonaIsOnlyThe1814Bootloader) {
    EXPECT_TRUE(IsSupportedBootloaderPersona(kMAudioVendorId,
                                             kFireWire1814BootloaderModelId));
    EXPECT_FALSE(IsSupportedBootloaderPersona(kMAudioVendorId, 0x00010071));
    EXPECT_FALSE(IsSupportedBootloaderPersona(kMAudioVendorId, 0x00010091));
    EXPECT_FALSE(IsSupportedBootloaderPersona(0x00000FDB,
                                              kFireWire1814BootloaderModelId));
}

TEST(BeBoBBootloaderPreparation, TriggerRequiresTheCatalogBootloaderCuePolicy) {
    auto identity = BootloaderIdentity();
    EXPECT_TRUE(ShouldPrepareBootloader(PlanFor(identity), kMAudioVendorId,
                                        kFireWire1814BootloaderModelId));
    EXPECT_FALSE(ShouldPrepareBootloader(PlanFor(identity), kMAudioVendorId, 0x00010071));

    identity.rootModelId = 0x00010071;
    EXPECT_FALSE(ShouldPrepareBootloader(PlanFor(identity), kMAudioVendorId,
                                         kFireWire1814BootloaderModelId));
}

// FW-163: preparation cannot hand the bootloader persona to a probe. Its
// bootstrap and FCP gate come from the static plan, which no preparation
// outcome (read failure, rejected build, failed cue) can change.
TEST(BeBoBBootloaderPreparation, FailedPreparationHasNoPathToAGenericProbe) {
    const auto plan = PlanFor(BootloaderIdentity());
    EXPECT_EQ(ASFW::Audio::SelectProbeBootstrap(plan), ASFW::Audio::ProbeBootstrap::Unsupported);
    EXPECT_EQ(ASFW::DeviceProfiles::Audio::AudioDeviceCatalog::CommandFilterFor(plan),
              ASFW::Discovery::AvcCommandFilterId::BlockAll);
    EXPECT_NE(plan.support, ASFW::DeviceProfiles::Audio::SupportDisposition::Supported);
}

TEST(BeBoBBootloaderPreparation, CoordinatorChecksIdentityRouteDateAndWritesValidCueOnce) {
    using namespace ASFW;
    Discovery::DeviceRegistry registry{};
    const auto route = BindRoute(registry);
    Async::Testing::DeferredFireWireBus bus{};
    MapInfo(bus, 1, 0x3230303730343031ULL);
    Protocols::BeBoB::Bootloader::BeBoBBootloaderPreparationCoordinator coordinator{
        bus, registry};
    auto identity = BootloaderIdentity();
    bool alive = true;

    const auto shouldPrepare = [&] {
        return coordinator.Prepare(PlanFor(identity), kMAudioVendorId,
                                   kFireWire1814BootloaderModelId, route, FW::FwSpeed::S400,
                                   [&alive] { return alive; });
    };
    EXPECT_TRUE(shouldPrepare());
    EXPECT_FALSE(shouldPrepare());
    ASSERT_EQ(bus.ReadCount(), 1U);
    ASSERT_EQ(bus.WriteCount(), 1U);
    EXPECT_EQ(bus.ReadAt(0).address.addressHi, kAddressHi);
    EXPECT_EQ(bus.ReadAt(0).address.addressLo, kInfoAddressLo);
    EXPECT_EQ(bus.WriteAt(0).address.addressHi, kAddressHi);
    EXPECT_EQ(bus.WriteAt(0).address.addressLo, kRequestAddressLo);
    const std::array<uint8_t, 12> expected{
        1, 0, 0, 0, 0, 0, 0x11, 0x01, 0, 0, 0, 0};
    EXPECT_EQ(bus.WriteAt(0).data,
              std::vector<uint8_t>(expected.begin(), expected.end()));

    alive = false;
    EXPECT_TRUE(bus.CompleteNextWrite(Async::AsyncStatus::kSuccess));
}

TEST(BeBoBBootloaderPreparation, CoordinatorRejectsWrongPersonaAndStaleRouteBeforeReading) {
    using namespace ASFW;
    Discovery::DeviceRegistry registry{};
    auto route = BindRoute(registry);
    Async::Testing::DeferredFireWireBus bus{};
    Protocols::BeBoB::Bootloader::BeBoBBootloaderPreparationCoordinator coordinator{
        bus, registry};
    auto identity = BootloaderIdentity();
    const auto alive = [] { return true; };

    EXPECT_FALSE(coordinator.Prepare(PlanFor(identity), kMAudioVendorId, 0x00010071, route,
                                     FW::FwSpeed::S400, alive));
    EXPECT_EQ(bus.ReadCount(), 0U);
    registry.InvalidateLiveMappingsForBusReset();
    EXPECT_FALSE(coordinator.Prepare(PlanFor(identity), kMAudioVendorId,
                                     kFireWire1814BootloaderModelId, route,
                                     FW::FwSpeed::S400, alive));
    EXPECT_EQ(bus.ReadCount(), 0U);
    EXPECT_EQ(bus.WriteCount(), 0U);
}

TEST(BeBoBBootloaderPreparation, CoordinatorDoesNotWriteForUnsupportedDateOrInactiveLoader) {
    using namespace ASFW;
    const auto runWithoutCue = [](uint32_t loader, uint64_t date) {
        Discovery::DeviceRegistry registry{};
        const auto route = BindRoute(registry);
        Async::Testing::DeferredFireWireBus bus{};
        MapInfo(bus, loader, date);
        Protocols::BeBoB::Bootloader::BeBoBBootloaderPreparationCoordinator coordinator{
            bus, registry};
        const auto identity = BootloaderIdentity();
        EXPECT_TRUE(coordinator.Prepare(PlanFor(identity), kMAudioVendorId,
                                        kFireWire1814BootloaderModelId, route, FW::FwSpeed::S400,
                                        [] { return true; }));
        EXPECT_EQ(bus.ReadCount(), 1U);
        EXPECT_EQ(bus.WriteCount(), 0U);
    };
    runWithoutCue(1, 0x3230303730343030ULL);
    runWithoutCue(0, 0x3230303730343031ULL);
}

TEST(BeBoBBootloaderPreparation, RequiresActiveBootloaderAndSupportedBuildDate) {
    auto state = BeginPreparation().state;
    auto step = AdvancePreparation(state, InfoReadSucceeded{Info(1, 0, 0x3230303730343031ULL)});
    ASSERT_TRUE(std::holds_alternative<Retired>(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::FirmwareAlreadyRunning);

    step = AdvancePreparation(ReadingInfo{}, InfoReadSucceeded{Info(1, 1, 0x3230303730343030ULL)});
    ASSERT_TRUE(std::holds_alternative<Retired>(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::UnsupportedBuild);
}

TEST(BeBoBBootloaderPreparation, ReadsOnceThenCuesOnceAndWaitsForReenumeration) {
    auto step = BeginPreparation();
    ASSERT_TRUE(std::holds_alternative<ReadInfoBlock>(step.action));
    step = AdvancePreparation(step.state,
        InfoReadSucceeded{Info(0x1234, 1, 0x3230303730343031ULL)});
    ASSERT_TRUE(std::holds_alternative<WriteCue>(step.action));
    ASSERT_TRUE(std::holds_alternative<AwaitingReenumeration>(step.state));
    EXPECT_EQ(std::get<WriteCue>(step.action).cue.ProtocolVersion(), 0x1234U);
    step = AdvancePreparation(step.state, CueWriteSucceeded{});
    ASSERT_TRUE(std::holds_alternative<Done>(step.action));
    EXPECT_TRUE(std::holds_alternative<AwaitingReenumeration>(step.state));
    step = AdvancePreparation(step.state, CueWriteSucceeded{});
    EXPECT_TRUE(std::holds_alternative<Done>(step.action));
    EXPECT_TRUE(std::holds_alternative<AwaitingReenumeration>(step.state));
    step = AdvancePreparation(step.state, GenerationInvalidated{});
    ASSERT_TRUE(std::holds_alternative<Retired>(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::GenerationChanged);
}

TEST(BeBoBBootloaderPreparation, FailedInfoReadsAreBoundedAndGenerationChangeStopsWork) {
    auto state = BeginPreparation().state;
    for (uint8_t i = 1; i < kMaxInfoReadAttempts; ++i) {
        auto step = AdvancePreparation(state, InfoReadFailed{});
        EXPECT_TRUE(std::holds_alternative<ReadInfoBlock>(step.action));
        state = step.state;
    }
    auto step = AdvancePreparation(state, InfoReadFailed{});
    ASSERT_TRUE(std::holds_alternative<Retired>(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::InfoUnavailable);

    step = AdvancePreparation(BeginPreparation().state, GenerationInvalidated{});
    ASSERT_TRUE(std::holds_alternative<Retired>(step.state));
    EXPECT_EQ(std::get<Retired>(step.state).reason, RetireReason::GenerationChanged);
}
} // namespace
