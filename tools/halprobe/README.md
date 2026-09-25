# hal_geometry — what Core Audio actually reports and does

Reads back the HAL's view of a device, as opposed to what the driver asked
for: declared latencies, safety offsets, ZTS period, buffer size and range,
the published clock, real callback spans and timing, and how buffer-size
requests are honoured. It needs **no dext rebuild and no privileges**.

It is the declared-side companion of [`../rtl/`](../rtl/README.md), which
measures the physical round trip. Vocabulary:
[`documentation/LATENCY_VOCABULARY.md`](../../documentation/LATENCY_VOCABULARY.md).

| File | What it is |
|---|---|
| `hal_core.h` / `hal_core.c` | platform-neutral maths and evidence JSON: centred clock fit, jump classification, callback-interval statistics, sweep verdicts. Unit-tested on any host (`tests/tools/HalCoreTests.cpp`). |
| `hal_geometry.c` | macOS front end: property reads, IOProc, clock sampling, sweep |
| `build.sh` | builds all host measurement tools (see `tools/rtl/build.sh`) |

## Build

```sh
tools/halprobe/build.sh      # or: cmake -S tools -B build/tools && cmake --build build/tools
```

## Use

```sh
./hal_geometry                          # snapshot every device -- passive, no IO
./hal_geometry -d ASFW                  # devices whose name contains "ASFW"
./hal_geometry -d ASFW --clock 20       # STARTS IO: fit the HAL timeline for 20 s
./hal_geometry -d ASFW --io 10          # STARTS IO: callback spans and intervals
./hal_geometry -d ASFW --sweep          # STARTS IO, CHANGES buffer size, restores it
./hal_geometry -d ASFW --json out.json  # also write the evidence record
```

Exit codes: `0` success, `1` enumeration failure / no matching device / cannot
write JSON, `2` bad argument, `130` interrupted.

## What each section means

**Snapshot** (always, passive). Every value that could not be read prints as
`<NOT READ>` and is `null` in JSON — never as a zero, which is a legal value
for every one of these properties. The `PREDICTED round-trip` (Apple's
composition: `2×io + device latency + safety offset + stream latency`, both
directions — identical to `rtl_loopback`'s declared round trip) is **withheld**
when any input is missing.

**Clock watch** (`--clock N`). Samples `AudioDeviceGetCurrentTime` at 50 Hz and
fits `sampleTime = a + b·hostSeconds`. The fit centres the host axis first:
absolute uptime is ~10⁵–10⁷ s, where the textbook least-squares form loses the
slope to cancellation (`HalFit.NaiveFormulaWouldHaveFailedAtThisUptime`).
Reported: fitted rate, ppm against nominal, worst residual, **backward jumps**
and **forward re-anchors** (advance exceeding the wall clock by > 5 ms of
frames).

> This measures the **HAL-published timeline**, which the driver drives
> through its zero-timestamps. It is not the physical word clock. A bad slope
> here is a ZTS problem, not necessarily a clock problem.

Refuses to run if the nominal sample rate cannot be read (no 48 kHz guess).
`AudioDeviceGetCurrentTime` only works while *this* client runs the device, so
a silent IOProc is started for the duration.

**IO probe** (`--io N`). Installs a silent IOProc: callback count, distinct
frame counts, and host-time **callback intervals** (median, min, max, sd, µs).
More than one frame count means callback size varies.

**Sweep** (`--sweep`). Walks the device's advertised buffer-size range (range
endpoints plus powers of two inside it), requests each size, reads it back
(`ok` / `coerced` / `rejected` / `unreadable`) and records the spans actually
delivered. The original size is restored afterwards — also on Ctrl-C and on
exit. It refuses to run if the current size or its range cannot be read, since
it could not restore them.

## JSON

Schema `asfw.hal_geometry.v1`: `provenance` (tool SHA, UTC timestamp, OS,
argv) and one object per device with `snapshot` fields, `declared`,
`clock`, `io` and `sweep` (each `null` when not run).
