// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Scripted Fireface register sequencing, clock admission, and resource-policy tests.
#include "DICEDuplexTestSupport.hpp"
#include "Audio/Protocols/RME/FirefaceFamilyDriver.hpp"
#include "Audio/Protocols/RME/FirefaceDeviceProtocol.hpp"
#include "Audio/Protocols/Backends/RmeAudioBackend.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include <utility>

namespace {
using namespace ASFW::Testing::DICE;
using ::ASFW::Async::InterfaceCompletionCallback;

class FirefaceScriptBus final : public IFireWireBus {
public:
    struct Write { uint16_t hi; uint32_t lo; std::vector<uint8_t> bytes; };
    AsyncHandle ReadBlock(Generation, NodeId, FWAddress address, uint32_t length, FwSpeed,
                          InterfaceCompletionCallback callback) override {
        const uint64_t key = (static_cast<uint64_t>(address.addressHi) << 32) | address.addressLo;
        reads.push_back(key);
        uint32_t value = 0;
        if (key == 0x000200000100ULL) value = revision;
        else if (key == 0x801c0000ULL) value = status0;
        else if (key == 0x801c0004ULL) value = status1;
        else if (key == 0x801c0008ULL) value = captureWord;
        else if (key == 0x80100524ULL) value = flashStatus;
        else if (key == 0x80100290ULL) value = revision;
        if (key == failReadAddress) { callback(AsyncStatus::kHardwareError, {}); return {}; }
        if (readHook) readHook(key);
        std::array<uint8_t, 4> bytes{static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
            static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
        if (deferRead) { deferRead = false; delayed = std::move(callback); delayedBytes = bytes; }
        else callback(AsyncStatus::kSuccess, std::span<const uint8_t>(bytes.data(), std::min<size_t>(length, 4)));
        return {};
    }
    AsyncHandle WriteBlock(Generation, NodeId, FWAddress address, std::span<const uint8_t> bytes,
                          FwSpeed, InterfaceCompletionCallback callback) override {
        writes.push_back({address.addressHi, address.addressLo, {bytes.begin(), bytes.end()}});
        const uint64_t key = (static_cast<uint64_t>(address.addressHi) << 32) | address.addressLo;
        if (key == failWriteAddress) { callback(AsyncStatus::kHardwareError, {}); return {}; }
        callback(AsyncStatus::kSuccess, {});
        return {};
    }
    AsyncHandle Lock(Generation, NodeId, FWAddress, LockOp, std::span<const uint8_t>, uint32_t,
                     FwSpeed, InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kHardwareError, {}); return {};
    }
    bool Cancel(AsyncHandle) override { return false; }
    FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S800; }
    uint32_t HopCount(NodeId, NodeId) const override { return 1; }
    Generation GetGeneration() const override { return Generation{1}; }
    NodeId GetLocalNodeID() const override { return NodeId{0}; }
    void DeliverDelayed() { if (delayed) { auto cb=std::move(delayed); cb(AsyncStatus::kSuccess, delayedBytes); } }
    uint32_t revision{0x24d};
    uint32_t status0{0x01c00000};
    uint32_t status1{0x00000007};
    uint32_t captureWord{6};
    uint32_t flashStatus{0};
    uint64_t failReadAddress{0};
    uint64_t failWriteAddress{0};
    std::function<void(uint64_t)> readHook;
    bool deferRead{false};
    std::vector<Write> writes;
    std::vector<uint64_t> reads;
    InterfaceCompletionCallback delayed;
    std::array<uint8_t, 4> delayedBytes{};
};

TEST(FirefaceIntegrationTests, Non48kAdmissionReturnsBeforeAnyRegisterTraffic) {
    FirefaceScriptBus bus;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(
        io, ASFW::Audio::RME::FirefaceModel::kFF400, false);
    const auto result = family.Configure({}, {.sampleRateHz = 44100});
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), kIOReturnUnsupported);
    EXPECT_TRUE(bus.reads.empty());
    EXPECT_TRUE(bus.writes.empty());
}

TEST(FirefaceIntegrationTests, ProtocolInitializationPublishesFixedGeometryWithoutBusTraffic) {
    FirefaceScriptBus bus;
    RouteState route;
    ASFW::Audio::RME::FirefaceDeviceProtocol protocol(
        bus, bus, route.registry, route.route, ASFW::Audio::RME::FirefaceModel::kFF400, false);
    EXPECT_EQ(protocol.Initialize(), kIOReturnSuccess);
    EXPECT_TRUE(bus.writes.empty());
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 18U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 18U);
    EXPECT_EQ(caps.deviceToHostStreamCount, 1U);
    EXPECT_EQ(caps.hostToDeviceStreamCount, 1U);
    EXPECT_STREQ(protocol.GetName(), "RME Fireface 400");
}

TEST(FirefaceIntegrationTests, PublicationConfigIsFixedDuplex48kWithoutMidi) {
    ASFW::Discovery::DeviceRecord record{};
    record.guid = 0x1234;
    record.vendorId = ASFW::DeviceProfiles::Audio::kRmeVendorId;
    record.modelId = ASFW::DeviceProfiles::Audio::kRmeRootModelId;
    for (const auto [builder, channels] : {
             std::pair{ASFW::DeviceProfiles::Audio::ProfileBuilderId::RmeFireface400, 18U},
             std::pair{ASFW::DeviceProfiles::Audio::ProfileBuilderId::RmeFireface800, 28U}}) {
        const auto config = ASFW::Audio::RmeAudioBackend::BuildNubConfig(record, builder, "RME");
        EXPECT_EQ(config.profileBuilderId, static_cast<uint32_t>(builder));
        EXPECT_EQ(config.inputChannelCount, channels);
        EXPECT_EQ(config.outputChannelCount, channels);
        EXPECT_EQ(config.channelCount, channels);
        EXPECT_EQ(config.sampleRates, (std::vector<uint32_t>{48000U}));
        ASSERT_EQ(config.captureStreams.size(), 1U);
        ASSERT_EQ(config.playbackStreams.size(), 1U);
        EXPECT_EQ(config.captureStreams[0].pcmChannels, channels);
        EXPECT_EQ(config.captureStreams[0].am824Slots, channels);
        EXPECT_EQ(config.captureStreams[0].midiPorts, 0U);
        EXPECT_EQ(config.playbackStreams[0].pcmChannels, channels);
        EXPECT_EQ(config.playbackStreams[0].am824Slots, channels);
        EXPECT_EQ(config.playbackStreams[0].midiPorts, 0U);
        EXPECT_TRUE(config.resolvedGeometryRequired);
    }
}

TEST(FirefaceRegisterTests, RegisterTupleUsesLittleEndianWithoutChangingBigEndianWriter) {
    RecordingFireWireBus bus;
    RouteState routeState;
    ProtocolRegisterIO io(bus, bus, routeState.registry, routeState.route);
    std::optional<AsyncStatus> status;
    (void)io.WriteQuadLE(MakeDICEAddress(0x80100500U), 0x12345678U,
        [&status](AsyncStatus value) { status = value; });
    ASSERT_EQ(status, AsyncStatus::kSuccess);
    ASSERT_EQ(bus.Operations().size(), 1U);
    EXPECT_EQ(bus.Operations()[0].payload, (std::vector<uint8_t>{0x78, 0x56, 0x34, 0x12}));
    EXPECT_EQ(ASFW::Audio::RME::InitWords(ASFW::Audio::RME::FirefaceModel::kFF400, 3, true),
              (std::array<uint32_t, 3>{48000, (18U << 11U) + 3U, 18}));
    EXPECT_EQ(ASFW::Audio::RME::InitWords(ASFW::Audio::RME::FirefaceModel::kFF800, 2, true),
              (std::array<uint32_t, 3>{48000, (28U << 11U) + 2U, 28U | 0x800U}));
    bus.ClearOperations();
    const auto words = ASFW::Audio::RME::InitWords(ASFW::Audio::RME::FirefaceModel::kFF400, 3, false);
    (void)io.WriteBlockLEQuadlets(MakeDICEAddress(0x80100500U), words,
        [&status](AsyncStatus value) { status = value; });
    ASSERT_EQ(status, AsyncStatus::kSuccess);
    ASSERT_EQ(bus.Operations().size(), 1U);
    EXPECT_EQ(bus.Operations()[0].payload, (std::vector<uint8_t>{
        0x80, 0xbb, 0x00, 0x00, 0x03, 0x90, 0x00, 0x00, 0x12, 0x00, 0x00, 0x00}));
    EXPECT_EQ(ASFW::Audio::RME::StartWord(ASFW::Audio::RME::FirefaceModel::kFF400, 5, false),
              0x80000000U | 18U | (5U << 5U));
    EXPECT_TRUE(ASFW::Audio::RME::IsValidAssignedChannel(ASFW::Audio::RME::FirefaceModel::kFF400, 7));
    EXPECT_FALSE(ASFW::Audio::RME::IsValidAssignedChannel(ASFW::Audio::RME::FirefaceModel::kFF400, 8));
    EXPECT_TRUE(ASFW::Audio::RME::IsValidAssignedChannel(ASFW::Audio::RME::FirefaceModel::kFF800, 63));
}

TEST(FirefaceSequenceTests, FF800UsesActualReservedPlaybackAndDeviceSelectedCaptureChannels) {
    FirefaceScriptBus bus;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, true);
    ASFW::Audio::AudioDuplexChannels channels{};
    channels.hostToDeviceIsoChannel = 3;
    const auto resources = family.GetResourcePolicy();
    EXPECT_TRUE(resources.playback.reserveHostResources);
    EXPECT_FALSE(resources.capture.reserveHostResources);
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    EXPECT_TRUE(bus.writes.empty()); // Assignment cannot be initialized before IRM playback reservation.
    auto assigned = family.AssignChannels(channels);
    ASSERT_TRUE(assigned);
    EXPECT_EQ(assigned->deviceToHostIsoChannel, 6);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[0].hi, 0U);
    EXPECT_EQ(bus.writes[0].lo, 0x801c0000U);
    EXPECT_EQ(bus.writes[0].bytes, std::vector<uint8_t>(28U * 4U, 0));
    EXPECT_EQ(bus.writes[1].hi, 2U);
    EXPECT_EQ(bus.writes[1].lo, 0x1cU);
    EXPECT_EQ(bus.writes[1].bytes, (std::vector<uint8_t>{0x80,0xbb,0,0, 3,0xe0,0,0, 0x1c,0x08,0,0}));
    ASSERT_TRUE(family.ArmDeviceTxAndEnable());
    EXPECT_EQ(bus.writes.back().lo, 0x28U);
    EXPECT_EQ(bus.writes.back().bytes, (std::vector<uint8_t>{0x1c,0x08,0,0x80}));
    EXPECT_EQ(family.Stop(), kIOReturnSuccess);
    EXPECT_EQ(bus.writes.back().lo, 0x34U);
    EXPECT_EQ(bus.writes.back().bytes, (std::vector<uint8_t>{0,0,0,0, 0,0,0,0, 0,0,0,0}));
    ASSERT_EQ(bus.writes.size(), 4U); // no mixer or clock-source writes
    EXPECT_EQ(bus.writes[2].hi, 2U);
    EXPECT_EQ(bus.writes[3].hi, 2U);
}

TEST(FirefaceSequenceTests, FF800S400TupleDoesNotSetTheS800Flag) {
    FirefaceScriptBus bus; RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false);
    ASFW::Audio::AudioDuplexChannels channels{}; channels.hostToDeviceIsoChannel = 1;
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    ASSERT_TRUE(family.AssignChannels(channels));
    EXPECT_EQ(bus.writes[1].hi, 0x0002U);
    EXPECT_EQ(bus.writes[1].lo, 0x1cU);
    EXPECT_EQ(bus.writes[1].bytes, (std::vector<uint8_t>{0x80,0xbb,0,0, 1,0xe0,0,0, 28,0,0,0}));
}

TEST(FirefaceSequenceTests, FF400RejectsOutOfRangeChannelBeforeAnyStreamingWrites) {
    FirefaceScriptBus bus;
    bus.revision = 0x146;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    const size_t afterRevisionRead = bus.writes.size();
    channels.deviceToHostIsoChannel = 8;
    EXPECT_FALSE(family.AssignChannels(channels));
    EXPECT_EQ(bus.writes.size(), afterRevisionRead); // No mask or init command on bad host assignment.
}

TEST(FirefaceSequenceTests, FF400SequenceUsesCaptureShiftAndExactStopTuple) {
    FirefaceScriptBus bus;
    bus.revision = 0x146;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false);
    ASFW::Audio::AudioDuplexChannels channels{};
    channels.hostToDeviceIsoChannel = 2;
    channels.deviceToHostIsoChannel = 5;
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    ASSERT_EQ(bus.writes.size(), 1U); // FF400 get_revision command only.
    EXPECT_EQ(bus.writes[0].lo, 0x80100520U);
    ASSERT_TRUE(family.AssignChannels(channels));
    ASSERT_EQ(bus.writes.size(), 3U);
    EXPECT_EQ(bus.writes[1].lo, 0x801c0000U);
    EXPECT_EQ(bus.writes[1].bytes, std::vector<uint8_t>(18U * 4U, 0));
    EXPECT_EQ(bus.writes[2].lo, 0x80100500U);
    EXPECT_EQ(bus.writes[2].bytes, (std::vector<uint8_t>{0x80,0xbb,0,0, 2,0x90,0,0, 0x12,0,0,0}));
    ASSERT_TRUE(family.ArmDeviceTxAndEnable());
    EXPECT_EQ(bus.writes.back().lo, 0x8010050cU);
    EXPECT_EQ(bus.writes.back().bytes, (std::vector<uint8_t>{0xb2,0,0,0x80}));
    EXPECT_EQ(family.Stop(), kIOReturnSuccess);
    EXPECT_EQ(bus.writes.back().lo, 0x80100504U);
    EXPECT_EQ(bus.writes.back().bytes, (std::vector<uint8_t>{0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0}));
    const auto policy = family.GetResourcePolicy();
    EXPECT_EQ(policy.playback.allowedIsoChannels, 0xffU);
    EXPECT_EQ(policy.capture.allowedIsoChannels, 0xffU);
    ASSERT_EQ(bus.writes.size(), 5U); // get_revision + fetch + init + start + stop only
}

TEST(FirefaceSequenceTests, FirmwareZeroOldAndReadFailureAreRejectedBeforeInit) {
    for (const auto [revision, fail] : {std::pair<uint32_t,uint64_t>{0,0}, {0x24c,0}, {0x24d,0}, {0x24d,0x000200000100ULL}}) {
        FirefaceScriptBus bus; bus.revision = revision; bus.failReadAddress = fail;
        RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
        ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false);
        auto configured = family.Configure({}, {.sampleRateHz = 48000});
        if (revision == 0x24d && fail == 0) EXPECT_TRUE(configured);
        else EXPECT_FALSE(configured);
        EXPECT_TRUE(bus.writes.empty());
    }
}

TEST(FirefaceSequenceTests, FF400FirmwareGateRejectsZeroOldAndAsyncReadErrors) {
    for (const auto [revision, fail] : {std::pair<uint32_t,uint64_t>{0,0}, {0x145,0}, {0x146,0x80100524ULL}, {0x146,0x80100290ULL}}) {
        FirefaceScriptBus bus; bus.revision = revision; bus.failReadAddress = fail;
        RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
        ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false);
        EXPECT_FALSE(family.Configure({}, {.sampleRateHz = 48000}));
        ASSERT_EQ(bus.writes.size(), 1U);
        EXPECT_EQ(bus.writes.front().lo, 0x80100520U); // only the permitted get_revision command
    }
}

TEST(FirefaceSequenceTests, ExternalSourceRequiresSelectedActiveLocked48Clock) {
    FirefaceScriptBus bus;
    bus.status1 = 0x1000U | 0x6U; // configured word clock, 48 kHz
    bus.status0 = 0x01c00000U | 0x06000000U; // active internal: not the selected external source
    RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false);
    EXPECT_FALSE(family.Configure({}, {.sampleRateHz = 48000}));
    ASSERT_EQ(bus.writes.size(), 1U);
    EXPECT_EQ(bus.writes.front().lo, 0x80100520U); // Revision query only; no fetch/init/mixer writes.

    bus.status0 = 0x01000000U | 0x20000000U | 0x40000000U | 0x06000000U;
    ASFW::Audio::RME::FirefaceFamilyDriver locked(io, ASFW::Audio::RME::FirefaceModel::kFF400, false);
    EXPECT_TRUE(locked.Configure({}, {.sampleRateHz = 48000}));
}

TEST(FirefaceSequenceTests, FF800CapturePollTimesOutAtBoundAndHonorsCancellation) {
    FirefaceScriptBus bus; bus.captureWord = 0xffffffffU;
    RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    const uint64_t before = ASFW::Audio::Session::UptimeMilliseconds();
    auto assignment = family.AssignChannels(channels);
    const uint64_t elapsed = ASFW::Audio::Session::UptimeMilliseconds() - before;
    EXPECT_FALSE(assignment);
    EXPECT_EQ(assignment.error(), kIOReturnTimeout);
    EXPECT_GE(elapsed, 490U);
    EXPECT_LT(elapsed, 530U);
    EXPECT_EQ(family.Stop(), kIOReturnSuccess);
    EXPECT_EQ(bus.writes.back().lo, 0x34U); // assignment timeout resets the partially configured device

    FirefaceScriptBus cancelBus; RouteState cancelRoute;
    ProtocolRegisterIO cancelIo(cancelBus, cancelBus, cancelRoute.registry, cancelRoute.route);
    ASFW::Audio::RME::FirefaceFamilyDriver cancellable(cancelIo, ASFW::Audio::RME::FirefaceModel::kFF800, false);
    ASSERT_TRUE(cancellable.Configure(channels, {.sampleRateHz = 48000}));
    std::atomic<bool> cancel{true}; cancellable.SetTeardownCancelToken(&cancel);
    auto cancelled = cancellable.AssignChannels(channels);
    EXPECT_FALSE(cancelled);
    EXPECT_EQ(cancelled.error(), kIOReturnAborted);
    EXPECT_EQ(cancellable.Stop(), kIOReturnSuccess);
}

TEST(FirefaceSequenceTests, BusResetDuringCaptureSelectionStopsFurtherProtocolIO) {
    FirefaceScriptBus bus; RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    bus.readHook = [&route](uint64_t address) {
        if (address == 0x801c0008ULL) route.registry.InvalidateLiveMappingsForBusReset();
    };
    auto assigned = family.AssignChannels(channels);
    EXPECT_FALSE(assigned);
    EXPECT_EQ(assigned.error(), kIOReturnOffline);
    const size_t writesAtReset = bus.writes.size();
    EXPECT_EQ(family.Stop(), kIOReturnOffline); // stale route forbids post-reset writes
    EXPECT_EQ(bus.writes.size(), writesAtReset);
}

TEST(FirefaceSequenceTests, NewRouteGenerationDiscardsOldInitStateAndReinitializes) {
    FirefaceScriptBus bus; RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    ASSERT_TRUE(family.AssignChannels(channels));
    ASSERT_EQ(bus.writes.size(), 2U);

    route.registry.InvalidateLiveMappingsForBusReset();
    ::ASFW::Discovery::ConfigROM rom{};
    rom.bib.guid = route.route.guid;
    rom.gen = Generation{2};
    rom.nodeId = 0x02;
    const auto updated = route.registry.UpsertFromROM(rom, ::ASFW::Discovery::LinkPolicy{});
    EXPECT_EQ(updated.guid, rom.bib.guid);
    const auto next = route.registry.CurrentRoute(rom.bib.guid);
    ASSERT_TRUE(next);
    io.UpdateRoute(*next);
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    auto assigned = family.AssignChannels(channels);
    ASSERT_TRUE(assigned);
    ASSERT_EQ(bus.writes.size(), 4U);
    EXPECT_EQ(bus.writes[2].lo, 0x801c0000U);
    EXPECT_EQ(bus.writes[3].lo, 0x1cU);
}

TEST(FirefaceSequenceTests, FailedFetchInitStartAndStopWritesHaveSafeRollbackBoundaries) {
    auto makeFamily = [](FirefaceScriptBus& bus, RouteState& route, ProtocolRegisterIO*& io,
                         std::unique_ptr<ProtocolRegisterIO>& owned,
                         ASFW::Audio::RME::FirefaceFamilyDriver*& family,
                         std::unique_ptr<ASFW::Audio::RME::FirefaceFamilyDriver>& ownedFamily) {
        owned = std::make_unique<ProtocolRegisterIO>(bus, bus, route.registry, route.route);
        io = owned.get();
        ownedFamily = std::make_unique<ASFW::Audio::RME::FirefaceFamilyDriver>(
            *io, ASFW::Audio::RME::FirefaceModel::kFF800, false);
        family = ownedFamily.get();
    };
    ASFW::Audio::AudioDuplexChannels channels{};
    {
        FirefaceScriptBus bus; RouteState route; ProtocolRegisterIO* io{};
        std::unique_ptr<ProtocolRegisterIO> owned; ASFW::Audio::RME::FirefaceFamilyDriver* family{};
        std::unique_ptr<ASFW::Audio::RME::FirefaceFamilyDriver> ownedFamily;
        makeFamily(bus, route, io, owned, family, ownedFamily);
        ASSERT_TRUE(family->Configure(channels, {.sampleRateHz=48000}));
        bus.failWriteAddress = 0x801c0000ULL;
        EXPECT_FALSE(family->AssignChannels(channels));
        EXPECT_EQ(bus.writes.size(), 1U); // failed fetch is not followed by init or stop traffic
        EXPECT_EQ(family->Stop(), kIOReturnSuccess);
        EXPECT_EQ(bus.writes.size(), 1U);
    }
    {
        FirefaceScriptBus bus; RouteState route; ProtocolRegisterIO* io{};
        std::unique_ptr<ProtocolRegisterIO> owned; ASFW::Audio::RME::FirefaceFamilyDriver* family{};
        std::unique_ptr<ASFW::Audio::RME::FirefaceFamilyDriver> ownedFamily;
        makeFamily(bus, route, io, owned, family, ownedFamily);
        ASSERT_TRUE(family->Configure(channels, {.sampleRateHz=48000}));
        bus.failWriteAddress = 0x00020000001cULL;
        EXPECT_FALSE(family->AssignChannels(channels));
        EXPECT_EQ(family->Stop(), kIOReturnSuccess); // uncertain init completion is reset
        EXPECT_EQ(bus.writes.back().lo, 0x34U);
    }
    {
        FirefaceScriptBus bus; RouteState route; ProtocolRegisterIO* io{};
        std::unique_ptr<ProtocolRegisterIO> owned; ASFW::Audio::RME::FirefaceFamilyDriver* family{};
        std::unique_ptr<ASFW::Audio::RME::FirefaceFamilyDriver> ownedFamily;
        makeFamily(bus, route, io, owned, family, ownedFamily);
        ASSERT_TRUE(family->Configure(channels, {.sampleRateHz=48000}));
        ASSERT_TRUE(family->AssignChannels(channels));
        bus.failWriteAddress = 0x000200000028ULL;
        EXPECT_FALSE(family->ArmDeviceTxAndEnable());
        EXPECT_EQ(family->Stop(), kIOReturnSuccess);
        EXPECT_EQ(bus.writes.back().lo, 0x34U);
    }
    {
        FirefaceScriptBus bus; RouteState route; ProtocolRegisterIO* io{};
        std::unique_ptr<ProtocolRegisterIO> owned; ASFW::Audio::RME::FirefaceFamilyDriver* family{};
        std::unique_ptr<ASFW::Audio::RME::FirefaceFamilyDriver> ownedFamily;
        makeFamily(bus, route, io, owned, family, ownedFamily);
        ASSERT_TRUE(family->Configure(channels, {.sampleRateHz=48000}));
        ASSERT_TRUE(family->AssignChannels(channels));
        ASSERT_TRUE(family->ArmDeviceTxAndEnable());
        bus.failWriteAddress = 0x000200000034ULL;
        EXPECT_EQ(family->Stop(), kIOReturnError);
        bus.failWriteAddress = 0;
        EXPECT_EQ(family->Stop(), kIOReturnSuccess);
    }
}

TEST(FirefaceSequenceTests, LateLECallbackAfterRegisterHelperDestructionUsesStableRouteRegistry) {
    FirefaceScriptBus bus;
    RouteState route;
    std::optional<AsyncStatus> result;
    bus.deferRead = true;
    {
        ProtocolRegisterIO io(bus, bus, route.registry, route.route);
        (void)io.ReadQuadLE(MakeDICEAddress(0x801c0000U),
            [&result](AsyncStatus status, uint32_t) { result = status; });
    }
    route.registry.InvalidateLiveMappingsForBusReset();
    bus.DeliverDelayed();
    EXPECT_EQ(result, AsyncStatus::kStaleGeneration);
}



} // namespace
