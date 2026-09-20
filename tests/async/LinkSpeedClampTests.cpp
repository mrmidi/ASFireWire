// Regression cover for the single link-speed authority.
//
// Self-ID reports what a node CLAIMS it can do; it is not evidence that the link
// sustains that rate. A Midas Venice F24 behind a Thunderbolt bridge advertised
// S400 and returned evt_missing_ack for every S400 request while completing at
// S200 — the Config-ROM scan learned that and downgraded, but GetSpeed() kept
// reporting the Self-ID value, so the DICE section read went out at S400 and
// timed out. These tests pin the clamp that closes that gap.
//
// Shape follows Linux drivers/firewire/core-device.c:615-640, which keeps one
// speed per device (fw_device::max_speed), seeds it from Self-ID, probes it
// downwards, and then uses it for every subsequent transaction.

#include <gtest/gtest.h>

#include "ASFWDriver/Async/FireWireBusImpl.hpp"
#include "ASFWDriver/Async/Interfaces/ILinkSpeedSource.hpp"
#include "ASFWDriver/Async/AsyncSubsystem.hpp"
#include "ASFWDriver/Bus/TopologyManager.hpp"
#include "ASFWDriver/Bus/SelfIDCapture.hpp"
#include "ASFWDriver/Discovery/SpeedPolicy.hpp"

#include <optional>

namespace {

using ASFW::FW::FwSpeed;
using ASFW::FW::NodeId;

// The exact Self-ID sequence reported by the Midas Venice F24 bus
// (ASFW diagnostics, generation 1): node 0 is the device, node 1 is the local
// Mac and bus root. Speed lives in bits [15:14] of the node quadlet
// (IEEE 1394-1995 8.4.2.4), so the device's advertised rate is patched there
// rather than reconstructed, keeping the rest of the topology exactly as the
// hardware presented it.
constexpr uint32_t kDeviceSelfID = 0x807F8890;  // node 0, S400, parent on p0
constexpr uint32_t kLocalSelfID  = 0x817FC476;  // node 1, S800, root
constexpr uint32_t kNodeIDRegister = 0xC800FFC1;  // valid, local = node 1

uint32_t WithSpeed(uint32_t selfId, uint8_t speedCode) {
    return (selfId & ~(0x3u << 14)) | ((uint32_t(speedCode) & 0x3u) << 14);
}

ASFW::Driver::SelfIDCapture::Result TwoNodeBus(uint8_t deviceSpeedCode) {
    ASFW::Driver::SelfIDCapture::Result result;
    result.valid = true;
    result.generation = 1;
    result.quads = {0x00010001, WithSpeed(kDeviceSelfID, deviceSpeedCode), kLocalSelfID};
    result.sequences = {{1, 1}, {2, 1}};
    result.crcError = false;
    result.timedOut = false;
    return result;
}

/// Answers whatever the test tells it to, so the clamp can be exercised
/// independently of how SpeedPolicy arrives at a value.
class StubSpeedSource final : public ASFW::Async::ILinkSpeedSource {
  public:
    std::optional<FwSpeed> value;
    [[nodiscard]] std::optional<FwSpeed> ObservedSpeed(NodeId) const noexcept override {
        return value;
    }
};

class LinkSpeedClamp : public ::testing::Test {
  protected:
    void SeedTopology(uint8_t nodeZeroSpeedCode) {
        const auto snapshot =
            topo.UpdateFromSelfID(TwoNodeBus(nodeZeroSpeedCode), 1000, kNodeIDRegister);
        ASSERT_TRUE(snapshot.has_value())
            << "topology build failed: "
            << ASFW::Driver::TopologyManager::TopologyBuildErrorCodeString(snapshot.error().code);
    }

    ASFW::Async::AsyncSubsystem async;
    ASFW::Driver::TopologyManager topo;
};

// ---------------------------------------------------------------------------
// FireWireBusImpl::GetSpeed clamping
// ---------------------------------------------------------------------------

TEST_F(LinkSpeedClamp, NoSpeedSourceReportsAdvertisedSpeed) {
    SeedTopology(2 /* S400 */);
    ASFW::Async::FireWireBusImpl bus(async, topo);
    EXPECT_EQ(FwSpeed::S400, bus.GetSpeed(NodeId{0}));
}

TEST_F(LinkSpeedClamp, UnobservedNodeKeepsAdvertisedSpeed) {
    SeedTopology(2 /* S400 */);
    StubSpeedSource source;  // value stays nullopt: nothing observed yet
    ASFW::Async::FireWireBusImpl bus(async, topo, &source);
    // Absence of evidence is not evidence of a slow link.
    EXPECT_EQ(FwSpeed::S400, bus.GetSpeed(NodeId{0}));
}

TEST_F(LinkSpeedClamp, ObservedSlowerThanAdvertisedClampsDown) {
    // The F24 case: Self-ID says S400, the link only answers at S200.
    SeedTopology(2 /* S400 */);
    StubSpeedSource source;
    source.value = FwSpeed::S200;
    ASFW::Async::FireWireBusImpl bus(async, topo, &source);
    EXPECT_EQ(FwSpeed::S200, bus.GetSpeed(NodeId{0}));
}

TEST_F(LinkSpeedClamp, ObservedFasterThanAdvertisedDoesNotRaiseSpeed) {
    // A stale observation from a previous generation must never push a node
    // above what the current topology says it supports.
    SeedTopology(1 /* S200 */);
    StubSpeedSource source;
    source.value = FwSpeed::S800;
    ASFW::Async::FireWireBusImpl bus(async, topo, &source);
    EXPECT_EQ(FwSpeed::S200, bus.GetSpeed(NodeId{0}));
}

TEST_F(LinkSpeedClamp, ObservedEqualToAdvertisedIsUnchanged) {
    SeedTopology(2 /* S400 */);
    StubSpeedSource source;
    source.value = FwSpeed::S400;
    ASFW::Async::FireWireBusImpl bus(async, topo, &source);
    EXPECT_EQ(FwSpeed::S400, bus.GetSpeed(NodeId{0}));
}

// ---------------------------------------------------------------------------
// SpeedPolicy as the evidence source
// ---------------------------------------------------------------------------

TEST(SpeedPolicyObservation, UnseenNodeReportsNoObservation) {
    ASFW::Discovery::SpeedPolicy policy;
    EXPECT_FALSE(policy.ObservedSpeed(NodeId{7}).has_value());
}

TEST(SpeedPolicyObservation, TimeoutDowngradeBecomesTheObservation) {
    ASFW::Discovery::SpeedPolicy policy;
    policy.RecordTimeout(0, FwSpeed::S400);
    const auto observed = policy.ObservedSpeed(NodeId{0});
    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(FwSpeed::S200, *observed);
}

TEST(SpeedPolicyObservation, SuccessRecordsTheSpeedThatWorked) {
    ASFW::Discovery::SpeedPolicy policy;
    policy.RecordSuccess(0, FwSpeed::S200);
    const auto observed = policy.ObservedSpeed(NodeId{0});
    ASSERT_TRUE(observed.has_value());
    EXPECT_EQ(FwSpeed::S200, *observed);
}

TEST(SpeedPolicyObservation, ResetClearsObservationsAcrossBusReset) {
    // Node IDs are reassigned by a bus reset, so an observation keyed to the old
    // numbering must not survive into the next generation.
    ASFW::Discovery::SpeedPolicy policy;
    policy.RecordTimeout(0, FwSpeed::S400);
    ASSERT_TRUE(policy.ObservedSpeed(NodeId{0}).has_value());

    policy.Reset();
    EXPECT_FALSE(policy.ObservedSpeed(NodeId{0}).has_value());
}

// End-to-end: what discovery learns is what the bus facade reports.
TEST_F(LinkSpeedClamp, PolicyObservationReachesGetSpeed) {
    SeedTopology(2 /* S400 */);
    ASFW::Discovery::SpeedPolicy policy;
    ASFW::Async::FireWireBusImpl bus(async, topo, &policy);

    EXPECT_EQ(FwSpeed::S400, bus.GetSpeed(NodeId{0})) << "before any evidence";

    policy.RecordTimeout(0, FwSpeed::S400);  // what the ROM scan does on exhaustion
    EXPECT_EQ(FwSpeed::S200, bus.GetSpeed(NodeId{0})) << "after the downgrade";

    policy.Reset();  // what the bus-reset hook does
    EXPECT_EQ(FwSpeed::S400, bus.GetSpeed(NodeId{0})) << "evidence cleared by reset";
}

} // namespace
