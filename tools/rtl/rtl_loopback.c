// rtl_loopback.c -- uncompensated electrical round-trip latency measurement.
//
// Phase 1 of documentation/AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md. Measures the
// physical round trip with an output->input cable and NO DAW in the path, so
// nothing along the way can silently compensate the number we are trying to
// read.
//
//   clang -O1 -o rtl_loopback rtl_loopback.c -framework CoreAudio -framework CoreFoundation
//
//   ./rtl_loopback                        list matching devices + declared geometry
//   ./rtl_loopback -d ASFW --measure      run the measurement (starts real IO)
//   ./rtl_loopback -d ASFW --measure --frames 64 --trials 32
//   ./rtl_loopback --selftest             check the analysis maths, no hardware
//
// WHAT THE NUMBERS MEAN
//
// The governing contract is Apple's, not ours. Jeff Moore, coreaudio-api
// 2002/Aug/msg00055: "The output time stamp passed in to your IOProc reflects
// the driver's safety offset, but not the latency in the hardware." And
// 2004/Oct/msg00266, endorsing Sven Behne's model verbatim: an input timestamp
// minus input hardware latency is the analog capture instant; an output
// timestamp plus output hardware latency is the audible instant; the minimum
// thru time is
//
//     in_hw_latency + in_buf + in_safety + out_buf + out_safety + out_hw_latency
//
// Those two halves are what this tool separates:
//
//   RTL_raw    frames between writing a sample into an output buffer and seeing
//              it in an input buffer, counted by accumulating each callback's
//              frame count. It never reads mSampleTime, so it is immune to the
//              reporting-only latency properties and sensitive only to what
//              moves real timing. It is the whole thru time. THE EXIT NUMBER.
//
//   RTL_ts     the same event pair in the sample-time domain the HAL hands
//              clients. Because those timestamps carry safety but NOT hardware
//              latency, a truthful device returns
//                  RTL_ts = in_hw_latency + out_hw_latency
//              -- NOT zero. This is the measured hardware latency of the whole
//              analog path, converters included.
//
//   residual   RTL_ts minus the hardware latency we declare (device + stream,
//              both directions). THIS is the signed amount by which our
//              declarations misstate the physical path, and the Phase 2
//              reference-plane input. Positive means we under-declare: real
//              audio arrives later than we claim.
//
//   RTL_raw - RTL_ts is the scheduling distance, and reduces algebraically to
//   the per-callback input/output timestamp skew. It should reconcile with
//   2*io + in_safety + out_safety and contains NO latency term, so it cannot
//   tell you whether a declared latency reached the HAL. Only the residual can.
//
// TRIAL VALIDITY
//
// RTL_raw counts delivered frames, so a callback the HAL skipped under overload
// is invisible to it: elapsed time advances while the counter does not, and the
// measurement silently reads short by the dropped span. Wall-clock lag is
// therefore tracked per trial against mach_absolute_time -- which no driver
// re-anchoring can move -- and any trial straddling a gap is rejected rather
// than averaged in. A sample-time re-anchor with continuous delivery is the
// other case, and invalidates only RTL_ts; the two are distinguished, not
// conflated.
//
// BENCH SELF-CHECK (see README.md): change a reporting-only latency field,
// rebuild, rerun. Neither RTL_raw nor RTL_ts may move -- the physical path did
// not change, and IOProc timestamps do not carry hardware latency. The declared
// figure moves, so the residual moves with it. Do not probe with safety offsets
// or the client buffer size: those legitimately change physical timing.
//
// All analysis happens after the run. The IOProc allocates nothing, logs
// nothing, and touches only preallocated storage.
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_TRIALS   128
#define MAX_WINDOW   16384
#define MAX_CHANNELS 64

static double g_h2s = 0.0;                      // host ticks -> seconds
static void init_hostclock(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_h2s = (double)tb.numer / (double)tb.denom * 1e-9;
}

// ------------------------------------------------------------------ helpers
static int getprop(AudioObjectID o, AudioObjectPropertySelector sel,
                   AudioObjectPropertyScope scope, void *buf, UInt32 sz) {
    AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, buf) == noErr;
}

static CFStringRef dev_name(AudioObjectID d) {
    CFStringRef s = NULL;
    getprop(d, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &s, sizeof s);
    return s;
}

static UInt32 chan_count(AudioObjectID d, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, sc,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0, n = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz) != noErr || !sz) return 0;
    AudioBufferList *bl = malloc(sz);
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, bl) == noErr)
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
            n += bl->mBuffers[i].mNumberChannels;
    free(bl);
    return n;
}

static UInt32 stream_latency(AudioObjectID d, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, sc,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0, worst = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz) != noErr || !sz) return 0;
    AudioObjectID *st = malloc(sz);
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, st) == noErr)
        for (UInt32 i = 0; i < sz / sizeof(AudioObjectID); i++) {
            UInt32 v = 0;
            if (getprop(st[i], kAudioStreamPropertyLatency,
                        kAudioObjectPropertyScopeGlobal, &v, sizeof v) && v > worst)
                worst = v;
        }
    free(st);
    return worst;
}

// Buffer lists arrive either as one interleaved buffer or as N mono buffers.
// Both shapes are legal; neither is guaranteed, so never assume.
static UInt32 bl_frames(const AudioBufferList *bl) {
    if (!bl || !bl->mNumberBuffers || !bl->mBuffers[0].mNumberChannels) return 0;
    return bl->mBuffers[0].mDataByteSize /
           (UInt32)(sizeof(Float32) * bl->mBuffers[0].mNumberChannels);
}

static UInt32 bl_channels(const AudioBufferList *bl) {
    UInt32 n = 0;
    if (!bl) return 0;
    for (UInt32 i = 0; i < bl->mNumberBuffers; i++) n += bl->mBuffers[i].mNumberChannels;
    return n;
}

static Float32 *bl_slot(const AudioBufferList *bl, UInt32 frame, UInt32 ch) {
    if (!bl) return NULL;
    UInt32 base = 0;
    for (UInt32 i = 0; i < bl->mNumberBuffers; i++) {
        const AudioBuffer *b = &bl->mBuffers[i];
        if (ch < base + b->mNumberChannels) {
            Float32 *p = (Float32 *)b->mData;
            if (!p) return NULL;
            return &p[(size_t)frame * b->mNumberChannels + (ch - base)];
        }
        base += b->mNumberChannels;
    }
    return NULL;
}

// ----------------------------------------------------------- timeline audit
// Two different failures break a run, and they invalidate different numbers, so
// they must be told apart rather than lumped into one "break".
//
//   dropped frames   the HAL skipped a callback: the sample timeline and the
//                    wall clock BOTH advance past the frames we were handed.
//                    RTL_raw counts delivered frames, so it reads short by the
//                    gap and the trial is unusable.
//   re-anchor        the driver's sample timeline jumped while delivery stayed
//                    continuous: the wall clock does NOT corroborate. Only
//                    RTL_ts is affected.
//
// The gap itself is detected exactly, from integer frame counts. The wall clock
// is consulted only to choose between those two hypotheses, which differ by the
// full magnitude of the gap. That is why this needs no threshold tuned against
// jitter: a lag-only test cannot have one, because at small buffer sizes a
// dropped callback and ordinary jitter are the same magnitude. A discrepancy
// matching neither hypothesis is counted ambiguous, and ambiguous rejects.
//
// The advance is compared against the frame count of the callback BEFORE it,
// not of the callback reporting it: comparing against the current span invents
// a break on every size change, and this driver's callback size does vary.
typedef struct {
    int    have, haveSample;
    UInt32 prevN;
    double prevSample, prevHost;
    UInt32 gapEvents, ambiguous, anchorEvents;
    double gapFrames, worstAnchor;
} audit_t;

// hostTol is the wall-clock disagreement worth acting on, and is sized from the
// SMALLEST callback in the run rather than the one in hand: the frames a skipped
// callback costs are its own, not its predecessor's, so a tolerance scaled to
// the current span misses a small callback dropped after a large one.
static void audit_step(audit_t *a, double sampleTime, int sampleValid,
                       double hostSec, UInt32 n, double sr, double hostTol) {
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
                    // hostTol, NOT tol. This is the only verdict that preserves
                    // RTL_raw, so it must assert the wall clock saw nothing --
                    // against the jitter scale, which is what "nothing" means.
                    // Scaling it to the jump would let a large re-anchor buy
                    // room for a real loss to hide inside: a 64-frame loss under
                    // a 936-frame re-anchor would read as re-anchor only.
                    a->anchorEvents++;
                    if (fabs(sampleGap) > fabs(a->worstAnchor)) a->worstAnchor = sampleGap;
                } else {
                    a->ambiguous++;
                }
            } else if (fabs(hostGap) > hostTol) {
                // The sample timeline says delivery was continuous and the wall
                // clock says it was not. That is conflicting evidence, not
                // agreement, and accepting the sample timeline here would let a
                // lost callback hide behind a re-anchor that happened to
                // preserve the coordinates. Jitter is one-sided and well inside
                // hostTol, so this does not fire on a merely late callback.
                a->ambiguous++;
            }
        } else if (fabs(hostGap) > hostTol) {
            // With no sample-time witness the wall clock is all there is, and
            // it cannot separate a gap from jitter on its own.
            a->ambiguous++;
        }
    }
    a->prevHost = hostSec;
    a->prevN    = n;
    a->have     = 1;
    // After an invalid timestamp the next delta would span two callbacks, so
    // drop the witness rather than compare against the wrong span.
    if (sampleValid) { a->prevSample = sampleTime; a->haveSample = 1; }
    else               a->haveSample = 0;
}

// ------------------------------------------------------------- shared state
enum { ST_WARMUP, ST_GAP, ST_CAPTURE, ST_DONE };

typedef struct {
    double  emitAbs;       // absolute delivered-frame index of the impulse
    double  emitOt;        // outputTime.mSampleTime at that same frame
    double  capAbs;        // absolute delivered-frame index of the first capture
    double  capIt;         // inputTime.mSampleTime at that same frame
    UInt32  n;
    int     tsValid;       // both timestamps were valid -- distinct from "zero"
    UInt32  badAtStart;    // frame gaps, bounded to this trial
    UInt32  badAtEnd;
    UInt32  anchorAtStart; // re-anchors, bounded to this trial
    UInt32  anchorAtEnd;
    Float32 win[MAX_WINDOW];
} trial_t;

static struct {
    UInt32  outCh, inCh, trials, window, gapFrames, warmupFrames;
    Float32 amplitude;
    double  sampleRate;

    int     state;
    UInt32  trial;
    UInt32  remaining;
    double  pos;           // accumulated DELIVERED frames
    double  hostStart;     // seconds, first callback
    int     haveHostStart;
    UInt32  cycles;
    UInt32  spans[16], spanCount, minSpan;

    audit_t itAudit, otAudit;

    Float32 chPeak[MAX_CHANNELS];
    UInt32  inChans;

    _Atomic int done;      // the only field the main thread reads while IO runs
    _Atomic unsigned overloads;

    trial_t t[MAX_TRIALS];
} g;

// A trial is unusable if either domain lost frames or could not classify what
// happened; only a corroborated re-anchor is survivable, and only for RTL_raw.
static UInt32 audit_bad(void) {
    return g.itAudit.gapEvents + g.itAudit.ambiguous +
           g.otAudit.gapEvents + g.otAudit.ambiguous;
}
static UInt32 audit_anchors(void) {
    return g.itAudit.anchorEvents + g.otAudit.anchorEvents;
}

static OSStatus overload_listener(AudioObjectID o, UInt32 n,
                                  const AudioObjectPropertyAddress *a, void *u) {
    (void)o; (void)n; (void)a; (void)u;
    atomic_fetch_add_explicit(&g.overloads, 1u, memory_order_relaxed);
    return noErr;
}

static OSStatus rtl_ioproc(AudioObjectID dev, const AudioTimeStamp *now,
                           const AudioBufferList *in, const AudioTimeStamp *it,
                           AudioBufferList *out, const AudioTimeStamp *ot,
                           void *ud) {
    (void)dev; (void)now; (void)ud;

    const UInt32 n = bl_frames(out) ? bl_frames(out) : bl_frames(in);
    if (!n) return noErr;

    if (out)
        for (UInt32 i = 0; i < out->mNumberBuffers; i++)
            if (out->mBuffers[i].mData)
                memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);

    g.cycles++;
    if (!g.minSpan || n < g.minSpan) g.minSpan = n;
    if (g.spanCount < 16) {
        int seen = 0;
        for (UInt32 i = 0; i < g.spanCount; i++) if (g.spans[i] == n) seen = 1;
        if (!seen) g.spans[g.spanCount++] = n;
    }

    // Wall clock, read directly rather than through any driver-supplied
    // timestamp: a re-anchored sample timeline must not be able to hide a gap
    // in delivered frames, and only an independent clock guarantees that.
    const double hostSec = (double)mach_absolute_time() * g_h2s;
    if (!g.haveHostStart) { g.hostStart = hostSec; g.haveHostStart = 1; }

    const int itOk = it && (it->mFlags & kAudioTimeStampSampleTimeValid);
    const int otOk = ot && (ot->mFlags & kAudioTimeStampSampleTimeValid);
    // Three quarters of the smallest callback: a lost callback costs at least a
    // whole one, while scheduling jitter stays well below that.
    double hostTol = 0.75 * (double)g.minSpan;
    if (hostTol < 8.0) hostTol = 8.0;
    audit_step(&g.itAudit, itOk ? it->mSampleTime : 0.0, itOk, hostSec, n,
               g.sampleRate, hostTol);
    audit_step(&g.otAudit, otOk ? ot->mSampleTime : 0.0, otOk, hostSec, n,
               g.sampleRate, hostTol);

    if (in) {
        const UInt32 nc = bl_channels(in);
        if (nc && nc <= MAX_CHANNELS) {
            g.inChans = nc;
            for (UInt32 c = 0; c < nc; c++)
                for (UInt32 f = 0; f < n; f++) {
                    const Float32 *s = bl_slot(in, f, c);
                    if (!s) break;
                    const Float32 a = fabsf(*s);
                    if (a > g.chPeak[c]) g.chPeak[c] = a;
                }
        }
    }

    switch (g.state) {
    case ST_WARMUP:
    case ST_GAP:
        if (g.remaining > n) { g.remaining -= n; break; }
        g.remaining = 0;
        if (g.trial >= g.trials) { g.state = ST_DONE; break; }
        {
            trial_t *t = &g.t[g.trial];
            Float32 *o = bl_slot(out, 0, g.outCh);
            if (o) *o = g.amplitude;
            t->emitAbs       = g.pos;
            t->capAbs        = g.pos;
            t->emitOt        = otOk ? ot->mSampleTime : 0.0;
            t->capIt         = itOk ? it->mSampleTime : 0.0;
            t->tsValid       = otOk && itOk;
            t->badAtStart    = audit_bad();
            t->anchorAtStart = audit_anchors();
            t->n             = 0;
            g.state          = ST_CAPTURE;
        }
        __attribute__((fallthrough));
    case ST_CAPTURE: {
        trial_t *t = &g.t[g.trial];
        for (UInt32 f = 0; f < n && t->n < g.window; f++) {
            const Float32 *s = bl_slot(in, f, g.inCh);
            t->win[t->n++] = s ? *s : 0.0f;
        }
        if (t->n >= g.window) {
            t->badAtEnd    = audit_bad();
            t->anchorAtEnd = audit_anchors();
            g.trial++;
            g.remaining = g.gapFrames;
            g.state = (g.trial >= g.trials) ? ST_DONE : ST_GAP;
        }
        break;
    }
    default: break;
    }

    g.pos += (double)n;
    if (g.state == ST_DONE)
        atomic_store_explicit(&g.done, 1, memory_order_release);
    return noErr;
}

// ----------------------------------------------------------------- analysis
typedef struct {
    int    ok;
    int    tsValid;
    double rawFrames, tsFrames;
    double peak, noise, onset;
    int    inverted;
} result_t;

static result_t analyse(const trial_t *t) {
    result_t r; memset(&r, 0, sizeof r);
    if (t->n < 16) return r;

    const UInt32 nf = t->n < 32 ? t->n : 32;
    double acc = 0;
    for (UInt32 i = 0; i < nf; i++) acc += (double)t->win[i] * t->win[i];
    r.noise = sqrt(acc / nf);

    UInt32 pk = 0;
    double best = 0;
    for (UInt32 i = 0; i < t->n; i++) {
        const double a = fabs((double)t->win[i]);
        if (a > best) { best = a; pk = i; }
    }
    r.peak = best;
    r.inverted = t->win[pk] < 0;
    if (best < 1e-4 || best < r.noise * 8.0 || pk == 0 || pk + 1 >= t->n) return r;

    // Converter anti-alias filters are close to linear phase, so the response
    // rings symmetrically about the group delay: the peak is the estimator and
    // the onset is early by the pre-ring. Report both; trust the peak.
    const double y0 = fabs((double)t->win[pk - 1]);
    const double y1 = best;
    const double y2 = fabs((double)t->win[pk + 1]);
    const double den = y0 - 2.0 * y1 + y2;
    const double frac = fabs(den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0;

    UInt32 on = pk;
    while (on > 0 && fabs((double)t->win[on - 1]) > best * 0.1) on--;
    r.onset = (double)on;

    const double detect = (double)pk + frac;
    r.rawFrames = (t->capAbs + detect) - t->emitAbs;
    // Validity is a flag, never a sentinel: a residual of exactly zero is the
    // most interesting result this tool can produce, not a missing one.
    r.tsValid = t->tsValid;
    if (r.tsValid) r.tsFrames = (t->capIt + detect) - t->emitOt;
    r.ok = 1;
    return r;
}

static int cmpd(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double median(double *v, int n) {
    qsort(v, n, sizeof(double), cmpd);
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static void stats(const char *label, double *v, int n, double sr) {
    if (!n) { printf("  %-24s <none>\n", label); return; }
    double mean = 0;
    for (int i = 0; i < n; i++) mean += v[i];
    mean /= n;
    double sd = 0;
    for (int i = 0; i < n; i++) sd += (v[i] - mean) * (v[i] - mean);
    sd = n > 1 ? sqrt(sd / (n - 1)) : 0.0;
    const double med = median(v, n);
    printf("  %-24s %9.2f fr  %8.3f ms   [%.2f .. %.2f]  sd %.2f\n",
           label, med, med * 1000.0 / sr, v[0], v[n - 1], sd);
}

// ---------------------------------------------------------------- declared
typedef struct {
    UInt32 io, safIn, safOut, latIn, latOut, strIn, strOut;
} declared_t;

static declared_t read_declared(AudioObjectID d) {
    declared_t c; memset(&c, 0, sizeof c);
    getprop(d, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
            &c.io, sizeof c.io);
    getprop(d, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput,  &c.safIn,  sizeof c.safIn);
    getprop(d, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &c.safOut, sizeof c.safOut);
    getprop(d, kAudioDevicePropertyLatency,      kAudioObjectPropertyScopeInput,  &c.latIn,  sizeof c.latIn);
    getprop(d, kAudioDevicePropertyLatency,      kAudioObjectPropertyScopeOutput, &c.latOut, sizeof c.latOut);
    c.strIn  = stream_latency(d, kAudioObjectPropertyScopeInput);
    c.strOut = stream_latency(d, kAudioObjectPropertyScopeOutput);
    return c;
}

static UInt32 declared_hw(const declared_t *c)    { return c->latIn + c->latOut + c->strIn + c->strOut; }
static UInt32 declared_sched(const declared_t *c) { return 2 * c->io + c->safIn + c->safOut; }

static void print_declared(AudioObjectID d, const declared_t *c, Float64 sr) {
    printf("  %-24s %.1f Hz, %u in / %u out\n", "format", sr,
           chan_count(d, kAudioObjectPropertyScopeInput),
           chan_count(d, kAudioObjectPropertyScopeOutput));
    printf("  %-24s %u\n", "buffer frame size", c->io);
    printf("  %-24s %u in / %u out\n", "safety offset", c->safIn, c->safOut);
    printf("  %-24s %u in / %u out\n", "device latency", c->latIn, c->latOut);
    printf("  %-24s %u in / %u out\n", "stream latency", c->strIn, c->strOut);
    // Apple's composition, coreaudio-api 2004/Oct/msg00266 (Moore confirming
    // Behne): the two halves are scheduling and hardware, and only the second
    // appears in RTL_ts.
    printf("  %-24s %u fr  (2*%u io + %u+%u safety)\n", "declared scheduling",
           declared_sched(c), c->io, c->safIn, c->safOut);
    printf("  %-24s %u fr  (%u+%u dev + %u+%u stream)\n", "declared hw latency",
           declared_hw(c), c->latIn, c->latOut, c->strIn, c->strOut);
    printf("  %-24s %u fr  %.3f ms\n", "declared round-trip",
           declared_sched(c) + declared_hw(c),
           sr > 0 ? (declared_sched(c) + declared_hw(c)) * 1000.0 / sr : 0.0);
}

// ----------------------------------------------------------------- selftest
static Float32 synth_ir(double i, double d, double amp) {
    const double fc = 0.45, L = 24.0;
    const double t = i - d;
    if (fabs(t) > L) return 0.0f;
    const double x = 2.0 * fc * t;
    const double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
    const double w = 0.5 * (1.0 + cos(M_PI * t / L));
    return (Float32)(amp * sinc * w);
}

static void fill_trial(trial_t *t, double d, double amp, unsigned *rng) {
    memset(t, 0, sizeof *t);
    t->n = 4096;
    t->emitAbs = 1000.0; t->capAbs = 1000.0;
    t->emitOt  = 5000.0; t->capIt  = 5000.0 - 900.0;
    t->tsValid = 1;
    for (UInt32 i = 0; i < t->n; i++) {
        *rng = *rng * 1103515245u + 12345u;
        const double noise = ((double)((*rng >> 16) & 0xFFFF) / 32768.0 - 1.0) * 1e-4;
        t->win[i] = synth_ir((double)i, d, amp) + (Float32)noise;
    }
}

static int selftest(void) {
    static const double delays[] = { 137.0, 512.0, 733.4, 1024.25, 2999.75 };
    unsigned rng = 12345;
    int failures = 0;
    static trial_t t;

    printf("--- detector: recovering known delays ---\n");
    printf("  %-10s %-12s %-12s %-12s %s\n", "true", "RTL_raw", "err (fr)", "RTL_ts", "verdict");
    for (unsigned k = 0; k < sizeof delays / sizeof *delays; k++) {
        const double d = delays[k];
        fill_trial(&t, d, (k & 1) ? -0.9 : 0.9, &rng);
        const result_t r = analyse(&t);
        const double err = r.rawFrames - d;
        const double tsErr = r.tsFrames - (d - 900.0);
        const int ok = r.ok && r.tsValid && fabs(err) < 0.2 && fabs(tsErr) < 0.2;
        if (!ok) failures++;
        printf("  %-10.2f %-12.3f %-+12.4f %-12.3f %s\n",
               d, r.rawFrames, err, r.tsFrames, ok ? "ok" : "FAIL");
    }

    // An empty window must be rejected, not fitted to noise.
    {
        memset(&t, 0, sizeof t);
        t.n = 4096; t.tsValid = 1;
        for (UInt32 i = 0; i < t.n; i++) {
            rng = rng * 1103515245u + 12345u;
            t.win[i] = (Float32)(((double)((rng >> 16) & 0xFFFF) / 32768.0 - 1.0) * 1e-4);
        }
        const result_t r = analyse(&t);
        if (r.ok) failures++;
        printf("  %-10s %-12s %-12s %-12s %s\n", "silence", "-", "-", "-",
               r.ok ? "FAIL (fitted noise)" : "ok (rejected)");
    }

    // A residual of exactly zero is a result, not a missing value.
    {
        fill_trial(&t, 900.0, 0.9, &rng);
        const result_t r = analyse(&t);
        const int ok = r.ok && r.tsValid && fabs(r.tsFrames) < 0.2;
        if (!ok) failures++;
        printf("  %-10s %-12.3f %-12s %-12.3f %s\n", "zero-resid", r.rawFrames, "-",
               r.tsFrames, ok ? "ok (retained)" : "FAIL (dropped)");
    }

    // Invalid timestamps must be flagged, not encoded as the value 0.
    {
        fill_trial(&t, 512.0, 0.9, &rng);
        t.tsValid = 0;
        const result_t r = analyse(&t);
        const int ok = r.ok && !r.tsValid && r.rawFrames > 511.0;
        if (!ok) failures++;
        printf("  %-10s %-12.3f %-12s %-12s %s\n", "no-ts", r.rawFrames, "-", "-",
               ok ? "ok (raw kept, ts flagged)" : "FAIL");
    }

    printf("\n--- timeline audit ---\n");
    {
        // Varying callback size is not a break. The reviewer's case: a larger
        // warmup callback ahead of the trial's own smaller ones.
        const UInt32 n[]  = { 128, 64, 64, 64, 128, 64 };
        const double st[] = { 0, 128, 192, 256, 320, 448 };
        audit_t a; memset(&a, 0, sizeof a);
        for (int i = 0; i < 6; i++)
            audit_step(&a, st[i], 1, st[i] / 48000.0, n[i], 48000.0, 48.0);
        const int ok = !a.gapEvents && !a.anchorEvents && !a.ambiguous;
        if (!ok) failures++;
        printf("  %-26s %u gap / %u anchor / %u amb   %s\n", "128 then 64, continuous",
               a.gapEvents, a.anchorEvents, a.ambiguous, ok ? "ok" : "FAIL");
    }
    {
        // One 64-frame callback skipped, after a 128-frame warmup. A tolerance
        // scaled to the run's LARGEST span accepts this; corroboration does not.
        const UInt32 n[]  = { 128, 64, 64, 64 };
        const double st[] = { 0, 128, 192, 320 };   // 64 frames missing at the end
        audit_t a; memset(&a, 0, sizeof a);
        for (int i = 0; i < 4; i++)
            audit_step(&a, st[i], 1, st[i] / 48000.0, n[i], 48000.0, 48.0);
        const int ok = a.gapEvents == 1 && fabs(a.gapFrames - 64.0) < 0.5 && !a.anchorEvents;
        if (!ok) failures++;
        printf("  %-26s %u gap, %.0f fr lost   %s\n", "skipped 64 after 128",
               a.gapEvents, a.gapFrames, ok ? "ok" : "FAIL (gap accepted)");
    }
    {
        // A re-anchor: sample time jumps, the wall clock does not follow.
        const UInt32 n[]  = { 64, 64, 64, 64 };
        const double st[] = { 0, 64, 128, 1128 };
        const double ht[] = { 0, 64, 128, 192 };
        audit_t a; memset(&a, 0, sizeof a);
        for (int i = 0; i < 4; i++)
            audit_step(&a, st[i], 1, ht[i] / 48000.0, n[i], 48000.0, 48.0);
        const int ok = a.anchorEvents == 1 && !a.gapEvents &&
                       fabs(a.worstAnchor - 936.0) < 0.5;
        if (!ok) failures++;
        printf("  %-26s %u anchor, worst %+.0f fr   %s\n", "1000-frame jump",
               a.anchorEvents, a.worstAnchor, ok ? "ok" : "FAIL");
    }

    {
        // Delivery lost behind a re-anchor that preserved the sample
        // coordinates: the timeline looks continuous, the wall clock does not.
        const UInt32 n[]  = { 64, 64, 64 };
        const double st[] = { 0, 64, 128 };
        const double ht[] = { 0, 64, 256 };
        audit_t a; memset(&a, 0, sizeof a);
        for (int i = 0; i < 3; i++)
            audit_step(&a, st[i], 1, ht[i] / 48000.0, n[i], 48000.0, 48.0);
        const int ok = a.ambiguous == 1 && !a.gapEvents;
        if (!ok) failures++;
        printf("  %-26s %u ambiguous   %s\n", "clocks disagree",
               a.ambiguous, ok ? "ok" : "FAIL (conflict accepted)");
    }
    {
        // A merely late callback must not be rejected.
        const UInt32 n[]  = { 64, 64, 64 };
        const double st[] = { 0, 64, 128 };
        const double ht[] = { 0, 64, 148 };
        audit_t a; memset(&a, 0, sizeof a);
        for (int i = 0; i < 3; i++)
            audit_step(&a, st[i], 1, ht[i] / 48000.0, n[i], 48000.0, 48.0);
        const int ok = !a.ambiguous && !a.gapEvents && !a.anchorEvents;
        if (!ok) failures++;
        printf("  %-26s %u ambiguous   %s\n", "jitter within tolerance",
               a.ambiguous, ok ? "ok" : "FAIL (jitter rejected)");
    }

    {
        // Both faults at once: 64 frames lost while the timeline re-anchors
        // around them. Tested in both directions, since a tolerance scaled to
        // the jump is permissive regardless of its sign.
        for (int sign = 1; sign >= -1; sign -= 2) {
            const UInt32 n[]  = { 64, 64, 64, 64 };
            const double st[] = { 0, 64, 128, 128 + 64 + 64 + sign * 936.0 };
            const double ht[] = { 0, 64, 128, 256 };   // 64 delivered, 64 lost
            audit_t a; memset(&a, 0, sizeof a);
            for (int i = 0; i < 4; i++)
                audit_step(&a, st[i], 1, ht[i] / 48000.0, n[i], 48000.0, 48.0);
            const int ok = !a.anchorEvents && (a.ambiguous || a.gapEvents);
            if (!ok) failures++;
            printf("  %-26s %u gap / %u anchor / %u amb   %s\n",
                   sign > 0 ? "loss under +936 anchor" : "loss under -936 anchor",
                   a.gapEvents, a.anchorEvents, a.ambiguous,
                   ok ? "ok" : "FAIL (loss hid in the anchor)");
        }
    }

    printf("\n--- aggregation: scheduling distance must be paired per trial ---\n");
    {
        // Re-anchored trials contribute to RTL_raw but not RTL_ts, so the two
        // medians describe different sets and their difference is meaningless.
        double raw[]   = { 1000, 1000, 1400, 1400 };   // last two re-anchored
        double ts[]    = { 600, 600 };                 // paired subset only
        double sched[] = { 400, 400 };                 // per-trial differences
        const double paired  = median(sched, 2);
        const double unpaired = median(raw, 4) - median(ts, 2);
        const int ok = fabs(paired - 400.0) < 0.5 && fabs(unpaired - 400.0) > 0.5;
        if (!ok) failures++;
        printf("  %-26s paired %.0f fr, median-difference %.0f fr   %s\n",
               "mixed trial sets", paired, unpaired, ok ? "ok" : "FAIL");
    }

    printf("\n  %s\n", failures ? "SELFTEST FAILED" : "selftest passed");
    return failures ? 1 : 0;
}

// --------------------------------------------------------------------- main
int main(int argc, char **argv) {
    init_hostclock();
    const char *filter = "ASFW";
    int measure = 0, reqFrames = 0;
    g.outCh = 0; g.inCh = 0; g.trials = 20; g.window = 4096; g.amplitude = 0.9f;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-d")       && i + 1 < argc) filter = argv[++i];
        else if (!strcmp(argv[i], "--measure"))                measure = 1;
        else if (!strcmp(argv[i], "--selftest"))               return selftest();
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) reqFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc) g.trials = (UInt32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) g.window = (UInt32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out-ch") && i + 1 < argc) g.outCh = (UInt32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--in-ch")  && i + 1 < argc) g.inCh  = (UInt32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--amp")    && i + 1 < argc) g.amplitude = (Float32)atof(argv[++i]);
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); return 2; }
    }
    if (g.trials > MAX_TRIALS) g.trials = MAX_TRIALS;
    if (g.window > MAX_WINDOW) g.window = MAX_WINDOW;

    AudioObjectPropertyAddress da = { kAudioHardwarePropertyDevices,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &da, 0, NULL, &sz) != noErr) {
        fprintf(stderr, "cannot enumerate devices\n");
        return 1;
    }
    AudioObjectID *devs = malloc(sz);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &sz, devs);
    const UInt32 ndev = sz / sizeof(AudioObjectID);

    AudioObjectID dev = kAudioObjectUnknown;
    char devName[256] = {0};
    for (UInt32 i = 0; i < ndev; i++) {
        CFStringRef nm = dev_name(devs[i]);
        char b[256] = {0};
        if (nm) CFStringGetCString(nm, b, sizeof b, kCFStringEncodingUTF8);
        if (nm) CFRelease(nm);
        if (!strstr(b, filter)) continue;
        if (!chan_count(devs[i], kAudioObjectPropertyScopeInput) ||
            !chan_count(devs[i], kAudioObjectPropertyScopeOutput)) {
            printf("skipping \"%s\": needs both input and output on one device\n", b);
            continue;
        }
        dev = devs[i];
        snprintf(devName, sizeof devName, "%s", b);
        break;
    }
    free(devs);
    if (dev == kAudioObjectUnknown) {
        fprintf(stderr, "no duplex device matching \"%s\"\n", filter);
        return 1;
    }

    if (reqFrames > 0) {
        AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
        UInt32 v = (UInt32)reqFrames;
        AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof v, &v);
    }

    Float64 sr = 48000;
    getprop(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
            &sr, sizeof sr);
    g.sampleRate = sr;

    const declared_t dc = read_declared(dev);
    printf("\n=== %s (device %u) ===\n", devName, dev);
    print_declared(dev, &dc, sr);

    if (!measure) {
        printf("\nDeclarations only. Pass --measure to start real IO and measure the\n"
               "physical path; connect an output->input cable first.\n");
        return 0;
    }

    g.gapFrames    = (UInt32)(sr * 0.25);
    g.warmupFrames = (UInt32)(sr * 0.5);
    g.remaining    = g.warmupFrames;
    g.state        = ST_WARMUP;

    printf("\nmeasuring: %u trials, window %u fr, out ch %u -> in ch %u, amp %.2f\n",
           g.trials, g.window, g.outCh, g.inCh, g.amplitude);

    AudioObjectPropertyAddress ola = { kAudioDeviceProcessorOverload,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(dev, &ola, overload_listener, NULL);

    AudioDeviceIOProcID id = NULL;
    if (AudioDeviceCreateIOProcID(dev, rtl_ioproc, NULL, &id) != noErr) {
        fprintf(stderr, "cannot create IOProc\n");
        return 1;
    }
    if (AudioDeviceStart(dev, id) != noErr) {
        fprintf(stderr, "cannot start device\n");
        AudioDeviceDestroyIOProcID(dev, id);
        return 1;
    }

    const double expected = (g.warmupFrames + (double)g.trials * (g.gapFrames + g.window)) / sr;
    const int timeout = (int)(expected * 4.0) + 5;
    for (int i = 0; i < timeout * 20 &&
                    !atomic_load_explicit(&g.done, memory_order_acquire); i++)
        usleep(50000);
    const int completed = atomic_load_explicit(&g.done, memory_order_acquire);
    AudioDeviceStop(dev, id);
    AudioDeviceDestroyIOProcID(dev, id);
    AudioObjectRemovePropertyListener(dev, &ola, overload_listener, NULL);
    // AudioDeviceStop has quiesced the IOProc, so the rest of g is now this
    // thread's alone and needs no further synchronisation.

    if (!completed)
        printf("\nWARNING: timed out after %d s with %u/%u trials -- IO may have stalled\n",
               timeout, g.trial, g.trials);

    printf("\n--- IO ---\n");
    printf("  %-24s %u\n", "callbacks", g.cycles);
    printf("  %-24s", "distinct spans");
    for (UInt32 i = 0; i < g.spanCount; i++) printf(" %u", g.spans[i]);
    printf("%s\n", g.spanCount > 1 ? "   <-- spans vary" : "");
    printf("  %-24s %u\n", "processor overloads",
           atomic_load_explicit(&g.overloads, memory_order_relaxed));
    printf("  %-24s %u events, %.0f frames lost%s\n", "delivered-frame gaps",
           g.itAudit.gapEvents + g.otAudit.gapEvents,
           g.itAudit.gapFrames,
           (g.itAudit.gapEvents || g.otAudit.gapEvents) ? "   <-- rejects RTL_raw" : "");
    printf("  %-24s %u events (worst %+.0f fr)%s\n", "sample-time re-anchors",
           g.itAudit.anchorEvents + g.otAudit.anchorEvents,
           fabs(g.itAudit.worstAnchor) > fabs(g.otAudit.worstAnchor)
               ? g.itAudit.worstAnchor : g.otAudit.worstAnchor,
           (g.itAudit.anchorEvents || g.otAudit.anchorEvents) ? "   <-- rejects RTL_ts only" : "");
    printf("  %-24s %u%s\n", "unclassified",
           g.itAudit.ambiguous + g.otAudit.ambiguous,
           (g.itAudit.ambiguous || g.otAudit.ambiguous) ? "   <-- rejected as unsafe" : "");
    printf("  %-24s", "input channel peaks");
    for (UInt32 c = 0; c < g.inChans && c < 8; c++) printf(" %.3f", g.chPeak[c]);
    printf("\n");

    double raw[MAX_TRIALS], ts[MAX_TRIALS], pre[MAX_TRIALS], sched[MAX_TRIALS];
    int nr = 0, nts = 0, npre = 0, nsched = 0;
    int noSignal = 0, droppedT = 0, anchoredT = 0, inverted = 0;
    double worstNoise = 0, minPeak = 1e9;
    for (UInt32 i = 0; i < g.trial; i++) {
        const trial_t *tr = &g.t[i];
        const result_t r = analyse(tr);
        if (!r.ok) { noSignal++; continue; }
        if (r.noise > worstNoise) worstNoise = r.noise;
        if (r.peak < minPeak) minPeak = r.peak;
        if (r.inverted) inverted++;

        // A gap in delivered frames makes RTL_raw read short by the gap, so the
        // trial is rejected outright rather than averaged in.
        if (tr->badAtEnd != tr->badAtStart) { droppedT++; continue; }
        raw[nr++] = r.rawFrames;
        pre[npre++] = (tr->capAbs + r.onset) - tr->emitAbs;

        // A re-anchor invalidates only the sample-time reading; RTL_raw above
        // stands, because it never consulted those timestamps.
        if (tr->anchorAtEnd != tr->anchorAtStart) { anchoredT++; continue; }
        if (r.tsValid) {
            ts[nts++] = r.tsFrames;
            // Paired within the trial. Differencing medians of two different
            // trial sets is not this quantity and can be arbitrarily wrong.
            sched[nsched++] = r.rawFrames - r.tsFrames;
        }
    }

    printf("\n--- round trip ---\n");
    if (!nr) {
        printf("  no usable trial (%u attempted, %d without signal, %d dropped frames).\n",
               g.trial, noSignal, droppedT);
        printf("  check the loopback cable, output volume, and input gain; the\n"
               "  per-channel peaks above show where signal actually landed.\n");
        return 1;
    }
    printf("  %-24s %d of %u", "usable trials", nr, g.trial);
    if (noSignal)  printf("   (%d no signal)", noSignal);
    if (droppedT)  printf("   (%d spanned a frame gap)", droppedT);
    printf("\n");
    printf("  %-24s peak %.3f, noise %.5f (%.1f dB SNR)%s\n", "signal",
           minPeak, worstNoise,
           worstNoise > 0 ? 20.0 * log10(minPeak / worstNoise) : 99.0,
           (worstNoise > 0 && minPeak / worstNoise < 30.0) ? "   <-- weak" : "");
    if (inverted)
        printf("  %-24s %d trials (cable or device inverts)\n", "polarity inverted", inverted);

    stats("RTL_raw", raw, nr, sr);
    stats("RTL_raw (onset)", pre, npre, sr);
    if (nts) stats("RTL_ts (hw latency)", ts, nts, sr);
    else     printf("  %-24s <none: %d trials spanned a re-anchor>\n",
                    "RTL_ts (hw latency)", anchoredT);

    if (nsched)
        printf("\n  scheduling distance     %.2f fr measured, %u declared (2*io + safety)\n",
               median(sched, nsched), declared_sched(&dc));
    else
        printf("\n  scheduling distance     unavailable (no trial had usable timestamps)\n");

    if (nts) {
        const double mts = median(ts, nts);
        const double resid = mts - (double)declared_hw(&dc);
        printf("  hardware latency        %.2f fr measured, %u declared (dev + stream)\n",
               mts, declared_hw(&dc));
        printf("\n  RESIDUAL                %+.2f fr  (%+.3f ms)\n",
               resid, resid * 1000.0 / sr);
        printf("  The signed amount by which our declarations misstate the physical\n"
               "  path. Positive means we under-declare: audio really arrives later\n"
               "  than we claim. This is the Phase 2 reference-plane input.\n");
        if (anchoredT)
            printf("  (%d trial(s) excluded from RTL_ts for spanning a re-anchor.)\n", anchoredT);
    }
    return 0;
}
