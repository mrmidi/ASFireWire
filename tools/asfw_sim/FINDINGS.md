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
