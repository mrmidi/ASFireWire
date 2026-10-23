// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceNonEapRateTests.cpp - rates of a DICE device without EAP.
//
// Built with ASFW_DICE_MULTIRATE_VALIDATION=1, as every Xcode build is
// (project.yml), so 2x and 4x clocks are programmable.

#include <gtest/gtest.h>

#include "DICEDuplexTestSupport.hpp"
#include "FakeDiceWaitClock.hpp"
#include "FakeTimerScheduler.hpp"
#include "SyntheticDiceImages.hpp"

#include "Audio/Protocols/DICE/TCAT/DICETcatProtocol.hpp"
#include "Audio/Protocols/Duplex/FamilyDriver.hpp"
#include "Audio/Runtime/RateValidation.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>

namespace {

using namespace ASFW::Testing::DICE;

// The real TCAT protocol over a simulated device.
struct ProtocolRig {
    explicit ProtocolRig(const DiceDeviceImage& image) : bus(image) {
        bus.Device().ResetToIdle();
        protocol = std::make_unique<ASFW::Audio::DICE::TCAT::DICETcatProtocol>(
            bus, bus, routeState.registry, routeState.route, nullptr, waitClock, &notifications);
        EXPECT_EQ(protocol->Initialize(), kIOReturnSuccess);
        family = protocol->AsFamilyDriver();
        EXPECT_NE(family, nullptr);
        family->SetTeardownCancelToken(&cancel);
        bus.RouteNotificationsTo(notifications);
    }

    // Start an async stage and advance virtual time until it completes.
    template <typename Start>
    IOReturn Run(const char*, Start&& start) {
        std::optional<IOReturn> result;
        start([&result](IOReturn status) { result = status; });
        for (uint32_t ticks = 0; !result && ticks < 400 && timer.PendingCount() != 0; ++ticks)
            timer.Advance(10'000'000ULL);
        return result.value_or(kIOReturnTimeout);
    }

    RecordingFireWireBus bus;
    RouteState routeState;
    ASFW::Testing::FakeTimerScheduler timer;
    ASFW::Testing::FakeDiceWaitClock waitClock{timer};
    ASFW::Audio::DICE::DiceNotificationRouter notifications{routeState.registry};
    std::atomic<bool> cancel{false};
    std::unique_ptr<ASFW::Audio::IDeviceProtocol> protocol;
    ASFW::Audio::FamilyDriver* family{nullptr};
};

} // namespace

static_assert(ASFW::Audio::Runtime::kDiceHardwareBatch);

// A DICE without EAP describes its current mode only, so every CLOCK_CAPS rate
// is announced and a host-selected rate's layout is read once the clock is
// locked there, as TCAT's RestartStreaming does (Saffire.kext 4.3.0 0xdb02).
// A re-describe after a bus reset never puts a placeholder back over it.
TEST(DiceNonEapRates, HostSelectedRateLayoutIsReadAfterTheSwitch) {
    using ASFW::Audio::Runtime::RateFormation;
    ProtocolRig rig(SyntheticDiceImages::kFirestudio2626RegistersFollowMode);
    const auto formation = [&](uint32_t rate) {
        RateFormation result{};
        if (const auto catalog = rig.protocol->RateFormations()) {
            const auto found = std::ranges::find(*catalog, rate, &RateFormation::sampleRateHz);
            if (found != catalog->end()) result = *found;
        }
        EXPECT_EQ(result.sampleRateHz, rate) << "not in the catalog";
        return result;
    };
    const auto capture = [](const RateFormation& f) {
        uint32_t total = 0;
        for (const auto& stream : f.capture) total += stream.pcmChannels;
        return total;
    };
    ASSERT_EQ(rig.Run("initial", [&](auto done) { rig.protocol->EnsureRuntimeStreamGeometry(done); }),
              kIOReturnSuccess);
    ASSERT_EQ(rig.protocol->RateFormations()->size(), 4U);
    EXPECT_FALSE(formation(48000).layoutProvisional);
    EXPECT_TRUE(formation(96000).layoutProvisional);
    EXPECT_EQ(capture(formation(96000)), 26U) << "placeholder: the 48 kHz layout";

    // The rate window's ApplyRate programs the clock; its readback observes it.
    const auto applied = rig.family->ApplyClockIdle({.sampleRateHz = 96000});
    ASSERT_TRUE(applied) << applied.error();
    ASSERT_EQ(rig.Run("observe", [&](auto done) {
        rig.protocol->ReadRateObservation([done](IOReturn status, auto observed) {
            EXPECT_TRUE(observed.clockConfirmed);
            EXPECT_EQ(observed.caps.sampleRateHz, 96000U);
            EXPECT_EQ(observed.caps.hostInputPcmChannels, 18U);
            done(status);
        });
    }), kIOReturnSuccess);
    EXPECT_FALSE(formation(96000).layoutProvisional);
    EXPECT_EQ(capture(formation(96000)), 18U);
    EXPECT_TRUE(formation(88200).layoutProvisional) << "one observed rate is evidence for that rate only";
    EXPECT_EQ(capture(formation(48000)), 26U);

    // Bus reset: the protocol re-describes the device at the new generation.
    rig.bus.BusReset();
    ASFW::Discovery::ConfigROM rom{};
    rom.bib.guid = rig.routeState.route.guid;
    rom.gen = rig.bus.GetGeneration();
    rom.nodeId = rig.routeState.route.nodeId;
    (void)rig.routeState.registry.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
    const auto next = rig.routeState.registry.CurrentRoute(rom.bib.guid);
    ASSERT_TRUE(next);
    rig.protocol->UpdateRuntimeContext(*next, nullptr);
    ASSERT_EQ(rig.Run("re-describe", [&](auto done) { rig.protocol->EnsureRuntimeStreamGeometry(done); }),
              kIOReturnSuccess);
    EXPECT_FALSE(formation(48000).layoutProvisional);
    EXPECT_EQ(capture(formation(48000)), 26U);
    EXPECT_FALSE(formation(96000).layoutProvisional);
    EXPECT_EQ(capture(formation(96000)), 18U);
}
