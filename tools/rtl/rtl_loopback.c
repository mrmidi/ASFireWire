// rtl_loopback.c -- uncompensated electrical round-trip latency measurement.
//
// Command-line front end. It selects the device, prints, and writes the
// evidence file; the CoreAudio IO itself lives in rtl_session.{h,c}, shared
// with the ASFW app, and every decision about what a number means -- timeline
// audit, trial admission, detection, aggregation, evidence JSON -- lives in the
// platform-neutral core, which is unit-tested on any host
// (tests/tools/RtlCoreTests.cpp).
//
// Measures the physical round trip with an output->input cable and NO DAW in
// the path, so nothing along the way can silently compensate the number.
//
//   tools/rtl/build.sh                       build (or: cmake -S tools -B build/tools)
//   ./rtl_loopback                           list matching device + declared geometry
//   ./rtl_loopback -d ASFW --measure         run the measurement (starts real IO)
//   ./rtl_loopback -d ASFW --measure --frames 64 --trials 32 --window auto --json out.json
//   ./rtl_loopback --selftest                check the analysis maths, no hardware
//   ./rtl_loopback -d ASFW --tone            play a sine, find every glitch (dropout/slip/click)
//   ./rtl_loopback -d ASFW --tone --frames 32 --duration 60
//
// See tools/rtl/README.md for the meaning of every number.
//
// The IOProc (rtl_session.c) allocates nothing, logs nothing, and touches only
// preallocated storage. All analysis happens after the run.
#include "rtl_core.h"
#include "rtl_session.h"
#include "tone_core.h"
#include "tone_io.h"

#include <CoreAudio/CoreAudio.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

#ifndef RTL_GIT_SHA
#define RTL_GIT_SHA "unknown"
#endif

static void print_declared(uint32_t d, const rtl_declared_t *c, double sr) {
    printf("  %-24s %.1f Hz, %u in / %u out\n", "format", sr,
           rtl_dev_channels(d, 1),
           rtl_dev_channels(d, 0));
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
    const uint32_t rt = rtl_declared_round_trip(c, &valid);
    if (valid)
        printf("  %-24s %u fr  %.3f ms\n", "declared round-trip", rt,
               sr > 0 ? rt * 1000.0 / sr : 0.0);
    else
        printf("  %-24s unavailable -- a declaration could not be read\n", "declared round-trip");
    for (int i = 0; i < c->nmissing && i < RTL_DECLARED_MAX_MISSING; i++)
        printf("  %-24s %s   <-- NOT READ\n", i ? "" : "declarations missing", c->missing[i]);
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
        "                    [--amp F] [--json <path>] [--selftest]\n"
        "       rtl_loopback [-d <name-substring>] --tone [--frames N] [--duration S]\n"
        "                    [--freq HZ] [--amp F] [--out-ch N] [--in-ch N] [--list N]\n");
}

// ---------------------------------------------------------------------- tone
// --tone: play a continuous sine on one output, capture the looped-back input
// for a fixed duration, and find every glitch in it (tone_core). The IO is a
// tone session; the analysis below runs after the session has stopped.
static int run_tone(uint32_t dev, double sr, double durationSec, double freqHz,
                    float amplitude, uint32_t outCh, uint32_t inCh, uint32_t maxListed) {
    // Another client already running the device sets its IO cycle: the HAL
    // runs the device at the smallest buffer any client asked for, while this
    // tool's callbacks keep its own size. Say so, or the run gets labelled
    // with the wrong buffer size.
    if (rtl_dev_running_somewhere(dev))
        printf("\nNOTE: another client is already running this device. Its IO buffer, not\n"
               "      ours, may set the device's IO cycle; the glitch rate below belongs\n"
               "      to that cycle.\n");

    printf("\ntone: %.1f Hz, amp %.2f, out ch %u -> in ch %u, %.0f s capture\n",
           freqHz, amplitude, outCh, inCh, durationSec);

    const rtl_tone_config_t cfg = {
        .device = dev, .sampleRate = sr, .outChannel = outCh, .inChannel = inCh,
        .durationSec = durationSec, .freqHz = freqHz, .amplitude = amplitude,
    };
    rtl_session_status_t sst;
    rtl_session_t *sess = rtl_session_create_tone(&cfg, &sst);
    if (!sess) {
        fprintf(stderr, "cannot allocate a %.0f s capture\n", durationSec);
        return 1;
    }
    if (rtl_session_run(sess) != RTL_SESSION_OK) {
        fprintf(stderr, "cannot start IO\n");
        rtl_session_destroy(sess);
        return 1;
    }
    // The session has stopped: the capture is this thread's now.
    uint64_t capLen = 0;
    const float *cap = rtl_session_tone_capture(sess, &capLen);
    uint32_t cbLen = 0;
    const tone_cb_t *cb = rtl_session_tone_callbacks(sess, &cbLen);

    if (rtl_session_outcome(sess) != RTL_OUTCOME_COMPLETED)
        printf("\nWARNING: timed out after %u s with %.1f s captured -- IO may have stalled\n",
               rtl_session_timeout_seconds(sess), (double)capLen / sr);

    uint32_t gaps = 0, missing = 0;
    tone_io_summary(cb, cbLen, &gaps, &missing);
    const unsigned overloads = rtl_session_overloads(sess);
    printf("\n--- IO ---\n");
    printf("  %-24s %u", "callbacks", cbLen);
    if (cbLen) printf("  (%.1f frames each on average -- this tool's buffer)",
                      (double)capLen / cbLen);
    printf("\n");
    printf("  %-24s %u\n", "processor overloads", overloads);
    printf("  %-24s %u\n", "input sample-time gaps", gaps);
    printf("  %-24s %u\n", "missing timestamps", missing);

    // Raw capture for offline inspection: float32 little-endian, mono, no header.
    const char *dump = getenv("RTL_TONE_DUMP");
    if (dump) {
        FILE *df = fopen(dump, "wb");
        if (df) {
            fwrite(cap, sizeof *cap, (size_t)capLen, df);
            fclose(df);
            printf("  %-24s %s (%llu float32 frames at %.0f Hz)\n", "raw capture written", dump,
                   (unsigned long long)capLen, sr);
        }
    }

    enum { kStored = 256 };
    static tone_event_t events[kStored];
    tone_result_t r;
    if (tone_analyse(cap, capLen, sr, freqHz, events, kStored, &r) != 0) {
        fprintf(stderr, "analysis ran out of memory\n");
        rtl_session_destroy(sess);
        return 1;
    }
    printf("\n--- tone ---\n");
    if (!r.valid) {
        printf("  no verdict: %s\n", r.invalidReason);
        rtl_session_destroy(sess);
        return 1;
    }
    const double seconds = (double)(r.analysedTo - r.analysedFrom) / sr;
    printf("  %-24s %.3f (%.1f dBFS), noise %.5f rms (%.1f dB SNR)\n", "looped-back tone",
           r.amplitude, 20.0 * log10(r.amplitude), r.noiseRms,
           r.noiseRms > 0 ? 20.0 * log10(r.amplitude / r.noiseRms) : 99.0);
    printf("  %-24s %.1f s (%llu fr), tone arrived at capture frame %llu\n", "analysed",
           seconds, (unsigned long long)(r.analysedTo - r.analysedFrom),
           (unsigned long long)r.onsetSample);
    printf("  %-24s %u dropouts, %u slips, %u clicks  (%.2f per minute)\n", "glitches",
           r.counts[TONE_EVENT_DROPOUT], r.counts[TONE_EVENT_SLIP], r.counts[TONE_EVENT_CLICK],
           seconds > 0 ? r.eventCount * 60.0 / seconds : 0.0);
    if (!r.eventCount) {
        printf("  clean: every analysed frame matches the tone\n");
        rtl_session_destroy(sess);
        return 0;
    }

    const uint32_t listed = r.storedEvents < maxListed ? r.storedEvents : maxListed;
    printf("\n  %-4s %-8s %14s %10s %7s %9s %9s  %s\n", "#", "kind", "input sample", "t (s)",
           "frames", "silent", "slip fr", "IO");
    for (uint32_t i = 0; i < listed; i++) {
        const tone_event_t *e = &events[i];
        const tone_cb_t *c = tone_cb_for(cb, cbLen, e->firstSample);
        const double off = c ? (double)(e->firstSample - c->captureStart) : 0.0;
        const double inSample = (c && c->itValid) ? c->itSample + off : -1.0;
        const double t = (double)e->firstSample / sr;
        char slip[16] = "";
        if (e->kind == TONE_EVENT_SLIP) snprintf(slip, sizeof slip, "%+.2f", e->slipFrames);
        printf("  %-4u %-8s %14.0f %10.3f %7llu %9u %9s  %s\n", i + 1,
               tone_event_name(e->kind), inSample, t,
               (unsigned long long)(e->lastSample - e->firstSample + 1), e->silentSamples, slip,
               c ? tone_io_near(cb, cbLen, c) : "");
    }
    if (r.eventCount > listed)
        printf("  ... %u more not listed\n", r.eventCount - listed);
    printf("\n  \"input sample\" is the HAL input sample time of the event's first frame.\n"
           "  A slip is measured modulo one tone period (%.1f frames).\n", sr / freqHz);
    rtl_session_destroy(sess);
    return 0;
}

// --------------------------------------------------------------------- main
int main(int argc, char **argv) {
    const char *filter = "ASFW";
    const char *jsonPath = NULL;
    int measure = 0, reqFrames = 0, autoWindow = 0, tone = 0, ampSet = 0;
    uint32_t trials = 20, window = 4096, listEvents = 20;
    float amplitude = 0.9f;
    double duration = 30.0, freq = 997.0;
    uint32_t outCh = 0, inCh = 0;

    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "-d")       && i + 1 < argc) filter = argv[++i];
        else if (!strcmp(argv[i], "--measure"))                measure = 1;
        else if (!strcmp(argv[i], "--selftest")) {
            const int f = rtl_selftest_all(stdout);
            printf("\n%s\n", f ? "SELFTEST FAILED" : "selftest passed at 44.1 / 48 / 96 kHz");
            return f ? 1 : 0;
        }
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) reqFrames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trials") && i + 1 < argc) trials = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--window") && i + 1 < argc) {
            const char *w = argv[++i];
            if (!strcmp(w, "auto")) autoWindow = 1; else window = (uint32_t)atoi(w);
        }
        else if (!strcmp(argv[i], "--out-ch") && i + 1 < argc) outCh = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--in-ch")  && i + 1 < argc) inCh  = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--amp")    && i + 1 < argc) { amplitude = (float)atof(argv[++i]); ampSet = 1; }
        else if (!strcmp(argv[i], "--tone"))                     tone = 1;
        else if (!strcmp(argv[i], "--duration") && i + 1 < argc) duration = atof(argv[++i]);
        else if (!strcmp(argv[i], "--freq")   && i + 1 < argc) freq = atof(argv[++i]);
        else if (!strcmp(argv[i], "--list")   && i + 1 < argc) listEvents = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--json")   && i + 1 < argc) jsonPath = argv[++i];
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        else { fprintf(stderr, "unknown argument: %s\n", argv[i]); usage(stderr); return 2; }
    }
    if (tone && measure) {
        fprintf(stderr, "--tone and --measure are separate runs; pick one\n");
        return 2;
    }
    if (tone && !(duration >= 1.0 && duration <= 600.0)) {
        fprintf(stderr, "--duration must be 1..600 seconds\n");
        return 2;
    }
    if (tone && !ampSet) amplitude = 0.5f;  // a continuous sine; the impulse default is too hot
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
    char devName[RTL_DEV_STRING_MAX] = {0}, devUid[RTL_DEV_STRING_MAX] = {0};
    for (UInt32 i = 0; i < ndev; i++) {
        char b[RTL_DEV_STRING_MAX];
        rtl_dev_name(devs[i], b, sizeof b);
        if (!strstr(b, filter)) continue;
        if (!rtl_dev_channels(devs[i], 1) || !rtl_dev_channels(devs[i], 0)) {
            printf("skipping \"%s\": needs both input and output on one device\n", b);
            continue;
        }
        dev = devs[i];
        snprintf(devName, sizeof devName, "%s", b);
        rtl_dev_uid(dev, devUid, sizeof devUid);
        break;
    }
    free(devs);
    if (dev == kAudioObjectUnknown) {
        fprintf(stderr, "no duplex device matching \"%s\"\n", filter);
        return 1;
    }

    uint32_t actualFrames = 0;
    if (reqFrames > 0) {
        if (!rtl_dev_request_buffer_frames(dev, (uint32_t)reqFrames, &actualFrames)) {
            // Buffer size sets the scheduling distance the measurement is
            // reconciled against, so a request that did not take must be
            // visible before any number is.
            fprintf(stderr,
                    "buffer frame size: requested %u, device reports %u -- "
                    "the run below is at %u, not %u\n",
                    (uint32_t)reqFrames, actualFrames, actualFrames, (uint32_t)reqFrames);
        }
    }

    // No silent 48 kHz fallback: every frame<->ms conversion below depends on it.
    double sr = 0.0;
    if (!rtl_dev_sample_rate(dev, &sr)) {
        fprintf(stderr, "cannot read the nominal sample rate of \"%s\"\n", devName);
        return 1;
    }

    rtl_declared_t dc;
    rtl_dev_read_declared(dev, &dc);
    if (!actualFrames) actualFrames = dc.io;
    printf("\n=== %s (device %u) ===\n", devName, dev);
    print_declared(dev, &dc, sr);

    if (autoWindow) {
        window = rtl_auto_window(&dc);
        printf("  %-24s %u fr (4x declared round trip, clamped)\n", "auto window", window);
    }

    if (tone) {
        if (!(freq > 0.0 && freq < sr / 2.0)) {
            fprintf(stderr, "--freq must be between 0 and %.0f Hz\n", sr / 2.0);
            return 2;
        }
        return run_tone(dev, sr, duration, freq, amplitude, outCh, inCh, listEvents);
    }

    if (!measure) {
        printf("\nDeclarations only. Pass --measure to start real IO and measure the\n"
               "physical path; connect an output->input cable first.\n");
        return 0;
    }

    const rtl_impulse_config_t cfg = {
        .device = dev, .sampleRate = sr, .outChannel = outCh, .inChannel = inCh,
        .trials = trials, .window = window, .amplitude = amplitude,
    };
    rtl_session_status_t sst;
    rtl_session_t *sess = rtl_session_create_impulse(&cfg, &sst);
    if (!sess) {
        fprintf(stderr, "%s\n", rtl_session_status_text(sst));
        return 1;
    }

    printf("\nmeasuring: %u trials, window %u fr (max RTL %.1f ms), out ch %u -> in ch %u, amp %.2f\n",
           trials, window, (window - RTL_EDGE_GUARD_FRAMES) * 1000.0 / sr,
           outCh, inCh, amplitude);

    sst = rtl_session_run(sess);
    if (sst != RTL_SESSION_OK) {
        fprintf(stderr, "%s\n", sst == RTL_SESSION_CANNOT_CREATE_IOPROC ? "cannot create IOProc"
                                                                       : "cannot start device");
        rtl_session_destroy(sess);
        return 1;
    }
    // The session has stopped: the engine is this thread's now.
    const rtl_engine_t *e = rtl_session_engine(sess);
    const int completed = rtl_session_outcome(sess) == RTL_OUTCOME_COMPLETED;

    if (!completed)
        printf("\nWARNING: timed out after %u s with %u/%u trials -- IO may have stalled\n",
               rtl_session_timeout_seconds(sess), e->trial, trials);

    const unsigned overloads = rtl_session_overloads(sess);
    printf("\n--- IO ---\n");
    printf("  %-24s %u\n", "callbacks", e->cycles);
    printf("  %-24s", "distinct spans");
    for (uint32_t i = 0; i < e->spanCount; i++) printf(" %u", e->spans[i]);
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
    const uint32_t inChans = rtl_session_input_channels(sess);
    for (uint32_t c = 0; c < inChans && c < 8; c++)
        printf(" %.3f", rtl_session_channel_peak(sess, c));
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
            .bufferRequested = reqFrames > 0 ? (uint32_t)reqFrames : 0u,
            .bufferActual = actualFrames, .window = window, .trialsRequested = trials,
            .overloadsTotal = overloads, .timedOut = !completed,
        };
        FILE *jf = fopen(jsonPath, "w");
        if (!jf || rtl_write_json(jf, &p, &dc, e, reports, &s) != 0) {
            fprintf(stderr, "cannot write %s\n", jsonPath);
            if (jf) fclose(jf);
            rtl_session_destroy(sess);
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
        rtl_session_destroy(sess);
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
    rtl_session_destroy(sess);
    return 0;
}
