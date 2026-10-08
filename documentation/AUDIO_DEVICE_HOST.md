# Audio device host: one lifecycle shell for every audio family

**Status:** design, for review (2026-10-08; U1–U6 answered, §8). No code is implied until the stages in §6 are
accepted one at a time.

**What this replaces.** The four audio backends: `AVCAudioBackend`, `DiceAudioBackend`,
`MotuAudioBackend` and `RmeAudioBackend` (`Audio/Protocols/Backends/`), and the
`IAudioBackend` interface they implement. They are replaced by one `AudioDeviceHost` plus a
small `FamilyAdapter` per family. Names are provisional.

**Relationship to other docs.**

| Doc | Owns |
|---|---|
| [`AUDIO_SESSION_REDESIGN.md`](AUDIO_SESSION_REDESIGN.md) | The session: `SessionScheduler`, `RestartRoutine`, `StopRoutine`, `FamilyDriver`. This doc sits on top of it and changes none of it. |
| [`DEVICE_BACKEND_UNIFICATION.md`](DEVICE_BACKEND_UNIFICATION.md) | The protocol-neutral query surface and the **two-protocol rule** (§4.1 here applies it). |
| [`DICE_TCAT_ARCHITECTURE.md`](DICE_TCAT_ARCHITECTURE.md) | DICE geometry. The DICE adapter's `Describe` (§4.3) carries it unchanged. |
| [`AUDIO_BACKENDS_CONTROLS.md`](AUDIO_BACKENDS_CONTROLS.md) | Controls. Out of scope here (§7). |
| [`TOKEN_BASED_LIFECYCLE.md`](TOKEN_BASED_LIFECYCLE.md) | Route tokens. Consumed unchanged. |

Our code is cited at `feature/dice-multirate` = `3b4df5d2`.

---

## 1. Why

### 1.1 The streaming part is already shared

Since S5 every family implements `FamilyDriver` (`Duplex/FamilyDriver.hpp`). One
`RestartRoutine` and one `StopRoutine` decide the order of steps for all of them. Below
that, CIP/AMDTP, isoch and ZTS code is shared too.

What is not shared is the code **around** the session: publishing the nub, deciding
whether a fault deserves a restart, and making teardown safe. Each backend writes that
itself.

### 1.2 Four copies of one shell

Every backend owns the same set of parts:

| Part | AV/C | DICE | MOTU | RME |
|---|---|---|---|---|
| Work queue | `com.asfw.audio.avc` (`AVCAudioBackend.cpp:32`) | `com.asfw.audio.dice` (`DiceAudioBackend.cpp:203`) | `com.asfw.audio.motu` (`MotuAudioBackend.cpp:41`) | `com.asfw.audio.rme` (`RmeAudioBackend.cpp:20`) |
| Teardown latch: `stopping_`, `teardownStarted_`, `teardownComplete_` | `:281-321` | `:225-295` | `:55-81` | `:29-50` |
| `PublicationGate` | guards publication | guards publication | guards recovery only | guards recovery only |
| Recovery dedupe | per GUID | per GUID | **one flag for the whole backend** | per GUID for resume, **one flag** for timing loss |
| `IOLock` + GUID sets | yes | yes | yes | yes |

Line counts: AV/C 579, DICE 1,305, MOTU 413, RME 243. That's 2,540 lines in total, and
most of the DICE file is `EnsureNubForGuid` (`:670-1071`), which this doc keeps as the
DICE adapter's `Describe`.

The four `BeginTeardown` functions do the same five things in the same order: latch
`stopping_`, wait out a second caller, close the gate, drain the queue with
`DispatchSync`, then mark it complete. They differ only in test hooks and logging.

### 1.3 Dead code that still looks alive

**Nothing calls a backend's `StartStreaming` or `StopStreaming`.** The only callers of
`StartStreaming`/`StopStreaming`/`RequestClockConfig` in the tree are `ASFWDriver.cpp:1231,1239`
and `ASFWAudioNub.cpp:641,684,815,935,954`, and all of them go to `AudioCoordinator`.
`AudioCoordinator` has called the session directly since `22761e8e` (2026-09-19), where
`backend->StartStreaming(guid)` became a session call. Today it is `sessions_.Attach(guid, clock)`
(`AudioCoordinator.cpp:306`) and `sessions_.Detach(guid)` (`:348`).

So these are dead, along with everything only they used:

| Dead | Where | What it used to do that no longer happens |
|---|---|---|
| `*AudioBackend::StartStreaming` / `StopStreaming` (all four) | AV/C `:323-450`, DICE `:1073-1136`, MOTU `:187-244`, RME `:122-145` | DICE and MOTU called `EnsureNubForGuid` again after a successful `Attach` (DICE `:1104`, MOTU `:217`). |
| `DiceAudioBackend::RequestClockConfig` | `:1138-1153` | Called `EnsureNubForGuid` after a successful clock change. `AudioCoordinator::RequestClockConfig` (`:369-435`) does not. |
| AV/C `activeGuid_`, `configByGuid_` | `AVCAudioBackend.hpp:110,114` | Set or read only by the dead start/stop. The comment at `AVCAudioBackend.cpp:212` describes behaviour that no longer exists. |

Fields that are written but never read, in live code as well:

- DICE `attemptsByGuid_`, `retryOutstanding_`, `kCapsRetryDelayMs`, `kCapsRetryMaxAttempts`
  (`DiceAudioBackend.hpp:134-135,145-146`): only erased.
- DICE `activeStreamingGuids_` (`:136`), MOTU `activeStreamingGuids_` (`MotuAudioBackend.hpp:105`),
  RME `active_` (`RmeAudioBackend.hpp:76`): inserted and erased, never read.
- DICE `publicationRejectCount_` (`:143`): never touched. MOTU `publicationRejectCount_`
  (`:103`) and `recoveryRejectCount_` (`:95`): incremented, never read.
- `hardware_` in DICE (`:89`), AV/C (`:71`), MOTU (`:90`): stored, never used.

Whether anything depended on the two lost `EnsureNubForGuid` calls is **not known**. §8 U1
asks the question. This doc does not assume they were harmless.

### 1.4 Differences that look accidental

The four backends were written at different times, and they disagree on things that have
nothing to do with their protocols. Each row below is a decision the merged host has to
make once (§5).

| # | Question | AV/C | DICE | MOTU | RME |
|---|---|---|---|---|---|
| D1 | Is publication blocked during teardown? | gate (`:56-75`) | gate (`:673-692`, re-checked `:767-796`) | **no check** in `EnsureNubForGuid` (`:99-185`); `OnStreamsRestarted` reaches it unguarded (`hpp:70`) | `stopping_` only (`:111`), no gate |
| D2 | Republish onto a live nub | not done | validates with `RefreshNubProperties` and **never** replaces the runtime config under a live nub (`:1030-1037`) | **replaces** the endpoint config every time (`:181-183`) | **replaces** it every time (`:118`) |
| D3 | Bus reset while streaming | restart, not tied to the run (`:133`) | restart, not tied to the run (`:322`, `:364-371`) | **no `OnDeviceResumed`** | restart, **tied** to the run (`:78,85`) |
| D4 | Dedupe for timing-loss recovery | per GUID | per GUID | one flag for the whole backend (`:258`) | one flag for the whole backend (`:154`) |
| D5 | Is the fault real? | wait 256 ms, then ask whether RX replay came back (`:213-244`) | read device health; drop the fault if the clock is locked (`:398-407`) | always restart | always restart |
| D6 | Teardown test hooks | 4 | 4 | 2 | **none**, and no RME case in `BackendLifecycleRaceTests.cpp` |

D5 is a real protocol difference (§4.1). The other rows are not.

Two smaller findings:

- **DICE logs "lock health DEGRADED ... -> recover" and then does nothing**
  (`DiceAudioBackend.cpp:616-621`). The TCAT kexts also only log lock loss
  (`AUDIO_SESSION_REDESIGN.md` §2.4), so the behaviour matches the reference. The log text
  is what's wrong.
- **For MOTU, nothing comes back after a bus reset.** `AudioCoordinator::HandleBusReset`
  quiesces the host transport for every family (`:128`), and MOTU then has no rebind
  request. Linux also does not restart MOTU on reset (`motu/motu.c:143-149` only
  re-registers the handler address). It recovers on the next `snd_motu_stream_start_duplex`,
  which updates the iso resources (`motu/motu-stream.c:236-248`). Whether our timing-loss
  callback fires with the transport stopped, and so stands in for that, is unverified.
  Decided: add the rebind (§8 U3, Δ6).

---

## 2. Evidence: what is protocol and what is not

Read from the four backends, the per-family parts are exactly three:

1. **How the endpoint is described.** These are four different sources for the same
   `Model::ASFWAudioDevice` value:
   - DICE reads it from the device: caps, then the resolver, then rates/formations,
     names and labels (`:670-1071`).
   - MOTU uses live caps, or the model's fixed chunk layout before the first start
     (`:142-169`).
   - RME uses a fixed table keyed on the profile builder (`RmeAudioBackend.hpp:35-61`).
   - AV/C takes what `AVC::DiscoveryCoordinator` pushes (`DiscoveryCoordinator.cpp:189`,
     then `AudioCoordinator.cpp:216-222`).
2. **Whether a runtime fault is real** (D5).
3. **Device events only one family has.** For DICE this is the notification mailbox
   (`DiceNotificationRouter`, `:443-487`).

Everything else is the same shell: admission, route checks, queueing, dedupe, run tying,
teardown.

`Model::ASFWAudioDevice` is already the neutral description. It is a plain value that
every family fills in and the nub consumes (`Model/ASFWAudioDevice.hpp`). This design does
not invent a new `DeviceDescription` type. It makes `ASFWAudioDevice` the one thing a family
hands to the host.

---

## 3. What we have

```
AudioCoordinator  (IDeviceObserver, IAVCAudioConfigListener)
  ├── AudioSessions sessions_          — streaming (shared, S0–S6)
  ├── AudioNubPublisher publisher_     — creates/refreshes/terminates nubs
  ├── IsochDuplexHostTransport         — the one host transport
  ├── DiceAudioBackend  ┐
  ├── MotuAudioBackend  │ each: queue, gate, latch, lock, sets,
  ├── RmeAudioBackend   │ publication, recovery, teardown
  └── AVCAudioBackend   ┘
  BackendForGuid(guid) → switch on ChooseAudioBackend(plan)   (AudioCoordinator.cpp:244-269)
  OnDeviceRemoved → CancelRemoteDeviceWork on all four       (:180-183)
  BeginTeardown   → BeginTeardown on all four, in order     (:445-448)
```

`AudioCoordinator` also reaches `motu_` directly for the MOTU capture diagnostics
(`:475-518`). That stays a MOTU-only path, reached through the MOTU adapter.

---

## 4. Target architecture

### 4.1 Principles

1. **One shell.** Admission, route checks, queueing, dedupe, run tying and teardown are
   written once, in `AudioDeviceHost`.
2. **A family states only what differs on the wire or in its sources** (§2). Every
   `FamilyAdapter` method is pure virtual. A family with nothing to do writes that out,
   with its reason, as `FamilyDriver` already requires.
3. **Two-protocol rule** (`DEVICE_BACKEND_UNIFICATION.md`). A method goes on
   `FamilyAdapter` only if two unrelated families implement it meaningfully. `Describe`:
   all four. `JudgeRuntimeFault`: DICE (health read) and AV/C (settle + replay). Anything
   DICE-only, such as notification bits, stays inside the DICE adapter and enters the
   host as a neutral event.
4. **The session is not touched.** The host calls the same `AudioSessions` API the
   backends call today: `RequestRestart`, `RunningRun`, `IsStreaming`, `IsCancelled`,
   `Snapshot`, `IsReconciling`.
5. **No wire change from the merge itself.** Every behaviour change is a declared delta
   (§5), recorded in the commit and checked by goldens.
6. **Adapters hold no state of their own.** Per-device state lives in the host's
   `EndpointState` (§4.7) or in the device's protocol object. An adapter is a set of
   translations from inputs to a description, a verdict or an event, so it can be tested
   with plain values.
7. **If the SDK allows a structural change, we use it.** A published device may change
   its stream layout, formats, rate, ring size, latencies and controls inside the
   configuration-change window (§4.7). A changed description is reconfigured there, not
   refused until the endpoint is recreated. E0–E6 keep today's refuse rule (§4.3), and
   E7 replaces it.

### 4.2 Components

Target shape after E7, with the per-device protocols underneath unchanged:

```
CoreAudio HAL
   │
ASFWAudioNub (per device) ── start / stop / rate change / config window
   │
AudioCoordinator (one: device observer, one-active-device rule, removal order,
   │              host transport stop)
   ├── AudioNubPublisher · IsochDuplexHostTransport · AudioSessions   (unchanged)
   │
   └── AudioDeviceHost (one)                                          [NEW]
         │  queue com.asfw.audio.host · PublicationGate · teardown latch
         │  per-device recovery dedupe · Record() → [AudioHost]
         │  EndpointState per device: committed ASFWAudioDevice + revision   (E7)
         │
         ├─ Describe ─► diff ─► publish (first time)
         │                   └► reconfigure in the config window           (E7)
         ├─ OnDeviceResumed ─► RequestRestart(kBusResetRebind)
         ├─ OnRuntimeFault ──► adapter.JudgeRuntimeFault ─► restart or drop
         └─ OnDeviceEvent ◄── adapters: DescriptionChanged · ClockStatus · ConfigChange
               │
         FamilyAdapter (per family, no state of its own)                 [NEW]
           ├── Dice   events from the notification mailbox
           ├── Avc    events from discovery
           ├── Motu   events from the notification address               (E7c)
           └── Rme    no events (no notification mechanism known)

AudioRuntimeRegistry ── IDeviceProtocol per device ── FamilyDriver     (unchanged)
   DICE: DICETcatProtocol/SPro24Dsp → DiceFamilyDriver → DiceDeviceIo
   AV/C: BeBoB (M-Audio special, Onyx, Fireworks), GenericAvc, ApogeeDuet → FCP, CMP/PCR
   MOTU: MotuV2Protocol → MOTU registers
   RME:  FirefaceDeviceProtocol / FirefaceFamilyDriver → RME registers
```

```
Audio/Host/
  AudioDeviceHost       one instance; owns the shell
  FamilyAdapter         interface, one implementation per family
  DiceFamilyAdapter     Describe = today's EnsureNubForGuid body; notifications → events
  AvcFamilyAdapter      Describe = DiscoveryCoordinator's config; fault = settle + replay
  MotuFamilyAdapter     Describe = live caps or fixed chunks; fault = always restart
  RmeFamilyAdapter      Describe = BuildNubConfig; fault = always restart
```

**`AudioDeviceHost`** (concrete, `final`, no interface over it, like `SessionScheduler`):

| Entry point | Called by | Does |
|---|---|---|
| `RefreshPublication(guid)` | device added/resumed, restart observer, AV/C config ready | admission → record + current policy + route → `adapter.Describe` (may be async) → admission + route again → publish or refresh (§4.3) |
| `OnDeviceResumed(guid)` | coordinator, only while streaming | `RequestRestart(kBusResetRebind)` on the queue, deduped per GUID (D3) |
| `OnRuntimeFault(guid, reason)` | timing loss, cycle inconsistent, adapter events | tie to the run → dedupe per GUID → queue → `adapter.JudgeRuntimeFault` → `RequestRestart` or drop |
| `OnDeviceEvent(guid, DeviceEvent)` | adapters (DICE notifications) | `kStreamConfigChanged` → run-tied restart; `kClockStatusChanged` → health probe (§4.4) |
| `CancelRemoteDeviceWork(guid)` | coordinator on removal | drops queued work and dedupe entries for the GUID |
| `BeginTeardown()` | coordinator | latch → wait for a second caller → close the gate → drain the queue → complete. The one copy of §1.2. |

It owns one `PublicationGate` for publication **and** recovery, one teardown latch, one
`IOLock`, a per-GUID recovery set, and the test hooks DICE and AV/C have today
(`SetBeforePublishHookForTesting`, `SetOnTeardownDrainStartedHookForTesting`,
`SetOnTeardownGateClosedHookForTesting`, `SetOnSecondaryTeardownWaitingHookForTesting`).

**`FamilyAdapter`** (pure interface):

```cpp
class FamilyAdapter {
public:
    virtual ~FamilyAdapter() = default;
    virtual const char* Name() const noexcept = 0;

    // Build the endpoint description. May complete later (DICE loads geometry
    // asynchronously); completes exactly once with a description, KeepCommitted
    // (leave the live endpoint alone, §4.3), or a DescribeRefusal{kr, reason}.
    // A refusal is not retried by the host.
    virtual void Describe(const DescribeInput& in, DescribeDone done) = 0;

    // Runs on the host queue, may block; returns promptly once
    // context.Cancelled() reads true.
    virtual FaultVerdict JudgeRuntimeFault(uint64_t guid, DuplexRestartReason reason,
                                           FaultContext& context) = 0;

    // The host wires its event sink in once, before device callbacks begin;
    // null detaches it at teardown.
    virtual void SetEventSink(DeviceEventSink* sink) noexcept = 0;
};

// DescribedWithNote{device, note}: a description built partly from a model constant
// (E3a: MOTU after a failed register read); the host prints `note=` beside the outcome.
using DescribeResult = std::variant<Model::ASFWAudioDevice, KeepCommitted, DescribeRefusal,
                                    DescribedWithNote>;
enum class FaultVerdict { kRestart, kSelfHealed, kDeviceLeft };
```

`FaultContext` is what a judgement may ask about the live system, implemented by the host
for one device and one fault: `Cancelled()`, `StillStreaming()`,
`ReceiveReplayEstablished()` (AV/C's test), `ReadHealth(timeoutMs)` (DICE's test, through
`FamilyDriver::ReadHealth`, holding the protocol alive), and `Sleep(ms)` (replaceable in
tests). The adapter needs no references of its own. The code is in
`ASFWDriver/Audio/Host/FamilyAdapter.hpp`.

`DescribeInput` holds the registry record snapshot, the resolved policy and route token,
and the `shared_ptr<IDeviceProtocol>`. These are exactly the inputs the four
`EnsureNubForGuid` versions gather today. The adapter gets values, never the registry,
which keeps it host-testable without a registry fake.

### 4.3 Publish or refresh: one rule (E0–E6)

This is the interim rule. E7 (§4.7) replaces its refusal branch with a reconfiguration.

Today there are three rules (D2). The host uses DICE's, because it is the only one that
protects the running graph:

- **No nub yet:** `endpoint->UpdateConfig(desc)`, then `publisher.EnsureNub(guid, desc, tag)`.
- **Nub exists:** `publisher.RefreshNubProperties(guid, desc, tag)` only. The runtime
  config is never replaced under a live nub. A mismatch latches restart rejection, as it
  does today (`AudioNubPublisher.hpp:40-46`).

The DICE check "catalog endpoint already committed a formation → leave it alone"
(`DiceAudioBackend.cpp:781-786`) stays inside the DICE adapter until E7. Its job is to keep
a republish from overwriting a formation that a rate transaction committed later. An
example is a device without EAP: discovered at 48 kHz, switched to 96 kHz (where the
transaction learned and committed the mid-mode layout), then republished from the 48 kHz
snapshot. AV/C catalog endpoints commit through the same transaction, but
`AudioCoordinator::OnAVCAudioConfigurationReady` (`:218-220`) writes discovery's config
with no such guard (§8 U4). In E7 the guard becomes the general rule: a republish is a diff
against the committed revision.

### 4.4 Health probe: neutral, triggered by DICE

`ProbeDuplexHealth` (`:489-622`) is mostly not DICE-specific. It reads
`FamilyDriver::ReadHealth`, then compares `nominalRateHz` against the nub's rate and the
session's desired clock, and calls `nub->NotifyDeviceClockChanged` on a real
device-initiated change. Every family fills `nominalRateHz` (BeBoB `BeBoBProtocol.cpp:542`,
MOTU `MotuV2Protocol.cpp:607`, Duet `ApogeeDuetDuplex.cpp:661`, RME
`FirefaceFamilyDriver.hpp:170`), so the check moves to the host unchanged. Only DICE raises
`kClockStatusChanged` today, so only DICE runs it. That is no wire change.

The DICE-specific parts stay in the DICE adapter: decoding notification bits and the
`FormatNotification`/`FormatGlobalStatus`/`FormatExtStatus` log strings.

### 4.5 Queues

Today there are four backend queues, but at most one GUID streams (`AudioCoordinator.cpp:282-298`,
`TODO(ASFW-MULTIDEVICE)`). The queues carry only recovery blocks and health probes;
publication runs on the caller's queue. The host has **one** queue, named
`com.asfw.audio.host` (decided, §8 U2). E6 updates `CLAUDE.md`, which names
`com.asfw.audio.dice` among the audio queues.

### 4.6 What stays where it is

- `AudioCoordinator` keeps device observation, the single-active-GUID rule, host transport
  stop, removal ordering (`:153-214`) and the start/stop/clock entry points. It loses the
  four backend members and `BackendForGuid`'s switch, which becomes "adapter for GUID".
- `AudioSessions` and everything under `Audio/Session/`: unchanged.
- `IDeviceProtocol` and `FamilyDriver`: unchanged here. Taking the geometry and controls
  out of `IDeviceProtocol` is later work (§7).

### 4.7 Reconfiguration through the SDK window (E7)

**What the SDK allows** (DriverKit 27.0 SDK headers, read 2026-10-08):

- `IOUserAudioClockDevice.iig:225-230`, `RequestDeviceConfigurationChange`: "The sorts of
  changes that must go through this mechanism are anything that affects either the
  structure of the device or IO. This includes, but is not limited to, changing stream
  layout, adding/removing controls, changing the nominal sample rate of the device,
  changing any sample formats on any stream on the device, changing the size of the ring
  buffer, changing presentation latency, and changing the safety offset."
- `:261`, `PerformDeviceConfigurationChange`: "IO will be stopped prior to the performing
  the configuration change." The host restarts IO when it returns
  (`IOUserAudioDriver.iig:67-69`).
- Window-only setters: `SetZeroTimeStampPeriod` (`IOUserAudioClockDevice.iig:357`) and
  `IOUserAudioStream::SetIOMemoryDescriptor` (`IOUserAudioStream.iig:435`).
- `SetAvailableStreamFormats` and `SetCurrentStreamFormat` (`IOUserAudioStream.iig:269-288`,
  `:222-237`) carry no extra restriction; each notifies the host.
- `AddStream`/`RemoveStream`, `SetPreferredChannelsForStereo`,
  `SetPreferredOutput/InputChannelLayout` (`IOUserAudioDevice.iig:452-552`) are available.

**What we already use.** The rate transaction on `feature/dice-multirate`: inside the window,
`ASFWAudioDevice::PerformDeviceConfigurationChange` (`ASFWAudioDevice.cpp:1164-1300`)
installs the formation and calls `SetCurrentStreamFormat` with the new channel count, the
ZTS period and the latencies (`:900-960`). The available-format list is built once, at
graph build, with each rate's own channel count from its formation
(`ASFWAudioDriverGraph.cpp:452-470`). `ADKVirtualAudioLab`'s configuration probe does the
same (list at create, `ConfigurationProbeDevice.cpp:80`; select in the window, `:179-180`).
Switching between pre-listed formats with different channel counts is therefore exercised.
It is also observed on hardware. On the Pro 24 DSP on this branch (user screenshot of
Audio MIDI Setup, 2026-10-08), the input format menu lists 16 ch at 44.1 and 48 kHz and
12 ch at 88.2 and 96 kHz. With 96 kHz selected, the device shows 12 inputs and 8 outputs.

**What is not exercised.** Replacing the **available-format list** inside the window. A
device without EAP needs this, because at publish only its current mode's channel counts
are known. It is SDK-permitted, and the TCAT vendor kext does the IOAudioFamily equivalent
on every mode change. It rebuilds the format lists of the same stream objects from the
re-read registers, inside a pause / `completeConfigurationChange` bracket (§6, E7a). Our
code hasn't run the ADK version yet; E7d tests it on the Pro 24 DSP.

**Unused today:** `RemoveControl`, `SetPreferredChannelsForStereo`, and the preferred
channel layouts. `AddControl` is used at graph build only. `SetWantsStreamFormatsRestored`
is called at graph build.

**The design.** The host keeps one `EndpointState` per device: the committed
`ASFWAudioDevice` and a revision number. Every trigger that may change the description
(start, restart, reset rebind, a device event, a rate request) runs `Describe` and diffs the
result against the committed revision:

| Diff | Action |
|---|---|
| none | nothing (`Record(Publish, Unchanged)`) |
| no nub yet | publish (first revision) |
| only things the window can change (rates, formats, channel counts, latencies, ZTS period, controls, names, preferred stereo pair) | `RequestDeviceConfigurationChange` with the new revision; in the window install it, then commit. On failure, restore the prior revision as the rate transaction does today |
| something the window cannot change, or the window failed and restore failed | keep today's refusal and latch (`NubGeometryRefresh`), recorded with the reason |

The rate transaction becomes one case of this. A rate change is a description change
whose new formation comes from the catalog (EAP) or from a register read after switching
(without EAP: switch, re-read, `SetAvailableStreamFormats` with the real mode's entry, then
`SetCurrentStreamFormat`). This follows the TCAT kext (`RestartStreaming →
PopulateDeviceStruct → CreateStreams`, `DICE_TCAT_ARCHITECTURE.md` §2.5), not Linux's
per-mode cache. Profiles do not supply geometry.

**Across the seam.** The nub's properties are read by the audio driver once
(`NubGeometryRefresh.hpp:6-18`), so a revision crosses the seam the way a formation crosses
it today (`InstallRateFormation`), as a value with the route incarnation, epoch and
generation it was read under. No pointer into either service's memory crosses (FW-60).

**Device events for every family.** `DescriptionChanged` is a neutral `DeviceEvent`:
- DICE raises it from the config-change notification bits.
- AV/C raises it from discovery.
- MOTU raises it from its notification address. The vendor kext registers that address and
  re-sends it after a reset (`SendPseudoAddressSpaceAddress`,
  `tmp/motu-research/motu-vendor/evidence.md`). We never call
  `MotuV2Protocol::RegisterAsyncMessageAddress`. E7c wires it, re-sent before the Δ6
  restart in the vendor's order.

**Controls and channel roles as description data.** AV/C already publishes controls as
data (`ASFWAudioDevice::avcControls`); DICE answers per-control virtuals. Once `Describe`
returns the control list for every family, a changed control set is just another diff,
installed with `AddControl`/`RemoveControl` in the window. The same applies to the preferred
stereo pair (the device's main outputs) and to whether the HAL should restore saved formats
(not for a device whose formats depend on its own state: MOTU optical, DICE without EAP).
These ride on E7 but are separate stages (§6).

---

## 5. Declared deltas

Each delta is a behaviour change the merge makes on purpose. Each one is recorded in the
commit that makes it.

| Delta | From | To | Wire change? | Proof |
|---|---|---|---|---|
| Δ1 (D1) | MOTU/RME publish with no gate | gate for every family | no; refuses a publication during teardown | host test: publication during teardown is refused and counted |
| Δ2 (D2) | MOTU/RME replace the endpoint config under a live nub | refresh only | no; RME's config is constant, and MOTU publishes the live counts once E3a lands (Δ7) | host test plus a MOTU log check on hardware |
| Δ3 (D4) | MOTU/RME dedupe timing loss with one flag for the whole backend | per GUID | no; one GUID streams | host test |
| Δ4 (D3) | RME ties bus-reset rebind to the run | not tied, as DICE and AV/C do | possibly: a reset rebind can no longer be dropped as stale | session golden for RME reset, if the rig covers RME; otherwise hardware |
| Δ5 | DICE "DEGRADED -> recover" log | "DEGRADED (logged only, as TCAT)" | no | none needed |
| Δ6 (D3, U3) | MOTU: nothing restarts the stream after a bus reset while streaming | `Rebind` restart, as every other family | **yes**: a full restart sequence after the reset | host test for the rebind; session golden for a MOTU reset if the rig covers MOTU; hardware best effort (§8 U6) |
| Δ8 (D2, E4) | AV/C: a discovery re-delivery for a live nub overwrites the endpoint config (`AudioCoordinator.cpp:218-220`) while the nub keeps its graph | refresh check only (§4.3); a changed description latches "geometry changed" instead of silently diverging | no | host test; Phase 88 replug on hardware |
| Δ7 (U5) | MOTU publishes a fixed 14 × 14 before the first start | publishes the counts from the optical config register (`0x0c04`), read before publication | **yes, one extra quadlet read**; HAL-visible: an ADAT-mode 828mk2 publishes 22 channels per ADAT direction at 44.1/48 kHz instead of 14 | `Describe` unit tests for each optical combination; MOTU golden for the extra read |

Not a delta: D5 stays per family, which is what `JudgeRuntimeFault` is for.

Δ6 evidence. The vendor MOTU kext restarts streaming after a reset. Its device work loop
stops isoch on the reset, then re-sends the notification address
(`com_motu_driver_FWA_Box::SendPseudoAddressSpaceAddress`) and restarts the engine
(`DoManualStartAfterResume`). Source: `tmp/motu-research/motu-vendor/evidence.md`, the
`0x13e85-0x13f6b` and `0x14034` blocks. Linux re-registers the address on reset
(`motu/motu.c:143-149`) and updates iso resources on the next start
(`motu/motu-stream.c:236-248`). Our MOTU code never registers a notification address:
`MotuV2Protocol::RegisterAsyncMessageAddress` has no caller. So the rebind is a plain
session restart today. If address registration is wired later, the rebind must re-send it
before streaming, in the vendor's order.

---

## 6. Staging

Each stage deletes the path it replaces (no double paths), keeps every golden under
`tests/golden/session/` byte-identical unless it says otherwise, and ends with
`./build.sh --test-only` checked by grepping `error:` (the script's pass line is not
trusted).

**Who writes what** (decided 2026-10-08). The deciding question is what catches a mistake
before it reaches the wire. Opus reviews every diff, whoever wrote it.

| Model | Stages | Why |
|---|---|---|
| **Haiku** | E0, E2, E6; every test written from the spec (E1 host tests, E7b diff-classifier tests) | Mechanical. The compiler, the suite, goldens and grep catch mistakes. Tests written by a different author than the code check the spec, not the implementation. |
| **Sonnet** | E3, E3a, E4 | Code moves that keep existing behaviour across several files, with goldens and Linux references as the safety net. They need reading, not new design. |
| **Opus** | E1 (host core), E5, E7b–E7d; all reviews | Concurrency and teardown, the FW-218 reset history, the AudioDriverKit window contract. E7c is new wire behaviour with no hardware to test it. |

Below, *Haiku*, *Sonnet* and *Me* (Opus) mark each stage's author.

- **E0: delete the dead code** (§1.3; U1 answered: remove). *Haiku*.
  Delete:
  - the four `StartStreaming`/`StopStreaming` bodies, plus `IAudioBackend::Start/StopStreaming`;
  - `DiceAudioBackend::RequestClockConfig`;
  - AV/C `activeGuid_`/`configByGuid_`;
  - the write-only fields;
  - the unused `hardware_` members, with their constructor parameters.

  Also fix the stale comments: DICE `:807-809` (the retry no longer happens through
  `StartStreaming`) and AV/C `:212`. Check: the build, the suite, and the grep in §1.3 still
  finds no callers.
- **E1: `AudioDeviceHost` + `FamilyAdapter`, with no family moved yet.** *Me* for the host,
  *Haiku* for the host tests, written from §4.2 and §4.3 alone, without reading my
  implementation. The tests cover: double teardown; publication racing teardown
  (the existing `BackendLifecycleRaceTests.cpp` cases, ported); recovery queued just
  before the drain; `Describe` completing after teardown; per-GUID dedupe; refresh vs
  publish. A test must fail when its behaviour is broken; I check this by breaking each
  behaviour once.
- **E2: RME onto the host** (Δ1, Δ3, Δ4). *Haiku*. Smallest backend, constant
  description. Adds the RME teardown coverage it has never had (D6).
- **E3a: MOTU describes from the optical register** (Δ7). *Sonnet*. Before publication,
  `MotuV2Protocol::EnsureRuntimeStreamGeometry` reads `InOutConfV2` (`0x0c04`)
  asynchronously, as DICE loads its caps. Counts come from the same arithmetic `Configure`
  already uses (`MotuV2Protocol.cpp:383-393`), moved into one function that both call. This
  matches Linux, which reads the register at PCM open (`motu/motu-pcm.c:143`,
  `motu-protocol-v2.c:227-262`). The backend comment calling an early read a deadlock
  (`MotuAudioBackend.cpp:145-148`) is wrong and goes. The fixed 14-chunk fallback stays
  only for a failed read, recorded as `Publish/Published` with a reason.
  **Done (E3a).** The arithmetic is `ResolveV2PcmChunks` (`MotuV2Registers.hpp`), called by
  `PrepareDuplex` and by `MotuV2Protocol::EnsureRuntimeStreamGeometry` (the published
  counts). The published rates (44.1/48 kHz) are one rate mode, so one count per direction is
  right for all of them; a `static_assert` stops anyone adding 88.2/96 kHz without per-rate
  formations (open question for that day: ADAT is 22 at 1x and 18 at 2x). Bit positions
  (input 9:8, output 11:10) agree with Linux `motu-protocol-v2.c:25-32`.
- **E3: MOTU onto the host** (Δ1, Δ2, Δ3, Δ6). *Sonnet*, after E2 and E3a. Without E3a,
  Δ2 would make an ADAT-mode MOTU latch "geometry changed" on its first restart. The
  capture-diagnostics path (`AudioCoordinator.cpp:475-518`) goes through the MOTU adapter.
  **Done (E3).** `MotuFamilyAdapter` is stateless: Describe is the E3a read, every runtime fault
  restarts, no device events. `MotuAudioBackend` is gone; `MotuCaptureCommand` asks the host
  for the device's family (`KindForGuid`) instead of comparing against the backend. A failed
  register read publishes the fixed 14 x 14 only before a nub exists, as `DescribedWithNote`
  (the host prints `note=optical-read-failed-fixed-geometry` on the `Publish` line); against a
  live nub it is a `RefusedDescribe` (`reason=optical-config-read-failed`), because a fixed
  fallback there would latch "geometry changed" on one lost transaction. A reset while MOTU
  streams now runs `Rebind/Queued` then `Rebind/RestartRequested` (Δ6).
- **E4: AV/C onto the host** (Δ8). *Sonnet*. `DiscoveryCoordinator` pushes the config, so
  `AvcFamilyAdapter::Describe` returns what discovery last delivered or `kIOReturnNotReady`,
  and discovery's ready event calls `OfferDiscoveredDescription` (store, then
  `RefreshPublication`). The 256 ms settle moves into `JudgeRuntimeFault`. Hardware: Phase 88
  and Duet, timing loss and replug.
  **Done (E4, commit 29499da3).** Cycle-inconsistent stays unforwarded for AV/C, as before.
  The restart observer now also runs `RefreshPublication` for AV/C, so every restart re-checks
  the stored discovery description against the live nub (part of Δ8).
- **E5: DICE onto the host** (Δ5). *Me*. `EnsureNubForGuid`'s body becomes `Describe`,
  with every refusal kept and its log line unchanged. Notifications become `DeviceEvent`s.
  The health probe moves to the host (§4.4). The session goldens already run the real
  `DiceAudioBackend` (`AUDIO_SESSION_REDESIGN.md` S3); after E5 they run the host plus the
  adapter, and must not change. Hardware: Pro 24 DSP cold start, reset burst while playing
  (the FW-218 check), and rate change.
- **E6: delete `IAudioBackend` and the four backend files; update the docs.** *Haiku*.
  Includes `project.yml` + `xcodegen generate` (CI checks the generated project), the
  `CMakeLists` for tests, `CLAUDE.md`'s queue name (`com.asfw.audio.host`), and the "Fate
  of existing code" table in `AUDIO_SESSION_REDESIGN.md` §5.

E7 (§4.7) starts after E6. Unlike E0–E6, it changes behaviour on purpose; each part
declares its deltas in its commit.

- **E7a: removed (2026-10-08). The vendor kext already shows the mechanism.** The Saffire.kext
  4.3.0 decompile (`tmp/dicere/dec_Saffire430/`; vtable slots resolved in
  `FOCUSRITE/3.9/Saffire.i64`) shows how a DICE driver with no tables changes mode:
  1. `performFormatChange` (`V_AudioEngine::performFormatChange`) accepts **only a rate
     change**. A requested format whose channel count differs from the device's current
     stream count is refused with `kIOReturnUnsupported`. A rate change stores the rate,
     notifies clients and calls `RequestStreamingRestart`.
  2. `RestartStreaming` writes the clock, re-reads the registers (`PopulateDeviceStruct`,
     `V___RestartStreaming.c:465`), then `AllocateStreams` (`:524`) and `CreateStreams`
     (`:526`).
  3. `CreateStreams` (@0x3b54) runs between `pauseAudioEngine` (vtable +2640) and
     `resumeAudioEngine` + `completeConfigurationChange` (+2648, +2824). It does not
     destroy streams. For each stream still present, `createNewAudioStream` (@0x47ee):
     - `clearAvailableFormats` (+2664), then `addAvailableFormat` (+2648) once per rate
       bit in the device's CLOCK_CAPS mask, **all with the channel count just read**;
     - `setFormat` (+2616) at the current rate.

     A stream no longer present is kept but hidden: `setStreamAvailable(false)`, buffers
     and formats cleared. A new stream is added with `addAudioStream`.

  So the kext lists every advertised rate with one channel count, the current mode's, and
  after a switch rebuilds the same stream objects' format lists from the re-read
  registers. That is variant (ii) of the earlier plan. Variants (i) and (iii) are
  dropped. The HAL above IOAudioFamily is the same coreaudiod that runs ADK drivers, and
  the ADK equivalent is SDK-permitted (§4.7), so no separate experiment runs. E7d tests
  the path on hardware through the `ASFW_DICE_IGNORE_EAP` lane.
- **E7b: `EndpointState` and the reconfigure branch.** *Me* for the host; *Haiku* for the
  diff classifier's tests (pure values). The rate transaction moves under the host as one
  case of a description change. The DICE committed-formation guard (§4.3) becomes the
  general revision diff, and U4 goes away. `NubGeometryRefresh`'s latch stays only for the
  last row of §4.7's table. New `HostEvent` `Reconfigure` with outcomes `Requested`,
  `Committed`, `Restored`, `RestoreFailed`, `RefusedNotWindowable`.
- **E7c: MOTU notification address.** *Me*. Register at first publication and re-send it
  after every reset, before the Δ6 restart (vendor order). Release it on teardown
  (`motu-transaction.c:121-133`). Notifications become `DescriptionChanged` / `ClockStatus`
  events.
- **E7d: DICE without EAP, every advertised rate, the kext's way.** *Me*. Builds on E7b.
  Replaces the "observed mode only" rule at `DiceAudioBackend.cpp:939-950`.
  - **Publish:** every rate in CLOCK_CAPS with one channel count, the current mode's, as the
    kext does (E7a). A CoreAudio format always carries a count, so there is no way to
    announce "unknown". When every entry has the same count, Audio MIDI Setup shows a plain
    rate menu and one format label, and when counts differ it groups formats by rate.
    - **Known and identical, for comparison: Phase 88.** Its live nub carries five
      `ASFWRateFormations` entries, 32–96 kHz, each 10 PCM per direction (`ioreg`,
      2026-10-08). They come from the Music subunit capabilities that AV/C discovery reads
      (`AvcAudioConfig.cpp:177-227`). AMS shows them collapsed: 32–96 kHz plus
      "10 ch 32-bit Float".
    - **Known and different: the Pro 24 DSP with EAP**, grouped per rate (§4.7).
    - **Not known: DICE without EAP.** The same collapsed view, but only the current mode's
      count is real. The other modes' entries are the kext's assumption until a switch
      re-reads the registers. The endpoint records which entries are confirmed, so a
      republish or a later switch never treats an assumed count as a committed formation.
  - **A rate change into another mode,** inside the window: switch the clock, re-read the
    TX/RX registers, `SetAvailableStreamFormats` with every rate at the new count, then
    `SetCurrentStreamFormat`. The stream objects stay the same. On failure, restore the
    prior list and format.
  - **A host request for a channel count the device doesn't have** is refused, as the
    kext's `performFormatChange` does.
  - **Test lane:** a build setting `ASFW_DICE_IGNORE_EAP` (in `project.yml`; never ships
    enabled) makes the Pro 24 DSP skip its EAP catalog. It is the only DICE device on hand,
    and its plain registers agree with its EAP data for the current mode
    (`DICE_TCAT_ARCHITECTURE.md` §5 Q2).
  - **Pass:**
    - before the first switch, AMS shows a plain rate menu (44.1–96 kHz) and one
      "16 ch" input format, with no per-rate counts;
    - 48 → 96 kHz then shows 12 inputs and 8 outputs, matching the EAP run in §4.7;
    - 96 → 48 kHz shows 16 inputs again;
    - device and stream object IDs are unchanged;
    - IO resumes without reopening the device, and playback and capture work at both
      rates;
    - `[RateTxn]` plus one line per list replacement (old and new counts).
  - **Regression:** the same switches with EAP (the normal path).
  - **Last-resort fallback,** only if ADK refuses the list replacement: recreate the
    endpoint on a mode change.
  - **Who needs this in the field:** only DICE devices without EAP that advertise more than
    one mode. In the catalog: Pro 40 TCD3070, the Weiss DACs, unlisted DICE devices. None
    of the recorded devices without EAP (Venice, StudioLive, MultiMix: 44.1/48 kHz only)
    does.
- **Follow-ups that ride on E7, each a separate stage:** controls as description data with
  `AddControl`/`RemoveControl` in the window; preferred stereo pair
  (`SetPreferredChannelsForStereo`) from the description; `SetWantsStreamFormatsRestored`
  from the description.

Hardware is batched: E2 and E3 have none of their own, E3a and E4/E5 have the checks
listed above, and all of it runs once at the end, per the hardware-cost rule in
`CLAUDE.md`. No MOTU or RME hardware is on hand, so those are best effort (§8 U6): host
tests, goldens and the references are the bar.

### 6.1 Instrumentation is part of every stage

A stage is not done until a hardware run can show which path ran and what it decided.
The host is event-driven, not a hot path: each decision below happens at most a few times
per start, reset or fault, so it is logged **every time**, at `ASFW_LOG` (Default level,
always persisted).

**One emission site.** Every decision goes through one function,
`AudioDeviceHost::Record(guid, HostEvent, HostOutcome, IOReturn)`. It writes one line and
increments a per-outcome counter. Nothing else in the host logs a decision. Because the
outcome is an enum in the host's interface and the host tests assert on the counters, a
path that skips `Record` fails a test. Logging does not depend on whoever writes the stage
remembering to add it.

Line format, with one tag so a single predicate finds the whole trail:

```
[AudioHost] guid=%016llx family=%s event=%s outcome=%s kr=0x%08x (%s) run=%llu
```

| `HostEvent` | `HostOutcome` values |
|---|---|
| `Publish` | `Published`, `Refreshed`, `PublishFailed`, `RefusedTeardown`, `RefusedStaleRoute`, `RefusedNoPolicy`, `RefusedNoAdapter`, `RefusedDescribe` (with the adapter's `kr` and reason), `RefusedGeometryChanged`, `KeptCommittedFormation` |
| `Rebind` (bus reset while streaming) | `Queued`, `Deduped`, `NotStreaming`, `Cancelled`, `RestartRequested`, `Declined`, `Failed` |
| `RuntimeFault` | `Queued`, `Deduped`, `Cancelled`, `SelfHealed`, `DeviceLeft`, `RestartRequested`, `Declined` (the session declined, e.g. a stale run), `Failed` |
| `DeviceEvent` | `ConfigChange`, `NotStreaming`, `Cancelled`, `ClockProbe`, `ClockHealthy`, `ClockDegraded` (logged only, as TCAT), `DeviceRateChange` (with device and host rates), `ClockEcho`, `ProbeFailed`; E7 adds `DescriptionChanged` |
| `Reconfigure` (E7) | `Requested`, `Committed` (with old and new revision), `Restored`, `RestoreFailed`, `RefusedNotWindowable` |
| `Teardown` | `Drained` (one summary line: drain ms plus every counter since start) |

Adapters keep their existing protocol-detail lines (the DICE geometry and rate lines in
`EnsureNubForGuid`, the AV/C settle line, the MOTU `prepare duplex ... txChunks rxChunks`
line), unchanged, because existing hardware notes are read by them. An adapter's
`Describe` refusal returns a `kr` and a short reason string, which `Record` prints. The
adapter does not print it a second time.

**Reading it** (the `zsh` builtin shadows `log`, so use the absolute path):

```bash
/usr/bin/log show --last 20m --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[AudioHost]" OR eventMessage CONTAINS "[Session]"'
```

The driver log ring retains the same lines when `log show` has aged them out.

**Per-stage check.** Each stage lists, in its commit, the `[AudioHost]` lines a hardware
run should show for the scenarios it touches. Examples: cold start, which shows
`Publish/Published` and then the session's start line, and a reset while playing, which
shows `Rebind/Queued` and then `Rebind/RestartRequested` and a new `run=`. A missing line
on hardware is a finding, not noise.

### 6.2 Rules for delegated tasks (Haiku and Sonnet)

Subagents get `CLAUDE.md` but not my memory, so each task prompt repeats these rules:

- One task, one worktree, one commit. Never `git stash` (this repo has old stashes from
  other branches).
- Temporary files go in the repo's `./tmp`, never `/tmp`.
- `./build.sh --test-only` can report a pass when tests failed: grep its output for
  `error:` and for failed test counts yourself.
- New source files go in `project.yml`, then run `xcodegen generate`. Never edit the
  pbxproj.
- No behaviour change beyond the deltas the task names. A difference you find that is not
  listed: stop and report it, don't fix it.
- Keep each log line's text unless the task says otherwise; log lines are what hardware
  runs are read by.
- Every decision your change adds or moves goes through `AudioDeviceHost::Record` (§6.1),
  and a host test asserts its outcome counter. Instrumentation is part of the task, not a
  follow-up. A diff that adds a path without a `Record` call is not done.
- List in the commit message the `[AudioHost]` lines a hardware run should show for the
  paths you touched.

---

## 7. Not in scope

- **Taking geometry out of `IDeviceProtocol`** (`GetRuntimeAudioStreamCaps`,
  `EnsureRuntimeStreamGeometry`, `RateFormations`, `GetChannelLabels`). After E5 every
  description is built in one adapter per family, which is the precondition. MOTU's
  fixed-chunk fallback (after E3a, only for a failed register read) and RME's 18/28 table
  remain the last host-side geometry for these families.
- **The control model itself.** There are two control models today: AV/C publishes
  controls as data (`ASFWAudioDevice::avcControls`), while DICE/Saffire answer per-control
  virtuals (`IDeviceProtocol::SupportsBooleanControl` etc., used at
  `ASFWAudioNub.cpp:1058-1109`). The data shape belongs to `AUDIO_BACKENDS_CONTROLS.md`.
  This doc only fixes where it travels (the description, §4.7).
- **Multi-device streaming.** One host queue doesn't block it; nothing here adds it.
- **Turning BeBoB/Apogee/MOTU's async chains into straight-line code**
  (`AUDIO_SESSION_REDESIGN.md` S5 note). That needs wire traces first and hardware.

---

## 8. Open questions

- **U1. Answered (2026-10-08): remove as dead** (E0). The two calls lost in `22761e8e`:
  - **DICE, after start and after a clock change.** `EnsureNubForGuid` on a live nub ends
    in `RefreshNubProperties` (`DiceAudioBackend.cpp:1032-1036`), which only checks and
    latches. It never writes properties (`AudioNubPublisher.cpp` `RefreshNubProperties`;
    `NubGeometryRefresh.hpp:6-24`). The same check still runs after every fault and reset
    restart through the restart observer (`SessionScheduler.cpp:702-705`). The rate itself
    is kept in sync by `AudioCoordinator::RequestClockConfig` (`:420-427`) and, for
    catalog endpoints, by the rate transaction.
  - **MOTU, after start.** It called `endpoint->UpdateConfig` with live caps, which feeds
    the direct-memory allocation (`AudioEndpointRuntime.hpp:548-552`) while the HAL graph
    keeps its published channel count. Losing it removed a hazard. The real fix is
    publishing the right counts up front (E3a).
  - **Remaining gap:** nothing checks a non-catalog DICE device's geometry between
    publication and the first start. Not wired now; E7b's revision diff covers it.
- **U2. Decided (2026-10-08):** one queue, `com.asfw.audio.host`, for all families.
- **U3. Decided (2026-10-08):** add the MOTU rebind; streaming must recover. See Δ6.
- **U4. Explained; resolved by E7b.** The risk is a republish overwriting a formation that a
  rate transaction committed later (§4.3 has the worked example). DICE guards it. AV/C
  doesn't, but its discovery usually reads complete per-rate information from the device,
  so a re-delivery would read the device at its committed rate and match. Still unchecked:
  when `AVC::DiscoveryCoordinator` re-delivers (`DiscoveryCoordinator.cpp:189`). E7b makes
  the question moot: every republish is a diff against the committed revision.
- **U5. Answered (2026-10-08): no.** 14 × 14 is right only while neither optical direction
  of an 828mk2 is in ADAT mode. ADAT adds 8 chunks at 1x and 4 at 2x, independently per
  direction: Linux `motu-protocol-v2.c:246-262` (`V2_IN_OUT_CONF` at PCM open,
  `motu-pcm.c:143`), and our own `MotuV2Protocol.cpp:383-393`, which computes it at
  `Configure`. Today an ADAT-mode device is published as 14 while the wire carries 22. This
  is not verified on hardware. Fixed by E3a (Δ7). Further MOTU material, not yet folded in:
  `tmp/motu-research/` (vendor kext notes for v1/v2/v3 geometry, the PR #172 review) and
  the vendor driver under `~/DEV/FirWireDriver/OTHER/KEXTs/MOTU`. Vendor note to check
  against E3a/E7d: the vendor's `AllowOptical` accepts optical only at 1x, while Linux
  reserves ADAT chunks at 1x and 2x (`tmp/motu-research/motu-vendor/v1-v2-v3-comparison.md`).
- **U6. Answered (2026-10-08):** no MOTU or RME hardware on hand. Best effort: host tests,
  goldens, the references, and the vendor drivers in `~/DEV/FirWireDriver/OTHER/KEXTs/`
  (MOTU, RME) read with IDA when a behaviour question needs them.
