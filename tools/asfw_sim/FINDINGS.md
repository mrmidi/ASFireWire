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

## F3 — the governing law

```
survivable producer stall (ms)  ≈  (kReadDelay + kTxDataHorizonPackets) / 8
```

| variant | readDelay | horizon | predicted | measured |
|---|---:|---:|---:|---:|
| **HEAD** | 256 | 400 | 82.0 ms | **78.0 ms** |
| readDelay 512 | 512 | 400 | 114.0 ms | 110.0 ms |
| readDelay 1024 | 1024 | 400 | 178.0 ms | 173.9 ms |
| horizon 160 | 256 | 160 | 52.0 ms | 56.4 ms |
| horizon 1200 | 256 | 1200 | 182.0 ms | 123.0 ms* |
| readDelay 1024 + horizon 800 | 1024 | 800 | 228.0 ms | 218.9 ms |

\* the horizon row saturates because this sweep varies `kTxDataHorizonPackets`
without the `kTxPreparationLeadPackets` it derives in the real header; E cannot
lead further than the 678-packet lead allows. In the driver the two move together.

**`kReadDelay` is not a history depth — it is the producer-stall recovery budget.**
It sets how many packets of catch-up burst can still be fed real RX timing after a
stall. HEAD's 256 packets = **32 ms** of tolerance, against an observed 68 ms
watchdog cadence: the Duet was running at **more than double** its budget, so
permanent audio death was guaranteed the first time the IT IRQ path went silent.

## F4 — consequences for the fix

- The planned change (`kReadDelay` 256→1024, `kCapacity` 512→2048) **is correct** and
  raises tolerance 32 ms → 128 ms (measured cliff 173.9 ms with the horizon). But the
  justification in the plan was wrong; the drift argument is irrelevant and the
  "structurally unfillable" argument is false. Rewrite the comment around F3.
- It is a **mitigation, not a cure**. It buys margin against `TX-IRQ-001`; it does not
  stop a longer stall from killing the stream permanently and silently.
- The cure is a **bounded, counted re-projection**: when the exposure frontier falls
  behind the write frontier, the frame cursor must re-align (one audible glitch)
  instead of staying permanently offset (total silence). Commit `81aab17` removed the
  realign-on-`overwritten` path to stop it orphaning frames — correct for that case,
  but it left no path back from a stall. Both behaviours are needed, discriminated by
  cause.
- Pending-frame retention (`AVC-TX-EXPOSURE-001`) does **not** help here: after the
  cliff no packet ever covers those frames again, so a pending range grows without
  bound and ends in an xrun.

## Reproduce

```bash
cd tools/asfw_sim && uv sync && uv run pytest -v
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
