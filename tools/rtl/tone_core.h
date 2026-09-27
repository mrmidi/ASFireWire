// tone_core.h -- continuous-tone glitch detection for the loopback tools.
//
// Platform-neutral like rtl_core: no CoreAudio, unit-tested on any host
// (tests/tools/ToneCoreTests.cpp). rtl_loopback --tone plays tone_gen_* on one
// output, captures the looped-back input, and hands the capture to
// tone_analyse() after IO has stopped.
//
// A sine of known frequency is the reference: fitting it block by block leaves
// only what the path did wrong. Each glitch is classified by what it did to
// the signal, which tells two driver failures apart:
//
//   dropout  samples went silent where the tone should be, phase unchanged
//            afterwards: content was replaced in place (e.g. silence
//            substituted for late PCM). Judged per sample, so one 8-frame
//            packet of silence is a dropout, not a click.
//   slip     phase moved afterwards: frames were lost (+) or repeated (-)
//   click    a residual spike with amplitude and phase intact
//
// Off-reference stretches within 1024 frames of each other are one event, and
// its slip is the net shift across all of it: a disturbance that knocks the
// phase away and lets it settle back is one click with a net slip of ~0.
// A slip is measured modulo one tone period (48.1 frames at 997 Hz / 48 kHz).
#ifndef ASFW_TONE_CORE_H
#define ASFW_TONE_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Samples per fitted block. Also the time resolution of an event's extent.
#define TONE_BLOCK 64

typedef struct {
    double phase;  // radians, kept in [0, 2*pi)
    double step;   // radians per frame
    float  amp;
} tone_gen_t;

void  tone_gen_init(tone_gen_t *g, double sampleRate, double freqHz, float amp);
float tone_gen_next(tone_gen_t *g);

typedef enum {
    TONE_EVENT_CLICK = 0,
    TONE_EVENT_DROPOUT,
    TONE_EVENT_SLIP,
    TONE_EVENT_KINDS
} tone_event_kind_t;

const char *tone_event_name(tone_event_kind_t k);

typedef struct {
    tone_event_kind_t kind;
    uint64_t firstSample;   // capture index of the first sample off the reference
    uint64_t lastSample;    // inclusive
    double   slipFrames;    // + frames lost, - frames repeated (mod one period)
    uint32_t offSamples;    // samples off the reference inside the event
    uint32_t silentSamples; // of those, samples that were silent (dropped out)
    float    minAmplitude;  // lowest block amplitude inside, relative to reference
    float    peakResidual;  // largest |residual|, relative to reference amplitude
} tone_event_t;

typedef struct {
    int         valid;
    const char *invalidReason;  // set when !valid
    double      amplitude;      // reference amplitude of the looped-back tone
    double      noiseRms;       // median per-block fit residual
    double      threshold;      // |residual| above this is off the reference
    uint64_t    onsetSample;    // first sample of the first block carrying the tone
    uint64_t    analysedFrom;   // [analysedFrom, analysedTo) capture indices
    uint64_t    analysedTo;
    uint32_t    counts[TONE_EVENT_KINDS];
    uint32_t    eventCount;     // all detected events
    uint32_t    storedEvents;   // how many of them fit in the caller's array
} tone_result_t;

// Analyse n captured samples. events may be NULL when maxEvents is 0.
// Returns 0 when the analysis ran (out->valid says whether it could conclude),
// nonzero on an internal allocation failure.
int tone_analyse(const float *x, uint64_t n, double sampleRate, double freqHz,
                 tone_event_t *events, uint32_t maxEvents, tone_result_t *out);

#ifdef __cplusplus
}
#endif

#endif
