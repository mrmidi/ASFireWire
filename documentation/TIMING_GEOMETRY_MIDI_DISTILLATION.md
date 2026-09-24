# midi timing/HAL geometry — distillation

Epic FW-177, ticket FW-179. This reviews the timing/HAL geometry work on `origin/midi` **as research
output**, not as code to copy. For each abstraction it records:
- the problem it solved, and whether that problem still exists on `main`;
- whether the abstraction is fundamental or experimental scaffolding;
- which of its inputs are facts and which are policy;
- the verdict: `KEEP AS-IS`, `KEEP MATH / REHOME`, `SIMPLIFY`, `MERGE`, `DELETE`, or `DEFER`.

Row IDs `G-nn` refer to [`TIMING_GEOMETRY_INVENTORY.md`](TIMING_GEOMETRY_INVENTORY.md).

Epic 1 already fixed one constraint ([`RESOLVED_DEVICE_POLICY_BOUNDARY.md`](RESOLVED_DEVICE_POLICY_BOUNDARY.md)):
"Nub/HAL geometry and timing" belongs to this epic, and `midi::ResolvedAudioEndpointProfile` must not be
copied wholesale.

**The reference stacks are not checked out in this container** (`references/` is absent). Every
Linux claim below comes from midi's own source comments (`amdtp-stream.c:302-308`). It must be
cross-checked against `references/linux-sound-firewire-stack/` before any behaviour that depends on it
changes on the wire.

## 1. midi's model in one picture

```
IAudioDeviceProfile / RateTimingPolicy (per rate, data)   AmdtpRateGeometry (fdf, SYT interval)
            │                                                        │
            └──────────────┬─────────────────────────────────────────┘
                           ▼
        DeriveHalTimingProjection(profile, rate, channels, tuning?, limits?, revision)
                           │   (Audio/DriverKit/Config/HalTimingProjection.hpp)
                           ▼
        ResolveAudioGeometry(formation, timingPolicy, tuning?, limits, revision)
                           │   (Audio/Shared/AudioGeometryResolver.hpp, pure, std::expected)
                           ▼
                ResolvedAudioGeometry  ──► graph (declarations, ZTS period)
                                       ──► rate change (SetZeroTimeStampPeriod, re-declare)
                                       ──► configuration candidate validation
AudioGeometryReport ──► app "Audio Geometry" panel      AudioRuntimeTuning ──► app tuning sweep
```

The core idea is sound, and main lacks it: **one pure function turns (wire facts + device timing policy
+ HAL buffer policy) into one resolved value, and every consumer reads that value.** Most of what surrounds
the resolver is scaffolding for experiments.

## 2. Verdicts

| midi abstraction / method | Problem it solved | Still exists on main? | Fundamental or scaffolding | Facts vs policy | Verdict |
|---|---|---|---|---|---|
| `ResolveAudioGeometry` (`Audio/Shared/AudioGeometryResolver.hpp`) | Many consumers each derived geometry themselves | **Yes** (G-02, G-08…G-11, G-19) | fundamental | inputs: rate geometry (fact), device timing policy (policy), HAL profile (policy) | **KEEP MATH / REHOME**. Keep a pure `std::expected` resolver. Drop from it: `topologyRevision`, allocation limits, byte sizes, `pcmCacheCapacityFrames` and the tuning override. Those belong to memory ownership (FW-209) or are scaffolding. Output only timing/HAL quantities. |
| `ResolvedAudioGeometry` fields | One value for all consumers | yes | fundamental | output | **SIMPLIFY**. Keep rate, fdf and SYT interval (copied from the wire fact, not re-derived), ring frames, ZTS period, IO budget, the four declarations, and both transfer delays. Remove channel counts and byte sizes: they are wire/memory, not timing. |
| `DeriveHalTimingProjection` (`HalTimingProjection.hpp`) | Glue from a profile to the resolver, shared by the graph and the rate change | yes (the graph and rate change share nothing on main, G-22) | glue | — | **MERGE** into the resolver's caller: a single `ResolveTimingGeometry(profile, rate)` entry point. The separate `rawProfileOutputSafetyFrames` diagnostic is not needed. |
| `AmdtpTransferDelayTicks(rate, syt, mode)` (`Audio/Wire/AMDTP/AmdtpTransferDelay.hpp`) | Transfer delay was a 12800 literal in ≥ 3 places (G-04) | **Yes** | fundamental wire maths (Linux derivation) | fact derived from the rate geometry | **KEEP MATH** in `Audio/Wire/AMDTP/`. It gives 12800 at 48/96/192 kHz (matching main today), but **13162 at 44.1/88.2/176.4 kHz and 14848 at 32 kHz**, where main sends 12800. Adopting it is a wire change at the 44.1 family. **Decision required** (see §3). |
| `AudioHalBufferProfileForRate(rate)` + V3 values (12288 ring / 12288 ZTS / 1024 IO at 48 kHz) | Rate-dependent HAL geometry; ADK caps the client buffer at `min(ZTS·3/8, 4096)` | The rate-dependence is **yes** (G-05…G-07 are rate-independent frame counts). The values are **no**: main ships 1536/1536/512 and they are hardware-validated on DICE (`dice-working-1536`). | the shape is fundamental; the values are an experiment | policy | **KEEP SHAPE, NOT VALUES**. The function becomes `HalBufferProfileForRate(rate)` and returns main's current profile at every rate. Moving to midi's 12288/1024 changes HAL-visible geometry and anchor rate, so it needs an Epic 2 baseline before and after. Values are **DEFER → FW-221** (multi-rate) or a dedicated, measured change. |
| `AudioTimingGeometry::FrameRingFrames(rate)` / `ZeroTimestampPeriodFrames(rate)` / `IsV3SampleRate` | the same as above | yes | duplicates of the profile function | policy | **MERGE** into the one HAL-profile function. `IsV3SampleRate` becomes "rate has an `AmdtpRateGeometry` and a HAL profile". |
| `AudioGeometryPolicy::FramesPerPacket/RateAddend/Tx/RxSafetyOffsetFrames/ReportedLatencyFrames` | The Saffire-style ladder | yes (the ladder is copied into 6 DICE profiles plus Weiss; main's own copy of this header is dead, G-02, G-10) | the ladder is device policy, but the fpp term is a wire fact | fpp = fact (`sytIntervalFrames`); packets and addend = policy | **SIMPLIFY**. Take fpp from `AmdtpRateGeometry` rather than a second ladder. Keep a single `LadderSafetyFrames(delayPackets, rate)` helper. Ladder profiles call it instead of re-deriving. |
| `AudioGeometryPolicy::CompletionBatchFrames(rate)` | The input-safety floor must cover one completion batch at the running rate | **yes**: main's floor uses 48 kHz-only `kMaximumNominalFramesPerInterrupt = 40` (G-11, G-14) | fundamental | derived from cadence (fact) and group size (policy) | **KEEP MATH, adjusted**. midi counts all 6 packets as DATA (48 frames at 48 kHz). Main's floor uses the true worst-case DATA count, 5 × 8 = 40. Keep main's 40 at 48 kHz, derived instead as `maxDataPacketsInGroup × fpp(rate)` using midi's `DataPacketsInWindow` scan. This is identical at 48 kHz and correct at 96 kHz. |
| `AudioGeometryPolicy::RequiredOutputSafetyFrames(floor, rate)` | Output safety derived from the payload-finality freeze (`kTxContentFreezeCycleSlots`) | only as part of midi's finality engine, which main does not have | depends on midi TX finality | policy | **DEFER → FW-209** (TX ownership/finality). Main keeps the profile value. |
| Saffire empirical calibration (`SaffireReportedInput/OutputLatencyFrames` 53/52, `kSaffireRxDelayPackets = 10`) | Declared round trip +47 frames against the measured `RTL_ts` | **yes**: main declares 29/29 for Saffire (G-08) | evidence, device policy | measured | **DEFER → a measured change after FW-176**. It is a correction to the HAL-visible declaration, justified by midi RTL evidence. It must be re-measured on main with the Epic 2 tools, not imported by recall. |
| `RateTimingPolicy` table in `ResolvedAudioEndpointProfile` (per-rate data instead of virtuals) | Timing policy as data resolved once | partly: main uses virtuals (fine), but looks the profile up 3 times (G-19) | the data shape is a design choice | policy | **DO NOT PORT** (Epic 1 ruling). The goal (resolve once) is met by resolving timing geometry once per rate and caching it. `TimingFor()`'s silent fallback to `timing[0]` for an unknown rate is a defect to avoid: an unknown rate must be an error. |
| `CommonProfileBuilder` derived defaults (safety = one completion batch, transfer delay from the formula) | Profiles with unknown timing guessed literals (`16`, `64`) | yes (Generic DICE, BeBoB, Phase88 and MOTU declare 64/128 literals) | policy | — | **DEFER → per-device measured change**. The literals are hardware-tested behaviour. The resolver floors input safety (already done on main), so a small profile value is safe. |
| `AudioGeometryReport` + `DataPacketsInWindow` + app panel + `AudioTuningWireFormats` | The app displayed geometry it would otherwise re-derive | the app shows no geometry on main | diagnostics / UI | derived | **KEEP `DataPacketsInWindow` (pure maths)**. Treat the report and panel as **DEFER** (observability; it can be built from `ResolvedTimingGeometry` later). |
| `AudioRuntimeTuning` + `TuningGroup` (runtime override of declarations, depths, HAL geometry) | Sweeping values without rebuilding | no | **experimental scaffolding**, and a second runtime authority by construction | policy override | **DELETE / do not port**. It would violate the epic's hard rule (two authoritative derivation paths). If sweeping is needed again, do it with a build flag or an MCP dev tool that recompiles. |
| `SetZeroTimeStampPeriod` on rate change (in the perform window) | ZTS period changes with rate | not on main (the period is fixed at `init`) | fundamental for multi-rate | — | **DEFER → FW-221**. The model allows a rate-dependent period. Main keeps the same period at every rate until multi-rate validates a change. |
| candidate-geometry validation before a hardware apply (`PerformDeviceConfigurationChange`) | Reject impossible geometry before touching hardware | the rate change on main has no geometry check | fundamental | — | **KEEP IDEA**. The rate-change path resolves first and refuses if resolution fails, before it asks the nub to change the transport. |
| `HardwareSampleTimeline` rational conversions | ticks ↔ frames | yes (G-17) | fundamental | maths | **DEFER → FW-186**. It is already on main, and the timeline epic owns it. |

## 3. Decisions that change observable behaviour

Each of these needs your decision, the Epic 2 baseline, or both. None is taken implicitly by the port.

| # | Change | Where it changes behaviour | Proposed default for this epic |
|---|---|---|---|
| D1 | Transfer delay from `AmdtpTransferDelayTicks` instead of the 12800 literal | on-wire SYT at 32 kHz and the 44.1 kHz family (12800 → 14848 / 13162) | Make the formula the authority, **but** keep 12800 for the rates it changes until the multi-rate epic validates them on hardware. The exception is written down in one place and tested. |
| D2 | midi V3 HAL values (12288 / 12288 / 1024) | HAL ring, anchor rate, client buffer ceiling | Keep main's 1536 / 1536 / 512 at every rate. |
| D3 | Saffire declared latency 53/52 instead of 29/29 | DAW compensation | Out of scope. Needs an RTL baseline on main first (FW-176). |
| D4 | Re-publish declarations on a rate change | the HAL sees correct latency/safety after 48 → 96 kHz | **Fix** (it is a defect, G-22). At 48 kHz nothing changes. |
| D5 | Input-safety floor, made rate-general | the floor at 96/192 kHz | Identical at 48 kHz (still 128). At 96 kHz the floor becomes align32(80 + 64) = 160 instead of 128. Default: apply (a rate-general floor is the point of the epic); it only matters for devices running at 2×. |
