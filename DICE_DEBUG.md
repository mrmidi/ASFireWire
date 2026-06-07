# DICE TX Silence — Debug Analysis

_Branch: `DICE` · Snapshot: 2026-06-06 · Device: Focusrite **Saffire PRO 24 DSP**_

---

## UPDATE 2026-06-07 (later) — TX timeline offset fix + Saffire output-lead RE

### The "reading wrong address" bug, mechanically

The TX consumer was emitting silence because `audioFirst = timelineFirstFrame − txOutputOffsetFrames_`
landed **ahead of CoreAudio's write head**, in a ring slot that still held the previous
4096-frame lap. Not a bad pointer — the *right physical slot, wrong generation*.

- Ring is 4096 frames, addressed `mod 4096`; slot reused once per lap.
  `SlotForFrame`/`GenerationForFrame` in `IsochAudioTxPipeline.hpp:291/305`, coverage check
  at `IsochAudioTxPipeline.cpp:1360`.
- Real log: `expected=[673792,673800) gen=165  stamp=[669696,669704) stampGen=164` →
  `missToStamp=4096` (one lap), `lagToWritten=−732` (audioFirst past `writtenEnd`).
- Root cause: `sched − wr ≈ 4840` (wire clock leads HAL write head; observed **4436–4856**,
  drifts run-to-run) while the offset was only **3072** → `audioFirst` overshoots the writer
  by ~1768 → future → stale slot → silence.

### Fix applied (uncommitted, tests green)

- `txOutputOffsetFrames_` seed **3072 → 5120** via new named constant
  `IsochAudioTxPipeline::kTxOutputOffsetFrames` (hpp). 5120 > max `sched−wr` so `audioFirst`
  lands behind the writer; effective lead ≈ `offset − (sched−wr)` ≈ **256–288 frames (~6 ms)**,
  not 5120 of latency.
- Killed the **duplicated** seed-and-silence-fill path: `SetDirectTxRuntimeBinding` and
  `ResetForStart` both had a full copy → single `SeedTxTimeline(oldestValid)` helper.
- Tests now reference the constant (no re-hardcoded `3072`/`3080`/`3120`/`3840`). All 1197 pass.
- **Stopgap, not the cure** — see RE below.

### Simulators

- `tools/debug/asfw_tx_math_sim.py` — parses kernel logs, tunes offset from captured data.
- `tools/debug/asfw_tx_ring_sim.py` — **forward** two-clock sim (no hardware): reproduces the
  bug from first principles (3072 → 0% covered, `missToStamp=4096`; 5120 → 100%) and shows the
  fixed offset fails again if `--gap` exceeds the window (run-to-run fragility).

### Saffire output-lead RE — the durable design (the real reveal)

Decompiled `FillFirewireBuffers` (0xE778) + `adjustOutputPhase` (0xC9C2) in `Saffire.i64`.
Focusrite's **device-facing output lead is bounded to 1–4 isoch cycles**, not thousands of frames:

```c
v23 = (v24 + 3072) % mod;                 // seed = current device phase + ONE cycle (3072 ticks)
v23 = adjustOutputPhase(a1, v89, v24, v23); // deadbanded slewing PLL (a2+392 = deadband)
v25 = extOffsetDiff(v23, v24);            // LEAD in 24.576 MHz ticks
if (v25 < 7620) { if (v25 <= 3071) warn; ...ship DATA, SYT from v23... }   // accept ≈2.48 cyc
else            { if (v25 > 12287) warn; v23 = -1; }                       // >4 cyc → NO-DATA
```

Unit: `3072 ticks = 1 cycle = 6 frames` (512 ticks/frame). Phase wraps at
`0xBB80000 = 4096×48000` ticks (8 s). So in frames the lead window is **6 → ~15, hard ceiling 24**.

**Why theirs is tiny and ours was 5120:** Saffire re-derives output phase **every send-callback**
from the recovered device master clock (`dev+1570780`) and pulls PCM at the matching client-buffer
index — timestamp and data share one per-callback phase, so there is **no free-running timeline
drifting from the write pointer**, hence no `sched−wr` gap to cancel. Our 5120 compensates for a
two-clock gap the reference design simply doesn't have.

**Doctrine:** deploy 5120 to confirm the misses collapse, but the target architecture is
output-phase = recovered-clock + ~1–2 cycles with per-callback data pull (offset ≈ 6–15 frames,
immune to `sched−wr` variance). Also confirms FW-26: `(*(a1+1570752) + 256) & 0x1FF` = a
**512-entry SYT-cadence ring seeded at its midpoint (256)**. Full detail in memory
`saffire-output-lead-bounded-cycles`.

---

## UPDATE 2026-06-07 — decider fired + Saffire IT-stack comparison

The split-snapshot logging fix landed (the old one-line `ADK snapshot` was truncated
by os_log at `writeFrames=`, dropping every TX counter; now 3 lines `/io` `/ring`
`/tx`). The decisive counters across **3 device-start sessions** (11:39 / 11:40 / 11:41):

| session @t | pcmNZ | pcmZero | %NZ | disc | epoch bumps |
|---|---|---|---|---|---|
| 11:39 ≈6 s | 4612 | 32254 | **12.5 %** | 2 | 2 |
| 11:40 ≈12 s | 8704 | 60916 | **12.5 %** | 1 | 1 |
| 11:41 ≈6 s | 4680 | 32168 | **12.7 %** | 0 | **0** |

**Findings:**
1. **Decider: ~87.5 % of encoded packets read ZERO source, steadily** → source-side
   (Hypothesis A), **not** stale DMA. `faults=0 underrun=0`, no fatals. The 12.5 %
   is *exactly* 1/8 (= framesPerPacket = channels) and content/epoch-independent →
   smells **structural** (read-position/stride), not "quiet music."
2. **Epoch-retire is NOT the steady click driver.** The 11:41 session had **zero**
   epoch bumps and the **same** 87.5 % zero. Epoch bumps are rare (0–2/session)
   startup-transition events. (Corrects the earlier "epoch = prime suspect" lead.)
3. **ARX1 lock flaps every ~200–400 ms, all sessions** (`lock[none]`↔`lock[ARX1]`)
   → device can't hold lock on our TX → separate **TX SYT/timing** problem (P2),
   independent of the zero payload.

So the two real bugs are **(P1) source 87 % zero** and **(P2) ARX1 won't hold
lock**. Next probe for P1: enable `ASFWEnableIsochTxVerifier=true` for per-packet
`IT TX PREP src=[hex]` **with a known full-scale tone** to split "wrong region"
vs "silent content"; also log isoch binding base vs HAL `outBase`.

### Saffire IT-stack vs ours (RE'd via `Saffire.i64`, idalib)

Saffire TX = **one** sample buffer (the DCL payloads), three `IOAudioEngine`
callbacks on a single coordinate (absolute sample frame mod ring) vs the **true**
hardware position:
- `getCurrentSampleFrame` (0x2788) — real HW DMA pos, not extrapolated.
- `clipOutputSamples` (0x16580) — float→int24 into `*(a3[43]+8*pkt)+8` at
  `firstSampleFrame`; **no ready-gate, no silence branch**.
- `eraseOutputSamples` (0x2d3a) — `bzero` the consumed region; silence is the
  default wherever clip hasn't written.
- `FillFirewireBuffers` (0xe778) — per-packet CIP+SYT from **live device-clock
  recovery** (`tstampToOffsets` + `adjustOutputPhase` deadband + 512-cadence ring);
  ships whatever clip left. Discontinuity → re-prime flag + counter, **never** dumps
  audio to silence.
- **No epoch, no Pending gate, no affine source-pin, no second cursor.** Clock
  wander is absorbed by SYT/phase only, never by the PCM path.

Ours layers **two clocks + once-per-epoch affine pin + retire-to-silence** on top of
what should be one shared ring cursor. **Epoch-removal draft** (Saffire-shaped, NOT
yet applied): the affine map is just a fixed offset `O = epochAnchorSource −
epochAnchorTimeline` ⇒ `sourceFirst = timelineFirstFrame + O`. Replace all epoch
state with `bool sourceMapped_` + `int64_t txSourceTimelineOffset_` pinned once;
delete `currentEpoch_/epochAnchored_/epochAnchor*/lastDiscontinuityGeneration_`,
slot `epoch`, the discontinuity block (`IsochAudioTxPipeline.cpp:1219`),
`RetiredEpochSilence`, both `epoch!=currentEpoch_` checks; disconnect
`playbackRingDiscontinuityGeneration` from the PCM mapping; downgrade
`TxSourceOverwritten`/`TxReadAhead` FATALs to per-packet silence. **This is hygiene —
it does NOT fix P1/P2.** Full write-up in memory `tx-it-stack-vs-saffire-epoch-removal`.

---

## Problem statement

Host→device isochronous audio (our TX, isoch **channel 0**) is on the wire with
**correct framing but all-zero PCM payload**. The user hears only periodic very
short clicks. The device (channel 1, device→host) transmits real audio.

FireBug capture (truncated) shows our ch0 DATA packets as:

```
003:2105:2560  Isoch channel 0, tag 1, sy 0, size 296   00090078 9002f0b0 00000000 00000000
003:2106:2620  Isoch channel 0, tag 1, sy 0, size 296   00090080 900200b0 00000000 00000000
```

CIP is valid (`fmt=0x10` AM824, `fdf=0x02` 48 kHz, `SYT=0xf0b0`/`0x00b0`…, not
`0xFFFF`), `DBS=9`, `DBC += 8`, N‑D‑D‑D cadence, 8 frames/packet. Only the
AM824 sample quadlets are zero.

## What is NOT the bug (confirmed)

| Ruled out | Evidence |
|---|---|
| Wire framing (CIP/DBC/SYT/cadence/geometry) | FireBug: valid, matches AMDTP 48 kHz blocking |
| PCM format (low-aligned signed 24-in-32) | ADK advertises it; `RawPcm24In32Encoder` consumes it; internally consistent |
| Pipeline stalled / fatal teardown | `playRd` advances 130840→261928 at exactly 48000 fr/s; no `ADK FATAL` |
| Stuck in startup silence | `playbackRingReadFrame` only advances on **completed prepared** slots → epoch anchored, prep + completion running every packet |
| Shadow-buffer indirection (patch X, send Y) | `IsochTxDmaRing.cpp:322` `lastDesc->dataAddress = payloadIOVA`; `IsochTxDescriptorSlab.hpp:53/67` `PayloadPtr`/`PayloadIOVA` are virtual/IOVA views of the **same** DMA buffer; `PublishToDevice` flushes it |
| 768-frame lead "wrong / really 1152" | `PacketLeadFrames(Output)=768` (`TimingCursorPolicy.hpp`) is enforced at **transmit** time; observed `playWr−playRd ≈ 1200` = lead + ring transit, expected pipeline depth |

## Architecture / data flow

```
CoreAudio HAL  --writes interleaved int32 frames-->  shared output ring (4096 frames)
   |  WriteEnd(sampleTime,frameCount)
   v
AudioClientCursor.PublishWriteEnd        AudioClientCursor.hpp:40   (writtenEnd = sampleTime + frameCount)
   |
PublishPlaybackRingWriteEnd              ASFWAudioDriverIO.cpp:19   (oldestValid = writtenEnd - capacity)
   |
ScheduleTxPayloadPreparation (wake)      ASFWAudioDriverIO.cpp:50
   |
IsochTransmitContext: Refill, then DrainPreparationRequests   IsochTransmitContext.cpp:265-300
   |                                                            (Refill builds silence; prep patches after)
   v
IsochTxDmaRing::PreparePayloads          IsochTxDmaRing.cpp:425
   |  walks slots from HW command pointer; writable = distance>64, guard<=4
   v
IsochAudioTxPipeline::PreparePayload     IsochAudioTxPipeline.cpp:1143
   |  anchor: source = writtenEnd - 768; source advances 1:1 with timeline
   |  reads CoreAudio frame via directOutputReader_.Frame(sourceFirst+n)
   v
TxAudioPacketWriter::WritePacket         TxAudioPacketWriter.cpp:14   (reader_.Frame() -> EncodeDirectTxPcmFrame)
   |  PublishToDevice + barrier (IsochTxDmaRing.cpp:494)
   v
OHCI IT DMA  -->  FireWire bus (ch0)
```

### Source read mapping is coordinate-consistent

- `AudioStreamMemory::OutputFrame(n)` = `outputBase + (n % outputFrameCapacity) * outputChannels` (`AudioStreamMemory.hpp:49`).
- `outputFrameCapacity` = HAL ring size (4096, matches `outRing=4096` in logs).
- `writtenEnd` is in CoreAudio `sampleTime` units (`= sampleTime + frameCount`).
- `sourceFirst ≈ writtenEnd − 768`, well within `[writtenEnd−4096, writtenEnd)`.
- `memory.outputChannels` = HAL `snapshot.outputChannels` (`IsochTransmitContext.cpp:436`); encode lanes `pcmChannels` (8) ≤ stride (8). **No stride mismatch.**

**Conclusion:** if CoreAudio wrote audio, we read audio. So zero on the wire ⇒
either the source is zero, or the DMA transmit is stale.

## The decisive disambiguator

Both the encoder (`TxAudioPacketWriter.cpp:110`) and the **anyNonzero audit**
(`IsochAudioTxPipeline.cpp:1388-1400`) read the *same* source via
`directOutputReader_.Frame()`. The audit feeds two counters
(`IsochAudioTxPipeline.cpp:1413-1419`):

- **`txPcmNonzeroPackets` (`pcmNZ`)** vs **`txPcmAllZeroPackets` (`pcmZero`)**

| Outcome | Meaning | Root area |
|---|---|---|
| `pcmNZ ≈ 0`, `pcmZero` ~100% | CoreAudio buffer is silent at the frames we read | **Hypothesis A** |
| `pcmNZ` high, wire still zero | We encoded real PCM but OHCI shipped stale zeros | **Hypothesis B** |

### Hypothesis A — source is silent (MOST LIKELY)

Sub-causes: (1) nothing is actually playing / device isn't the selected default
output; (2) **`binding.outputBase` is not the live HAL IO buffer** — the DICE
direct-binding is "late-bind pending" (see `audio-isoch-queue-boundary` memory).
The cursor/`writtenEnd` come from the shared control block (so they look healthy)
while `Frame()` could read a stale/placeholder page. Periodic clicks = transients
during (re)binding / epoch re-anchors.

### Hypothesis B — stale / incoherent DMA transmit (less likely)

Prep only patches slots `distance > kPreparationDeadlinePackets` (64 packets ≈
8 ms) ahead of the HW command pointer (`IsochTxLayout.hpp:51-52`,
`IsochTxDmaRing.cpp:453-457`); OHCI does not prefetch payload that far ahead.
Same-buffer descriptor + `PublishToDevice` flush. The completion hash
(`IsochTxDmaRing.cpp:391`) compares CPU-visible memory before/after — it **cannot**
prove what OHCI fetched, so B can't be excluded by existing checks, only by
observing actual bytes (TX verifier `src=[…] wire=[…]`).

## DICE notifications — what the device is telling us

- Device repeatedly writes `bits=0x40` (~5/s) to host notify address `0x0100000000`.
- `0x40` = `Notify::kExtStatus` ("extended/clock status changed → read EXT_STATUS").
  Not a standard FFADO notification bit (FFADO documents only bits 0–5).
- The 104-byte `ReadGlobalState` at `0xFFFFE0000028` is the GLOBAL section
  (base + 0x28, after a 10-quadlet pointer table). Within it:
  - `NOTIFICATION` @0x08 reads `0x40` (still set — we never clear it).
  - `STATUS` @0x54: bit0 = source locked, [15:8] = nominal-rate index.
  - `EXT_STATUS` @0x58: lock bits AES0-3=0-3, ADAT=4, TDIF=5, **ARX1-4=6-9**, WC=10;
    slips in high half (ARX1 slip = bit22). **ARX1 = our host→device TX stream.**
    So `EXT_STATUS & 0x40` = "device locked to our transmit."
- The periodic `[DICE] Global:` reads are `DiceAudioBackend::ProbeDuplexHealth`
  (`DiceAudioBackend.cpp:161`) reacting to each notification via the mailbox
  observer (`DiceAudioBackend.cpp:54`): it reads STATUS/EXT_STATUS and recovers if
  degraded. Previously it logged raw hex and was **silent on the healthy path**.
- We do **not** write-1-to-clear `GLOBAL_NOTIFICATION`; **neither does FFADO**
  (it only reads it). Per project doctrine, do NOT add a speculative clear-write
  (untested bus behaviour; could be feeding the observed AT `evt_timeout`s).
- Nickname decodes (byte-swapped per quadlet) to **"Pro24DSP-…"** — the
  `[DICE] Global:` log previously printed it with wrong byte order.

Decode connects to silence: if `EXT_STATUS.ARX1` is **flapping or unset**, the
device can't hold lock on our transmit → a timing/SYT problem (consistent with
periodic clicks). If ARX1 is solidly locked and only AES/ADAT/WC toggle, the
notifications are benign unplugged-input chatter and the zeros are pure
Hypothesis A.

## Instrumentation landed this session (uncommitted; builds green)

| File | Change |
|---|---|
| `DICETypes.hpp` | Full `ExtStatusBits` map (AES/ADAT/TDIF/ARX1-4/WC + slips); pure decoders `FormatGlobalStatus` / `FormatExtStatus` / `FormatNotification` (snprintf into caller buffer) |
| `DICETransaction.cpp:197` | `[DICE] Global:` now logs `clock=LOCKED 48000Hz ext=lock[ARX1|…] slip[…] notify=ExtStatus` |
| `LocalRequestWiring.cpp:124` | notification entry now logs decoded `meaning=ExtStatus` (single point every notification passes through) |
| `DiceAudioBackend.cpp` `ProbeDuplexHealth` | logs decoded confirm on **every** notification — healthy (rate-limited 1/s) and `DEGRADED … -> recover` |
| `ASFWAudioDriverIO.cpp` WriteEnd | **resurrected** `MaybeLogDirectAudioDebugSnapshot` (was defined but **never called**; only `ForceLog` fired at lifecycle). Self-throttled to 5 s; modulo bounds RT capture cost |

Deliberately **not** done: write-1-to-clear of `GLOBAL_NOTIFICATION` (needs
`Saffire.kext`/FFADO confirmation first — IDA available).

## Diagnostic plan (next capture)

Rebuild **with version bump** (`./build.sh`), then:

```bash
log stream --predicate 'process == "kernel"' \
| grep -E "ADK snapshot|ADK TX FIRST PCM|ADK TX ANCHOR|ADK FATAL|ADK FORCED|\[DICE\] Global: clock=|DICE notification quadlet|DiceAudioBackend: (notify|lock health)"
```

For per-packet `src=[hex] wire=[hex]` (Tier 2), set Info.plist
`ASFWEnableIsochTxVerifier=true` and add `|IT TX PREP|ADK TX STARTUP DEFER|ADK TX PREP`.

| Grep token | Answers |
|---|---|
| `ADK snapshot` | `pcmNZ` vs `pcmZero` (decider); `prep/pending/startup/retired`; `faults(...)` |
| `ADK TX FIRST PCM` | first prepared packet `src=[…] wire=[…]` (one-shot) |
| `ADK TX ANCHOR` | epoch anchored, `source`/`timeline`/`lead` |
| `[DICE] Global: clock=` | `clock=LOCKED/UNLOCKED`, `ext=lock[ARX1…] slip[…]` |
| `DiceAudioBackend: notify` | confirmed health per notification |
| `IT TX PREP` (verifier) | per-packet source + encoded bytes |

### Reading it

- `pcmNZ` climbing + `ext=lock[…ARX1…]` steady, but FireBug still zero → Hypothesis B (confirm with `IT TX PREP src` non-zero).
- `pcmZero` ~100%, `pcmNZ≈0` → Hypothesis A; check `ADK TX ANCHOR`/`startup` and `binding.outputBase` vs the live stream buffer.
- `ext` missing `ARX1`, or `slip[ARX1]`, or `DEGRADED` lines → device can't hold lock on our TX → timing/SYT, not content.

## Open follow-ups

- Confirm `binding.outputBase` at bind time equals the live HAL IO buffer CoreAudio writes (DICE late-bind seam).
- Delete dead cursor builder `TryBuildPlaybackRingPacket` / `InitializeDirectOutputCursor` (`IsochAudioTxPipeline.cpp:606/627`).
- Align geometry tests to the real 128-frame WriteEnd cadence.
- Decide (with reference confirmation) whether to ack/clear `GLOBAL_NOTIFICATION`.
- `ProbeDuplexHealth` clang-tidy cognitive-complexity is 29 (>25) after the decode logging — optional extract-helper cleanup.
