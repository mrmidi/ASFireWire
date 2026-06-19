# ZTS, Buffers, and Interrupt/DCL Cadence — Saffire.kext reference vs ASFW

**Status:** analysis / reference. No code changes.
**Goal:** document how Focusrite's `Saffire.kext` (DICE/TCAT, IOAudioFamily) anchors the
CoreAudio clock and sizes its buffers, how its isoch DCL program is constructed (interrupt
cadence), and how that compares to ASFW's DriverKit/AudioDriverKit pipeline — so we can match
Saffire's behavior first and optimize later.

> **Reading order / state labels**
>
> - Sections 4–5 describe the pre-FW-46/FW-53 broken working-tree state.
> - Section 11 describes the intermediate FW-46 through FW-52 implementation.
> - Section 13 is the current adopted geometry and supersedes the earlier ASFW
>   comparison and playbook values.
> - Historical Saffire reverse-engineering remains reference evidence; it does
>   not override AudioDriverKit's ZTS contract or the current ASFW constants.

Reverse-engineered from `…/OTHER/KEXTs/unpacked/Saffire.kext-extracted/Saffire.i64`
(build `Saffire - 4.1.4.18735`, x86_64, Jun 2014). Addresses are file offsets in `__text`.
Cross-reference: prior memory notes *ZTS clock model (Saffire reference)* and
*TX IT-stack vs Saffire*.

> TL;DR
> - **Saffire has exactly one frame-domain/CoreAudio sample ring.** It also has
>   packet-domain DCL receive buffers, but those never participate in frame-wrap
>   timing. `ReadFirewireBuffers` deinterleaves them into the 15360-frame
>   IOAudioEngine sample ring at 48 kHz.
> - **One timestamp per buffer wrap.** `IOAudioEngine::takeTimeStamp(incr, hostTime)` fires
>   *once* per full ring wrap; CoreAudio interpolates sample position in between from the
>   declared rate. Host time is `clock_get_uptime()` captured once at the group IRQ, then
>   back-interpolated 125 µs/packet. **No per-packet / per-tick extrapolation.**
> - **Interrupt every 12 packets (1.5 ms) at 48k**, 160 groups per DCL traversal.
> - The historical ASFW state differed on all three axes: separate DMA ring +
>   app rings + HAL buffer, a ZTS period that published many anchors per drain,
>   and an 8-packet (1 ms) interrupt cadence.
> - **Historical silence suspect:** that working tree had ZTS/IO period =
>   **48 frames** but
>   the app rings are **512 frames**, and `512 % 48 = 32 ≠ 0`. That re-introduces exactly the
>   period↔ring wrap mismatch that commit `79e45e92` set out to kill (it had aligned both to
>   512). See §5. Section 13 records the replacement geometry.

---

## 1. Saffire RX DCL program — how the DMA program is built

`Saffire::PrepareRecvDCLs(device, stream)` @ `0xFD..` builds the receive DCL program using
`IOFireWireBus` DCL primitives (this is the IOFireWireFamily mechanism we deliberately don't
re-derive — we only document the resulting *shape*):

```
total packets  = numGroups * packetsPerGroup      → stream+320
numGroups      = device[390480]                    → stream+316 (outer loop / group records)
packetsPerGroup= device[390482]                    → inner loop / packets per group
```

Construction (per group `v29` in `0..numGroups`, per packet `i` in `0..packetsPerGroup`):

1. `createDCLPool()` once (`IOFireWireBusAux::createDCLPool`).
2. For each packet: `pool->addReceivePacket(0, header=8, update=1, {bufAddr, size})`
   (vtable +296). Buffers come from one `IOBufferMemoryDescriptor::inTaskWithPhysicalMask`
   allocation masked with the FireWire physical buffer mask (DMA-visible, page-aligned).
3. **Only the last DCL of each group** gets `IOFWDCL::setRefcon(groupRecord)` +
   `IOFWDCL::setCallback(Saffire::RecvGroupCallback)`. → one completion interrupt per group.
4. Groups are chained into a ring: `setBranch(lastDCL, firstDCL)`.

### Geometry values (`Saffire::UpdateIsochBufferParams` @ `0xF506` + `initHardware` @ `0xC1..`)

`numGroups` scales with rate; `packetsPerGroup` is fixed by a latency-mode field
(`device[390478]`, default `1`):

| Sample rate | framesPerPacket¹ | numGroups | packetsPerGroup | total packets | DCL ring depth |
|------------:|:---------------:|:---------:|:---------------:|:-------------:|:--------------:|
| 48 kHz      | 8               | 160       | 12              | 1920          | 240 ms         |
| 96 kHz      | 16              | 80        | 12              | 960           | 120 ms         |
| 192 kHz     | 32              | 40        | 12              | 480           | 60 ms          |

¹ `SamplingRateToSamplesPerFrameMax()` @ `0x14154`: samples per blocking data packet —
8 @ 32/44.1/48k, 16 @ 88.2/96k, 32 @ 176.4/192k (== ASFW `framesPerData`).

**Interrupt cadence @48k = 12 packets = 1.5 ms**, 160 callbacks per full ring traversal.

---

## 2. Saffire clock anchor path

### 2a. `Saffire::RecvGroupCallback(group)` @ `0xF678`
IR DCL group completion (INPUT_LAST interrupt). Minimal:

```c
clock_get_uptime(&t);
absolutetime_to_nanoseconds(t, &hostNs);
group[0] = hostNs;                 // stamp the group's completion host time (ns)
Saffire::ReadFirewireBuffers(dev, group);
groupIndex = (groupIndex+1) % numGroups;   // round-robin
```

Host time is captured **once per group**, at the interrupt, in the IOFireWire workloop.

### 2b. `Saffire::ReadFirewireBuffers(dev, group)` @ `0xCF24` — data mover **and** clock anchor
Loops the group's `packetsPerGroup` (=12) packets. Key behaviors:

- **Per-packet host time (back-interpolation, 125 µs/packet — no extrapolation):**
  ```
  packetHostTimeNs = group[0] - 125000 * (packetsPerGroup - 1);   // walk back to first pkt
  …per packet: packetHostTimeNs += 125000;
  ```
- **PCM write** into the engine sample buffer `v128 = stream+456`, big-endian → host with
  `_byteswap_ulong(x) << 8` (24-in-32). Write index `v122` advances per frame, wraps at
  `v129 = bufferFrames`.
- **Master clock stream only** (`thisStream == clockStream`, selected by
  `dev->clockStreamIndex` @ `dev+1569704`): recovers device SYT, fills a **512-entry SYT-diff
  ring** at `dev+1569728` (index masked `& 0x1FF`), maintains a rolling cadence
  (`dev+1570756/1570760`) and the SYT-recovered `masterClock` (`extendTstamp` →
  `dev+1570780`). This is the device-clock recovery (TX phase is slaved to it — separate from
  the host clock published to CoreAudio).

- **`IOAudioEngine::takeTimeStamp` (vtable +2784) — the host clock anchor:**

  | Call | Args | When | Host time used |
  |------|------|------|----------------|
  | **Seed** | `takeTimeStamp(0 /*incrementLoopCount=false*/, &t)` | once, at lock acquisition | `packetHostTimeNs` of the start packet — establishes sample-0 anchor without bumping the loop count |
  | **Wrap** | `takeTimeStamp(1 /*incrementLoopCount=true*/, &t)` | when the write index wraps (`v122 > v15`), master stream + locked only | `packetHostTimeNs` of the packet that crossed the wrap |

  ⇒ **exactly one timestamp per engine-buffer wrap.** CoreAudio interpolates the running
  sample position between wraps from the declared sample rate; the kext does **not** push a
  timestamp per packet or per group. This is the canonical IOAudioEngine contract.

---

## 3. Saffire CoreAudio buffer sizing

`SaffireAudioEngine::updateAudioEngineSettings()` @ `0x42..`:

```
framesPerPacket = SamplesPerFrameMax(rate)                       // engine[73] = 8 @48k
bufferFrames    = framesPerPacket * numGroups * packetsPerGroup  // engine[72]
inputOffset     = framesPerPacket * inputDelayPackets            // engine[74]  (safety offset)
outputOffset    = framesPerPacket * outputDelayPackets           // engine[75]  (safety offset)
```

At 48k: `bufferFrames = 8 × 160 × 12 = 15360 frames (≈320 ms)`.

**Saffire RX uses exactly two buffers — and only one of them is a frame-domain ring:**

1. **DCL receive packet buffers** (`stream+288`, the `IOBufferMemoryDescriptor` from
   `PrepareRecvDCLs`, FW-physical-mask, ~1920 slots of raw isoch packet = CIP header + AM824).
   This is a **packet-domain** buffer; it does not participate in frame-wrap math.
2. **IOAudioEngine sample buffer** (`stream+456`), deinterleaved PCM, **sized = DCL depth in
   frames** = `framesPerPacket × numGroups × packetsPerGroup` = `8 × 160 × 12 = 15360` @48k.

`ReadFirewireBuffers` **deinterleaves** buffer 1 → buffer 2 in one hop
(`_byteswap_ulong(x) << 8`, 24-in-32). So it is *not* zero-copy — there is one copy/deinterleave
step. The decisive point is that **there is exactly one frame-domain ring** (the engine sample
buffer), and it is simultaneously the deinterleave *target*, the CoreAudio read *source*, **and**
the `takeTimeStamp` wrap *reference*. There is **no intermediate app-level PCM ring** between the
wire and the HAL.

Latency is **not** the buffer size; it is governed by the safety offsets (`engine[74]/[75]`)
plus the separately-reported presentation latency (`updateSampleLatencies` @ `0x284A`: base +
rate ladder +29/+59/+119 frames @1x/2x/4x). See memory note *Saffire latency/offset model*.

Because the only frame-domain ring has a single producer (the deinterleave write) and a single
consumer-window (CoreAudio erase head + safety offset), and the timestamp anchor is taken on
*that* ring's wrap, **a buffer-geometry wrap mismatch is structurally impossible** — there are no
two *frame* rings whose wrap points can disagree.

---

## 4. Historical ASFW Model Before FW-53

> This section is a snapshot of the broken working tree used during the
> comparison. It is not the current implementation; Section 13 supersedes its
> ASFW values.

### 4a. IR DMA ring — `IsochRxDmaRing` / `IsochReceiveContext`
- `kNumDescriptors = 512`, one `INPUT_LAST` descriptor per packet, `kMaxPacketSize = 4096`.
- Interrupt: `kIntAlways` on every 8th descriptor
  (`IsochEventGroup.hpp::IsTimingGroupBoundary`, `index % 8 == 7`,
  `TimingGroupPacketCount48k() == 8`). → **interrupt every 8 packets = 1 ms @48k**,
  64 interrupts per ring lap. Ring depth in time = 512 × 125 µs = **64 ms**.

### 4b. Clock anchor — `IsochReceiveContext::DrainCompleted` (`IsochReceiveContext.cpp`)
- `drainHostTicks = mach_absolute_time()` captured **once per drain**.
- Per packet: back-interpolate `packetHostTicks = drainHostTicks − cycleHostTicks ×
  ((8−1) − posInGroup)` (125 µs/packet — same idea as Saffire).
- SYT cadence observed into `rxSytCadence` (512-entry ring, `kReadDelay = 256` — mirrors
  Saffire's 512-entry SYT-diff ring and 256-entry read lag).
- After draining the group, **once**, gated on `sytConsecutiveValid_ ≥ 16`:
  `clockPublisher_.Publish(absoluteFrameCursor_, drainHostTicks, nanosPerSampleQ8)` →
  updates the authoritative ZTS (`RxClock`).

### 4c. ZTS → HAL — `ASFWAudioDriverZts.cpp::PublishSharedZeroTimestampToHAL`
- Steps in `P = TimingCursorPolicy::HalZeroTimestampPeriodFrames()` increments, and for every
  `P`-boundary crossed since the last event frame calls
  `audioDevice->UpdateCurrentZeroTimestamp(nextFrame, targetHostTicks)` (`targetHostTicks`
  linearly interpolated from `eventFrame/eventHostTicks/nanosPerSampleQ8`).
- ⇒ **multiple `UpdateCurrentZeroTimestamp` calls per drain** (one per `P` boundary), *not*
  one per buffer wrap. The HAL's `IOUserAudioClockAlgorithm` smooths.

### 4d. Buffers — `Audio/Config/AudioConstants.hpp` (historical snapshot)
- `kAudioRingBufferFrames = 512` (input app ring), `kAudioOutputRingFrames = 512` (playback).
- `kAudioIoPeriodFrames = 48`; `HalIoPeriodFrames() == HalZeroTimestampPeriodFrames() == 48`.
- `kOutputConsumerLeadFrames = 384`, `kOutputCursorResyncDeadbandFrames = 64`.
- These app rings are **distinct** from the IR DMA descriptor ring (512 × 4096 B) **and** from
  the HAL's IO buffer. Three independent ring boundaries plus a ZTS period.

---

## 5. Historical Side-by-Side and Silence Diagnosis

| Axis | Saffire.kext | ASFW (historical snapshot) |
|------|--------------|---------------------|
| Framework | IOAudioFamily (kext) | AudioDriverKit (dext) |
| RX DMA program | DCL pool, 1920 pkts (160 grp × 12) @48k | OHCI INPUT_LAST ring, 512 descriptors |
| Interrupt cadence | every **12** packets (1.5 ms), 160/lap | every **8** packets (1 ms), 64/lap |
| DMA ring depth | 240 ms @48k | 64 ms @48k |
| CoreAudio buffer | **15360 frames, == DCL depth, DMA'd directly** | 512-frame app rings, **separate** from DMA ring |
| Clock primitive | `takeTimeStamp(incr, hostTime)` | `UpdateCurrentZeroTimestamp(sampleFrame, hostTicks)` |
| Timestamp cadence | **once per buffer wrap (~320 ms)** | once per ZTS period (**48 frames, ~1 ms**) |
| Host-time source | `clock_get_uptime` at group IRQ | `mach_absolute_time` at drain |
| Back-interpolation | 125 µs/packet, no extrapolation | 125 µs/packet, no extrapolation |
| Interp between anchors | CoreAudio (declared rate) | HAL `IOUserAudioClockAlgorithm` |

### The structural difference
Saffire's correctness comes from having **exactly one frame-domain ring**: the wire-packet buffer
is deinterleaved once into the CoreAudio sample buffer, `takeTimeStamp` marks *that* ring's wrap,
and the safety offset defines the read window. The wire buffer is packet-domain and never enters
the frame-wrap math, so there is no second frame ring to fall out of phase.

ASFW splits the *frame* path into ≥2 frame-domain rings (app PCM ring + HAL buffer) on top of the
wire-packet ring + a ZTS period. Correctness then *requires* those boundaries to stay
phase-aligned. Commit `79e45e92` ("Synchronize ring buffer capacity with 512-frame ZTS
period") fixed the earlier **85/15 % silence** exactly this way: it shrank the app rings
`4096 → 512` so they equalled the then-current 512-frame ZTS period — a clean 1:1 wrap.

### Current-silence suspect (actionable)
That working-tree snapshot (`AudioConstants.hpp` and
`TimingCursorPolicy.hpp` were both modified in `git status`) had moved the
IO/ZTS period to **48 frames** while leaving the app rings at **512**:

- `512 % 48 = 32 ≠ 0` — the ZTS period no longer divides the ring. Anchor frames and ring wrap
  drift 32 frames out of phase every lap → the consumer reads not-yet-written / silent regions:
  **the same wrap-mismatch class `79e45e92` was meant to eliminate.**
- The invariant that guarded this — `static_assert((kAudioOutputRingFrames %
  kAudioIoPeriodFrames) == 0, "Output ring must be an integer number of IO periods")` — was
  present at the `79e45e92` revision and was **absent** from that snapshot.
  Restoring it would have rejected the invalid `512 % 48` configuration.

### Historical Proposed Remedy

> This was the immediate pre-ADK-contract proposal. Sections 9 and 13
> supersede its equality and wrap-publication recommendations.

1. **Re-establish one period for everything.** Pick a single ZTS/IO period `P` and make every
   ring an exact integer multiple of it (Saffire's `P` *is* the buffer; the simplest match is
   `P == ring`, i.e. the `79e45e92` 512/512 state). Restore the
   `ring % period == 0` static_assert as the guard.
2. **Anchor at wrap, not per period.** Saffire publishes one timestamp per full wrap and lets
   the HAL/CoreAudio interpolate. The mirror-pump's per-`P`-boundary publication is acceptable
   only while `P` evenly divides the ring and the anchors stay monotonic; if matching Saffire
   exactly, publish once per ring wrap with the back-interpolated wrap host-time.
3. **Interrupt cadence is a minor knob.** Saffire = 12 pkts/1.5 ms, ASFW = 8 pkts/1 ms — both
   fine. Unify `TimingGroupPacketCount48k()` (RX) with the TX `kTimingGroupPackets` per the
   `IsochEventGroup.hpp` TODO; it does **not** drive the silence.

---

## 7. IT (transmit) side — how Saffire builds and drives the output stream

The transmit path mirrors the receive path's *structure* but inverts the data flow and adds an
output-phase PLL. Same group geometry, same 1.5 ms callback cadence; the interesting part is how
the SYT and the data/no-data decision are produced.

### 7a. `Saffire::PrepareSendDCLs(device, stream)` @ `0x10304` — TX DCL program
Same geometry source as RX: `numGroups = device[390479]` (160 @48k),
`packetsPerGroup = device[390481]` (12 @48k), total 1920 packet slots. **But every slot is built
as a pair of DCLs:**

- a **DATA** send DCL, buffer size `v59 = 4·samples + 8` (CIP header 8 B + AM824 payload), and
- a **NO-DATA** send DCL, buffer size **8** (CIP header only — the empty/NODATA packet).

Both are created with `pool->addSendPacket(0, 1, &range)` (vtable +304), and **both get the same
`IOFWDCL::setTimeStampPtr(timeStampPtr)` + `setFlags(6)`.** `timeStampPtr` is a per-slot `UInt32`
that **the OHCI hardware writes with the actual cycle time the packet was transmitted** — this is
the feedback the fill routine reads back next round. The completion callback
(`SendGroupCallback`) is attached to the **last DATA and last NO-DATA DCL of each group**, so
again **one interrupt per group = every 12 packets = 1.5 ms** @48k.

A final fixup pass wires the branches so each slot's data-DCL *and* no-data-DCL both point at the
next slot; at runtime the fill routine swings each slot's incoming branch to either the data or
the no-data variant. ⇒ a **dual-DCL-per-slot ring** where data-vs-empty is chosen per cycle by
re-pointing branches, not by rewriting descriptors.

### 7b. `Saffire::SendGroupCallback(group)` @ `0xF782` — OUTPUT_LAST interrupt
- `clock_get_uptime()` → host time stamped into the group record.
- → `FillFirewireBuffers(dev, group)` to fill *this* group's outgoing packets with the next
  round of data (TX is **fill-on-completion**, running one group behind the DMA read head).
- Advances the group index round-robin; the rest is jitter telemetry (proc/notify/interval
  min·avg·max, dumped every 1001 callbacks) — diagnostic only.

### 7c. `Saffire::FillFirewireBuffers(dev, group)` @ `0xE778` — packetizer + SYT generator
Per packet in the group (this is the load-bearing logic for our IT bring-up):

1. **Read back the real send time.** `tstampToOffsets(*timeStampPtr)` of the prior packet →
   `curPhase` (device transmit phase, in 24.576 MHz ticks, domain `0xBB80000` = 4096·48000 =
   8 s). The TX timeline is *re-derived from hardware every callback* — there is no
   free-running host timeline.
2. **Continuity guard.** Predicted next phase = `curPhase + 3072` (one isoch cycle = 3072 ticks
   = 6 frames @48k), stored at `stream+376`. A miss of exactly 3072 within 2 callbacks is
   tolerated (single-cycle glitch); anything else triggers a dropout reset that force-resyncs
   every stream.
3. **Output phase.** Seed (when fresh, `stream+380 == -1`) = `curPhase + 3072` (lead the device
   by one cycle). Then `adjustOutputPhase()` (§7d) slews it.
4. **Graft the RX-recovered cadence.** `stream+406` is a **512-entry index** (`& 0x1FF`) seeded
   at the **midpoint 256** (`(rxWriteIdx + 256) & 0x1FF`). The applied SYT phase =
   `adjPhase + SYTdiff[idx]`, reading the device-recovered SYT-diff ring at
   `dev + 2·idx + 1569728` — i.e. the outgoing cadence is the RX-recovered cadence **delayed by
   256 entries.** This is exactly ASFW's `rxSytCadence` with `kReadDelay = 256`.
5. **Data vs NO-DATA (lead gate).** `lead = extOffsetDiff(adjPhase, curPhase)`:
   - `lead < 7620` ticks (~2.48 cyc / ~15 frames) → **ship DATA** (`lead ≤ 3071`, under one
     cycle, logs a "too tight" warning but still ships, negatives included).
   - `lead ≥ 7620` → **NO-DATA** packet: `phase = -1`, `SYT = 0xFFFF`. (`> 12287` only escalates
     a log line; it is *not* a separate behavioral boundary.)
   - ⇒ device-facing TX lead is **bounded to ~1–2.5 isoch cycles.**
6. **SYT encode.** `SYT = (phase % 3072) | ((phase / 3072) << 12)` — sub-cycle offset in the low
   12 bits, cycle count in bits [15:12] — byte-swapped into CIP header bytes `+6..7`. CIP header
   bytes `+0..5` (SID/DBS/FMT/FDF…) are carried forward from `stream+362..367`; DBC continuity is
   advanced by samples-per-packet.
7. **PCM pull.** Source samples come from the client playback buffer at cursor `stream+352`,
   indexed to the computed phase; NO-DATA payloads are `memset` to zero. The chosen slot branch
   is swung to the data or no-data DCL accordingly.

### 7d. `Saffire::adjustOutputPhase(dev, stream, curPhase, seedPhase)` @ `0xC9C2` — the PLL
A **deadbanded hold-and-nudge** slew, *not* a per-packet re-anchor. It compares the proposed
output phase against the recovered device master clock (`dev+1570780`), computes a correction
scaled by the rolling cadence (`dev+1570756/1570760`), and:
- if `|correction| ≤ deadband` (`stream+392`) → **return the phase unchanged** (hold), only
  updating min/max trackers;
- else → apply the correction (`(seedPhase + corr) mod domain`) and clear the "fresh" flag.

This is the TX analogue of "anchor at wrap, don't re-extrapolate every tick": Saffire holds the
output phase steady and nudges only when it drifts outside the band.

### 7e. What this means for ASFW's IT bring-up

| Aspect | Saffire | Implication for us |
|--------|---------|--------------------|
| TX DCL shape | dual DCL per slot (data + no-data), branch-swung per cycle | our IT ring must be able to emit a true NO-DATA (CIP-only, SYT=0xFFFF) packet per cycle, selected at fill time — not just a silent data packet |
| Fill trigger | OUTPUT_LAST interrupt, one group behind DMA | fill-on-completion, 12-packet groups; **prefill NO-DATA before RUN** (see memory *Saffire TX prefill before RUN*) so the first interrupt never meets an uncommitted slot |
| TX clock | re-derived **every callback** from HW-reported send timestamp (`setTimeStampPtr`) + device master clock + 1 cycle | the OHCI must write back actual transmit cycle time; phase is closed-loop against the device, **no free-running host timeline** |
| Output lead | bounded ~1–2.5 cycles; beyond → NO-DATA | the historical `txOutputOffsetFrames_ = 5120` stopgap has been removed; current TX phase is recovered from hardware completion feedback and RX cadence |
| SYT source | `adjPhase + RX-SYTdiff[idx−256]`, 512-entry ring seeded at 256 | identical to our `rxSytCadence` (`kReadDelay = 256`); the TX SYT is the RX-recovered device cadence, **not** a host-derived value (memory *TX SYT generation requirements*) |
| PLL | deadbanded hold-and-nudge vs device master | don't re-anchor TX phase every packet; hold and correct only outside a deadband |

Net: **Saffire's IT is a closed loop slaved to the RX-recovered device clock** — hardware
timestamp feedback → output phase (lead 1 cycle) → deadband PLL → SYT = device cadence delayed
256 entries → data/no-data gate. The host PCM is merely *pulled* at the phase-matched cursor; the
host clock (`takeTimeStamp`) drives **CoreAudio presentation only**, never the wire timing.

---

## 8. Playbook — matching Saffire on ASFW (OHCI isoch DMA, no DCL)

This is the practical part. It is a lot of moving parts spanning two subsystems (CoreAudio HAL
and FireWire/OHCI), heavy timing work, and — critically — **we do not run IOFireWireFamily's DCL
engine.** Saffire's timing emerges from DCL primitives (`createDCLPool`, `addReceivePacket`,
`setTimeStampPtr`, `setCallback`, `setBranch`); ASFW programs **OHCI IR/IT descriptor rings
directly** (`IsochRxDmaRing` / `IsochTxDmaRing`). The *mechanism* differs; the *observable bus
and clock behavior* is reproducible. This section says how, and — per the project's wire-compat
doctrine — the bar is "behaves like the reference," not "uses the same machinery."

> **The single biggest trap:** letting CoreAudio and FireWire each grow their *own* timeline.
> Saffire keeps exactly **two clock domains, each single-sourced**, and derives everything else.
> The historical ZTS authority arbitration, mirror-pump period mismatch,
> free-running TX timeline, and 5120-frame offset were symptoms of
> **double/triple timing paths**. The adopted model collapses them to the two
> domains below.

### 8.1 The two-domain model — and nothing more

| Domain | Single source | Drives | Must NOT touch |
|--------|---------------|--------|----------------|
| **A — Host time** (CPU uptime) | RX-interrupt-derived anchors on the declared ADK ZTS grid; nominally one 192-frame anchor per 32-packet group | CoreAudio presentation / sample-rate tracking (`UpdateCurrentZeroTimestamp`) | the wire / SYT |
| **B — Device SYT cadence** (FireWire cycle phase) | the **RX-recovered SYT-diff ring** (512 entries, read 256-delayed) | TX packet phase + SYT field | CoreAudio sample position |

In Saffire these never cross-drive: `takeTimeStamp` uses `clock_get_uptime` (Domain A); the TX
phase uses the recovered `masterClock` (Domain B). **Mirror that separation exactly.** Concretely
for us:

- Kill every *intermediate* timeline. No per-tick re-extrapolation between anchors (the HAL's
  `IOUserAudioClockAlgorithm` already smooths — feed it raw periodic anchors).
- The removed `ZtsAuthority` `RxClock / TxClock / DuplexAggregate` selection
  must not return. There is **one** host anchor source (RX drain) and **one**
  device cadence source (RX SYT). TX consumes Domain B; it is not its own clock
  authority.
- The ZTS publisher must publish on the declared ADK grid. Ring wrap is a
  divisibility boundary, not the publication trigger.

### 8.2 DCL → OHCI descriptor mapping (what we build instead)

| Saffire DCL concept | ASFW OHCI equivalent | Notes / risk |
|---------------------|----------------------|--------------|
| `addReceivePacket(hdr=8, update=1)` | `INPUT_LAST` descriptor w/ status writeback, one per packet | `IsochRxDmaRing` already does this |
| `setCallback` on last DCL of group | `kIntAlways` on every Nth descriptor (`IsTimingGroupBoundary`) | current **N=32** for both RX and TX; Saffire's N=12 remains reference evidence |
| group = N packets, 1 callback/group | interrupt every N packets | ASFW chooses 32 to contain eight complete D/D/D/N blocks and a nominal 192 frames |
| DCL ring length (1920 pkt @48k) | IR descriptor ring depth (ours 512) | need **not** match Saffire; must be an integer number of interrupt groups |
| **dual DCL per slot** (data + no-data, branch-swung) | **single IT descriptor block whose CIP header + payload length we rewrite per refill** | we swing *payload*, not branches: emit a data-CIP **or** a NO-DATA-CIP (`SYT=0xFFFF`, CIP-only) per cycle |
| `setTimeStampPtr` (HW writes actual send cycle) | OHCI `OUTPUT_LAST` **`timeStamp`** writeback (`statusWord & 0xFFFF`) | **RESOLVED — spec-guaranteed *and* already implemented.** OHCI 1.1 §9.1.4 (Table 9-3): with the status-control (`s`) bit set, HW writes `xferStatus`+`timeStamp` = `cycleSeconds[2:0]:cycleCount[12:0]` = "the cycle for which the IT DMA controller queued the transmission of this packet." We set that bit (`IsochTxDmaRing.cpp:184`) and decode it in `ResyncCycleTracking` (`(hwTimestamp & 0x1FFF) % 8000`, `(hwTimestamp>>13)&7`). No `CYCLE_TIMER` fallback needed. |
| `IOBufferMemoryDescriptor` (FW phys mask) | `IsochDMAMemory` slabs | already DMA-visible |
| DCL engine handles skip/branch/coherency | **us:** skip-addr at `0x08`, status-writeback order, `OSSynchronizeIO`/`IoBarrier` | CLAUDE.md gotchas — these were free for Saffire, they're on us |

### 8.3 Buffer setup — sizing both sides (OHCI DMA + CoreAudio)

The timing rules rest on this. There are **three buffer roles**; Saffire collapses the *frame*
side to **one ring**, and so should we.

| Role | Saffire | ASFW adopted | Domain |
|------|---------|----------|--------|
| Wire DMA buffer | DCL packet buffers (1920 slots, raw CIP+AM824) | IR/IT descriptor ring (512 × 4096 B) | **packet** |
| Frame ring (HAL-facing PCM) | IOAudioEngine sample buffer (15360 fr) | directly mapped 1536-frame HAL ring per direction | **frame** |
| Hop wire→frame | one deinterleave copy | wire → directly mapped HAL ring | — |

**Rules to match Saffire:**

1. **One frame-domain ring.** Deinterleave directly from the OHCI packet payload into the
   HAL-facing PCM buffer: `OHCI packet → (CIP strip + AM824→PCM deinterleave) → HAL PCM ring at
   the phase-matched cursor`. Kill the intermediate app PCM ring (or make the app ring *be* the
   HAL buffer). The wire ring stays packet-domain and never enters frame-wrap math.
2. **Size the wire DMA ring independently** of the frame ring: an integer number of interrupt
   groups, deep enough to cover interrupt latency. Saffire's 1920 pkt (240 ms) is generous; our
   512 pkt (64 ms) is fine. It need **not** equal the frame ring in count.
3. **Size the frame ring as an integer multiple of every frame-domain
   boundary.** The adopted invariants are:
   ```text
   frameRing % ztsPeriod == 0
   frameRing % maxIO == 0
   frameRing % timingGroupFrames == 0
   frameRing % frameAlignment == 0
   ```
   Equality is not required. The adopted values are a 1536-frame ring,
   192-frame ZTS/group advance, 512-frame maximum client IO, and 32-frame
   alignment.
4. **Publish on the ADK ZTS grid, not on either ring wrap.** Saffire's
   `takeTimeStamp` wrap rule is an IOAudioFamily contract. ASFW's
   `UpdateCurrentZeroTimestamp` publishes fixed-period grid points derived
   from RX packet timestamps.

**Cheat-sheet @48k:**

| Quantity | Saffire | ASFW target |
|----------|---------|-------------|
| framesPerPacket | 8 | 8 |
| packetsPerGroup (= interrupt cadence) | 12 | **32** |
| framesPerGroup | 96 | **192 nominal** |
| wire DMA ring | 1920 pkt / 240 ms | 512 pkt / 64 ms (**independent**) |
| **frame ring (PCM)** | **15360 fr** | **1536 fr**, divisible by ZTS/group advance 192 and max IO 512 |
| timestamp cadence | per 15360-fr wrap | nominally one 192-frame grid anchor per group |
| safety offset | framesPerPacket × delayPkts | profile output 48; input floor 256, raised for client geometry |

The must-not-break relationship is divisibility across ZTS, maximum IO,
timing-group advance, and alignment. Section 13 contains the authoritative
constants and assertions.

### 8.4 RX recipe (Domain A anchor)

1. Decode each packet's OHCI receive `timeStamp` prefix and expand it against
   the drain-entry cycle-timer/host-time pair. Do not infer packet age solely
   from its position inside the drained group.
2. **Publish the CoreAudio anchor on the declared ZTS-period grid from the IR interrupt** — see **§9 (authoritative)** for the exact ADK contract. ADK is *not* "once per buffer wrap" like Saffire's IOAudioEngine. In the adopted geometry, the nominal case is one 192-frame anchor per 32-packet interrupt group.
3. Keep the 1536-frame ring divisible by the 192-frame ZTS/group advance,
   512-frame maximum client IO, and 32-frame alignment. Do not collapse these
   distinct quantities back into one value.
4. Feed raw periodic anchors; never re-extrapolate per tick.

### 8.5 TX recipe (Domain B closed loop)

1. Read the **actual transmit cycle** from the `OUTPUT_LAST` `timeStamp` writeback (OHCI 1.1
   §9.1.4, `statusWord & 0xFFFF`) — **already implemented** in `ResyncCycleTracking`. This is what
   closes the loop off real hardware send-time instead of a host projection; no `CYCLE_TIMER`
   back-interpolation is needed.
2. Per refill: `curPhase` (from HW feedback) → lead one isoch cycle (`+3072` ticks) → deadband
   PLL against the RX-recovered cadence → `SYT = adjPhase + SYTdiff[idx−256]`, encoded
   `(phase % 3072) | ((phase / 3072) << 12)`.
3. Lead gate: ship **data** if lead `< ~7620` ticks (~2.5 cyc), else **NO-DATA** (`SYT=0xFFFF`).
4. **Prefill NO-DATA before RUN**; fill-on-completion, one group behind the DMA head.
5. Keep the removed free-running host TX timeline and
   `txOutputOffsetFrames_ = 5120` path deleted. Reintroducing either would
   restore the double timing path.

### 8.6 Boundary checklist (CoreAudio ↔ FireWire)

- [ ] **One host-time authority:** RX-interrupt-derived anchors on the declared
      ADK ZTS grid. Nominally one 192-frame anchor per 32-packet group.
- [ ] **One** device cadence ring, RX-sourced, 256-delayed (Domain B) → wire only.
- [ ] Frame ring divisible by ZTS period, maximum client IO, nominal timing
      group advance, and alignment; RX and TX interrupt cadence equal.
- [ ] No third timeline, no cross-domain driving, no per-tick re-extrapolation.
- [ ] Small safety offset (~8 frames) governs latency; reported/presentation latency is separate
      metadata (do not bake it into the anchor — Saffire keeps `updateSampleLatencies` out of
      `takeTimeStamp`).
- [ ] TX SYT comes from the RX cadence, never from a host-derived value.

### 8.7 Honest caveats (why it's work, but tractable)

- **It is achievable with isoch DMA programs** — the DCL engine is a convenience layer over the
  same OHCI descriptors Saffire's hardware ultimately executes. Nothing in §8.1–8.5 needs DCL.
- TX timestamp feedback is **no longer a risk**: OHCI 1.1 §9.1.4 guarantees per-packet
  `timeStamp` writeback on `OUTPUT_LAST` (status bit set), and the current path
  extracts it for completion-driven phase recovery. Hardware verification of
  lead and relock behavior remains more important than obtaining the stamp.
- **We own the OHCI plumbing** Saffire got for free: descriptor skip/branch layout, status
  writeback ordering, DMA coherency barriers. Bugs here look like timing bugs.
- **Single-queue confinement helps:** RX drain and TX fill run on the one "Default" dispatch
  queue, so the shared cadence ring needs no locks (see memory *Single Default-queue
  concurrency*).
- Cross-references: memory *ZTS clock model (Saffire reference)*, *TX SYT generation
  requirements*, *Saffire output lead is bounded*, *Saffire TX prefill before RUN*,
  *TX silence RESOLVED; residual underruns*.

---

## 9. ZTS publish cadence — the AudioDriverKit contract (authoritative)

The previous sections describe Saffire, which is an **IOAudioFamily (kext)** driver. ASFW is an
**AudioDriverKit (dext)**, and ADK's zero-timestamp contract is *different* from IOAudioEngine's
`takeTimeStamp`. This section is the authoritative answer to "must the HW interrupt cadence match
the ZTS publish period?" — sourced from the ADK headers in `tmp/AudioDriverKit/`. It **refines
§8.4**: the "anchor once per buffer wrap" advice there is the IOAudioEngine model and does *not*
map cleanly onto ADK.

### 9.1 What the ADK headers actually say

- **`zero_timestamp_period`** (declared at `IOUserAudioDevice::Create`, also `SetZeroTimeStampPeriod`)
  — *"the number of sample frames the host can expect between successive time stamps… if
  GetZeroTimeStamp() returned a sample time of X, the host can expect that the next valid time
  stamp will be **X plus the value of this property**."* (`IOUserAudioDevice.h:87`)
- **`UpdateCurrentZeroTimestamp(in_sample_time, in_host_time)`** — *"Updating the current timestamp
  **should use the time passed in the hardware interrupt**."* (`IOUserAudioClockDevice.h:841`)
- **`IOUserAudioDriver`** — *"The host drives its timing using the timestamps provided by
  `UpdateCurrentZeroTimestamp()`/`GetCurrentZeroTimestamp()`. The series of timestamps provides a
  **mapping between the device's sample time and `mach_absolute_time()`**."* (`IOUserAudioDriver.h:79`)
- **`IOUserAudioClockAlgorithm`** = `Raw | SimpleIIR | TwelvePtMovingWindowAverage` (default
  **SimpleIIR**) — *"the Host applies a … filter to the time stamp stream."* (`AudioDriverKitTypes.h:415`)

So ADK is **not** "once per wrap." It is a **fixed declared period** the host *predicts against*
(`next = last + period`), fed a stream of `(sampleTime, hostTime)` anchors **from the hardware
interrupt**, which the host then **smooths** (IIR by default).

### 9.2 The three periods — keep them distinct

| Period | Our symbol | Meaning | Hard constraint |
|--------|------------|---------|-----------------|
| **ZTS period** | `HalZeroTimestampPeriodFrames` / `Create(in_zero_timestamp_period)` | sample-frame spacing the host predicts between anchors | published sample times must land on this grid; **`ring % ztsPeriod == 0`** |
| **HAL IO period** | `kAudioIoPeriodFrames` | how the HAL chunks ring read/write | **`ring % ioPeriod == 0`** (the 512%48 silence, §5) |
| **HW interrupt** | `IsTimingGroupBoundary` (every N packets) | when *our* code runs to publish | must be frequent enough to service the ZTS grid |

That historical snapshot set **all the ZTS/IO knobs to one value** (`kAudioIoPeriodFrames = 48`, passed to
`Create` *and* `SetZeroTimeStampPeriod`, *and* used as the mirror-pump step). The ring is 512.
`512 % 48 = 32 ≠ 0` violates **both** ring constraints at once — that is the §5 silence stated in
ADK terms: **the ring must be an integer number of ZTS periods.**

### 9.3 The answer to the dilemma

**The HW interrupt and the ZTS period do not have to be equal — but the interrupt is the publish
site, and the cleanest correct design makes them equal.** Precisely:

1. The host predicts `next_ts = last_ts + ztsPeriod`, so every published `in_sample_time` **must
   sit exactly on the ZTS grid** (`X, X+P, X+2P …`, monotonic). You cannot publish arbitrary
   cursor values.
2. ADK says publish **from the hardware interrupt** using the interrupt's captured time. The
   interrupt is therefore the *floor* on publish granularity.
3. If `ztsPeriod == frames-advanced-per-interrupt`, you publish **exactly one** anchor per
   interrupt = `(cursorAtInterrupt, interruptHostTime)`. No mirror-pump while-loop, no
   back-interpolation. **This is the user's instinct, and the ADK doc endorses it.**
4. If `ztsPeriod ≠ per-interrupt advance`, you publish the grid points crossed *this* interrupt
   (0, 1, or many), each back-interpolated to its exact grid frame's host time (the current
   mirror-pump while-loop). Correct, but more moving parts.

**The RX wrinkle:** our blocking-cadence RX advances a *variable* number of frames per interrupt
(data packets carry 8 frames, NO-DATA carry 0), while `ztsPeriod` is a *fixed* declared constant.
So `ztsPeriod == per-interrupt advance` is only achievable if the interrupt boundary is pinned to
a **constant-frame block** (interrupt every K packets where K packets always sum to a fixed frame
count — e.g. a full blocking/SYT_INTERVAL period). Otherwise keep a fixed `ztsPeriod`, ensure
`ring % ztsPeriod == 0`, and interpolate grid frames at each interrupt.

### 9.4 Recommended ADK-correct model

1. **Declare one ZTS period** that divides the ring: `Create(in_zero_timestamp_period = P)` with
   `ring % P == 0` **and** `ioPeriod` consistent with it. (Fixes §5 in the contract's own terms.)
2. **Publish from the IR interrupt**, sample times strictly on the `P` grid, host time = the
   interrupt's captured time (or interpolated to the grid frame if `P ≠ per-interrupt advance`).
3. **Prefer `P` = the per-interrupt frame advance** so it's one-anchor-per-interrupt with no loop;
   pin the interrupt to a constant-frame block to make the advance constant.
4. **Feed raw periodic anchors — do not pre-smooth or re-extrapolate.** The HAL's
   `IOUserAudioClockAlgorithm` (default SimpleIIR) filters the stream; double-filtering fights it.
5. **Do not copy Saffire's once-per-wrap literally.** That is the IOAudioEngine `takeTimeStamp`
   model (no declared period). ADK's contract is the periodic grid above; §8.4's "once per wrap"
   only coincides with ADK if `wrap == ztsPeriod`.

### 9.5 Historical Position Before Adoption

- `Create(kAudioIoPeriodFrames=48)` + `SetZeroTimeStampPeriod(48)` + mirror-pump 48-step loop:
  internally consistent on the *period* (all 48) but **`512 % 48 ≠ 0`** breaks the ring constraint
  (§5 / FW-46).
- The mirror-pump while-loop is the "`P ≠ per-interrupt advance`" path (§9.3 #4) — functional but
  the more complex of the two; moving to one-anchor-per-interrupt (§9.3 #3) removes it.
- This whole topic is tracked separately from the §8.4 quick-fix as its own issue (do it *right*
  per the ADK contract), so the immediate-silence fix (FW-46) and the cadence redesign don't
  block each other.

### 9.6 FW-53 implementation

- The declared ZTS period remains **512 frames** because the accepted 512-frame ring cannot use
  the 48-frame advance of an 8-packet blocking group (`512 % 48 != 0`).
- The IR drain detects every crossed 512-frame grid point and derives its host time from the
  captured interrupt time plus packet back-interpolation. Raw anchors are not smoothed.
- Real anchors cross the dext boundary through a latest-value mailbox in the shared direct-audio
  control block. Each accepted anchor advances a generation and sends an `OSAction` through
  `ASFWAudioNub`; the AudioDriverKit consumer snapshots the newest generation available.
- IO callbacks no longer pump ZTS publication. Frame zero is a local synthetic startup timestamp;
  the core remains the sole producer of real anchors.
- Anchor generation, invalid publication, raw publication, and HAL publication counts are exposed
  in the direct-audio diagnostics.

---

## 10. Function map (Saffire.i64)

| Address | Symbol | Role |
|---------|--------|------|
| `0xC1xx` | `Saffire::initHardware` | sets rate=48000, latency-mode field 390478=1; copies geometry from `UpdateIsochBufferParams` |
| `0xF506` | `Saffire::UpdateIsochBufferParams` | numGroups (160/80/40) + packetsPerGroup (12) per rate |
| `0xFDxx` | `Saffire::PrepareRecvDCLs` | builds RX DCL program; callback on last DCL of each group |
| `0x10304` | `Saffire::PrepareSendDCLs` | TX DCL program; `IOFWDCL::setTimeStampPtr` for OUTPUT timestamps |
| `0xF678` | `Saffire::RecvGroupCallback` | IR group IRQ; `clock_get_uptime` → `ReadFirewireBuffers` |
| `0xCF24` | `Saffire::ReadFirewireBuffers` | RX data mover + SYT recovery + `takeTimeStamp` anchor |
| `0xF782` | `Saffire::SendGroupCallback` | IT group IRQ → `FillFirewireBuffers` |
| `0xE778` | `Saffire::FillFirewireBuffers` | TX fill: HW-timestamp feedback → phase → SYT → data/no-data gate |
| `0xC9C2` | `Saffire::adjustOutputPhase` | deadbanded hold-and-nudge output-phase PLL vs device master clock |
| `0x42xx` | `SaffireAudioEngine::updateAudioEngineSettings` | engine buffer geometry (frames, offsets) |
| `0x284A` | `SaffireAudioEngine::updateSampleLatencies` | reported (presentation) latency, separate from offsets |
| `0x14154` | `SamplingRateToSamplesPerFrameMax` | 8/16/32 frames per data packet |

Device-struct field offsets referenced (byte offset / dword index):
`numGroups` 1561920/390480 · `packetsPerGroup` 1561928/390482 · `clockStreamIndex`
1569704 · `SYT-diff ring` 1569728 (512 entries) · `masterClock` 1570780 · stream array
base 1565864 (480-byte stride) · engine sample buffer `stream+456`, frame capacity
`(stream+440) >> 2`.

---

## 11. FW-46 through FW-52 implementation status (2026-06-12)

> **Historical intermediate state.** Section 13 supersedes this geometry.

That intermediate revision used:

- one central geometry: 512-frame input/output rings, 512-frame HAL IO and ZTS
  periods, 32-frame alignment, matching 8-packet RX/TX groups;
- raw RX-owned host anchors on the declared 512-frame ZTS grid, queued from the
  IR drain and delivered in order by a coalesced Nub action; frame zero remains
  a local synthetic startup timestamp;
- mapped HAL input/output allocations as the frame rings, with direct `int32`
  playback encoding and no intermediate AMDTP PCM ring or float scratch copy;
- RX host anchors for CoreAudio and `RxSytCadence` for TX phase/SYT, with the old
  authority arbitration and extrapolated ZTS timeline removed;
- one completion stamp per retired OUTPUT packet and a coalesced Nub/OSAction
  notification that prepares replacement groups one 192-packet OHCI ring lap
  ahead, while retaining the then-current 8-packet hardware-visible group;
- explicit DATA/NO-DATA disposition. NO-DATA emits an 8-byte CIP-only packet
  with `SYT=0xFFFF` and consumes no PCM frames. DBC follows the observed
  Apple/Saffire blocking cadence rule; the PCM cursor does not advance merely
  because a NO-DATA packet occupied a bus cycle;
- diagnostics for anchors, queue depth/overflow, ZTS action dispatch/coalescing,
  mirrors, stale anchors, TX lead, DATA/NO-DATA, post-lock forced NO-DATA,
  completion wakes, and refill latency.

The shared timing transport layout changed and its ABI is now version 4.
The TX control block now carries the producer failure stage, reason, absolute
packet/range, cursors, and replay epoch so `producer-fatal-status` can be
diagnosed from the core-side stop log.

No build, test executable, formatter, code generator, Xcode operation, or
hardware run was performed in this implementation session because builds freeze
the development system. Manual compile and rig verification remain required,
including the two-hour duplex soak and wire checks for DBC/FDF/SYT/length and
N-D-D-D cadence.

---

## 12. Timing Geometry Simulation & Saffire Safety Offset Validation

To mathematically prove and dynamically validate these timing relationships, we created a Python-based callback and queue simulator: [asfw_timing_geometry_sim.py](file:///Volumes/SDExt/DEV/ASFireWire/tools/asfw_timing_geometry_sim.py).

### 12a. Simulator Design
The simulator models:
- **Circular Frame Rings**: Circular buffer tracking per-frame write/read generations to catch starvations, underruns, and overwrites (with correct wrap-around generation indexing).
- **CoreAudio Callback Scheduler**: CoreAudio thread wakeup with Gaussian host-time jitter.
  Each wakeup gets exactly **one** jitter draw, fixed when the wakeup is armed. (An earlier
  revision re-drew jitter every 125 µs packet while waiting and clamped monotonically, which
  ratcheted the wake time up by max-of-draws + 10 µs/packet — fabricating ~44 underruns for
  512-frame IO periods with small output safety. Fixed 2026-06-12.)
- **ZTS Anchor Publication**: Static aligned anchors vs. crossed-grid interpolation.
- **Variable-size Callbacks**: Simulating variable sizes (e.g., `[96, 192, 288]` frames) to test HAL buffer resizing behaviors.

### 12b. The target_read_start Correction
During simulator testing, we identified and corrected a double-counting bug in the input read window calculation:
- **Old Formula**: `target_read_start = client_sample_time - g.input_safety_frames + g.io_period_frames` (which shifted the read window too close to the current time, requiring the input safety offset to be double-padded).
- **New Formula**: `target_read_start = client_sample_time - g.input_safety_frames`.

This corrected the duplex safety equation to:
$$\text{Input Safety} \ge \text{Output Safety} + \text{IO Period} + \text{Jitter Buffer}$$

### 12c. Decompiled Saffire Safety Offset Validation
Using the corrected formula, we verified Saffire's decompiled offsets at 48 kHz:
* **Output Safety**: 48 frames (1.0 ms)
* **Input Safety**: 128 frames (2.67 ms)
* **IO Period**: 64 frames (1.33 ms)

The equation matches perfectly:
$$128 \ge 48 + 64 + 16 \text{ (jitter buffer margin)}$$

The simulator runs this configuration with:
- **Playback Underruns**: 0
- **Capture Starvations**: 0
- **Total RTL (Round-Trip Latency)**: **5.0 ms** (Saffire Safety Out (1.0 ms) + Safety In (2.67 ms) + IO Period (1.33 ms)).

This mathematically proves that the original Saffire driver was fully optimized for professional-grade ultra-low latency (5.0 ms RTL) at 64-frame buffer sizes.

### 12d. Simulated Geometries Summary (48 kHz)

| Geometry | Output Safety | Input Safety | IO Period | Ring Size | Starvations / Underruns | Total RTL |
|:---|:---:|:---:|:---:|:---:|:---:|:---:|
| **Saffire Profile** | 48 | 128 | 64 | 768 | 0 / 0 | **5.0 ms** |
| **Low-Latency** | 96 | 224 | 96 | 768 | 0 / 0 | **8.67 ms** |
| **Clean Aligned** | 192 | 448 | 192 | 768 | 0 / 0 | **17.33 ms** |
| **Variable Callbacks** | 192 | 544 | 192 (max 288) | 768 | 0 / 0 | **19.33 ms** |


---

## 13. ADOPTED GEOMETRY (2026-06-12) — aligned DMA program, no synthesized anchors

Decision per §9.3 case 3 and the §12 simulator: **the interrupt IS the ZTS callback.** The DMA
program is aligned so the ZTS grid coincides with the interrupt-group boundaries; the driver does
not synthesize anchors mid-group.

### 13a. Values (`AudioTimingGeometry.hpp`, single source)

| Constant | Old | New | Why |
|---|---:|---:|---|
| `kRxPacketsPerGroup` / `kTxPacketsPerGroup` | 8 | **32** | 32 pkts = 8 whole D/D/D/N blocks → constant nominal advance **192** frames per interrupt (4 ms) |
| `kHalZeroTimestampPeriodFrames` | 512 | **192** | `== kNominalFramesPerTimingGroup`: one anchor per interrupt, on the grid, with the interrupt's captured host time |
| `kHalIoPeriodFrames` (max client IO) | 512 | 512 | unchanged; client buffer size is the client's choice |
| `kFrameRingFrames` | 512 | **1536** | divisible by 192 **and** 512; old ring == max IO had **zero output headroom** (whole-ring rewrite per 512-frame IO cycle — the sim's SAFE BASELINE overwrites) |
| `kInputSafetyFloorFrames` | — | **256** | input lands in the HAL ring only at the group drain (4 ms batches); floor = group (192) + jitter (64). Saffire's decompiled 128 assumed its own 1.5 ms groups |
| `kTxSharedSlotPackets` | (= ring, 512) | **1024** | packet-domain backing capacity, independent of the frame ring |
| `kTxHardwareRingPackets` | — | **192** | six complete 32-packet OHCI groups |
| `kTxPreparationSlackPackets` | — | **192** | six additional groups for DriverKit scheduling |
| `kTxPreparationLeadPackets` | 200 | **384** | hardware ring plus scheduling slack |

Frame rings are **no longer power-of-two** (any ring that is a whole number of groups carries a
factor of 3); indexing is modulo everywhere (verified: no mask-based frame-ring indexing existed).

### 13b. Related correctness fixes landed with the geometry

1. **`IOUserAudioDevice::Create` was passed `kAudioIoPeriodFrames` as the declared
   zero-timestamp period** (`ASFWAudioDriverGraph.cpp`). Harmless while every constant was 512;
   wrong the moment they diverge. Now passes `kHalZeroTimestampPeriodFrames`.
2. **Anchor-step validation poison** (`ASFWAudioDriverZts.cpp`): the drain required
   `sampleFrame == last + P` exactly. After the synthetic frame-0 prime, a slow SYT
   qualification (or any mid-stream relock) makes the first real anchor land k·P ahead — it was
   rejected, and then *every* subsequent anchor was rejected against the stale `last`,
   permanently starving the HAL of real timestamps. Now accepts any on-grid, monotonic step.
3. **Input safety floor** applied at HAL registration (`ASFWAudioDriverGraph.cpp`):
   `max(profile, group + 64)` — see 13a.
4. `static_assert`s: ZTS grid ≡ group boundaries, ring % {ZTS, maxIO, group, 32} == 0,
   IR descriptor ring (512) and TX HW ring (192) are whole numbers of groups, TX shared slots
   hold the full preparation lead.

The implemented input-safety rule currently applies
`max(profileInputSafety, timingGroupFrames + 64)`, yielding a 256-frame floor.
That is only the minimum batch-plus-jitter protection. For arbitrary client IO
sizes, the runtime target is:

```text
inputSafetyFrames =
    max(profileInputSafety,
        outputSafetyFrames + actualClientIOFrames + jitterMargin,
        timingGroupFrames + jitterMargin)
```

In particular, 256 frames must not be treated as sufficient merely because it
is the compiled floor when a 512-frame client transfer requires a larger
window.

### 13c. What deliberately stays

- **The grid-crossing publish loop in `IsochReceiveContext.cpp` remains** — not as a synthesis
  mechanism but as the drift guard. The ADK contract requires published sample times to sit
  exactly on the declared grid; device crystal drift (ppm) occasionally makes a group advance
  ±8 frames, so the cursor does not always land exactly on the boundary at the interrupt. In the
  nominal cadence the loop degenerates to exactly one anchor per interrupt at the boundary with
  the interrupt's captured time (the within-packet interpolation contributes ≤ 125 µs and only
  at slip points).
- 8-frame/packet blocking cadence, Saffire output safety (48), RxSytCadence/TX phase machinery —
  untouched.

### 13d. Per-packet timestamp correlation

The multi-group staleness defect described in the original adoption note has
been addressed. IR runs with `ContextControl.isochHeader` set, and each packet
payload carries the eight-byte driver prefix containing its OHCI `timeStamp`
and isochronous header quadlets. The receive processor decodes that timestamp
per packet; the cycle-timer/host-time pair sampled at drain entry is only the
expansion reference.

This matters more with 32-packet groups: assigning one group-relative
timestamp to an older coalesced group would be four milliseconds late.

**Verification priority: high after the first stable duplex run.** Force or
observe multi-group drains and cycle-timer wraparound, then verify that every
ZTS anchor is derived from the packet that actually crossed its 192-frame grid
point.

### 13e. Verification Status at Initial Adoption

- `tools/asfw_timing_geometry_sim.py`: both ASFW TARGET cases (512-frame client w/ 48/624
  offsets; 64-frame client w/ 48/256) pass with 0 underruns / 0 starvations / 0 overwrites;
  full assertion suite green. Sim scheduler one-draw fix included (§12a).
- C++ geometry test updated (`tests/audio/AmdtpRateGeometryTests.cpp`).
- At this point, **no Xcode build / hardware run had been performed** (builds freeze the dev system). Required before
  trust: full build, `IOUserAudioDevice::Create` period sanity in HAL logs (`SetZeroTimeStampPeriod(192)`),
  wire check that IR/IT interrupts now fire every 32 packets, duplex soak.

### 13f. First hardware run (2026-06-12 13:40) — two bring-up failures, both fixed

Log: IT starts, `IT FATAL UNDERRUN: fillAbsIdx=224 expectedGen=1 commitGen=0` at +8 ms, then
`TX geometry/ABI mismatch abi=3 slots=512 group=32 zts=512` → `StartDevice failed at
ValidateTxTransportGeometry`.

1. **`zts=512` mismatch:** `IsochService.cpp` has two `SetSharedMemoryDescriptors` call sites;
   the one that runs during duplex start passed a **literal `512`** as `ztsPeriodFrames` (the
   other passed the geometry constant). The control block therefore carried 512 against the
   expected 192. Fixed: both sites now pass `kHalZeroTimestampPeriodFrames`. (Constants-rule
   violation — the literal predated the geometry split.)
2. **FATAL UNDERRUN at fillAbsIdx=224:** a bring-up race that predates the geometry change and
   simply had never run on hardware. The IT DMA starts inside `StartStreaming`, but the pump
   (`TxPreparationReady`) is gated on `isRunning`, which StartDevice sets only *after*
   streaming start + geometry validation (~22 ms in this log). The prefill covered exactly
   `kTxPreparationLeadPackets` (224), so the refill met an uncommitted slot at the **second**
   group completion — 8 ms in (old geometry would have hit at 2 ms: prefill 200, group 8).
   Fixed twofold:
   - `PrefillTxRingBeforeStart` now seeds the **entire** shared slot ring.
     In this historical run, `kTxSharedSlotPackets` was 512 packets, providing
     64 ms of valid NO-DATA backing capacity; refill could not reach lap 2
     before ~40 ms after IT RUN.
   - StartDevice runs an explicit `PrepareTransmitSlots` catch-up immediately after
     `isRunning = true`, instead of waiting for the next completion wake.

For that run, the full-ring prefill provided a 64 ms hard safety budget with
the pump live at ~22 ms and topped up every 4 ms thereafter. It did **not**
require the first 64 ms visible on the wire to be NO-DATA: DATA could appear
earlier as the live pump replaced future slots. Until replacement, every slot
remained a valid CIP-only NO-DATA packet.

The current shared ring is 1024 packets, so the same full-ring policy now
provides 128 ms of packet-domain backing capacity. This is capacity, not
mandatory startup silence or added HAL latency.
