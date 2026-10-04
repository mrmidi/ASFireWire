#include <gtest/gtest.h>

#include "ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "ASFWDriver/Discovery/DeviceRegistry.hpp"
#include "ASFWDriver/Protocols/AVC/CMP/CMPClient.hpp"

#include <array>
#include <cstring>
#include <deque>
#include <unordered_map>

namespace {

using ASFW::Async::AsyncHandle;
using ASFW::Async::AsyncStatus;
using ASFW::Async::FWAddress;
using ASFW::Async::IFireWireBus;
using ASFW::Async::InterfaceCompletionCallback;
using ASFW::CMP::CMPClient;
using ASFW::CMP::CMPDevice;
using ASFW::CMP::CMPStatus;
using ASFW::FW::FwSpeed;
using ASFW::FW::Generation;
using ASFW::FW::NodeId;

namespace PCRRegisters = ASFW::CMP::PCRRegisters;

class CMPBus final : public IFireWireBus {
public:
    ASFW::Discovery::DeviceRegistry routes;
    AsyncHandle ReadBlock(Generation, NodeId node, FWAddress address, uint32_t,
                          FwSpeed, InterfaceCompletionCallback callback) override {
        if (failNextRead) {
            failNextRead = false;
            callback(AsyncStatus::kTimeout, {});
            return NextHandle();
        }
        const bool isMpr = address.addressLo == PCRRegisters::kOMPR ||
                           address.addressLo == PCRRegisters::kIMPR;
        const uint32_t value = isMpr ? mprValue : pcrByNode_[node.value];
        const uint32_t wire = OSSwapHostToBigInt32(value);
        std::array<uint8_t, 4> payload{};
        std::memcpy(payload.data(), &wire, sizeof(wire));
        callback(AsyncStatus::kSuccess, payload);
        return NextHandle();
    }

    AsyncHandle WriteBlock(Generation, NodeId, FWAddress, std::span<const uint8_t>,
                           FwSpeed, InterfaceCompletionCallback callback) override {
        callback(AsyncStatus::kSuccess, {});
        return NextHandle();
    }

    AsyncHandle Lock(Generation, NodeId node, FWAddress address, ASFW::FW::LockOp op,
                     std::span<const uint8_t> operand, uint32_t responseLength, FwSpeed,
                     InterfaceCompletionCallback callback) override {
        ++lockCount;
        EXPECT_EQ(op, ASFW::FW::LockOp::kCompareSwap);
        EXPECT_EQ(operand.size(), 8U);
        EXPECT_EQ(responseLength, 4U);
        EXPECT_EQ(address.addressHi, 0xFFFFU);
        if (operand.size() != 8) { callback(AsyncStatus::kHardwareError, {}); return NextHandle(); }
        Fault fault{};
        if (!faults.empty()) { fault = faults.front(); faults.pop_front(); }
        uint32_t expectedWire = 0;
        uint32_t desiredWire = 0;
        std::memcpy(&expectedWire, operand.data(), sizeof(expectedWire));
        std::memcpy(&desiredWire, operand.data() + sizeof(expectedWire), sizeof(desiredWire));
        const uint32_t expected = OSSwapBigToHostInt32(expectedWire);
        const uint32_t desired = OSSwapBigToHostInt32(desiredWire);
        uint32_t observed = pcrByNode_[node.value];
        if (conflictOnce) {
            conflictOnce = false;
            observed ^= 0x00000001U;
        } else if (observed == expected && fault.apply) {
            pcrByNode_[node.value] = desired;
        }
        const uint32_t observedWire = OSSwapHostToBigInt32(observed);
        std::array<uint8_t, 4> payload{};
        std::memcpy(payload.data(), &observedWire, sizeof(observedWire));
        if (fault.failRead) failNextRead = true;
        if (fault.reset) routes.InvalidateLiveMappingsForBusReset();
        if (deferLock) {
            deferLock = false;
            pending = std::move(callback);
            pendingPayload = payload;
            return NextHandle();
        }
        callback(fault.status, fault.status == AsyncStatus::kSuccess ? std::span<const uint8_t>{payload}.first(fault.responseBytes) : std::span<const uint8_t>{});
        return NextHandle();
    }

    bool Cancel(AsyncHandle) override { return false; }
    FwSpeed GetSpeed(NodeId) const override { return routeSpeed; }
    uint32_t HopCount(NodeId, NodeId) const override { return 0; }
    uint8_t GetGapCount() const override { return gapCount; }
    Generation GetGeneration() const override { return Generation{1}; }
    NodeId GetLocalNodeID() const override { return NodeId{0}; }

    uint32_t mprValue{0x80000001U}; // S400, one plug
    FwSpeed routeSpeed{FwSpeed::S400};
    uint8_t gapCount{63};
    std::unordered_map<uint8_t, uint32_t> pcrByNode_{{2, 0x80000000U}, {3, 0x80000000U}};
    struct Fault {
        AsyncStatus status{AsyncStatus::kSuccess};
        bool apply{true};
        bool failRead{false};
        bool reset{false};
        size_t responseBytes{4};
    };
    std::deque<Fault> faults;
    bool failNextRead{false};
    bool deferLock{false};
    InterfaceCompletionCallback pending;
    std::array<uint8_t, 4> pendingPayload{};
    bool conflictOnce{false};
    uint32_t lockCount{0};

private:
    AsyncHandle NextHandle() { return AsyncHandle{++nextHandle_}; }
    uint32_t nextHandle_{0};
};

CMPDevice Device(ASFW::Discovery::DeviceRegistry& routes, uint64_t guid, uint8_t node,
                 uint32_t generation = 1) {
    ASFW::Discovery::ConfigROM rom{};
    rom.bib.guid = guid;
    rom.gen = Generation{generation};
    rom.nodeId = node;
    (void)routes.UpsertFromROM(rom, ASFW::Discovery::LinkPolicy{});
    return CMPDevice{.route = *routes.CurrentRoute(guid)};
}

TEST(CMPConnectionTests, LeasesAreIndependentForDifferentDeviceGuids) {
    CMPBus bus;
    CMPClient cmp(bus, bus, bus.routes);
    CMPStatus first = CMPStatus::Failed;
    CMPStatus second = CMPStatus::Failed;

    cmp.ConnectOPCR(Device(bus.routes, 0xA, 2), 0, 5, [&first](CMPStatus status) { first = status; });
    cmp.ConnectOPCR(Device(bus.routes, 0xB, 3), 0, 7, [&second](CMPStatus status) { second = status; });

    EXPECT_EQ(first, CMPStatus::Success);
    EXPECT_EQ(second, CMPStatus::Success);
    EXPECT_EQ(bus.lockCount, 2U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81058000U);
    EXPECT_EQ(bus.pcrByNode_[3], 0x81078000U);
}

TEST(CMPConnectionTests, DisconnectWithoutLocalLeaseDoesNotDecrementRemoteP2P) {
    CMPBus bus;
    bus.pcrByNode_[2] = 0x81058000U;
    CMPClient cmp(bus, bus, bus.routes);
    CMPStatus status = CMPStatus::Failed;

    cmp.DisconnectOPCR(Device(bus.routes, 0xA, 2), 0, [&status](CMPStatus result) { status = result; });

    EXPECT_EQ(status, CMPStatus::Success);
    EXPECT_EQ(bus.lockCount, 0U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81058000U);
}

TEST(CMPConnectionTests, RetriesCompareSwapContentionWithFreshPCRRead) {
    CMPBus bus;
    bus.conflictOnce = true;
    CMPClient cmp(bus, bus, bus.routes);
    CMPStatus status = CMPStatus::Failed;

    cmp.ConnectOPCR(Device(bus.routes, 0xA, 2), 0, 5, [&status](CMPStatus result) { status = result; });

    EXPECT_EQ(status, CMPStatus::Success);
    EXPECT_EQ(bus.lockCount, 2U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81058000U);
}

TEST(CMPConnectionTests, ProgramsOutputPCRSpeedAndOverheadFromLiveGapCount) {
    CMPBus bus;
    bus.routeSpeed = FwSpeed::S200;
    bus.gapCount = 8;
    CMPClient cmp(bus, bus, bus.routes);
    CMPStatus status = CMPStatus::Failed;

    cmp.ConnectOPCR(Device(bus.routes, 0xA, 2), 0, 5, [&status](CMPStatus result) { status = result; });

    // gap 8 derives 166 bandwidth units; oPCR overhead encoding selects ID 6.
    EXPECT_EQ(status, CMPStatus::Success);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81055800U);
}

TEST(CMPConnectionTests, ProgramsInputPCROnlyWithConnectionAndChannelFields) {
    CMPBus bus;
    bus.routeSpeed = FwSpeed::S200;
    bus.gapCount = 8;
    CMPClient cmp(bus, bus, bus.routes);
    CMPStatus status = CMPStatus::Failed;

    cmp.ConnectIPCR(Device(bus.routes, 0xA, 2), 0, 5, [&status](CMPStatus result) { status = result; });

    // iPCR has no data-rate or overhead-ID fields; those bits must remain zero.
    EXPECT_EQ(status, CMPStatus::Success);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81050000U);
}

TEST(CMPConnectionTests, RejectsBroadcastPCRWithoutCompareSwap) {
    CMPBus bus;
    bus.pcrByNode_[2] = 0xC0000000U; // online + foreign broadcast connection
    CMPClient cmp(bus, bus, bus.routes);
    CMPStatus status = CMPStatus::Success;

    cmp.ConnectOPCR(Device(bus.routes, 0xA, 2), 0, 5, [&status](CMPStatus result) { status = result; });

    EXPECT_EQ(status, CMPStatus::NoResources);
    EXPECT_EQ(bus.lockCount, 0U);
    EXPECT_EQ(bus.pcrByNode_[2], 0xC0000000U);
}

TEST(CMPConnectionTests, RejectsPlugOutsideMasterPlugRegisterCount) {
    CMPBus bus;
    bus.mprValue = 0x80000000U; // S400, no plugs
    CMPClient cmp(bus, bus, bus.routes);
    CMPStatus status = CMPStatus::Success;

    cmp.ConnectOPCR(Device(bus.routes, 0xA, 2), 0, 5, [&status](CMPStatus result) { status = result; });

    EXPECT_EQ(status, CMPStatus::NotFound);
    EXPECT_EQ(bus.lockCount, 0U);
}

TEST(CMPConnectionTests, NewGenerationDropsOldLeaseBeforeReconnect) {
    CMPBus bus;
    CMPClient cmp(bus, bus, bus.routes);
    CMPStatus first = CMPStatus::Failed;
    cmp.ConnectOPCR(Device(bus.routes, 0xA, 2, 1), 0, 5, [&first](CMPStatus status) { first = status; });
    ASSERT_EQ(first, CMPStatus::Success);

    // A reset restores remote PCR state. The new routing epoch must not be
    // blocked by the lease that represented the previous generation.
    bus.pcrByNode_[3] = 0x80000000U;
    CMPStatus second = CMPStatus::Failed;
    cmp.ConnectOPCR(Device(bus.routes, 0xA, 3, 2), 0, 7, [&second](CMPStatus status) { second = status; });

    EXPECT_EQ(second, CMPStatus::Success);
    EXPECT_EQ(bus.lockCount, 2U);
    EXPECT_EQ(bus.pcrByNode_[3], 0x81078000U);
}

TEST(CMPConnectionTests, RejectsRouteInvalidatedBeforeCMPAdmission) {
    CMPBus bus;
    CMPClient cmp(bus, bus, bus.routes);
    const CMPDevice stale = Device(bus.routes, 0xA, 2);
    bus.routes.InvalidateLiveMappingsForBusReset();

    CMPStatus status = CMPStatus::Success;
    cmp.ConnectOPCR(stale, 0, 5, [&status](CMPStatus result) { status = result; });

    EXPECT_EQ(status, CMPStatus::Failed);
    EXPECT_EQ(bus.lockCount, 0U);
}


TEST(CMPConnectionTests, AppliedConnectWithLostResponseReconcilesWithoutDoubleIncrement) {
    CMPBus bus;
    bus.faults.push_back({AsyncStatus::kTimeout, true});
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    int callbacks = 0;
    cmp.ConnectIPCR(device, 0, 5, [&](CMPStatus status) {
        ++callbacks;
        EXPECT_EQ(status, CMPStatus::Success);
    });
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(bus.lockCount, 1U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81050000U);
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.pcrByNode_[2], 0x80050000U);
}

TEST(CMPConnectionTests, UnappliedTimeoutRetriesWithinBound) {
    CMPBus bus;
    bus.faults.push_back({AsyncStatus::kTimeout, false});
    CMPClient cmp(bus, bus, bus.routes);
    cmp.ConnectIPCR(Device(bus.routes, 0xA, 2), 0, 5,
        [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.lockCount, 2U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81050000U);
}

TEST(CMPConnectionTests, PersistentTimeoutIsBoundedAndCleanupDoesNotInventConnection) {
    CMPBus bus;
    for (int i = 0; i < 3; ++i) bus.faults.push_back({AsyncStatus::kTimeout, false});
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    int callbacks = 0;
    cmp.ConnectIPCR(device, 0, 5, [&](CMPStatus status) {
        ++callbacks;
        EXPECT_EQ(status, CMPStatus::Timeout);
    });
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(bus.lockCount, 3U);
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.lockCount, 3U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x80000000U);
}

TEST(CMPConnectionTests, FailedReadbackRetainsIntentForCleanup) {
    CMPBus bus;
    bus.faults.push_back({AsyncStatus::kTimeout, true, true});
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    cmp.ConnectIPCR(device, 0, 5, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Timeout); });
    // A second admission must not erase the unresolved transition.
    cmp.ConnectIPCR(device, 0, 5, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Failed); });
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.lockCount, 2U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x80050000U);
}

TEST(CMPConnectionTests, UncertainCleanupDoesNotTouchUnrelatedConnection) {
    CMPBus bus;
    bus.faults.push_back({AsyncStatus::kTimeout, true, true});
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    cmp.ConnectIPCR(device, 0, 5, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Timeout); });
    bus.pcrByNode_[2] = 0x81070000U;
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Failed); });
    EXPECT_EQ(bus.lockCount, 1U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81070000U);
}

TEST(CMPConnectionTests, AppliedDisconnectWithLostResponseDoesNotDecrementTwice) {
    CMPBus bus;
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    cmp.ConnectIPCR(device, 0, 5, [](CMPStatus status) { ASSERT_EQ(status, CMPStatus::Success); });
    bus.faults.push_back({AsyncStatus::kTimeout, true});
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.lockCount, 2U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x80050000U);
}

TEST(CMPConnectionTests, ResetDuringCASNeverAdoptsOldRoute) {
    CMPBus bus;
    bus.faults.push_back({AsyncStatus::kTimeout, true, false, true});
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    cmp.ConnectIPCR(device, 0, 5,
        [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::GenerationMismatch); });
    cmp.DisconnectIPCR(device, 0,
        [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::GenerationMismatch); });
    EXPECT_EQ(bus.lockCount, 1U);
}

TEST(CMPConnectionTests, SharedQuadletCASRejectsMalformedResponse) {
    CMPBus bus;
    bus.faults.push_back({AsyncStatus::kSuccess, true, false, false, 2});
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    cmp.ConnectIPCR(device, 0, 5, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Failed); });
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.pcrByNode_[2], 0x80050000U);
}


TEST(CMPConnectionTests, SharedCASUsesBigEndianCompareThenReplacementAndDecodesOldValue) {
    CMPBus bus;
    bus.pcrByNode_[2] = 0x00112233U;
    int callbacks = 0;
    bus.CompareSwapQuad(Generation{1}, NodeId{2},
        FWAddress{FWAddress::AddressParts{0xFFFF, 0xF0000984}},
        0x00112233U, 0x44556677U, FwSpeed::S400,
        [&](AsyncStatus status, uint32_t oldValue) {
            ++callbacks;
            EXPECT_EQ(status, AsyncStatus::kSuccess);
            EXPECT_EQ(oldValue, 0x00112233U);
        });
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(bus.pcrByNode_[2], 0x44556677U);
}

TEST(CMPConnectionTests, AppliedOutputCASTimeoutPreservesSpeedOverheadAndPayload) {
    CMPBus bus;
    bus.pcrByNode_[2] = 0x8000002CU;
    bus.faults.push_back({AsyncStatus::kTimeout, true});
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    cmp.ConnectOPCR(device, 0, 5, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.pcrByNode_[2], 0x8105802CU);
    cmp.DisconnectOPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.pcrByNode_[2], 0x8005802CU);
}

TEST(CMPConnectionTests, InvalidDisconnectDoesNotClaimSuccessOrModifyPCR) {
    CMPBus bus;
    CMPClient cmp(bus, bus, bus.routes);
    cmp.DisconnectIPCR(Device(bus.routes, 0xA, 2), 31,
        [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Failed); });
    EXPECT_EQ(bus.lockCount, 0U);
}


TEST(CMPConnectionTests, PendingConnectCannotBeDisconnectedOrAdmittedTwice) {
    CMPBus bus;
    bus.deferLock = true;
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    int completions = 0;
    cmp.ConnectIPCR(device, 0, 5, [&](CMPStatus status) {
        ++completions;
        EXPECT_EQ(status, CMPStatus::Success);
    });
    EXPECT_EQ(completions, 0);
    cmp.ConnectIPCR(device, 0, 5, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Failed); });
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Failed); });
    EXPECT_EQ(bus.lockCount, 1U);
    auto pending = std::move(bus.pending);
    ASSERT_TRUE(pending);
    pending(AsyncStatus::kSuccess, bus.pendingPayload);
    EXPECT_EQ(completions, 1);
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.lockCount, 2U);
}

TEST(CMPConnectionTests, ExistingP2POnSameChannelIsNotAdoptedWithoutIntent) {
    CMPBus bus;
    bus.pcrByNode_[2] = 0x81050000U;
    CMPClient cmp(bus, bus, bus.routes);
    cmp.ConnectIPCR(Device(bus.routes, 0xA, 2), 0, 5,
        [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::NoResources); });
    EXPECT_EQ(bus.lockCount, 0U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81050000U);
}

TEST(CMPConnectionTests, DisconnectReadbackFailureRetainsOwnedLeaseUntilRetry) {
    CMPBus bus;
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    cmp.ConnectIPCR(device, 0, 5, [](CMPStatus status) { ASSERT_EQ(status, CMPStatus::Success); });
    bus.faults.push_back({AsyncStatus::kTimeout, true, true});
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Failed); });
    cmp.DisconnectIPCR(device, 0, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.lockCount, 2U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x80050000U);
}


TEST(CMPConnectionTests, KnownCASMismatchDoesNotLeaveUncertainLeaseAfterReadFailure) {
    CMPBus bus;
    bus.conflictOnce = true;
    bus.faults.push_back({AsyncStatus::kSuccess, true, true});
    CMPClient cmp(bus, bus, bus.routes);
    const auto device = Device(bus.routes, 0xA, 2);
    cmp.ConnectIPCR(device, 0, 5, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Failed); });
    cmp.ConnectIPCR(device, 0, 5, [](CMPStatus status) { EXPECT_EQ(status, CMPStatus::Success); });
    EXPECT_EQ(bus.lockCount, 2U);
}

} // namespace
