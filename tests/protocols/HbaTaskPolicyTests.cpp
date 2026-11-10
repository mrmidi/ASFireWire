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
constexpr uint8_t kReserve6 = 0x16;
constexpr uint8_t kRelease6 = 0x17;
constexpr uint8_t kReserve10 = 0x56;
constexpr uint8_t kRelease10 = 0x57;

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
                           kReserve6, kRelease10, kRead10}) {
            EXPECT_EQ(Disposition::NotPresent, Policy::Classify(1, op, ready))
                << "op 0x" << std::hex << int(op) << " ready " << ready;
        }
    }
}

// #139: the Minolta FS-V2 hung on the SCAN after a preview while RELEASE was
// answered locally. RESERVE/RELEASE go to the target like any other command.
TEST(HbaTaskPolicyTests, ReserveReleaseReachTheTarget) {
    for (uint8_t op : {kReserve6, kRelease6, kReserve10, kRelease10}) {
        EXPECT_EQ(Disposition::Forward, Policy::Classify(0, op, true));
        EXPECT_EQ(Disposition::Busy, Policy::Classify(0, op, false));
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

// Apple's SBP-2 transport ceiling (512 PTEs x 0xF000). The Hasselblad H4D reads
// its calibration block as one 1,966,080-byte READ with a 5,242,880-byte retry;
// the former 1 MiB cap clipped both and the camera never initialised capture.
TEST(HbaTaskPolicyTests, TransferCapMatchesAppleAndCoversHasselbladCalibrationReads) {
    EXPECT_EQ(Policy::kMaxTransferBytes, 512ull * 0xF000ull);
    EXPECT_GE(Policy::kMaxTransferBytes, 1'966'080ull);
    EXPECT_GE(Policy::kMaxTransferBytes, 5'242'880ull);
}

// --- Attach probe, REPORT LUNS, short transfers --------------------------------

TEST(HbaTaskPolicyTests, OnlyTarget0TURsInsideTheCreateCallAreAttachProbe) {
    EXPECT_TRUE(Policy::IsAttachProbeTUR(0, Policy::kOpTestUnitReady, true));
    // Every TUR after the create returns goes to the target (disk drivers send their own).
    EXPECT_FALSE(Policy::IsAttachProbeTUR(0, Policy::kOpTestUnitReady, false));
    // Only TEST UNIT READY; INQUIRY and the rest of the probe go to the target.
    EXPECT_FALSE(Policy::IsAttachProbeTUR(0, kInquiry, true));
    EXPECT_FALSE(Policy::IsAttachProbeTUR(0, Policy::kOpRequestSense, true));
    EXPECT_FALSE(Policy::IsAttachProbeTUR(1, Policy::kOpTestUnitReady, true));
}

TEST(HbaTaskPolicyTests, ReportLunsListsTheOneLoginLunAsLun0) {
    const auto all = Policy::BuildReportLunsData(0x00);
    ASSERT_EQ(all.length, 16u);
    const uint8_t expected[16] = {0, 0, 0, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < 16; ++i) {
        EXPECT_EQ(all.bytes[i], expected[i]) << "byte " << i;
    }
    const auto everything = Policy::BuildReportLunsData(0x02);
    EXPECT_EQ(everything.length, 16u);
    EXPECT_EQ(everything.bytes[3], 8u);
    // Well-known LUNs only: none.
    const auto wellKnown = Policy::BuildReportLunsData(0x01);
    EXPECT_EQ(wellKnown.length, 8u);
    EXPECT_EQ(wellKnown.bytes[3], 0u);
}

TEST(HbaTaskPolicyTests, ReportLunsCopyStopsAtTheAllocationLength) {
    const auto data = Policy::BuildReportLunsData(0x00);
    // The family asks for the 16-byte header first; a shorter allocation truncates.
    EXPECT_EQ(Policy::CopyBackLength(16, data.length, 4096), 16u);
    EXPECT_EQ(Policy::CopyBackLength(8, data.length, 4096), 8u);
    EXPECT_EQ(Policy::CopyBackLength(64, data.length, 4096), 16u);
    EXPECT_EQ(Policy::CopyBackLength(64, data.length, 4), 4u);
}

TEST(HbaTaskPolicyTests, EveryShortDataInBounces) {
    // The family's probe lengths seen on HW; the target wrote 36 bytes into each.
    for (uint64_t length : {4u, 5u, 6u, 36u, 134u, 255u, 511u}) {
        EXPECT_TRUE(Policy::ShouldBounceTransfer(length, true)) << length;
    }
    EXPECT_FALSE(Policy::ShouldBounceTransfer(0, true));
    EXPECT_FALSE(Policy::ShouldBounceTransfer(512, true));
    EXPECT_FALSE(Policy::ShouldBounceTransfer(61440, true));  // image reads stay in place
}

TEST(HbaTaskPolicyTests, ShortUnalignedDataOutBouncesLikeApple) {
    // Apple double-buffers lengths under 512 that are not quadlet multiples.
    EXPECT_TRUE(Policy::ShouldBounceTransfer(2, false));
    EXPECT_TRUE(Policy::ShouldBounceTransfer(511, false));
    EXPECT_FALSE(Policy::ShouldBounceTransfer(36, false));  // vendor 0x0F command
    EXPECT_FALSE(Policy::ShouldBounceTransfer(0, false));
    EXPECT_FALSE(Policy::ShouldBounceTransfer(4, false));
    EXPECT_FALSE(Policy::ShouldBounceTransfer(512, false));
    EXPECT_FALSE(Policy::ShouldBounceTransfer(513, false));
}

TEST(HbaTaskPolicyTests, CopyBackNeverExceedsRequestSourceOrMapping) {
    EXPECT_EQ(Policy::CopyBackLength(6, 36, 16384), 6u);   // target delivered more
    EXPECT_EQ(Policy::CopyBackLength(6, 4, 16384), 4u);    // target delivered less
    EXPECT_EQ(Policy::CopyBackLength(6, 36, 5), 5u);       // short mapping
    EXPECT_EQ(Policy::CopyBackLength(0, 36, 16384), 0u);
}
