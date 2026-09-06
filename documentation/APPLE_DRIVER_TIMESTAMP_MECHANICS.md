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

**This retires a concern raised earlier in `COREAUDIO_HAL_TIMING_DOMAINS.md` §2.**
ASFW's ZTS period is not coarse by Apple's own precedent — it is twice as fine as
AppleUSBAudio's. What ASFW does *not* yet have is §2's interpolation and §5's
filter.

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

**This puts a real number on "it takes a few time stamps to lock" (M14477): the
driver-side filter alone is 33 timestamps deep**, and the HAL's own filter sits
downstream of it. At ASFW's 170.7 ms period, 33 timestamps is ~5.6 s; at
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

And §3 stands on its own: it retires the "our ZTS period is too coarse" concern
by precedent, regardless of transport.

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
buys is latency-to-service: how soon software reacts to a completed packet.

This is worth weighing against `[[tx-irq-001-interrupt-path-stall]]`, where the
entire OHCI interrupt path was observed to latch up. Running an order of
magnitude more interrupts than the reference stack is not evidence of a cause,
but it is a large unexamined difference from the only implementation known to
have shipped on this hardware. **`kPacketsPerCompletionGroup` is a one-constant
experiment.**

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
§8.3 that buys nothing in timing resolution, because the hardware stamps every
packet regardless. `kPacketsPerCompletionGroup` is a one-constant experiment.

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
That is consistent with DICE devices exposing their own clock and sample-count
registers over the TCAT interface, making the OHCI packet timestamp redundant
for that family. It is a genuinely different clocking strategy, not a variation
in tuning.

| | AppleFWAudio (AV/C) | Saffire (DICE) |
|---|---|---|
| RX callback | every N-th group (20 ms) | **every group** |
| RX per-packet timestamp | yes | **none** |
| clock source | OHCI packet timestamps + SYT | device registers over TCAT |

## Still open

- **`AM824DCLRead`/`AM824DCLWrite` (legacy DCL).** Not examined; the NuDCL path
  is the live one and the rate-family builders there are stubs.
- **Saffire TX.** `PrepareSendDCLs` (`0x10304`) has two `setCallback` and two
  `setTimeStampPtr` sites, so its TX *does* stamp — the placement has not been
  read.
- **Whether ASFW's anchor matches §8.8's discipline.** The one item here that
  changes ASFW code rather than documentation.
