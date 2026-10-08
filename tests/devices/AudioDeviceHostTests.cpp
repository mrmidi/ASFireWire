// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioDeviceHostTests.cpp
// Behaviour of the audio device host (documentation/AUDIO_DEVICE_HOST.md §4,
// §6.1). Assertions are on the host's per-outcome counters: every decision the
// host makes is Record()ed exactly once.
//
// These tests were written from the design doc (§4.2, §4.3, §4.4, §6.1) and the
// public headers (FamilyAdapter.hpp, AudioDeviceHost.hpp), not from the
// implementation.

#include <gtest/gtest.h>
#include <net.asfw.driver/ASFWAudioNub.h>

#include "Async/Interfaces/IFireWireBus.hpp"
#include "Audio/Core/AudioEndpointRuntime.hpp"
#include "Audio/Core/AudioNubPublisher.hpp"
#include "Audio/Core/AudioRuntimeRegistry.hpp"
#include "Audio/Host/AudioDeviceHost.hpp"
#include "Audio/Model/ASFWAudioDevice.hpp"
#include "Audio/Protocols/Backends/IsochDuplexHostTransport.hpp"
#include "Audio/Session/AudioSessions.hpp"
#include "Bus/IRM/IRMClient.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Testing/HostDriverKitStubs.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::IFireWireBus;
using ASFW::Audio::AudioBackendKind;
using ASFW::Audio::AudioNubPublisher;
using ASFW::Audio::AudioRuntimeRegistry;
using ASFW::Audio::DuplexRestartReason;
using ASFW::Audio::IIsochDuplexHostTransport;
using ASFW::Audio::Host::AudioDeviceHost;
using ASFW::Audio::Host::DescribeDone;
using ASFW::Audio::Host::DescribeInput;
using ASFW::Audio::Host::DescribeRefusal;
using ASFW::Audio::Host::DescribeResult;
using ASFW::Audio::Host::DeviceEvent;
using ASFW::Audio::Host::DeviceEventSink;
using ASFW::Audio::Host::FamilyAdapter;
using ASFW::Audio::Host::FaultContext;
using ASFW::Audio::Host::FaultVerdict;
using ASFW::Audio::Host::HostEvent;
using ASFW::Audio::Host::HostOutcome;
using ASFW::Audio::Model::ASFWAudioDevice;
using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::RomEntry;
using ASFW::Driver::HardwareInterface;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::LockOp;
using ASFW::FW::NodeId;
using ASFW::IRM::IRMClient;

constexpr auto kWaitLimit = std::chrono::seconds(5);

class NullFireWireBus final : public IFireWireBus {
public:
    AsyncHandle ReadBlock(Generation, NodeId, FWAddress, uint32_t, FwSpeed,
                          ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{.value = 1};
    }
    AsyncHandle WriteBlock(Generation, NodeId, FWAddress, std::span<const uint8_t>, FwSpeed,
                           ASFW::Async::InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return AsyncHandle{.value = 2};
    }
    AsyncHandle Lock(Generation, NodeId, FWAddress, LockOp, std::span<const uint8_t>,
                     uint32_t responseLength, FwSpeed,
                     ASFW::Async::InterfaceCompletionCallback callback) override {
        std::array<uint8_t, 8> zeroes{};
        callback(AsyncStatus::kSuccess, std::span<const uint8_t>(zeroes.data(), responseLength));
        return AsyncHandle{.value = 3};
    }
    bool Cancel(AsyncHandle) override { return false; }
    [[nodiscard]] FwSpeed GetSpeed(NodeId) const override { return FwSpeed::S400; }
    [[nodiscard]] uint32_t HopCount(NodeId, NodeId) const override { return 0; }
    [[nodiscard]] Generation GetGeneration() const override { return Generation{1}; }
    [[nodiscard]] NodeId GetLocalNodeID() const override { return NodeId{0}; }
};

class FakeHostTransport final : public IIsochDuplexHostTransport {
public:
    kern_return_t BeginSplitDuplex(uint64_t) noexcept override { return kIOReturnSuccess; }
    kern_return_t ReservePlaybackResources(uint64_t, IRMClient&, uint64_t, uint32_t,
                                           ASFW::Audio::Backends::IRMReservationResult& outResult) noexcept override {
        outResult.channel = 1;
        outResult.status = kIOReturnSuccess;
        return kIOReturnSuccess;
    }
    kern_return_t ReserveCaptureResources(uint64_t, IRMClient&, uint64_t, uint32_t,
                                          ASFW::Audio::Backends::IRMReservationResult& outResult) noexcept override {
        outResult.channel = 2;
        outResult.status = kIOReturnSuccess;
        return kIOReturnSuccess;
    }
    kern_return_t PrepareReceive(uint8_t, HardwareInterface&, ASFW::Audio::Runtime::IDirectAudioBindingSource*,
                                 const ASFW::Audio::DirectRxFormatDescriptor& = {}) noexcept override { return kIOReturnSuccess; }
    kern_return_t PrepareTransmit(uint8_t, HardwareInterface&, uint8_t, ASFW::FW::FwSpeed) noexcept override { return kIOReturnSuccess; }
    kern_return_t PrepareReceiveStream(uint32_t, uint8_t, HardwareInterface&, ASFW::Audio::Runtime::IDirectAudioBindingSource*,
                                       uint32_t, const ASFW::Audio::DirectRxFormatDescriptor& = {}) noexcept override { return kIOReturnSuccess; }
    kern_return_t PrepareTransmitStream(uint32_t, uint8_t, HardwareInterface&, uint8_t, ASFW::FW::FwSpeed) noexcept override { return kIOReturnSuccess; }
    kern_return_t StartPreparedReceive() noexcept override { return kIOReturnSuccess; }
    kern_return_t StartPreparedTransmit() noexcept override { return kIOReturnSuccess; }
    kern_return_t StopPreparedReceive() noexcept override { return kIOReturnSuccess; }
    kern_return_t StopPreparedTransmit() noexcept override { return kIOReturnSuccess; }
    kern_return_t StopAll() noexcept override { return kIOReturnSuccess; }
    [[nodiscard]] bool IsReceiveReplayEstablished() const noexcept override { return replayEstablished; }
    bool replayEstablished{false};
};

// A scriptable adapter: tests set what Describe and JudgeRuntimeFault answer.
// By default Describe refuses (so a publication that is not under test does
// not write any endpoint config) and JudgeRuntimeFault asks for a restart.
class FakeAdapter final : public FamilyAdapter {
public:
    [[nodiscard]] const char* Name() const noexcept override { return "Fake"; }
    void Describe(const DescribeInput& in, DescribeDone done) override {
        ++describeCalls;
        lastInput = in;
        if (onDescribe) {
            onDescribe(in, std::move(done));
            return;
        }
        done(DescribeRefusal{kIOReturnNotReady, "fake-default"});
    }
    [[nodiscard]] FaultVerdict JudgeRuntimeFault(uint64_t guid, DuplexRestartReason reason,
                                                FaultContext& context) override {
        ++judgeCalls;
        lastGuid = guid;
        lastReason = reason;
        return onJudge ? onJudge(context) : FaultVerdict::kRestart;
    }
    void SetEventSink(DeviceEventSink* sink) noexcept override { this->sink = sink; }

    std::function<void(const DescribeInput&, DescribeDone)> onDescribe{};
    std::function<FaultVerdict(FaultContext&)> onJudge{};
    int describeCalls{0};
    int judgeCalls{0};
    DescribeInput lastInput{};
    uint64_t lastGuid{0};
    std::optional<DuplexRestartReason> lastReason{};
    DeviceEventSink* sink{nullptr};
};

struct HostFixture {
    NullFireWireBus bus{};
    IRMClient irmClient{bus};
    HardwareInterface hardware{};
    DeviceRegistry registry{};
    AudioRuntimeRegistry runtime{};
    FakeHostTransport hostTransport{};
    std::atomic<bool> cancel{false};
    ASFW::Audio::Session::AudioSessions sessions{
        registry, runtime, hostTransport, hardware, &cancel,
        [](uint64_t) -> ASFW::Audio::Runtime::IDirectAudioBindingSource* { return nullptr; }};
    AudioNubPublisher publisher{nullptr};
    FakeAdapter adapter{};
    AudioDeviceHost host{publisher, registry, runtime, sessions, hostTransport};

    HostFixture() { host.Install(AudioBackendKind::Dice, adapter); }

    // A Saffire Pro 24 DSP: its catalog policy resolves to the DICE family.
    void SeedDiceDevice(uint64_t guid) {
        ConfigROM rom{};
        rom.gen = Generation{1};
        rom.firstSeen = Generation{1};
        rom.lastValidated = Generation{1};
        rom.nodeId = 2;
        rom.bib.guid = guid;
        rom.bib.maxRec = 8;
        rom.rootDirMinimal = {
            RomEntry{.key = CfgKey::VendorId, .value = ASFW::DeviceProfiles::Audio::kFocusriteVendorId},
            RomEntry{.key = CfgKey::ModelId, .value = ASFW::DeviceProfiles::Audio::kSPro24DspModelId},
        };
        ASFW::Discovery::UnitDirectory unit{};
        unit.offsetQuadlets = 5;
        unit.unitSpecId = ASFW::DeviceProfiles::Audio::kFocusriteVendorId;
        unit.unitSwVersion = 0x000001;
        rom.unitDirectories.push_back(unit);
        ASFW::Discovery::LinkPolicy link{};
        (void)registry.UpsertFromROM(rom, link);
    }
};

constexpr uint64_t kGuid = 0x00130e0400000001ULL;
constexpr uint64_t kUnknownGuid = 0x00130e04000000ffULL;

ASFWAudioDevice MakeDescription(const std::string& name, uint32_t inputs, uint32_t outputs) {
    ASFWAudioDevice d{};
    d.deviceName = name;
    d.inputChannelCount = inputs;
    d.outputChannelCount = outputs;
    d.channelCount = inputs > outputs ? inputs : outputs;
    return d;
}

// Restart decisions a fault or rebind can record. Exactly one of these must
// be recorded per judged or resumed request when the session is not attached.
uint64_t RestartOutcomes(const AudioDeviceHost& host, HostEvent event) {
    return host.OutcomeCount(event, HostOutcome::RestartRequested) +
           host.OutcomeCount(event, HostOutcome::Declined) +
           host.OutcomeCount(event, HostOutcome::Failed);
}

// True when no endpoint config is visible for `guid`: either no endpoint
// runtime exists, or it holds no valid config.
bool NoEndpointConfig(AudioRuntimeRegistry& runtime, uint64_t guid) {
    auto endpoint = runtime.FindEndpointRuntime(guid);
    if (!endpoint) {
        return true;
    }
    ASFWAudioDevice config{};
    return !endpoint->CopyConfig(config);
}

// Shared expectations for a stream-configuration change that reaches the host.
void ExpectConfigChangeFaultQueued(HostFixture& f) {
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::DeviceEvent, HostOutcome::ConfigChange), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Queued), 1U);
    ASSERT_EQ(f.adapter.judgeCalls, 1);
    EXPECT_EQ(f.adapter.lastGuid, kGuid);
    ASSERT_TRUE(f.adapter.lastReason.has_value());
    EXPECT_EQ(*f.adapter.lastReason, DuplexRestartReason::kDeviceConfigChange);
}

// ---------------------------------------------------------------------------
// Smoke tests (E1 skeleton), kept as they were.
// ---------------------------------------------------------------------------

TEST(AudioDeviceHostTests, InstallWiresTheEventSink) {
    HostFixture f;
    EXPECT_EQ(f.adapter.sink, &f.host);
}

TEST(AudioDeviceHostTests, DescribeRefusalIsRecordedOnce) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    ASSERT_EQ(f.host.KindForGuid(kGuid), AudioBackendKind::Dice);
    f.host.RefreshPublication(kGuid);
    EXPECT_EQ(f.adapter.describeCalls, 1);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 1U);
}

TEST(AudioDeviceHostTests, TeardownIsRecordedAndRefusesLaterPublication) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.host.BeginTeardown();
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Teardown, HostOutcome::Drained), 1U);
    EXPECT_EQ(f.adapter.sink, nullptr);
    f.host.RefreshPublication(kGuid);
    EXPECT_EQ(f.adapter.describeCalls, 0);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::RefusedTeardown), 1U);
}

// ---------------------------------------------------------------------------
// Publication (§4.2 RefreshPublication, §4.3)
// ---------------------------------------------------------------------------

TEST(AudioDeviceHostTests, UnknownGuidIsRefusedForNoPolicy) {
    HostFixture f;
    f.host.RefreshPublication(kUnknownGuid);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::RefusedNoPolicy), 1U);
    EXPECT_EQ(f.adapter.describeCalls, 0);
    EXPECT_TRUE(NoEndpointConfig(f.runtime, kUnknownGuid));
}

TEST(AudioDeviceHostTests, DeviceWithoutInstalledAdapterIsRefusedForNoAdapter) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    // A second host over the same registry and sessions, with nothing installed.
    AudioDeviceHost bare{f.publisher, f.registry, f.runtime, f.sessions, f.hostTransport};
    ASSERT_EQ(bare.KindForGuid(kGuid), AudioBackendKind::Dice);
    bare.RefreshPublication(kGuid);
    EXPECT_EQ(bare.OutcomeCount(HostEvent::Publish, HostOutcome::RefusedNoAdapter), 1U);
    EXPECT_EQ(f.adapter.describeCalls, 0);
}

TEST(AudioDeviceHostTests, DescribeRefusalPublishesNothingAndWritesNoConfig) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.adapter.onDescribe = [](const DescribeInput&, DescribeDone done) {
        done(DescribeRefusal{kIOReturnNotReady, "no geometry yet"});
    };
    f.host.RefreshPublication(kGuid);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::Published), 0U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::Refreshed), 0U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 0U);
    EXPECT_TRUE(NoEndpointConfig(f.runtime, kGuid));
}

TEST(AudioDeviceHostTests, DescriptionIsWrittenToEndpointBeforeNubPublish) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    const ASFWAudioDevice desc = MakeDescription("Pro 24 DSP", 16, 8);
    f.adapter.onDescribe = [&desc](const DescribeInput&, DescribeDone done) { done(desc); };
    f.host.RefreshPublication(kGuid);

    // The publisher has no driver, so no nub can be created (§4.3 "No nub yet").
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::Published), 0U);

    auto endpoint = f.runtime.FindEndpointRuntime(kGuid);
    ASSERT_NE(endpoint, nullptr);
    ASFWAudioDevice written{};
    ASSERT_TRUE(endpoint->CopyConfig(written));
    EXPECT_EQ(written.deviceName, desc.deviceName);
    EXPECT_EQ(written.inputChannelCount, desc.inputChannelCount);
    EXPECT_EQ(written.outputChannelCount, desc.outputChannelCount);
    EXPECT_EQ(written.channelCount, desc.channelCount);
}

// A family that fell back to a model constant says so with DescribedWithNote: the
// publication is decided exactly as for a plain description (one outcome, the
// endpoint config written), and the note rides on the same [AudioHost] line.
TEST(AudioDeviceHostTests, DescriptionWithANoteIsPublishedLikeAPlainOne) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    const ASFWAudioDevice desc = MakeDescription("Fallback", 14, 14);
    f.adapter.onDescribe = [&desc](const DescribeInput&, DescribeDone done) {
        done(ASFW::Audio::Host::DescribedWithNote{desc, "register-read-failed"});
    };
    f.host.RefreshPublication(kGuid);

    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::RefusedDescribe), 0U);
    auto endpoint = f.runtime.FindEndpointRuntime(kGuid);
    ASSERT_NE(endpoint, nullptr);
    ASFWAudioDevice written{};
    ASSERT_TRUE(endpoint->CopyConfig(written));
    EXPECT_EQ(written.deviceName, desc.deviceName);
    EXPECT_EQ(written.inputChannelCount, 14U);
    EXPECT_EQ(written.outputChannelCount, 14U);
}

TEST(AudioDeviceHostTests, KeepCommittedLeavesEndpointAlone) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.adapter.onDescribe = [](const DescribeInput&, DescribeDone done) {
        done(ASFW::Audio::Host::KeepCommitted{});
    };
    f.host.RefreshPublication(kGuid);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::KeptCommittedFormation), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 0U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::Published), 0U);
    EXPECT_TRUE(NoEndpointConfig(f.runtime, kGuid));
}

TEST(AudioDeviceHostTests, DescribeInputCarriesDiscoveredDescriptionAndCancelClearsIt) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.host.RefreshPublication(kGuid);
    ASSERT_EQ(f.adapter.describeCalls, 1);
    EXPECT_EQ(f.adapter.lastInput.record.guid, kGuid);
    ASSERT_NE(f.adapter.lastInput.policy, nullptr);
    EXPECT_EQ(f.adapter.lastInput.protocol, nullptr);
    EXPECT_FALSE(f.adapter.lastInput.committed.has_value());
    EXPECT_FALSE(f.adapter.lastInput.discovered.has_value());

    const ASFWAudioDevice discovered = MakeDescription("Discovered Device", 8, 10);
    f.host.OfferDiscoveredDescription(kGuid, discovered);
    f.host.RefreshPublication(kGuid);
    ASSERT_GE(f.adapter.describeCalls, 2);
    ASSERT_TRUE(f.adapter.lastInput.discovered.has_value());
    EXPECT_EQ(f.adapter.lastInput.discovered->deviceName, "Discovered Device");
    EXPECT_EQ(f.adapter.lastInput.discovered->inputChannelCount, 8U);
    EXPECT_EQ(f.adapter.lastInput.discovered->outputChannelCount, 10U);

    const int before = f.adapter.describeCalls;
    f.host.CancelRemoteDeviceWork(kGuid);
    f.host.RefreshPublication(kGuid);
    ASSERT_EQ(f.adapter.describeCalls, before + 1);
    EXPECT_FALSE(f.adapter.lastInput.discovered.has_value());
}

TEST(AudioDeviceHostTests, DescribeCompletingAfterTeardownBeganIsRefused) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    DescribeDone pending{};
    f.adapter.onDescribe = [&pending](const DescribeInput&, DescribeDone done) {
        pending = std::move(done);  // completes later, on this test's thread
    };
    f.host.RefreshPublication(kGuid);
    ASSERT_TRUE(static_cast<bool>(pending));
    ASSERT_EQ(f.adapter.describeCalls, 1);

    // The gate-closed hook fires inside CloseAndWait, before teardown waits for
    // the in-flight publication. That is the signal that teardown is waiting.
    std::promise<void> gateClosed;
    auto gateClosedFuture = gateClosed.get_future();
    f.host.SetOnTeardownGateClosedHookForTesting([&gateClosed]() { gateClosed.set_value(); });

    std::thread teardown([&f]() { f.host.BeginTeardown(); });
    const bool waiting = gateClosedFuture.wait_for(kWaitLimit) == std::future_status::ready;
    EXPECT_TRUE(waiting) << "teardown never closed the gate";

    // Complete the description now, after teardown began. Always release the
    // stored callback so the teardown thread can finish even if the wait failed.
    pending(DescribeResult{MakeDescription("Late Description", 2, 2)});
    teardown.join();

    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::RefusedTeardown), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::PublishFailed), 0U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Publish, HostOutcome::Published), 0U);
    EXPECT_TRUE(NoEndpointConfig(f.runtime, kGuid));
    EXPECT_TRUE(f.host.IsTeardownCompleteForTesting());
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Teardown, HostOutcome::Drained), 1U);
}

// ---------------------------------------------------------------------------
// Runtime faults (§4.2 OnRuntimeFault, FaultContext)
// ---------------------------------------------------------------------------

TEST(AudioDeviceHostTests, SelfHealedFaultIsDroppedWithoutRestart) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.adapter.onJudge = [](FaultContext&) { return FaultVerdict::kSelfHealed; };
    f.host.OnRuntimeFault(kGuid, DuplexRestartReason::kRecoverAfterTimingLoss);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Queued), 1U);
    ASSERT_EQ(f.adapter.judgeCalls, 1);
    EXPECT_EQ(f.adapter.lastGuid, kGuid);
    ASSERT_TRUE(f.adapter.lastReason.has_value());
    EXPECT_EQ(*f.adapter.lastReason, DuplexRestartReason::kRecoverAfterTimingLoss);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::SelfHealed), 1U);
    EXPECT_EQ(RestartOutcomes(f.host, HostEvent::RuntimeFault), 0U);
}

TEST(AudioDeviceHostTests, RestartVerdictRecordsExactlyOneRestartOutcome) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.adapter.onJudge = [](FaultContext&) { return FaultVerdict::kRestart; };
    f.host.OnRuntimeFault(kGuid, DuplexRestartReason::kRecoverAfterCycleInconsistent);
    ASSERT_EQ(f.adapter.judgeCalls, 1);
    EXPECT_EQ(RestartOutcomes(f.host, HostEvent::RuntimeFault), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::SelfHealed), 0U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::DeviceLeft), 0U);
}

TEST(AudioDeviceHostTests, DeviceLeftVerdictIsRecordedWithoutRestart) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.adapter.onJudge = [](FaultContext&) { return FaultVerdict::kDeviceLeft; };
    f.host.OnRuntimeFault(kGuid, DuplexRestartReason::kRecoverAfterLockLoss);
    ASSERT_EQ(f.adapter.judgeCalls, 1);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::DeviceLeft), 1U);
    EXPECT_EQ(RestartOutcomes(f.host, HostEvent::RuntimeFault), 0U);
}

TEST(AudioDeviceHostTests, SecondFaultWhileQueuedIsDedupedThenReleasedByDrain) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    auto* queue = f.host.WorkQueueForTesting();
    ASSERT_NE(queue, nullptr);
    queue->SetManualDispatchForTesting(true);

    f.host.OnRuntimeFault(kGuid, DuplexRestartReason::kRecoverAfterTimingLoss);
    f.host.OnRuntimeFault(kGuid, DuplexRestartReason::kRecoverAfterTimingLoss);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Queued), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Deduped), 1U);
    EXPECT_EQ(queue->PendingTaskCountForTesting(), 1U);
    EXPECT_EQ(f.adapter.judgeCalls, 0);

    queue->DrainAllForTesting();
    EXPECT_EQ(f.adapter.judgeCalls, 1);

    // The drain released the dedupe entry: a new fault queues again.
    f.host.OnRuntimeFault(kGuid, DuplexRestartReason::kRecoverAfterTimingLoss);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Queued), 2U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Deduped), 1U);
    queue->DrainAllForTesting();
    EXPECT_EQ(f.adapter.judgeCalls, 2);
    EXPECT_EQ(RestartOutcomes(f.host, HostEvent::RuntimeFault), 2U);
}

TEST(AudioDeviceHostTests, QueuedFaultCancelledByTeardownIsNeverJudged) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    auto* queue = f.host.WorkQueueForTesting();
    ASSERT_NE(queue, nullptr);
    queue->SetManualDispatchForTesting(true);

    f.host.OnRuntimeFault(kGuid, DuplexRestartReason::kRecoverAfterTimingLoss);
    ASSERT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Queued), 1U);
    ASSERT_EQ(queue->PendingTaskCountForTesting(), 1U);

    f.host.BeginTeardown();
    queue->DrainAllForTesting();
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Cancelled), 1U);
    EXPECT_EQ(f.adapter.judgeCalls, 0);
    EXPECT_EQ(RestartOutcomes(f.host, HostEvent::RuntimeFault), 0U);
}

TEST(AudioDeviceHostTests, FaultAfterTeardownIsCancelledImmediately) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    auto* queue = f.host.WorkQueueForTesting();
    ASSERT_NE(queue, nullptr);
    queue->SetManualDispatchForTesting(true);

    f.host.BeginTeardown();
    f.host.OnRuntimeFault(kGuid, DuplexRestartReason::kRecoverAfterTimingLoss);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Cancelled), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Queued), 0U);
    EXPECT_EQ(queue->PendingTaskCountForTesting(), 0U);
    queue->DrainAllForTesting();
    EXPECT_EQ(f.adapter.judgeCalls, 0);
}

TEST(AudioDeviceHostTests, FaultWithoutAdapterIsDeclinedAndNotQueued) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    AudioDeviceHost bare{f.publisher, f.registry, f.runtime, f.sessions, f.hostTransport};
    auto* queue = bare.WorkQueueForTesting();
    ASSERT_NE(queue, nullptr);
    queue->SetManualDispatchForTesting(true);

    bare.OnRuntimeFault(kGuid, DuplexRestartReason::kRecoverAfterTimingLoss);
    EXPECT_EQ(bare.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Declined), 1U);
    EXPECT_EQ(bare.OutcomeCount(HostEvent::RuntimeFault, HostOutcome::Queued), 0U);
    EXPECT_EQ(queue->PendingTaskCountForTesting(), 0U);
    EXPECT_EQ(f.adapter.judgeCalls, 0);
}

TEST(AudioDeviceHostTests, FaultContextAnswersMatchTheHostState) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.hostTransport.replayEstablished = true;

    std::vector<uint32_t> sleeps;
    f.host.SetSleepForTesting([&sleeps](uint32_t ms) { sleeps.push_back(ms); });

    bool sawCancelled = true;
    bool sawStreaming = true;
    bool sawReplay = false;
    bool readHealthFailed = false;
    f.adapter.onJudge = [&](FaultContext& ctx) {
        ctx.Sleep(42);
        sawCancelled = ctx.Cancelled();
        sawStreaming = ctx.StillStreaming();
        sawReplay = ctx.ReceiveReplayEstablished();
        readHealthFailed = !ctx.ReadHealth(AudioDeviceHost::kHealthReadTimeoutMs).has_value();
        return FaultVerdict::kSelfHealed;
    };
    f.host.OnRuntimeFault(kGuid, DuplexRestartReason::kRecoverAfterTimingLoss);

    ASSERT_EQ(f.adapter.judgeCalls, 1);
    ASSERT_EQ(sleeps.size(), 1U);
    EXPECT_EQ(sleeps.front(), 42U);
    EXPECT_TRUE(sawReplay);
    EXPECT_FALSE(sawStreaming);
    EXPECT_FALSE(sawCancelled);
    EXPECT_TRUE(readHealthFailed) << "ReadHealth without a protocol must return an error";
}

// ---------------------------------------------------------------------------
// Device events (§4.2 OnDeviceEvent, §4.4)
// ---------------------------------------------------------------------------

TEST(AudioDeviceHostTests, StreamConfigChangeRaisesConfigChangeAndDeviceConfigFault) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.host.OnDeviceEvent(kGuid, DeviceEvent::kStreamConfigChanged, 0);
    ExpectConfigChangeFaultQueued(f);
    EXPECT_EQ(RestartOutcomes(f.host, HostEvent::RuntimeFault), 1U);
}

TEST(AudioDeviceHostTests, ClockStatusWhileNotStreamingQueuesNoProbe) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    auto* queue = f.host.WorkQueueForTesting();
    ASSERT_NE(queue, nullptr);
    queue->SetManualDispatchForTesting(true);

    f.host.OnDeviceEvent(kGuid, DeviceEvent::kClockStatusChanged, 0);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::DeviceEvent, HostOutcome::NotStreaming), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::DeviceEvent, HostOutcome::ClockProbe), 0U);
    EXPECT_EQ(queue->PendingTaskCountForTesting(), 0U);
    EXPECT_EQ(f.adapter.judgeCalls, 0);
}

TEST(AudioDeviceHostTests, ClockStatusAfterTeardownIsCancelled) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.host.BeginTeardown();
    f.host.OnDeviceEvent(kGuid, DeviceEvent::kClockStatusChanged, 0);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::DeviceEvent, HostOutcome::Cancelled), 1U);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::DeviceEvent, HostOutcome::NotStreaming), 0U);
}

TEST(AudioDeviceHostTests, EventsThroughTheInstalledSinkMatchDirectCalls) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    ASSERT_NE(f.adapter.sink, nullptr);
    f.adapter.sink->OnDeviceEvent(kGuid, DeviceEvent::kStreamConfigChanged, 0);
    ExpectConfigChangeFaultQueued(f);
    EXPECT_EQ(RestartOutcomes(f.host, HostEvent::RuntimeFault), 1U);
}

// ---------------------------------------------------------------------------
// Rebind on bus reset (§4.2 OnDeviceResumed)
// ---------------------------------------------------------------------------

TEST(AudioDeviceHostTests, ResumeOfNonStreamingDeviceIsNotStreaming) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.host.OnDeviceResumed(kGuid);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Rebind, HostOutcome::NotStreaming), 1U);
    EXPECT_EQ(RestartOutcomes(f.host, HostEvent::Rebind), 0U);
}

TEST(AudioDeviceHostTests, ResumeAfterTeardownIsCancelled) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    f.host.BeginTeardown();
    f.host.OnDeviceResumed(kGuid);
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Rebind, HostOutcome::Cancelled), 1U);
    EXPECT_EQ(RestartOutcomes(f.host, HostEvent::Rebind), 0U);
}

// ---------------------------------------------------------------------------
// Teardown (§4.2 BeginTeardown, §6.1)
// ---------------------------------------------------------------------------

TEST(AudioDeviceHostTests, TeardownIsIdempotent) {
    HostFixture f;
    f.host.BeginTeardown();
    f.host.BeginTeardown();
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Teardown, HostOutcome::Drained), 1U);
    EXPECT_TRUE(f.host.IsTeardownCompleteForTesting());
}

TEST(AudioDeviceHostTests, ConcurrentSecondTeardownWaitsForTheFirst) {
    HostFixture f;

    std::promise<void> drainStarted;
    std::promise<void> releaseDrain;
    std::promise<void> secondaryWaiting;
    auto drainStartedFuture = drainStarted.get_future();
    auto releaseDrainFuture = releaseDrain.get_future();
    auto secondaryWaitingFuture = secondaryWaiting.get_future();
    std::atomic<int> secondarySignals{0};

    f.host.SetOnTeardownDrainStartedHookForTesting([&drainStarted, &releaseDrainFuture]() {
        drainStarted.set_value();
        releaseDrainFuture.wait();  // thread A parks here until released below
    });
    f.host.SetOnSecondaryTeardownWaitingHookForTesting([&secondaryWaiting, &secondarySignals]() {
        if (secondarySignals.fetch_add(1) == 0) {
            secondaryWaiting.set_value();
        }
    });

    std::atomic<bool> aReturned{false};
    std::atomic<bool> bReturned{false};
    std::atomic<bool> bSawIncomplete{false};

    std::thread a([&f, &aReturned]() {
        f.host.BeginTeardown();
        aReturned = true;
    });

    const bool drainStartedOk = drainStartedFuture.wait_for(kWaitLimit) == std::future_status::ready;
    EXPECT_TRUE(drainStartedOk);

    std::thread b([&f, &bReturned, &bSawIncomplete]() {
        f.host.BeginTeardown();
        bSawIncomplete = !f.host.IsTeardownCompleteForTesting();
        bReturned = true;
    });

    const bool secondaryOk = secondaryWaitingFuture.wait_for(kWaitLimit) == std::future_status::ready;
    EXPECT_TRUE(secondaryOk);
    EXPECT_FALSE(bReturned.load()) << "second caller returned while the first was still draining";

    releaseDrain.set_value();
    a.join();
    b.join();

    EXPECT_TRUE(aReturned.load());
    EXPECT_TRUE(bReturned.load());
    EXPECT_FALSE(bSawIncomplete.load()) << "second caller returned before teardown completed";
    EXPECT_TRUE(f.host.IsTeardownCompleteForTesting());
    EXPECT_EQ(f.host.OutcomeCount(HostEvent::Teardown, HostOutcome::Drained), 1U);
}

TEST(AudioDeviceHostTests, TeardownDetachesTheEventSink) {
    HostFixture f;
    ASSERT_EQ(f.adapter.sink, &f.host);
    f.host.BeginTeardown();
    EXPECT_EQ(f.adapter.sink, nullptr);
}

// ---------------------------------------------------------------------------
// Misc
// ---------------------------------------------------------------------------

TEST(AudioDeviceHostTests, KindForGuidResolvesSeededDeviceOnly) {
    HostFixture f;
    f.SeedDiceDevice(kGuid);
    EXPECT_EQ(f.host.KindForGuid(kGuid), AudioBackendKind::Dice);
    EXPECT_FALSE(f.host.KindForGuid(kUnknownGuid).has_value());
    EXPECT_FALSE(f.host.KindForGuid(0).has_value());
}

TEST(AudioDeviceHostTests, ToStringNamesEveryEventAndOutcome) {
    for (uint8_t i = 0; i < static_cast<uint8_t>(HostEvent::kCount); ++i) {
        const char* name = ASFW::Audio::Host::ToString(static_cast<HostEvent>(i));
        ASSERT_NE(name, nullptr) << "event " << int{i};
        EXPECT_STRNE(name, "") << "event " << int{i};
        EXPECT_STRNE(name, "?") << "event " << int{i};
    }
    for (uint8_t i = 0; i < static_cast<uint8_t>(HostOutcome::kCount); ++i) {
        const char* name = ASFW::Audio::Host::ToString(static_cast<HostOutcome>(i));
        ASSERT_NE(name, nullptr) << "outcome " << int{i};
        EXPECT_STRNE(name, "") << "outcome " << int{i};
        EXPECT_STRNE(name, "?") << "outcome " << int{i};
    }
}

} // namespace
