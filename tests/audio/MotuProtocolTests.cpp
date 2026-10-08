// SPDX-License-Identifier: Apache-2.0
//
// MotuProtocol transport-adapter tests.
//
// These pin the exact register traffic the protocol emits. The wire values were
// confirmed against a real 828mkII on 2026-07-26: a clock status read returned
// 0x00000008 (48 kHz, internal), matching the device's own front panel, and writing
// 0x00000000 moved its hardware rate display to 44.1 kHz.
//
// Async address register semantics cross-validated with Linux
// sound/firewire/motu/motu-transaction.c:74-133.

#include <gtest/gtest.h>

#include "Audio/Protocols/MOTU/MotuProtocol.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "tests/mocks/FakeTimerScheduler.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Audio::Motu::ClockStatus;
using ASFW::Audio::Motu::kAddrBase;
using ASFW::Audio::Motu::kAsyncMessageRegionStart;
using ASFW::Audio::Motu::ClockSourceV2;
using ASFW::Audio::Motu::MotuProtocol;
using ASFW::Audio::Motu::Reg;
using ASFW::Audio::DuplexPrepareResult;
using ASFW::Audio::DuplexStageResult;
using ASFW::Audio::DuplexConfirmResult;

constexpr uint32_t kUltraliteSwVersion = 0x00000dU;

constexpr uint32_t k828mk2SwVersion = 0x000003U;
constexpr uint16_t kNodeId = 0x0001U;

/// Minimal registry-backed route so the protocol's ProtocolRegisterIO sees a current
/// route. Mirrors the fixture used by the DICE protocol tests; the GUID is arbitrary
/// but carries MOTU's OUI so it reads correctly in any failure output.
struct RouteState {
    ASFW::Discovery::DeviceRegistry registry;
    ASFW::Discovery::DeviceRouteToken route{};

    RouteState() {
        ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = 0x0001F20000000001ULL;
        rom.gen = ASFW::FW::Generation{1};
        rom.nodeId = kNodeId;
        (void)registry.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
        route = *registry.CurrentRoute(rom.bib.guid);
    }
};

/// Records every transaction and replays programmed quadlet values. Completions fire
/// inline, which keeps the tests free of any scheduling assumptions.
class RecordingBus final : public ASFW::Async::IFireWireBusOps,
                           public ASFW::Async::IFireWireBusInfo {
public:
    struct Write {
        uint32_t addressLo{0};
        uint32_t value{0};
    };

    std::vector<Write> writes;
    std::vector<uint32_t> reads;
    std::optional<uint32_t> readValue;
    std::function<void(uint32_t, uint32_t)> onWrite;
    bool deferWrites{false};
    std::vector<std::function<void()>> deferredWrites;
    void CompleteNextWrite() {
        ASSERT_FALSE(deferredWrites.empty());
        auto completion = std::move(deferredWrites.front());
        deferredWrites.erase(deferredWrites.begin());
        completion();
    }
    /// Per-register replay, keyed by address low bits. Consulted before `readValue`, so a
    /// sequence touching several registers (duplex bring-up reads the optical config and
    /// the packet format) can give each one a distinct value.
    std::map<uint32_t, uint32_t> readValues;
    AsyncStatus readStatus{AsyncStatus::kSuccess};
    AsyncStatus writeStatus{AsyncStatus::kSuccess};
    AsyncStatus failWritesAfterFirst{AsyncStatus::kSuccess};

    AsyncHandle ReadBlock(ASFW::FW::Generation, ASFW::FW::NodeId, FWAddress address, uint32_t,
                          ASFW::FW::FwSpeed,
                          ASFW::Async::InterfaceCompletionCallback callback) override {
        reads.push_back(address.addressLo);
        const auto perAddress = readValues.find(address.addressLo);
        if (readStatus != AsyncStatus::kSuccess ||
            (perAddress == readValues.end() && !readValue.has_value())) {
            callback(readStatus, {});
            return AsyncHandle{1};
        }
        const uint32_t value =
            (perAddress != readValues.end()) ? perAddress->second : *readValue;
        const uint8_t payload[4] = {static_cast<uint8_t>(value >> 24),
                                    static_cast<uint8_t>(value >> 16),
                                    static_cast<uint8_t>(value >> 8),
                                    static_cast<uint8_t>(value)};
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(payload, 4));
        return AsyncHandle{1};
    }

    AsyncHandle WriteBlock(ASFW::FW::Generation, ASFW::FW::NodeId, FWAddress address,
                           std::span<const uint8_t> data, ASFW::FW::FwSpeed,
                           ASFW::Async::InterfaceCompletionCallback callback) override {
        uint32_t value = 0;
        for (const uint8_t byte : data) {
            value = (value << 8) | byte;
        }
        const bool isFirstWrite = writes.empty();
        writes.push_back(Write{address.addressLo, value});

        const AsyncStatus status =
            (!isFirstWrite && failWritesAfterFirst != AsyncStatus::kSuccess)
                ? failWritesAfterFirst
                : writeStatus;
        if (onWrite) onWrite(address.addressLo, value);
        if (deferWrites) deferredWrites.push_back([callback = std::move(callback), status] { callback(status, {}); });
        else callback(status, {});
        return AsyncHandle{1};
    }

    AsyncHandle Lock(ASFW::FW::Generation, ASFW::FW::NodeId, FWAddress, ASFW::FW::LockOp,
                     std::span<const uint8_t>, uint32_t, ASFW::FW::FwSpeed,
                     ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{1};
    }

    bool Cancel(AsyncHandle) override { return false; }

    ASFW::FW::FwSpeed GetSpeed(ASFW::FW::NodeId) const override {
        return ASFW::FW::FwSpeed::S400;
    }
    uint32_t HopCount(ASFW::FW::NodeId, ASFW::FW::NodeId) const override { return 1; }
    ASFW::FW::Generation GetGeneration() const override { return ASFW::FW::Generation{2}; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return ASFW::FW::NodeId{0}; }
};

constexpr uint32_t LowOf(Reg reg) {
    return static_cast<uint32_t>(kAddrBase) + static_cast<uint32_t>(reg);
}

//==============================================================================
// Clock status
//==============================================================================

TEST(MotuProtocolTests, DecodesTheClockStatusObservedOnHardware) {
    RecordingBus bus;
    bus.readValue = 0x00000008U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<ClockStatus> observed;
    protocol.ReadClockStatus([&](IOReturn status, ClockStatus clock) {
        EXPECT_EQ(status, kIOReturnSuccess);
        observed = clock;
    });

    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(observed->raw, 0x00000008U);
    EXPECT_EQ(observed->sampleRateHz, 48000U);
    ASSERT_TRUE(observed->source.has_value());
    EXPECT_EQ(*observed->source, ClockSourceV2::Internal);

    ASSERT_EQ(bus.reads.size(), 1U);
    EXPECT_EQ(bus.reads[0], LowOf(Reg::ClockStatusV2));
    EXPECT_EQ(protocol.CachedSampleRateHz(), 48000U);
}

TEST(MotuProtocolTests, ReportsReadFailureAndLeavesCacheUnset) {
    RecordingBus bus;
    bus.readStatus = AsyncStatus::kTimeout;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.ReadClockStatus([&](IOReturn s, ClockStatus) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnTimeout);
    EXPECT_EQ(protocol.CachedSampleRateHz(), 0U);
}

//==============================================================================
// Sample rate write (read-modify-write)
//==============================================================================

TEST(MotuProtocolTests, WritesRateWhilePreservingClockSource) {
    RecordingBus bus;
    // 48 kHz on an external ADAT clock: rate bits change, source bits must not.
    bus.readValue = 0x00000009U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.SetSampleRate(44100U, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 1U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::ClockStatusV2));
    EXPECT_EQ(bus.writes[0].value, 0x00000001U); // rate index 0, source 1 preserved
    EXPECT_EQ(protocol.CachedSampleRateHz(), 44100U);
}

TEST(MotuProtocolTests, SkipsTheWriteWhenTheRateAlreadyMatches) {
    RecordingBus bus;
    bus.readValue = 0x00000008U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.SetSampleRate(48000U, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    EXPECT_TRUE(bus.writes.empty());
}

TEST(MotuProtocolTests, RejectsAnUnsupportedRateWithoutTouchingTheDevice) {
    RecordingBus bus;
    bus.readValue = 0x00000008U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.SetSampleRate(32000U, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnUnsupported);
    EXPECT_TRUE(bus.writes.empty());
}

//==============================================================================
// Async message address registration / release
//==============================================================================

TEST(MotuProtocolTests, RegistersAsyncAddressAsHiLoPair) {
    RecordingBus bus;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    const uint64_t hostAddress = kAsyncMessageRegionStart + 0x20U;
    std::optional<IOReturn> status;
    protocol.RegisterAsyncMessageAddress(0xffc0U, hostAddress, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::AsyncAddrHi));
    EXPECT_EQ(bus.writes[0].value, (0xffc0U << 16) | 0x0000ffffU);
    EXPECT_EQ(bus.writes[1].addressLo, LowOf(Reg::AsyncAddrLo));
    EXPECT_EQ(bus.writes[1].value, 0xe0000020U);
    EXPECT_TRUE(protocol.HasRegisteredAsyncAddress());
}

TEST(MotuProtocolTests, RejectsAddressOutsideTheDeviceAcceptedRegion) {
    RecordingBus bus;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.RegisterAsyncMessageAddress(0xffc0U, 0x0000'1000'0000ULL,
                                         [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnBadArgument);
    EXPECT_TRUE(bus.writes.empty());
    EXPECT_FALSE(protocol.HasRegisteredAsyncAddress());
}

TEST(MotuProtocolTests, ReleaseZeroesBothHalves) {
    RecordingBus bus;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.ReleaseAsyncMessageAddress([&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::AsyncAddrHi));
    EXPECT_EQ(bus.writes[0].value, 0U);
    EXPECT_EQ(bus.writes[1].addressLo, LowOf(Reg::AsyncAddrLo));
    EXPECT_EQ(bus.writes[1].value, 0U);
    EXPECT_FALSE(protocol.HasRegisteredAsyncAddress());
}

TEST(MotuProtocolTests, DoesNotClaimRegistrationWhenTheSecondWriteFails) {
    RecordingBus bus;
    bus.failWritesAfterFirst = AsyncStatus::kTimeout;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.RegisterAsyncMessageAddress(0xffc0U, kAsyncMessageRegionStart,
                                         [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnTimeout);
    EXPECT_FALSE(protocol.HasRegisteredAsyncAddress());
}

TEST(MotuProtocolTests, ShutdownReleasesOnlyWhenRegistered) {
    RecordingBus bus;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    // Nothing registered: shutdown must not poke the device.
    EXPECT_EQ(protocol.Shutdown(), kIOReturnSuccess);
    EXPECT_TRUE(bus.writes.empty());

    protocol.RegisterAsyncMessageAddress(0xffc0U, kAsyncMessageRegionStart, nullptr);
    ASSERT_EQ(bus.writes.size(), 2U);
    ASSERT_TRUE(protocol.HasRegisteredAsyncAddress());

    EXPECT_EQ(protocol.Shutdown(), kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 4U);
    EXPECT_EQ(bus.writes[2].value, 0U);
    EXPECT_EQ(bus.writes[3].value, 0U);
    EXPECT_FALSE(protocol.HasRegisteredAsyncAddress());
}

//==============================================================================
// Duplex bring-up
//
// Register semantics from Linux motu-stream.c: begin_session (:62-83) writes both
// iso channels and both activation bits in one word; ensure_packet_formats
// (:201-225) sets the exclude-differed-chunks bit per direction only when that
// direction carries just its fixed chunk count, and ORs in the link speed.
//==============================================================================

namespace {

/// Optical config word (0x0c04): input mode in bits [9:8], output in [11:10].
constexpr uint32_t OpticalWord(uint32_t inMode, uint32_t outMode) {
    return ((inMode & 0x3U) << 8) | ((outMode & 0x3U) << 10);
}
constexpr uint32_t kOptNone = 0U;
constexpr uint32_t kOptAdat = 1U;
constexpr uint32_t kOptSpdif = 2U;

/// RecordingBus reports S400, whose wire code is 2 (IEEE 1394-1995 §8.4.2.4).
constexpr uint32_t kExpectedSpeedCode = 2U;

constexpr ASFW::Audio::AudioClockConfig kClock48k{.sampleRateHz = 48000U};

constexpr uint8_t kRxChannel = 5U;  // host->device (playback)
constexpr uint8_t kTxChannel = 9U;  // device->host (capture)

ASFW::Audio::AudioDuplexChannels MakeChannels() {
    ASFW::Audio::AudioDuplexChannels channels{};
    channels.hostToDeviceIsoChannel = kRxChannel;
    channels.deviceToHostIsoChannel = kTxChannel;
    return channels;
}

} // namespace

// The audio session reaches every protocol through
// IDeviceProtocol::AsFamilyDriver(). A protocol that returns nullptr there is
// simply never driven -- no bring-up, no streaming, silently. This is the single
// assertion that MOTU is reachable at all, so it guards the whole family.
TEST(MotuV2DuplexTests, ExposesItselfAsFamilyDriver) {
    RecordingBus bus;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    ASFW::Audio::IDeviceProtocol& asProtocol = protocol;
    EXPECT_NE(asProtocol.AsFamilyDriver(), nullptr);
}

TEST(MotuV2DuplexTests, ReportsChunkGeometryThroughRuntimeCaps) {
    RecordingBus bus;
    // PrepareDuplex now sets the device clock rate first, mirroring Linux's
    // snd_motu_stream_reserve_duplex (motu-stream.c:143-164), so the clock word must read back.
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    // No ADAT: both directions carry only the fixed 14 chunks the v2 models use at
    // 44.1/48 kHz (motu-protocol-v2.c:274-282, snd_motu_spec_828mk2/ultralite).
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptSpdif, kOptSpdif);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<DuplexPrepareResult> result;
    protocol.PrepareDuplex(MakeChannels(), kClock48k,
                           [&](IOReturn status, DuplexPrepareResult r) {
                               EXPECT_EQ(status, kIOReturnSuccess);
                               result = r;
                           });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->runtimeCaps.hostInputPcmChannels, 14U);
    EXPECT_EQ(result->runtimeCaps.hostOutputPcmChannels, 14U);
    EXPECT_EQ(result->runtimeCaps.sampleRateHz, 48000U);
    EXPECT_EQ(result->appliedClock.sampleRateHz, 48000U);
    // MOTU is not an AM824 stream; the slot counts must stay zero so nothing treats it
    // as one.
    EXPECT_EQ(result->runtimeCaps.deviceToHostAm824Slots, 0U);
    EXPECT_EQ(result->runtimeCaps.hostToDeviceAm824Slots, 0U);
    EXPECT_EQ(result->channels.hostToDeviceIsoChannel, kRxChannel);
    EXPECT_EQ(result->channels.deviceToHostIsoChannel, kTxChannel);
}

TEST(MotuV2DuplexTests, AdatOpticalAddsChunksToTheAffectedDirection) {
    RecordingBus bus;
    // PrepareDuplex now sets the device clock rate first, mirroring Linux's
    // snd_motu_stream_reserve_duplex (motu-stream.c:143-164), so the clock word must read back.
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    // ADAT on the optical input only: capture (TX, device->host) gains the 8 extra
    // chunks at mode 0; playback keeps the fixed baseline
    // (motu-protocol-v2.c:253-269).
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptAdat, kOptSpdif);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<DuplexPrepareResult> result;
    protocol.PrepareDuplex(MakeChannels(), kClock48k,
                           [&](IOReturn, DuplexPrepareResult r) { result = r; });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->runtimeCaps.hostInputPcmChannels, 22U);  // 14 + 8
    EXPECT_EQ(result->runtimeCaps.hostOutputPcmChannels, 14U);
}

//==============================================================================
// Published geometry (E3a): the optical config is read ahead of publication
// and the endpoint's counts come from it.
//==============================================================================

namespace {

struct GeometryCase {
    uint32_t inMode;
    uint32_t outMode;
    uint32_t inputChannels;   // capture (device->host)
    uint32_t outputChannels;  // playback (host->device)
};

// Published rates (44.1/48 kHz) are rate mode 0: ADAT adds 8 chunks.
constexpr GeometryCase kGeometryCases[] = {
    {kOptNone, kOptNone, 14, 14},   {kOptNone, kOptAdat, 14, 22},
    {kOptNone, kOptSpdif, 14, 14},  {kOptAdat, kOptNone, 22, 14},
    {kOptAdat, kOptAdat, 22, 22},   {kOptAdat, kOptSpdif, 22, 14},
    {kOptSpdif, kOptNone, 14, 14},  {kOptSpdif, kOptAdat, 14, 22},
    {kOptSpdif, kOptSpdif, 14, 14},
};

}  // namespace

TEST(MotuV2GeometryTests, EveryOpticalCombinationIsPublishedFromTheRegister) {
    for (const GeometryCase& c : kGeometryCases) {
        SCOPED_TRACE(testing::Message() << "in=" << c.inMode << " out=" << c.outMode);
        RecordingBus bus;
        bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(c.inMode, c.outMode);
        RouteState routes;
        MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

        std::optional<IOReturn> status;
        protocol.EnsureRuntimeStreamGeometry([&](IOReturn s) { status = s; });

        ASSERT_TRUE(status.has_value());
        EXPECT_EQ(*status, kIOReturnSuccess);
        ASFW::Audio::AudioStreamRuntimeCaps caps{};
        ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(caps.hostInputPcmChannels, c.inputChannels);
        EXPECT_EQ(caps.hostOutputPcmChannels, c.outputChannels);
        EXPECT_EQ(caps.deviceToHostPcmChunks, c.inputChannels);
        EXPECT_EQ(caps.hostToDevicePcmChunks, c.outputChannels);
        EXPECT_EQ(caps.sampleRateHz, 48000U);
    }
}

TEST(MotuV2GeometryTests, ReadingTheGeometryIsOneQuadletReadAndNoWrites) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptAdat, kOptAdat);
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    protocol.EnsureRuntimeStreamGeometry([](IOReturn) {});

    // The one extra transaction publication adds: the optical config, nothing else.
    ASSERT_EQ(bus.reads.size(), 1U);
    EXPECT_EQ(bus.reads[0], LowOf(Reg::InOutConfV2));
    EXPECT_TRUE(bus.writes.empty());
}

TEST(MotuV2GeometryTests, BeforeAnyReadTheCapsAreTheFixedTable) {
    RecordingBus bus;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 14U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 14U);
    EXPECT_TRUE(bus.reads.empty());
}

TEST(MotuV2GeometryTests, FailedReadReportsTheErrorAndFallsBackToTheFixedTable) {
    RecordingBus bus;
    bus.readStatus = AsyncStatus::kTimeout;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.EnsureRuntimeStreamGeometry([&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnTimeout);
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 14U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 14U);
}

TEST(MotuV2GeometryTests, FailedReReadDoesNotKeepTheEarlierAnswer) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptAdat, kOptAdat);
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);
    protocol.EnsureRuntimeStreamGeometry([](IOReturn) {});
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    ASSERT_EQ(caps.hostInputPcmChannels, 22U);

    bus.readStatus = AsyncStatus::kTimeout;
    std::optional<IOReturn> status;
    protocol.EnsureRuntimeStreamGeometry([&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_NE(*status, kIOReturnSuccess);
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 14U);  // fixed table, not the stale 22
}

TEST(MotuV2GeometryTests, ReservedOpticalEncodingPublishesTheFixedCounts) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(3U, 3U);
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.EnsureRuntimeStreamGeometry([&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 14U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 14U);
}

TEST(MotuV2GeometryTests, ARereadFollowsAnOpticalModeChange) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptSpdif, kOptSpdif);
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);
    protocol.EnsureRuntimeStreamGeometry([](IOReturn) {});

    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptAdat, kOptSpdif);
    protocol.EnsureRuntimeStreamGeometry([](IOReturn) {});

    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 22U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 14U);
}

// Configure (PrepareDuplex) shares the arithmetic with the published description,
// at every rate mode: at 2x (96 kHz) ADAT adds 4 chunks, not 8
// (motu-protocol-v2.c:253-269), and the prepared geometry wins over the
// published (1x) one while a duplex is prepared there.
TEST(MotuV2GeometryTests, PrepareAtTwiceTheRateUsesTheFourChunkAdatExtra) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000018U;  // 96 kHz, internal
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptAdat, kOptAdat);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);
    protocol.EnsureRuntimeStreamGeometry([](IOReturn) {});

    std::optional<DuplexPrepareResult> result;
    protocol.PrepareDuplex(MakeChannels(), ASFW::Audio::AudioClockConfig{.sampleRateHz = 96000U},
                           [&](IOReturn, DuplexPrepareResult r) { result = r; });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->runtimeCaps.hostInputPcmChannels, 18U);
    EXPECT_EQ(result->runtimeCaps.hostOutputPcmChannels, 18U);
    ASFW::Audio::AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 18U);
    EXPECT_EQ(caps.sampleRateHz, 96000U);
}

TEST(MotuV2GeometryTests, PrepareAtPublishedRatesAgreesWithThePublishedCounts) {
    for (const GeometryCase& c : kGeometryCases) {
        SCOPED_TRACE(testing::Message() << "in=" << c.inMode << " out=" << c.outMode);
        RecordingBus bus;
        bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U;  // 48 kHz
        bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(c.inMode, c.outMode);
        bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
        RouteState routes;
        MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);
        protocol.EnsureRuntimeStreamGeometry([](IOReturn) {});
        ASFW::Audio::AudioStreamRuntimeCaps published{};
        ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(published));

        std::optional<DuplexPrepareResult> result;
        protocol.PrepareDuplex(MakeChannels(), kClock48k,
                               [&](IOReturn, DuplexPrepareResult r) { result = r; });

        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result->runtimeCaps.hostInputPcmChannels, published.hostInputPcmChannels);
        EXPECT_EQ(result->runtimeCaps.hostOutputPcmChannels, published.hostOutputPcmChannels);
    }
}

TEST(MotuV2DuplexTests, RejectsARateWithNoChunkLayoutWithoutTouchingTheDevice) {
    RecordingBus bus;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    // 192 kHz is mode 2, whose fixed chunk count is 0 for these models -- unsupported.
    std::optional<IOReturn> status;
    protocol.PrepareDuplex(MakeChannels(), ASFW::Audio::AudioClockConfig{.sampleRateHz = 192000U},
                           [&](IOReturn s, DuplexPrepareResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnUnsupported);
    EXPECT_TRUE(bus.writes.empty());
    EXPECT_TRUE(bus.reads.empty());
}

TEST(MotuV2DuplexTests, HealthReportsLockedOnlyWhenTheClockWordDecodes) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<ASFW::Audio::DuplexHealthResult> health;
    protocol.ReadDuplexHealth([&](IOReturn status, ASFW::Audio::DuplexHealthResult r) {
        EXPECT_EQ(status, kIOReturnSuccess);
        health = r;
    });

    ASSERT_TRUE(health.has_value());
    EXPECT_TRUE(health->sourceLocked);
    EXPECT_TRUE(health->clockReferenceHealthy);
    EXPECT_EQ(health->nominalRateHz, 48000U);
}

TEST(MotuV2DuplexTests, HealthReportsUnlockedWhenTheClockReadFails) {
    RecordingBus bus;
    bus.readStatus = AsyncStatus::kTimeout;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<ASFW::Audio::DuplexHealthResult> health;
    protocol.ReadDuplexHealth(
        [&](IOReturn, ASFW::Audio::DuplexHealthResult r) { health = r; });

    // Missing evidence must never be reported as healthy, or a needed recovery is
    // suppressed.
    ASSERT_TRUE(health.has_value());
    EXPECT_FALSE(health->sourceLocked);
    EXPECT_FALSE(health->clockReferenceHealthy);
}

TEST(MotuV2DuplexTests, SetAssignedChannelsOverridesTheProvisionalIsoChannels) {
    RecordingBus bus;
    // PrepareDuplex now sets the device clock rate first, mirroring Linux's
    // snd_motu_stream_reserve_duplex (motu-stream.c:143-164), so the clock word must read back.
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptNone, kOptNone);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    bus.readValues[LowOf(Reg::IsocCommControl)] = 0U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    protocol.PrepareDuplex(MakeChannels(), kClock48k, [](IOReturn, DuplexPrepareResult) {});

    // IRM allocation replaced the provisional numbers after prepare; the device must be
    // programmed with the committed ones.
    ASFW::Audio::AudioDuplexChannels committed{};
    committed.hostToDeviceIsoChannel = 11U;
    committed.deviceToHostIsoChannel = 12U;
    protocol.SetAssignedChannels(committed);

    bus.writes.clear();
    protocol.ProgramTxAndEnableDuplex([](IOReturn, DuplexStageResult) {});

    ASSERT_EQ(bus.writes.size(), 1U);
    // 11 at shift 24, 12 at shift 16, with both change/activate pairs set.
    EXPECT_EQ(bus.writes[0].value, 0xCBCC0000U);
}

TEST(MotuV2DuplexTests, PrepareSetsBothExcludeBitsWhenOpticalIsNotAdat) {
    RecordingBus bus;
    // PrepareDuplex now sets the device clock rate first, mirroring Linux's
    // snd_motu_stream_reserve_duplex (motu-stream.c:143-164), so the clock word must read back.
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptSpdif, kOptSpdif);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.PrepareDuplex(MakeChannels(), kClock48k,
                           [&](IOReturn s, DuplexPrepareResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);

    // Optical config must be consulted before the packet format can be computed.
    // The clock rate is applied before the optical config is consulted, so its
    // read-modify-write leads both sequences.
    ASSERT_EQ(bus.reads.size(), 3U);
    EXPECT_EQ(bus.reads[0], LowOf(Reg::ClockStatusV2));
    EXPECT_EQ(bus.reads[1], LowOf(Reg::InOutConfV2));
    EXPECT_EQ(bus.reads[2], LowOf(Reg::PacketFormat));

    // The device is already at the requested rate here, and SetSampleRate skips a
    // no-op clock write, so only the packet format is written.
    ASSERT_EQ(bus.writes.size(), 1U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::PacketFormat));
    // 0x80 = TX exclude, 0x40 = RX exclude, low nibble = speed.
    EXPECT_EQ(bus.writes[0].value, 0x80U | 0x40U | kExpectedSpeedCode);
}

TEST(MotuV2DuplexTests, PrepareClearsExcludeBitsWhenOpticalIsAdat) {
    RecordingBus bus;
    // PrepareDuplex now sets the device clock rate first, mirroring Linux's
    // snd_motu_stream_reserve_duplex (motu-stream.c:143-164), so the clock word must read back.
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    // ADAT on both directions adds chunks beyond the fixed baseline, so neither
    // direction may claim the fixed layout (motu-protocol-v2.c:253-269).
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptAdat, kOptAdat);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0x000000C0U; // both bits already set
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.PrepareDuplex(MakeChannels(), kClock48k,
                           [&](IOReturn s, DuplexPrepareResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 1U); // clock already at rate; only packet format written
    EXPECT_EQ(bus.writes[0].value, kExpectedSpeedCode); // both exclude bits cleared
}

TEST(MotuV2DuplexTests, PrepareRejectsUndecodableOpticalGeometry) {
    RecordingBus bus;
    // PrepareDuplex now sets the device clock rate first, mirroring Linux's
    // snd_motu_stream_reserve_duplex (motu-stream.c:143-164), so the clock word must read back.
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    // Mode 3 is reserved; decoding fails. Claiming the fixed layout on unproven
    // evidence would truncate the stream, so both bits must stay clear.
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(3U, 3U);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0x000000C0U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.PrepareDuplex(MakeChannels(), kClock48k,
                           [&](IOReturn s, DuplexPrepareResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnUnsupported);
    EXPECT_TRUE(bus.writes.empty());
}

TEST(MotuV2DuplexTests, PrepareMovesADeviceOffAMismatchedClockRate) {
    // The regression this guards: RunDuplexStart never calls ApplyClockConfig, so if
    // PrepareDuplex does not apply the rate the device stays where it powered up. A
    // UltraLite at 44.1 kHz with the host asking for 48 kHz then fails
    // WaitForStableGlobalClock forever, which overruns CoreAudio's StartDevice budget and
    // surfaces as kIOReturnTimeout. Linux applies the rate in the reserve stage for the
    // same reason (motu-stream.c:143-164).
    RecordingBus bus;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000000U; // 44.1 kHz, internal
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptSpdif, kOptSpdif);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.PrepareDuplex(MakeChannels(), kClock48k,
                           [&](IOReturn s, DuplexPrepareResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);

    // The clock word is rewritten before the packet format, and the rate field
    // (bits [5:3]) moves to index 1 = 48 kHz while the source bits stay put.
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::ClockStatusV2));
    EXPECT_EQ(bus.writes[0].value, 0x00000008U);
    EXPECT_EQ(bus.writes[1].addressLo, LowOf(Reg::PacketFormat));
}

TEST(MotuV2DuplexTests, PrepareReportsOpticalReadFailureWithoutWriting) {
    RecordingBus bus;
    bus.readStatus = AsyncStatus::kTimeout;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.PrepareDuplex(MakeChannels(), kClock48k,
                           [&](IOReturn s, DuplexPrepareResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnTimeout);
    EXPECT_TRUE(bus.writes.empty());
}

TEST(MotuV2DuplexTests, EnableActivatesBothDirectionsWithTheirChannels) {
    RecordingBus bus;
    // PrepareDuplex now sets the device clock rate first, mirroring Linux's
    // snd_motu_stream_reserve_duplex (motu-stream.c:143-164), so the clock word must read back.
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptNone, kOptNone);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    bus.readValues[LowOf(Reg::IsocCommControl)] = 0U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    protocol.PrepareDuplex(MakeChannels(), kClock48k, [](IOReturn, DuplexPrepareResult) {});
    bus.writes.clear();

    std::optional<IOReturn> status;
    protocol.ProgramTxAndEnableDuplex([&](IOReturn s, DuplexStageResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 1U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::IsocCommControl));
    // change|activate for RX (bits 31/30) with channel 5 at shift 24, and the same
    // for TX (bits 23/22) with channel 9 at shift 16.
    EXPECT_EQ(bus.writes[0].value, 0xC5C90000U);
}

TEST(MotuV2DuplexTests, EnableBeforePrepareIsRejectedAndWritesNothing) {
    RecordingBus bus;
    bus.readValue = 0U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.ProgramTxAndEnableDuplex([&](IOReturn s, DuplexStageResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnNotReady);
    EXPECT_TRUE(bus.writes.empty());
}

TEST(MotuV2DuplexTests, ProgramRxIsASuccessfulNoOpOnV2) {
    RecordingBus bus;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.ProgramRx([&](IOReturn s, DuplexStageResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    EXPECT_TRUE(bus.writes.empty());
    EXPECT_TRUE(bus.reads.empty());
}

TEST(MotuV2DuplexTests, ConfirmAcceptsBothDirectionsOnTheExpectedChannels) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptNone, kOptNone);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    bus.readValues[LowOf(Reg::IsocCommControl)] = 0xC5C90000U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    protocol.PrepareDuplex(MakeChannels(), kClock48k, [](IOReturn, DuplexPrepareResult) {});

    std::optional<IOReturn> status;
    protocol.ConfirmDuplexStart([&](IOReturn s, DuplexConfirmResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
}

TEST(MotuV2DuplexTests, ConfirmRejectsWhenTheDeviceReportsDifferentChannels) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptNone, kOptNone);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    // Activated, but on channels 1/2 rather than the 5/9 we asked for.
    bus.readValues[LowOf(Reg::IsocCommControl)] = 0xC1C20000U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    protocol.PrepareDuplex(MakeChannels(), kClock48k, [](IOReturn, DuplexPrepareResult) {});

    std::optional<IOReturn> status;
    protocol.ConfirmDuplexStart([&](IOReturn s, DuplexConfirmResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnNotReady);
}

TEST(MotuV2DuplexTests, ConfirmRejectsWhenOnlyOneDirectionIsActivated) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptNone, kOptNone);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    // RX activated on 5, TX channel right but its activation bit clear.
    bus.readValues[LowOf(Reg::IsocCommControl)] = 0xC5890000U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    protocol.PrepareDuplex(MakeChannels(), kClock48k, [](IOReturn, DuplexPrepareResult) {});

    std::optional<IOReturn> status;
    protocol.ConfirmDuplexStart([&](IOReturn s, DuplexConfirmResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnNotReady);
}

TEST(MotuV2DuplexTests, StopDeactivatesBothDirectionsAndKeepsTheChannelFields) {
    RecordingBus bus;
    // PrepareDuplex now sets the device clock rate first, mirroring Linux's
    // snd_motu_stream_reserve_duplex (motu-stream.c:143-164), so the clock word must read back.
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptNone, kOptNone);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    bus.readValues[LowOf(Reg::IsocCommControl)] = 0xC5C90000U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    protocol.PrepareDuplex(MakeChannels(), kClock48k, [](IOReturn, DuplexPrepareResult) {});
    protocol.ProgramTxAndEnableDuplex([](IOReturn, DuplexStageResult) {});
    bus.writes.clear();

    EXPECT_EQ(protocol.StopDuplex(), kIOReturnSuccess);

    ASSERT_EQ(bus.writes.size(), 1U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::IsocCommControl));
    // Activation bits cleared, change bits set, channel numbers untouched.
    EXPECT_EQ(bus.writes[0].value, 0x85890000U);
}

TEST(MotuV2DuplexTests, StopIsANoOpWhenDuplexWasNeverEnabled) {
    RecordingBus bus;
    bus.readValue = 0U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    EXPECT_EQ(protocol.StopDuplex(), kIOReturnSuccess);
    EXPECT_TRUE(bus.writes.empty());
}


//==============================================================================
// UltraLite enablement (unit version 0x0d)
//==============================================================================

TEST(MotuV2DuplexTests, UltraLiteDefersFetchingUntilHostStarted) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptSpdif, kOptSpdif);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, kUltraliteSwVersion);

    protocol.PrepareDuplex(MakeChannels(), kClock48k, [](IOReturn, DuplexPrepareResult) {});

    EXPECT_EQ(bus.writes.size(), 1U); // packet format only; no early fetching
    bus.readValues[LowOf(Reg::IsocCommControl)] = ASFW::Audio::Motu::EncodeIsoCommStart(0, MakeChannels().hostToDeviceIsoChannel, MakeChannels().deviceToHostIsoChannel);
    protocol.ConfirmDuplexStart([](IOReturn status, DuplexConfirmResult) { EXPECT_EQ(status, kIOReturnSuccess); });
    // The Spartan models need the fetch-enable write the 828mk2 skips
    // (motu-protocol-v2.c:190-225). At 48 kHz on an internal clock the model-specific bit
    // stays clear, so only bit 25 is set.
    bool sawFetchWrite = false;
    for (const auto& w : bus.writes) {
        if (w.addressLo == LowOf(Reg::ClockStatusV2)) {
            sawFetchWrite = true;
            EXPECT_EQ(w.value & 0x02000000U, 0x02000000U) << "fetch enable not set";
            EXPECT_EQ(w.value & 0x04000000U, 0U) << "model-specific set at 48k internal";
        }
    }
    EXPECT_TRUE(sawFetchWrite) << "UltraLite must write the clock status register";
}

TEST(MotuV2DuplexTests, The828mk2SkipsTheFetchingModeWrite) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptSpdif, kOptSpdif);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    protocol.PrepareDuplex(MakeChannels(), kClock48k, [](IOReturn, DuplexPrepareResult) {});

    for (const auto& w : bus.writes) {
        EXPECT_NE(w.addressLo, LowOf(Reg::ClockStatusV2))
            << "828mk2 (Altera ACEX 1K) must not write fetching mode";
    }
}

} // namespace

//==============================================================================
// Channel labels
//==============================================================================

TEST(MotuProtocolChannelLabelTests, UltraLiteNamesFollowHostChannelOrder) {
    RecordingBus bus;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, kUltraliteSwVersion);

    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    ASSERT_TRUE(protocol.GetChannelLabels(inNames, outNames));
    ASSERT_EQ(inNames.size(), 14U);
    ASSERT_EQ(outNames.size(), 14U);
    EXPECT_EQ(inNames[0], "Mic 1");
    EXPECT_EQ(inNames[1], "Mic 2");
    EXPECT_EQ(outNames[0], "Main L");
    EXPECT_EQ(outNames[1], "Main R");
    EXPECT_EQ(outNames[12], "Phones L");
    // Labels are static, so no register traffic is needed to answer.
    EXPECT_TRUE(bus.reads.empty());
}

TEST(MotuProtocolChannelLabelTests, UnmappedModelsReportNoLabels) {
    RecordingBus bus;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, /*896HD*/ 0x000005U);

    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    EXPECT_FALSE(protocol.GetChannelLabels(inNames, outNames));
}

TEST(MotuV2DuplexTests, FamilyDriverReadsHealthAndStatesEveryStep) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    bus.readValues[LowOf(Reg::InOutConfV2)] = 0;
    RouteState routes;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);
    ASFW::Audio::FamilyDriver& family = *protocol.AsFamilyDriver();

    EXPECT_EQ(family.LoadGeometry(), kIOReturnSuccess);
    const auto health = family.ReadHealth(1000);
    ASSERT_TRUE(health.has_value());
    EXPECT_TRUE(health->sourceLocked);
    EXPECT_EQ(health->nominalRateHz, 48000U);
    // No per-direction connection to drop; StopDuplex switches both off.
    EXPECT_EQ(family.DisconnectPlayback(), kIOReturnUnsupported);
    EXPECT_EQ(family.DisconnectCapture(), kIOReturnUnsupported);
    EXPECT_EQ(family.BreakConnections(), kIOReturnUnsupported);
}

TEST(MotuV3Protocol, Current48kGeometryUsesBanksAndChangesNoClock) {
    RecordingBus bus; RouteState routes;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x100;
    bus.readValues[LowOf(Reg::OpticalBanksV3)] = 0x101;
    bus.readValues[LowOf(Reg::PacketFormat)] = 0;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, 0x15);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    ASSERT_EQ(protocol.LoadGeometry(), kIOReturnSuccess);
    const auto caps = protocol.RuntimeCaps();
    ASSERT_TRUE(caps);
    EXPECT_EQ(caps->sampleRateHz, 48000U);
    EXPECT_EQ(caps->hostInputPcmChannels, 26U);
    EXPECT_EQ(caps->hostOutputPcmChannels, 22U);
    const auto formations = protocol.RateFormations();
    ASSERT_TRUE(formations); ASSERT_EQ(formations->size(), 6U);
    EXPECT_FALSE(formations->front().hardwareValidated);
    const auto prepared = protocol.Configure(MakeChannels(), kClock48k);
    ASSERT_TRUE(prepared);
    ASSERT_EQ(bus.writes.size(), 3U); // address pair and format; no speculative rate writes
    EXPECT_EQ(bus.writes.back().addressLo, LowOf(Reg::PacketFormat));
    std::optional<IOReturn> changed;
    protocol.SetSampleRate(96000, [&](auto s) { changed = s; });
    EXPECT_EQ(changed, kIOReturnNotReady); // rate change needs a timeout scheduler
    EXPECT_EQ(bus.writes.size(), 3U);
}
TEST(MotuV3Protocol, ArmStagesUseCapturedActivateAndStreamConfiguration) {
    RecordingBus bus; RouteState routes;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x100;
    bus.readValues[LowOf(Reg::OpticalBanksV3)] = 0;
    bus.readValues[LowOf(Reg::PacketFormat)] = 0;
    bus.readValues[LowOf(Reg::IsocCommControl)] = 0x5b59; // observed low half
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, 0x15);
    ASSERT_TRUE(protocol.Configure(MakeChannels(), kClock48k));
    ASSERT_TRUE(protocol.ArmDeviceRx());
    EXPECT_EQ(bus.writes.back().value, 0x80800000U);
    ASSERT_TRUE(protocol.ArmDeviceTxAndEnable());
    ASSERT_GE(bus.writes.size(), 4U);
    EXPECT_EQ(bus.writes[bus.writes.size()-2].value & 0xffff, 0U);
    EXPECT_EQ(bus.writes.back().addressLo, LowOf(Reg::StreamConfigV3));
    EXPECT_EQ(bus.writes.back().value, 0x00120000U);
}
TEST(MotuV2DuplexTests, FetchWriteFailureIsPartOfConfirmAndStopRetriesARejectedWrite) {
    RecordingBus bus; RouteState routes;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 8;
    bus.readValues[LowOf(Reg::InOutConfV2)] = 0;
    bus.readValues[LowOf(Reg::PacketFormat)] = 0;
    bus.readValues[LowOf(Reg::IsocCommControl)] = 0;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, 13);
    ASSERT_TRUE(protocol.Configure(MakeChannels(), kClock48k));
    ASSERT_TRUE(protocol.ArmDeviceTxAndEnable());
    bus.writeStatus = AsyncStatus::kTimeout;
    EXPECT_FALSE(protocol.Confirm());
    EXPECT_EQ(protocol.Stop(), kIOReturnTimeout);
    bus.writeStatus = AsyncStatus::kSuccess;
    bus.writes.clear();
    EXPECT_EQ(protocol.Stop(), kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 2U); // mute completed, then deactivate completed
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::ClockStatusV2));
    EXPECT_EQ(bus.writes[0].value & 0x02000000U, 0U);
    EXPECT_EQ(bus.writes[1].addressLo, LowOf(Reg::IsocCommControl));
}
TEST(MotuV2GeometryTests, PublishedFormationsFollowCurrentRateAndEightPreWidths) {
    RecordingBus bus; RouteState routes;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x18; // 96k internal
    bus.readValues[LowOf(Reg::InOutConfV2)] = 0x500;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, 15);
    ASSERT_EQ(protocol.LoadGeometry(), kIOReturnSuccess);
    const auto caps = protocol.RuntimeCaps();
    ASSERT_TRUE(caps);
    EXPECT_EQ(caps->sampleRateHz, 96000U);
    EXPECT_EQ(caps->hostInputPcmChannels, 18U);
    EXPECT_EQ(caps->hostOutputPcmChannels, 14U);
    const auto forms = protocol.RateFormations();
    ASSERT_TRUE(forms); ASSERT_EQ(forms->size(), 4U);
    EXPECT_EQ(forms->back().sampleRateHz, 96000U);
    EXPECT_EQ(forms->back().capture.front().pcmChannels, 18U);
    EXPECT_EQ(forms->back().capture.front().dataBlockSize, 16U);
}

TEST(MotuV1Protocol, Original828UsesSharedClockRegisterAndTrailingCaptureStatus) {
    RecordingBus bus; RouteState routes;
    bus.readValues[LowOf(Reg::IsocCommControl)] = 0x41408004; // SPDIF input, ADAT output, 48k
    bus.readValues[LowOf(Reg::PacketFormat)] = 0;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, 1);
    ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
    ASSERT_EQ(protocol.LoadGeometry(), kIOReturnSuccess);
    auto caps = protocol.RuntimeCaps(); ASSERT_TRUE(caps);
    EXPECT_EQ(caps->sampleRateHz, 48000U);
    EXPECT_EQ(caps->hostInputPcmChannels, 10U); EXPECT_EQ(caps->hostOutputPcmChannels, 18U);
    const auto formations = protocol.RateFormations(); ASSERT_TRUE(formations);
    ASSERT_EQ(formations->size(), 2U);
    EXPECT_EQ(formations->front().capture[0].dataBlockSize, 10U);
    EXPECT_EQ(formations->front().playback[0].dataBlockSize, 15U);
    EXPECT_EQ(formations->front().packedCaptureMessageChunks, 2U);
    EXPECT_EQ(formations->front().packedPlaybackMessageChunks, 0U);
    std::optional<IOReturn> result;
    protocol.SetSampleRate(44100, [&](auto status) { result = status; });
    ASSERT_EQ(result, kIOReturnSuccess);
    EXPECT_EQ(bus.writes.back().addressLo, LowOf(Reg::IsocCommControl));
    EXPECT_EQ(bus.writes.back().value, 0x8000U); // upper command strobes cleared
    bus.writes.clear();
    protocol.SetSampleRate(96000, [&](auto status) { result = status; });
    EXPECT_EQ(result, kIOReturnUnsupported); EXPECT_TRUE(bus.writes.empty());
}
TEST(MotuV1Protocol, Original896ReservesAdatAtDoubleRateAndPreservesOutputOnAtStop) {
    RecordingBus bus; RouteState routes;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x83000018;
    bus.readValues[LowOf(Reg::PacketFormat)] = 0;
    bus.readValues[LowOf(Reg::IsocCommControl)] = 0;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, 2);
    ASSERT_EQ(protocol.LoadGeometry(), kIOReturnSuccess);
    const auto formations = protocol.RateFormations(); ASSERT_TRUE(formations);
    ASSERT_EQ(formations->size(), 4U);
    EXPECT_EQ(formations->back().sampleRateHz, 96000U);
    EXPECT_EQ(formations->back().capture[0].pcmChannels, 18U);
    EXPECT_EQ(formations->back().capture[0].dataBlockSize, 15U);
    const auto prepared = protocol.Configure(MakeChannels(), {.sampleRateHz = 96000});
    ASSERT_TRUE(prepared);
    ASSERT_EQ(protocol.Stop(), kIOReturnSuccess);
    ASSERT_GE(bus.writes.size(), 3U);
    EXPECT_EQ(bus.writes[bus.writes.size()-2].value, 0x03000018U);
    EXPECT_EQ(bus.writes.back().addressLo, LowOf(Reg::IsocCommControl));
}
TEST(MotuV3Protocol, AllFireWireModelsPublishSixRatesAndOtherModelsRefuseInitialization) {
    for (uint32_t version : {0x15U, 0x17U, 0x19U, 0x1bU}) {
        RecordingBus bus; RouteState routes;
        bus.readValues[LowOf(Reg::IsocCommControl)] = 0x1234;
        bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x300; // current 96k
        bus.readValues[LowOf(Reg::OpticalBanksV3)] = 0;
        bus.readValues[LowOf(Reg::PacketFormat)] = 0;
        MotuProtocol protocol(bus, bus, routes.registry, routes.route, version);
        ASSERT_EQ(protocol.Initialize(), kIOReturnSuccess);
        ASSERT_EQ(protocol.LoadGeometry(), kIOReturnSuccess);
        ASSERT_EQ(protocol.RateFormations()->size(), 6U);
        EXPECT_TRUE(protocol.Configure(MakeChannels(), {.sampleRateHz = 96000}));
        EXPECT_TRUE(protocol.ArmDeviceTxAndEnable());
        ASSERT_FALSE(bus.writes.empty());
        EXPECT_EQ(bus.writes.back().value & 0xffffU, version == 0x15 ? 0U : 0x1234U);
        // The observed 828mk3 48k-only word must not leak to other models/rates.
        for (const auto& write : bus.writes) EXPECT_NE(write.addressLo, LowOf(Reg::StreamConfigV3));
    }
    for (uint32_t version : {0x30U, 0x33U, 0x35U, 0x37U, 0x39U, 0x45U}) {
        RecordingBus bus; RouteState routes;
        MotuProtocol protocol(bus, bus, routes.registry, routes.route, version);
        EXPECT_EQ(protocol.Initialize(), kIOReturnUnsupported); EXPECT_TRUE(bus.writes.empty());
    }
}
TEST(MotuV3Protocol, ClockChangeRequiresMatchingNotificationAndReadbackWithBoundedTimeout) {
    using namespace ASFW::Audio::Motu;
    RecordingBus bus; RouteState routes; ASFW::Testing::FakeTimerScheduler timer;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x100;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, 0x1b, nullptr, &timer);
    std::optional<IOReturn> result; int completions = 0;
    protocol.SetSampleRate(96000, [&](auto status) { result = status; ++completions; });
    ASSERT_EQ(bus.writes.size(), 3U); EXPECT_FALSE(result);
    EXPECT_EQ(bus.writes.back().value, 0x300U);
    const uint64_t address = kAsyncMessageRegionStart | bus.writes[1].value;
    const std::array<uint8_t,4> clockChanged{0,0,0,2};
    ASFW::Async::LocalRequestContext ctx{.destOffset = address, .sourceID = kNodeId,
        .generation = routes.route.generation.value, .writePayload = clockChanged};
    auto wrong = ctx; ++wrong.generation; (void)Notifications::Handle(wrong); EXPECT_FALSE(result);
    wrong = ctx; ++wrong.sourceID; (void)Notifications::Handle(wrong); EXPECT_FALSE(result);
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x300;
    EXPECT_EQ(Notifications::Handle(ctx).rcode, ASFW::Async::ResponseCode::Complete);
    ASSERT_EQ(result, kIOReturnSuccess); EXPECT_EQ(protocol.CachedSampleRateHz(), 96000U);
    timer.Advance(4'000'000'000ULL); EXPECT_EQ(completions, 1);
    result.reset();
    protocol.SetSampleRate(192000, [&](auto status) { result = status; ++completions; });
    EXPECT_FALSE(result); timer.Advance(4'000'000'000ULL);
    EXPECT_EQ(result, kIOReturnTimeout); EXPECT_EQ(completions, 2);
    (void)Notifications::Handle(ctx); EXPECT_EQ(completions, 2); // late event cannot revive timeout
}
TEST(MotuV3Protocol, NotificationBeforeAckAndRouteCancellationAreSafe) {
    using namespace ASFW::Audio::Motu;
    RecordingBus bus; RouteState routes; ASFW::Testing::FakeTimerScheduler timer;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x100;
    MotuProtocol protocol(bus, bus, routes.registry, routes.route, 0x19, nullptr, &timer);
    int calls = 0; std::optional<IOReturn> result;
    bus.onWrite = [&](uint32_t address, uint32_t value) {
        if (address != LowOf(Reg::ClockStatusV2)) return;
        bus.readValues[address] = value;
        const std::array<uint8_t,4> changed{0,0,0,2};
        (void)Notifications::Handle({.destOffset = kAsyncMessageRegionStart | bus.writes[1].value,
            .sourceID = kNodeId, .generation = routes.route.generation.value, .writePayload = changed});
    };
    protocol.SetSampleRate(88200, [&](auto status) { result = status; ++calls; });
    EXPECT_EQ(result, kIOReturnSuccess); EXPECT_EQ(calls, 1);
    bus.onWrite = {}; result.reset();
    protocol.SetSampleRate(192000, [&](auto status) { result = status; ++calls; });
    EXPECT_FALSE(result);
    auto updated = routes.route; ++updated.routeEpoch;
    protocol.UpdateRuntimeContext(updated, nullptr);
    EXPECT_EQ(result, kIOReturnAborted);
    timer.Advance(4'000'000'000ULL); EXPECT_EQ(calls, 2);
}

TEST(MotuProtocolTests, ShutdownAddressReleaseCompletesAfterProtocolDestruction) {
    RecordingBus bus; RouteState routes;
    auto protocol = std::make_unique<MotuProtocol>(bus,bus,routes.registry,routes.route,3);
    protocol->RegisterAsyncMessageAddress(0xffc0U,kAsyncMessageRegionStart,nullptr);
    ASSERT_TRUE(protocol->HasRegisteredAsyncAddress()); bus.deferWrites = true;
    EXPECT_EQ(protocol->Shutdown(),kIOReturnSuccess);
    protocol.reset(); // both register IO completions must own their state
    bus.CompleteNextWrite(); bus.CompleteNextWrite();
    EXPECT_TRUE(bus.deferredWrites.empty()); ASSERT_EQ(bus.writes.size(),4U);
    EXPECT_EQ(bus.writes[2].value,0U); EXPECT_EQ(bus.writes[3].value,0U);
}
