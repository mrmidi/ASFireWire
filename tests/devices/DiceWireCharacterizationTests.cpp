// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceWireCharacterizationTests.cpp - Golden wire traces of today's DICE bring-up.
//
// Each test drives a DICE
// protocol through its FamilyDriver (the callback interface it used before),
// in the order the session calls it, against a SimulatedDiceDevice built from a recorded device, and
// compares every transaction put on the wire with tests/golden/dice/*.trace.
//
// These tests characterize behaviour; they do not judge it. A golden may record
// a failure or an invariant violation ("!" line) that today's code really
// produces. Stage S1 replaces the bring-up and must reproduce these traces
// except for differences it declares.
//
// Protocols are built exactly as FamilyProtocolConstruction builds them in
// production, with a virtual-time wait clock in place of DriverKitWaitClock.
//
// Regenerate after an intended change: ASFW_UPDATE_GOLDEN=1, then review the diff.

#include <gtest/gtest.h>

#include "DICEDuplexTestSupport.hpp"
#include "FakeDiceWaitClock.hpp"
#include "FakeTimerScheduler.hpp"
#include "SimulatedDiceDevice.hpp"
#include "SyntheticDiceImages.hpp"
#include "WireTrace.hpp"

#include "Audio/Protocols/DICE/Focusrite/SPro24DspProtocol.hpp"
#include "Audio/Protocols/DICE/TCAT/DICETcatProtocol.hpp"
#include "Audio/Protocols/Duplex/FamilyDriver.hpp"

#include <atomic>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>

namespace {

using namespace ASFW::Testing::DICE;
using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::AudioDuplexChannels;
using ASFW::Audio::AudioStreamRuntimeCaps;
using ASFW::Audio::IDeviceProtocol;
using ASFW::Audio::FamilyDriver;
using ASFW::Audio::kMaxAudioStreamsPerDirection;
using ASFW::Testing::FakeDiceWaitClock;
using ASFW::Testing::FakeTimerScheduler;

constexpr uint32_t kPollTickMs = 10;
constexpr uint32_t kMaxTicks = 400;  // 4 s of virtual time bounds every stage

constexpr uint32_t kClockSelect44kInternal =
    (ClockRateIndex::k44100 << ClockSelectBits::kRateShift) |
    static_cast<uint32_t>(ClockSource::Internal);

// One device on one bus, with the protocol production would build for it.
struct DiceRig {
    explicit DiceRig(const DiceDeviceImage& image, SimulatedDiceOptions options = {})
        : bus(image, options) {
        bus.Device().ResetToIdle();
        if (std::string_view(image.key) == "saffire-pro24-dsp") {
            protocol = std::make_unique<ASFW::Audio::DICE::Focusrite::SPro24DspProtocol>(
                bus, bus, routeState.registry, routeState.route, nullptr, waitClock, &notifications);
        } else {
            protocol = std::make_unique<ASFW::Audio::DICE::TCAT::DICETcatProtocol>(
                bus, bus, routeState.registry, routeState.route, nullptr, waitClock, &notifications);
        }
        EXPECT_EQ(protocol->Initialize(), kIOReturnSuccess);
        family = protocol->AsFamilyDriver();
        EXPECT_NE(family, nullptr);
        family->SetTeardownCancelToken(&cancel);
        bus.Trace().Clear();
        bus.RouteNotificationsTo(notifications);
    }

    // Stage the device's clock: what it was asked for and what it achieved.
    void SetClock(uint32_t clockSelect, uint32_t achievedRateIndex, bool locked) {
        bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, clockSelect);
        bus.Device().SetAchievedClock(achievedRateIndex, locked);
    }

    void Mark(const std::string& line) { bus.Trace().Add(line); }

    // Start an async stage and pump the virtual clock until it completes.
    template <typename Start>
    IOReturn Run(const char* stage, Start&& start) {
        Mark(std::string("## ") + stage);
        std::optional<IOReturn> result;
        // Measure the stage's wait on the virtual clock, not by counting pump
        // ticks: a synchronous implementation advances the clock itself while
        // it waits, and must produce the same trace.
        const uint64_t startNs = timer.NowNs();
        start([&result](IOReturn status) { result = status; });
        uint32_t ticks = 0;
        for (; !result && ticks < kMaxTicks; ++ticks) {
            if (timer.PendingCount() == 0) {
                Mark("# stalled: no completion and no timer pending");
                break;
            }
            timer.Advance(uint64_t{kPollTickMs} * 1'000'000ULL);
        }
        const uint64_t waitedMs = (timer.NowNs() - startNs) / 1'000'000ULL;
        if (waitedMs > 0) {
            Mark("# waited " + std::to_string(waitedMs) + "ms");
        }
        if (!result) {
            Mark("# gave up waiting");
        }
        const IOReturn status = result.value_or(kIOReturnTimeout);
        char line[48];
        std::snprintf(line, sizeof(line), "## -> 0x%08x", static_cast<uint32_t>(status));
        Mark(line);
        return status;
    }

    // Run a synchronous stage, with its marker ahead of the traffic it causes.
    template <typename Call>
    IOReturn Sync(const char* stage, Call&& call) {
        Mark(std::string("## ") + stage);
        const IOReturn status = call();
        char line[48];
        std::snprintf(line, sizeof(line), "## -> 0x%08x", static_cast<uint32_t>(status));
        Mark(line);
        return status;
    }

    // Channels the way DuplexStreamProfile::ResolveChannels assigns them for an
    // idle device: stream 0 capture = 1, playback = 0, then the lowest free.
    AudioDuplexChannels ChannelsFor(const AudioStreamRuntimeCaps& caps) const {
        AudioDuplexChannels channels{};
        channels.captureStreamCount = std::max<uint32_t>(1, std::min(caps.deviceToHostStreamCount, kMaxAudioStreamsPerDirection));
        channels.playbackStreamCount = std::max<uint32_t>(1, std::min(caps.hostToDeviceStreamCount, kMaxAudioStreamsPerDirection));
        channels.deviceToHostIsoChannel = 1;
        channels.hostToDeviceIsoChannel = 0;
        channels.captureIsoChannels[0] = 1;
        channels.playbackIsoChannels[0] = 0;
        uint8_t next = 2;
        for (uint32_t i = 1; i < channels.captureStreamCount; ++i) {
            channels.captureIsoChannels[i] = next++;
        }
        for (uint32_t i = 1; i < channels.playbackStreamCount; ++i) {
            channels.playbackIsoChannels[i] = next++;
        }
        return channels;
    }

    // Every stage the session runs for a start, in its order.
    IOReturn Start(uint32_t rateHz) {
        IOReturn status = Run("EnsureRuntimeStreamGeometry", [&](auto done) {
            protocol->EnsureRuntimeStreamGeometry(done);
        });
        if (status != kIOReturnSuccess) {
            return status;
        }
        AudioStreamRuntimeCaps caps{};
        (void)protocol->GetRuntimeAudioStreamCaps(caps);
        const AudioDuplexChannels channels = ChannelsFor(caps);

        status = Run("PrepareDuplex", [&](auto done) {
            done(StatusOf(family->Configure(channels, AudioClockConfig{.sampleRateHz = rateHz})));
        });
        if (status != kIOReturnSuccess) {
            return status;
        }
        EXPECT_TRUE(family->AssignChannels(channels));
        status = Run("ProgramRx", [&](auto done) { done(StatusOf(family->ArmDeviceRx())); });
        if (status != kIOReturnSuccess) {
            return status;
        }
        status = Run("ProgramTxAndEnableDuplex",
                     [&](auto done) { done(StatusOf(family->ArmDeviceTxAndEnable())); });
        if (status != kIOReturnSuccess) {
            return status;
        }
        return Run("ConfirmDuplexStart", [&](auto done) { done(StatusOf(family->Confirm())); });
    }

    IOReturn Stop() { return Sync("StopDuplex", [this] { return family->Stop(); }); }

    template <typename T>
    static IOReturn StatusOf(const std::expected<T, IOReturn>& result) {
        return result ? kIOReturnSuccess : result.error();
    }

    // Final device state, so leftovers (an owner, an armed stream) show up.
    void Summarize() {
        const auto& dev = bus.Device();
        std::string line = "== owner=";
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%016llx enable=%u tx=[",
                      static_cast<unsigned long long>(dev.Owner()), dev.Enable());
        line += buf;
        for (uint32_t i = 0; i < dev.TxCount(); ++i) {
            line += (i ? "," : "") + std::to_string(dev.TxIso(i));
        }
        line += "] rx=[";
        for (uint32_t i = 0; i < dev.RxCount(); ++i) {
            line += (i ? "," : "") + std::to_string(dev.RxIso(i));
        }
        line += "]";
        Mark(line);
    }

    void ExpectGolden(const char* scenario) {
        Summarize();
        const std::string path = std::string("dice/") + bus.Device().Image().key + "__" + scenario + ".trace";
        ::ASFW::Testing::ExpectMatchesGolden(bus.Trace(), path);
    }

    RecordingFireWireBus bus;
    RouteState routeState;
    FakeTimerScheduler timer;
    FakeDiceWaitClock waitClock{timer};
    ::ASFW::Audio::DICE::DiceNotificationRouter notifications{routeState.registry};
    std::atomic<bool> cancel{false};
    std::unique_ptr<IDeviceProtocol> protocol;
    FamilyDriver* family{nullptr};
};

// Publication must wait for callback-driven hardware preparation. Application
// StartIO then reuses the verified clock rather than reconfiguring live hardware.
TEST(DiceInitialPreparation, JoinsCallbacksWithoutSleepingAndTheStartWritesTheClockAgain) {
    DiceRig rig(DiceDeviceImages::kVeniceF24);
    unsigned completions = 0;
    auto done = [&](IOReturn status) { EXPECT_EQ(status, kIOReturnSuccess); ++completions; };
    rig.protocol->EnsureRuntimeStreamGeometry(done);
    rig.protocol->EnsureRuntimeStreamGeometry(done);
    EXPECT_EQ(completions, 0U);
    EXPECT_EQ(rig.bus.Enable(), 0U);
    rig.timer.Advance(399'000'000ULL);
    EXPECT_EQ(completions, 0U);
    rig.timer.Advance(1'000'000ULL);
    ASSERT_EQ(completions, 2U);
    EXPECT_EQ(rig.waitClock.Sleeps(), 0U);
    EXPECT_EQ(rig.waitClock.Backoffs(), 0U);
    EXPECT_EQ(rig.bus.Enable(), 0U);
    auto clockWrites = [&] {
        return std::ranges::count_if(rig.bus.Operations(), [](const auto& op) {
            return op.kind == OpKind::Write && op.addressLo == 0xe0000074;
        });
    };
    EXPECT_EQ(clockWrites(), 1);
    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(rig.protocol->GetRuntimeAudioStreamCaps(caps));
    rig.bus.ClearOperations();
    // TCAT RestartStreaming writes CLOCK_SELECT on every start (kext 0xe3fe).
    ASSERT_TRUE(rig.family->Configure(rig.ChannelsFor(caps), {.sampleRateHz = caps.sampleRateHz}));
    EXPECT_EQ(clockWrites(), 1);
}

TEST(DiceInitialPreparation, DisablesDeviceBeforeColdClockEvenWhenAlreadyDisabled) {
    for (const uint32_t initialEnable : {0U, 1U}) {
        SCOPED_TRACE(initialEnable);
        DiceRig rig(DiceDeviceImages::kVeniceF24);
        rig.bus.Device().SetGlobalQuad(GlobalOffset::kEnable, initialEnable);
        ASSERT_EQ(rig.Run("initial", [&](auto done) {
            rig.protocol->EnsureRuntimeStreamGeometry(done);
        }), kIOReturnSuccess);
        const auto& ops = rig.bus.Operations();
        const auto disable = std::ranges::find_if(ops, [](const auto& op) {
            return op.kind == OpKind::Write && op.addressLo == 0xE0000078U &&
                   op.payload.size() == 4 && ASFW::FW::ReadBE32(op.payload.data()) == 0;
        });
        const auto clock = std::ranges::find_if(ops, [](const auto& op) {
            return op.kind == OpKind::Write && op.addressLo == 0xE0000074U;
        });
        ASSERT_NE(disable, ops.end());
        ASSERT_NE(clock, ops.end());
        EXPECT_LT(disable, clock);
        EXPECT_EQ(rig.bus.Enable(), 0U);
        EXPECT_EQ(rig.waitClock.Sleeps(), 0U);
    }
}

TEST(DiceInitialPreparation, FailedDisableStopsBeforeClockAndCompletesOnce) {
    DiceRig rig(DiceDeviceImages::kVeniceF24);
    rig.bus.FailNext(OpKind::Write, 0xE0000078U, ASFW::Async::AsyncStatus::kTimeout);
    unsigned completions = 0;
    rig.protocol->EnsureRuntimeStreamGeometry([&](IOReturn status) {
        EXPECT_EQ(status, kIOReturnTimeout);
        ++completions;
    });
    rig.timer.Advance(500'000'000ULL);
    EXPECT_EQ(completions, 1U);
    EXPECT_FALSE(std::ranges::any_of(rig.bus.Operations(), [](const auto& op) {
        return op.kind == OpKind::Write && op.addressLo == 0xE0000074U;
    }));
}

TEST(DiceInitialPreparation, AcceptanceDoesNotCompleteBeforeRelock) {
    DiceRig rig(DiceDeviceImages::kVeniceF24);
    rig.bus.SetClockSelectWriteHandler([&] {
        rig.bus.Device().SetAchievedClock(ClockRateIndex::k48000, false);
        (void)rig.timer.ScheduleAfter(40'000'000ULL, [&] {
            rig.bus.Device().RaiseNotification(NotifyBits::kClockAccepted | NotifyBits::kTxConfigChange);
        });
        (void)rig.timer.ScheduleAfter(220'000'000ULL, [&] {
            rig.bus.Device().SetAchievedClock(ClockRateIndex::k48000, true);
            rig.bus.Device().RaiseNotification(NotifyBits::kLockChange);
        });
    });
    std::optional<IOReturn> result;
    rig.protocol->EnsureRuntimeStreamGeometry([&](IOReturn status) { result = status; });
    rig.timer.Advance(610'000'000ULL);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(rig.bus.Enable(), 0U);
    rig.timer.Advance(10'000'000ULL);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, kIOReturnSuccess);
    EXPECT_EQ(rig.waitClock.Sleeps(), 0U);
    EXPECT_EQ(rig.waitClock.Backoffs(), 0U);
}

TEST(DiceInitialPreparation, LateLockEventCannotValidateAnOlderGlobalSnapshot) {
    DiceRig rig(DiceDeviceImages::kVeniceF24);
    unsigned globalReads = 0;
    rig.bus.SetGlobalReadCompletedHandler([&] {
        if (++globalReads != 3) return; // pre-claim, owner verification, first clock poll
        rig.bus.Device().SetAchievedClock(ClockRateIndex::k48000, false);
        rig.bus.Device().RaiseNotification(NotifyBits::kLockChange);
        (void)rig.timer.ScheduleAfter(20'000'000ULL, [&] {
            rig.bus.Device().SetAchievedClock(ClockRateIndex::k48000, true);
            rig.bus.Device().RaiseNotification(NotifyBits::kLockChange);
        });
    });
    std::optional<IOReturn> result;
    rig.protocol->EnsureRuntimeStreamGeometry([&](IOReturn status) { result = status; });
    rig.timer.Advance(400'000'000ULL);
    EXPECT_FALSE(result.has_value());
    rig.timer.Advance(10'000'000ULL);
    EXPECT_FALSE(result.has_value());
    rig.timer.Advance(10'000'000ULL);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, kIOReturnSuccess);
    EXPECT_EQ(rig.waitClock.Sleeps(), 0U);
}

TEST(DiceInitialPreparation, ShutdownCancelsPendingPreparationExactlyOnce) {
    DiceRig rig(DiceDeviceImages::kVeniceF24);
    unsigned completions = 0;
    rig.protocol->EnsureRuntimeStreamGeometry([&](IOReturn status) {
        EXPECT_EQ(status, kIOReturnAborted); ++completions;
    });
    EXPECT_EQ(rig.protocol->Shutdown(), kIOReturnSuccess);
    EXPECT_EQ(completions, 1U);
    const auto operations = rig.bus.Operations().size();
    rig.protocol.reset();
    rig.timer.Advance(5'000'000'000ULL);
    EXPECT_EQ(completions, 1U);
    EXPECT_EQ(rig.bus.Operations().size(), operations);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(DiceInitialPreparation, StaleGlobalAcceptanceDoesNotBypassTheAcceptanceWait) {
    DiceRig rig(DiceDeviceImages::kVeniceF24);
    rig.bus.Device().SetNotificationRegister(NotifyBits::kClockAccepted);
    rig.bus.SetClockSelectWriteHandler([] {}); // retain the old locked state, emit no new acceptance
    std::optional<IOReturn> result;
    rig.protocol->EnsureRuntimeStreamGeometry([&](IOReturn status) { result = status; });
    rig.timer.Advance(540'000'000ULL);
    EXPECT_FALSE(result.has_value());
    rig.timer.Advance(10'000'000ULL); // reference-compatible final live confirmation at 150 ms
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, kIOReturnSuccess);
    EXPECT_EQ(rig.waitClock.Sleeps(), 0U);
}

TEST(DiceInitialPreparation, UnlockedClockTimesOutWithoutEnablingStreams) {
    DiceRig rig(DiceDeviceImages::kVeniceF24,
                {.clockResponse = DiceClockResponse::kAcceptNeverLock});
    std::optional<IOReturn> result;
    rig.protocol->EnsureRuntimeStreamGeometry([&](IOReturn status) { result = status; });
    rig.timer.Advance(3'400'000'000ULL);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, kIOReturnTimeout);
    EXPECT_EQ(rig.bus.Enable(), 0U);
    EXPECT_EQ(rig.waitClock.Sleeps(), 0U);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

TEST(DiceInitialPreparation, StaleRouteCancelsWithoutTouchingTheNewGeneration) {
    DiceRig rig(DiceDeviceImages::kVeniceF24);
    std::optional<IOReturn> result;
    rig.protocol->EnsureRuntimeStreamGeometry([&](IOReturn status) { result = status; });
    rig.routeState.registry.InvalidateLiveMappingsForBusReset();
    const auto operations = rig.bus.Operations().size();
    rig.timer.Advance(400'000'000ULL);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, kIOReturnOffline);
    EXPECT_EQ(rig.bus.Operations().size(), operations);
    EXPECT_EQ(rig.timer.PendingCount(), 0U);
}

// Issue #190: a StudioLive 32.4.2 sends 48 PCM and receives 34 in one stream
// each, wider than the old 32-slot AM824 limit. Its only formation was refused
// and the host side clamped both directions to 32 (AUX1-16 and 2TrackL/R were
// missing), while the device slipped on our 32-slot playback packets.
TEST(DiceWideStream, StudioLive3242KeepsEveryChannelOnTheWireAndTheHost) {
    DiceRig rig(DiceDeviceImages::kStudiolive3242);
    ASSERT_EQ(rig.Run("initial", [&](auto done) { rig.protocol->EnsureRuntimeStreamGeometry(done); }),
              kIOReturnSuccess);

    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(rig.protocol->GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.hostInputPcmChannels, 48U);
    EXPECT_EQ(caps.hostOutputPcmChannels, 34U);
    EXPECT_EQ(caps.deviceToHostAm824Slots, 48U);
    EXPECT_EQ(caps.hostToDeviceAm824Slots, 34U);

    const auto formations = rig.protocol->RateFormations();
    ASSERT_TRUE(formations);
    ASSERT_EQ(formations->size(), 1U);  // CLOCK_CAPS offers 48 kHz only
    const auto& formation = formations->front();
    EXPECT_EQ(formation.sampleRateHz, 48000U);
    ASSERT_EQ(formation.capture.size(), 1U);
    EXPECT_EQ(formation.capture[0].pcmChannels, 48U);
    EXPECT_EQ(formation.capture[0].dataBlockSize, 48U);
    ASSERT_EQ(formation.playback.size(), 1U);
    EXPECT_EQ(formation.playback[0].pcmChannels, 34U);
    EXPECT_EQ(formation.playback[0].dataBlockSize, 34U);

    std::vector<std::string> inNames;
    std::vector<std::string> outNames;
    ASSERT_TRUE(rig.protocol->GetChannelLabels(inNames, outNames));
    ASSERT_EQ(inNames.size(), 48U);
    EXPECT_EQ(inNames.back(), "AUX16");
    ASSERT_EQ(outNames.size(), 34U);
    EXPECT_EQ(outNames.back(), "2TrackR");

    ASSERT_TRUE(rig.family->Configure(rig.ChannelsFor(caps), {.sampleRateHz = 48000}));
}

TEST(DiceInitialPreparation, PreservesObservedRateAndPreparesRestoredRateBeforeStart) {
    DiceRig rig(DiceDeviceImages::kVeniceF24);
    rig.SetClock(kClockSelect44kInternal, ClockRateIndex::k44100, true);
    ASSERT_EQ(rig.Run("initial", [&](auto done) { rig.protocol->EnsureRuntimeStreamGeometry(done); }),
              kIOReturnSuccess);
    AudioStreamRuntimeCaps caps{};
    ASSERT_TRUE(rig.protocol->GetRuntimeAudioStreamCaps(caps));
    EXPECT_EQ(caps.sampleRateHz, 44100U);
    ASSERT_TRUE(rig.family->ApplyClockIdle({.sampleRateHz = 48000}));
    rig.bus.ClearOperations();
    ASSERT_TRUE(rig.family->Configure(rig.ChannelsFor(caps), {.sampleRateHz = 48000}));
    EXPECT_EQ(std::ranges::count_if(rig.bus.Operations(), [](const auto& op) {
        return op.kind == OpKind::Write && op.addressLo == 0xe0000074;
    }), 1);
}

TEST(DiceInitialPreparation, ReadsObservedHighRateWithoutLowerAdvertisedFallback) {
    // Synthetic high-rate clock state on the recorded register layout. The
    // stream widths here are test inputs, not measured high-rate hardware caps.
    for (const auto& rate : ASFW::Audio::DICE::kDiceRateTable) {
        if (rate.hz <= 48000) continue;
        SCOPED_TRACE(rate.hz);
        DiceRig rig(DiceDeviceImages::kStudiolive3242);
        uint32_t selection = 0;
        ASSERT_TRUE(DiceClockSelectForRate(rate.hz, ClockSource::Internal, selection));
        rig.SetClock(selection, rate.rateIndex, true);
        rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockCaps, rate.capsBit);
        ASSERT_EQ(rig.Run("initial", [&](auto done) {
            rig.protocol->EnsureRuntimeStreamGeometry(done);
        }), kIOReturnSuccess);
        AudioStreamRuntimeCaps caps{};
        ASSERT_TRUE(rig.protocol->GetRuntimeAudioStreamCaps(caps));
        EXPECT_EQ(caps.sampleRateHz, rate.hz);
        const auto formations = rig.protocol->RateFormations();
        ASSERT_TRUE(formations);
        ASSERT_EQ(formations->size(), 1U);
        EXPECT_EQ(formations->front().sampleRateHz, rate.hz);
    }
}

TEST(DiceInitialPreparation, RestoresHostRateWhenCurrentModeMaskExcludesIt) {
    // Reproduce #190's changing clock-capability mask on the recorded layout.
    // Exercise both a stream restart and a stopped-IO clock change.
    for (const bool idleChange : {false, true}) {
        SCOPED_TRACE(idleChange);
        DiceRig rig(DiceDeviceImages::kStudiolive3242);
        uint32_t selection = 0;
        ASSERT_TRUE(DiceClockSelectForRate(88200, ClockSource::Internal, selection));
        rig.SetClock(selection, ClockRateIndex::k88200, true);
        rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockCaps,
                                      ASFW::Audio::DICE::RateCaps::k88200);
        ASSERT_EQ(rig.Run("initial", [&](auto done) {
            rig.protocol->EnsureRuntimeStreamGeometry(done);
        }), kIOReturnSuccess);
        AudioStreamRuntimeCaps caps{};
        ASSERT_TRUE(rig.protocol->GetRuntimeAudioStreamCaps(caps));
        ASSERT_EQ(caps.sampleRateHz, 88200U);
        rig.bus.ClearOperations();
        if (idleChange) {
            const auto applied = rig.family->ApplyClockIdle({.sampleRateHz = 48000});
            ASSERT_TRUE(applied) << applied.error();
            EXPECT_EQ(applied->runtimeCaps.sampleRateHz, 48000U);
        } else {
            const auto prepared = rig.family->Configure(rig.ChannelsFor(caps),
                                                        {.sampleRateHz = 48000});
            ASSERT_TRUE(prepared) << prepared.error();
            EXPECT_EQ(prepared->runtimeCaps.sampleRateHz, 48000U);
        }
        EXPECT_EQ(std::ranges::count_if(rig.bus.Operations(), [](const auto& op) {
            return op.kind == OpKind::Write && op.addressLo == 0xe0000074;
        }), 1);
    }
}

TEST(DiceInitialPreparation, ExternalRateObservationRetainsBothMeasuredRates) {
    DiceRig rig(DiceDeviceImages::kStudiolive3242);
    ASSERT_EQ(rig.Run("initial", [&](auto done) {
        rig.protocol->EnsureRuntimeStreamGeometry(done);
    }), kIOReturnSuccess);
    rig.SetClock(kClockSelect44kInternal, ClockRateIndex::k44100, true);
    rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockCaps,
                                  ASFW::Audio::DICE::RateCaps::k44100);
    rig.bus.ClearOperations();
    ASSERT_EQ(rig.Run("external-rate", [&](auto done) {
        rig.protocol->ReadRateObservation([done](IOReturn status, auto observed) {
            EXPECT_TRUE(observed.clockConfirmed);
            EXPECT_EQ(observed.caps.sampleRateHz, 44100U);
            EXPECT_EQ(observed.caps.hostInputPcmChannels, 48U);
            EXPECT_EQ(observed.caps.hostOutputPcmChannels, 34U);
            done(status);
        });
    }), kIOReturnSuccess);
    ASSERT_EQ(rig.family->LoadGeometry(), kIOReturnSuccess);
    const auto formations = rig.protocol->RateFormations();
    ASSERT_TRUE(formations);
    ASSERT_EQ(formations->size(), 2U);
    EXPECT_EQ(formations->front().sampleRateHz, 44100U);
    EXPECT_EQ(formations->back().sampleRateHz, 48000U);
    EXPECT_EQ(std::ranges::count_if(rig.bus.Operations(), [](const auto& op) {
        return op.kind == OpKind::Write || op.kind == OpKind::Lock;
    }), 0);
}

std::string KeyName(const ::testing::TestParamInfo<const DiceDeviceImage*>& info) {
    std::string name = info.param->key;
    for (char& c : name) {
        if (c == '-') c = '_';
    }
    return name;
}

// A MultiMix announcing two playback streams (synthetic; see
// SyntheticDiceImages.hpp): the device is programmed for both, as Alesis's own
// kext does.
TEST(DiceWireMultimixTwoPlayback, StartStopAt48k) {
    DiceRig rig(SyntheticDiceImages::kMultimixTwoPlayback);
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("start-stop-48k");
}

// ---- every recorded device: clean start and stop at 48 kHz -----------------

class DiceWireAllDevices : public ::testing::TestWithParam<const DiceDeviceImage*> {};

TEST_P(DiceWireAllDevices, StartStopAt48k) {
    DiceRig rig(*GetParam());
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("start-stop-48k");
}

INSTANTIATE_TEST_SUITE_P(Recorded, DiceWireAllDevices,
                         ::testing::ValuesIn(DiceDeviceImages::kAll), KeyName);

// ---- scenarios, on the Pro 24 DSP (production path, no timer) and on a
// ---- multi-stream TCAT device (Venice F24, timer-driven retries) -----------

class DiceWireScenarios : public ::testing::TestWithParam<const DiceDeviceImage*> {};

TEST_P(DiceWireScenarios, AlreadyAtTargetClock) {
    DiceRig rig(*GetParam());
    rig.SetClock(kClockSelect48kInternal, ClockRateIndex::k48000, true);
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("already-at-target");
}

TEST_P(DiceWireScenarios, RequestedRateNotAchieved) {
    // CLOCK_SELECT says 48k, the device runs 44.1k.
    DiceRig rig(*GetParam());
    rig.SetClock(kClockSelect48kInternal, ClockRateIndex::k44100, true);
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("requested-not-achieved");
}

TEST_P(DiceWireScenarios, StartFrom44k) {
    DiceRig rig(*GetParam());
    rig.SetClock(kClockSelect44kInternal, ClockRateIndex::k44100, true);
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("start-from-44k");
}

TEST_P(DiceWireScenarios, IdleClockChange44kTo48k) {
    DiceRig rig(*GetParam());
    rig.SetClock(kClockSelect44kInternal, ClockRateIndex::k44100, true);
    (void)rig.Run("ApplyClockConfig 48000", [&](auto done) {
        done(DiceRig::StatusOf(rig.family->ApplyClockIdle(AudioClockConfig{.sampleRateHz = 48000})));
    });
    rig.ExpectGolden("idle-clock-change-44k-48k");
}

// The device accepts the new rate at once but reaches it later, as a Pro 24
// DSP did on hardware (2026-09-25). The idle apply now waits for the rate, so
// the start that follows finds the device at target and does not write
// CLOCK_SELECT again (S4a).
TEST_P(DiceWireScenarios, IdleClockChangeSettlesLate) {
    DiceRig rig(*GetParam(), SimulatedDiceOptions{.rateSettlesAfterStatusReads = 3});
    rig.SetClock(kClockSelect44kInternal, ClockRateIndex::k44100, true);
    (void)rig.Run("ApplyClockConfig 48000", [&](auto done) {
        done(DiceRig::StatusOf(rig.family->ApplyClockIdle(AudioClockConfig{.sampleRateHz = 48000})));
    });
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("idle-clock-change-settles-late");
}

TEST_P(DiceWireScenarios, ClockNeverAccepted) {
    DiceRig rig(*GetParam(), SimulatedDiceOptions{.clockResponse = DiceClockResponse::kNeverAccept});
    rig.SetClock(kClockSelect44kInternal, ClockRateIndex::k44100, true);
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("clock-never-accepted");
}

TEST_P(DiceWireScenarios, ClockNeverLocks) {
    DiceRig rig(*GetParam(), SimulatedDiceOptions{.clockResponse = DiceClockResponse::kAcceptNeverLock});
    rig.SetClock(kClockSelect44kInternal, ClockRateIndex::k44100, true);
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("clock-never-locks");
}

TEST_P(DiceWireScenarios, ForeignOwner) {
    DiceRig rig(*GetParam());
    rig.bus.Device().SetOwner(0xFFC2000100000000ULL);  // another node's handler
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("foreign-owner");
}

// GLOBAL_VERSION with a major other than 1 is refused before the owner claim,
// the first write (Linux dice-transaction.c:303-322).
TEST(DiceWireVersionGate, AnUnknownMajorVersionIsRefusedBeforeTheClaim) {
    DiceDeviceImage image = DiceDeviceImages::kFirestudioProject;
    image.version = 0x02000000;
    DiceRig rig(image);
    const uint64_t ownerBefore = rig.bus.Device().Owner();
    EXPECT_EQ(rig.Start(48000), kIOReturnUnsupported);
    EXPECT_EQ(rig.bus.Device().Owner(), ownerBefore);
    EXPECT_EQ(rig.bus.Device().Enable(), 0U);
}

TEST_P(DiceWireScenarios, BusResetDuringClockChange) {
    DiceRig rig(*GetParam());
    rig.SetClock(kClockSelect44kInternal, ClockRateIndex::k44100, true);
    rig.bus.SetClockSelectWriteHandler([&rig] { rig.bus.BusReset(); });
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("bus-reset-during-clock-change");
}

TEST_P(DiceWireScenarios, TeardownDuringClockWait) {
    DiceRig rig(*GetParam(), SimulatedDiceOptions{.clockResponse = DiceClockResponse::kAcceptLater});
    rig.SetClock(kClockSelect44kInternal, ClockRateIndex::k44100, true);
    (void)rig.timer.ScheduleAfter(25'000'000ULL, [&rig] {
        rig.Mark("# teardown requested");
        rig.cancel.store(true, std::memory_order_release);
    });
    (void)rig.Start(48000);
    (void)rig.Stop();
    rig.ExpectGolden("teardown-during-clock-wait");
}

TEST_P(DiceWireScenarios, HealthAfterLockNotification) {
    DiceRig rig(*GetParam());
    rig.SetClock(kClockSelect48kInternal, ClockRateIndex::k48000, true);
    (void)rig.Start(48000);
    rig.bus.Device().RaiseNotification(NotifyBits::kLockChange);
    (void)rig.Run("ReadDuplexHealth", [&](auto done) {
        done(DiceRig::StatusOf(rig.family->ReadHealth(1000)));
    });
    (void)rig.Stop();
    rig.ExpectGolden("health-after-lock-notification");
}

TEST_P(DiceWireScenarios, Rate88kRefused) {
    DiceRig rig(*GetParam());
    (void)rig.Start(88200);
    rig.ExpectGolden("rate-88k-refused");
}

INSTANTIATE_TEST_SUITE_P(Recorded, DiceWireScenarios,
                         ::testing::Values(&DiceDeviceImages::kSaffirePro24Dsp,
                                           &DiceDeviceImages::kVeniceF24),
                         KeyName);

} // namespace
