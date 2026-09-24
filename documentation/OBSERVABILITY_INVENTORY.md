# Observability inventory

Epic FW-169, ticket FW-170. This is an audit of every observability mechanism in
the `main` tree as of the FW-169 branch, which covers FW-173, 172, 174, 176, 171
and 175. For each mechanism it records who produces it, who reads it, how much it
disturbs the system it measures, and what should happen to it.

The companion documents are:
- [`LATENCY_VOCABULARY.md`](LATENCY_VOCABULARY.md): the names used for latency,
  depth and the planes they are measured at.
- [`MEASUREMENT_BASELINE.md`](MEASUREMENT_BASELINE.md): how to capture a baseline
  before changing timing.
- [`MCP_TELEMETRY_RESOURCES.md`](MCP_TELEMETRY_RESOURCES.md) §14.5: the stable
  audio telemetry contract (wire v4).

**This is a snapshot.** The audio stack refactor (milestones 3, 4 and 6) will move
or replace most of the audio rows. Use the re-audit checklist at the end.

## Rules this inventory enforces

1. **Every mechanism has a consumer.** A counter or trace that nothing reads is
   deleted, not kept "just in case". FW-171 removed the ones this audit found.
2. **Hot paths log anomalies only.** On the isochronous, IO-callback and
   TX-preparation paths, a clean run prints only the coarse liveness heartbeat
   (`[TxPrep]`). Any other line means something went wrong. Telemetry rings are
   still drained so they never overflow.
3. **Stable summary vs research traces.** The audio telemetry snapshot
   (selector 1013) is a small, versioned health summary for the app, MCP and
   field reports. Experiments go to the log ring or a sidecar tool. They never
   grow that ABI.
4. **Layer boundaries apply to instrumentation too.** Transport code must not
   gain audio-format knowledge in order to log it (CLAUDE.md, *Layer
   boundaries*).
5. **Declared is not measured.** A value the driver *declares* to CoreAudio is
   evidence of intent, not of behaviour. Electrical round trip comes from
   `tools/rtl`. HAL geometry comes from `tools/halprobe`.

Perturbation classes used below:

| Class | Meaning |
|---|---|
| **none** | Off the hot path, or read on demand only |
| **low** | Relaxed atomics or a bounded ring write on a hot path; no IO |
| **gated** | Hot-path log line that fires only on an anomaly or a state transition |
| **periodic** | Fixed-rate log line (a heartbeat) |
| **verbose** | Emitted per event, but only at a raised verbosity level |

## 1. Transport, bus and controller

| Mechanism | Event / content | Producer (owner) | Consumer | Perturbation | Disposition |
|---|---|---|---|---|---|
| `LogRing` + categories (`Logging/LogRing.hpp`) | every `ASFW_LOG*` record, with category and level | all subsystems | selectors 1011/1012/1014 → app log view, MCP `asfw_log_query` | low (bounded ring) | **keep** — the primary research-trace sink. Category IDs are frozen; `PayloadWriter` (21) is reserved and has no producer |
| os_log mirror (`LogConfig`) | the same records, to the unified log | `Logging.cpp` | `/usr/bin/log`, `capture_baseline.sh` | depends on the call site | **keep**. `LogConfig initialized` is the liveness marker (CLAUDE.md) |
| `AsyncTraceCapture` (`Debug/`) | async TX/RX packet trace | `Async/Tx/Submitter`, `Async/Rx/PacketRouter` | selectors 1006/1008, MCP transactions | low | **keep** |
| `BusResetPacketCapture` (`Debug/`) | packets around a bus reset | `Async/Rx/RxPath` | status/diagnostic snapshots | low, reset-scoped | **keep** |
| `MetricsSink` (`BusReset`, `Topology`, counters) | reset/Self-ID/topology counters | `ControllerCore*` | `StatusPublisher` → status snapshot (selector 11) | none | **keep**. The `IsochRx` part was removed in FW-171 (it had no reader) |
| `StatusPublisher` | controller status snapshot plus notifications | `WatchdogCoordinator` | app status listener (selectors 10/11) | none | **keep** |
| `DiagnosticsService` + selectors 1000–1010 | bus contract, topology, role, OHCI, PHY, CSR, BM, post-reset timing | `UserClient/Handlers/DiagnosticsHandler` | app Diagnostics, MCP | none (on demand) | **keep** |
| `DiagnosticLogger` | decoded interrupt-event strings | `ControllerCoreInterrupts` | the log | verbose | **keep** |
| `ATTraceRing` (`Async/Engine/ATTrace.hpp`) | the last 256 AT arm/wake events | `ATManagerImpl` | `ATManager::DumpTrace()`, **which has no caller** | low (write on every AT arm) | **retire candidate**: written but never read. Either wire it into a panic/timeout dump or delete it (follow-up; not changed on this branch) |
| `Signposts` (`Diagnostics/`) | os_signpost intervals | **none**: only an unused include in `IsochReceiveContext.cpp` | none | none | **retire candidate** (follow-up) |
| `[TempTX]` / `[TempRX]` / `[TempROMScan*]` | per-packet async header dumps | `AsyncCommandImpl`, `RxPath`, ROM scan | a human with raised verbosity | verbose (V4) | **re-evaluate**: the "Temp" prefix marks bring-up debugging. Rename them or remove them once the async work settles |
| `[SpeedDecision]` / `[SelfIDSpeed]` / `[ROMScanSpeed]` / `[SectionReadSpeed]` / `[RoleEvidence]` / `[CyclePolicy]` / `[IRM]` / `[PHY]` / `[IRQ]` | bus-policy decisions | `Bus/`, `ConfigROM/`, `Controller/` | the log, MCP | gated (per reset or per decision) | **keep** |
| `[FSM]` / `[Lifecycle]` / `[Controller]` | controller and ROM-scan state transitions | `Controller/`, `ConfigROM/` | the log | gated | **keep** |
| `[SCSIHBA]` / `[SBP2Nub]` / `[SBP2Bridge]` | SBP-2 HBA and nub lifecycle | `SCSIController/`, `Protocols/SBP2` | the log | gated | **keep** |

## 2. Isochronous transport

| Mechanism | Event / content | Producer | Consumer | Perturbation | Disposition |
|---|---|---|---|---|---|
| `[RxDrain]` | the drain-eligibility transition (running × verbosity) | `WatchdogCoordinator::TickIsochReceive` | the log | gated (per transition) | **keep**. It names the precondition when the Zts/TxSyt drains go silent |
| `IIsochReceiveConsumer::ServiceConsumerDiagnostics` | a periodic hook into the audio consumer | watchdog, every 100 ticks **whenever RX runs** (FW-175) | `DirectAudioReceiveConsumer`: `[AudioIO]` error report, RX interval fallback close | none | **keep**. It is payload-neutral, so transport does not learn what the consumer does |
| `ZtsTelemetryRing` (`Isoch/Receive/ZtsTelemetry.hpp`) → `[Zts]` | ZTS seed/update records | the RX consumer | the watchdog drain → log; `tools/zts_sim.py` | low ring + verbose drain (verbosity ≥ 1) | **keep, but move**: ZTS is an audio clock concept living under `Isoch/`. The midi branch moved it to `Audio/Runtime/`. Port that move with milestone 3 |
| `[Isoch]` | context start/stop/error | `Isoch/` | the log | gated | **keep** |
| IT refill latency buckets, `LogStatistics`, `LogHardwareState`, `isochLogDivider_`, `itLogDivider_` | periodic IT/IR statistics | — | — | — | **removed (FW-171)**: no consumer, and periodic output on the hot path |
| Selectors 3 / 34 / 35 (`GetMetricsSnapshot`, `GetIsochRxMetrics`, …) and `ControllerMetrics` | the old metrics snapshot | — | — | — | **removed (FW-171)**. The numbers are left unassigned |

## 3. Audio

### 3.1 Stable summary (selector 1013, wire v4)

| Mechanism | Content | Producer | Consumer | Perturbation | Disposition |
|---|---|---|---|---|---|
| `AudioTelemetrySnapshot` (`Audio/Runtime/AudioTelemetrySnapshot.hpp`) | per endpoint: TX preparation latency + committed margin (current / interval / lifetime + histograms), RX capture occupancy, overrun/starvation, RX bring-up attribution, and completed-interval **duration and end ticks** | ATCB atomics written by the IO callback, TX preparation and the RX consumer; copied by `AudioEndpointRuntime` under the endpoint lock | app `AudioTelemetryView` / `RxTelemetrySection`; MCP `asfw_get_audio_telemetry`, `asfw_get_audio_stream_health` | low (relaxed atomics); the interval publish uses a seqlock | **keep — stable contract**. Changes must bump the version and update the golden fixture |

Units and planes of the retained summary fields:

| Field group | Unit | Plane / meaning | Observed or derived |
|---|---|---|---|
| `*PreparationLatencyTicks`, latency histogram | host ticks (header timebase) | TX preparation wake → work done (driver thread) | observed |
| committed margin (`*MarginPackets`), margin histogram | isoch packets | committed-to-hardware packets ahead of the OHCI IT read position | observed |
| `preparationLeadPackets`, `hardwareFloorPackets` | packets | declared geometry (`AudioTimingGeometry`) | declared constants |
| RX available / free headroom / occupancy histogram | frames | capture ring: producer (RX consumer) vs CoreAudio read cursor | observed |
| RX overrun / starvation events + frames | events, frames | capture ring ownership violations | observed |
| RX attribution counters (`rxPacketsSeen`, …) | packets | decode outcome per received packet | observed |
| `*CompletedIntervalDurationTicks` / `EndHostTicks` | host ticks | the window the interval fields cover | observed (0 = unknown) |

None of these is a latency in the sense of [`LATENCY_VOCABULARY.md`](LATENCY_VOCABULARY.md).
Margin and occupancy are **depths**. Converting them into time requires a rate,
and they must not be reported as round-trip latency.

### 3.2 Log lines

| Tag | Event | Producer | Perturbation | Disposition |
|---|---|---|---|---|
| `[TxPrep]` | liveness + margin heartbeat, every 5 s; also closes the TX/RX intervals | `ASFWAudioDriverZts.cpp` TX preparation | periodic (5 s) | **keep**: the only expected line on a clean run |
| `[TxPrepRange]` | preparation stopped short / frame short | TX preparation | gated | **keep** |
| `[TxPrepFrame]` | a frame deficit while preparing | TX preparation | gated (`frameShort`) | **keep** |
| `[TxExposure]` | exposure change or unhealthy | TX preparation | gated (changed ∨ unhealthy) | **keep** |
| `[TxAlign]` | frame-cursor self-heal | TX preparation | gated (once at start, then per re-alignment) | **keep** |
| `[TxReplay]` / `[TxReplayRearm]` | replay read failure / reclamp | TX preparation | gated | **keep** |
| `[TxProducerFatal]` | a fatal producer fault record | TX preparation | gated (fatal) | **keep** |
| `[TxWire]` | consecutive-packet dropout detection on the final wire payload | `TxWirePayloadTelemetry` | gated | **keep, not sampled**. FW-171 decided against midi's 1-in-64 sampling: dropout detection needs consecutive packets, and main inspects final content |
| `[RxReplayReset]` | RX replay reset / bootstrap phase | `DirectAudioReceiveConsumer` | gated, bounded per start | **keep** |
| `[AudioIO]` | the IO callback returned an error | `DirectAudioReceiveConsumer::ServiceConsumerDiagnostics` | gated (new error generation) | **keep** |
| `TxSyt` trace (`TxSytTraceLatest`) | the latest TX SYT decision | TX SYT path | low latest-value slot + verbose drain | **keep**: consumed by `tools/zts_sim.py` |
| `TimingCursorPolicy (fallback, not applied)` | fallback cursor policy values | `ASFWAudioDevice.cpp` | once per configuration | **keep, relabelled (FW-171)**. It never reports applied values; do not use it as evidence |
| `HAL buffer profile`, `Reported HAL latency`, `GetZeroTimestampPeriod`, `txTransferDelay` | declared geometry | `ASFWAudioDevice*` | once per configuration | **keep**; collected by `capture_baseline.sh` |
| `[MAudioTxClock]`, `[Fireworks]`, `[EFC]`, `[BeBoB]`, `[Onyx]`, `[M8]`, `[MAudio]`, `[DV]`, `[BootloaderCue]`, `[DeviceIdentity]` | device-family control and bring-up | family backends | gated / per command | **keep**; they belong to their families, not to the stable summary |

### 3.3 Other audio mechanisms

| Mechanism | Producer | Consumer | Disposition |
|---|---|---|---|
| `AmdtpTxPacketizer::TelemetrySnapshot()` | the packetizer, on every state change | **host tests only** (`AmdtpDirectTxTests`); the driver accessor was removed in FW-171 | **re-evaluate**: keep it only if the tests need it as an inspection seam, and do not add a driver reader |
| `MotuRxDiagnosticCapture` | MOTU RX path | `AudioCoordinator` (MOTU bring-up) | **keep** for MOTU bring-up; midi deletes it, so revisit at convergence |
| `RxCaptureBufferTelemetry` | RX consumer (Observe/Record*), interval close by `[TxPrep]` or the RX fallback | snapshot §3.1 | **keep** |
| PayloadWriter telemetry, `IDiagSink`, `MaybeLogDirectAudioDebugSnapshot`, `DirectAudioDebugLogState`, ATCB `txLast/Min/MaxLeadTicks`, `PacketizerTelemetrySnapshot`/`PayloadWriterCounters` accessors | — | — | **removed (FW-171)**. `tools/analyze_payloadwriter.py` is kept and marked HISTORICAL |

## 4. Host-side measurement tools

| Tool | Measures | Starts IO? | Output | Disposition |
|---|---|---|---|---|
| `tools/rtl/rtl_loopback` (FW-173) | electrical round trip: `RTL_raw`, `RTL_ts`, scheduling distance, residual, with per-trial verdicts including overload and window-edge | `--measure` only | text; `--json` (`asfw.rtl_loopback.v1`) | **keep**. Its core is unit-tested (`RtlCoreTests`) |
| `tools/halprobe/hal_geometry` (FW-172) | HAL declarations, the HAL-published timeline (ppm, residual, jumps), IO callback intervals, buffer-size sweep | `--clock`/`--io`/`--sweep` | text; `--json` (`asfw.hal_geometry.v1`) | **keep** (`HalCoreTests`) |
| `tools/baseline/` (FW-176) | N-session distribution plus lap classes | yes | a `documentation/baselines/<run>/` directory | **keep**. **No baseline has been captured yet** |
| `tools/zts_sim.py`, `tools/*_sim.py` | offline timing models | no | — | **keep** |
| `tools/analyze_payloadwriter.py` | the retired `[PayloadWriter]` format | no | — | **historical** |

## 5. midi-branch mechanisms not ported

These exist on `origin/midi` and deliberately are **not** on main. The reason is
given for each, so that convergence is a decision rather than an accident.

| midi mechanism | Why not ported now | Revisit |
|---|---|---|
| `Audio/Runtime/AudioLedgerIntervals.hpp` + `AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md` | It is tied to midi's timing model, which the audio refactor replaces. The vocabulary was distilled into `LATENCY_VOCABULARY.md` instead | milestone 3/4 (timing SSOT) |
| `Audio/DriverKit/Runtime/TxCycleTrace.hpp` | A research trace. Under rule 3 it belongs in the log ring or a sidecar, not the snapshot | only if a timing investigation needs it |
| midi's `AudioTelemetrySnapshot` changes + `AudioTelemetryWireParsingTests.swift` | Superseded by wire v4 (FW-175), which adds a header, a size budget, a seqlock and a golden fixture | converge midi onto v4 |
| `[TxWire]` 1-in-64 sampling (midi `d4eaebe`) | It breaks consecutive-packet dropout detection (see §3.2) | — |
| `ZtsTelemetry.hpp` moved to `Audio/Runtime/` | Correct layering, but a pure move better done alongside the RX refactor | milestone 3 |
| `Midi/Trace/*` (`MidiMessageTrace`, `MidiTraceLog`, `MidiTracingByteSink`) | MIDI is not on main yet | when MIDI lands |
| `Diagnostics/StatusNotificationPolicy.hpp` | Status-notification throttling lives with midi's `StatusPublisher` changes; not observability debt on main | status/app convergence |
| `SignalFormatProbeModels.swift`, `SelectProbeBootstrap` removal, AVC `Plug0StreamDiscovery` move | Protocol probing, not measurement | protocol convergence |
| midi's dead latency path (`SetQueueControl` only ever called with `nullptr`) | Dead on midi too | never |

## 6. Re-audit checklist (after milestones 3, 4 and 6)

Run this when the audio refactor lands, and update the tables above:

- [ ] Every row in §3 still has the producer it names. Anything moved has a new
      owner, and anything deleted is marked removed.
- [ ] Every retained mechanism still has a consumer. `rg` for the accessor, the
      selector or the log tag in `ASFW/`, `tools/`, `skills/` and the tests.
- [ ] The stable snapshot is still wire v4, or has been bumped with the golden
      fixture regenerated, `MCP_TELEMETRY_RESOURCES.md` §14.5 updated, and the
      Swift decoder tests passing.
- [ ] Hot paths print nothing on a clean run except `[TxPrep]` (and any new
      heartbeat, which must be listed here). Check with a 60 s clean playback
      and `/usr/bin/log show --info --debug`.
- [ ] No transport file (`Isoch/`, `Async/`, `Bus/`, `Hardware/`) gained a
      content-format or audio concept for the sake of logging. `ZtsTelemetry`
      has moved out of `Isoch/`.
- [ ] Decide the retire candidates: `ATTraceRing`, `Signposts`, the `[Temp*]`
      lines, `AmdtpTxPacketizer::TelemetrySnapshot`, `MotuRxDiagnosticCapture`.
- [ ] Re-capture the baseline ([`MEASUREMENT_BASELINE.md`](MEASUREMENT_BASELINE.md))
      on the new stack and compare against the pre-refactor one.
- [ ] Revisit every "revisit" entry in §5.
