# Measurement baseline — current `main`

**Status: recipe ready, baseline NOT YET CAPTURED.** No measurement of the
current `main` exists anywhere: every recorded RTL result so far was taken on
the `midi` branch, whose provenance commits are not ancestors of `main`. The
first run of this recipe on real hardware creates the reference that the
timing, ZTS and TX-ownership work (Linear FW-177 / FW-186 / FW-209) must be
compared against.

Epic FW-169, ticket FW-176. Vocabulary: [`LATENCY_VOCABULARY.md`](LATENCY_VOCABULARY.md).

## Why a baseline, and why a distribution

The audio stack is about to be restructured. The project rule is that known-good
behaviour stays protected until its replacement is independently demonstrated,
and that requires numbers from the *old* stack before it is gone.

Recorded electrical RTL on `midi` was stable **within** a session but landed in
different classes **across** stream starts — 8.8 ms to 84 ms, with offsets of
whole 288-frame laps. A single session's median therefore says little. The
baseline is a **distribution over N sessions**, each a fresh stream start,
with the classes named.

## What is captured

Nothing new is added to the driver. The baseline uses only what `main` already
exposes plus the two host tools:

| Evidence | Source | Starts IO? |
|---|---|---|
| HAL declarations (latency, safety, stream latency, buffer size/range, ZTS period, clock algorithm) | `hal_geometry --json` ([tools/halprobe](../tools/halprobe/README.md)) | no |
| declared scheduling / hardware latency / round trip | `rtl_loopback` declarations-only mode | no |
| electrical round trip: `RTL_raw`, `RTL_ts`, scheduling distance, residual, per-trial verdicts | `rtl_loopback --measure --window auto --json`, once per session ([tools/rtl](../tools/rtl/README.md)) | yes |
| HAL-published timeline slope, ppm, residual, jumps | `hal_geometry --clock` | yes |
| driver-applied geometry: `HAL buffer profile`, `Reported HAL latency`, `GetZeroTimestampPeriod`, TX/RX transfer delay | unified log | — |
| runtime heartbeats: `[TxPrep]`, `[Zts]`, anomaly lines | unified log | — |
| commit, driver version, OS, machine, command | `provenance.json` | — |

The `TimingCursorPolicy … outSafety=8` log line is deliberately not used: it
prints fallback values, not what was applied.

## Prerequisites

1. macOS with the ASFW driver installed from the commit being baselined, and
   the device attached and publishing audio.
2. **One cable, output 1 → input 1**, electrical (no speaker/mic). The result
   includes both converters.
3. Output at unity, no DSP / monitor mix on the device. Input gain set so the
   returned impulse peaks around −6 dBFS.
4. Nothing else using the device; quit DAWs and players.
5. A quiet machine: overloads and dropped callbacks reject trials.

## Procedure

```sh
tools/rtl/build.sh                                         # builds rtl_loopback + hal_geometry
tools/baseline/capture_baseline.sh -d "<device>" -n 8 -t 20 -f 128
```

| Option | Default | Meaning |
|---|---|---|
| `-d` | `ASFW` | device name substring |
| `-n` | 8 | sessions (fresh stream starts) |
| `-t` | 20 | trials per session |
| `-f` | unchanged | client buffer size; record it — it moves the scheduling distance |
| `-c` | 30 | seconds of HAL clock watch (0 disables) |
| `-p` | 3 | pause between sessions |
| `-o` | `documentation/baselines/<UTC>-<device>-<sha>` | output directory |

The script refuses to measure if the `rtl_loopback` self-test fails.

Repeat per representative device (at least one DICE, one BeBoB/OXFW-class
device) and per buffer size of interest. Do not mix buffer sizes or rates in one
directory: the summary flags that as "not one baseline".

## Reading the summary

`summary.md` reports:

- accepted vs. run trials and the rejection breakdown by verdict. **The
  acceptance rate is part of the result.**
- the distribution of per-session medians of `RTL_raw`, `RTL_ts`, scheduling
  distance and residual;
- `RTL_raw` **classes** (session medians within 16 frames grouped together) with
  their offsets in frames and in 288-frame laps. More than one class means the
  round trip depends on how a stream started. Report the distribution, never one
  session's number.
- the HAL-published clock's ppm, residual and jumps.

Re-summarise with different grouping without re-measuring:

```sh
python3 tools/baseline/summarize_baseline.py <dir> --class-tolerance 32 --lap-frames 288
```

## Comparing before and after

A later change is compared against the committed baseline directory for the
same device, rate and buffer size:

1. same `repo_commit` family? same driver version? same OS?
2. acceptance rate and rejection breakdown — a change that makes trials
   unmeasurable is a regression even if the medians look fine;
3. class set and class centres of `RTL_raw` — new classes or a moved centre;
4. residual — the only number that tests a declaration (see the bench
   self-check in [`tools/rtl/README.md`](../tools/rtl/README.md));
5. HAL clock ppm and jumps.

## Recorded baselines

None yet. Commit each baseline directory under `documentation/baselines/` with
its `summary.md`, and list it here:

| Directory | Device | Rate | Buffer | Sessions | Classes | Notes |
|---|---|---|---|---|---|---|
| — | — | — | — | — | — | not yet captured |
