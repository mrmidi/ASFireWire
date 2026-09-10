// SPDX-License-Identifier: Apache-2.0
//
// MotuV2Protocol transport-adapter tests.
//
// These pin the exact register traffic the protocol emits. The wire values were
// confirmed against a real 828mkII on 2026-07-26: a clock status read returned
// 0x00000008 (48 kHz, internal), matching the device's own front panel, and writing
// 0x00000000 moved its hardware rate display to 44.1 kHz.
//
// Async address register semantics cross-validated with Linux
// sound/firewire/motu/motu-transaction.c:74-133.

#include <gtest/gtest.h>

#include "Audio/Protocols/MOTU/MotuV2Protocol.hpp"
#include "Discovery/DeviceRegistry.hpp"

#include <map>
#include <optional>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Audio::Motu::ClockStatus;
using ASFW::Audio::Motu::kAddrBase;
using ASFW::Audio::Motu::kAsyncMessageRegionStart;
using ASFW::Audio::Motu::ClockSourceV2;
using ASFW::Audio::Motu::MotuV2Protocol;
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
        callback(status, {});
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

TEST(MotuV2ProtocolTests, DecodesTheClockStatusObservedOnHardware) {
    RecordingBus bus;
    bus.readValue = 0x00000008U;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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

TEST(MotuV2ProtocolTests, ReportsReadFailureAndLeavesCacheUnset) {
    RecordingBus bus;
    bus.readStatus = AsyncStatus::kTimeout;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.ReadClockStatus([&](IOReturn s, ClockStatus) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnTimeout);
    EXPECT_EQ(protocol.CachedSampleRateHz(), 0U);
}

//==============================================================================
// Sample rate write (read-modify-write)
//==============================================================================

TEST(MotuV2ProtocolTests, WritesRateWhilePreservingClockSource) {
    RecordingBus bus;
    // 48 kHz on an external ADAT clock: rate bits change, source bits must not.
    bus.readValue = 0x00000009U;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.SetSampleRate(44100U, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 1U);
    EXPECT_EQ(bus.writes[0].addressLo, LowOf(Reg::ClockStatusV2));
    EXPECT_EQ(bus.writes[0].value, 0x00000001U); // rate index 0, source 1 preserved
    EXPECT_EQ(protocol.CachedSampleRateHz(), 44100U);
}

TEST(MotuV2ProtocolTests, SkipsTheWriteWhenTheRateAlreadyMatches) {
    RecordingBus bus;
    bus.readValue = 0x00000008U;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.SetSampleRate(48000U, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    EXPECT_TRUE(bus.writes.empty());
}

TEST(MotuV2ProtocolTests, RejectsAnUnsupportedRateWithoutTouchingTheDevice) {
    RecordingBus bus;
    bus.readValue = 0x00000008U;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.SetSampleRate(32000U, [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnUnsupported);
    EXPECT_TRUE(bus.writes.empty());
}

//==============================================================================
// Async message address registration / release
//==============================================================================

TEST(MotuV2ProtocolTests, RegistersAsyncAddressAsHiLoPair) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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

TEST(MotuV2ProtocolTests, RejectsAddressOutsideTheDeviceAcceptedRegion) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.RegisterAsyncMessageAddress(0xffc0U, 0x0000'1000'0000ULL,
                                         [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnBadArgument);
    EXPECT_TRUE(bus.writes.empty());
    EXPECT_FALSE(protocol.HasRegisteredAsyncAddress());
}

TEST(MotuV2ProtocolTests, ReleaseZeroesBothHalves) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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

TEST(MotuV2ProtocolTests, DoesNotClaimRegistrationWhenTheSecondWriteFails) {
    RecordingBus bus;
    bus.failWritesAfterFirst = AsyncStatus::kTimeout;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.RegisterAsyncMessageAddress(0xffc0U, kAsyncMessageRegionStart,
                                         [&](IOReturn s) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnTimeout);
    EXPECT_FALSE(protocol.HasRegisteredAsyncAddress());
}

TEST(MotuV2ProtocolTests, ShutdownReleasesOnlyWhenRegistered) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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

// The coordinator reaches every protocol through
// IDeviceProtocol::AsDuplexDeviceControl(). A protocol that returns nullptr there is
// simply never driven -- no bring-up, no streaming, silently. This is the single
// assertion that MOTU is reachable at all, so it guards the whole family.
TEST(MotuV2DuplexTests, ExposesItselfAsDuplexDeviceControl) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    ASFW::Audio::IDeviceProtocol& asProtocol = protocol;
    EXPECT_NE(asProtocol.AsDuplexDeviceControl(), nullptr);

    const ASFW::Audio::IDeviceProtocol& asConstProtocol = protocol;
    EXPECT_NE(asConstProtocol.AsDuplexDeviceControl(), nullptr);
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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<DuplexPrepareResult> result;
    protocol.PrepareDuplex(MakeChannels(), kClock48k,
                           [&](IOReturn, DuplexPrepareResult r) { result = r; });

    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->runtimeCaps.hostInputPcmChannels, 22U);  // 14 + 8
    EXPECT_EQ(result->runtimeCaps.hostOutputPcmChannels, 14U);
}

TEST(MotuV2DuplexTests, RejectsARateWithNoChunkLayoutWithoutTouchingTheDevice) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.PrepareDuplex(MakeChannels(), kClock48k,
                           [&](IOReturn s, DuplexPrepareResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 1U); // clock already at rate; only packet format written
    EXPECT_EQ(bus.writes[0].value, kExpectedSpeedCode); // both exclude bits cleared
}

TEST(MotuV2DuplexTests, PrepareTreatsUndecodableOpticalConfigAsDifferedChunks) {
    RecordingBus bus;
    // PrepareDuplex now sets the device clock rate first, mirroring Linux's
    // snd_motu_stream_reserve_duplex (motu-stream.c:143-164), so the clock word must read back.
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    // Mode 3 is reserved; decoding fails. Claiming the fixed layout on unproven
    // evidence would truncate the stream, so both bits must stay clear.
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(3U, 3U);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0x000000C0U;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.PrepareDuplex(MakeChannels(), kClock48k,
                           [&](IOReturn s, DuplexPrepareResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 1U); // clock already at rate; only packet format written
    EXPECT_EQ(bus.writes[0].value, kExpectedSpeedCode);
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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    std::optional<IOReturn> status;
    protocol.ProgramTxAndEnableDuplex([&](IOReturn s, DuplexStageResult) { status = s; });

    ASSERT_TRUE(status.has_value());
    EXPECT_EQ(*status, kIOReturnNotReady);
    EXPECT_TRUE(bus.writes.empty());
}

TEST(MotuV2DuplexTests, ProgramRxIsASuccessfulNoOpOnV2) {
    RecordingBus bus;
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    EXPECT_EQ(protocol.StopDuplex(), kIOReturnSuccess);
    EXPECT_TRUE(bus.writes.empty());
}


//==============================================================================
// UltraLite enablement (unit version 0x0d)
//==============================================================================

TEST(MotuV2DuplexTests, UltraLiteWritesFetchingModeDuringPrepare) {
    RecordingBus bus;
    bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptSpdif, kOptSpdif);
    bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
    bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U; // 48 kHz, internal
    RouteState routes;
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, kUltraliteSwVersion);

    protocol.PrepareDuplex(MakeChannels(), kClock48k, [](IOReturn, DuplexPrepareResult) {});

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
    MotuV2Protocol protocol(bus, bus, routes.registry, routes.route, k828mk2SwVersion);

    protocol.PrepareDuplex(MakeChannels(), kClock48k, [](IOReturn, DuplexPrepareResult) {});

    for (const auto& w : bus.writes) {
        EXPECT_NE(w.addressLo, LowOf(Reg::ClockStatusV2))
            << "828mk2 (Altera ACEX 1K) must not write fetching mode";
    }
}

} // namespace
