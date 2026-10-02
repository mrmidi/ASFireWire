// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvcDiscoveryLifecycleTests.cpp - AVCDiscovery through attach, bus reset,
// resume and rescan, against a simulated AV/C unit.
//
// The host IOLock aborts on a recursive lock as the dext's os_unfair_lock does,
// so a completion that re-enters AVCDiscovery's lock fails these tests the way
// it crashed the driver on hardware.

#include <gtest/gtest.h>
#include "ASFWDriver/Audio/Protocols/AVC/DiscoveryCoordinator.hpp"

#include "AvcDeviceImages.inc"
#include "FakeTimerScheduler.hpp"
#include "RecordingFireWireBus.hpp"
#include "SimulatedAvcUnit.hpp"

#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Discovery/FWDevice.hpp"
#include "ASFWDriver/Discovery/FWUnit.hpp"
#include "ASFWDriver/Discovery/IDeviceManager.hpp"
#include "ASFWDriver/Protocols/AVC/AVCDiscovery.hpp"
#include "ASFWDriver/Protocols/BeBoB/Bootloader/BeBoBBootloaderCue.hpp"
#include "ASFWDriver/Protocols/BeBoB/Bootloader/BeBoBBootloaderPreparation.hpp"
#include "ASFWDriver/Audio/Model/ASFWAudioDevice.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace {

using namespace ASFW;
using namespace ASFW::AVC::Testing;
using namespace ASFW::Protocols::AVC;
using namespace ASFW::Testing;

// Units come from the test, not from ROM scanning; only observer registration
// matters to AVCDiscovery.
class FakeDeviceManager final : public Discovery::IDeviceManager {
public:
    std::vector<std::shared_ptr<Discovery::FWUnit>> FindUnitsBySpec(uint32_t, std::optional<uint32_t>) const override {
        return {};
    }
    std::vector<std::shared_ptr<Discovery::FWUnit>> GetAllUnits() const override { return {}; }
    std::vector<std::shared_ptr<Discovery::FWUnit>> GetReadyUnits() const override { return {}; }
    void RegisterUnitObserver(Discovery::IUnitObserver*) override {}
    void UnregisterUnitObserver(Discovery::IUnitObserver*) override {}
    CallbackHandle RegisterUnitCallback(uint32_t, std::optional<uint32_t>, UnitCallback) override { return 0; }
    void UnregisterCallback(CallbackHandle) override {}
    std::shared_ptr<Discovery::FWDevice> GetDeviceByGUID(Discovery::Guid64) const override { return nullptr; }
    std::shared_ptr<Discovery::FWDevice> GetDeviceByNode(Discovery::Generation, uint8_t) const override {
        return nullptr;
    }
    std::vector<std::shared_ptr<Discovery::FWDevice>> GetDevicesByGeneration(Discovery::Generation) const override {
        return {};
    }
    std::vector<std::shared_ptr<Discovery::FWDevice>> GetAllDevices() const override { return {}; }
    std::vector<std::shared_ptr<Discovery::FWDevice>> GetReadyDevices() const override { return {}; }
    void RegisterDeviceObserver(Discovery::IDeviceObserver*) override {}
    void UnregisterDeviceObserver(Discovery::IDeviceObserver*) override {}
    std::shared_ptr<Discovery::FWDevice> UpsertDevice(const Discovery::DeviceRecord&,
                                                      const Discovery::ConfigROM&) override {
        return nullptr;
    }
    void MarkDeviceLost(Discovery::Guid64) override {}
    void TerminateDevice(Discovery::Guid64) override {}
};

// An AV/C unit no catalog row names: generic discovery (AvcProbeAdmissionTests).
constexpr uint64_t kGuid = 0x00ABCD0000000001ULL;
constexpr uint32_t kVendor = 0x00ABCD;
constexpr uint32_t kModel = 0x000001;
constexpr uint32_t kTa1394Specifier = 0x00A02D;
constexpr uint32_t kAvcVersion = 0x010001;
constexpr uint16_t kDeviceNode = 0;
constexpr uint64_t kRescanDelayNs = 250ULL * 1'000'000ULL;

class DiscoveryRig {
public:
    DiscoveryRig() : sim_(kDuet) {
        bus_.SetLocalNodeID(FW::NodeId{1});
        sim_.SetNodeId(FW::NodeId{static_cast<uint8_t>(kDeviceNode)});
        const auto record = RouteAt(1);
        device_ = Discovery::FWDevice::Create(record, Rom(1));
        unit_ = Discovery::FWUnit::Create(device_, 9,
                                          {{Discovery::CfgKey::Unit_Spec_Id, kTa1394Specifier},
                                           {Discovery::CfgKey::Unit_Sw_Version, kAvcVersion}});
        // As the device manager does before OnUnitPublished: only a Ready unit
        // has its FCP responses routed to it (AVCDiscovery::RebuildNodeIDMap).
        unit_->Publish();
        coordinator_ = std::make_shared<ASFW::Audio::AVC::DiscoveryCoordinator>(routes_, bus_, bus_, &listener_);
        discovery_ = std::make_shared<AVCDiscovery>(nullptr, routes_, manager_, bus_, bus_, timers_, coordinator_);
        sim_.AttachToBus(bus_, [this](uint16_t srcNode, uint32_t gen, std::span<const uint8_t> payload) {
            if (auto transport = discovery_->AcquireFCPTransportForNodeID(srcNode)) {
                transport->OnFCPResponse(srcNode, gen, payload);
            }
        });
    }

    ~DiscoveryRig() { discovery_->Shutdown(); }

    /// A bus reset as ControllerCore delivers it: every route is invalidated
    /// first (so commands for the old generation fail at once), then discovery
    /// hears of the reset.
    void BusReset(uint32_t generation) {
        bus_.SetGeneration(FW::Generation{generation});
        sim_.SetGeneration(FW::Generation{generation});
        routes_.InvalidateLiveMappingsForBusReset();
        discovery_->OnBusReset(generation);
    }

    /// The ROM rescan finds the unit again on the new generation.
    void Resume(uint32_t generation) {
        (void)RouteAt(generation);
        device_->Resume(Discovery::Generation{generation}, kDeviceNode, Discovery::LinkPolicy{});
        unit_->Resume();
        discovery_->OnUnitResumed(unit_);
    }

    [[nodiscard]] AVCDiscovery& Discovery() noexcept { return *discovery_; }
    [[nodiscard]] SimulatedAvcUnit& Sim() noexcept { return sim_; }
    [[nodiscard]] FakeTimerScheduler& Timers() noexcept { return timers_; }
    [[nodiscard]] RecordingFireWireBus& Bus() noexcept { return bus_; }
    [[nodiscard]] std::shared_ptr<Discovery::FWUnit> Unit() const noexcept { return unit_; }
    [[nodiscard]] ASFW::Audio::AVC::DiscoveryCoordinator& Coordinator() noexcept { return *coordinator_; }
    [[nodiscard]] unsigned Published() const noexcept { return listener_.published; }
    void SetAudioActive(bool active) noexcept { listener_.streaming = active; }
    [[nodiscard]] size_t FcpWrites() const {
        return static_cast<size_t>(std::ranges::count_if(bus_.Operations(), [](const RecordedOp& op) {
            return op.kind == OpKind::Write && op.addressLo == 0xF0000B00U;
        }));
    }
    /// Let 40 s of driver time pass: deferred completions, FCP timeouts, retries.
    void Settle() { for (int i = 0; i < 4000; ++i) timers_.Advance(10ULL * 1000 * 1000); }

private:
    static Discovery::ConfigROM Rom(uint32_t generation) {
        Discovery::ConfigROM rom{};
        rom.bib.guid = kGuid;
        rom.gen = FW::Generation{generation};
        rom.nodeId = kDeviceNode;
        rom.rootDirMinimal = {{Discovery::CfgKey::VendorId, kVendor}, {Discovery::CfgKey::ModelId, kModel}};
        Discovery::UnitDirectory unit{};
        unit.offsetQuadlets = 9;
        unit.unitSpecId = kTa1394Specifier;
        unit.unitSwVersion = kAvcVersion;
        rom.unitDirectories = {unit};
        return rom;
    }

    Discovery::DeviceRecord RouteAt(uint32_t generation) {
        bus_.SetGeneration(FW::Generation{generation});
        sim_.SetGeneration(FW::Generation{generation});
        return routes_.UpsertFromROM(Rom(generation), Discovery::LinkPolicy{});
    }

    struct Listener final : ASFW::Audio::IAVCAudioConfigListener {
        void OnAVCAudioConfigurationReady(uint64_t, const ASFW::Audio::Model::ASFWAudioDevice&) noexcept override { ++published; }
        bool IsAudioActive(uint64_t) const noexcept override { return streaming; }
        unsigned published{0};
        bool streaming{false};
    } listener_;
    RecordingFireWireBus bus_;
    FakeTimerScheduler timers_;
    Discovery::DeviceRegistry routes_;
    FakeDeviceManager manager_;
    SimulatedAvcUnit sim_;
    std::shared_ptr<ASFW::Audio::AVC::DiscoveryCoordinator> coordinator_;
    std::shared_ptr<Discovery::FWDevice> device_;
    std::shared_ptr<Discovery::FWUnit> unit_;
    std::shared_ptr<AVCDiscovery> discovery_;
};

TEST(AvcDiscoveryLifecycle, BusResetDuringResumeRescanDoesNotReenterTheDiscoveryLock) {
    // Hardware, 2026-10-01: a Phase 88 reset the bus mid-attach; on resume the
    // failed attach was rescanned, the device reset again with a command
    // pending, and the dext aborted on a recursive os_unfair_lock: OnBusReset
    // held the discovery lock while the unit unwound its rescan synchronously
    // into IsRescanCurrent.
    DiscoveryRig rig;

    // Attach starts, and its first command goes unanswered.
    rig.Sim().SetTimeoutNext();
    rig.Discovery().OnUnitPublished(rig.Unit());
    auto* unit = rig.Discovery().GetAVCUnit(kGuid);
    ASSERT_NE(unit, nullptr);
    EXPECT_EQ(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::Running);

    // The device resets the bus: the attach fails.
    rig.BusReset(2);

    EXPECT_EQ(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::Failed);

    // Back on the bus: the failed attach is rescanned after the delay, and
    // this time the device goes quiet again with the first command pending.
    rig.Resume(2);
    rig.Sim().SetTimeoutNext();
    rig.Timers().Advance(kRescanDelayNs);
    EXPECT_EQ(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::Running);

    // Second reset with the rescan in flight: the rescan fails cleanly.
    rig.BusReset(3);
    EXPECT_EQ(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::Failed);
}

TEST(AvcDiscoveryLifecycle, ResumeRescansAFailedAttachOnlyOnce) {
    // A device that keeps resetting mid-discovery is retried once, not in a loop.
    DiscoveryRig rig;
    rig.Sim().SetTimeoutNext();
    rig.Discovery().OnUnitPublished(rig.Unit());
    rig.BusReset(2);
    rig.Resume(2);
    rig.Sim().SetTimeoutNext();
    rig.Timers().Advance(kRescanDelayNs);
    rig.BusReset(3);

    auto* unit = rig.Discovery().GetAVCUnit(kGuid);
    ASSERT_NE(unit, nullptr);
    ASSERT_EQ(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::Failed);
    rig.Resume(3);
    rig.Timers().Advance(kRescanDelayNs);
    EXPECT_EQ(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::Failed);
    EXPECT_EQ(rig.Timers().PendingCount(), 0U);
}

TEST(AvcDiscoveryLifecycle, AFinishedAttachEndsInAVisiblePublicationState) {
    // Never "waiting" once discovery finished: published, or failed with a reason.
    DiscoveryRig rig;
    rig.Discovery().OnUnitPublished(rig.Unit());
    rig.Settle();
    auto* unit = rig.Discovery().GetAVCUnit(kGuid);
    ASSERT_NE(unit, nullptr);
    ASSERT_NE(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::Running);
    const auto status = rig.Coordinator().Status(kGuid);
    EXPECT_FALSE(std::holds_alternative<ASFW::Audio::AVC::WaitingForDiscovery>(status));
    EXPECT_EQ(rig.Published(), std::holds_alternative<ASFW::Audio::AVC::Ready>(status) ? 1U : 0U);
}

TEST(AvcDiscoveryLifecycle, AManualRefreshNeverRepublishes) {
    DiscoveryRig rig;
    rig.Discovery().OnUnitPublished(rig.Unit());
    rig.Settle();
    const auto state = rig.Coordinator().Status(kGuid);
    ASSERT_EQ(rig.Published(), 1U) << "attach must publish; failed: "
        << (std::holds_alternative<ASFW::Audio::AVC::Failed>(state) ? std::get<ASFW::Audio::AVC::Failed>(state).reason : "-");
    rig.Discovery().ReScanAllUnits();
    rig.Settle();
    EXPECT_EQ(rig.Published(), 1U);
    EXPECT_TRUE(std::holds_alternative<ASFW::Audio::AVC::Ready>(rig.Coordinator().Status(kGuid)));
}

TEST(AvcDiscoveryLifecycle, ARefreshIsRefusedWhileTheDeviceStreams) {
    DiscoveryRig rig;
    rig.Discovery().OnUnitPublished(rig.Unit());
    rig.Settle();
    auto* unit = rig.Discovery().GetAVCUnit(kGuid);
    ASSERT_NE(unit, nullptr);
    const auto session = unit->CopyExchangeLog().session;
    const auto writes = rig.FcpWrites();
    rig.SetAudioActive(true);
    rig.Discovery().ReScanAllUnits();
    rig.Settle();
    EXPECT_EQ(rig.FcpWrites(), writes) << "no diagnostic frame may reach a streaming device";
    EXPECT_EQ(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::BlockedByAudio);
    EXPECT_EQ(unit->CopyExchangeLog().session, session) << "the attach log is kept";
    // Idle again: the refresh runs.
    rig.SetAudioActive(false);
    rig.Discovery().ReScanAllUnits();
    rig.Settle();
    EXPECT_GT(rig.FcpWrites(), writes);
    EXPECT_EQ(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::Completed);
}

TEST(AvcDiscoveryLifecycle, ASecondRefreshWhileOneRunsIsBusyNotASecondSession) {
    DiscoveryRig rig;
    rig.Discovery().OnUnitPublished(rig.Unit());
    rig.Settle();
    auto* unit = rig.Discovery().GetAVCUnit(kGuid);
    ASSERT_NE(unit, nullptr);
    const auto session = unit->CopyExchangeLog().session;
    rig.Sim().SetTimeoutNext(); // Keep the first refresh in flight.
    rig.Discovery().ReScanAllUnits();
    ASSERT_EQ(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::Running);
    rig.Discovery().ReScanAllUnits();
    EXPECT_EQ(unit->CopyExchangeLog().session, session + 1) << "one refresh, one log session";
    rig.Settle();
    EXPECT_EQ(unit->GetDiscoveryStatus(), AVCDiscoveryStatus::Completed);
}

// ---------------------------------------------------------------------------
// 1814 bootloader preparation gates the producer (phase 4.4).
// ---------------------------------------------------------------------------

namespace Boot = ASFW::Protocols::BeBoB::Bootloader;
constexpr uint64_t kGuid1814 = 0x000D6C0000001814ULL;
constexpr uint64_t kSupportedLoaderDate = 0x3230303730343031ULL; // "20070401"

class RecordingListener final : public ASFW::Audio::IAVCAudioConfigListener {
public:
    void OnAVCAudioConfigurationReady(uint64_t, const ASFW::Audio::Model::ASFWAudioDevice&) noexcept override {
        ++published;
    }
    bool IsAudioActive(uint64_t) const noexcept override { return false; }
    unsigned published{0};
};

constexpr uint32_t kFirmwarePersonaModel = 0x00010071; // 1814 after its firmware starts

class BootloaderRig {
public:
    BootloaderRig() {
        bus_.SetLocalNodeID(FW::NodeId{1});
        bus_.SetReadResponder([this](Async::FWAddress address, uint32_t) -> std::optional<std::vector<uint8_t>> {
            if (address.addressHi != Boot::kAddressHi || address.addressLo != Boot::kInfoAddressLo) return std::nullopt;
            std::vector<uint8_t> info(Boot::kInfoBlockBytes, 0);
            const auto store = [&info](size_t at, uint64_t value, size_t bytes) {
                for (size_t i = 0; i < bytes; ++i) info[at + i] = static_cast<uint8_t>(value >> (8U * i));
            };
            store(Boot::kProtocolVersionOffset, 1, 4);
            store(Boot::kBootloaderVersionOffset, loaderActive ? 1 : 0, 4);
            store(Boot::kSoftwareDateOffset, kSupportedLoaderDate, 8);
            return info;
        });
        coordinator_ = std::make_shared<ASFW::Audio::AVC::DiscoveryCoordinator>(routes_, bus_, bus_, &listener_);
        discovery_ = std::make_shared<AVCDiscovery>(nullptr, routes_, manager_, bus_, bus_, timers_, coordinator_);
    }
    ~BootloaderRig() { discovery_->Shutdown(); }

    /// The device appears (or reappears) on `generation` with `model`, as the
    /// ROM scan reports it: device first, then its AV/C unit.
    void Appear(uint32_t generation, uint32_t model) {
        if (device_) { routes_.InvalidateLiveMappingsForBusReset(); discovery_->OnBusReset(generation); }
        bus_.SetGeneration(FW::Generation{generation});
        const auto record = routes_.UpsertFromROM(Rom(generation, model), Discovery::LinkPolicy{});
        device_ = Discovery::FWDevice::Create(record, Rom(generation, model));
        unit_ = Discovery::FWUnit::Create(device_, 9, {{Discovery::CfgKey::Unit_Spec_Id, kTa1394Specifier},
                                                       {Discovery::CfgKey::Unit_Sw_Version, model}});
        unit_->Publish();
        discovery_->OnDeviceAdded(device_);
        discovery_->OnUnitPublished(unit_);
    }

    [[nodiscard]] size_t Count(OpKind kind, uint32_t addressLo) const {
        return static_cast<size_t>(std::ranges::count_if(bus_.Operations(), [&](const RecordedOp& op) {
            return op.kind == kind && op.addressLo == addressLo;
        }));
    }
    [[nodiscard]] size_t InfoReads() const { return Count(OpKind::Read, Boot::kInfoAddressLo); }
    [[nodiscard]] size_t Cues() const { return Count(OpKind::Write, Boot::kRequestAddressLo); }
    [[nodiscard]] size_t FcpWrites() const { return Count(OpKind::Write, 0xF0000B00U); }
    [[nodiscard]] ASFW::Audio::AVC::PublicationState Status() const { return coordinator_->Status(kGuid1814); }

    bool loaderActive{true};
    RecordingListener listener_;
    RecordingFireWireBus bus_;
    FakeTimerScheduler timers_;
    Discovery::DeviceRegistry routes_;
    FakeDeviceManager manager_;
    std::shared_ptr<Discovery::FWDevice> device_;
    std::shared_ptr<Discovery::FWUnit> unit_;
    std::shared_ptr<ASFW::Audio::AVC::DiscoveryCoordinator> coordinator_;
    std::shared_ptr<AVCDiscovery> discovery_;

private:
    static Discovery::ConfigROM Rom(uint32_t generation, uint32_t model) {
        Discovery::ConfigROM rom{};
        rom.bib.guid = kGuid1814;
        rom.gen = FW::Generation{generation};
        rom.nodeId = kDeviceNode;
        rom.rootDirMinimal = {{Discovery::CfgKey::VendorId, Boot::kMAudioVendorId},
                              {Discovery::CfgKey::ModelId, model}};
        Discovery::UnitDirectory unit{};
        unit.offsetQuadlets = 9;
        unit.unitSpecId = kTa1394Specifier;
        unit.unitSwVersion = model;
        rom.unitDirectories = {unit};
        return rom;
    }
};

template <class State> bool Is(const ASFW::Audio::AVC::PublicationState& state) {
    return std::holds_alternative<State>(state);
}

TEST(AvcBootloaderPreparation, ColdBootCuesOnceAndTheFirmwarePersonaGetsTheOnlyProducer) {
    BootloaderRig rig;
    rig.Appear(1, Boot::kFireWire1814BootloaderModelId);
    // Loader active: one info read, one cue, and nothing else. No AV/C unit
    // (no transport producer), no FCP frame, no publication, no failure.
    EXPECT_EQ(rig.InfoReads(), 1U);
    EXPECT_EQ(rig.Cues(), 1U);
    EXPECT_EQ(rig.discovery_->GetAVCUnit(kGuid1814), nullptr);
    EXPECT_EQ(rig.FcpWrites(), 0U);
    EXPECT_EQ(rig.listener_.published, 0U);
    EXPECT_TRUE(Is<ASFW::Audio::AVC::WaitingForDiscovery>(rig.Status()));

    // The firmware starts and the device returns as its firmware persona.
    rig.Appear(2, kFirmwarePersonaModel);
    EXPECT_EQ(rig.InfoReads(), 1U);
    EXPECT_EQ(rig.Cues(), 1U);
    EXPECT_NE(rig.discovery_->GetAVCUnit(kGuid1814), nullptr);
    EXPECT_EQ(rig.FcpWrites(), 0U); // Profile-owned: discovery sends no AV/C.
    EXPECT_EQ(rig.listener_.published, 1U);
    EXPECT_TRUE(Is<ASFW::Audio::AVC::Ready>(rig.Status()));
}

TEST(AvcBootloaderPreparation, LoaderStillActiveOnANewRouteIsReadAgainNeverCuedAgainAndFailsVisibly) {
    BootloaderRig rig;
    rig.Appear(1, Boot::kFireWire1814BootloaderModelId);
    rig.Appear(2, Boot::kFireWire1814BootloaderModelId); // The cue did not take.
    EXPECT_EQ(rig.InfoReads(), 2U);
    EXPECT_EQ(rig.Cues(), 1U);
    EXPECT_EQ(rig.discovery_->GetAVCUnit(kGuid1814), nullptr);
    EXPECT_EQ(rig.listener_.published, 0U);
    EXPECT_TRUE(Is<ASFW::Audio::AVC::Failed>(rig.Status()));
}

TEST(AvcBootloaderPreparation, ConfirmedFirmwareIsReusedOnANewRouteOfTheSameIncarnation) {
    BootloaderRig rig;
    rig.loaderActive = false;
    rig.Appear(1, Boot::kFireWire1814BootloaderModelId);
    rig.Appear(2, Boot::kFireWire1814BootloaderModelId);
    EXPECT_EQ(rig.InfoReads(), 1U);
    EXPECT_EQ(rig.Cues(), 0U);
    EXPECT_FALSE(Is<ASFW::Audio::AVC::Failed>(rig.Status()));
    // The loader persona itself is never an AV/C producer.
    EXPECT_EQ(rig.discovery_->GetAVCUnit(kGuid1814), nullptr);
    EXPECT_EQ(rig.FcpWrites(), 0U);
}

} // namespace
