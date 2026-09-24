// SBP2TargetBridgeTests — the HBA ↔ SBP-2 bridge driven end to end on the host:
// real DeviceManager + SessionRegistry + CommandExecutor + readiness gate over a
// DeferredFireWireBus, with the HBA side replaced by the hub observer and task
// callbacks. Invariant under test: every task the HBA hands the bridge completes
// exactly once, whatever the device does (answer, go silent, vanish).

#include <gtest/gtest.h>

#include "ASFWDriver/Discovery/DeviceManager.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2WireFormats.hpp"
#include "ASFWDriver/Protocols/SBP2/SCSICommandSet.hpp"
#include "ASFWDriver/Protocols/SBP2/Session/SessionRegistry.hpp"
#include "ASFWDriver/SCSIController/SBP2BridgeHub.hpp"
#include "ASFWDriver/SCSIController/SBP2TargetBridge.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"
#include "tests/mocks/DeferredFireWireBus.hpp"
#include "FakeSessionScheduler.hpp"
#include "FakeTimerScheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace {

using ASFW::Discovery::CfgKey;
using ASFW::Discovery::ConfigROM;
using ASFW::Discovery::DeviceKind;
using ASFW::Discovery::DeviceManager;
using ASFW::Discovery::DeviceRecord;
using ASFW::Discovery::DeviceRegistry;
using ASFW::Discovery::Generation;
using ASFW::Discovery::LifeState;
using ASFW::Discovery::LinkPolicy;
using ASFW::Discovery::RomEntry;
using ASFW::Protocols::SBP2::AddressSpaceManager;
using ASFW::Protocols::SBP2::SBP2BridgeHub;
using ASFW::Protocols::SBP2::SBP2TargetBridge;
using ASFW::Protocols::SBP2::SessionRegistry;
using ASFW::Protocols::SBP2::Wire::LoginORB;
using ASFW::Protocols::SBP2::Wire::LoginResponse;
using ASFW::Protocols::SBP2::Wire::StatusBlock;
using ASFW::Testing::FakeSessionScheduler;
using ASFW::Testing::FakeTimerScheduler;
namespace SCSI = ASFW::Protocols::SBP2::SCSI;
namespace SBPStatus = ASFW::Protocols::SBP2::Wire::SBPStatus;

constexpr uint64_t kGuid = 0x00206B4004000000ULL;

uint64_t DecodeAddressFromWritePayload(std::span<const uint8_t> payload) {
    const uint16_t hi = static_cast<uint16_t>((payload[2] << 8) | payload[3]);
    const uint32_t lo = (static_cast<uint32_t>(payload[4]) << 24) |
                        (static_cast<uint32_t>(payload[5]) << 16) |
                        (static_cast<uint32_t>(payload[6]) << 8) | payload[7];
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

uint64_t ReadORBAddress(AddressSpaceManager& manager, uint64_t orb, size_t hiOff, size_t loOff) {
    uint32_t hi = 0;
    uint32_t lo = 0;
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete, manager.ReadQuadlet(orb + hiOff, &hi));
    EXPECT_EQ(ASFW::Async::ResponseCode::Complete, manager.ReadQuadlet(orb + loOff, &lo));
    return (static_cast<uint64_t>(OSSwapBigToHostInt32(hi) & 0xFFFFu) << 32) |
           OSSwapBigToHostInt32(lo);
}

class BridgeRig {
public:
    BridgeRig()
        : registry(std::make_shared<SessionRegistry>(bus, bus, addressManager, deviceRegistry,
                                                     deviceManager, scheduler, &queue)) {
        queue.SetManualDispatchForTesting(true);
        ASFW::Testing::SetHostMonotonicClockForTesting([this]() { return nowNs; });
        bus.SetGeneration(ASFW::FW::Generation{1});
        bus.SetLocalNodeID(ASFW::FW::NodeId{0x2A});
        bus.SetDefaultSpeed(ASFW::FW::FwSpeed::S400);

        SBP2BridgeHub::SetTargetObserver(
            [this](uint64_t guid, bool up) { edges.emplace_back(guid, up); });
        bridge = std::make_shared<SBP2TargetBridge>(registry, deviceManager, &queue, &timers);
        bridge->Start();
        SBP2BridgeHub::Set(bridge);
    }

    ~BridgeRig() {
        SBP2BridgeHub::Clear();
        bridge->Shutdown();
        SBP2BridgeHub::ClearTargetObserver();
        ASFW::Testing::ResetHostMonotonicClockForTesting();
    }

    void UpsertDevice(Generation generation, uint8_t nodeId) {
        DeviceRecord record{};
        record.guid = kGuid;
        record.vendorId = 0x00206B;
        record.modelId = 0x000001;
        record.kind = DeviceKind::Unknown;
        record.vendorName = "MINOLTA";
        record.modelName = "Scanner";
        record.gen = generation;
        record.nodeId = nodeId;
        record.link = LinkPolicy{};
        record.state = LifeState::Ready;

        ConfigROM rom{};
        rom.bib.guid = kGuid;
        rom.gen = generation;
        rom.nodeId = nodeId;
        rom.vendorName = record.vendorName;
        rom.modelName = record.modelName;
        rom.rootDirMinimal = {
            RomEntry{CfgKey::Unit_Spec_Id, ASFW::Protocols::SBP2::kSBP2UnitSpecId, 0, 0},
            RomEntry{CfgKey::Unit_Sw_Version, ASFW::Protocols::SBP2::kSBP2UnitSwVersion, 0, 0},
            RomEntry{CfgKey::Logical_Unit_Number, 0x000000, 0, 0},
            RomEntry{CfgKey::Management_Agent_Offset, 0x000080, 1, 0},
            RomEntry{CfgKey::Unit_Characteristics, 0x080400, 0, 0},
        };
        (void)deviceRegistry.UpsertFromROM(rom, record.link);
        ASSERT_NE(nullptr, deviceManager.UpsertDevice(record, rom));
    }

    void Drain() {
        while (queue.DrainReadyForTesting() > 0U) {
        }
    }

    void AdvanceMs(uint64_t ms) {
        nowNs += ms * 1'000'000ULL;
        scheduler.Advance(ms * 1'000'000ULL);
        timers.Advance(ms * 1'000'000ULL);
        Drain();
    }

    // Answer the pending login write + login response + status: session LoggedIn.
    void CompleteLogin() {
        ASSERT_EQ(1u, bus.PendingWriteCount());
        const auto& write = bus.WriteAt(bus.WriteCount() - 1);
        const uint64_t orb = DecodeAddressFromWritePayload(write.data);
        const uint64_t responseAddress =
            ReadORBAddress(addressManager, orb, offsetof(LoginORB, loginResponseAddressHi),
                           offsetof(LoginORB, loginResponseAddressLo));
        statusAddress = ReadORBAddress(addressManager, orb, offsetof(LoginORB, statusFIFOAddressHi),
                                       offsetof(LoginORB, statusFIFOAddressLo));

        LoginResponse response{};
        response.length = OSSwapHostToBigInt16(LoginResponse::kSize);
        response.loginID = OSSwapHostToBigInt16(0x0042);
        response.commandBlockAgentAddressHi = OSSwapHostToBigInt32(0x0000'FFFFu);
        response.commandBlockAgentAddressLo = OSSwapHostToBigInt32(0x0020'0000u);
        response.reconnectHold = OSSwapHostToBigInt16(1);

        ASSERT_TRUE(bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
        addressManager.ApplyRemoteWrite(
            responseAddress,
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&response), sizeof(response)});
        PostStatus(0);
        Drain();
    }

    // Ack every pending write (post-login BUSY_TIMEOUT, then the command's
    // ORB_POINTER); return the ORB address from the 8-byte ORB_POINTER write.
    uint64_t AckCommandFetch() {
        uint64_t orb = 0;
        while (bus.PendingWriteCount() > 0) {
            const auto& write = bus.PendingWriteAt(0);
            if (write.data.size() == 8) {
                orb = DecodeAddressFromWritePayload(write.data);
            }
            EXPECT_TRUE(bus.CompleteNextWrite(ASFW::Async::AsyncStatus::kSuccess));
            Drain();
        }
        EXPECT_NE(0u, orb);
        return orb;
    }

    void PostStatus(uint64_t orb) {
        StatusBlock status{};
        status.details = 0;
        status.sbpStatus = SBPStatus::kNoAdditionalInfo;
        status.orbOffsetHi = OSSwapHostToBigInt16(static_cast<uint16_t>((orb >> 32) & 0xFFFFu));
        status.orbOffsetLo = OSSwapHostToBigInt32(static_cast<uint32_t>(orb & 0xFFFF'FFFFu));
        addressManager.ApplyRemoteWrite(
            statusAddress,
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(&status), sizeof(status)});
        Drain();
    }

    // Plug in, log in, pass the readiness probe: target announced to the HBA.
    void BringUp() {
        UpsertDevice(Generation{1}, 0x00);
        Drain();
        CompleteLogin();
        PostStatus(AckCommandFetch()); // readiness TUR → GOOD
        ASSERT_EQ(1u, edges.size());
        ASSERT_TRUE(edges.back().second);
        ASSERT_TRUE(bridge->IsReady());
    }

    // Hot-unplug as the core sees it: bus reset, then the device is missing
    // from the next two discovery passes (suspended, then terminated).
    void Unplug() {
        bus.SetGeneration(ASFW::FW::Generation{2});
        registry->OnBusReset(2);
        deviceManager.MarkDeviceLost(kGuid);
        registry->RefreshTargets(Generation{2});
        deviceManager.MarkDeviceLost(kGuid);
        registry->RefreshTargets(Generation{2});
        Drain();
    }

    ASFW::Async::Testing::DeferredFireWireBus bus;
    FakeSessionScheduler scheduler;
    FakeTimerScheduler timers;
    AddressSpaceManager addressManager{nullptr};
    DeviceRegistry deviceRegistry;
    DeviceManager deviceManager;
    IODispatchQueue queue;
    std::shared_ptr<SessionRegistry> registry;
    std::shared_ptr<SBP2TargetBridge> bridge;
    std::vector<std::pair<uint64_t, bool>> edges;
    uint64_t nowNs{0};
    uint64_t statusAddress{0};
};

// Counts completions of one HBA task.
struct TaskProbe {
    int completions{0};
    SCSI::CommandResult last{};
    SBP2TargetBridge::TaskCallback Callback() {
        return [this](const SCSI::CommandResult& r) {
            ++completions;
            last = r;
        };
    }
};

TEST(SBP2TargetBridgeTests, BringUpAnnouncesTargetAfterReadinessProbe) {
    BridgeRig rig;
    rig.BringUp();
    EXPECT_EQ(kGuid, rig.edges.back().first);
}

TEST(SBP2TargetBridgeTests, InFlightTaskCompletesOnceWhenDeviceIsUnplugged) {
    BridgeRig rig;
    rig.BringUp();

    TaskProbe inquiry;
    rig.bridge->SubmitTask(SCSI::BuildInquiryRequest(36), inquiry.Callback());
    rig.Drain();
    (void)rig.AckCommandFetch(); // ORB fetched; the device never answers

    rig.Unplug();
    rig.AdvanceMs(120'000);

    EXPECT_EQ(1, inquiry.completions);
    EXPECT_NE(0, inquiry.last.transportStatus);
}

TEST(SBP2TargetBridgeTests, QueuedTasksCompleteOnceWhenDeviceIsUnplugged) {
    BridgeRig rig;
    rig.BringUp();

    TaskProbe first;
    TaskProbe second;
    rig.bridge->SubmitTask(SCSI::BuildInquiryRequest(36), first.Callback());
    rig.bridge->SubmitTask(SCSI::BuildTestUnitReadyRequest(), second.Callback());
    rig.Drain();
    (void)rig.AckCommandFetch();

    rig.Unplug();
    rig.AdvanceMs(120'000);

    EXPECT_EQ(1, first.completions);
    EXPECT_EQ(1, second.completions);
}

TEST(SBP2TargetBridgeTests, SilentDeviceTaskCompletesOnceWithinItsTimeout) {
    BridgeRig rig;
    rig.BringUp();

    TaskProbe inquiry;
    auto request = SCSI::BuildInquiryRequest(36);
    request.timeoutMs = 10'000;
    rig.bridge->SubmitTask(std::move(request), inquiry.Callback());
    rig.Drain();
    (void)rig.AckCommandFetch(); // fetched, never answered

    // ORB timeout + LUN-reset management timeout, with margin.
    rig.AdvanceMs(20'000);
    EXPECT_EQ(1, inquiry.completions);
    rig.AdvanceMs(120'000);
    EXPECT_EQ(1, inquiry.completions);
}

// The ORB timer arms only once the ORB_POINTER write completes. If that write
// never completes (lost ack, dead AT path), the task must still complete within
// its own timeout — the SAM create-probe INQUIRY blocks target creation
// uninterruptibly until it does (issue #139).
TEST(SBP2TargetBridgeTests, TaskCompletesOnceWhenFetchWriteNeverCompletes) {
    BridgeRig rig;
    rig.BringUp();

    TaskProbe inquiry;
    auto request = SCSI::BuildInquiryRequest(36);
    request.timeoutMs = 10'000;
    rig.bridge->SubmitTask(std::move(request), inquiry.Callback());
    rig.Drain(); // ORB_POINTER write issued, left pending forever

    rig.AdvanceMs(20'000);
    EXPECT_EQ(1, inquiry.completions);
    rig.AdvanceMs(120'000);
    EXPECT_EQ(1, inquiry.completions);
}

TEST(SBP2TargetBridgeTests, ShutdownCompletesInFlightAndQueuedTasksOnce) {
    BridgeRig rig;
    rig.BringUp();

    TaskProbe first;
    TaskProbe second;
    rig.bridge->SubmitTask(SCSI::BuildInquiryRequest(36), first.Callback());
    rig.bridge->SubmitTask(SCSI::BuildTestUnitReadyRequest(), second.Callback());
    rig.Drain();
    (void)rig.AckCommandFetch();

    rig.bridge->Shutdown();
    rig.AdvanceMs(120'000);

    EXPECT_EQ(1, first.completions);
    EXPECT_EQ(1, second.completions);
    ASSERT_FALSE(rig.edges.empty());
    EXPECT_FALSE(rig.edges.back().second); // terminal down edge for the HBA
}

// Quiesce (sleep, driver stop) shuts the bridge down, which issues a logout
// the quiesced bus never answers. Destroying the registry then cancels that
// write through the bus it borrowed — so ServiceContext::Reset must destroy the
// registry before the controller (and its bus).
TEST(SBP2TargetBridgeTests, RegistryTeardownCancelsPendingLogoutThroughTheBus) {
    BridgeRig rig;
    rig.BringUp();
    rig.bridge->Shutdown();
    ASSERT_EQ(1u, rig.bus.PendingWriteCount()); // logout ORB write, unanswered

    rig.registry.reset();

    EXPECT_EQ(0u, rig.bus.PendingWriteCount());
}

} // namespace
