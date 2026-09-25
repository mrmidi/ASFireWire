# rtl_loopback — uncompensated electrical round-trip latency

Measures the physical round trip through a device with an **output → input
cable and no DAW in the path**, so nothing along the way can silently
compensate the number being read. The tool talks to the HAL directly and
derives its headline number by counting delivered frames, not by reading
timestamps.

Vocabulary (`RTL_raw`, `RTL_ts`, scheduling distance, residual, reference
planes) is shared with the rest of the project: see
[`documentation/LATENCY_VOCABULARY.md`](../../documentation/LATENCY_VOCABULARY.md).
Baseline procedure: [`documentation/MEASUREMENT_BASELINE.md`](../../documentation/MEASUREMENT_BASELINE.md).

## Contents

| File | What it is |
|---|---|
| `rtl_core.h` / `rtl_core.c` | platform-neutral core: timeline audit, trial admission, detector, aggregation, evidence JSON, self-test. No CoreAudio. Unit-tested on any host (`tests/tools/RtlCoreTests.cpp`). |
| `rtl_loopback.c` | macOS front end: device selection, declared geometry, IOProc, report |
| `analyze_rtl.py` | offline analyser for pre-recorded WAV/AIFF click trains (the DAW-era method) |
| `test_analyze_rtl.py` | unit tests for the analyser (run by ctest when Python 3 is available) |
| `build.sh` | builds every host measurement tool into `build/tools` |

`../halprobe/` covers the declared side in more depth (clock slope, callback
spans, buffer-size sweep). Run it first when something looks odd.

The DAW-era Logic captures (`duet_48k_mic_120bpm_click_roundtrip.aif`,
SHA-256 `a716737d…edb591`; `logic_pro_120bpm_click_and_c_rhodes_roundtrip.wav`,
SHA-256 `5d80a609…7453`) are **not** committed to `main`; they live on the
`midi` branch (`git show origin/midi:tools/rtl/<name> > <name>`). They were
compensated by the DAW and are historical context, not reference evidence.

## Build

```sh
tools/rtl/build.sh                 # or: cmake -S tools -B build/tools && cmake --build build/tools
tools/rtl/rtl_loopback --selftest  # no hardware needed; runs at 44.1, 48 and 96 kHz
```

The binary is stamped with the commit it was built from (`-dirty` if the tools
tree had local changes); that SHA is written into every JSON evidence file.
Never commit the binary — `.gitignore` excludes it.

### Self-test

Four layers, none of which needs hardware, run at 44.1, 48 and 96 kHz:

**Detector.** Known delays recovered from synthetic trials carrying the
symmetric pre-ringing a real converter pair produces. Integer delays come back
exact; fractional ones carry up to **0.16 frames** of parabolic-interpolation
bias, the instrument's resolution floor. An empty window is rejected rather
than fitted to noise, a residual of exactly zero is retained, and a peak sitting
on the capture-window edge is flagged rather than measured.

**Timeline audit.** Unit cases for each classification, including the boundary
ones: a varying callback size is not a break, a small callback dropped after a
large one is still caught, disagreeing clocks are rejected, a merely late
callback is not, and a loss hidden under a re-anchor of either sign does not
read as a re-anchor.

**Aggregation.** The scheduling distance is a paired per-trial difference,
never a difference of medians over two different trial sets.

**Adversarial simulator.** Drives the *same engine the hardware path runs* over
a known physical timeline and injects dropped callbacks, re-anchors of both
signs, missing timestamps, changed buffer sizes, scheduling jitter and
processor overloads — alone and in combination. Every expectation comes from
what was injected, never from recomputing the classifier's tolerances. A trial
spanning an injected fault must never be admitted; a trial spanning none must be
admitted *and* recover the injected round trip exactly.

Run it after any edit to the core. A measurement from an unverified detector is
not evidence. CI runs the same checks through `tests/tools`.

## Physical setup

1. One **balanced or unbalanced cable, output 1 → input 1**. Electrical, not
   acoustic. The result therefore **includes both converters** (DAC then ADC)
   and cannot separate them — see `I4` / `J1` in the vocabulary.
2. Output at unity, no DSP/monitor mix engaged on the device.
3. Input gain set so the returned impulse peaks around −6 dBFS. The tool prints
   the achieved SNR and warns below 30×.
4. Nothing else playing to the device. Quit anything holding it open.

## Procedure

```sh
./rtl_loopback -d ASFW                                   # declarations only, no IO
./rtl_loopback -d ASFW --measure --window auto           # the measurement
./rtl_loopback -d ASFW --measure --frames 64 --trials 32 --window auto --json run.json
```

| Option | Meaning |
|---|---|
| `-d <substr>` | first duplex device whose name contains `<substr>` (default `ASFW`) |
| `--measure` | start real IO and measure; without it only declarations are printed |
| `--frames N` | request a client buffer size; a refused/coerced request is reported |
| `--trials N` | impulses to emit (max 128, default 20) |
| `--window N\|auto` | capture window per trial (max 16384). `auto` = 4× the declared round trip, at least 1024. The window must exceed the real RTL, which is exactly what is unknown — prefer `auto` or a generous value |
| `--out-ch N` / `--in-ch N` | channels carrying the impulse (default 0 → 0) |
| `--amp F` | impulse amplitude (default 0.9) |
| `--json <path>` | write the evidence record (schema `asfw.rtl_loopback.v1`): provenance, declarations, IO audit, **every trial** with its verdict, and the summary |
| `--selftest` | run the self-test and exit |

Exit codes: `0` success or declarations-only, `1` no device / IO failure / no
accepted trial / self-test failure, `2` bad argument.

Record, for each run, the JSON file — it carries the sample rate, buffer size,
declarations, results **and the acceptance rate with its rejection breakdown**.
The rate is part of the measurement: a figure from 3 of 20 accepted trials is a
different claim from one out of 20.

If a buffer size rejects heavily, raising `--frames` is legitimate but produces a
**different measurement configuration** — larger buffers move the scheduling
distance. Keep the smaller-buffer run and its rejection breakdown.

**One session is not a baseline.** Recorded runs have landed in different
288-frame lap classes across stream restarts while staying stable within a
session. Use `tools/baseline/capture_baseline.sh`, which runs N sessions and
reports the distribution.

## Reading the numbers

The governing contract is Apple's. Jeff Moore, coreaudio-api 2002/Aug/msg00055:

> The output time stamp passed in to your IOProc reflects the driver's safety
> offset, but not the latency in the hardware.

And 2004/Oct/msg00266, where Moore endorses Sven Behne's model: input timestamp
− in_hw_latency is the analog capture instant, output timestamp +
out_hw_latency is the audible instant, and the minimum thru time is
`in_hw_latency + in_buf + in_safety + out_buf + out_safety + out_hw_latency`.

| Number | Meaning |
|---|---|
| **`RTL_raw`** | frames between writing a sample into an output buffer and seeing it in an input buffer, counted by accumulating each callback's frame count. Never reads `mSampleTime`, so it is immune to reporting-only latency properties. The whole thru time. **The headline number.** |
| **`RTL_ts`** | the same event pair in the HAL sample-time domain. Timestamps carry safety but **not** hardware latency, so a truthful device returns `in_hw_latency + out_hw_latency` — **not zero**. |
| **scheduling distance** | `RTL_raw − RTL_ts`, paired per trial. Should reconcile with `2×io + in_safety + out_safety`. Contains **no latency term**. |
| **`RESIDUAL`** | `median(RTL_ts) −` declared hardware latency (device + stream, both directions). Positive means we under-declare. **Withheld** if any declaration could not be read. |

`RTL_raw (onset)` is reported beside `RTL_raw` for comparison with
threshold-based analysers, which read early by the pre-ring. Trust the peak.

### Trial admission

**One gate, and it fails closed** (`rtl_trial_verdict()` in `rtl_core.c`). A
trial is admitted only if nothing anomalous happened anywhere inside it:

| Verdict | JSON id | Meaning |
|---|---|---|
| `accepted` | `accepted` | nothing happened; the only verdict that reaches any statistic |
| `lost frames` | `lost_frames` | the HAL skipped a callback, so `RTL_raw` reads short by the gap |
| `unclassified` | `unclassified` | the two clocks disagreed in a way matching neither hypothesis |
| `re-anchor` | `re_anchor` | the driver's sample timeline jumped |
| `no timestamps` | `no_timestamps` | timing evidence was missing |
| `processor overload` | `processor_overload` | the HAL reported an IO overload inside the trial |
| `no signal` | `no_signal` | no impulse above the noise floor |
| `window edge` | `window_edge` | the peak sits within 16 frames of the window end — the true arrival may lie beyond it |

The gap is detected **exactly**, from integer frame counts; the wall clock
(`mach_absolute_time`, which no driver re-anchoring can move) only decides which
failure it was. The re-anchor verdict is judged against the jitter scale, not
the jump size, so a large jump cannot hide a loss.

Processor overloads reject the trial because an overloaded cycle can lose or
delay frames in ways the timeline audit cannot always prove. The per-trial
overload count is in the JSON.

Also watch:

- **`distinct spans` shows more than one value** — callback size varies; any
  reasoning that assumed a fixed span is wrong (`hal_geometry --sweep`).
- **weak SNR** — raise input gain before trusting sub-frame precision.
- **high `sd` across trials** — spread in `RTL_raw` is the observable form of
  the variable `I1`/`J4` waits.

## Bench self-check

Establishes that the measurement reads the physical path and not our own
bookkeeping.

Device latency on `main` comes from the device profile's
`TxReportedLatencyFrames()` / `RxReportedLatencyFrames()` and reaches
`SetOutputLatency` / `SetInputLatency` unchanged
(`ASFWDriver/Audio/DriverKit/ASFWAudioDriverGraph.cpp`, the "Reported HAL
latency" block). Only safety offsets are floored on the way. For the Apogee
Duet that is `ApogeeDuetProfile::TxReportedLatencyFrames` (currently 128).

1. Measure. Record `RTL_raw`, `RTL_ts`, and `RESIDUAL`.
2. Change the reported output latency by +100 frames. Rebuild, reinstall, rerun.
3. **`RTL_raw` must not move** — the physical path did not change.
4. **`RTL_ts` must not move either** — timestamps do not carry hardware latency.
5. **`declared hw latency` must rise by 100, and `RESIDUAL` must fall by 100.**
6. Restore the original value.

Movement at step 3 or 4 means a measurement is contaminated by a declared
value. A `RESIDUAL` that does *not* move at step 5 means the property never
reached the HAL.

**Do not probe with safety offsets or the client buffer size.** Those
legitimately change physical timing.
