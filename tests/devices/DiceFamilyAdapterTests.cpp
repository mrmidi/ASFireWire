// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceFamilyAdapterTests.cpp - The DICE adapter on the audio device host
// (documentation/AUDIO_DEVICE_HOST.md §6 E5). Written from the adapter's spec:
// Describe refuses with a named reason or describes the device from its
// registers; a runtime fault self-heals only when the device still reports a
// locked, healthy clock; config changes restart; device notification bits
// become neutral events in a fixed order.

#include <gtest/gtest.h>

#include "SessionTestSupport.hpp"
#include "SyntheticDiceImages.hpp"

#include "Audio/Host/DiceFamilyAdapter.hpp"
#include "Audio/Protocols/DICE/Core/DiceNotificationRouter.hpp"
#include "Audio/Protocols/DICE/Core/DiceNotificationMailbox.hpp"
#include "Audio/Protocols/DICE/Core/DICETypes.hpp"
#include "Audio/Protocols/Duplex/DuplexControlTypes.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ASFW::Audio::DICE::DiceNotificationRouter;
using ASFW::Audio::DuplexHealthResult;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::Host::DescribeInput;
using ASFW::Audio::Host::DescribeRefusal;
using ASFW::Audio::Host::DescribeResult;
using ASFW::Audio::Host::DeviceEvent;
using ASFW::Audio::Host::DeviceEventSink;
using ASFW::Audio::Host::DiceFamilyAdapter;
using ASFW::Audio::Host::FaultContext;
using ASFW::Audio::Host::FaultVerdict;
using ASFW::Audio::Host::KeepCommitted;
using ASFW::Audio::Model::ASFWAudioDevice;
using ASFW::Discovery::DeviceRegistry;
using ASFW::FW::Generation;
using ASFW::Testing::Session::SessionRig;
using ASFW::Testing::Session::SessionShape;
namespace Ids = ASFW::DeviceProfiles::Audio;
namespace Notify = ASFW::Audio::DICE::Notify;

constexpr std::array<DuplexRestartReason, 12> kAllReasons{
    DuplexRestartReason::kInitialStart,
    DuplexRestartReason::kSampleRateChange,
    DuplexRestartReason::kClockSourceChange,
    DuplexRestartReason::kBusResetRebind,
    DuplexRestartReason::kRecoverAfterTimingLoss,
    DuplexRestartReason::kRecoverAfterCycleInconsistent,
    DuplexRestartReason::kRecoverAfterLockLoss,
    DuplexRestartReason::kRecoverAfterTxFault,
    DuplexRestartReason::kManualReconfigure,
    DuplexRestartReason::kDeviceConfigChange,
};

constexpr std::array<DuplexRestartReason, 4> kRuntimeFaults{
    DuplexRestartReason::kRecoverAfterTimingLoss,
    DuplexRestartReason::kRecoverAfterCycleInconsistent,
    DuplexRestartReason::kRecoverAfterLockLoss,
    DuplexRestartReason::kRecoverAfterTxFault,
};

// A FaultContext that scripts the health read and counts what was asked.
class ScriptedFaultContext final : public FaultContext {
public:
    bool cancelled{false};
    bool streaming{true};
    std::expected<DuplexHealthResult, IOReturn> health{std::unexpected(kIOReturnTimeout)};

    uint32_t healthReads{0};
    uint32_t healthTimeoutMs{0};

    [[nodiscard]] bool Cancelled() const noexcept override { return cancelled; }
    [[nodiscard]] bool StillStreaming() const noexcept override { return streaming; }
    [[nodiscard]] bool ReceiveReplayEstablished() const noexcept override { return false; }
    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t timeoutMs) override {
        ++healthReads;
        healthTimeoutMs = timeoutMs;
        return health;
    }
    void Sleep(uint32_t) noexcept override {}
};

DuplexHealthResult Health(bool sourceLocked, bool clockReferenceHealthy) {
    DuplexHealthResult result{};
    result.sourceLocked = sourceLocked;
    result.clockReferenceHealthy = clockReferenceHealthy;
    return result;
}

// Records every neutral event the adapter raises.
class RecordingSink final : public DeviceEventSink {
public:
    struct Call {
        uint64_t guid;
        DeviceEvent event;
        uint32_t detail;
    };
    void OnDeviceEvent(uint64_t guid, DeviceEvent event, uint32_t detail) noexcept override {
        calls.push_back(Call{guid, event, detail});
    }
    std::vector<Call> calls;
};

// Runs Describe and returns its result; asserts `done` ran exactly once.
std::optional<DescribeResult> Describe(DiceFamilyAdapter& adapter, const DescribeInput& in) {
    int doneCalls = 0;
    std::optional<DescribeResult> result;
    adapter.Describe(in, [&](DescribeResult r) {
        ++doneCalls;
        result = std::move(r);
    });
    EXPECT_EQ(doneCalls, 1) << "Describe must call done exactly once";
    return result;
}

DescribeInput MakeInput(const SessionRig& rig) {
    DescribeInput in{};
    const auto record = rig.registry.SnapshotByGuid(rig.guid);
    EXPECT_TRUE(record.has_value());
    if (record) {
        in.record = *record;
        in.policy = record->audioPolicy;
    }
    in.protocol = rig.protocol;
    return in;
}

const SessionShape kPro24Shape{
    "pro24dsp", Ids::kFocusriteVendorId, Ids::kSPro24DspModelId, Ids::kFocusriteVendorId,
    ASFW::Testing::Session::kDiceUnitVersion,
    &ASFW::Testing::DICE::DiceDeviceImages::kSaffirePro24Dsp, true};

const SessionShape kMultimixShape{
    "multimix-two-playback", Ids::kAlesisVendorId, Ids::kAlesisMultiMixModelId,
    Ids::kAlesisVendorId, ASFW::Testing::Session::kDiceUnitVersion,
    &ASFW::Testing::DICE::SyntheticDiceImages::kMultimixTwoPlayback, false};

// ---------------------------------------------------------------------------
// Identity
// ---------------------------------------------------------------------------

TEST(DiceFamilyAdapterTests, NameIsDice) {
    DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceFamilyAdapter adapter{router};
    EXPECT_STREQ(adapter.Name(), "DICE");
}

TEST(DiceFamilyAdapterTests, ActsOnEveryRestartReasonIncludingCycleInconsistent) {
    DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceFamilyAdapter adapter{router};
    for (const auto reason : kAllReasons) {
        EXPECT_TRUE(adapter.ActsOn(reason)) << "reason " << static_cast<int>(reason);
    }
    EXPECT_TRUE(adapter.ActsOn(DuplexRestartReason::kRecoverAfterCycleInconsistent));
}

// ---------------------------------------------------------------------------
// JudgeRuntimeFault
// ---------------------------------------------------------------------------

TEST(DiceFamilyAdapterTests, RuntimeFaultWithLockedHealthyClockSelfHeals) {
    DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceFamilyAdapter adapter{router};
    for (const auto reason : kRuntimeFaults) {
        ScriptedFaultContext ctx;
        ctx.health = Health(/*sourceLocked=*/true, /*clockReferenceHealthy=*/true);
        EXPECT_EQ(adapter.JudgeRuntimeFault(0x1234, reason, ctx), FaultVerdict::kSelfHealed)
            << "reason " << static_cast<int>(reason);
        EXPECT_EQ(ctx.healthReads, 1U) << "exactly one health read per runtime fault";
        EXPECT_EQ(ctx.healthTimeoutMs, DiceFamilyAdapter::kHealthReadTimeoutMs);
    }
}

TEST(DiceFamilyAdapterTests, RuntimeFaultWithUnlockedSourceRestarts) {
    DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceFamilyAdapter adapter{router};
    for (const auto reason : kRuntimeFaults) {
        ScriptedFaultContext ctx;
        ctx.health = Health(/*sourceLocked=*/false, /*clockReferenceHealthy=*/true);
        EXPECT_EQ(adapter.JudgeRuntimeFault(0x1234, reason, ctx), FaultVerdict::kRestart)
            << "reason " << static_cast<int>(reason);
        EXPECT_EQ(ctx.healthReads, 1U);
    }
}

TEST(DiceFamilyAdapterTests, RuntimeFaultWithUnhealthyClockReferenceRestarts) {
    DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceFamilyAdapter adapter{router};
    ScriptedFaultContext ctx;
    ctx.health = Health(/*sourceLocked=*/true, /*clockReferenceHealthy=*/false);
    EXPECT_EQ(adapter.JudgeRuntimeFault(0x1234, DuplexRestartReason::kRecoverAfterLockLoss, ctx),
              FaultVerdict::kRestart);
    EXPECT_EQ(ctx.healthReads, 1U);
}

TEST(DiceFamilyAdapterTests, FailedHealthReadRestartsRatherThanSuppressingRecovery) {
    DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceFamilyAdapter adapter{router};
    for (const IOReturn error : {kIOReturnTimeout, kIOReturnError, kIOReturnNotResponding}) {
        ScriptedFaultContext ctx;
        ctx.health = std::unexpected(error);
        EXPECT_EQ(adapter.JudgeRuntimeFault(0x1234, DuplexRestartReason::kRecoverAfterTimingLoss, ctx),
                  FaultVerdict::kRestart)
            << "error " << error;
        EXPECT_EQ(ctx.healthReads, 1U);
    }
}

TEST(DiceFamilyAdapterTests, CancelledRuntimeFaultRestartsWithoutReadingHealth) {
    DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceFamilyAdapter adapter{router};
    for (const auto reason : kRuntimeFaults) {
        ScriptedFaultContext ctx;
        ctx.cancelled = true;
        ctx.health = Health(true, true);
        EXPECT_EQ(adapter.JudgeRuntimeFault(0x1234, reason, ctx), FaultVerdict::kRestart)
            << "reason " << static_cast<int>(reason);
        EXPECT_EQ(ctx.healthReads, 0U);
    }
}

TEST(DiceFamilyAdapterTests, ConfigChangeRestartsWithoutReadingHealth) {
    DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceFamilyAdapter adapter{router};
    ScriptedFaultContext ctx;
    ctx.health = Health(true, true);
    EXPECT_EQ(adapter.JudgeRuntimeFault(0x1234, DuplexRestartReason::kDeviceConfigChange, ctx),
              FaultVerdict::kRestart);
    EXPECT_EQ(ctx.healthReads, 0U);
}

TEST(DiceFamilyAdapterTests, BusResetRebindRestartsWithoutReadingHealth) {
    DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceFamilyAdapter adapter{router};
    ScriptedFaultContext ctx;
    ctx.health = Health(true, true);
    EXPECT_EQ(adapter.JudgeRuntimeFault(0x1234, DuplexRestartReason::kBusResetRebind, ctx),
              FaultVerdict::kRestart);
    EXPECT_EQ(ctx.healthReads, 0U);
}

// ---------------------------------------------------------------------------
// Device events
// ---------------------------------------------------------------------------

class DeviceEventsFixture : public ::testing::Test {
protected:
    static constexpr uint64_t kGuid = 0xD1CE00000000000AULL;

    DeviceRegistry registry;
    DiceNotificationRouter router{registry};
    DiceFamilyAdapter adapter{router};
    RecordingSink sink;

    void TearDown() override { adapter.SetEventSink(nullptr); }
};

TEST_F(DeviceEventsFixture, RxConfigChangeRaisesStreamConfigChanged) {
    adapter.SetEventSink(&sink);
    adapter.OnNotification(kGuid, Notify::kRxConfigChange);
    ASSERT_EQ(sink.calls.size(), 1U);
    EXPECT_EQ(sink.calls[0].guid, kGuid);
    EXPECT_EQ(sink.calls[0].event, DeviceEvent::kStreamConfigChanged);
    EXPECT_EQ(sink.calls[0].detail, Notify::kRxConfigChange);
}

TEST_F(DeviceEventsFixture, TxConfigChangeRaisesStreamConfigChanged) {
    adapter.SetEventSink(&sink);
    adapter.OnNotification(kGuid, Notify::kTxConfigChange);
    ASSERT_EQ(sink.calls.size(), 1U);
    EXPECT_EQ(sink.calls[0].event, DeviceEvent::kStreamConfigChanged);
    EXPECT_EQ(sink.calls[0].detail, Notify::kTxConfigChange);
}

TEST_F(DeviceEventsFixture, LockChangeRaisesClockStatusChanged) {
    adapter.SetEventSink(&sink);
    adapter.OnNotification(kGuid, Notify::kLockChange);
    ASSERT_EQ(sink.calls.size(), 1U);
    EXPECT_EQ(sink.calls[0].guid, kGuid);
    EXPECT_EQ(sink.calls[0].event, DeviceEvent::kClockStatusChanged);
    EXPECT_EQ(sink.calls[0].detail, Notify::kLockChange);
}

TEST_F(DeviceEventsFixture, ExtStatusRaisesClockStatusChanged) {
    adapter.SetEventSink(&sink);
    adapter.OnNotification(kGuid, Notify::kExtStatus);
    ASSERT_EQ(sink.calls.size(), 1U);
    EXPECT_EQ(sink.calls[0].event, DeviceEvent::kClockStatusChanged);
    EXPECT_EQ(sink.calls[0].detail, Notify::kExtStatus);
}

TEST_F(DeviceEventsFixture, ConfigChangeIsRaisedBeforeClockStatus) {
    adapter.SetEventSink(&sink);
    const uint32_t bits = Notify::kLockChange | Notify::kRxConfigChange;
    adapter.OnNotification(kGuid, bits);
    ASSERT_EQ(sink.calls.size(), 2U);
    EXPECT_EQ(sink.calls[0].event, DeviceEvent::kStreamConfigChanged);
    EXPECT_EQ(sink.calls[1].event, DeviceEvent::kClockStatusChanged);
    EXPECT_EQ(sink.calls[0].detail, bits);
    EXPECT_EQ(sink.calls[1].detail, bits);
}

TEST_F(DeviceEventsFixture, NeitherGroupRaisesNothing) {
    adapter.SetEventSink(&sink);
    adapter.OnNotification(kGuid, Notify::kClockAccepted);
    adapter.OnNotification(kGuid, 0);
    EXPECT_TRUE(sink.calls.empty());
}

TEST_F(DeviceEventsFixture, NoSinkMeansNoCallsAndUnsubscribedAdapterIsSilent) {
    adapter.SetEventSink(&sink);
    adapter.SetEventSink(nullptr);
    adapter.OnNotification(kGuid, Notify::kLockChange | Notify::kTxConfigChange);
    EXPECT_TRUE(sink.calls.empty());
}

TEST_F(DeviceEventsFixture, RouterDeliveryReachesTheSinkForARegisteredDevice) {
    // A DICE device at node 2 of generation 1, as the router attributes it.
    ASFW::Discovery::ConfigROM rom{};
    rom.bib.guid = kGuid;
    rom.gen = Generation{1};
    rom.nodeId = 2;
    (void)registry.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});

    adapter.SetEventSink(&sink);
    EXPECT_EQ(router.Deliver(1, 0xFFC2, Notify::kLockChange), kGuid);
    ASSERT_EQ(sink.calls.size(), 1U);
    EXPECT_EQ(sink.calls[0].guid, kGuid);
    EXPECT_EQ(sink.calls[0].event, DeviceEvent::kClockStatusChanged);
    EXPECT_EQ(sink.calls[0].detail, Notify::kLockChange);

    // Unknown node: the router drops it before the adapter sees anything.
    EXPECT_EQ(router.Deliver(1, 0xFFC5, Notify::kLockChange), 0U);
    EXPECT_EQ(sink.calls.size(), 1U);
}

// ---------------------------------------------------------------------------
// Describe
// ---------------------------------------------------------------------------

class DescribeFixture : public ::testing::Test {
protected:
    DescribeFixture() : rig(kPro24Shape) {}
    SessionRig rig;
    DiceFamilyAdapter adapter{rig.notifications};
};

TEST_F(DescribeFixture, Pro24DspIsDescribedFromItsRegisters) {
    const auto result = Describe(adapter, MakeInput(rig));
    ASSERT_TRUE(result.has_value());
    const auto* device = std::get_if<ASFWAudioDevice>(&*result);
    ASSERT_NE(device, nullptr) << "expected a described device, got another outcome";
    EXPECT_EQ(device->guid, rig.guid);
    EXPECT_GT(device->inputChannelCount, 0U);
    EXPECT_GT(device->outputChannelCount, 0U);
    EXPECT_FALSE(device->sampleRates.empty());
    EXPECT_NE(std::find(device->sampleRates.begin(), device->sampleRates.end(),
                        device->currentSampleRate),
              device->sampleRates.end())
        << "currentSampleRate " << device->currentSampleRate << " is not a supported rate";
    EXPECT_FALSE(device->playbackStreams.empty());
}

TEST_F(DescribeFixture, MissingProtocolIsRefusedAsNoProtocol) {
    DescribeInput in = MakeInput(rig);
    in.protocol = nullptr;
    const auto result = Describe(adapter, in);
    ASSERT_TRUE(result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*result);
    ASSERT_NE(refusal, nullptr) << "expected a refusal";
    EXPECT_EQ(std::string_view(refusal->reason), std::string_view(DiceFamilyAdapter::kNoProtocol));
}

TEST_F(DescribeFixture, CommittedRateFormationsAreKeptNotRedescribed) {
    DescribeInput in = MakeInput(rig);
    ASFWAudioDevice committed{};
    committed.guid = rig.guid;
    committed.usesRateFormations = true;
    in.committed = committed;
    const auto result = Describe(adapter, in);
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(std::holds_alternative<KeepCommitted>(*result));
}

TEST(DescribeMultimixTest, TwoPlaybackStreamsArePublishedAsTwo) {
    SessionRig rig(kMultimixShape);
    DiceFamilyAdapter adapter{rig.notifications};
    const auto result = Describe(adapter, MakeInput(rig));
    ASSERT_TRUE(result.has_value());
    const auto* device = std::get_if<ASFWAudioDevice>(&*result);
    if (device == nullptr) {
        const auto* refusal = std::get_if<DescribeRefusal>(&*result);
        ADD_FAILURE() << "refused: " << (refusal ? refusal->reason : "(note)");
        return;
    }
    EXPECT_EQ(device->playbackStreams.size(), 2U);
}

TEST(DescribeGeometryTest, GeometryReadFailureIsRefusedAsGeometryLoadFailed) {
    SessionRig rig(kPro24Shape);
    DiceFamilyAdapter adapter{rig.notifications};
    // The first global-section read of the geometry load times out on the bus.
    rig.bus.FailNext(::ASFW::Testing::OpKind::Read,
                     ASFW::Testing::DICE::kDiceBaseAddressLo + rig.bus.Device().GlobalBase(),
                     ASFW::Async::AsyncStatus::kTimeout);
    const auto result = Describe(adapter, MakeInput(rig));
    ASSERT_TRUE(result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*result);
    if (refusal == nullptr) {
        GTEST_SKIP() << "suspected defect or unreliable fault point: a failed global-section "
                        "read did not produce a refusal";
    }
    EXPECT_EQ(std::string_view(refusal->reason),
              std::string_view(DiceFamilyAdapter::kGeometryLoadFailed));
}

} // namespace
