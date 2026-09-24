// RtlCoreTests.cpp -- the analysis core of tools/rtl/rtl_loopback (FW-173).
//
// rtl_core is the part of the electrical round-trip tool that decides what a
// number means: timeline audit, trial admission, detector, aggregation and the
// JSON evidence record. It is platform-neutral C, so the whole decision path is
// tested here without CoreAudio or hardware.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include "rtl_core.h"
}

namespace {

std::unique_ptr<rtl_trial_t> MakeTrial() {
    return std::make_unique<rtl_trial_t>();
}

// A trial window containing one band-limited impulse at `delay` frames, with
// the same timestamp geometry the tool's self-test uses (900-frame skew).
void FillTrial(rtl_trial_t& t, double delay, double amp, uint32_t n = 4096) {
    std::memset(&t, 0, sizeof t);
    t.n = n;
    t.emitAbs = 1000.0;
    t.capAbs = 1000.0;
    t.emitOt = 5000.0;
    t.capIt = 5000.0 - 900.0;
    t.tsValid = 1;
    for (uint32_t i = 0; i < n; ++i) {
        t.win[i] = rtl_synth_ir(static_cast<double>(i), delay, amp);
    }
}

rtl_audit_t RunAudit(const std::vector<uint32_t>& spans, const std::vector<double>& sample,
                     const std::vector<double>& host, double sr) {
    rtl_audit_t a{};
    for (size_t i = 0; i < spans.size(); ++i) {
        rtl_audit_step(&a, sample[i], 1, host[i] / sr, spans[i], sr, 48.0);
    }
    return a;
}

std::string WriteJsonToString(const rtl_provenance_t& p, const rtl_declared_t& dc,
                              const rtl_engine_t& e, const rtl_trial_report_t* reports,
                              const rtl_summary_t& s) {
    FILE* f = std::tmpfile();
    EXPECT_NE(f, nullptr);
    EXPECT_EQ(rtl_write_json(f, &p, &dc, &e, reports, &s), 0);
    std::fflush(f);
    std::rewind(f);
    std::string out;
    char buf[4096];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

class RtlRates : public ::testing::TestWithParam<double> {};

} // namespace

// ------------------------------------------------------------------ detector

TEST(RtlDetector, RecoversKnownDelaysWithinResolutionFloor) {
    auto t = MakeTrial();
    for (double d : {137.0, 512.0, 733.4, 1024.25, 2999.75}) {
        for (double amp : {0.9, -0.9}) {
            FillTrial(*t, d, amp);
            const rtl_result_t r = rtl_analyse(t.get());
            ASSERT_TRUE(r.ok) << d;
            EXPECT_TRUE(r.tsValid);
            EXPECT_FALSE(r.atWindowEdge);
            EXPECT_NEAR(r.rawFrames, d, 0.2) << d;
            EXPECT_NEAR(r.tsFrames, d - 900.0, 0.2) << d;
            EXPECT_EQ(r.inverted, amp < 0 ? 1 : 0);
            EXPECT_LE(r.onset, d);  // onset is early by the pre-ring, never late
        }
    }
}

TEST(RtlDetector, RejectsSilenceInsteadOfFittingNoise) {
    auto t = MakeTrial();
    std::memset(t.get(), 0, sizeof *t);
    t->n = 4096;
    t->tsValid = 1;
    unsigned rng = 12345;
    for (uint32_t i = 0; i < t->n; ++i) {
        rng = rng * 1103515245u + 12345u;
        t->win[i] = static_cast<float>((((rng >> 16) & 0xFFFF) / 32768.0 - 1.0) * 1e-4);
    }
    const rtl_result_t r = rtl_analyse(t.get());
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(rtl_trial_verdict(t.get(), r.ok, r.atWindowEdge), TRIAL_NO_SIGNAL);
}

TEST(RtlDetector, ZeroResidualIsRetainedNotTreatedAsMissing) {
    auto t = MakeTrial();
    FillTrial(*t, 900.0, 0.9);
    const rtl_result_t r = rtl_analyse(t.get());
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(r.tsValid);
    EXPECT_NEAR(r.tsFrames, 0.0, 0.2);
}

TEST(RtlDetector, InvalidTimestampsAreFlaggedNotEncodedAsZero) {
    auto t = MakeTrial();
    FillTrial(*t, 512.0, 0.9);
    t->tsValid = 0;
    const rtl_result_t r = rtl_analyse(t.get());
    ASSERT_TRUE(r.ok);
    EXPECT_FALSE(r.tsValid);
    EXPECT_GT(r.rawFrames, 511.0);
    EXPECT_EQ(rtl_trial_verdict(t.get(), r.ok, r.atWindowEdge), TRIAL_NO_TIMESTAMPS);
}

TEST(RtlDetector, PeakOnWindowEdgeIsRejected) {
    // The 84 ms run on the midi branch sat exactly on the default 4096-frame
    // window edge: the true arrival may have been later.
    auto t = MakeTrial();
    FillTrial(*t, 4096.0 - 4.0, 0.9);
    const rtl_result_t r = rtl_analyse(t.get());
    EXPECT_TRUE(r.atWindowEdge);
    EXPECT_EQ(rtl_trial_verdict(t.get(), r.ok, r.atWindowEdge), TRIAL_WINDOW_EDGE);

    // A maximum on the very last sample cannot be interpolated at all: still an
    // edge, not "no signal".
    std::memset(t.get(), 0, sizeof *t);
    t->n = 4096;
    t->tsValid = 1;
    t->win[4095] = 0.9f;
    const rtl_result_t last = rtl_analyse(t.get());
    EXPECT_FALSE(last.ok);
    EXPECT_TRUE(last.atWindowEdge);

    // Just outside the guard band is measured normally.
    FillTrial(*t, 4096.0 - RTL_EDGE_GUARD_FRAMES - 30.0, 0.9);
    const rtl_result_t inside = rtl_analyse(t.get());
    EXPECT_TRUE(inside.ok);
    EXPECT_FALSE(inside.atWindowEdge);
}

// ---------------------------------------------------------------- audit

TEST_P(RtlRates, AuditVaryingSpanIsNotABreak) {
    const auto a = RunAudit({128, 64, 64, 64, 128, 64}, {0, 128, 192, 256, 320, 448},
                            {0, 128, 192, 256, 320, 448}, GetParam());
    EXPECT_EQ(a.gapEvents, 0u);
    EXPECT_EQ(a.anchorEvents, 0u);
    EXPECT_EQ(a.ambiguous, 0u);
}

TEST_P(RtlRates, AuditCatchesSmallCallbackDroppedAfterLargeOne) {
    const auto a = RunAudit({128, 64, 64, 64}, {0, 128, 192, 320}, {0, 128, 192, 320}, GetParam());
    EXPECT_EQ(a.gapEvents, 1u);
    EXPECT_NEAR(a.gapFrames, 64.0, 0.5);
    EXPECT_EQ(a.anchorEvents, 0u);
}

TEST_P(RtlRates, AuditReAnchorWhenWallClockDoesNotFollow) {
    const auto a = RunAudit({64, 64, 64, 64}, {0, 64, 128, 1128}, {0, 64, 128, 192}, GetParam());
    EXPECT_EQ(a.anchorEvents, 1u);
    EXPECT_EQ(a.gapEvents, 0u);
    EXPECT_NEAR(a.worstAnchor, 936.0, 0.5);
}

TEST_P(RtlRates, AuditDisagreeingClocksAreAmbiguous) {
    const auto a = RunAudit({64, 64, 64}, {0, 64, 128}, {0, 64, 256}, GetParam());
    EXPECT_EQ(a.ambiguous, 1u);
    EXPECT_EQ(a.gapEvents, 0u);
}

TEST_P(RtlRates, AuditLateCallbackWithinToleranceIsAccepted) {
    const auto a = RunAudit({64, 64, 64}, {0, 64, 128}, {0, 64, 148}, GetParam());
    EXPECT_EQ(a.ambiguous, 0u);
    EXPECT_EQ(a.gapEvents, 0u);
    EXPECT_EQ(a.anchorEvents, 0u);
}

TEST_P(RtlRates, AuditLossHiddenUnderReAnchorOfEitherSign) {
    for (double sign : {1.0, -1.0}) {
        const auto a = RunAudit({64, 64, 64, 64}, {0, 64, 128, 256 + sign * 936.0},
                                {0, 64, 128, 256}, GetParam());
        EXPECT_EQ(a.anchorEvents, 0u) << sign;
        EXPECT_TRUE(a.ambiguous || a.gapEvents) << sign;
    }
}

TEST(RtlAudit, MissingTimestampDropsTheWitnessAndIsCounted) {
    rtl_audit_t a{};
    rtl_audit_step(&a, 0, 1, 0.0, 64, 48000.0, 48.0);
    rtl_audit_step(&a, 0, 0, 64 / 48000.0, 64, 48000.0, 48.0);  // invalid
    // The next delta would span two callbacks; it must not be read as a gap.
    rtl_audit_step(&a, 192, 1, 128 / 48000.0, 64, 48000.0, 48.0);
    EXPECT_EQ(a.noTsEvents, 1u);
    EXPECT_EQ(a.gapEvents, 0u);
    EXPECT_EQ(a.anchorEvents, 0u);
}

// ------------------------------------------------------------ admission

TEST(RtlAdmission, EveryCauseRejectsAndOrderIsStable) {
    rtl_trial_t t{};
    t.tsValid = 1;
    EXPECT_EQ(rtl_trial_verdict(&t, 1, 0), TRIAL_ACCEPTED);

    t.gapAt[1] = 1;
    t.ambAt[1] = 1;
    EXPECT_EQ(rtl_trial_verdict(&t, 1, 0), TRIAL_GAP);  // gap outranks the rest
    t.gapAt[1] = 0;
    EXPECT_EQ(rtl_trial_verdict(&t, 1, 0), TRIAL_AMBIGUOUS);
    t.ambAt[1] = 0;
    t.anchorAt[1] = 1;
    EXPECT_EQ(rtl_trial_verdict(&t, 1, 0), TRIAL_ANCHOR);
    t.anchorAt[1] = 0;
    t.tsValid = 0;
    EXPECT_EQ(rtl_trial_verdict(&t, 1, 0), TRIAL_NO_TIMESTAMPS);
    t.tsValid = 1;
    t.ovlAt[0] = 5;
    t.ovlAt[1] = 6;
    EXPECT_EQ(rtl_trial_verdict(&t, 1, 0), TRIAL_OVERLOAD);
    t.ovlAt[1] = 5;
    EXPECT_EQ(rtl_trial_verdict(&t, 0, 0), TRIAL_NO_SIGNAL);
    EXPECT_EQ(rtl_trial_verdict(&t, 1, 1), TRIAL_WINDOW_EDGE);
}

TEST(RtlAdmission, VerdictNamesAndIdsAreDefinedForEveryVerdict) {
    for (int v = 0; v < TRIAL_VERDICTS; ++v) {
        const auto verdict = static_cast<rtl_verdict_t>(v);
        EXPECT_STRNE(rtl_verdict_name(verdict), "invalid");
        const std::string id = rtl_verdict_id(verdict);
        EXPECT_EQ(id.find(' '), std::string::npos) << id;  // machine-readable
    }
    EXPECT_STREQ(rtl_verdict_name(TRIAL_VERDICTS), "invalid");
}

// ------------------------------------------------------ adversarial engine

TEST_P(RtlRates, AdversarialSimulatorNeverAdmitsAnInjectedFault) {
    size_t n = 0;
    const rtl_sim_cfg_t* cfgs = rtl_sim_configs(&n);
    ASSERT_GE(n, 13u);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_EQ(rtl_sim_run(&cfgs[i], GetParam(), nullptr), 0) << cfgs[i].name;
    }
}

TEST_P(RtlRates, FullSelfTestPasses) {
    EXPECT_EQ(rtl_selftest(GetParam(), nullptr), 0);
}

INSTANTIATE_TEST_SUITE_P(Rates, RtlRates, ::testing::Values(44100.0, 48000.0, 96000.0));

TEST(RtlEngine, ZeroFrameCallbackIsIgnored) {
    std::vector<rtl_trial_t> store(1);
    rtl_engine_t e;
    rtl_engine_init(&e, store.data(), 1, 256, 64, 64, 0.9f, 48000.0);
    rtl_step_t s{};
    s.n = 0;
    EXPECT_EQ(rtl_engine_step(&e, &s), 0);
    EXPECT_EQ(e.cycles, 0u);
}

TEST(RtlEngine, InitClampsTrialsAndWindow) {
    std::vector<rtl_trial_t> store(1);
    rtl_engine_t e;
    rtl_engine_init(&e, store.data(), 100000, 100000, 64, 64, 0.9f, 48000.0);
    EXPECT_EQ(e.trials, static_cast<uint32_t>(RTL_MAX_TRIALS));
    EXPECT_EQ(e.window, static_cast<uint32_t>(RTL_MAX_WINDOW));
    EXPECT_EQ(e.state, RTL_ST_WARMUP);
}

// ---------------------------------------------------------- aggregation

TEST(RtlAggregation, MedianDoesNotReorderItsInput) {
    const double v[] = {5.0, 1.0, 4.0, 2.0, 3.0};
    double copy[5];
    std::memcpy(copy, v, sizeof v);
    EXPECT_DOUBLE_EQ(rtl_median(copy, 5), 3.0);
    EXPECT_EQ(std::memcmp(copy, v, sizeof v), 0);
    const double even[] = {4.0, 1.0, 3.0, 2.0};
    EXPECT_DOUBLE_EQ(rtl_median(even, 4), 2.5);
    EXPECT_DOUBLE_EQ(rtl_median(nullptr, 0), 0.0);
}

TEST(RtlAggregation, StatComputesSampleSd) {
    const double v[] = {2.0, 4.0, 4.0, 4.0, 5.0, 5.0, 7.0, 9.0};
    const rtl_stat_t s = rtl_stat(v, 8);
    EXPECT_EQ(s.n, 8);
    EXPECT_DOUBLE_EQ(s.mean, 5.0);
    EXPECT_NEAR(s.sd, std::sqrt(32.0 / 7.0), 1e-12);
    EXPECT_DOUBLE_EQ(s.min, 2.0);
    EXPECT_DOUBLE_EQ(s.max, 9.0);
    EXPECT_DOUBLE_EQ(s.median, 4.5);
}

TEST(RtlAggregation, SchedulingDistanceIsPairedPerTrial) {
    const double raw[] = {1000, 1000, 1400, 1400};
    const double ts[] = {600, 600};
    const double sched[] = {400, 400};
    EXPECT_NEAR(rtl_median(sched, 2), 400.0, 0.5);
    EXPECT_GT(std::fabs(rtl_median(raw, 4) - rtl_median(ts, 2) - 400.0), 0.5);
}

// ------------------------------------------------------------- declared

TEST(RtlDeclared, RoundTripIsWithheldWhenAnythingIsMissing) {
    rtl_declared_t dc{};
    dc.io = 64;
    dc.safIn = 50;
    dc.safOut = 50;
    dc.latIn = 40;
    dc.latOut = 67;
    dc.strIn = 0;
    dc.strOut = 0;
    int valid = 0;
    EXPECT_EQ(rtl_declared_sched(&dc), 2u * 64u + 50u + 50u);
    EXPECT_EQ(rtl_declared_hw(&dc), 107u);
    EXPECT_EQ(rtl_declared_round_trip(&dc, &valid), 228u + 107u);
    EXPECT_EQ(valid, 1);

    rtl_declared_need(&dc, 0, "output device latency");
    EXPECT_EQ(dc.nmissing, 1);
    EXPECT_EQ(rtl_declared_round_trip(&dc, &valid), 0u);
    EXPECT_EQ(valid, 0);
    rtl_declared_need(&dc, 1, "not missing");
    EXPECT_EQ(dc.nmissing, 1);
}

TEST(RtlDeclared, MissingListNeverOverflows) {
    rtl_declared_t dc{};
    for (int i = 0; i < RTL_DECLARED_MAX_MISSING + 5; ++i) rtl_declared_need(&dc, 0, "x");
    EXPECT_EQ(dc.nmissing, RTL_DECLARED_MAX_MISSING + 5);  // counted, but stored bounded
}

TEST(RtlDeclared, AutoWindowCoversFourTimesTheDeclaredRoundTrip) {
    rtl_declared_t dc{};
    dc.io = 64;
    dc.safIn = dc.safOut = 50;
    dc.latIn = 40;
    dc.latOut = 67;
    EXPECT_EQ(rtl_auto_window(&dc), 4u * 335u);

    rtl_declared_t tiny{};
    EXPECT_EQ(rtl_auto_window(&tiny), 1024u);

    rtl_declared_t huge{};
    huge.io = 4096;
    huge.latIn = huge.latOut = 4096;
    EXPECT_EQ(rtl_auto_window(&huge), static_cast<uint32_t>(RTL_MAX_WINDOW));

    rtl_declared_t missing{};
    rtl_declared_need(&missing, 0, "buffer frame size");
    EXPECT_EQ(rtl_auto_window(&missing), static_cast<uint32_t>(RTL_MAX_WINDOW));
}

// -------------------------------------------------- summary + evidence JSON

namespace {

struct SimRun {
    std::vector<rtl_trial_t> store;
    rtl_engine_t e{};
    uint32_t overloads = 0;
};

uint32_t Overloads(void* ctx) { return static_cast<SimRun*>(ctx)->overloads; }

// Drive the engine over a clean timeline with a delay-line loopback: the input
// is the output delayed by `rtl` frames.
void DriveLoopback(SimRun& run, uint32_t trials, double rtl, int overloadAt = -1) {
    run.store.assign(trials, rtl_trial_t{});
    rtl_engine_init(&run.e, run.store.data(), trials, 1024, 256, 256, 0.9f, 48000.0);
    run.e.overloads = Overloads;
    run.e.overloadsCtx = &run;

    std::vector<double> emits;
    double pos = 0;
    struct Ctx { const std::vector<double>* emits; double base; double rtl; } ctx{&emits, 0, rtl};
    auto input = [](void* c, uint32_t frame) -> float {
        auto* x = static_cast<Ctx*>(c);
        double v = 0;
        for (double e : *x->emits) v += rtl_synth_ir(x->base + frame, e + x->rtl, 0.9);
        return static_cast<float>(v);
    };
    for (int i = 1; i <= 2000 && run.e.state != RTL_ST_DONE; ++i) {
        if (i == overloadAt) run.overloads++;
        float slot = 0;
        ctx.base = pos;
        rtl_step_t s{};
        s.n = 64;
        s.hostSec = pos / 48000.0;
        s.itSample = pos;
        s.itValid = 1;
        s.otSample = pos + 900.0;
        s.otValid = 1;
        s.input = input;
        s.ctx = &ctx;
        s.outSlot = &slot;
        rtl_engine_step(&run.e, &s);
        if (slot != 0) emits.push_back(pos);
        pos += 64;
    }
}

} // namespace

TEST(RtlSummary, AcceptedTrialsFormOnePopulationAndResidualNeedsDeclarations) {
    SimRun run;
    DriveLoopback(run, 6, 300.0);
    ASSERT_EQ(run.e.trial, 6u);

    rtl_declared_t dc{};
    dc.io = 64;
    dc.latIn = 100;
    dc.latOut = 100;
    std::vector<rtl_trial_report_t> reports(6);
    rtl_summary_t s{};
    rtl_summarize(&run.e, &dc, reports.data(), &s);

    EXPECT_EQ(s.trialsRun, 6u);
    EXPECT_EQ(s.accepted, 6);
    EXPECT_EQ(s.raw.n, 6);
    EXPECT_EQ(s.ts.n, 6);
    EXPECT_EQ(s.sched.n, 6);
    EXPECT_NEAR(s.raw.median, 300.0, 0.25);
    // otSample leads itSample by 900 frames, so RTL_ts = 300 - 900.
    EXPECT_NEAR(s.ts.median, -600.0, 0.25);
    EXPECT_NEAR(s.sched.median, 900.0, 0.25);
    ASSERT_TRUE(s.residualValid);
    EXPECT_NEAR(s.residualFrames, -600.0 - 200.0, 0.25);

    rtl_declared_need(&dc, 0, "input safety offset");
    rtl_summarize(&run.e, &dc, reports.data(), &s);
    EXPECT_FALSE(s.residualValid);
}

TEST(RtlSummary, OverloadRejectsOnlyTheTrialItLandedIn) {
    SimRun run;
    // Warmup 256 frames = 4 callbacks; the emit is callback 5, capture runs to
    // callback 20. Callback 10 is inside trial 0.
    DriveLoopback(run, 3, 300.0, /*overloadAt=*/10);
    std::vector<rtl_trial_report_t> reports(3);
    rtl_summary_t s{};
    rtl_summarize(&run.e, nullptr, reports.data(), &s);
    EXPECT_EQ(reports[0].verdict, TRIAL_OVERLOAD);
    EXPECT_EQ(reports[0].overloads, 1u);
    EXPECT_EQ(reports[1].verdict, TRIAL_ACCEPTED);
    EXPECT_EQ(reports[2].verdict, TRIAL_ACCEPTED);
    EXPECT_EQ(s.accepted, 2);
    EXPECT_EQ(s.tally[TRIAL_OVERLOAD], 1u);
    EXPECT_FALSE(s.residualValid);  // no declarations supplied
}

TEST(RtlEvidence, JsonCarriesProvenanceEveryTrialAndNoNaN) {
    SimRun run;
    DriveLoopback(run, 3, 300.0, 10);
    rtl_declared_t dc{};
    dc.io = 64;
    rtl_declared_need(&dc, 0, "output \"device\" latency");  // must be escaped
    std::vector<rtl_trial_report_t> reports(3);
    rtl_summary_t s{};
    rtl_summarize(&run.e, &dc, reports.data(), &s);

    rtl_provenance_t p{};
    p.tool = "rtl_loopback";
    p.toolVersion = "abc123";
    p.timestampUtc = "2026-09-24T00:00:00Z";
    p.osVersion = "27.0";
    p.deviceName = "ASFW Test\tDevice";
    p.deviceUid = nullptr;
    p.argv = "rtl_loopback --measure";
    p.sampleRate = 48000.0;
    p.window = 1024;
    p.trialsRequested = 3;
    p.overloadsTotal = 1;

    const std::string json = WriteJsonToString(p, dc, run.e, reports.data(), s);
    EXPECT_NE(json.find("\"schema\": \"asfw.rtl_loopback.v1\""), std::string::npos);
    EXPECT_NE(json.find("\"tool_version\": \"abc123\""), std::string::npos);
    EXPECT_NE(json.find("\"device_uid\": null"), std::string::npos);
    EXPECT_NE(json.find("ASFW Test\\tDevice"), std::string::npos);
    EXPECT_NE(json.find("output \\\"device\\\" latency"), std::string::npos);
    EXPECT_NE(json.find("\"verdict\": \"processor_overload\""), std::string::npos);
    EXPECT_NE(json.find("\"processor_overload\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"residual_frames\": null"), std::string::npos);
    // JSON has no NaN/Infinity literals; a value position must never hold one.
    for (const char* bad : {": nan", ": -nan", ": inf", ": -inf", ": NaN", ": Infinity"}) {
        EXPECT_EQ(json.find(bad), std::string::npos) << bad;
    }
    // Three trial objects.
    size_t count = 0;
    for (size_t at = json.find("\"index\":"); at != std::string::npos;
         at = json.find("\"index\":", at + 1)) {
        ++count;
    }
    EXPECT_EQ(count, 3u);
}

TEST(RtlEvidence, JsonStringEscapesControlCharacters) {
    FILE* f = std::tmpfile();
    ASSERT_NE(f, nullptr);
    rtl_json_string(f, "a\"b\\c\nd\x01");
    rtl_json_string(f, nullptr);
    std::rewind(f);
    char buf[128] = {};
    const size_t n = std::fread(buf, 1, sizeof buf - 1, f);
    std::fclose(f);
    EXPECT_EQ(std::string(buf, n), "\"a\\\"b\\\\c\\nd\\u0001\"null");
}
