// HbaTaskPolicyTests — the SCSI HBA's target-ID and per-task decisions.

#include "ASFWDriver/SCSIController/HbaTaskPolicy.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace Policy = ASFW::Protocols::SBP2::HbaTaskPolicy;
using Policy::Disposition;

namespace {

constexpr uint8_t kInquiry = 0x12;
constexpr uint8_t kRead10 = 0x28;

// Target IDs the family's per-ID loops visit for a reported highest ID.
// willTerminate uses `<` (OSS IOSCSIParallelInterfaceController.cpp); start's
// scan uses `<=`. `inclusive` lets a test cover Apple fixing willTerminate too.
std::vector<uint64_t> FamilyLoopIDs(uint64_t highest, bool inclusive) {
    std::vector<uint64_t> ids;
    for (uint64_t id = 0; inclusive ? id <= highest : id < highest; ++id) {
        ids.push_back(id);
    }
    return ids;
}

bool Contains(const std::vector<uint64_t>& ids, uint64_t id) {
    for (uint64_t v : ids) {
        if (v == id) {
            return true;
        }
    }
    return false;
}

} // namespace

// #139: with highest ID 0 the strict-< willTerminate loop never ran, so target
// 0's held task outlived the kernel's work loop. It must be flushed under the
// current bound and under a fixed (<=) one.
TEST(HbaTaskPolicyTests, WillTerminateFlushesTheBridgedTargetUnderEitherBound) {
    EXPECT_TRUE(Contains(FamilyLoopIDs(Policy::kReportedHighestTargetID, false),
                         Policy::kBridgedTargetID));
    EXPECT_TRUE(Contains(FamilyLoopIDs(Policy::kReportedHighestTargetID, true),
                         Policy::kBridgedTargetID));
    EXPECT_FALSE(Contains(FamilyLoopIDs(0, false), Policy::kBridgedTargetID)); // the old bug
}

// The bring-up scan probes every ID up to the reported highest. Only the
// bridged target may reach the scanner, or it is published twice.
TEST(HbaTaskPolicyTests, BringUpScanReachesTheScannerOnlyThroughTarget0) {
    for (uint64_t id : FamilyLoopIDs(Policy::kReportedHighestTargetID, true)) {
        const Disposition d = Policy::Classify(id, kInquiry, /*sessionReady*/ true);
        if (id == Policy::kBridgedTargetID) {
            EXPECT_EQ(Disposition::Forward, d);
        } else {
            EXPECT_EQ(Disposition::NotPresent, d) << "target " << id;
        }
    }
}

TEST(HbaTaskPolicyTests, OtherTargetsAnswerNothingEvenSyntheticOnes) {
    for (bool ready : {true, false}) {
        for (uint8_t op : {Policy::kOpTestUnitReady, Policy::kOpRequestSense,
                           Policy::kOpReserve6, Policy::kOpRelease10, kRead10}) {
            EXPECT_EQ(Disposition::NotPresent, Policy::Classify(1, op, ready))
                << "op 0x" << std::hex << int(op) << " ready " << ready;
        }
    }
}

TEST(HbaTaskPolicyTests, ReserveReleaseNeverReachTheWire) {
    for (bool ready : {true, false}) {
        for (uint8_t op : {Policy::kOpReserve6, Policy::kOpRelease6,
                           Policy::kOpReserve10, Policy::kOpRelease10}) {
            EXPECT_EQ(Disposition::SyntheticGood, Policy::Classify(0, op, ready));
        }
    }
}

TEST(HbaTaskPolicyTests, NotReadyKeepsProbesMovingAndBusiesDataCommands) {
    EXPECT_EQ(Disposition::SyntheticGood,
              Policy::Classify(0, Policy::kOpTestUnitReady, false));
    EXPECT_EQ(Disposition::SyntheticGood,
              Policy::Classify(0, Policy::kOpRequestSense, false));
    EXPECT_EQ(Disposition::Busy, Policy::Classify(0, kInquiry, false));
    EXPECT_EQ(Disposition::Busy, Policy::Classify(0, kRead10, false));
}

TEST(HbaTaskPolicyTests, ReadySessionForwardsEverythingElse) {
    for (uint8_t op : {Policy::kOpTestUnitReady, Policy::kOpRequestSense, kInquiry, kRead10}) {
        EXPECT_EQ(Disposition::Forward, Policy::Classify(0, op, true));
    }
}
