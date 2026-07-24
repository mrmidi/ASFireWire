# asfw_sim

Four-cursor TX geometry simulator for ASFireWire. It exists because the existing
`tools/*.py` simulators model only **three** cursors and are therefore
structurally blind to the failure they were being used to rule out.

```
W = CoreAudio write frontier      host frames that exist
E = exposed frame end             host frames mapped into prepared TX packets
C = completion / committed end    packet slots safe for OHCI ownership
R = RX replay reader vs producer  observed timing available to build TX SYT
```

`tools/tx_data_horizon_burst_sim.py` and `tools/asfw_timing_geometry_sim.py`
contain **zero** references to `R`. They assume `E` advances to `W + horizon`
whenever the producer runs, so they cannot express a stalled exposure frontier.

**Results so far: [FINDINGS.md](FINDINGS.md).** They falsify the mechanism both
2026-07 triage reports assert, and identify a different one.

## Anti-drift

Constants are parsed live from the driver headers by `headers.py`
(`AudioTimingGeometry.hpp`, `AudioHalBufferProfiles.hpp`, `RxSequenceReplay.hpp`)
and `tests/test_constants_match_headers.py` fails if they move. Nothing here
hard-codes a geometry: both triage reports reached wrong conclusions from stale
constants, and that is the failure this design prevents.

## Run

```bash
uv sync --extra dev
uv run pytest -v

uv run asfw-sim                           # ALL live driver geometry (default)
uv run asfw-sim plan-io --frames 4096     # cost a larger CoreAudio buffer size
uv run asfw-sim run --stall-ms 100        # one scenario
uv run asfw-sim run --drift-ppm -4478 --zts-mode uncorrelated   # F9 reproduction
uv run asfw-sim cliff                     # bisect the stall-tolerance cliff
uv run asfw-sim sweep                     # readDelay / horizon comparison
uv run asfw-sim scenario                  # run the shipped YAML hypotheses
uv run asfw-sim scenario my-idea.yaml     # ...or your own
```

## Scenarios

A scenario is a hypothesis you can run, not a config file. It names a claim,
states the geometry the claim is about, and declares what must hold if the claim
is true -- so a wrong hypothesis fails loudly instead of producing a number
someone reads optimistically. `tests/test_scenarios.py` runs every shipped one.

```yaml
name: f3-capacity-2048-fix
description: kCapacity alone, 512 -> 2048; kReadDelay untouched.
base: driver          # inherit the live headers; `none` demands a full geometry
rate: 48000

geometry:             # overrides on top of `base`
  replay_capacity: 2048

scenario:
  duration_s: 20
  stall_ms: 100
  stall_at_s: 5

expect:               # or `sweep:` to explore instead of assert
  collapsed: false
  written_fraction_min: 0.95
```

`sweep:` takes lists for any geometry or scenario field and runs the cartesian
product. Overrides are checked against the header's own `static_assert` set;
an uncompilable geometry warns, or fails if you set `require_valid: true`.

This paid for itself immediately: `f3-readdelay-sweep.yaml` falsified the first
version of the F3 law on its first run, by varying `kReadDelay` and `kCapacity`
independently where every earlier sweep had moved them together.

## Fidelity

`producer.py` mirrors `PrepareTransmitSlots` (`ASFWAudioDriverZts.cpp:189-547`)
including its loop bound, `frameTargetSatisfied` early exit, the
`ahead` / `overwritten` / epoch branches, and the frame-cursor projection at
`:399-437`. `replay.py` mirrors `RxSequenceReplay.hpp:136-345` branch for branch.

Deliberate limits: not a bit-accurate SYT encoder, no OHCI descriptor model, no
CoreAudio HAL model. The seqlock failure classes cannot fire in a single-threaded
model and are present only for telemetry parity. Only 1x rates (44.1/48 kHz) are
implemented in the driver today — the 2x/4x tiers in `geometry.py` are carried so
the math is ready, and are not a validation gate.

## Clock domains and tick rates (F9)

> **WARNING: Three different clocks appear in this system. They are NOT the
> same oscillator.** Confusing FireWire ticks with host ticks introduces a
> systematic 2.3% error on Apple Silicon — half of F9's observed 4478 ppm.
> See `documentation/ZTS_AND_SYT.md` §2 Step 4 for the authoritative
> conversion pipeline.

### The three clocks

| Clock | Frequency | 1 tick = | Source |
|---|---|---|---|
| **FireWire bus** (cycle timer) | 24.576 MHz | 40.69 ns | IEEE 1394 bus specification; crystal-derived cycle master |
| **Apple Silicon host** (`mach_absolute_time`) | 24.000 MHz | 41.67 ns | SoC timebase; `mach_timebase_info` = {numer: 125, denom: 3} |
| **Intel Mac host** (`mach_absolute_time`) | 1.000 GHz | 1.00 ns | `mach_timebase_info` = {numer: 1, denom: 1} |

The FireWire tick (40.69 ns) and the Apple Silicon host tick (41.67 ns) are
**close but NOT equal** — a 125/128 = 2.34% ratio. Any code that subtracts
FireWire offset ticks directly from `mach_absolute_time` without converting
through nanoseconds accumulates ~2300 ppm of systematic error per second of
stream time. On Intel the ratio is 40.69:1 — a 97.6% error.

### The correct conversion (ZTS anchor pipeline)

```
FireWire ticks → nanoseconds → host ticks
     (fixed bus spec)     (platform-dependent mach_timebase_info)
```

```cpp
// FW ticks → nanos (FIXED, platform-independent):
nanos = fwTicks * 1'000'000'000 / 24'576'000;

// nanos → host ticks (PLATFORM-DEPENDENT, must use mach_timebase_info):
hostTicks = nanos * timebase.denom / timebase.numer;
//   Silicon: nanos * 3 / 125
//   Intel:   nanos * 1 / 1
```

ASFW's `TimingUtils.hpp` does this correctly via `FireWireTicksToNanos()` →
`nanosToHostTicks()`, both using runtime-queried `mach_timebase_info`. The
full worked example with real telemetry values is in
`documentation/ZTS_AND_SYT.md` §6B (age=2519 FW ticks → 2459 host ticks on
Silicon, matching the 125/128 ratio).

### Professional audio devices and PLLs

Professional FireWire audio interfaces (Saffire, Duet, MOTU, RME, etc.)
typically include a hardware PLL that locks their DAC/ADC sample clock to the
incoming isochronous stream's SYT/cadence. This smooths short-term jitter and
absorbs small clock mismatches between the bus and the device's local
oscillator. In practice this means:

- The device's audio oscillator drift relative to the bus is **bounded and
  small** (±20–50 ppm for a good PLL, not thousands of ppm).
- Short-term packet arrival jitter does not translate to audible clock error —
  the PLL tracks the average rate.
- A sustained rate mismatch beyond the PLL's capture range causes the device
  to slip (click/dropout), not a slow drift.

The F9 4478 ppm observed on the Duet is therefore **not** explainable by
oscillator drift alone — it exceeds any reasonable PLL capture range and would
have caused the device to lose lock entirely. This reinforces that F9 is a
driver-side structural issue (preparation path or ZTS correlation failure),
not a hardware clock problem.

### Why the sim is immune to the tick trap

The sim works in **bus cycles and audio frames**, never in host ticks or
nanoseconds. The `bus_drift_ppm` parameter models the device's audio
oscillator drift relative to the bus clock. The `zts_mode` parameter models
whether CoreAudio's IO callback timing tracks the bus rate (via ZTS) or runs
at nominal host rate. No tick-to-tick conversion exists in the sim, so the
2.3% trap cannot manifest here.

### The bus clock does not drift

The FireWire cycle rate (8000 cycles/s) is generated by the cycle master's
hardware crystal. Apple's own AVCVideoServices sample code treats the
tick-to-nanosecond conversion as a fixed constant (`40.690104167 ns/tick`)
with no measured-rate compensation. The "lost cycles" handling in that code
is for DCL callback scheduling jitter (host was late), not bus drift.

What **can** drift (±20–100 ppm, crystal tolerance):
- The device's **audio oscillator** vs the bus clock (separate crystal,
  bounded by the device PLL)
- The **host mach clock** vs the bus clock (separate oscillator)

Both are modeled by `bus_drift_ppm` (audio oscillator) and `zts_mode`
(whether the HAL compensates for host-vs-bus drift via ZTS correlation).

### IIR alpha is a sensitivity parameter, not a measured constant

The driver selects `IOUserAudioClockAlgorithm::SimpleIIR` but does not expose
its transfer function or smoothing coefficient. The sim's `iir_alpha` (default
0.25) is therefore **invented**. Correlated-mode results are sensitivity
analysis: they show that for any reasonable alpha (0.05–0.90), the geometry
absorbs realistic drift without collapsing. The exact alpha determines
convergence speed (peak transient deviation), not whether the stream survives.
Run `scenarios/f9-iir-sensitivity.yaml` to verify across the range.
