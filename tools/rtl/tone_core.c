// tone_core.c -- see tone_core.h.
#include "tone_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TWO_PI (2.0 * M_PI)

// Blocks skipped after the tone arrives, so the converters' response to a
// tone switching on from silence is never judged: on the Pro 24 DSP that
// start transient lasts ~450 frames and read as one "click" per run. 32
// blocks = 2048 frames (43 ms at 48 kHz).
#define ONSET_SETTLE_BLOCKS 32
// Blocks skipped before the capture ends.
#define SETTLE_BLOCKS 4
// A block "fits cleanly" when the sine alone explains it this well.
#define CLEAN_FIT_NOISE_FACTOR 3.0
#define CLEAN_FIT_AMPLITUDE_TOLERANCE 0.1
// A residual sample this many noise-RMS off the reference is a glitch.
#define GLITCH_NOISE_FACTOR 12.0
#define GLITCH_FLOOR 2e-5
// A phase shift of at least this many frames afterwards is a slip.
#define SLIP_MIN_FRAMES 0.25
// Blocks used for the initial phase reference.
#define REFERENCE_BLOCKS 64
// Off-reference stretches closer than this are one disturbance: a glitch
// often knocks the phase away and lets it settle back over several blocks,
// which must be reported once, with its net slip, not as a string of clicks.
#define MERGE_GAP_FRAMES 1024

void tone_gen_init(tone_gen_t *g, double sampleRate, double freqHz, float amp) {
    g->phase = 0.0;
    g->step  = TWO_PI * freqHz / sampleRate;
    g->amp   = amp;
}

float tone_gen_next(tone_gen_t *g) {
    const float s = g->amp * (float)sin(g->phase);
    g->phase += g->step;
    if (g->phase >= TWO_PI) g->phase -= TWO_PI;
    return s;
}

const char *tone_event_name(tone_event_kind_t k) {
    switch (k) {
    case TONE_EVENT_CLICK:   return "click";
    case TONE_EVENT_DROPOUT: return "dropout";
    case TONE_EVENT_SLIP:    return "slip";
    default:                 return "?";
    }
}

typedef struct {
    double amp;    // fitted sine amplitude
    double phase;  // fitted phase: x ~ amp * sin(w*n + phase) + dc
    double dc;
    double err;    // RMS of the fit residual
} block_fit_t;

// Least-squares fit of a*sin(w n) + b*cos(w n) + c over one block.
static block_fit_t fit_block(const float *x, uint64_t n0, double w) {
    double ss = 0, sc = 0, s1 = 0, cc = 0, c1 = 0, xs = 0, xc = 0, x1 = 0;
    for (uint32_t j = 0; j < TONE_BLOCK; j++) {
        const double t = w * (double)(n0 + j);
        const double s = sin(t), c = cos(t), v = x[n0 + j];
        ss += s * s; sc += s * c; s1 += s;
        cc += c * c; c1 += c;
        xs += v * s; xc += v * c; x1 += v;
    }
    const double N = TONE_BLOCK;
    // Cramer's rule on the 3x3 normal equations.
    const double det = ss * (cc * N - c1 * c1) - sc * (sc * N - c1 * s1) + s1 * (sc * c1 - cc * s1);
    block_fit_t f = {0, 0, 0, 0};
    if (fabs(det) < 1e-12) return f;
    const double a = (xs * (cc * N - c1 * c1) - sc * (xc * N - c1 * x1) + s1 * (xc * c1 - cc * x1)) / det;
    const double b = (ss * (xc * N - c1 * x1) - xs * (sc * N - c1 * s1) + s1 * (sc * x1 - xc * s1)) / det;
    const double c = (ss * (cc * x1 - c1 * xc) - sc * (sc * x1 - s1 * xc) + xs * (sc * c1 - cc * s1)) / det;
    // a sin + b cos = A sin(t + phi), A cos phi = a, A sin phi = b.
    f.amp = sqrt(a * a + b * b);
    f.phase = atan2(b, a);
    f.dc = c;
    double e = 0;
    for (uint32_t j = 0; j < TONE_BLOCK; j++) {
        const double t = w * (double)(n0 + j);
        const double r = x[n0 + j] - (a * sin(t) + b * cos(t) + c);
        e += r * r;
    }
    f.err = sqrt(e / N);
    return f;
}

static int cmp_double(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

// Median of v[0..n); reorders v.
static double median_inplace(double *v, uint64_t n) {
    if (!n) return 0.0;
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    return (n & 1) ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

static double wrap_pi(double a) {
    a = fmod(a + M_PI, TWO_PI);
    if (a < 0) a += TWO_PI;
    return a - M_PI;
}

typedef struct {
    int      open;
    uint64_t first, last;
    double   slip;
    double   minAmp;
    double   peak;
    uint32_t off;
    uint32_t silent;
} open_event_t;

typedef struct {
    int      have;
    uint64_t first, last;
    double   slip;      // net over everything merged
    double   minAmp;
    double   peak;
    uint32_t off, silent;
} pending_event_t;

static void flush_pending(pending_event_t *p, tone_event_t *events, uint32_t maxEvents,
                          tone_result_t *out, double amplitude) {
    if (!p->have) return;
    tone_event_kind_t kind;
    // A dropout: most off-reference samples were silent where the tone
    // should have been, i.e. content was replaced rather than distorted.
    if (fabs(p->slip) >= SLIP_MIN_FRAMES)                    kind = TONE_EVENT_SLIP;
    else if (p->silent >= 2 && 2 * p->silent >= p->off)       kind = TONE_EVENT_DROPOUT;
    else                                                     kind = TONE_EVENT_CLICK;
    out->counts[kind]++;
    if (out->storedEvents < maxEvents && events) {
        tone_event_t *e = &events[out->storedEvents++];
        e->kind = kind;
        e->firstSample = p->first;
        e->lastSample = p->last;
        e->slipFrames = p->slip;
        e->offSamples = p->off;
        e->silentSamples = p->silent;
        e->minAmplitude = (float)p->minAmp;
        e->peakResidual = (float)(amplitude > 0 ? p->peak / amplitude : 0.0);
    }
    out->eventCount++;
    p->have = 0;
}

// Close the stretch that just ended; merge it into the pending disturbance
// when it follows closely, otherwise report the pending one and start anew.
static void close_event(open_event_t *ev, double slipFrames, pending_event_t *p,
                        tone_event_t *events, uint32_t maxEvents, tone_result_t *out,
                        double amplitude) {
    if (p->have && ev->first <= p->last + MERGE_GAP_FRAMES) {
        p->last = ev->last;
        p->slip += slipFrames;
        if (ev->minAmp < p->minAmp) p->minAmp = ev->minAmp;
        if (ev->peak > p->peak) p->peak = ev->peak;
        p->off += ev->off;
        p->silent += ev->silent;
    } else {
        flush_pending(p, events, maxEvents, out, amplitude);
        *p = (pending_event_t){1, ev->first, ev->last, slipFrames, ev->minAmp, ev->peak,
                               ev->off, ev->silent};
    }
    ev->open = 0;
}

int tone_analyse(const float *x, uint64_t n, double sampleRate, double freqHz,
                 tone_event_t *events, uint32_t maxEvents, tone_result_t *out) {
    memset(out, 0, sizeof *out);
    const double w = TWO_PI * freqHz / sampleRate;
    const uint64_t nb = n / TONE_BLOCK;
    if (nb < 32) {
        out->invalidReason = "capture too short";
        return 0;
    }

    block_fit_t *fit = malloc((size_t)nb * sizeof *fit);
    double *scratch = malloc((size_t)nb * sizeof *scratch);
    if (!fit || !scratch) {
        free(fit);
        free(scratch);
        return 1;
    }

    double maxAmp = 0;
    for (uint64_t k = 0; k < nb; k++) {
        fit[k] = fit_block(x, k * TONE_BLOCK, w);
        if (fit[k].amp > maxAmp) maxAmp = fit[k].amp;
    }
    if (maxAmp < 1e-4) {
        out->invalidReason = "no tone in the capture (check cable, volume and gain)";
        goto done;
    }

    // Reference amplitude: the typical block that carries the tone at all.
    uint64_t m = 0;
    for (uint64_t k = 0; k < nb; k++)
        if (fit[k].amp > 0.25 * maxAmp) scratch[m++] = fit[k].amp;
    const double ref = median_inplace(scratch, m);

    uint64_t onset = nb, end = 0;
    for (uint64_t k = 0; k < nb; k++)
        if (fit[k].amp > 0.5 * ref) { onset = k; break; }
    for (uint64_t k = nb; k-- > 0;)
        if (fit[k].amp > 0.5 * ref) { end = k + 1; break; }
    if (onset + ONSET_SETTLE_BLOCKS + SETTLE_BLOCKS + 16 > end) {
        out->invalidReason = "too little tone after it arrived";
        goto done;
    }
    const uint64_t from = onset + ONSET_SETTLE_BLOCKS, to = end - SETTLE_BLOCKS;

    m = 0;
    for (uint64_t k = from; k < to; k++) scratch[m++] = fit[k].err;
    const double noise = median_inplace(scratch, m);
    // Fitting a sine to noise always "finds" one; refuse to judge a tone that
    // does not stand clearly above the fit residual.
    if (ref < 10.0 * noise) {
        out->invalidReason = "no tone above the noise (check cable, volume and gain)";
        goto done;
    }
    m = 0;
    for (uint64_t k = from; k < to; k++) scratch[m++] = fit[k].dc;
    const double dc = median_inplace(scratch, m);

    // Phase reference: circular mean over the first clean blocks.
    double sx = 0, sy = 0;
    uint32_t used = 0;
    for (uint64_t k = from; k < to && used < REFERENCE_BLOCKS; k++) {
        if (fit[k].err > CLEAN_FIT_NOISE_FACTOR * noise) continue;
        sx += cos(fit[k].phase);
        sy += sin(fit[k].phase);
        used++;
    }
    if (used < 4) {
        out->invalidReason = "no clean stretch to take the phase reference from";
        goto done;
    }
    double phase = atan2(sy, sx);

    const double threshold = fmax(GLITCH_NOISE_FACTOR * noise, GLITCH_FLOOR);
    out->valid = 1;
    out->amplitude = ref;
    out->noiseRms = noise;
    out->threshold = threshold;
    out->onsetSample = onset * TONE_BLOCK;
    out->analysedFrom = from * TONE_BLOCK;
    out->analysedTo = to * TONE_BLOCK;

    open_event_t ev = {0, 0, 0, 0, 1.0, 0, 0, 0};
    pending_event_t pend = {0, 0, 0, 0, 1.0, 0, 0, 0};
    for (uint64_t k = from; k < to; k++) {
        const uint64_t n0 = k * TONE_BLOCK;
        double peak = 0;
        uint64_t firstOff = 0, lastOff = 0;
        uint32_t off = 0, silent = 0;
        int bad = 0;
        for (uint32_t j = 0; j < TONE_BLOCK; j++) {
            const double expected = ref * sin(w * (double)(n0 + j) + phase);
            const double r = x[n0 + j] - (expected + dc);
            if (fabs(r) > threshold) {
                if (!bad) firstOff = j;
                lastOff = j;
                bad = 1;
                off++;
                // Silent where the tone is clearly non-zero.
                if (fabs(x[n0 + j] - dc) <= threshold && fabs(expected) > 2.0 * threshold)
                    silent++;
            }
            if (fabs(r) > peak) peak = fabs(r);
        }
        if (!bad) {
            if (ev.open) close_event(&ev, 0.0, &pend, events, maxEvents, out, ref);
            continue;
        }

        const int cleanFit = fit[k].err <= CLEAN_FIT_NOISE_FACTOR * noise &&
                             fabs(fit[k].amp - ref) <= CLEAN_FIT_AMPLITUDE_TOLERANCE * ref;
        if (cleanFit) {
            // The tone itself is intact here; only its phase disagrees with
            // the reference. Whatever happened is over: re-lock on it and
            // report the shift. A shift of +k frames means k frames are
            // missing from the stream.
            const double slipFrames = wrap_pi(fit[k].phase - phase) / w;
            if (!ev.open) {
                ev.open = 1;
                ev.first = n0 + firstOff;
                ev.minAmp = 1.0;
                ev.peak = 0;
                ev.off = ev.silent = 0;
            }
            ev.last = n0 + lastOff;
            if (peak > ev.peak) ev.peak = peak;
            ev.off += off;
            ev.silent += silent;
            phase = fit[k].phase;
            close_event(&ev, slipFrames, &pend, events, maxEvents, out, ref);
            continue;
        }

        if (!ev.open) {
            ev.open = 1;
            ev.first = n0 + firstOff;
            ev.minAmp = 1.0;
            ev.peak = 0;
            ev.off = ev.silent = 0;
        }
        ev.last = n0 + lastOff;
        if (peak > ev.peak) ev.peak = peak;
        ev.off += off;
        ev.silent += silent;
        const double relAmp = fit[k].amp / ref;
        if (relAmp < ev.minAmp) ev.minAmp = relAmp;
    }
    if (ev.open) close_event(&ev, 0.0, &pend, events, maxEvents, out, ref);
    flush_pending(&pend, events, maxEvents, out, ref);

done:
    free(fit);
    free(scratch);
    return 0;
}
