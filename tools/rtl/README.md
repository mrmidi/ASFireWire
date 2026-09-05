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
It also checks that an empty window is *rejected* rather than fitted to noise.

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

## Reading the three numbers

| Number | Meaning |
|---|---|
| **`RTL_raw`** | frames between writing a sample into an output buffer and seeing it in an input buffer, counted by accumulating each callback's frame count. Never reads `mSampleTime`, so it is **immune to the reporting-only latency fields** and sensitive only to things that move real timing (safety offsets, buffer size, the hardware path). **This is the Phase 1 exit number.** |
| **`RTL_ts`** | the same event pair in the device sample-time domain the HAL hands to clients. A perfectly compensating host sees this. If every declaration were truthful it would be **0**; what it actually is, is the signed amount by which our declared path misstates the physical one. Positive = we under-declare, audio really arrives later than we claim. **This is the Phase 2 reference-plane input.** |
| **compensation** | `RTL_raw − RTL_ts`. Should reconcile with `2×io + latencies + safety offsets` from the declarations block. If it does not, a property we set is not reaching the HAL. |

`RTL_raw (onset)` is reported beside `RTL_raw` for comparison with
threshold-based analysers, which read early by the pre-ring extent. Trust the
peak: linear-phase converter filters ring symmetrically about the group delay.

### What invalidates a run

The tool prints all three; none is a hard failure, but each changes what the
number means.

- **`distinct spans` shows more than one value** — callback size varies, so any
  reasoning that assumed a fixed span is wrong (`hal_geometry --sweep` explores this).
- **`sample-time continuity` reports breaks** — a domain jumped rather than
  advancing by the frame count. `RTL_ts` is unreliable for that run, and the jump
  itself is review finding 5's territory (epoch transition without cursor
  translation). `RTL_raw` survives, because it never reads those timestamps.
- **weak SNR** — raise input gain before trusting sub-frame precision.
- **high `sd` across trials** — spread in `RTL_raw` is the observable form of the
  variable `I1`/`J4` waits the ledger marks unmeasured. Worth recording; it is
  free data for Phase 3 item 4.

## Bench self-check

Establishes that `RTL_raw` measures the physical path and not our own
bookkeeping — without which the whole measurement is circular.

In `ASFWDriver/Audio/Families/OXFW/OxfwProfileBuilder.cpp`, the Duet's 48 kHz
block sets `outputLatencyFrames = 67`. That value reaches
`SetOutputLatency` untouched (`ASFWAudioDriverGraph.cpp:686`); only output
*safety* passes through `RequiredOutputSafetyFrames`. So it is purely a
declaration.

1. Measure. Record `RTL_raw` and `RTL_ts`.
2. Change `outputLatencyFrames` from 67 to, say, 167. Rebuild, reinstall, rerun.
3. **`RTL_raw` must not move.** The physical path did not change.
4. **`RTL_ts` must move by −100 frames**, and the declared round-trip by +100.
5. Restore 67.

Failure at step 3 means `RTL_raw` is contaminated by a declared value, and no
number this tool produces can be trusted until that is understood.

**Do not probe with safety offsets or the client buffer size.** Those feed
`SetOutputSafetyOffset`/`SetInputSafetyOffset` and legitimately change physical
timing, so a moving measurement would prove nothing either way.

## Exit criterion

An absolute electrical RTL figure from a path with no compensation, validated by
the self-check — recorded here alongside the declarations that produced it, so
Phase 2 can decide the reference plane against a measured number instead of a model.
