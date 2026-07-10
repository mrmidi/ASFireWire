# Sample-Rate Expansion Strategy: 44.1 kHz Family and Beyond

**Status:** accepted design analysis (2026-07-09). Implementation not started;
wire enablement additionally gated on hardware captures (§8).

**Relationship to [`44100.md`](44100.md):** that document is the
reverse-engineering evidence base (AppleFWAudio DCL/NuDCL, Saffire kext). This
document is the implementation decision derived from it, after completing the
cross-check its open question #6 required: validation against the Linux ALSA
FireWire stack, now present in-tree.

**Reference pinning:** Linux citations below refer to
`references/linux-sound-firewire-stack/` (symlink into a sparse clone of
`github.com/torvalds/linux`, commit `2c7c88a412aa` fetched 2026-07-09,
gitignored/local-only). Line numbers are valid for that commit. Do not copy
code from references (GPLv2); behavioral truth only.

---

## 1. Decision at a glance

Implement **two** of the three candidate models from `44100.md`, in this order:

1. **Extend the RX-sequence replay path (44100.md §9, "Saffire model") to the
   44.1 family first.** This is not new architecture: replay is already ASFW's
   production TX cadence source at 48 kHz (§2 below). Replay is
   cadence-agnostic — at 44.1 kHz the device's own stream carries the correct
   blocking pattern and real-oscillator SYT deltas, and ASFW reproduces them
   without knowing the rate's fractional structure. The 44.1 work therefore
   reduces to *rate plumbing* (§6), not a cadence engine.

2. **Then add one rational, tick-domain "ideal sequence" generator** — the
   Apple NuDCL phase/deadline model of 44100.md §7–8, which cross-validates as
   mathematically identical to Linux's synthesized sequence path (§4 below).
   One accumulator parameterized by `(sampleRate, sytInterval)` serves every
   rate, replaces both hardcoded 48k cadence classes, and covers the cases
   replay cannot: pre-warmup start, playback-only devices, unusable RX SYT,
   and future AVC/Oxford paths.

3. **Do not implement the legacy 5/6-frame non-blocking model (44100.md §4)
   now.** Every current target is blocking
   (`ASFWDriver/Audio/Protocols/DeviceStreamModeQuirks.cpp:33,43,47`; Linux
   DICE is `CIP_BLOCKING`, `dice/dice-stream.c:508`). Design the rational
   engine so non-blocking is the same accumulator with unit = frame instead of
   unit = SYT event (in Linux the difference is ~10 lines on shared state);
   build it only when a non-blocking device exists to test against.

---

## 2. Pivotal codebase fact: replay is already the shipped TX architecture

`44100.md` §9 presents the Saffire measured-SYT replay as a comparison point.
Reading the live TX path shows it is not a candidate — it is the design ASFW
already ships at 48 kHz:

- `PrepareTransmitSlots`
  (`ASFWDriver/Audio/DriverKit/ASFWAudioDriverZts.cpp:198-362`) prepares every
  TX packet by reading one `RxSequenceEntry` from `rxSequenceReplay` and
  copying the device's `dataBlocks` into `timing.replayDataBlocks`
  (`:261`). The TX data/no-data cadence **is the device's RX cadence,
  replayed 1:1** through a 512-entry ring consumed 256 entries behind the RX
  writer (`Audio/Wire/AMDTP/RxSequenceReplay.hpp:95-99`) — the exact Saffire
  geometry (512-delta ring, 256 read delay).
- TX SYT is the device's delay-free SYT offset re-anchored to the output
  packet's bus time plus `txTransferDelayTicks`
  (`ASFWAudioDriverZts.cpp:291-296` via `ComputeReplaySytFromTicks`).
- `RxSytCadence` (`Audio/Wire/AMDTP/RxSytCadence.hpp`) re-expresses the
  Saffire kext's delta ring and cites the same IDA ranges as `44100.md` §9.
- The local `Blocking48kCadence` is vestigial: `PrepareTransmitSlots` always
  sets `timing.replayValid = true` (`:206`), so the packetizer's local-cadence
  branch (`Audio/Wire/AMDTP/AmdtpTxPacketizer.cpp:115-127`) is never consulted
  in steady state. Before replay warms up, TX sends CIP-header-only no-data
  packets; if replay underruns mid-stream, TX fatals (`TxReplayUnavailable`)
  rather than falling back.

This matches mainline Linux for DICE exactly: `dice/dice-stream.c:453` starts
the AMDTP domain with `replay_seq=true, on_the_fly=false`, and
`pool_seq_descs` (`amdtp-stream.c:560-587`) selects `pool_replayed_seq`
(replay the device's cached `data_blocks` + `syt_offset` sequence,
`amdtp-stream.c:542-559`, cache filled by `cache_seq`, `:506-529`) over
`pool_ideal_seq_descs` (synthesized, `:531-540`).

**ASFW's TX is Linux's replay mode. What ASFW lacks is Linux's other source —
the ideal sequence — which is precisely the Apple NuDCL model.** The
44.1-family question is therefore not "which model to adopt" but "finish the
model pair Linux and Apple both converged on."

## 3. Consequence for 44.1: cadence comes from the device

For a DICE duplex device at 44.1 kHz, the device transmits its own blocking
schedule — 8-frame data packets, ~441 data packets per 640 cycles, no-data
elsewhere, SYT deltas of 4458/4459 ticks reflecting its real oscillator. The
replay ring reproduces that pattern on TX without any 44.1-specific code.
Following the device's realized clock (including its fractional pattern and
drift) is strictly stronger than regenerating a nominal pattern — this is the
Saffire lesson of `44100.md` §9, and it is why the fractional-cadence problem
largely disappears for the duplex case.

## 4. Cross-validation results (44100.md open question #6 — resolved)

The Apple-derived model survives the Linux check in full:

**Apple NuDCL ≡ Linux ideal blocking.** Linux `calculate_syt_offset`
(`amdtp-stream.c:410-447`) is a per-cycle deadline DDA in the 24.576 MHz tick
domain: if the next SYT deadline lands inside the current cycle, emit that
offset and advance the deadline by ~4458.23 ticks; otherwise subtract one
cycle and report `SYT_NO_INFO`. `pool_blocking_data_blocks`
(`amdtp-stream.c:350-366`) then sets `data_blocks = syt_interval` iff the SYT
is valid — i.e. **the data/no-data decision is a byproduct of the SYT deadline
machine**, exactly the structure `44100.md` §7 recovered from
`AM824NuDCLWrite::CalculatePacketHeaderData()`. Two independent,
hardware-proven stacks converged on one algorithm.

Numeric checks:

| Quantity | Apple (IDA, 44100.md) | Linux (amdtp-stream.c) | Exact value |
|---|---|---|---|
| SYT step at 44.1, ticks | `44,582,312 / 10^4 = 4458.2312` | `1386/1387` sequence over 147 events (`:420-436`) | `24,576,000 × 8 / 44,100 = 4458.23129…` |
| 147-event period sum | — | `147 × step = 655,360` ticks exactly (34 carries per 147) | matches Saffire's observed 34-long-per-147 delta pattern (44100.md §9) |
| Blocking data ratio at 44.1 | 441 data / 640 cycles (§7) | same (falls out of the deadline DDA) | `44,100 / 8 / 8,000 = 441/640` |
| SYT intervals | 8/16/32 by family (§9 `GetSYTIntervalBySampleRate`) | `amdtp_syt_intervals` (`:138-146`): 8 @32k/44.1/48, 16 @88.2/96, 32 @176.4/192 | matches ASFW `AmdtpRateGeometryForSampleRate` (`Audio/Wire/AMDTP/AmdtpRateGeometry.hpp:52-64`) |
| Non-blocking 44.1 | 5/6 frames, 39×5 + 41×6 per 80 cycles (§4) | `pool_ideal_nonblocking_data_blocks` (`:368-407`): same distribution, phase-rotated (`6 6 5 6 5…`) | `441 = 39×5 + 41×6` |

Notes carried forward from the comparison:

- Linux's non-blocking rotation is deliberate: *"packets with a rounded-up
  number of blocks occur as early as possible in the sequence (to prevent
  underruns of the device's buffer)"* (`amdtp-stream.c:396-401`). Preserve
  this if non-blocking mode is ever built.
- For blocking mode Linux adds one SYT interval of ticks to the transfer
  delay: `transfer_delay += TICKS_PER_SECOND * syt_interval / rate`
  (`amdtp-stream.c:291-292`) — at 44.1 that is ~4458 ticks on top of the
  `TRANSFER_DELAY_TICKS = 0x2e00` base (`:29`). Apple's startup seed
  (`117,220,000`) and 15-cycle presentation lead are Apple-internal policy;
  ASFW's own knob is `txTransferDelayTicks` (default 12800,
  `Audio/DriverKit/Runtime/AudioTransportControlBlock.hpp:158-159`). The
  per-rate blocking adjustment is reference behavior to evaluate during
  capture validation.
- Apple's fixed-point step (`44,582,312`) carries ~0.02 ppm error; Linux's
  147-period integer sequence is exact. A 64-bit rational accumulator
  (`num = 24,576,000 × sytInterval`, `den = rate`) reproduces the exact
  sequence with no drift and no per-rate magic numbers — prefer it.

## 5. Option-by-option verdict

| Option (44100.md §) | Verdict | Rationale |
|---|---|---|
| §9 Saffire replay | **Extend to 44.1 — primary** | Already the shipped TX architecture; rate-agnostic by construction; matches Linux DICE (`replay_seq=true`); follows the device's real oscillator rather than a nominal rate. |
| §7–8 Apple NuDCL phase/deadline | **Second — as the single synthesized engine for all rates** | Cross-validated as the same algorithm as Linux's ideal path. Required where replay cannot work: pre-warmup, playback-only devices, unusable RX SYT (Linux `CIP_UNAWARE_SYT` analog), future AVC/Oxford. Today these cases FATAL (`TxReplayUnavailable`); Linux degrades to ideal instead. |
| §4 legacy 5/6 non-blocking | **Defer** | No current consumer (all profiles blocking). Keep reachable as a parameterization of the same engine; build only with hardware to test. |

Design rules for the synthesized engine (per the no-double-paths doctrine):

1. **One rational accumulator, not per-rate classes.** State lives in the
   FireWire tick domain; the data/no-data decision, SYT offset, DBC advance,
   and 16-cycle wrap all derive from the same phase state — the conclusion of
   `44100.md` §8, now Linux-confirmed. 48 kHz becomes the degenerate exact
   case (step 4096 → `N,D,D,D`), so `Blocking48kCadence` and
   `NonBlocking48kCadence` (`Audio/Wire/AMDTP/AmdtpCadence.{hpp,cpp}`) are
   **deleted, not kept alongside**. Every future rate — 32 kHz included — is a
   parameter, not new code. This is what makes continuous rate expansion
   cheap.
2. **Ownership boundaries stay as drawn in 44100.md §8** (the code already
   honors them): the engine lives in `Audio/Wire/AMDTP`; decisions stay in
   bus-tick time; `Isoch/` remains payload-opaque; host time explains the bus
   clock to CoreAudio (ZTS) and never sources packet cadence.

## 6. Gap inventory for 44.1 kHz (what actually changes)

The cadence is the easy part; the 48k assumption lives in the plumbing.
In rough dependency order:

1. **Packetizer gate.** `AmdtpTxPacketizer::Configure` hard-rejects
   `sampleRate != 48000` (`Audio/Wire/AMDTP/AmdtpTxPacketizer.cpp:43`).
2. **Non-integer ticks-per-frame.** The ZTS frame projection divides by
   `kTicksPerSample48k = 512`
   (`ASFWAudioDriverZts.cpp:349-358`, constant at
   `Audio/Wire/AMDTP/AmdtpTiming.hpp:21`). At 44.1, ticks/frame = 557.278… —
   all such conversions must become 64-bit rational
   (`frames = deltaTicks × rate / 24,576,000`).
3. **`RxSytCadence` bootstrap.** `kNominalStepTicks48k` (4096) seeds the first
   delta (`Audio/Wire/AMDTP/RxSytCadence.hpp:19-20`); needs the per-rate
   nominal (44.1: 4458 exact; the Saffire kext used 4456 as its bootstrap —
   `44100.md` §9 — exactness is better and free).
4. **`AudioTimingGeometry`**
   (`Shared/Isoch/AudioTimingGeometry.hpp:29-47`): `kSampleRateHz = 48000`,
   `kCadenceBlockPackets/Frames` (4/24) and every packet↔frame conversion
   assume 6 frames/cycle; 44.1 averages 5.5125 (441/80) and needs the rational
   form. Convenient accident: a 6-packet interrupt group still carries 32 or
   40 frames at 44.1 blocking (4–5 data events × 8 frames), so
   `kMin/MaxNominalFramesPerInterrupt` hold at 1x rates — and the sim proved
   the bounds are **tier-uniform across both families** ({32,40} at 44.1/48,
   {64,80} at 88.2/96, {128,160} at 176.4/192, exact {24} at 32k), so
   per-tier values cover 44.1 and 48 together. The static asserts
   tying ring wrap to the 4-packet cadence period cannot generalize (the 44.1
   blocking period is 640 cycles, which does not divide the 512-slot
   timeline); they only protect the local 48k cadence and should be replaced
   by exposure-based invariants once the cadence engine is stateful.
5. **DICE bring-up is rate-hardwired.** The protocol API is literally
   `PrepareDuplex48k` with `kDiceClockSelect48kInternal`
   (`Audio/Protocols/DICE/TCAT/DICETcatProtocol.cpp:240-249`;
   `Audio/Protocols/IDeviceProtocol.hpp:63-83`). Parameterize by rate using
   the DICE rate-code index (Linux `snd_dice_rates` + `CLOCK_RATE_MASK`,
   `dice/dice-stream.c:19-90`), and read `GLOBAL_CLOCK_CAPS` (offset `0x64`,
   already named at `Audio/Protocols/DICE/Core/DICETypes.hpp:202` but never
   read) to populate the nub's supported-rate list instead of assuming.
6. **HAL rate switching is not wired.** The ADK graph already publishes
   multiple rates (`ASFWAudioDriverGraph.cpp:294-306`,
   `SetAvailableSampleRates` from nub properties), but there is no
   `PerformDeviceConfigurationChange` handler, so a HAL-initiated rate change
   has no path to *stop streams → reprogram CLOCK_SELECT → restart duplex*.
   `DiceDuplexRestartCoordinator` is the natural vehicle: a rate change is a
   restart with new parameters.
7. **Fallback-ladder policy (explicit decision).** Keep start-up as-is
   (no-data until replay warms, ≈32 ms — behaviorally equivalent to Linux
   `on_the_fly=false`), keep FATAL on mid-stream replay underrun, and treat
   the ideal engine as the source for *non-replay device classes* — not as a
   mid-stream hot-swap. Hot-swapping cadence sources mid-stream is exactly the
   double-path trap.

The RX side needs essentially nothing for cadence: it decodes per-packet
`dataBlocks` from the CIP header, and the replay ring stores whatever arrives.

### ZTS chain audit (2026-07-10): structurally rate-clean, three residuals

The CoreAudio zero-timestamp path was audited for rate dependencies and is
already rate-agnostic by construction — 44.1 needs no ZTS redesign:

- The anchor is `(sampleFrame, hostTicks)`: `sampleFrame` is decoded-frame
  *counting* (CIP data blocks — works under any cadence) and `hostTicks` is
  the packet receive host timestamp. No rate constant anywhere
  (`HostClockAnchor.hpp`).
- `hostNanosPerSampleQ8` is computed live as `(10^9 << 8) / sampleRateHz`
  (`IsochReceiveContext.cpp:558-563`) from the shared `AudioStreamProfile`,
  not from a 48k constant — and the ADK mirror forwards only
  `sampleFrame`/`hostTicks` to `UpdateCurrentZeroTimestamp`; CoreAudio derives
  the effective device rate from the anchor cadence itself, so 44.1 emerges
  automatically from the device clock.
- The anchor-publish gate is `packetFirstAudioFrame % kHalZeroTimestampPeriodFrames
  == 0` — a frame-count modulo. The active profile (`dice-working-1536`)
  satisfies `1536 % {8,16,32} == 0`, so every ZTS boundary coincides with a
  packet-first frame at every family rate and boundaries are always observed.

Residuals:

1. **Make the boundary-observability invariant explicit**: anchors are
   published only when a packet's *first* frame lands exactly on the ZTS
   period boundary, which silently requires
   `zeroTimestampPeriodFrames % framesPerDataPacket == 0` (8/16/32). True
   today by luck of 1536; add a static assert in
   `AudioHalBufferProfiles.hpp` so a future profile cannot silently stop
   anchor publication.
2. `RxSytCadence` establishment (which gates anchor publishing via
   `rxClockEstablished`) warms up over 512 data packets: 92.9 ms at 44.1 vs
   85.3 ms at 48k — no change needed, just the per-rate bootstrap seed from
   item 3 above.
3. **Tooling**: `tools/zts_sim.py` hardcodes `SAMPLE_RATE = 48_000` and its
   slope identities (`MACH/48000`); it needs a `--rate` parameter before it
   can verify 44.1 `[Zts]`/`[TxSyt]` captures.

(The TX-side `kTicksPerSample48k` projection is item 2 above; note the
constant is defined twice — `AmdtpTiming.hpp:21` and `TimingUtils.hpp:56` —
and both copies plus their pinning tests go away with the rational form.)

## 7. Roadmap

- **Phase 1 — 44.1 kHz via replay on DICE.** Items 1–6 above. Validation:
  extend `tools/asfw_timing_geometry_sim.py` (currently hardcodes
  `CADENCE = (8,8,8,0)` and `SAMPLE_RATE = 48000`) to rational cadences;
  extend `tests/audio/BlockingCadenceTests.cpp`; then one batched hardware
  session with a 44.1 capture (§8).
- **Phase 2 — the rational ideal engine**, replacing both 48k cadence
  classes. Golden-sequence unit tests are pure math and host-testable: the
  147-period 1386/1387 SYT sequence, the 441/640 D/N pattern, `N,D,D,D` at
  48k, and the 5/6 non-blocking distribution are all independently derivable
  for byte-exact comparison. **The Python-side validation exists:**
  `tools/amdtp_blocking_cadence_sim.py` simulates the rational engine, a port
  of the Apple NuDCL fixed-point machine (44100.md §7 constants), and the
  replay path, and asserts every oracle above plus the §6 geometry claims
  (6-cycle group frame bounds, ring-wrap non-alignment, DBC continuity, exact
  replay rate-lock at ±500 ppm), per-second packet-rate accounting (8000 pps
  at every rate; {5512,5513} data packets per 1 s window in the 44.1 family,
  exact on 2 s boundaries), and the ZTS anchor model (boundary observability,
  278–279-cycle anchor spacing at 44.1, slope bounded by one cycle, Q8
  quantization ≤ 0.114 ppm, and the ~25%-observability counterexample for a
  period that violates the §6 static-assert residual). Notable measured result: the Apple
  fixed-point model tracks the exact engine bit-identically for ~10.26 s of
  bus time, then precesses ~0.0208 ppm fast (first D/N flip at the cycle its
  0.925-unit/event step deficit predicts, verified closed-form) — harmless on
  the wire, but the exact rational engine is the better implementation and
  gives deterministic golden tests.
  **Phase-2 scaffolding now exists, but is deliberately inactive in the
  packetizer:** `RationalBlockingCadence` is a standalone exact integer
  primitive, and `tools/amdtp_blocking_cadence_sim.py --write-golden
  tests/audio/generated/AmdtpBlockingCadence441Golden.hpp` emits its
  zero-seed 441/640 cadence plus 147-event SYT oracle. `BlockingCadenceTests`
  compares the C++ primitive against that generated data; `--verify-golden`
  detects an out-of-date checked-in vector. `AmdtpTxPacketizer` still selects
  only the existing 48 kHz cadence classes, so none of this enables 44.1 kHz
  or chooses a wire-visible startup seed.
- **Phase 3 — 88.2/176.4 kHz.** Same engine with `sytInterval` 16/32. The
  geometry ladder (`Shared/Isoch/AudioGeometryPolicy.hpp`) already branches on
  `rate > 48000/96000`, but `ValidAtRate(88200/176400)` static asserts and the
  2x/4x frames-per-interrupt-group bounds need adding; DBC advances by 16/32;
  packet sizes and isoch bandwidth need re-validation at high channel counts.
- **Phase 4 — 32 kHz and rate-switch UX.** 32 kHz is integer (4 frames/cycle)
  and trivial under the same engine. Later: the AVC/Oxford path —
  `AVCDiscovery` currently forces 48 kHz even when device caps report 44.1
  (`Protocols/AVC/AVCDiscovery.cpp:409-451`); that path is where the
  synthesized engine becomes load-bearing, since such devices may not offer a
  replayable RX stream.

## 8. Open risks / capture gates

Wire enablement of 44.1 remains gated on `44100.md` open questions 7–9:

1. Device tolerance of ASFW's warmup no-data prefix at 44.1 (blocking seed and
   no-data placement). **The engine's startup seed is not a wire contract**:
   the sim's zero-seed golden prefix (`DDDN…`) is an engine regression anchor
   only. Apple's recovered seed opens `N,N,N,D…`; an accumulate-then-test
   accumulator opens `N,D,D…` — all rate-identical, differing only in start
   phase, which *is* wire-visible. Seed, presentation lead, reset behavior,
   and the cycle-epoch relationship are a separate deliverable derived from
   the active bus epoch or a capture before the synthesized engine ever
   drives a live stream. (In the replay-driven DICE path this is moot at
   steady state — the device dictates placement and startup is a no-data
   prefill — so the exposure is the ideal-engine fallback mode and
   prefill/restart policy.)
2. DBC-in-no-data convention confirmation against the target device (ASFW
   carries DBC unchanged in no-data packets, matching the working 48k state).
3. The transfer delay/presentation lead the device actually expects
   (Linux adds one SYT interval for blocking; ASFW uses a flat 12800 ticks).
4. The Apple NuDCL resynchronization branch (44100.md §7, deliberately
   unrecovered) does **not** need porting: in this architecture its job is
   done by replay re-anchoring plus the existing half-ring utilities
   (`extOffsetDiff`, `Audio/Wire/AMDTP/AmdtpTiming.hpp:47-56`). If a
   synthesized-mode drift servo is ever needed, that is a separate,
   capture-driven design.

Housekeeping noted during analysis: `IsochEventGroup.hpp:30`
(`TimingGroupPacketCount48k()`) is packet-domain and rate-independent — the
name falsely implies a rate dependency; rename during Phase 1.
