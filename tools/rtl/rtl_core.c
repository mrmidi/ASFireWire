// rtl_core.c -- platform-neutral core of the electrical round-trip latency tool.
// See rtl_core.h for the contract and tools/rtl/README.md for the meaning of
// every number. Nothing here may include CoreAudio, CoreFoundation or mach.
#include "rtl_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RTL_PI 3.14159265358979323846

// ----------------------------------------------------------- timeline audit
// The gap itself is detected exactly, from integer frame counts. The wall clock
// is consulted only to choose between "dropped" and "re-anchor", which differ
// by the full magnitude of the gap, so no threshold is tuned against jitter.
//
// The advance is compared against the frame count of the callback BEFORE it:
// comparing against the current span invents a break on every size change, and
// callback size does vary.
void rtl_audit_step(rtl_audit_t *a, double sampleTime, int sampleValid,
                    double hostSec, uint32_t n, double sr, double hostTol) {
    if (a->have) {
        const double delivered = (double)a->prevN;
        const double hostGap = (hostSec - a->prevHost) * sr - delivered;
        if (sampleValid && a->haveSample) {
            const double sampleGap = (sampleTime - a->prevSample) - delivered;
            if (fabs(sampleGap) > 0.5) {
                double tol = 0.5 * fabs(sampleGap);
                if (tol < 0.25 * delivered) tol = 0.25 * delivered;
                if (fabs(hostGap - sampleGap) <= tol) {
                    a->gapEvents++;
                    a->gapFrames += sampleGap;
                } else if (fabs(hostGap) <= hostTol) {
                    // hostTol, NOT tol: this is the only verdict that preserves
                    // RTL_raw, so it must assert the wall clock saw nothing at
                    // the jitter scale. Scaling it to the jump would let a real
                    // loss hide inside a large re-anchor.
                    a->anchorEvents++;
                    if (fabs(sampleGap) > fabs(a->worstAnchor)) a->worstAnchor = sampleGap;
                } else {
                    a->ambiguous++;
                }
            } else if (fabs(hostGap) > hostTol) {
                // The sample timeline says continuous, the wall clock says not:
                // conflicting evidence. Jitter is one-sided and inside hostTol.
                a->ambiguous++;
            }
        } else if (fabs(hostGap) > hostTol) {
            // No sample-time witness: the wall clock alone cannot separate a
            // gap from jitter.
            a->ambiguous++;
        }
    }
    a->prevHost = hostSec;
    a->prevN    = n;
    a->have     = 1;
    // After an invalid timestamp the next delta would span two callbacks.
    if (sampleValid) { a->prevSample = sampleTime; a->haveSample = 1; }
    else             { a->haveSample = 0; a->noTsEvents++; }
}

// ------------------------------------------------------------------ verdicts
static const char *const kVerdictName[TRIAL_VERDICTS] = {
    "accepted", "lost frames", "unclassified", "re-anchor",
    "no timestamps", "processor overload", "no signal", "window edge",
};
static const char *const kVerdictId[TRIAL_VERDICTS] = {
    "accepted", "lost_frames", "unclassified", "re_anchor",
    "no_timestamps", "processor_overload", "no_signal", "window_edge",
};

const char *rtl_verdict_name(rtl_verdict_t v) {
    return (v >= 0 && v < TRIAL_VERDICTS) ? kVerdictName[v] : "invalid";
}
const char *rtl_verdict_id(rtl_verdict_t v) {
    return (v >= 0 && v < TRIAL_VERDICTS) ? kVerdictId[v] : "invalid";
}

// ONE admission decision, in one place. A trial is admitted only if nothing
// anomalous happened anywhere inside it. Every classification is a rejection,
// so no classifier verdict can widen what gets in.
rtl_verdict_t rtl_trial_verdict(const rtl_trial_t *t, int haveSignal, int atWindowEdge) {
    if (t->gapAt[1]    != t->gapAt[0])    return TRIAL_GAP;
    if (t->ambAt[1]    != t->ambAt[0])    return TRIAL_AMBIGUOUS;
    if (t->anchorAt[1] != t->anchorAt[0]) return TRIAL_ANCHOR;
    if (t->noTsAt[1]   != t->noTsAt[0] || !t->tsValid) return TRIAL_NO_TIMESTAMPS;
    // A HAL overload means an IO cycle ran late or was skipped; whether it cost
    // frames is exactly what the audit cannot always prove, so fail closed.
    if (t->ovlAt[1]    != t->ovlAt[0])    return TRIAL_OVERLOAD;
    if (!haveSignal) return TRIAL_NO_SIGNAL;
    if (atWindowEdge) return TRIAL_WINDOW_EDGE;
    return TRIAL_ACCEPTED;
}

// ------------------------------------------------------------------ engine
uint32_t rtl_engine_gap_events(const rtl_engine_t *e)    { return e->itAudit.gapEvents    + e->otAudit.gapEvents; }
uint32_t rtl_engine_ambiguous(const rtl_engine_t *e)     { return e->itAudit.ambiguous    + e->otAudit.ambiguous; }
uint32_t rtl_engine_anchor_events(const rtl_engine_t *e) { return e->itAudit.anchorEvents + e->otAudit.anchorEvents; }
uint32_t rtl_engine_missing_ts(const rtl_engine_t *e)    { return e->itAudit.noTsEvents   + e->otAudit.noTsEvents; }
double   rtl_engine_gap_frames(const rtl_engine_t *e)    { return e->itAudit.gapFrames    + e->otAudit.gapFrames; }
double   rtl_engine_worst_anchor(const rtl_engine_t *e) {
    return fabs(e->itAudit.worstAnchor) > fabs(e->otAudit.worstAnchor)
               ? e->itAudit.worstAnchor : e->otAudit.worstAnchor;
}

static uint32_t engine_overloads(const rtl_engine_t *e) {
    return e->overloads ? e->overloads(e->overloadsCtx) : 0u;
}

static void trial_mark(const rtl_engine_t *e, rtl_trial_t *t, int which) {
    t->gapAt[which]    = rtl_engine_gap_events(e);
    t->ambAt[which]    = rtl_engine_ambiguous(e);
    t->anchorAt[which] = rtl_engine_anchor_events(e);
    t->noTsAt[which]   = rtl_engine_missing_ts(e);
    t->cbAt[which]     = e->cycles;
    t->ovlAt[which]    = engine_overloads(e);
}

void rtl_engine_init(rtl_engine_t *e, rtl_trial_t *trialStore, uint32_t trials,
                     uint32_t window, uint32_t gapFrames, uint32_t warmupFrames,
                     float amplitude, double sampleRate) {
    memset(e, 0, sizeof *e);
    if (trials > RTL_MAX_TRIALS) trials = RTL_MAX_TRIALS;
    if (window > RTL_MAX_WINDOW) window = RTL_MAX_WINDOW;
    e->t = trialStore;
    e->trials = trials;
    e->window = window;
    e->gapFrames = gapFrames;
    e->warmupFrames = warmupFrames;
    e->amplitude = amplitude;
    e->sampleRate = sampleRate;
    e->remaining = warmupFrames;
    e->state = RTL_ST_WARMUP;
}

// The measurement state machine. The simulator drives exactly this code, so the
// adversarial self-test exercises what the hardware path runs.
int rtl_engine_step(rtl_engine_t *e, const rtl_step_t *s) {
    const uint32_t n = s->n;
    if (!n) return e->state == RTL_ST_DONE;

    e->cycles++;
    if (!e->minSpan || n < e->minSpan) e->minSpan = n;
    if (e->spanCount < RTL_MAX_SPANS) {
        int seen = 0;
        for (uint32_t i = 0; i < e->spanCount; i++) if (e->spans[i] == n) seen = 1;
        if (!seen) e->spans[e->spanCount++] = n;
    }

    // Three quarters of the smallest callback: a lost callback costs at least a
    // whole one, while scheduling jitter stays well below that.
    double hostTol = 0.75 * (double)e->minSpan;
    if (hostTol < 8.0) hostTol = 8.0;
    rtl_audit_step(&e->itAudit, s->itSample, s->itValid, s->hostSec, n, e->sampleRate, hostTol);
    rtl_audit_step(&e->otAudit, s->otSample, s->otValid, s->hostSec, n, e->sampleRate, hostTol);

    switch (e->state) {
    case RTL_ST_WARMUP:
    case RTL_ST_GAP:
        if (e->remaining > n) { e->remaining -= n; break; }
        e->remaining = 0;
        if (e->trial >= e->trials || !e->t) { e->state = RTL_ST_DONE; break; }
        {
            rtl_trial_t *t = &e->t[e->trial];
            if (s->outSlot) *s->outSlot = e->amplitude;
            t->emitAbs = e->pos;
            t->capAbs  = e->pos;
            t->emitOt  = s->otValid ? s->otSample : 0.0;
            t->capIt   = s->itValid ? s->itSample : 0.0;
            t->tsValid = s->otValid && s->itValid;
            t->n       = 0;
            trial_mark(e, t, 0);
            e->state = RTL_ST_CAPTURE;
        }
        /* fall through */
    case RTL_ST_CAPTURE: {
        rtl_trial_t *t = &e->t[e->trial];
        for (uint32_t f = 0; f < n && t->n < e->window; f++)
            t->win[t->n++] = s->input ? s->input(s->ctx, f) : 0.0f;
        if (t->n >= e->window) {
            trial_mark(e, t, 1);
            e->trial++;
            e->remaining = e->gapFrames;
            e->state = (e->trial >= e->trials) ? RTL_ST_DONE : RTL_ST_GAP;
        }
        break;
    }
    default: break;
    }

    e->pos += (double)n;
    return e->state == RTL_ST_DONE;
}

// ---------------------------------------------------------------- detector
rtl_result_t rtl_analyse(const rtl_trial_t *t) {
    rtl_result_t r;
    memset(&r, 0, sizeof r);
    if (t->n < 16) return r;

    const uint32_t nf = t->n < 32 ? t->n : 32;
    double acc = 0;
    for (uint32_t i = 0; i < nf; i++) acc += (double)t->win[i] * t->win[i];
    r.noise = sqrt(acc / nf);

    uint32_t pk = 0;
    double best = 0;
    for (uint32_t i = 0; i < t->n; i++) {
        const double a = fabs((double)t->win[i]);
        if (a > best) { best = a; pk = i; }
    }
    r.peak = best;
    r.inverted = t->win[pk] < 0;
    if (best < 1e-4 || best < r.noise * 8.0 || pk == 0 || pk + 1 >= t->n) {
        // A maximum on the very last sample is the window edge, not an impulse
        // we can place; say so rather than "no signal".
        if (best >= 1e-4 && best >= r.noise * 8.0 && pk + 1 >= t->n) r.atWindowEdge = 1;
        return r;
    }

    // Converter anti-alias filters are close to linear phase: the response
    // rings symmetrically about the group delay, so the peak is the estimator
    // and the onset is early by the pre-ring. Report both; trust the peak.
    const double y0 = fabs((double)t->win[pk - 1]);
    const double y1 = best;
    const double y2 = fabs((double)t->win[pk + 1]);
    const double den = y0 - 2.0 * y1 + y2;
    const double frac = fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;

    uint32_t on = pk;
    while (on > 0 && fabs((double)t->win[on - 1]) > best * 0.1) on--;
    r.onset = (double)on;

    const double detect = (double)pk + frac;
    r.rawFrames = (t->capAbs + detect) - t->emitAbs;
    // Validity is a flag, never a sentinel: a residual of exactly zero is the
    // most interesting result this tool can produce.
    r.tsValid = t->tsValid;
    if (r.tsValid) r.tsFrames = (t->capIt + detect) - t->emitOt;
    r.atWindowEdge = (pk + RTL_EDGE_GUARD_FRAMES >= t->n);
    r.ok = 1;
    return r;
}

// ------------------------------------------------------------- aggregation
static int cmpd(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

double rtl_median(const double *v, int n) {
    if (n <= 0) return 0.0;
    double stackBuf[RTL_MAX_TRIALS];
    double *w = n <= RTL_MAX_TRIALS ? stackBuf : (double *)malloc(sizeof(double) * (size_t)n);
    if (!w) return 0.0;
    memcpy(w, v, sizeof(double) * (size_t)n);
    qsort(w, (size_t)n, sizeof(double), cmpd);
    const double m = (n & 1) ? w[n / 2] : 0.5 * (w[n / 2 - 1] + w[n / 2]);
    if (w != stackBuf) free(w);
    return m;
}

rtl_stat_t rtl_stat(const double *v, int n) {
    rtl_stat_t s;
    memset(&s, 0, sizeof s);
    s.n = n;
    if (n <= 0) return s;
    s.min = s.max = v[0];
    for (int i = 0; i < n; i++) {
        s.mean += v[i];
        if (v[i] < s.min) s.min = v[i];
        if (v[i] > s.max) s.max = v[i];
    }
    s.mean /= n;
    double ss = 0;
    for (int i = 0; i < n; i++) ss += (v[i] - s.mean) * (v[i] - s.mean);
    s.sd = n > 1 ? sqrt(ss / (n - 1)) : 0.0;
    s.median = rtl_median(v, n);
    return s;
}

// -------------------------------------------------------------- declared
void rtl_declared_need(rtl_declared_t *c, int ok, const char *what) {
    if (ok) return;
    if (c->nmissing < RTL_DECLARED_MAX_MISSING) c->missing[c->nmissing] = what;
    c->nmissing++;
}

uint32_t rtl_declared_hw(const rtl_declared_t *c)    { return c->latIn + c->latOut + c->strIn + c->strOut; }
uint32_t rtl_declared_sched(const rtl_declared_t *c) { return 2 * c->io + c->safIn + c->safOut; }

uint32_t rtl_declared_round_trip(const rtl_declared_t *c, int *valid) {
    const int ok = c->nmissing == 0;
    if (valid) *valid = ok;
    return ok ? rtl_declared_sched(c) + rtl_declared_hw(c) : 0u;
}

uint32_t rtl_auto_window(const rtl_declared_t *c) {
    // The declared figure is a guess at best, and the tool exists because it
    // can be wrong, so leave generous room: 4x, never below 1024 frames. An
    // unreadable declaration gets the maximum window.
    int valid = 0;
    const uint32_t rt = rtl_declared_round_trip(c, &valid);
    if (!valid) return RTL_MAX_WINDOW;
    uint64_t w = (uint64_t)rt * 4u;
    if (w < 1024u) w = 1024u;
    if (w > RTL_MAX_WINDOW) w = RTL_MAX_WINDOW;
    return (uint32_t)w;
}

// --------------------------------------------------------------- summary
void rtl_summarize(const rtl_engine_t *e, const rtl_declared_t *dc,
                   rtl_trial_report_t *reports, rtl_summary_t *out) {
    memset(out, 0, sizeof *out);
    out->trialsRun = e->trial;
    out->minPeak = 0.0;
    int havePeak = 0;

    double raw[RTL_MAX_TRIALS], ts[RTL_MAX_TRIALS], pre[RTL_MAX_TRIALS], sched[RTL_MAX_TRIALS];
    int nr = 0;
    for (uint32_t i = 0; i < e->trial && i < RTL_MAX_TRIALS; i++) {
        const rtl_trial_t *tr = &e->t[i];
        const rtl_result_t r = rtl_analyse(tr);
        const rtl_verdict_t v = rtl_trial_verdict(tr, r.ok, r.atWindowEdge);
        if (reports) {
            reports[i].verdict = v;
            reports[i].result = r;
            reports[i].callbacks = tr->cbAt[1] - tr->cbAt[0];
            reports[i].overloads = tr->ovlAt[1] - tr->ovlAt[0];
        }
        out->tally[v]++;
        if (r.ok) {
            if (r.noise > out->worstNoise) out->worstNoise = r.noise;
            if (!havePeak || r.peak < out->minPeak) { out->minPeak = r.peak; havePeak = 1; }
            if (r.inverted) out->inverted++;
        }
        // Fails closed: one gate, and only what passes it is measured. Every
        // aggregate therefore draws on the same trials by construction.
        if (v != TRIAL_ACCEPTED) continue;
        raw[nr]   = r.rawFrames;
        ts[nr]    = r.tsFrames;
        sched[nr] = r.rawFrames - r.tsFrames;
        pre[nr]   = (tr->capAbs + r.onset) - tr->emitAbs;
        nr++;
    }
    out->accepted = nr;
    out->raw   = rtl_stat(raw, nr);
    out->onset = rtl_stat(pre, nr);
    out->ts    = rtl_stat(ts, nr);
    out->sched = rtl_stat(sched, nr);
    if (dc) {
        out->declaredSched = rtl_declared_sched(dc);
        out->declaredHw    = rtl_declared_hw(dc);
        // A residual against a declaration we could not read is wrong in a way
        // that looks exactly like a real answer: withhold it.
        out->residualValid = (dc->nmissing == 0 && nr > 0);
        if (out->residualValid) out->residualFrames = out->ts.median - (double)out->declaredHw;
    }
}

// ---------------------------------------------------------------- evidence
void rtl_json_string(FILE *f, const char *s) {
    if (!s) { fputs("null", f); return; }
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", *p);
            else fputc(*p, f);
        }
    }
    fputc('"', f);
}

// JSON has no NaN/Inf; never emit one.
static void json_num(FILE *f, double v) {
    if (isfinite(v)) fprintf(f, "%.6f", v);
    else fputs("null", f);
}

static void json_stat(FILE *f, const char *key, const rtl_stat_t *s, double sr) {
    fprintf(f, "    ");
    rtl_json_string(f, key);
    fprintf(f, ": {\"n\": %d", s->n);
    if (s->n > 0) {
        fputs(", \"median_frames\": ", f); json_num(f, s->median);
        fputs(", \"median_ms\": ", f);     json_num(f, sr > 0 ? s->median * 1000.0 / sr : NAN);
        fputs(", \"mean_frames\": ", f);   json_num(f, s->mean);
        fputs(", \"sd_frames\": ", f);     json_num(f, s->sd);
        fputs(", \"min_frames\": ", f);    json_num(f, s->min);
        fputs(", \"max_frames\": ", f);    json_num(f, s->max);
    }
    fputc('}', f);
}

int rtl_write_json(FILE *f, const rtl_provenance_t *p, const rtl_declared_t *dc,
                   const rtl_engine_t *e, const rtl_trial_report_t *reports,
                   const rtl_summary_t *s) {
    if (!f || !p || !dc || !e || !s) return -1;
    const double sr = p->sampleRate;
    fputs("{\n  \"schema\": \"asfw.rtl_loopback.v1\",\n  \"provenance\": {\n", f);
    fputs("    \"tool\": ", f);          rtl_json_string(f, p->tool);
    fputs(",\n    \"tool_version\": ", f); rtl_json_string(f, p->toolVersion);
    fputs(",\n    \"timestamp_utc\": ", f); rtl_json_string(f, p->timestampUtc);
    fputs(",\n    \"os_version\": ", f);  rtl_json_string(f, p->osVersion);
    fputs(",\n    \"device_name\": ", f); rtl_json_string(f, p->deviceName);
    fputs(",\n    \"device_uid\": ", f);  rtl_json_string(f, p->deviceUid);
    fputs(",\n    \"argv\": ", f);        rtl_json_string(f, p->argv);
    fputs(",\n    \"sample_rate_hz\": ", f); json_num(f, sr);
    fprintf(f, ",\n    \"buffer_frames_requested\": %u,\n    \"buffer_frames_actual\": %u,\n"
               "    \"window_frames\": %u,\n    \"trials_requested\": %u,\n"
               "    \"processor_overloads_total\": %u,\n    \"timed_out\": %s\n  },\n",
            p->bufferRequested, p->bufferActual, p->window, p->trialsRequested,
            p->overloadsTotal, p->timedOut ? "true" : "false");

    fprintf(f, "  \"declared\": {\n    \"buffer_frames\": %u,\n"
               "    \"safety_offset_in\": %u,\n    \"safety_offset_out\": %u,\n"
               "    \"device_latency_in\": %u,\n    \"device_latency_out\": %u,\n"
               "    \"stream_latency_in\": %u,\n    \"stream_latency_out\": %u,\n"
               "    \"scheduling_frames\": %u,\n    \"hw_latency_frames\": %u,\n"
               "    \"missing\": [",
            dc->io, dc->safIn, dc->safOut, dc->latIn, dc->latOut, dc->strIn, dc->strOut,
            rtl_declared_sched(dc), rtl_declared_hw(dc));
    for (int i = 0; i < dc->nmissing && i < RTL_DECLARED_MAX_MISSING; i++) {
        if (i) fputs(", ", f);
        rtl_json_string(f, dc->missing[i]);
    }
    fputs("]\n  },\n", f);

    fprintf(f, "  \"io\": {\n    \"callbacks\": %u,\n    \"distinct_spans\": [", e->cycles);
    for (uint32_t i = 0; i < e->spanCount; i++) fprintf(f, "%s%u", i ? ", " : "", e->spans[i]);
    fprintf(f, "],\n    \"gap_events\": %u,\n    \"gap_frames\": ", rtl_engine_gap_events(e));
    json_num(f, rtl_engine_gap_frames(e));
    fprintf(f, ",\n    \"re_anchor_events\": %u,\n    \"worst_re_anchor_frames\": ",
            rtl_engine_anchor_events(e));
    json_num(f, rtl_engine_worst_anchor(e));
    fprintf(f, ",\n    \"clock_disagreements\": %u,\n    \"missing_timestamps\": %u\n  },\n",
            rtl_engine_ambiguous(e), rtl_engine_missing_ts(e));

    fputs("  \"trials\": [\n", f);
    for (uint32_t i = 0; i < s->trialsRun && reports; i++) {
        const rtl_trial_report_t *t = &reports[i];
        fprintf(f, "    {\"index\": %u, \"verdict\": ", i);
        rtl_json_string(f, rtl_verdict_id(t->verdict));
        fprintf(f, ", \"impulse_found\": %s, \"timestamps_valid\": %s, \"at_window_edge\": %s",
                t->result.ok ? "true" : "false", t->result.tsValid ? "true" : "false",
                t->result.atWindowEdge ? "true" : "false");
        fputs(", \"rtl_raw_frames\": ", f);
        if (t->result.ok) json_num(f, t->result.rawFrames); else fputs("null", f);
        fputs(", \"rtl_ts_frames\": ", f);
        if (t->result.ok && t->result.tsValid) json_num(f, t->result.tsFrames); else fputs("null", f);
        fputs(", \"onset_frames\": ", f);
        if (t->result.ok) json_num(f, t->result.onset); else fputs("null", f);
        fputs(", \"peak\": ", f);  json_num(f, t->result.peak);
        fputs(", \"noise_rms\": ", f); json_num(f, t->result.noise);
        fprintf(f, ", \"inverted\": %s, \"callbacks\": %u, \"overloads\": %u}%s\n",
                t->result.inverted ? "true" : "false", t->callbacks, t->overloads,
                i + 1 < s->trialsRun ? "," : "");
    }
    fputs("  ],\n  \"summary\": {\n", f);
    fprintf(f, "    \"trials_run\": %u,\n    \"accepted\": %d,\n    \"rejected\": {",
            s->trialsRun, s->accepted);
    int first = 1;
    for (int v = 1; v < TRIAL_VERDICTS; v++) {
        if (!s->tally[v]) continue;
        fprintf(f, "%s", first ? "" : ", ");
        rtl_json_string(f, rtl_verdict_id((rtl_verdict_t)v));
        fprintf(f, ": %u", s->tally[v]);
        first = 0;
    }
    fputs("},\n", f);
    json_stat(f, "rtl_raw", &s->raw, sr);          fputs(",\n", f);
    json_stat(f, "rtl_raw_onset", &s->onset, sr);  fputs(",\n", f);
    json_stat(f, "rtl_ts", &s->ts, sr);            fputs(",\n", f);
    json_stat(f, "scheduling_distance", &s->sched, sr); fputs(",\n", f);
    fprintf(f, "    \"declared_scheduling_frames\": %u,\n    \"declared_hw_latency_frames\": %u,\n",
            s->declaredSched, s->declaredHw);
    fputs("    \"residual_frames\": ", f);
    if (s->residualValid) json_num(f, s->residualFrames); else fputs("null", f);
    fputs(",\n    \"residual_ms\": ", f);
    if (s->residualValid && sr > 0) json_num(f, s->residualFrames * 1000.0 / sr); else fputs("null", f);
    fputs(",\n    \"min_peak\": ", f); json_num(f, s->minPeak);
    fputs(",\n    \"worst_noise_rms\": ", f); json_num(f, s->worstNoise);
    fprintf(f, ",\n    \"inverted_trials\": %d\n  }\n}\n", s->inverted);
    return ferror(f) ? -1 : 0;
}

// ---------------------------------------------------------------- selftest
float rtl_synth_ir(double i, double d, double amp) {
    const double fc = 0.45, L = 24.0;
    const double t = i - d;
    if (fabs(t) > L) return 0.0f;
    const double x = 2.0 * fc * t;
    const double sinc = fabs(x) < 1e-9 ? 1.0 : sin(RTL_PI * x) / (RTL_PI * x);
    const double w = 0.5 * (1.0 + cos(RTL_PI * t / L));
    return (float)(amp * sinc * w);
}

static void fill_trial(rtl_trial_t *t, double d, double amp, unsigned *rng) {
    memset(t, 0, sizeof *t);
    t->n = 4096;
    t->emitAbs = 1000.0; t->capAbs = 1000.0;
    t->emitOt  = 5000.0; t->capIt  = 5000.0 - 900.0;
    t->tsValid = 1;
    for (uint32_t i = 0; i < t->n; i++) {
        *rng = *rng * 1103515245u + 12345u;
        const double noise = ((double)((*rng >> 16) & 0xFFFF) / 32768.0 - 1.0) * 1e-4;
        t->win[i] = rtl_synth_ir((double)i, d, amp) + (float)noise;
    }
}

#define LOG(...) do { if (log) fprintf(log, __VA_ARGS__); } while (0)

// ------------------------------------------------------- adversarial simulator
// Drives the real engine over a known physical timeline, injects faults, and
// derives every expectation from WHAT WAS INJECTED -- never by recomputing the
// classifier's own tolerances. A trial spanning a fault must never be admitted;
// one spanning none must be admitted and recover the injected round trip.
typedef struct {
    double   trueRtl, dropped, anchor, posBefore;
    double   emitPhys[RTL_MAX_TRIALS];
    int      nEmits;
    unsigned rng;
    uint32_t overloads;
} sim_state_t;

static float sim_input(void *ctx, uint32_t frame) {
    sim_state_t *sim = (sim_state_t *)ctx;
    const double physical = sim->posBefore + sim->dropped + (double)frame;
    double v = 0;
    for (int i = 0; i < sim->nEmits; i++)
        v += rtl_synth_ir(physical, sim->emitPhys[i] + sim->trueRtl, 0.9);
    sim->rng = sim->rng * 1103515245u + 12345u;
    v += ((double)((sim->rng >> 16) & 0xFFFF) / 32768.0 - 1.0) * 1e-5;
    return (float)v;
}

static uint32_t sim_overloads(void *ctx) { return ((const sim_state_t *)ctx)->overloads; }

static const rtl_sim_cfg_t kSimConfigs[] = {
    // name                    rtl  drop     anchor        noTs jitter wide ovl
    { "clean",                 300, -1, 0,  -1, 0,      -1,  0,  0, -1 },
    { "clean, wider spans",    300, -1, 0,  -1, 0,      -1,  0,  1, -1 },
    { "jitter under tolerance",300, -1, 0,  -1, 0,      -1, 30,  0, -1 },
    { "dropped callback",      300,  3, 64, -1, 0,      -1,  0,  0, -1 },
    { "re-anchor",             300, -1, 0,   3, 936.0,  -1,  0,  0, -1 },
    { "re-anchor, negative",   300, -1, 0,   3, -936.0, -1,  0,  0, -1 },
    { "drop under re-anchor",  300,  3, 64,  3, 936.0,  -1,  0,  0, -1 },
    { "drop, negative anchor", 300,  3, 64,  3, -936.0, -1,  0,  0, -1 },
    { "missing timestamps",    300, -1, 0,  -1, 0,       3,  0,  0, -1 },
    { "drop, no timestamps",   300,  3, 64, -1, 0,       3,  0,  0, -1 },
    { "drop, wider spans",     300,  3, 64, -1, 0,      -1,  0,  1, -1 },
    { "all three at once",     300,  3, 64,  3, 936.0,   3,  0,  0, -1 },
    { "processor overload",    300, -1, 0,  -1, 0,      -1,  0,  0,  3 },
};

const rtl_sim_cfg_t *rtl_sim_configs(size_t *n) {
    if (n) *n = sizeof kSimConfigs / sizeof kSimConfigs[0];
    return kSimConfigs;
}

int rtl_sim_run(const rtl_sim_cfg_t *c, double sr, FILE *log) {
    // 4 trials of 512 frames: small enough to keep on the heap briefly.
    enum { kTrials = 4 };
    rtl_trial_t *trials = (rtl_trial_t *)calloc(kTrials, sizeof(rtl_trial_t));
    if (!trials) { LOG("      out of memory\n"); return 1; }
    rtl_engine_t e;
    rtl_engine_init(&e, trials, kTrials, 512, 128, 128, 0.9f, sr);
    sim_state_t sim;
    memset(&sim, 0, sizeof sim);
    sim.trueRtl = c->trueRtl;
    sim.rng = 4321;
    e.overloads = sim_overloads;
    e.overloadsCtx = &sim;

    // Spans stay below trueRtl so an impulse never lands in its own emit
    // callback, which is the one place the simulator could not record it.
    static const uint32_t patNarrow[4] = { 128, 64, 64, 64 };
    static const uint32_t patWide[4]   = { 256, 96, 96, 96 };
    const uint32_t *pat = c->wideSpans ? patWide : patNarrow;

    int faults[5], nf = 0;
    if (c->dropAt     >= 0) faults[nf++] = c->dropAt;
    if (c->anchorAt   >= 0) faults[nf++] = c->anchorAt;
    if (c->noTsAt     >= 0) faults[nf++] = c->noTsAt;
    if (c->overloadAt >= 0) faults[nf++] = c->overloadAt;

    unsigned jrng = 777;
    for (int i = 1; i <= 400 && e.state != RTL_ST_DONE; i++) {
        const uint32_t n = pat[(i - 1) % 4];
        if (i == c->dropAt)     sim.dropped += c->dropFrames;   // never delivered
        if (i == c->anchorAt)   sim.anchor  += c->anchorFrames; // timeline re-origined
        if (i == c->overloadAt) sim.overloads++;                // HAL notification
        const int tsOk = (i != c->noTsAt);

        const double physical = e.pos + sim.dropped;
        double jitter = 0.0;
        if (c->jitterFrames > 0) {                              // late, never early
            jrng = jrng * 1103515245u + 12345u;
            jitter = (double)((jrng >> 16) & 0xFF) / 255.0 * c->jitterFrames;
        }
        float slot = 0.0f;
        sim.posBefore = e.pos;
        rtl_step_t st = {
            .n = n,
            .hostSec  = (physical + jitter) / sr,
            .itSample = physical + sim.anchor,         .itValid = tsOk,
            .otSample = physical + sim.anchor + 900.0, .otValid = tsOk,
            .input = sim_input, .ctx = &sim, .outSlot = &slot,
        };
        rtl_engine_step(&e, &st);
        if (slot != 0.0f && sim.nEmits < RTL_MAX_TRIALS)
            sim.emitPhys[sim.nEmits++] = sim.posBefore + sim.dropped;
    }

    int failures = 0, accepted = 0, expectedAccepted = 0;
    for (uint32_t k = 0; k < e.trial; k++) {
        const rtl_trial_t *t = &e.t[k];
        const rtl_result_t r = rtl_analyse(t);
        const rtl_verdict_t v = rtl_trial_verdict(t, r.ok, r.atWindowEdge);

        // A fault at the emit callback itself precedes the measured interval
        // and cannot corrupt it; anything after does.
        int affected = 0;
        for (int f = 0; f < nf; f++)
            if ((uint32_t)faults[f] > t->cbAt[0] && (uint32_t)faults[f] <= t->cbAt[1])
                affected = 1;

        if (v == TRIAL_ACCEPTED) accepted++;
        if (!affected) expectedAccepted++;

        if (affected && v == TRIAL_ACCEPTED) {
            LOG("      trial %u: injected fault ADMITTED (RTL_raw %.2f vs true %.2f)\n",
                k, r.rawFrames, c->trueRtl);
            failures++;
        } else if (!affected && v != TRIAL_ACCEPTED) {
            LOG("      trial %u: clean trial rejected as %s\n", k, rtl_verdict_name(v));
            failures++;
        } else if (v == TRIAL_ACCEPTED && fabs(r.rawFrames - c->trueRtl) > 0.25) {
            LOG("      trial %u: admitted but reports %.2f, injected %.2f\n",
                k, r.rawFrames, c->trueRtl);
            failures++;
        }
    }
    if (e.trial == 0) { LOG("      no trials ran\n"); failures++; }

    LOG("  %-26s %d/%u admitted (expected %d)   %s\n", c->name,
        accepted, e.trial, expectedAccepted, failures ? "FAIL" : "ok");
    free(trials);
    return failures;
}

typedef struct {
    const char *name;
    uint32_t    n[6];
    double      st[6];
    double      ht[6];  // host frames; negative = same as st
    int         count;
} audit_case_t;

static rtl_audit_t run_audit(const audit_case_t *c, double sr) {
    rtl_audit_t a;
    memset(&a, 0, sizeof a);
    for (int i = 0; i < c->count; i++) {
        const double host = c->ht[0] < 0 ? c->st[i] : c->ht[i];
        rtl_audit_step(&a, c->st[i], 1, host / sr, c->n[i], sr, 48.0);
    }
    return a;
}

int rtl_selftest(double sr, FILE *log) {
    static const double delays[] = { 137.0, 512.0, 733.4, 1024.25, 2999.75 };
    unsigned rng = 12345;
    int failures = 0;
    rtl_trial_t *t = (rtl_trial_t *)calloc(1, sizeof(rtl_trial_t));
    if (!t) return 1;

    LOG("=== selftest at %.0f Hz ===\n", sr);
    LOG("--- detector: recovering known delays ---\n");
    LOG("  %-10s %-12s %-12s %-12s %s\n", "true", "RTL_raw", "err (fr)", "RTL_ts", "verdict");
    for (unsigned k = 0; k < sizeof delays / sizeof *delays; k++) {
        const double d = delays[k];
        fill_trial(t, d, (k & 1) ? -0.9 : 0.9, &rng);
        const rtl_result_t r = rtl_analyse(t);
        const double err = r.rawFrames - d;
        const double tsErr = r.tsFrames - (d - 900.0);
        const int ok = r.ok && r.tsValid && !r.atWindowEdge && fabs(err) < 0.2 && fabs(tsErr) < 0.2;
        if (!ok) failures++;
        LOG("  %-10.2f %-12.3f %-+12.4f %-12.3f %s\n", d, r.rawFrames, err, r.tsFrames,
            ok ? "ok" : "FAIL");
    }

    {   // An empty window must be rejected, not fitted to noise.
        memset(t, 0, sizeof *t);
        t->n = 4096; t->tsValid = 1;
        for (uint32_t i = 0; i < t->n; i++) {
            rng = rng * 1103515245u + 12345u;
            t->win[i] = (float)(((double)((rng >> 16) & 0xFFFF) / 32768.0 - 1.0) * 1e-4);
        }
        const rtl_result_t r = rtl_analyse(t);
        if (r.ok) failures++;
        LOG("  %-10s %-12s %-12s %-12s %s\n", "silence", "-", "-", "-",
            r.ok ? "FAIL (fitted noise)" : "ok (rejected)");
    }
    {   // A residual of exactly zero is a result, not a missing value.
        fill_trial(t, 900.0, 0.9, &rng);
        const rtl_result_t r = rtl_analyse(t);
        const int ok = r.ok && r.tsValid && fabs(r.tsFrames) < 0.2;
        if (!ok) failures++;
        LOG("  %-10s %-12.3f %-12s %-12.3f %s\n", "zero-resid", r.rawFrames, "-",
            r.tsFrames, ok ? "ok (retained)" : "FAIL (dropped)");
    }
    {   // Invalid timestamps must be flagged, not encoded as the value 0.
        fill_trial(t, 512.0, 0.9, &rng);
        t->tsValid = 0;
        const rtl_result_t r = rtl_analyse(t);
        const int ok = r.ok && !r.tsValid && r.rawFrames > 511.0;
        if (!ok) failures++;
        LOG("  %-10s %-12.3f %-12s %-12s %s\n", "no-ts", r.rawFrames, "-", "-",
            ok ? "ok (raw kept, ts flagged)" : "FAIL");
    }
    {   // A peak on the window edge may be a truncated impulse: flag it.
        fill_trial(t, 4096.0 - 4.0, 0.9, &rng);
        const rtl_result_t r = rtl_analyse(t);
        const rtl_verdict_t v = rtl_trial_verdict(t, r.ok, r.atWindowEdge);
        const int ok = r.atWindowEdge && v == TRIAL_WINDOW_EDGE;
        if (!ok) failures++;
        LOG("  %-10s %-12s %-12s %-12s %s\n", "edge", "-", "-", "-",
            ok ? "ok (window edge rejected)" : "FAIL (edge admitted)");
    }

    LOG("\n--- timeline audit ---\n");
    {
        const audit_case_t c = { "128 then 64, continuous",
            { 128, 64, 64, 64, 128, 64 }, { 0, 128, 192, 256, 320, 448 }, { -1 }, 6 };
        const rtl_audit_t a = run_audit(&c, sr);
        const int ok = !a.gapEvents && !a.anchorEvents && !a.ambiguous;
        if (!ok) failures++;
        LOG("  %-26s %u gap / %u anchor / %u amb   %s\n", c.name,
            a.gapEvents, a.anchorEvents, a.ambiguous, ok ? "ok" : "FAIL");
    }
    {
        const audit_case_t c = { "skipped 64 after 128",
            { 128, 64, 64, 64 }, { 0, 128, 192, 320 }, { -1 }, 4 };
        const rtl_audit_t a = run_audit(&c, sr);
        const int ok = a.gapEvents == 1 && fabs(a.gapFrames - 64.0) < 0.5 && !a.anchorEvents;
        if (!ok) failures++;
        LOG("  %-26s %u gap, %.0f fr lost   %s\n", c.name, a.gapEvents, a.gapFrames,
            ok ? "ok" : "FAIL (gap accepted)");
    }
    {
        const audit_case_t c = { "1000-frame jump",
            { 64, 64, 64, 64 }, { 0, 64, 128, 1128 }, { 0, 64, 128, 192 }, 4 };
        const rtl_audit_t a = run_audit(&c, sr);
        const int ok = a.anchorEvents == 1 && !a.gapEvents && fabs(a.worstAnchor - 936.0) < 0.5;
        if (!ok) failures++;
        LOG("  %-26s %u anchor, worst %+.0f fr   %s\n", c.name, a.anchorEvents,
            a.worstAnchor, ok ? "ok" : "FAIL");
    }
    {
        const audit_case_t c = { "clocks disagree",
            { 64, 64, 64 }, { 0, 64, 128 }, { 0, 64, 256 }, 3 };
        const rtl_audit_t a = run_audit(&c, sr);
        const int ok = a.ambiguous == 1 && !a.gapEvents;
        if (!ok) failures++;
        LOG("  %-26s %u ambiguous   %s\n", c.name, a.ambiguous,
            ok ? "ok" : "FAIL (conflict accepted)");
    }
    {
        const audit_case_t c = { "jitter within tolerance",
            { 64, 64, 64 }, { 0, 64, 128 }, { 0, 64, 148 }, 3 };
        const rtl_audit_t a = run_audit(&c, sr);
        const int ok = !a.ambiguous && !a.gapEvents && !a.anchorEvents;
        if (!ok) failures++;
        LOG("  %-26s %u ambiguous   %s\n", c.name, a.ambiguous,
            ok ? "ok" : "FAIL (jitter rejected)");
    }
    for (int sign = 1; sign >= -1; sign -= 2) {
        // 64 frames lost while the timeline re-anchors around them, both signs.
        audit_case_t c = { sign > 0 ? "loss under +936 anchor" : "loss under -936 anchor",
            { 64, 64, 64, 64 }, { 0, 64, 128, 0 }, { 0, 64, 128, 256 }, 4 };
        c.st[3] = 128 + 64 + 64 + sign * 936.0;
        const rtl_audit_t a = run_audit(&c, sr);
        const int ok = !a.anchorEvents && (a.ambiguous || a.gapEvents);
        if (!ok) failures++;
        LOG("  %-26s %u gap / %u anchor / %u amb   %s\n", c.name,
            a.gapEvents, a.anchorEvents, a.ambiguous,
            ok ? "ok" : "FAIL (loss hid in the anchor)");
    }

    LOG("\n--- aggregation: scheduling distance must be paired per trial ---\n");
    {
        // Mixing trial sets makes the median difference meaningless; the paired
        // per-trial difference is the only valid scheduling distance.
        const double raw[]   = { 1000, 1000, 1400, 1400 };
        const double ts[]    = { 600, 600 };
        const double sched[] = { 400, 400 };
        const double paired   = rtl_median(sched, 2);
        const double unpaired = rtl_median(raw, 4) - rtl_median(ts, 2);
        const int ok = fabs(paired - 400.0) < 0.5 && fabs(unpaired - 400.0) > 0.5;
        if (!ok) failures++;
        LOG("  %-26s paired %.0f fr, median-difference %.0f fr   %s\n",
            "mixed trial sets", paired, unpaired, ok ? "ok" : "FAIL");
    }

    LOG("\n--- adversarial: injected faults must never reach the statistics ---\n");
    size_t ncfg = 0;
    const rtl_sim_cfg_t *cfgs = rtl_sim_configs(&ncfg);
    for (size_t i = 0; i < ncfg; i++) failures += rtl_sim_run(&cfgs[i], sr, log);

    LOG("\n  %s at %.0f Hz\n", failures ? "SELFTEST FAILED" : "selftest passed", sr);
    free(t);
    return failures;
}
