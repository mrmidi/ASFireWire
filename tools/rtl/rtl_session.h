// rtl_session.h -- the one CoreAudio IO implementation behind the loopback
// measurements. Both front ends call it: the command line (rtl_loopback.c) and
// the ASFW app (Swift, through the bridging header).
//
// Two kinds of session share one lifecycle:
//   impulse  N trials through rtl_engine_step() -> RTL_raw / RTL_ts / residual
//   tone     a continuous sine, captured, then analysed by tone_analyse()
//
// THREADING AND REAL-TIME CONTRACT
//
//   create    non-real-time. Allocates EVERYTHING the IOProc will touch (trial
//             store or capture + callback records), reads the host clock scale,
//             initialises the engine, and touches every page of those buffers
//             so the IOProc never takes a first-write page fault. Nothing is
//             allocated after this returns.
//   run       blocking, called from a non-real-time thread. Registers the
//             overload listener, creates and starts the IOProc, then sleeps in
//             50 ms steps until the IOProc reports done (atomic), the caller
//             cancels (atomic) or the timeout expires. Then AudioDeviceStop(),
//             which does not return until the IOProc has finished, destroys the
//             IOProc and removes the listener.
//   IOProc    real-time thread. Allocates nothing, takes no lock, logs nothing,
//             calls no Swift/ObjC, touches no CF object. It reads and writes
//             only memory created by create(), plus C11 atomics for completion,
//             progress and the overload count.
//   cancel / progress / overloads
//             may be called from any thread while run() is blocking.
//   results   every other accessor is valid only AFTER run() has returned, on
//             the thread that owns the session: AudioDeviceStop() has by then
//             quiesced the IOProc, so the engine and the capture are the
//             caller's.
//
// A session runs once. Destroy it after reading the results.
#ifndef ASFW_TOOLS_RTL_SESSION_H
#define ASFW_TOOLS_RTL_SESSION_H

#include <stddef.h>
#include <stdint.h>

#include "rtl_core.h"
#include "tone_core.h"
#include "tone_io.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------ device queries
// Non-real-time helpers over one CoreAudio device. `dev` is an AudioObjectID.

#define RTL_DEV_STRING_MAX 256

// UTF-8 name / UID. Return 1 when read; an unreadable property yields an empty string and 0.
int rtl_dev_name(uint32_t dev, char *buf, size_t cap);
int rtl_dev_uid(uint32_t dev, char *buf, size_t cap);

// Channels in one direction (input != 0 for the input scope), counted over the
// stream configuration -- the same indexing the IOProc uses for --out-ch/--in-ch.
uint32_t rtl_dev_channels(uint32_t dev, int input);

// The nominal sample rate. Returns 0 (and leaves *sr untouched) when it cannot
// be read or is not positive: there is no silent fallback rate.
int rtl_dev_sample_rate(uint32_t dev, double *sr);

// Nonzero when some client is already running this device.
int rtl_dev_running_somewhere(uint32_t dev);

// The declared geometry. Properties that would not read are listed in
// out->missing (static strings), never silently zero.
void rtl_dev_read_declared(uint32_t dev, rtl_declared_t *out);

// Request a client buffer size and report what the device adopted in *got.
// Returns 0 when the request was refused or silently altered.
int rtl_dev_request_buffer_frames(uint32_t dev, uint32_t want, uint32_t *got);

// ----------------------------------------------------------------- sessions
typedef struct rtl_session rtl_session_t;

typedef enum {
    RTL_SESSION_OK = 0,
    RTL_SESSION_BAD_ARGUMENT,
    RTL_SESSION_NO_MEMORY,
    RTL_SESSION_CANNOT_CREATE_IOPROC,
    RTL_SESSION_CANNOT_START
} rtl_session_status_t;

const char *rtl_session_status_text(rtl_session_status_t st);

typedef enum {
    RTL_OUTCOME_NOT_RUN = 0,
    RTL_OUTCOME_COMPLETED,
    RTL_OUTCOME_TIMED_OUT,   // the IOProc never finished: IO may have stalled
    RTL_OUTCOME_CANCELLED
} rtl_session_outcome_t;

typedef struct {
    uint32_t device;                 // AudioObjectID
    double   sampleRate;             // nominal rate the caller read; must be > 0
    uint32_t outChannel, inChannel;  // zero-based channel indices
    uint32_t trials;                 // 0..RTL_MAX_TRIALS
    uint32_t window;                 // 0..RTL_MAX_WINDOW capture frames per trial
    float    amplitude;
} rtl_impulse_config_t;

typedef struct {
    uint32_t device;
    double   sampleRate;
    uint32_t outChannel, inChannel;
    double   durationSec;            // 1..600
    double   freqHz;                 // 0 < f < sampleRate / 2
    float    amplitude;
} rtl_tone_config_t;

// Seconds the impulse run is expected to take at this rate: warm-up, then per
// trial the gap and the capture window. The run's timeout is a multiple of it.
double rtl_impulse_expected_seconds(uint32_t trials, uint32_t window, double sampleRate);

// The self-test at 44.1, 48 and 96 kHz (the rates the driver streams), logging
// to `log` (may be NULL). Returns the total failure count; 0 means it passed.
int rtl_selftest_all(FILE *log);

// Both return NULL on failure and set *status (which may be NULL).
rtl_session_t *rtl_session_create_impulse(const rtl_impulse_config_t *cfg,
                                          rtl_session_status_t *status);
rtl_session_t *rtl_session_create_tone(const rtl_tone_config_t *cfg,
                                       rtl_session_status_t *status);
void rtl_session_destroy(rtl_session_t *s);

// Blocking. Returns OK when IO ran and stopped (see rtl_session_outcome for how
// it ended). A cancel requested before run() starts no IO and returns OK with
// outcome CANCELLED.
rtl_session_status_t rtl_session_run(rtl_session_t *s);

// Any thread. Idempotent.
void rtl_session_cancel(rtl_session_t *s);

// Any thread, while run() is blocking: progress through a monotonic counter.
// Impulse: trials captured of trials. Tone: frames captured of frames wanted.
void rtl_session_progress(const rtl_session_t *s, uint64_t *done, uint64_t *total);

// Processor overloads reported by the HAL so far. Any thread.
uint32_t rtl_session_overloads(const rtl_session_t *s);

// ----- results: after run() has returned -----
rtl_session_outcome_t rtl_session_outcome(const rtl_session_t *s);
uint32_t rtl_session_timeout_seconds(const rtl_session_t *s);

// Impulse. The engine is valid until destroy; feed it to rtl_summarize() and
// rtl_write_json().
const rtl_engine_t *rtl_session_engine(const rtl_session_t *s);
uint32_t rtl_session_input_channels(const rtl_session_t *s);
// Peak |sample| seen on an input channel over the run (0 for an unseen channel).
float rtl_session_channel_peak(const rtl_session_t *s, uint32_t ch);

// Tone. The capture is mono float32 at the session's sample rate.
const float *rtl_session_tone_capture(const rtl_session_t *s, uint64_t *frames);
const tone_cb_t *rtl_session_tone_callbacks(const rtl_session_t *s, uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif // ASFW_TOOLS_RTL_SESSION_H
