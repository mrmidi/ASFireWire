// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Scripted Fireface register sequencing, clock admission, and resource-policy tests.
#include "DICEDuplexTestSupport.hpp"
#include "Audio/Protocols/RME/FirefaceFamilyDriver.hpp"
#include "Audio/Protocols/RME/FirefaceDeviceProtocol.hpp"
#include "Audio/Host/RmeFamilyAdapter.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>
#include <condition_variable>
#include <map>
#include <mutex>

namespace {
using namespace ASFW::Testing::DICE;
using ::ASFW::Async::InterfaceCompletionCallback;

// Real-time boundary tests need callbacks on a separate serial executor.
// The operation itself is exercised with FakeTimerScheduler below.
class FirefaceTestScheduler final : public ASFW::Scheduling::ITimerScheduler {
public:
    FirefaceTestScheduler() : worker_([this] { Run(); }) {}
    ~FirefaceTestScheduler() { Stop(); }
    void Stop() {
        { std::lock_guard guard(mutex_); stopped_ = true; pending_.clear(); }
        wake_.notify_all();
        if (worker_.joinable()) worker_.join();
    }
    ASFW::Scheduling::TimerToken ScheduleAfter(uint64_t delay, std::function<void()> fn) override {
        std::lock_guard guard(mutex_);
        if (stopped_) return 0;
        const auto token = ++next_;
        pending_.emplace(token, Entry{Clock::now() + std::chrono::nanoseconds(delay), std::move(fn)});
        wake_.notify_all();
        return token;
    }
    void Cancel(ASFW::Scheduling::TimerToken token) override {
        std::lock_guard guard(mutex_); pending_.erase(token); wake_.notify_all();
    }
    uint64_t NowNs() const noexcept override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
    }
private:
    using Clock = std::chrono::steady_clock;
    struct Entry { Clock::time_point deadline; std::function<void()> fn; };
    void Run() {
        std::unique_lock guard(mutex_);
        while (!stopped_) {
            if (pending_.empty()) { wake_.wait(guard); continue; }
            const auto next = std::ranges::min_element(pending_, {}, [](const auto& entry) { return entry.second.deadline; });
            if (next->second.deadline > Clock::now()) { const auto deadline = next->second.deadline; wake_.wait_until(guard, deadline); continue; }
            auto fn = std::move(next->second.fn); pending_.erase(next);
            guard.unlock(); fn(); guard.lock();
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::map<uint64_t, Entry> pending_;
    uint64_t next_{0};
    bool stopped_{false};
    std::thread worker_;
};

class FirefaceScriptBus final : public IFireWireBus {
public:
    ~FirefaceScriptBus() override { timer.Stop(); }
    FirefaceTestScheduler timer;
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
        // FF400 flash status reads back on the command register itself
        // (FFADO fireface_def.h:58-59,123-124; RME 3.41 Wait 0x6c0a polls 0x80100520).
        else if (key == 0x80100520ULL) value = flashStatus;
        else if (key == 0x80100524ULL) value = 0xdeadbeefU; // a different register: never 0
        else if (key == 0x80100290ULL) value = revision;
        if (key == failReadAddress) { callback(AsyncStatus::kHardwareError, {}); return {}; }
        if (readHook) readHook(key);
        if (length > 4) {  // block reads: flash settings and the status block
            std::vector<uint8_t> block;
            const auto put = [&block](uint32_t w) {
                for (int b = 0; b < 4; ++b) block.push_back(static_cast<uint8_t>(w >> (8 * b)));
            };
            if (key == 0x0003000f0000ULL) {
                for (uint32_t i = 0; i < length / 4; ++i) put(flashImage[i]);
            } else if (key == 0x80100290ULL) {
                const uint32_t first = (ff400FlashAddr - 0x00060000U) / 4;
                for (uint32_t i = 0; i < length / 4; ++i) put(flashImage[first + i]);
            } else if (key == 0x801c0000ULL) {
                put(status0); put(status1); put(captureWord); put(status3);
            }
            block.resize(length, 0);
            callback(AsyncStatus::kSuccess, std::span<const uint8_t>(block.data(), block.size()));
            return {};
        }
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
        if (writeHook) writeHook(key);
        if (key == failWriteAddress) { callback(AsyncStatus::kHardwareError, {}); return {}; }
        if (key == 0x80100288ULL && bytes.size() == 8) {  // FF400 flash address + length latch
            ff400FlashAddr = static_cast<uint32_t>(bytes[0]) | (bytes[1] << 8) |
                             (bytes[2] << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
        }
        if (deferWrite) { deferWrite = false; delayedWrite = std::move(callback); }
        else callback(AsyncStatus::kSuccess, {});
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
    // A recognisable settings image: quadlet i of the block holds 0x5e770000 | i.
    static constexpr uint32_t FlashWord(uint32_t i) noexcept { return 0x5e770000U | i; }
    // Not a valid settings image: decodes to an error at the first field read.
    std::array<uint32_t, 64> flashImage = [] {
        std::array<uint32_t, 64> image{};
        for (uint32_t i = 0; i < image.size(); ++i) image[i] = FlashWord(i);
        return image;
    }();
    uint32_t ff400FlashAddr{0};
    uint32_t status3{0};
    uint32_t revision{0x24d};
    // Internal clock at 48 kHz, as a real FF800 reports it after init (2026-10-10
    // field log): the sync-reference field (status0 & 0x01c00000) reads 0.
    uint32_t status0{0x060014c0};
    uint32_t status1{0x88001007};
    uint32_t captureWord{6};
    uint32_t flashStatus{0};
    uint64_t failReadAddress{0};
    uint64_t failWriteAddress{0};
    std::function<void(uint64_t)> readHook;
    std::function<void(uint64_t)> writeHook;
    void DeliverDelayedWrite() { if (delayedWrite) { auto cb = std::move(delayedWrite); cb(AsyncStatus::kSuccess, {}); } }
    bool deferWrite{false};
    InterfaceCompletionCallback delayedWrite;
    bool deferRead{false};
    std::vector<Write> writes;
    std::vector<uint64_t> reads;
    InterfaceCompletionCallback delayed;
    std::array<uint8_t, 4> delayedBytes{};
};

struct AsyncFF800Rig {
    FirefaceScriptBus bus;
    RouteState route;
    ProtocolRegisterIO io{bus, bus, route.registry, route.route};
    ASFW::Testing::FakeTimerScheduler timer;
    std::optional<IOReturn> status;
    uint8_t channel{0};
    unsigned completions{0};
    std::shared_ptr<ASFW::Audio::RME::FF800TransmitStart> operation;
    ~AsyncFF800Rig() { if (operation) operation->Cancel(); }
    void Begin(const std::atomic<bool>* cancel = nullptr) {
        operation = ASFW::Audio::RME::FF800TransmitStart::Create(io, timer,
            ASFW::Audio::RME::InitWords(ASFW::Audio::RME::FirefaceModel::kFF800, 0, true), cancel);
        operation->Begin([this](IOReturn result, uint8_t assigned) {
            status = result; channel = assigned; ++completions;
        });
    }
};

TEST(FirefaceAsyncStartTests, DelayedFrequencyCompletionGetsAFullSettleAndLeavesQueueResponsive) {
    AsyncFF800Rig rig;
    rig.bus.deferWrite = true;
    rig.Begin();
    EXPECT_TRUE(rig.bus.writes.empty()); // Begin only enqueues; it never sleeps.
    rig.timer.Advance(120'000'000);
    ASSERT_EQ(rig.bus.writes.size(), 1U);
    EXPECT_FALSE(rig.status);
    bool unrelatedRan = false;
    (void)rig.timer.ScheduleAfter(1, [&] { unrelatedRan = true; });
    rig.timer.Advance(1);
    EXPECT_TRUE(unrelatedRan);
    rig.bus.DeliverDelayedWrite();
    rig.timer.Advance(99'000'000);
    EXPECT_EQ(rig.bus.writes.size(), 1U);
    rig.timer.Advance(1'000'000);
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnSuccess);
    EXPECT_EQ(rig.channel, 6U);
    EXPECT_EQ(rig.completions, 1U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(FirefaceAsyncStartTests, MissingRateResponseUsesTheSharedDeadlineAndIgnoresLateReply) {
    AsyncFF800Rig rig;
    rig.bus.deferRead = true;
    rig.Begin();
    rig.timer.Advance(200'000'000);
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnTimeout);
    EXPECT_EQ(rig.bus.writes.size(), 1U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
    rig.bus.DeliverDelayed();
    rig.timer.Advance(2'000'000'000);
    EXPECT_EQ(rig.completions, 1U);
    EXPECT_EQ(rig.bus.writes.size(), 1U);
}

TEST(FirefaceAsyncStartTests, RateReportedAfterTheSettleStillGatesThePacketFormat) {
    // From PR #201: a card that keeps answering 44.1 kHz past the 100 ms settle
    // must not get the packet format until it reports the new rate.
    AsyncFF800Rig rig;
    rig.bus.status0 = 0x01c000b0U; rig.bus.status1 = 0x80001201U; // internal, 44.1 kHz
    rig.Begin();
    (void)rig.timer.ScheduleAfter(150'000'000, [&] {
        rig.bus.status0 = 0x01c000c0U; rig.bus.status1 = 0xa0001207U; // now at 48 kHz
    });
    rig.timer.Advance(149'000'000);
    EXPECT_EQ(rig.bus.writes.size(), 1U) << "the format reached a card still at 44.1 kHz";
    rig.timer.Advance(50'000'000);
    ASSERT_GE(rig.bus.writes.size(), 2U);
    EXPECT_EQ(rig.bus.writes[1].lo, 0x20U);
    rig.timer.Advance(500'000'000);
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnSuccess);
    EXPECT_EQ(rig.completions, 1U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(FirefaceAsyncStartTests, NoChannelResetsAndRetriesExactlyOnce) {
    AsyncFF800Rig rig;
    rig.bus.captureWord = 0xffffffffU;
    unsigned formats = 0;
    rig.bus.writeHook = [&](uint64_t address) {
        if (address == ASFW::Audio::RME::Register::kFF800PacketFormat && ++formats == 2)
            rig.bus.captureWord = 1;
    };
    rig.Begin();
    rig.timer.Advance(700'000'000);
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnSuccess);
    EXPECT_EQ(rig.channel, 1U);
    ASSERT_EQ(rig.bus.writes.size(), 5U);
    EXPECT_EQ(rig.bus.writes[2].lo, 0x34U);
    EXPECT_EQ(rig.bus.writes[2].bytes, std::vector<uint8_t>(12, 0));
    EXPECT_EQ(formats, 2U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(FirefaceAsyncStartTests, RetryRevalidatesTheClockBeforeAcceptingAnyChannel) {
    AsyncFF800Rig rig;
    rig.bus.captureWord = 0xffffffffU;
    unsigned formats = 0;
    rig.bus.writeHook = [&](uint64_t address) {
        if (address == ASFW::Audio::RME::Register::kFF800Stop) {
            rig.bus.status0 = 0x01c000b0U; rig.bus.status1 = 0x80001201U;
        }
        if (address == ASFW::Audio::RME::Register::kFF800PacketFormat && ++formats == 2)
            rig.bus.captureWord = 1;
    };
    rig.Begin();
    rig.timer.Advance(800'000'000);
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnNotReady);
    EXPECT_EQ(rig.completions, 1U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(FirefaceAsyncStartTests, SecondAllocationTimeoutCompletesAndDoesNotRetryAgain) {
    AsyncFF800Rig rig;
    rig.bus.captureWord = 0xffffffffU;
    rig.Begin();
    rig.timer.Advance(1'200'000'000);
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnTimeout);
    ASSERT_EQ(rig.bus.writes.size(), 5U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
    rig.timer.Advance(10'000'000'000);
    EXPECT_EQ(rig.bus.writes.size(), 5U);
    EXPECT_EQ(rig.completions, 1U);
}

TEST(FirefaceAsyncStartTests, CancellationClosesTimersAndLateWriteAfterOwnerDestruction) {
    AsyncFF800Rig rig;
    rig.bus.deferWrite = true;
    rig.Begin();
    rig.timer.Advance(0);
    rig.operation->Cancel();
    rig.operation.reset();
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnAborted);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
    rig.bus.DeliverDelayedWrite();
    rig.timer.Advance(2'000'000'000);
    EXPECT_EQ(rig.bus.writes.size(), 1U);
    EXPECT_EQ(rig.completions, 1U);
}

TEST(FirefaceAsyncStartTests, TeardownDuringSettlePreventsThePacketFormat) {
    AsyncFF800Rig rig;
    std::atomic<bool> cancel{false};
    rig.Begin(&cancel);
    rig.timer.Advance(20'000'000);
    cancel.store(true);
    rig.timer.Advance(5'000'000);
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnAborted);
    EXPECT_EQ(rig.bus.writes.size(), 1U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(FirefaceAsyncStartTests, GenerationInvalidationPreventsFurtherIO) {
    AsyncFF800Rig rig;
    rig.Begin();
    rig.timer.Advance(20'000'000);
    rig.route.registry.InvalidateLiveMappingsForBusReset();
    rig.timer.Advance(5'000'000);
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnOffline);
    EXPECT_EQ(rig.bus.writes.size(), 1U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(FirefaceAsyncStartTests, MissingSecondStatusReplySharesTheRateDeadline) {
    AsyncFF800Rig rig;
    rig.bus.readHook = [&](uint64_t address) {
        if (address == ASFW::Audio::RME::Register::kStatus + 4) rig.bus.deferRead = true;
    };
    rig.Begin();
    rig.timer.Advance(200'000'000);
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnTimeout);
    EXPECT_EQ(rig.bus.writes.size(), 1U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(FirefaceAsyncStartTests, FailedResetDoesNotIssueAnotherInit) {
    AsyncFF800Rig rig;
    rig.bus.captureWord = 0xffffffffU;
    rig.bus.failWriteAddress = ASFW::Audio::RME::Register::kFF800Stop;
    rig.Begin();
    rig.timer.Advance(600'000'000);
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnError);
    ASSERT_EQ(rig.bus.writes.size(), 3U);
    EXPECT_EQ(rig.bus.writes.back().lo, 0x34U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(FirefaceAsyncStartTests, DroppingTheOperationCancelsItsSettleContinuation) {
    AsyncFF800Rig rig;
    rig.Begin();
    rig.timer.Advance(20'000'000);
    rig.operation.reset();
    ASSERT_TRUE(rig.status);
    EXPECT_EQ(*rig.status, kIOReturnAborted);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
    rig.timer.Advance(2'000'000'000);
    EXPECT_EQ(rig.bus.writes.size(), 1U);
    EXPECT_EQ(rig.completions, 1U);
}

TEST(FirefaceIntegrationTests, ShutdownCancelsThePendingStartWithoutBlockingTheTimerQueue) {
    FirefaceScriptBus bus;
    RouteState route;
    ASFW::Audio::RME::FirefaceDeviceProtocol protocol(bus, bus, route.registry, route.route,
        ASFW::Audio::RME::FirefaceModel::kFF800, true, &bus.timer);
    auto* family = protocol.AsFamilyDriver();
    ASSERT_TRUE(family->Configure({}, {.sampleRateHz = 48000}));
    (void)bus.timer.ScheduleAfter(20'000'000, [&] { (void)protocol.Shutdown(); });
    const auto assigned = family->AssignChannels({});
    ASSERT_FALSE(assigned);
    EXPECT_EQ(assigned.error(), kIOReturnAborted);
    EXPECT_EQ(bus.writes.size(), 1U);
    EXPECT_EQ(family->Stop(), kIOReturnSuccess);
}

TEST(FirefaceIntegrationTests, ShutdownBeforeStartPublicationPreventsAnyInit) {
    FirefaceScriptBus bus;
    RouteState route;
    ASFW::Audio::RME::FirefaceDeviceProtocol protocol(bus, bus, route.registry, route.route,
        ASFW::Audio::RME::FirefaceModel::kFF800, true, &bus.timer);
    auto* family = protocol.AsFamilyDriver();
    ASSERT_TRUE(family->Configure({}, {.sampleRateHz = 48000}));
    ASSERT_EQ(protocol.Shutdown(), kIOReturnSuccess);
    const auto assigned = family->AssignChannels({});
    ASSERT_FALSE(assigned);
    EXPECT_EQ(assigned.error(), kIOReturnAborted);
    EXPECT_TRUE(bus.writes.empty());
}

TEST(FirefaceIntegrationTests, MissingTimerSchedulerRefusesFF800BeforeInit) {
    FirefaceScriptBus bus;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, true);
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    const auto assigned = family.AssignChannels({});
    ASSERT_FALSE(assigned);
    EXPECT_EQ(assigned.error(), kIOReturnNotReady);
    EXPECT_TRUE(bus.writes.empty());
}

std::vector<uint8_t> MuteMask(uint32_t channels) {
    std::vector<uint8_t> bytes(channels * 4U, 0);
    for (uint32_t i = 0; i < channels; ++i) bytes[i * 4U] = 1; // LE quadlet value 1
    return bytes;
}

void ExpectStopThenMute(const FirefaceScriptBus& bus, uint32_t stopLo, uint32_t channels) {
    ASSERT_GE(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[bus.writes.size() - 2].lo, stopLo);
    EXPECT_EQ(bus.writes.back().lo, 0x801c0000U);
    EXPECT_EQ(bus.writes.back().bytes, MuteMask(channels));
}

TEST(FirefaceIntegrationTests, Non48kAdmissionReturnsBeforeAnyRegisterTraffic) {
    FirefaceScriptBus bus;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(
        io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
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
        bus, bus, route.registry, route.route, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
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
        const auto config = ASFW::Audio::Host::RmeFamilyAdapter::BuildNubConfig(record, builder, "RME");
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

TEST(FirefaceIntegrationTests, PublicationNamesEveryChannelInWireOrder) {
    // RME 3.41 channel-name tables (gChannelNames_FF400_in/out at 0x8020/0x8140,
    // FF800 at 0x8260/0x8420); FFADO addDirPorts has the same order: analog,
    // phones (playback), S/PDIF, ADAT.
    ASFW::Discovery::DeviceRecord record{};
    using ASFW::DeviceProfiles::Audio::ProfileBuilderId;
    using ASFW::Audio::Host::RmeFamilyAdapter;
    const auto ff400 = RmeFamilyAdapter::BuildNubConfig(record, ProfileBuilderId::RmeFireface400, "RME");
    ASSERT_EQ(ff400.inputChannelNames.size(), 18U);
    ASSERT_EQ(ff400.outputChannelNames.size(), 18U);
    EXPECT_EQ(ff400.inputChannelNames[0], "Mic/Line 1");
    EXPECT_EQ(ff400.inputChannelNames[2], "Inst/Line 3");
    EXPECT_EQ(ff400.inputChannelNames[8], "SPDIF L");
    EXPECT_EQ(ff400.inputChannelNames[17], "ADAT 8");
    EXPECT_EQ(ff400.outputChannelNames[6], "Phones 7");
    EXPECT_EQ(ff400.outputChannelNames[9], "SPDIF R");
    const auto ff800 = RmeFamilyAdapter::BuildNubConfig(record, ProfileBuilderId::RmeFireface800, "RME");
    ASSERT_EQ(ff800.inputChannelNames.size(), 28U);
    ASSERT_EQ(ff800.outputChannelNames.size(), 28U);
    EXPECT_EQ(ff800.inputChannelNames[0], "Analog 1");
    EXPECT_EQ(ff800.inputChannelNames[9], "Mic 10");
    EXPECT_EQ(ff800.inputChannelNames[10], "SPDIF L");
    EXPECT_EQ(ff800.inputChannelNames[27], "ADAT 16");
    EXPECT_EQ(ff800.outputChannelNames[8], "Phones 9");
    EXPECT_EQ(ff800.outputChannelNames[12], "ADAT 1");
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
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, true, &bus.timer);
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
    // The frequency lands first and alone, then the packet format and the size
    // of the stream the card transmits. No fetch mask yet.
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[0].hi, 2U);
    EXPECT_EQ(bus.writes[0].lo, 0x1cU);
    EXPECT_EQ(bus.writes[0].bytes, (std::vector<uint8_t>{0x80,0xbb,0,0}));
    EXPECT_EQ(bus.writes[1].hi, 2U);
    EXPECT_EQ(bus.writes[1].lo, 0x20U);
    EXPECT_EQ(bus.writes[1].bytes, (std::vector<uint8_t>{3,0xe0,0,0, 0x1c,0x08,0,0}));
    ASSERT_TRUE(family.ArmDeviceTxAndEnable());
    EXPECT_EQ(bus.writes.back().lo, 0x28U);
    EXPECT_EQ(bus.writes.back().bytes, (std::vector<uint8_t>{0x1c,0x08,0,0x80}));
    ASSERT_TRUE(family.Confirm());
    EXPECT_EQ(bus.writes.back().hi, 0U);
    EXPECT_EQ(bus.writes.back().lo, 0x801c0000U);
    EXPECT_EQ(bus.writes.back().bytes, std::vector<uint8_t>(28U * 4U, 0)); // fetch every channel
    EXPECT_EQ(family.Stop(), kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 6U); // init (2), start, fetch, stop, mute; no mixer or clock-source writes
    EXPECT_EQ(bus.writes[4].hi, 2U);
    EXPECT_EQ(bus.writes[4].lo, 0x34U);
    EXPECT_EQ(bus.writes[4].bytes, (std::vector<uint8_t>{0,0,0,0, 0,0,0,0, 0,0,0,0}));
    EXPECT_EQ(bus.writes[5].lo, 0x801c0000U);
    EXPECT_EQ(bus.writes[5].bytes, MuteMask(28U)); // stop first, then mute
}

TEST(FirefaceSequenceTests, FF800S400TupleDoesNotSetTheS800Flag) {
    FirefaceScriptBus bus; RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{}; channels.hostToDeviceIsoChannel = 1;
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    ASSERT_TRUE(family.AssignChannels(channels));
    EXPECT_EQ(bus.writes[0].hi, 0x0002U);
    EXPECT_EQ(bus.writes[0].lo, 0x1cU);
    EXPECT_EQ(bus.writes[0].bytes, (std::vector<uint8_t>{0x80,0xbb,0,0}));
    EXPECT_EQ(bus.writes[1].lo, 0x20U);
    EXPECT_EQ(bus.writes[1].bytes, (std::vector<uint8_t>{1,0xe0,0,0, 28,0,0,0}));
}

TEST(FirefaceSequenceTests, FF800SetsRateBeforeProgrammingStreamFormat) {
    // Scripted transition from the PR #196 card's 44.1 kHz status to 48 kHz.
    FirefaceScriptBus bus;
    bus.status0 = 0x01c000b0U; bus.status1 = 0x80001201U;  // internal, 44.1 kHz, as our field log reads it
    bus.captureWord = 0xffffffffU; bus.status3 = 0xffffffffU;
    bool at48kWhenTheFormatLanded = false;
    bus.writeHook = [&bus, &at48kWhenTheFormatLanded](uint64_t key) {
        if (key == 0x00020000001cULL) {  // the frequency lands: the card is at 48 kHz now
            bus.status0 = 0x01c000c0U; bus.status1 = 0xa0001207U;
        } else if (key == 0x000200000020ULL) {  // the format lands: the card picks its channel
            at48kWhenTheFormatLanded = bus.status1 == 0xa0001207U;
            bus.captureWord = 1;
        }
    };
    RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    const auto assigned = family.AssignChannels(channels);
    ASSERT_TRUE(assigned) << "a channel published only after the frequency took effect was missed";
    EXPECT_EQ(assigned->deviceToHostIsoChannel, 1U);
    EXPECT_TRUE(at48kWhenTheFormatLanded) << "the packet format reached the card while it was still at 44.1 kHz";
    ASSERT_GE(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[0].lo, 0x1cU);
    EXPECT_EQ(bus.writes[1].lo, 0x20U);
}

TEST(FirefaceSequenceTests, FF800RateSettleDoesNotWaitForTheDefaultRegisterTimeout) {
    FirefaceScriptBus bus;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    bus.deferRead = true; // Lose the first status response after the rate write.

    const auto before = ASFW::Audio::Session::UptimeMilliseconds();
    const auto result = family.AssignChannels(channels);
    const auto elapsed = ASFW::Audio::Session::UptimeMilliseconds() - before;
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), kIOReturnTimeout);
    EXPECT_GE(elapsed, 180U);
    EXPECT_LT(elapsed, 400U);
    ASSERT_EQ(bus.writes.size(), 1U); // No format after a lost response.
    EXPECT_EQ(family.Stop(), kIOReturnSuccess);
    ExpectStopThenMute(bus, 0x34U, 28U);
    bus.DeliverDelayed(); // A late response must not resume initialization.
    EXPECT_EQ(bus.writes.size(), 3U);
}

TEST(FirefaceSequenceTests, FF800RateSettleSharesOneBudgetAcrossBothStatusReads) {
    FirefaceScriptBus bus;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    bus.readHook = [&bus](uint64_t key) {
        if (key == 0x801c0000ULL) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (key == 0x801c0004ULL) bus.deferRead = true;
    };

    const auto before = ASFW::Audio::Session::UptimeMilliseconds();
    const auto result = family.AssignChannels(channels);
    const auto elapsed = ASFW::Audio::Session::UptimeMilliseconds() - before;
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), kIOReturnTimeout);
    EXPECT_GE(elapsed, 180U);
    EXPECT_LT(elapsed, 320U); // Both status reads share the 200 ms post-write deadline.
    ASSERT_EQ(bus.writes.size(), 1U);
}

TEST(FirefaceSequenceTests, FF800ResponsiveOldRateKeepsTheAdvisorySettleFallback) {
    FirefaceScriptBus bus;
    bus.status0 = 0x01c000b0U;
    bus.status1 = 0x80001201U;
    bus.captureWord = 0xffffffffU;
    bus.writeHook = [&bus](uint64_t key) {
        if (key == 0x000200000020ULL) {
            bus.status0 = 0x01c000c0U;
            bus.status1 = 0x80001207U;
            bus.captureWord = 1;
        }
    };
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));

    const auto before = ASFW::Audio::Session::UptimeMilliseconds();
    ASSERT_TRUE(family.AssignChannels(channels));
    const auto elapsed = ASFW::Audio::Session::UptimeMilliseconds() - before;
    EXPECT_GE(elapsed, 180U);
    EXPECT_LT(elapsed, 400U);
    ASSERT_EQ(bus.writes.size(), 2U);
    EXPECT_EQ(bus.writes[1].lo, 0x20U);
}

TEST(FirefaceSequenceTests, FF800CancelledRateReadDoesNotSendTheFormat) {
    FirefaceScriptBus bus;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    std::atomic<bool> cancel{false};
    family.SetTeardownCancelToken(&cancel);
    bus.readHook = [&cancel](uint64_t key) {
        if (key == 0x801c0000ULL) cancel.store(true, std::memory_order_release);
    };
    const auto result = family.AssignChannels(channels);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), kIOReturnAborted);
    ASSERT_EQ(bus.writes.size(), 1U);
    cancel.store(false, std::memory_order_release);
    EXPECT_EQ(family.Stop(), kIOReturnSuccess);
    ExpectStopThenMute(bus, 0x34U, 28U);
}

TEST(FirefaceIntegrationTests, HealthReadHonorsTheSessionBudget) {
    FirefaceScriptBus bus;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    bus.deferRead = true;
    const auto before = ASFW::Audio::Session::UptimeMilliseconds();
    const auto result = family.ReadHealth(40);
    const auto elapsed = ASFW::Audio::Session::UptimeMilliseconds() - before;
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), kIOReturnTimeout);
    EXPECT_LT(elapsed, 200U);
}

TEST(FirefaceSequenceTests, FF400RejectsOutOfRangeChannelBeforeAnyStreamingWrites) {
    FirefaceScriptBus bus;
    bus.revision = 0x146;
    RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
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
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};
    channels.hostToDeviceIsoChannel = 2;
    channels.deviceToHostIsoChannel = 5;
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    ASSERT_EQ(bus.writes.size(), 5U); // get_revision, then the flash settings dry-run read
    EXPECT_EQ(bus.writes[0].lo, 0x80100520U);
    ASSERT_TRUE(family.AssignChannels(channels));
    ASSERT_EQ(bus.writes.size(), 6U);
    EXPECT_EQ(bus.writes[5].lo, 0x80100500U);
    EXPECT_EQ(bus.writes[5].bytes, (std::vector<uint8_t>{0x80,0xbb,0,0, 2,0x90,0,0, 0x12,0,0,0}));
    ASSERT_TRUE(family.ArmDeviceTxAndEnable());
    EXPECT_EQ(bus.writes.back().lo, 0x8010050cU);
    EXPECT_EQ(bus.writes.back().bytes, (std::vector<uint8_t>{0xb2,0,0,0x80}));
    ASSERT_TRUE(family.Confirm());
    EXPECT_EQ(bus.writes.back().lo, 0x801c0000U);
    EXPECT_EQ(bus.writes.back().bytes, std::vector<uint8_t>(18U * 4U, 0));
    EXPECT_EQ(family.Stop(), kIOReturnSuccess);
    ASSERT_EQ(bus.writes.size(), 10U); // revision + flash read + init + start + fetch + stop + mute only
    EXPECT_EQ(bus.writes[8].lo, 0x80100504U);
    EXPECT_EQ(bus.writes[8].bytes, (std::vector<uint8_t>{0,0,0,0,0,0,0,0,0,0,0,0,1,0,0,0}));
    EXPECT_EQ(bus.writes[9].lo, 0x801c0000U);
    EXPECT_EQ(bus.writes[9].bytes, MuteMask(18U));
    const auto policy = family.GetResourcePolicy();
    EXPECT_EQ(policy.playback.allowedIsoChannels, 0xffU);
    EXPECT_EQ(policy.capture.allowedIsoChannels, 0xffU);
}

TEST(FirefaceSequenceTests, FF400FlashWaitCountsPollsNotWallClock) {
    // RME 3.41 Wait (0x6c0a) and FFADO wait_while_busy (MAX_FLASH_BUSY_RETRIES 25,
    // fireface_flash.cpp:33-62) poll a fixed number of times. A slow bus makes
    // each poll take longer; it must not make the driver give up early.
    FirefaceScriptBus bus; bus.revision = 0x146;
    uint32_t busyReads = 4;
    bus.flashStatus = 1;
    bus.readHook = [&](uint64_t address) {
        if (address != 0x80100520ULL || busyReads == 0) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));  // 4 x 15 ms > 50 ms
        if (--busyReads == 0) bus.flashStatus = 0;
    };
    RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
    EXPECT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    EXPECT_NE(std::find(bus.reads.begin(), bus.reads.end(), 0x80100290ULL), bus.reads.end());
}

TEST(FirefaceSequenceTests, FF400RevisionPollsTheFlashCommandRegisterUntilIdle) {
    FirefaceScriptBus bus; bus.revision = 0x146;
    uint32_t busyReads = 2;
    bus.flashStatus = 1;
    bus.readHook = [&](uint64_t address) {
        if (address == 0x80100520ULL && busyReads > 0 && --busyReads == 0) bus.flashStatus = 0;
    };
    RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
    EXPECT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    const auto reads = bus.reads;
    EXPECT_EQ(std::count(reads.begin(), reads.end(), 0x80100524ULL), 0);
    const auto revisionRead = std::find(reads.begin(), reads.end(), 0x80100290ULL);
    ASSERT_NE(revisionRead, reads.end());
    EXPECT_EQ(std::count(reads.begin(), revisionRead, 0x80100520ULL), 3) // busy, busy, idle
        << "the revision is read only after the command register reads 0";
}

namespace {
std::vector<uint8_t> LeWords(std::initializer_list<uint32_t> words) {
    std::vector<uint8_t> out;
    for (const uint32_t w : words)
        for (int b = 0; b < 4; ++b) out.push_back(static_cast<uint8_t>(w >> (8 * b)));
    return out;
}
void ExpectFlashImage(const ASFW::Audio::RME::FirefaceFamilyDriver& family) {
    const auto& settings = family.LastFlashSettings();
    ASSERT_TRUE(settings.has_value());
    for (uint32_t i = 0; i < settings->size(); ++i)
        EXPECT_EQ((*settings)[i], FirefaceScriptBus::FlashWord(i)) << "quadlet " << i;
}
} // namespace

TEST(FirefaceFlashTests, FF800ReadsTheSettingsBlockWithoutWritingAnything) {
    // FFADO read_flash: the FF800 settings are a plain block read at
    // 0x3000f0000 (fireface_def.h:117, fireface_flash.cpp:96-103).
    FirefaceScriptBus bus; RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    EXPECT_NE(std::find(bus.reads.begin(), bus.reads.end(), 0x0003000f0000ULL), bus.reads.end());
    EXPECT_TRUE(bus.writes.empty()) << "a dry run writes nothing to an FF800";
    ExpectFlashImage(family);
}

TEST(FirefaceFlashTests, FF400ProgramsAddressAndLengthThenReadsTheBounceBuffer) {
    // FFADO read_flash FF400 path (fireface_flash.cpp:105-121): [addr, bytes] to
    // 0x80100288, READ (2) to 0x80100520, wait idle, read 0x80100290; 32 quadlets
    // per pass, so the 59-quadlet block takes two passes.
    FirefaceScriptBus bus; bus.revision = 0x146; RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    ASSERT_EQ(bus.writes.size(), 5U);
    EXPECT_EQ(bus.writes[1].lo, 0x80100288U);
    EXPECT_EQ(bus.writes[1].bytes, LeWords({0x00060000U, 128U}));
    EXPECT_EQ(bus.writes[2].lo, 0x80100520U);
    EXPECT_EQ(bus.writes[2].bytes, LeWords({0x2U}));
    EXPECT_EQ(bus.writes[3].bytes, LeWords({0x00060080U, 108U}));
    EXPECT_EQ(bus.writes[4].bytes, LeWords({0x2U}));
    ExpectFlashImage(family);
}

TEST(FirefaceFlashTests, FF400FlashCommandRegisterOnlyEverSeesReadOrRevision) {
    // 0x80100520 also takes WRITE (1) and ERASE (0xc/0xd/0xe) (fireface_def.h:135-140).
    FirefaceScriptBus bus; bus.revision = 0x146; RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    ASSERT_TRUE(family.AssignChannels(channels));
    ASSERT_TRUE(family.ArmDeviceTxAndEnable());
    ASSERT_TRUE(family.Confirm());
    EXPECT_EQ(family.Stop(), kIOReturnSuccess);
    for (const auto& w : bus.writes) {
        if (w.lo != 0x80100520U) continue;
        EXPECT_TRUE(w.bytes == LeWords({0x2U}) || w.bytes == LeWords({0xfU}));
    }
}

TEST(FirefaceFlashTests, DryRunRefusesAnImageItCannotDecodeAndStillStarts) {
    FirefaceScriptBus bus; RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    const auto& dryRun = family.LastSettingsDryRun();
    ASSERT_TRUE(dryRun.decodeError.has_value());
    EXPECT_EQ(dryRun.decodeError->quadlet, ASFW::Audio::RME::FlashField::kSpdifInputMode);
    EXPECT_FALSE(dryRun.config.has_value());
    EXPECT_TRUE(bus.writes.empty());
}

TEST(FirefaceFlashTests, ConfigComesFromTheFlashMatchesTheStatusMirrorAndIsWritten) {
    namespace F = ASFW::Audio::RME::FlashField;
    FirefaceScriptBus bus; RouteState route;
    bus.flashImage.fill(0);
    bus.flashImage[F::kSpdifInputMode] = 1;        // coaxial
    bus.flashImage[F::kSyncReference] = 3;         // word clock
    bus.flashImage[F::kOutputLevel] = 2;           // high gain
    bus.flashImage[F::kPlugSelect0] = 1;           // input 7 front
    bus.flashImage[F::kInstrumentPlugSelect] = 1;  // input 1 front
    bus.flashImage[F::kSampleRate] = 48000;
    bus.status1 = 0x00001007U;  // internal, saved word-clock reference, 48 kHz
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    const auto& dryRun = family.LastSettingsDryRun();
    ASSERT_FALSE(dryRun.decodeError.has_value());
    ASSERT_TRUE(dryRun.config.has_value());
    EXPECT_EQ(*dryRun.config, (ASFW::Audio::RME::ConfigWords{0x00000408U, 0x00000B30U, 0x8000101FU}));
    ASSERT_TRUE(dryRun.status.has_value());
    EXPECT_TRUE(dryRun.status->Matches());
    ASSERT_EQ(bus.writes.size(), 1U) << "only the configuration register is written";
    EXPECT_EQ(bus.writes[0].hi, 0x0000U);
    EXPECT_EQ(bus.writes[0].lo, 0xfc88f014U);
    EXPECT_EQ(bus.writes[0].bytes, LeWords({0x00000408U, 0x00000B30U, 0x8000101FU}));
}

TEST(FirefaceFlashTests, DryRunRunsOncePerDeviceSoRetriesDoNotFloodTheLogRing) {
    // A field log lost its history to ~13 dry-run lines per failed start.
    FirefaceScriptBus bus; RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    const auto flashReads = [&bus] {
        return std::ranges::count(bus.reads, 0x0003000f0000ULL);
    };
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    EXPECT_EQ(flashReads(), 1);
    EXPECT_TRUE(family.LastFlashSettings().has_value()) << "the first result stays readable";
}

TEST(FirefaceFlashTests, DryRunRetriesAfterAFailedFlashRead) {
    FirefaceScriptBus bus; RouteState route;
    bus.failReadAddress = 0x0003000f0000ULL;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    EXPECT_FALSE(family.LastFlashSettings().has_value());
    bus.failReadAddress = 0;
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    EXPECT_TRUE(family.LastFlashSettings().has_value());
}

// A decodable image: every field the decoder reads holds a defined value.
void LoadValidFlash(FirefaceScriptBus& bus) {
    bus.flashImage.fill(0);
    bus.flashImage[ASFW::Audio::RME::FlashField::kSpdifInputMode] = 1;
    bus.flashImage[ASFW::Audio::RME::FlashField::kSampleRate] = 48000;
}

TEST(FirefaceFlashTests, ConfigWriteForcesClockMasterOverASavedAutosync) {
    FirefaceScriptBus bus; RouteState route;
    LoadValidFlash(bus);
    bus.flashImage[ASFW::Audio::RME::FlashField::kClockMode] = 1;  // autosync
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    ASSERT_EQ(bus.writes.size(), 1U);
    ASSERT_EQ(bus.writes[0].lo, 0xfc88f014U);
    ASSERT_EQ(bus.writes[0].bytes.size(), 12U);
    EXPECT_EQ(bus.writes[0].bytes[8] & 0x1U, 0x1U) << "quadlet 2 bit 0: clock master";
    const auto& upload = family.LastSettingsDryRun();
    ASSERT_TRUE(upload.config.has_value());
    EXPECT_EQ((*upload.config)[2] & 0x1U, 0U) << "the flash itself still says autosync";
}

TEST(FirefaceFlashTests, FF400WritesItsOwnConfigRegister) {
    FirefaceScriptBus bus; bus.revision = 0x146; RouteState route;
    LoadValidFlash(bus);
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    const auto config = std::ranges::find_if(bus.writes, [](const auto& w) { return w.lo == 0x80100514U; });
    ASSERT_NE(config, bus.writes.end());
    EXPECT_EQ(config->bytes.size(), 12U);
    EXPECT_EQ(config->bytes[8] & 0x1U, 0x1U);
    EXPECT_FALSE(std::ranges::any_of(bus.writes, [](const auto& w) { return w.lo == 0xfc88f014U; }));
}

TEST(FirefaceFlashTests, AFailedConfigWriteNeverBlocksTheStart) {
    FirefaceScriptBus bus; RouteState route;
    LoadValidFlash(bus);
    bus.failWriteAddress = 0x0000fc88f014ULL;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    ASSERT_TRUE(family.AssignChannels({}));
    EXPECT_FALSE(family.LastSettingsDryRun().written.has_value()) << "a failed write is not reported as written";
}

TEST(FirefaceFlashTests, AnUndecodableFlashWritesNoConfig) {
    FirefaceScriptBus bus; RouteState route;  // default image: not a settings block
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASSERT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    ASSERT_TRUE(family.AssignChannels({}));
    EXPECT_FALSE(std::ranges::any_of(bus.writes, [](const auto& w) { return w.lo == 0xfc88f014U; }));
}

TEST(FirefaceFlashTests, AFailedFlashReadNeverBlocksTheStart) {
    FirefaceScriptBus bus; bus.failReadAddress = 0x0003000f0000ULL; RouteState route;
    ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    EXPECT_TRUE(family.Configure({}, {.sampleRateHz = 48000}));
    EXPECT_FALSE(family.LastFlashSettings().has_value());
}

TEST(FirefaceSequenceTests, FirmwareZeroOldAndReadFailureAreRejectedBeforeInit) {
    for (const auto [revision, fail] : {std::pair<uint32_t,uint64_t>{0,0}, {0x24c,0}, {0x24d,0}, {0x24d,0x000200000100ULL}}) {
        FirefaceScriptBus bus; bus.revision = revision; bus.failReadAddress = fail;
        RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
        ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
        auto configured = family.Configure({}, {.sampleRateHz = 48000});
        if (revision == 0x24d && fail == 0) EXPECT_TRUE(configured);
        else EXPECT_FALSE(configured);
        EXPECT_TRUE(bus.writes.empty());
    }
}

TEST(FirefaceSequenceTests, FF400FirmwareGateRejectsZeroOldAndAsyncReadErrors) {
    for (const auto [revision, fail] : {std::pair<uint32_t,uint64_t>{0,0}, {0x145,0}, {0x146,0x80100520ULL}, {0x146,0x80100290ULL}}) {
        FirefaceScriptBus bus; bus.revision = revision; bus.failReadAddress = fail;
        RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
        ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
        EXPECT_FALSE(family.Configure({}, {.sampleRateHz = 48000}));
        ASSERT_EQ(bus.writes.size(), 1U);
        EXPECT_EQ(bus.writes.front().lo, 0x80100520U); // only the permitted get_revision command
    }
}

TEST(FirefaceSequenceTests, ExternalSourceRequiresSelectedActiveLocked48Clock) {
    FirefaceScriptBus bus;
    bus.status1 = 0x1000U | 0x6U; // configured word clock, 48 kHz
    bus.status0 = 0x01800000U | 0x06000000U; // sync reference "none" (FFADO SR0_AUTOSYNC_SRC_NONE)
    RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
    EXPECT_FALSE(family.Configure({}, {.sampleRateHz = 48000}));
    for (const auto& w : bus.writes)  // flash protocol only; no fetch/init/mixer writes
        EXPECT_TRUE(w.lo == 0x80100520U || w.lo == 0x80100288U) << std::hex << w.lo;

    bus.status0 = 0x01000000U | 0x20000000U | 0x40000000U | 0x06000000U;
    ASFW::Audio::RME::FirefaceFamilyDriver locked(io, ASFW::Audio::RME::FirefaceModel::kFF400, false, &bus.timer);
    EXPECT_TRUE(locked.Configure({}, {.sampleRateHz = 48000}));
}

TEST(FirefaceSequenceTests, InternalFlagWinsOverSavedExternalSelection) {
    // The device keeps the saved external choice (bits 12:10) while bit 0 selects
    // internal clock; Linux parse_clock_bits tests bit 0 first.
    for (const uint32_t savedExternal : {0x0400U, 0x0c00U, 0x1000U, 0x1800U}) {
        FirefaceScriptBus bus;
        bus.status1 = savedExternal | 0x1U | 0x6U; // internal, 48 kHz
        RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
        ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
        ASFW::Audio::AudioDuplexChannels channels{};
        ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000})) << std::hex << savedExternal;
        ASSERT_TRUE(family.AssignChannels(channels)) << std::hex << savedExternal;
        const auto health = family.ReadHealth(0);
        ASSERT_TRUE(health);
        EXPECT_TRUE(health->sourceLocked) << std::hex << savedExternal;
    }
}

TEST(FirefaceSequenceTests, InternalClockIgnoresTheSyncReferenceField) {
    // status0 & 0x01c00000 names the reference a slave follows (FFADO
    // fireface_def.h:323-329); a master leaves it at whatever it last saw.
    // Bit 0 of status1 alone means internal (Linux ff-protocol-former.c:54).
    // Status words below are the FF800 field log of 2026-10-10.
    for (const uint32_t status0 : {0x060014c0U, 0x060014c1U, 0x01800000U, 0x00400000U}) {
        for (const uint32_t status1 : {0x88001007U, 0xa8001007U}) {
            FirefaceScriptBus bus;
            bus.status0 = status0;
            bus.status1 = status1;
            bus.captureWord = 1;
            RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
            ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
            ASFW::Audio::AudioDuplexChannels channels{};
            ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000})) << std::hex << status0;
            const auto assigned = family.AssignChannels(channels);
            ASSERT_TRUE(assigned) << std::hex << status0 << " " << status1;
            EXPECT_EQ(assigned->deviceToHostIsoChannel, 1U);
            const auto health = family.ReadHealth(0);
            ASSERT_TRUE(health);
            EXPECT_TRUE(health->sourceLocked) << std::hex << status0;
        }
    }
}

TEST(FirefaceSequenceTests, InternalClockStillRequires48kConfigured) {
    FirefaceScriptBus bus;
    bus.status1 = 0x88001001U;  // internal, 44.1 kHz: the FF800 before our init
    RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    const auto assigned = family.AssignChannels(channels);
    ASSERT_FALSE(assigned);
    EXPECT_EQ(assigned.error(), kIOReturnNotReady);
}

TEST(FirefaceSequenceTests, FieldFF800StartsFromItsRealFlashAndStatus) {
    // Replays the FF800 field log of 2026-10-10: its flash block, its status
    // before init, and what it reported after init (48 kHz, transmit channel 1).
    FirefaceScriptBus bus;
    bus.flashImage.fill(0);
    bus.flashImage[4] = 1; bus.flashImage[9] = 3; bus.flashImage[29] = 2; bus.flashImage[30] = 1;
    bus.flashImage[36] = 1; bus.flashImage[38] = 1; bus.flashImage[43] = 0xac44;
    bus.status0 = 0x040014b0U; bus.status1 = 0x88001001U;
    bus.captureWord = 0xffffffffU; bus.status3 = 0xffffffffU;
    bus.writeHook = [&bus](uint64_t key) {
        if (key == 0x00020000001cULL) {  // FF800 init
            bus.status0 = 0x060014c0U; bus.status1 = 0x88001007U; bus.captureWord = 1;
        }
    };
    RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};

    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    const auto& dryRun = family.LastSettingsDryRun();
    ASSERT_TRUE(dryRun.config.has_value());
    EXPECT_EQ(*dryRun.config, (ASFW::Audio::RME::ConfigWords{0x00000914U, 0x0000035eU, 0x8000101fU}));
    ASSERT_TRUE(dryRun.status.has_value());
    EXPECT_TRUE(dryRun.status->Matches());

    const auto assigned = family.AssignChannels(channels);
    ASSERT_TRUE(assigned) << "the field log failed here with kIOReturnNotReady";
    EXPECT_EQ(assigned->deviceToHostIsoChannel, 1U);
    ASSERT_TRUE(family.ArmDeviceTxAndEnable());
    ASSERT_TRUE(family.Confirm());
    // Her configuration, written once and before init (FFADO init_hardware
    // writes it before streaming starts).
    const auto config = std::ranges::find_if(bus.writes, [](const auto& w) { return w.lo == 0xfc88f014U; });
    const auto init = std::ranges::find_if(bus.writes, [](const auto& w) { return w.hi == 0x2U && w.lo == 0x1cU; });
    ASSERT_NE(config, bus.writes.end());
    ASSERT_NE(init, bus.writes.end());
    EXPECT_LT(config, init);
    EXPECT_EQ(config->bytes, LeWords({0x00000914U, 0x0000035eU, 0x8000101fU}));
    EXPECT_EQ(std::ranges::count_if(bus.writes, [](const auto& w) { return w.lo == 0xfc88f014U; }), 1);
}

TEST(FirefaceSequenceTests, FF800CapturePollTimesOutAtBoundAndHonorsCancellation) {
    FirefaceScriptBus bus; bus.captureWord = 0xffffffffU;
    RouteState route; ProtocolRegisterIO io(bus, bus, route.registry, route.route);
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    const uint64_t before = ASFW::Audio::Session::UptimeMilliseconds();
    auto assignment = family.AssignChannels(channels);
    const uint64_t elapsed = ASFW::Audio::Session::UptimeMilliseconds() - before;
    EXPECT_FALSE(assignment);
    EXPECT_EQ(assignment.error(), kIOReturnTimeout);
    EXPECT_GE(elapsed, 1190U);
    // Sanitized CI can add scheduling overhead to the 500 ms polling deadline.
    EXPECT_LT(elapsed, 1600U);
    EXPECT_EQ(family.Stop(), kIOReturnSuccess);
    ExpectStopThenMute(bus, 0x34U, 28U); // assignment timeout resets the partially configured device

    FirefaceScriptBus cancelBus; RouteState cancelRoute;
    ProtocolRegisterIO cancelIo(cancelBus, cancelBus, cancelRoute.registry, cancelRoute.route);
    ASFW::Audio::RME::FirefaceFamilyDriver cancellable(cancelIo, ASFW::Audio::RME::FirefaceModel::kFF800, false, &cancelBus.timer);
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
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
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
    ASFW::Audio::RME::FirefaceFamilyDriver family(io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
    ASFW::Audio::AudioDuplexChannels channels{};
    ASSERT_TRUE(family.Configure(channels, {.sampleRateHz = 48000}));
    ASSERT_TRUE(family.AssignChannels(channels));
    ASSERT_EQ(bus.writes.size(), 2U); // frequency, then the packet format

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
    EXPECT_EQ(bus.writes[2].lo, 0x1cU);
    EXPECT_EQ(bus.writes[2].bytes, bus.writes[0].bytes); // the frequency goes first again
    EXPECT_EQ(bus.writes[3].lo, 0x20U);
}

TEST(FirefaceSequenceTests, FailedFetchInitStartAndStopWritesHaveSafeRollbackBoundaries) {
    auto makeFamily = [](FirefaceScriptBus& bus, RouteState& route, ProtocolRegisterIO*& io,
                         std::unique_ptr<ProtocolRegisterIO>& owned,
                         ASFW::Audio::RME::FirefaceFamilyDriver*& family,
                         std::unique_ptr<ASFW::Audio::RME::FirefaceFamilyDriver>& ownedFamily) {
        owned = std::make_unique<ProtocolRegisterIO>(bus, bus, route.registry, route.route);
        io = owned.get();
        ownedFamily = std::make_unique<ASFW::Audio::RME::FirefaceFamilyDriver>(
            *io, ASFW::Audio::RME::FirefaceModel::kFF800, false, &bus.timer);
        family = ownedFamily.get();
    };
    ASFW::Audio::AudioDuplexChannels channels{};
    {
        FirefaceScriptBus bus; RouteState route; ProtocolRegisterIO* io{};
        std::unique_ptr<ProtocolRegisterIO> owned; ASFW::Audio::RME::FirefaceFamilyDriver* family{};
        std::unique_ptr<ASFW::Audio::RME::FirefaceFamilyDriver> ownedFamily;
        makeFamily(bus, route, io, owned, family, ownedFamily);
        ASSERT_TRUE(family->Configure(channels, {.sampleRateHz=48000}));
        ASSERT_TRUE(family->AssignChannels(channels));
        ASSERT_TRUE(family->ArmDeviceTxAndEnable());
        bus.failWriteAddress = 0x801c0000ULL;
        EXPECT_FALSE(family->Confirm()); // a fetch mask the device refused fails the start
        bus.failWriteAddress = 0;
        EXPECT_EQ(family->Stop(), kIOReturnSuccess);
        ExpectStopThenMute(bus, 0x34U, 28U);
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
        ExpectStopThenMute(bus, 0x34U, 28U);
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
        ExpectStopThenMute(bus, 0x34U, 28U);
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
        const size_t beforeFailedStop = bus.writes.size();
        EXPECT_EQ(family->Stop(), kIOReturnError);
        EXPECT_EQ(bus.writes.size(), beforeFailedStop + 1U); // no mute after a refused stop
        bus.failWriteAddress = 0;
        EXPECT_EQ(family->Stop(), kIOReturnSuccess);
        ExpectStopThenMute(bus, 0x34U, 28U);
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
