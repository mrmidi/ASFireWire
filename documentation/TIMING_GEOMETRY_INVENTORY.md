# Timing & HAL geometry inventory — `main` (acceptance artifact)

Epic FW-177, ticket FW-178. This is the **initial** symbol-level trace of every producer, consumer, stored
copy, fallback and local re-derivation of timing/HAL geometry on `main`, taken at `a60239c`
(main `9417cc2` plus the Epic 2 series). The final-reconciliation ticket FW-185 must revisit
**every row** of this document; it is not throwaway analysis.

Vocabulary follows [`LATENCY_VOCABULARY.md`](LATENCY_VOCABULARY.md). Where this document says "depth",
it means a buffer or runway size, which is *not* a latency.

**How to read the tables**

- **ID**: a stable row identifier (`G-nn`). Keep IDs when updating a row; do not renumber.
- **Authority**: what currently decides the value that is actually used.
- **Dup / fallback**: other derivations that exist for the same quantity, reachable or not.
- **Disposition / Replacement / Validation**: left **open** in this pass on purpose. They are filled in
  by FW-180 (target owner) and FW-181/183 (the change), and verified by FW-185. Allowed values:
  `KEEP`, `ADAPT`, `REPLACE → x`, `DELETE`, `DEFER → epic`, `COMPAT → reason + test`.

---

## 1. Summary of findings

These are the structural problems the trace found. Details are in the rows referenced.

1. **The self-declared "single source of truth" is dead.** `Shared/Isoch/AudioGeometryPolicy.hpp`
   calls itself the SSOT for rate-dependent geometry. Yet no device profile, the graph, or the rate-change
   path calls `FramesPerPacket`, `Tx/RxSafetyOffsetFrames` or `ReportedLatencyFrames`. The only live
   symbol in it is `RequiredInputSafetyFrames` (G-11). The real authority for HAL declarations is the
   per-device `IAudioDeviceProfile` virtuals (G-08…G-11).
2. **The frames-per-packet ladder has ≥ 7 independent derivations** (G-02). There is one wire authority,
   `AmdtpRateGeometryForSampleRate` (`sytIntervalFrames`). The same `8/16/32` ladder is also hand-written
   in six DICE profiles, in `WeissIntProfile.cpp`, in the dead policy header, and as 48 kHz constants in
   `TimingCursorPolicy` and `AudioTimingGeometry`.
3. **Timing is re-resolved downstream of the nub.** The nub carries wire geometry only (streams, rates,
   mode). The audio side calls `AudioProfileRegistry::FindProfile(vendor, model, guid, builderId)`
   **four** separate times (twice in the graph, StartIO, direct binding) to recover timing policy (G-19). Nothing
   guarantees that the three lookups, or the publisher's own resolution, agree.
4. **HAL declarations are frozen at graph construction.** Latency and safety offsets are computed once,
   at the graph's `currentSampleRate`. `HandleChangeSampleRate` never re-publishes them (G-08…G-12), so
   after a rate change the HAL keeps the old rate's declarations. Profiles with rate ladders (Saffire,
   StudioLive, MultiMix, Venice, Weiss) therefore declare their 48 kHz values at 96 kHz.
5. **Transfer delay has four definitions and two values** (G-04). 12800 ticks is live, from the
   `IAudioDeviceProfile` default, the ATCB initializers, and `MAudioInternalTxTiming`. `kTransferDelayTicks =
   0x2E00` (11776) is defined twice (in two copies of `TimingUtils.hpp`) and used nowhere. No profile
   overrides the default, so the value is effectively a constant, not a derivation.
6. **Whole profile systems are dead.** `AudioTxProfiles.hpp` (A/B/C TX buffer profiles),
   `AudioRxProfiles.hpp` (A/B/C RX profiles) and the constants `kReportedDeviceLatencyFrames` /
   `kReportedSafetyOffsetFrames` have no live consumer (G-16). They advertise latency and safety values
   that are never applied.
7. **`TimingCursorPolicy` is mostly unused.** It is a live authority in exactly two places: the
   no-profile fallback for HAL declarations (with values 29/0 latency and 8/8 safety, which match no
   device profile), and the ZTS-period self-check in the graph. Its cursor-offset, lead, deadline and
   deadband methods have no production caller (G-15).
8. **Duplicated local constants.** The graph declares its own `kSchedulingJitterFrames = 64` next to
   `AudioTimingGeometry::kSchedulingJitterFrames = 64` (G-11). `kMaxAmdtpDbs` / `kMaxPcmChannels` exist in
   both `Isoch::Config` and `Encoding` (G-21). `TimingUtils.hpp` exists twice, token-identical apart from
   comments (G-18).
9. **Silent 48 kHz defaults** exist in ≥ 12 places where a missing rate should be an error (G-01).
10. **HAL ring, ZTS period and client IO budget are rate-independent frame counts** (G-05…G-07). At
    96 kHz the same 1536-frame ZTS period is half the wall time. Whether that is intended is undocumented.
    Multi-rate owns the answer, but this epic must make the quantity a function of rate, even if it
    returns the same number.

---

## 2. Wire / device facts that feed timing

These are inputs to timing policy. The ownership rule (FW-180) keeps them as wire authority.

| ID | Symbol / method | File | Semantic value | P/C | Callers / consumers | Stored copies | Current authority | Dup / fallback |
|---|---|---|---|---|---|---|---|---|
| G-01 | `ParsedAudioDriverConfig::currentSampleRate`, `kDefaultSampleRate` | `Audio/DriverKit/Config/AudioDriverConfig.hpp:71,14` | current sample rate | P (nub parse) | graph (latency/safety), StartIO, Zts `ArmPrimaryTxProducer`, direct binding | `ivars.device.currentSampleRate`; `AudioEndpointRuntime::config_.currentSampleRate`; `AudioGraphBinding::sampleRateHz`; `ASFWAudioNub::currentSampleRateHz` | nub property, then `HandleChangeSampleRate` writes `ivars.device.currentSampleRate` (`ASFWAudioDevice.cpp:784`) | **silent 48000 fallbacks**: `ASFWAudioNub.cpp:127,174,246`; `AudioEndpointRuntime.hpp:480`; `ASFWAudioDriverZts.cpp:944`; `AudioStreamProfile.hpp:23`; `Model/ASFWAudioDevice.hpp:59`; `AmdtpTypes.hpp:37`; `BlockingCadence()` ctor `AmdtpCadence.hpp:88`; every profile `BuildConfig` `sampleRate = 48000` (Generic DICE, Saffire, StudioLive ×2, MultiMix, Venice, Weiss, Duet, MOTU, Phase88, BeBoB, M-Audio) |
| G-02 | `AmdtpRateGeometryForSampleRate().sytIntervalFrames` | `Audio/Wire/AMDTP/AmdtpRateGeometry.hpp:57` | frames per DATA packet (SYT interval) | P | `AmdtpTxPacketizer`, RX decode | `AmdtpStreamConfig::framesPerDataPacket` | wire table (IEC 61883-6 SFC) | `AudioGeometryPolicy::FramesPerPacket` (dead); `AudioTimingGeometry::kFramesPerDataPacket = 8` (used by `TimingCursorPolicy`, `MotuV2Profile.cpp`, ATCB); `TimingCursorPolicy::FramesPerPacketMax()` (constant 8); local ladders in `FocusriteSaffireProfile.cpp:116,138`, `PreSonusStudioLiveProfile.cpp:71,84`, `PreSonusStudioLive2442Profile.cpp:127,140`, `AlesisMultiMixProfile.cpp:118,131`, `MidasVeniceProfile.cpp:114,127`, `WeissIntProfile.cpp:28` |
| G-03 | `AmdtpRateGeometry::nominalFramesPerCycle`, `BlockingCadence`, `RationalBlockingCadence` | `AmdtpRateGeometry.hpp`, `AmdtpCadence.{hpp,cpp}` | cadence (frames per cycle, D/N pattern) | P | packetizer, RX SYT cadence | — | wire | `AudioTimingGeometry::kCadenceBlockPackets/Frames`, `kMinAvgCadencePackets/Frames` (48 kHz and 44.1 kHz budget constants; used only in static_asserts and tests) |
| G-20 | supported-rate sets | `HardwareSampleTimeline::IsSupportedSampleRate` {44.1, 48, 96, 192}; `AmdtpRateGeometryForSampleRate` {32…192}; `IAudioDeviceProfile::SupportedSampleRates` (default {48000}); `kDiceMaxSupportedRateHz = 48000` (`DICETypes.hpp:515`); packetizer non-blocking = 48 kHz only (`AmdtpTxPacketizer.cpp:54`); M-Audio internal TX = 48 kHz only (`ASFWAudioDriverZts.cpp:884`) | which rates may run | P | HAL advertisement, timeline epochs | — | profile `SupportedSampleRates` + DICE cap | five independent rate gates; `HardwareSampleTimeline::NominalBusTicksPerFrame` returns 0 for 44.1 kHz although `IsSupportedSampleRate(44100)` is true |
| G-21 | `kMaxAmdtpDbs`, `kMaxPcmChannels` | `Audio/Config/AudioConstants.hpp:11,15`; `Audio/Wire/AMDTP/AmdtpRateGeometry.hpp:9,13` | DBS / channel ceilings | P | channel clamps | — | both (identical values) | exact duplicate in two namespaces |

## 3. Timing / HAL geometry

| ID | Symbol / method | File | Semantic value | P/C | Callers / consumers | Stored copies | Current authority | Dup / fallback |
|---|---|---|---|---|---|---|---|---|
| G-04 | `IAudioDeviceProfile::Rx/TxTransferDelayTicks` (default 12800, **no overrides**) | `Audio/DriverKit/Config/IAudioDeviceProfile.hpp:88,94` | IEC 61883-6 presentation (transfer) delay, ticks | P | `ArmPrimaryTxProducer` stores into ATCB (`ASFWAudioDriverZts.cpp:965-970`) | ATCB `rx/txTransferDelayTicks{12800}` (initializers, `AudioTransportControlBlock.hpp:658`); read by `DirectAudioReceiveConsumer.cpp:368`, `ASFWAudioDriverZts.cpp:613-664`, telemetry log `ASFWAudioDevice.cpp:319` | profile default | `MAudioInternalTxTiming::kInternalTxTransferDelayTicks = 12'800` (`MAudioInternalTxTiming.cpp:11`, used by `MAudioTxClockBridge::Arm`); **dead** `kTransferDelayTicks = 0x2E00` in `Common/TimingUtils.hpp:49` **and** `Audio/Wire/AMDTP/TimingUtils.hpp:47` (+ `kTransferDelayNanos`) — different value, no users. No derivation from rate/SYT interval: the 12800 is Linux's 48 kHz blocking result, hard-coded |
| G-05 | `AudioHalBufferProfile::frameRingFrames` → `AudioTimingGeometry::kFrameRingFrames` (1536, profile 2) | `Shared/Isoch/AudioHalBufferProfiles.hpp`, `AudioTimingGeometry.hpp` | HAL stream ring capacity (frames) | P | `AudioEndpointRuntime` buffer allocation (`AudioEndpointRuntime.hpp:481`) | `Isoch::Config::kAudioRingBufferFrames`, `kAudioOutputRingFrames` (`AudioConstants.hpp:26,32`); `directOutput/InputCapacityFrames_`; re-derived from mapping length by `FrameCapacityFromSegment` (`ASFWAudioDriverDirect.cpp:122`) | compile-time `-DASFW_AUDIO_HAL_BUFFER_PROFILE` (default 2) | profiles 0/1 are compile-time alternatives; rate-independent |
| G-06 | `AudioHalBufferProfile::zeroTimestampPeriodFrames` → `kHalZeroTimestampPeriodFrames` (1536) | same | ZTS period (frames) | P | `IOUserAudioDevice::init(target_period)` (`ASFWAudioDriverGraph.cpp:303,318`); graph self-check `GetZeroTimestampPeriod()` vs `TimingCursorPolicy::HalZeroTimestampPeriodFrames()` (`:787`); `HardwareSampleTimeline::kZeroTimestampPeriodFrames`; `DirectAudioReceiveConsumer.cpp:410`; `MAudioTxClockBridge.cpp:19`; `ArmPrimaryTxProducer` (`Zts.cpp:959`) | `TimingCursorPolicy::ZtsPeriodFrames()` / `HalZeroTimestampPeriodFrames()` (pass-through); `TimingCursorPolicySnapshot::ztsPeriodFrames` (log only) | compile-time profile | rate-independent frame count (a period of 32 ms @48k is 16 ms @96k); **FW-186 owns ZTS semantics**, this epic owns the period as geometry |
| G-07 | `AudioHalBufferProfile::clientIoBudgetFrames` → `kHalIoPeriodFrames` (512) | same | maximum client IO span the ring is sized for | P | IO callback span validation (`ASFWAudioDriverIO.cpp:187`); Zts (`:1213`); `ASFWAudioDevice.cpp:603`; `kTxFrameExposureWindowPackets` budget; `AudioGeometryPolicy` exposure check | `Isoch::Config::kAudioIoPeriodFrames` (used by forced debug snapshot, `ASFWAudioDriverDirect.cpp:30`); `TimingCursorPolicy::HalIoPeriodFrames()` | compile-time profile | **not published to HAL** — no `SetIOBufferFrameSize`/range call; the HAL's actual maximum buffer is ADK-derived. Unverified whether HAL can request > 512 (IO callback validates "against stream-ring capacity") |
| G-08 | `IAudioDeviceProfile::TxReportedLatencyFrames(rate)` | `IAudioDeviceProfile.hpp:76` + 15 overrides | declared output device latency (A-term, LATENCY_VOCABULARY) | P | graph → `SetOutputLatency` (`ASFWAudioDriverGraph.cpp:729,763`) | none (published once) | profile, at graph-time rate | fallback `TimingCursorPolicy::ReportedLatencyFrames(Output)` = 29 when `FindProfile` is null (`:734`); dead: `AudioGeometryPolicy::ReportedLatencyFrames`, `kReportedDeviceLatencyFrames = 24`, `RxBufferProfile::inputLatencyFrames`. **Not re-published on rate change.** Values: Saffire/StudioLive/StudioLive2442/MultiMix/Venice/Weiss ladder 29/59/119; Generic DICE, BeBoB, Phase88, Duet, MOTU 128; Onyx 400F/820i 256; M-Audio 1814 112 / ProjectMix 104 |
| G-09 | `IAudioDeviceProfile::RxReportedLatencyFrames(rate)` | same | declared input device latency | P | graph → `SetInputLatency` | — | profile | fallback 0 (`TimingCursorPolicy`); M-Audio 113 / 104; others as G-08 |
| G-10 | `IAudioDeviceProfile::TxSafetyOffsetFrames(rate)` | same | declared output safety offset | P | graph → `SetOutputSafetyOffset` | — | profile | fallback 8 (`TimingCursorPolicy::SafetyOffsetFrames`); dead: `AudioGeometryPolicy::TxSafetyOffsetFrames`, `TxBufferProfile::safetyOffsetFrames` (A 64 / **B 96 active** / C 32) via dead `kReportedSafetyOffsetFrames`. Values: ladder profiles (6+addend)×fpp = 48/128/320; Generic DICE/BeBoB/Phase88/Duet/MOTU 64; Onyx 192; M-Audio 48 |
| G-11 | `IAudioDeviceProfile::RxSafetyOffsetFrames(rate)` → `RequiredInputSafetyFrames` floor | `IAudioDeviceProfile.hpp:72`; `AudioGeometryPolicy.hpp` (namespace `ASFW::Audio`) via shim `Audio/Config/InputSafetyPolicy.hpp` | declared input safety offset | P | graph → `SetInputSafetyOffset` after floor (`ASFWAudioDriverGraph.cpp:744-759`) | — | profile, floored at `align32(max(profile, kMaximumNominalFramesPerInterrupt + jitter))` = ≥ 128 @48k | **local `constexpr kSchedulingJitterFrames = 64`** in the graph duplicates `AudioTimingGeometry::kSchedulingJitterFrames`; `kMaximumNominalFramesPerInterrupt = 40` is a 48 kHz-only frame count (6 packets × 8 fpp minus N), wrong at 96 kHz |
| G-12 | `IOUserAudioStream::SetLatency(0)` | `ASFWAudioDriverGraph.cpp:594-600` | per-stream latency | P | HAL | — | literal 0 | — |
| G-13 | TX runway / ownership depths: `kTxHardwareRingPackets` (48), `kTxPreparationLeadPackets`, `kTxCoverageLeadPackets`, `kTxPreparationSlackPackets`, `kTxSharedSlotPackets` (912), `kTimelineSlots` (1024), `kTxExposureLead*`, `kTxFrameExposureWindow*`, `TxDataHorizonFrames(rate)` | `AudioTimingGeometry.hpp:~90-190` | **depths, not latency** | P | `ASFWAudioDriverZts.cpp` (prep target, telemetry), `AudioEndpointRuntime.hpp:323`, `ASFWAudioDevice.cpp:255,338,436` (queue validation), `ASFWAudioDriverIO.cpp:205`, `DiceTxStreamEngine.hpp:115` | ATCB telemetry buckets | compile-time | `TimingCursorPolicy::PacketLeadFrames/StartupLeadFrames/PreparationDeadlineFrames` (384, unused), `Isoch::Config::kOutputConsumerLeadFrames` (384, only referenced by a comment and a static_assert); ownership semantics belong to FW-209 |
| G-14 | interrupt group: `kTimingGroupPackets`/`kRx/TxPacketsPerGroup` (6), `kMin/MaxNominalFramesPerInterrupt` (32/40), `kNominalFramesPerTimingGroup` (36), `kRxDescriptorPackets` (504) | `AudioTimingGeometry.hpp` | completion cadence (packets); frames per completion (**48 kHz only**) | P | queue validation `ASFWAudioDevice.cpp:259,342,439`; input-safety floor (G-11) | — | compile-time | frame counts are only correct at 8 fpp |
| G-15 | `TimingCursorPolicy` (whole class) | `Audio/Config/TimingCursorPolicy.hpp` | "DICE 1x" cursor/latency/safety bundle | P | **live**: graph fallback (G-08…G-11), ZTS self-check (G-06); **log only**: StartIO snapshot (`ASFWAudioDevice.cpp:523`); **no caller**: `CursorOffsetFrames`, `Hardware/Host*Frame*` conversions, `PacketLeadFrames`, `StartupLeadFrames`, `PreparationDeadlineFrames`, `CursorResyncDeadbandFrames`, `RxAuthorityUpdatePeriodFrames` | `TimingCursorPolicySnapshot` | — | hard-coded 48/128/384/64 duplicates of other constants; tests `tests/audio/TimingCursorPolicyTests.cpp` |
| G-16 | `AudioTxProfiles.hpp` (`TxBufferProfile` A/B/C, `gActiveTxProfile`, `SetActiveTxProfile`), `AudioRxProfiles.hpp` (`RxBufferProfile` A/B/C), `kReportedDeviceLatencyFrames`, `kReportedSafetyOffsetFrames`, `kTxQueueCapacityFrames`, `kRxQueueCapacityFrames`, `kTransferChunkFrames` | `Audio/Config/*`, `ASFWAudioDriverPrivate.hpp:35-37` | historical tuning profiles | — | **none** (only the umbrella include `AudioConfig.hpp` and each other) | — | — | dead; advertise safety/latency values that are never applied |
| G-17 | `HardwareSampleTimeline::BusTicksToAudioFrames`, `AudioFramesToBusTicks`, `NominalBusTicksPerFrame`, `kZeroTimestampPeriodFrames` | `Audio/Runtime/HardwareSampleTimeline.hpp:77-122` | ticks ↔ frames conversion (rational) | P | ZTS / timeline | — | pure maths (rational, rate-general) | **CLOSED (FW-186, T5)**: 0 in the 44.1 kHz family is by design (no integral value; the conversions are exact rational); 32 kHz added (768) |
| G-18 | `ASFW::Timing` utilities (`gHostTimebaseInfo`, `hostTicksToNanos`, `nanosToHostTicks`, FW time maths) | `Common/TimingUtils.hpp` (20 includers) **and** `Audio/Wire/AMDTP/TimingUtils.hpp` (2 includers) | host-time conversion | P | many | — | either (same namespace; including both in one TU is a redefinition error) | whole-file duplicate |
| G-19 | `AudioProfileRegistry::FindProfile(vendor, model, guid, builderId)` on the audio side | `ASFWAudioDriverGraph.cpp:155` (name/channels/rates) **and** `:719` (declarations), `ASFWAudioDevice.cpp:164` (StartIO), `ASFWAudioDriverDirect.cpp:131` — **four** lookups (the first pass listed three; corrected during FW-183) | **re-resolution** of timing policy (and wire format) downstream of the nub | C | graph declarations, StartIO TX clock domain + transfer delay, direct binding wire format | — | three independent lookups per start | the publisher already resolved the device; nothing checks the lookups agree. StartIO fails hard if null, the graph silently falls back to `TimingCursorPolicy` |

## 4. Rate-change path

| ID | Path | Finding |
|---|---|---|
| G-22 | `ASFWAudioDevice::HandleChangeSampleRate` (`ASFWAudioDevice.cpp:~770-840`) → `nub->RequestSampleRateChange` → `SetSampleRate` | Updates `ivars.device.currentSampleRate` and stream formats. It does **not** recompute or re-publish latency, safety offset, ZTS period or IO budget (G-05…G-11). Transfer delay is re-read at the next StartIO (`ArmPrimaryTxProducer`), so the declarations and the runtime disagree after a rate change. |

## 5. Scope notes

- **Bus-level timing** (`Bus/Timing/*`: post-reset timing, iso allocation gate) and the generic OHCI
  descriptor geometry (`Isoch/Core/IsochDmaGeometry.hpp`, `Isoch/Receive/IsochRxTiming.hpp`) are transport,
  not audio timing geometry. They are listed so FW-185 knows they were considered and excluded.
- **ZTS, clock anchor, device timeline** (`HostClockAnchor.hpp`, `DeviceTimeline.hpp`, `ICycleTimeline.hpp`,
  the ZTS logic in `ASFWAudioDriverZts.cpp`) are traced by FW-187 (Epic 4). This epic owns only the
  geometry *inputs* they consume (G-06, G-04, G-17).
- **Family-specific wire timing** (`Audio/Wire/MOTU/*Timing.hpp`, `MAudioInternalTxTiming`,
  `RxSytCadence.hpp`, `AmdtpTiming.hpp`) is wire mechanism. Only their transfer-delay and rate
  inputs are in scope (G-04, G-20).

## 6. Search log (re-run for FW-185)

The trace used these searches. FW-185 must re-run them against the final tree and account for every hit:

```sh
rg -n 'SafetyOffsetFrames|ReportedLatencyFrames|TransferDelayTicks|FramesPerPacket\(' ASFWDriver
rg -nw 'kFrameRingFrames|kHalZeroTimestampPeriodFrames|kHalIoPeriodFrames|kAudioRingBufferFrames|kAudioIoPeriodFrames' ASFWDriver
rg -n '48000|48.000' ASFWDriver --glob '*.{cpp,hpp}'
rg -n '12800|12.800|0x2E00|11776' ASFWDriver
rg -n 'FindProfile\(' ASFWDriver/Audio/DriverKit
rg -n 'TimingCursorPolicy|AudioGeometryPolicy|TxBufferProfile|RxBufferProfile' ASFWDriver tests
rg -n 'SetOutputLatency|SetInputLatency|SetOutputSafetyOffset|SetInputSafetyOffset|SetLatency\(|GetZeroTimestampPeriod' ASFWDriver
```

---

## 7. Final state (FW-185 reconciliation)

Reconciled against the tree at the end of Epic 3: FW-181/182/183, then D1/D3 (FW-182b), eight-packet
groups (FW-183b), V3 geometry (FW-183c) and the tree guard (FW-184). Decisions D1–D3 are in
[`TIMING_GEOMETRY_OWNERSHIP.md`](TIMING_GEOMETRY_OWNERSHIP.md) §0. The implementation record for V3 is in
§7 of that document.

Every row now has a single authority. The retired paths are held out by `tools/timing_geometry_guard.py`
(ctest `TimingGeometryGuard`, plus `TimingGeometryGuardPyTests` for the scanner itself).

### Final graph

```
AmdtpRateGeometryForSampleRate(rate) ──┐          IAudioDeviceProfile (ivars.device.profile,
  fdf, sytIntervalFrames, cadence      │            resolved ONCE in BuildAudioGraph)
AmdtpTransferDelayTicks (D1)  ─────────┤            latency / safety via TimingLadder
HalBufferProfileForRate(rate) (V3) ────┤                       │
  ring == ZTS: 12288 @1x, 24576 @2x    │                       │
  kAllocatedFrameRingFrames = 24576    │                       │
                                       ▼                       ▼
                  ResolveTimingGeometry → ResolvedTimingGeometry (ivars.device.timing)
                                       │
   ┌───────────────┬───────────────────┼─────────────────────┬──────────────────────┐
   ▼               ▼                   ▼                     ▼                      ▼
 graph:          advertised         rate change            StartIO:               direct binding:
 init(ZTS),      rates: only        (config-change         ATCB rx/tx             active ring
 Set*Latency,    resolvable ones    window): nub →         transfer delay         (UpdateDirect-
 Set*Safety,                        SetSampleRate →                               AudioGeometry)
 ring check                         SetZeroTimeStampPeriod
                                    → formats → re-declare
Endpoint runtime (nub side): allocates 24576 once, publishes ActiveRingFramesForRate(rate).
Timeline / RX consumer / M-Audio bridge: HalBufferProfileForRate(rate) per epoch (FW-186 threads
the resolved value itself).
```

### Row dispositions

| ID | Disposition | Replacement / final authority | Validation |
|---|---|---|---|
| G-01 | ADAPT | `ivars.device.currentSampleRate`; the resolver rejects 0 and unknown rates. The remaining 48 kHz defaults are publication/device defaults (nub parse, profile `BuildConfig`, DICE/MOTU/AV/C backends). There are three on the timing path, none of them silent. The endpoint allocation default is validated by the resolver. `ASFWAudioDevice.cpp:318` only feeds a log line. M-Audio internal TX (`ASFWAudioDriverZts.cpp:884`) is validated at 48 kHz only by design. | `TimingGeometryTests.UnknownRateIsAnErrorNotA48kFallback` |
| G-02 | REPLACE → wire table + `TimingLadder` | `sytIntervalFrames` is copied into the resolved value; the profile ladders use `TimingLadder::FramesPerPacket` | `TimingGeometryTests.WireFactsAreCopiedNotRederived`, `LadderHelperReproducesLegacyProfileLadders` |
| G-03 | KEEP | `AmdtpRateGeometry` / `AmdtpCadence`; `kMinAvgCadence*` are budget constants | `MaxDataPacketsBoundMatchesTheProductionCadence` |
| G-04 | REPLACE → `AppliedTransferDelayTicks` (D1: blocking formula at every rate) | profile virtuals deleted; ATCB reset values read `Encoding::kAmdtpReferenceBlockingTransferDelayTicks`; StartIO copies `ivars.device.timing`; the 0x2E00 constants are deleted | `TransferDelayIsTheBlockingFormulaAtEveryRate`, `ProfileTimingPinTests`, guard |
| G-05 | REPLACE → `HalBufferProfileForRate(rate).frameRingFrames` (active) in `kAllocatedFrameRingFrames` (V3) | the compile-time selector is deleted; `Isoch::Config::kAudioRingBufferFrames` is the allocation alias; endpoint runtime `ActiveRingFramesForRate`; graph ring check; binding `UpdateDirectAudioGeometry` | `V3HalBufferProfilePerRateTier`, `ResolvedHalGeometryFollowsTheRateTier`, `AudioEndpointRuntime.RateChangeMovesActiveRingAndReusesMemory`, guard |
| G-06 | ADAPT | `ivars.device.timing.zeroTimestampPeriodFrames`: `init`, then `SetZeroTimeStampPeriod` in the rate-change window; the timeline, RX consumer and M-Audio bridge read `HalBufferProfileForRate(rate)` | `HardwareSampleTimelineTests.EpochTakesTheZtsPeriodOfItsRate`, `MAudioTxClockBridgeTests`, `RateChangeMovesActiveRingInsideTheAllocation` |
| G-07 | ADAPT | `clientIoBudgetFrames` = 1024 (nominal); `kMaxClientIoFrames` = 4096 (the ADK ceiling) sizes the TX budgets and the Zts exposure step classifier; `kHalIoPeriodFrames` remains for logs | `SaffireGeometryIsUnified`, `tx_data_horizon_burst_sim.py suite` |
| G-08/G-09 | REPLACE → profile via resolver | `TimingCursorPolicy` fallback and dead policies deleted; declared at graph time and on every rate change | `ProfileTimingPinTests` (every profile × 7 rates), guard |
| G-10 | REPLACE → profile via resolver | as G-08 | as G-08 |
| G-11 | REPLACE → `ResolveInputSafetyFrames` = max(profile, `CompletionBatchFrames(rate)`) (D3) | `RequiredInputSafetyFrames`, `InputSafetyPolicy.hpp` and the graph-local jitter constant are deleted | `InputSafetyIsTheProfileValueFlooredAtOneBatch`, `RxDrivenTimingTests.InputSafetyIsVisibilityMarginNotClientWindow` |
| G-12 | KEEP | literal 0 per stream | — |
| G-13 | ADAPT (depths stay in `AudioTimingGeometry`; late binding → FW-209) | horizon floor `max(400 cycles, 4096 + 64)`; lead 1648; store = timeline = 1696 | `AmdtpRateGeometryTests`, `TxRefillCoverageTests`, `AudioDriverTxProducerTests`, `asfw_sim` constants gate |
| G-14 | ADAPT | eight-packet groups on IR and IT (FW-183b); the fixed-phase frame constants are deleted; `CompletionBatchFrames(rate)` | `RxDrivenTimingTests.GeometryUsesEightCycleInterruptsAndCurrentTxDepths`, cross-layer static_assert |
| G-15 | DELETE | the one `[Timing]` line prints `ResolvedTimingGeometry` | guard |
| G-16 | DELETE | — | guard |
| G-17 | CLOSED (FW-186, T5) | `HardwareSampleTimeline`: every HAL-ladder rate; nominal ticks only where integral | `EpochTakesTheZtsPeriodOfItsRate` (all seven rates), `NominalTicksOnlyWhereIntegral`, `ZtsCharacterization.Clean32k` |
| G-18 | DELETE (the duplicate) | `Common/TimingUtils.hpp` | build |
| G-19 | REPLACE → one `FindProfile` in `BuildAudioGraph` → `ivars.device.profile` | StartIO and the direct binding read `ivars.device.profile` | guard (`second-profile-lookup`) |
| G-20 | ADAPT (partly) | the graph advertises only resolvable rates (4x dropped: `ExceedsAllocation`); the profile/DICE gates → FW-221 | `ProfileTimingPinTests` (4x → `ExceedsAllocation`) |
| G-21 | REPLACE → aliases of the `Encoding` values | — | build |
| G-22 | ADAPT | `HandleChangeSampleRate` validates and requests the window; `PerformDeviceConfigurationChange` commits in midi's order; external resync shares it | macOS CI build; hardware 48 → 96 → 48 check (open) |
| G-23 (new) | DELETE | `tools/calc_buffer_sizes.py` parsed the deleted `AudioTxProfiles.hpp` (G-16) and no longer ran | — |
| G-24 (new) | ADD | V3: `kAllocatedFrameRingFrames`, `ProfileFitsAllocation`, `AdkMaxClientIoFrames`, `kMaxClientIoFrames`, `kTxExposureFloorFrames`, `TimingGeometryError::kExceedsAllocation`, `pendingSampleRateHz` | the static_asserts in `AudioHalBufferProfiles.hpp` / `AudioTimingGeometry.hpp` |

### Search log, re-run (§6)

| Search | Final hits | Accounted |
|---|---|---|
| `SafetyOffsetFrames\|ReportedLatencyFrames\|TransferDelayTicks\|FramesPerPacket(` | profile virtuals and overrides (policy input, G-08…G-11); `TimingLadder`; the resolver; `ProfileTimingGeometry`; ATCB fields and their StartIO store / Zts reads; `MAudioInternalTxTiming` (formula-pinned); declarations in the graph and rate change from `ivars.device.timing` | all |
| `kFrameRingFrames\|kHalZeroTimestampPeriodFrames\|kHalIoPeriodFrames\|kAudioRingBufferFrames\|kAudioIoPeriodFrames` | the first two: none. `kHalIoPeriodFrames`: its definition plus two log/diagnostic uses. `kAudioRingBufferFrames`: allocation alias (endpoint runtime). `kAudioIoPeriodFrames`: the debug snapshot | all |
| `48000\|48.000` | 119 lines: wire tables, device/protocol defaults, the reference rate of static_asserts, and the three timing-path uses in G-01 | G-01 |
| `12800\|12.800\|0x2E00\|11776` | the wire formula and its static_asserts, the M-Audio static_assert, comments, and the unrelated `kSpeedToMbps` (S1600 = 12800 Mbit) | all |
| `FindProfile(` | registry declaration/definition, and the one graph call | G-19 |
| `TimingCursorPolicy\|AudioGeometryPolicy\|TxBufferProfile\|RxBufferProfile` | one historical comment | G-15/G-16 |
| `Set*Latency\|Set*SafetyOffset\|SetLatency(\|GetZeroTimestampPeriod` (+ `SetZeroTimeStampPeriod`) | the graph (from `ivars.device.timing`), the rate-change commit, the Zts log/self-check | G-06, G-08…G-12, G-22 |

### Reverse audit: who reads the resolved values

- `ivars.device.profile`:
  - written once in `BuildAudioGraph`;
  - read by StartIO (`ASFWAudioDevice.cpp:174`: stream profile), the direct binding (wire format), and the rate-change validation.
- `ivars.device.timing`:
  - written by the graph and by `CommitSampleRate`;
  - read by the graph (init, declarations, ring check), `ArmPrimaryTxProducer` (transfer delays into the ATCB), the direct binding (active ring and rate), and the rate-change no-op check.

No other audio-side code derives these quantities; the guard enforces it.

### Open after this epic

- **Hardware checks (user):**
  - 48 kHz at client buffers 64/512/1024/4096
  - a 48 → 96 → 48 kHz switch
  - the Saffire RTL residual
  - a 44.1 kHz smoke test
- **FW-186:** done. The ZTS period is threaded per epoch (`ZeroTimestampPeriodForRate`); the rate gap is closed in T5 (see G-17).
- **FW-209:** late binding. It removes the ~0.21 s start-up prefill that the 1696 store adds on prepare-time TX.
- **FW-221:** the remaining supported-rate gates, and 4x rates (a larger allocation).
