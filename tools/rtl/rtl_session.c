// rtl_session.c -- the CoreAudio IO shared by rtl_loopback and the ASFW app.
// See rtl_session.h for the threading and real-time contract.
//
// Moved out of rtl_loopback.c; the device queries, the IOProcs and the run loop
// are the code the command line has always run. Differences are structural
// only: state lives in a heap session instead of file-scope globals, so the
// caller can own, cancel and inspect it.
//
// Every decision about what a number means stays in rtl_core / tone_core.
#include "rtl_session.h"

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_CHANNELS 64

typedef enum { KIND_IMPULSE, KIND_TONE } session_kind_t;

struct rtl_session {
    session_kind_t kind;
    uint32_t       device;
    double         sampleRate;
    double         hostToSec;      // mach host ticks -> seconds
    uint32_t       outCh, inCh;

    // Shared with the IOProc and the listener: the only members touched by more
    // than one thread while IO runs.
    _Atomic int      done;         // IOProc -> run(): the work is complete
    _Atomic int      cancel;       // any thread -> run()
    _Atomic uint32_t overloads;    // listener -> IOProc and callers
    _Atomic uint64_t progress;     // IOProc -> any thread

    uint64_t       progressTotal;  // fixed at create
    uint32_t       timeoutSec;     // fixed at create
    int            ran;
    rtl_session_outcome_t outcome;

    // impulse
    rtl_engine_t   engine;
    rtl_trial_t   *trials;         // `engine.trials` items, owned
    uint32_t       inChans;
    float          chPeak[MAX_CHANNELS];

    // tone
    tone_gen_t     gen;
    float         *cap;            // capCap floats, owned
    uint64_t       capCap, capLen;
    tone_cb_t     *cb;             // cbCap records, owned
    uint32_t       cbCap, cbLen;
};

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

static int dev_string(AudioObjectID d, AudioObjectPropertySelector sel, char *buf, size_t cap) {
    CFStringRef s = NULL;
    if (!buf || !cap) return 0;
    buf[0] = '\0';
    if (getprop(d, sel, kAudioObjectPropertyScopeGlobal, &s, sizeof s) && s) {
        cfstring_to_c(s, buf, cap);
        CFRelease(s);
        return 1;
    }
    return 0;
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

static void zero_output(AudioBufferList *out) {
    if (!out) return;
    for (UInt32 i = 0; i < out->mNumberBuffers; i++)
        if (out->mBuffers[i].mData)
            memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);
}

// ------------------------------------------------------------ device queries
int rtl_dev_name(uint32_t dev, char *buf, size_t cap) {
    return dev_string(dev, kAudioObjectPropertyName, buf, cap);
}

int rtl_dev_uid(uint32_t dev, char *buf, size_t cap) {
    return dev_string(dev, kAudioDevicePropertyDeviceUID, buf, cap);
}

uint32_t rtl_dev_channels(uint32_t dev, int input) {
    AudioObjectPropertyAddress a = {
        kAudioDevicePropertyStreamConfiguration,
        input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput,
        kAudioObjectPropertyElementMain };
    UInt32 sz = 0, n = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &sz) != noErr || !sz) return 0;
    AudioBufferList *bl = malloc(sz);
    if (!bl) return 0;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &sz, bl) == noErr)
        for (UInt32 i = 0; i < bl->mNumberBuffers; i++)
            n += bl->mBuffers[i].mNumberChannels;
    free(bl);
    return n;
}

int rtl_dev_sample_rate(uint32_t dev, double *sr) {
    Float64 v = 0.0;
    if (!getprop(dev, kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
                 &v, sizeof v) || !(v > 0.0))
        return 0;
    if (sr) *sr = v;
    return 1;
}

int rtl_dev_running_somewhere(uint32_t dev) {
    UInt32 v = 0;
    return getprop(dev, kAudioDevicePropertyDeviceIsRunningSomewhere,
                   kAudioObjectPropertyScopeGlobal, &v, sizeof v) && v;
}

void rtl_dev_read_declared(uint32_t d, rtl_declared_t *out) {
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
    *out = c;
}

int rtl_dev_request_buffer_frames(uint32_t d, uint32_t want, uint32_t *outGot) {
    AudioObjectPropertyAddress a = { kAudioDevicePropertyBufferFrameSize,
                                     kAudioObjectPropertyScopeGlobal,
                                     kAudioObjectPropertyElementMain };
    UInt32 v = want;
    const OSStatus st = AudioObjectSetPropertyData(d, &a, 0, NULL, sizeof v, &v);
    UInt32 got = 0;
    if (!getprop(d, kAudioDevicePropertyBufferFrameSize,
                 kAudioObjectPropertyScopeGlobal, &got, sizeof got)) {
        if (outGot) *outGot = 0;
        return 0;
    }
    if (outGot) *outGot = got;
    return st == noErr && got == want;
}

// ------------------------------------------------------------------- status
const char *rtl_session_status_text(rtl_session_status_t st) {
    switch (st) {
    case RTL_SESSION_OK:                    return "ok";
    case RTL_SESSION_BAD_ARGUMENT:          return "bad argument";
    case RTL_SESSION_NO_MEMORY:             return "cannot allocate the session";
    case RTL_SESSION_CANNOT_CREATE_IOPROC:  return "cannot create IOProc";
    case RTL_SESSION_CANNOT_START:          return "cannot start device";
    }
    return "unknown";
}

// ----------------------------------------------------------- IO (any thread)
// HAL notification thread. Touches only the atomic counter in the session.
static OSStatus overload_listener(AudioObjectID o, UInt32 n,
                                  const AudioObjectPropertyAddress *a, void *u) {
    (void)o; (void)n; (void)a;
    rtl_session_t *s = (rtl_session_t *)u;
    atomic_fetch_add_explicit(&s->overloads, 1u, memory_order_relaxed);
    return noErr;
}

// ----------------------------------------------------------- IO (real-time)
// Everything from here to rtl_session_create_* runs on the HAL's real-time
// thread: no allocation, no locks, no logging, no CF, no Swift. It reads and
// writes only memory the session owns, plus the atomics above.

// Read by the engine at trial boundaries, on the IO thread.
static uint32_t read_overloads(void *ctx) {
    const rtl_session_t *s = (const rtl_session_t *)ctx;
    return atomic_load_explicit(&s->overloads, memory_order_relaxed);
}

typedef struct { const AudioBufferList *in; UInt32 ch; } ioctx_t;

static float io_input(void *ctx, uint32_t frame) {
    const ioctx_t *c = (const ioctx_t *)ctx;
    const Float32 *p = bl_slot(c->in, frame, c->ch);
    return p ? *p : 0.0f;
}

static OSStatus impulse_ioproc(AudioObjectID dev, const AudioTimeStamp *now,
                               const AudioBufferList *in, const AudioTimeStamp *it,
                               AudioBufferList *out, const AudioTimeStamp *ot,
                               void *ud) {
    (void)dev; (void)now;
    rtl_session_t *s = (rtl_session_t *)ud;

    const UInt32 n = bl_frames(out) ? bl_frames(out) : bl_frames(in);
    if (!n) return noErr;

    zero_output(out);

    if (in) {
        const UInt32 nc = bl_channels(in);
        if (nc && nc <= MAX_CHANNELS) {
            s->inChans = nc;
            for (UInt32 c = 0; c < nc; c++)
                for (UInt32 f = 0; f < n; f++) {
                    const Float32 *p = bl_slot(in, f, c);
                    if (!p) break;
                    const Float32 a = fabsf(*p);
                    if (a > s->chPeak[c]) s->chPeak[c] = a;
                }
        }
    }

    const int itOk = it && (it->mFlags & kAudioTimeStampSampleTimeValid);
    const int otOk = ot && (ot->mFlags & kAudioTimeStampSampleTimeValid);
    ioctx_t ctx = { in, s->inCh };
    // The wall clock is read here, directly, rather than taken from any
    // driver-supplied timestamp: a re-anchored sample timeline must not be able
    // to hide a gap in delivered frames.
    rtl_step_t st = {
        .n = n,
        .hostSec  = (double)mach_absolute_time() * s->hostToSec,
        .itSample = itOk ? it->mSampleTime : 0.0, .itValid = itOk,
        .otSample = otOk ? ot->mSampleTime : 0.0, .otValid = otOk,
        .input = io_input, .ctx = &ctx,
        .outSlot = bl_slot(out, 0, s->outCh),
    };
    const int finished = rtl_engine_step(&s->engine, &st);
    atomic_store_explicit(&s->progress, s->engine.trial, memory_order_relaxed);
    if (finished)
        atomic_store_explicit(&s->done, 1, memory_order_release);
    return noErr;
}

// Plays a continuous sine on one output and records the looped-back input. Like
// the impulse mode, it only generates, copies and records; all analysis runs
// after AudioDeviceStop().
static OSStatus tone_ioproc(AudioObjectID dev, const AudioTimeStamp *now,
                            const AudioBufferList *in, const AudioTimeStamp *it,
                            AudioBufferList *out, const AudioTimeStamp *ot,
                            void *ud) {
    (void)dev; (void)now; (void)ot;
    rtl_session_t *s = (rtl_session_t *)ud;

    const UInt32 nOut = bl_frames(out);
    zero_output(out);
    // The generator advances once per output frame whether or not the channel
    // exists, so the tone stays phase-continuous in output sample time.
    for (UInt32 f = 0; f < nOut; f++) {
        const float v = tone_gen_next(&s->gen);
        Float32 *p = bl_slot(out, f, s->outCh);
        if (p) *p = v;
    }

    const UInt32 nIn = bl_frames(in);
    if (!nIn || atomic_load_explicit(&s->done, memory_order_relaxed)) return noErr;
    if (s->cbLen < s->cbCap) {
        const int itOk = it && (it->mFlags & kAudioTimeStampSampleTimeValid);
        s->cb[s->cbLen++] = (tone_cb_t){
            .hostSec = (double)mach_absolute_time() * s->hostToSec,
            .itSample = itOk ? it->mSampleTime : 0.0,
            .captureStart = s->capLen,
            .n = nIn,
            .overloads = atomic_load_explicit(&s->overloads, memory_order_relaxed),
            .itValid = (uint8_t)itOk,
        };
    }
    for (UInt32 f = 0; f < nIn && s->capLen < s->capCap; f++) {
        const Float32 *p = bl_slot(in, f, s->inCh);
        s->cap[s->capLen++] = p ? *p : 0.0f;
    }
    atomic_store_explicit(&s->progress, s->capLen, memory_order_relaxed);
    if (s->capLen >= s->capCap || s->cbLen >= s->cbCap)
        atomic_store_explicit(&s->done, 1, memory_order_release);
    return noErr;
}

// ------------------------------------------------------- create (non-RT)
static double host_ticks_to_sec(void) {
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    return (double)tb.numer / (double)tb.denom * 1e-9;
}

// Writes one byte per page so the kernel backs the whole buffer now. A fresh
// calloc/malloc of this size is mapped lazily: without this, the IOProc's first
// write to each page would take a page fault on the real-time thread. The
// volatile store keeps the compiler from folding it into the allocation.
static void prefault(void *p, size_t bytes) {
    volatile unsigned char *b = (volatile unsigned char *)p;
    const size_t page = (size_t)getpagesize();
    for (size_t i = 0; i < bytes; i += page) b[i] = 0;
}

static rtl_session_t *session_alloc(session_kind_t kind, uint32_t device, double sr,
                                    uint32_t outCh, uint32_t inCh) {
    rtl_session_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->kind = kind;
    s->device = device;
    s->sampleRate = sr;
    s->hostToSec = host_ticks_to_sec();
    s->outCh = outCh;
    s->inCh = inCh;
    atomic_init(&s->done, 0);
    atomic_init(&s->cancel, 0);
    atomic_init(&s->overloads, 0u);
    atomic_init(&s->progress, 0u);
    return s;
}

void rtl_session_destroy(rtl_session_t *s);

double rtl_impulse_expected_seconds(uint32_t trials, uint32_t window, double sampleRate) {
    if (!(sampleRate > 0.0)) return 0.0;
    const uint32_t warmup = (uint32_t)(sampleRate * 0.5);
    const uint32_t gap = (uint32_t)(sampleRate * 0.25);
    return (warmup + (double)trials * (gap + window)) / sampleRate;
}

int rtl_selftest_all(FILE *log) {
    int failures = 0;
    failures += rtl_selftest(44100.0, log);
    failures += rtl_selftest(48000.0, log);
    failures += rtl_selftest(96000.0, log);
    return failures;
}

static rtl_session_t *fail(rtl_session_status_t *status, rtl_session_status_t st) {
    if (status) *status = st;
    return NULL;
}

rtl_session_t *rtl_session_create_impulse(const rtl_impulse_config_t *cfg,
                                          rtl_session_status_t *status) {
    if (!cfg || !(cfg->sampleRate > 0.0) || cfg->trials > RTL_MAX_TRIALS ||
        cfg->window > RTL_MAX_WINDOW)
        return fail(status, RTL_SESSION_BAD_ARGUMENT);

    rtl_session_t *s = session_alloc(KIND_IMPULSE, cfg->device, cfg->sampleRate,
                                     cfg->outChannel, cfg->inChannel);
    if (!s) return fail(status, RTL_SESSION_NO_MEMORY);
    // Zero trials is legal (the engine finishes at once); keep the store non-NULL.
    const size_t nTrials = cfg->trials ? cfg->trials : 1;
    s->trials = calloc(nTrials, sizeof *s->trials);
    if (!s->trials) {
        rtl_session_destroy(s);
        return fail(status, RTL_SESSION_NO_MEMORY);
    }
    prefault(s->trials, nTrials * sizeof *s->trials);

    const double sr = cfg->sampleRate;
    rtl_engine_init(&s->engine, s->trials, cfg->trials, cfg->window,
                    (uint32_t)(sr * 0.25), (uint32_t)(sr * 0.5), cfg->amplitude, sr);
    s->engine.overloads = read_overloads;
    s->engine.overloadsCtx = s;

    s->progressTotal = cfg->trials;
    const double expected = rtl_impulse_expected_seconds(cfg->trials, cfg->window, sr);
    s->timeoutSec = (uint32_t)((int)(expected * 4.0) + 5);
    if (status) *status = RTL_SESSION_OK;
    return s;
}

rtl_session_t *rtl_session_create_tone(const rtl_tone_config_t *cfg,
                                       rtl_session_status_t *status) {
    if (!cfg || !(cfg->sampleRate > 0.0) ||
        !(cfg->durationSec >= 1.0 && cfg->durationSec <= 600.0) ||
        !(cfg->freqHz > 0.0 && cfg->freqHz < cfg->sampleRate / 2.0))
        return fail(status, RTL_SESSION_BAD_ARGUMENT);

    rtl_session_t *s = session_alloc(KIND_TONE, cfg->device, cfg->sampleRate,
                                     cfg->outChannel, cfg->inChannel);
    if (!s) return fail(status, RTL_SESSION_NO_MEMORY);
    s->capCap = (uint64_t)(cfg->durationSec * cfg->sampleRate);
    s->cbCap = (uint32_t)(s->capCap / 8 + 1024);  // HAL buffers are >= 15 frames
    s->cap = malloc((size_t)s->capCap * sizeof *s->cap);
    s->cb = malloc((size_t)s->cbCap * sizeof *s->cb);
    if (!s->cap || !s->cb) {
        rtl_session_destroy(s);
        return fail(status, RTL_SESSION_NO_MEMORY);
    }
    prefault(s->cap, (size_t)s->capCap * sizeof *s->cap);
    prefault(s->cb, (size_t)s->cbCap * sizeof *s->cb);
    tone_gen_init(&s->gen, cfg->sampleRate, cfg->freqHz, cfg->amplitude);
    s->progressTotal = s->capCap;
    s->timeoutSec = (uint32_t)((int)(cfg->durationSec * 2.0) + 5);
    if (status) *status = RTL_SESSION_OK;
    return s;
}

void rtl_session_destroy(rtl_session_t *s) {
    if (!s) return;
    free(s->trials);
    free(s->cap);
    free(s->cb);
    free(s);
}

// ---------------------------------------------------------------------- run
rtl_session_status_t rtl_session_run(rtl_session_t *s) {
    if (!s || s->ran) return RTL_SESSION_BAD_ARGUMENT;
    s->ran = 1;
    if (atomic_load_explicit(&s->cancel, memory_order_acquire)) {
        s->outcome = RTL_OUTCOME_CANCELLED;
        return RTL_SESSION_OK;
    }

    const AudioObjectID dev = s->device;
    AudioObjectPropertyAddress ola = { kAudioDeviceProcessorOverload,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(dev, &ola, overload_listener, s);

    AudioDeviceIOProcID id = NULL;
    const AudioDeviceIOProc proc = s->kind == KIND_IMPULSE ? impulse_ioproc : tone_ioproc;
    if (AudioDeviceCreateIOProcID(dev, proc, s, &id) != noErr) {
        AudioObjectRemovePropertyListener(dev, &ola, overload_listener, s);
        return RTL_SESSION_CANNOT_CREATE_IOPROC;
    }
    if (AudioDeviceStart(dev, id) != noErr) {
        AudioDeviceDestroyIOProcID(dev, id);
        AudioObjectRemovePropertyListener(dev, &ola, overload_listener, s);
        return RTL_SESSION_CANNOT_START;
    }

    // This thread only waits; the IOProc does the work.
    for (uint32_t i = 0; i < s->timeoutSec * 20u &&
                         !atomic_load_explicit(&s->done, memory_order_acquire) &&
                         !atomic_load_explicit(&s->cancel, memory_order_acquire); i++)
        usleep(50000);
    const int completed = atomic_load_explicit(&s->done, memory_order_acquire);
    const int cancelled = atomic_load_explicit(&s->cancel, memory_order_acquire);
    AudioDeviceStop(dev, id);
    AudioDeviceDestroyIOProcID(dev, id);
    AudioObjectRemovePropertyListener(dev, &ola, overload_listener, s);
    // AudioDeviceStop has quiesced the IOProc: the session is this thread's now.

    s->outcome = completed ? RTL_OUTCOME_COMPLETED
               : cancelled ? RTL_OUTCOME_CANCELLED
                           : RTL_OUTCOME_TIMED_OUT;
    return RTL_SESSION_OK;
}

void rtl_session_cancel(rtl_session_t *s) {
    if (s) atomic_store_explicit(&s->cancel, 1, memory_order_release);
}

void rtl_session_progress(const rtl_session_t *s, uint64_t *done, uint64_t *total) {
    if (!s) return;
    if (done) *done = atomic_load_explicit(&s->progress, memory_order_relaxed);
    if (total) *total = s->progressTotal;
}

uint32_t rtl_session_overloads(const rtl_session_t *s) {
    return s ? atomic_load_explicit(&s->overloads, memory_order_relaxed) : 0;
}

// ------------------------------------------------------------------ results
rtl_session_outcome_t rtl_session_outcome(const rtl_session_t *s) {
    return s ? s->outcome : RTL_OUTCOME_NOT_RUN;
}

uint32_t rtl_session_timeout_seconds(const rtl_session_t *s) { return s ? s->timeoutSec : 0; }

const rtl_engine_t *rtl_session_engine(const rtl_session_t *s) {
    return s && s->kind == KIND_IMPULSE ? &s->engine : NULL;
}

uint32_t rtl_session_input_channels(const rtl_session_t *s) { return s ? s->inChans : 0; }

float rtl_session_channel_peak(const rtl_session_t *s, uint32_t ch) {
    return s && ch < MAX_CHANNELS ? s->chPeak[ch] : 0.0f;
}

const float *rtl_session_tone_capture(const rtl_session_t *s, uint64_t *frames) {
    if (!s || s->kind != KIND_TONE) { if (frames) *frames = 0; return NULL; }
    if (frames) *frames = s->capLen;
    return s->cap;
}

const tone_cb_t *rtl_session_tone_callbacks(const rtl_session_t *s, uint32_t *count) {
    if (!s || s->kind != KIND_TONE) { if (count) *count = 0; return NULL; }
    if (count) *count = s->cbLen;
    return s->cb;
}
