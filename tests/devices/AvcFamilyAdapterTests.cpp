// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcFamilyAdapterTests.cpp
// The AV/C adapter on the audio device host (documentation/AUDIO_DEVICE_HOST.md
// §4.2, §6 E4). Plain values in, plain values out: no host, no bus, no driver.
// Describe returns what discovery pushed or refuses; JudgeRuntimeFault is the
// 256 ms settle that AVCAudioBackend::HandleTimingLoss ran, driven here through
// a scripted FaultContext whose Sleep advances a fake clock.

#include <gtest/gtest.h>

#include "Audio/Host/AvcFamilyAdapter.hpp"
#include "Audio/Protocols/Duplex/DuplexControlTypes.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ASFW::Audio::DuplexHealthResult;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::Host::AvcFamilyAdapter;
using ASFW::Audio::Host::DescribeInput;
using ASFW::Audio::Host::DescribeRefusal;
using ASFW::Audio::Host::DescribeResult;
using ASFW::Audio::Host::DeviceEvent;
using ASFW::Audio::Host::DeviceEventSink;
using ASFW::Audio::Host::FaultContext;
using ASFW::Audio::Host::FaultVerdict;
using ASFW::Audio::Model::ASFWAudioDevice;
using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::RomEntry;
using ASFW::FW::Generation;

constexpr uint64_t kPhase88Guid = 0x000aac0300b1d1f7ULL;
constexpr uint64_t kDuetGuid = 0x0003db0a0000d112ULL;
constexpr uint64_t kDiceGuid = 0x00130e0400000001ULL;
constexpr uint32_t kTa1394Specifier = 0x00A02D;
constexpr uint32_t kAvcVersion = 0x010001;

constexpr uint32_t kSettleMs = AvcFamilyAdapter::kTimingLossSettleMs;
constexpr uint32_t kPollMs = AvcFamilyAdapter::kTimingLossPollMs;
constexpr uint32_t kMaxSleeps = kSettleMs / kPollMs;

// Seed `registry` with one device whose identity matches the catalog row for
// the given evidence, and return its record (with the catalog's policy).
DeviceRecord SeedDevice(DeviceRegistry& registry, uint64_t guid, uint32_t vendorId,
                        uint32_t modelId, uint32_t unitSpecId, uint32_t unitSwVersion) {
    ConfigROM rom{};
    rom.gen = Generation{1};
    rom.firstSeen = Generation{1};
    rom.lastValidated = Generation{1};
    rom.nodeId = 2;
    rom.bib.guid = guid;
    rom.bib.maxRec = 8;
    rom.rootDirMinimal = {
        RomEntry{.key = CfgKey::VendorId, .value = vendorId},
        RomEntry{.key = CfgKey::ModelId, .value = modelId},
    };
    ASFW::Discovery::UnitDirectory unit{};
    unit.offsetQuadlets = 5;
    unit.unitSpecId = unitSpecId;
    unit.unitSwVersion = unitSwVersion;
    rom.unitDirectories.push_back(unit);
    ASFW::Discovery::LinkPolicy link{};
    (void)registry.UpsertFromROM(rom, link);
    auto record = registry.SnapshotByGuid(guid);
    EXPECT_TRUE(record.has_value());
    return record.value_or(DeviceRecord{});
}

DeviceRecord SeedPhase88(DeviceRegistry& registry) {
    return SeedDevice(registry, kPhase88Guid, ASFW::DeviceProfiles::Audio::kTerraTecVendorId,
                      ASFW::DeviceProfiles::Audio::kPhase88RackFwModelId, kTa1394Specifier,
                      kAvcVersion);
}

DeviceRecord SeedDuet(DeviceRegistry& registry) {
    return SeedDevice(registry, kDuetGuid, ASFW::DeviceProfiles::Audio::kApogeeVendorId,
                      ASFW::DeviceProfiles::Audio::kApogeeDuetModelId, kTa1394Specifier,
                      kAvcVersion);
}

DeviceRecord SeedDice(DeviceRegistry& registry) {
    return SeedDevice(registry, kDiceGuid, ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
                      ASFW::DeviceProfiles::Audio::kSPro24DspModelId,
                      ASFW::DeviceProfiles::Audio::kFocusriteVendorId, 0x000001);
}

DescribeInput MakeInput(const DeviceRecord& record, std::optional<ASFWAudioDevice> discovered) {
    DescribeInput in{};
    in.record = record;
    in.policy = record.audioPolicy;
    in.discovered = std::move(discovered);
    return in;
}

ASFWAudioDevice MakeDiscovered(uint64_t guid) {
    ASFWAudioDevice config{};
    config.guid = guid;
    config.deviceName = "Discovered Unit";
    config.inputChannelCount = 10;
    config.outputChannelCount = 8;
    config.channelCount = 10;
    config.sampleRates = {44100U, 48000U, 96000U};
    config.currentSampleRate = 48000U;
    return config;
}

struct DescribeOutcome {
    int doneCalls{0};
    std::optional<DescribeResult> result{};
};

DescribeOutcome RunDescribe(AvcFamilyAdapter& adapter, const DescribeInput& in) {
    DescribeOutcome outcome;
    adapter.Describe(in, [&outcome](DescribeResult r) {
        ++outcome.doneCalls;
        outcome.result = std::move(r);
    });
    return outcome;
}

// A FaultContext whose answers follow the number of Sleep calls made so far, so
// each test scripts "the device stops streaming during the third wait" without a
// clock. Sleep only counts; nothing here really waits.
class ScriptedFaultContext final : public FaultContext {
public:
    // Reads true until `streamingEndsAfterSleeps` sleeps have happened.
    std::optional<uint32_t> streamingEndsAfterSleeps{};
    // Reads true once `cancelledAfterSleeps` sleeps have happened.
    std::optional<uint32_t> cancelledAfterSleeps{};
    bool replayEstablished{false};

    uint32_t sleepCalls{0};
    uint32_t sleptMs{0};
    mutable uint32_t replayReads{0};
    mutable uint32_t healthReads{0};

    [[nodiscard]] bool Cancelled() const noexcept override {
        return cancelledAfterSleeps.has_value() && sleepCalls >= *cancelledAfterSleeps;
    }
    [[nodiscard]] bool StillStreaming() const noexcept override {
        return !(streamingEndsAfterSleeps.has_value() && sleepCalls >= *streamingEndsAfterSleeps);
    }
    [[nodiscard]] bool ReceiveReplayEstablished() const noexcept override {
        ++replayReads;
        return replayEstablished;
    }
    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t) override {
        ++healthReads;
        return std::unexpected(kIOReturnTimeout);
    }
    void Sleep(uint32_t milliseconds) noexcept override {
        ++sleepCalls;
        sleptMs += milliseconds;
    }
};

class RecordingSink final : public DeviceEventSink {
public:
    void OnDeviceEvent(uint64_t, DeviceEvent, uint32_t) noexcept override { ++events; }
    int events{0};
};

// ---------------------------------------------------------------------------
// Describe (§4.2: what discovery pushed, or a refusal)
// ---------------------------------------------------------------------------

TEST(AvcFamilyAdapterTests, SettleConstantsKeepTheBackendValues) {
    // AVCAudioBackend: 256 ms settle polled every 32 ms (AppleFWAudio debounce).
    EXPECT_EQ(kSettleMs, 256U);
    EXPECT_EQ(kPollMs, 32U);
    EXPECT_EQ(kMaxSleeps, 8U);
}

TEST(AvcFamilyAdapterTests, DiscoveredDescriptionIsReturnedUnchanged) {
    DeviceRegistry registry;
    const DeviceRecord phase88 = SeedPhase88(registry);
    ASSERT_NE(phase88.audioPolicy, nullptr);
    const ASFWAudioDevice discovered = MakeDiscovered(kPhase88Guid);

    AvcFamilyAdapter adapter;
    const auto outcome = RunDescribe(adapter, MakeInput(phase88, discovered));
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* desc = std::get_if<ASFWAudioDevice>(&*outcome.result);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->guid, discovered.guid);
    EXPECT_EQ(desc->deviceName, discovered.deviceName);
    EXPECT_EQ(desc->inputChannelCount, discovered.inputChannelCount);
    EXPECT_EQ(desc->outputChannelCount, discovered.outputChannelCount);
    EXPECT_EQ(desc->channelCount, discovered.channelCount);
    EXPECT_EQ(desc->sampleRates, discovered.sampleRates);
    EXPECT_EQ(desc->currentSampleRate, discovered.currentSampleRate);
}

TEST(AvcFamilyAdapterTests, DuetAndPhase88BothDescribeFromDiscovery) {
    DeviceRegistry registry;
    const DeviceRecord duet = SeedDuet(registry);
    ASSERT_NE(duet.audioPolicy, nullptr);

    AvcFamilyAdapter adapter;
    const auto outcome = RunDescribe(adapter, MakeInput(duet, MakeDiscovered(kDuetGuid)));
    ASSERT_TRUE(outcome.result.has_value());
    const auto* desc = std::get_if<ASFWAudioDevice>(&*outcome.result);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->guid, kDuetGuid);
}

TEST(AvcFamilyAdapterTests, NoDiscoveredDescriptionIsRefusedAsAwaitingDiscovery) {
    DeviceRegistry registry;
    const DeviceRecord phase88 = SeedPhase88(registry);
    ASSERT_NE(phase88.audioPolicy, nullptr);

    AvcFamilyAdapter adapter;
    const auto outcome = RunDescribe(adapter, MakeInput(phase88, std::nullopt));
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*outcome.result);
    ASSERT_NE(refusal, nullptr);
    EXPECT_EQ(refusal->status, kIOReturnNotReady);
    EXPECT_STREQ(refusal->reason, "awaiting-avc-discovery");
}

TEST(AvcFamilyAdapterTests, NonAvcPolicyIsRefusedEvenWithADiscoveredDescription) {
    DeviceRegistry registry;
    const DeviceRecord dice = SeedDice(registry);
    ASSERT_NE(dice.audioPolicy, nullptr);

    AvcFamilyAdapter adapter;
    const auto outcome = RunDescribe(adapter, MakeInput(dice, MakeDiscovered(kDiceGuid)));
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*outcome.result);
    ASSERT_NE(refusal, nullptr);
    EXPECT_EQ(refusal->status, kIOReturnUnsupported);
    EXPECT_STREQ(refusal->reason, "not-an-avc-policy");
}

TEST(AvcFamilyAdapterTests, NullPolicyIsRefusedAsNotAnAvcPolicy) {
    AvcFamilyAdapter adapter;
    DescribeInput in{};
    in.record.guid = kPhase88Guid;
    in.discovered = MakeDiscovered(kPhase88Guid);
    const auto outcome = RunDescribe(adapter, in);
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*outcome.result);
    ASSERT_NE(refusal, nullptr);
    EXPECT_EQ(refusal->status, kIOReturnUnsupported);
}

// ---------------------------------------------------------------------------
// JudgeRuntimeFault (§4.2: settle 256 ms, then ask whether RX replay is back)
// ---------------------------------------------------------------------------

TEST(AvcFamilyAdapterTests, StalledReceiveRestartsAfterTheFullSettle) {
    AvcFamilyAdapter adapter;
    ScriptedFaultContext context;
    const auto verdict = adapter.JudgeRuntimeFault(
        kPhase88Guid, DuplexRestartReason::kRecoverAfterTimingLoss, context);
    EXPECT_EQ(verdict, FaultVerdict::kRestart);
    EXPECT_EQ(context.sleepCalls, kMaxSleeps);
    EXPECT_EQ(context.sleptMs, kSettleMs);
    EXPECT_EQ(context.replayReads, 1U);
    EXPECT_EQ(context.healthReads, 0U);  // AV/C reads no register evidence
}

TEST(AvcFamilyAdapterTests, ReplayReestablishedDuringSettleIsSelfHealed) {
    AvcFamilyAdapter adapter;
    ScriptedFaultContext context;
    context.replayEstablished = true;
    const auto verdict = adapter.JudgeRuntimeFault(
        kPhase88Guid, DuplexRestartReason::kRecoverAfterTimingLoss, context);
    EXPECT_EQ(verdict, FaultVerdict::kSelfHealed);
    // The backend waited the whole window before asking; it did not shortcut.
    EXPECT_EQ(context.sleepCalls, kMaxSleeps);
    EXPECT_EQ(context.sleptMs, kSettleMs);
}

TEST(AvcFamilyAdapterTests, StoppedStreamingMidSettleLeavesAfterTheSleepThatObservedIt) {
    AvcFamilyAdapter adapter;
    ScriptedFaultContext context;
    context.streamingEndsAfterSleeps = 3;  // the device goes away during the third wait
    context.replayEstablished = true;      // must not turn a departure into a heal
    const auto verdict = adapter.JudgeRuntimeFault(
        kPhase88Guid, DuplexRestartReason::kRecoverAfterTimingLoss, context);
    EXPECT_EQ(verdict, FaultVerdict::kDeviceLeft);
    EXPECT_EQ(context.sleepCalls, 3U);
    EXPECT_EQ(context.replayReads, 0U);
}

TEST(AvcFamilyAdapterTests, NotStreamingAtTheStartLeavesWithoutSleeping) {
    AvcFamilyAdapter adapter;
    ScriptedFaultContext context;
    context.streamingEndsAfterSleeps = 0;
    const auto verdict = adapter.JudgeRuntimeFault(
        kPhase88Guid, DuplexRestartReason::kRecoverAfterTimingLoss, context);
    EXPECT_EQ(verdict, FaultVerdict::kDeviceLeft);
    EXPECT_EQ(context.sleepCalls, 0U);
}

TEST(AvcFamilyAdapterTests, StoppedStreamingDuringTheLastWaitIsStillADeparture) {
    AvcFamilyAdapter adapter;
    ScriptedFaultContext context;
    context.streamingEndsAfterSleeps = kMaxSleeps;  // gone when the last wait ends
    context.replayEstablished = true;
    const auto verdict = adapter.JudgeRuntimeFault(
        kPhase88Guid, DuplexRestartReason::kRecoverAfterTimingLoss, context);
    // The backend re-checked liveness after the loop, before the replay test.
    EXPECT_EQ(verdict, FaultVerdict::kDeviceLeft);
    EXPECT_EQ(context.sleepCalls, kMaxSleeps);
    EXPECT_EQ(context.replayReads, 0U);
}

TEST(AvcFamilyAdapterTests, CancelledAtTheStartReturnsWithoutSleeping) {
    AvcFamilyAdapter adapter;
    ScriptedFaultContext context;
    context.cancelledAfterSleeps = 0;
    const auto verdict = adapter.JudgeRuntimeFault(
        kPhase88Guid, DuplexRestartReason::kRecoverAfterTimingLoss, context);
    EXPECT_EQ(verdict, FaultVerdict::kDeviceLeft);
    EXPECT_EQ(context.sleepCalls, 0U);
    EXPECT_EQ(context.replayReads, 0U);
}

TEST(AvcFamilyAdapterTests, CancelledMidSettleReturnsWithinOnePollTick) {
    AvcFamilyAdapter adapter;
    ScriptedFaultContext context;
    context.cancelledAfterSleeps = 2;
    context.replayEstablished = true;
    const auto verdict = adapter.JudgeRuntimeFault(
        kPhase88Guid, DuplexRestartReason::kRecoverAfterTimingLoss, context);
    EXPECT_EQ(verdict, FaultVerdict::kDeviceLeft);
    EXPECT_EQ(context.sleepCalls, 2U);
    EXPECT_LE(context.sleptMs, kSettleMs);
    EXPECT_EQ(context.replayReads, 0U);
}

TEST(AvcFamilyAdapterTests, NoScriptSleepsMoreThanTheSettleAllows) {
    // Whatever the script does, the judgement never exceeds 8 polls / 256 ms.
    for (uint32_t endsAfter = 0; endsAfter <= kMaxSleeps + 2; ++endsAfter) {
        for (const bool cancel : {false, true}) {
            AvcFamilyAdapter adapter;
            ScriptedFaultContext context;
            context.streamingEndsAfterSleeps = endsAfter;
            if (cancel) context.cancelledAfterSleeps = endsAfter;
            (void)adapter.JudgeRuntimeFault(
                kPhase88Guid, DuplexRestartReason::kRecoverAfterTimingLoss, context);
            EXPECT_LE(context.sleepCalls, kMaxSleeps) << "endsAfter " << endsAfter;
            EXPECT_LE(context.sleptMs, kSettleMs) << "endsAfter " << endsAfter;
        }
    }
}

// ---------------------------------------------------------------------------
// Device events (§4.2: AV/C raises none; the sink is not retained)
// ---------------------------------------------------------------------------

TEST(AvcFamilyAdapterTests, SetEventSinkStoresNothingAndAcceptsNull) {
    AvcFamilyAdapter adapter;
    RecordingSink sink;
    adapter.SetEventSink(&sink);
    adapter.SetEventSink(nullptr);
    EXPECT_EQ(sink.events, 0);
}

TEST(AvcFamilyAdapterTests, NameIsAvc) {
    AvcFamilyAdapter adapter;
    EXPECT_STREQ(adapter.Name(), "AV/C");
}

} // namespace
