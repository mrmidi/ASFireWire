# How Apple's own audio drivers generate zero timestamps

Read against `AppleUSBAudio` (open source, no decompilation), because it is the
same `IOAudioEngine::takeTimeStamp` contract `AppleFWAudio` implements. The
FireWire mapping is **not yet done** — see [Open, blocked](#open-blocked-on-ida).

> **Rule for the FireWire half of this document, when it is written.** Apple's
> FireWire audio stack is expressed in **DCLs** (DMA Command List). A DCL
> statement is worthless on its own: every DCL claim here must be mapped to the
> **OHCI isochronous DMA program and its descriptors** it compiles into —
> which descriptor, which branch, which interrupt bit. If it cannot be mapped
> to descriptors, it does not go in this document.

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

## 7. What this changes for ASFW

| finding | consequence |
|---|---|
| ZTS period 341 ms in Apple's own driver | ASFW's 170 ms is **not** too coarse. Retire that concern. |
| Timestamp interpolated to sub-frame at the wrap point | ASFW takes its boundary from a packet observation without an equivalent intra-batch interpolation. Worth comparing. |
| First timestamp taken at first frame with actual data | ASFW anchors from the first RX observation; whether that is the *first frame that moved data* is unverified. |
| 33-tap FIR on intervals, 4-tap during startup | ASFW has no driver-side timestamp filter. The HAL's is the only one. |
| Input adds one frame of reference-plane correction | ASFW's RX/TX reference planes are not stated in these terms. Relevant to the residual (§4 of the HAL doc). |

None of these is yet a defect claim. They are differences against a working
reference, which is the bar the repo sets for wire/behavioural questions.

## Open, blocked on IDA

The FireWire half needs the disassembly databases and the IDA MCP server was
**not reachable** this session (`ConnectionRefused`, nothing on `127.0.0.1:13337`).

Databases present:

- `/Users/mrmidi/Desktop/FWA_KEXT_CLEAN/AppleFWAudio-update.i64` — BeBoB, OXFW
  and other AV/C devices
- `/Users/mrmidi/Desktop/Saffire.i64` — DICE-based, expected to differ

Questions to answer there, in this order:

1. **Interrupt cadence.** Which isoch DMA descriptors carry the interrupt bit,
   and at what packet interval? Compare against USB's 2 ms in / 64 ms out and
   against the ~20 ms / ~100 ms already recorded in
   `[[apple-fwaudio-isoch-geometry]]`.
2. **Where the timestamp is taken.** Which descriptor completion, and is there
   an interpolation equivalent to §2 — i.e. does it recover a sub-packet
   position, or does it timestamp at packet granularity?
3. **The first timestamp.** Is there an equivalent of the "first frame with
   non-zero `frActCount`" scan?
4. **Filtering.** Is there a driver-side FIR, or does AppleFWAudio hand raw
   values to the HAL?
5. **DICE vs AV/C.** Where `Saffire.i64` diverges from `AppleFWAudio`.

Every DCL-level answer must be carried down to the OHCI descriptors it
generates, per the rule at the top.
