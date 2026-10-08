// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuFamilyAdapterTests.cpp
// The MOTU adapter on the audio device host (documentation/AUDIO_DEVICE_HOST.md
// §4.2, §6 E3/E3a). The adapter runs against the real MotuV2Protocol over a
// recording bus, so Describe's register read, the channel counts it publishes
// and the wire traffic it causes are all the production ones.
// Policies come from a real DeviceRegistry seeded with a ConfigROM, so the
// catalog decision is the one the driver would make.

#include <gtest/gtest.h>

#include "Audio/Host/MotuFamilyAdapter.hpp"
#include "Audio/Model/NubGeometryRefresh.hpp"
#include "Audio/Protocols/MOTU/MotuV2Protocol.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Audio::DuplexHealthResult;
using ASFW::Audio::DuplexPrepareResult;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::Host::DescribedWithNote;
using ASFW::Audio::Host::DescribeInput;
using ASFW::Audio::Host::DescribeRefusal;
using ASFW::Audio::Host::DescribeResult;
using ASFW::Audio::Host::DeviceEvent;
using ASFW::Audio::Host::DeviceEventSink;
using ASFW::Audio::Host::FaultContext;
using ASFW::Audio::Host::FaultVerdict;
using ASFW::Audio::Host::MotuFamilyAdapter;
using ASFW::Audio::Model::ASFWAudioDevice;
using ASFW::Audio::Model::ClassifyGeometryRefresh;
using ASFW::Audio::Model::GeometryRefreshDecision;
using ASFW::Audio::Motu::MotuV2Protocol;
using ASFW::Audio::Motu::Reg;
using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::RomEntry;
using ASFW::FW::Generation;
namespace Ids = ASFW::DeviceProfiles::Audio;

constexpr uint64_t kMotuGuid = 0x0001f20000000001ULL;
constexpr uint64_t kDiceGuid = 0x00130e0400000001ULL;

constexpr uint32_t kOptNone = 0U;
constexpr uint32_t kOptAdat = 1U;
constexpr uint32_t kOptSpdif = 2U;

constexpr uint32_t OpticalWord(uint32_t inMode, uint32_t outMode) {
    return ((inMode & 0x3U) << 8) | ((outMode & 0x3U) << 10);
}

constexpr uint32_t LowOf(Reg reg) {
    return static_cast<uint32_t>(ASFW::Audio::Motu::kAddrBase) + static_cast<uint32_t>(reg);
}

// Replays programmed quadlets; can hold a read open until the test completes it.
class RecordingBus final : public ASFW::Async::IFireWireBusOps,
                           public ASFW::Async::IFireWireBusInfo {
public:
    std::vector<uint32_t> reads;
    std::vector<uint32_t> writes;
    std::map<uint32_t, uint32_t> readValues;
    AsyncStatus readStatus{AsyncStatus::kSuccess};
    // Hold read completions until CompletePending().
    bool deferReads{false};
    std::vector<std::function<void()>> pending;

    void CompletePending() {
        auto work = std::move(pending);
        pending.clear();
        for (auto& fn : work) fn();
    }

    AsyncHandle ReadBlock(ASFW::FW::Generation, ASFW::FW::NodeId, FWAddress address, uint32_t,
                          ASFW::FW::FwSpeed,
                          ASFW::Async::InterfaceCompletionCallback callback) override {
        reads.push_back(address.addressLo);
        const auto find = readValues.find(address.addressLo);
        auto deliver = [this, callback, found = find != readValues.end(),
                        value = find != readValues.end() ? find->second : 0U]() mutable {
            if (readStatus != AsyncStatus::kSuccess || !found) {
                callback(readStatus, {});
                return;
            }
            const uint8_t payload[4] = {static_cast<uint8_t>(value >> 24),
                                        static_cast<uint8_t>(value >> 16),
                                        static_cast<uint8_t>(value >> 8),
                                        static_cast<uint8_t>(value)};
            callback(AsyncStatus::kSuccess, std::span<const uint8_t>(payload, 4));
        };
        if (deferReads) {
            pending.push_back(std::move(deliver));
        } else {
            deliver();
        }
        return AsyncHandle{1};
    }

    AsyncHandle WriteBlock(ASFW::FW::Generation, ASFW::FW::NodeId, FWAddress address,
                           std::span<const uint8_t>, ASFW::FW::FwSpeed,
                           ASFW::Async::InterfaceCompletionCallback callback) override {
        writes.push_back(address.addressLo);
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{1};
    }

    AsyncHandle Lock(ASFW::FW::Generation, ASFW::FW::NodeId, FWAddress, ASFW::FW::LockOp,
                     std::span<const uint8_t>, uint32_t, ASFW::FW::FwSpeed,
                     ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{1};
    }

    bool Cancel(AsyncHandle) override { return false; }
    ASFW::FW::FwSpeed GetSpeed(ASFW::FW::NodeId) const override { return ASFW::FW::FwSpeed::S400; }
    uint32_t HopCount(ASFW::FW::NodeId, ASFW::FW::NodeId) const override { return 1; }
    ASFW::FW::Generation GetGeneration() const override { return ASFW::FW::Generation{2}; }
    ASFW::FW::NodeId GetLocalNodeID() const override { return ASFW::FW::NodeId{0}; }
};

DeviceRecord SeedDevice(DeviceRegistry& registry, uint64_t guid, uint32_t vendorId,
                        uint32_t modelId, uint32_t unitSpecId, uint32_t unitSwVersion) {
    ConfigROM rom{};
    rom.gen = Generation{1};
    rom.firstSeen = Generation{1};
    rom.lastValidated = Generation{1};
    rom.nodeId = 1;
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

DeviceRecord SeedMotu(DeviceRegistry& registry) {
    // The root directory publishes model_id 0; the model lives in Unit_Sw_Version.
    return SeedDevice(registry, kMotuGuid, Ids::kMotuVendorId, 0U, Ids::kMotuVendorId,
                      Ids::kMotu828mk2SwVersion);
}

DeviceRecord SeedDice(DeviceRegistry& registry) {
    return SeedDevice(registry, kDiceGuid, Ids::kFocusriteVendorId, Ids::kSPro24DspModelId,
                      Ids::kFocusriteVendorId, 0x000001);
}

// The real MOTU protocol over a recording bus, on a route the registry issued.
struct MotuRig {
    MotuRig() {
        record = SeedMotu(registry);
        const auto route = registry.CurrentRoute(kMotuGuid);
        EXPECT_TRUE(route.has_value());
        protocol = std::make_shared<MotuV2Protocol>(bus, bus, registry, *route,
                                                    Ids::kMotu828mk2SwVersion);
    }

    DescribeInput Input() const {
        DescribeInput in{};
        in.record = record;
        in.policy = record.audioPolicy;
        in.protocol = protocol;
        return in;
    }

    RecordingBus bus;
    DeviceRegistry registry;
    DeviceRecord record;
    std::shared_ptr<MotuV2Protocol> protocol;
};

struct DescribeOutcome {
    int doneCalls{0};
    std::optional<DescribeResult> result{};
};

DescribeOutcome RunDescribe(MotuFamilyAdapter& adapter, const DescribeInput& in) {
    DescribeOutcome outcome;
    adapter.Describe(in, [&outcome](DescribeResult r) {
        ++outcome.doneCalls;
        outcome.result = std::move(r);
    });
    return outcome;
}

const ASFWAudioDevice* AsDescription(const DescribeOutcome& outcome) {
    return outcome.result ? std::get_if<ASFWAudioDevice>(&*outcome.result) : nullptr;
}

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

// ---------------------------------------------------------------------------
// Refusals
// ---------------------------------------------------------------------------

TEST(MotuFamilyAdapterTests, NullPolicyIsRefusedAsNotAMotuPolicy) {
    MotuRig rig;
    MotuFamilyAdapter adapter;
    DescribeInput in = rig.Input();
    in.policy = nullptr;
    const auto outcome = RunDescribe(adapter, in);
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*outcome.result);
    ASSERT_NE(refusal, nullptr);
    EXPECT_EQ(refusal->status, kIOReturnUnsupported);
    EXPECT_STREQ(refusal->reason, "not-a-motu-policy");
    EXPECT_TRUE(rig.bus.reads.empty());
}

TEST(MotuFamilyAdapterTests, NonMotuPolicyIsRefusedAsNotAMotuPolicy) {
    MotuRig rig;
    const DeviceRecord dice = SeedDice(rig.registry);
    ASSERT_NE(dice.audioPolicy, nullptr);

    MotuFamilyAdapter adapter;
    DescribeInput in = rig.Input();
    in.record = dice;
    in.policy = dice.audioPolicy;
    const auto outcome = RunDescribe(adapter, in);
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*outcome.result);
    ASSERT_NE(refusal, nullptr);
    EXPECT_EQ(refusal->status, kIOReturnUnsupported);
    EXPECT_STREQ(refusal->reason, "not-a-motu-policy");
    EXPECT_TRUE(rig.bus.reads.empty());
}

TEST(MotuFamilyAdapterTests, MotuPolicyWithoutProtocolIsRefusedAsNotReady) {
    MotuRig rig;
    ASSERT_NE(rig.record.audioPolicy, nullptr);
    MotuFamilyAdapter adapter;
    DescribeInput in = rig.Input();
    in.protocol = nullptr;
    const auto outcome = RunDescribe(adapter, in);
    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*outcome.result);
    ASSERT_NE(refusal, nullptr);
    EXPECT_EQ(refusal->status, kIOReturnNotReady);
    EXPECT_STREQ(refusal->reason, "no-protocol");
}

// ---------------------------------------------------------------------------
// Describe: counts come from the optical register (E3a)
// ---------------------------------------------------------------------------

struct OpticalCase {
    uint32_t inMode;
    uint32_t outMode;
    uint32_t inputChannels;
    uint32_t outputChannels;
};

// Every combination (off / ADAT / S-PDIF per direction). Published rates are
// 44.1/48 kHz = 1x, where an ADAT port adds 8 chunks to its own direction only.
constexpr OpticalCase kOpticalCases[] = {
    {kOptNone, kOptNone, 14, 14},   {kOptNone, kOptAdat, 14, 22},
    {kOptNone, kOptSpdif, 14, 14},  {kOptAdat, kOptNone, 22, 14},
    {kOptAdat, kOptAdat, 22, 22},   {kOptAdat, kOptSpdif, 22, 14},
    {kOptSpdif, kOptNone, 14, 14},  {kOptSpdif, kOptAdat, 14, 22},
    {kOptSpdif, kOptSpdif, 14, 14},
};

TEST(MotuFamilyAdapterTests, DescribesEveryOpticalCombinationFromTheRegister) {
    for (const OpticalCase& c : kOpticalCases) {
        SCOPED_TRACE(testing::Message() << "in=" << c.inMode << " out=" << c.outMode);
        MotuRig rig;
        rig.bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(c.inMode, c.outMode);
        MotuFamilyAdapter adapter;

        const auto outcome = RunDescribe(adapter, rig.Input());

        EXPECT_EQ(outcome.doneCalls, 1);
        const ASFWAudioDevice* dev = AsDescription(outcome);
        ASSERT_NE(dev, nullptr) << "a clean read is a plain description, with no note";
        EXPECT_EQ(dev->inputChannelCount, c.inputChannels);
        EXPECT_EQ(dev->outputChannelCount, c.outputChannels);
        EXPECT_EQ(dev->channelCount, std::max(c.inputChannels, c.outputChannels));
    }
}

TEST(MotuFamilyAdapterTests, DescribeCarriesIdentityNamesAndPublishedRates) {
    MotuRig rig;
    rig.bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptSpdif, kOptSpdif);
    MotuFamilyAdapter adapter;

    const auto outcome = RunDescribe(adapter, rig.Input());

    const ASFWAudioDevice* dev = AsDescription(outcome);
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->guid, kMotuGuid);
    EXPECT_EQ(dev->vendorId, Ids::kMotuVendorId);
    EXPECT_EQ(dev->deviceName, "MOTU 828mkII");
    EXPECT_EQ(dev->inputPlugName, "Input");
    EXPECT_EQ(dev->outputPlugName, "Output");
    EXPECT_EQ(dev->sampleRates, (std::vector<uint32_t>{44100U, 48000U}));
    EXPECT_EQ(dev->currentSampleRate, 48000U);
    // The port names follow the model table, in host channel order.
    EXPECT_FALSE(dev->inputChannelNames.empty());
    EXPECT_FALSE(dev->outputChannelNames.empty());
}

TEST(MotuFamilyAdapterTests, DescribeIsOneQuadletReadAndNoWrite) {
    MotuRig rig;
    rig.bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptAdat, kOptAdat);
    MotuFamilyAdapter adapter;

    (void)RunDescribe(adapter, rig.Input());

    // The one extra transaction publication gained (AUDIO_DEVICE_HOST.md section 5, Δ7).
    ASSERT_EQ(rig.bus.reads.size(), 1U);
    EXPECT_EQ(rig.bus.reads[0], LowOf(Reg::InOutConfV2));
    EXPECT_TRUE(rig.bus.writes.empty());
}

TEST(MotuFamilyAdapterTests, DescribeCompletesOnlyAfterTheRegisterReadDoes) {
    MotuRig rig;
    rig.bus.deferReads = true;
    rig.bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptAdat, kOptNone);
    MotuFamilyAdapter adapter;

    DescribeOutcome outcome;
    adapter.Describe(rig.Input(), [&outcome](DescribeResult r) {
        ++outcome.doneCalls;
        outcome.result = std::move(r);
    });
    EXPECT_EQ(outcome.doneCalls, 0) << "Describe must not complete before the read does";

    rig.bus.CompletePending();

    EXPECT_EQ(outcome.doneCalls, 1);
    const ASFWAudioDevice* dev = AsDescription(outcome);
    ASSERT_NE(dev, nullptr);
    EXPECT_EQ(dev->inputChannelCount, 22U);
}

TEST(MotuFamilyAdapterTests, ADescribeAfterAnOpticalModeChangeFollowsTheRegister) {
    MotuRig rig;
    rig.bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptSpdif, kOptSpdif);
    MotuFamilyAdapter adapter;
    const auto before = RunDescribe(adapter, rig.Input());
    ASSERT_NE(AsDescription(before), nullptr);
    EXPECT_EQ(AsDescription(before)->inputChannelCount, 14U);

    rig.bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptAdat, kOptSpdif);
    const auto after = RunDescribe(adapter, rig.Input());

    ASSERT_NE(AsDescription(after), nullptr);
    EXPECT_EQ(AsDescription(after)->inputChannelCount, 22U);
    EXPECT_EQ(rig.bus.reads.size(), 2U);
}

// What the nub was published with must equal what the first start prepares,
// or the host's refresh (Δ2) would latch "geometry changed" on the first
// restart of an ADAT-mode device. That is why E3a precedes E3.
TEST(MotuFamilyAdapterTests, PublishedGeometryMatchesWhatPrepareDuplexResolves) {
    for (const OpticalCase& c : kOpticalCases) {
        SCOPED_TRACE(testing::Message() << "in=" << c.inMode << " out=" << c.outMode);
        MotuRig rig;
        rig.bus.readValues[LowOf(Reg::ClockStatusV2)] = 0x00000008U;  // 48 kHz, internal
        rig.bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(c.inMode, c.outMode);
        rig.bus.readValues[LowOf(Reg::PacketFormat)] = 0U;
        MotuFamilyAdapter adapter;
        const auto published = RunDescribe(adapter, rig.Input());
        ASSERT_NE(AsDescription(published), nullptr);

        std::optional<DuplexPrepareResult> prepared;
        ASFW::Audio::AudioDuplexChannels channels{};
        channels.hostToDeviceIsoChannel = 5;
        channels.deviceToHostIsoChannel = 9;
        rig.protocol->PrepareDuplex(channels, ASFW::Audio::AudioClockConfig{.sampleRateHz = 48000U},
                                    [&](IOReturn, DuplexPrepareResult r) { prepared = r; });
        ASSERT_TRUE(prepared.has_value());
        EXPECT_EQ(prepared->runtimeCaps.hostInputPcmChannels,
                  AsDescription(published)->inputChannelCount);
        EXPECT_EQ(prepared->runtimeCaps.hostOutputPcmChannels,
                  AsDescription(published)->outputChannelCount);

        // And the host's later refresh describes the same endpoint.
        const auto refreshed = RunDescribe(adapter, rig.Input());
        ASSERT_NE(AsDescription(refreshed), nullptr);
        EXPECT_EQ(ClassifyGeometryRefresh(*AsDescription(published), *AsDescription(refreshed)),
                  GeometryRefreshDecision::kMayRefresh);
    }
}

// ---------------------------------------------------------------------------
// A failed register read
// ---------------------------------------------------------------------------

TEST(MotuFamilyAdapterTests, FailedReadBeforeAnyNubPublishesTheFixedGeometryWithANote) {
    MotuRig rig;
    rig.bus.readStatus = AsyncStatus::kTimeout;
    MotuFamilyAdapter adapter;

    const auto outcome = RunDescribe(adapter, rig.Input());

    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* described = std::get_if<DescribedWithNote>(&*outcome.result);
    ASSERT_NE(described, nullptr) << "the device must still appear";
    EXPECT_STREQ(described->note, MotuFamilyAdapter::kFixedGeometryNote);
    EXPECT_EQ(described->device.inputChannelCount, 14U);
    EXPECT_EQ(described->device.outputChannelCount, 14U);
    EXPECT_EQ(described->device.channelCount, 14U);
    EXPECT_EQ(described->device.sampleRates, (std::vector<uint32_t>{44100U, 48000U}));
}

TEST(MotuFamilyAdapterTests, FailedReadAgainstALiveNubIsARefusalNotAFixedFallback) {
    MotuRig rig;
    rig.bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(kOptAdat, kOptAdat);
    MotuFamilyAdapter adapter;
    const auto first = RunDescribe(adapter, rig.Input());
    ASSERT_NE(AsDescription(first), nullptr);

    // The nub is live with 22 x 22; one transaction is lost.
    DescribeInput in = rig.Input();
    in.committed = *AsDescription(first);
    rig.bus.readStatus = AsyncStatus::kTimeout;
    const auto outcome = RunDescribe(adapter, in);

    EXPECT_EQ(outcome.doneCalls, 1);
    ASSERT_TRUE(outcome.result.has_value());
    const auto* refusal = std::get_if<DescribeRefusal>(&*outcome.result);
    ASSERT_NE(refusal, nullptr) << "a 14 x 14 fallback would latch a geometry change";
    EXPECT_EQ(refusal->status, kIOReturnTimeout);
    EXPECT_STREQ(refusal->reason, MotuFamilyAdapter::kReadFailedReason);
}

TEST(MotuFamilyAdapterTests, ReservedOpticalEncodingPublishesTheFixedCounts) {
    MotuRig rig;
    rig.bus.readValues[LowOf(Reg::InOutConfV2)] = OpticalWord(3U, 3U);
    MotuFamilyAdapter adapter;

    const auto outcome = RunDescribe(adapter, rig.Input());

    const ASFWAudioDevice* dev = AsDescription(outcome);
    ASSERT_NE(dev, nullptr) << "the read succeeded; it is not a fallback";
    EXPECT_EQ(dev->inputChannelCount, 14U);
    EXPECT_EQ(dev->outputChannelCount, 14U);
}

// ---------------------------------------------------------------------------
// JudgeRuntimeFault and SetEventSink
// ---------------------------------------------------------------------------

TEST(MotuFamilyAdapterTests, EveryRuntimeFaultRestartsWithoutAskingAnything) {
    MotuFamilyAdapter adapter;
    const DuplexRestartReason reasons[] = {
        DuplexRestartReason::kRecoverAfterTimingLoss,
        DuplexRestartReason::kRecoverAfterCycleInconsistent,
        DuplexRestartReason::kRecoverAfterLockLoss,
        DuplexRestartReason::kRecoverAfterTxFault,
        DuplexRestartReason::kDeviceConfigChange,
        DuplexRestartReason::kBusResetRebind,
    };
    for (const auto reason : reasons) {
        CountingFaultContext context;
        EXPECT_EQ(adapter.JudgeRuntimeFault(kMotuGuid, reason, context), FaultVerdict::kRestart);
        EXPECT_EQ(context.calls, 0) << "MOTU reads no health evidence before restarting";
    }
}

TEST(MotuFamilyAdapterTests, InstallingAnEventSinkRaisesNothing) {
    MotuFamilyAdapter adapter;
    RecordingSink sink;
    adapter.SetEventSink(&sink);
    adapter.SetEventSink(nullptr);
    EXPECT_EQ(sink.events, 0);
    EXPECT_STREQ(adapter.Name(), "MOTU");
}

}  // namespace
