#include <gtest/gtest.h>

#include "AudioWire/AMDTP/TimingUtils.hpp"

using namespace ASFW::Timing;

namespace {

static constexpr uint32_t EncodedCycleTimer(uint32_t seconds,
                                            uint32_t cycle,
                                            uint32_t offset) {
    return ((seconds & 0x7F) << ASFW::Timing::kCycleTimerSecondsShift)
         | ((cycle & 0x1FFF) << ASFW::Timing::kCycleTimerCyclesShift)
         | (offset & 0x0FFF);
}

struct TimebaseGuard {
    mach_timebase_info_data_t old;

    TimebaseGuard(uint32_t numer, uint32_t denom) : old(gHostTimebaseInfo) {
        gHostTimebaseInfo.numer = numer;
        gHostTimebaseInfo.denom = denom;
    }

    ~TimebaseGuard() {
        gHostTimebaseInfo = old;
    }
};

} // namespace

// 1. Constants sanity
TEST(TimingUtils, ConstantsMatchFireWireAnd48kBlockingAssumptions) {
    EXPECT_EQ(kTicksPerCycle, 3072u);
    EXPECT_EQ(kCyclesPerSecond, 8000u);
    EXPECT_EQ(kTicksPerSecond, 24'576'000ull);
    EXPECT_EQ(kNanosPerCycle, 125'000ull);

    EXPECT_EQ(kTicksPerSample48k, 512u);
    EXPECT_EQ(kSytInterval48k, 8u);
    EXPECT_EQ(kSytPacketStepTicks48k, 4096u);

    EXPECT_EQ(kEightSecondTicks, 196'608'000);
    EXPECT_EQ(kSytFieldDomainTicks, 49'152);
}

// 2. tstampToOffsets(seconds, cycle, offset)
TEST(TimingUtils, TstampToOffsetsCollapsesSecondsCyclesAndOffset) {
    EXPECT_EQ(tstampToOffsets(0, 0, 0), 0);
    EXPECT_EQ(tstampToOffsets(0, 1, 0), 3072);
    EXPECT_EQ(tstampToOffsets(0, 0, 512), 512);
    EXPECT_EQ(tstampToOffsets(1, 0, 0), 24'576'000);
    EXPECT_EQ(tstampToOffsets(0, 7999, 3071), 24'575'999);
    EXPECT_EQ(tstampToOffsets(2, 3, 4), 49'161'220);
}

// 3. decodeCycleTimer()
TEST(TimingUtils, DecodeCycleTimerExtractsSecondsCycleAndOffset) {
    const uint32_t encoded = EncodedCycleTimer(5, 1234, 2048);
    const auto fields = decodeCycleTimer(encoded);
    EXPECT_EQ(fields.seconds, 5u);
    EXPECT_EQ(fields.cycle, 1234u);
    EXPECT_EQ(fields.offset, 2048u);
}

TEST(TimingUtils, DecodeCycleTimerHandlesMaxFields) {
    const uint32_t encoded = EncodedCycleTimer(127, 7999, 4095);
    const auto fields = decodeCycleTimer(encoded);
    EXPECT_EQ(fields.seconds, 127u);
    EXPECT_EQ(fields.cycle, 7999u);
    EXPECT_EQ(fields.offset, 4095u);
}

// 4. tstampToOffsets(CycleTimerFields)
TEST(TimingUtils, TstampToOffsetsFromFieldsMatchesScalarOverload) {
    CycleTimerFields fields{
        .seconds = 3,
        .cycle = 456,
        .offset = 789,
    };
    EXPECT_EQ(tstampToOffsets(fields), tstampToOffsets(3, 456, 789));
}

// 5. encodedTstampToOffsets()
TEST(TimingUtils, EncodedTstampToOffsetsPreservesFullCycleTimerDomain) {
    const uint32_t encoded = EncodedCycleTimer(3, 4002, 128);
    EXPECT_EQ(encodedTstampToOffsets(encoded), tstampToOffsets(3, 4002, 128));
}

TEST(TimingUtils, EncodedTstampToOffsetsIsNotCycleOnly) {
    const uint32_t encoded = EncodedCycleTimer(3, 4002, 128);
    EXPECT_NE(encodedTstampToOffsets(encoded), tstampToOffsets(0, 4002, 128));
}

// 6. normalizeOffsetDomain()
TEST(TimingUtils, NormalizeOffsetDomainWrapsToEightSecondDomain) {
    EXPECT_EQ(normalizeOffsetDomain(0), 0);
    EXPECT_EQ(normalizeOffsetDomain(kEightSecondTicks), 0);
    EXPECT_EQ(normalizeOffsetDomain(kEightSecondTicks + 123), 123);
    EXPECT_EQ(normalizeOffsetDomain(-1), kEightSecondTicks - 1);
    EXPECT_EQ(normalizeOffsetDomain(-kEightSecondTicks - 5), kEightSecondTicks - 5);
}

// 7. extOffsetDiff(a, b)
TEST(TimingUtils, ExtOffsetDiffReturnsSignedShortestDelta) {
    EXPECT_EQ(extOffsetDiff(4096, 0), 4096);
    EXPECT_EQ(extOffsetDiff(0, 4096), -4096);
}

TEST(TimingUtils, ExtOffsetDiffHandlesEightSecondWrapForward) {
    EXPECT_EQ(extOffsetDiff(100, kEightSecondTicks - 200), 300);
}

TEST(TimingUtils, ExtOffsetDiffHandlesEightSecondWrapBackward) {
    EXPECT_EQ(extOffsetDiff(kEightSecondTicks - 200, 100), -300);
}

TEST(TimingUtils, ExtOffsetDiffHandlesHalfWrapBoundary) {
    const int64_t half = kEightSecondTicks / 2;
    EXPECT_EQ(extOffsetDiff(half, 0), half);
    EXPECT_EQ(extOffsetDiff(half + 1, 0), -(half - 1));
}

// 7b. extOffsetDiff1s(a, b)
TEST(TimingUtils, ExtOffsetDiff1sSignedShortestPath) {
    EXPECT_EQ(extOffsetDiff1s(28158, 24062), 4096);
    EXPECT_EQ(extOffsetDiff1s(24062, 28158), -4096);
    EXPECT_EQ(extOffsetDiff1s(100, 100), 0);
}

TEST(TimingUtils, ExtOffsetDiff1sWrapsAcrossOneSecondDomain) {
    constexpr int64_t kOneSecondTicks = 24'576'000LL;
    EXPECT_EQ(extOffsetDiff1s(10, kOneSecondTicks - 10), 20);
    EXPECT_EQ(extOffsetDiff1s(kOneSecondTicks - 10, 10), -20);
}

TEST(TimingUtils, ExtOffsetDiff1sHandlesMultiSecondDivergedTicks) {
    const int64_t phaseTicks = 70'885'968;
    const int64_t projected = 16'349'184;

    // Standard 8-second diff returns huge/wrong lead
    const int64_t lead8s = extOffsetDiff(phaseTicks, projected);
    EXPECT_EQ(lead8s, 54'536'784);

    // 1-second modulo diff correctly resolves shortest path modulo 1-second
    const int64_t lead1s = extOffsetDiff1s(phaseTicks, projected);
    EXPECT_EQ(lead1s, 5'384'784);
}

// 8. sytToFieldTicks()
TEST(TimingUtils, SytToFieldTicksCollapsesCycleNibbleAndOffset) {
    EXPECT_EQ(sytToFieldTicks(0x0000), 0);
    EXPECT_EQ(sytToFieldTicks(0x1000), 3072);
    EXPECT_EQ(sytToFieldTicks(0x1ABC), 3072 + 0x0ABC);
    EXPECT_EQ(sytToFieldTicks(0xFFFF), 15 * 3072 + 0x0FFF);
}

// 9. SYTDiffInOffsets(new, old)
TEST(TimingUtils, SYTDiffInOffsetsComputesSignedForwardAndBackwardDeltas) {
    EXPECT_EQ(SYTDiffInOffsets(0x1000, 0x0000), 3072);
    EXPECT_EQ(SYTDiffInOffsets(0x0000, 0x1000), -3072);
}

TEST(TimingUtils, SYTDiffInOffsetsHandlesSixteenCycleWrapForward) {
    EXPECT_EQ(SYTDiffInOffsets(0x0000, 0xF000), 3072);
}

TEST(TimingUtils, SYTDiffInOffsetsHandlesSixteenCycleWrapBackward) {
    EXPECT_EQ(SYTDiffInOffsets(0xF000, 0x0000), -3072);
}

TEST(TimingUtils, SYTDiffInOffsetsRecognizes48kBlockingStepAcrossCycleNibbleWrap) {
    EXPECT_EQ(SYTDiffInOffsets(0xF2B0, 0xDAB0), 4096);
    EXPECT_EQ(SYTDiffInOffsets(0x06B0, 0xF2B0), 4096);
    EXPECT_EQ(SYTDiffInOffsets(0x1AB0, 0x06B0), 4096);
}

// 10. extendTstamp(baseCycle, syt)
TEST(TimingUtils, ExtendTstampUsesBaseCycleWindow) {
    EXPECT_EQ(extendTstamp(100, 0x6123), tstampToOffsets(0, 102, 0x123));
}

TEST(TimingUtils, ExtendTstampCarriesToNextSixteenCycleWindow) {
    EXPECT_EQ(extendTstamp(111, 0x1123), tstampToOffsets(0, 113, 0x123));
}

TEST(TimingUtils, ExtendTstampCycleOnlyMayReturnCycleBeyondOneSecond) {
    EXPECT_EQ(extendTstamp(7998, 0x1123), tstampToOffsets(0, 8001, 0x123));
}

// 11. extendTstampFromCycleTimer(baseCycleTimer, syt)
TEST(TimingUtils, ExtendTstampFromCycleTimerPreservesSecondsField) {
    const uint32_t base = EncodedCycleTimer(7, 100, 0);
    EXPECT_EQ(extendTstampFromCycleTimer(base, 0x6123), tstampToOffsets(7, 102, 0x123));
}

TEST(TimingUtils, ExtendTstampFromCycleTimerCarriesToNextSixteenCycleWindow) {
    const uint32_t base = EncodedCycleTimer(7, 111, 0);
    EXPECT_EQ(extendTstampFromCycleTimer(base, 0x1123), tstampToOffsets(7, 113, 0x123));
}

TEST(TimingUtils, ExtendTstampFromCycleTimerCarriesAcrossCycleSecondBoundary) {
    const uint32_t base = EncodedCycleTimer(7, 7998, 0);
    EXPECT_EQ(extendTstampFromCycleTimer(base, 0x1123), tstampToOffsets(8, 1, 0x123));
}

TEST(TimingUtils, ExtendTstampFromCycleTimerWrapsSecondsAt128) {
    const uint32_t base = EncodedCycleTimer(127, 7998, 0);
    EXPECT_EQ(extendTstampFromCycleTimer(base, 0x1123), tstampToOffsets(0, 1, 0x123));
}

TEST(TimingUtils, ExtendTstampFromCycleTimerIsNotCycleOnlyProjection) {
    const uint32_t base = EncodedCycleTimer(5, 4000, 0);
    const int64_t full = extendTstampFromCycleTimer(base, 0x2123);
    const int64_t cycleOnly = extendTstamp(4000, 0x2123);
    EXPECT_EQ(full, tstampToOffsets(5, 4002, 0x123));
    EXPECT_EQ(cycleOnly, tstampToOffsets(0, 4002, 0x123));
    EXPECT_NE(full, cycleOnly);
}

// 12. encodedFWTimeToNanos()
TEST(TimingUtils, EncodedFWTimeToNanosConvertsSecondsCyclesAndOffset) {
    EXPECT_EQ(encodedFWTimeToNanos(EncodedCycleTimer(0, 0, 0)), 0ull);
    EXPECT_EQ(encodedFWTimeToNanos(EncodedCycleTimer(1, 0, 0)), 1'000'000'000ull);
    EXPECT_EQ(encodedFWTimeToNanos(EncodedCycleTimer(0, 1, 0)), 125'000ull);
    EXPECT_EQ(encodedFWTimeToNanos(EncodedCycleTimer(0, 0, 1536)), 62'500ull);
    EXPECT_EQ(encodedFWTimeToNanos(EncodedCycleTimer(2, 3, 1536)),
              2'000'000'000ull + 3 * 125'000ull + 62'500ull);
}

// 13. nanosToEncodedFWTime()
TEST(TimingUtils, NanosToEncodedFWTimeConvertsRepresentableValues) {
    EXPECT_EQ(nanosToEncodedFWTime(0), EncodedCycleTimer(0, 0, 0));
    EXPECT_EQ(nanosToEncodedFWTime(1'000'000'000ull), EncodedCycleTimer(1, 0, 0));
    EXPECT_EQ(nanosToEncodedFWTime(125'000ull), EncodedCycleTimer(0, 1, 0));
    EXPECT_EQ(nanosToEncodedFWTime(62'500ull), EncodedCycleTimer(0, 0, 1536));
}

TEST(TimingUtils, NanosToEncodedFWTimeWrapsAt128Seconds) {
    EXPECT_EQ(nanosToEncodedFWTime(128ull * kNanosPerSecond), EncodedCycleTimer(0, 0, 0));
    EXPECT_EQ(nanosToEncodedFWTime(129ull * kNanosPerSecond), EncodedCycleTimer(1, 0, 0));
}

TEST(TimingUtils, FireWireNanosRoundTripForRepresentableValue) {
    const uint64_t nanos =
        2ull * kNanosPerSecond +
        3ull * kNanosPerCycle +
        62'500ull;
    const uint32_t encoded = nanosToEncodedFWTime(nanos);
    EXPECT_EQ(encoded, EncodedCycleTimer(2, 3, 1536));
    EXPECT_EQ(encodedFWTimeToNanos(encoded), nanos);
}

// 14. deltaFWTimeNanos(a, b)
TEST(TimingUtils, DeltaFWTimeNanosComputesSignedDelta) {
    const uint32_t a = nanosToEncodedFWTime(2ull * kNanosPerSecond);
    const uint32_t b = nanosToEncodedFWTime(1ull * kNanosPerSecond);
    EXPECT_EQ(deltaFWTimeNanos(a, b), 1'000'000'000ll);
    EXPECT_EQ(deltaFWTimeNanos(b, a), -1'000'000'000ll);
}

TEST(TimingUtils, DeltaFWTimeNanosUsesShortestPathAcross128SecondWrapForward) {
    const uint32_t a = nanosToEncodedFWTime(100'000'000ull); // 0.1s
    const uint32_t b = nanosToEncodedFWTime(127ull * kNanosPerSecond + 900'000'000ull); // 127.9s
    EXPECT_EQ(deltaFWTimeNanos(a, b), 200'000'000ll);
}

TEST(TimingUtils, DeltaFWTimeNanosUsesShortestPathAcross128SecondWrapBackward) {
    const uint32_t a = nanosToEncodedFWTime(127ull * kNanosPerSecond + 900'000'000ull);
    const uint32_t b = nanosToEncodedFWTime(100'000'000ull);
    EXPECT_EQ(deltaFWTimeNanos(a, b), -200'000'000ll);
}

// 15. normalizeToFWTimeRange()
TEST(TimingUtils, NormalizeToFWTimeRangeWrapsPositiveValues) {
    EXPECT_EQ(normalizeToFWTimeRange(0), 0ull);
    EXPECT_EQ(normalizeToFWTimeRange(kFWTimeWrapNanos), 0ull);
    EXPECT_EQ(normalizeToFWTimeRange(kFWTimeWrapNanos + 42), 42ull);
}

TEST(TimingUtils, NormalizeToFWTimeRangeWrapsNegativeValues) {
    EXPECT_EQ(normalizeToFWTimeRange(-1), uint64_t(kFWTimeWrapNanos - 1));
    EXPECT_EQ(normalizeToFWTimeRange(-kFWTimeWrapNanos - 7), uint64_t(kFWTimeWrapNanos - 7));
}

// 16. hostTicksToNanos() and nanosToHostTicks()
TEST(TimingUtils, HostTicksToNanosReturnsZeroWhenTimebaseUninitialized) {
    TimebaseGuard guard(0, 0);
    EXPECT_EQ(hostTicksToNanos(12345), 0ull);
}

TEST(TimingUtils, HostTicksToNanosUsesTimebaseRatio) {
    TimebaseGuard guard(2, 1);
    EXPECT_EQ(hostTicksToNanos(50), 100ull);
}

TEST(TimingUtils, NanosToHostTicksReturnsZeroWhenTimebaseUninitialized) {
    TimebaseGuard guard(0, 0);
    EXPECT_EQ(nanosToHostTicks(12345), 0ull);
}

TEST(TimingUtils, NanosToHostTicksUsesInverseTimebaseRatio) {
    TimebaseGuard guard(2, 1);
    EXPECT_EQ(nanosToHostTicks(100), 50ull);
}

TEST(TimingUtils, HostTickConversionsHandleLargeValuesWithoutOverflowingIntermediate) {
    TimebaseGuard guard(125, 3);
    const uint64_t ticks = 1'000'000'000'000ull;
    const uint64_t nanos = hostTicksToNanos(ticks);
    EXPECT_GT(nanos, ticks);
    // The 125/3 ratio truncates on both divisions, so the round trip may lose a
    // couple of ticks. The point of this test is the 128-bit intermediate, not
    // bit-exact reversibility — allow a small tolerance.
    const uint64_t roundTrip = nanosToHostTicks(nanos);
    EXPECT_GE(roundTrip, ticks - 2);
    EXPECT_LE(roundTrip, ticks + 2);
}

// 17. initializeHostTimebase()
TEST(TimingUtils, InitializeHostTimebaseIsIdempotentWhenAlreadyInitialized) {
    TimebaseGuard guard(1, 1);
    EXPECT_TRUE(initializeHostTimebase());
    EXPECT_EQ(gHostTimebaseInfo.numer, 1u);
    EXPECT_EQ(gHostTimebaseInfo.denom, 1u);
}

TEST(TimingUtilsIntegration, InitializeHostTimebasePopulatesTimebase) {
    TimebaseGuard guard(0, 0);
    EXPECT_TRUE(initializeHostTimebase());
    EXPECT_NE(gHostTimebaseInfo.numer, 0u);
    EXPECT_NE(gHostTimebaseInfo.denom, 0u);
}
