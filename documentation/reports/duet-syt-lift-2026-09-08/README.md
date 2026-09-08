# Duet: replay phase promoted to absolute time

## Result

ASFW's receive anchor uses a modulo-16-cycle replay representation as an
absolute presentation time. For the retained Duet seed below, this adds exactly
49,152 FireWire ticks (2 ms, 96 frames at 48 kHz). A standalone reproduction
using the current ASFW headers reproduces the logged host timestamp exactly.

This explains the extra 2 ms in the driver's timestamp construction. It does
**not yet establish the cause of the approximately 0.9 ms HAL host-time
discontinuities in the new Instruments screenshot**. Those require individual
anchor and HAL-consumption observations. The running build's retained ZTS
records are gated to roughly four-second intervals.

No production code, stream state, or installed driver was changed for this
investigation. `reproduce.cpp` is a standalone characterization, not a shipping
source or a test asserting desired behavior.

## Live evidence

Read over the ASFW MCP control plane at `http://127.0.0.1:8766/mcp`, using the
bundled client. The initial ring statistics reported oldest sequence 1,
latest sequence 11856, and **zero dropped records**. Subsequent reads continued
into later sequences. These are separate snapshots, not one atomic sample.

The fresh stream's seed was mirrored to HAL at sequence 11331:

```text
[ZTS] rx epoch=2 sample=12288 host=8897650584889 period=12288
```

Sequence 11332 reported a 208 ms startup wait. Sequence 11341 supplied the RX
inputs (the telemetry log is drained later than the anchor publication):

```text
SEED count=1 frame=12288 host=8897650584889
drainHost=8897650550747 drainCycle=0x8caa58c7
rxCycle=0x8ca9f000 age=20679 rawRxTs=0xca9f
syt=0x1159 desc=33 dec=8 rate=48000 rateQ8=5333333
```

The retained sequence continued `count=17,33,48,...,533`, with an aggregate
slope of 499.984819991 host ticks/frame. The 35 retained records in this epoch
place their anchors 1409.583–2183.250 microseconds after the corresponding
pre-drain snapshots. This is not a measurement of HAL notification delay.

The stream-health snapshot reported 1,534,096 accepted RX packets, no rejected
packets or geometry errors, and one replay reset (initialization). TX was
running. A separate cursor snapshot reported zero deadline faults and missed
frames, nine expired PCM lookup attempts, and one PCM discontinuity. These
counters do not validate the HAL timestamp reference plane.

The two old `invalid-rx-timestamp` records at sequences 9937/9939 have
`drain=0x00000000` and precede both fresh seeds. Do not attribute them to the
current epoch's periodic HAL events.

## Where the 2 ms comes from

`OxfwProfileBuilder.cpp:55` calls `Common::AddDefaultTiming`; its Duet overrides
change latency/safety, not the default RX/TX transfer delays of 12,800 ticks
(`CommonProfileBuilder.cpp:118-119`). `ASFWAudioDevice.cpp:400-405` installs them.

For the seed, the receive cycle is 2719, whose low nibble is 15. SYT `0x1159`
has cycle nibble 1 and offset 345:

```text
forward cycle distance        = (1 - 15) modulo 16 = 2
direct forward SYT lead       = 2 * 3072 + 345 = 6489 ticks
configured RX replay delay    = 12800 ticks
replay offset                 = 6489 + 49152 - 12800 = 42841 ticks
RX absolute presentation lead = 42841 + 12800 = 55641 ticks
extra absolute time           = 55641 - 6489 = 49152 ticks = 2 ms
```

`ComputeReplaySytOffset` (`Audio/Wire/AMDTP/RxSequenceReplay.hpp:67-99`) performs
the modulo adjustment before subtraction. The helper's behavior is appropriate
for the unsigned replay representation. The problematic consumer is
`Audio/Engine/Direct/Rx/DirectAudioReceiveConsumer.cpp:491-503`: it adds the
delay back and hands the result to `HardwareSampleTimeline` as absolute bus
time. There is no modulo reduction at that boundary.

That timeline maps the absolute time into host ticks. The mailbox and
`ASFWAudioDriverZts.cpp:636-637` forward it unchanged to AudioDriverKit.

## Reference cross-check

Reference files were inspected in place; no reference implementation was
copied into the driver or reproduction.

* Linux `references/linux-sound-firewire-stack/firewire/amdtp-stream.c:484-503`
  computes the same nonnegative replay offset. Its consumer at `:1019-1030`
  adds transfer delay and masks the result into 16-bit SYT. An extra 16 cycles
  therefore disappears from the wire encoding. This is not an absolute host
  timestamp calculation.
* FFADO `references/libffado-2.5.0/src/libieee1394/cycletimer.h:390-440`
  reconstructs a received SYT against the receive cycle directly, without a
  transfer-delay threshold. The RX processor calls that function at
  `src/libstreaming/amdtp/AmdtpReceiveStreamProcessor.cpp:109`.
* Linux identifies Duet as blocking and requiring a non-NO_INFO SYT at
  `firewire/oxfw/oxfw.c:164-166`. Its OXFW start path nevertheless says playback
  timing follows the data-block sequence, not SYT presentation time
  (`firewire/oxfw/oxfw-stream.c:382-385`). Thus fixing the absolute software
  coordinate does not by itself validate the Duet's physical DAC reference.
* Local Apple reverse-engineering notes describe a separate bus/host
  correlation and timestamp calculation in
  `documentation/APPLE_DRIVER_TIMESTAMP_MECHANICS.md:446-477`. They do not
  establish that the Linux replay-offset adjustment belongs in RX anchoring.

## Why a local RX-only fix is insufficient

The TX replay path also uses `transmitBusTicks + replayOffset + txDelay` as
absolute time (`ASFWAudioDriverZts.cpp:775-781`). With equal delays and the same
phase branch, RX and TX both carry the extra 2 ms. It cancels in the one-time
TX content seed:

```text
firstFrame = observedFrame + (txPresentation - rxPresentation) / 512
```

Removing only RX's extra period increases that difference by 49,152 ticks:
**the initial TX frame moves forward 96 frames**. Correcting both absolute
coordinates preserves that difference in the reproduced equal-phase case.
This cancellation is conditional; it is not a proof about every replay phase.

The offset also has a discontinuity at the configurable threshold. A direct
lead of 12,799 ticks becomes 61,951; 12,800 becomes 12,800. A one-tick direct
change thus produces an absolute step of -49,151 ticks. This branch crossing
was reproduced synthetically, not observed in the retained live anchors.

Even changing only the configured replay delay from 12,800 to 6,000 moves the
reconstructed absolute RX time by 2 ms for the same received packet. Its
re-encoded wire SYT remains identical. This demonstrates why wire round-trip
tests alone cannot validate absolute presentation time.

## Reproduce

From the repository root:

```sh
c++ -std=c++23 -DASFW_HOST_TEST -I tests/mocks -I ASFWDriver \
  documentation/reports/duet-syt-lift-2026-09-08/reproduce.cpp \
  -o /tmp/asfw-duet-syt-reproduce
/tmp/asfw-duet-syt-reproduce
```

Observed output:

```text
directLead=6489 replayOffset=42841 reconstructedLead=55641 extra=49152 FW ticks
currentHost=8897650584889 directHost=8897650536889 difference=2000 us (48000 host ticks)
TX first frame: current=12888 RX-only change=12984 both direct=12888
threshold: direct 12799 -> 12800, reconstructed 61951 -> 12800 (step -49151 ticks)
```

## Next implementation boundary

Represent replay phase and absolute presentation time separately. Decode the
RX presentation using its receive-cycle reference, and resolve TX's replayed
SYT against the intended transmit cycle under an explicit presentation policy.
Preserve wire replay behavior while verifying the absolute coordinates and TX
content seed together. Validate same-nibble and 16-cycle/second/bus wraps,
transfer-delay threshold crossings, and unequal RX/TX delays.

For the HAL discontinuity investigation, capture every anchor plus its actual
HAL publication instant before claiming the 2 ms reference error explains
the screenshot's ~0.9 ms predictor residual. The previously identified
Instruments integer-division jitter artifact is a separate issue.

## Resolution (same day)

Fixed by reducing the reconstructed presentation lead into the SYT window at
both absolute-time consumers, not by changing the replay helper.

`ComputePresentationLeadTicks` (`Audio/Wire/AMDTP/RxSequenceReplay.hpp`) returns
`(sytOffset + transferDelayTicks) mod 49152`. `ComputeReplaySytOffset` keeps its
existing contract: its return is a phase, and `ComputeReplaySyt` still inverts
it. Two call sites changed —
`DirectAudioReceiveConsumer.cpp` (RX anchor) and `ASFWAudioDriverZts.cpp`
(TX replay presentation).

Why the reduction is exact rather than a clamp: a SYT cannot express a lead of a
full window or more, so the modulo only ever removes the artefact. Applying it to
both coordinates leaves their difference unchanged, which is what the section
above requires for the TX content seed. It also removes the threshold
discontinuity outright — a one-tick input change now produces a one-tick output
change, where it previously produced a -49,151-tick step.

`reproduce.cpp` still prints its original output and that is correct: it
exercises `ComputeReplaySytOffset` directly, and that function was never wrong in
its own terms. The behaviour it characterises now lives in
`tests/audio/RxSequencePresentationLeadTests.cpp` with the assertions expressed
against the fixed consumers. Four of those seven cases fail if the reduction is
removed; the other three assert the invariances that let the defect survive
(wire encoding unchanged, unlifted branch untouched, NO_INFO sentinel preserved).

The M-Audio internal transmit path (`ASFWAudioDriverZts.cpp:722`) was left alone.
Its `sytOffsetTicks` is an offset within the packet's own cycle (0..3071,
`MAudioInternalTxTiming.hpp:74`), never a lifted phase, so the sum cannot reach
a window and the reduction would be a no-op.

### What this does not fix

The magnitude is 96 frames at 48 kHz against a 60-frame output safety offset, so
it is large enough to matter on its own. It is nonetheless a **constant** for any
device whose SYT lead stays on one side of the transfer delay, which the Duet's
does — 7401 ticks against 12,800, on all 200 anchors of a 13.3-minute run. A
constant offset moves the anchor line without tilting it, so this is not a
candidate explanation for per-anchor jitter, and 200 retained anchors measured
over that run are collinear to 0.246 microseconds of standard deviation.

### Measured context for the strategy question

Read over the MCP control plane from the same ring, 200 anchors, 13.3 minutes:

```text
syt                          0x34e9 on all 200 (bit-identical)
rxCycle low nibble           1 on all 200
descriptorIndex mod 8        1 on all 200
device frames vs bus cycles  cumulative residual 0 ticks over 38,215,680 frames
                             -> |ratio - 512| < 0.157 ppm
device frames vs host clock  -32.02 ppm, stdev 0.96 ppm
```

The fixed phase is a consequence of the Phase 1 geometry: 12,288 frames is 512
whole cadence blocks and 192 whole completion groups, so the anchor lands at the
identical phase of everything. That aliasing cannot manufacture the constant
SYT, however — a constant at every 12,288-frame boundary requires the
SYT-per-frame ratio to be 512 within 0.0001 ppm, or wrong by a multiple of
7812 ppm. The startup `[RxWire]` burst confirms it off the anchor grid: every
SYT in a run lies on a 512-tick lattice at one fixed phase.

So for this device the SYT equals `frameCount * 512 + constant`, which the driver
already holds in `absoluteFrameCursor_`. It is well-formed and it is not an
independent clock observation. Linux reaches the same place from the device side:
`oxfw-stream.c:382-385` records that the device ignores received SYT presentation
time and recovers its media clock from the data-block sequence, and
`oxfw-stream.c:161-163` that the OXFW 970/971 ASIC has no SYT-driven playback
timing at all. Apple's own AV/C driver builds its timestamp from a cycle-time and
`clock_get_uptime()` pair (`InitializeTimeStampClock`, 0x434ce) validated by FDF
and DBC (`HandleNuDCLCallbackFirstTimeProcessing`, 0x3ed40); SYT does not appear
in it.

Two claims elsewhere in the tree are falsified by the measurement and should be
corrected when the clock-strategy work lands:
`APPLE_DRIVER_TIMESTAMP_MECHANICS.md` section 7 ("the sub-cycle precision
AppleUSBAudio estimates is, on FireWire, supplied by the device ... strictly
better than interpolation"), and section 8.10.2's exemption of the tested
hardware ("not a bug today -- the tested hardware (Duet, DICE) does carry SYT").
