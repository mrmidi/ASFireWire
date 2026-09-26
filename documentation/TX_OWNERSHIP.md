# TX ownership: PCM → packet → DMA (milestone 6)

**Status:** T0 (design + inventory), T1 (goldens) and T2 (dead surfaces) done, 2026-09-26, branch `refactor/tx-ownership` off main `52d05ca8`.
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
| AM824 label on unfilled PCM | `0x00`, not MBLA `0x40` (T3 changes this) |

**The stall case, hypothesis only.** When CoreAudio resumes, `W` and the frame target (`W` + the 4160-frame
horizon) jump by the stalled frames. The producer then prepares ahead of the RX replay it depends on. Each underflow
commits a NO-DATA packet, and TX and RX then advance in step, so the gap persists.

The rig runs 0.5 s, shorter than the ~1 s `[TxExposure]` sampler that drives the self-heal, so recovery is not
exercised. On hardware this would be up to ~1 s of silence after a CoreAudio stall. **Not verified on hardware.**
It is the `W`/`E` rendezvous that T4 deletes.

Two earlier rig models were wrong and were corrected before these results were recorded:
- CoreAudio writing on the nominal clock for the 1814, which lost all PCM;
- CoreAudio acting only at interrupt boundaries, which made periodic 16-frame gaps.

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
| Payload clear before exposure | `AmdtpTxPacketizer.cpp:382-386` (policy set at `DiceTxStreamEngine.cpp:210`) | Zeroes the payload; an unfilled AM824 PCM slot goes out as `0x00000000` (label 0x00) | **REPLACE (T3)** → encoded silence (AM824 MBLA `0x40000000`) |
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
