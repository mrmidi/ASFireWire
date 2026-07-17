#include <gtest/gtest.h>

#include "ASFWDriver/Async/Interfaces/IFireWireBus.hpp"
#include "ASFWDriver/Protocols/AVC/CMP/CMPClient.hpp"

#include <array>
#include <cstring>
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
    AsyncHandle ReadBlock(Generation, NodeId node, FWAddress address, uint32_t,
                          FwSpeed, InterfaceCompletionCallback callback) override {
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

    AsyncHandle Lock(Generation, NodeId node, FWAddress, ASFW::FW::LockOp,
                     std::span<const uint8_t> operand, uint32_t, FwSpeed,
                     InterfaceCompletionCallback callback) override {
        ++lockCount;
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
        } else if (observed == expected) {
            pcrByNode_[node.value] = desired;
        }
        const uint32_t observedWire = OSSwapHostToBigInt32(observed);
        std::array<uint8_t, 4> payload{};
        std::memcpy(payload.data(), &observedWire, sizeof(observedWire));
        callback(AsyncStatus::kSuccess, payload);
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
    bool conflictOnce{false};
    uint32_t lockCount{0};

private:
    AsyncHandle NextHandle() { return AsyncHandle{++nextHandle_}; }
    uint32_t nextHandle_{0};
};

CMPDevice Device(uint64_t guid, uint8_t node, uint32_t generation = 1) {
    return CMPDevice{.guid = guid, .nodeId = NodeId{node}, .generation = Generation{generation}};
}

TEST(CMPConnectionTests, LeasesAreIndependentForDifferentDeviceGuids) {
    CMPBus bus;
    CMPClient cmp(bus, bus);
    CMPStatus first = CMPStatus::Failed;
    CMPStatus second = CMPStatus::Failed;

    cmp.ConnectOPCR(Device(0xA, 2), 0, 5, [&first](CMPStatus status) { first = status; });
    cmp.ConnectOPCR(Device(0xB, 3), 0, 7, [&second](CMPStatus status) { second = status; });

    EXPECT_EQ(first, CMPStatus::Success);
    EXPECT_EQ(second, CMPStatus::Success);
    EXPECT_EQ(bus.lockCount, 2U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81058000U);
    EXPECT_EQ(bus.pcrByNode_[3], 0x81078000U);
}

TEST(CMPConnectionTests, DisconnectWithoutLocalLeaseDoesNotDecrementRemoteP2P) {
    CMPBus bus;
    bus.pcrByNode_[2] = 0x81058000U;
    CMPClient cmp(bus, bus);
    CMPStatus status = CMPStatus::Failed;

    cmp.DisconnectOPCR(Device(0xA, 2), 0, [&status](CMPStatus result) { status = result; });

    EXPECT_EQ(status, CMPStatus::Success);
    EXPECT_EQ(bus.lockCount, 0U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81058000U);
}

TEST(CMPConnectionTests, RetriesCompareSwapContentionWithFreshPCRRead) {
    CMPBus bus;
    bus.conflictOnce = true;
    CMPClient cmp(bus, bus);
    CMPStatus status = CMPStatus::Failed;

    cmp.ConnectOPCR(Device(0xA, 2), 0, 5, [&status](CMPStatus result) { status = result; });

    EXPECT_EQ(status, CMPStatus::Success);
    EXPECT_EQ(bus.lockCount, 2U);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81058000U);
}

TEST(CMPConnectionTests, ProgramsOutputPCRSpeedAndOverheadFromLiveGapCount) {
    CMPBus bus;
    bus.routeSpeed = FwSpeed::S200;
    bus.gapCount = 8;
    CMPClient cmp(bus, bus);
    CMPStatus status = CMPStatus::Failed;

    cmp.ConnectOPCR(Device(0xA, 2), 0, 5, [&status](CMPStatus result) { status = result; });

    // gap 8 derives 166 bandwidth units; oPCR overhead encoding selects ID 6.
    EXPECT_EQ(status, CMPStatus::Success);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81055800U);
}

TEST(CMPConnectionTests, ProgramsInputPCROnlyWithConnectionAndChannelFields) {
    CMPBus bus;
    bus.routeSpeed = FwSpeed::S200;
    bus.gapCount = 8;
    CMPClient cmp(bus, bus);
    CMPStatus status = CMPStatus::Failed;

    cmp.ConnectIPCR(Device(0xA, 2), 0, 5, [&status](CMPStatus result) { status = result; });

    // iPCR has no data-rate or overhead-ID fields; those bits must remain zero.
    EXPECT_EQ(status, CMPStatus::Success);
    EXPECT_EQ(bus.pcrByNode_[2], 0x81050000U);
}

TEST(CMPConnectionTests, RejectsBroadcastPCRWithoutCompareSwap) {
    CMPBus bus;
    bus.pcrByNode_[2] = 0xC0000000U; // online + foreign broadcast connection
    CMPClient cmp(bus, bus);
    CMPStatus status = CMPStatus::Success;

    cmp.ConnectOPCR(Device(0xA, 2), 0, 5, [&status](CMPStatus result) { status = result; });

    EXPECT_EQ(status, CMPStatus::NoResources);
    EXPECT_EQ(bus.lockCount, 0U);
    EXPECT_EQ(bus.pcrByNode_[2], 0xC0000000U);
}

TEST(CMPConnectionTests, RejectsPlugOutsideMasterPlugRegisterCount) {
    CMPBus bus;
    bus.mprValue = 0x80000000U; // S400, no plugs
    CMPClient cmp(bus, bus);
    CMPStatus status = CMPStatus::Success;

    cmp.ConnectOPCR(Device(0xA, 2), 0, 5, [&status](CMPStatus result) { status = result; });

    EXPECT_EQ(status, CMPStatus::NotFound);
    EXPECT_EQ(bus.lockCount, 0U);
}

TEST(CMPConnectionTests, NewGenerationDropsOldLeaseBeforeReconnect) {
    CMPBus bus;
    CMPClient cmp(bus, bus);
    CMPStatus first = CMPStatus::Failed;
    cmp.ConnectOPCR(Device(0xA, 2, 1), 0, 5, [&first](CMPStatus status) { first = status; });
    ASSERT_EQ(first, CMPStatus::Success);

    // A reset restores remote PCR state. The new routing epoch must not be
    // blocked by the lease that represented the previous generation.
    bus.pcrByNode_[3] = 0x80000000U;
    CMPStatus second = CMPStatus::Failed;
    cmp.ConnectOPCR(Device(0xA, 3, 2), 0, 7, [&second](CMPStatus status) { second = status; });

    EXPECT_EQ(second, CMPStatus::Success);
    EXPECT_EQ(bus.lockCount, 2U);
    EXPECT_EQ(bus.pcrByNode_[3], 0x81078000U);
}

} // namespace
