// hal_geometry.c -- HAL-side diagnostic for ASFW audio geometry.
//
// Reads back what Core Audio actually reports and does, as opposed to what the
// driver asked for. Requires no dext rebuild and no privileges.
//
//   clang -O1 -o hal_geometry hal_geometry.c -framework CoreAudio -framework CoreFoundation
//
//   ./hal_geometry                     snapshot every device
//   ./hal_geometry -d ASFW             snapshot devices matching a name substring
//   ./hal_geometry -d ASFW --clock 20  watch the clock for 20s: slope, ppm, jumps
//   ./hal_geometry -d ASFW --io 10     install an IOProc for 10s: actual spans
//   ./hal_geometry -d ASFW --sweep     set buffer size 32..4096, observe actual spans
//
// --sweep and --io start real IO on the device. Do not run them while tracking.
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

#define MAXOBS 65536

static double g_h2s = 0.0;                      // host ticks -> seconds
static void init_hostclock(void) {
    mach_timebase_info_data_t tb; mach_timebase_info(&tb);
    g_h2s = (double)tb.numer / (double)tb.denom * 1e-9;
}

// ---------------------------------------------------------------- properties
static int getprop(AudioObjectID o, AudioObjectPropertySelector sel,
                   AudioObjectPropertyScope scope, void *buf, UInt32 sz) {
    AudioObjectPropertyAddress a = { sel, scope, kAudioObjectPropertyElementMain };
    return AudioObjectGetPropertyData(o, &a, 0, NULL, &sz, buf) == noErr;
}
static void show_u32(AudioObjectID d, const char *label,
                     AudioObjectPropertySelector sel, AudioObjectPropertyScope sc) {
    UInt32 v = 0;
    if (getprop(d, sel, sc, &v, sizeof v)) printf("  %-28s %u\n", label, v);
    else                                   printf("  %-28s <n/a>\n", label);
}
static void cfshow(const char *label, CFStringRef s) {
    char b[256] = {0};
    if (s && CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8))
        printf("  %-28s %s\n", label, b);
}
static CFStringRef dev_name(AudioObjectID d) {
    CFStringRef s = NULL; UInt32 sz = sizeof s;
    getprop(d, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &s, sz);
    return s;
}
static UInt32 chans(AudioObjectID d, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, sc,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0, n = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz) != noErr || !sz) return 0;
    AudioBufferList *bl = malloc(sz);
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, bl) == noErr)
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++) n += bl->mBuffers[i].mNumberChannels;
    free(bl);
    return n;
}
static UInt32 stream_latency(AudioObjectID d, AudioObjectPropertyScope sc) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams, sc,
                                     kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(d, &a, 0, NULL, &sz) != noErr || !sz) return 0;
    AudioObjectID *st = malloc(sz);
    UInt32 worst = 0;
    if (AudioObjectGetPropertyData(d, &a, 0, NULL, &sz, st) == noErr) {
        for (UInt32 i = 0; i < sz / sizeof(AudioObjectID); i++) {
            UInt32 v = 0;
            if (getprop(st[i], kAudioStreamPropertyLatency,
                        kAudioObjectPropertyScopeGlobal, &v, sizeof v) && v > worst)
                worst = v;
        }
    }
    free(st);
    return worst;
}

static void snapshot(AudioObjectID d) {
    CFStringRef nm = dev_name(d), uid = NULL;
    getprop(d, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal,
            &uid, sizeof uid);
    printf("\n=== device %u ===\n", d);
    cfshow("name", nm);
    cfshow("uid", uid);

    Float64 sr = 0;
    getprop(d, kAudioDevicePropertyNominalSampleRate,
            kAudioObjectPropertyScopeGlobal, &sr, sizeof sr);
    printf("  %-28s %.1f\n", "nominal sample rate", sr);
    printf("  %-28s %u in / %u out\n", "channels",
           chans(d, kAudioObjectPropertyScopeInput),
           chans(d, kAudioObjectPropertyScopeOutput));

    UInt32 zts = 0;
    if (getprop(d, 'ring', kAudioObjectPropertyScopeGlobal, &zts, sizeof zts) && sr > 0)
        printf("  %-28s %u   (%.1f ms @ %.0f Hz)\n", "ZTS period ('ring')",
               zts, zts * 1000.0 / sr, sr);
    else show_u32(d, "ZTS period ('ring')", 'ring', kAudioObjectPropertyScopeGlobal);

    show_u32(d, "buffer frame size", kAudioDevicePropertyBufferFrameSize,
             kAudioObjectPropertyScopeGlobal);
    AudioValueRange r;
    if (getprop(d, kAudioDevicePropertyBufferFrameSizeRange,
                kAudioObjectPropertyScopeGlobal, &r, sizeof r))
        printf("  %-28s %.0f .. %.0f\n", "buffer size range", r.mMinimum, r.mMaximum);
    show_u32(d, "uses variable buffer sizes", kAudioDevicePropertyUsesVariableBufferFrameSizes,
             kAudioObjectPropertyScopeGlobal);

    UInt32 sin = 0, sout = 0, lin = 0, lout = 0;
    getprop(d, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeInput,  &sin,  sizeof sin);
    getprop(d, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput, &sout, sizeof sout);
    getprop(d, kAudioDevicePropertyLatency,      kAudioObjectPropertyScopeInput,  &lin,  sizeof lin);
    getprop(d, kAudioDevicePropertyLatency,      kAudioObjectPropertyScopeOutput, &lout, sizeof lout);
    UInt32 stin  = stream_latency(d, kAudioObjectPropertyScopeInput);
    UInt32 stout = stream_latency(d, kAudioObjectPropertyScopeOutput);
    printf("  %-28s %u in / %u out\n", "safety offset",  sin,  sout);
    printf("  %-28s %u in / %u out\n", "device latency", lin,  lout);
    printf("  %-28s %u in / %u out\n", "stream latency", stin, stout);

    show_u32(d, "clock algorithm", 'clok', kAudioObjectPropertyScopeGlobal);
    show_u32(d, "clock is stable",  'cstb', kAudioObjectPropertyScopeGlobal);
    show_u32(d, "running somewhere", kAudioDevicePropertyDeviceIsRunningSomewhere,
             kAudioObjectPropertyScopeGlobal);

    // Apple's own round-trip composition (coreaudio-api, William Stewart 2005):
    //   2 x IO size + in/out device latency + in/out safety offset
    UInt32 io = 0;
    getprop(d, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
            &io, sizeof io);
    if (sr > 0) {
        UInt32 tot = 2 * io + lin + lout + sin + sout + stin + stout;
        printf("  %-28s %u frames = %.2f ms\n", "PREDICTED round-trip",
               tot, tot * 1000.0 / sr);
        printf("      = 2*%u io + %u+%u dev lat + %u+%u safety + %u+%u stream lat\n",
               io, lin, lout, sin, sout, stin, stout);
    }
    if (nm)  CFRelease(nm);
    if (uid) CFRelease(uid);
}

static int run_io_bg(AudioObjectID d, AudioDeviceIOProcID *out);
static void stop_io_bg(AudioObjectID d, AudioDeviceIOProcID id);

// -------------------------------------------------------------- clock watch
static void watch_clock(AudioObjectID d, int seconds) {
    Float64 sr = 48000;
    getprop(d, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
            &sr, sizeof sr);
    printf("\n--- clock watch, %ds ---\n", seconds);
    // AudioDeviceGetCurrentTime only works while THIS client has the device
    // running -- another process holding it open is not enough. Start our own
    // silent IOProc for the duration.
    AudioDeviceIOProcID own = NULL;
    int started = (run_io_bg(d, &own) == 0);
    if (!started) printf("  (could not start our own IOProc; times may be unavailable)\n");
    usleep(200000);

    static double st[MAXOBS], ht[MAXOBS];
    int n = 0, jumps = 0;
    double prev_s = -1;
    for (int i = 0; i < seconds * 50 && n < MAXOBS; i++) {
        AudioTimeStamp ts; memset(&ts, 0, sizeof ts);
        ts.mFlags = kAudioTimeStampSampleHostTimeValid;
        if (AudioDeviceGetCurrentTime(d, &ts) == noErr &&
            (ts.mFlags & kAudioTimeStampSampleTimeValid)) {
            st[n] = ts.mSampleTime;
            ht[n] = (double)ts.mHostTime * g_h2s;
            if (prev_s >= 0 && st[n] < prev_s) jumps++;
            prev_s = st[n];
            n++;
        }
        usleep(20000);
    }
    if (started) stop_io_bg(d, own);
    if (n < 10) {
        printf("  only %d usable samples -- device may not permit client IO\n", n);
        return;
    }
    // least squares: sampleTime = a + b*hostSeconds ; b should equal sr
    double sx=0, sy=0, sxx=0, sxy=0;
    for (int i = 0; i < n; i++) { sx+=ht[i]; sy+=st[i]; sxx+=ht[i]*ht[i]; sxy+=ht[i]*st[i]; }
    double b = (n*sxy - sx*sy) / (n*sxx - sx*sx);
    double a = (sy - b*sx) / n;
    double worst = 0;
    for (int i = 0; i < n; i++) {
        double resid = fabs(st[i] - (a + b*ht[i]));
        if (resid > worst) worst = resid;
    }
    printf("  samples              %d over %.1fs\n", n, ht[n-1]-ht[0]);
    printf("  fitted rate          %.3f Hz  (nominal %.1f)\n", b, sr);
    printf("  clock error          %+.1f ppm\n", (b - sr) / sr * 1e6);
    printf("  worst residual       %.2f frames %s\n", worst,
           worst > 2.0 ? "  <-- exceeds the ~1 sample target" : "");
    printf("  non-monotonic jumps  %d %s\n", jumps, jumps ? " <-- timeline discontinuity" : "");
}

// ---------------------------------------------------------------- IO probe
typedef struct { UInt32 counts[64]; UInt32 nseen; UInt32 last; UInt32 cycles; } iostats;
static iostats g_io;

static OSStatus ioproc(AudioObjectID d, const AudioTimeStamp *now,
                       const AudioBufferList *in, const AudioTimeStamp *it,
                       AudioBufferList *out, const AudioTimeStamp *ot, void *ud) {
    (void)d;(void)now;(void)it;(void)ot;(void)ud;
    UInt32 frames = 0;
    const AudioBufferList *bl = (in && in->mNumberBuffers) ? in : out;
    if (bl && bl->mNumberBuffers && bl->mBuffers[0].mNumberChannels)
        frames = bl->mBuffers[0].mDataByteSize /
                 (sizeof(Float32) * bl->mBuffers[0].mNumberChannels);
    g_io.cycles++;
    g_io.last = frames;
    for (UInt32 i = 0; i < g_io.nseen; i++)
        if (g_io.counts[i] == frames) return noErr;
    if (g_io.nseen < 64) g_io.counts[g_io.nseen++] = frames;
    return noErr;
}

static int run_io(AudioObjectID d, int seconds, int quiet) {
    memset(&g_io, 0, sizeof g_io);
    AudioDeviceIOProcID id = NULL;
    if (AudioDeviceCreateIOProcID(d, ioproc, NULL, &id) != noErr) {
        if (!quiet) printf("  cannot create IOProc\n"); return -1;
    }
    if (AudioDeviceStart(d, id) != noErr) {
        if (!quiet) printf("  cannot start device\n");
        AudioDeviceDestroyIOProcID(d, id); return -1;
    }
    sleep(seconds);
    AudioDeviceStop(d, id);
    AudioDeviceDestroyIOProcID(d, id);
    return 0;
}

static int run_io_bg(AudioObjectID d, AudioDeviceIOProcID *out) {
    memset(&g_io, 0, sizeof g_io);
    if (AudioDeviceCreateIOProcID(d, ioproc, NULL, out) != noErr) return -1;
    if (AudioDeviceStart(d, *out) != noErr) {
        AudioDeviceDestroyIOProcID(d, *out); *out = NULL; return -1;
    }
    return 0;
}
static void stop_io_bg(AudioObjectID d, AudioDeviceIOProcID id) {
    if (!id) return;
    AudioDeviceStop(d, id);
    AudioDeviceDestroyIOProcID(d, id);
}

static void io_probe(AudioObjectID d, int seconds) {
    printf("\n--- IOProc probe, %ds ---\n", seconds);
    if (run_io(d, seconds, 0) != 0) return;
    printf("  cycles               %u\n", g_io.cycles);
    printf("  distinct frameCounts %u:", g_io.nseen);
    for (UInt32 i = 0; i < g_io.nseen; i++) printf(" %u", g_io.counts[i]);
    printf("\n");
    if (g_io.nseen > 1)
        printf("  --> spans VARY. Any code assuming a fixed callback size is wrong.\n");
}

// ------------------------------------------------------------------- sweep
static void sweep(AudioObjectID d) {
    UInt32 sizes[] = {32,64,128,256,512,1024,2048,4096};
    UInt32 saved = 0;
    getprop(d, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
            &saved, sizeof saved);
    printf("\n--- buffer size sweep (requested -> read back -> actual spans) ---\n");
    printf("  %-10s %-12s %s\n", "requested", "read back", "observed frameCounts");
    for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain };
        OSStatus e = AudioObjectSetPropertyData(d, &a, 0, NULL, sizeof(UInt32), &sizes[i]);
        UInt32 back = 0;
        getprop(d, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                &back, sizeof back);
        char obs[256] = "(io failed)";
        if (run_io(d, 2, 1) == 0) {
            obs[0] = 0;
            for (UInt32 k = 0; k < g_io.nseen; k++) {
                char t[16]; snprintf(t, sizeof t, "%u ", g_io.counts[k]);
                strncat(obs, t, sizeof obs - strlen(obs) - 1);
            }
        }
        printf("  %-10u %-12u %s%s\n", sizes[i], back, obs,
               (e != noErr) ? "  (set rejected)" : (back != sizes[i] ? "  <-- COERCED" : ""));
    }
    AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(d, &a, 0, NULL, sizeof(UInt32), &saved);
    printf("  restored to %u\n", saved);
}

// -------------------------------------------------------------------- main
int main(int argc, char **argv) {
    init_hostclock();
    const char *filter = NULL;
    int clocks = 0, ios = 0, dosweep = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-d") && i+1 < argc)          filter = argv[++i];
        else if (!strcmp(argv[i], "--clock") && i+1 < argc) clocks = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--io") && i+1 < argc)    ios = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--sweep"))               dosweep = 1;
        else if (argv[i][0] != '-')                         filter = argv[i];
    }

    AudioObjectPropertyAddress da = { kAudioHardwarePropertyDevices,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &da, 0, NULL, &sz) != noErr)
        { fprintf(stderr, "cannot enumerate devices\n"); return 1; }
    AudioObjectID *devs = malloc(sz);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, NULL, &sz, devs);
    UInt32 n = sz / sizeof(AudioObjectID);

    for (UInt32 i = 0; i < n; i++) {
        CFStringRef nm = dev_name(devs[i]);
        if (filter) {
            char b[256] = {0};
            if (nm) CFStringGetCString(nm, b, sizeof b, kCFStringEncodingUTF8);
            if (!strstr(b, filter)) { if (nm) CFRelease(nm); continue; }
        }
        if (nm) CFRelease(nm);
        snapshot(devs[i]);
        if (clocks)  watch_clock(devs[i], clocks);
        if (ios)     io_probe(devs[i], ios);
        if (dosweep) sweep(devs[i]);
    }
    free(devs);
    return 0;
}
