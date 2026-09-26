# TX ownership: PCM → packet → DMA (milestone 6)

**Status:** T0–T4 done (design, goldens, dead surfaces, silence-first + `[TxPlace]`, audio-side fill), 2026-09-26, branch `refactor/tx-ownership` off main `52d05ca8`.
Linear FW-209 (milestone 6), condensing FW-210 (main inventory), FW-211 (midi distillation) and FW-212 (ownership
model). The stages are in §8. This document is T0.

**Goal.** One owner at every step from CoreAudio's output to a retired DMA slot. PCM enters a packet exactly once,
by one component, at one known point in time.

Every fact below names its source. Hardware facts carry device, rate, buffer and build. "Hypothesis" means
unmeasured.

## 1. What we measured before changing anything (B0, partial)

All on a Saffire Pro 24 DSP with an electrical loopback from output 1 to input 1, on the PR #150 build (main
`52d05ca8` is the same code). Details are in the B0 notes.

**Round-trip latency, 512-frame client buffer.**

| Tool | Rate | Measured RTL | Declared to CoreAudio | Not declared |
|---|---|---|---|---|
| Oblique RTL Utility | 44.1 kHz | 8190 fr = 185.714 ms | 1257 fr | 6933 fr = 157.2 ms |
| Oblique RTL Utility | 48 kHz | 8835 fr = 184.062 ms | 1257 fr | 7578 fr = 157.9 ms |
| `tools/rtl/rtl_loopback` | 48 kHz | 8835.03 fr = 184.063 ms (20/20 trials, sd 0) | 1257 fr | +7578 fr = +157.9 ms |

- Our tool and Oblique agree to 0.001 ms.
- The scheduling part is exactly as declared: 1152 measured, 1152 declared (2 × 512 IO + 80 + 48 safety).
- All of the error is in the hardware path: 7683 fr measured against 105 declared (53 + 52 device latency).
- The undeclared part is about 157.5 ms at both rates. It is constant in *time*, not in frames, which points to a
  delay counted in packets or cycles.

**Reference from `midi`** (an older midi build, not rebuilt; user's capture): at 48 kHz with a 64-frame buffer, RTL
was 373 fr = 7.771 ms, equal to its declaration (2 × 64 + 80 + 60 safety + 53 + 52 device). The hardware path was
105.03 fr measured against 105 declared, a residual of +0.03 fr, and Oblique agreed.

This shows the device's physical path is 105 frames. **Main's extra ~158 ms is our own software placement.**
Hypothesis, unproven: it comes from where TX places frames relative to transmission. Which milestone owns fixing
the residual is **not decided** (milestone 6 or later). B3 measures it and reports it; it is not a pass criterion
yet.

**Small client buffers glitch.** At 48 kHz, current main gives dropouts and clicks at 32- and 16-frame buffers on
an Oblique test sine. midi did not solve this either, although `c3e27a53`'s commit message says it fixed 32-sample
glitches. The user's hypothesis is that it lines up with the TX interrupt period: 8 packets = 1 ms, longer than the
0.67 / 0.33 ms IO period. Not investigated, and no milestone assigned.

**Stability bar.** Main ran DICE for one 7.4 h stream at 44.1 kHz (95,551 ZTS) without a restart or fault. This
milestone must not regress that.

## 1b. The TX placement meter `[TxPlace]` (T3)

It splits the loopback's ~158 ms between TX and RX. Once a second, and at once after start, the TX producer logs:

```
[TxPlace] pkt=<packet> frame=<first audio frame> halSample=<HAL sample time when it left> offset=<frame - halSample> rate=<Hz>
```

- It uses the newest completed DATA packet.
- The packet's transmit cycle comes from its completion stamp, mapped to host time through the transport's clock
  pair.
- The HAL sample time comes from the anchor mailbox CoreAudio is fed.
- **Negative `offset` means the frame left after its HAL time**, so output is late by that many frames.

If TX placement holds the ~158 ms, expect about **−7578 at 48 kHz** (−6933 at 44.1 kHz). A value near the host
rig's is the other outcome: then TX is placed correctly and the RX side is the one to look at.

The host rig checks that the meter reports what the wire shows (`TxOwnershipGolden.PlacementMeterAgreesWithTheWire`).
For the 1814 in the rig it reads `offset=+26`. The rig publishes no HAL anchor for the DICE cases, so the meter is
silent there.

To read it on hardware:
`asfw_log_query {"categories":["DirectAudio"],"contains":"[TxPlace]"}`.

### `[TxPlace]` on hardware, 2026-09-26: TX placement holds the delay

Pro 24 DSP, 48 kHz, build `2dbae402`, from the driver ring:

| When | `offset` |
|---|---|
| First measurement, packet 2048 (~1 s after start) | **−10**: correctly placed |
| Next measurement (packet 10047), and every one after | **−7556, then a steady −7594 (±2)** |

−7594 frames matches the RTL residual (+7578 frames) to within ~16 frames.

**Mechanism, confirmed in the ring and the code:**
1. Right after start the frame-target loop (target = `W` + the 4160-frame horizon; `[TxPrepFrame] deficit=2820`)
   prepares packets ahead of the RX replay they depend on. The ring shows
   `[TxReplay] fail=ahead pkt=2559 cur=2169 prod=2169`, plus 1257 more suppressed within ~0.8 s.
2. On `kAheadOfProducer` the producer "holds the reader where it is and ships one NODATA packet"
   (`ASFWAudioDriverTxProducer.cpp`, the `kAheadOfProducer` branch). Its comment calls this transient and
   self-resolving.
3. It is not. Each such NO-DATA packet uses a transmit cycle but no replay entry and no frames, so the replay reader
   and the TX frame cursor end up one cycle further behind real time **for good**.
4. ~1258 of them = 1258 × 6 = ~7548 frames = **157 ms**, the RTL residual.
5. Afterwards the reader is far enough behind that replay entries always exist, so the lag is exactly constant.

This is the host rig's stall case (§1a), happening at every start on hardware.

**Same-stream check, 16-frame buffer (2026-09-26).** One continuous stream (driver sequence 6538 onward); the tool
runs joined it rather than restarting IO:
- `rtl_loopback --measure --frames 16`: RTL 7825 fr (163.0 ms), 20/20 trials.
  - Scheduling: 160 measured = 160 declared.
  - Hardware path: 7665 against 105 declared, residual **+7560**.
- `[TxPlace]` in the same stream: **−7562 to −7564**.

So TX placement accounts for the whole residual to within ~2–4 frames, and RX contributes about nothing. The
residual does not depend on buffer size (+7578 at 512 in an earlier stream, +7560 at 16).

**The lag also grows mid-stream.** In this stream `[TxPlace]` moved from −7506 to −7564 around packet 146k (~18 s in)
without a restart. This is consistent with another client starting IO, which makes `W` jump, then a preparation
burst and more `kAheadOfProducer` NO-DATA packets. It is inferred from timing, not correlated to a logged event.

**Small buffers in the same stream.** `--tone` at 32 and 16 frames, 5 s each: 0 dropouts, 0 slips, and one "click"
per run. It sits at the same place each time, ~450 frames right after the tone arrives, and is inaudible. That is the
tone's own start transient, and the tool now settles past it. Caveats:
- the device IO cycle was probably 16, because Oblique was also running at 16;
- the earlier audible glitches were on a long-running older dext.

Which of those differences matters is not known.

**Consequence for T4.** Removing the frame-target loop is necessary but not sufficient. The producer must never
commit a packet for a cycle whose replay entry does not exist yet. It must stop that preparation pass instead of
shipping NO-DATA. Preparation depth for replay-driven streams is bounded by RX availability, not by `W`. The check
after T4: `[TxPlace]` stays near the first reading (−10) and the RTL residual falls toward 0.

## 1a. TX goldens (T1): what the host rig shows today

`tests/audio/AudioDriverTxProducerTests.cpp` (`TxOwnershipGolden.*`, goldens in `tests/golden/tx/`) runs the real
producer, the real WriteEnd step and an emulated IT consumer. The WriteEnd body is extracted to
`HandleOutputWriteEnd` (`ASFWAudioDriverOutputWrite.cpp`) so the test calls the same code as the IO handler.

**The rig's model.**
- CoreAudio acts at every packet time and writes each sample as a tag naming its frame and channel.
- Its "now" comes from the latest published zero timestamp, as the HAL's does. The DICE cases publish none (the rig
  feeds RX replay directly), so the device frame clock `packet × rate / 8000` stands in.
- Only 48 kHz is covered: the rig's RX feed models the 48 kHz blocking cadence only. There is no restart case yet
  (T6).

These are **rig results**, not hardware results:

| Case | Result |
|---|---|
| Saffire, 512 or 64 frames, lead = io + 48 | Every DATA packet after the prefill carries consecutive written frames. `offset +0`: a frame leaves when the device clock reaches it |
| Saffire, late writer (lead 0) | Every DATA packet is silent: the writes land in packets already sent |
| Saffire, 50 ms CoreAudio stall | After the stall, TX replay underflows 1124 times, and every packet from about packet 2990 to the end of the run (4000) is **NO-DATA** |
| 1814, 512 frames | PCM after the first 7 DATA packets. The packet on the wire is about 26 frames ahead of the HAL's "now", leaving ~22 frames of the 48-frame lead |
| AM824 label on unfilled PCM | Was `0x00`; since T3 it is MBLA `0x40` (`tests/golden/tx/maudio-1814`) |

**The stall case, hypothesis only.** When CoreAudio resumes, `W` and the frame target (`W` + the 4160-frame
horizon) jump by the stalled frames. The producer then prepares ahead of the RX replay it depends on. Each underflow
commits a NO-DATA packet, and TX and RX then advance in step, so the gap persists.

The rig runs 0.5 s, shorter than the ~1 s `[TxExposure]` sampler that drives the self-heal, so recovery is not
exercised. On hardware this would be up to ~1 s of silence after a CoreAudio stall. **Not verified on hardware.**
It is the `W`/`E` rendezvous that T4 deletes.

Two earlier rig models were wrong and were corrected before these results were recorded:
- CoreAudio writing on the nominal clock for the 1814, which lost all PCM;
- CoreAudio acting only at interrupt boundaries, which made periodic 16-frame gaps.

## 1c. T4: the audio-side fill (2026-09-26)

**What changed:**
- **WriteEnd** publishes `W` and the playback-ring range and wakes the producer. It no longer touches packets.
- **The producer** prepares packets only to the coverage target. The `W` + 4160-frame target, `[TxPrepFrame]`,
  `[TxExposure]` and the self-heal re-arm driven by a diagnostic sample are gone.
- **`FillTransmitPayloads`** (`ASFWAudioDriverOutputWrite.cpp`) copies `[filled, min(W, E))` from the HAL output
  ring into the armed packets, once each. A packet at or behind the finality frontier keeps its armed silence and
  is counted as `framesMissedFinality`:
  - frontier = projected hardware packet + `kTxFillFinalityGuardPackets` (3, provisional);
  - projected hardware packet = `completionCursor` plus the whole bus cycles since that refill's clock pair.
- **A replay-driven packet whose RX entry does not exist yet ends the pass** instead of shipping NO-DATA. The one
  exception is a packet the hardware would reach first (below completion + 48 + 8). That is shipped as NO-DATA,
  counted in `txReplayForcedNoData`, and logged as `[TxReplay] forced NO-DATA`.
- **`[TxPrep]`** reports `forcedNoData` and `missedFinality` in place of the exposure lead.
- **`[TxPrepRange]`** logs only a hole below the descriptor floor.

**Declared golden deltas** (`tests/golden/tx`):

| Case | Change |
|---|---|
| Saffire, 512 and 64 frames | First DATA packet directly after the prefill (1696), not at 1896. The 200 "ahead" NO-DATA packets are gone |
| Saffire stall | After the stall the 3-of-4 cadence continues (1144 DATA / 382 NO-DATA), where it used to be 1134 NO-DATA of 1526 |
| 1814 | 2 more silent start-up packets (frames 56–71 now miss the enforced guard) |

`StallDoesNotRunTxAheadOfRxReplay` asserts 0 underflows and 0 forced NO-DATA in the stall case; before T4 the rig
counted 1124.

**On hardware, check** (acceptance ownership undecided; see §1):
- `[TxPlace]` stays near the first reading (−10) for the whole stream;
- the RTL residual falls toward 0;
- `forcedNoData=0` in `[TxPrep]`;
- the tone check at 16/32/64/512, because the accidental 157 ms cushion is gone.

**Still left:**
- The write-less `txExposure*` fields and `requestedTargetFrameEnd` (T8).
- The exposure-window geometry that still sizes the 1648-packet cap and the 1696-slot ring (B3/T8).

## 1d. T4 on hardware, and the fill moved to the IO thread (2026-09-26)

**RTL, 48 kHz, 64-frame buffer:**
- 367 fr (7.646 ms) against 361 declared: **residual +6 fr (0.126 ms)**, down from +7578.
- Oblique RTL Utility: 367.
- `[TxPlace]` in the same stream: −4.

The 157 ms is gone. What we declare to CoreAudio now matches the physical path to within ~6 frames.

**What it cost.** With the fill on the TX preparation queue, `missedFinality` grew in bursts (~27 fr/s over ~70 s)
while a 64-frame client ran. `rtl_loopback --tone --frames 64` heard them: 3 clicks in 5 s with the one-packet-silence
signature.
- A frame has ~1 ms between WriteEnd and its packet's deadline (48-frame safety minus the 3-packet guard).
- Queue wakes were measured up to **13.7 ms** late (4–7 ms worst per 5 s interval).
- The old path never met this: it wrote on the IO thread, 157 ms early.

**Decision (user, 2026-09-26): the fill runs inside WriteEnd, on CoreAudio's IO thread, its only owner.**
- The preparation queue still arms packets with silence ~18 ms ahead (coverage) and no longer fills.
- Still one PCM copy.
- RT-safe: no locks, no allocation; the timeline and the clock pair are read through their seqlocks.
- This is how the vendor drivers fill: inside their own callback context.

**Test:** `SmallBufferFillDoesNotWaitForTheQueue` never runs WriteEnd's queue wake and uses a 64-frame buffer. After
start-up no frame may miss its deadline. With the fill moved back to the queue it fails (80 → 576 missed).

**Hardware result of the IO-thread fill (build `7ff335bd`, 48 kHz, 64-frame buffer, 2026-09-26):**

| | Queue fill | Fill in WriteEnd |
|---|---|---|
| `--tone --frames 64`, 5 s | 3 clicks | **clean** |
| RTL (declared 361) | 367, +6 | **365, +4 fr (0.084 ms)**; Oblique 365 |
| `[TxPlace]` | −4 | −2 / −4 |
| `missedFinality` while streaming | +~27 fr/s | **flat for ~2 min** |
| Worst producer wake per 5 s | 4–13.7 ms | 0.1–0.6 ms |
| IT ring laps | ~30 | 0 |

The residual is now entirely TX placement (+4 against `[TxPlace]` −2 to −4).

**Full sweep on `7ff335bd` (48 kHz, pre-T5 baseline).** RTL = 2B + 237 fixed frames (S_in 80 + S_out 48 + L 109):

| Buffer | Declared | Measured | Oblique | Residual | `--tone`, 5 s |
|---|---|---|---|---|---|
| 64 | 361 | 365 (7.604 ms) | 365 | +4 | clean |
| 32 | 297 | 301 (6.271 ms) | 301 | +4 | clean |
| 16 | 265 | 269 (5.604 ms, as predicted) | 269 | +4 | clean |

`missedFinality` still jumps at every stream start: frames CoreAudio wrote before any TX packet existed for them. It
must be reset per stream and must exclude those frames, which is pending.

The drop in wake latency is not explained. A lighter queue once the fill left it would account for it, but this
session also had no Music.app playing.

**Also seen, T5's area:** on one boot an IT refill ~12 ms late lapped the 48-packet cyclic descriptor ring twice. The
controller re-sent 96 old packets and `[TxPlace]` moved −4 → −580 for good. That boot then logged ~30 more
`IT lap sighted` events without a further shift.

## 1e. Experiment E1: instrument the CoreAudio budget, then spend it (2026-09-26)

Model (user): **RTL ≈ L_in + S_in + 2B + S_out + L_out**, all around the ZTS-projected hardware NOW.
- S_in is our promise that input is in the ring S_in behind NOW.
- S_out is our deadline to put output on the wire S_out ahead of NOW.
- L is the fixed device path.

Pre-E1 baseline (build `7ff335bd`, 48 kHz): see §1d. RTL = 2B + 237 fixed (S_in 80 + S_out 48 + measured L 109), residual
+4 at 16/32/64.

**E1a (this commit): instrument, and declare the measured L_out.**
- `[TxPrep]` gains the budget in the model's terms:
  - `sOutMinPk`: the smallest S_out headroom in the interval, in packets ahead of the finality frontier (−1: nothing
    filled);
  - `sInMinFr`: the smallest S_in headroom, capture frames already written past the HAL read end (−1: no reads);
  - `sInStarve`: capture starvations;
  - `missedFinality` now counts this stream only, and frames before CoreAudio's first write are no longer counted.
- The start-up heartbeat burst is gone: a new margin low is logged only near the descriptor floor.
- **Declared delta: Saffire output latency 52 → 56** at the 1× rates (32/44.1/48 kHz). `RTL_ts` measures 109.03, and
  `[TxPlace]` puts the +4 on output.
  - 2×/4× follow the profile's doubling rule (112/224), unmeasured and parked.
  - Pins updated: `ProfileTimingPinTable.inc`, `DiceProfileTests`, `tests/golden/dice-profiles`.
- Note: midi measured `RTL_ts` 105.01 with the same 53/52, so midi's TX placement was ~4 frames earlier than ours.
  Recovering those 4 real frames is a separate item; E1a only declares them.

**Keep E1a if** the residual lands at ~0 (±1) at 16/32/64, the tone runs are clean, and nothing else moves.
**Revert** output latency to 52 otherwise.

**E1a on hardware (Pro 24 DSP, 48 kHz, build `de9ea3f`, 16 frames, 110 s stream):**
- RTL 271 measured against 269 declared: residual **+2.03** (Oblique: 271). The measured path moved +2 against the
  pre-E1a run, so it varies by ~2 frames between starts (placement alternates −2/−4 on this stream). The declared
  latency only reaches the HAL, so this is not E1a moving audio. Kept: with 52 the residual would be +6.
- Tone at 16 frames clean.
- `sOutMinPk` 0–5 packets. At a 512-frame buffer it was 22–23. No room to cut S_out before T5.
- `sInMinFr` 24–36 frames of the 80; no starvation after start-up. The cold start's first interval showed one
  start-up starvation (`sInMinFr=0`).
- Open: `missedFinality=32648` (~680 ms of frames) in the first 5 s of one stream, 0 after it. Cause unknown.
- Instrument bug: the first heartbeat of a stream carried `sOutMinPk` over from the previous stream.

**What L is made of (assumption, not verified on the board).** L = 109–111 frames at 48 kHz.
- Converters: we assume the Pro 24 DSP uses the TI PCM3168A (user). Its datasheet (SBAS452A) gives group delay
  ADC 27/fS + DAC 28/fS = **55 frames** in single-rate mode, whatever the rate. Dual rate differs (ADC 17/fS,
  DAC 28/fS), so the 2×/4× doubling rule is not what the converters do.
- The Windows Focusrite 4.0.0 table agrees: its "I/O" column excludes AD/DA (its footnote), and RTL − (in + out)
  = 58–60 frames at 44.1 kHz for buffers 32–256 (the 512 row does not fit and is left out).
- The remaining ~54–56 frames are FireWire transport: 12800-tick transfer delay per direction (~25 frames each at
  48 kHz) plus a few frames of placement.
- Our declared device latencies include the converters, as CoreAudio expects. Windows' reported I/O does not, so
  compare RTL with RTL: at the same buffer ours is ~1.75 ms longer (32: 6.27 vs 4.444 ms; 64: 7.60 vs 5.897 ms),
  and that gap is in safety and transport, not the converters.

**E1b (this commit): cut S_in by 16 frames, S_out unchanged.**
- **Declared delta: Saffire capture safety 10 → 8 packets** (80 → 64 frames at 1×). 2×/4× follow the formula
  (160/384), unmeasured and parked. Pins and `tests/golden/dice-profiles` updated.
- Instruments:
  - The S_out interval minimum is reset when the producer is armed.
  - Reads that starve before the stream's first complete read are start-up: counted in `sInStart` for the whole
    stream, and kept out of `sInStarve` and `sInMinFr`, which now describe the steady state only.
- Expected: RTL −16 frames at every buffer (16: 253, 32: 285, 64: 349).
- **Keep if** tone is clean at 16/32/64, the residual stays where E1a left it, `sInStarve` stays 0, and steady-state
  `sInMinFr` stays ≥ 8. **Revert** to 10 packets otherwise.
- S_out waits for T5: part of the output margin is the provisional 3-packet guard, which exists because the fill
  projects the hardware position instead of reading it.

**E1b on hardware (Pro 24 DSP, 48 kHz, build `f1e28985`): reverted.**
- RTL moved exactly as predicted: 255 at 16 frames, 287 at 32 (residual +2.03 against 253/285; Oblique agrees).
- At a 16-frame buffer (Oblique holding the device, ~150 s) steady-state `sInMinFr` sat at 16, the 16 frames cut.
  Five intervals dropped to 0, three with `sInStarve` 1–2, all after start-up (`sInStart=0`).
- The 16-frame tone had 4 clicks with 1–3 silent samples each, which fits reads zero-filled 1–3 frames short. The
  same tone at 80 was clean. The 32-frame tone was clean, but Oblique was still holding the device at 16.
- Capture safety back to 10 packets. The instruments stay.
- Reading: at 16 frames the input path needs 64 + a tail of up to ~16 frames, so S_in cannot drop until the
  capture ring is filled closer to the read (not by the interrupt cadence alone).
- Seen again: `missedFinality=32580` in the first 5 s of a stream started by a 16-frame client (32648 last time).
  Reproducible, but only seen with a 16-frame client starting the stream. 16 frames is an experimental buffer
  size, so this is parked until after T5 (user, 2026-09-26). First step when it is picked up: a one-shot line on
  the first miss of a stream (frame, its packet, the projected hardware packet, time since StartIO).

## 1f. T5: the finite IT queue (2026-09-26)

**Why.** The IT descriptor ring was cyclic (48 packets, 6 ms), and completion was inferred from the CommandPtr
plus a lap detector reading OUTPUT_LAST timestamps. A refill more than one ring late replayed stale packets and
permanently lagged the stream (the −580 event in §1d). midi replaced this with a finite queue (`c4dd1700`),
deepened the ring (`8298af5f`) and fixed the uncached-fill crash the deeper slab exposed (`51a150d6`). T5 ports
the mechanism, not the code, and without midi's late-payload binding.

**T5a (`1f067fb4`): DMA fills without memset.** Cache-inhibited DMA memory rejects `dc zva`, which `__bzero` uses
above a size threshold. `Shared/Memory/UncachedFill.hpp` holds the one plain-store fill; a boundary test flags
memset over a DMA region base.

**T5b (`5ea43602`): RX replay history 512 → 2048, read delay kept at 256.** Measured on hardware: `[TxSyt]`
shows the RX observation replayed 394 cycles later = read delay 256 + the ~138-packet TX lead when the reader
began. So the replay distance follows the lead by itself; what limits a producer stall is history − read delay
(was 256 cycles, 32 ms; now 1792, 224 ms). Replay distance and start-up timing are unchanged.

**T5c (this commit): the finite queue.** Behaviour cross-checked with Linux `firewire-ohci`:
- Prime ends the chain with a zero branch; the cycle-loss skip address stays self-linked.
- Completion is OUTPUT_LAST xferStatus, walked from the completion cursor up to the mapped end
  (`ohci.c:2918-2922`; the status bit is set on every OUTPUT_LAST, `ohci.c:3302-3306`). No CommandPtr
  arithmetic; the lap detector and its counters are deleted.
- The newest completed descriptor is not recycled until its successor completes (`ohci.c:954-982`). Unlike
  midi, the public completion cursor still counts it: the fill projects the hardware position from that cursor,
  and holding it back one packet would spend one of the three guard packets.
- Each refill builds a zero-terminated batch in recycled slots, publishes it, then links it with one aligned
  store to the old tail's branch word, then always WAKEs (`ohci.c:1137-1141`, `3471-3476`).
- If every mapped packet completed before the refill ran, the queue ran dry: `MappedRegionExhausted`, the
  context stops through the existing fault path, and a restart recovers. The `IT: Stopped` line reports
  `minGap`, `criticalGaps`, `maxDelta` and `exhausted`.
- Geometry: ring 48 → **504** (63 ms, RX parity; AppleFWAudio's DCL ring is ~100 ms), slack = one ring,
  coverage 144 → **1008**, shared slots 1696 → **1512**. The frame-exposure window no longer sizes anything
  (T8 deletes the constants). Depth is not latency: the fill writes PCM up to the finality guard whatever is
  mapped.
- Budgets: a refill may be up to 63 ms late; the producer may stall up to 63 ms; the replay history covers
  224 ms.
- **Declared delta** (TX goldens): the start-up NO-DATA prefill is 1512 packets instead of 1696, so DATA starts
  184 packets (23 ms) earlier. PCM placement unchanged (Saffire `offset=+0`).
- Tests: the lap tests are replaced by finite-queue tests, each mutation-checked: a cyclic tail, recycling the
  newest completion, an unbounded status walk, a missing tail link, an unterminated batch, ignoring exhaustion
  and waking only when idle each fail at least one test. The older refill tests now run in production order
  (the first refill maps packet 504 into slot 0).
- T5d (next commit): 64-bit packet index end to end.

**T5d: 64-bit TX packet index.** The transport maps absolute 64-bit packets and accepts a slot only at
`ExpectedTxCommitGeneration(packet)`, but the slot-provider port, the stream engine, the packet timeline and
both payload writers carried a 32-bit index. After 2^32 packets (6.2 days at 8000/s) the producer would commit
the wrong generation and the next refill would FATAL. All of them are 64-bit now; the writers' finality margin
is a signed 64-bit difference. The `[TxWire]` log printed the index with `%u` and now uses `%llu`. A test commits
packet 2^32 + 5 and checks its generation (fails if the index is truncated). No wire change; goldens unchanged.

**On hardware, to check:** `IT: Stopped` shows `exhausted=0`; `minGap` stays far above 0; no `IT FATAL`; start-up
still plays; RTL and tone unchanged (the ring does not add latency); a 48 ↔ 44.1 switch.

## 2. How we got here: keep midi's understanding, not its architecture

The core TX problem is **pacing**. CoreAudio writes PCM when its IO thread runs; this is `W`, the client write end.
The transport produces packets on the hardware clock. Main joins the two at one point: the IO callback encodes
straight into packets that already exist. So:

- A frame whose packet does not exist yet is dropped. That is `W > E`, where `E` is the exposed end
  (`AmdtpPayloadWriter.cpp:104-110`, `framesWithoutPacket`).
- A write into a packet the hardware has already sent is only counted (`AmdtpPayloadWriter.cpp:167-175`,
  `wroteIntoTransmitted`, `racedReuse`).
- The workaround is to prepare packets very far ahead. The frame horizon is at least 4160 frames
  (`AudioTimingGeometry.hpp:114-122`) and the preparation lead is 1648 packets. The `[TxPrepFrame] deficit` lines are
  that rendezvous showing up.

midi diagnosed this correctly in `a18d7293` (2026-08-10) but fixed it by adding a **second PCM copy**:
`TxPcmStagingRing`, reshaped into `PcmPublicationCache` in `677a6360`. The HAL output ring CoreAudio already writes is
that buffer. The copy made PCM enter packets too early, and each later midi mechanism existed to get it in later:

- late re-encode (`5b421ac2`);
- freeze at the mapping frontier and dual payload images (`6ae514df`);
- descriptor rebind (`75e706c0`, `c9e31d67`);
- the producer→transport doorbell (`33414fc2`, `dfafe60c`).

Per the user (2026-09-26): chasing `W > E` produced nothing lasting. **midi holds the better CoreAudio
understanding**, and that is kept:
- safety offsets;
- buffer size ≠ latency;
- real output latency = safety offset + client IO buffer + device/stream latency;
- the timing geometry;
- a residual of 0 is achievable.

Its architecture is not kept. **A second PCM copy is a bug by design.**

## 3. Main inventory (FW-210) and dispositions

Legend for cursor kinds: AF = audio-frame cursor, PK = packet cursor, DMA = DMA cursor.

| Surface | file:line | What it is | Disposition |
|---|---|---|---|
| WriteEnd handler | `ASFWAudioDriverIO.cpp:185-256` | Publishes `W` and the playback-ring range, requests preparation, **encodes PCM into packets** (`:236`, `:248`) | **ADAPT (T4)**: keep publishing `W` + range + the prep request; stop touching packets |
| `outputClientWriteEndFrame` (`W`) | `Audio/DriverKit/Runtime/AudioClientCursor.hpp` | CoreAudio write frontier (AF) | **KEEP**: the only PCM publication frontier |
| `PlaybackRingRange` (`playbackRingWriteFrame`, `OldestValidFrame`) | `ASFWAudioDriverIO.cpp:19-47`, `PlaybackRingRange.hpp` | Valid window of the HAL output ring | **KEEP**: becomes the fill's source window `[oldestValid, W)` |
| `playbackRingReadFrame` | `ASFWAudioDriverIO.cpp:253` | Set to "copied into packets" at WriteEnd | **REPLACE (T4)** → the fill's `filledEnd` |
| `AmdtpPayloadWriter::WriteFloat32Interleaved` | `AmdtpPayloadWriter.cpp:72-208` | Encodes PCM into exposed packets on the RT IO thread | **ADAPT (T4)**: becomes the fill, called by the producer |
| `MotuPayloadWriter::WriteFloat32Interleaved` | `MotuPayloadWriter.cpp:43-133` | Same, MOTU layout | **ADAPT (T4)**: same |
| `DiceTxStreamEngine::WriteHostOutputFloat32` | `DiceTxStreamEngine.cpp:169-175` | RT forwarder to the writer | **DELETE (T4)** |
| `AmdtpPacketTimeline` slots + `SnapshotSlotForAudioFrame` | `AmdtpPacketTimeline.cpp` | Frame→packet map read by the RT writer through a seqlock | **ADAPT (T4)**: read by the producer's fill only; RT access goes |
| `AmdtpPacketTimeline::Published` state | `AmdtpPacketTimeline.hpp` | Declared, never set | **DELETED (T2)** |
| `exposedFrameEnd_` (`E`) | `AmdtpPacketTimeline.cpp:104-107` | High-water only; not retracted on `RevertToNoData` | **DELETE (T4)** along with the `W`/`E` rendezvous |
| `DiceTxStreamEngine::nextAudioFrame_` | `DiceTxStreamEngine.hpp`, `.cpp:65-99,157` | The content-frame cursor (AF) | **KEEP**: the single frame cursor |
| Packetizer `telemetryNextAudioFrame_` + its own Align/ReArm | `AmdtpTxPacketizer.cpp:119,132,149,227,276,316,343` | Shadow frame cursor; its header claims ownership | **DELETED (T2)**: also its telemetry snapshot, the 3-argument `PrepareNextPacket`, and the packetizer epoch guard (dead: the engine sets `plan.epoch` from its own epoch in the same call). Tests frame packets through `tests/support/TxPacketizerTestSupport.hpp`; align-once is now tested on the engine |
| Payload clear before exposure | `AmdtpTxPacketizer.cpp:382-386` (policy set at `DiceTxStreamEngine.cpp:210`) | Zeroes the payload; an unfilled AM824 PCM slot goes out as `0x00000000` (label 0x00) | **DONE (T3)**: the payload is still cleared, then every PCM slot is armed with encoded silence through the slot encoding and channel map (AM824 `0x40000000`; raw PCM stays 0). Golden delta: the 1814's start-up silent packets carry label `0x40` |
| `RevertToNoData` | `AmdtpTxPacketizer.cpp:284-321` | Rewinds DBC and rewrites the slot as NO-DATA (MOTU timing unavailable) | **KEEP**; T2 removes its shadow-cursor write |
| TX frame alignment `[TxAlign]` | `ASFWAudioDriverTxProducer.cpp:449-505` | Places TX frames from the RX replay entry (§6) | **KEEP until B3** (see §6) |
| Alignment re-arm on a replay failure | `ASFWAudioDriverTxProducer.cpp:342-367` | Re-arms after a non-ahead replay miss | **KEEP**; it is reviewed in T6 as part of the recovery transaction |
| Self-heal re-arm from a diagnostic sample | `ASFWAudioDriverTxProducer.cpp:256-275` (reads `txExposureSampleWriteFrame`) | A diagnostic value drives control | **DELETE (T4)** |
| Frame-target loop condition | `ASFWAudioDriverTxProducer.cpp:148-159` | Prepares until the packet **and** frame targets are both met | **REPLACE (T4)**: availability never enters the loop condition |
| `TxDataHorizonFrames` as a content horizon | `AudioTimingGeometry.hpp:114-122`; used at `TxProducer:818` | ≥ 4160 frames of exposure | **DELETE as a content horizon (T4)**; the transport runway stays a transport margin |
| `DextTxSlotProvider::AcquireWritableSlot` | `ASFWAudioDriverPrivate.hpp:139-151` | Returns a slot pointer without checking it's free | **ADAPT (T5)**: reuse justified by descriptor completion |
| `DextTxSlotProvider::PublishSlot` | `ASFWAudioDriverPrivate.hpp:153-220` | Header/length, the `[TxWire]` scan (`:202`), commit (`:217`) | **KEEP** the commit; **DELETE** `[TxWire]` (T8) |
| `IsochTxPacketMeta.commitGeneration`, `committedEnd` | `IsochTxQueue.hpp` | Producer → transport commit token and cursor (PK) | **KEEP** |
| `completionCursor` | `IsochTxQueue.hpp`; written by `IsochTxDmaRing.cpp` refill | Inferred from the command-pointer delta (`:87`) + lap guesses (`:167`, `:637`) | **REPLACE (T5)** → counted from descriptor xferStatus |
| `DetectRingLaps` | `IsochTxDmaRing.cpp:167-343` | Lap inference from cycle stamps | **DELETE (T5)** |
| Cyclic 48-packet descriptor ring | `IsochTxDmaRing.cpp` Prime/Refill | Hardware can revisit old descriptors | **REPLACE (T5)** → a finite zero-terminated chain |
| `packetIndex` as `uint32_t` | `AmdtpTypes.hpp` (`PreparedTxPacket`, `TxPacketSlotView`) vs the 64-bit commit generation | Diverges after 2³² packets (~6.2 days) | **ADAPT (T5)**: 64-bit end to end |
| Completion stamps | `IsochTxQueue.hpp` | Cycle stamps per completed packet; M-Audio clock input | **KEEP** |
| Interrupt every 8 packets | `IsochTxDmaRing.cpp:473, 934` (`IsTimingGroupBoundary`); `AudioTimingGeometry.hpp:60-62` | TX/RX interrupt cadence | **KEEP**. Changing it is a geometry change (§7), not a tweak |
| StopIO prep-queue drain | `ASFWAudioDevice.cpp:633-634` | Drains only when `mAudioInternalTxActive` | **ADAPT (T6)**: drain for every family |
| `txSecondaryActive` | `ASFWAudioDriverPrivate.hpp:268` | Plain `bool` read by the RT thread | **ADAPT (T6)**: atomic |
| `FireWireAudioEngine` / `DirectOutputReader` | `Audio/Engine/Direct/*` | Never bound; dead read path | **DELETED (T2)**, with `DirectAudioEngineTests` (it tested only this engine) |
| `IsochTransmitContextTests.cpp`, `DirectTxProbeTests`, `TxAudioPacket*Tests` | `tests/audio/` | Not built (stale includes) | **DELETED (T2)** |
| `HashTxPayload`, `TxFatalSnapshot`, `txCompletedPayloadHash*`/`txCompletedPcmSlots`/`txCompletedStartupSilenceSlots` | `TxPayloadHash.hpp`, ATCB, `AudioRtCounters.hpp` | No callers; **no writer anywhere** (only `Reset()` and the debug snapshot touched them) | **DELETED (T2)**: the whole `TxFatalSnapshot`, not just its hashes (T0 assumed the rest was written; it is not). `ADK FORCED FATAL` keeps the live `fatalReason`/`fatalGeneration` |
| `txScheduledSampleFrame`, `txCompletedSampleFrame` | ATCB (`:760-761` reset only) | Never written except by reset; read by `DirectAudioDebugSnapshot.hpp:190-193` and two tests | **DELETED (T2)** with its readers |

## 4. midi distillation (FW-211)

| midi mechanism | Commit | Verdict |
|---|---|---|
| `TxPcmStagingRing` → `PcmPublicationCache` (second PCM copy) | `a18d7293`, `677a6360` | **Drop**: bug by design; the HAL ring is the publication |
| Silence-first valid DATA packets (encoded silence, never withhold) | `59f3b501` | **Take (T3)** |
| Arm-then-fill is byte-identical to encoding up front | `5b421ac2` | **Take**, as a test property |
| Dual payload images; freeze at the mapping frontier | `6ae514df` | **Drop / park** |
| Descriptor rebind against the live position | `75e706c0`, `c9e31d67` | **Park**: only if B3 measures torn packets. Keep the lesson: a missed deadline is final, and a failed position read writes nothing |
| Finite DMA, xferStatus completion, continuation anchor, detached batch + WAKE | `c4dd1700` | **Take (T5)**, cross-checked against Linux `ohci.c` |
| Finality lead from the fetch horizon, not the interrupt group | `c3e27a53` | **Take the principle**; the number is measured in B3 |
| Producer→transport doorbell | `33414fc2`, `dfafe60c` | **Drop**: the producer writes in place, and the WriteEnd wake already exists |
| Output safety derived from the finality lead | `5d1ea98c` | **Take (T8)** |
| One recovery transaction | V3 review, P1 | **Take (T6)** |
| `TxLatencySession`, decision capture, ledgers, `TxCycleTraceRing`, telemetry wire v6, payload-seal re-hash | various | **Drop** (FW-171) |
| `TxRefillFlightRecorder` pattern | — | **Take the pattern** for B3's measurement sidecar |

## 5. Target model

```
CoreAudio IO thread            publishes W (and the ring's valid range); wakes the producer
        │                      — never touches packets, DMA, or packet indices
HAL output ring                THE PCM publication: absolute frame = sample time, valid [oldestValid, W)
        │
producer (prep queue)          arms planned packets deep with VALID SILENCE (committedEnd, transport runway)
        │                      fills PCM once: copy ring → armed payload, in place, ahead of the finality frontier
        │                      a frame not in [oldestValid, W) stays silence (counted); never withheld
transport (core queue)         payload-opaque: maps committed slots, finite DMA, completion from descriptor status
        │
OHCI IT                        fetches the payload when it reaches the packet
```

**Frontiers.** Each has one unit, one owner and one monotonicity rule. Collapse any two that turn out to mean the
same thing.

| Frontier | Unit | Owner | Meaning of crossing it |
|---|---|---|---|
| `W` | audio frames | CoreAudio (IO thread) | Frames below `W` are written in the HAL ring and may be read |
| `committedEnd` | packets | producer | The packet is armed with valid silence and may be DMA-mapped. Transport runway, **not** content latency |
| `filledEnd` | packets | producer | The packet's PCM was copied from the ring (or it stays silence). Never goes back |
| `finalityFrontier` | packets | derived: hardware position + guard | No payload write at or behind it. The position source and the guard are decided in B3 |
| `completedEnd` | packets | transport | Descriptor status says the hardware finished; the slot is reusable after the continuation anchor |

**Rules.**
1. **No second PCM copy.** PCM is read from the HAL ring once, by the fill.
2. **Availability never gates production.** Missing PCM gives valid silence and the cadence continues. NO-DATA is
   never a stand-in for missing content: NO-DATA consumes no frames and would shift the cadence.
3. **Transport runway ≠ content latency.** Arming depth is transport safety; only `finalityFrontier` sets what
   CoreAudio is charged.
4. **Transport stays payload-opaque** (FW-60 boundary). The audio side reads no MMIO. The hardware position it uses
   comes from `completionCursor` or from a timeline projection (IT sends one packet per cycle).
5. **The fill is the last write.** Nothing writes a packet's payload after its fill decision (lesson from the
   midi payload seal).

## 6. FW-194: is `[TxAlign]` the timeline projection?

**No. It uses the same frame numbers but a different time basis.**

- `[TxAlign]` (`ASFWAudioDriverTxProducer.cpp:449-505`) places the TX frame cursor at
  `replay.firstAudioFrame + (outputPresentation − sourcePresentation) × rate`.
  - `replay.firstAudioFrame` is the RX consumer's `absoluteFrameCursor_` (`DirectAudioReceiveConsumer.cpp:337`).
    After Epic 4 that is the Receive-epoch timeline frame, so the frame domain is shared.
  - Both presentation times are **SYT-presentation** ticks: the source SYT plus the RX transfer delay, against the
    output anchor plus the SYT offset plus the TX transfer delay.
- The Epic 4 timeline maps the same frames to **arrival** bus time (decision (a) in
  `HARDWARE_TIMELINE_OWNERSHIP.md`).

Routing `[TxAlign]` through the timeline would therefore move the TX frame origin by the arrival-vs-presentation
difference. That changes where output frames land in time, which is exactly what the RTL residual (§1) measures.
**T7 is not decided here.** It is decided after B3, against measured RTL, together with the residual's milestone.

## 7. Instrumentation: rules and take/leave

Metering became a heavy part of TX on both branches. On a healthy stream, main logs WARNING lines every second
(user's 7.4 h log): `[TxPrepFrame]`, `[TxExposure]`, `[TxPrepRange] (suppressed=1337)`, plus a NOTICE `[TxSyt]`. A
diagnostic sample even drives control: the self-heal at `TxProducer:256-275`.

**Rules.**
1. No instrumentation inside an ownership contract (the queue ABI, the slot provider, the fill loop) unless it is
   needed for correctness.
2. No diagnostic value drives control.
3. Hot paths log anomalies only, plus one coarse heartbeat.
4. Measurements needed for a decision (B3) go in a removable sidecar in the `TxRefillFlightRecorder` pattern, not in
   `AudioTransportControlBlock`. After the decision it is demoted or deleted.
5. From midi's research stack, port the conclusions, not the code (FW-171).

| Instrument | Verdict |
|---|---|
| `[TxProducerFatal]`, `txProducerFault`, `TxFatalSnapshot` (without the dead hashes) | **Take**: correctness |
| Completion stamps | **Take**: M-Audio clock input, not telemetry |
| `[Zts]` SEED/UPD, one in 16 | **Take**: coarse liveness |
| `[TxPrep]` heartbeat | **Take**, reduced to one line: margin + fill health |
| Fill anomalies (missed finality, torn, silence because late) | **Take**, anomaly-only (new, T4) |
| `[TxPrepFrame]`, `[TxExposure]`, `txExposure*`, debt counters | **Leave (T4)**: they measure `W`/`E` |
| `[TxPrepRange]` per-second WARNING | **Leave** (or only on `stoppedShort`) |
| Writer RT counters (`withoutPkt`, `wroteIntoTransmitted`, `racedReuse`, …) | **Leave (T4)** |
| `[TxWire]` every-packet scan in `PublishSlot` | **Leave (T8)**: it inspects the payload before PCM arrives, so it misleads |
| `[TxPrep]` histograms + seqlock interval | **Leave (T8)**; keep what the heartbeat prints |
| `txSytTrace` per-second `[TxSyt]` | **Demote** to anomaly-only |
| Host tools (`halprobe`, `rtl_loopback`) | **Take**: measurement lives on the host |

**The interrupt cadence is geometry.** The 8-packet group feeds the ZTS period, frame alignment, ring sizes and laps,
transmit depth, the completion batch and the safety declarations. Changing it is an Epic 3 geometry change: derive it
through `TimingLadder`, declare deltas in `ProfileTimingPinTable`, and verify on hardware. It is never a
single-constant tweak.

## 8. Stages

| Stage | Content | Ticket |
|---|---|---|
| B0 | Baseline: RTL (done at 512, §1); soak ring capture; Instruments jitter on fresh 44.1 / fresh 48 / 48 after ≥2 h | FW-176 |
| T0 | This document | FW-210/211/212 |
| T1 | TX goldens: `TxProducerRig` + a fake HAL output ring + a WriteEnd driver + a per-packet `WireTrace` | FW-219 |
| T2 | Dead surfaces + the packetizer shadow cursor | FW-213 |
| T3 | Silence-first valid DATA packets | FW-215 |
| T4 | Audio-side fill; delete the `W`/`E` rendezvous and its metering | FW-213/214/215 |
| T5 | Finite DMA + descriptor-status completion; 64-bit packet index | FW-216 |
| T6 | Lifetimes + one recovery transaction | FW-218 |
| T7 | FW-194, decided after B3 (§6) | FW-194 |
| B3 | Measure: dispatch latency, finality distance, missed/torn fills, RTL, 16/32-frame buffers | FW-217 |
| T8 | Derived output safety + remaining instrumentation cleanup | FW-171 overlap |
| T9 | Reverse audit + docs | FW-220 |

**Not in this milestone:**
- 88.2/96 kHz (milestone 7).
- MIDI composition into packets (milestone 8). The fill must leave room for it.
- The telemetry ABI redesign (FW-175).

**Undecided and owned by no milestone yet:**
- the RTL residual (§1);
- the small-buffer glitches (§1).
