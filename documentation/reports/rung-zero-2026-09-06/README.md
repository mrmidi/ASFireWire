# Rung zero, and why the measurements around it are not yet trustworthy — 2026-09-06 (evening)

Duet, 48 kHz, `tools/rtl/rtl_loopback -d Duet --measure`, output/input channel 0,
20 trials, 4096-frame window, client buffer 512 unless stated. Driver read over
the MCP control plane. Parent: [latency plan](../../AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md),
[timing domains §6](../../COREAUDIO_HAL_TIMING_DOMAINS.md).
Sibling: [earlier follow-up](../latency-followup-2026-09-06/README.md).

## Established

**The latency lattice has a floor and it was reached.** `RTL_ts = 66.95 + k x 288`
frames. k = 0 measured repeatedly; k = 2, 4, 5, 6 measured earlier the same day;
k = 13 appears in the sibling report's 64-frame runs (`3810.97 - 66.95 = 13 x 288`).
The fractional `.95` is identical in every measurement ever taken.

**66.95 frames (1.395 ms) is the entire physical path** — converters, wire,
device. Everything above it is accumulated displacement.

Cross-buffer corroboration: the sibling report measured `RTL_ts` 66.92 at a
**128**-frame client buffer; this session measured 66.95 at **512**. `RTL_ts` is
buffer-invariant; only the scheduling term scales.

**The prepared lead is not in the physical path.** Measured at three depths in
one session, geometry confirmed in the hot path each time via
`[TxV3] preparedTarget`:

| dispatch slack | prepared target | lead | `RTL_ts` |
|---:|---:|---:|---:|
| 72 (default) | 120 pkt | 720 fr | 66.95 |
| 48 | 96 pkt | 576 fr | 66.95 |
| 36 | 84 pkt | 504 fr | 75.95 (degraded, see below) |

144 frames of lead removed, zero frames of measured change.
`AudioTimingGeometry.hpp:158` ("storage capacity is not presentation latency")
is correct. **Dispatch slack is not a lever on output latency.** The
transmit-depth sweep is closed.

**The asserted dispatch-slack floor of 72 is real.** First hardware evidence for
`AudioTimingGeometry.hpp:214`. The largest coalesced completion delta
(`maxDelta`) was 16 at slack 72 — under a quarter of the budget — and reached
exactly **36** at slack 36, i.e. the budget itself. The assert's own sentence,
"a coalesced completion delta larger than this holes the descriptor ring", was
reached rather than argued.

**At rung zero we over-declare by 40 frames.** Declared hardware latency 107,
measured 67, residual −40.05 fr (−0.834 ms) — reproduced independently in the
sibling report (−40.08). This is the only declaration error left at the floor,
and it is the constant offset between what a host displays and the truth.

## The measurement record

| # | slack | `RTL_ts` | sd | SNR | accepted | note |
|--:|------:|---------:|-----:|------:|---------:|------|
| 1 | 72 | 66.95 | 0.01 | 33.6 | 20/20 | old build, after controller replug |
| 2 | 72 | 66.95 | 0.01 | 33.1 | 20/20 | old build, repeat |
| 3 | 48 | 66.95 | 0.01 | 31.7 | 15/20 | new build; 5 no-signal |
| 4 | 36 | 75.95 | 22.98 | 28.1 | 20/20 | SNR flagged *weak*; range [75.93 .. 131.95] |
| 5 | 72 | **74.95** | **11.55** | 32.5 | 20/20 | **control; did not reproduce #1** |
| 6 | 24 | 77.95 | 4.92 | **33.6** | 20/20 | lead 432 fr, the shortest of the session |

Driver state at #5 and #6: `lapsRecovered=0 lapEvents=0 lapUnresolvable=0
irqSilence=0`, zero Isoch errors, zero `[TxContent]` faults, geometry confirmed
in the hot path via `[TxV3] preparedTarget` each time — every counter clean.

### Run #6 kills the signal-quality explanation

Run #6 measured at **33.6 dB, the joint-best SNR of the session and identical to
run #1**, and produced sd 4.92 where run #1 produced sd 0.01. Weak signal was
offered as a possible cause of run #4's spread; it cannot explain #6. The jitter
is in the system, not the correlator.

### `RTL_ts` tracks elapsed time, not the setting

| | #3 | #4 | #5 | #6 |
|---|---:|---:|---:|---:|
| dispatch slack | 48 | 36 | 72 | 24 |
| prepared lead (fr) | 576 | 504 | 720 | 432 |
| `RTL_ts` | 66.95 | 75.95 | 74.95 | 77.95 |
| `carried` per 5000 polls | 2 | — | 68 | 77 |

The slack sequence is non-monotonic and spans 432-720 frames of lead. `RTL_ts`
and `carried` are both monotone upward with wall-clock session time. The setting
explains neither. Run #6 also re-closes the prepared-lead question from the
opposite direction: the *shortest* lead of the session produced the *highest*
latency.

The distribution converges rather than widening -- max 131.95 -> 100.96 -> 89.95,
sd 22.98 -> 11.55 -> 4.92, floor 74.94 -> 77.94 -- as though a bimodal split were
collapsing into a single higher mode. No mechanism is offered for that.

### Run #6 is where the stale counter cost a measurement

Run #6 existed to answer one question: did a coalesced completion delta exceed
the 24-packet slack budget? `maxDelta` reported 36 throughout -- run #4's
high-water, carried across three stream restarts because `ResetForStart` does not
clear it. The counter that would have answered the question was reporting a
different geometry. This is problem 5 below, and it is no longer hypothetical.

## Problems with the measurements

This is the important section. **None of the numbers above should be treated as
a controlled result.**

### 1. The control did not reproduce

Run #5 repeats run #1's geometry exactly and measures +8 frames with sd 11.55
against sd 0.01. The SNR was healthy (32.5 dB), so weak signal does not explain
it. Whatever changed was not the setting and did not leave when the setting was
restored. Until a repeat of #1 reproduces #1, **every per-setting difference in
this table is confounded.**

### 2. Setting and elapsed time are not separated

Runs were taken in sequence — 72, 48, 36, 72 — on one continuously running
system with no reset between conditions. Degradation increased monotonically
with *time*, not with the setting: `carried` (watchdog ticks that refilled with
no new interrupt) went from **2 per 5000 polls** at #3 to **68 per 5000 polls**
at #5, with the interrupt rate unchanged (`irq/poll` 1.70 vs 1.71). Delivery
became bursty over the session. A design that varies one factor while an
uncontrolled one drifts underneath it cannot attribute anything.

### 3. Latency moved with no lap loss

Run #5 is +8 frames with `lapsRecovered=0` and `lapUnresolvable=0` — the cycle
timer adjudicated every observation and found nothing. So the lap-loss mechanism
identified for the 288-frame lattice is **not** the only thing that can move this
number, and the 8-frame shift is a different phenomenon from the 288-frame
lattice. Conflating the two would repeat the error this investigation already
made once.

The 8-frame shift is unexplained. It happens to equal `frames=8`, the granularity
at which the hardware timeline observation advances (`[TxSeed] nominal=512
frames=8`), which would make it one observation granule. That is pattern-matching
on a small number and is recorded as a coincidence, not a finding.

### 4. The lap fix is untested

Every measurement this session sits inside a single rung (66.95 .. 75.95). No
288-frame step occurred, so nothing exercised `RecoverConsumedDelta`.
`lapsRecovered=0` is a control reading. The fix is neither confirmed nor
falsified.

### 5. The instrumentation reported health during three separate failures

- `[TxPrep] marginMin/marginMax` sat pinned at exactly the prepared target
  through a fatally stopped transmit path. It is a tautology —
  `margin = committedAfter - completion` while the producer prepares to
  `completion + target` — and cannot detect anything.
- `asfw_get_audio_stream_health` reported `receivingData` while TX was dead. It
  reads RX counters only; there is no TX verdict.
- `maxDelta` is a session high-water that is **not** reset by `ResetForStart`,
  so run #5 reports `maxDelta=36` — run #4's value, at run #4's geometry. The
  same applies to `lapsRecovered`, `lapEvents` and `lapUnresolvable`.
- `headroomMin`/`headroomRunMin` map the `UINT32_MAX` not-recorded sentinel onto
  `0`, the same value real producer starvation produces
  (`ASFWAudioDriverZts.cpp:1345`). The one counter that tracked the lattice
  cannot be read.

An earlier conclusion in this session — "the driver disagrees, every counter says
healthy" — was wrong for the first two reasons above plus a log query truncated
at 40 records, ~6000 short of the fault. The operator's ears were the better
instrument.

### 6. Measuring perturbs the system

Seven consecutive `rtl_loopback` runs preceded the first TX-IRQ-001 stall of the
day; two runs plus idle preceded the second. Each run opens, starts and stops the
device. Whether repeated cycling provokes the stall is **not** established — the
stall has also occurred unattended after 1h53m — but a measurement protocol that
may cause the fault it measures cannot be assumed neutral.

### 7. Single runs per condition, and correlator precision varies

Each row is one run. SNR ranged 28.1–33.6 dB across them and the tool flagged
#4 *weak*. Runs at sd 0.01 and runs at sd 23 are not the same kind of
observation and should not share a table column without that caveat.

### 8. Ring eviction

The driver log ring holds 39718 records and wrapped during the session
(`oldest` advanced to 43166). Records from earlier runs were evicted before they
could be compared. `droppedRecords` remained 0 throughout, so nothing was lost to
overflow — but chronology older than the window is simply gone.

## Not established

- That the 288-frame lattice is caused by lap loss. Argued from code and
  consistent with the evidence; never observed directly.
- That `RecoverConsumedDelta` prevents it.
- What the 8-frame shift in run #5 is.
- Whether repeated measurement provokes TX-IRQ-001.
- Any input/output split. The loopback measures the round trip; `RTL_ts` is the
  whole physical path both ways. A host's "output latency" display is its own
  arithmetic over our declarations, not a measurement.

## Protocol for the next measurement

Derived from what went wrong above.

1. **Fix the observability first** — per-stream counter reset, a TX verdict,
   `carried` as a rate, the headroom aliasing. Three failures this session were
   invisible to the instruments; more runs against blind instruments buy nothing.
2. **One condition per driver lifetime.** Replug or reload between conditions so
   accumulated state cannot cross a comparison.
3. **Repeat each condition at least three times**, and report the spread, not
   just the median.
4. **Re-measure the baseline last**, and discard the series if it does not
   reproduce the baseline first taken.
5. **Reject runs below ~31 dB SNR** rather than reporting them alongside clean
   ones.
6. **Record the driver state with each run** — `lapsRecovered`, `carried`,
   `irqSilence`, `maxDelta`, `[TxV3] preparedTarget`, epoch — in the same file as
   the RTL output.
