# asfw_sim — Phase A findings (2026-07-19)

Run against HEAD `e5e0c1f` (`audio/stabilize`), constants parsed live from
`AudioTimingGeometry.hpp` / `AudioHalBufferProfiles.hpp` / `RxSequenceReplay.hpp`.

## Constants, as the driver actually defines them

Profile `dice-working-1536`. Every derived value reproduced by `headers.py`:

```
kHalIoPeriodFrames                 512      kTxCoverageLeadPackets           144
kFrameRingFrames                  1536      kTxFrameExposureWindowPackets    534
kTxDataHorizonPackets              400      kTxPreparationLeadPackets        678
kTxExposureLeadFrames             2400      kTxSharedSlotPackets             912
kTxExposureLeadPackets             438      kTimelineSlots                  1024
                                             RxSequenceReplay kCapacity       512
                                             RxSequenceReplay kReadDelay      256
```

## F1 — the triage reports' stated mechanism is WRONG

Both PDFs and `AVC_RECOVERY_AND_SYNC_ALGO_AND_BUGS.md` §4 claim the 678-packet
preparation lead is "structurally unfillable" from a 512-entry / 256-delay replay
ring, because TX would request entries RX has not yet observed (`fail=ahead`).

**Falsified.** The reader cursor is not tied to the packet index. It advances once
per *prepared packet*, and packets are prepared at the same long-run rate RX
publishes (both are 8000/s, bus-locked). So

```
reader - producer  ==  -kReadDelay   (constant, independent of the 678 lead)
```

A steady-state run at HEAD constants with a healthy RX produces **zero**
`ahead` and **zero** `overwritten`, 99.4 % of host frames written. The 678-vs-256
comparison is not a valid invariant, and `678 > 512` is not why streams die.

> Method note: an earlier revision of this model *did* reproduce a 100 % collapse
> here — because it omitted the frame-cursor projection at
> `ASFWAudioDriverZts.cpp:399-437`. That term maps an RX observation forward from
> the cycle it was received to the cycle the packet will be transmitted, and it is
> exactly what cancels the reader's lag. The "reproduction" was the model's bug,
> not the driver's. Recorded because it is the failure mode a sim like this invites.

## F2 — what actually kills the stream: a producer stall past a hard cliff

Inject a producer-wake stall (the observable signature of `TX-IRQ-001`: the IT
interrupt path dies and the ~68 ms `Poll()` watchdog carries the stream).

```
stall  0–78 ms   -> 99.4 % frames written, healthy
stall  78.0 ms   -> last surviving value
stall  78.1 ms   -> 0 % frames written, PERMANENTLY
```

The transition is a **cliff, not a slope**, and the death is **absolute and
permanent**: cumulative written-fraction decays exactly as `t_stall / t_total`
(82.2 % @6 s, 61.7 % @8 s, 49.3 % @10 s, 24.6 % @20 s, 16.4 % @30 s) — i.e. not one
further frame is written after the stall, forever.

`align == 1` throughout: the frame cursor never re-arms. The steady-state deficit
after collapse is only **~1000 frames (21 ms)** and **does not grow** — E advances
at exactly W's rate, permanently offset just behind it. Every frame arrives
slightly before its packet exists and is dropped.

That is the zombie precisely: transport perfectly alive, one packet per cycle,
valid CIP/SYT, DBC continuous, ~1 % of a buffer's worth of phase error — and 100 %
silence. No counter looks alarming.

## F3 — capacity, not read delay, is the recovery budget

> **Corrected 2026-07-19 by `scenarios/f3-readdelay-sweep.yaml`.** The first
> version of F3 claimed `survivable stall ≈ (kReadDelay + horizon)/8`. That law
> was fitted along a diagonal where every sweep happened to set
> `kCapacity = 2 × kReadDelay`, so it silently attributed to `kReadDelay` an
> effect that belongs to `kCapacity`. Varying them independently — which the YAML
> scenario did on its first run — falsifies it.

Measured stall-tolerance cliff (ms), 48 kHz, horizon 400:

| `kReadDelay` \ `kCapacity` | 512 | 1024 | 2048 | 4096 |
|---|---:|---:|---:|---:|
| **128** | 62.0 | 67.4 | 195.4 | **451.4** |
| **256 (HEAD)** | **78.0** | 78.0 | 179.1 | 435.1 |
| **512** | – | 110.0 | 146.9 | 402.9 |
| **1024** | – | – | 173.9 | 339.1 |

Two things the diagonal hid:

1. **`kCapacity` dominates.** It sets how far the reader may fall behind before
   its history is overwritten. During a stall the producer keeps publishing while
   the reader is frozen, so the lag grows by the stall length; survival requires
   the lag to stay inside the ring.
2. **`kReadDelay` is mildly *inverse*.** At a fixed capacity, a *larger* read
   delay lowers tolerance (4096: 451 ms at 128 → 339 ms at 1024), because the
   reader starts closer to the overwrite boundary and has less room to absorb the
   stall.

No clean closed form fits the whole surface — the earlier additive law holds only
on the diagonal it was fitted on. Treat the table as the result and re-measure
with `asfw-sim scenario` when any related constant moves.

HEAD's 78 ms tolerance still sits below the 68 ms watchdog cadence plus any
excursion, which is what made the Duet's dead-IRQ path fatal.

## F4 — consequences for the fix

The originally planned change (`kReadDelay` 256→1024, `kCapacity` 512→2048) is
**strictly dominated**. Ranked candidates:

| `kReadDelay` | `kCapacity` | stall tolerance | bring-up to `established` | slab |
|---:|---:|---:|---:|---:|
| 256 | 512 (**HEAD**) | 78.0 ms | 32.0 ms | 16 KiB |
| 1024 | 2048 (*first proposal*) | 173.9 ms | **128.0 ms** | 64 KiB |
| **256** | **2048** | **179.1 ms** | **32.0 ms** | 64 KiB |
| 128 | 2048 | 195.4 ms | 16.0 ms | 64 KiB |
| **128** | **4096** | **451.4 ms** | **16.0 ms** | 128 KiB |

**Recommendation: leave `kReadDelay` at 256 and raise `kCapacity` alone.**
It is a one-constant change, gives *more* tolerance than the original two-constant
proposal, and avoids the 32→128 ms bring-up regression that raising `kReadDelay`
would cause (`MarkEstablished()` requires `producer >= kReadDelay`, and that gates
deferred IT start). `kCapacity = 4096` buys 451 ms for 128 KiB of shared memory if
the extra margin is wanted.

Still true regardless of the constant chosen:

- This is a **mitigation, not a cure**. A stall past the (now larger) cliff still
  kills the stream permanently and silently.
- The cure is a **bounded, counted re-projection**: when the exposure frontier
  falls behind the write frontier, re-align the frame cursor (one audible glitch)
  instead of leaving it permanently offset (total silence). Commit `81aab17`
  removed realign-on-`overwritten` to stop it orphaning frames — right for that
  case, but it left no path back from a stall. Both behaviours are needed,
  discriminated by cause.
- Pending-frame retention (`AVC-TX-EXPOSURE-001`) does **not** help: after the
  cliff no packet ever covers those frames again.

## Reproduce

```bash
cd tools/asfw_sim && uv sync --extra dev && uv run pytest -v
uv run asfw-sim scenario          # the shipped hypotheses
```

## F5 — the CoreAudio buffer-size range (15–576) is a geometry consequence

AudioDriverKit exposes **no** setter for `kAudioDevicePropertyBufferFrameSizeRange`
(confirmed against the DriverKit 27.0 SDK: `IOUserAudioDevice.iig` has only
`Set{Input,Output}SafetyOffset`; `IOUserAudioStream.iig` has only
`SetIOMemoryDescriptor` and `SetLatency`). The HAL derives the range from the
stream's `IOMemoryDescriptor`, i.e. from `kFrameRingFrames = 1536` — the
compile-time profile `dice-working-1536`.

Both FireWire devices report exactly **15–576** despite different safety offsets
(Saffire 48/128, Duet 64/128) and different device latencies (29/29 vs 128/128),
so the bound is purely ring-derived and independent of those. Observed ratio
576/1536 = **0.375**.

> **Unverified.** One data point cannot establish the HAL's formula, and the
> MacBook Air (15–4096) does not disclose its ring size, so the 0.375 ratio is a
> conjecture, not a result. Confirm it with one build that changes only
> `frameRingFrames` and re-reads the property.

`asfw-sim plan-io --frames 4096` re-derives the whole dependent chain and checks
every `static_assert` in the header:

| constant | current | proposed |
|---|---:|---:|
| `kHalIoPeriodFrames` | 512 | **4096** |
| `kFrameRingFrames` | 1536 | **16384** |
| `kHalZeroTimestampPeriodFrames` | 1536 | **4096** |
| `kTxDataHorizonPackets` | 400 | **696** |
| `kTxExposureLeadFrames` | 2400 | **4176** |
| `kTxFrameExposureWindowPackets` | 534 | **1506** |
| `kTxPreparationLeadPackets` | 678 | **1650** |
| `kTxSharedSlotPackets` | 912 | **1704** |
| `kTimelineSlots` | 1024 | **2048** |

All asserts hold. Costs: shared TX slab 1831 → 3421 KiB/stream, and prefill
before IT arm 114 → **213 ms** (StartIO validates `committedEnd == numSlots`
before arm, so prefill latency scales with `kTxSharedSlotPackets`).

Simulated at 48 kHz, 20 s:

| geometry | clean | after a 100 ms stall |
|---|---:|---:|
| current (512 IO) | 99.4 % | **24.6 % (collapsed)** |
| proposed (4096 IO) | 98.8 % | 70.7 % (degraded) |
| proposed + `kReadDelay` 2048 | 97.7 % | **97.7 %** |

The larger horizon that a 4096-frame IO budget forces *also* raises stall
tolerance 82 → 119 ms as a side effect. Pairing it with `kReadDelay = 2048`
(343 ms) makes the stall path a non-issue — the buffer-size work and the F3 fix
are complementary, not competing.

## F6 — the direct answer: why `W > E` in the current implementation

`W > E` does **not** require a stall, a bus reset, or an RX epoch change. It is
produced by a **monotonic ratchet** that any RX observation loss drives, and that
the design has no path back from.

### The chain, each link in code

1. `DirectAudioReceiveConsumer::ConsumePacket` early-returns on
   `packet.payload.empty()` (`DirectAudioReceiveConsumer.cpp:150`). An errored or
   zero-length IR descriptor therefore publishes **no** replay entry — while the
   device really did send those frames, so the true audio timeline advanced.
2. `PrepareTransmitSlots` consumes exactly **one** replay entry per prepared
   packet (`ASFWAudioDriverZts.cpp:241`). One fewer entry published means the
   reader reaches the producer and returns `kAheadOfProducer`.
3. `ahead` ships a NODATA packet and holds the reader
   (`ASFWAudioDriverZts.cpp:308-317`), and **NODATA never advances
   `exposedFrameEnd_`** (`AmdtpTxPacketizer.cpp:209-219`). `E` stands still for
   that packet.
4. `W` is continuously re-anchored to the device clock through the ZTS anchor, so
   it keeps advancing at the true frame rate regardless.
5. `AlignFrameCursorOnce` is **one-shot** (`AmdtpTxPacketizer.cpp:126-135`) and
   `ahead` deliberately does not re-arm it — correctly, since commit `81aab17`,
   because re-arming orphans host frames. **But that leaves no path back.**

Each lost RX observation therefore costs `E` **~6.8 frames** of ground against
`W`, permanently and cumulatively.

### Measured (48 kHz, 1 RX observation lost per 200 cycles = 0.5 %)

```
 t(s)         W         E      W-E    ahead  align  state
    4    191488    193888    -2400        0      1  ok      <- E leads by exactly the horizon
    8    383488    385600    -2112      328      1  ok
   12    575488    576304     -816      488      1  ok
   16    767488    767048     +440      648      1  SILENT  <- W crosses E
   20    959488    957760    +1728      808      1  SILENT
   36   1727488   1720624    +6864     1448      1  SILENT
```

Linear, unbounded, never recovers. `align` stays 1 throughout.

| RX loss | lost/s | W−E after 30 s | frames written |
|---:|---:|---:|---:|
| 0 % | 0 | −2400 (healthy) | 99.6 % |
| 0.125 % | 10 | −1520 | 99.6 % |
| 0.5 % | 40 | **+5200** | 45.6 % |
| 1.25 % | 100 | +18640 | 22.0 % |
| 5 % | 400 | +85840 | 10.2 % |

### The load-bearing consequence

`kTxExposureLeadFrames = 2400` is **not a steady-state cushion. It is a one-time
budget of ~320 lost RX observations for the entire life of the stream.** Nothing
ever replenishes it.

At 8000 packets/s that is 0.04 % of an hour's traffic. A device or controller that
drops one packet in 800 kills audio in ~64 s; one in 200 kills it in ~16 s. This
is exactly "cold start is perfect, dies soon enough", and it explains the observed
zombie without invoking `TX-IRQ-001` at all — though a stall (F2) reaches the same
end state faster.

> The §5 finding in `AVC_RECOVERY_TRIAGE_FINDINGS.pdf` flagged this same
> early-return, but as a `receive-cycle-gap` mis-attribution. The real damage is
> larger and quieter: it silently steals the exposure budget.

### What the fix has to do

- **Necessary:** never let a lost observation cost the timeline frames. Publish a
  replay entry for a dropped/errored descriptor using the deterministic blocking
  cadence for `dataBlocks` (the frame count is knowable even when the SYT is not),
  so `E` keeps pace with the device.
- **Sufficient on its own only if loss is the sole source.** The general cure
  remains a **bounded, counted re-projection**: when `W − E` exceeds a threshold,
  re-align once, count it, and log it — one audible glitch instead of permanent
  silence.
- Growing `kCapacity` (F4) does **not** help here: the entries were never
  published, so no amount of history contains them.

## F7 — the sim can classify a real run without another hardware session

RX loss (F6) and a producer stall (F2) reach the **same end state**: `W > E`,
`align == 1`, `ahead` in the hundreds, transport perfectly healthy, near-total
silence. Counters alone cannot separate them. The **slope of `W − E`** can:

| cause | written | `ahead` | `reclamp` | `W−E` | **slope/s** |
|---|---:|---:|---:|---:|---:|
| healthy | 99.6 % | 0 | 1 | −2400 | **0** |
| rx-observation-loss (F6) | 45.6 % | 1208 | 1 | +5200 | **+311** |
| producer-stall (F2) | 16.4 % | 886 | 2 | +1032 | **−9 (flat)** |
| replay-ring-churn | 99.6 % | 299 | 1 | −2400 | −2 |
| scheduler-latency | 99.6 % | 0 | 1 | −2400 | 0 |

Two things fall out:

- **`ahead` is not diagnostic.** A small replay ring emits 299 of them with audio
  fully intact. Any triage that treats `fail=ahead` as the fault will chase the
  wrong defect.
- **A budget being *consumed* ramps; a budget *spent once* goes flat.** That is
  the whole classifier.

The ramp also inverts to a rate: at a measured 6.79 frames of `E` forfeited per
lost observation, slope ÷ 6.79 = lost RX packets/s.

```bash
uv run asfw-sim fingerprint          # the table above, recomputed from live geometry
uv run asfw-sim diagnose --deficit-start -2400 --deficit-end 380 --elapsed-s 60
#   rx-observation-loss (F6)  (confidence: high)
#   estimated RX loss: 6.8 packets/s (0.085% of 8000/s)
```

Two `[PayloadWriter]` deficit values and the seconds between them are the entire
input — no FireBug trace, no full ring dump.

> **Threshold note.** An earlier revision used a noise floor of one IO period per
> second (51 frames/s) and classified a −2400 → +380 ramp as a *stall*. That
> hides the dangerous case: a leak slow enough to look stable in a short capture
> still spends the horizon eventually — 2400 frames over an hour is only
> 0.67 frames/s. The floor is now 1 frame/s and any positive slope reports a
> time-to-silence instead of a verdict implying safety.

**Limit.** This classifies; it does not observe. The sim still cannot tell you
whether *your* hardware drops packets — it tells you what to conclude once you
have two deficit samples from the ring.

## F8 — hardware confirmation, and a self-inflicted reproduction

### F6 confirmed on hardware: 22.6 frames/s ramp

A 58-minute Duet session, sampled from the MCP ring at two points 23 minutes
apart within the *same* session (`comp` is a packet counter at 8000/s, so it
gives exact elapsed time):

```
comp 17,033,508  d =  77,124        <- W - E, frames
comp 28,092,828  d = 108,292
                 slope = +22.6 frames/s
```

Classifier verdict: `rx-observation-loss (F6)`, estimated **3.3 lost RX
packets/s = 0.042 %**. One packet in 2400. At that rate the 2400-frame horizon is
spent in **107 seconds** — which is exactly "cold start is perfect, dies soon
enough".

Supporting evidence from the same records: `w=0 noPkt=6656 outside=0` (100 %
silence, none of it slot reuse); `prepared=21077837/7037485/28115322` →
DATA fraction **0.7497**, a textbook 48 kHz blocking cadence, i.e. transport
perfectly healthy; `aligned=1 epoch=3`, the cursor never re-arming.

`gen=197462/131096` is the predicted `MarkHandled` divergence: the handled
generation is frozen while the requested one climbs, because `audioTargetSatisfied`
is never true (`ASFWAudioDriverZts.cpp:884-896`).

> **Window-length caveat, learned the hard way.** The first 2.7-second sample of
> this same stream showed a slope of **−2.9 frames/s** — flat, which the
> classifier calls a *stall*. The ramp is only visible over minutes. A short
> capture will confidently misclassify F6 as F2. Sample over ≥ 5 minutes, or use
> `comp` deltas to reconstruct a long baseline from two records.

### AVC-RECOVERY-001 reproduced live

An `asfw://telemetry/snapshot` MCP read during the active run issued
`GetAVCUnits` plus a full PHY dump; `PHY Read reg 0` **timed out for 115 ms** —
past the 78 ms cliff of F2. The AV/C backend escalated and the recovery failed
exactly as the triage documents predicted:

```
IT: Prime failed - committed prefill=306582 must cover 48 descriptors within 912 slots
[FSM] terminal state=Failed cause=StartTransmit status=0xe00002c9
```

Full timeline: `captures/2026-07-19-duet-avc-recovery-001.md`. This confirms
`AVC-RECOVERY-001` (stale `committedEnd` reaches `Prime`) and `AVC-RECOVERY-002`
(a 115 ms transient, with `busResetCount=0` and a healthy device, escalated into
CMP teardown, IRM release, and a terminal `Failed` state).

**Operational rule: during an active audio run use only `asfw_log_query`.**
`read asfw://telemetry/snapshot`, `health`, `summary`, and discovery all issue
MMIO on the driver's queue, and a single PHY timeout exceeds the stall cliff.

## F9 — a THIRD mechanism: E advances at a slower *rate* than W

The 2026-07-19 cold-start capture (`captures/2026-07-19-duet-coldstart-to-silence.json`,
192 s, 337 cursor points) does not match F6 or F2.

```
lead E-W:  +1352  ->  -39,860 frames     crossed zero at ~57 s
W rate:    48,000.0 frames/s
E rate:    47,785.1 frames/s             <- 4,478 ppm slow
replay failures: ahead=19, reclamped=22  <- FAR too few for F6
```

Nineteen `ahead` events cannot account for ~40,000 frames of lost ground at
~6.8 frames each. This is not the event ratchet. **`E` simply advances at a
slower rate than `W`, continuously.**

An independent cross-check from the earlier 58-minute session's cumulative
counters: `prepared=21077837/7037485/28115322` gives a DATA fraction of
**0.749692** against the 0.750000 that 48 kHz blocking requires — **410 ppm
slow** on its own, from cadence alone.

Three mechanisms now reach the same silent end state, and they are separable:

| | driver | `W-E` shape | signature |
|---|---|---|---|
| **F2** | producer stall > cliff | one step, then flat | burst of `ahead`, `align=1` |
| **F6** | lost RX observations | linear ramp | `ahead` count ∝ deficit |
| **F9** | E/W rate mismatch | linear ramp | **`ahead` count ≪ deficit** |

F6 and F9 both ramp, so the slope alone does not separate them — the
discriminator is whether the `ahead` count can *pay for* the lost frames at
~6.8 frames each. In this capture it cannot, by three orders of magnitude.

**Open:** why the TX cadence under-produces. Candidates, none yet tested — the
replayed RX cadence itself being short of 0.75; NODATA emitted on paths that do
not increment `ahead`; or `dataBlocks` being taken from an entry whose frame
count does not match what the device sent. `asfw-sim capture` now makes this
measurable per run instead of per shell one-liner.
