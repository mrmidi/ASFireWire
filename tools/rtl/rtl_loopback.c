// rtl_loopback.c -- uncompensated electrical round-trip latency measurement.
//
// macOS shell around rtl_core.{h,c}. This file only talks to CoreAudio: it
// finds the device, reads the declared geometry, runs the IOProc, and prints.
// Every decision about what a number means -- timeline audit, trial admission,
// detection, aggregation, evidence JSON -- lives in the platform-neutral core,
// which is unit-tested on any host (tests/tools/RtlCoreTests.cpp).
//
// Measures the physical round trip with an output->input cable and NO DAW in
// the path, so nothing along the way can silently compensate the number.
//
//   tools/rtl/build.sh                       build (or: cmake -S tools -B build/tools)
//   ./rtl_loopback                           list matching device + declared geometry
//   ./rtl_loopback -d ASFW --measure         run the measurement (starts real IO)
//   ./rtl_loopback -d ASFW --measure --frames 64 --trials 32 --window auto --json out.json
//   ./rtl_loopback --selftest                check the analysis maths, no hardware
//
// See tools/rtl/README.md for the meaning of every number, and
// documentation/LATENCY_VOCABULARY.md for the shared vocabulary.
//
// The IOProc allocates nothing, logs nothing, and touches only preallocated
// storage. All analysis happens after the run.
#include "rtl_core.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#ifndef RTL_GIT_SHA
#define RTL_GIT_SHA "unknown"
#endif

#define MAX_CHANNELS 64

static double g_h2s = 0.0;  // host ticks -> seconds
static void init_hostclock(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_h2s = (double)tb.numer / (double)tb.denom * 1e-9;
}

// State shared with the IOProc. Only `done`, `overloads` and the peaks are read
// by the main thread, and only after AudioDeviceStop() has quiesced the IOProc
// (except `done`, which is atomic).
static struct {
    rtl_engine_t     engine;
    UInt32           outCh, inCh;
    Float32          chPeak[MAX_CHANNELS];
    UInt32           inChans;
    _Atomic int      done;
    _Atomic unsigned overloads;
} g;

static rtl_trial_t g_trials[RTL_MAX_TRIALS];

// ------------------------------------------------------------------ helpers
static int getprop(AudioObjectID o, AudioObjectPropertySelector sel,
                   AudioObjectPropertyScope scope, void *buf, UInt32 sz) {
    AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, buf) == noErr;
}

static void cfstring_to_c(CFStringRef s, char *buf, size_t cap) {
    buf[0] = '\0';
    if (s) CFStringGetCString(s, buf, (CFIndex)cap, kCFStringEncodingUTF8);
}

static void dev_string(AudioObjectID d, AudioObjectPropertySelector sel, char *buf, size_t cap) {
    CFStringRef s = NULL;
    buf[0] = '\0';
    if (getprop(d, sel, kAudioObjectPropertyScopeGlobal, &s, sizeof s) && s) {
        cfstring_to_c(s, buf, cap);
        CFRelease(s);
    }
}

static UInt32 chan_count(AudioObjectID d, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, sc,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0, n = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz) != noErr || !sz) return 0;
    AudioBufferList *bl = malloc(sz);
    if (!bl) return 0;
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, bl) == noErr)
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
            n += bl->mBuffers[i].mNumberChannels;
    free(bl);
    return n;
}

// Worst stream latency in a scope. A scope with no streams legitimately has no
// value, so absence is not a missing declaration.
static UInt32 stream_latency(AudioObjectID d, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, sc,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0, worst = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz) != noErr || !sz) return 0;
    AudioObjectID *st = malloc(sz);
    if (!st) return 0;
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

// ------------------------------------------------------------------ declared
static rtl_declared_t read_declared(AudioObjectID d) {
    rtl_declared_t c;
    memset(&c, 0, sizeof c);
    rtl_declared_need(&c, getprop(d, kAudioDevicePropertyBufferFrameSize,
                                  kAudioObjectPropertyScopeGlobal, &c.io, sizeof c.io),
                      "buffer frame size");
    rtl_declared_need(&c, getprop(d, kAudioDevicePropertySafetyOffset,
                                  kAudioObjectPropertyScopeInput, &c.safIn, sizeof c.safIn),
                      "input safety offset");
    rtl_declared_need(&c, getprop(d, kAudioDevicePropertySafetyOffset,
                                  kAudioObjectPropertyScopeOutput, &c.safOut, sizeof c.safOut),
                      "output safety offset");
    rtl_declared_need(&c, getprop(d, kAudioDevicePropertyLatency,
                                  kAudioObjectPropertyScopeInput, &c.latIn, sizeof c.latIn),
                      "input device latency");
    rtl_declared_need(&c, getprop(d, kAudioDevicePropertyLatency,
                                  kAudioObjectPropertyScopeOutput, &c.latOut, sizeof c.latOut),
                      "output device latency");
    c.strIn  = stream_latency(d, kAudioObjectPropertyScopeInput);
    c.strOut = stream_latency(d, kAudioObjectPropertyScopeOutput);
    return c;
}

/// Request a client buffer size and report what the device actually adopted.
/// Returns 0 when the request was refused or silently altered.
static int request_buffer_frames(AudioObjectID d, UInt32 want, UInt32 *outGot) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 v = want;
    const OSStatus st = AudioObjectSetPropertyData(d, &a, 0, NULL, sizeof v, &v);
    UInt32 got = 0;
    if (!getprop(d, kAudioDevicePropertyBufferFrameSize,
                 kAudioObjectPropertyScopeGlobal, &got, sizeof got)) {
        *outGot = 0;
        return 0;
    }
    *outGot = got;
    return st == noErr && got == want;
}

static void print_declared(AudioObjectID d, const rtl_declared_t *c, Float64 sr) {
    printf("  %-24s %.1f Hz, %u in / %u out\n", "format", sr,
           chan_count(d, kAudioObjectPropertyScopeInput),
           chan_count(d, kAudioObjectPropertyScopeOutput));
    printf("  %-24s %u\n", "buffer frame size", c->io);
    printf("  %-24s %u in / %u out\n", "safety offset", c->safIn, c->safOut);
    printf("  %-24s %u in / %u out\n", "device latency", c->latIn, c->latOut);
    printf("  %-24s %u in / %u out\n", "stream latency", c->strIn, c->strOut);
    // Apple's composition, coreaudio-api 2004/Oct/msg00266 (Moore confirming
    // Behne): scheduling and hardware halves; only the second is in RTL_ts.
    printf("  %-24s %u fr  (2*%u io + %u+%u safety)\n", "declared scheduling",
           rtl_declared_sched(c), c->io, c->safIn, c->safOut);
    printf("  %-24s %u fr  (%u+%u dev + %u+%u stream)\n", "declared hw latency",
           rtl_declared_hw(c), c->latIn, c->latOut, c->strIn, c->strOut);
    int valid = 0;
    const UInt32 rt = rtl_declared_round_trip(c, &valid);
    if (valid)
        printf("  %-24s %u fr  %.3f ms\n", "declared round-trip", rt,
               sr > 0 ? rt * 1000.0 / sr : 0.0);
    else
        printf("  %-24s unavailable -- a declaration could not be read\n", "declared round-trip");
    for (int i = 0; i < c->nmissing && i < RTL_DECLARED_MAX_MISSING; i++)
        printf("  %-24s %s   <-- NOT READ\n", i ? "" : "declarations missing", c->missing[i]);
}

// ------------------------------------------------------------- CoreAudio IO
static OSStatus overload_listener(AudioObjectID o, UInt32 n,
                                  const AudioObjectPropertyAddress *a, void *u) {
    (void)o; (void)n; (void)a; (void)u;
    atomic_fetch_add_explicit(&g.overloads, 1u, memory_order_relaxed);
    return noErr;
}

// Read by the engine at trial boundaries, on the IO thread.
static uint32_t read_overloads(void *ctx) {
    (void)ctx;
    return atomic_load_explicit(&g.overloads, memory_order_relaxed);
}

typedef struct { const AudioBufferList *in; UInt32 ch; } ioctx_t;

static float io_input(void *ctx, uint32_t frame) {
    const ioctx_t *c = (const ioctx_t *)ctx;
    const Float32 *p = bl_slot(c->in, frame, c->ch);
    return p ? *p : 0.0f;
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

    if (in) {
        const UInt32 nc = bl_channels(in);
        if (nc && nc <= MAX_CHANNELS) {
            g.inChans = nc;
            for (UInt32 c = 0; c < nc; c++)
                for (UInt32 f = 0; f < n; f++) {
                    const Float32 *p = bl_slot(in, f, c);
                    if (!p) break;
                    const Float32 a = fabsf(*p);
                    if (a > g.chPeak[c]) g.chPeak[c] = a;
                }
        }
    }

    const int itOk = it && (it->mFlags & kAudioTimeStampSampleTimeValid);
    const int otOk = ot && (ot->mFlags & kAudioTimeStampSampleTimeValid);
    ioctx_t ctx = { in, g.inCh };
    // The wall clock is read here, directly, rather than taken from any
    // driver-supplied timestamp: a re-anchored sample timeline must not be able
    // to hide a gap in delivered frames.
    rtl_step_t st = {
        .n = n,
        .hostSec  = (double)mach_absolute_time() * g_h2s,
        .itSample = itOk ? it->mSampleTime : 0.0, .itValid = itOk,
        .otSample = otOk ? ot->mSampleTime : 0.0, .otValid = otOk,
        .input = io_input, .ctx = &ctx,
        .outSlot = bl_slot(out, 0, g.outCh),
    };
    if (rtl_engine_step(&g.engine, &st))
        atomic_store_explicit(&g.done, 1, memory_order_release);
    return noErr;
}

// ---------------------------------------------------------------- provenance
static void os_version(char *buf, size_t cap) {
    size_t len = cap;
    buf[0] = '\0';
    if (sysctlbyname("kern.osproductversion", buf, &len, NULL, 0) != 0) buf[0] = '\0';
}

static void utc_now(char *buf, size_t cap) {
    const time_t t = time(NULL);
    struct tm tmv;
    gmtime_r(&t, &tmv);
    strftime(buf, cap, "%Y-%m-%dT%H:%M:%SZ", &tmv);
}

static void join_argv(int argc, char **argv, char *buf, size_t cap) {
    size_t used = 0;
    buf[0] = '\0';
    for (int i = 0; i < argc && used + 1 < cap; i++) {
        const int w = snprintf(buf + used, cap - used, "%s%s", i ? " " : "", argv[i]);
        if (w < 0) break;
        used += (size_t)w;
    }
}

static void usage(FILE *f) {
    fprintf(f,
        "usage: rtl_loopback [-d <name-substring>] [--measure] [--frames N]\n"
        "                    [--trials N] [--window N|auto] [--out-ch N] [--in-ch N]\n"
        "                    [--amp F] [--json <path>] [--selftest]\n");
}

// --------------------------------------------------------------------- main
int main(int argc, char **argv) {
    init_hostclock();
    const char *filter = "ASFW";
    const char *jsonPath = NULL;
    int measure = 0, reqFrames = 0, autoWindow = 0;
    UInt32 trials = 20, window = 4096;
    Float32 amplitude = 0.9f;
    g.outCh = 0; g.inCh = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-d")       && i + 1 < argc) filter = argv[++i];
        else if (!strcmp(argv[i], "--measure"))                measure = 1;
        else if (!strcmp(argv[i], "--selftest")) {
            int f = 0;
            f += rtl_selftest(44100.0, stdout);
            f += rtl_selftest(48000.0, stdout);
            f += rtl_selftest(96000.0, stdout);
            printf("\n%s\n", f ? "SELFTEST FAILED" : "selftest passed at 44.1 / 48 / 96 kHz");
            return f ? 1 : 0;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) reqFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc) trials = (UInt32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) {
            const char *w = argv[++i];
            if (!strcmp(w, "auto")) autoWindow = 1; else window = (UInt32)atoi(w);
        }
        else if (!strcmp(argv[i], "--out-ch") && i + 1 < argc) g.outCh = (UInt32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--in-ch")  && i + 1 < argc) g.inCh  = (UInt32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--amp")    && i + 1 < argc) amplitude = (Float32)atof(argv[++i]);
        else if (!strcmp(argv[i], "--json")   && i + 1 < argc) jsonPath = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); usage(stderr); return 2; }
    }
    if (trials > RTL_MAX_TRIALS) trials = RTL_MAX_TRIALS;
    if (window > RTL_MAX_WINDOW) window = RTL_MAX_WINDOW;

    AudioObjectPropertyAddress da = { kAudioHardwarePropertyDevices,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &da, 0, NULL, &sz) != noErr) {
        fprintf(stderr, "cannot enumerate devices\n");
        return 1;
    }
    AudioObjectID *devs = malloc(sz);
    if (!devs) return 1;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &sz, devs);
    const UInt32 ndev = sz / sizeof(AudioObjectID);

    AudioObjectID dev = kAudioObjectUnknown;
    char devName[256] = {0}, devUid[256] = {0};
    for (UInt32 i = 0; i < ndev; i++) {
        char b[256];
        dev_string(devs[i], kAudioObjectPropertyName, b, sizeof b);
        if (!strstr(b, filter)) continue;
        if (!chan_count(devs[i], kAudioObjectPropertyScopeInput) ||
            !chan_count(devs[i], kAudioObjectPropertyScopeOutput)) {
            printf("skipping \"%s\": needs both input and output on one device\n", b);
            continue;
        }
        dev = devs[i];
        snprintf(devName, sizeof devName, "%s", b);
        dev_string(dev, kAudioDevicePropertyDeviceUID, devUid, sizeof devUid);
        break;
    }
    free(devs);
    if (dev == kAudioObjectUnknown) {
        fprintf(stderr, "no duplex device matching \"%s\"\n", filter);
        return 1;
    }

    UInt32 actualFrames = 0;
    if (reqFrames > 0) {
        if (!request_buffer_frames(dev, (UInt32)reqFrames, &actualFrames)) {
            // Buffer size sets the scheduling distance the measurement is
            // reconciled against, so a request that did not take must be
            // visible before any number is.
            fprintf(stderr,
                    "buffer frame size: requested %u, device reports %u -- "
                    "the run below is at %u, not %u\n",
                    (UInt32)reqFrames, actualFrames, actualFrames, (UInt32)reqFrames);
        }
    }

    // No silent 48 kHz fallback: every frame<->ms conversion below depends on it.
    Float64 sr = 0.0;
    if (!getprop(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                 &sr, sizeof sr) || !(sr > 0.0)) {
        fprintf(stderr, "cannot read the nominal sample rate of \"%s\"\n", devName);
        return 1;
    }

    const rtl_declared_t dc = read_declared(dev);
    if (!actualFrames) actualFrames = dc.io;
    printf("\n=== %s (device %u) ===\n", devName, dev);
    print_declared(dev, &dc, sr);

    if (autoWindow) {
        window = rtl_auto_window(&dc);
        printf("  %-24s %u fr (4x declared round trip, clamped)\n", "auto window", window);
    }

    if (!measure) {
        printf("\nDeclarations only. Pass --measure to start real IO and measure the\n"
               "physical path; connect an output->input cable first.\n");
        return 0;
    }

    rtl_engine_init(&g.engine, g_trials, trials, window,
                    (UInt32)(sr * 0.25), (UInt32)(sr * 0.5), amplitude, sr);
    g.engine.overloads = read_overloads;
    g.engine.overloadsCtx = NULL;

    printf("\nmeasuring: %u trials, window %u fr (max RTL %.1f ms), out ch %u -> in ch %u, amp %.2f\n",
           trials, window, (window - RTL_EDGE_GUARD_FRAMES) * 1000.0 / sr,
           g.outCh, g.inCh, amplitude);

    AudioObjectPropertyAddress ola = { kAudioDeviceProcessorOverload,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(dev, &ola, overload_listener, NULL);

    AudioDeviceIOProcID id = NULL;
    if (AudioDeviceCreateIOProcID(dev, rtl_ioproc, NULL, &id) != noErr) {
        fprintf(stderr, "cannot create IOProc\n");
        AudioObjectRemovePropertyListener(dev, &ola, overload_listener, NULL);
        return 1;
    }
    if (AudioDeviceStart(dev, id) != noErr) {
        fprintf(stderr, "cannot start device\n");
        AudioDeviceDestroyIOProcID(dev, id);
        AudioObjectRemovePropertyListener(dev, &ola, overload_listener, NULL);
        return 1;
    }

    const rtl_engine_t *e = &g.engine;
    const double expected =
        (e->warmupFrames + (double)trials * (e->gapFrames + window)) / sr;
    const int timeout = (int)(expected * 4.0) + 5;
    for (int i = 0; i < timeout * 20 &&
                    !atomic_load_explicit(&g.done, memory_order_acquire); i++)
        usleep(50000);
    const int completed = atomic_load_explicit(&g.done, memory_order_acquire);
    AudioDeviceStop(dev, id);
    AudioDeviceDestroyIOProcID(dev, id);
    AudioObjectRemovePropertyListener(dev, &ola, overload_listener, NULL);
    // AudioDeviceStop has quiesced the IOProc: the engine is this thread's now.

    if (!completed)
        printf("\nWARNING: timed out after %d s with %u/%u trials -- IO may have stalled\n",
               timeout, e->trial, trials);

    const unsigned overloads = atomic_load_explicit(&g.overloads, memory_order_relaxed);
    printf("\n--- IO ---\n");
    printf("  %-24s %u\n", "callbacks", e->cycles);
    printf("  %-24s", "distinct spans");
    for (UInt32 i = 0; i < e->spanCount; i++) printf(" %u", e->spans[i]);
    printf("%s\n", e->spanCount > 1 ? "   <-- spans vary" : "");
    printf("  %-24s %u\n", "processor overloads", overloads);
    // Every cause below rejects the whole trial. They are broken out because
    // they say which failure the machine or the driver has.
    printf("  %-24s %u events, %.0f frames lost\n", "delivered-frame gaps",
           rtl_engine_gap_events(e), rtl_engine_gap_frames(e));
    printf("  %-24s %u events (worst %+.0f fr)\n", "sample-time re-anchors",
           rtl_engine_anchor_events(e), rtl_engine_worst_anchor(e));
    printf("  %-24s %u\n", "clocks disagreed", rtl_engine_ambiguous(e));
    printf("  %-24s %u\n", "missing timestamps", rtl_engine_missing_ts(e));
    if (rtl_engine_gap_events(e) || rtl_engine_anchor_events(e) ||
        rtl_engine_ambiguous(e) || rtl_engine_missing_ts(e) || overloads)
        printf("  %-24s any of the above inside a trial rejects that whole trial\n", "");
    printf("  %-24s", "input channel peaks");
    for (UInt32 c = 0; c < g.inChans && c < 8; c++) printf(" %.3f", g.chPeak[c]);
    printf("\n");

    static rtl_trial_report_t reports[RTL_MAX_TRIALS];
    rtl_summary_t s;
    rtl_summarize(e, &dc, reports, &s);

    if (jsonPath) {
        char ts[32], osv[64], argvBuf[1024];
        utc_now(ts, sizeof ts);
        os_version(osv, sizeof osv);
        join_argv(argc, argv, argvBuf, sizeof argvBuf);
        const rtl_provenance_t p = {
            .tool = "rtl_loopback", .toolVersion = RTL_GIT_SHA, .timestampUtc = ts,
            .osVersion = osv, .deviceName = devName, .deviceUid = devUid,
            .argv = argvBuf, .sampleRate = sr,
            .bufferRequested = reqFrames > 0 ? (UInt32)reqFrames : 0u,
            .bufferActual = actualFrames, .window = window, .trialsRequested = trials,
            .overloadsTotal = overloads, .timedOut = !completed,
        };
        FILE *jf = fopen(jsonPath, "w");
        if (!jf || rtl_write_json(jf, &p, &dc, e, reports, &s) != 0) {
            fprintf(stderr, "cannot write %s\n", jsonPath);
            if (jf) fclose(jf);
            return 1;
        }
        fclose(jf);
        printf("  %-24s %s\n", "evidence written", jsonPath);
    }

    printf("\n--- round trip ---\n");
    printf("  %-24s %d of %u accepted", "trials", s.accepted, s.trialsRun);
    for (int v = 1; v < TRIAL_VERDICTS; v++)
        if (s.tally[v]) printf("   (%u %s)", s.tally[v], rtl_verdict_name((rtl_verdict_t)v));
    printf("\n");
    if (!s.accepted) {
        printf("  no trial was admitted. If the rejections are \"no signal\", check the\n"
               "  loopback cable, output volume and input gain -- the per-channel peaks\n"
               "  above show where signal actually landed. \"window edge\" means the\n"
               "  round trip is longer than the window: use --window auto or larger.\n"
               "  Otherwise the timeline was not quiet enough: raise --frames or quiet\n"
               "  the machine.\n");
        return 1;
    }
    printf("  %-24s peak %.3f, noise %.5f (%.1f dB SNR)%s\n", "signal",
           s.minPeak, s.worstNoise,
           s.worstNoise > 0 ? 20.0 * log10(s.minPeak / s.worstNoise) : 99.0,
           (s.worstNoise > 0 && s.minPeak / s.worstNoise < 30.0) ? "   <-- weak" : "");
    if (s.inverted)
        printf("  %-24s %d trials (cable or device inverts)\n", "polarity inverted", s.inverted);

    const rtl_stat_t *rows[3] = { &s.raw, &s.onset, &s.ts };
    const char *labels[3] = { "RTL_raw", "RTL_raw (onset)", "RTL_ts (hw latency)" };
    for (int i = 0; i < 3; i++)
        printf("  %-24s %9.2f fr  %8.3f ms   [%.2f .. %.2f]  sd %.2f\n", labels[i],
               rows[i]->median, rows[i]->median * 1000.0 / sr, rows[i]->min, rows[i]->max,
               rows[i]->sd);

    printf("\n  scheduling distance     %.2f fr measured, %u declared (2*io + safety)\n",
           s.sched.median, s.declaredSched);
    printf("  hardware latency        %.2f fr measured, %u declared (dev + stream)\n",
           s.ts.median, s.declaredHw);
    if (!s.residualValid) {
        // Fails closed for the same reason trial admission does.
        printf("\n  RESIDUAL                unavailable -- %d declared propert%s could not be read.\n",
               dc.nmissing, dc.nmissing == 1 ? "y" : "ies");
        printf("  The measurements above stand on their own; the residual does\n"
               "  not, because the declaration it subtracts is incomplete.\n");
    } else {
        printf("\n  RESIDUAL                %+.2f fr  (%+.3f ms)\n",
               s.residualFrames, s.residualFrames * 1000.0 / sr);
        printf("  The signed amount by which our declarations misstate the physical\n"
               "  path. Positive means we under-declare: audio really arrives later\n"
               "  than we claim.\n");
    }
    return 0;
}
