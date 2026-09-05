# Phase 3 hardware validation — 2026-09-05

Validates the Phase 3 instrument repairs landed in `c912231e`, `e4464ae2`,
`c9e31d67`, `f0e724fa`, `9740d4b5`. Provenance in
[evidence/provenance.txt](evidence/provenance.txt): dext `0.3.0/44`, source at
`9740d4b5`, macOS 27.0 arm64.

Parent plan: [Audio latency ledger and timing SSOT](../../AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md).

## Status: partial. The physical path could not be measured.

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

## Not yet captured: the driver-side telemetry

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
