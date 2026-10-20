// tone_io.c -- see tone_io.h. Moved unchanged in behavior from rtl_loopback.c so
// the command line and the app share one implementation.
#include "tone_io.h"

const tone_cb_t *tone_cb_for(const tone_cb_t *cb, uint32_t count, uint64_t i) {
    if (!cb || !count) return NULL;
    uint32_t lo = 0, hi = count - 1;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo + 1) / 2;
        if (cb[mid].captureStart <= i) lo = mid; else hi = mid - 1;
    }
    return &cb[lo];
}

const char *tone_io_near(const tone_cb_t *cb, uint32_t count, const tone_cb_t *c) {
    if (!cb || !count || !c) return "";
    const uint32_t idx = (uint32_t)(c - cb);
    if (idx >= count) return "";
    const uint32_t lo = idx >= 2 ? idx - 2 : 0;
    const uint32_t hi = idx + 2 < count ? idx + 2 : count - 1;
    int gap = 0, overload = 0, missing = 0;
    for (uint32_t k = lo; k <= hi; k++) {
        if (!cb[k].itValid) missing = 1;
        if (k == 0) continue;
        const tone_cb_t *p = &cb[k - 1], *q = &cb[k];
        if (p->itValid && q->itValid && q->itSample != p->itSample + p->n) gap = 1;
        if (q->overloads != p->overloads) overload = 1;
    }
    if (overload) return "overload nearby";
    if (gap)      return "input sample-time gap nearby";
    if (missing)  return "missing timestamp nearby";
    return "";
}

void tone_io_summary(const tone_cb_t *cb, uint32_t count, uint32_t *gaps, uint32_t *missing) {
    uint32_t g = 0, m = 0;
    for (uint32_t k = 0; cb && k < count; k++) {
        if (!cb[k].itValid) { m++; continue; }
        if (k && cb[k - 1].itValid && cb[k].itSample != cb[k - 1].itSample + cb[k - 1].n)
            g++;
    }
    if (gaps) *gaps = g;
    if (missing) *missing = m;
}
