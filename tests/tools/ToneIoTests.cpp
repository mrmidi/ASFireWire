// ToneIoTests.cpp -- the per-callback IO record behind rtl_loopback --tone and
// the app's tone test. It decides which callback delivered a glitch and whether
// CoreAudio itself misbehaved near it, so a glitch is not blamed on the driver
// when the HAL dropped a callback.

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

extern "C" {
#include "tone_io.h"
}

namespace {

// `count` back-to-back callbacks of `n` frames each, timestamps valid, no
// overloads.
std::vector<tone_cb_t> CleanRun(uint32_t count, uint32_t n) {
    std::vector<tone_cb_t> cb(count);
    for (uint32_t k = 0; k < count; k++) {
        cb[k].hostSec = k * 0.001;
        cb[k].itSample = 1000.0 + static_cast<double>(k) * n;
        cb[k].captureStart = static_cast<uint64_t>(k) * n;
        cb[k].n = n;
        cb[k].overloads = 0;
        cb[k].itValid = 1;
    }
    return cb;
}

}  // namespace

TEST(ToneIo, EmptyRunHasNoCallbackAndNoSummary) {
    EXPECT_EQ(tone_cb_for(nullptr, 0, 0), nullptr);
    const tone_cb_t none{};
    EXPECT_EQ(tone_cb_for(&none, 0, 5), nullptr);
    uint32_t gaps = 99, missing = 99;
    tone_io_summary(nullptr, 0, &gaps, &missing);
    EXPECT_EQ(gaps, 0u);
    EXPECT_EQ(missing, 0u);
}

TEST(ToneIo, CallbackForNamesTheCallbackThatDeliveredTheFrame) {
    const auto cb = CleanRun(10, 64);
    const auto count = static_cast<uint32_t>(cb.size());
    // First and last frame of each callback land on that callback.
    for (uint32_t k = 0; k < count; k++) {
        EXPECT_EQ(tone_cb_for(cb.data(), count, static_cast<uint64_t>(k) * 64), &cb[k]);
        EXPECT_EQ(tone_cb_for(cb.data(), count, static_cast<uint64_t>(k) * 64 + 63), &cb[k]);
    }
    // Past the end stays on the last record rather than running off the array.
    EXPECT_EQ(tone_cb_for(cb.data(), count, 10'000), &cb[count - 1]);
}

TEST(ToneIo, CallbackForHandlesASingleRecord) {
    const auto cb = CleanRun(1, 128);
    EXPECT_EQ(tone_cb_for(cb.data(), 1, 0), &cb[0]);
    EXPECT_EQ(tone_cb_for(cb.data(), 1, 127), &cb[0]);
}

TEST(ToneIo, CleanIoReportsNothingNearby) {
    const auto cb = CleanRun(10, 64);
    const auto count = static_cast<uint32_t>(cb.size());
    for (uint32_t k = 0; k < count; k++)
        EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[k]), "");
    uint32_t gaps = 99, missing = 99;
    tone_io_summary(cb.data(), count, &gaps, &missing);
    EXPECT_EQ(gaps, 0u);
    EXPECT_EQ(missing, 0u);
}

TEST(ToneIo, SampleTimeGapIsFoundWithinTwoCallbacksOnly) {
    auto cb = CleanRun(12, 64);
    // The HAL skipped a callback's worth of sample time before callback 6.
    for (uint32_t k = 6; k < cb.size(); k++) cb[k].itSample += 64;
    const auto count = static_cast<uint32_t>(cb.size());

    uint32_t gaps = 0, missing = 0;
    tone_io_summary(cb.data(), count, &gaps, &missing);
    EXPECT_EQ(gaps, 1u);
    EXPECT_EQ(missing, 0u);

    // The gap is between callbacks 5 and 6: callbacks 4..7 are within +-2 of
    // a pair that includes it; callback 2 (window 0..4) and 9 (7..11) are not.
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[4]), "input sample-time gap nearby");
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[6]), "input sample-time gap nearby");
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[8]), "input sample-time gap nearby");
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[2]), "");
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[10]), "");
}

TEST(ToneIo, OverloadOutranksGapAndMissingTimestamp) {
    auto cb = CleanRun(8, 64);
    cb[3].itValid = 0;
    for (uint32_t k = 5; k < cb.size(); k++) cb[k].itSample += 64;
    for (uint32_t k = 4; k < cb.size(); k++) cb[k].overloads = 1;
    const auto count = static_cast<uint32_t>(cb.size());
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[4]), "overload nearby");
}

TEST(ToneIo, MissingTimestampIsCountedAndDoesNotCountAsAGap) {
    auto cb = CleanRun(8, 64);
    cb[4].itValid = 0;
    cb[4].itSample = 0.0;  // a callback without a timestamp carries no sample time
    const auto count = static_cast<uint32_t>(cb.size());

    uint32_t gaps = 0, missing = 0;
    tone_io_summary(cb.data(), count, &gaps, &missing);
    EXPECT_EQ(missing, 1u);
    // Neighbours of an invalid callback cannot be compared, so no gap is claimed.
    EXPECT_EQ(gaps, 0u);
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[4]), "missing timestamp nearby");
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[6]), "missing timestamp nearby");
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[7]), "");
}

TEST(ToneIo, WindowIsClampedAtBothEnds) {
    auto cb = CleanRun(3, 64);
    cb[2].overloads = 1;
    const auto count = static_cast<uint32_t>(cb.size());
    // Callback 0's window is 0..2; callback 2 changed overloads relative to 1.
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[0]), "overload nearby");
    EXPECT_STREQ(tone_io_near(cb.data(), count, &cb[2]), "overload nearby");
}

TEST(ToneIo, NullInputsAreHarmless) {
    const auto cb = CleanRun(4, 64);
    const tone_cb_t outside = cb[0];
    EXPECT_STREQ(tone_io_near(cb.data(), static_cast<uint32_t>(cb.size()), nullptr), "");
    EXPECT_STREQ(tone_io_near(nullptr, 0, &outside), "");
}
