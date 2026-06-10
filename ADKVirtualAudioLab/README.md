# ADKVirtualAudioLab

A closed, hardware-free AudioDriverKit lab. It publishes a virtual CoreAudio device whose
output path feeds the AMDTP/DICE TX protocol stack into a fake isoch slot ring instead of
real OHCI DMA. It is a wind tunnel for the ASFW audio path — **not a second driver**.

Verdicts from this lab are always of the form *"the ADK contract is satisfied/violated"*,
never *"ASFW is fixed"*. ASFW-specific seams (the two-IOService nub/driver split, the
audio↔isoch queue boundary, DICE clock lock, real cycle timer) are explicitly out of scope
and must be validated on the bench.

---

## Targets (`project.yml`)

| Target | Type | Pacing | Purpose |
|--------|------|--------|---------|
| `ADKVirtualAudioLab` | driver-extension (DriverKit SDK) | Real — coreaudiod drives the rhythm | Real-time viability: callback cadence, jitter, lifecycle/ownership behavior that only exists at runtime |
| `ADKVirtualAudioLabTests` | macOS CLI tool | Synthetic — test code calls the WriteEnd path in a loop | Logic correctness: deterministic, millions of cycles, golden vectors, adversarial schedules |

> **Signing status:** `project.yml` currently sets `CODE_SIGNING_ALLOWED: NO`. That makes the
> dext target compile-only. Before any runtime question (O*/C* below) can be answered, the
> dext must actually load: ad-hoc signing + `systemextensionsctl developer on` + the bench
> machine's existing SIP/AMFI posture.

---

## Goals

1. **Verify ADK ownership & lifecycle contract.** `OSSharedPtr` retain conventions,
   `AddObject`/`RemoveObject`, `free()` ordering, init-failure paths, ivars lifetime vs.
   captured blocks. These fail as opaque dext crashes; crashing a 10-file lab costs minutes.
2. **Verify the HAL IO contract.** What the host minimally requires before it starts IO
   cycles, and how zero-timestamp publication drives WriteEnd. This directly informs the
   ASFW publish/binding bug (HAL never starting IO when ZTS anchors don't arrive).
3. **Prove AMDTP TX packetization under real pacing.** CIP/DBC/SYT/cadence invariants
   holding across thousands of real WriteEnd callbacks, not just a for-loop.
4. **Rehearse the ZTS discipline.** Raw periodic anchors at ring-wrap cadence, no per-tick
   extrapolation (the model RE'd from the Saffire kext reference).

## Non-goals

OHCI descriptors, DMA coherency, interrupt timing, real cycle-timer phase, the ASFW
nub-matching path, MIDI (optional last phase only). If a lab-green packetizer still
glitches in ASFW, the bug is by construction in the transport/timing layer — that triage
boundary is the value the lab buys.

---

## SDK-verified facts (DriverKit 25.5)

Every API claim in this lab must be verified against the real headers at
`$(xcrun --sdk driverkit --show-sdk-path)/System/DriverKit/System/Library/Frameworks/`.
This rule exists because an earlier external research pass **fabricated** at least three
API surfaces (`kIOUserAudioIOOperationBeginSample`, `IOUserMIDIDestination::ScheduledWrite`
with a `timeTag` packet struct, and kernel-style `IOMemoryDescriptor::prepare()/complete()`).
Facts established so far:

### AudioDriverKit

- **IO operations** (`AudioDriverKitTypes.h`): exactly two —
  `IOUserAudioIOOperationBeginRead = 0` and `IOUserAudioIOOperationWriteEnd = 1`.
  WriteEnd is *"called just after writing data to the device's stream buffers"* and is
  *required* if the device has output streams. There is no BeginSample/EndSample.
- **`IOOperationHandler`** (`AudioDriverKitTypes.h`): 5-arg block
  `(in_device, in_io_operation, in_io_buffer_frame_size, in_sample_time, in_host_time)`.
  Per `IOUserAudioDevice.h` `SetIOOperationHandler` docs: *"called on a real time priority
  thread, so any work should only call real-time safe operations and never block. Many of
  the calls to various IOUserAudioObjects are synchronized against the work queue, so any
  necessary information to perform IO should be cached and captured in the block."*
  → Treat object getters as forbidden inside the handler; capture pointers up front.
- **Zero timestamps** (`IOUserAudioClockDevice.h`):
  `UpdateCurrentZeroTimestamp(uint64_t sample_time, uint64_t host_time)` — doc says
  *"should use the time passed in the hardware interrupt."*
  `GetZeroTimestampPeriod()` exists; **no `SetZeroTimestampPeriod` exists** — the period is
  fixed by the `in_zero_timestamp_period` argument to `IOUserAudioDevice::init/Create`
  (docs: the host expects successive ZTS sample times to differ by exactly this value).
  Changing it requires the device-configuration-change flow, not a setter.
- **Clock smoothing** (`AudioDriverKitTypes.h`, `IOUserAudioClockDevice.h`):
  `IOUserAudioClockAlgorithm { Raw='raww', SimpleIIR='iirf' (default), TwelvePtMovingWindowAverage='mavg' }`
  via `SetClockAlgorithm`. The host filters our timestamps — feed raw periodic anchors and
  do **not** pre-smooth or re-extrapolate in the driver.
- **Transport type** (`AudioDriverKitTypes.h`): `IOUserAudioTransportType::FireWire = '1394'`,
  settable via `IOUserAudioClockDevice::SetTransportType`.
- **Stream buffer** (`IOUserAudioStream.h`): `IOUserAudioStream::Create(driver, direction,
  IOMemoryDescriptor*)`; buffer retrieved with `GetIOMemoryDescriptor()` and mapped into the
  dext with `CreateMapping`. The HAL writes float PCM into this buffer; the driver reads it
  at WriteEnd at offset `(sample_time % ring_frames) * bytes_per_frame`.
- **`StartIO`/`StopIO`** (`IOUserAudioDevice.h`): may take as long as necessary; called for
  all streams added to the device. `in_supports_prewarming` is an init-time capability flag.
- **User client** (`AudioDriverKitTypes.h`): `kIOUserAudioDriverUserClientType = 1128363364`
  is the only type the lab driver forwards to `super::NewUserClient`.

### MIDIDriverKit (Milestone 4 only)

- Delivery is `IOUserMIDIDestination::SetIOBlock(MIDIIOBlock)` where
  `MIDIIOBlock = ^(IOUserMIDIUMPWord const* umpWords, size_t numWords)`
  (`MIDIDriverKitTypes.h`). It runs **on the real-time thread**, delivers **UMP words**
  (MIDI 2.0 Universal MIDI Packets), not a MIDI 1.0 byte stream, and carries **no
  per-packet host timeTag**. Any AM824 MIDI slot work therefore needs a UMP→MIDI1
  bytestream translator feeding a lock-free FIFO consumed at WriteEnd.

### DriverKit memory

- DriverKit's `IOMemoryDescriptor` has **no** `prepare()`/`complete()`/`withAddressRange()`
  — those are kernel-kext APIs. `IOBufferMemoryDescriptor::Create` + `CreateMapping` is the
  whole story in a dext.

---

## What we need to confirm (open questions)

Answered empirically, in the dext, with counters — not from docs or LLMs.

### Ownership / lifecycle (O)

- **O1** — Retain conventions across `OSTypeAlloc` → `init` → `AddObject` → `RemoveObject`
  → `free`. Does teardown order matter (`RemoveObject` before reset? stream before device?).
- **O2** — Can the `IOOperationHandler` block fire after `StopIO` returns? After the device
  is removed? (Determines whether the raw `ivars` capture in the block is safe or needs a
  generation guard.)
- **O3** — Init-failure paths: `super::init` succeeds but our init fails → does the
  framework call `free()`; do we leak the half-built device?

### HAL / clock contract (C)

- **C1** — Minimal trigger for IO cycles: registered device + `StartIO` + first
  `UpdateCurrentZeroTimestamp` — is that sufficient? How many anchors before the first
  WriteEnd? What is the observed relationship between anchor host_time and the first
  callback? *(This is the lab proxy for the ASFW "no HAL ZTS anchor → no IO cycle" bug.)*
- **C2** — ZTS tolerance: jittered anchor (±x µs), late anchor, skipped period — at what
  point does the HAL glitch or stop IO? Difference between `Raw` and `SimpleIIR` clock
  algorithms in observed behavior.
- **C3** — WriteEnd shape: does `in_io_buffer_frame_size` vary? Is `in_sample_time`
  contiguous across callbacks? What happens across `StopIO`/`StartIO` restart (sample time
  reset or continuous)? Startup burst behavior.
- **C4** — Does the ZTS period (512 here) interact with HAL IO buffer size in a way that
  constrains ASFW's choice?

### Protocol invariants (P) — host-testable, no dext required

- **P1** — Blocking 48k cadence: exactly 6000 data packets per 8000 cycles (3:1
  data:no-data pattern), stable across cycle-counter wrap.
- **P2** — DBC continuity: `dbc(n+1) = dbc(n) + data_blocks mod 256`, including the
  blocking-mode no-data-packet DBC rule.
- **P3** — CIP bit-exactness against `CipHeader.hpp`: Q0 SID/DBS/FN/QPC/SPH, Q1
  EOH/FMT=0x10/**FDF=0x02 at 48 kHz**/SYT. Cross-checked against Linux `amdtp-am824`
  behavior (in-tree reference at `../firewire/`).
- **P4** — Frame coverage: `firstAudioFrame`/`framesInPacket` tile the WriteEnd sample
  timeline with no gaps or overlaps — including under adversarial schedules (irregular
  frame counts, skipped callback, sample-time jump).
- **P5** — SYT discipline: monotonic with expected increment on data packets, `0xFFFF` on
  no-data; constant device sub-cycle graft (0x0b0) rehearsed against a simulated cycle
  timeline; transmit lead stays within the bounded window: warn under 1 cycle (≤3071
  ticks), ship while < 7620 ticks (~2.5 cycles), no-data at ≥ 7620. *(Corrected
  2026-06-10 from "1–4 cycles": re-verified `FillFirewireBuffers` control flow — the
  12287-tick test only escalates logging inside the already-rejecting branch.)*

---

## Architecture

### Target layout

```
ADKVirtualAudioLab/
├── Protocols/        Portable protocol core. Zero DriverKit includes. Namespaces
│   ├── Audio/AMDTP/    (ASFW::Protocols::Audio::...) match ASFW exactly so graduation
│   ├── Audio/IEC61883/ into ASFWDriver/Isoch is a `git mv`, not a rewrite.
│   └── Audio/DICE/
├── Core/             Orchestration, compiled into BOTH targets.
│   ├── TxLabController     HAL-facing coordination (today: Driver/VirtualAudioDeviceController)
│   └── TxTimingModel       Owns frame → cycle → SYT mapping. The unit under research.
├── Ports/            The explicit seams (tiny pure-virtual headers).
│   ├── IAmdtpTxSlotProvider   production: OHCI IT DMA ring · lab: fake ring
│   ├── ICycleTimeline         production: cycle timer · lab: synthesized from sample_time
│   └── IDiagSink              RT-safe counters/snapshot sink
├── Lab/              Host-side adapters and instruments.
│   ├── FakeIsochTxSlotProvider   dumb storage: 256 slots × 512 B (mirrors IT ring geometry)
│   ├── VerifyingSlotProvider     decorator wrapping ANY provider — the invariant checker
│   ├── SimulatedCycleTimeline    6 frames per 125 µs cycle at 48 kHz
│   ├── WriteEndTraceReplayer     replays recorded (sample_time, host_time, frames) sequences
│   └── PacketDump                hex/quadlet inspection helpers
├── Driver/           .iig classes + thin translation ONLY. Zero logic.
│   ├── VirtualAudioDriver.{iig,cpp}
│   └── VirtualAudioDevice.{iig,cpp}
└── Tests/            Scenario runners: replayer → controller → verifier.
```

**Current state vs. target:** the `Core/` move happened in Step 0 and `Stubs.cpp` was
dissolved in Step 5 — controller, `AudioIOPath`, the DICE layer, and
`FakeIsochTxSlotProvider` are all real implementations now; the tool target's source
list is `Tests + Core + Protocols + Lab`. Remaining gaps vs. the target layout: the
`Ports/` split (`ICycleTimeline`, `IDiagSink`) and the Step 6 / Milestone 2
instruments (`VerifyingSlotProvider`, `SimulatedCycleTimeline`, `TxTimingModel`,
`WriteEndTraceReplayer`).

### Scaffolding reuse assessment

The existing scaffolding is ~90% reusable — it is interface-first, and the absence of
implementations is the planned work, not a defect. Do **not** rewrite the `Protocols/`
headers "while we're at it": they are namespace-matched to ASFW for the `git mv`
graduation, and churn there is divergence risk for zero design gain.

| Verdict | Files | Notes |
|---------|-------|-------|
| **Reuse as-is** | All of `Protocols/` (AMDTP, IEC61883, DICE), `FakeIsochTxSlotProvider.hpp`, `PacketDump.hpp`, `Driver/*.iig`, `project.yml` structure | Types/seams are the right vocabulary; `.iig` overrides verified against the real SDK; `PacketTimelineSlot` atomics are overkill under single-queue but become load-bearing at ASFW's audio↔isoch boundary |
| **Fix in place** | `IEC61883/Syt.hpp` | `static [[nodiscard]]` attribute order — won't compile (Step 0) |
| | `Driver/VirtualAudioDevice.cpp` | hardcoded `/8` bytes-per-frame in two places (derive from format); `StopIO` discards `super::StopIO`'s return. The raw `ivarsPtr` block capture is the SDK-documented pattern ("cache and capture in the block") — keep, annotate, and answer its teardown safety as O2 |
| | `Driver/AudioIOPath.hpp` | `OutputBufferView` duplicates `AmdtpTypes.hpp`'s `HostAudioBufferView` field-for-field — collapse to one type during the `Core/` move |
| **Relocate** | `VirtualAudioDeviceController.hpp`, `AudioIOPath.hpp` → `Core/`; `IAmdtpTxSlotProvider.hpp` → `Ports/` | Already pure C++, just in the untestable directory (Step 0); `Ports/` move is cosmetic, do it when adding `ICycleTimeline`/`IDiagSink` |
| **Discard** | `Driver/Stubs.cpp` | The only throwaway, designed as one — even its signatures are copies of the headers |
| **Net-new** | `Tests/`, `VerifyingSlotProvider`, `SimulatedCycleTimeline`, `TxTimingModel`, `IDiagSink`, `WriteEndTraceReplayer` | No conflict with anything existing |

One deliberate non-fix: `VirtualAudioDriver.cpp`'s teardown order (`RemoveObject` before
reset) is among the things the lab exists to validate (O1) — don't "fix" it preemptively.

### Key design decisions

- **The verifier is the product.** `VerifyingSlotProvider` asserts P1–P5 on every
  `PublishSlot`, recording violations as sticky counters via `IDiagSink` (no RT logging,
  ever). The same decorator runs in three contexts: host tests (`Verifying(Fake)`), the lab
  dext (`Verifying(Fake)` under real pacing), and later ASFW bring-up
  (`Verifying(RealDmaRing)`). The lab's most valuable artifact graduates into a production
  diagnostic instead of dying with the lab.
- **Time is an explicit port.** The lab has no 8 kHz cycle timer and does not pretend to:
  `SimulatedCycleTimeline` derives cycle position from WriteEnd sample time (6 frames/cycle
  at 48 kHz). Adversarial schedules — jitter, dropped periods, the 7999→0 bus-cycle wrap —
  become constructor arguments to a test instead of things observed on hardware.
- **Trace replay closes the pacing gap.** Record real `(sample_time, host_time,
  frame_count)` sequences once (from the lab dext, later from bench ASFW), check the trace
  in, replay forever on host. Regression tests then run against genuine coreaudiod
  scheduling, including its startup burst, without hardware.
- **Copy-then-port, not shared library.** Sharing sources with the DICE branch would couple
  the closed lab to its churn. The discipline that makes copying safe: `Protocols/` and
  `Core/` stay byte-portable (no `Lab/` or `Driver/` includes), and any divergence from
  ASFW's layout is treated as a lab bug.
- **Observability without RT violations.** Sticky counters + a bounded ring of recent
  packet summaries; dumped via `IOLog` at `StopIO` or a low-rate snapshot. Never per-packet
  logging in the IO path.
- **Dext clock model.** The lab device is its own clock master: a dispatch timer fires once
  per ZTS period and calls `UpdateCurrentZeroTimestamp(sample, host)` with raw values —
  deliberately mirroring the reference model (anchor per ring wrap, host smooths via the
  clock algorithm, no driver-side extrapolation).

---

## Implementation plan

### Milestone 1 — "host pipeline green" (current target)

The full TX chain running in the tool target with the verifier passing P1–P4 over
millions of synthetic cycles. Needs no signing, no dext load, no hardware; everything
else stacks on top. Steps in dependency order:

#### Step 0 — Make the tool target build and run (S) — ✅ DONE

- Create `Tests/main.cpp` — referenced by `project.yml` but missing, so the target cannot
  generate. A plain `main()` with a tiny `CHECK()` macro; no test-framework dependency
  (keeps the lab closed).
- Fix `IEC61883/Syt.hpp`: `static [[nodiscard]] bool ...` is ill-formed (an attribute
  cannot sit between declaration specifiers); must be `[[nodiscard]] static bool ...`.
  Clang rejects this on first compile.
- Do the `Core/` move (controller + `AudioIOPath` out of `Driver/`) and add `Core` to the
  tool target's sources in `project.yml`, so orchestration becomes host-testable. While
  moving, collapse `OutputBufferView` into `AmdtpTypes.hpp`'s field-identical
  `HostAudioBufferView`.
- Small `VirtualAudioDevice.cpp` fixes: derive bytes-per-frame from the format struct
  instead of the hardcoded `/8` (two places); propagate `super::StopIO`'s return value.

#### Step 1 — Leaf units: pure functions, no dependencies (S each, parallelizable) — ✅ DONE

All five units implemented and green (8000+ checks, 0 failures): cadence pattern
N,D,D,D with exactly 6000 data / 8000 cycles and 48 000 frames per second of bus time;
CIP golden vectors (`q0=0x00020010`, `q1=0x90021234` for the canonical 48k stereo
stream); DBC wrap and long-run continuity; SYT encode/no-info; PCM codec bit-exact for
all three encodings. Cadence stubs removed from `Stubs.cpp` (now real in
`Protocols/Audio/AMDTP/AmdtpCadence.cpp`).

| Unit | Implementation | Test vectors |
|------|---------------|--------------|
| `PcmSlotCodec` | clamp → scale 2²³−1 → label per encoding (AM824 `0x40` prefix vs Raw24-in-32 BE/LE) | ±1.0, 0.0, ±0.5, >1.0 clamp, denormals; bit-exact expected quadlets |
| `CipHeaderBuilder` | Q0/Q1 bit packing per `CipHeaderConfig`; `BuildNoData` with the no-data FDF | hand-packed words; FDF=0x02 @ 48 kHz; field positions cross-checked against `../firewire/` amdtp |
| `DbcCounter` | mod-256 advance; `ValueForNextPacket` | wrap 255→0; advance-by-8 sequences |
| `SytFormatter` | `kNoInfo`; cycle[3:0]‖offset[11:0] encoding | known cycle/offset pairs |
| `Blocking48kCadence` | 6-frames-per-cycle accumulator, emit 8 when ≥8 → N,D,D,D pattern | exactly 6000 data per 8000 cycles; pattern stable across counter wrap |

> ✅ **Golden-check done** (Linux `sound/firewire/amdtp-stream.c` + FFADO
> `AmdtpTransmitStreamProcessor.cpp`, both agree):
> - DBC carries the index of the packet's **first** data block (advance-after-emit;
>   `CIP_DBC_IS_END_EVENT` is a device quirk we don't implement).
> - **No-data packets carry the DBC unchanged**, FDF=0xFF, SYT=0xFFFF, DBS keeps the
>   stream value; FN/QPC always 0 for AM824.
> - No-data packets are **CIP-header-only (8 bytes)** — FFADO comment: *"DICE-II doesn't
>   like"* payload in no-data packets. The Step 3 packetizer must set `byteCount = 8`.
> - 48 kHz FDF = `AMDTP_FDF_AM824 | CIP_SFC_48000` = 0x02.

#### Step 2 — `AmdtpPacketTimeline` (M) — ✅ DONE

Slot state machine (`Empty → ExposedForAudio → Published → Completed`), `AttachSlots`,
`ExposeDataPacket`, `MarkNoDataPacket`, and `FindSlotForAudioFrame` over
`firstAudioFrame`/`framesInPacket`. Tests: lookups at exact boundaries (first/last frame
of a packet, frame between packets, frame outside the window), generation-counter
behavior on slot reuse.

Design decisions encoded (and documented in `AmdtpPacketTimeline.cpp`):
1. `packetIndex` is absolute/monotonic; ring mapping (`% slotCount`) is internal.
2. `FindSlotForAudioFrame` is a bounded scan — frame→slot is not arithmetic in
   blocking mode (no-data positions carry zero frames).
3. Ring reuse bumps the slot generation and fully evicts the old occupant
   (`SlotByIndex` of the evicted absolute index returns null).
4. Timeline owns `Empty→ExposedForAudio` and retirement; provider path owns
   `→Published`. No-data positions go straight to `Completed`, invisible to frame
   lookup. Field/state ordering: fields written before release-store of `state`,
   read after acquire-load.

#### Step 3 — `AmdtpTxPacketizer::PrepareNextPacket` (M) — ✅ DONE

First integration point: cadence decides data/no-data → DBC advances → CIP built →
payload defaults written (clear + non-audio slot init per `AmdtpTxPolicy`) → timeline
exposure → `PreparedTxPacket` out. Runs with `AmdtpTimingState.txClockValid = false`
(SYT = 0xFFFF) until Milestone 2; the `AmdtpTimingState` parameter is the seam.

Design decisions encoded (documented in `AmdtpTxPacketizer.cpp`):
1. `Configure` selects the cadence and rejects non-48 kHz rates (honest failure);
   `dbs == 0` derives `pcmChannels + midiSlots`. This wired the previously-dead
   `cadence_` pointer.
2. `packetIndex` comes from the caller's slot view; the packetizer owns no cycle
   numbering.
3. Slot bytes are wire-order big-endian — the single logical→bus conversion point.
4. Frame continuity owned by the packetizer (`nextAudioFrame_`, seeded by `Reset`);
   `AmdtpTimingState.nextAudioFrame` reserved for Milestone 2 rebase.
5. Golden rules applied: no-data = CIP-header-only 8 bytes, DBC unchanged; data DBC =
   first block index, advanced after emit.
6. **Failure contract:** no state (cadence/DBC/frame counter) mutates on any failure
   path — a failed call is retryable with a corrected slot (covered by tests).

Verified: golden byte images for data/no-data packets (sid/dbs/dbc/syt placement),
non-audio slot defaults (0x80000000 BE in MIDI slots, PCM zeroed), Reset seeding,
non-blocking 6-frame variant, and a 10⁶-cycle run asserting P1 (ratio exactly 3:4),
P2 (DBC continuity), P4 (gapless frame tiling) inline.

#### Step 4 — `AmdtpPayloadWriter` (M) — ✅ DONE

The reference-model piece (one buffer, absolute sample frame): fill PCM into
*already-exposed* packets via `FindSlotForAudioFrame`. The four counters
(`framesVisited/Written/WithoutPacket/OutsidePacket`) are the lab analog of the ASFW
`pcmNZ`/`pcmZero` decider — implement them honestly; they are the diagnostic payload.
Tests: write windows spanning packet boundaries, partially overlapping the timeline,
missing it entirely; counters must account for every frame.

Design decisions encoded (documented in `AmdtpPayloadWriter.cpp`):
1. **Per-frame count-and-skip** (per the Saffire one-buffer model): a partially
   coverable window is never rejected wholesale; every frame is individually written
   or counted into exactly one miss bucket, so
   `framesVisited == framesWritten + framesWithoutPacket + framesOutsidePacket` holds
   by construction.
2. Miss classification via a new timeline query, `ExposedFrameEnd()` — a monotonic
   high-water mark (one past the highest frame ever exposed) that survives
   publication/retirement/eviction. Miss at/beyond it = `framesWithoutPacket`
   (writer ahead of the packetizer); below it = `framesOutsidePacket` (packet
   existed, now published/retired/evicted — writer too late).
3. The host view is a **window into a ring** of `frameCapacity` frames
   (`interleavedFloat32` points at ring offset `firstFrame % frameCapacity`, matching
   the WriteEnd producer in `VirtualAudioDevice.cpp`); reads wrap modulo the
   capacity; `frameCapacity == 0` degrades to a flat buffer.
4. The codec emits logical quadlets; the writer is the payload's logical→bus
   conversion point and always serializes big-endian (the LE encoding pre-swaps).
5. Channel policy: host channels beyond `pcmChannels` dropped, missing host channels
   encode PCM zero, non-PCM (MIDI) slots never touched. Counters accumulate locally,
   published once per call with relaxed atomics — no per-frame RMW in the IO path.

Verified: bit-exact payloads for windows inside one packet, spanning three packets,
and wrapping the host ring; count-and-skip splits on partial overlap; ahead-vs-late
classification (empty timeline, far-future window, Published slot, ring eviction);
channel adaptation; MIDI-slot preservation; all three encodings; invalid views as
no-ops; and a 25 000-iteration lockstep run (600 000 frames, all written, counters
balanced).

#### Step 5 — Engine wiring (S–M) — ✅ DONE

`FakeIsochTxSlotProvider` real storage (drop stub), `DiceTxStreamEngine`
`Configure`/`PrepareNextTransmitSlot`/`WriteHostOutputFloat32` glue,
`FocusriteSaffireProfile` quirks (Raw24-in-32 per the DICE-branch codec), and the two
connections that were dead: `cadence_` selection in `Configure` (done in Step 3) and
`BindDiceTxEngine` in the controller.

This step turned out to be first implementation, not rewiring: the entire DICE layer
was header-only (`DiceTxStreamEngine`, `DiceStreamConfigMapper`, both profiles, the
registry), and `Stubs.cpp` — being in `Driver/` — wasn't even part of the tool target,
so the host build had no `FakeIsochTxSlotProvider`/controller implementations at all.

Design decisions encoded (documented in the respective `.cpp` files):
1. **Publish-at-prepare** (`DiceTxStreamEngine`): `PrepareNextTransmitSlot` exposes
   the packet on the timeline and immediately publishes the wire image to the
   provider; the backing bytes stay live (slot remains `ExposedForAudio`), so host
   PCM written later lands in the same storage until ring reuse — the Saffire model:
   a structurally valid packet whose payload is silence until audio arrives.
2. The engine is composition glue only (quirks → `AmdtpTxPolicy`, mapper →
   `AmdtpStreamConfig`, packetizer/writer/timeline bound to one provider); failed
   prepare advances no state and publishes nothing; counters are sticky.
3. `FocusriteSaffireProfile` quirks: host→device = `RawSigned24In32BE` (verified
   against the DICE-branch `RawPcm24In32Encoder` — 24-bit low-aligned, no label,
   host→BE swap), device→host = AM824-labeled; per-direction framing as observed on
   the wire. `GenericDiceProfile` = spec-shaped AM824 catch-all, owned by the
   registry so `GenericProfile()` never returns null.
4. `FakeIsochTxSlotProvider`: acquiring a ring position evicts its previous
   publication (the hardware-ring analog); `PublishSlot` records the
   `PreparedTxPacket` verbatim — the Step 6 verifier's hook point.
5. The controller wires the loop in `Initialize` (engine→provider,
   IO-path→engine) and falls back to the generic profile for unknown identities;
   `AudioIOPath` passes the WriteEnd view through unmodified (the writer owns ring
   arithmetic).
6. **`Stubs.cpp` is deleted** — everything it stubbed is now real, so the M1 exit
   criterion item is satisfied at Step 5 already.

Verified: config mapping, profile matching/quirks, registry lookup + generic
fallback, engine configure validation (direction, non-48k), prepare→publish→fill
byte-exact on the fake ring (CIP image, Raw24 payload with no label byte, SYT
stamping with a valid TX clock), acquire-failure counting with no cadence advance,
provider ring-reuse eviction, and controller end-to-end for both the Saffire path
(Raw24) and the generic fallback (AM824 labels).

#### Step 6 — `VerifyingSlotProvider` + scenario pump (M — the payoff)

Decorator asserting P1–P4 on every `PublishSlot`, violations to `IDiagSink` sticky
counters; a pump simulating WriteEnd sequences (regular 512-frame, irregular sizes,
skipped callback, sample-time jump) driving controller → engine → `Verifying(Fake)`.

**Milestone 1 exit criteria:** zero violations over ≥10⁶ cycles on the regular schedule;
adversarial schedules produce the *expected* counter signatures (a skipped callback shows
as `framesWithoutPacket`, never as silent corruption); `Stubs.cpp` deleted.

Rough shape: Steps 0–1 are about a day of mostly-mechanical work with high test density;
Steps 2–4 are the real engineering (the timeline↔writer interaction is where two-clock
bugs hide); Steps 5–6 are glue plus the instrument that outlives the lab.

### Milestone 2 — SYT realism

`TxTimingModel` + `SimulatedCycleTimeline` behind `ICycleTimeline`; constant device
sub-cycle graft (0x0b0) on the simulated timeline; verify transmit-lead bounds
(ship < 7620 ticks ≈ 2.5 cycles, warn ≤ 3071, no-data at ≥ 7620 — see P5); P5 green.
`WriteEndTraceReplayer` grows trace-file support.

### Milestone 3 — Dext bring-up

Enable ad-hoc signing; load (`systemextensionsctl developer on` + bench SIP/AMFI
posture); timer-driven ZTS; play audio at the virtual device; read verifier counters from
the StopIO dump. Answer O1–O3 and C1–C4 and **record the answers in this README**.
Exit: zero invariant violations over minutes of real HAL pacing; lifecycle matrix
documented. Capture a real WriteEnd trace and add it to the host regression suite.

### Milestone 4 — MIDI (optional)

`IOUserMIDIDestination::SetIOBlock` → UMP→MIDI1 translator → lock-free FIFO → AM824
`0x80`-label slot at WriteEnd. Only after Milestones 1–3 are green.

---

## Build

```bash
xcodegen generate                       # project.yml → .xcodeproj
xcodebuild -scheme ADKVirtualAudioLab        # dext (compile-only until signing enabled)
xcodebuild -scheme ADKVirtualAudioLabTests   # host test tool
```

Runtime prerequisites for the dext (Milestone 3): ad-hoc code signing enabled in `project.yml`,
`systemextensionsctl developer on`, and the bench machine's SIP/AMFI posture for unsigned
dext loading.

---

## Doctrine

- **Headers are truth.** No API claim enters this lab without a citation to the actual SDK
  header. LLM output and blog posts are topic outlines at best — this project exists partly
  because a research pass confidently fabricated API surfaces.
- **Wire compatibility is the correctness bar** for everything that graduates: the lab
  proves the math; the in-tree Linux reference (`firewire/`) and the bench prove the wire.
- **Lab verdicts are ADK-contract verdicts.** A green lab plus a broken ASFW means the bug
  is in an ASFW-specific seam the lab deliberately omits — that triage is the point.
