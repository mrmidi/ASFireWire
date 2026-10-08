// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioDeviceHostTests.cpp
// Behaviour of the audio device host (documentation/AUDIO_DEVICE_HOST.md §4,
// §6.1). Assertions are on the host's per-outcome counters: every decision the
// host makes is Record()ed exactly once.

#include <gtest/gtest.h>
#include <net.asfw.driver/ASFWAudioNub.h>

#include "Async/Interfaces/IFireWireBus.hpp"
#include "Audio/Core/AudioNubPublisher.hpp"
#include "Audio/Core/AudioRuntimeRegistry.hpp"
#include "Audio/Host/AudioDeviceHost.hpp"
#include "Audio/Protocols/Backends/IsochDuplexHostTransport.hpp"
#include "Audio/Session/AudioSessions.hpp"
#include "Bus/IRM/IRMClient.hpp"
#include "DeviceProfiles/Audio/AudioDeviceIds.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Testing/HostDriverKitStubs.hpp"

#include <array>
#include <atomic>
#include <functional>
#include <span>

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
using ASFW::Audio::Host::DeviceEventSink;
using ASFW::Audio::Host::FamilyAdapter;
using ASFW::Audio::Host::FaultContext;
using ASFW::Audio::Host::FaultVerdict;
using ASFW::Audio::Host::HostEvent;
using ASFW::Audio::Host::HostOutcome;
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
    [[nodiscard]] FaultVerdict JudgeRuntimeFault(uint64_t, DuplexRestartReason, FaultContext& context) override {
        ++judgeCalls;
        return onJudge ? onJudge(context) : FaultVerdict::kRestart;
    }
    void SetEventSink(DeviceEventSink* sink) noexcept override { this->sink = sink; }

    std::function<void(const DescribeInput&, DescribeDone)> onDescribe{};
    std::function<FaultVerdict(FaultContext&)> onJudge{};
    int describeCalls{0};
    int judgeCalls{0};
    DescribeInput lastInput{};
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

// Smoke tests for the E1 skeleton. The behavioural suite (§6 E1) is added
// alongside, written from the design doc.

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

} // namespace
