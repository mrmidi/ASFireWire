# Phase 1 — uncompensated electrical round-trip latency

Phase 1 of [`documentation/AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md`](../../documentation/AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md).

The Logic captures in this directory establish a **4.2× improvement** robustly,
but leave the absolute figure ambiguous: physical RTL is either ~270 frames
(5.6 ms) or ~541 (11.3 ms) depending on whether Logic compensated by our
declared values. The difference matters — 11.3 ms is audible for monitoring.
Only a measurement with nothing in the path that could compensate can say which.

Hence: **no DAW.** The tool talks to the HAL directly, and reports a number
derived by counting frames rather than by reading timestamps.

## Contents

| File | What it is |
|---|---|
| `rtl_loopback.c` | the measurement. Emits impulses, detects them, reports three numbers |
| `analyze_rtl.py` | offline analyser for pre-recorded WAV/AIFF click trains (the Logic-era method) |
| `*.aif`, `*.wav` | the Logic captures that produced the 4.2× figure |

`../halprobe/hal_geometry.c` covers the declared side in more depth (clock
slope, buffer-size sweep, span variation). Run it first when something looks odd.

## Build

```sh
cd tools/rtl
clang -O1 -o rtl_loopback rtl_loopback.c -framework CoreAudio -framework CoreFoundation
./rtl_loopback --selftest      # no hardware needed
```

The self-test synthesises trials with known delays — including the symmetric
pre-ringing a real converter pair produces — and checks that the detector
recovers them. Integer delays come back exact; fractional delays carry up to
**0.16 frames** of parabolic-interpolation bias, which is the instrument's
resolution floor (3 µs at 48 kHz, against an RTL of several hundred frames).
It also checks that an empty window is *rejected* rather than fitted to noise,
that a residual of exactly zero is *retained* rather than mistaken for a missing
value, and that a varying callback size is not read as a timeline break.

Run it after any edit to the detector. A measurement from an unverified
detector is not evidence.

## Physical setup

1. One **balanced or unbalanced cable, output 1 → input 1**. Electrical, not
   acoustic: this keeps speaker/air/mic out of the number. That also means the
   result **includes both converters** (DAC then ADC) and cannot separate them —
   see `I4` and `J1` in the ledger.
2. Output at unity, no DSP/monitor mix engaged on the device.
3. Input gain set so the returned impulse peaks around −6 dBFS. The tool prints
   the achieved SNR and warns below 30×.
4. Nothing else playing to the device. Quit anything holding it open.

## Procedure

```sh
./rtl_loopback -d ASFW                    # declarations only, no IO
./rtl_loopback -d ASFW --measure          # the measurement
./rtl_loopback -d ASFW --measure --frames 64 --trials 32
```

Record, for each run: sample rate, buffer size, the full declarations block, and
all three result numbers.

## Reading the numbers

The governing contract is Apple's. Jeff Moore, coreaudio-api
[2002/Aug/msg00055](/Users/mrmidi/DEV/MailingList/wayback-machine-downloader/archives/coreaudio_archives/2002/archives/coreaudio-api/2002/Aug/msg00055.html):

> The output time stamp passed in to your IOProc reflects the driver's safety
> offset, but not the latency in the hardware.

And [2004/Oct/msg00266](/Users/mrmidi/DEV/MailingList/wayback-machine-downloader/archives/coreaudio_archives/2004/archives/coreaudio-api/2004/Oct/msg00266.html),
where Moore endorses Sven Behne's model verbatim ("You have things correct"):
input timestamp − in_hw_latency is the analog capture instant, output timestamp
+ out_hw_latency is the audible instant, and the minimum thru time is
`in_hw_latency + in_buf + in_safety + out_buf + out_safety + out_hw_latency`.

Those two halves — scheduling and hardware — are what the tool separates.

| Number | Meaning |
|---|---|
| **`RTL_raw`** | frames between writing a sample into an output buffer and seeing it in an input buffer, counted by accumulating each callback's frame count. It never reads `mSampleTime`, so it is immune to the reporting-only latency properties and sensitive only to what moves real timing. It is the whole thru time. **The Phase 1 exit number.** |
| **`RTL_ts`** | the same event pair in the sample-time domain the HAL hands clients. Because those timestamps carry safety but **not** hardware latency, a truthful device returns `in_hw_latency + out_hw_latency` — **not zero.** This is the measured hardware latency of the analog path, converters included. |
| **scheduling distance** | `RTL_raw − RTL_ts`. Reduces algebraically to the per-callback input/output timestamp skew, and should reconcile with `2×io + in_safety + out_safety`. It contains **no latency term**, so it cannot tell you whether a declared latency reached the HAL. |
| **`RESIDUAL`** | `RTL_ts −` declared hardware latency (device + stream, both directions). The signed amount by which our declarations misstate the physical path; positive means we under-declare. **This is the Phase 2 reference-plane input.** |

`RTL_raw (onset)` is reported beside `RTL_raw` for comparison with
threshold-based analysers, which read early by the pre-ring extent. Trust the
peak: linear-phase converter filters ring symmetrically about the group delay.

### What invalidates a trial

The two failure modes are distinguished, because they invalidate different
numbers.

- **A gap in delivered frames** — the HAL skipped a callback under overload.
  `RTL_raw` counts delivered frames, so it reads *short by the gap* and the
  trial is **rejected outright**. Detected against `mach_absolute_time`, which
  no driver re-anchoring can move; reported as `delivered-frame lag`, alongside
  the device's own `processor overloads` count.
- **A sample-time re-anchor** — the driver's timeline jumped while delivery
  stayed continuous. This invalidates **`RTL_ts` only**; `RTL_raw` stands,
  because it never consulted those timestamps. Reported as
  `sample-time re-anchors`, and such trials are excluded from `RTL_ts` while
  still counting toward `RTL_raw`. The jump itself is review finding 5's
  territory (epoch transition without cursor translation).

Also watch:

- **`distinct spans` shows more than one value** — callback size varies, so any
  reasoning that assumed a fixed span is wrong (`hal_geometry --sweep` explores this).
- **weak SNR** — raise input gain before trusting sub-frame precision.
- **high `sd` across trials** — spread in `RTL_raw` is the observable form of the
  variable `I1`/`J4` waits the ledger marks unmeasured. Worth recording; it is
  free data for Phase 3 item 4.

## Bench self-check

Establishes that the measurement reads the physical path and not our own
bookkeeping — without which the whole thing is circular.

In `ASFWDriver/Audio/Families/OXFW/OxfwProfileBuilder.cpp`, the Duet's 48 kHz
block sets `outputLatencyFrames = 67`. That value reaches `SetOutputLatency`
untouched (`ASFWAudioDriverGraph.cpp:686`); only output *safety* passes through
`RequiredOutputSafetyFrames`. So it is purely a declaration.

1. Measure. Record `RTL_raw`, `RTL_ts`, and `RESIDUAL`.
2. Change `outputLatencyFrames` from 67 to, say, 167. Rebuild, reinstall, rerun.
3. **`RTL_raw` must not move** — the physical path did not change.
4. **`RTL_ts` must not move either.** IOProc timestamps carry safety but not
   hardware latency (Moore, 2002), so a latency property cannot shift them.
5. **`declared hw latency` must rise by 100, and `RESIDUAL` must fall by 100.**
6. Restore 67.

Movement at step 3 or 4 means a measurement is contaminated by a declared value,
and nothing this tool prints can be trusted until that is understood. A
`RESIDUAL` that does *not* move at step 5 means the property never reached the
HAL.

**Do not probe with safety offsets or the client buffer size.** Those feed
`SetOutputSafetyOffset`/`SetInputSafetyOffset` and legitimately change physical
timing, so a moving measurement would prove nothing either way.

## Exit criterion

An absolute electrical RTL figure from a path with no compensation, validated by
the self-check — recorded here alongside the declarations that produced it, so
Phase 2 can decide the reference plane against a measured residual instead of a
model.
