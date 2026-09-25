# Timing & HAL geometry — ownership model

Epic FW-177, ticket FW-180. This document turns two inputs into the target ownership model:
- the main inventory, [`TIMING_GEOMETRY_INVENTORY.md`](TIMING_GEOMETRY_INVENTORY.md) (rows `G-nn`);
- the midi distillation, [`TIMING_GEOMETRY_MIDI_DISTILLATION.md`](TIMING_GEOMETRY_MIDI_DISTILLATION.md)
  (decisions `D1`…`D5`).

It names **one final authority for every semantic quantity**. The implementation tickets (FW-181/182/183)
and the regression tests (FW-184) follow it. FW-185 checks the final tree against it.

## 0. Decisions taken (2026-09-24, after the first migration)

The first migration (FW-181/182/183) kept main's shipped values. The owner then
decided the open items of [`TIMING_GEOMETRY_MIDI_DISTILLATION.md`](TIMING_GEOMETRY_MIDI_DISTILLATION.md) §3:

| # | Decision | Where |
|---|---|---|
| D1 | Transfer delay is the Linux-derived blocking formula at every rate, as on midi: 12800 ticks at 48/96/192 kHz, 13162 in the 44.1 kHz family, 14848 at 32 kHz. The compatibility path that pinned 12800 is gone. | `AppliedTransferDelayTicks` |
| D2 | midi's V3 HAL geometry (12288 frames at 48 kHz, 24576 at 96 kHz, 1024 IO budget) is hardware-validated and is adopted, together with 8-packet completion groups. The 504-packet TX in-flight ring and the late-binding finality engine stay in FW-209: main's TX writes final content at prepare time. | `HalBufferProfileForRate`, §7 |
| D3 | The Saffire Pro 14/24/24 DSP calibration is hardware-validated and is adopted: latency 53 in / 52 out at 48 kHz (doubling per rate tier), capture safety 10 packets. The input-safety floor is one completion batch — no jitter term, no alignment — so calibrated values stand. The Pro 40 was not calibrated and keeps the vendor ladder. | `FocusriteSaffireProfile.cpp`, `ResolveInputSafetyFrames` |

§2–§4 and §6 are updated for D1–D3. §7 records how D2 was implemented, including
the one derived value that differs from the plan (the TX store).

## 1. The model

```
wire / device facts                                 device timing policy
─────────────────────                               ────────────────────
AmdtpRateGeometryForSampleRate(rate)                IAudioDeviceProfile (resolved ONCE per audio
  → fdf, sytIntervalFrames, framesPerCycle            driver instance) at a rate:
AmdtpTransferDelayTicks(rate, syt, mode)              Tx/RxReportedLatencyFrames, Tx/RxSafetyOffsetFrames
                                                      (ladder profiles call ONE ladder helper)
HAL buffer policy
─────────────────
HalBufferProfileForRate(rate)
  → ring, ZTS period, client IO budget
                     │                                       │
                     └──────────────┬────────────────────────┘
                                    ▼
          ResolveTimingGeometry(rate, DeviceTimingPolicy)       pure, std::expected, host-tested
          Audio/Runtime/ResolvedTimingGeometry.hpp
                                    │
                                    ▼
                  ResolvedTimingGeometry  (immutable value, one per rate)
            ┌──────────────┬──────────────────┼──────────────────┬───────────────────┐
            ▼              ▼                  ▼                  ▼                   ▼
    graph: init(ZTS),   rate change:      StartIO: ATCB       IO callback:       timeline / ZTS
    Set*Latency,        re-resolve BEFORE  rx/txTransferDelay  client IO budget   (FW-186 owns
    Set*SafetyOffset    touching transport,                                       semantics; reads
                        re-declare                                                 the period here)
```

### Rules

1. **Wire facts stay wire authority.** `sytIntervalFrames`, `fdf` and the cadence come from
   `AmdtpRateGeometryForSampleRate` only. Timing code copies them into the resolved value and never
   re-derives them from `rate > 48000` ladders (G-02).
2. **One resolution per rate.** The audio driver resolves the profile once, at graph construction
   (`ivars.profile`, G-19). It calls `ResolveTimingGeometry` once at graph construction and once per
   accepted rate change. Consumers read `ivars.timing`. They must not call `FindProfile`, profile timing
   virtuals, `AudioTimingGeometry` HAL constants or `TimingCursorPolicy` for these quantities.
3. **Resolve before acting.** A rate change resolves the new geometry first. If resolution fails (an
   unsupported rate, or a ring larger than the shared allocation), the change is refused before the
   nub reconfigures the transport. The commit, including the ZTS period, happens only inside the
   host's configuration-change window.
4. **Unknown is an error, not 48 kHz.** Resolution fails for a rate without `AmdtpRateGeometry`. No
   silent 48000 fallback remains inside the timing path (G-01). The fallbacks in nub parsing and profile
   `BuildConfig` defaults are wire/publication concerns. They are listed and dispositioned, but they
   are not removed here unless they feed timing.
5. **Depths are not timing geometry.** The TX runway/ownership depths (G-13) stay where they are and
   belong to FW-209. The interrupt group (G-14) stays a transport-cadence constant. Only its
   *frame* consequences become rate-general, through the completion-batch helper.
6. **Compatibility exceptions are explicit and tested.** A behaviour difference between the resolver's
   maths and today's wire behaviour (D1) lives in one named function with a test. It is never
   duplicated as a literal.

## 2. `ResolvedTimingGeometry` (provisional names)

| Field | Unit | Source | Notes |
|---|---|---|---|
| `sampleRateHz` | Hz | input | must have an `AmdtpRateGeometry` |
| `fdf`, `sytIntervalFrames` | —, frames | `AmdtpRateGeometryForSampleRate` | copied |
| `frameRingFrames` | frames | `HalBufferProfileForRate` | the ACTIVE ring: 12288 at 1x, 24576 at 2x (D2) |
| `allocatedFrameRingFrames` | frames | `kAllocatedFrameRingFrames` | 24576, the shared-memory size the active ring lives in |
| `zeroTimestampPeriodFrames` | frames | `HalBufferProfileForRate` | equal to the active ring (D2) |
| `clientIoBudgetFrames` | frames | `HalBufferProfileForRate` | 1024 (nominal; ADK lets clients pick up to 4096) |
| `outputLatencyFrames` / `inputLatencyFrames` | frames | profile | declared (A-terms) |
| `outputSafetyOffsetFrames` | frames | profile | declared |
| `profileInputSafetyFrames` | frames | profile | kept for the log line only |
| `inputSafetyOffsetFrames` | frames | `max(profile, CompletionBatchFrames(rate))` | 48 frames at 1x, no jitter or alignment (D3) |
| `rxTransferDelayTicks` / `txTransferDelayTicks` | 24.576 MHz ticks | `AppliedTransferDelayTicks` = blocking formula | 12800 at 48/96/192 kHz, 13162 at 44.1 family, 14848 at 32 kHz (D1) |

Errors: `UnsupportedSampleRate`, `InvalidHalProfile`, `InvalidSafetyOffset` (a zero safety offset, or a
safety offset ≥ the ring), `ExceedsAllocation` (the rate's ring is larger than the shared allocation:
176.4/192 kHz).

## 3. Final authority per quantity

The disposition is the **target**. FW-181/183 execute it, and FW-185 verifies it row by row.

| ID | Quantity | Final authority | Disposition of today's paths |
|---|---|---|---|
| G-01 | current sample rate | `ivars.device.currentSampleRate` (nub + rate change), passed to the resolver | timing-path 48000 fallbacks (`ASFWAudioDriverZts.cpp:944`) **DELETE** (the resolver rejects 0). Nub/profile/model defaults **KEEP**: publication, not timing (FW-221 revisits). |
| G-02 | frames per DATA packet | `AmdtpRateGeometryForSampleRate().sytIntervalFrames` | six DICE ladders and Weiss **REPLACE →** `TimingLadder` helper (fpp from the wire table); `AudioGeometryPolicy::FramesPerPacket/RateAddend` **DELETE**; `TimingCursorPolicy::FramesPerPacketMax` **DELETE**; `AudioTimingGeometry::kFramesPerDataPacket` **KEEP** as the 48 kHz cadence-structure constant used by static_asserts and MOTU; the ATCB use is dispositioned in FW-185 |
| G-03 | cadence | `AmdtpRateGeometry` + `AmdtpCadence` | **KEEP**; `kMinAvgCadence*` **KEEP** (budget constants) |
| G-04 | transfer delay | `ResolvedTimingGeometry::rx/txTransferDelayTicks` ← `TransferDelayTicksForWire` | `IAudioDeviceProfile::Rx/TxTransferDelayTicks` (no overrides) **DELETE**; the ATCB initializers stay as a reset value but are **ADAPTed** to take the constant from the wire header; `MAudioInternalTxTiming` 12800 **REPLACE →** `AmdtpTransferDelayTicks(48000, 8)`; `kTransferDelayTicks/Nanos = 0x2E00` in both `TimingUtils.hpp` **DELETE** |
| G-05 | HAL ring frames | `HalBufferProfileForRate().frameRingFrames` (active) inside `kAllocatedFrameRingFrames` | the compile-time selector (`kActiveAudioHalBufferProfile`, the 512/192/1536 profiles, `ASFW_AUDIO_HAL_BUFFER_PROFILE`) **DELETED**; `Isoch::Config::kAudioRingBufferFrames/kAudioOutputRingFrames` **KEEP** as allocation aliases; the endpoint runtime publishes the active ring per rate (`ActiveRingFramesForRate`), the graph checks it against `ivars.device.timing`, and the direct binding wraps on it (`UpdateDirectAudioGeometry` on a rate change) |
| G-06 | ZTS period | `ResolvedTimingGeometry::zeroTimestampPeriodFrames` | the graph `init` reads `ivars.timing`; the rate-change window calls `SetZeroTimeStampPeriod` when it differs; `HardwareSampleTimeline` (per epoch), `DirectAudioReceiveConsumer` and `MAudioTxClockBridge` read `HalBufferProfileForRate(rate)` -- the same value the resolver publishes. Threading the resolved value itself through the timeline stays with **FW-186** |
| G-07 | client IO budget | `ResolvedTimingGeometry::clientIoBudgetFrames` | identical at every rate (1024): the IO callback and Zts read `kHalIoPeriodFrames`. TX budgets use `kMaxClientIoFrames` (4096, the ADK ceiling), not the budget; `TimingCursorPolicy::HalIoPeriodFrames` **DELETE** |
| G-08/G-09 | declared latency | profile, via the resolver | graph `TimingCursorPolicy` fallback **DELETE** (unreachable: `FindProfile` never returns null); `AudioGeometryPolicy::ReportedLatencyFrames`, `kReportedDeviceLatencyFrames`, `RxBufferProfile::inputLatencyFrames` **DELETE**; ladder profiles **REPLACE →** `TimingLadder::ReportedLatencyFrames` |
| G-10 | declared output safety | profile, via the resolver | as G-08, plus `TxBufferProfile::safetyOffsetFrames` / `kReportedSafetyOffsetFrames` **DELETE** |
| G-11 | declared input safety | the resolver: profile floored at the rate-general batch | `RequiredInputSafetyFrames` **ADAPT** (it takes the batch frames for the rate); the graph's local `kSchedulingJitterFrames` **DELETE** (use `AudioTimingGeometry::kSchedulingJitterFrames`); `AudioGeometryPolicy.hpp` apart from the input-safety function **DELETE**; the `InputSafetyPolicy.hpp` shim **DELETE** |
| G-12 | per-stream latency | literal 0 | **KEEP** (intentional: all latency is on the device) |
| G-13 | TX depths | `AudioTimingGeometry` | **DEFER → FW-209**; the dead `TimingCursorPolicy` lead/deadline/deadband and `kOutputConsumerLeadFrames` / `kOutputCursorResyncDeadbandFrames` **DELETE** |
| G-14 | interrupt group | `AudioTimingGeometry::kTimingGroupPackets` (packets) | **KEEP**; `kMin/MaxNominalFramesPerInterrupt` are **REPLACEd** in the safety floor by `CompletionBatchFrames(rate)` (they stay only while tests use them) |
| G-15 | `TimingCursorPolicy` | — | **DELETE** the class, its log line and its tests (the StartIO log line is superseded by one `[Timing]` line printing `ResolvedTimingGeometry`) |
| G-16 | Tx/Rx buffer profiles, reported constants, queue capacities | — | **DELETE** |
| G-17 | timeline conversions | `HardwareSampleTimeline` | **DEFER → FW-186** (44.1 kHz `NominalBusTicksPerFrame` gap included); owned by [`HARDWARE_TIMELINE_OWNERSHIP.md`](HARDWARE_TIMELINE_OWNERSHIP.md), gap closed in its stage T5 |
| G-18 | host-time utilities | `Common/TimingUtils.hpp` | `Audio/Wire/AMDTP/TimingUtils.hpp` **DELETE**; its 2 includers are repointed |
| G-19 | profile resolution on the audio side | one `FindProfile` at graph construction → `ivars.profile` | the StartIO and direct-binding lookups **REPLACE →** `ivars.profile`; the unreachable null branch in StartIO **DELETE** |
| G-20 | supported rates | profile `SupportedSampleRates` ∩ `AmdtpRateGeometry` ∩ the DICE cap ∩ resolvable | the graph advertises only rates the resolver accepts (4x rates are dropped, as midi's `IsV3SampleRate` does); the other gates **DEFER → FW-221** |
| G-21 | DBS/channel ceilings | `Encoding::kMaxAmdtpDbs/kMaxPcmChannels` | the `Isoch::Config` copies **REPLACE →** aliases of the `Encoding` values (wire authority) |
| G-22 | rate change | `HandleChangeSampleRate` validates and requests the configuration-change window; `PerformDeviceConfigurationChange` commits: nub → `SetSampleRate` → `SetZeroTimeStampPeriod` → stream formats → re-declare → direct binding (midi's order). The device-initiated resync shares the path. | **ADAPT** |

## 4. Lifetime and what may change

| Value | Fixed at | May change on a rate change? |
|---|---|---|
| `ivars.profile` | graph construction (`FindProfile` once) | no: the device identity is fixed for the life of the audio driver instance |
| allocated ring | device creation (24576 frames, compile-time) | no. A rate whose ring exceeds it is refused. |
| active ring | graph; each accepted rate change | yes, inside the allocation: the endpoint runtime and the direct binding move together, no descriptor is replaced |
| ZTS period | `IOUserAudioDevice::init`; each accepted rate change | yes, only inside `PerformDeviceConfigurationChange` (`SetZeroTimeStampPeriod`) |
| declarations | graph; after each accepted rate change | yes |
| transfer delay | each StartIO (copied into the ATCB) | yes (read from `ivars.timing` at the next StartIO) |

## 5. Out of scope, with owners

- Timeline, ZTS projection and clock anchor semantics → **FW-186** (Epic 4).
- TX runway/ownership depths, payload-finality-derived output safety → **FW-209** (Epic 6).
- The remaining supported-rate gates (profile, DICE cap) and 4x rates (a larger allocation) →
  **FW-221** (multi-rate).

## 6. Behavioural acceptance (48 kHz)

With D1–D3 applied, these values are asserted by host tests (FW-184) and are to be confirmed on hardware
with the Epic 2 tools (`hal_geometry --json`, the `[Timing]` and `Reported HAL latency` log lines):
- ZTS period 12288; active ring 12288 in a 24576 allocation; IO budget 1024 (clients up to 4096);
- at 96 kHz: ZTS period and active ring 24576;
- transfer delay 12800 / 12800;
- each profile's declared latency and safety at 48 kHz (Saffire 53/52 and 80), and the input-safety
  floor of 48.

## 7. D2 implementation record (FW-183c)

- **Allocated vs active.** Shared memory is allocated once at `kAllocatedFrameRingFrames` (24576, the
  96 kHz ring). The HAL wraps the stream buffer on the ZTS period, so the active ring is a function of
  the rate and moves inside the allocation. `IOUserAudioStream` keeps the whole buffer.
- **ZTS consumers moved in the same commit** as the period change (midi's ac5c53c clicked when they
  lagged): `HardwareSampleTimeline` per epoch, `DirectAudioReceiveConsumer`, `MAudioTxClockBridge`.
- **TX store: 1696 packets, not the planned 1512.** V3 lets a client pick 4096 frames
  (`min(zts·3/8, 4096)`). One WriteEnd advances W by the whole IO before the producer runs, so the
  content horizon must hold one write: `TxDataHorizonFrames = max(400 cycles, 4096 + 64)` = 4160 frames
  at 1x. That raises the exposure lead to 760 packets, the frame window to 1504 and the preparation lead
  to 1648; the store is lead + one hardware ring = 1696. The plan's 1512 kept the 2400-frame horizon,
  which `tools/tx_data_horizon_burst_sim.py` shows losing the tail of every 4096-frame write (46,872
  frames in one second, then a descriptor fatal). The sim's own earlier `plan-io --frames 4096`
  derivation (FINDINGS.md F5: lead 1650, store 1704) and `buffer_geometry_verify.py solve` (1696)
  agree independently.
- **Evidence.** Burst sim: 64–4096-frame writes survive an 87.5 ms producer stall with at least 947
  packets of descriptor margin; payload-ownership sim push model 0 % dropout at 1648/1696. `asfw_sim`:
  the stall cliff moves 78 → 129 ms, and a 100 ms stall is now survived.
- **Cost.** On main's prepare-time TX the start-up prefill grows: the first DATA packet follows the
  prefilled store (~0.21 s at 48 kHz, was ~0.11 s). Late binding (FW-209) removes the gap.
- **4x rates** need a 49152-frame ring and are refused (`ExceedsAllocation`) and not advertised, as on
  midi.
