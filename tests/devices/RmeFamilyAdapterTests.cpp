// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RmeFamilyAdapterTests.cpp
// The RME Fireface adapter on the audio device host (documentation/AUDIO_DEVICE_HOST.md
// §4.2, §6 E2). Plain values in, plain values out: no host, no bus, no driver.
// Policies come from a real DeviceRegistry seeded with a ConfigROM, so the
// catalog decision is the one the driver would make.

#include <gtest/gtest.h>

#include "Audio/Host/RmeFamilyAdapter.hpp"
#include "Audio/Protocols/IDeviceProtocol.hpp"
#include "Audio/Protocols/Duplex/DuplexControlTypes.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ASFW::Audio::DuplexHealthResult;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::IDeviceProtocol;
using ASFW::Audio::Host::DescribeDone;
using ASFW::Audio::Host::DescribeInput;
using ASFW::Audio::Host::DescribeRefusal;
using ASFW::Audio::Host::DescribeResult;
using ASFW::Audio::Host::DeviceEvent;
using ASFW::Audio::Host::DeviceEventSink;
using ASFW::Audio::Host::FaultContext;
using ASFW::Audio::Host::FaultVerdict;
using ASFW::Audio::Host::RmeFamilyAdapter;
using ASFW::Audio::Model::ASFWAudioDevice;
using ASFW::Audio::Model::StreamMode;
using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::RomEntry;
using ASFW::FW::Generation;
using ASFW::DeviceProfiles::Audio::ProfileBuilderId;

constexpr uint64_t kRmeGuid = 0x000a3501000000a1ULL;
constexpr uint64_t kDiceGuid = 0x00130e0400000001ULL;

// Minimal protocol: the adapter only asks it for its name.
class FakeProtocol final : public IDeviceProtocol {
public:
    explicit FakeProtocol(std::string name) : name_(std::move(name)) {}
    IOReturn Initialize() override { return kIOReturnSuccess; }
    IOReturn Shutdown() override { return kIOReturnSuccess; }
    const char* GetName() const override { return name_.c_str(); }

private:
    std::string name_;
};

// Counts every FaultContext call. The RME verdict reads no health evidence, so
// every counter must stay at zero.
class CountingFaultContext final : public FaultContext {
public:
    [[nodiscard]] bool Cancelled() const noexcept override { ++calls; return false; }
    [[nodiscard]] bool StillStreaming() const noexcept override { ++calls; return true; }
    [[nodiscard]] bool ReceiveReplayEstablished() const noexcept override { ++calls; return false; }
    [[nodiscard]] std::expected<DuplexHealthResult, IOReturn> ReadHealth(uint32_t) override {
        ++calls;
        return std::unexpected(kIOReturnTimeout);
    }
    void Sleep(uint32_t) noexcept override { ++calls; }
    mutable int calls{0};
};

class RecordingSink final : public DeviceEventSink {
public:
    void OnDeviceEvent(uint64_t, DeviceEvent, uint32_t) noexcept override { ++events; }
    int events{0};
};

// Seed `registry` with one device whose identity matches the catalog row for
// the given vendor/model/unit evidence, and return its record.
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

DeviceRecord SeedRme(DeviceRegistry& registry, uint32_t unitSwVersion) {
    return SeedDevice(registry, kRmeGuid, ASFW::DeviceProfiles::Audio::kRmeVendorId,
                      ASFW::DeviceProfiles::Audio::kRmeRootModelId,
                      ASFW::DeviceProfiles::Audio::kRmeUnitSpecifierId, unitSwVersion);
}

DeviceRecord SeedDice(DeviceRegistry& registry) {
    return SeedDevice(registry, kDiceGuid, ASFW::DeviceProfiles::Audio::kFocusriteVendorId,
                      ASFW::DeviceProfiles::Audio::kSPro24DspModelId,
                      ASFW::DeviceProfiles::Audio::kFocusriteVendorId, 0x000001);
}

// Run Describe synchronously and capture the one completion it makes.
struct DescribeOutcome {
    int doneCalls{0};
    std::optional<DescribeResult> result{};
};

DescribeOutcome RunDescribe(RmeFamilyAdapter& adapter, const DescribeInput& in) {
    DescribeOutcome outcome;
    adapter.Describe(in, [&outcome](DescribeResult r) {
        ++outcome.doneCalls;
        outcome.result = std::move(r);
    });
    return outcome;
}

DescribeInput MakeInput(const DeviceRecord& record, std::shared_ptr<IDeviceProtocol> protocol) {
    DescribeInput in{};
    in.record = record;
    in.policy = record.audioPolicy;
    in.protocol = std::move(protocol);
    return in;
}

// ---------------------------------------------------------------------------
// Refusals (§4.2: Describe refuses a non-RME or unprotocolled device)
// ---------------------------------------------------------------------------

TEST(RmeFamilyAdapterTests, NullPolicyIsRefusedAsNotAnRmePolicy) {
    RmeFamilyAdapter adapter;
    DescribeInput in{};
    in.record.guid = kRmeGuid;
    in.protocol = std::make_shared<FakeProtocol>("unused");
    const auto outcome = RunDescribe(adapter, in);
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*outcome.result);
    ASSERT_NE(refusal, nullptr);
    EXPECT_EQ(refusal->status, kIOReturnUnsupported);
    EXPECT_STREQ(refusal->reason, "not-an-rme-policy");
}

TEST(RmeFamilyAdapterTests, NonRmePolicyIsRefusedAsNotAnRmePolicy) {
    DeviceRegistry registry;
    const DeviceRecord dice = SeedDice(registry);
    ASSERT_NE(dice.audioPolicy, nullptr);
    ASSERT_NE(dice.audioPolicy->plan.family, ASFW::DeviceProfiles::Audio::AudioFamilyProviderId::RmeRegister);

    RmeFamilyAdapter adapter;
    const auto outcome = RunDescribe(adapter, MakeInput(dice, std::make_shared<FakeProtocol>("DICE")));
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*outcome.result);
    ASSERT_NE(refusal, nullptr);
    EXPECT_EQ(refusal->status, kIOReturnUnsupported);
    EXPECT_STREQ(refusal->reason, "not-an-rme-policy");
}

TEST(RmeFamilyAdapterTests, RmePolicyWithoutProtocolIsRefusedAsNotReady) {
    DeviceRegistry registry;
    const DeviceRecord rme = SeedRme(registry, ASFW::DeviceProfiles::Audio::kRmeFireface800UnitVersion);
    ASSERT_NE(rme.audioPolicy, nullptr);

    RmeFamilyAdapter adapter;
    const auto outcome = RunDescribe(adapter, MakeInput(rme, nullptr));
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*outcome.result);
    ASSERT_NE(refusal, nullptr);
    EXPECT_EQ(refusal->status, kIOReturnNotReady);
    EXPECT_STREQ(refusal->reason, "no-protocol");
}

// ---------------------------------------------------------------------------
// Description (§4.2: fixed 48 kHz, one blocking stream per direction)
// ---------------------------------------------------------------------------

TEST(RmeFamilyAdapterTests, Fireface800DescriptionHasTwentyEightChannelsAndOneStreamPerDirection) {
    DeviceRegistry registry;
    const DeviceRecord rme = SeedRme(registry, ASFW::DeviceProfiles::Audio::kRmeFireface800UnitVersion);
    ASSERT_NE(rme.audioPolicy, nullptr);
    ASSERT_EQ(rme.audioPolicy->plan.profileBuilder, ProfileBuilderId::RmeFireface800);

    RmeFamilyAdapter adapter;
    const auto outcome = RunDescribe(adapter, MakeInput(rme, std::make_shared<FakeProtocol>("RME Fireface 800")));
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* desc = std::get_if<ASFWAudioDevice>(&*outcome.result);
    ASSERT_NE(desc, nullptr);

    EXPECT_EQ(desc->inputChannelCount, 28U);
    EXPECT_EQ(desc->outputChannelCount, 28U);
    EXPECT_EQ(desc->channelCount, 28U);
    EXPECT_EQ(desc->sampleRates, (std::vector<uint32_t>{48000U}));
    EXPECT_EQ(desc->currentSampleRate, 48000U);
    EXPECT_EQ(desc->streamMode, StreamMode::kBlocking);
    EXPECT_TRUE(desc->resolvedGeometryRequired);

    ASSERT_EQ(desc->captureStreams.size(), 1U);
    ASSERT_EQ(desc->playbackStreams.size(), 1U);
    EXPECT_EQ(desc->captureStreams[0].pcmChannels, 28U);
    EXPECT_EQ(desc->captureStreams[0].am824Slots, 28U);
    EXPECT_EQ(desc->playbackStreams[0].pcmChannels, 28U);
    EXPECT_EQ(desc->playbackStreams[0].am824Slots, 28U);

    EXPECT_EQ(desc->deviceName, "RME Fireface 800");
    EXPECT_EQ(desc->guid, kRmeGuid);
    EXPECT_EQ(desc->vendorId, rme.vendorId);
    EXPECT_EQ(desc->modelId, rme.modelId);
    EXPECT_EQ(desc->profileBuilderId, static_cast<uint32_t>(ProfileBuilderId::RmeFireface800));
}

TEST(RmeFamilyAdapterTests, Fireface400DescriptionHasEighteenChannels) {
    DeviceRegistry registry;
    const DeviceRecord rme = SeedRme(registry, ASFW::DeviceProfiles::Audio::kRmeFireface400UnitVersion);
    ASSERT_NE(rme.audioPolicy, nullptr);
    ASSERT_EQ(rme.audioPolicy->plan.profileBuilder, ProfileBuilderId::RmeFireface400);

    RmeFamilyAdapter adapter;
    const auto outcome = RunDescribe(adapter, MakeInput(rme, std::make_shared<FakeProtocol>("RME Fireface 400")));
    ASSERT_TRUE(outcome.result.has_value());
    const auto* desc = std::get_if<ASFWAudioDevice>(&*outcome.result);
    ASSERT_NE(desc, nullptr);
    EXPECT_EQ(desc->inputChannelCount, 18U);
    EXPECT_EQ(desc->outputChannelCount, 18U);
    EXPECT_EQ(desc->channelCount, 18U);
    ASSERT_EQ(desc->captureStreams.size(), 1U);
    EXPECT_EQ(desc->captureStreams[0].pcmChannels, 18U);
    EXPECT_EQ(desc->playbackStreams[0].am824Slots, 18U);
    EXPECT_EQ(desc->deviceName, "RME Fireface 400");
}

TEST(RmeFamilyAdapterTests, BuildNubConfigWithoutProtocolNameFallsBackToRmeFireface) {
    DeviceRegistry registry;
    const DeviceRecord rme = SeedRme(registry, ASFW::DeviceProfiles::Audio::kRmeFireface800UnitVersion);
    const auto config = RmeFamilyAdapter::BuildNubConfig(
        rme, ProfileBuilderId::RmeFireface800, nullptr);
    EXPECT_EQ(config.deviceName, "RME Fireface");
    EXPECT_EQ(config.channelCount, 28U);
}

// ---------------------------------------------------------------------------
// Runtime faults (§4.2: every fault restarts, no health read)
// ---------------------------------------------------------------------------

TEST(RmeFamilyAdapterTests, JudgeRuntimeFaultAlwaysRestartsAndNeverTouchesContext) {
    RmeFamilyAdapter adapter;
    for (uint8_t i = 0; i <= static_cast<uint8_t>(DuplexRestartReason::kDeviceConfigChange); ++i) {
        CountingFaultContext context;
        const auto verdict = adapter.JudgeRuntimeFault(kRmeGuid, static_cast<DuplexRestartReason>(i), context);
        EXPECT_EQ(verdict, FaultVerdict::kRestart) << "reason " << static_cast<int>(i);
        EXPECT_EQ(context.calls, 0) << "reason " << static_cast<int>(i);
    }
}

// ---------------------------------------------------------------------------
// Device events (§4.2: the Fireface raises none; the sink is not retained)
// ---------------------------------------------------------------------------

TEST(RmeFamilyAdapterTests, SetEventSinkStoresNothingAndAcceptsNull) {
    RmeFamilyAdapter adapter;
    RecordingSink sink;
    EXPECT_NO_THROW(adapter.SetEventSink(&sink));
    EXPECT_NO_THROW(adapter.SetEventSink(nullptr));
    EXPECT_EQ(sink.events, 0);
}

} // namespace
