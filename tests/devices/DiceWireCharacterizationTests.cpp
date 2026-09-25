// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceWireCharacterizationTests.cpp - Golden wire traces of today's DICE bring-up.
//
// Stage S0 of documentation/AUDIO_SESSION_REDESIGN.md. Each test drives a DICE
// protocol through IDuplexDeviceControl, in the order the session
// calls it, against a SimulatedDiceDevice built from a recorded device, and
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
#include "WireTrace.hpp"

#include "Audio/Protocols/DICE/Focusrite/SPro24DspProtocol.hpp"
#include "Audio/Protocols/DICE/TCAT/DICETcatProtocol.hpp"
#include "Audio/Protocols/Duplex/IDuplexDeviceControl.hpp"

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
using ASFW::Audio::IDuplexDeviceControl;
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
        control = protocol->AsDuplexDeviceControl();
        EXPECT_NE(control, nullptr);
        control->SetTeardownCancelToken(&cancel);
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
            control->EnsureRuntimeStreamGeometry(done);
        });
        if (status != kIOReturnSuccess) {
            return status;
        }
        AudioStreamRuntimeCaps caps{};
        (void)protocol->GetRuntimeAudioStreamCaps(caps);
        const AudioDuplexChannels channels = ChannelsFor(caps);

        status = Run("PrepareDuplex", [&](auto done) {
            control->PrepareDuplex(channels, AudioClockConfig{.sampleRateHz = rateHz},
                                   [done](IOReturn s, auto) { done(s); });
        });
        if (status != kIOReturnSuccess) {
            return status;
        }
        control->SetAssignedChannels(channels);
        status = Run("ProgramRx", [&](auto done) {
            control->ProgramRx([done](IOReturn s, auto) { done(s); });
        });
        if (status != kIOReturnSuccess) {
            return status;
        }
        status = Run("ProgramTxAndEnableDuplex", [&](auto done) {
            control->ProgramTxAndEnableDuplex([done](IOReturn s, auto) { done(s); });
        });
        if (status != kIOReturnSuccess) {
            return status;
        }
        return Run("ConfirmDuplexStart", [&](auto done) {
            control->ConfirmDuplexStart([done](IOReturn s, auto) { done(s); });
        });
    }

    IOReturn Stop() { return Sync("StopDuplex", [this] { return control->StopDuplex(); }); }

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
    IDuplexDeviceControl* control{nullptr};
};

std::string KeyName(const ::testing::TestParamInfo<const DiceDeviceImage*>& info) {
    std::string name = info.param->key;
    for (char& c : name) {
        if (c == '-') c = '_';
    }
    return name;
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
    // DICE_STABILITY_REGRESSION.md §3: CLOCK_SELECT says 48k, the device runs 44.1k.
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
        rig.control->ApplyClockConfig(AudioClockConfig{.sampleRateHz = 48000},
                                      [done](IOReturn s, auto) { done(s); });
    });
    rig.ExpectGolden("idle-clock-change-44k-48k");
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
        rig.control->ReadDuplexHealth([done](IOReturn s, auto) { done(s); });
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
