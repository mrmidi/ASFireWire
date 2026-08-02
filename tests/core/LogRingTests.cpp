// Unit tests for the driver-owned log ring (Logging/LogRing.*): slot claim
// and publish, wrap-around retention, category/level/substring filtering,
// cursor continuation, and the stats surface the MCP layer exposes.

#include "Logging/LogRing.hpp"

#include <gtest/gtest.h>

#include <cstdarg>
#include <string>
#include <thread>
#include <vector>

namespace {

using ASFW::Logging::LogCategory;
using ASFW::Logging::LogLevel;
using ASFW::Logging::LogRecord;
using ASFW::Logging::LogRing;
using ASFW::Logging::LogRingQuery;

void Emit(LogRing& ring, LogCategory category, LogLevel level,
          const char* format, ...) {
    va_list args;
    va_start(args, format);
    ring.Append(category, level, format, args);
    va_end(args);
}

struct RingFixture {
    explicit RingFixture(uint32_t capacity)
        : ring(LogRing::CreateForTest(capacity)) {}
    ~RingFixture() { LogRing::DestroyForTest(ring); }
    LogRing* ring;
};

TEST(LogRingTests, AppendsAndQueriesInOrder) {
    RingFixture fixture(64);
    ASSERT_NE(fixture.ring, nullptr);

    Emit(*fixture.ring, LogCategory::CMP, LogLevel::Notice, "[CMP] connect ch=%u", 19U);
    Emit(*fixture.ring, LogCategory::Isoch, LogLevel::Error, "[Isoch] IT dead evt=0x%02x", 0x11U);

    LogRecord records[8];
    const auto result = fixture.ring->Query(LogRingQuery{}, records, 8);
    ASSERT_EQ(result.recordCount, 2U);
    EXPECT_EQ(records[0].sequence, 1U);
    EXPECT_STREQ(records[0].message, "[CMP] connect ch=19");
    EXPECT_EQ(records[0].category, static_cast<uint8_t>(LogCategory::CMP));
    EXPECT_EQ(records[1].sequence, 2U);
    EXPECT_STREQ(records[1].message, "[Isoch] IT dead evt=0x11");
    EXPECT_EQ(records[1].level, static_cast<uint8_t>(LogLevel::Error));
    EXPECT_EQ(result.nextSequence, 2U);
    EXPECT_EQ(result.latestSequence, 2U);
}

TEST(LogRingTests, WrapAroundKeepsOnlyNewestCapacityRecords) {
    RingFixture fixture(8);
    ASSERT_NE(fixture.ring, nullptr);

    for (int index = 0; index < 20; ++index) {
        Emit(*fixture.ring, LogCategory::Async, LogLevel::Notice, "msg %d", index);
    }

    LogRecord records[16];
    const auto result = fixture.ring->Query(LogRingQuery{}, records, 16);
    ASSERT_EQ(result.recordCount, 8U);
    EXPECT_EQ(result.oldestSequence, 13U);
    EXPECT_STREQ(records[0].message, "msg 12"); // sequence 13 carries "msg 12"
    EXPECT_STREQ(records[7].message, "msg 19");
}

TEST(LogRingTests, FiltersByCategoryLevelAndSubstring) {
    RingFixture fixture(64);
    ASSERT_NE(fixture.ring, nullptr);

    Emit(*fixture.ring, LogCategory::CMP, LogLevel::Notice, "[CMP] iPCR clear");
    Emit(*fixture.ring, LogCategory::FCP, LogLevel::Error, "[FCP] timeout");
    Emit(*fixture.ring, LogCategory::CMP, LogLevel::Debug, "[CMP] verbose detail");

    LogRecord records[8];

    LogRingQuery cmpOnly{};
    cmpOnly.categoryMask = 1U << static_cast<uint8_t>(LogCategory::CMP);
    auto result = fixture.ring->Query(cmpOnly, records, 8);
    ASSERT_EQ(result.recordCount, 2U);
    EXPECT_STREQ(records[0].message, "[CMP] iPCR clear");
    EXPECT_STREQ(records[1].message, "[CMP] verbose detail");

    LogRingQuery errorsOnly{};
    errorsOnly.maxLevel = static_cast<uint8_t>(LogLevel::Error);
    result = fixture.ring->Query(errorsOnly, records, 8);
    ASSERT_EQ(result.recordCount, 1U);
    EXPECT_STREQ(records[0].message, "[FCP] timeout");

    LogRingQuery containsQuery{};
    snprintf(containsQuery.contains, sizeof(containsQuery.contains), "iPCR");
    result = fixture.ring->Query(containsQuery, records, 8);
    ASSERT_EQ(result.recordCount, 1U);
    EXPECT_STREQ(records[0].message, "[CMP] iPCR clear");
}

TEST(LogRingTests, CursorContinuesWhereThePreviousQueryStopped) {
    RingFixture fixture(64);
    ASSERT_NE(fixture.ring, nullptr);

    for (int index = 0; index < 10; ++index) {
        Emit(*fixture.ring, LogCategory::AVC, LogLevel::Notice, "msg %d", index);
    }

    LogRecord records[4];
    auto first = fixture.ring->Query(LogRingQuery{}, records, 4);
    ASSERT_EQ(first.recordCount, 4U);
    EXPECT_STREQ(records[3].message, "msg 3");

    LogRingQuery next{};
    next.afterSequence = first.nextSequence;
    auto second = fixture.ring->Query(next, records, 4);
    ASSERT_EQ(second.recordCount, 4U);
    EXPECT_STREQ(records[0].message, "msg 4");
}

TEST(LogRingTests, StaleFutureCursorReplaysCurrentRingAndFlagsReset) {
    RingFixture fixture(64);
    ASSERT_NE(fixture.ring, nullptr);

    Emit(*fixture.ring, LogCategory::AVC, LogLevel::Notice, "fresh session");
    Emit(*fixture.ring, LogCategory::AVC, LogLevel::Notice, "still fresh");

    LogRingQuery query{};
    query.afterSequence = 9'999; // cursor from a prior ring instance
    LogRecord records[4];
    const auto result = fixture.ring->Query(query, records, 4);

    EXPECT_TRUE(result.cursorReset);
    EXPECT_EQ(result.recordCount, 2U);
    EXPECT_EQ(records[0].sequence, 1U);
    EXPECT_EQ(result.nextSequence, 2U);
}

TEST(LogRingTests, ScanBudgetBoundsOneCall) {
    RingFixture fixture(64);
    ASSERT_NE(fixture.ring, nullptr);

    for (int index = 0; index < 32; ++index) {
        Emit(*fixture.ring, LogCategory::Async, LogLevel::Notice, "msg %d", index);
    }

    LogRecord records[32];
    LogRingQuery query{};
    query.categoryMask = 1U << static_cast<uint8_t>(LogCategory::CMP); // no hits
    const auto result = fixture.ring->Query(query, records, 32, /*scanBudget=*/10);
    EXPECT_EQ(result.recordCount, 0U);
    EXPECT_EQ(result.scannedCount, 10U);
    EXPECT_EQ(result.nextSequence, 10U); // resumable
}

TEST(LogRingTests, TruncatesOverlongMessagesAndReportsFullLength) {
    RingFixture fixture(8);
    ASSERT_NE(fixture.ring, nullptr);

    const std::string longText(500, 'x');
    Emit(*fixture.ring, LogCategory::Audio, LogLevel::Notice, "%s", longText.c_str());

    LogRecord records[1];
    const auto result = fixture.ring->Query(LogRingQuery{}, records, 1);
    ASSERT_EQ(result.recordCount, 1U);
    EXPECT_EQ(records[0].messageLength, sizeof(records[0].message) - 1);
    EXPECT_EQ(strlen(records[0].message), sizeof(records[0].message) - 1);
}

TEST(LogRingTests, ConcurrentAppendsClaimUniqueSequences) {
    RingFixture fixture(4096);
    ASSERT_NE(fixture.ring, nullptr);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        writers.emplace_back([&ring = *fixture.ring, thread] {
            for (int index = 0; index < kPerThread; ++index) {
                Emit(ring, LogCategory::Isoch, LogLevel::Notice, "t%d #%d",
                     thread, index);
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }

    const auto stats = fixture.ring->Stats();
    EXPECT_EQ(stats.latestSequence, kThreads * kPerThread);
    EXPECT_EQ(stats.totalEmitted, kThreads * kPerThread);

    LogRecord records[256];
    LogRingQuery query{};
    uint64_t seen = 0;
    uint64_t lastSequence = 0;
    while (true) {
        const auto result = fixture.ring->Query(query, records, 256);
        if (result.recordCount == 0 &&
            result.nextSequence >= result.latestSequence) {
            break;
        }
        for (uint32_t index = 0; index < result.recordCount; ++index) {
            EXPECT_GT(records[index].sequence, lastSequence);
            lastSequence = records[index].sequence;
            ++seen;
        }
        query.afterSequence = result.nextSequence;
    }
    EXPECT_EQ(seen, kThreads * kPerThread);
}

TEST(LogRingTests, PacksRecordsIntoWireFormatWithinBudget) {
    RingFixture fixture(64);
    ASSERT_NE(fixture.ring, nullptr);

    Emit(*fixture.ring, LogCategory::CMP, LogLevel::Notice, "[CMP] first");
    Emit(*fixture.ring, LogCategory::FCP, LogLevel::Error, "[FCP] second");

    uint8_t buffer[4096];
    ASFW::Logging::LogRingQueryRequestWire request{};
    const size_t used =
        ASFW::Logging::PackLogRecords(*fixture.ring, request, buffer, sizeof(buffer));
    ASSERT_GT(used, sizeof(ASFW::Logging::LogRingQueryResponseHeaderWire));

    ASFW::Logging::LogRingQueryResponseHeaderWire header{};
    memcpy(&header, buffer, sizeof(header));
    EXPECT_EQ(header.recordCount, 2U);
    EXPECT_EQ(header.nextSequence, 2U);
    EXPECT_EQ(header.payloadBytes + sizeof(header), used);

    const uint8_t* cursor = buffer + sizeof(header);
    ASFW::Logging::LogRecordHeaderWire first{};
    memcpy(&first, cursor, sizeof(first));
    EXPECT_EQ(first.sequence, 1U);
    EXPECT_EQ(first.category, static_cast<uint8_t>(LogCategory::CMP));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(cursor + sizeof(first)),
                          first.messageLength),
              "[CMP] first");

    cursor += sizeof(first) + first.messageLength;
    ASFW::Logging::LogRecordHeaderWire second{};
    memcpy(&second, cursor, sizeof(second));
    EXPECT_EQ(second.sequence, 2U);
    EXPECT_EQ(second.level, static_cast<uint8_t>(LogLevel::Error));
}

TEST(LogRingTests, PackerStopsAtByteBudgetAndResumesFromCursor) {
    RingFixture fixture(64);
    ASSERT_NE(fixture.ring, nullptr);

    for (int index = 0; index < 12; ++index) {
        Emit(*fixture.ring, LogCategory::Async, LogLevel::Notice,
             "padding message number %d with a reasonably long body", index);
    }

    // Small budget: only a few records fit; the header cursor must let the
    // caller continue exactly where the packed stream stopped.
    uint8_t buffer[256];
    ASFW::Logging::LogRingQueryRequestWire request{};
    size_t used =
        ASFW::Logging::PackLogRecords(*fixture.ring, request, buffer, sizeof(buffer));
    ASFW::Logging::LogRingQueryResponseHeaderWire header{};
    memcpy(&header, buffer, sizeof(header));
    ASSERT_GT(header.recordCount, 0U);
    ASSERT_LT(header.recordCount, 12U);
    EXPECT_EQ(header.nextSequence, header.recordCount);

    uint32_t total = header.recordCount;
    while (total < 12) {
        request.afterSequence = header.nextSequence;
        used = ASFW::Logging::PackLogRecords(*fixture.ring, request, buffer,
                                             sizeof(buffer));
        ASSERT_GT(used, 0U);
        memcpy(&header, buffer, sizeof(header));
        if (header.recordCount == 0) {
            break;
        }
        total += header.recordCount;
    }
    EXPECT_EQ(total, 12U);
}

TEST(LogRingTests, PackerAppliesFiltersAndSkipsScannedRecords) {
    RingFixture fixture(64);
    ASSERT_NE(fixture.ring, nullptr);

    Emit(*fixture.ring, LogCategory::CMP, LogLevel::Notice, "[CMP] keep");
    Emit(*fixture.ring, LogCategory::Async, LogLevel::Notice, "[Async] drop");
    Emit(*fixture.ring, LogCategory::CMP, LogLevel::Notice, "[CMP] keep two");

    uint8_t buffer[4096];
    ASFW::Logging::LogRingQueryRequestWire request{};
    request.categoryMask = 1U << static_cast<uint8_t>(LogCategory::CMP);
    (void)ASFW::Logging::PackLogRecords(*fixture.ring, request, buffer, sizeof(buffer));

    ASFW::Logging::LogRingQueryResponseHeaderWire header{};
    memcpy(&header, buffer, sizeof(header));
    EXPECT_EQ(header.recordCount, 2U);
    EXPECT_EQ(header.nextSequence, 3U); // scanned past the filtered record
}

TEST(LogRingTests, PackerMarksStaleCursorAndReplaysCurrentHistory) {
    RingFixture fixture(64);
    ASSERT_NE(fixture.ring, nullptr);
    Emit(*fixture.ring, LogCategory::CMP, LogLevel::Notice, "fresh record");

    uint8_t buffer[4096];
    ASFW::Logging::LogRingQueryRequestWire request{};
    request.afterSequence = 4'096;
    (void)ASFW::Logging::PackLogRecords(*fixture.ring, request, buffer, sizeof(buffer));

    ASFW::Logging::LogRingQueryResponseHeaderWire header{};
    memcpy(&header, buffer, sizeof(header));
    EXPECT_EQ(header.recordCount, 1U);
    EXPECT_EQ(header.nextSequence, 1U);
    EXPECT_NE(header.flags & ASFW::Logging::kLogRingQueryFlagCursorReset, 0U);
}

TEST(LogRingTests, PackerContinuesPastSparseFilterScanBudget) {
    RingFixture fixture(10'000);
    ASSERT_NE(fixture.ring, nullptr);

    for (int index = 0; index < 8'300; ++index) {
        Emit(*fixture.ring, LogCategory::Async, LogLevel::Notice, "noise %d", index);
    }
    Emit(*fixture.ring, LogCategory::CMP, LogLevel::Error, "[CMP] retained match");

    uint8_t buffer[4096];
    ASFW::Logging::LogRingQueryRequestWire request{};
    request.categoryMask = 1U << static_cast<uint8_t>(LogCategory::CMP);
    size_t used =
        ASFW::Logging::PackLogRecords(*fixture.ring, request, buffer, sizeof(buffer));

    ASFW::Logging::LogRingQueryResponseHeaderWire header{};
    memcpy(&header, buffer, sizeof(header));
    // One drain call is bounded at 8192 examined slots. A sparse filter must
    // still provide a resumable empty page rather than claiming it caught up.
    ASSERT_EQ(used, sizeof(header));
    ASSERT_EQ(header.recordCount, 0U);
    ASSERT_EQ(header.nextSequence, 8'192U);
    ASSERT_LT(header.nextSequence, header.latestSequence);

    request.afterSequence = header.nextSequence;
    used = ASFW::Logging::PackLogRecords(*fixture.ring, request, buffer,
                                         sizeof(buffer));
    memcpy(&header, buffer, sizeof(header));
    ASSERT_GT(used, sizeof(header));
    ASSERT_EQ(header.recordCount, 1U);
    EXPECT_EQ(header.nextSequence, 8'301U);
}

TEST(LogRingTests, SanitizerStripsOsLogAnnotations) {
    char output[64];
    ASFW::Logging::SanitizeOsLogFormat(
        "[CMP] value=%{public}s hex=0x%{private}08x plain=%d", output,
        sizeof(output));
    EXPECT_STREQ(output, "[CMP] value=%s hex=0x%08x plain=%d");

    ASFW::Logging::SanitizeOsLogFormat("%100%{broken", output, sizeof(output));
    EXPECT_STREQ(output, "%100%"); // malformed annotation ends the copy safely
}

TEST(LogRingTests, RingLogToFormatsThroughSanitizer) {
    RingFixture fixture(16);
    ASSERT_NE(fixture.ring, nullptr);

    ASFW::Logging::RingLogTo(*fixture.ring, LogCategory::FCP, LogLevel::Notice,
                             "[FCP] node=%{public}s tLabel=%u", "0xffc0", 7U);

    LogRecord records[1];
    const auto result = fixture.ring->Query(LogRingQuery{}, records, 1);
    ASSERT_EQ(result.recordCount, 1U);
    EXPECT_STREQ(records[0].message, "[FCP] node=0xffc0 tLabel=7");
}

TEST(LogRingTests, StatsCountPerCategory) {
    RingFixture fixture(64);
    ASSERT_NE(fixture.ring, nullptr);

    Emit(*fixture.ring, LogCategory::CMP, LogLevel::Notice, "a");
    Emit(*fixture.ring, LogCategory::CMP, LogLevel::Notice, "b");
    Emit(*fixture.ring, LogCategory::DICE, LogLevel::Notice, "c");

    const auto stats = fixture.ring->Stats();
    EXPECT_EQ(stats.perCategory[static_cast<uint8_t>(LogCategory::CMP)], 2U);
    EXPECT_EQ(stats.perCategory[static_cast<uint8_t>(LogCategory::DICE)], 1U);
    EXPECT_EQ(stats.totalEmitted, 3U);
}

TEST(LogRingTests, ConcurrentContentionIsReportedRatherThanRacingTheSlot) {
    RingFixture fixture(1);
    ASSERT_NE(fixture.ring, nullptr);

    constexpr int kThreads = 8;
    constexpr int kPerThread = 1'000;
    std::vector<std::thread> writers;
    writers.reserve(kThreads);
    for (int thread = 0; thread < kThreads; ++thread) {
        writers.emplace_back([&ring = *fixture.ring, thread] {
            for (int index = 0; index < kPerThread; ++index) {
                Emit(ring, LogCategory::Isoch, LogLevel::Notice, "t%d #%d", thread, index);
            }
        });
    }
    for (auto& writer : writers) {
        writer.join();
    }

    const auto stats = fixture.ring->Stats();
    EXPECT_EQ(stats.latestSequence, kThreads * kPerThread);
    EXPECT_EQ(stats.totalEmitted + stats.droppedRecords, kThreads * kPerThread);
}

} // namespace
