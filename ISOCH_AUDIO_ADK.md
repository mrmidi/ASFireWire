# ISOCH_AUDIO_ADK.md — Wiring the ADK Audio Stack to the FireWire Transport

**Status:** design reference, agreed 2026-06-10. The protocol half is already
validated in `ADKVirtualAudioLab/` (19.5-minute soak: 9.36M packets, 0 verifier
violations, 0 payload races). This document describes the remaining work: connecting
that stack to real OHCI hardware through a clean boundary.

**Read this first if you are new (or coming back after months):** every term is
defined in the [Glossary](#glossary) at the bottom. The document is written so you
can follow it top to bottom without holding the whole design in your head.

---

## 1. The one-paragraph version

The **audio dext** (AudioDriverKit, `ASFWAudioDriver`) will own *everything that
knows what audio is*: float→wire-format conversion, CIP headers, DATA/NO-DATA
cadence, SYT timestamps, the timeline, and talking to CoreAudio. The **core driver**
(`ASFWDriver`) will own *everything that knows what OHCI is*: descriptor rings, MMIO
registers, interrupts, DMA memory. Between them sit **three shared memory regions**
(a payload slab, a metadata ring, a control block) and a handful of **setup-time
methods on the nub**. At steady state, nobody calls anybody: the audio side writes
packets and metadata into shared memory, the core's interrupt handler patches
descriptors from that metadata and wakes the hardware. That's the whole design.

```
┌────────────────────────── one dext process ──────────────────────────┐
│                                                                       │
│  ASFWAudioDriver (ADK)              ASFWDriver (core)                 │
│  ── own dispatch queue ──           ── own dispatch queue ──          │
│                                                                       │
│  CoreAudio HAL ⇄ float ring         OHCI registers, interrupts        │
│  RT WriteEnd: float → slab    ┌──►  refill ISR: patch descriptors     │
│  pump: CIP/SYT/cadence/expose │     completion: timestamps → ctrl blk │
│                               │                                       │
│           ┌───────────────────┴────────────────────┐                  │
│           │   SHARED MEMORY (core-allocated):       │                  │
│           │   1. payload slab   (packet bytes)      │                  │
│           │   2. metadata ring  (per-packet patch)  │                  │
│           │   3. control block  (clock, cursors)    │                  │
│           └─────────────────────────────────────────┘                  │
└───────────────────────────────────────────────────────────────────────┘
```

Both services live in **one process** — "calls" between them are plain C++ calls,
not IPC. The discipline we need is *queue* discipline (each side has its own
`IODispatchQueue`) and *memory* discipline (lock-free structures with explicit
ownership), not serialization.

---

## 2. Why this shape (and not something else)

### 2.1 Why move the protocol into the audio dext?

Because we proved it works there. The lab (`ADKVirtualAudioLab/`) runs the complete
AMDTP TX stack — packetizer, payload writer, SYT timing model, cadence — under a
real CoreAudio HAL with zero hardware, and it is *correct*: byte-exact CIP images,
exact 75/25 DATA/NO-DATA cadence at 48 kHz, wire-parity with a real Saffire.kext
capture (see §8). Keeping the protocol next to CoreAudio also means the core driver
never needs to know what a sample, a channel, or a MIDI slot is.

### 2.2 Why shared memory instead of method calls at runtime?

A packet goes out every 125 µs. Anything per-packet must be lock-free and
allocation-free. Method calls between the two IOServices are cheap (same process)
but they hop dispatch queues — fine at setup, poison at 8 kHz. So:

- **Setup/teardown** (allocate, start, stop): nub methods.
- **Steady state** (every packet, every period): shared memory only.

### 2.3 Why does the metadata ring exist at all? (the history question)

Old drivers (Saffire.kext on IOFireWireFamily) didn't have one — because they didn't
have a boundary. Kext-era drivers described their stream as a **DCL program** (a
linked list of "send this buffer, this size" commands), mutated it in place from
interrupt callbacks, and the FireWire family synchronously recompiled the mutations
into OHCI descriptors. The DCL program *was* their metadata structure; the
synchronous in-kernel call *was* their commit protocol.

We have a boundary (audio dext must not touch OHCI), so the same information flows
through a lock-free ring instead of a function call. Linux has the identical
structure at *its* boundary: FFADO queues packets via the `FW_CDEV_IOC_QUEUE_ISO`
ioctl with a per-packet header/length array, and `firewire-ohci` builds descriptors
from it. Every stack that separates protocol from descriptors grows this ring at
whatever boundary it has.

### 2.4 Why no DCL-like abstraction in the core?

DCLs were a *general-purpose program language* because IOFireWireFamily served
arbitrary unknown clients. Our core has exactly one isoch client with one fixed
stream shape, so the "program" degenerates to a **static circular descriptor ring**
built once at allocation, where the only runtime mutation is copying two quadlets
and a length from a metadata slot. Linux `firewire-ohci` — our authoritative OHCI
reference — has no DCL layer either. Don't build one.

---

## 3. The three shared regions

All three are allocated by the **core** (it owns DMA constraints and IOVA mapping)
and mapped by the **audio dext** via `IOMemoryDescriptor::CreateMapping`, exactly
like the existing `CopyDirectAudioMemory` channel. Layouts live in **one shared
header** compiled into both targets, with `static_assert`s on size and offsets.

### 3.1 Payload slab

The actual packet bytes the hardware will DMA out.

```
numSlots × slotStride bytes
slot i  =  [ CIP header (8 B) | PCM payload (up to maxPacketBytes-8) | pad ]
```

- `slotStride` = `maxPacketBytes` rounded up to a cache line (e.g. 512 B for our
  296 B Saffire packets).
- Written by: **audio dext only** (pump writes CIP + zeroes; RT WriteEnd writes PCM).
- Read by: **hardware only** (the OL descriptor for slot *i* points at slot *i*'s
  physical/IOVA address — programmed once at allocation, never repointed).
- The core CPU never reads or writes payload bytes. Ever.

### 3.2 Metadata ring

One entry per slot, same count as the slab. This is what replaces the DCL program.

```c
struct TxPacketMeta {                  // one cache line, 64 B
    uint32_t immediateHeader[2];       // ready-to-blit OHCI IT header quadlets,
                                       //   pre-encoded by the audio dext
    uint32_t payloadLength;            // bytes: 8 (NO-DATA) or 8+frames*dbs*4
    uint32_t _pad0;
    uint64_t packetIndex;              // absolute index this entry describes
    std::atomic<uint64_t> commitGen;   // seqlock-style: ISR trusts entry only if
                                       //   commitGen == expected generation for
                                       //   this packetIndex
    uint8_t  _pad1[64 - 32];
};
```

- Written by: **audio dext pump** (work queue). Write fields first, then
  release-store `commitGen` last.
- Read by: **core refill ISR**. Acquire-load `commitGen` first; if it doesn't match
  the expected value for the slot it's about to arm → **underrun, fatal** (§6.2).
- The core does not interpret `immediateHeader` — it can't tell DATA from NO-DATA.
  That is the proof the boundary is clean.

### 3.3 Control block (v2 of `AudioTransportControlBlock`)

Small, fixed, both directions. Every field has exactly one writer.

| Field | Writer | Reader | Purpose |
|---|---|---|---|
| `streamGeneration` | core | audio | bumped on bus reset / dead context / teardown; audio side re-negotiates on change |
| `statusWord` | core | audio | running / stopped / underrun / error code |
| `clockPair` (seqlock: `hostTimeMid`, `cycleTimer32`) | core | audio | bus-clock ↔ host-clock affine anchor, from a **bracketed** register read (§5.1) |
| `startAnchor` (`startCycle`, `firstPacketIndex`) | core | audio | written *before* arming cycleMatch start (§5.2) |
| `completionCursor` (`lastRetiredPacketIndex`) | core | audio | how far the hardware has actually transmitted |
| `completionStamps[]` (small ring of `{packetIndex, xferStatusTimestamp}`) | core | audio | per-batch hardware TX timestamps (§5.3) |
| `exposeCursor` (`highestCommittedPacketIndex`) | audio | core | how far metadata is committed (ISR sanity check / telemetry) |

**Iron rule on indices (learned the hard way, commit `0d897ecb`):** every index
crossing the boundary is an **absolute 64-bit monotonic counter** (absolute packet
index, absolute sample time). Each ring has exactly **one owner of its modulus**;
`% size` happens only at the point of access, only by the owner. The
4096-vs-512 wrap-mismatch bug existed because two components applied different
moduli to the same memory — never let that happen again. Sizes (`numSlots`,
`ztsPeriodFrames`) are explicit in the control block and **asserted at startup by
both sides** so a mismatch fails loudly at bring-up, not as a silence mystery.

---

## 4. Who does what, when (the steady-state loop)

Three actors. None of them ever waits on another.

### 4.1 Audio dext pump (work queue, runs every ZTS period — the lab's heartbeat)

For every packet index from `exposeCursor` up to the coverage target
(`completionCursor + leadPackets + margin`):

1. Ask the cadence: DATA or NO-DATA for this cycle?
2. Compute SYT (timing model; see §5.4) and DBC.
3. Write into the **slab**: CIP header; zero the PCM region
   (`clearPayloadBeforeExposure` — this is why underrun on the *HAL* side degrades
   to silence, §6.1).
4. Write into the **metadata ring**: pre-encoded `immediateHeader[2]`,
   `payloadLength`, `packetIndex`; release-store `commitGen`.
5. Advance `exposeCursor`.

Also once per wrap: publish the ZTS anchor to CoreAudio (§5.5).

### 4.2 RT WriteEnd (CoreAudio's real-time thread — the hot path)

Unchanged from the lab, byte for byte the same algorithm:

1. HAL hands a window of float frames at an absolute sample time.
2. Payload writer maps each frame → (packet, frame-in-packet) via the timeline,
   takes a **seqlock snapshot** of the slot (generation + fields before, validate
   after — this killed the M3 wild-pointer crash), and writes sign-extended
   Raw24 PCM directly into the slab.
3. Counters: `framesWritten` / `framesWithoutPacket` / `framesOutsidePacket` /
   `framesRacedReuse`. These are the health gauges; `racedReuse` must stay 0.

Rules for this thread: cached raw pointers only (mapped in `init`, kept until
`free()`), relaxed atomics, no logging, no allocation, never dereference an
`OSSharedPtr`.

### 4.3 Core refill ISR (core queue, every `interruptInterval` packets)

1. For retired descriptors: read `xferStatus` (hardware wrote the actual TX cycle
   into it), push `{packetIndex, timestamp}` into `completionStamps`, advance
   `completionCursor`.
2. Refresh `clockPair` (bracketed cycle-timer read under the seqlock).
3. For each slot to re-arm: acquire-load `commitGen` from the metadata ring.
   - Committed → copy `immediateHeader` into the OMI, set OL `reqCount` =
     `payloadLength`, fix the branch word, continue.
   - Not committed → **stop the context, set `statusWord` = underrun, bump
     `streamGeneration`** (§6.2). Do not invent a packet.
4. `IoBarrier`, wake the context.

The ISR's entire knowledge of AMDTP is "copy 8 bytes and a length". ~Constant time,
no protocol branches.

---

## 5. Timing (the part that has eaten the most weeks — read slowly)

There are **two clocks** and they must never be conflated
(doctrine from the Saffire.kext RE):

- **Bus clock** (24.576 MHz cycle timer, 3072 ticks per 125 µs cycle) → drives
  *wire phase*: which cycle a packet goes out, what SYT it carries.
- **Host clock** (`mach_absolute_time`) → drives *CoreAudio*: ZTS anchors.

The control block carries the bridge between them. Four mechanisms:

### 5.1 Clock pairs — bracketed reads, not interrupt timestamps

To map bus time ↔ host time, the core reads: `hostTime A` → `CYCLE_TIMER` register →
`hostTime B`, publishes `{(A+B)/2, cycleTimer}` under the seqlock. This is the Linux
`CYCLE_TIMER` ioctl trick; jitter is bounded by the bracket width. **Never** use "host
time captured in an ISR" as a precision source — interrupt latency would smear
straight into the audio clock.

### 5.2 Deterministic start — you don't *measure* the start cycle, you *choose* it

OHCI IT contexts support **cycleMatch**: arm the context to begin at a chosen future
cycle. Sequence: prefill lead packets → core writes `startAnchor{startCycle,
firstPacketIndex}` to the control block → arm. Because each subsequent packet
occupies the next cycle, the **entire packet→cycle mapping is pure arithmetic from
that anchor**: `cycle(i) = startCycle + (i − firstPacketIndex)`. The audio dext can
compute SYTs and seed its timeline before the first interrupt ever fires. The first
descriptor's `xferStatus` then merely *verifies* reality matched the plan.

### 5.3 Hardware timestamps — the silicon stamps every packet for free

The IT OUTPUT_LAST descriptor's `xferStatus` word contains the actual cycle the
packet hit the wire, written by hardware at TX time — zero interrupt latency. The
core copies these out in §4.3. Composing 5.1 + 5.3 gives "packet *i* left at host
time *h(i)*" with no ISR jitter anywhere in the chain.

### 5.4 TX SYT — settled empirically, do not relitigate

SYT = **our transmit cycle** (known by arithmetic, §5.2) **plus the transfer-delay
lead** (cycle nibble), with the device's **constant sub-cycle offset grafted on**
(`0x?B0` for the bench Saffire — confirmed on both directions of the real capture).
Two dead ends already explored and rejected: smooth free-running 4096-tick SYT
(offset smears) and slaving to the device's absolute recovered phase (arbitrary
origin → pinned negative lead → `syt=0xFFFF` silence). Lead health target:
device-facing ~1–2.5 cycles (Saffire's own driver holds 6–15 frames; the lab
settles at ~1.06 cycles — inside the band).

### 5.5 ZTS for CoreAudio — anchor at wrap, never extrapolate

The Saffire model, verified by RE and cross-validated against ADK headers:
- One float-ring wrap = one ZTS period (the HAL wraps its writes at the period —
  ring length **must equal** the period, commit `0d897ecb`).
- `UpdateCurrentZeroTimestamp(sampleTime, hostTicks)` is called **only at ring
  wrap**, with a raw periodic anchor derived from completion timestamps + clock
  pairs. The HAL smooths internally (`IOUserAudioClockAlgorithm`) — feeding it
  per-tick extrapolated values was our old bug.

---

## 6. Policies (decided — these are the answers, not open questions)

### 6.1 HAL-side underrun (CoreAudio skips a callback) → **silence, automatically**

Packets are zeroed at exposure (§4.1 step 3). If the RT thread never writes them,
they ship as valid silent DATA packets with correct CIP/DBC/SYT and the MIDI
placeholder intact. No detection logic needed; the lab's gap-scenario test asserts
exactly this behavior.

### 6.2 DMA-side underrun (ISR reaches an uncommitted slot) → **fatal, restart**

The core *cannot* fabricate a filler packet: a correct NO-DATA packet still needs
the right DBC, and the core doesn't know what DBC is. A glitch-through path would
also mask real pacing bugs. Stop the context, flag status, bump generation; the
audio dext tears down and restarts the stream. Apple and FFADO treat TX DMA
starvation the same way. With proper lead this never fires; if telemetry shows it
firing, fix the pacing, don't soften the policy.

### 6.3 Bus reset / dead context → **generation discipline**

Core bumps `streamGeneration` on any event that invalidates the timeline (bus reset
kills channel/bandwidth allocations and the context). Audio dext checks generation
at its natural touchpoints (pump tick, wrap); on mismatch: stop publishing, drain,
re-negotiate via setup methods. This is the systematic version of the
"re-arm on null→nonnull" lesson from the direct-binding bug.

### 6.4 Ring depth vs latency — **depth is not latency**

Audible latency is set by the **lead** (how far ahead of the wire we commit), not
ring capacity. A 256-slot ring with an 8-frame lead has the same latency as a
64-slot ring with an 8-frame lead; extra slots are just recovery headroom for a
late ISR. Sizing: `interruptInterval` = 8–16 packets (1–2 ms), ring ≥ 2 ×
(lead + interrupt batch + margin) → 128–256 slots is comfortable.

### 6.5 Copies — **one transform pass is the floor, and we're on it**

True zero-copy is physically impossible for AMDTP (the HAL writes contiguous float;
the wire needs big-endian Raw24/AM824 with CIP headers spliced in every 8 frames).
The single RT pass float-ring → slab *is* the packetizer, the same single pass
Saffire's clip/`FillFirewireBuffers` did. No intermediate buffers, no second PCM
copy at interrupt time:

> Saffire split push/pull as: clip = push (HAL pace, float→wire into a middle
> buffer); Fill = pull (bus pace, copy middle→isoch + phase PLL).
> We split it as: **PCM = push once, straight into the slab**; everything else
> Saffire did at interrupt time (cadence, phase/SYT, packet arming) is our pump +
> refill ISR. Both roles survive; the second byte-copy doesn't.

### 6.6 Safety offset & reported latency — what to tell CoreAudio (RE'd from Saffire.kext)

Two CoreAudio settings that are easy to confuse but do completely different jobs.
Keep them separate the way Saffire.kext does (RE'd 2026-06-10 from `Saffire.i64`):

- **Safety offset** (`SetOutputSafetyOffset` / input equiv) — how far CoreAudio's
  mix/read head must stay clear of the I/O head. **Affects timing.** Too small →
  glitches. This is the one that matters for correctness.
- **Reported latency** (`SetOutputLatency` / presentation latency) — *metadata* apps
  read for A/V sync. **Does not touch the clock.** Wrong → lip-sync drift, not glitches.

The lab currently sets `SetOutputSafetyOffset(0)`; that is a placeholder, not a
decision. Saffire's real model:

**Frames per packet (the rate ladder — liftable verbatim):**

| Sample rate | frames/packet | SYT_INTERVAL |
|---|---|---|
| 32 / 44.1 / 48 kHz | 8 | 1× |
| 88.2 / 96 kHz | 16 | 2× |
| 176.4 / 192 kHz | 32 | 4× |

**Safety offset** = `delayPackets × framesPerPacket` (sample frames), separate counts
for input and output. The `framesPerPacket` ladder above is fixed and liftable; the
`delayPackets` multiplier is **read from the device's DICE config space**, *not* a
constant in the kext — capture it per device (it lives near the stream-config block
we already decode; see [[dice-notification-decode]]). Buffer size is the same shape:
`framesPerPacket × packetsPerBuffer × bufferCount`.

**Reported latency** = `deviceBase + LADDER (+ hostAddend)`, where:

| Rate | LADDER (frames) | ≈ time |
|---|---|---|
| 1× (48 kHz) | 29 | ~600 µs (≈3.6 isoch cycles) |
| 2× (96 kHz) | 59 | ~600 µs |
| 4× (192 kHz) | 119 | ~600 µs |

The ladder ≈ doubles with rate, i.e. it is a **constant ~600 µs transmit/SYT
presentation term** — liftable directly. `hostAddend` is a runtime-tunable value the
control panel folds in (Saffire's `AddToReportedLatency`, an 8-byte input/output
user-client call) for DSP/plugin latency; we have no equivalent yet and can omit it.

**`delayPackets` is a fixed lookup table, NOT negotiated from the device** (confirmed
2026-06-10 — `Saffire::UpdateIsochBufferParams` @ 0xf506, called only from
`initHardware` and `RestartStreaming`). It is a `latencyMode × rate` table of integer
constants:

| latencyMode | input delayPackets | output delayPackets |
|---|---|---|
| 0 (lowest) | 14 | 2 |
| 1 | 16 | 6 |
| 2 | 18 | 10 |
| 3 (safest) | 20 | 14 |

Then a rate bump adds to **both**: `+2` at 88.2/96 kHz, `+4` at 176.4/192 kHz, `+0`
at ≤48 kHz. (Same function also sets the isoch DMA-program depth = 160/80/40 packets
for ≤48 / ≤96 / ≤192 kHz — i.e. a constant ~20 ms hardware buffer.)

So the output safety offset at 48 kHz, lowest latency mode, is `2 packets ×
8 frames = 16 frames` (~333 µs); safest mode is `20 × 8 = 160 frames` (~3.3 ms).
Because delay is counted in *packets* (125 µs each), the time-domain offset is nearly
rate-independent before the high-rate bump.

**For ASFW:** the entire model is liftable — no per-device capture needed. Lift the
frames/packet ladder, the reported-latency ladder, and this `delayPackets` table;
expose `latencyMode` as our own buffer-size/safe-mode preference. Start at mode 0–1
for TX (output offset 2–6 packets) and widen only if `framesWithoutPacket` climbs
(§6.1). Full RE detail in [[saffire-latency-offset-model]]. Do not bake reported
latency into the ZTS timestamp (§5.5) — Saffire keeps it out of `takeTimeStamp`, and
so must we.

---

## 7. The nub API (additions to `ASFWAudioNub.iig`)

Setup/teardown only — nothing here is called at steady state.

```cpp
// Negotiate + allocate everything for one TX stream. Core builds the static
// descriptor ring, does PrepareForDMA, returns mappable regions.
kern_return_t AllocateTxIsochResources(
    uint32_t numSlots, uint32_t maxPacketBytes, uint32_t interruptInterval,
    IOMemoryDescriptor** outPayloadSlab,
    IOMemoryDescriptor** outMetadataRing,
    IOMemoryDescriptor** outControlBlock);   // or extend the existing control region

kern_return_t FreeTxIsochResources();

// Writes startAnchor to the control block BEFORE arming cycleMatch.
kern_return_t StartTxStream(uint32_t channel, uint32_t speed);
kern_return_t StopTxStream();
```

Clock samples are published by the isoch core through the shared control block.
There is no separate audio-facing raw `CYCLE_TIMER` RPC.

RX mirrors this later (`AllocateRxIsochResources`: core fills an RX metadata ring
with `{actualLength, descriptorTimestamp}` per received packet — which is exactly
what the FW-26 SYT-delta cadence ring wants to eat).

---

## 8. Ground truth from the bench (Saffire Pro 24 DSP, 48 kHz)

Wire capture with Saffire.kext driving, decoded 2026-06-10. These numbers are
**the spec** for this device; the lab reproduces them byte-for-byte.

| Direction | Channel | dbs | Layout | Packet size | PCM encoding |
|---|---|---|---|---|---|
| host→device | 0 | 9 | 8 audio + 1 MIDI | 296 B data / 8 B no-data | raw 24-in-32, **sign-extended** (negatives `0xFFxxxxxx`), no label |
| device→host | 1 | 17 | 16 audio + 1 MIDI | 552 B | AM824, label `0x40` |

- Empty MIDI slot = `0x80000000`, both directions, present even in silence.
- Both directions carry the constant SYT sub-cycle `0x?B0` (the graft, §5.4).
- Blocking mode: 8 frames/data packet, FDF `0x02`, cadence N,D,D,D… (exactly 75%
  data), DBC +8 per data packet, unchanged across NO-DATA.
- **Sign-extension lesson:** positive-only or quiet test material *cannot*
  distinguish zero-padding from sign-extension in the top byte — that's how the
  first RE got it wrong. Capture loud bipolar audio before concluding encoding.
- **Capture-tool lesson:** FireBug often displays raw little-endian memory. Verify
  orientation against known fixed patterns first (CIP q0 must decode; AM824 `0x40`
  / MIDI `0x80` labels must be the *leading* byte). The 2026-06-10 capture was
  logical big-endian.

---

## 9. Bring-up plan (each phase has a gate; don't skip gates)

**Phase 0 — shared header.** `TxPacketMeta`, control block v2, all
`static_assert`s, in one header included by both targets and the lab.
*Gate: both targets compile; lab tests still green.*

**Phase 1 — rehearse the contract in the lab.** Teach the lab's fake slot provider
to act as the "core ISR": consume the metadata ring, enforce commitGen, apply the
underrun-fatal policy, emit fake completion stamps. The verifier and the
`raced_reuse` counter are the regression gate. Costs nothing on the bench,
de-risks the only genuinely new logic.
*Gate: soak run with verifier all-zero and at least one forced underrun proving
the fatal path.*

**Phase 2 — core side, no audio.** Implement `AllocateTxIsochResources` + static
descriptor ring + refill ISR in `ASFWDriver`; drive it from a host-test harness or
a trivial generator producing NO-DATA-shaped metadata. Watch with FireBug.
*Gate: a clean fixed-cadence stream on the wire, cycleMatch start verified against
first descriptor xferStatus.*

**Phase 3 — connect.** Point the ADK pump + payload writer at the real slab and
metadata ring instead of the lab fakes. TX only.
*Gate: device ARX lock solid (EXT_STATUS bits, no ~300 ms flapping), audible clean
audio, `framesWithoutPacket ≈ 0`, `racedReuse = 0`.*

**Phase 4 — timing closure.** ZTS anchors from completion stamps + clock pairs at
wrap; retire the `txOutputOffsetFrames_` stopgap (it papers over a two-clock gap
the reference driver doesn't have).
*Gate: lead telemetry steady in the 1–2.5 cycle band; no drift against the
device's recovered clock over ≥ 30 min.*

**Phase 5 — RX mirror + full duplex.** RX metadata ring feeds the FW-26 SYT-delta
cadence ring; ZTS reference moves to the RX side (the Saffire model anchors on IR).

---

## 10. Pitfalls index (every one of these has already cost a day or more)

1. **Wrap modulus mismatch** — HAL wraps at the ZTS period; one modulus owner per
   ring; absolute indices across boundaries. (commit `0d897ecb`)
2. **RT slot reuse race** — payload writer must take seqlock snapshots; re-reading
   slot fields after validation = wild pointer. (M3 crash, fixed `94297af4`)
3. **Mapping lifetime** — map in `init` before the RT handler exists; keep until
   `free()`; a late WriteEnd may run after StopIO.
4. **`OSSharedPtr` on the RT thread** — never; cached raw pointers only.
5. **Sign extension** — see §8; production `RawPcm24In32Encoder` is correct,
   the lab codec was not until 2026-06-10.
6. **Stale dext after rebuild** — bump `CFBundleVersion` and check for fresh log
   markers before trusting any bench result.
7. **IT descriptor skip address** lives at offset `0x08` (branch word), not `0x04`
   — follow Linux + `ASFWDriver/Isoch/README.md`, not the OHCI 1.1 diagrams.
8. **Barriers** — `IoBarrier`/`OSSynchronizeIO` after descriptor writes, before
   waking the context; read `xferStatus` before acting on completions.
9. **Don't clear DICE NOTIFICATION speculatively** — we don't, FFADO doesn't.
10. **Placement-new ABI clash** — dext TUs pulling libc++ `<new>` transitively must
    `#include <new>` before DriverKit headers.

---

## Glossary

| Term | Meaning |
|---|---|
| **ADK** | AudioDriverKit — Apple's user-space framework for CoreAudio drivers. Our audio dext is one. |
| **AM824** | IEC 61883-6 audio slot format: 1 label byte + 24-bit sample. Label `0x40` = linear audio, `0x80` = empty MIDI. |
| **AMDTP** | The streaming protocol layering CIP + audio slots over isochronous packets. |
| **CIP header** | First 8 bytes of every packet's payload: source ID, **dbs**, **DBC**, format, **SYT**. |
| **cadence** | The fixed DATA/NO-DATA pattern. 48 kHz blocking = 6 data per 8 cycles (75%). |
| **cycle** | The 125 µs bus heartbeat. One isoch packet per channel per cycle. 8000/s. |
| **cycleMatch** | OHCI feature: start an IT context at a chosen future cycle number. |
| **dbs** | Data block size — quadlets per frame on the wire (9 = 8 audio + 1 MIDI for our TX). |
| **DBC** | Data block counter — running frame counter in the CIP; +8 per data packet at 48 kHz blocking. |
| **DCL** | Kext-era "program language" describing isoch streams to IOFireWireFamily. We deliberately have no equivalent. |
| **dext** | DriverKit extension — user-space driver process. |
| **IOVA** | The bus address the DMA engine uses for a buffer (what descriptors point at). |
| **ISR** | Interrupt service routine — here, the core's handler for IT-context interrupts. |
| **IT / IR context** | OHCI isochronous transmit / receive DMA engine instance. |
| **lead** | How far ahead of the wire cycle a packet is committed. Sets latency. Target 1–2.5 cycles. |
| **nub** | `ASFWAudioNub` — the IOService the core publishes and the audio dext matches on; carries the setup API. |
| **OMI / OL** | `OUTPUT_MORE_IMMEDIATE` / `OUTPUT_LAST` — the OHCI descriptor pair per packet: OMI holds the 1394 isoch header inline; OL points at the payload. |
| **pump** | The work-queue loop in the audio dext that exposes future packets (CIP, SYT, metadata). |
| **reported latency** | Presentation-latency *metadata* CoreAudio hands apps for A/V sync. Does **not** affect timing. Saffire = device base + rate ladder (29/59/119 fr). See §6.6. |
| **safety offset** | How far CoreAudio's mix/read head must stay clear of the I/O head. **Affects timing** — too small glitches. Saffire = delayPackets × framesPerPacket. See §6.6. |
| **seqlock** | Lock-free pattern: writer bumps a generation around its writes; reader snapshots, copies, re-checks the generation, retries/aborts on mismatch. |
| **slab** | The shared payload buffer: one fixed-stride slot per in-flight packet. |
| **SYT** | 16-bit presentation timestamp in the CIP: cycle nibble + 12-bit sub-cycle offset (0–3071). |
| **WriteEnd** | The ADK real-time IO callback after the HAL writes a window of float frames. |
| **xferStatus** | Status word hardware writes into a completed descriptor; for IT it contains the actual TX cycle. |
| **ZTS** | Zero timestamp — `UpdateCurrentZeroTimestamp(sample, hostTime)` anchor CoreAudio uses to pace IO. Published once per ring wrap. |

---

*Related material: `ADKVirtualAudioLab/README.md` (lab source of truth),
`ASFWDriver/Isoch/README.md` (descriptor layout), `DICE_DEBUG.md` (TX silence
investigation), Linux `firewire/` and `IOFireWireFamily.kmodproj/` in-tree
references (wire-compat doctrine: spec is the floor, references are the ceiling).*
