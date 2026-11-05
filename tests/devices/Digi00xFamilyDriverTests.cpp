// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Digi00xFamilyDriverTests.cpp - Digi 002/003 register sequencing on a scripted
// bus. The expected orders are cross-validated with Linux
// sound/firewire/digi00x/digi00x-stream.c (reserve_duplex, start_duplex,
// begin_session, finish_session).

#include "DICEDuplexTestSupport.hpp"
#include "Audio/Protocols/Digi00x/Digi00xFamilyDriver.hpp"

#include <atomic>
#include <map>
#include <vector>

namespace {
using namespace ASFW::Testing::DICE;
using ::ASFW::Async::InterfaceCompletionCallback;
namespace Digi = ASFW::Audio::Digi00x;
namespace Reg = ASFW::Audio::Digi00x::Register;

// Big-endian register file. Reads answer from `regs`; quadlet writes update it
// (except the write-only streaming-set register) and are recorded in order.
class Digi00xScriptBus final : public IFireWireBus {
public:
    struct Write {
        uint64_t address;
        uint32_t value;
        bool operator==(const Write&) const = default;
    };

    std::map<uint64_t, uint32_t> regs{
        {Reg::kStreamingState, 0},
        {Reg::kLocalRate, 1},      // 48 kHz
        {Reg::kClockSource, 0},    // internal
        {Reg::kDetectExternal, 0},
        {Reg::kExternalRate, 0xf},
    };
    std::vector<Write> writes;
    std::vector<uint64_t> reads;
    uint64_t failWriteAddress{0};
    uint64_t failReadAddress{0};

    AsyncHandle ReadBlock(Generation, NodeId, FWAddress address, uint32_t length, FwSpeed,
                          InterfaceCompletionCallback callback) override {
        const uint64_t key = Key(address);
        reads.push_back(key);
        if (key == failReadAddress || length != 4) {
            callback(AsyncStatus::kHardwareError, {});
            return {};
        }
        const uint32_t v = regs[key];
        const std::array<uint8_t, 4> be{static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                                        static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
        callback(AsyncStatus::kSuccess, be);
        return {};
    }
    AsyncHandle WriteBlock(Generation, NodeId, FWAddress address, std::span<const uint8_t> bytes,
                           FwSpeed, InterfaceCompletionCallback callback) override {
        const uint64_t key = Key(address);
        if (bytes.size() != 4) {
            callback(AsyncStatus::kHardwareError, {});
            return {};
        }
        const uint32_t v = (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) |
                           (uint32_t{bytes[2]} << 8) | uint32_t{bytes[3]};
        writes.push_back({key, v});
        if (key == failWriteAddress) {
            callback(AsyncStatus::kHardwareError, {});
            return {};
        }
        if (key != Reg::kStreamingSet) regs[key] = v;
        callback(AsyncStatus::kSuccess, {});
        return {};
    }
    AsyncHandle Lock(Generation, NodeId, FWAddress, LockOp, std::span<const uint8_t>, uint32_t,
                     FwSpeed, InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kHardwareError, {});
        return {};
    }
    bool Cancel(AsyncHandle) override { return false; }
    FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S400; }
    uint32_t HopCount(NodeId, NodeId) const override { return 1; }
    Generation GetGeneration() const override { return Generation{1}; }
    NodeId GetLocalNodeID() const override { return NodeId{0}; }

private:
    static uint64_t Key(FWAddress a) {
        return (static_cast<uint64_t>(a.addressHi) << 32) | a.addressLo;
    }
};

struct Rig {
    Digi00xScriptBus bus;
    RouteState route;
    ProtocolRegisterIO io{bus, bus, route.registry, route.route};
    Digi::Digi00xFamilyDriver driver{io};

    static ASFW::Audio::AudioDuplexChannels Channels(uint8_t capture, uint8_t playback) {
        ASFW::Audio::AudioDuplexChannels c{};
        c.deviceToHostIsoChannel = capture;
        c.hostToDeviceIsoChannel = playback;
        return c;
    }
    // Configure, assign and enable, then forget the writes so far.
    void StartAt(uint32_t hz, uint8_t capture = 5, uint8_t playback = 6) {
        ASSERT_TRUE(driver.Configure(Channels(capture, playback), {.sampleRateHz = hz}).has_value());
        ASSERT_TRUE(driver.AssignChannels(Channels(capture, playback)).has_value());
        ASSERT_TRUE(driver.ArmDeviceRx().has_value());
        ASSERT_TRUE(driver.ArmDeviceTxAndEnable().has_value());
    }
};

using W = Digi00xScriptBus::Write;
constexpr W kStop{Reg::kStreamingSet, 3};
constexpr W kRelease{Reg::kIsochChannels, 0};

TEST(Digi00xFamilyDriver, AFullStartAndStopFollowsTheReferenceOrder) {
    Rig rig;
    rig.bus.regs[Reg::kLocalRate] = 0;  // device left at 44.1 kHz
    const auto channels = Rig::Channels(/*capture=*/5, /*playback=*/6);

    const auto prepared = rig.driver.Configure(channels, {.sampleRateHz = 48000});
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(prepared->appliedClock.sampleRateHz, 48000U);
    ASSERT_TRUE(rig.driver.AssignChannels(channels).has_value());
    ASSERT_TRUE(rig.driver.ArmDeviceRx().has_value());
    ASSERT_TRUE(rig.driver.ArmDeviceTxAndEnable().has_value());
    ASSERT_TRUE(rig.driver.Confirm().has_value());
    ASSERT_EQ(rig.driver.Stop(), kIOReturnSuccess);

    const std::vector<W> expected{
        kStop, kRelease,                 // finish any old session
        {Reg::kLocalRate, 1},            // 48 kHz
        {Reg::kIsochChannels, 0x00050006},
        {Reg::kStreamingSet, 1},         // state 0 counts as 2: one step
        kStop, kRelease,                 // Stop
    };
    EXPECT_EQ(rig.bus.writes, expected);
}

TEST(Digi00xFamilyDriver, TheRateIsNotRewrittenWhenAlreadySet) {
    Rig rig;
    ASSERT_TRUE(rig.driver.Configure(Rig::Channels(0, 1), {.sampleRateHz = 48000}).has_value());
    EXPECT_EQ(rig.bus.writes, (std::vector<W>{kStop, kRelease}));
}

TEST(Digi00xFamilyDriver, OnlySingleRatesAreStreamed) {
    for (const uint32_t hz : {88200U, 96000U, 32000U, 192000U}) {
        Rig rig;
        const auto prepared = rig.driver.Configure(Rig::Channels(0, 1), {.sampleRateHz = hz});
        ASSERT_FALSE(prepared.has_value()) << hz;
        EXPECT_EQ(prepared.error(), kIOReturnUnsupported);
        EXPECT_TRUE(rig.bus.writes.empty()) << hz;
        EXPECT_TRUE(rig.bus.reads.empty()) << hz;
    }
}

TEST(Digi00xFamilyDriver, GeometryIsEighteenPcmChannelsPlusMidi) {
    Rig rig;
    ASSERT_TRUE(rig.driver.Configure(Rig::Channels(2, 3), {.sampleRateHz = 44100}).has_value());
    const auto caps = rig.driver.RuntimeCaps();
    ASSERT_TRUE(caps.has_value());
    EXPECT_EQ(caps->sampleRateHz, 44100U);
    EXPECT_EQ(caps->hostInputPcmChannels, 18U);
    EXPECT_EQ(caps->hostOutputPcmChannels, 18U);
    EXPECT_EQ(caps->deviceToHostAm824Slots, 19U);
    EXPECT_EQ(caps->hostToDeviceAm824Slots, 19U);
    EXPECT_EQ(caps->deviceToHostStreamCount, 1U);
    EXPECT_EQ(caps->hostToDeviceStreamCount, 1U);
}

TEST(Digi00xFamilyDriver, TheCountdownStartsBelowTheCurrentState) {
    Rig rig;
    rig.bus.regs[Reg::kStreamingState] = 3;
    rig.StartAt(48000);
    std::vector<W> steps;
    for (const auto& w : rig.bus.writes)
        if (w.address == Reg::kStreamingSet && w.value != 3) steps.push_back(w);
    EXPECT_EQ(steps, (std::vector<W>{{Reg::kStreamingSet, 2}, {Reg::kStreamingSet, 1}}));
}

TEST(Digi00xFamilyDriver, AnUnexpectedStreamingStateIsRefusedAndStopStillRuns) {
    Rig rig;
    rig.bus.regs[Reg::kStreamingState] = 9;
    const auto channels = Rig::Channels(5, 6);
    ASSERT_TRUE(rig.driver.Configure(channels, {.sampleRateHz = 48000}).has_value());
    ASSERT_TRUE(rig.driver.AssignChannels(channels).has_value());
    EXPECT_FALSE(rig.driver.ArmDeviceTxAndEnable().has_value());
    rig.bus.writes.clear();
    ASSERT_EQ(rig.driver.Stop(), kIOReturnSuccess);
    EXPECT_EQ(rig.bus.writes, (std::vector<W>{kStop, kRelease}));
}

TEST(Digi00xFamilyDriver, AFailedChannelWriteIsStillReleasedOnStop) {
    Rig rig;
    rig.bus.failWriteAddress = Reg::kIsochChannels;
    const auto channels = Rig::Channels(5, 6);
    // Configure's own release fails too: finishing the old session is required.
    EXPECT_FALSE(rig.driver.Configure(channels, {.sampleRateHz = 48000}).has_value());
    rig.bus.failWriteAddress = 0;
    ASSERT_TRUE(rig.driver.Configure(channels, {.sampleRateHz = 48000}).has_value());
    rig.bus.failWriteAddress = Reg::kIsochChannels;
    EXPECT_FALSE(rig.driver.AssignChannels(channels).has_value());
    rig.bus.failWriteAddress = 0;
    rig.bus.writes.clear();
    ASSERT_EQ(rig.driver.Stop(), kIOReturnSuccess);
    EXPECT_EQ(rig.bus.writes, (std::vector<W>{kStop, kRelease}));
}

TEST(Digi00xFamilyDriver, InternalClockIsHealthyAtTheProgrammedRate) {
    Rig rig;
    rig.StartAt(48000);
    const auto h = rig.driver.ReadHealth(0);
    ASSERT_TRUE(h.has_value());
    EXPECT_TRUE(h->sourceLocked);
    EXPECT_EQ(h->nominalRateHz, 48000U);
    EXPECT_TRUE(rig.driver.Confirm().has_value());
}

TEST(Digi00xFamilyDriver, ExternalClockNeedsSignalAndAMatchingRate) {
    Rig rig;
    rig.bus.regs[Reg::kClockSource] = 1;  // S/PDIF
    rig.StartAt(48000);

    rig.bus.regs[Reg::kDetectExternal] = 1;
    rig.bus.regs[Reg::kExternalRate] = 1;  // 48 kHz
    EXPECT_TRUE(rig.driver.ReadHealth(0)->sourceLocked);

    rig.bus.regs[Reg::kExternalRate] = 0xf;  // desync
    EXPECT_FALSE(rig.driver.ReadHealth(0)->sourceLocked);
    const auto confirmed = rig.driver.Confirm();
    ASSERT_FALSE(confirmed.has_value());
    EXPECT_EQ(confirmed.error(), kIOReturnNotReady);

    rig.bus.regs[Reg::kExternalRate] = 0;  // locked, but at 44.1 kHz
    EXPECT_FALSE(rig.driver.ReadHealth(0)->sourceLocked);

    rig.bus.regs[Reg::kExternalRate] = 1;
    rig.bus.regs[Reg::kDetectExternal] = 0;  // no signal
    EXPECT_FALSE(rig.driver.ReadHealth(0)->sourceLocked);
}

TEST(Digi00xFamilyDriver, StagesOutOfOrderAreRefused) {
    Rig rig;
    EXPECT_FALSE(rig.driver.AssignChannels(Rig::Channels(0, 1)).has_value());
    EXPECT_FALSE(rig.driver.ArmDeviceRx().has_value());
    EXPECT_FALSE(rig.driver.ArmDeviceTxAndEnable().has_value());
    EXPECT_TRUE(rig.bus.writes.empty());
}

TEST(Digi00xFamilyDriver, StopBeforeAnythingSendsNothing) {
    Rig rig;
    EXPECT_EQ(rig.driver.Stop(), kIOReturnSuccess);
    EXPECT_TRUE(rig.bus.writes.empty());
}

TEST(Digi00xFamilyDriver, TheClockCanOnlyChangeWhileIdle) {
    Rig rig;
    rig.StartAt(48000);
    const auto busy = rig.driver.ApplyClockIdle({.sampleRateHz = 44100});
    ASSERT_FALSE(busy.has_value());
    EXPECT_EQ(busy.error(), kIOReturnBusy);
    ASSERT_EQ(rig.driver.Stop(), kIOReturnSuccess);
    rig.bus.writes.clear();
    const auto applied = rig.driver.ApplyClockIdle({.sampleRateHz = 44100});
    ASSERT_TRUE(applied.has_value());
    EXPECT_EQ(applied->appliedClock.sampleRateHz, 44100U);
    EXPECT_EQ(rig.bus.writes, (std::vector<W>{{Reg::kLocalRate, 0}}));
}

TEST(Digi00xFamilyDriver, TeardownCancelStopsAllTraffic) {
    Rig rig;
    std::atomic<bool> cancel{true};
    rig.driver.SetTeardownCancelToken(&cancel);
    const auto prepared = rig.driver.Configure(Rig::Channels(0, 1), {.sampleRateHz = 48000});
    ASSERT_FALSE(prepared.has_value());
    EXPECT_EQ(prepared.error(), kIOReturnAborted);
    EXPECT_TRUE(rig.bus.writes.empty());
}

TEST(Digi00xFamilyDriver, ResourcesAreReservedByTheHostBothWays) {
    Rig rig;
    const auto policy = rig.driver.GetResourcePolicy();
    EXPECT_TRUE(policy.playback.reserveHostResources);
    EXPECT_TRUE(policy.capture.reserveHostResources);
    EXPECT_TRUE(rig.driver.GetStopPolicy().stopHostContextsBeforeDevice);
}

} // namespace
