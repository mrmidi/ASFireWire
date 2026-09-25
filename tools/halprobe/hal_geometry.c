// hal_geometry.c -- HAL-side diagnostic for ASFW audio geometry (macOS shell).
//
// Reads back what Core Audio actually reports and does, as opposed to what the
// driver asked for. Requires no dext rebuild and no privileges. The maths and
// the JSON record live in hal_core.{h,c} (unit-tested on any host); this file
// only talks to the HAL.
//
//   tools/halprobe/build.sh
//   ./hal_geometry                         snapshot every device (passive, no IO)
//   ./hal_geometry -d ASFW                 snapshot devices matching a name substring
//   ./hal_geometry -d ASFW --clock 20      STARTS IO: watch the HAL timeline 20 s
//   ./hal_geometry -d ASFW --io 10         STARTS IO: real callback spans + timing
//   ./hal_geometry -d ASFW --sweep         STARTS IO and CHANGES the buffer size
//                                          across its range, then restores it
//   ./hal_geometry -d ASFW --json out.json write the evidence record as well
//
// --clock, --io and --sweep start real IO on the device. Do not run them while
// recording or tracking.
#include "hal_core.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#ifndef HAL_GIT_SHA
#define HAL_GIT_SHA "unknown"
#endif

#define MAX_DEVICES   32
#define MAX_CLOCK_OBS 65536
#define MAX_CALLBACKS 65536

static double g_h2s = 0.0;  // host ticks -> seconds
static void init_hostclock(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_h2s = (double)tb.numer / (double)tb.denom * 1e-9;
}

// Set by SIGINT/SIGTERM; long waits poll it so a sweep can restore the buffer
// size before exiting. Nothing CoreAudio is called from the handler itself.
static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

// ---------------------------------------------------------------- properties
static int getprop(AudioObjectID o, AudioObjectPropertySelector sel,
                   AudioObjectPropertyScope scope, void *buf, UInt32 sz) {
    AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, buf) == noErr;
}

static hal_u32_t getu32(AudioObjectID d, AudioObjectPropertySelector sel,
                        AudioObjectPropertyScope sc) {
    hal_u32_t v = {0, 0};
    v.valid = getprop(d, sel, sc, &v.value, sizeof v.value);
    if (!v.valid) v.value = 0;
    return v;
}

static void dev_string(AudioObjectID d, AudioObjectPropertySelector sel, char *buf, size_t cap) {
    CFStringRef s = NULL;
    buf[0] = '\0';
    if (getprop(d, sel, kAudioObjectPropertyScopeGlobal, &s, sizeof s) && s) {
        CFStringGetCString(s, buf, (CFIndex)cap, kCFStringEncodingUTF8);
        CFRelease(s);
    }
}

static UInt32 chans(AudioObjectID d, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, sc,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0, n = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz) != noErr || !sz) return 0;
    AudioBufferList *bl = malloc(sz);
    if (!bl) return 0;
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, bl) == noErr)
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++) n += bl->mBuffers[i].mNumberChannels;
    free(bl);
    return n;
}

// A scope with no streams legitimately has no stream latency: not "missing".
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

static void print_u32(const char *label, hal_u32_t v) {
    if (v.valid) printf("  %-28s %u\n", label, v.value);
    else         printf("  %-28s <NOT READ>\n", label);
}

// ---------------------------------------------------------------- snapshot
static char g_names[MAX_DEVICES][256];
static char g_uids[MAX_DEVICES][256];

static void snapshot(AudioObjectID d, hal_device_report_t *r, char *name, char *uid) {
    dev_string(d, kAudioObjectPropertyName, name, 256);
    dev_string(d, kAudioDevicePropertyDeviceUID, uid, 256);
    r->name = name;
    r->uid = uid;
    r->objectId = d;

    printf("\n=== device %u ===\n", d);
    printf("  %-28s %s\n", "name", name[0] ? name : "<NOT READ>");
    printf("  %-28s %s\n", "uid", uid[0] ? uid : "<NOT READ>");

    Float64 sr = 0;
    r->nominalRateValid = getprop(d, kAudioDevicePropertyNominalSampleRate,
                                  kAudioObjectPropertyScopeGlobal, &sr, sizeof sr) && sr > 0;
    r->nominalRate = r->nominalRateValid ? sr : 0.0;
    if (r->nominalRateValid) printf("  %-28s %.1f\n", "nominal sample rate", sr);
    else                     printf("  %-28s <NOT READ>\n", "nominal sample rate");

    r->inChannels = chans(d, kAudioObjectPropertyScopeInput);
    r->outChannels = chans(d, kAudioObjectPropertyScopeOutput);
    printf("  %-28s %u in / %u out\n", "channels", r->inChannels, r->outChannels);

    r->ztsPeriod = getu32(d, 'ring', kAudioObjectPropertyScopeGlobal);
    if (r->ztsPeriod.valid && r->nominalRateValid)
        printf("  %-28s %u   (%.1f ms @ %.0f Hz)\n", "ZTS period ('ring')",
               r->ztsPeriod.value, r->ztsPeriod.value * 1000.0 / sr, sr);
    else
        print_u32("ZTS period ('ring')", r->ztsPeriod);

    r->bufferFrames = getu32(d, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    print_u32("buffer frame size", r->bufferFrames);
    AudioValueRange range;
    r->bufferRangeValid = getprop(d, kAudioDevicePropertyBufferFrameSizeRange,
                                  kAudioObjectPropertyScopeGlobal, &range, sizeof range);
    if (r->bufferRangeValid) {
        r->bufferMin = range.mMinimum;
        r->bufferMax = range.mMaximum;
        printf("  %-28s %.0f .. %.0f\n", "buffer size range", range.mMinimum, range.mMaximum);
    } else {
        printf("  %-28s <NOT READ>\n", "buffer size range");
    }
    r->variableBufferSizes = getu32(d, kAudioDevicePropertyUsesVariableBufferFrameSizes,
                                    kAudioObjectPropertyScopeGlobal);
    print_u32("uses variable buffer sizes", r->variableBufferSizes);

    // Declarations: every unreadable one is named, never summed as zero.
    rtl_declared_t *c = &r->declared;
    memset(c, 0, sizeof *c);
    c->io = r->bufferFrames.value;
    rtl_declared_need(c, r->bufferFrames.valid, "buffer frame size");
    rtl_declared_need(c, getprop(d, kAudioDevicePropertySafetyOffset,
                                 kAudioObjectPropertyScopeInput, &c->safIn, sizeof c->safIn),
                      "input safety offset");
    rtl_declared_need(c, getprop(d, kAudioDevicePropertySafetyOffset,
                                 kAudioObjectPropertyScopeOutput, &c->safOut, sizeof c->safOut),
                      "output safety offset");
    rtl_declared_need(c, getprop(d, kAudioDevicePropertyLatency,
                                 kAudioObjectPropertyScopeInput, &c->latIn, sizeof c->latIn),
                      "input device latency");
    rtl_declared_need(c, getprop(d, kAudioDevicePropertyLatency,
                                 kAudioObjectPropertyScopeOutput, &c->latOut, sizeof c->latOut),
                      "output device latency");
    c->strIn  = stream_latency(d, kAudioObjectPropertyScopeInput);
    c->strOut = stream_latency(d, kAudioObjectPropertyScopeOutput);
    printf("  %-28s %u in / %u out\n", "safety offset", c->safIn, c->safOut);
    printf("  %-28s %u in / %u out\n", "device latency", c->latIn, c->latOut);
    printf("  %-28s %u in / %u out\n", "stream latency", c->strIn, c->strOut);

    r->clockAlgorithm = getu32(d, 'clok', kAudioObjectPropertyScopeGlobal);
    r->clockIsStable = getu32(d, 'cstb', kAudioObjectPropertyScopeGlobal);
    r->runningSomewhere = getu32(d, kAudioDevicePropertyDeviceIsRunningSomewhere,
                                 kAudioObjectPropertyScopeGlobal);
    print_u32("clock algorithm", r->clockAlgorithm);
    print_u32("clock is stable", r->clockIsStable);
    print_u32("running somewhere", r->runningSomewhere);

    // Apple's composition (Moore/Behne, coreaudio-api 2004): 2 x IO size +
    // device latency + safety offset + stream latency, both directions.
    int valid = 0;
    const uint32_t tot = rtl_declared_round_trip(c, &valid);
    if (valid && r->nominalRateValid) {
        printf("  %-28s %u frames = %.2f ms\n", "PREDICTED round-trip", tot, tot * 1000.0 / sr);
        printf("      = 2*%u io + %u+%u dev lat + %u+%u safety + %u+%u stream lat\n",
               c->io, c->latIn, c->latOut, c->safIn, c->safOut, c->strIn, c->strOut);
    } else {
        printf("  %-28s unavailable -- declarations not read:", "PREDICTED round-trip");
        for (int i = 0; i < c->nmissing && i < RTL_DECLARED_MAX_MISSING; i++)
            printf("%s %s", i ? "," : "", c->missing[i]);
        if (!r->nominalRateValid) printf("%s nominal sample rate", c->nmissing ? "," : "");
        printf("\n");
    }
}

// ---------------------------------------------------------------- IO
// Preallocated IOProc storage. The callback only stores; analysis runs after
// AudioDeviceStop has quiesced it.
static struct {
    hal_iostats_t  stats;
    double         hostSec[MAX_CALLBACKS];
    _Atomic int    n;
} g_io;

static OSStatus ioproc(AudioObjectID d, const AudioTimeStamp *now,
                       const AudioBufferList *in, const AudioTimeStamp *it,
                       AudioBufferList *out, const AudioTimeStamp *ot, void *ud) {
    (void)d; (void)now; (void)it; (void)ot; (void)ud;
    const double host = (double)mach_absolute_time() * g_h2s;
    // Silence whatever we are handed: this client must not make noise.
    if (out)
        for (UInt32 i = 0; i < out->mNumberBuffers; i++)
            if (out->mBuffers[i].mData)
                memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);
    UInt32 frames = 0;
    const AudioBufferList *bl = (out && out->mNumberBuffers) ? out : in;
    if (bl && bl->mNumberBuffers && bl->mBuffers[0].mNumberChannels)
        frames = bl->mBuffers[0].mDataByteSize /
                 (UInt32)(sizeof(Float32) * bl->mBuffers[0].mNumberChannels);
    hal_iostats_record(&g_io.stats, frames);
    const int i = atomic_load_explicit(&g_io.n, memory_order_relaxed);
    if (i < MAX_CALLBACKS) {
        g_io.hostSec[i] = host;
        atomic_store_explicit(&g_io.n, i + 1, memory_order_release);
    }
    return noErr;
}

static void io_reset(void) {
    memset(&g_io.stats, 0, sizeof g_io.stats);
    atomic_store_explicit(&g_io.n, 0, memory_order_relaxed);
}

static int io_start(AudioObjectID d, AudioDeviceIOProcID *id) {
    io_reset();
    *id = NULL;
    if (AudioDeviceCreateIOProcID(d, ioproc, NULL, id) != noErr) return -1;
    if (AudioDeviceStart(d, *id) != noErr) {
        AudioDeviceDestroyIOProcID(d, *id);
        *id = NULL;
        return -1;
    }
    return 0;
}

static void io_stop(AudioObjectID d, AudioDeviceIOProcID id) {
    if (!id) return;
    AudioDeviceStop(d, id);
    AudioDeviceDestroyIOProcID(d, id);
}

// Sleeps in small steps so a signal can interrupt a long run.
static void wait_seconds(double s) {
    for (double t = 0; t < s && !g_stop; t += 0.05) usleep(50000);
}

// -------------------------------------------------------------- clock watch
static void watch_clock(AudioObjectID d, int seconds, hal_device_report_t *r) {
    printf("\n--- clock watch, %ds (STARTS IO) ---\n", seconds);
    printf("  measures the HAL-published timeline (driven by the driver's ZTS),\n"
           "  not the physical word clock\n");
    if (!r->nominalRateValid) {
        // Every figure below is relative to the nominal rate; do not guess one.
        printf("  refused: nominal sample rate could not be read\n");
        return;
    }
    const double sr = r->nominalRate;
    // AudioDeviceGetCurrentTime only works while THIS client has the device
    // running -- another process holding it open is not enough.
    AudioDeviceIOProcID own = NULL;
    const int started = (io_start(d, &own) == 0);
    if (!started) printf("  (could not start our own IOProc; times may be unavailable)\n");
    usleep(200000);

    static double st[MAX_CLOCK_OBS], ht[MAX_CLOCK_OBS];
    int n = 0;
    for (int i = 0; i < seconds * 50 && n < MAX_CLOCK_OBS && !g_stop; i++) {
        AudioTimeStamp ts;
        memset(&ts, 0, sizeof ts);
        ts.mFlags = kAudioTimeStampSampleHostTimeValid;
        if (AudioDeviceGetCurrentTime(d, &ts) == noErr &&
            (ts.mFlags & kAudioTimeStampSampleTimeValid) &&
            (ts.mFlags & kAudioTimeStampHostTimeValid)) {
            st[n] = ts.mSampleTime;
            ht[n] = (double)ts.mHostTime * g_h2s;
            n++;
        }
        usleep(20000);
    }
    if (started) io_stop(d, own);
    r->clockRan = 1;
    r->clockSamples = n;
    if (n < 10) {
        printf("  only %d usable samples -- device may not permit client IO\n", n);
        return;
    }
    r->clockFit = hal_fit_line(ht, st, n);
    // Forward re-anchor threshold: a quarter of the nominal rate's 20 ms poll
    // step is far above jitter, far below any real timeline re-origin.
    r->clockJumps = hal_classify_jumps(ht, st, n, sr, sr * 0.005);
    printf("  samples              %d over %.1fs\n", n, r->clockFit.spanSeconds);
    if (!r->clockFit.ok) {
        printf("  fit failed (degenerate host time span)\n");
        return;
    }
    printf("  fitted rate          %.3f Hz  (nominal %.1f)\n", r->clockFit.slope, sr);
    printf("  clock error          %+.1f ppm\n", hal_ppm(r->clockFit.slope, sr));
    printf("  worst residual       %.2f frames %s\n", r->clockFit.worstResidual,
           r->clockFit.worstResidual > 2.0 ? "  <-- exceeds the ~1 sample target" : "");
    printf("  backward jumps       %u %s\n", r->clockJumps.backward,
           r->clockJumps.backward ? " <-- timeline went backwards" : "");
    printf("  forward re-anchors   %u", r->clockJumps.forward);
    if (r->clockJumps.forward) printf("  (worst +%.0f fr) <-- timeline discontinuity", r->clockJumps.worstForward);
    printf("\n");
}

// ---------------------------------------------------------------- IO probe
static void io_probe(AudioObjectID d, int seconds, hal_device_report_t *r) {
    printf("\n--- IOProc probe, %ds (STARTS IO) ---\n", seconds);
    AudioDeviceIOProcID id = NULL;
    if (io_start(d, &id) != 0) { printf("  cannot start IO\n"); return; }
    wait_seconds(seconds);
    io_stop(d, id);
    r->ioRan = 1;
    r->io = g_io.stats;
    const int n = atomic_load_explicit(&g_io.n, memory_order_acquire);
    r->ioIntervalsUs = hal_callback_intervals_us(g_io.hostSec, n);
    printf("  cycles               %u\n", r->io.cycles);
    printf("  distinct frameCounts %u:", r->io.nspans);
    for (UInt32 i = 0; i < r->io.nspans; i++) printf(" %u", r->io.spans[i]);
    printf("\n");
    if (r->ioIntervalsUs.n > 0)
        printf("  callback interval    median %.1f us  [%.1f .. %.1f]  sd %.1f us\n",
               r->ioIntervalsUs.median, r->ioIntervalsUs.min, r->ioIntervalsUs.max,
               r->ioIntervalsUs.sd);
    if (r->io.nspans > 1)
        printf("  --> spans VARY. Any code assuming a fixed callback size is wrong.\n");
}

// ------------------------------------------------------------------- sweep
static AudioObjectID g_restoreDev = kAudioObjectUnknown;
static UInt32 g_restoreFrames = 0;
static int g_restorePending = 0;

static int set_buffer_frames(AudioObjectID d, UInt32 v) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    return AudioObjectSetPropertyData(d, &a, 0, NULL, sizeof v, &v) == noErr;
}

// Runs on normal exit and after a signal-interrupted sweep.
static void restore_buffer_size(void) {
    if (!g_restorePending) return;
    g_restorePending = 0;
    if (set_buffer_frames(g_restoreDev, g_restoreFrames))
        printf("  buffer size restored to %u\n", g_restoreFrames);
    else
        fprintf(stderr, "WARNING: could not restore buffer size %u\n", g_restoreFrames);
}

static void sweep(AudioObjectID d, hal_device_report_t *r) {
    printf("\n--- buffer size sweep (STARTS IO, CHANGES buffer size) ---\n");
    if (!r->bufferFrames.valid || !r->bufferRangeValid) {
        printf("  refused: current buffer size or its range could not be read,\n"
               "  so it could not be restored afterwards\n");
        return;
    }
    uint32_t sizes[HAL_MAX_SWEEP_ROWS];
    const int nsizes = hal_sweep_sizes(r->bufferMin, r->bufferMax, sizes, HAL_MAX_SWEEP_ROWS);
    g_restoreDev = d;
    g_restoreFrames = r->bufferFrames.value;
    g_restorePending = 1;
    r->sweepRan = 1;
    printf("  %-10s %-12s %-12s %s\n", "requested", "read back", "verdict", "observed frameCounts");
    for (int i = 0; i < nsizes && !g_stop; i++) {
        hal_sweep_row_t *row = &r->sweep[r->sweepRows++];
        memset(row, 0, sizeof *row);
        row->requested = sizes[i];
        const int setOk = set_buffer_frames(d, sizes[i]);
        UInt32 back = 0;
        const int backOk = getprop(d, kAudioDevicePropertyBufferFrameSize,
                                   kAudioObjectPropertyScopeGlobal, &back, sizeof back);
        row->readBack = back;
        row->verdict = hal_sweep_verdict(setOk, backOk, sizes[i], back);
        AudioDeviceIOProcID id = NULL;
        if (io_start(d, &id) == 0) {
            wait_seconds(2.0);
            io_stop(d, id);
            row->ioOk = 1;
            row->io = g_io.stats;
        }
        printf("  %-10u %-12u %-12s", row->requested, row->readBack,
               hal_sweep_verdict_id(row->verdict));
        if (!row->ioOk) printf(" (io failed)");
        for (UInt32 k = 0; k < row->io.nspans; k++) printf(" %u", row->io.spans[k]);
        printf("\n");
    }
    r->sweepRestoredTo = g_restoreFrames;
    r->sweepRestoreOk = set_buffer_frames(d, g_restoreFrames);
    g_restorePending = !r->sweepRestoreOk;
    printf("  %s %u\n", r->sweepRestoreOk ? "restored to" : "FAILED to restore", g_restoreFrames);
}

// -------------------------------------------------------------- provenance
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

static void usage(FILE *f) {
    fprintf(f,
        "usage: hal_geometry [-d <name-substring>] [--clock SECONDS] [--io SECONDS]\n"
        "                    [--sweep] [--json <path>]\n"
        "  --clock, --io and --sweep start real IO; --sweep changes the buffer size\n");
}

// -------------------------------------------------------------------- main
int main(int argc, char **argv) {
    init_hostclock();
    const char *filter = NULL, *jsonPath = NULL;
    int clocks = 0, ios = 0, dosweep = 0;
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-d") && i + 1 < argc)      filter = argv[++i];
        else if (!strcmp(argv[i], "--clock") && i + 1 < argc) clocks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--io") && i + 1 < argc)    ios = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sweep"))                 dosweep = 1;
        else if (!strcmp(argv[i], "--json") && i + 1 < argc)  jsonPath = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        else if (argv[i][0] != '-')                           filter = argv[i];
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); usage(stderr); return 2; }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    atexit(restore_buffer_size);

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
    const UInt32 n = sz / sizeof(AudioObjectID);

    static hal_device_report_t reports[MAX_DEVICES];
    int nrep = 0;
    for (UInt32 i = 0; i < n && nrep < MAX_DEVICES && !g_stop; i++) {
        char nm[256];
        dev_string(devs[i], kAudioObjectPropertyName, nm, sizeof nm);
        if (filter && !strstr(nm, filter)) continue;
        hal_device_report_t *r = &reports[nrep];
        memset(r, 0, sizeof *r);
        snapshot(devs[i], r, g_names[nrep], g_uids[nrep]);
        if (clocks > 0) watch_clock(devs[i], clocks, r);
        if (ios > 0)    io_probe(devs[i], ios, r);
        if (dosweep)    sweep(devs[i], r);
        nrep++;
    }
    free(devs);
    restore_buffer_size();

    if (filter && nrep == 0) {
        fprintf(stderr, "no device matching \"%s\"\n", filter);
        return 1;
    }

    if (jsonPath) {
        char ts[32], osv[64], argvBuf[1024];
        utc_now(ts, sizeof ts);
        os_version(osv, sizeof osv);
        size_t used = 0;
        argvBuf[0] = '\0';
        for (int i = 0; i < argc && used + 1 < sizeof argvBuf; i++) {
            const int w = snprintf(argvBuf + used, sizeof argvBuf - used, "%s%s", i ? " " : "", argv[i]);
            if (w < 0) break;
            used += (size_t)w;
        }
        const hal_provenance_t p = { HAL_GIT_SHA, ts, osv, argvBuf };
        FILE *jf = fopen(jsonPath, "w");
        if (!jf || hal_write_json(jf, &p, reports, nrep) != 0) {
            fprintf(stderr, "cannot write %s\n", jsonPath);
            if (jf) fclose(jf);
            return 1;
        }
        fclose(jf);
        printf("\nevidence written: %s\n", jsonPath);
    }
    return g_stop ? 130 : 0;
}
