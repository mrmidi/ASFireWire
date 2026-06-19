#include <gtest/gtest.h>

#include "ASFWDriver/Protocols/SBP2/Session/FetchAgent.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2CommandORB.hpp"
#include "ASFWDriver/Protocols/SBP2/AddressSpaceManager.hpp"
#include "ASFWDriver/Protocols/SBP2/SBP2WireFormats.hpp"
#include "ASFWDriver/Testing/HostDriverKitStubs.hpp"
#include "tests/mocks/DeferredFireWireBus.hpp"
#include "FakeSessionScheduler.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace {

using ASFW::Protocols::SBP2::AddressSpaceManager;
using ASFW::Protocols::SBP2::FetchAgent;
using ASFW::Protocols::SBP2::SBP2CommandORB;
using ASFW::Async::Testing::DeferredFireWireBus;
using ASFW::Testing::FakeSessionScheduler;
using ASFW::Async::AsyncStatus;
namespace Wire = ASFW::Protocols::SBP2::Wire;

constexpr uint64_t kMs = 1'000'000ULL;

ASFW::Async::FWAddress MakeAddr(uint16_t hi, uint32_t lo, uint16_t node) {
    return ASFW::Async::FWAddress(
        ASFW::Async::FWAddress::QualifiedAddressParts{hi, lo, node});
}

struct Rig {
    DeferredFireWireBus bus;
    FakeSessionScheduler scheduler;
    AddressSpaceManager addressManager{nullptr};

    Rig() {
        bus.SetGeneration(ASFW::FW::Generation{1});
        bus.SetLocalNodeID(ASFW::FW::NodeId{0x21});
        bus.SetDefaultSpeed(ASFW::FW::FwSpeed::S400);
    }

    FetchAgent::Binding Binding() const {
        return FetchAgent::Binding{
            .generation = 1,
            .nodeID = 0x02,
            .fetchAgentAddress = MakeAddr(0xFFFF, 0x0000'0050u, 0x02),
            .doorbellAddress = MakeAddr(0xFFFF, 0x0000'0054u, 0x02),
            .agentResetAddress = MakeAddr(0xFFFF, 0x0000'0058u, 0x02),
            .maxPayloadSize = 2048,
        };
    }
};

// Build a solicited status block targeting the given ORB address.
Wire::StatusBlock StatusFor(const ASFW::Async::FWAddress& orbAddr, uint8_t sbpStatus) {
    Wire::StatusBlock block{};
    block.details = 0x00;  // solicited (source bits not the 0x80 unsolicited pattern)
    block.sbpStatus = sbpStatus;
    block.orbOffsetHi = OSSwapHostToBigInt16(orbAddr.addressHi);
    block.orbOffsetLo = OSSwapHostToBigInt32(orbAddr.addressLo);
    return block;
}

TEST(FetchAgentTests, SubmitRejectedWhenUnbound) {
    Rig rig;
    FetchAgent agent(rig.bus, rig.bus, rig.scheduler);
    SBP2CommandORB orb(rig.addressManager, reinterpret_cast<void*>(0x1), 16);
    EXPECT_FALSE(agent.Submit(&orb));
}

TEST(FetchAgentTests, ImmediateSubmitWritesToFetchAgentAndArmsTimeoutOnSuccess) {
    Rig rig;
    FetchAgent agent(rig.bus, rig.bus, rig.scheduler);
    agent.Bind(rig.Binding());

    SBP2CommandORB orb(rig.addressManager, reinterpret_cast<void*>(0x1), 16);
    orb.SetFlags(SBP2CommandORB::kImmediate | SBP2CommandORB::kNotify | SBP2CommandORB::kNormalORB);
    orb.SetTimeout(500);
    int completions = 0;
    int lastSbp = -1;
    orb.SetCompletionCallback([&](int /*transport*/, uint8_t sbp) {
        ++completions;
        lastSbp = sbp;
    });

    ASSERT_TRUE(agent.Submit(&orb));
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());      // fetch-agent write issued
    EXPECT_EQ(0u, rig.scheduler.PendingCount());     // timeout not armed until write ACKs

    ASSERT_TRUE(rig.bus.CompleteNextWrite(AsyncStatus::kSuccess));
    EXPECT_EQ(1u, rig.scheduler.PendingCount());     // timeout armed after success
    EXPECT_EQ(0, completions);

    // Solicited status completes the ORB and cancels the timeout.
    ASSERT_TRUE(agent.OnStatusBlock(StatusFor(orb.GetORBAddress(), 0x00), Wire::StatusBlock::kMaxSize));
    EXPECT_EQ(1, completions);
    EXPECT_EQ(0x00, lastSbp);
    EXPECT_EQ(0u, rig.scheduler.PendingCount());     // timeout canceled on completion
}

TEST(FetchAgentTests, OrbTimeoutFailsCommandWhenNoStatusArrives) {
    Rig rig;
    FetchAgent agent(rig.bus, rig.bus, rig.scheduler);
    agent.Bind(rig.Binding());

    SBP2CommandORB orb(rig.addressManager, reinterpret_cast<void*>(0x2), 16);
    orb.SetFlags(SBP2CommandORB::kImmediate);
    orb.SetTimeout(500);
    int completions = 0;
    int lastTransport = 0;
    orb.SetCompletionCallback([&](int transport, uint8_t /*sbp*/) {
        ++completions;
        lastTransport = transport;
    });

    ASSERT_TRUE(agent.Submit(&orb));
    ASSERT_TRUE(rig.bus.CompleteNextWrite(AsyncStatus::kSuccess));
    ASSERT_EQ(1u, rig.scheduler.PendingCount());

    rig.scheduler.Advance(499 * kMs);
    EXPECT_EQ(0, completions);
    rig.scheduler.Advance(2 * kMs);                  // cross the 500 ms deadline
    EXPECT_EQ(1, completions);
    EXPECT_EQ(-1, lastTransport);
}

TEST(FetchAgentTests, WriteRetryExhaustionFailsOrbAndResetsAgent) {
    Rig rig;
    FetchAgent agent(rig.bus, rig.bus, rig.scheduler);
    agent.Bind(rig.Binding());
    agent.SetWriteRetriesForTesting(0);              // no retries → first failure is terminal

    SBP2CommandORB orb(rig.addressManager, reinterpret_cast<void*>(0x3), 16);
    orb.SetFlags(SBP2CommandORB::kImmediate);
    orb.SetTimeout(500);
    int completions = 0;
    int lastTransport = 0;
    orb.SetCompletionCallback([&](int transport, uint8_t /*sbp*/) {
        ++completions;
        lastTransport = transport;
    });

    ASSERT_TRUE(agent.Submit(&orb));
    ASSERT_EQ(1u, rig.bus.PendingWriteCount());
    ASSERT_TRUE(rig.bus.CompleteNextWrite(AsyncStatus::kHardwareError));   // fetch-agent write fails

    EXPECT_EQ(1, completions);
    EXPECT_EQ(-1, lastTransport);
    EXPECT_EQ(1u, rig.bus.PendingWriteCount());       // agent-reset write issued
}

} // namespace
