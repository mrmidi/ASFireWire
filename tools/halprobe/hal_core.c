// hal_core.c -- platform-neutral core of the HAL geometry probe. See hal_core.h.
#include "hal_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------ clock fit
hal_fit_t hal_fit_line(const double *x, const double *y, int n) {
    hal_fit_t f;
    memset(&f, 0, sizeof f);
    f.n = n;
    if (!x || !y || n < 2) return f;

    // Centre both axes on their first sample. Differences of large absolute
    // values are exact enough; their squares summed are not.
    const double x0 = x[0], y0 = y[0];
    double mx = 0, my = 0;
    for (int i = 0; i < n; i++) { mx += x[i] - x0; my += y[i] - y0; }
    mx /= n; my /= n;
    double sxx = 0, sxy = 0;
    for (int i = 0; i < n; i++) {
        const double dx = (x[i] - x0) - mx, dy = (y[i] - y0) - my;
        sxx += dx * dx;
        sxy += dx * dy;
    }
    f.spanSeconds = x[n - 1] - x0;
    if (!(sxx > 0.0)) return f;  // all host times equal: no slope exists

    f.slope = sxy / sxx;
    // Intercept expressed at x0 (first observation), in absolute sample time.
    f.intercept = y0 + my - f.slope * mx;
    for (int i = 0; i < n; i++) {
        const double pred = f.intercept + f.slope * (x[i] - x0);
        const double r = fabs(y[i] - pred);
        if (r > f.worstResidual) f.worstResidual = r;
    }
    f.ok = 1;
    return f;
}

double hal_ppm(double fitted, double nominal) {
    return nominal > 0 ? (fitted - nominal) / nominal * 1e6 : 0.0;
}

// ------------------------------------------------------------ jumps
hal_jumps_t hal_classify_jumps(const double *h, const double *s, int n,
                               double rate, double threshold) {
    hal_jumps_t j;
    memset(&j, 0, sizeof j);
    for (int i = 1; i < n; i++) {
        const double advance = s[i] - s[i - 1];
        if (advance < 0) {
            j.backward++;
            if (advance < j.worstBackward) j.worstBackward = advance;
            continue;
        }
        const double excess = advance - (h[i] - h[i - 1]) * rate;
        if (excess > threshold) {
            j.forward++;
            if (excess > j.worstForward) j.worstForward = excess;
        }
    }
    return j;
}

// ------------------------------------------------------------ IO callbacks
void hal_iostats_record(hal_iostats_t *s, uint32_t frames) {
    s->cycles++;
    for (uint32_t i = 0; i < s->nspans; i++)
        if (s->spans[i] == frames) return;
    if (s->nspans < HAL_MAX_SPANS) s->spans[s->nspans++] = frames;
}

rtl_stat_t hal_callback_intervals_us(const double *h, int n) {
    rtl_stat_t empty;
    memset(&empty, 0, sizeof empty);
    if (!h || n < 2) return empty;
    double *d = (double *)malloc(sizeof(double) * (size_t)(n - 1));
    if (!d) return empty;
    for (int i = 1; i < n; i++) d[i - 1] = (h[i] - h[i - 1]) * 1e6;
    const rtl_stat_t s = rtl_stat(d, n - 1);
    free(d);
    return s;
}

// ------------------------------------------------------------ sweep
hal_sweep_verdict_t hal_sweep_verdict(int setOk, int readBackOk, uint32_t requested,
                                      uint32_t readBack) {
    if (!readBackOk) return HAL_SWEEP_UNREADABLE;
    if (!setOk) return HAL_SWEEP_REJECTED;
    return readBack == requested ? HAL_SWEEP_OK : HAL_SWEEP_COERCED;
}

const char *hal_sweep_verdict_id(hal_sweep_verdict_t v) {
    switch (v) {
    case HAL_SWEEP_OK:         return "ok";
    case HAL_SWEEP_COERCED:    return "coerced";
    case HAL_SWEEP_REJECTED:   return "rejected";
    case HAL_SWEEP_UNREADABLE: return "unreadable";
    }
    return "invalid";
}

int hal_sweep_sizes(double minFrames, double maxFrames, uint32_t *out, int cap) {
    if (!out || cap <= 0 || !(minFrames >= 1.0) || !(maxFrames >= minFrames)) return 0;
    const uint32_t lo = (uint32_t)ceil(minFrames);
    const uint32_t hi = maxFrames > 1048576.0 ? 1048576u : (uint32_t)floor(maxFrames);
    int n = 0;
    out[n++] = lo;
    for (uint32_t p = 1; p <= hi && n < cap; p <<= 1) {
        if (p > lo && p < hi) out[n++] = p;
        if (p > (1u << 30)) break;
    }
    if (hi != lo && n < cap) out[n++] = hi;
    return n;
}

// ------------------------------------------------------------ report
static void json_num(FILE *f, double v) {
    if (isfinite(v)) fprintf(f, "%.6f", v);
    else fputs("null", f);
}

static void json_u32(FILE *f, const char *key, hal_u32_t v, int comma) {
    fputs("      ", f);
    rtl_json_string(f, key);
    if (v.valid) fprintf(f, ": %u", v.value);
    else fputs(": null", f);
    fputs(comma ? ",\n" : "\n", f);
}

static void json_spans(FILE *f, const hal_iostats_t *s) {
    fputc('[', f);
    for (uint32_t i = 0; i < s->nspans; i++) fprintf(f, "%s%u", i ? ", " : "", s->spans[i]);
    fputc(']', f);
}

static void json_device(FILE *f, const hal_device_report_t *d, int last) {
    fputs("    {\n      \"name\": ", f);   rtl_json_string(f, d->name);
    fputs(",\n      \"uid\": ", f);        rtl_json_string(f, d->uid);
    fprintf(f, ",\n      \"object_id\": %u,\n      \"nominal_sample_rate_hz\": ", d->objectId);
    if (d->nominalRateValid) json_num(f, d->nominalRate); else fputs("null", f);
    fprintf(f, ",\n      \"channels_in\": %u,\n      \"channels_out\": %u,\n",
            d->inChannels, d->outChannels);
    json_u32(f, "zts_period_frames", d->ztsPeriod, 1);
    json_u32(f, "buffer_frames", d->bufferFrames, 1);
    fputs("      \"buffer_frame_range\": ", f);
    if (d->bufferRangeValid) {
        fputs("[", f); json_num(f, d->bufferMin); fputs(", ", f); json_num(f, d->bufferMax); fputs("]", f);
    } else {
        fputs("null", f);
    }
    fputs(",\n", f);
    json_u32(f, "uses_variable_buffer_sizes", d->variableBufferSizes, 1);
    json_u32(f, "clock_algorithm", d->clockAlgorithm, 1);
    json_u32(f, "clock_is_stable", d->clockIsStable, 1);
    json_u32(f, "running_somewhere", d->runningSomewhere, 1);

    const rtl_declared_t *c = &d->declared;
    fprintf(f, "      \"declared\": {\"safety_offset_in\": %u, \"safety_offset_out\": %u, "
               "\"device_latency_in\": %u, \"device_latency_out\": %u, "
               "\"stream_latency_in\": %u, \"stream_latency_out\": %u, \"missing\": [",
            c->safIn, c->safOut, c->latIn, c->latOut, c->strIn, c->strOut);
    for (int i = 0; i < c->nmissing && i < RTL_DECLARED_MAX_MISSING; i++) {
        if (i) fputs(", ", f);
        rtl_json_string(f, c->missing[i]);
    }
    int valid = 0;
    const uint32_t rt = rtl_declared_round_trip(c, &valid);
    fputs("], \"predicted_round_trip_frames\": ", f);
    if (valid) fprintf(f, "%u", rt); else fputs("null", f);
    fputs("},\n", f);

    fputs("      \"clock\": ", f);
    if (!d->clockRan) {
        fputs("null", f);
    } else {
        fprintf(f, "{\"reference\": \"hal_published_timeline\", \"samples\": %d, \"fit_ok\": %s",
                d->clockSamples, d->clockFit.ok ? "true" : "false");
        fputs(", \"span_seconds\": ", f); json_num(f, d->clockFit.spanSeconds);
        if (d->clockFit.ok) {
            fputs(", \"fitted_rate_hz\": ", f); json_num(f, d->clockFit.slope);
            fputs(", \"ppm_vs_nominal\": ", f);
            if (d->nominalRateValid) json_num(f, hal_ppm(d->clockFit.slope, d->nominalRate));
            else fputs("null", f);
            fputs(", \"worst_residual_frames\": ", f); json_num(f, d->clockFit.worstResidual);
        }
        fprintf(f, ", \"backward_jumps\": %u, \"forward_reanchors\": %u, \"worst_forward_frames\": ",
                d->clockJumps.backward, d->clockJumps.forward);
        json_num(f, d->clockJumps.worstForward);
        fputs("}", f);
    }
    fputs(",\n      \"io\": ", f);
    if (!d->ioRan) {
        fputs("null", f);
    } else {
        fprintf(f, "{\"cycles\": %u, \"distinct_spans\": ", d->io.cycles);
        json_spans(f, &d->io);
        fprintf(f, ", \"interval_us\": {\"n\": %d", d->ioIntervalsUs.n);
        if (d->ioIntervalsUs.n > 0) {
            fputs(", \"median\": ", f); json_num(f, d->ioIntervalsUs.median);
            fputs(", \"min\": ", f);    json_num(f, d->ioIntervalsUs.min);
            fputs(", \"max\": ", f);    json_num(f, d->ioIntervalsUs.max);
            fputs(", \"sd\": ", f);     json_num(f, d->ioIntervalsUs.sd);
        }
        fputs("}}", f);
    }
    fputs(",\n      \"sweep\": ", f);
    if (!d->sweepRan) {
        fputs("null", f);
    } else {
        fputs("{\"rows\": [", f);
        for (int i = 0; i < d->sweepRows && i < HAL_MAX_SWEEP_ROWS; i++) {
            const hal_sweep_row_t *r = &d->sweep[i];
            fprintf(f, "%s{\"requested\": %u, \"read_back\": %u, \"verdict\": ",
                    i ? ", " : "", r->requested, r->readBack);
            rtl_json_string(f, hal_sweep_verdict_id(r->verdict));
            fprintf(f, ", \"io_ok\": %s, \"observed_spans\": ", r->ioOk ? "true" : "false");
            json_spans(f, &r->io);
            fputc('}', f);
        }
        fprintf(f, "], \"restored_to\": %u, \"restore_ok\": %s}",
                d->sweepRestoredTo, d->sweepRestoreOk ? "true" : "false");
    }
    fprintf(f, "\n    }%s\n", last ? "" : ",");
}

int hal_write_json(FILE *f, const hal_provenance_t *p, const hal_device_report_t *devices,
                   int ndevices) {
    if (!f || !p || (ndevices > 0 && !devices)) return -1;
    fputs("{\n  \"schema\": \"asfw.hal_geometry.v1\",\n  \"provenance\": {\"tool\": \"hal_geometry\", "
          "\"tool_version\": ", f);
    rtl_json_string(f, p->toolVersion);
    fputs(", \"timestamp_utc\": ", f); rtl_json_string(f, p->timestampUtc);
    fputs(", \"os_version\": ", f);    rtl_json_string(f, p->osVersion);
    fputs(", \"argv\": ", f);          rtl_json_string(f, p->argv);
    fputs("},\n  \"devices\": [\n", f);
    for (int i = 0; i < ndevices; i++) json_device(f, &devices[i], i + 1 == ndevices);
    fputs("  ]\n}\n", f);
    return ferror(f) ? -1 : 0;
}
