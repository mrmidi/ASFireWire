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
//   ./rtl_loopback --selftest            check the detection maths, no hardware
//
// WHAT IT REPORTS
//
//   RTL_raw   frames between writing a sample into an output buffer and seeing
//             it in an input buffer, counted purely by accumulating the frame
//             count of each IOProc callback. It never reads mSampleTime, so it
//             is immune to kAudioDevicePropertyLatency / kAudioStreamPropertyLatency
//             -- the reporting-only fields. It IS sensitive to the safety
//             offsets and the buffer size, which legitimately move real timing.
//             This is the Phase 1 exit number.
//
//   RTL_ts    the same event pair expressed in the device sample-time domain
//             the HAL hands to clients:
//                 (inputTime.mSampleTime  + detect offset)
//               - (outputTime.mSampleTime + emit offset)
//             A host that compensates perfectly using our declarations sees
//             this. If every declaration were truthful it would be 0. What it
//             actually is, is the amount by which the driver's declared path
//             misstates the physical one -- signed. Positive means we under-
//             declare (real audio arrives later than we claim).
//
//   RTL_raw - RTL_ts is the total compensation the HAL currently applies, and
//   should reconcile with 2*io + latencies + safety offsets from the properties
//   block. When it does not, one of those properties is not reaching the HAL.
//
// BENCH SELF-CHECK (see README.md): change a reporting-only latency field in
// the profile, rebuild, rerun. RTL_raw must not move; RTL_ts must move by the
// same amount with the opposite sign. Do not probe with safety offsets or the
// client buffer size -- those change physical timing, so a moving measurement
// would prove nothing.
//
// All analysis happens after the run. The IOProc allocates nothing, logs
// nothing, and touches only preallocated storage.
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_TRIALS   128
#define MAX_WINDOW   16384
#define MAX_CHANNELS 64
#define MAX_CBLOG    (1u << 16)

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

// ------------------------------------------------------------- shared state
enum { ST_WARMUP, ST_GAP, ST_CAPTURE, ST_DONE };

typedef struct {
    double  emitAbs;       // absolute frame index of the impulse we wrote
    double  emitOt;        // outputTime.mSampleTime at that same frame
    double  capAbs;        // absolute frame index of the first captured frame
    double  capIt;         // inputTime.mSampleTime at that same frame
    UInt32  n;             // frames captured
    Float32 win[MAX_WINDOW];
} trial_t;

static struct {
    // configuration
    UInt32  outCh, inCh, trials, window, gapFrames, warmupFrames;
    Float32 amplitude;

    // IOProc state
    int     state;
    UInt32  trial;
    UInt32  remaining;     // frames left in the current warmup/gap
    double  pos;           // accumulated frames, the common cadence counter
    UInt32  cycles;
    UInt32  spans[16], spanCount;

    // continuity audit of the sample-time domains
    double  prevIt, prevOt;
    UInt32  itBreaks, otBreaks;
    double  worstItBreak, worstOtBreak;

    // per-channel peak, so a wrong --in-ch is visible rather than silent
    Float32 chPeak[MAX_CHANNELS];
    UInt32  inChans, outChans;

    trial_t t[MAX_TRIALS];
} g;

static OSStatus rtl_ioproc(AudioObjectID dev, const AudioTimeStamp *now,
                           const AudioBufferList *in, const AudioTimeStamp *it,
                           AudioBufferList *out, const AudioTimeStamp *ot,
                           void *ud) {
    (void)dev; (void)now; (void)ud;

    const UInt32 n = bl_frames(out) ? bl_frames(out) : bl_frames(in);
    if (!n) return noErr;

    // Output is silent unless this callback carries an impulse.
    if (out)
        for (UInt32 i = 0; i < out->mNumberBuffers; i++)
            if (out->mBuffers[i].mData)
                memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);

    g.cycles++;
    if (g.spanCount < 16) {
        int seen = 0;
        for (UInt32 i = 0; i < g.spanCount; i++) if (g.spans[i] == n) seen = 1;
        if (!seen) g.spans[g.spanCount++] = n;
    }

    // Sample-time continuity. A domain that does not advance by exactly the
    // frame count has jumped, and every number derived from it in this run is
    // suspect -- report it rather than averaging over it.
    if (it && (it->mFlags & kAudioTimeStampSampleTimeValid)) {
        if (g.prevIt != 0.0) {
            const double d = it->mSampleTime - g.prevIt;
            if (fabs(d - (double)n) > 0.5) {
                g.itBreaks++;
                if (fabs(d - (double)n) > fabs(g.worstItBreak)) g.worstItBreak = d - (double)n;
            }
        }
        g.prevIt = it->mSampleTime;
    }
    if (ot && (ot->mFlags & kAudioTimeStampSampleTimeValid)) {
        if (g.prevOt != 0.0) {
            const double d = ot->mSampleTime - g.prevOt;
            if (fabs(d - (double)n) > 0.5) {
                g.otBreaks++;
                if (fabs(d - (double)n) > fabs(g.worstOtBreak)) g.worstOtBreak = d - (double)n;
            }
        }
        g.prevOt = ot->mSampleTime;
    }

    if (in) {
        const UInt32 nc = bl_channels(in);
        if (nc && nc <= MAX_CHANNELS) {
            g.inChans = nc;
            for (UInt32 c = 0; c < nc; c++) {
                // Stride differs between the two buffer shapes; resolve each
                // frame rather than assuming one.
                for (UInt32 f = 0; f < n; f++) {
                    const Float32 *s = bl_slot(in, f, c);
                    if (!s) break;
                    const Float32 a = fabsf(*s);
                    if (a > g.chPeak[c]) g.chPeak[c] = a;
                }
            }
        }
    }
    if (out) g.outChans = bl_channels(out);

    switch (g.state) {
    case ST_WARMUP:
    case ST_GAP:
        if (g.remaining > n) { g.remaining -= n; break; }
        g.remaining = 0;
        if (g.trial >= g.trials) { g.state = ST_DONE; break; }
        // Emit at offset 0 of this callback so the emit instant is exactly the
        // callback's own timestamp, with no intra-buffer term to attribute.
        {
            trial_t *t = &g.t[g.trial];
            Float32 *o = bl_slot(out, 0, g.outCh);
            if (o) *o = g.amplitude;
            t->emitAbs = g.pos;
            t->emitOt  = (ot && (ot->mFlags & kAudioTimeStampSampleTimeValid))
                             ? ot->mSampleTime : 0.0;
            t->capAbs  = g.pos;
            t->capIt   = (it && (it->mFlags & kAudioTimeStampSampleTimeValid))
                             ? it->mSampleTime : 0.0;
            t->n       = 0;
            g.state    = ST_CAPTURE;
        }
        // fall through: capture this callback's input too
        __attribute__((fallthrough));
    case ST_CAPTURE: {
        trial_t *t = &g.t[g.trial];
        for (UInt32 f = 0; f < n && t->n < g.window; f++) {
            const Float32 *s = bl_slot(in, f, g.inCh);
            t->win[t->n++] = s ? *s : 0.0f;
        }
        if (t->n >= g.window) {
            g.trial++;
            g.remaining = g.gapFrames;
            g.state = (g.trial >= g.trials) ? ST_DONE : ST_GAP;
        }
        break;
    }
    default: break;
    }

    g.pos += (double)n;
    return noErr;
}

// ----------------------------------------------------------------- analysis
typedef struct {
    int    ok;
    double rawFrames, tsFrames;
    double peak, noise, onset;
    int    inverted;
} result_t;

static result_t analyse(const trial_t *t) {
    result_t r; memset(&r, 0, sizeof r);
    if (t->n < 16) return r;

    // The earliest physically plausible arrival is one buffer away, so the head
    // of the window is signal-free and usable as a noise reference.
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
    if (t->capIt != 0.0 && t->emitOt != 0.0)
        r.tsFrames = (t->capIt + detect) - t->emitOt;
    r.ok = 1;
    return r;
}

static int cmpd(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static void stats(const char *label, double *v, int n, double sr) {
    if (!n) { printf("  %-22s <none>\n", label); return; }
    qsort(v, n, sizeof(double), cmpd);
    const double med = (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
    double mean = 0;
    for (int i = 0; i < n; i++) mean += v[i];
    mean /= n;
    double sd = 0;
    for (int i = 0; i < n; i++) sd += (v[i] - mean) * (v[i] - mean);
    sd = n > 1 ? sqrt(sd / (n - 1)) : 0.0;
    printf("  %-22s %9.2f fr  %8.3f ms   [%.2f .. %.2f]  sd %.2f\n",
           label, med, med * 1000.0 / sr, v[0], v[n - 1], sd);
}

// ----------------------------------------------------------------- selftest
// The detection arithmetic is the part of this tool most able to be quietly
// wrong, and it is pure computation -- so it is checked without hardware.
// Synthesises trials whose delay is known exactly, including the linear-phase
// pre-ringing a real converter pair produces, and reports the recovery error.
static Float32 synth_ir(double i, double d, double amp) {
    const double fc = 0.45, L = 24.0;          // band-limited, symmetric
    const double t = i - d;
    if (fabs(t) > L) return 0.0f;
    const double x = 2.0 * fc * t;
    const double sinc = fabs(x) < 1e-9 ? 1.0 : sin(M_PI * x) / (M_PI * x);
    const double w = 0.5 * (1.0 + cos(M_PI * t / L));
    return (Float32)(amp * sinc * w);
}

static int selftest(void) {
    static const double delays[] = { 137.0, 512.0, 733.4, 1024.25, 2999.75 };
    const double skew = 900.0;
    unsigned rng = 12345;
    int failures = 0;

    printf("--- selftest: recovering known delays from synthetic trials ---\n");
    printf("  %-10s %-12s %-12s %-12s %s\n",
           "true", "RTL_raw", "err (fr)", "RTL_ts", "verdict");

    for (unsigned k = 0; k < sizeof delays / sizeof *delays; k++) {
        const double d = delays[k];
        static trial_t t;
        memset(&t, 0, sizeof t);
        t.n = 4096;
        t.emitAbs = 1000.0; t.capAbs = 1000.0;
        t.emitOt  = 5000.0; t.capIt  = 5000.0 - skew;
        const double amp = (k & 1) ? -0.9 : 0.9;   // exercise both polarities
        for (UInt32 i = 0; i < t.n; i++) {
            rng = rng * 1103515245u + 12345u;
            const double noise = ((double)((rng >> 16) & 0xFFFF) / 32768.0 - 1.0) * 1e-4;
            t.win[i] = synth_ir((double)i, d, amp) + (Float32)noise;
        }
        const result_t r = analyse(&t);
        const double err = r.rawFrames - d;
        const double tsErr = r.tsFrames - (d - skew);
        const int ok = r.ok && fabs(err) < 0.2 && fabs(tsErr) < 0.2;
        if (!ok) failures++;
        printf("  %-10.2f %-12.3f %-+12.4f %-12.3f %s\n",
               d, r.rawFrames, err, r.tsFrames, ok ? "ok" : "FAIL");
    }

    // A window with nothing in it must be rejected, not fitted to noise.
    {
        static trial_t t;
        memset(&t, 0, sizeof t);
        t.n = 4096;
        for (UInt32 i = 0; i < t.n; i++) {
            rng = rng * 1103515245u + 12345u;
            t.win[i] = (Float32)(((double)((rng >> 16) & 0xFFFF) / 32768.0 - 1.0) * 1e-4);
        }
        const result_t r = analyse(&t);
        const int ok = !r.ok;
        if (!ok) failures++;
        printf("  %-10s %-12s %-12s %-12s %s\n", "silence", "-", "-", "-",
               ok ? "ok (rejected)" : "FAIL (fitted noise)");
    }

    printf("  %s\n", failures ? "SELFTEST FAILED" : "selftest passed");
    return failures ? 1 : 0;
}

// ---------------------------------------------------------------- reporting
static void declared(AudioObjectID d, Float64 sr) {
    UInt32 io = 0, sin = 0, sout = 0, lin = 0, lout = 0;
    getprop(d, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
            &io, sizeof io);
    getprop(d, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput,  &sin,  sizeof sin);
    getprop(d, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &sout, sizeof sout);
    getprop(d, kAudioDevicePropertyLatency,      kAudioObjectPropertyScopeInput,  &lin,  sizeof lin);
    getprop(d, kAudioDevicePropertyLatency,      kAudioObjectPropertyScopeOutput, &lout, sizeof lout);
    const UInt32 stin  = stream_latency(d, kAudioObjectPropertyScopeInput);
    const UInt32 stout = stream_latency(d, kAudioObjectPropertyScopeOutput);
    const UInt32 tot = 2 * io + lin + lout + sin + sout + stin + stout;

    printf("  %-22s %.1f Hz, %u in / %u out\n", "format", sr,
           chan_count(d, kAudioObjectPropertyScopeInput),
           chan_count(d, kAudioObjectPropertyScopeOutput));
    printf("  %-22s %u\n", "buffer frame size", io);
    printf("  %-22s %u in / %u out\n", "safety offset", sin, sout);
    printf("  %-22s %u in / %u out\n", "device latency", lin, lout);
    printf("  %-22s %u in / %u out\n", "stream latency", stin, stout);
    printf("  %-22s %u fr  %.3f ms  = 2*%u + %u+%u lat + %u+%u safety + %u+%u stream\n",
           "declared round-trip", tot, sr > 0 ? tot * 1000.0 / sr : 0.0,
           io, lin, lout, sin, sout, stin, stout);
}

int main(int argc, char **argv) {
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

    printf("\n=== %s (device %u) ===\n", devName, dev);
    declared(dev, sr);

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
    for (int i = 0; i < timeout * 20 && g.state != ST_DONE; i++) usleep(50000);
    AudioDeviceStop(dev, id);
    AudioDeviceDestroyIOProcID(dev, id);

    if (g.state != ST_DONE)
        printf("\nWARNING: timed out after %d s with %u/%u trials -- IO may have stalled\n",
               timeout, g.trial, g.trials);

    printf("\n--- IO ---\n");
    printf("  %-22s %u\n", "callbacks", g.cycles);
    printf("  %-22s", "distinct spans");
    for (UInt32 i = 0; i < g.spanCount; i++) printf(" %u", g.spans[i]);
    printf("%s\n", g.spanCount > 1 ? "   <-- spans vary" : "");
    printf("  %-22s in %u breaks (worst %+.0f fr) / out %u breaks (worst %+.0f fr)%s\n",
           "sample-time continuity", g.itBreaks, g.worstItBreak, g.otBreaks, g.worstOtBreak,
           (g.itBreaks || g.otBreaks) ? "   <-- timeline jumped; RTL_ts is unreliable" : "");
    printf("  %-22s", "input channel peaks");
    for (UInt32 c = 0; c < g.inChans && c < 8; c++) printf(" %.3f", g.chPeak[c]);
    printf("\n");

    double raw[MAX_TRIALS], ts[MAX_TRIALS], pre[MAX_TRIALS];
    int nr = 0, nts = 0, npre = 0, bad = 0, inverted = 0;
    double worstNoise = 0, minPeak = 1e9;
    for (UInt32 i = 0; i < g.trial; i++) {
        const result_t r = analyse(&g.t[i]);
        if (!r.ok) { bad++; continue; }
        raw[nr++] = r.rawFrames;
        if (r.tsFrames != 0.0) ts[nts++] = r.tsFrames;
        // Onset rather than peak. Reported alongside so the gap between this
        // tool and any threshold-based analyser is visible rather than
        // mysterious: linear-phase pre-ringing makes the onset read early.
        pre[npre++] = (g.t[i].capAbs + r.onset) - g.t[i].emitAbs;
        if (r.noise > worstNoise) worstNoise = r.noise;
        if (r.peak < minPeak) minPeak = r.peak;
        if (r.inverted) inverted++;
    }

    printf("\n--- round trip ---\n");
    if (!nr) {
        printf("  no trial produced a detectable impulse (%u attempted).\n", g.trial);
        printf("  check the loopback cable, output volume, and input gain; the\n"
               "  per-channel peaks above show where signal actually landed.\n");
        return 1;
    }
    printf("  %-22s %d of %u%s\n", "usable trials", nr, g.trial,
           bad ? "   (rejected: no impulse above the noise floor)" : "");
    printf("  %-22s peak %.3f, noise %.5f (%.1f dB SNR)%s\n", "signal",
           minPeak, worstNoise,
           worstNoise > 0 ? 20.0 * log10(minPeak / worstNoise) : 99.0,
           (worstNoise > 0 && minPeak / worstNoise < 30.0) ? "   <-- weak" : "");
    if (inverted)
        printf("  %-22s %d of %d trials (cable or device inverts)\n",
               "polarity inverted", inverted, nr);
    stats("RTL_raw", raw, nr, sr);
    stats("RTL_raw (onset)", pre, npre, sr);
    if (nts) stats("RTL_ts residual", ts, nts, sr);

    if (nts) {
        qsort(raw, nr, sizeof(double), cmpd);
        qsort(ts, nts, sizeof(double), cmpd);
        const double mraw = raw[nr / 2], mts = ts[nts / 2];
        printf("\n  compensation applied by the HAL: %.2f fr (%.3f ms)\n",
               mraw - mts, (mraw - mts) * 1000.0 / sr);
        printf("  compare with the declared round-trip above; a mismatch means a\n"
               "  declared property is not reaching the HAL.\n");
        if (fabs(mts) > 4.0)
            printf("  RTL_ts residual is %+.1f fr: a perfectly compensating host is\n"
                   "  %s by that much. This is the Phase 2 reference-plane input.\n",
                   mts, mts > 0 ? "LATE" : "EARLY");
    }
    return 0;
}
