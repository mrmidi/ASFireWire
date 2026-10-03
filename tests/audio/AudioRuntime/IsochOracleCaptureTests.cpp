#include "Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp"
#include "Audio/DriverKit/Runtime/IsochOracleCapture.hpp"
#include "Shared/ASFWIsochOracleCaptureABI.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace {

using ASFW::Audio::Runtime::AudioTransportControlBlock;
using ASFW::Audio::Runtime::IsochOracleCapture;
using ASFW::Audio::Runtime::IsochOracleDirection;
using ASFW::Audio::Runtime::IsochOracleRecord;
using ASFW::Audio::Runtime::kIsochOracleCaptureRecords;
using ASFW::Audio::Runtime::kIsochOracleFlagData;
using ASFW::Audio::Runtime::kIsochOracleFlagHasCycle;
using ASFW::Audio::Runtime::kIsochOracleFlagHasDbc;
using ASFW::Audio::Runtime::kIsochOracleFlagHasSph;
using ASFW::Audio::Runtime::kIsochOracleFlagValidCip;

// The export copies records verbatim, so the layout is a contract with the
// offline comparison, not an implementation detail.
TEST(IsochOracleCaptureTests, RecordLayoutMatchesExportAbi) {
    EXPECT_EQ(sizeof(IsochOracleRecord), 24u);
    EXPECT_EQ(sizeof(IsochOracleRecord), sizeof(ASFWIsochOracleRecordV1));
    EXPECT_EQ(offsetof(IsochOracleRecord, packetIndex),
              offsetof(ASFWIsochOracleRecordV1, packetIndex));
    EXPECT_EQ(offsetof(IsochOracleRecord, cycleTimestamp),
              offsetof(ASFWIsochOracleRecordV1, cycleTimestamp));
    EXPECT_EQ(offsetof(IsochOracleRecord, firstSph),
              offsetof(ASFWIsochOracleRecordV1, firstSph));
    EXPECT_EQ(offsetof(IsochOracleRecord, wireLengthBytes),
              offsetof(ASFWIsochOracleRecordV1, wireLengthBytes));
    EXPECT_EQ(offsetof(IsochOracleRecord, transferStatus),
              offsetof(ASFWIsochOracleRecordV1, transferStatus));
    EXPECT_EQ(offsetof(IsochOracleRecord, residualCount),
              offsetof(ASFWIsochOracleRecordV1, residualCount));
    EXPECT_EQ(offsetof(IsochOracleRecord, dbc),
              offsetof(ASFWIsochOracleRecordV1, dbc));
    EXPECT_EQ(offsetof(IsochOracleRecord, flags),
              offsetof(ASFWIsochOracleRecordV1, flags));
}

TEST(IsochOracleCaptureTests, TxRecordPreservesWireFactsOfDataPacket) {
    IsochOracleCapture capture{};
    capture.Reset(3);

    capture.RecordTxPrepared(0, 424, true, 0x18, 0x1234abcd, true);

    IsochOracleRecord record{};
    ASSERT_TRUE(capture.Tx().ReadRecord(0, record));
    EXPECT_EQ(record.packetIndex, 0u);
    EXPECT_EQ(record.wireLengthBytes, 424u);
    EXPECT_EQ(record.dbc, 0x18u);
    EXPECT_EQ(record.firstSph, 0x1234abcdu);
    EXPECT_TRUE(record.flags & kIsochOracleFlagData);
    EXPECT_TRUE(record.flags & kIsochOracleFlagHasSph);
    EXPECT_TRUE(record.flags & kIsochOracleFlagHasDbc);
    // Not yet completed by hardware.
    EXPECT_FALSE(record.flags & kIsochOracleFlagHasCycle);
    EXPECT_EQ(capture.Generation(), 3u);
}

// A NO-DATA packet carries no SPH; the flag must stay clear so offline can
// tell "no SPH" from "SPH happened to be zero".
TEST(IsochOracleCaptureTests, TxNoDataRecordClearsDataAndSphFlags) {
    IsochOracleCapture capture{};
    capture.Reset(1);

    capture.RecordTxPrepared(7, 8, false, 0x00, 0xdeadbeef, false);

    IsochOracleRecord record{};
    ASSERT_TRUE(capture.Tx().ReadRecord(0, record));
    EXPECT_EQ(record.wireLengthBytes, 8u);
    EXPECT_FALSE(record.flags & kIsochOracleFlagData);
    EXPECT_FALSE(record.flags & kIsochOracleFlagHasSph);
    EXPECT_EQ(record.firstSph, 0u);
}

TEST(IsochOracleCaptureTests, RxRecordPreservesTransportAndContentFields) {
    IsochOracleCapture capture{};
    capture.Reset(1);

    capture.RecordRxPacket(11, 520, 0x8451, 3568, true, true, 0x20, 0x0000c0de,
                           true, 0x5048, true);

    IsochOracleRecord record{};
    ASSERT_TRUE(capture.Rx().ReadRecord(0, record));
    EXPECT_EQ(record.packetIndex, 11u);
    EXPECT_EQ(record.wireLengthBytes, 520u);
    EXPECT_EQ(record.transferStatus, 0x8451u);
    EXPECT_EQ(record.residualCount, 3568u);
    EXPECT_EQ(record.dbc, 0x20u);
    EXPECT_EQ(record.cycleTimestamp, 0x5048u);
    EXPECT_TRUE(record.flags & kIsochOracleFlagData);
    EXPECT_TRUE(record.flags & kIsochOracleFlagValidCip);
    EXPECT_TRUE(record.flags & kIsochOracleFlagHasCycle);
    EXPECT_TRUE(record.flags & kIsochOracleFlagHasDbc);
}

// An RX packet the decoder rejected still belongs in the window: the oracle
// counted it, and dropping it here would compare the wrong cadence.
TEST(IsochOracleCaptureTests, RxRecordWithoutValidCipClearsDbcFlag) {
    IsochOracleCapture capture{};
    capture.Reset(1);

    capture.RecordRxPacket(0, 8, 0x8451, 4080, false, false, 0x00, 0, false,
                           0x5049, true);

    IsochOracleRecord record{};
    ASSERT_TRUE(capture.Rx().ReadRecord(0, record));
    EXPECT_EQ(record.wireLengthBytes, 8u);
    EXPECT_FALSE(record.flags & kIsochOracleFlagData);
    EXPECT_FALSE(record.flags & kIsochOracleFlagValidCip);
    EXPECT_FALSE(record.flags & kIsochOracleFlagHasDbc);
    EXPECT_TRUE(record.flags & kIsochOracleFlagHasCycle);
}

TEST(IsochOracleCaptureTests, SectionFreezesAtCapacityAndCountsSuppressed) {
    IsochOracleCapture capture{};
    capture.Reset(1);

    for (uint32_t i = 0; i < kIsochOracleCaptureRecords; ++i) {
        capture.RecordTxPrepared(i, 424, true, static_cast<uint8_t>(i), i, true);
    }
    EXPECT_TRUE(capture.Tx().Frozen());
    EXPECT_EQ(capture.Tx().Count(), kIsochOracleCaptureRecords);
    EXPECT_EQ(capture.Tx().Suppressed(), 0u);

    for (uint32_t i = 0; i < 17; ++i) {
        capture.RecordTxPrepared(kIsochOracleCaptureRecords + i, 424, true, 0, 0,
                                 true);
    }
    EXPECT_EQ(capture.Tx().Count(), kIsochOracleCaptureRecords);
    EXPECT_EQ(capture.Tx().Suppressed(), 17u);

    // The freeze must not corrupt the last in-window record.
    IsochOracleRecord last{};
    ASSERT_TRUE(capture.Tx().ReadRecord(kIsochOracleCaptureRecords - 1, last));
    EXPECT_EQ(last.packetIndex, kIsochOracleCaptureRecords - 1);
    EXPECT_FALSE(capture.Tx().ReadRecord(kIsochOracleCaptureRecords, last));
}

TEST(IsochOracleCaptureTests, BackfillAttachesCompletionCycleToItsOwnPacket) {
    IsochOracleCapture capture{};
    capture.Reset(1);
    for (uint32_t i = 0; i < 4; ++i) {
        capture.RecordTxPrepared(100 + i, 424, true, 0, 0, true);
    }

    capture.BackfillTxCompletionCycle(102, 0x501e);

    IsochOracleRecord record{};
    ASSERT_TRUE(capture.Tx().ReadRecord(2, record));
    EXPECT_EQ(record.packetIndex, 102u);
    EXPECT_EQ(record.cycleTimestamp, 0x501eu);
    EXPECT_TRUE(record.flags & kIsochOracleFlagHasCycle);
    EXPECT_EQ(capture.Tx().Backfilled(), 1u);
    EXPECT_EQ(capture.Tx().LostCycles(), 0u);

    // Neighbours must stay untouched.
    ASSERT_TRUE(capture.Tx().ReadRecord(1, record));
    EXPECT_FALSE(record.flags & kIsochOracleFlagHasCycle);
}

// A completion for a packet outside the window must be counted, never guessed
// onto a neighbouring record — an offset comparison built on a guess is worse
// than one that reports the gap.
TEST(IsochOracleCaptureTests, BackfillOutsideWindowCountsLostCycle) {
    IsochOracleCapture capture{};
    capture.Reset(1);
    for (uint32_t i = 0; i < 4; ++i) {
        capture.RecordTxPrepared(100 + i, 424, true, 0, 0, true);
    }

    capture.BackfillTxCompletionCycle(99, 0x1111);   // before the window
    capture.BackfillTxCompletionCycle(104, 0x2222);  // past the window

    EXPECT_EQ(capture.Tx().LostCycles(), 2u);
    EXPECT_EQ(capture.Tx().Backfilled(), 0u);
    for (uint32_t i = 0; i < 4; ++i) {
        IsochOracleRecord record{};
        ASSERT_TRUE(capture.Tx().ReadRecord(i, record));
        EXPECT_FALSE(record.flags & kIsochOracleFlagHasCycle);
    }
}

TEST(IsochOracleCaptureTests, BackfillOnEmptySectionCountsLostCycle) {
    IsochOracleCapture capture{};
    capture.Reset(1);

    capture.BackfillTxCompletionCycle(0, 0x3333);

    EXPECT_EQ(capture.Tx().LostCycles(), 1u);
    EXPECT_EQ(capture.Tx().Count(), 0u);
}

// Completions lag preparation, so the drain must outlive the TX freeze and
// stop only once the last captured packet has been accounted for.
TEST(IsochOracleCaptureTests, DrainFinishesOnlyAfterFreezeAndLastPacket) {
    IsochOracleCapture capture{};
    capture.Reset(1);

    capture.RecordTxPrepared(0, 424, true, 0, 0, true);
    capture.NoteTxCompletionDrained(0);
    EXPECT_FALSE(capture.TxCompletionDrainFinished());

    for (uint32_t i = 1; i < kIsochOracleCaptureRecords; ++i) {
        capture.RecordTxPrepared(i, 424, true, 0, 0, true);
    }
    ASSERT_TRUE(capture.Tx().Frozen());

    capture.NoteTxCompletionDrained(kIsochOracleCaptureRecords - 2);
    EXPECT_FALSE(capture.TxCompletionDrainFinished());

    capture.NoteTxCompletionDrained(kIsochOracleCaptureRecords - 1);
    EXPECT_TRUE(capture.TxCompletionDrainFinished());
}

TEST(IsochOracleCaptureTests, CompleteRequiresBothDirectionsFrozen) {
    IsochOracleCapture capture{};
    capture.Reset(1);

    for (uint32_t i = 0; i < kIsochOracleCaptureRecords; ++i) {
        capture.RecordTxPrepared(i, 424, true, 0, 0, true);
    }
    EXPECT_FALSE(capture.Complete());

    for (uint32_t i = 0; i < kIsochOracleCaptureRecords; ++i) {
        capture.RecordRxPacket(i, 520, 0x8451, 3568, true, true, 0, 0, false, 0,
                               true);
    }
    EXPECT_TRUE(capture.Complete());
}

TEST(IsochOracleCaptureTests, ResetClearsBothSectionsAndCounters) {
    IsochOracleCapture capture{};
    capture.Reset(1);
    capture.RecordTxPrepared(0, 424, true, 0, 0, true);
    capture.RecordRxPacket(0, 520, 0x8451, 3568, true, true, 0, 0, false, 0, true);
    capture.BackfillTxCompletionCycle(0, 0x501e);
    capture.BackfillTxCompletionCycle(999, 0x501e);
    capture.SetTxCompletionCursor(42);

    capture.Reset(9);

    EXPECT_EQ(capture.Generation(), 9u);
    EXPECT_EQ(capture.Tx().Count(), 0u);
    EXPECT_EQ(capture.Rx().Count(), 0u);
    EXPECT_EQ(capture.Tx().Backfilled(), 0u);
    EXPECT_EQ(capture.Tx().LostCycles(), 0u);
    EXPECT_EQ(capture.Tx().Suppressed(), 0u);
    EXPECT_EQ(capture.TxCompletionCursor(), 0u);
    EXPECT_FALSE(capture.TxCompletionDrainFinished());
    EXPECT_FALSE(capture.Complete());
}

TEST(IsochOracleCaptureTests, SectionSelectorMapsDirections) {
    IsochOracleCapture capture{};
    capture.Reset(1);
    capture.RecordTxPrepared(0, 424, true, 0, 0, true);

    EXPECT_EQ(capture.Section(IsochOracleDirection::kTx).Count(), 1u);
    EXPECT_EQ(capture.Section(IsochOracleDirection::kRx).Count(), 0u);
}

// The capture is armed by the same reset that arms the rest of the control
// block, before the IT/IR contexts start, and carries that generation.
TEST(IsochOracleCaptureTests, ControlBlockResetArmsCaptureWithItsGeneration) {
    AudioTransportControlBlock control{};
    control.isochOracleCapture.RecordTxPrepared(0, 424, true, 0, 0, true);

    control.ResetForStart();

    EXPECT_EQ(control.isochOracleCapture.Tx().Count(), 0u);
    EXPECT_EQ(control.isochOracleCapture.Generation(),
              control.generation.load(std::memory_order_acquire));

    const uint64_t firstGeneration = control.isochOracleCapture.Generation();
    control.ResetForStart();
    EXPECT_EQ(control.isochOracleCapture.Generation(), firstGeneration + 1);
}

} // namespace
