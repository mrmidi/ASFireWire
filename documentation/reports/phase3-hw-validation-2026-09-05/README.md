# Phase 3 hardware validation — 2026-09-05

Validates the Phase 3 instrument repairs landed in `c912231e`, `e4464ae2`,
`c9e31d67`, `f0e724fa`, `9740d4b5`. Provenance in
[evidence/provenance.txt](evidence/provenance.txt): dext `0.3.0/44`, source at
`9740d4b5`, macOS 27.0 arm64.

Parent plan: [Audio latency ledger and timing SSOT](../../AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md).

## Status: telemetry validated, one defect found, RTL measured on the second pass.

The driver log ring — not `log stream` — is where this telemetry lives.
`ASFW_LOG` always writes the ring and mirrors to os_log only when the mirror is
enabled, so the predicate capture returned nothing but its own header. Read it
with `asfw_log_query` over the MCP control plane on `127.0.0.1:8765/mcp`
(initialize, then `notifications/initialized`, then `tools/call`). The ring held
19520 records with **0 dropped** and `oldestSequence` 1, so no history was lost.

## Defect found: I1 and I2 are never measured on an RX-clocked device

`ObserveTxHardware` returns immediately unless the sample timeline is
TX-driven, and both ledger intervals were recorded inside it. This device is
RX-clocked (`[ZTS] rx`), which is the normal case, so `I1` and `I2` produced no
samples and no unresolved count — their heartbeat lines never printed at all.
Whether TX drives the timeline decides who may publish a boundary; it does not
decide whether the TX path's own intervals occur. Fixed by gating only the
boundary publication on the clock source.

## Measured: RTL, on the run that had a return path

The 60-trial run inside `capture.sh` had signal (peak 0.064, 32.5 dB SNR) and
admitted **60 of 60** trials.

| | frames | ms |
|---|---:|---:|
| `RTL_raw` | 1158.95 (sd 0.01) | 24.145 |
| `RTL_ts` | 930.95 (sd 0.01) | 19.395 |
| scheduling distance | 228.00 measured / 228 declared | — |
| `RESIDUAL` | +823.95 | +17.166 |

**This is the third `RTL_ts` value, and the three are spaced in exact hardware
ring laps.** With 48 packets per TX ring and 6 frames per packet, one lap is 288
frames:

| session | `RTL_ts` | delta from lowest | in ring laps |
|---|---:|---:|---:|
| prior fresh 64 | 354.95 | 0 | 0.00 |
| this run | 930.95 | 576.00 | **2.00** |
| earlier session | 2370.95 | 2016.00 | **7.00** |

Every pairwise delta is an exact multiple of 288 frames — 576, 1440, 2016. That
answers the question the restart series was going to ask: the ring implicated is
`kTransmitInFlightPackets` (48), not the 168-slot timeline array. The offset is a
whole number of TX descriptor-ring laps established at start.

Note also that 354.95 need not be the zero-lap base; it is only the lowest seen.
One lap below it is 66.95 frames, which is close to the driver's own named
below-client budget (~84 frames), so the possibility that *every* session
observed so far is displaced is open, not excluded.

## Earlier in the session: no return path was connected

**No loopback return path is connected.** Driving each output against each
input leaves input 1 flat at 0.000 and input 0 at its own noise floor
(0.003–0.004) regardless of which output carries the impulse, at amplitude 0.90.
That is an open input, not a weak return.

| out → in | input peaks | trials |
|---|---|---|
| 0 → 0 | 0.004 / 0.000 | 0 of 20 (20 no signal) |
| 0 → 1 | 0.003 / 0.000 | 0 of 3 (3 no signal) |
| 1 → 0 | 0.003 / 0.000 | 0 of 3 (3 no signal) |
| 1 → 1 | 0.003 / 0.000 | 0 of 3 (3 no signal) |

So `RTL_raw`, `RTL_ts` and `RESIDUAL` are **not measured in this session**, and
nothing here supersedes the provisional 12.145 ms from
[the 2026-09-05 latency investigation](../latency-investigation-2026-09-05/README.md).
Connect either the output→input cable or the mic-near-headphones rig and re-run.

## What was measured: the client-side audit, across four geometries

Every run clean. This exercises the driver end to end under the Phase 3 changes
and confirms buffer negotiation still holds after the reinstall.

| requested | observed spans | callbacks | gaps | re-anchors | clock disagreements | missing ts | overloads |
|---|---|---|---|---|---|---|---|
| 64 | 64 | 2229 | 0 | 0 | 0 | 0 | 0 |
| 128 | 128 | 1094 | 0 | 0 | 0 | 0 | 0 |
| 256 | 256 | 551 | 0 | 0 | 0 | 0 | 0 |
| 64 (return) | 64 | 2222 | 0 | 0 | 0 | 0 | 0 |

The RTL preflight landed in `f0e724fa` reported no buffer-request failure and no
unreadable declaration in any run, so the declared column is trustworthy here.

## Captured: the driver-side telemetry

Cumulative since driver start.

`[TxFill] filled=4274639 lost=3910 tooLate=311 unavailable=790074 silentData=74680 rebound=2133428 rejected=0 missedDeadline=609 stampsMissed=0`

| Counter | Value | Reading |
|---|---:|---|
| `stampsMissed` | **0** | the cursor drain never fell behind the stamp ring, so no ZTS boundary was lost to overrun |
| `rejected` | **0** | no late rebind failed geometry validation |
| `lost` | 3910 of 3.6M | 0.1% of accepted publications were sealed on the armed image |
| `missedDeadline` | 609 | the live-position recheck does fire — the old stale-snapshot guard was letting these through |
| `tooLate` | 311 | producer refused a slot already past finality |

`[Ledger] J3 recv->decode n=5457959 unres=0 min=0 mean=310 max=9240 us hist=[1419491,1165937,1774330,1040869,56668,493,161,10]`

Mean 310 µs against a 0.75 ms nominal, but with a real tail: 664 samples above
2 ms and 10 above 8 ms, max 9.24 ms. The nominal describes the centre and says
nothing about that tail, which is the point of measuring it.

`[Ledger] J4 decode->read n=146655 unres=207203 min=0 mean=786 max=6081 us`

**`unresolved` exceeds `samples`.** 59% of reads asked for a newest frame not yet
in the capture ring, which the primitive reports the same way as a stamp that
aged out. Those are different conditions — one says the reader is ahead of the
writer, the other says the measurement lost its endpoint — and conflating them
means the J4 histogram describes only the lagging half of the distribution.
Splitting the two is outstanding.

## Superseded: the earlier claim that no telemetry was captured

`[Ledger]`, `[TxFill]` and the rest are the actual Phase 3 deliverable, and the
agent's sandbox returns zero lines from `log stream` silently — it cannot
capture them. Run [`capture.sh`](capture.sh); it needs no loopback, since every
counter in it is driver-side.

What that capture has to answer:

| Line | Question it settles |
|---|---|
| `[Ledger] I1/I2/J3/J4` | the four distributions the ledger has only ever asserted; `unres=` must be small or the numbers are thin |
| `[TxFill] lost=` | publications accepted but sealed on the armed image — should be 0 or near it |
| `[TxFill] missedDeadline=` | how often the live-position recheck abandons a rebind; 0 means the old stale-snapshot guard was never actually being exercised |
| `[TxFill] stampsMissed=` | completion stamps aged out before the observer drained them; must be 0, or the ZTS boundary fix is running behind |
| `[TxFill] rejected=` | now counts packets, not attempts — not comparable with pre-`c912231e` captures |

## Files

- [provenance.txt](evidence/provenance.txt)
- [rtl-64-primary.txt](evidence/rtl-64-primary.txt) — 20 trials, no signal
- [rtl-sweep-64.txt](evidence/rtl-sweep-64.txt), [128](evidence/rtl-sweep-128.txt), [256](evidence/rtl-sweep-256.txt), [64r](evidence/rtl-sweep-64r.txt)
