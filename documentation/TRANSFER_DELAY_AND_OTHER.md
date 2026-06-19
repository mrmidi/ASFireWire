# TRANSFER_DELAY_AND_OTHER.md — TX payload, SYT transfer delay, and the 85/15 silence

**Date:** 2026-06-12. **Bench:** Saffire Pro 24 DSP @ 48 kHz blocking, DICE branch.
**Evidence sources:** the earlier FireBug wire captures (counter `088:46xx` "capture 1",
`099:53xx`/"another capture" = "capture 2"), the current `032:3882..` FireBug excerpt, the
corresponding kernel logs (ZTS grid / DICE notify / ADK IO lines), payload-writer gauges,
`sound/firewire` (Linux kernel, authoritative per repo doctrine),
`libffado-2.5.0` (in-tree), the Saffire.kext RE corpus (`ZTS_BUFFERS_AND_IRS.md` §7), and IEC 61883-6.

This doc records three confirmed problems and one already-correct subsystem, each with its complete
evidence chain and fix vector. It is synchronized through the 20:11 post-wire-lead-guard run attached
on 2026-06-12. Read §2.8 for the newest result and §5 for how the timing constants compose.

---

## 0. Executive summary

| # | Problem | Status | Fix vector | Size |
|---|---------|--------|------------|------|
| A | TX PCM payload mangled on the wire (sign lost + 8 bits dropped) | **Wire-proven**, mechanism located | Declare **Float32** HAL stream format (lab parity), feed the already-correct `EncodeFloat32` path | small-medium |
| B | TX receiver lock is unstable even though the transfer-delay addend and cadence are present | **Guard experiment falsified**: modeled wire lead stayed positive, but ARX1 still flapped; the guard caused 24 timing reseeds | Keep the addend and wire-lead gauge; do not use the modeled floor as a reset policy; preserve the content cursor across timing reacquisition; investigate the execution-anchor/phase bridge | small fix + investigation |
| C | 85/15 silence (88.6 % of written frames are zero in both current snapshots) | **Narrowed**: wrap-mismatch and write-into-past ruled out; source-side silence implicated; repeated frame remapping was an additional self-inflicted discontinuity | One-shot frame alignment per stream; then steady-state delta gauges and source-side probe if residual | measurement-gated |
| D | Safety offsets / reported latency (§6.6 model) | **Already implemented and wired** | None (verify the start-time log line); couples into B/C as the host-side margin constant | none |

Fix order (with gates): **separate timing reacquisition from content mapping → re-measure B/C → A → source-side probe if C persists.**
Rationale in §6.

---

## 1. Problem A — TX PCM payload encoding

### 1.1 Wire evidence

Our stream is isoch **channel 0** (CIP SID 0 = node `ffc0`, DBS 9 = 8 audio slots + 1 MIDI slot).
The device transmits on channel 1 (SID 2, DBS 17). Capture 1, our data packets:

```
0000fe4a 0000fe54 0000fe6d 0000fe92 0000febf 0000feec 0000ff15 0000ff33   ← packet 1
0000ff43 0000ff45 0000ff3c 0000ff28 0000ff13 0000ff03 0000fef7 0000fef5   ← packet 2
0000fefd 0000ff11 0000ff30 0000ff53 0000ff75 0000ff95 0000ffb1 0000ffca   ← packet 3
0000ffe2 0000fffa 00000010 00000023 00000037 0000004b 0000005e 0000006d   ← packet 4
```

Read these as **signed 16-bit** values and you get a smooth waveform:
−438, −428, −403, … −54, −30, −6, **+16, +35, +55** … — it crosses zero continuously.
Read them any other way (24-bit unsigned, 24-bit signed, full int32) and the sequence has a
discontinuous jump at the `ffxx → 00xx` boundary. The signal is therefore 16-bit-quantized,
**zero-extended**, sitting in the low 16 bits of each slot.

What the wire *should* carry for this device: the Saffire.kext host→device capture
(documented in `PcmSlotCodec.cpp:41-48`) shows **raw sign-extended 24-in-32** samples, e.g.
`0xFFFC9F0C` — top byte is sign extension, no AM824 label. For the −438-ish sample above the
correct wire quadlet is `fffe4a00`. (The absence of a `0x40` MBLA label on our channel 0 is
**correct** — the `kRawPcm24In32` quirk selects `RawSigned24In32BE`; the device→host direction
uses labels, host→device does not. Matches the RE.)

Independent corroboration from the new gauges (§3.3): `maxAbs=16776674` = 2²⁴ − 542 = `0x00FFFDE2`
— a small *negative* sample (−542) appearing as a huge positive because its 24-bit value sits
zero-extended in an int32. Two instruments, same diagnosis.

### 1.2 Mechanism (exact chain)

1. `FillPcm24Format` (`ASFWAudioDriverGraph.cpp:83-96`) declares the HAL stream as
   LinearPCM, `FormatFlagIsSignedInteger | NativeEndian`, `mBitsPerChannel = 24` in a 4-byte
   frame, **no high-align flag** → 24-bit sample low-aligned in `int32`, pad byte zero.
   The HAL's float→int converter honors exactly that: the ring holds `0x00FE4A00`-style values
   for negative samples (zero pad, **not** sign extension — proven by the wire, see 1.1).
2. The RT write path (`ASFWAudioDriverIO.cpp:226-234`) hands that ring to
   `AmdtpPayloadWriter::WriteInt32Interleaved`, which calls
   `PcmSlotCodec::EncodeInt32(sample, RawSigned24In32BE)` per slot
   (`AmdtpPayloadWriter.cpp:119-124`).
3. `EncodeInt32` → `NormalizeSigned24` (`PcmSlotCodec.cpp:66-70`) does **`sample >> 8`** — written
   for *full-scale int32* input (its own comment: "Preserve the old int32 → float → signed24
   full-scale mapping"). Applied to a low-aligned 24-in-32 value it:
   - throws away the 8 LSBs and scales the signal by 1/256 (≈ −48 dB), and
   - because the ring value was zero-padded, never reconstructs the sign:
     `0x00FE4A00 >> 8 = 0x0000FE4A`.

Result on the wire: signal at −48 dB with every negative half-wave folded positive.
Decomposition check: ring `0x00FE4A00` → `>>8` → `0x0000FE4A` = observed, for **every** sample
in both captures, both polarities.

### 1.3 Chosen fix — Float32 stream format, lab parity (decision 2026-06-12)

> Rejected alternative (for the record): patching `EncodeInt32`/`NormalizeSigned24` to
> sign-extend low-aligned input (`NormalizeSigned24In32LowAligned`, `AM824Encoder.hpp:25`).
> It would produce correct wire bytes, but keeps the int24-in-32 declaration and the
> "who aligns/extends what" ambiguity that caused this bug. **Not the chosen direction.**

The chosen fix is to make the production driver match the lab, which has soaked clean for
19.5 minutes and whose payload writer is byte-for-byte the origin of ours:

- **Lab format** (`ADKVirtualAudioLab/Driver/VirtualAudioDevice.cpp:272-280`):
  LinearPCM, `FormatFlagIsFloat | FormatFlagsNativeEndian`, `mBitsPerChannel = 32`,
  `mBytesPerFrame = channels × 4`.
- **Lab writer** (`ADKVirtualAudioLab/Protocols/Audio/AMDTP/AmdtpPayloadWriter.cpp:76-137`):
  `HostAudioBufferView.interleavedFloat32` → `PcmSlotCodec::EncodeFloat32(sample, encoding)`.
- **Lab codec float path** (`PcmSlotCodec.cpp:23-48`, identical in both trees): clamp to ±1.0,
  symmetric scale by 2²³−1, round half away from zero, then either AM824 label or raw
  sign-extended 32-bit. **This path is already correct in our tree** — it just has no caller.
  `EncodeRawSigned24In32BE(−0.0000523…)` emits exactly the `0xFFFC9F0C`-shaped quadlets the
  Saffire.kext capture shows.

With Float32 declared, the HAL performs **no integer conversion at all**; the only
float→wire transform is ours, in one pass (preserves the ADK §6.5 "one transform pass" floor),
in code the lab already validated. The entire `NormalizeSigned24` question disappears.

### 1.4 Touch points

| File | Change |
|------|--------|
| `ASFWAudioDriverGraph.cpp:83-96` | `FillPcm24Format` → declare Float32/32-bit (lift the lab block verbatim); rename to match (`FillFloat32Format`) |
| `ASFWAudioDriverGraph.cpp:261-262` | Caller of the above — note it shapes **both** input and output formats (see scope note below) |
| `Runtime/AudioStreamMemory.hpp:14` | `const int32_t* outputBase` → `const float*` (and the frame-pointer helper at :55) |
| `Runtime/DirectAudioBindingSource.hpp:17` | same retype |
| `AmdtpTypes.hpp:48-49` | `HostAudioBufferView.interleavedInt32` → `interleavedFloat32` (lab already has this shape) |
| `AmdtpPayloadWriter.cpp:71-148` | `WriteInt32Interleaved` → `WriteFloat32Interleaved`; `EncodeInt32` → `EncodeFloat32`; non-zero detection + `maxAbs` move to float domain (or compute from the encoded 24-bit value to keep the counter integer-valued) |
| `DiceTxStreamEngine.{hpp,cpp}:101-104` | `WriteHostOutputInt32` → `WriteHostOutputFloat32` |
| `ASFWAudioDriverIO.cpp:226-234` | view construction picks up the new field/type |
| `tests/audio/AmdtpDirectTxTests.cpp:111` | test fixtures move to float source material. **Lab lesson applies:** use bipolar test material — positive-only samples cannot distinguish zero-padding from sign-extension (this exact blind spot let the lab codec bug survive initially) |
| `PcmSlotCodec.{hpp,cpp}` | `EncodeInt32`/`NormalizeSigned24` lose their last caller → delete (keep `Float32ToSigned24` + the three encoders) |

**Scope note — input direction.** `FillPcm24Format` shapes the input stream too
(`Graph.cpp:261`). If the input stream also goes Float32, the RX decode path
(`DirectRxPacketDecoder`/`RawPcm24In32Decoder` → capture ring) must produce float instead of
int32. Recommendation: convert **output first** (it is the broken, actively-debugged direction),
input in a follow-up commit for symmetry — CoreAudio handles per-direction formats independently,
and the input path currently works on the bench (it is the RX monitor we rely on).

### 1.5 Validation

- FireBug: our channel-0 audio slots show `ffxxxxxx`-style sign-extended quadlets for negative
  samples; amplitude back at the played level (no −48 dB attenuation).
- Gauge line: `maxAbs` in the new domain tracks played amplitude; bipolar test signal round-trips.
- Existing unit tests re-pass after fixture conversion.

---

## 2. Problem B — TX SYT transfer delay and unstable receiver lock

### 2.1 What the spec defines

IEC 61883-6: the SYT in a CIP header is the **presentation time** = quantized event (sample)
time + `TRANSFER_DELAY`. The receiver buffers the packet and presents it to the DAC when its
local CYCLE_TIME matches the SYT. The transmitter must get the packet onto the bus strictly
before that time (`packet_arrival ≤ event_time + TRANSFER_DELAY`).

- `DEFAULT_TRANSFER_DELAY` = **479.17 µs** = 354.17 µs (max bus traversal, sized so a stream
  survives a controlled short bus reset) + 125 µs (one nominal cycle).
- **Blocking** transmission adds the SYT_INTERVAL accumulation time `SYT_INTERVAL / fs`:
  729.17 µs @ 32 k, 660.58 µs @ 44.1 k, **645.84 µs @ 48 k**.
- Consumer receivers must support the default; **professional equipment may shorten it**.

In 24.576 MHz ticks (3072 ticks/cycle):

| Quantity | µs | ticks | cycles |
|---|---|---|---|
| Bus-traversal term | 354.17 | 8,704 (`0x2200`) | 2.833 |
| + one cycle = spec default | 479.17 | 11,776 (`0x2E00`) | 3.833 |
| Blocking addend @48 k (8 × 512) | 166.67 | 4,096 | 1.333 |
| **Spec blocking total @48 k** | **645.84** | **15,872 (`0x3E00`)** | **5.167** |

The blocking addend is a clean **4,096 ticks at every standard rate**
(`SYT_INTERVAL × ticksPerFrame`: 8×512 @≤48 k, 16×256 @96 k, 32×128 @192 k; 44.1 k family:
8×557.38… → 4,459 — Linux computes it as `TICKS_PER_SECOND * syt_interval / rate`).

### 2.2 Four-way triangulation

| Source | Value | Lead over transmit cycle |
|---|---|---|
| IEC 61883-6 blocking @48 k | 15,872 ticks | 5.17 cycles |
| **Linux** `sound/firewire/amdtp-stream.c` | 8,704 + 4,096 = **12,800 ticks** | 4.17 cycles |
| **FFADO** `config.h.in:159` `AMDTP_TRANSMIT_TRANSFER_DELAY` | 8,704 ticks (+ transmit window, see 2.3) | ~2.8–3.8 cycles |
| **Saffire device's own TX** (capture 1, ch 1) | SYT nibble = transmit cycle + 5 | ≈5.06 cycles |
| **Our TX** (`TxTimingModel`) | seeded at 3,072, gated < 7,620 | **1 … <2.5 cycles, drifts, ships negative** |

Reconciliation of the apparent disagreement between FFADO (8,704) and spec (11,776):
they are the **same number** — Linux line 303 computes
`transfer_delay = TRANSFER_DELAY_TICKS − TICKS_PER_CYCLE` (11,776 − 3,072 = 8,704) because its
`compute_syt` cycle arithmetic naturally contributes the +1-cycle term. All references agree on
substance; only bookkeeping differs.

Wire measurement method + caveat: FireBug stamps `sec:cycle:offset` per packet; lead =
`(SYT[15:12] − cycle mod 16) mod 16`. Capture 1: our nibbles 4/5/7 at cycles 4614/4615/4617 →
lead ≡ 14 (i.e. **−2**, presentation in the past); device nibbles d/f at 4616/4618 → **+5**.
Capture 2: ours a/b/c at 5369/5370/5371 → **+1**; device 9 at 5369 → 0. The cross-capture device
discrepancy (+5 vs 0) means absolute FireBug-cycle trust across captures is limited — but the
load-bearing observation is intra-capture and undeniable: **our lead is not a constant**, it
varies per session and can be negative. A presentation time at-or-behind the transmit cycle is
unusable by the DICE SRC.

### 2.3 How the references implement it (the part our port lost)

**Linux** (`/Users/mrmidi/DEV/FirWireDriver/sound/firewire/amdtp-stream.c`) — three pieces:

1. *Setup* (`amdtp_stream_set_parameters`, :303-307):
   ```c
   s->transfer_delay = TRANSFER_DELAY_TICKS - TICKS_PER_CYCLE;          // 8704
   if (s->flags & CIP_BLOCKING)
       s->transfer_delay += TICKS_PER_SECOND * s->syt_interval / rate;  // +4096 @48k
   ```
2. *TX encode* (`compute_syt`, :1019-1027) — the delay is added **at encode time, against the
   packet's actual OHCI transmit cycle** (`desc->cycle` from `compute_ohci_it_cycle`):
   ```c
   syt_offset += transfer_delay;
   syt = ((cycle + syt_offset / TICKS_PER_CYCLE) << 12) | (syt_offset % TICKS_PER_CYCLE);
   ```
3. *Symmetric inverse on capture* (`compute_syt_offset`, :484-503, used by `cache_seq` :519) —
   when Linux recovers a device's cadence from the received stream to drive its own TX (exactly
   our duplex DICE case), it **subtracts** the transfer delay from the received SYT-vs-cycle
   difference and stores **delay-free sub-cycle offsets** in the sequence cache (their analog of
   our `rxSytCadence`), including `CIP_SYT_CYCLE_MODULUS` borrow handling for the 4-bit cycle
   wrap. Round trip: `rx_offset = (deviceSYT − rx_cycle) − delay; tx_syt = tx_cycle + rx_offset + delay`.

**FFADO** (`libffado-2.5.0`): `transmit_at_time = presentation_time − transfer_delay`
(`AmdtpTransmitStreamProcessor.cpp:110`), window-guarded by
`AMDTP_MAX_CYCLES_TO_TRANSMIT_EARLY = 1` and `AMDTP_MIN_CYCLES_BEFORE_PRESENTATION = 1`
(`config.h.in:167-175`). Notably the delay is a **per-device config override**
(`dice_avdevice.cpp:880-888`, `xmit_transfer_delay`) — precedent for putting ours in the device
profile.

**Saffire.kext** (reconciling the prior RE in `ZTS_BUFFERS_AND_IRS.md` §7c-7d): the
3071/7620/12287 tick band RE'd from `FillFirewireBuffers` is a **fill-cursor-vs-DCL-execution
governor** (how far the fill loop's phase may run ahead of hardware before it inserts NO-DATA).
The *wire* SYT ends up at ≈+5 cycles because the encoded timestamp additionally carries the
grafted SYT-diff ring offset. Two distinct quantities. The kext governs one and transmits the
other.

### 2.4 Our defect, precisely

`TxTimingModel.hpp:12-16`:

```cpp
int64_t initialLeadTicks{3072};   // 1 cycle — Saffire FillFirewireBuffers seed
int64_t tightLeadTicks{3072};     // below: warning, still ships (negatives included)
int64_t acceptLeadTicks{7620};    // ≥ 2.48 cycles → gate: no-data + RESEED
```

Our port uses **one variable** — `phaseTicks_` — as both the fill governor *and* the wire SYT
(`decision.syt = SytForPhase(phaseTicks_)`, `TxTimingModel.cpp:75`). The constants are the
correctly-RE'd *governor* band; using them on the wire value clamps our presentation lead into
`[<2.5)` cycles, below FFADO's 2.83 floor, half the spec blocking value, and lets negative leads
ship (`kLate` is non-gating, Saffire-parity — but Saffire's parity applies to the governor, not
the wire). The transfer-delay term that separates the two quantities in every reference was
silently dropped in the port. Consequences observed: per-session lead lottery (−2 vs +1), device
SRC repeatedly acquiring/dropping our stream — the `EXT_STATUS` ARX1 lock flapping in the DICE
log (`ext=lock[ARX1]` ↔ `lock[none]` on nearly every notification) — and each flap/gate reseeds
the timing model, which re-rolls the frame-cursor mapping (couples into Problem C, §3.4).

### 2.5 Fix vector

**Minimal (do this first):** add the addend at encode only —

```cpp
// TxTimingModel: IEC 61883-6 TRANSFER_DELAY, Linux amdtp-stream.c parity.
// 8704 (= 0x2E00 default − 1 cycle, folded into the cycle arithmetic)
// + SYT_INTERVAL accumulation for blocking mode.
int64_t xmitTransferDelayTicks{8704 + 4096};   // 12,800 @ 48 k blocking

decision.syt = SytForPhase(phaseTicks_ + config_.xmitTransferDelayTicks);
```

- **Gate constants unchanged** — they keep governing the fill phase exactly as RE'd. Only the
  wire presentation moves out to spec. `decision.leadTicks`/health continue to measure the raw
  phase (governor semantics preserved).
- Rate ladder: `8704 + TICKS_PER_SECOND × SYT_INTERVAL / rate` (Linux :307 formula verbatim).
- Default **12,800** (Linux parity — proven DICE interop on Linux); the spec blocking value
  15,872 and the device's own ≈15.4 k are the upper reference if the device wants more. Make it
  a device-profile field next to `TxSafetyOffsetFrames` (FFADO precedent).
- Sub-cycle interaction: 12,800 mod 3072 = 512 ticks, so the grafted device sub-cycle phase
  shifts by a **constant** 512 ticks — still constant, which is the property that matters
  (the graft exists to track the device's sub-cycle *rate*, see memory
  `tx-syt-generation-requirements`).

**Full symmetry (follow-up, when touching the cadence path):** mirror Linux's capture side —
strip the transfer delay (with the cycle-modulus borrow handling of `compute_syt_offset`) when
building `rxSytCadence` entries, re-add at encode. This makes the cached cadence delay-free and
self-documenting, and removes the hidden coupling between RX recovery phase and TX presentation.

### 2.6 Original validation predictions

1. FireBug: our channel-0 SYT nibble sits at `transmit cycle + 4` **constant**, every capture,
   every session (vs today's per-session lottery).
2. The DICE `ExtStatus` ARX1 flapping stops; `ext=lock[ARX1]` becomes stable.
3. Gauge line: `withoutPkt`/`outsidePkt` collapse (reseed churn gone — §3.4).
4. `txPostLockNoDataPackets` stops climbing (no more gate-trips at 2.5 cycles).

The 19:49 run falsified predictions 1 and 2. The addend is active, but the raw phase can be late
enough to consume more than the full 12,800-tick delay. The corrected interpretation follows.

### 2.7 Current run — addend active, absolute wire phase still late

The new build contains the encode-time addend and delay-free RX comparison. The emitted channel-0
SYT sequence is cadence-correct:

```
c0b0, d4b0, e8b0, 00b0, 14b0, 28b0, 40b0, ...
```

Every data packet advances by `0x1400` = 4,096 ticks, exactly the 48 kHz blocking interval. The
remaining defect is absolute lead. Using the FireBug packet cycle and subcycle offset:

| Stream | Measured presentation lead |
|---|---:|
| Device channel 1 | +3.92, +4.24, +4.59 cycles, repeating |
| Our channel 0 | -2.14, -2.85, -2.51 cycles, repeating |

This is not random per-packet SYT jitter. It is a stable cadence placed about 6.7 cycles too late.
The runtime gauge independently reports the same failure:

```
txLead(last=-16988 min=-20569 max=2258)
```

At the last sample, adding 12,800 ticks still leaves `-4,188` ticks of wire lead. Therefore the
current non-gating `kLate` policy can emit a presentation timestamp behind the packet. DICE reacts
exactly as expected: all 12 visible `ExtStatus` reads alternate `ARX1, none, ARX1, none, ...`
(11 transitions in 11 adjacent pairs), with no slip bit set.

**B2 experiment:** preserve Saffire's negative raw-governor tolerance only while the receiver
still has a modeled presentation window:

```cpp
wireLeadTicks = leadTicks + xmitTransferDelayTicks;
if (wireLeadTicks < 3072) { // FFADO minimum: one cycle before presentation
    emit NO-DATA;
    reacquire phase;
}
```

This was deliberately separate from the upper `acceptLeadTicks` governor gate. The 20:11 run
falsified it as a stability fix and exposed a worse coupling; see §2.8. The lower gate has therefore
been removed while retaining `wireLeadTicks` as a diagnostic.

### 2.8 Current run — positive modeled wire lead, ARX1 still flaps

The lower wire-lead guard did what it was coded to do. The runtime gauge no longer showed a
receiver-facing timestamp behind the modeled packet anchor:

```
txLead(last=-6347 min=-9316 max=3691)
wireLead(last=6453 min=3484 max=16491)
```

The minimum modeled wire lead was 3,484 ticks, above the 3,072-tick floor. That did **not** produce
receiver stability. The eight visible DICE status reads are:

```
none, ARX1, none, ARX1, none, ARX1, none, ARX1
```

That is seven transitions in seven adjacent pairs, still with no slip bit. Therefore
`wireLeadTicks >= one cycle` in this model is not a sufficient lock condition, and repeated
reacquisition is not a valid corrective action.

The critical new evidence is the reset side effect. The excerpt contains **24**
`TX timeline seeded and aligned` messages. Every timing reseed also reassigned the audio frame
cursor:

```
deltaTicks:   1,112,064 .. 1,245,696
deltaFrames:      2,172 ..     2,433
frame jump:       2,168 ..     2,432
```

The magnitude itself is geometrically expected: the pump prepares about 384 cycles ahead, and
`384 cycles * 6 frames/cycle = 2,304 frames`. The defect is applying that initial bridge again
after the stream is already running. Timing phase recovery and audio-content continuity are
different state machines. A timing-model reseed may change future SYTs; it must not skip roughly
2,300 source frames.

The short FireBug excerpt supports the same separation:

- Channel 0 follows the correct blocking cadence (DATA, NO-DATA, DATA, DATA, DATA, NO-DATA, ...).
- Its data SYTs advance `a0b0, b4b0, c8b0, e0b0, f4b0, 08b0, 20b0, ...`: exactly 4,096 ticks
  per eight-frame packet.
- There is no random packet-to-packet SYT jitter in the visible window.
- Absolute FireBug packet timing is not trustworthy enough here to override the runtime anchor
  gauge: channel 1 also appears late under shortest-path decoding. Use the capture for cadence and
  continuity, not as an absolute lead oracle.

**Landed correction after this run:**

1. Remove the lower `wireLeadTicks` reset policy; keep the value and log as diagnostics.
2. Make frame-cursor alignment one-shot after `ResetForStart`.
3. Later timing reseeds preserve `nextAudioFrame_` and can no longer remap content.

This does not claim to solve ARX1. It removes a proven self-inflicted discontinuity so the next
bench run can measure the remaining timing fault without 24 content-cursor jumps mixed into it.

---

## 3. Problem C — the 85/15 silence

### 3.1 Architecture recap (why packets can legitimately ship zeros)

The direct TX path is *fill-in-place with a pre-zeroed default*:

1. The pump prepares packets up to `completionCursor + kTxPreparationLeadPackets` (384 packets
   = 48 ms) and publishes them to the DMA-shared ring **immediately**, PCM region zeroed
   (`clearPayloadBeforeExposure`, `DiceTxStreamEngine.cpp:86`, `ASFWAudioDriverZts.cpp:155-276`).
2. The RT writer later overwrites the PCM in place, mapping HAL `sampleTime` → packet via the
   timeline (`AmdtpPayloadWriter`).
3. A packet ships audio **iff** the write lands (a) in a still-exposed slot and (b) before the
   OHCI fetches the payload at transmit time. Anything else ships the prefilled zeros — by
   design (§6.1 of `ISOCH_AUDIO_ADK.md`: HAL-side underrun degrades to silence).

History for context: the *original* 85/15 (512-frame era) was the ring/ZTS wrap-mismatch class,
fixed by `79e45e92`; the suspected regression of that class (`512 % 48 ≠ 0`, FW-46) is closed by
the 2026-06-12 geometry (ZTS 192, ring 1536, `1536 % 192 = 0`, static_asserts restored in
`AudioTimingGeometry.hpp`) — confirmed live in the bench log (`period=192`, `outRing=1536`).
The silence survived that fix, so the wrap class is **ruled out** for the current bug.

### 3.2 The blind spot that was closed (step-1 instrumentation)

Until 2026-06-12 the writer's four health gauges (`framesWritten / framesWithoutPacket /
framesOutsidePacket / framesRacedReuse` — design doc §4.2 calls them "the health gauges") were
computed but consumed by nothing, and a fifth condition was *uncountable*: a write into a packet
that has already **transmitted** but whose timeline slot is not yet reused lands in valid memory,
increments `framesWritten`, and never reaches the wire (timeline ring 512 packets = 64 ms deep;
transmit happens ~48 ms in → ~16 ms blind band). The instrumentation added the
`wroteIntoTx` counter (frame's packet index vs `completionCursor` at write time) and a periodic
log line.

### 3.3 Measured result (first 256 write_end callbacks ≈ 0.4 s)

```
ADK writer/visited=18432 written=13456 withoutPkt=2952 outsidePkt=2024
           racedReuse=0 wroteIntoTx=0 nonZero=1536 slotsNZ=3072 maxAbs=16777188
```

The 20:11 run is effectively identical:

```
ADK writer/visited=18432 written=13448 withoutPkt=2958 outsidePkt=2026
           racedReuse=0 wroteIntoTx=0 nonZero=1536 slotsNZ=3072 maxAbs=16777203
```

The guard changed timing resets, not the payload result: exactly 1,536 non-zero frames again, with
the miss buckets moving by only 6 and 2 frames.

The current FireBug excerpt catches channel 0 during the silent portion directly: every visible
PCM word in its 296-byte data packets is zero; only the expected `0x80000000` MIDI/no-data slot
markers remain. This confirms what reached the wire in that window, but by itself cannot distinguish
silent source-ring frames from missed in-place overwrites; the writer counters make that split.

| Counter | Value | % | Reading |
|---|---|---|---|
| `wroteIntoTx` | 0 | 0 % | **Write-into-transmitted ruled out** (for this window). The feared race isn't firing. |
| `racedReuse` | 0 | 0 % | Seqlock discipline healthy. |
| `withoutPkt` | 2,952 | 16.0 % | Frames beyond `ExposedFrameEnd` — writer ahead of pump exposure. |
| `outsidePkt` | 2,024 | 11.0 % | Frames below the exposure mark but slot gone — writer behind the window. |
| `written` | 13,456 | 73.0 % | Landed in writable slots. |
| `nonZero` | 1,536 | **11.4 % of written** | **The headline.** 88.6 % of successfully-delivered frames were zero *in the source ring*. |
| `slotsNZ` | 3,072 | = 2 × nonZero | Exactly two non-zero slots per non-zero frame → stereo pair (and consistent with the known L==R residual). |
| `maxAbs` | 16,777,188 | = 2²⁴ − 28 | Zero-extended negative — independent confirmation of Problem A. |

### 3.4 Interpretation

- **Misses on both edges simultaneously** (`withoutPkt` *and* `outsidePkt`) is consistent with a
  **jumping or badly seeded frame↔packet mapping**, not a simple static lead adjustment. The mapping is set by
  `AlignFrameCursor` at timing-model seed (`ASFWAudioDriverZts.cpp:184-201`,
  `alignedFrame = lastZtsFrame + SYT-phase-delta/512`) and was re-rolled on every reseed.
  The 20:11 excerpt proves this happened 24 times, jumping the mapping by 2,168–2,432 frames each
  time. The packetizer now accepts this alignment only once per stream start.
- **The dominant effect is upstream of the writer**: 73 % of frames are delivered into correct,
  writable, not-yet-transmitted packets — and 88.6 % of them carry silence *read from the HAL ring
  at exactly the window `write_end` just announced* (`[sampleTime, sampleTime+72)` at
  `sampleTime % 1536` — the same indexing the HAL uses). That is `DICE_DEBUG.md` **Hypothesis A**
  (CoreAudio buffer silent at the frames we read), and it matches the historical decider
  ("~87.5 % of encoded packets read ZERO source, steadily").
- **Honest caveat:** `visited = 18,432 = 256 × 72` exactly — this is the *first* snapshot window
  (~0.4 s). If playback started mid-window, pre-playback silence legitimately inflates the zero
  ratio. The decisive form of this measurement is the **delta between two consecutive snapshot
  lines in steady state with audio playing continuously**.

### 3.5 Fix vector (measurement-gated)

1. **With one-shot frame alignment landed**, re-run the bench; take ≥2 snapshot deltas in steady
   state while playing.
   - A single `TX timeline initially aligned` line per stream confirms content continuity.
   - `withoutPkt/outsidePkt → ~0` would confirm the remapping contribution.
   - `nonZero` still ~10 % → proceed to source-side probes:
2. **Source-side probes**, in order of cheapness:
   a. Verify buffer identity: `binding.memory.outputBase` (already surfaced as a pointer in
      `DirectAudioDebugSnapshot.hpp:151`) vs the live `IOUserAudioStream` IO-buffer address —
      a stale mapping after a stream re-create would read a dead buffer (this exact class
      existed before: memory `adk-direct-binding-publish-bug`).
   b. Zero-run *shape*: count run-lengths of zero frames in the ring per window. Periodic runs
      (e.g. one IO period of audio per N) point at HAL clock pacing — i.e. back to ZTS anchor
      quality; random long runs point at the mixer genuinely idling.
   c. If runs correlate with `ZTS publish` grid lines: re-examine the anchor cadence
      (FW-53 territory), not the TX path.
3. **Deliberately not in scope:** tuning `kTxPreparationSlackPackets` or any lead constant to
   "help" — the doctrine stands (don't tune old constants to mask an unmeasured race;
   memory `tx-silence-investigation`).

Also relevant, kept from the earlier analysis (deferred, not invalidated): the frame-cursor seed
itself maps the frame domain via `lastZtsFrame + SYT-phase-delta`, i.e. **not** via the packet
execution timeline, so the write-to-transmit margin is unpinned by construction even though it
currently lands on the safe side (`wroteIntoTx=0`). The designed form, when we touch this next:

```
frameAtTransmit  = ZTS-frame at busToHost(AnchorForPacket(nextPacketToPrepare))
alignedFrame     = frameAtTransmit − TxSafetyOffsetFrames(rate)    // §4 constant, rounded to 8
```

`DextTxExecutionTimeline::AnchorForPacket` (`ASFWAudioDriverPrivate.hpp:59-95`, OUTPUT_LAST
closed loop, Linux ohci.c:3055 analog) already provides the anchor; the change is confined to
the seed block. Do this only after B + the re-measurement, with `wroteIntoTx` as the regression
guard.

---

## 4. Problem D — safety offsets & reported latency (already correct, for completeness)

The §6.6 model (RE'd from Saffire.kext `UpdateIsochBufferParams` @0xf506) is **already
implemented and wired**:

- `FocusriteSaffireProfile.cpp:69-107`: `TxSafetyOffsetFrames = (6 + rateAddend) × framesPerPacket`
  → **48 frames @48 k** (latencyMode 1); `RxSafetyOffsetFrames = (16 + rateAddend) × fpp` → 128,
  raised to the **256-frame input floor** (`kInputSafetyFloorFrames`, one interrupt group +
  jitter) in `BuildAudioGraph`. Reported-latency ladder 29/59/119 kept separate. ✓
- Applied at `ASFWAudioDriverGraph.cpp:495-498` (`SetOutputSafetyOffset` / `SetInputSafetyOffset`).
  Runtime verification: the `"Reported HAL latency out=… safety out=48/in=256 frames"` log line.
- The attached excerpt contains only the later generic
  `TimingCursorPolicy ... outSafety=8 inSafety=8` snapshot. That line reports the fallback policy,
  not the device-profile values already applied in `BuildAudioGraph`; the applied-HAL line is
  outside the excerpt. Do not read the `8/8` diagnostic as proof that the Saffire profile was
  bypassed.
- Cross-check against the FFADO-successor protocol crate
  (`snd-firewire-ctl-services/protocols/dice`): **no latency/delay register exists anywhere in
  the DICE/TCAT model** — independent confirmation that `delayPackets` is host policy (a fixed
  `latencyMode × rate` table), not device config. Nothing to read from the device.
- Empirical note from the bench log (worth keeping in mind for §3): comparing `write_end` host
  times against the ZTS grid, the HAL's write for window `[F, F+72)` completes at
  `t(F) ± ~200 µs` — i.e. the *oldest* frame of each window has ≈zero margin against its nominal
  presentation time, and **the writer's entire real budget is the declared safety offset**
  (48 frames ≈ 1 ms at mode 1). If post-B measurements show pressure here, the §6.6 knob is
  `latencyMode` (mode 2–3 → 80–112 frames ≈ 1.7–2.3 ms), not a new constant.

---

## 5. The unified timing model (what each constant pins)

```
            ───────────────  bus / wire domain  ───────────────►
 packet K fetched           packet K on wire            device presents F
 by OHCI ≈ T(K)             at cycle T(K)               at SYT(F)
      │                          │                          │
      │                          │◄── TRANSFER_DELAY ──────►│   Problem B
      │                          │    (12,800 ticks @48k,       SYT = tx cycle + delay
      │                          │     constant, spec/Linux)    + cadence sub-phase
      │                          │
 ─────┼──────────────────────────┼───  host / HAL domain  ──►
      │                          │
   HAL write_end for F      ZTS time t(F)
   completes ≈ t(F)              │
      │◄── output safety ───────►│                            Problem D (done)
      │    offset (48 fr @48k)                                device may not consume F
      │                                                       before t(F)+safety
      │
 frame↔packet bridge: alignedFrame seed                       §3.5 next item
 (now one-shot: lastZtsFrame + SYT-phase delta;               pins T(K(F)) − t(F)
  designed: execution anchor − safety offset)                 to a chosen constant
```

Three constants, three independent jobs:

1. **TRANSFER_DELAY** (Problem B) — device-facing: how far the *presentation stamp* leads the
   wire packet. Spec-defined, reference-implemented, now present; absolute phase remains unstable.
2. **Output safety offset** (Problem D) — host-facing: how much slack the HAL guarantees between
   writing a frame and the device consuming it. Implemented.
3. **Frame-cursor seed** (§3.5) — the bridge: which packet carries which frame, hence whether
   the safety offset's promise is actually honored by the transmit schedule. It is now one-shot
   per stream, but the formula remains unpinned to the execution anchor; designed form specified.

Problem A is orthogonal (payload content, not timing) and Problem C is the *observable* whose
remaining cause the state-separation fix + delta measurement will isolate.

---

## 6. Execution order

| Step | Action | Gate to next |
|---|---|---|
| 1 | **Landed:** remove the falsified lower reset guard; make frame alignment one-shot per stream | One initial alignment log; no later content remaps |
| 2 | **B/C measurement:** ≥2 steady-state gauge deltas plus a longer FireBug window while playing | Reseed count, ARX1 transitions, `without/outsidePkt`, `nonZero` ratio |
| 3 | **A:** Float32 stream format + float writer path (lab parity), output direction first | Wire shows sign-extended quadlets at played amplitude; tests green |
| 4 | If `nonZero` still low: source-side probes (§3.5.2 a→c) | identified leg |
| 5 | Pin initial seed alignment to the execution anchor (§3.5 formula); `latencyMode` bump only with evidence | `wroteIntoTx` stays 0 |
| 6 | Follow-ups: input-direction Float32; full Linux cadence symmetry (strip/re-add); revisit lead-health bands as governor-only semantics | — |

Why measurement now precedes A: the lower guard was falsified, and the current code removes a
specific discontinuity that contaminated both timing and payload observations. One clean run is
needed to establish what remains before changing the sample format.

---

## 7. References

**Our tree**
- `ASFWDriver/Audio/Wire/AMDTP/PcmSlotCodec.cpp:23-85` — float path (correct, currently uncalled) / int path (`>>8`, Problem A)
- `ASFWDriver/Audio/Wire/AMDTP/AmdtpPayloadWriter.cpp:71-174` — RT writer + gauges
- `ASFWDriver/Audio/Wire/AMDTP/TxTimingModel.{hpp,cpp}` — phase/lead model (Problem B fix site)
- `ASFWDriver/Audio/DriverKit/ASFWAudioDriverZts.cpp:155-276` — pump; `:184-201` — frame-cursor seed
- `ASFWDriver/Audio/DriverKit/ASFWAudioDriverGraph.cpp:83-96, 261-262, 460-507` — stream format, offsets
- `ASFWDriver/Audio/DriverKit/Config/DICE/Isoch/Profiles/FocusriteSaffireProfile.cpp:69-107` — §6.6 values
- `ASFWDriver/Audio/DriverKit/ASFWAudioDriverPrivate.hpp:55-95` — `DextTxExecutionTimeline::AnchorForPacket`
- `ASFWDriver/Shared/Isoch/AudioTimingGeometry.hpp` — 2026-06-12 geometry + static_asserts
- `ADKVirtualAudioLab/Driver/VirtualAudioDevice.cpp:272-307` — the Float32 format block to lift
- `ADKVirtualAudioLab/Protocols/Audio/AMDTP/AmdtpPayloadWriter.cpp` — float writer (target shape)

**References (external, local checkouts)**
- Linux: `/Users/mrmidi/DEV/FirWireDriver/sound/firewire/amdtp-stream.c` — `:29` (0x2E00),
  `:303-307` (setup), `:484-503` (`compute_syt_offset`, capture-side strip), `:519-545`
  (`cache_seq`), `:1019-1027` (`compute_syt`, encode-side add)
- FFADO: `libffado-2.5.0/config.h.in:159,167,175`; `src/libstreaming/amdtp/AmdtpTransmitStreamProcessor.cpp:95-230`;
  `src/dice/dice_avdevice.cpp:880-888` (per-device override precedent)
- DICE register model (negative result): `/Users/mrmidi/DEV/FirWireDriver/snd-firewire-ctl-services/protocols/dice` — no latency/delay registers
- Saffire.kext RE: `ZTS_BUFFERS_AND_IRS.md` §7 (FillFirewireBuffers/adjustOutputPhase), IDA DB per memory `saffire-kext-ida-path`

**Repo docs this extends:** `ISOCH_AUDIO_ADK.md` (§4.2, §5.4, §6.1, §6.5, §6.6),
`ZTS_BUFFERS_AND_IRS.md` (§5, §7), `DICE_DEBUG.md` (decider table, Hypothesis A).

**Spec:** IEC 61883-6 (TRANSFER_DELAY, blocking/non-blocking delivery rules, SYT semantics);
IEC 61883-1 (CIP header, SYT field encoding).
