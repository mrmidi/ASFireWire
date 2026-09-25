// hal_core.h -- platform-neutral core of the HAL geometry probe.
//
// hal_geometry reads back what Core Audio actually reports and does for a
// device, as opposed to what the driver asked for. The maths (clock fit, jump
// classification, callback-interval statistics, sweep verdicts) and the JSON
// evidence record live here, free of CoreAudio, so they are unit-tested on any
// host (tests/tools/HalCoreTests.cpp). hal_geometry.c is the macOS shell.
//
// Declarations reuse rtl_core's rtl_declared_t so the two tools agree on the
// round-trip composition and on "missing is never zero".
#ifndef ASFW_TOOLS_HAL_CORE_H
#define ASFW_TOOLS_HAL_CORE_H

#include <stdint.h>
#include <stdio.h>

#include "rtl_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_MAX_SPANS        64
#define HAL_MAX_SWEEP_ROWS   32

// A property that may not have been readable. `valid == 0` means NOT READ --
// never report its value as a zero.
typedef struct {
    uint32_t value;
    int      valid;
} hal_u32_t;

// ------------------------------------------------------------ clock fit
typedef struct {
    int    ok;            // enough points and a non-degenerate host span
    int    n;
    double intercept;     // sample time at the FIRST host time (centred fit)
    double slope;         // frames per host second -- the observed rate
    double worstResidual; // frames
    double spanSeconds;   // host time covered
} hal_fit_t;

// Least-squares sampleTime = a + b * hostSeconds. The host axis is centred on
// its first value before fitting: absolute uptime in seconds is ~1e5..1e7, and
// the textbook n*Sxx - Sx^2 form loses the slope to cancellation.
hal_fit_t hal_fit_line(const double *hostSec, const double *sampleTime, int n);

double hal_ppm(double fittedRate, double nominalRate);

// ------------------------------------------------------------ jumps
typedef struct {
    uint32_t backward;        // sample time went backwards
    uint32_t forward;         // sample time advanced more than the wall clock
                              // explains, by more than the threshold
    double   worstForward;    // frames beyond expectation, largest
    double   worstBackward;   // frames (negative), most negative
} hal_jumps_t;

// Classifies consecutive observations. Expected advance = host delta * rate;
// a forward re-anchor is an advance exceeding it by > thresholdFrames.
hal_jumps_t hal_classify_jumps(const double *hostSec, const double *sampleTime, int n,
                               double rate, double thresholdFrames);

// ------------------------------------------------------------ IO callbacks
typedef struct {
    uint32_t cycles;
    uint32_t spans[HAL_MAX_SPANS];
    uint32_t nspans;
} hal_iostats_t;

// Called from the IO thread: counts a callback and records its frame count.
void hal_iostats_record(hal_iostats_t *s, uint32_t frames);

// Interval statistics (microseconds) between consecutive callback host times.
rtl_stat_t hal_callback_intervals_us(const double *hostSec, int n);

// ------------------------------------------------------------ sweep
typedef enum {
    HAL_SWEEP_OK = 0,        // accepted as requested
    HAL_SWEEP_COERCED,       // accepted but read back differently
    HAL_SWEEP_REJECTED,      // the set call failed
    HAL_SWEEP_UNREADABLE,    // could not read the value back
} hal_sweep_verdict_t;

hal_sweep_verdict_t hal_sweep_verdict(int setOk, int readBackOk, uint32_t requested,
                                      uint32_t readBack);
const char *hal_sweep_verdict_id(hal_sweep_verdict_t v);

// Buffer sizes to sweep: powers of two within [minFrames, maxFrames], plus the
// range endpoints. Returns the count written (<= cap).
int hal_sweep_sizes(double minFrames, double maxFrames, uint32_t *out, int cap);

// ------------------------------------------------------------ report
typedef struct {
    uint32_t requested;
    uint32_t readBack;
    hal_sweep_verdict_t verdict;
    int      ioOk;
    hal_iostats_t io;
} hal_sweep_row_t;

typedef struct {
    // snapshot (always)
    const char *name;
    const char *uid;
    uint32_t    objectId;
    double      nominalRate;   // 0 if unreadable
    int         nominalRateValid;
    uint32_t    inChannels, outChannels;
    hal_u32_t   ztsPeriod;     // 'ring'
    hal_u32_t   bufferFrames;
    double      bufferMin, bufferMax;
    int         bufferRangeValid;
    hal_u32_t   variableBufferSizes;
    hal_u32_t   clockAlgorithm;  // 'clok'
    hal_u32_t   clockIsStable;   // 'cstb'
    hal_u32_t   runningSomewhere;
    rtl_declared_t declared;     // missing[] names every unreadable declaration

    // clock watch (optional)
    int         clockRan;
    int         clockSamples;
    hal_fit_t   clockFit;
    hal_jumps_t clockJumps;

    // IO probe (optional)
    int           ioRan;
    hal_iostats_t io;
    rtl_stat_t    ioIntervalsUs;

    // sweep (optional)
    int             sweepRan;
    int             sweepRows;
    hal_sweep_row_t sweep[HAL_MAX_SWEEP_ROWS];
    uint32_t        sweepRestoredTo;
    int             sweepRestoreOk;
} hal_device_report_t;

typedef struct {
    const char *toolVersion;
    const char *timestampUtc;
    const char *osVersion;
    const char *argv;
} hal_provenance_t;

// Writes {"schema": "asfw.hal_geometry.v1", "provenance": ..., "devices": [...]}.
int hal_write_json(FILE *f, const hal_provenance_t *p, const hal_device_report_t *devices,
                   int ndevices);

#ifdef __cplusplus
}
#endif

#endif // ASFW_TOOLS_HAL_CORE_H
