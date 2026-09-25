// HalCoreTests.cpp -- the maths and evidence record of tools/halprobe (FW-172).

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "hal_core.h"
}

namespace {

// A HAL timeline observed at 50 Hz starting at a realistic uptime.
struct Timeline {
    std::vector<double> host, sample;
};

Timeline MakeTimeline(double uptimeSec, double rate, double ppm, int n, double pollSec = 0.02) {
    Timeline t;
    const double actual = rate * (1.0 + ppm * 1e-6);
    for (int i = 0; i < n; ++i) {
        const double h = uptimeSec + i * pollSec;
        t.host.push_back(h);
        t.sample.push_back(123456.0 + (h - uptimeSec) * actual);
    }
    return t;
}

std::string ReadAll(FILE* f) {
    std::fflush(f);
    std::rewind(f);
    std::string out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    return out;
}

} // namespace

TEST(HalFit, RecoversPpmAtLargeUptimeWithoutCancellation) {
    // ~11.6 days of uptime: the textbook n*Sxx - Sx^2 form loses the slope
    // here; the centred fit must recover a 37 ppm error to well under 0.1 ppm.
    for (double uptime : {0.0, 1.0e5, 1.0e6, 1.0e7}) {
        const Timeline t = MakeTimeline(uptime, 48000.0, 37.1, 1000);
        const hal_fit_t f = hal_fit_line(t.host.data(), t.sample.data(), 1000);
        ASSERT_TRUE(f.ok) << uptime;
        EXPECT_NEAR(hal_ppm(f.slope, 48000.0), 37.1, 0.1) << uptime;
        EXPECT_LT(f.worstResidual, 0.01) << uptime;
        EXPECT_NEAR(f.intercept, 123456.0, 1e-3) << uptime;
        EXPECT_NEAR(f.spanSeconds, 999 * 0.02, 1e-6);
    }
}

TEST(HalFit, NaiveFormulaWouldHaveFailedAtThisUptime) {
    // Documents why the centring matters: reproduce the old formula.
    const Timeline t = MakeTimeline(1.0e7, 48000.0, 37.1, 1000);
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    const int n = 1000;
    for (int i = 0; i < n; ++i) {
        sx += t.host[i];
        sy += t.sample[i];
        sxx += t.host[i] * t.host[i];
        sxy += t.host[i] * t.sample[i];
    }
    const double naive = (n * sxy - sx * sy) / (n * sxx - sx * sx);
    EXPECT_GT(std::fabs(hal_ppm(naive, 48000.0) - 37.1), 1.0);
}

TEST(HalFit, DegenerateInputsAreNotOk) {
    const double x[] = {5.0, 5.0, 5.0};
    const double y[] = {1.0, 2.0, 3.0};
    EXPECT_FALSE(hal_fit_line(x, y, 3).ok);
    EXPECT_FALSE(hal_fit_line(x, y, 1).ok);
    EXPECT_FALSE(hal_fit_line(nullptr, y, 3).ok);
    EXPECT_DOUBLE_EQ(hal_ppm(48000.0, 0.0), 0.0);
}

TEST(HalFit, ResidualReportsAnInjectedStep) {
    Timeline t = MakeTimeline(5.0e5, 48000.0, 0.0, 500);
    for (int i = 250; i < 500; ++i) t.sample[i] += 288.0;  // a lap-sized re-anchor
    const hal_fit_t f = hal_fit_line(t.host.data(), t.sample.data(), 500);
    ASSERT_TRUE(f.ok);
    EXPECT_GT(f.worstResidual, 50.0);
}

TEST(HalJumps, ClassifiesBackwardAndForwardReAnchors) {
    Timeline t = MakeTimeline(1.0e6, 48000.0, 0.0, 100);
    const double thresh = 48000.0 * 0.005;
    hal_jumps_t clean = hal_classify_jumps(t.host.data(), t.sample.data(), 100, 48000.0, thresh);
    EXPECT_EQ(clean.backward, 0u);
    EXPECT_EQ(clean.forward, 0u);

    for (int i = 40; i < 100; ++i) t.sample[i] += 1536.0;   // forward re-anchor
    for (int i = 70; i < 100; ++i) t.sample[i] -= 5000.0;   // then backwards
    const hal_jumps_t j = hal_classify_jumps(t.host.data(), t.sample.data(), 100, 48000.0, thresh);
    EXPECT_EQ(j.forward, 1u);
    EXPECT_NEAR(j.worstForward, 1536.0, 1e-6);
    EXPECT_EQ(j.backward, 1u);
    EXPECT_NEAR(j.worstBackward, 960.0 - 5000.0, 1e-6);  // one poll of advance minus the step
}

TEST(HalJumps, JitterBelowThresholdIsNotAReAnchor) {
    Timeline t = MakeTimeline(1.0e6, 48000.0, 0.0, 100);
    for (int i = 1; i < 100; i += 2) t.sample[i] += 30.0;  // +-30 frames of reporting jitter
    const hal_jumps_t j =
        hal_classify_jumps(t.host.data(), t.sample.data(), 100, 48000.0, 48000.0 * 0.005);
    EXPECT_EQ(j.forward, 0u);
    EXPECT_EQ(j.backward, 0u);
}

TEST(HalIo, RecordsDistinctSpansAndIntervalStatistics) {
    hal_iostats_t s{};
    for (uint32_t frames : {512u, 512u, 256u, 512u, 256u}) hal_iostats_record(&s, frames);
    EXPECT_EQ(s.cycles, 5u);
    ASSERT_EQ(s.nspans, 2u);
    EXPECT_EQ(s.spans[0], 512u);
    EXPECT_EQ(s.spans[1], 256u);

    for (uint32_t i = 0; i < 200; ++i) hal_iostats_record(&s, 1000 + i);
    EXPECT_EQ(s.nspans, static_cast<uint32_t>(HAL_MAX_SPANS));  // bounded

    const double host[] = {1.0e6, 1.0e6 + 0.010, 1.0e6 + 0.020, 1.0e6 + 0.031};
    const rtl_stat_t st = hal_callback_intervals_us(host, 4);
    EXPECT_EQ(st.n, 3);
    EXPECT_NEAR(st.median, 10000.0, 1.0);
    EXPECT_NEAR(st.max, 11000.0, 1.0);
    EXPECT_EQ(hal_callback_intervals_us(host, 1).n, 0);
}

TEST(HalSweep, VerdictsAndSizes) {
    EXPECT_EQ(hal_sweep_verdict(1, 1, 64, 64), HAL_SWEEP_OK);
    EXPECT_EQ(hal_sweep_verdict(1, 1, 64, 128), HAL_SWEEP_COERCED);
    EXPECT_EQ(hal_sweep_verdict(0, 1, 64, 512), HAL_SWEEP_REJECTED);
    EXPECT_EQ(hal_sweep_verdict(1, 0, 64, 0), HAL_SWEEP_UNREADABLE);
    EXPECT_STREQ(hal_sweep_verdict_id(HAL_SWEEP_COERCED), "coerced");

    uint32_t out[HAL_MAX_SWEEP_ROWS];
    // The advertised range, not a fixed 32..4096 list.
    int n = hal_sweep_sizes(15.0, 576.0, out, HAL_MAX_SWEEP_ROWS);
    const std::vector<uint32_t> want{15, 16, 32, 64, 128, 256, 512, 576};
    ASSERT_EQ(static_cast<size_t>(n), want.size());
    for (int i = 0; i < n; ++i) EXPECT_EQ(out[i], want[i]) << i;

    n = hal_sweep_sizes(64.0, 64.0, out, HAL_MAX_SWEEP_ROWS);
    ASSERT_EQ(n, 1);
    EXPECT_EQ(out[0], 64u);
    EXPECT_EQ(hal_sweep_sizes(0.0, 64.0, out, HAL_MAX_SWEEP_ROWS), 0);
    EXPECT_EQ(hal_sweep_sizes(64.0, 32.0, out, HAL_MAX_SWEEP_ROWS), 0);
    EXPECT_LE(hal_sweep_sizes(1.0, 1.0e9, out, 4), 4);
}

TEST(HalEvidence, JsonMarksUnreadValuesAsNullAndWithholdsPrediction) {
    hal_device_report_t d{};
    d.name = "ASFW \"Duet\"";
    d.uid = nullptr;
    d.objectId = 77;
    d.nominalRateValid = 1;
    d.nominalRate = 48000.0;
    d.ztsPeriod = {1536, 1};
    d.bufferFrames = {0, 0};  // NOT READ
    d.clockAlgorithm = {2, 1};
    rtl_declared_need(&d.declared, 0, "buffer frame size");
    d.clockRan = 1;
    d.clockSamples = 1000;
    const Timeline t = MakeTimeline(1.0e6, 48000.0, 37.1, 1000);
    d.clockFit = hal_fit_line(t.host.data(), t.sample.data(), 1000);
    d.sweepRan = 1;
    d.sweepRows = 1;
    d.sweep[0].requested = 64;
    d.sweep[0].readBack = 128;
    d.sweep[0].verdict = HAL_SWEEP_COERCED;
    d.sweep[0].ioOk = 1;
    hal_iostats_record(&d.sweep[0].io, 128);
    d.sweepRestoredTo = 512;
    d.sweepRestoreOk = 1;

    const hal_provenance_t p{"abc", "2026-09-24T00:00:00Z", "27.0", "hal_geometry -d ASFW"};
    FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(hal_write_json(f, &p, &d, 1), 0);
    const std::string json = ReadAll(f);
    std::fclose(f);

    EXPECT_NE(json.find("\"schema\": \"asfw.hal_geometry.v1\""), std::string::npos);
    EXPECT_NE(json.find("\"name\": \"ASFW \\\"Duet\\\"\""), std::string::npos);
    EXPECT_NE(json.find("\"uid\": null"), std::string::npos);
    EXPECT_NE(json.find("\"zts_period_frames\": 1536"), std::string::npos);
    EXPECT_NE(json.find("\"buffer_frames\": null"), std::string::npos);
    EXPECT_NE(json.find("\"predicted_round_trip_frames\": null"), std::string::npos);
    EXPECT_NE(json.find("\"reference\": \"hal_published_timeline\""), std::string::npos);
    EXPECT_NE(json.find("\"ppm_vs_nominal\": 37.1"), std::string::npos);
    EXPECT_NE(json.find("\"verdict\": \"coerced\""), std::string::npos);
    EXPECT_NE(json.find("\"observed_spans\": [128]"), std::string::npos);
    EXPECT_NE(json.find("\"io\": null"), std::string::npos);
    for (const char* bad : {": nan", ": -nan", ": inf", ": -inf"}) {
        EXPECT_EQ(json.find(bad), std::string::npos) << bad;
    }
}

TEST(HalEvidence, EmptyDeviceListIsValid) {
    const hal_provenance_t p{"abc", "t", "os", "argv"};
    FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    ASSERT_EQ(hal_write_json(f, &p, nullptr, 0), 0);
    const std::string json = ReadAll(f);
    std::fclose(f);
    EXPECT_NE(json.find("\"devices\": [\n  ]"), std::string::npos);
}
