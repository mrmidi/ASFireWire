# TX refill flight recorder

Added after the September 7 [Apple Music failure](reports/rtl-repeat-2026-09-07/README.md).
This is diagnostic capture, not a fix for the lap estimator, retirement identity,
recovery starvation, or misleading health projection.

**September 7 follow-up:** the [finite-queue ownership repair](TX_FINITE_QUEUE_OWNERSHIP.md)
replaces inferred progress with safe retirement from descriptor status. The
recorder's historical `inferredDelta` field now holds the accepted retirement
count, excluding the retained continuation anchor. The observation-gap trigger
remains diagnostic; it does not decide whether a descriptor completed.

## Capture and ownership

Each TX ring owns 64 preallocated 128-byte records (8 KiB payload storage).
One record is written per refill batch after the existing MMIO snapshot. It
uses the already-read cycle timer, CommandPtr, context control and host-clock
bracket; it adds no MMIO reads, clock reads, payload hashing, allocation,
formatting, logging, blocking lock, or per-packet loop. Two shared atomic loads
capture completion/committed frontiers; mapped frontier is the writer's local
software fill index. These fields are observations, not an atomic producer/TX
transaction. The existing refill gate serializes writers.

The first observed interval >=48 cycles, completion delta >=48 packets, or refill
failure after that snapshot freezes the history, including the triggering
outcome. A 48-cycle interval is an ambiguity trigger, not a declaration of an
underrun. Flag 1 means previous cycle valid, 2 means completion delta >=ring,
4 means observed cycle gap >=ring. As with the underlying modulo timer, a full
eight-second alias cannot be detected here from cycle fields alone; host-tick
history is retained so it can be recognized offline if another trigger fires.

One consumer claims the frozen buffer through acquire/release publication.
It never reads mutable history. Restart/recovery does not reset the recorder;
subsequent writes are ignored. The history remains immutable after export.
The enclosing context must still quiesce users before destruction. A new
context lifetime starts a new recorder; there is no live reset command.

## Export

The primary TX watchdog diagnostic phase exports the frozen history once,
including when the context is stopped, under `[TxFlightA]`, `[TxFlightB]` and
`[TxFlightC]`. It does not depend on the shared control mapping still existing.
There are at most 192 lines. Formatting occurs after freeze, outside refill/IRQ;
it can affect subsequent scheduling but cannot change the frozen prehistory.
Read the driver log ring before eviction or controller destruction. Secondary
contexts require an explicit call to their `ExportFrozenRefills` method; the
existing primary watchdog hookup does not automatically enumerate them.

- A: context, chronological index/count, recorder epoch, source (1 interrupt
  callback, 2 watchdog, 0 unspecified/test), event entry ticks, cycle-read bracket.
- B: current/previous raw cycle timer, raw CommandPtr/control, previous/current
  ring slots, accepted retirement count, packets filled, flags and failure enum.
- C: pre-batch completion/mapped/committed frontiers, failed packet identity,
  expected/observed payload seals.

These are callback-level observations, **not physical IRQ arrival timestamps**.
The recorder itself adds no descriptor-binding array or descriptor-status
sweep. The completion path now walks status under the finite-queue ownership
contract linked above. Capture can expose a check beyond `mappedBefore`, and
separate a delayed observation from its subsequent fatal outcome; by itself it
cannot prove whether hardware skipped cycles or an interrupt waited upstream.

## Enable/disable and validation

`project.yml` sets `ASFW_TX_FLIGHT_RECORDER=1`. Build with
`./build.sh --no-bump --set ASFW_TX_FLIGHT_RECORDER=0` for an uninstrumented
comparison. The batch-capture block is compiled out in that configuration;
there is no runtime settings write in the audio path.

Validation: 28 targeted recorder/ring tests passed, including the real ring's
seal-failure path, freeze surviving ResetForStart, chronological wrap/truncation,
and a threaded immutable handoff. Removing frozen-write protection makes the
restart-preservation test fail (mutation killed). DriverKit/app build passed.
No driver installation or hardware test was performed for this change.

**Timing overhead is not yet measured.** Compare enabled and disabled builds
with the same playback workload and fresh starting state; inspect the existing
refill-duration tails, scheduling gaps and first-fault frequency. Do not use a
host microbenchmark as proof of DriverKit deadline neutrality. Keep the normal
refill policy unchanged during this comparison.
