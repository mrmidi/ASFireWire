// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceRouterReloadTests.cpp - Re-loading a TCAT device's own router program.
//
// Ordinary attachment and clock changes preserve firmware routing. The first
// tests retain coverage of the explicit opt-in loader; the default-policy
// regression asserts that it issues no router load on either path. The fake
// models section tables, router staging, command completion, and per-mode
// current-config images; it does not model analog/DSP side effects.

#include <gtest/gtest.h>

#include "DICEDuplexTestSupport.hpp"
#include "FakeDiceWaitClock.hpp"
#include "FakeTimerScheduler.hpp"
#include "SimulatedDiceDevice.hpp"

#include "Audio/Protocols/DICE/TCAT/DICETcatProtocol.hpp"
#include "Audio/Protocols/Duplex/FamilyDriver.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace {

using namespace ASFW::Testing::DICE;
using ASFW::Audio::AudioClockConfig;
using ASFW::Audio::FamilyDriver;
using ASFW::Testing::FakeDiceWaitClock;
using ASFW::Testing::FakeTimerScheduler;
namespace Opcode = ASFW::Audio::DICE::ExtensionCommandOpcode;

constexpr uint32_t kClockSelect44kInternal =
    (ClockRateIndex::k44100 << ClockSelectBits::kRateShift) |
    static_cast<uint32_t>(ClockSource::Internal);

struct FakeEapDevice {
    static constexpr uint32_t kBase = kDiceBaseAddressLo + ASFW::Audio::DICE::kDICEExtensionOffset;
    // Quadlet offsets/sizes as the section table states them.
    static constexpr uint32_t kCommandQ = 0x20, kCommandSizeQ = 2;
    static constexpr uint32_t kRouterQ = 0x40, kRouterSizeQ = 0x81;
    static constexpr uint32_t kCurrentQ = 0x100, kCurrentSizeQ = 0x1800;
    static constexpr uint32_t kCommand = kBase + kCommandQ * 4;
    static constexpr uint32_t kRouter = kBase + kRouterQ * 4;
    static constexpr uint32_t kCurrent = kBase + kCurrentQ * 4;

    bool hasRouter{true};
    std::map<uint32_t, uint32_t> quads;  // addressLo -> value
    struct Load {
        uint32_t opcode{0};
        std::vector<uint32_t> image;  // count, then entries
    };
    std::vector<Load> loads;
    std::vector<uint32_t> staged;

    FakeEapDevice() {
        SetTable(ASFW::Audio::DICE::CurrentConfigOffset::kLowRouter, {0x4248, 0x4349, 0x40b2});
        SetTable(ASFW::Audio::DICE::CurrentConfigOffset::kMiddleRouter, {0x4244, 0x4345});
        SetTable(ASFW::Audio::DICE::CurrentConfigOffset::kHighRouter, {});
    }

    void SetTable(uint32_t modeOffset, const std::vector<uint32_t>& entries) {
        const uint32_t base = kCurrent + modeOffset;
        quads[base] = static_cast<uint32_t>(entries.size());
        for (size_t i = 0; i < entries.size(); ++i) quads[base + 4 + static_cast<uint32_t>(i) * 4] = entries[i];
    }

    std::optional<std::vector<uint8_t>> Read(uint32_t addressLo, uint32_t length) {
        std::vector<uint8_t> bytes(length, 0);
        if (addressLo == kBase && length >= 72) {
            // caps, command, mixer, peak, router, streamFormat, currentConfig, standalone, application
            const uint32_t table[18] = {0, 0, kCommandQ, kCommandSizeQ, 0, 0, 0, 0,
                                        hasRouter ? kRouterQ : 0, hasRouter ? kRouterSizeQ : 0,
                                        0, 0, kCurrentQ, kCurrentSizeQ, 0, 0, 0, 0};
            for (int i = 0; i < 18; ++i) ASFW::FW::WriteBE32(bytes.data() + i * 4, table[i]);
            return bytes;
        }
        if (addressLo < kBase || length % 4 != 0) return std::nullopt;
        for (uint32_t i = 0; i < length; i += 4) {
            const auto it = quads.find(addressLo + i);
            ASFW::FW::WriteBE32(bytes.data() + i, it == quads.end() ? 0U : it->second);
        }
        return bytes;
    }

    void Write(uint32_t addressLo, std::span<const uint8_t> data) {
        for (uint32_t i = 0; i + 4 <= data.size(); i += 4) {
            quads[addressLo + i] = ASFW::FW::ReadBE32(data.data() + i);
        }
        if (addressLo == kRouter) {
            staged.clear();
            for (uint32_t i = 0; i + 4 <= data.size(); i += 4) staged.push_back(ASFW::FW::ReadBE32(data.data() + i));
        }
        if (addressLo == kCommand && data.size() == 4) {
            const uint32_t opcode = ASFW::FW::ReadBE32(data.data());
            if ((opcode & Opcode::kExecute) != 0) {
                loads.push_back({opcode, staged});
                quads[kCommand] = opcode & ~Opcode::kExecute;  // completes at once
                quads[kCommand + 4] = 0;                       // return: success
            }
        }
    }
};

struct ReloadRig {
    explicit ReloadRig(std::optional<bool> reload = std::nullopt)
        : bus(DiceDeviceImages::kSaffirePro24Dsp, SimulatedDiceOptions{}) {
        bus.Device().ResetToIdle();
        bus.SetExtensionResponder(
            [this](uint32_t addressLo, uint32_t length) { return eap.Read(addressLo, length); },
            [this](uint32_t addressLo, std::span<const uint8_t> data) { eap.Write(addressLo, data); });
        ASFW::Audio::DICE::TCAT::DICETcatRuntimePolicy policy{};
        if (reload) policy.reloadRouterAfterClockChange = *reload;
        protocol = std::make_unique<ASFW::Audio::DICE::TCAT::DICETcatProtocol>(
            bus, bus, routeState.registry, routeState.route, nullptr, waitClock, &notifications,
            policy);
        EXPECT_EQ(protocol->Initialize(), kIOReturnSuccess);
        family = protocol->AsFamilyDriver();
        family->SetTeardownCancelToken(&cancel);
        bus.RouteNotificationsTo(notifications);
    }

    IOReturn Configure48k() {
        const auto prepared = family->Configure(
            ASFW::Audio::AudioDuplexChannels{.deviceToHostIsoChannel = 1, .hostToDeviceIsoChannel = 0},
            AudioClockConfig{.sampleRateHz = 48000});
        return prepared ? kIOReturnSuccess : prepared.error();
    }

    RecordingFireWireBus bus;
    FakeEapDevice eap;
    RouteState routeState;
    FakeTimerScheduler timer;
    FakeDiceWaitClock waitClock{timer};
    ::ASFW::Audio::DICE::DiceNotificationRouter notifications{routeState.registry};
    std::atomic<bool> cancel{false};
    std::unique_ptr<ASFW::Audio::DICE::TCAT::DICETcatProtocol> protocol;
    FamilyDriver* family{nullptr};
};

TEST(DiceRouterReloadTests, FirstStartLoadsEveryRateModeTheDeviceHolds) {
    ReloadRig rig(true);
    ASSERT_EQ(rig.Configure48k(), kIOReturnSuccess);

    ASSERT_EQ(rig.eap.loads.size(), 2U);  // the high mode has no table
    EXPECT_EQ(rig.eap.loads[0].opcode, Opcode::kExecute | Opcode::kRateLow | Opcode::kLoadRouter);
    EXPECT_EQ(rig.eap.loads[0].image, (std::vector<uint32_t>{3, 0x4248, 0x4349, 0x40b2}));
    EXPECT_EQ(rig.eap.loads[1].opcode, Opcode::kExecute | Opcode::kRateMiddle | Opcode::kLoadRouter);
    EXPECT_EQ(rig.eap.loads[1].image, (std::vector<uint32_t>{2, 0x4244, 0x4345}));

    // Once per attach: a second start with no rate change loads nothing more.
    ASSERT_EQ(rig.family->Stop(), kIOReturnSuccess);
    ASSERT_EQ(rig.Configure48k(), kIOReturnSuccess);
    EXPECT_EQ(rig.eap.loads.size(), 2U);
}

TEST(DiceRouterReloadTests, RateChangeLoadsAgain) {
    ReloadRig rig(true);
    rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, kClockSelect44kInternal);
    rig.bus.Device().SetAchievedClock(ClockRateIndex::k44100, true);
    ASSERT_TRUE(rig.family->ApplyClockIdle(AudioClockConfig{.sampleRateHz = 48000}));
    EXPECT_EQ(rig.eap.loads.size(), 2U);

    // The start that follows changes nothing and was preceded by a load.
    ASSERT_EQ(rig.Configure48k(), kIOReturnSuccess);
    EXPECT_EQ(rig.eap.loads.size(), 2U);
}

TEST(DiceRouterReloadTests, DefaultPolicyPreservesRoutingAtStartAndRateChange) {
    ReloadRig rig;
    ASSERT_EQ(rig.Configure48k(), kIOReturnSuccess);
    rig.bus.Device().SetGlobalQuad(GlobalOffset::kClockSelect, kClockSelect44kInternal);
    rig.bus.Device().SetAchievedClock(ClockRateIndex::k44100, true);
    ASSERT_EQ(rig.family->Stop(), kIOReturnSuccess);
    ASSERT_TRUE(rig.family->ApplyClockIdle(AudioClockConfig{.sampleRateHz = 48000}));
    EXPECT_TRUE(rig.eap.loads.empty());
}

TEST(DiceRouterReloadTests, DeviceWithoutARouterIsLeftAlone) {
    ReloadRig rig(true);
    rig.eap.hasRouter = false;
    ASSERT_EQ(rig.Configure48k(), kIOReturnSuccess);
    EXPECT_TRUE(rig.eap.loads.empty());
    EXPECT_TRUE(rig.eap.staged.empty());
}

} // namespace
