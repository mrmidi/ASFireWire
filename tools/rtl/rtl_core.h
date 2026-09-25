// rtl_core.h -- platform-neutral core of the electrical round-trip latency tool.
//
// Everything that decides what a number MEANS lives here: the timeline audit,
// the measurement state machine, trial admission, the impulse detector, the
// aggregation and the JSON evidence record. None of it touches CoreAudio,
// CoreFoundation or mach, so the whole decision path builds and is unit-tested
// on any host (tests/tools/RtlCoreTests.cpp). The macOS shell (rtl_loopback.c)
// only moves samples and timestamps into rtl_engine_step() and prints.
//
// Vocabulary (RTL_raw, RTL_ts, scheduling distance, residual) is defined in
// documentation/LATENCY_VOCABULARY.md and tools/rtl/README.md.
#ifndef ASFW_TOOLS_RTL_CORE_H
#define ASFW_TOOLS_RTL_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTL_MAX_TRIALS   128
#define RTL_MAX_WINDOW   16384
#define RTL_MAX_SPANS    16
// A detected peak this close to the end of the capture window may be the
// window's edge rather than the impulse: the true arrival can lie past it.
#define RTL_EDGE_GUARD_FRAMES 16

// ----------------------------------------------------------- timeline audit
// Two failures invalidate different numbers and must be told apart:
//   dropped frames  the sample timeline and the wall clock BOTH advance past the
//                   frames delivered -> RTL_raw reads short, trial unusable.
//   re-anchor       the sample timeline jumps while the wall clock does not ->
//                   only RTL_ts is affected.
// Anything matching neither hypothesis is ambiguous, and ambiguous rejects.
typedef struct {
    int      have, haveSample;
    uint32_t prevN;
    double   prevSample, prevHost;
    uint32_t gapEvents, ambiguous, anchorEvents, noTsEvents;
    double   gapFrames, worstAnchor;
} rtl_audit_t;

// hostTol is the wall-clock disagreement (in frames) worth acting on.
void rtl_audit_step(rtl_audit_t *a, double sampleTime, int sampleValid,
                    double hostSec, uint32_t n, double sr, double hostTol);

// ------------------------------------------------------------------ trials
typedef enum {
    TRIAL_ACCEPTED = 0,
    TRIAL_GAP,
    TRIAL_AMBIGUOUS,
    TRIAL_ANCHOR,
    TRIAL_NO_TIMESTAMPS,
    TRIAL_OVERLOAD,        // the HAL reported a processor overload inside the trial
    TRIAL_NO_SIGNAL,
    TRIAL_WINDOW_EDGE,     // the peak sits on the capture-window edge
    TRIAL_VERDICTS
} rtl_verdict_t;

const char *rtl_verdict_name(rtl_verdict_t v);
// Stable machine-readable identifier (JSON), e.g. "lost_frames".
const char *rtl_verdict_id(rtl_verdict_t v);

typedef struct {
    double   emitAbs;      // absolute delivered-frame index of the impulse
    double   emitOt;       // outputTime.mSampleTime at that frame
    double   capAbs;       // absolute delivered-frame index of the first capture
    double   capIt;        // inputTime.mSampleTime at that frame
    uint32_t n;
    int      tsValid;      // both timestamps valid -- distinct from "zero"
    // Anomaly counters sampled at the trial's first [0] and last [1] callback.
    // Any change at all disqualifies the trial.
    uint32_t gapAt[2], ambAt[2], anchorAt[2], noTsAt[2], cbAt[2], ovlAt[2];
    float    win[RTL_MAX_WINDOW];
} rtl_trial_t;

// Admission depends on the timeline record, the overload record and the
// detector's two booleans -- never on a measured value.
rtl_verdict_t rtl_trial_verdict(const rtl_trial_t *t, int haveSignal, int atWindowEdge);

// ------------------------------------------------------------------ engine
enum { RTL_ST_WARMUP, RTL_ST_GAP, RTL_ST_CAPTURE, RTL_ST_DONE };

typedef struct {
    uint32_t n;                 // frames in this callback
    double   hostSec;           // wall clock, seconds -- never a driver timestamp
    double   itSample; int itValid;
    double   otSample; int otValid;
    float  (*input)(void *ctx, uint32_t frame);
    void    *ctx;
    float   *outSlot;           // where the impulse goes; NULL if unavailable
} rtl_step_t;

typedef struct {
    // configuration
    uint32_t trials, window, gapFrames, warmupFrames;
    float    amplitude;
    double   sampleRate;
    // Monotonic overload counter supplier (may be NULL = never overloaded).
    // Sampled at trial boundaries only, from the IO thread.
    uint32_t (*overloads)(void *ctx);
    void    *overloadsCtx;

    // state
    int      state;
    uint32_t trial;
    uint32_t remaining;
    double   pos;               // accumulated DELIVERED frames
    uint32_t cycles;
    uint32_t spans[RTL_MAX_SPANS], spanCount, minSpan;
    rtl_audit_t itAudit, otAudit;

    rtl_trial_t *t;             // caller-owned, capacity >= trials
} rtl_engine_t;

// Zeroes state and applies configuration. `trialStore` must hold `trials` items.
void rtl_engine_init(rtl_engine_t *e, rtl_trial_t *trialStore, uint32_t trials,
                     uint32_t window, uint32_t gapFrames, uint32_t warmupFrames,
                     float amplitude, double sampleRate);
// Returns 1 once every trial has been captured.
int  rtl_engine_step(rtl_engine_t *e, const rtl_step_t *s);

uint32_t rtl_engine_gap_events(const rtl_engine_t *e);
uint32_t rtl_engine_ambiguous(const rtl_engine_t *e);
uint32_t rtl_engine_anchor_events(const rtl_engine_t *e);
uint32_t rtl_engine_missing_ts(const rtl_engine_t *e);
double   rtl_engine_gap_frames(const rtl_engine_t *e);
double   rtl_engine_worst_anchor(const rtl_engine_t *e);

// ---------------------------------------------------------------- detector
typedef struct {
    int    ok;            // an impulse was found
    int    tsValid;
    int    atWindowEdge;  // ok, but peak within RTL_EDGE_GUARD_FRAMES of the end
    double rawFrames, tsFrames;
    double peak, noise, onset;
    int    inverted;
} rtl_result_t;

rtl_result_t rtl_analyse(const rtl_trial_t *t);

// ------------------------------------------------------------- aggregation
// Median of a COPY: the input is never reordered.
double rtl_median(const double *v, int n);

typedef struct {
    int    n;
    double median, mean, sd, min, max;
} rtl_stat_t;

rtl_stat_t rtl_stat(const double *v, int n);

// -------------------------------------------------------------- declared
#define RTL_DECLARED_MAX_MISSING 8
typedef struct {
    uint32_t io, safIn, safOut, latIn, latOut, strIn, strOut;
    // Names of the properties that would not read. A failed read leaves a zero
    // behind, which is legal for every field, so absence must be explicit.
    const char *missing[RTL_DECLARED_MAX_MISSING];
    int         nmissing;
} rtl_declared_t;

void     rtl_declared_need(rtl_declared_t *c, int ok, const char *what);
uint32_t rtl_declared_hw(const rtl_declared_t *c);     // latIn+latOut+strIn+strOut
uint32_t rtl_declared_sched(const rtl_declared_t *c);  // 2*io + safIn + safOut
// Apple's round-trip composition: scheduling + hardware. Returns 0 and sets
// *valid=0 when any declaration is missing (never a silent partial sum).
uint32_t rtl_declared_round_trip(const rtl_declared_t *c, int *valid);

// Auto window: 4x the declared round trip, at least 1024, clamped to max.
uint32_t rtl_auto_window(const rtl_declared_t *c);

// --------------------------------------------------------------- summary
typedef struct {
    rtl_verdict_t verdict;
    rtl_result_t  result;
    uint32_t      callbacks;  // callbacks the trial spanned
    uint32_t      overloads;  // overloads inside the trial
} rtl_trial_report_t;

typedef struct {
    uint32_t   trialsRun;
    int        accepted;
    uint32_t   tally[TRIAL_VERDICTS];
    double     minPeak, worstNoise;   // over trials where an impulse was found
    int        inverted;
    rtl_stat_t raw, onset, ts, sched; // accepted trials only (one population)
    uint32_t   declaredSched, declaredHw;
    int        residualValid;         // 0 if any declaration is missing
    double     residualFrames;        // median(RTL_ts) - declared hw
} rtl_summary_t;

// Fills `reports` (capacity e->trial) and `out`. Fails closed: only accepted
// trials contribute to any statistic.
void rtl_summarize(const rtl_engine_t *e, const rtl_declared_t *dc,
                   rtl_trial_report_t *reports, rtl_summary_t *out);

// ---------------------------------------------------------------- evidence
typedef struct {
    const char *tool;          // "rtl_loopback"
    const char *toolVersion;   // git SHA the tool was built from
    const char *timestampUtc;  // ISO-8601
    const char *osVersion;
    const char *deviceName;
    const char *deviceUid;
    const char *argv;          // joined command line
    double      sampleRate;
    uint32_t    bufferRequested;  // 0 = not requested
    uint32_t    bufferActual;
    uint32_t    window;
    uint32_t    trialsRequested;
    uint32_t    overloadsTotal;
    int         timedOut;
} rtl_provenance_t;

// Writes one JSON document. Returns 0 on success.
int rtl_write_json(FILE *f, const rtl_provenance_t *p, const rtl_declared_t *dc,
                   const rtl_engine_t *e, const rtl_trial_report_t *reports,
                   const rtl_summary_t *s);

// JSON string with escaping; NULL is written as null.
void rtl_json_string(FILE *f, const char *s);

// ---------------------------------------------------------------- selftest
// Detector, audit, aggregation and adversarial simulator, at the given rate.
// Prints a human report to `log` (may be NULL). Returns the failure count.
int rtl_selftest(double sampleRate, FILE *log);

// Adversarial simulator, exposed for tests. See rtl_core.c.
typedef struct {
    const char *name;
    double trueRtl;
    int    dropAt;    double dropFrames;
    int    anchorAt;  double anchorFrames;
    int    noTsAt;
    double jitterFrames;
    int    wideSpans;
    int    overloadAt;   // callback index at which one overload is reported
} rtl_sim_cfg_t;

// Returns the failure count for one configuration.
int rtl_sim_run(const rtl_sim_cfg_t *c, double sampleRate, FILE *log);
// Built-in configurations (count via *n).
const rtl_sim_cfg_t *rtl_sim_configs(size_t *n);

// Synthetic band-limited impulse used by the self-test and tests.
float rtl_synth_ir(double i, double delay, double amp);

#ifdef __cplusplus
}
#endif

#endif // ASFW_TOOLS_RTL_CORE_H
