// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#include <gtest/gtest.h>

#include "ASFWDriver/Audio/Protocols/MOTU/MOTU828Mk3Protocol.hpp"
#include "ASFWDriver/Common/WireFormat.hpp"
#include "ASFWDriver/Logging/LogRing.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "tests/mocks/FakeFireWireBus.hpp"
#include "tests/mocks/FakeTimerScheduler.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::InterfaceCompletionCallback;
using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::AudioDuplexChannels;
using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::DuplexRestartPhase;
using ASFW::Audio::FamilyDriver;
using ASFW::Audio::MOTU::BuildIsocControlWords;
using ASFW::Audio::MOTU::BuildStopIsocControl;
using ASFW::Audio::MOTU::kAudioBankControlOffset;
using ASFW::Audio::MOTU::kClockStatusOffset;
using ASFW::Audio::MOTU::kFetchPcmFrames;
using ASFW::Audio::MOTU::kIsocControlOffset;
using ASFW::Audio::MOTU::kRegisterAddressBase;
using ASFW::Audio::MOTU::MOTU828Mk3Protocol;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::NodeId;
using ASFW::Testing::FakeTimerScheduler;

class RecordingBus final : public ASFW::Async::Fakes::FakeFireWireBus {
public:
    struct Write final {
        uint32_t addressLo{0};
        uint32_t value{0};
    };

    AsyncHandle WriteBlock(Generation generation,
                           NodeId nodeId,
                           FWAddress address,
                           std::span<const uint8_t> payload,
                           FwSpeed speed,
                           InterfaceCompletionCallback callback) override {
        (void)generation;
        (void)nodeId;
        (void)speed;
        if (payload.size() >= sizeof(uint32_t)) {
            writes.push_back(Write{
                .addressLo = address.addressLo,
                .value = ASFW::FW::ReadBE32(payload.data()),
            });
        }
        callback(failWrites ? AsyncStatus::kTimeout : AsyncStatus::kSuccess, {});
        return AsyncHandle{++nextHandle_};
    }

    std::vector<Write> writes;
    bool failWrites{false};

private:
    uint32_t nextHandle_{0};
};

void SetQuad(RecordingBus& bus, uint8_t nodeId, uint32_t offset, uint32_t value) {
    std::vector<uint8_t> payload(sizeof(uint32_t));
    ASFW::FW::WriteBE32(payload.data(), value);
    bus.SetMemory(nodeId, kRegisterAddressBase + offset, std::move(payload));
}

AudioDuplexChannels TestChannels() {
    AudioDuplexChannels channels{};
    channels.deviceToHostIsoChannel = 3;
    channels.hostToDeviceIsoChannel = 2;
    channels.captureIsoChannels[0] = 3;
    channels.playbackIsoChannels[0] = 2;
    return channels;
}


// The register IO now addresses the device through a route token instead of a
// bare node id, so every test needs a registry whose current route matches the
// bus generation -- a stale route short-circuits each transaction with
// kStaleGeneration before it ever reaches the fake bus.
struct RouteState {
    ASFW::Discovery::DeviceRegistry registry;
    ASFW::Discovery::DeviceRouteToken route{};

    // The generation must match the fake bus: the route token carries it into
    // every transaction, and a mismatch is rejected as stale before the bus is
    // ever asked.
    explicit RouteState(uint32_t generation = 7) {
        ASFW::Discovery::ConfigROM rom{};
        rom.bib.guid = 0x0001F2FF00000001ULL;
        rom.gen = Generation{generation};
        rom.nodeId = 0x02;
        (void)registry.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
        route = *registry.CurrentRoute(rom.bib.guid);
    }
};

// A timer that defers to another thread, as the dext's Default queue does
// while the session thread blocks in a FamilyDriver step. The virtual-clock
// fake cannot serve here: AwaitStage blocks the only thread that could advance
// it.
class QueuedTimerScheduler final : public ASFW::Scheduling::ITimerScheduler {
public:
    [[nodiscard]] ASFW::Scheduling::TimerToken ScheduleAfter(
        uint64_t delayNs, std::function<void()> fn) override {
        std::lock_guard lock(mutex_);
        lastDelayNs_ = delayNs;
        pending_.push_back(Entry{++nextToken_, std::move(fn)});
        return nextToken_;
    }

    void Cancel(ASFW::Scheduling::TimerToken token) override {
        std::lock_guard lock(mutex_);
        std::erase_if(pending_, [token](const Entry& e) { return e.token == token; });
    }

    [[nodiscard]] uint64_t NowNs() const noexcept override { return 0; }

    // Runs the next scheduled callback, waiting up to a second for one.
    bool RunNext() {
        for (int attempt = 0; attempt < 1000; ++attempt) {
            std::function<void()> fn;
            {
                std::lock_guard lock(mutex_);
                if (!pending_.empty()) {
                    fn = std::move(pending_.front().fn);
                    pending_.pop_front();
                }
            }
            if (fn) {
                fn();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }

    [[nodiscard]] uint64_t LastDelayNs() {
        std::lock_guard lock(mutex_);
        return lastDelayNs_;
    }

private:
    struct Entry {
        ASFW::Scheduling::TimerToken token;
        std::function<void()> fn;
    };
    std::mutex mutex_;
    std::deque<Entry> pending_;
    ASFW::Scheduling::TimerToken nextToken_{0};
    uint64_t lastDelayNs_{0};
};

std::vector<std::string> RingMessagesContaining(uint64_t afterSequence, const char* needle) {
    ASFW::Logging::LogRingQuery query{};
    query.afterSequence = afterSequence;
    std::strncpy(query.contains, needle, sizeof(query.contains) - 1);
    std::vector<ASFW::Logging::LogRecord> records(64);
    std::vector<std::string> messages;
    for (;;) {
        const auto result = ASFW::Logging::LogRing::Shared().Query(
            query, records.data(), static_cast<uint32_t>(records.size()));
        for (uint32_t i = 0; i < result.recordCount; ++i) {
            messages.emplace_back(records[i].message);
        }
        if (result.nextSequence > result.latestSequence ||
            result.nextSequence == query.afterSequence) {
            break;
        }
        query.afterSequence = result.nextSequence;
    }
    return messages;
}

} // namespace

TEST(Motu828Mk3ProtocolTests, RunsReferenceOrderedDuplexLifecycleAt48k) {
    RecordingBus bus;
    bus.SetGeneration(Generation{7});
    bus.SetSpeed(NodeId{2}, FwSpeed::S400);
    FakeTimerScheduler scheduler;
    RouteState routes;
    MOTU828Mk3Protocol protocol(bus, bus, routes.registry, routes.route,
                                nullptr, &scheduler);

    SetQuad(bus, 2, kClockStatusOffset, 0x00000100u);
    SetQuad(bus, 2, kAudioBankControlOffset, 0x00000000u);
    IOReturn geometryStatus = kIOReturnNotReady;
    protocol.EnsureRuntimeStreamGeometry(
        [&geometryStatus](IOReturn status) { geometryStatus = status; });
    ASSERT_EQ(geometryStatus, kIOReturnSuccess);

    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(protocol.GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.sampleRateHz, 48000u);
    EXPECT_EQ(caps.hostInputPcmChannels, 18u);
    EXPECT_EQ(caps.hostOutputPcmChannels, 14u);
    EXPECT_EQ(caps.deviceToHostAm824Slots, 16u);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, 13u);

    const AudioDuplexChannels channels = TestChannels();
    IOReturn prepareStatus = kIOReturnNotReady;
    protocol.PrepareDuplex(
        channels, AudioClockConfig{.sampleRateHz = 48000},
        [&prepareStatus](IOReturn status, auto) { prepareStatus = status; });
    ASSERT_EQ(prepareStatus, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 5u);

    constexpr uint32_t kInitialIsoc = 0x00005b59u;
    const auto words = BuildIsocControlWords(kInitialIsoc, channels);
    ASSERT_TRUE(words.has_value());
    SetQuad(bus, 2, kIsocControlOffset, kInitialIsoc);

    bool rxCompleted = false;
    IOReturn rxStatus = kIOReturnNotReady;
    protocol.ProgramRx([&](IOReturn status, auto result) {
        rxCompleted = true;
        rxStatus = status;
        EXPECT_EQ(result.phase, DuplexRestartPhase::kDeviceRxProgrammed);
    });
    ASSERT_FALSE(rxCompleted);
    ASSERT_EQ(bus.writes.size(), 6u);
    EXPECT_EQ(bus.writes.back().addressLo, kRegisterAddressBase + kIsocControlOffset);
    EXPECT_EQ(bus.writes.back().value, words->deactivate);
    scheduler.Advance(19ULL * 1000ULL * 1000ULL);
    EXPECT_FALSE(rxCompleted);
    scheduler.Advance(1ULL * 1000ULL * 1000ULL);
    ASSERT_TRUE(rxCompleted);
    EXPECT_EQ(rxStatus, kIOReturnSuccess);

    IOReturn txStatus = kIOReturnNotReady;
    protocol.ProgramTxAndEnableDuplex([&](IOReturn status, auto result) {
        txStatus = status;
        EXPECT_EQ(result.phase, DuplexRestartPhase::kDeviceTxArmed);
    });
    ASSERT_EQ(txStatus, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 8u);
    EXPECT_EQ(bus.writes[6].addressLo, kRegisterAddressBase + kIsocControlOffset);
    EXPECT_EQ(bus.writes[6].value, words->activate);
    EXPECT_EQ(bus.writes[7].addressLo, kRegisterAddressBase + 0x0b1cu);
    EXPECT_EQ(bus.writes[7].value, 0x00120000u);

    SetQuad(bus, 2, kIsocControlOffset, words->activate);
    SetQuad(bus, 2, kClockStatusOffset, 0x00000100u);
    IOReturn confirmStatus = kIOReturnNotReady;
    protocol.ConfirmDuplexStart(
        [&confirmStatus](IOReturn status, auto) { confirmStatus = status; });
    ASSERT_EQ(confirmStatus, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 9u);
    EXPECT_EQ(bus.writes.back().addressLo, kRegisterAddressBase + kClockStatusOffset);
    EXPECT_EQ(bus.writes.back().value, 0x00000100u | kFetchPcmFrames);

    SetQuad(bus, 2, kClockStatusOffset, 0x00000100u | kFetchPcmFrames);
    IOReturn playbackStop = kIOReturnNotReady;
    protocol.DisconnectPlayback(
        [&playbackStop](IOReturn status) { playbackStop = status; });
    ASSERT_EQ(playbackStop, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 10u);
    EXPECT_EQ(bus.writes.back().value, 0x00000100u);

    SetQuad(bus, 2, kIsocControlOffset, words->activate);
    IOReturn captureStop = kIOReturnNotReady;
    protocol.DisconnectCapture(
        [&captureStop](IOReturn status) { captureStop = status; });
    ASSERT_EQ(captureStop, kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 11u);
    EXPECT_EQ(bus.writes.back().value, BuildStopIsocControl(words->activate));
}

// The session drives the protocol only through FamilyDriver.
// Each step must put the same writes on the bus as the callback chain above.
TEST(Motu828Mk3ProtocolTests, FamilyDriverStepsWriteTheCallbackChainSequence) {
    auto& ring = ASFW::Logging::LogRing::Shared();
    ring.Initialize();
    RecordingBus bus;
    bus.SetGeneration(Generation{7});
    bus.SetSpeed(NodeId{2}, FwSpeed::S400);
    QueuedTimerScheduler scheduler;
    RouteState routes;
    MOTU828Mk3Protocol protocol(bus, bus, routes.registry, routes.route,
                                nullptr, &scheduler);
    FamilyDriver* family = protocol.AsFamilyDriver();
    ASSERT_EQ(family, static_cast<FamilyDriver*>(&protocol));

    // LoadGeometry reads the device, unlike V2's.
    SetQuad(bus, 2, kClockStatusOffset, 0x00000100u);
    SetQuad(bus, 2, kAudioBankControlOffset, 0x00000000u);
    EXPECT_FALSE(family->RuntimeCaps().has_value());
    ASSERT_EQ(family->LoadGeometry(), kIOReturnSuccess);
    const auto caps = family->RuntimeCaps();
    ASSERT_TRUE(caps.has_value());
    EXPECT_EQ(caps->sampleRateHz, 48000u);
    EXPECT_EQ(caps->hostInputPcmChannels, 18u);
    EXPECT_EQ(caps->hostOutputPcmChannels, 14u);
    EXPECT_TRUE(bus.writes.empty());

    const AudioDuplexChannels channels = TestChannels();
    const auto prepared = family->Configure(channels, AudioClockConfig{.sampleRateHz = 48000});
    ASSERT_TRUE(prepared.has_value());
    EXPECT_EQ(prepared->runtimeCaps.hostInputPcmChannels, 18u);
    ASSERT_EQ(bus.writes.size(), 5u);
    const auto assigned = family->AssignChannels(channels);
    ASSERT_TRUE(assigned.has_value());
    EXPECT_EQ(assigned->hostToDeviceIsoChannel, 2u);

    constexpr uint32_t kInitialIsoc = 0x00005b59u;
    const auto words = BuildIsocControlWords(kInitialIsoc, channels);
    ASSERT_TRUE(words.has_value());
    SetQuad(bus, 2, kIsocControlOffset, kInitialIsoc);
    bool settleFired = false;
    std::thread timerQueue([&] { settleFired = scheduler.RunNext(); });
    const auto rx = family->ArmDeviceRx();
    timerQueue.join();
    ASSERT_TRUE(settleFired);
    ASSERT_TRUE(rx.has_value());
    EXPECT_EQ(rx->phase, DuplexRestartPhase::kDeviceRxProgrammed);
    EXPECT_EQ(scheduler.LastDelayNs(), 20ULL * 1000ULL * 1000ULL);
    ASSERT_EQ(bus.writes.size(), 6u);
    EXPECT_EQ(bus.writes.back().value, words->deactivate);

    const auto tx = family->ArmDeviceTxAndEnable();
    ASSERT_TRUE(tx.has_value());
    EXPECT_EQ(tx->phase, DuplexRestartPhase::kDeviceTxArmed);
    ASSERT_EQ(bus.writes.size(), 8u);
    EXPECT_EQ(bus.writes[6].value, words->activate);
    EXPECT_EQ(bus.writes[7].addressLo, kRegisterAddressBase + 0x0b1cu);
    EXPECT_EQ(bus.writes[7].value, 0x00120000u);

    SetQuad(bus, 2, kIsocControlOffset, words->activate);
    const uint64_t cursor = ring.Stats().latestSequence;
    const auto confirmed = family->Confirm();
    ASSERT_TRUE(confirmed.has_value());
    ASSERT_EQ(bus.writes.size(), 9u);
    EXPECT_EQ(bus.writes.back().value, 0x00000100u | kFetchPcmFrames);
    // The seed-window time anchor brackets the fetch-enable write.
    const auto begin = RingMessagesContaining(cursor, "ConfirmDuplexStart BEGIN");
    ASSERT_EQ(begin.size(), 1U);
    EXPECT_NE(begin[0].find("GUID=1f2ff00000001"), std::string::npos) << begin[0];
    const auto end = RingMessagesContaining(cursor, "ConfirmDuplexStart END");
    ASSERT_EQ(end.size(), 1U);
    EXPECT_NE(end[0].find("GUID=1f2ff00000001 status=0x00000000"), std::string::npos) << end[0];

    const auto health = family->ReadHealth(1000);
    ASSERT_TRUE(health.has_value());
    EXPECT_TRUE(health->sourceLocked);

    // The interleaved stop: each disconnect works, one register each.
    SetQuad(bus, 2, kClockStatusOffset, 0x00000100u | kFetchPcmFrames);
    EXPECT_EQ(family->DisconnectPlayback(), kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 10u);
    EXPECT_EQ(bus.writes.back().addressLo, kRegisterAddressBase + kClockStatusOffset);
    EXPECT_EQ(bus.writes.back().value, 0x00000100u);
    EXPECT_EQ(family->DisconnectCapture(), kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 11u);
    EXPECT_EQ(bus.writes.back().addressLo, kRegisterAddressBase + kIsocControlOffset);
    EXPECT_EQ(bus.writes.back().value, BuildStopIsocControl(words->activate));

    // Stop and BreakConnections both switch off both directions.
    EXPECT_EQ(family->Stop(), kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 13u);
    EXPECT_EQ(bus.writes[11].addressLo, kRegisterAddressBase + kClockStatusOffset);
    EXPECT_EQ(bus.writes[12].addressLo, kRegisterAddressBase + kIsocControlOffset);
    EXPECT_EQ(family->BreakConnections(), kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 15u);
    EXPECT_EQ(bus.writes[13].addressLo, kRegisterAddressBase + kClockStatusOffset);
    EXPECT_EQ(bus.writes[14].addressLo, kRegisterAddressBase + kIsocControlOffset);
}

// StopRoutine counts kIOReturnUnsupported as a successful device stop, so a
// Stop() that answered it would hide a device left streaming.
TEST(Motu828Mk3ProtocolTests, FamilyDriverStopReportsAFailedDeviceWrite) {
    RecordingBus bus;
    bus.SetGeneration(Generation{7});
    FakeTimerScheduler scheduler;
    RouteState routes;
    MOTU828Mk3Protocol protocol(bus, bus, routes.registry, routes.route,
                                nullptr, &scheduler);
    SetQuad(bus, 2, kClockStatusOffset, 0x00000100u | kFetchPcmFrames);
    SetQuad(bus, 2, kIsocControlOffset, 0x00005b59u);
    bus.failWrites = true;

    const IOReturn stopped = protocol.AsFamilyDriver()->Stop();
    EXPECT_NE(stopped, kIOReturnSuccess);
    EXPECT_NE(stopped, kIOReturnUnsupported);
    EXPECT_NE(protocol.AsFamilyDriver()->BreakConnections(), kIOReturnSuccess);
}

TEST(Motu828Mk3ProtocolTests, FamilyDriverConfirmMarkerCarriesTheFailure) {
    auto& ring = ASFW::Logging::LogRing::Shared();
    ring.Initialize();
    RecordingBus bus;
    bus.SetGeneration(Generation{7});
    FakeTimerScheduler scheduler;
    RouteState routes;
    MOTU828Mk3Protocol protocol(bus, bus, routes.registry, routes.route,
                                nullptr, &scheduler);

    const uint64_t cursor = ring.Stats().latestSequence;
    const auto confirmed = protocol.AsFamilyDriver()->Confirm();
    ASSERT_FALSE(confirmed.has_value());
    EXPECT_EQ(confirmed.error(), kIOReturnNotReady);
    char status[32];
    std::snprintf(status, sizeof(status), "status=0x%08x",
                  static_cast<uint32_t>(kIOReturnNotReady));
    const auto end = RingMessagesContaining(cursor, "ConfirmDuplexStart END");
    ASSERT_EQ(end.size(), 1U);
    EXPECT_NE(end[0].find(status), std::string::npos) << end[0];
    EXPECT_TRUE(bus.writes.empty());
}

TEST(Motu828Mk3ProtocolTests, RejectsUnprovenLiveRateChange) {
    RecordingBus bus;
    bus.SetGeneration(Generation{1});
    FakeTimerScheduler scheduler;
    RouteState routes{1};
    MOTU828Mk3Protocol protocol(bus, bus, routes.registry, routes.route,
                                nullptr, &scheduler);

    SetQuad(bus, 2, kClockStatusOffset, 0x00000000u); // 44.1 kHz
    SetQuad(bus, 2, kAudioBankControlOffset, 0x00000000u);
    IOReturn geometryStatus = kIOReturnNotReady;
    protocol.EnsureRuntimeStreamGeometry(
        [&geometryStatus](IOReturn status) { geometryStatus = status; });
    ASSERT_EQ(geometryStatus, kIOReturnSuccess);

    IOReturn prepareStatus = kIOReturnSuccess;
    protocol.PrepareDuplex(
        TestChannels(), AudioClockConfig{.sampleRateHz = 48000},
        [&prepareStatus](IOReturn status, auto) { prepareStatus = status; });
    EXPECT_EQ(prepareStatus, kIOReturnUnsupported);
    EXPECT_TRUE(bus.writes.empty());
}

TEST(Motu828Mk3ProtocolTests, TeardownTokenAbortsDeactivateSettle) {
    RecordingBus bus;
    bus.SetGeneration(Generation{1});
    FakeTimerScheduler scheduler;
    RouteState routes{1};
    MOTU828Mk3Protocol protocol(bus, bus, routes.registry, routes.route,
                                nullptr, &scheduler);

    SetQuad(bus, 2, kClockStatusOffset, 0x00000100u);
    SetQuad(bus, 2, kAudioBankControlOffset, 0x00000000u);
    protocol.EnsureRuntimeStreamGeometry([](IOReturn status) {
        ASSERT_EQ(status, kIOReturnSuccess);
    });
    protocol.PrepareDuplex(
        TestChannels(), AudioClockConfig{.sampleRateHz = 48000},
        [](IOReturn status, auto) { ASSERT_EQ(status, kIOReturnSuccess); });

    std::atomic<bool> cancel{false};
    protocol.SetTeardownCancelToken(&cancel);
    SetQuad(bus, 2, kIsocControlOffset, 0x00005b59u);
    IOReturn rxStatus = kIOReturnNotReady;
    protocol.ProgramRx([&rxStatus](IOReturn status, auto) { rxStatus = status; });
    cancel.store(true, std::memory_order_release);
    scheduler.Advance(20ULL * 1000ULL * 1000ULL);
    EXPECT_EQ(rxStatus, kIOReturnAborted);
}
