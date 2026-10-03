// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuAudioBackendTests.cpp - The MOTU backend's protocol-v3 half.
//
// Notifications. The 828 Mk3 writes its status word to the DICE notification address. Its
// steady-state buffer-fault word 0x000000C2 has 0x02 set, which DICE reads as
// TX_CFG_CHG: delivered to the DICE backend it would restart the Mk3's session
// for a configuration change it never made, and the buffer fault would go
// unanswered. These pin both halves: the router's words go to the backend the
// catalog chooses for the device, and the MOTU backend restarts on a buffer
// fault with its own reason.
//
// The nub. Before the device's registers are read the backend publishes a
// fallback geometry, and for the Mk3 that has to be the Mk3's: its own profile,
// named by builder id so the audio driver resolves the same profile instead of
// the generic DICE one.

#include <gtest/gtest.h>

#include "SessionTestSupport.hpp"

#include "Audio/Core/DeviceNotificationDispatch.hpp"
#include "Audio/Protocols/Backends/MotuAudioBackend.hpp"
#include "Audio/Protocols/DICE/Core/DiceNotificationRouter.hpp"
#include "Audio/Protocols/MOTU/MotuStatusWord.hpp"
#include "Audio/DriverKit/Config/AudioProfileRegistry.hpp"
#include "Audio/Core/AudioEndpointRuntime.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace {

using namespace ASFW::Testing::Session;
using ASFW::Audio::AudioBackendKind;
using ASFW::Audio::CurrentAudioBackendKind;
using ASFW::Audio::DeviceNotificationDispatch;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::IAudioBackend;
using ASFW::Audio::DICE::DiceNotificationRouter;
namespace Ids = ASFW::DeviceProfiles::Audio;

constexpr uint32_t kMk3BufferFaultWord = 0x000000C2;
// The Mk3 unit directory carries MOTU's OUI as its specifier.
const SessionShape kMk3{"motu-828mk3", Ids::kMotuVendorId, 0, Ids::kMotuVendorId,
                        Ids::kMotu828mk3SwVersion};
const SessionShape kMk2{"motu-828mk2", Ids::kMotuVendorId, 0, Ids::kMotuVendorId,
                        Ids::kMotu828mk2SwVersion};
const SessionShape kDice{"pro24dsp", Ids::kFocusriteVendorId, Ids::kSPro24DspModelId,
                         Ids::kFocusriteVendorId, kDiceUnitVersion};

struct RecordingBackend final : IAudioBackend {
    [[nodiscard]] const char* Name() const noexcept override { return "recording"; }
    [[nodiscard]] IOReturn StartStreaming(uint64_t) noexcept override { return kIOReturnSuccess; }
    [[nodiscard]] IOReturn StopStreaming(uint64_t) noexcept override { return kIOReturnSuccess; }
    void CancelRemoteDeviceWork(uint64_t) noexcept override {}
    void BeginTeardown() noexcept override {}
    void HandleDeviceNotification(uint64_t guid, uint32_t bits) noexcept override {
        seen.emplace_back(guid, bits);
    }
    std::vector<std::pair<uint64_t, uint32_t>> seen;
};

// Two devices on one bus, routed the way AudioCoordinator routes them.
struct TwoFamilyBus {
    static constexpr uint8_t kMk3Node = 2;
    static constexpr uint8_t kDiceNode = 3;
    // A write's source node ID: local bus (0x3FF) in bits 15:6.
    static constexpr uint16_t Source(uint8_t node) { return static_cast<uint16_t>(0xFFC0U | node); }

    TwoFamilyBus() {
        Install(kMk3, kMk3Node);
        Install(kDice, kDiceNode);
    }

    void Install(const SessionShape& shape, uint8_t node) {
        auto rom = MakeSessionRom(shape, ::ASFW::Discovery::Generation{1});
        rom.nodeId = node;
        (void)registry.UpsertFromROM(rom, link);
    }

    IAudioBackend* Resolve(uint64_t guid) {
        const auto kind = CurrentAudioBackendKind(registry, guid);
        if (!kind.has_value()) {
            return nullptr;
        }
        switch (*kind) {
            case AudioBackendKind::MotuRegister: return &motu;
            case AudioBackendKind::Dice: return &dice;
            case AudioBackendKind::Avc:
            case AudioBackendKind::RmeRegister: return nullptr;
        }
        return nullptr;
    }

    ::ASFW::Discovery::LinkPolicy link{.localToNode = FwSpeed::S400,
                                       .isochToNode = FwSpeed::S400};
    ::ASFW::Discovery::DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    RecordingBackend motu;
    RecordingBackend dice;
    DeviceNotificationDispatch dispatch{router, [this](uint64_t g) { return Resolve(g); }};
};

TEST(DeviceNotificationDispatchTests, TheMk3BufferFaultWordReachesTheMotuBackendOnly) {
    TwoFamilyBus bus;
    const uint64_t mk3 = SessionGuid(kMk3);
    ASSERT_EQ(CurrentAudioBackendKind(bus.registry, mk3), AudioBackendKind::MotuRegister);

    EXPECT_EQ(bus.router.Deliver(1, TwoFamilyBus::Source(TwoFamilyBus::kMk3Node),
                                 kMk3BufferFaultWord),
              mk3);

    ASSERT_EQ(bus.motu.seen.size(), 1U);
    EXPECT_EQ(bus.motu.seen[0], std::make_pair(mk3, kMk3BufferFaultWord));
    EXPECT_TRUE(bus.dice.seen.empty());
}

TEST(DeviceNotificationDispatchTests, ADiceNotificationStillReachesTheDiceBackend) {
    TwoFamilyBus bus;
    const uint64_t dice = SessionGuid(kDice);
    const uint32_t configChange =
        ASFW::Audio::DICE::Notify::kRxConfigChange | ASFW::Audio::DICE::Notify::kTxConfigChange;

    (void)bus.router.Deliver(1, TwoFamilyBus::Source(TwoFamilyBus::kDiceNode), configChange);

    ASSERT_EQ(bus.dice.seen.size(), 1U);
    EXPECT_EQ(bus.dice.seen[0], std::make_pair(dice, configChange));
    EXPECT_TRUE(bus.motu.seen.empty());
}

TEST(DeviceNotificationDispatchTests, NothingIsDispatchedAfterClose) {
    TwoFamilyBus bus;
    bus.dispatch.Close();

    (void)bus.router.Deliver(1, TwoFamilyBus::Source(TwoFamilyBus::kMk3Node), kMk3BufferFaultWord);
    (void)bus.router.Deliver(1, TwoFamilyBus::Source(TwoFamilyBus::kDiceNode), 0x30U);

    EXPECT_TRUE(bus.motu.seen.empty());
    EXPECT_TRUE(bus.dice.seen.empty());
}

// A scripted MOTU device streaming through the real session layer, with the
// real MOTU backend. The host-restart router stands in for CoreAudio, so the
// restart's reason is observable without running it.
struct MotuStatusRig {
    explicit MotuStatusRig(const SessionShape& shape) : rig(shape) {
        rig.sessions.SetHostRestartRouter([this](uint64_t, DuplexRestartReason reason) {
            restarts.push_back(reason);
            return true;
        });
    }

    void Notify(uint32_t word) {
        motu.HandleDeviceNotification(rig.guid, word);
        Drain();
    }
    void Drain() {
        if (auto* queue = motu.WorkQueueForTesting()) {
            queue->DispatchSync([] {});
        }
    }

    SessionRig rig;
    ASFW::Audio::MotuAudioBackend motu{rig.publisher, rig.registry, rig.runtime, rig.sessions,
                                       rig.hardware};
    std::vector<DuplexRestartReason> restarts;
};

TEST(MotuStatusWordRestartTests, ABufferFaultRestartsTheRunningMk3) {
    MotuStatusRig r{kMk3};
    ASSERT_EQ(r.rig.Start(), kIOReturnSuccess);
    ASSERT_TRUE(r.rig.IsStreaming());

    r.Notify(kMk3BufferFaultWord);

    ASSERT_EQ(r.restarts.size(), 1U);
    EXPECT_EQ(r.restarts[0], DuplexRestartReason::kRecoverAfterDeviceBufferFault);
}

TEST(MotuStatusWordRestartTests, EitherFaultBitAloneRestarts) {
    for (const uint32_t bit : {ASFW::Audio::MOTU::MotuStatus::kBufferUnderflow,
                               ASFW::Audio::MOTU::MotuStatus::kBufferOverflow}) {
        MotuStatusRig r{kMk3};
        ASSERT_EQ(r.rig.Start(), kIOReturnSuccess);
        r.Notify(bit);
        EXPECT_EQ(r.restarts.size(), 1U) << "bit=0x" << std::hex << bit;
    }
}

TEST(MotuStatusWordRestartTests, CleanAndDiscardedWordsDoNotRestart) {
    MotuStatusRig r{kMk3};
    ASSERT_EQ(r.rig.Start(), kIOReturnSuccess);

    r.Notify(0x01000002);  // clock locked, pedal: steady state
    r.Notify(kMk3BufferFaultWord | ASFW::Audio::MOTU::MotuStatus::kIgnoreWord);

    EXPECT_TRUE(r.restarts.empty());
}

TEST(MotuStatusWordRestartTests, AnIdleMk3IsNotRestarted) {
    MotuStatusRig r{kMk3};

    r.Notify(kMk3BufferFaultWord);

    EXPECT_TRUE(r.restarts.empty());
    EXPECT_FALSE(r.rig.IsStreaming());
}

TEST(MotuStatusWordRestartTests, AProtocolV2DeviceIsNotReadWithTheMk3BitMap) {
    MotuStatusRig r{kMk2};
    ASSERT_EQ(r.rig.Start(), kIOReturnSuccess);

    r.Notify(kMk3BufferFaultWord);

    EXPECT_TRUE(r.restarts.empty());
}

// A protocol that has not read the device yet: no live caps, no labels.
struct NotYetRead final : ASFW::Audio::IDeviceProtocol {
    IOReturn Initialize() override { return kIOReturnSuccess; }
    IOReturn Shutdown() override { return kIOReturnSuccess; }
    const char* GetName() const override { return "not yet read"; }
};

ASFW::Audio::Model::ASFWAudioDevice PublishBeforeFirstStart(const SessionShape& shape) {
    SessionRig rig{shape};
    rig.runtime.Insert(rig.guid, std::make_shared<NotYetRead>());
    ASFW::Audio::MotuAudioBackend motu{rig.publisher, rig.registry, rig.runtime, rig.sessions,
                                       rig.hardware};
    motu.OnDeviceRecordUpdated(rig.guid);
    ASFW::Audio::Model::ASFWAudioDevice published{};
    auto endpoint = rig.runtime.FindEndpointRuntime(rig.guid);
    EXPECT_TRUE(endpoint && endpoint->CopyConfig(published)) << shape.key;
    return published;
}

TEST(MotuNubTests, TheMk3PublishesItsOwnProfileBeforeTheRegistersAreRead) {
    const auto dev = PublishBeforeFirstStart(kMk3);
    const auto* profile = ASFW::Isoch::Audio::AudioProfileRegistry::ProfileForBuilderId(
        static_cast<uint32_t>(Ids::ProfileBuilderId::Motu828mk3));
    ASSERT_NE(profile, nullptr);

    EXPECT_EQ(dev.profileBuilderId, static_cast<uint32_t>(Ids::ProfileBuilderId::Motu828mk3));
    EXPECT_EQ(dev.inputChannelCount, profile->RxChannelCount());
    EXPECT_EQ(dev.outputChannelCount, profile->TxChannelCount());
    EXPECT_EQ(dev.sampleRates, std::vector<uint32_t>{48000U});
    EXPECT_EQ(dev.currentSampleRate, 48000U);
    // Not the v2 fixed-chunk fallback.
    EXPECT_NE(std::make_pair(dev.inputChannelCount, dev.outputChannelCount),
              std::make_pair(14U, 14U));
}

TEST(MotuNubTests, ProtocolV2KeepsTheFixedChunkFallbackAndNoBuilder) {
    const auto dev = PublishBeforeFirstStart(kMk2);

    EXPECT_EQ(dev.profileBuilderId, 0U);
    EXPECT_EQ(dev.inputChannelCount, 14U);
    EXPECT_EQ(dev.outputChannelCount, 14U);
    EXPECT_EQ(dev.sampleRates, (std::vector<uint32_t>{44100U, 48000U}));
}

} // namespace
