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

#include "AvcDeviceImages.inc"
#include "FakeTimerScheduler.hpp"
#include "RecordingFireWireBus.hpp"
#include "SimulatedAvcUnit.hpp"

#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Discovery/FWDevice.hpp"
#include "ASFWDriver/Discovery/FWUnit.hpp"
#include "ASFWDriver/Discovery/IDeviceManager.hpp"
#include "ASFWDriver/Protocols/AVC/AVCDiscovery.hpp"

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
        discovery_ = std::make_shared<AVCDiscovery>(nullptr, routes_, manager_, bus_, bus_, timers_, nullptr);
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

    RecordingFireWireBus bus_;
    FakeTimerScheduler timers_;
    Discovery::DeviceRegistry routes_;
    FakeDeviceManager manager_;
    SimulatedAvcUnit sim_;
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

} // namespace
