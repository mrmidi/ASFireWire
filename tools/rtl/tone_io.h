// tone_io.h -- per-callback IO record for the tone glitch check.
//
// Platform-neutral like tone_core: no CoreAudio, unit-tested on any host
// (tests/tools/ToneIoTests.cpp). The tone IOProc (rtl_session.c) stores one
// tone_cb_t per input callback; after IO stops, these helpers name the callback
// that delivered a capture index and the IO anomaly around it, so a glitch can
// be told apart from something CoreAudio did.
#ifndef ASFW_TONE_IO_H
#define ASFW_TONE_IO_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One IO callback, so a capture index can be named by sample time and host
// time, and so an event can be tied to the IO anomaly around it.
typedef struct {
    double   hostSec;
    double   itSample;       // input sample time of the callback's first frame
    uint64_t captureStart;   // capture index of that frame
    uint32_t n;              // input frames delivered
    uint32_t overloads;      // overload count seen at this callback
    uint8_t  itValid;
} tone_cb_t;

// The callback that delivered capture index i: the last record whose
// captureStart <= i (records are in capture order). NULL when count == 0.
const tone_cb_t *tone_cb_for(const tone_cb_t *cb, uint32_t count, uint64_t i);

// IO anomalies within +-2 callbacks of callback c (which must point into cb):
// an overload, an input sample-time gap (the sample time did not advance by
// the frames delivered) or a missing timestamp. Returns a short static label,
// or "" when the IO was clean.
const char *tone_io_near(const tone_cb_t *cb, uint32_t count, const tone_cb_t *c);

// Whole-run totals: input sample-time gaps and callbacks without a timestamp.
void tone_io_summary(const tone_cb_t *cb, uint32_t count, uint32_t *gaps, uint32_t *missing);

#ifdef __cplusplus
}
#endif

#endif
