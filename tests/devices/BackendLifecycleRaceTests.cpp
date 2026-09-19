// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BackendLifecycleRaceTests.cpp
// Controlled interleaving tests for audio backend concurrent teardown and
// publication-vs-teardown race conditions.

#include <gtest/gtest.h>

#include "Async/Interfaces/IFireWireBus.hpp"
#include "Audio/Core/AudioNubPublisher.hpp"
#include "Audio/Core/AudioRuntimeRegistry.hpp"
#include "Audio/Protocols/Backends/AVCAudioBackend.hpp"
#include "Audio/Protocols/Backends/DiceAudioBackend.hpp"
#include "Audio/Protocols/Backends/AudioDuplexCoordinator.hpp"
#include "Bus/IRM/IRMClient.hpp"
#include "Discovery/DeviceRegistry.hpp"
#include "Hardware/HardwareInterface.hpp"
#include "Testing/HostDriverKitStubs.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::IFireWireBus;
using ASFW::Audio::AudioNubPublisher;
using ASFW::Audio::AudioRuntimeRegistry;
using ASFW::Audio::AudioDuplexCoordinator;
using ASFW::Audio::AVCAudioBackend;
using ASFW::Audio::DiceAudioBackend;
using ASFW::Audio::IIsochDuplexHostTransport;
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
    kern_return_t ReservePlaybackResources(uint64_t, IRMClient&, uint64_t, uint32_t, uint8_t& outChannel) noexcept override {
        outChannel = 1;
        return kIOReturnSuccess;
    }
    kern_return_t ReserveCaptureResources(uint64_t, IRMClient&, uint64_t, uint32_t, uint8_t& outChannel) noexcept override {
        outChannel = 2;
        return kIOReturnSuccess;
    }
    kern_return_t PrepareReceive(uint8_t, HardwareInterface&, ASFW::Audio::Runtime::IDirectAudioBindingSource*,
                                 ASFW::Encoding::AudioWireFormat, uint32_t, uint32_t, bool, uint32_t,
                                 ASFW::Encoding::Motu::MotuPortMap) noexcept override { return kIOReturnSuccess; }
    kern_return_t PrepareTransmit(uint8_t, HardwareInterface&, uint8_t) noexcept override { return kIOReturnSuccess; }
    kern_return_t PrepareReceiveStream(uint32_t, uint8_t, HardwareInterface&, ASFW::Audio::Runtime::IDirectAudioBindingSource*,
                                       uint32_t, uint32_t, ASFW::Encoding::AudioWireFormat, uint32_t, bool, uint32_t,
                                       ASFW::Encoding::Motu::MotuPortMap) noexcept override { return kIOReturnSuccess; }
    kern_return_t PrepareTransmitStream(uint32_t, uint8_t, HardwareInterface&, uint8_t) noexcept override { return kIOReturnSuccess; }
    kern_return_t StartPreparedReceive() noexcept override { return kIOReturnSuccess; }
    kern_return_t StartPreparedTransmit() noexcept override { return kIOReturnSuccess; }
    kern_return_t StopPreparedReceive() noexcept override { return kIOReturnSuccess; }
    kern_return_t StopPreparedTransmit() noexcept override { return kIOReturnSuccess; }
    kern_return_t StopAll() noexcept override { return kIOReturnSuccess; }
};

struct TestFixture {
    NullFireWireBus bus{};
    IRMClient irmClient{bus};
    HardwareInterface hardware{};
    DeviceRegistry registry{};
    AudioRuntimeRegistry runtime{};
    FakeHostTransport hostTransport{};
    std::atomic<bool> cancel{false};
    AudioDuplexCoordinator coordinator{
        registry, runtime, hostTransport, hardware, &cancel,
        [](uint64_t) -> ASFW::Audio::Runtime::IDirectAudioBindingSource* {
            return nullptr;
        }};
    AudioNubPublisher publisher{nullptr};
    AVCAudioBackend avc{publisher, registry, runtime, hostTransport, coordinator, hardware};
    DiceAudioBackend dice{publisher, registry, runtime, coordinator, hardware};

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

// Case 1: Concurrent teardown on AVCAudioBackend
// Hold queued work open, start teardown A, invoke teardown B before releasing work.
// Neither call must report completion before the drain finishes.
TEST(BackendLifecycleRaceTests, AVCAudioBackendConcurrentTeardownWaitsForDrain) {
    TestFixture f;
    auto* queue = f.avc.WorkQueueForTesting();
    ASSERT_NE(queue, nullptr);

    std::unique_lock<std::mutex> queueLock(queue->ExecutionMutexForTesting());

    std::promise<void> drainStartedA;
    std::promise<void> waitingB;

    f.avc.SetOnTeardownDrainStartedHookForTesting([&] {
        drainStartedA.set_value();
    });
    f.avc.SetOnSecondaryTeardownWaitingHookForTesting([&] {
        waitingB.set_value();
    });

    auto futA = std::async(std::launch::async, [&] {
        f.avc.BeginTeardown();
    });

    // Wait until teardown A has reached the work queue drain (and is blocked on queueLock)
    drainStartedA.get_future().wait();

    auto futB = std::async(std::launch::async, [&] {
        f.avc.BeginTeardown();
    });

    // Wait until teardown B has entered the secondary wait loop
    waitingB.get_future().wait();

    // While queued work is held open, neither caller must have completed teardown
    EXPECT_EQ(futA.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    EXPECT_EQ(futB.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    EXPECT_FALSE(f.avc.IsTeardownCompleteForTesting());

    // Release queued work drain
    queueLock.unlock();

    // Both calls must now complete cleanly
    EXPECT_EQ(futA.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(futB.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(f.avc.IsTeardownCompleteForTesting());

    f.avc.SetOnTeardownDrainStartedHookForTesting({});
    f.avc.SetOnSecondaryTeardownWaitingHookForTesting({});
}

// Case 1 (DICE): Concurrent teardown on DiceAudioBackend
TEST(BackendLifecycleRaceTests, DiceAudioBackendConcurrentTeardownWaitsForDrain) {
    TestFixture f;
    auto* queue = f.dice.WorkQueueForTesting();
    ASSERT_NE(queue, nullptr);

    std::unique_lock<std::mutex> queueLock(queue->ExecutionMutexForTesting());

    std::promise<void> drainStartedA;
    std::promise<void> waitingB;

    f.dice.SetOnTeardownDrainStartedHookForTesting([&] {
        drainStartedA.set_value();
    });
    f.dice.SetOnSecondaryTeardownWaitingHookForTesting([&] {
        waitingB.set_value();
    });

    auto futA = std::async(std::launch::async, [&] {
        f.dice.BeginTeardown();
    });

    drainStartedA.get_future().wait();

    auto futB = std::async(std::launch::async, [&] {
        f.dice.BeginTeardown();
    });

    waitingB.get_future().wait();

    EXPECT_EQ(futA.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    EXPECT_EQ(futB.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);
    EXPECT_FALSE(f.dice.IsTeardownCompleteForTesting());

    queueLock.unlock();

    EXPECT_EQ(futA.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(futB.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_TRUE(f.dice.IsTeardownCompleteForTesting());

    f.dice.SetOnTeardownDrainStartedHookForTesting({});
    f.dice.SetOnSecondaryTeardownWaitingHookForTesting({});
}

// Case 4: Publication versus Teardown on AVCAudioBackend
// Pause publication after its admission check, run teardown concurrently, then release publication.
// Teardown waits or publication is cancelled; no publication occurs after teardown completes.
TEST(BackendLifecycleRaceTests, AVCAudioBackendPublicationPausedAfterAdmissionAbortsOnTeardown) {
    TestFixture f;
    const uint64_t guid = 0x0011223344556677ULL;
    ASFW::Audio::Model::ASFWAudioDevice config{};
    config.guid = guid;

    std::promise<void> admitted;
    std::promise<void> allowResume;
    std::promise<void> teardownGateClosed;

    f.avc.SetBeforePublishHookForTesting([&] {
        admitted.set_value();
        allowResume.get_future().wait();
    });

    f.avc.SetOnTeardownGateClosedHookForTesting([&] {
        teardownGateClosed.set_value();
    });

    auto pubFut = std::async(std::launch::async, [&] {
        f.avc.OnAudioConfigurationReady(guid, config);
    });

    admitted.get_future().wait();

    auto teardownFut = std::async(std::launch::async, [&] {
        f.avc.BeginTeardown();
    });

    // Wait until teardown has reached CloseAndWait, atomically closed the gate, and is waiting
    teardownGateClosed.get_future().wait();
    EXPECT_EQ(teardownFut.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    // Resume publication: it observes stopping_ is now true and aborts
    allowResume.set_value();

    pubFut.wait();
    teardownFut.wait();

    // Verification: no nub was published after teardown finished
    EXPECT_EQ(f.publisher.GetNub(guid), nullptr);
    EXPECT_EQ(f.avc.PublicationRejectCountForTesting(), 1u);

    f.avc.SetBeforePublishHookForTesting({});
    f.avc.SetOnTeardownGateClosedHookForTesting({});
}

// Case 4 (DICE): Publication versus Teardown on DiceAudioBackend
TEST(BackendLifecycleRaceTests, DiceAudioBackendPublicationPausedAfterAdmissionAbortsOnTeardown) {
    TestFixture f;
    const uint64_t guid = 0x00130E0402004713ULL;
    f.SeedDiceDevice(guid);

    std::promise<void> admitted;
    std::promise<void> allowResume;
    std::promise<void> teardownGateClosed;

    f.dice.SetBeforePublishHookForTesting([&] {
        admitted.set_value();
        allowResume.get_future().wait();
    });

    f.dice.SetOnTeardownGateClosedHookForTesting([&] {
        teardownGateClosed.set_value();
    });

    auto pubFut = std::async(std::launch::async, [&] {
        f.dice.EnsureNubForGuidForTesting(guid);
    });

    admitted.get_future().wait();

    auto teardownFut = std::async(std::launch::async, [&] {
        f.dice.BeginTeardown();
    });

    teardownGateClosed.get_future().wait();
    EXPECT_EQ(teardownFut.wait_for(std::chrono::milliseconds(0)), std::future_status::timeout);

    allowResume.set_value();

    pubFut.wait();
    teardownFut.wait();

    EXPECT_EQ(f.publisher.GetNub(guid), nullptr);
    EXPECT_GE(f.dice.PublicationRejectCountForTesting(), 1u);

    f.dice.SetBeforePublishHookForTesting({});
    f.dice.SetOnTeardownGateClosedHookForTesting({});
}

} // namespace
