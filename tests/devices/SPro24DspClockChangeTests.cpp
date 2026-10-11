// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SPro24DspClockChangeTests.cpp - The Saffire Pro 24 DSP around a clock change.
//
// Hardware, 2026-10-11: a rate change made while the DSP was still applying
// the previous one left it reporting "running" with digital zero on capture
// 1/2, until MixControl's DoDspReset (software notice 0x20) restarted it. The
// protocol now waits for the DSP to report running and not busy before a clock
// write. These tests drive the real SPro24DspProtocol over a simulated DICE
// device with a minimal TCAT extension space: section table, application
// section, DSP status quadlet and the software notice register.

#include <gtest/gtest.h>

#include "DICEDuplexTestSupport.hpp"
#include "FakeDiceWaitClock.hpp"
#include "FakeTimerScheduler.hpp"
#include "SimulatedDiceDevice.hpp"

#include "Audio/Protocols/DICE/Focusrite/SPro24DspProtocol.hpp"
#include "Audio/Protocols/Duplex/FamilyDriver.hpp"

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace {

using namespace ASFW::Testing::DICE;
using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::FamilyDriver;
using ASFW::Testing::FakeDiceWaitClock;
using ASFW::Testing::FakeTimerScheduler;
namespace Focusrite = ASFW::Audio::DICE::Focusrite;

constexpr uint32_t kClockSelect44kInternal =
    (ClockRateIndex::k44100 << ClockSelectBits::kRateShift) |
    static_cast<uint32_t>(ClockSource::Internal);

// The TCAT extension space, as far as the DSP steps read it.
struct FakeTcatExtension {
    static constexpr uint32_t kBase = kDiceBaseAddressLo + ASFW::Audio::DICE::kDICEExtensionOffset;
    static constexpr uint32_t kAppOffsetQuadlets = 0x40;
    static constexpr uint32_t kAppSizeQuadlets = 0x200;
    static constexpr uint32_t kApp = kBase + kAppOffsetQuadlets * 4;
    static constexpr uint32_t kStatus = kApp + Focusrite::Offsets::kDspStatus;
    static constexpr uint32_t kNotice = kApp + Focusrite::Offsets::kSwNotice;

    uint32_t status{Focusrite::DspStatus::kRunning};
    // Status reads left before the DSP reports running (after a restart, or
    // while it is still coming up).
    uint32_t notRunningFor{0};
    uint32_t statusReads{0};
    std::vector<uint32_t> notices;

    std::optional<std::vector<uint8_t>> Read(uint32_t addressLo, uint32_t length) {
        std::vector<uint8_t> bytes(length, 0);
        if (addressLo == kBase && length >= 72) {
            // Nine (offset, size) quadlet pairs; the application section is last.
            ASFW::FW::WriteBE32(bytes.data() + 64, kAppOffsetQuadlets);
            ASFW::FW::WriteBE32(bytes.data() + 68, kAppSizeQuadlets);
            return bytes;
        }
        if (addressLo == kStatus && length == 4) {
            ++statusReads;
            if (notRunningFor > 0 && --notRunningFor == 0) {
                status = Focusrite::DspStatus::kRunning;
            }
            ASFW::FW::WriteBE32(bytes.data(), status);
            return bytes;
        }
        return std::nullopt;
    }

    void Write(uint32_t addressLo, std::span<const uint8_t> data) {
        if (addressLo != kNotice || data.size() != 4) {
            return;
        }
        const uint32_t notice = ASFW::FW::ReadBE32(data.data());
        notices.push_back(notice);
        if (notice == static_cast<uint32_t>(Focusrite::SwNotice::DspReset) &&
            (status & Focusrite::DspStatus::kRunning) != 0) {
            status = 0;
            notRunningFor = 5;
        }
    }
};

struct Pro24DspRig {
    explicit Pro24DspRig(SimulatedDiceOptions options)
        : bus(DiceDeviceImages::kSaffirePro24Dsp, options) {
        bus.Device().ResetToIdle();
        bus.SetExtensionResponder(
            [this](uint32_t addressLo, uint32_t length) { return ext.Read(addressLo, length); },
            [this](uint32_t addressLo, std::span<const uint8_t> data) { ext.Write(addressLo, data); });
        protocol = std::make_unique<Focusrite::SPro24DspProtocol>(
            bus, bus, routeState.registry, routeState.route, nullptr, waitClock, &notifications);
        EXPECT_EQ(protocol->Initialize(), kIOReturnSuccess);
        family = protocol->AsFamilyDriver();
        family->SetTeardownCancelToken(&cancel);
        bus.RouteNotificationsTo(notifications);
        bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, kClockSelect44kInternal);
        bus.Device().SetAchievedClock(ClockRateIndex::k44100, true);
        bus.Trace().Clear();
    }

    IOReturn ApplyClock48k() {
        const auto applied = family->ApplyClockIdle(AudioClockConfig{.sampleRateHz = 48000});
        return applied ? kIOReturnSuccess : applied.error();
    }

    // Wire lines before the CLOCK_SELECT write.
    [[nodiscard]] size_t LineOfClockWrite() const {
        const auto& lines = bus.Trace().Lines();
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].rfind("W ffff.e0000074 ", 0) == 0) return i;
        }
        return lines.size();
    }

    RecordingFireWireBus bus;
    FakeTcatExtension ext;
    RouteState routeState;
    FakeTimerScheduler timer;
    FakeDiceWaitClock waitClock{timer};
    ::ASFW::Audio::DICE::DiceNotificationRouter notifications{routeState.registry};
    std::atomic<bool> cancel{false};
    std::unique_ptr<Focusrite::SPro24DspProtocol> protocol;
    FamilyDriver* family{nullptr};
};

TEST(SPro24DspClockChangeTests, InitialDspWaitUsesCallbacksBeforePublication) {
    Pro24DspRig rig(SimulatedDiceOptions{});
    rig.ext.status = Focusrite::DspStatus::kRunning | Focusrite::DspStatus::kBusy;
    (void)rig.timer.ScheduleAfter(500'000'000ULL, [&] {
        rig.ext.status = Focusrite::DspStatus::kRunning;
    });
    std::optional<IOReturn> result;
    rig.protocol->EnsureRuntimeStreamGeometry([&](IOReturn status) { result = status; });
    rig.timer.Advance(450'000'000ULL);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(rig.LineOfClockWrite(), rig.bus.Trace().Lines().size());
    rig.timer.Advance(50'000'000ULL);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(*result, kIOReturnSuccess);
    EXPECT_EQ(rig.waitClock.Sleeps(), 0U);
    EXPECT_EQ(rig.waitClock.Backoffs(), 0U);
    EXPECT_TRUE(rig.ext.notices.empty());
    EXPECT_EQ(rig.bus.Enable(), 0U);
}

TEST(SPro24DspClockChangeTests, RunningDspIsNotWaitedForOrRestarted) {
    Pro24DspRig rig(SimulatedDiceOptions{});
    ASSERT_EQ(rig.ApplyClock48k(), kIOReturnSuccess);
    EXPECT_EQ(rig.ext.statusReads, 1U);
    EXPECT_TRUE(rig.ext.notices.empty());
    EXPECT_LT(rig.timer.NowNs(), 1'000'000'000ULL);
}

TEST(SPro24DspClockChangeTests, ClockIsNotChangedWhileTheDspIsStillComingUp) {
    Pro24DspRig rig(SimulatedDiceOptions{});
    rig.ext.status = 0;
    rig.ext.notRunningFor = 4;
    ASSERT_EQ(rig.ApplyClock48k(), kIOReturnSuccess);

    // Every status read came before the write, the last one saw it running.
    const size_t clockWrite = rig.LineOfClockWrite();
    ASSERT_LT(clockWrite, rig.bus.Trace().Lines().size());
    size_t statusReadsBefore = 0;
    for (size_t i = 0; i < clockWrite; ++i) {
        if (rig.bus.Trace().Lines()[i].rfind("R ffff.e0200174 ", 0) == 0) ++statusReadsBefore;
    }
    EXPECT_EQ(statusReadsBefore, 4U);
    EXPECT_EQ(rig.ext.statusReads, 4U);
    EXPECT_TRUE(rig.ext.notices.empty());
}

} // namespace
