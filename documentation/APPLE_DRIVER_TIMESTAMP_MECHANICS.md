# How Apple's own audio drivers generate zero timestamps

Two sources, read in this order:

- **`AppleUSBAudio`** (§§1-7) — open source, no decompilation. Same
  `IOAudioEngine::takeTimeStamp` contract `AppleFWAudio` implements, so it
  establishes the shape.
- **`AppleFWAudio`** (§8) — disassembly, cross-checked against the
  **IOFireWireFamily-338-4-0 source** for every DCL API it calls.

> **Rule for anything DCL.** Apple's FireWire audio stack is expressed in
> **DCLs** (DMA Command List). A DCL statement is worthless on its own: a DCL
> claim must be carried down to the **OHCI isochronous DMA program and its
> descriptors** — which descriptor, which branch, which interrupt bit.
>
> §8.3 observes that rule. The mapping does **not** require Apple's OHCI driver:
> descriptor format is fixed by hardware, so Linux `firewire-ohci` and ASFW's own
> descriptor construction establish what any DCL must compile into. Each claim
> there cites all three.

Source: `/Users/mrmidi/Downloads/OSXUSBAudioDriverSource 4` (AppleUSBAudio,
~10.5-era with later `rdar://` fixes). Paths below are relative to it.

## 1. Interrupt cadence is wildly asymmetric — and deliberately so

`AppleUSBAudioStream.h:85-92`:

| | frame lists | USB frames/list | completion period | ring depth |
|---|---:|---:|---:|---:|
| **input** (`RECORD_*`) | 128 | 2 | **2 ms** (500 Hz) | 256 ms |
| **output** (`PLAY_*`) | 4 | 64 | **64 ms** (15.6 Hz) | 256 ms |
| output, sync variant | 4 | 32 | 32 ms | 128 ms |

Same buffer depth, **32x** difference in interrupt rate. Output takes about 16
interrupts a second.

This corroborates the standing note that Apple's FireWire stack ran ~20 ms RX /
~100 ms TX (`[[apple-fwaudio-isoch-geometry]]`): the same shape — fine-grained
input, very coarse output — appears in an entirely different transport. It is a
deliberate architectural choice, not a USB quirk.

## 2. Timestamp accuracy is decoupled from interrupt rate

This is the mechanism that makes a 64 ms interrupt period acceptable, and it is
the part worth copying.

A timestamp is taken **once per sample-ring wrap**, not once per completion. The
completion callback receives a `parameter` encoding *where inside the completed
frame list the wrap fell* (`AppleUSBAudioStream.cpp:3543-3549`):

```c
byteOffset   = parameter & 0xFFFF;          // byte within the transaction
frameIndex   = (parameter >> 16) - 1;       // frame within the list
byteCount    = pFrames[frameIndex].frActCount;
preWrapBytes = byteCount - byteOffset;
time = generateTimeStamp(frameIndex, preWrapBytes, byteCount);
takeTimeStamp(TRUE, &time);
```

`generateTimeStamp` (`AppleUSBAudioStream.cpp:1711`) then interpolates to
**sub-frame precision** off a per-USB-frame hardware anchor:

```
time = anchorTime + wallTimePerUSBCycle * ( remainingFullTransactions        preWrapBytes            )
                                          ( ------------------------  +  --------------------------- )
                                          (  transactionsPerUSBFrame     transactionsPerUSBFrame*byteCount )
```

Division is deferred to the last step deliberately, to keep precision
(`rdar://5178614` splits it in two to avoid roundoff).

**So the interrupt only has to be frequent enough to keep the ring fed. Timestamp
accuracy comes from knowing exactly where in the completed batch the wrap
occurred.** Coarse interrupts do not imply a coarse timestamp.

## 3. The ring wrap period — Apple's is *coarser* than ASFW's

`AppleUSBAudioStream.cpp:1367`: `numSamplesInBuffer = mCurSampleRate.whole / 4`,
rounded up to a multiple of two pages.

| driver | ZTS period @48 kHz | |
|---|---:|---|
| AppleUSBAudio | 16384 frames | **341.3 ms** |
| ASFW (`audio-engine-v3`) | 8192 frames | **170.7 ms** |

**This is precedent, not validation.** ASFW's configured ZTS period is not coarse
by Apple's own standard — it is twice as fine as AppleUSBAudio's. That bounds one
worry raised in `COREAUDIO_HAL_TIMING_DOMAINS.md` §2 and no more: a *configured*
period is not a *measured* publication cadence, and ring capacity says nothing
about first-anchor quality or whether the HAL's filter converges on this stack.
Those remain open and want measurement.

What ASFW does *not* have is §5's measured-rate filter. It does interpolate: it
projects a ZTS boundary falling inside a packet at the nominal rate
(`HardwareSampleTimeline.hpp:281-283`). The gap is filtered-rate vs nominal-rate
refinement, not the presence or absence of interpolation.

## 4. The first timestamp is special-cased, exactly as Jeff Moore prescribed

`AppleUSBAudioStream.cpp:3494-3513`. While `!mHaveTakenFirstTimeStamp`, the
handler **scans the frame list for the first frame with non-zero `frActCount`** —
the first frame that actually carried isoc data — and timestamps precisely
there, with `incrementLoopCount = FALSE`:

```c
for (UInt16 i = 0; i < numberOfFramesToCheck && pFrames; i++) {
    if (pFrames[i].frActCount && !self->mShouldStop) {
        time = self->generateTimeStamp(((SInt32) i) - 1, 0, 0);
        self->takeTimeStamp(FALSE, &time);
        break;
    }
}
```

This is the concrete implementation of "the first time stamp is the most
important one" (`COREAUDIO_HAL_TIMING_DOMAINS.md` §6, M3770). Apple does not
timestamp at start; it timestamps at the **first frame observed to have moved
data**.

## 5. The driver runs its own 33-tap FIR before the HAL ever sees the value

`AppleUSBAudioStream.cpp` `jitterFilter()`, added by `rdar://7378275`
("Improved timestamp generation accuracy"). It filters the **interval between
timestamps**, not the timestamp:

```c
filteredStampDifference = jitterFilter(rawStampDifference, mNumTimestamp);
filtered_time_nanos     = mLastFilteredTimeStamp_nanos + filteredStampDifference;
```

- symmetric 33-tap FIR, coefficients `{1,2,4,7,...,64,64,64,...,7,4,2,1}`,
  summing to exactly `kFilterScale == 1024` (`AppleUSBAudioStream.h:100-101`);
- for the **first 33 timestamps** a 4-tap boxcar `{256,256,256,256}` is used
  instead, so startup converges quickly rather than being dragged by an empty
  history;
- `nIter` must reset to zero if timestamps stop and restart.

**This puts a real number on the *driver-side* filter depth: 33 timestamps.** It
does not quantify M14477's "a few time stamps", which describes the HAL's own
filter sitting downstream — a different filter, and one the source still does not
put a number on. At ASFW's 170.7 ms period, 33 timestamps is ~5.6 s; at
AppleUSBAudio's output rate the same fill takes ~11 s. Apple evidently considers
that acceptable — because the 4-tap startup filter carries the first second.

## 6. Two details worth stealing

**Direction-dependent reference plane** (`AppleUSBAudioStream.cpp:1786`):

```c
if (getDirection() == kIOAudioStreamDirectionInput) {
    raw_time_nanos += divisor;   // one whole USB frame
}
```

with the comment that for input "we won't have access to this byte until one USB
frame later". The input and output timestamps are explicitly *not* the same
reference plane. ASFW should be able to state its own equivalent for RX vs TX.

**Only one stream timestamps.** Every `takeTimeStamp` site is gated on
`mMasterMode`. On a duplex device exactly one direction is the clock master;
the other never publishes. ASFW's equivalent is the
`HardwareTimelineSource::Receive`/`Transmit` choice at `ASFWAudioDevice.cpp:355`.

## 7. Most of this does **not** transfer to FireWire — and the reason matters

USB and FireWire do not have comparable timing substrates, so §2 and §5 are
compensations for a weakness FireWire does not have. Reading them as "best
practice to copy" would be a mistake.

**Why USB needs interpolation and a 33-tap FIR:**

- the only host-side time reference is the controller's own SOF, one USB frame
  per 1 ms, from a crystal unrelated to the device;
- an audio frame boundary does **not** align with a USB frame boundary — at
  48 kHz a frame carries 47, 48 or 49 samples, as the source comments note — so
  where a given sample sits inside a USB frame must be **estimated**;
- the anchor itself is noisy enough that ten of them are accumulated
  (`kAnchorsToAccumulate = 10`);
- the device's sample clock has no hardware relationship to the bus at all, so
  its rate must be *inferred* from the timestamp series.

Hence: sub-frame interpolation to recover position, and a 33-tap FIR to recover
rate. Both are host-side estimation standing in for a missing hardware clock.

**What FireWire has instead:**

- a bus-wide 24.576 MHz cycle timer distributed by cycle start packets, which
  the device is phase-locked to;
- exactly one isochronous packet per 125 us cycle, so the packet *is* the
  quantum — there is no "where inside the batch did this land" question to
  interpolate away;
- an OHCI hardware timestamp per packet, `sec[2:0]:cycle[12:0]`
  (`IsochTxDmaRing.cpp:877`), i.e. cycle-granular by construction;
- and, decisively, **IEC 61883-6 SYT**: the device itself states the
  presentation time of a data block at cycle-offset resolution
  (1/24.576 MHz ~ 40.7 ns), in the same clock domain the host reads.

So the sub-cycle precision AppleUSBAudio *estimates* is, on FireWire, *supplied
by the device*. ASFW already consumes it — `ComputeReplaySytOffset` at
`DirectAudioReceiveConsumer.cpp:430`, and
`presentationBusTicks = packetBusTicks + sytOffset + rxTransferDelayTicks`
at `:490`. That path is strictly better than interpolation, not a gap in it.

A heavy FIR is likewise suspect here: it buys jitter rejection at the cost of
group delay, against a rate reference that on FireWire is already hardware-
locked rather than inferred.

### So what does transfer

These three are about **anchoring discipline**, independent of clock quality:

| § | mechanism | why it still applies |
|---|---|---|
| 4 | First timestamp taken at the first frame *observed to have moved data*, not at start | Nothing about FireWire makes it safe to anchor on an assumed start instant. ASFW anchors from the first RX observation; whether that observation is the first packet that demonstrably carried audio is **unverified**. |
| 6 | Direction-dependent reference plane (input adds a whole frame) | ASFW's RX and TX reference planes are not stated in these terms, and §4 of the HAL doc makes that a live question for the residual. |
| 6 | Only the master stream publishes | ASFW's equivalent is the `Receive`/`Transmit` choice at `ASFWAudioDevice.cpp:355`. Same idea, already present. |

And §3 bounds — but does not close — the "our ZTS period is too coarse" concern:
a shipping driver ran a coarser configured period, which makes the number alone
unalarming. It does not measure ASFW's actual publication cadence or lock
behaviour, which is what the concern ultimately asks about.

**Cadence (§1) is an open question, not a transferable answer.** The 2 ms / 64 ms
split shows Apple was willing to run output interrupts very sparsely, and the
recorded ~20 ms / ~100 ms for their FireWire stack has the same shape — but the
FireWire numbers must come from the kext, not be inferred from USB.

## 8. AppleFWAudio: what the disassembly actually shows

IDA session over `AppleFWAudio-update.i64` (BeBoB / OXFW / AV-C devices).
Addresses are file offsets in that database. Every `IOFWDCL` call below was
checked against `IOFireWireFamily-338-4-0/IOFireWireFamily.kmodproj/IOFWDCL.h`.

Two receive implementations exist side by side — `AM824DCLRead` (legacy DCL) and
`AM824NuDCLRead` (NuDCL). The NuDCL path is the one examined here.

### 8.1 The 20 ms RX cadence is computed, not hardcoded

`AM824NuDCLRead::SetupReceiveBuffer` (`0x3d484`):

```c
buffersPerCallback = 0x4E20u / (125 * fNumPacketsPerBufferGroup);
```

`0x4E20` = 20000 (microseconds); `125` is us per isochronous cycle. So this is
"how many buffer groups span 20 ms". The `FireLog` immediately after prints
`kCallbackTimeoutInMSec=%lu` with the literal `20`, and the function refuses to
proceed if `fNumBufferGroups % buffersPerCallback != 0`.

**This confirms the ~20 ms RX interrupt cadence in `[[apple-fwaudio-isoch-geometry]]`
from source rather than from inference.**

### 8.2 Program shape

Built in `SetupReceiveBuffer`, a nested loop over `fNumBufferGroups` x
`fNumPacketsPerBufferGroup`:

- **one NuDCL receive command per isochronous packet**, created with three
  ranges — a 4-byte isoch header, an 8-byte CIP header, and the payload;
- `IOFWDCL::setTimeStampPtr(dcl, &fTimeStampArray[n])` on **every packet DCL**;
- a callback attached only to the last DCL of every `buffersPerCallback`-th
  group:

```c
if (!(bufferGroupIndex % buffersPerCallback)) {
    IOFWDCL::setFlags(lastDCL, 4u);                     // BIT(2)
    IOFWDCL::setRefcon(lastDCL, &perBufferGroupData[i]);
    IOFWDCL::setCallback(lastDCL, AM824NuDCLRead::s_NuDCLCallback);
}
```

- and finally `IOFWDCL::setBranch(lastDCL, firstDCL)` — the program is
  **circular**, the same shape as ASFW's descriptor ring.

`setFlags(…, 4)` is `kUpdateBeforeCallback` (`IOFWDCL.h:104`, `BIT(2)`).

### 8.3 DCL to descriptors — established, without reading AppleFWOHCI

An earlier revision parked this as "needs `AppleFWOHCI`". That was wrong. OHCI
descriptor format is fixed by **hardware**, so whatever Apple's DCL layer
expresses must compile into the same descriptors Linux and ASFW build. Two
independent implementations plus the hardware semantics settle it.

**Interrupt: `setCallback` on a DCL ⇒ interrupt bits set on that packet's
descriptor.**

The OHCI descriptor control word carries a 2-bit interrupt field at bits [5:4];
`3` means "interrupt on completion of this descriptor".

| source | encoding |
|---|---|
| Linux | `#define DESCRIPTOR_IRQ_ALWAYS (3 << 4)` (`ohci.c:66`), set per-descriptor in the IR queue path (`ohci.c:3298`) |
| ASFW | `kIntAlways` / `kIntNever` on `kCmdInputLast`, chosen by `IsTimingGroupBoundary(i)` (`IsochRxDmaRing.cpp:53,126`) |
| AppleFWAudio | callback on the last DCL of every `buffersPerCallback`-th group (§8.2) |

ASFW's `IsTimingGroupBoundary(packetIndex)` is
`(packetIndex % InterruptGroupPacketCount()) == (InterruptGroupPacketCount()-1)`
— interrupt on the **last packet of each group**, every other packet's
descriptor left at `kIntNever`. That is *structurally the same construction* as
Apple's "callback on the last DCL of every Nth buffer group". Same hardware
mechanism, same placement rule, different group size.

**Timestamp: a per-packet receive timestamp costs nothing.**

In OHCI IR header mode the controller writes the packet's timestamp **into the
buffer**, ahead of the payload — Linux: *"The OHCI controller puts the
isochronous header and trailer in the buffer, so we need at least 8 bytes"*
(`ohci.c:3326`). ASFW already decodes it:

```c
// OHCI IR header mode stores the 16-bit [sec:3][cycle:13] timestamp in
// the low half of the first little-endian quadlet. Cross-validated with
// Linux firewire/ohci.c:2765 and Apple's packetReceiveTime().
```
(`IsochRxTiming.hpp:29-31`, `DecodeReceiveTimestamp`)

So AppleFWAudio's `setTimeStampPtr` on **every** packet DCL is not a clever
trick and buys no interrupts: the hardware stamps every received packet inline
with its data whether anyone asked or not. Apple's 4-byte `isochHeaderRange` per
packet (§8.2) is that quadlet. **Every OHCI driver gets per-packet receive
timestamps for free; only the interrupt rate is a policy choice.**

*(Which of the two the family surfaces through `fTimeStampPtr` — the in-buffer
header quadlet or a descriptor status field — is an AppleFWOHCI detail and does
not matter here. The load-bearing fact is that a per-packet receive timestamp
exists without an interrupt, and that is guaranteed by the hardware.)*

**Branch:** `IOFWDCL::setBranch(lastDCL, firstDCL)` ⇒ the descriptor's branch
address pointing at the first descriptor block — a circular program, exactly
ASFW's ring.

### 8.3.1 The number that matters: ASFW interrupts 27x more often than Apple

Both drivers use the same mechanism and pick very different policy:

| | group size | interrupt period | interrupts/sec |
|---|---:|---:|---:|
| **AppleFWAudio** (RX) | 160 packets | **20 ms** | **50** |
| **ASFW** (`kPacketsPerCompletionGroup = 6`) | 6 packets | **750 us** | **1333** |

ASFW raises roughly **27 times more isochronous interrupts per second** than
Apple's driver did on the same class of hardware.

And by §8.3 that buys **nothing in timing resolution** — the per-packet
timestamps are written by hardware either way. The only thing a smaller group
buys is latency-to-service: how soon software reacts to a completed packet. That
is not nothing when the target is 2-3 ms RTL, because RX decode currently happens
*during* the completion drain — deferring the only service point to 20 ms defers
the input data with it, however precisely each packet was stamped.

This is worth weighing against `[[tx-irq-001-interrupt-path-stall]]`, where the
entire OHCI interrupt path was observed to latch up. Running an order of
magnitude more interrupts than the reference stack is not evidence of a cause,
but it is a large unexamined difference from the only implementation known to
have shipped on this hardware.

> **It is not a one-constant experiment.** An earlier revision said it was.
> `kPacketsPerCompletionGroup` (`IsochQueueGeometry.hpp:12`) is fused with three
> other policies:
>
> - `kPayloadFinalityLeadPackets = kPacketsPerCompletionGroup + 2` — the PCM
>   finalisation deadline moves with it;
> - `kRxPacketsPerGroup` / `kTxPacketsPerGroup` / `kTimingGroupPackets`
>   (`AudioTimingGeometry.hpp:49-53`) — RX and TX service cadence and the
>   32/40-frames-per-interrupt statistics all assume six;
> - two `static_assert`s: `48 % group == 0` and `group + 2 < 48`.
>
> Apple's 160 fails both asserts outright. 48 fails the finality assert *and*
> would leave a once-per-traversal modulo cursor structurally unable to observe a
> full lap. Even the legal divisors (8, 12, 16, 24) confound interrupt rate with
> PCM deadlines, so the result of any such run is uninterpretable.
>
> Decouple IRQ stride from finality lead and from the audio group size first.
> Then cadence can be varied as one thing at a time.

### 8.3.2 Callback placement is a per-configuration choice, not one policy

All three NuDCL program builders were read. They do **not** share a cadence.

| builder | addr | callback placement | per-packet `setTimeStampPtr` |
|---|---|---|---|
| `AM824NuDCLRead::SetupReceiveBuffer` | `0x3d484` | last DCL of every **N-th** group, N = 20 ms worth | yes, every packet |
| `AM824NuDCLWrite::SetupSendBufferForMacSync` | `0x4d8d0` | last DCL of every **N-th** group, N = 20 ms worth | yes, every packet |
| `AM824NuDCLWrite::SetupSendBufferForExternalSync` | `0x45012` | **exactly one**, after the loop, on the final DCL | yes, every packet |

All three compute the same `0x4E20 / (125 * packetsPerGroup)` group count;
`ExternalSync` computes it and then does not use it for callback placement.

`MacSync` additionally has a switch that installs **no callbacks at all**:

```c
if (LOBYTE(this[32].sampleRate)) { nextGroupIndex = groupIndex + 1; }   // no callback
else { ... if (!((groupIndex + 1) % groupsPerCallback)) { setCallback(...); } }
```

The split is by **who clocks the stream**, and it is the right split:

- **`ExternalSync`** — the *device* is clock master. TX has no independent
  timing duty, so it is serviced once per ring traversal; the RX side's 20 ms
  callbacks already carry the clock.
- **`MacSync`** — the *Mac* is clock master, so TX must be serviced on its own
  20 ms cadence.

**This is the configuration ASFW is in.** The Duet is clock master, ASFW runs an
RX-derived timeline (`HardwareTimelineSource::Receive`), so the comparable Apple
configuration is `ExternalSync`: **one TX interrupt per ring traversal.**

> **Correction.** An earlier revision of this section said "TX uses a single
> end-of-ring callback" without qualification, from having read only
> `ExternalSync`. That is true for device-clocked streams and false for
> Mac-clocked ones.

Note also that in the NuDCL path the sample-rate-family builders
`SetupSendBufferFor8NkHz` and `SetupSendBufferForMultipleOf44100Hz` are **stubs**
(7 and 10 bytes). NuDCL does not branch by rate at all — rate only selects
`SYT_INTERVAL` inside the builder (8 frames/packet at 44.1k and 48k, 16 at
88.2/96k, 32 at 176.4/192k, matching IEC 61883-6). The rate-family split is
legacy `AM824DCLWrite` only, where both are real implementations.

### 8.3.3 Interrupt rates side by side

| | group | period | interrupts/sec |
|---|---:|---:|---:|
| AppleFWAudio RX | 160 packets | 20 ms | 50 |
| AppleFWAudio TX, device-clocked | whole ring | ring traversal | ~10 |
| AppleFWAudio TX, Mac-clocked | 160 packets | 20 ms | 50 |
| **ASFW RX and TX** | **6 packets** | **750 us** | **1333** |

ASFW raises ~27x more RX interrupts than Apple, and — in the configuration it
actually runs, device-clocked — on the order of **100x more TX interrupts**. By
§8.3 that buys nothing in *timing resolution*, because the hardware stamps every
packet regardless; it does buy latency-to-service.

Two caveats on these ratios. They compare **callback placement**, which is what
the DCL setters show; they are not a full service-path comparison, since the work
that feeds and drains the ring is separate from the callback that triggers it.
And Linux establishes the OHCI interrupt *encoding*, not how Apple's NuDCL
compiler lowers every `setCallback` into descriptors — `IOFWDCL::setCallback`
itself only stores a pointer. Keep observed callback placement, inferred
descriptor policy, and actual interrupt counts distinguishable. Saffire, the
closer DICE reference, already shows a different callback policy from
AppleFWAudio (§8.9), so there is no single "reference cadence" to converge on.
See §8.3.1 for why this is not a one-constant change.

### 8.4 The filter is 4 taps, against USB's 33

`AM824NuDCLRead::CalculateNewTimeStamp` (`0x4331c`) keeps a 4-entry shift
register and averages it:

```c
// 4-iteration shift of the timing history, summing into timingValuesSum
newTimestampNanos = base + offset + timingValuesSum / 4 / 10;
```

Compare §5: AppleUSBAudio runs a symmetric **33-tap FIR** on the same quantity.
Same vendor, same era, same `takeTimeStamp` contract — **4 taps on FireWire, 33
on USB**.

That is §7's argument confirmed from Apple's own code rather than reasoned from
first principles. USB must infer rate from a noisy series; FireWire has a
bus-locked cycle timer and device-supplied SYT, so a 4-entry average suffices.
It also means a heavy filter is not merely unnecessary on FireWire but contrary
to how Apple built it.

### 8.5 The bus-clock/host-clock correlation

`AM824NuDCLRead::InitializeTimeStampClock` (`0x434ce`) reads, in one place:

- the FireWire cycle time (vtable call on the audio device), through
  `TimeConversionUtils::ConvertEncodedCycleTimeToNanoseconds`;
- `clock_get_uptime()`;
- converts both to nanoseconds and stores the **pair**, replicated across two
  entries, with a fixed-point numerator of `0x10000000` (2^28).

This is the concrete form of the requirement Jeff Moore gave a FireWire driver
author: *"you'll need to have an independent idea about how the FireWire clock
relates to the CPU clock"* (`COREAUDIO_HAL_TIMING_DOMAINS.md` M22275). There is
also `UpdateTimeStampClock(TimeConversionUtils::TimeRatioRecord*)` (`0x412f2`)
maintaining it during streaming.

### 8.6 Single master publisher — same as USB

`AM824NuDCLRead::SetMasterStream(bool)` (`0x40688`), with equivalents on the
write and legacy-DCL classes. Same idea as AppleUSBAudio's `mMasterMode` (§6)
and as ASFW's `Receive`/`Transmit` choice at `ASFWAudioDevice.cpp:355`.

### 8.7 Correction to an earlier note

"Buffer group" **is** an AppleFWAudio concept:
`PerReadBufferGroupDataStruct`, `fNumBufferGroups`,
`fNumBufferGroupsPerCallback`, `fPerBufferGroupDataArray`. A previous note
recorded it as *not* a FireWire concept on the strength of zero hits in
IOFireWireFamily. That was the wrong place to look: it is not an
*IOFireWireFamily* abstraction, but it is central to *AppleFWAudio*, where it is
the unit that callbacks are scheduled against.

### 8.8 The first timestamp: Apple validates the stream before anchoring

`AM824NuDCLRead::HandleNuDCLCallbackFirstTimeProcessing` (`0x3ed40`) walks the
CIP headers of the packets in the first serviced groups and refuses to anchor
until the stream proves itself:

- **FDF** (format-dependent field, i.e. sample rate) must equal `fExpectedFDF`;
  a mismatch logs `WRONG SAMPLE RATE` and resets `fStartupCounter` to 0;
- **DBC** (data block counter) must match the running expected value; a mismatch
  logs `bad calculated DBC`, **zeroes the sample-index and frames-per-packet
  arrays**, and restarts the count at 1;
- only once `fStartupCounter` passes its threshold does it log
  `good stream`, set `validSyncStream = 1`, latch the sample counter, set the
  DMA index and begin normal processing;
- after 4 bad-DBC events it sends a reset message (`1818326117` = `'lock'`).

This is the FireWire form of the discipline in §4, and it is **stricter** than
the USB one. AppleUSBAudio anchors at the first frame that demonstrably carried
data; AppleFWAudio additionally requires the *right rate* and a *continuous
data-block sequence* over a run of packets before it will anchor at all.

**Relevance to ASFW.** ASFW anchors from the first RX observation that passes
`ExpandReceiveTimestamp` and cadence checks. Whether that is equivalent to
Apple's "N consecutive packets with correct FDF and continuous DBC" is
**unverified**, and it bears directly on the open anchor question in
`COREAUDIO_HAL_TIMING_DOMAINS.md` §6.

### 8.9 DICE (`Saffire.i64`) diverges on both counts

Focusrite's Saffire driver is DICE/TCAT-based and also builds NuDCL programs
(`IOFWDCL` symbols are imported), but makes different choices.

`Saffire::PrepareRecvDCLs` (`0xfd14`):

- one send/receive DCL per packet, grouped, circular via `setBranch` — same
  skeleton;
- **`setCallback(lastDCL, RecvGroupCallback)` on the last DCL of *every* group**
  — no modulo, no 20 ms computation. Every group interrupts.
- **no `setTimeStampPtr` anywhere in the receive path** (confirmed by xref: zero
  references from `PrepareRecvDCLs`; only `PrepareSendDCLs` uses it, twice).

So a DICE driver does **not** harvest per-packet OHCI receive timestamps at all.

**That is the limit of what the DCL evidence supports.** Not asking NuDCL to
populate a timestamp slot means Saffire ignores the *OHCI hardware* stamp on
receive. It does **not** mean the receive clock is unrelated to the wire — and in
fact it is not. `Saffire::ReadFirewireBuffers` (`0xcf24`) extracts the SYT word
from the CIP header at `0xd5e7`, calls `SYTDiffInOffsets` and maintains a
512-entry delta history at `0xd69e-0xd73a`, and aligns sample positions through
`extendTstamp` / `tstampToOffsets` / `extOffsetDiff` from `0xdded`.

ASFW's own tree already says so: `RxSytCadence.hpp:11-15` names
`ReadFirewireBuffers at 0xd69e-0xd81e` as the behavioural source for its 512-SYT
warm-up. An earlier revision of this section concluded "its receive clock does
not come from the wire at all" — that was wrong, and contradicted by a header
comment in this repository.

The correct statement is narrower: **Saffire recovers its receive clock from the
CIP SYT field, not from the OHCI packet timestamp.** Two distinct clock sources
were conflated. Whether TCAT registers *also* participate is a separate question
this evidence does not answer.

`Saffire::PrepareSendDCLs` (`0x10304`) is different again — and stamps:

- **two parallel DCL chains per packet**, one for the payload and an 8-byte one
  for the CIP header, cross-linked by a second pass of `setBranch` at the end;
- `setFlags(dcl, 6u)` on **every** DCL of both chains;
- `setTimeStampPtr` on **both** DCLs of a packet, sharing one timestamp slot;
- `SendGroupCallback` on the last DCL of **every** group, again with no modulo.

| | AppleFWAudio (AV/C) | Saffire (DICE) |
|---|---|---|
| RX callback | every N-th group (20 ms) | **every group** |
| TX callback | every N-th group, or one per ring if device-clocked | **every group** |
| RX per-packet timestamp | yes | **none** |
| TX per-packet timestamp | yes | yes (two DCLs share one slot) |
| DCLs per packet | 1 | **2** (payload + header chains) |
| RX clock source | OHCI packet timestamps + SYT | **CIP SYT only** (512-delta history) |

So DICE stamps on transmit but not receive, and interrupts far more often than
AppleFWAudio in both directions. Both drivers recover the receive clock from the
wire; they differ in whether the OHCI packet timestamp participates.

### 8.10 ASFW's anchor gate against Apple's — the one code-level gap

Apple (§8.8) will not anchor until it has seen, over a run of packets:

1. **FDF** matching the expected sample rate, and
2. **DBC** running continuously.

ASFW's gate, at `DirectAudioReceiveConsumer.cpp:467-471`, is:

```c
if (result.framesDecoded != 0 && packetHostTicks != 0 &&
    clockPublisher_.IsBound() && cadence.established &&
    result.hasValidCip && result.syt != 0xffff &&
    hardwareTimeline.Source() == HardwareTimelineSource::Receive)
```

`cadence.established` is `validUpdates >= kWarmupUpdates`, and
`kWarmupUpdates = kEntryCount + 1 = 513` (`RxSytCadence.hpp:19-21`). A "valid
update" is a SYT whose delta from the previous SYT lies in `(0, 65535]`. So ASFW
*does* wait for 513 plausible SYT deltas before it will anchor.

> **They are cumulative, not consecutive — and that is a second gap.** An earlier
> revision of this section called them consecutive. On an invalid delta,
> `RxSytCadence::Observe` (`RxSytCadence.hpp:67`) clears only `previousSyt_`; it
> leaves `validUpdates_`, the 512-entry ring and `rollingCadenceTicks_` intact.
> So 512 valid deltas, then a break, then a reseed and **one** further valid
> delta reaches `established`. The consumer's rejection handler
> (`DirectAudioReceiveConsumer.cpp:417`) calls `ResetReplayEpochForDiscontinuity`
> only when replay is *already* established, so it cannot repair a broken
> warm-up either. A cadence ring holding a discontinuity is admitted as if it
> were clean. If consecutiveness is the intended contract — Apple's *is*
> consecutive, it resets its counter on mismatch — then this needs stating and
> enforcing, and the interrupted warm-up needs a test through the consumer.
>
> The warm-up interval was also given as ~64 ms. That assumed 8000 valid SYTs
> per second, i.e. one per isoch cycle. A blocking 48 kHz stream carries eight
> frames in each DATA packet, so it emits 6000 DATA packets/s and 2000 NO_DATA
> packets whose SYT is `0xffff` and contributes nothing. 513 valid deltas is
> therefore **~85 ms**, not 64 — and longer on any stream with more empties.

**But it waits on a different quantity.** Comparing gate by gate:

| check | Apple | ASFW |
|---|---|---|
| timing series plausible | **yes**, consecutive | **partly** — 513 *cumulative* SYT deltas |
| CIP structurally valid | yes | **yes** — `hasValidCip` |
| SYT present | yes | **yes** — `syt != 0xffff` |
| **FDF matches configured rate** | **yes**, resets startup counter on mismatch | **no** |
| **DBC continuous** | **yes**, zeroes arrays and restarts on mismatch | **no** |

Both gaps are verifiable in the tree:

- **FDF is parsed and never compared.** `RxAudioPacketProcessor.cpp:46` sets
  `result.fdf`; its only other appearance is a log line at
  `DirectAudioReceiveConsumer.cpp:713`. The expected value *is* available —
  `ResolvedAudioStreamProfile` carries `captureFdf` — and is never checked
  against the wire.
- **DBC is accumulated, not validated.** `DirectAudioReceiveConsumer.cpp:329-335`
  adds `(uint8_t)(result.dbc - lastDbc_)` into `rxDbcFrameCount` and stores
  `lastDbc_`. No discontinuity check, nothing gated.

So ASFW can anchor its HAL timeline on a stream that is running at a rate it did
not ask for, or whose data-block sequence has already broken — conditions under
which Apple explicitly refuses to anchor and, after four DBC failures, resets the
device.

**Why this matters beyond tidiness.** The anchor is taken once and, per M3770,
its error goes straight into the HAL's clock. `COREAUDIO_HAL_TIMING_DOMAINS.md`
§6 records an unexplained whole-lap offset in the TX content cursor -- an offset
that document declines to attribute to the anchor. Anchoring on an
unvalidated stream is a mechanism by which the first observation could be taken
from a packet that does not mean what the timeline assumes it means. **This is
not a demonstrated cause — it is an untested difference from the reference, in
exactly the area under investigation.**

### 8.10.1 But do NOT simply copy Apple's gates — they are device-specific

Apple could afford strict FDF and DBC gates because `AppleFWAudio` shipped
against a **known, tested set** of AV/C devices. ASFW is general-purpose, and
Linux — which supports far more hardware — needs a **twelve-flag quirk matrix**
to make the same checks safe (`amdtp-stream.h:44-58`):

```c
CIP_EMPTY_WITH_TAG0, CIP_DBC_IS_END_EVENT, CIP_WRONG_DBS,
CIP_SKIP_DBC_ZERO_CHECK, CIP_EMPTY_HAS_WRONG_DBC, CIP_JUMBO_PAYLOAD,
CIP_HEADER_WITHOUT_EOH, CIP_NO_HEADER, CIP_UNALIGHED_DBC,
CIP_UNAWARE_SYT, CIP_DBC_IS_PAYLOAD_QUADLETS
```

An unconditional DBC-continuity gate would break, by Linux's own annotations:

| family | quirk | why a naive DBC gate fails |
|---|---|---|
| **BeBoB** | `CIP_EMPTY_HAS_WRONG_DBC` | empty packets carry a wrong DBC |
| Fireworks | `CIP_DBC_IS_END_EVENT`, `CIP_SKIP_DBC_ZERO_CHECK`, `CIP_UNALIGHED_DBC`, `CIP_WRONG_DBS` | DBC means end-of-event; initial value not aligned |
| MOTU | `CIP_WRONG_DBS`, `CIP_SKIP_DBC_ZERO_CHECK`, `CIP_DBC_IS_END_EVENT` | as above |
| OXFW | `CIP_WRONG_DBS`, `CIP_DBC_IS_END_EVENT`, `CIP_DBC_IS_PAYLOAD_QUADLETS` | DBC may count **quadlets**, not data blocks |
| Tascam | `CIP_SKIP_DBC_ZERO_CHECK` | zero-DBC packets must be skipped |

BeBoB is precisely the family `AppleFWAudio` targeted — and even it needs a DBC
exception in Linux. ASFW already records one such decision by hand
(`DbcCounter.cpp:5-8`: *"CIP_DBC_IS_END_EVENT is a device quirk we do not
implement"*) but has no per-device flag mechanism to hang others on.

An FDF gate has its own limit: `CIP_NO_HEADER` (Fireface) means there is no CIP
header to read an FDF from at all.

### 8.10.2 The same reasoning exposes an existing ASFW limitation

ASFW's ZTS gate already requires `result.syt != 0xffff`. Six families set
`CIP_UNAWARE_SYT` — and the flag's meaning is **direction-specific**
(`amdtp-stream.h:37-39`):

> For outgoing packet, the value in SYT field of CIP is 0xffff.
> For incoming packet, the value in SYT field of CIP **is not handled**.

Linux acts on the incoming half by simply declining to read the field
(`amdtp-stream.c:807`: `if (!(s->flags & CIP_UNAWARE_SYT)) *syt = ...`). It never
asserts what is on the wire. `oxfw-stream.c:164-169` explains the flag in terms
of playback timing and NO_INFO compatibility — again a statement about what the
device *honours*, not about what it *sends*.

The families:

| family | source |
|---|---|
| Fireface (RME) | `fireface/amdtp-ff.c:165` |
| Fireworks (Echo) | `fireworks/fireworks_stream.c:32` |
| MOTU | `motu/amdtp-motu.c:440` |
| Digi00x | `digi00x/amdtp-dot.c:393` |
| Tascam | `tascam/amdtp-tascam.c:224` |
| OXFW (conditionally) | `oxfw/oxfw-stream.c:169` |

For these, ASFW's gate reads a field the device does not promise is a clock. The
correct conclusion is that **ASFW cannot assume every incoming SYT is a valid
clock observation** — and depending on what the device actually puts on the wire,
that breaks in one of two ways:

- if SYT really is `0xffff`, the gate is never true: ASFW never publishes an
  RX-derived ZTS boundary and never anchors an RX-clocked timeline;
- if SYT is non-`0xffff` but meaningless, the gate passes and ASFW anchors the
  HAL clock on a number the device never intended as timing. **This is the worse
  failure**, and an earlier revision of this section missed it by asserting the
  first case categorically.

Which occurs is per device and is not decided by the flag. It needs
direction-specific policy and per-device evidence, not a family table.

That is not a bug today — the tested hardware (Duet, DICE) does carry SYT — but
it is an **unflagged quirk assumption already baked into the anchor gate**, and
those families would need the `Transmit` timeline source or a device-clock path,
not merely a profile entry.

**Conclusion.** The gap in §8.10 is real, but the fix is not Apple's
unconditional checks. It is per-device capability flags modelled on Linux
`cip_flags`, living in `DeviceProfiles/`, with the *existing* `syt != 0xffff`
assumption folded in as the first such flag rather than left implicit.


## Still open

Ordered so that the cheap, decisive work comes first. The two measurement items
are the ones that produce evidence; the framework item is the one that would take
weeks and settle nothing about the current bug.

1. **Frame-to-presentation alignment across the epoch** — the discriminator.
   `COREAUDIO_HAL_TIMING_DOMAINS.md` §6(b): periodically re-derive `first` from
   the current RX observation and compare against the running `txNextFrame_`.
   Constant difference means the startup seed; lap-stepped growth means
   mid-epoch execution loss; no difference sends it back to correlator aliasing.
   A startup-only trace cannot separate these, because `PreviewTxRange` stops
   consulting bus/frame alignment after initialisation.
2. **Startup admission** — make the warm-up contract explicit and testable.
   §8.10's box shows `validUpdates_` survives a discontinuity, so a cadence ring
   containing a break is admitted as clean. Decide whether consecutiveness is the
   contract (Apple's is), then test an interrupted warm-up *through the
   consumer*, not just through `RxSytCadence`.
3. **Identifiable loopback markers.** The RTL stimulus repeats at exactly the
   ring period (288 frames), which is what makes lap-counting and correlator
   aliasing indistinguishable. A non-repeating marker separates "delayed payload"
   from "repeated payload" and would retire that ambiguity outright.
4. **A CIP capability-flag mechanism in `DeviceProfiles/`**, modelled on Linux
   `cip_flags`, carrying at minimum: SYT-aware (both directions — see §8.10.2),
   DBC semantics (start vs end event, quadlets vs blocks), empty-packet DBC
   validity, and header presence. Only once that exists can §8.10's FDF/DBC gates
   be added safely. This is device-support breadth, not a fix for the current
   latency question — it should not precede 1-3.

Explicitly **not** next: raising `kPacketsPerCompletionGroup`. §8.3.1 shows it is
fused with the payload-finality deadline and the audio group geometry, so no
legal value yields an interpretable result until those are decoupled.

Legacy `AM824DCLRead`/`AM824DCLWrite` are deliberately not covered: NuDCL is the
live path, and the rate-family builders there are stubs.
