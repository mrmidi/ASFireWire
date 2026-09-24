# Timing & HAL geometry — ownership model

Epic FW-177, ticket FW-180. This document turns two inputs into the target ownership model:
- the main inventory, [`TIMING_GEOMETRY_INVENTORY.md`](TIMING_GEOMETRY_INVENTORY.md) (rows `G-nn`);
- the midi distillation, [`TIMING_GEOMETRY_MIDI_DISTILLATION.md`](TIMING_GEOMETRY_MIDI_DISTILLATION.md)
  (decisions `D1`…`D5`).

It names **one final authority for every semantic quantity**. The implementation tickets (FW-181/182/183)
and the regression tests (FW-184) follow it. FW-185 checks the final tree against it.

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
3. **Resolve before acting.** A rate change resolves the new geometry first. If resolution fails, or
   if it needs a ring or ZTS period that cannot change on a live device, the change is refused before
   the nub reconfigures the transport.
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
| `frameRingFrames` | frames | `HalBufferProfileForRate` | main's 1536 at every rate (D2) |
| `zeroTimestampPeriodFrames` | frames | `HalBufferProfileForRate` | main's 1536 at every rate (D2) |
| `clientIoBudgetFrames` | frames | `HalBufferProfileForRate` | main's 512 |
| `outputLatencyFrames` / `inputLatencyFrames` | frames | profile | declared (A-terms) |
| `outputSafetyOffsetFrames` | frames | profile | declared |
| `profileInputSafetyFrames` | frames | profile | kept for the log line only |
| `inputSafetyOffsetFrames` | frames | `max(profile, align32(completionBatchFrames(rate) + kSchedulingJitterFrames))` | identical at 48 kHz: align32(40 + 64) = 128 (D5) |
| `rxTransferDelayTicks` / `txTransferDelayTicks` | 24.576 MHz ticks | `TransferDelayTicksForWire(rate, syt, mode)` | formula, with the D1 compatibility exception: 12800 at the rates where the formula differs, until FW-221 |

Errors: `UnsupportedSampleRate`, `InvalidHalProfile`, `InvalidDeclaration` (a zero safety offset, or a
safety offset ≥ the ring).

## 3. Final authority per quantity

The disposition is the **target**. FW-181/183 execute it, and FW-185 verifies it row by row.

| ID | Quantity | Final authority | Disposition of today's paths |
|---|---|---|---|
| G-01 | current sample rate | `ivars.device.currentSampleRate` (nub + rate change), passed to the resolver | timing-path 48000 fallbacks (`ASFWAudioDriverZts.cpp:944`) **DELETE** (the resolver rejects 0). Nub/profile/model defaults **KEEP**: publication, not timing (FW-221 revisits). |
| G-02 | frames per DATA packet | `AmdtpRateGeometryForSampleRate().sytIntervalFrames` | six DICE ladders and Weiss **REPLACE →** `TimingLadder` helper (fpp from the wire table); `AudioGeometryPolicy::FramesPerPacket/RateAddend` **DELETE**; `TimingCursorPolicy::FramesPerPacketMax` **DELETE**; `AudioTimingGeometry::kFramesPerDataPacket` **KEEP** as the 48 kHz cadence-structure constant used by static_asserts and MOTU; the ATCB use is dispositioned in FW-185 |
| G-03 | cadence | `AmdtpRateGeometry` + `AmdtpCadence` | **KEEP**; `kMinAvgCadence*` **KEEP** (budget constants) |
| G-04 | transfer delay | `ResolvedTimingGeometry::rx/txTransferDelayTicks` ← `TransferDelayTicksForWire` | `IAudioDeviceProfile::Rx/TxTransferDelayTicks` (no overrides) **DELETE**; the ATCB initializers stay as a reset value but are **ADAPTed** to take the constant from the wire header; `MAudioInternalTxTiming` 12800 **REPLACE →** `AmdtpTransferDelayTicks(48000, 8)`; `kTransferDelayTicks/Nanos = 0x2E00` in both `TimingUtils.hpp` **DELETE** |
| G-05 | HAL ring frames | `HalBufferProfileForRate().frameRingFrames` | `kActiveAudioHalBufferProfile` → **ADAPT** (the rate function returns it); `Isoch::Config::kAudioRingBufferFrames/kAudioOutputRingFrames` **KEEP** as allocation aliases of the same constant (the shared-memory size is compile-time, D2) |
| G-06 | ZTS period | `ResolvedTimingGeometry::zeroTimestampPeriodFrames` | the graph `init` and self-check read `ivars.timing`; `TimingCursorPolicy::{Zts,HalZeroTimestamp}PeriodFrames` **DELETE**; `HardwareSampleTimeline::kZeroTimestampPeriodFrames`, `DirectAudioReceiveConsumer`, `MAudioTxClockBridge` **DEFER → FW-186** (they read the same constant; the timeline epic threads the resolved value) |
| G-07 | client IO budget | `ResolvedTimingGeometry::clientIoBudgetFrames` | the IO callback and Zts read the compile-time constant, which is identical at every rate: **COMPAT** until FW-221, with an agreement test; `Isoch::Config::kAudioIoPeriodFrames` **KEEP** (debug snapshot alias); `TimingCursorPolicy::HalIoPeriodFrames` **DELETE** |
| G-08/G-09 | declared latency | profile, via the resolver | graph `TimingCursorPolicy` fallback **DELETE** (unreachable: `FindProfile` never returns null); `AudioGeometryPolicy::ReportedLatencyFrames`, `kReportedDeviceLatencyFrames`, `RxBufferProfile::inputLatencyFrames` **DELETE**; ladder profiles **REPLACE →** `TimingLadder::ReportedLatencyFrames` |
| G-10 | declared output safety | profile, via the resolver | as G-08, plus `TxBufferProfile::safetyOffsetFrames` / `kReportedSafetyOffsetFrames` **DELETE** |
| G-11 | declared input safety | the resolver: profile floored at the rate-general batch | `RequiredInputSafetyFrames` **ADAPT** (it takes the batch frames for the rate); the graph's local `kSchedulingJitterFrames` **DELETE** (use `AudioTimingGeometry::kSchedulingJitterFrames`); `AudioGeometryPolicy.hpp` apart from the input-safety function **DELETE**; the `InputSafetyPolicy.hpp` shim **DELETE** |
| G-12 | per-stream latency | literal 0 | **KEEP** (intentional: all latency is on the device) |
| G-13 | TX depths | `AudioTimingGeometry` | **DEFER → FW-209**; the dead `TimingCursorPolicy` lead/deadline/deadband and `kOutputConsumerLeadFrames` / `kOutputCursorResyncDeadbandFrames` **DELETE** |
| G-14 | interrupt group | `AudioTimingGeometry::kTimingGroupPackets` (packets) | **KEEP**; `kMin/MaxNominalFramesPerInterrupt` are **REPLACEd** in the safety floor by `CompletionBatchFrames(rate)` (they stay only while tests use them) |
| G-15 | `TimingCursorPolicy` | — | **DELETE** the class, its log line and its tests (the StartIO log line is superseded by one `[Timing]` line printing `ResolvedTimingGeometry`) |
| G-16 | Tx/Rx buffer profiles, reported constants, queue capacities | — | **DELETE** |
| G-17 | timeline conversions | `HardwareSampleTimeline` | **DEFER → FW-186** (44.1 kHz `NominalBusTicksPerFrame` gap included) |
| G-18 | host-time utilities | `Common/TimingUtils.hpp` | `Audio/Wire/AMDTP/TimingUtils.hpp` **DELETE**; its 2 includers are repointed |
| G-19 | profile resolution on the audio side | one `FindProfile` at graph construction → `ivars.profile` | the StartIO and direct-binding lookups **REPLACE →** `ivars.profile`; the unreachable null branch in StartIO **DELETE** |
| G-20 | supported rates | profile `SupportedSampleRates` ∩ `AmdtpRateGeometry` ∩ the DICE cap | **DEFER → FW-221** (five gates remain; this epic adds only "the resolver rejects a rate without wire geometry") |
| G-21 | DBS/channel ceilings | `Encoding::kMaxAmdtpDbs/kMaxPcmChannels` | the `Isoch::Config` copies **REPLACE →** aliases of the `Encoding` values (wire authority) |
| G-22 | rate change | `HandleChangeSampleRate` resolves first, re-declares after `SetSampleRate` | **ADAPT** (fixes the stale declarations after 48 → 96 kHz) |

## 4. Lifetime and what may change

| Value | Fixed at | May change on a rate change? |
|---|---|---|
| `ivars.profile` | graph construction (`FindProfile` once) | no: the device identity is fixed for the life of the audio driver instance |
| ring frames | device creation (the shared-memory size is compile-time) | no. If the new rate would need a different ring, the rate change is refused. |
| ZTS period | `IOUserAudioDevice::init` | no, in this epic (FW-221 may add `SetZeroTimeStampPeriod` inside the perform window, as midi does). The resolver checks equality and refuses otherwise. |
| declarations | graph; after each accepted rate change | yes |
| transfer delay | each StartIO (copied into the ATCB) | yes (read from `ivars.timing` at the next StartIO) |

## 5. Out of scope, with owners

- Timeline, ZTS projection and clock anchor semantics → **FW-186** (Epic 4).
- TX runway/ownership depths, payload-finality-derived output safety → **FW-209** (Epic 6).
- Supported-rate advertisement, rate-dependent HAL values, the ZTS period change on a rate change,
  the 44.1 kHz transfer delay (D1) → **FW-221** (multi-rate).
- Measured declaration corrections (Saffire 53/52, D3) → a dedicated change after the **FW-176** baseline.

## 6. Behavioural acceptance (48 kHz)

At 48 kHz the migration must not change anything observable. These values are asserted by host tests
(FW-184) and confirmed on hardware with the Epic 2 tools (`hal_geometry --json`, the `Reported HAL latency`
log line):
- ZTS period 1536; ring 1536; IO budget 512;
- transfer delay 12800 / 12800;
- each profile's declared latency and safety at 48 kHz, and the input-safety floor of 128.
