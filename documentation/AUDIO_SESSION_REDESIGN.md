# Audio session redesign: one restart routine, DICE first

**Status:** design, for review (2026-09-24). No code is implied until the stages in §6
are accepted one at a time.

**What this replaces.** `AudioDuplexCoordinator` and its helpers, which today own start,
stop, clock change and recovery for every audio family, and the DICE bring-up controller
that plugs into them. DICE is the first family to move; the others follow onto the same
model (§6).

**Relationship to other docs.**

| Doc | Owns |
|---|---|
| [`DICE_TCAT_ARCHITECTURE.md`](DICE_TCAT_ARCHITECTURE.md) | DICE **geometry**: profiles, the resolver, geometry stages B–D. This doc does not repeat it. |
| [`DEVICE_BACKEND_UNIFICATION.md`](DEVICE_BACKEND_UNIFICATION.md) | The protocol-neutral query surface, and the idea of a simulated DICE backend (used here in S0). |
| [`TOKEN_BASED_LIFECYCLE.md`](TOKEN_BASED_LIFECYCLE.md) | Route tokens and removal terminology. The session model here consumes route tokens unchanged. |
| [`DICE_STABILITY_REGRESSION.md`](DICE_STABILITY_REGRESSION.md) | The hardware-attested DICE working path. Its lessons are constraints here (§2.6). |

**Reference pinning.** Vendor kext addresses are from the IDA databases under
`/Users/mrmidi/DEV/FirWireDriver/OTHER/KEXTs/` (local-only), decompiled 2026-09-24 with
idalib; the scripts and dumps are in `tmp/dicere/`. Unless stated otherwise an address is
from **MidasFW 4.2.1**. Linux citations refer to `references/linux-sound-firewire-stack/`
(gitignored). Our code is cited at `main` = `9417cc2d`.

**License rule.** The vendor kexts are proprietary; Linux is GPLv2. Both are used for
**behaviour only**. Nothing here is, or may become, copied code.

---

## 1. Why

### 1.1 The size of the thing

| Part | Lines | What it is |
|---|---:|---|
| `Backends/AudioDuplexCoordinator.{hpp,cpp}` | 2,194 | Start, stop, clock change, recovery, for all families. `DuplexStartTransaction::Run` alone is ~650 lines (`AudioDuplexCoordinator.cpp:913`). |
| `RestartJournal`, `ClockRequestBroker`, `DuplexOperationGate`, `RestartSessionStore`, `DuplexRecoveryPolicy` | 1,134 | Machinery around the coordinator: per-GUID gate, session store, clock tokens, restart journal, recovery decisions. |
| `Duplex/DuplexControlTypes.hpp` | 470 | An 8-alternative lifecycle variant plus a 16-value phase enum plus a rollback ledger (`:33-138`). |
| `DICE/Core/DICEDuplexBringupController.{hpp,cpp}` | 1,957 | DICE bring-up as ~35 chained async steps (`Do*` methods). |
| `Backends/DiceAudioBackend.{hpp,cpp}` | 1,387 | Publication, notification handling, health probes, recovery gating. |

The TCAT vendor driver does the equivalent DICE job with **one** linear function plus a
debounce timer (§2.2).

### 1.2 Two concurrency models stacked

The coordinator runs on a worker queue (`com.asfw.audio.dice`, `DiceAudioBackend.cpp:297`)
and **blocks**: `SyncAsyncBridge` polls a completion flag every 10 ms, and
`StartStreaming` spins on the GUID gate with `IOSleep` for up to 12 s
(`AudioDuplexCoordinator.cpp:286-296`). Underneath it, the DICE controller is written as a
chain of async callbacks (`DICEDuplexBringupController.hpp:95-146`), because it was built
for a queue that must not block. Then `StopDuplex` blocks again for up to 5 s
(`DICEDuplexBringupController.cpp:1453-1481`).

We pay for both models and get the benefit of neither: the callback chain is hard to read,
and the blocking outer layer already makes a linear style possible.

### 1.3 Bugs live at the seams

Found in review during this design session, all at boundaries between separately-owned
pieces of state:

- A same-GUID persona change creates the new protocol, then the `DeviceManager` removal
  event deletes it (`ControllerCoreDiscovery.cpp:657` runs before `:666`;
  `AudioCoordinator.cpp:162`).
- Generic AV/C units get a CoreAudio nub but no protocol, so `StartIO` always fails
  (`AVCDiscovery.cpp:957`, `AudioDuplexCoordinator.cpp:967`).
- A changed AV/C geometry is never refused: `EnsureNub` returns early for an existing nub
  (`AudioNubPublisher.cpp:55`); only DICE calls `RefreshNubProperties`.
- DICE asks the IRM for exactly one fixed channel (`DuplexStreamProfile.hpp:175, 200-240`);
  both references let the IRM choose (§2.7).
- DICE config-change notifications are ignored (`DiceAudioBackend.cpp:577` returns unless
  `LOCK_CHG` or `EXT_STATUS`); TCAT restarts on them (§2.4).
- The DICE notification mailbox is one process-wide global with no source attribution
  (`DICENotificationMailbox.hpp`), so a second DICE device would share it.
- `DuplexStreamProfile::playbackWireFormat` (`:106`) is only ever logged
  (`AudioDuplexCoordinator.cpp:1278`) and disagrees with the encoder actually used
  (the audio-side profile's `TxWireFormat`).

### 1.4 Recovery fights churn instead of absorbing it

`DiceAudioBackend::HandleRecoveryEvent` (`:430-470`) carries a long comment about
CoreAudio's rapid `StartIO`/`StopIO` cycles producing false faults, and drops events by
checking in-flight operations and restart epochs. TCAT has no such code because it has
no such problem: requests are counted and coalesced, and HAL start/stop does not touch the
wire at all (§2.2).

---

## 2. Evidence

### 2.1 The TCAT SDK: five kexts, one codebase

All five DICE vendor kexts on hand are the TC Applied Technologies reference driver with
the class prefix renamed (`MidasFW::`, `PaeFireStudio::`, `Saffire::`, `WeissFirewire::`,
`AlesisFirewire::` → normalised to `V_::` for comparison).

| Kext | Build | Compared with Midas |
|---|---|---|
| PreSonus FireStudio | 4.2.1, 2016 | 353/353 same normalised symbols; **347/353 function bodies instruction-identical** |
| Weiss | 4.3.1, 2016 | Same logic: most bring-up functions score 0.9+ on decompiled token similarity, and the low scorers (0.5–0.85) diff as compiler restructuring. Adds a DCP control-panel channel |
| Focusrite Saffire | 4.1.4, 2014 | Older SDK revision of the same skeleton (similarity 0.57–0.98) |
| Alesis | 3.5.6, 2011 | Oldest revision; bus-reset handler instead of suspend/resume |

Midas and PreSonus differ in exactly two places: a build-number constant, and the
`probe()` model table. The vendor-specific content of every kext is:

- Info.plist match on `Unit_Spec_ID` = vendor OUI, `Unit_SW_Version` = 1, plus display strings.
- `V_Audio::probe` (Midas `0x15c8`): model index = GUID bits 31:22, name used only in `IOLog`.
- **No per-model geometry, wire format or quirk.** TX PCM goes through one
  `Float32ToSwapInt24_In_32_X86` in all five, including Saffire.kext, which drives the
  Pro 40. (Our Weiss and Generic DICE profiles still default to AM824 TX; see
  `DICE_TCAT_ARCHITECTURE.md` §3.2.)

### 2.2 The TCAT lifecycle

```
V_Audio::start (0x1724)                   per FireWire unit
  └─ runOnGate(StartDevAction) → V_::StartDev (0xb8f6)
        fill a free slot in a 5-entry device table
        RequestStreamingRestart (0xbbaa)   ← atomic counter += 1

bus reset   → SuspendDev (0xbc6c): mark suspended, StopStreaming(partial)
rediscovery → ResumeDev  (0xbde4): clear flag, RequestStreamingRestart
unplug      → RemoveDev  (0xbf06): RelinquishOwnership, StopStreaming, clear slot,
                                    RequestStreamingRestart if devices remain
rate change → SetNewSamplingRate (0x8ec8) → RequestStreamingRestart
clock src   → SetNewClockSyncSource (0x8fd2) → RequestStreamingRestart
RX/TX config-change notification → RequestStreamingRestart (§2.4)

TimerFired (0xa6b6), every 200 ms (period set in initHardware 0xa1fe):
  new requests since last tick?  → StopStreaming(full), countdown = 2
  countdown reaches 0            → RestartStreaming (0xdb62)
```

Two properties matter more than any individual step:

1. **One entry point.** Every event, whatever its source, becomes "request a restart".
   Storms of events coalesce into one restart after ~400 ms of quiet.
2. **The wire is independent of CoreAudio.** `V_AudioEngine::performAudioEngineStart`
   (`0x4682`) and `performAudioEngineStop` (`0x4968`) reset buffers and counters; neither
   starts or stops isochronous DMA. The only callers of `StopStreaming` and
   `RequestStreamingRestart` are device lifecycle, settings, notifications, the timer, and
   `performFormatChange`. Streams run whenever a device is present.

### 2.3 `RestartStreaming`: the whole bring-up

One function, top to bottom (`0xdb62`, with helpers):

| # | Step | Register / mechanism | Timeout |
|---|---|---|---|
| 1 | Read section table + GLOBAL for each present device | 0x28 B @ `0xFFFFE0000000`, then GLOBAL (`PopulateGlobalDeviceStruct 0xc25e`) | — |
| 2 | Claim owner **only if the bus generation changed** | CAS `OWNER` from `0xFFFF000000000000` (`GetOwnership 0xc624`); allocates a per-device notification address | — |
| 3 | Choose clock master; drop devices on another bus | configured master GUID, else first device | — |
| 4 | Validate rate and source against `CLOCK_CAPABILITIES` | fall back to first supported; mark "clock changed" | — |
| 5 | Write `CLOCK_SELECT` to every device | master: chosen source; others: ARX1 | wait `CLOCK_ACCEPTED`, ≤150 ms (2 ms sleeps) |
| 6 | Read TX/RX stream sections | `PopulateDeviceStruct 0xc89a`; `TX_NUMBER ≥ 3` or `RX_NUMBER > 4` = error | — |
| 7 | **Only if the clock changed**, wait for lock | master `LOCK_CHG` notification, then `STATUS` bit 0 | ≤1 s (4 ms sleeps) |
| 8 | Allocate isoch resources | `IOFWIsochChannel`, IRM picks **any** channel (`V_IsocPort::getSupported 0x1232c` returns mask `-1`); the port callback writes `ISOCHRONOUS` + TX `SPEED` / RX `SEQ_START=0` (`allocatePort 0x123bc` → `ReportChannelAndSpeedToHardware 0x11404`) | — |
| 9 | Start | prefill TX, `ENABLE=1` on every device, sleep 2 ms, start IT, then IR (`StartStreams 0xfb2e`) | — |

Any failure along the way calls `RequestStreamingRestart` and returns: the next timer tick
retries. There is no rollback ledger. `StopStreaming(full)` (`0x116ca`) is total: stop
host contexts, wait for callbacks to drain, `ENABLE=0` on every device, release channels
(`releasePort 0x1248e` writes `ISOCHRONOUS = -1`).

### 2.4 Notifications

`NotificationWriteCallback` (`0xd780`), per device:

| Bit | Meaning | TCAT reaction |
|---|---|---|
| `0x20` CLOCK_ACCEPTED | device took `CLOCK_SELECT` | decrements the step-5 wait counter |
| `0x01`/`0x02` RX/TX_CFG_CHG | device changed its stream config | `RequestStreamingRestart` (unless one is running) |
| `0x10` LOCK_CHG | lock state changed | step-7 counter; deferred `WriteNotificationReceived` (`0xb640`) reads `STATUS` and logs |

There is no health-probe or recovery ladder. Lock loss is logged; a real config change
restarts.

### 2.5 Linux

- **Owner** is claimed when the driver binds and re-claimed after every bus reset:
  `register_notification_address` CAS from `OWNER_NO_OWNER`, accepting "already ours"
  (`dice-transaction.c:163-215`), called again from `snd_dice_transaction_reinit` (`:254`).
  Released only when the driver unbinds (`:217`).
- **Clock:** `select_clock` always writes `CLOCK_SELECT`, waits `NOTIFICATION_TIMEOUT_MS`
  = **100 ms**, and tolerates a timeout only if the value did not change
  (`dice-stream.c:12, 60-98`).
- **Reserve then start:** stop, `finish_session` (all `ISOCHRONOUS=-1`, `ENABLE=0`),
  `select_clock`, re-read the stream counts, reserve resources (`:265-323`). Start writes
  each stream's `ISOCHRONOUS`/`SPEED`, then `ENABLE`, then starts the AMDTP domain and
  waits for it to be ready (`:382-470`; `ENABLE` at `:442`, domain start at `:453`). "Either no streams run or all streams run" (`:378-381`).
- **Channels:** the IRM chooses from channels 0–31 (`dice-stream.c:506`).
- **Bus reset:** the firmware disables streaming and is unresponsive for hundreds of ms,
  so Linux stops its streams and lets the application restart (`:587-607`).
- **Wire lifetime:** Linux streams while a PCM substream is open (`substreams_counter`),
  i.e. per client. TCAT streams while the device is present.

### 2.6 Our own hardware lessons (constraints on the redesign)

These were paid for on hardware and must survive any rewrite:

1. **Prefill the whole TX ring with NO-DATA before IT RUN.** `DICE_STABILITY_REGRESSION.md`
   §2: all known-good builds did; the regressed build seeded 144 packets.
2. **`CLOCK_SELECT` may be skipped only if the requested *and* the achieved rate match.**
   `DICE_STABILITY_REGRESSION.md` §3, hardware-validated. Rewriting `CLOCK_SELECT` while
   streams are being enabled wedged the device (`DICEDuplexBringupController.cpp:555-560`).
   TCAT and Linux always rewrite it, but only with every stream stopped.
3. **Clear every stream's `ISOCHRONOUS` on stop, not just stream 0.** A stale duplicate
   channel wedges the device until a power cycle (`DICEDuplexBringupController.hpp:111-116`).
   TCAT (`releasePort`) and Linux (`stop_streams`) both clear every stream.
4. **Program every stream before the single `ENABLE`** (Venice F32, two streams per
   direction; `DICEDuplexBringupController.hpp:119-123`).
5. **Host start order IR then IT, 2 ms after `ENABLE`** is the order attested on the
   Saffire Pro 24 DSP (`DuplexStreamProfile.hpp:33-58`; `DICE_STABILITY_REGRESSION.md` §2).
6. **Weiss INT202/203 need host IT before they can report source lock**
   (`FamilyProtocolConstruction.cpp:95`, `DiceWeissInt` policy).
7. **CoreAudio issues rapid `StartIO`/`StopIO` cycles** when it probes rates
   (`DiceAudioBackend.cpp:446-456`).

### 2.7 Where the sources disagree

| Question | TCAT | Linux | Ours today | Recommendation |
|---|---|---|---|---|
| Owner lifetime | claim on new generation, hold while present | claim at bind + after reset, hold until unbind | claim every prepare, release every stop | **Hold while present, re-claim per generation.** Both references agree, and notifications keep flowing while idle (needed for §2.4). |
| Isoch channel | IRM picks any of 64 | IRM picks from 0–31 | fixed one-bit mask, defaults 1/0 | **IRM picks**, mask 0–31 (the narrower, Linux mask). Write the result to the device. |
| `CLOCK_SELECT` rewrite | always, streams stopped | always, streams stopped | skip if requested + achieved match | Keep our skip rule (lesson 2); it is a strict subset of safe behaviour. Always stop first. |
| Clock-accepted wait | ~150 ms | 100 ms | 150 ms (comment wrongly cites Linux) | 150 ms, cite TCAT. |
| Lock wait | only if the clock changed, ≤1 s | via domain ready, 200 ms | always; up to 2 s + a 200 ms confirm poll | Wait when the clock changed or the device is unlocked; single 1 s bound. |
| Host start order | ENABLE → 2 ms → IT → IR | ENABLE → domain start | ENABLE → 2 ms → IR → IT | **Keep IR → IT** (lesson 5, hardware-attested). Record the TCAT difference. |
| Config-change notification | restart | wakes userspace | ignored | **Restart** (via the request counter). |
| Wire lifetime | while device present | while a client is open | while CoreAudio runs | **Decision §4.4.** |

---

## 3. What we have

### 3.1 Coordinator responsibilities

`AudioDuplexCoordinator` today is several jobs in one class:

| Job | Where |
|---|---|
| Admission: one operation per GUID, stop intents | `DuplexOperationGate`, `TryAcquireGuid` (`:1934`) |
| Session persistence, restart ids, epochs | `RestartSessionStore`, `IsRestartEpochCurrent` (`:1956`) |
| Clock-change requests as tokens with completions | `ClockRequestBroker`, `RunClockRequestLoop` (`:800`) |
| Recovery decisions | `DuplexRecoveryPolicy`, `RunRecoveryStreaming` (`:704`) |
| Phase journal / FSM logging | `RestartJournal` |
| The start sequence | `DuplexStartTransaction::Run` (`:913-1594`) |
| Global clock stability wait | `WaitForStableGlobalClock` (`:1595`) |
| Stop | `DuplexStartTransaction::Stop` (`:1668`) |
| Idle clock apply | `ApplyIdleClock` (`:1769`) |
| IRM, host isoch | `DuplexIRMReservations`, `IsochDuplexHostTransport` (kept, §5) |

### 3.2 Families behind it

`IDuplexDeviceControl` (`Duplex/IDuplexDeviceControl.hpp`) is implemented by DICE TCAT,
SPro24Dsp, BeBoB/PHASE 88/M-Audio special, Apogee Duet, Mackie Onyx / Fireworks (via the
shared AV/C+CMP base), and MOTU. Its stages are `EnsureRuntimeStreamGeometry`,
`PrepareDuplex`, `SetAssignedChannels`, `ProgramRx`, `ProgramTxAndEnableDuplex`,
`ConfirmDuplexStart`, `ApplyClockConfig`, `ReadDuplexHealth`, `Disconnect*`, `StopDuplex`.

### 3.3 Event entry points today

`StartIO` → nub → `AudioCoordinator::StartStreaming` → `AudioDuplexCoordinator::StartStreaming`;
`StopIO` → `StopStreaming`; rate or clock change → `RequestClockConfig`; timing loss, cycle
inconsistent, lock loss, TX fault → backend `HandleRecoveryEvent` → `RecoverStreaming`;
bus reset → rebind reason; removal → `CancelRemoteDevice` + `ClearSession`. Each has its
own guard logic.

---

## 4. Target architecture

### 4.1 Principles

1. **One entry point.** Anything that should change what is on the wire becomes
   `RequestRestart(reason)` on the device's session. Nothing else starts or stops streams.
2. **One linear restart routine** per session, written top to bottom as a sequence of
   `std::expected` steps. Reading it tells you the wire order.
3. **Stop is total and idempotent.** It clears every stream register, `ENABLE=0`, stops
   host contexts and releases every IRM allocation, whatever state it finds. No rollback
   ledger.
4. **Wire running is separate from HAL attached.** Whether the wire follows the device or
   CoreAudio is a per-family policy (§4.4), not an accident of where `StartIO` calls in.
5. **Geometry only from the device** (`DICE_TCAT_ARCHITECTURE.md`).
6. **The IRM picks channels**; the family writes the result to the device.
7. **Strong types for direction.** `DeviceTx`/`DeviceRx`, never "Tx" alone; the DICE
   register names and the host names are inverses (`AlesisMultiMixProfile.cpp:39` warns
   about this).
8. **Everything testable on the host**, against a simulated device (§6, S0).

### 4.2 Components

Names are provisional. Each has one job.

```
Audio/Session/
  SessionScheduler     per device: request counter, debounce, serial execution, backoff
  DesiredState         value type: rate, clock source, hal-attached, wire policy
  RestartRoutine       the neutral sequence (below)
  StopRoutine          total, idempotent stop
  FamilyDriver         interface each family implements
  SessionState         Absent | Idle | Restarting | Running | Faulted
Audio/Protocols/DICE/
  DiceRegisterMap      parsed value types (reuses DICETypes / DICETransaction parsing)
  DiceDeviceIo         blocking read / write / CAS / wait-for-notification
  DiceNotifications    per-device notification endpoint (replaces the global mailbox)
  DiceFamilyDriver     the TCAT sequence, linear
kept: IsochDuplexHostTransport, DuplexIRMReservations, DuplexStreamProfile (host geometry)
```

**`SessionScheduler`.** Counts requests with their reasons. When requests arrive it runs
`StopRoutine`, waits for a quiet period, then runs `RestartRoutine` once. A failed restart
schedules a retry with bounded backoff; N consecutive failures enter `Faulted` (logged
once, cleared by the next device event or explicit user action). It replaces
`DuplexOperationGate`, `RestartSessionStore`, `ClockRequestBroker`, `RestartJournal` and the
decision half of `DuplexRecoveryPolicy`. A clock change is "edit `DesiredState`, request a
restart". Recovery events are requests with a reason. There are no restart epochs to
compare, because a stale request is just another request.

**`RestartRoutine`** (neutral):

```cpp
std::expected<RunningSession, SessionError> RestartRoutine::Run(const DesiredState& want) {
    stop_.Run();                                               // always from a clean floor
    auto layout = TRY(family_.Configure(want));                // clock + read stream layout
    auto plan   = TRY(irm_.Reserve(layout, family_.Channels())); // IRM picks channels
    TRY(family_.Arm(plan));                                    // DICE: ISOCHRONOUS/SPEED; CMP: PCRs
    TRY(host_.Prepare(layout, plan));                          // DMA programs, whole-ring TX prefill
    TRY(family_.Enable());                                     // DICE: ENABLE=1; CMP: no-op
    TRY(host_.Start(family_.HostStartOrder()));                // e.g. IR then IT, 2 ms after enable
    TRY(family_.Confirm(host_));                               // DICE: lock/ARX; CMP: PCR + liveness
    return RunningSession{layout, plan};
}
```

`TRY` here is illustrative of early return on an unexpected value, not a proposed macro.
Every failure returns a `SessionError{step, status, detail}` and leaves recovery to the
scheduler, which always runs `StopRoutine` first.

**`FamilyDriver`** (interface; blocking; called only on the session queue):

| Method | DICE | AV/C + CMP | MOTU |
|---|---|---|---|
| `Configure(want)` | owner (per generation), `CLOCK_SELECT` + accepted/lock waits, read TX/RX | plug formats, clock | vendor registers |
| `Channels()` | IRM any (0–31) | IRM any | per protocol |
| `Arm(plan)` | per-stream `ISOCHRONOUS`, TX `SPEED`, RX `SEQ_START` | PCR connect | registers |
| `Enable()` | `ENABLE=1` | — | — |
| `HostStartOrder()` | IR → IT, 2 ms | per recipe | per recipe |
| `Confirm(host)` | lock / ARX policy (Weiss relaxed) | PCR leases, packet liveness | — |
| `Stop()` | `ENABLE=0`, every stream `-1` | PCR disconnect | registers |
| `OnDeviceEvent(e)` | map notification bits to requests | — | — |

The existing recipes (`StreamStartShape`: `CmpReceiveThenTransmit`, `ApogeeInterleaved`,
`MAudioSpecial`, `TransmitFirst`) become `HostStartOrder()` answers. `ApogeeInterleaved`
interleaves host starts with device stages, so `HostStartOrder()` returns a small plan
value (host directions plus where each sits relative to `Arm`/`Enable`) rather than a bare
order. It stays data that every family must state, not an optional hook (rule 2 below).
That is the only place where the neutral sequence bends.

**`DiceDeviceIo`.** Blocking register access on the session queue: `Read(addr, n)`,
`Write`, `CompareSwap64`, and `WaitNotification(mask, timeout)`, each returning
`std::expected`. Built on today's async bus ports. **Invariant:** bus completions must
never be delivered on the session queue, or a blocking wait deadlocks. This invariant is
what makes the linear style legal under DriverKit. Today `SyncAsyncBridge` polls at 10 ms;
whether a proper wait primitive is available in the DriverKit SDK must be checked against
the SDK headers before S1, not assumed.

**`DiceNotifications`.** One endpoint per device, attributed by source node (or a
per-device handler offset, as TCAT's per-device address space does), replacing the
global `NotificationMailbox`.

**Where polymorphism lives.** The scheduler and the restart routine are **concrete
`final` classes**. There is exactly one scheduling policy and one restart routine; that
is the point of the design, and an interface over them would advertise a second
implementation that must never exist. They are tested as the real thing, with fakes of
what they depend on (`FamilyDriver`, host transport, IRM). If a caller such as
`AudioCoordinator` or the nub later needs a test double, it gets a narrow consumer-side
interface (request restart, set desired state, wait for `Running`), never one covering
the scheduler's whole surface.

The one polymorphic seam is `FamilyDriver`, and it is a **pure interface, not an abstract
class**. Today's `IDuplexDeviceControl` shows why. It has defaulted virtuals
(`IDuplexDeviceControl.hpp:32-71`): `EnsureRuntimeStreamGeometry` succeeds,
`SetAssignedChannels` does nothing, `Disconnect*`/`BreakBothConnections` return
unsupported, `SetTeardownCancelToken` is ignored. A family that forgets one compiles and
misbehaves quietly; the `SetAssignedChannels` comment itself describes the failure
(device and host on different channels). Rules:

1. **Only pure virtuals, no data members, a virtual destructor.** Every family states its
   answer to every step.
2. **A no-op is written in the family, not inherited.** For example CMP's `Enable()` returns
   success explicitly, with a one-line reason, so a reviewer sees the decision.
3. **Shared sequencing lives in `RestartRoutine`, never in a base class.** No
   template-method bases with overridable hooks. That pattern is how DICE-shaped steps
   (`WaitForStableGlobalClock`) ended up inside the supposedly neutral coordinator.
4. **Shared per-family helpers are free functions or small composed objects**
   (`DiceDeviceIo`, a PCR helper), reached by composition, not inheritance.
5. **Resources flow in, not out.** The session owns the IRM client, host transport and
   cancellation token and passes them where needed. Today's
   `IDuplexDeviceControl::GetIRMClient()` asks the device for a bus resource and is not
   carried forward.

A closed `std::variant` of family drivers with an exhaustive `std::visit` (the shape
`FamilyProtocolConstruction` already has) was considered. It is deferred, not rejected:
stage S2 wraps each existing `IDuplexDeviceControl` in an adapter, which is natural as an
interface implementation and awkward inside a variant, and the virtual interface matches
`IDeviceProtocol`. Once S5 removes the adapter, moving to a variant is cheap if it is
wanted.

### 4.3 State

Per device: `Absent`, `Idle`, `Restarting`, `Running`, `Faulted`. The current step name is
carried for logging only. The 8-alternative lifecycle variant, the 16 phases and the
rollback ledger (`DuplexControlTypes.hpp`) go away, because stop no longer needs to know
what start acquired.

### 4.4 Central decision: does the wire follow the device or CoreAudio?

**TCAT:** the wire runs while the device is present; CoreAudio attaches to it (§2.2).
**Today:** the wire starts on `StartIO` and stops on `StopIO`.

Following TCAT for DICE would:

- remove the whole `StartIO`/`StopIO` churn class (§1.4, lesson 7);
- make `StartIO` fast: zero-timestamp anchors already exist, the clock is locked, and the
  device already accepts our stream;
- keep device notifications and lock state meaningful while idle.

It costs:

- isochronous bandwidth held while no app plays (fine for one device, relevant to
  multi-device later);
- a TX path that runs with no HAL client, sending NO-DATA or silence. The TX producer must
  not depend on a live CoreAudio buffer (see the TX pull work, memory
  `tx-pull-architecture-step4a`);
- `StartIO` must reset the TX fill cursor and exposure counters against a running wire
  (both were real bugs: memories `tx-fill-cursor-not-reset-on-startio`,
  `tx-exposure-counters-not-reset-on-restart`).

**Recommendation:** DICE follows TCAT, after a hardware spike on the Saffire Pro 24 DSP
(S4). Every other family keeps "wire follows CoreAudio" until evidence says otherwise.
Apple's own AV/C audio behaviour should be checked first; `IOFireWireAVC` in `references/`
is protocol only, so the audio driver would have to be sourced. `DesiredState` carries the
policy, so the scheduler does not care which one a family picks.

### 4.5 Concurrency and teardown

- One serial session queue per device. Only the scheduler runs on it.
- Bus completions, notifications and timers arrive on other queues and only enqueue
  requests or complete waits.
- Teardown sets a cancellation token that every blocking wait checks, then runs
  `StopRoutine` and drops cross-service views before freeing buffers, as the FW-60 rules in
  `CLAUDE.md` require.
- CoreAudio calls (`StartIO`, `StopIO`, rate change) only edit `DesiredState` and request;
  `StartIO` then waits for `Running` (bounded), as it waits for zero-timestamp today.

### 4.6 Multi-device

**Non-goal**, but not precluded. The per-device session, per-device notification endpoint and
table-shaped registry keep aggregation possible. Two candidate sync models are recorded:

- **TCAT:** one engine, one clock master, others slaved to ARX1 (§2.3 step 5).
- **CoreAudio aggregate devices**, which did not exist in the IOKit era, could provide
  multi-device sync without a driver-side engine merge.

No decision is taken here.

---

## 5. Fate of existing code

| Today | Becomes |
|---|---|
| `AudioDuplexCoordinator` public API | `SessionScheduler` requests |
| `DuplexStartTransaction::Run` | `RestartRoutine` |
| `DuplexStartTransaction::Stop`, `DICEDuplexBringupController::DoStop*` | `StopRoutine` + `FamilyDriver::Stop` |
| `ApplyIdleClock`, `ClockRequestBroker` | deleted: clock change = desired state + restart |
| `DuplexOperationGate`, `RestartSessionStore`, `RestartJournal` | deleted: scheduler serialisation + `SessionState` |
| `DuplexRecoveryPolicy` | deleted: retry/backoff in the scheduler |
| `WaitForStableGlobalClock` | moves into `DiceFamilyDriver::Configure` (it is DICE-shaped) |
| Lifecycle variant, 16 phases, rollback ledger | deleted |
| `IDuplexDeviceControl` | `FamilyDriver` (adapter during S2); its defaulted virtuals and `GetIRMClient()` are not carried forward (§4.2) |
| `AudioDuplexCoordinator` class shape | concrete `final` `SessionScheduler`; no interface over it (§4.2) |
| `DICEDuplexBringupController` (async chain) | `DiceFamilyDriver` (linear) |
| `DICETransaction`, `DICETypes` | kept for parsing; I/O moves to `DiceDeviceIo` |
| `DICENotificationMailbox` (global) | `DiceNotifications` (per device) |
| `DiceAudioBackend` recovery / health probe / `TryBeginRecovery` | deleted: notifications become requests |
| `DiceAudioBackend::EnsureNubForGuid` | kept (publication; later endpoint-lifecycle work) |
| `DuplexStreamProfile` | kept for host geometry; `playbackWireFormat` and the fixed-channel defaults deleted |
| `SyncAsyncBridge` | folded into the `DeviceIo` blocking primitive (one place) |
| `IsochDuplexHostTransport`, `DuplexIRMReservations` | kept; IRM gains the "any channel" policy |

---

## 6. Staging

Each stage deletes the path it replaces (no double paths), is hardware-checkable, and keeps
the Saffire Pro 24 DSP working. DICE hardware on hand: Pro 24 DSP only; the other DICE rows
rely on fixtures plus the vendors' identical code (§2.1).

- **S0: simulated DICE device and golden wire traces.** A host-test device built from the
  five fixture dumps: register space, notification writes, bus-reset and timeout injection.
  First, record today's controller's transaction sequence (address, op, value) per scenario as
  golden traces. No production change.
- **S1: DICE goes linear.** `DiceDeviceIo` + `DiceFamilyDriver` behind the existing
  `IDuplexDeviceControl`. The simulated device must produce the **same wire trace** as S0,
  except for deltas declared in the stage. Delete `DICEDuplexBringupController`.
- **S2: the scheduler replaces the coordinator.** `SessionScheduler` + `RestartRoutine` +
  `StopRoutine` for **all** families; non-DICE families run through an adapter over their
  current `IDuplexDeviceControl`. Delete the coordinator and its helpers. Hardware: Pro 24 DSP,
  M-Audio 1814, PHASE 88, Apogee Duet.
- **S3: wire fixes, each declared.** IRM picks channels; config-change notifications
  restart; owner held while present and re-claimed per generation; per-device notification
  endpoint.
- **S4: DICE wire follows the device**, after a spike (§4.4).
- **S5: native `FamilyDriver` for CMP families and MOTU.** Delete the adapter and
  `IDuplexDeviceControl`.
- **S6: geometry stages B–D**, owned by `DICE_TCAT_ARCHITECTURE.md` §4.2. They are independent
  of S1–S5 and can interleave.

## 7. Verification

- **Host:** golden-trace equivalence against the simulated device (S0/S1); scenario tests for
  request coalescing, stop idempotence, bus reset mid-restart, missing `CLOCK_ACCEPTED`, lost
  owner, and teardown during a blocking wait; `DiceFixtureGeometryTests` unchanged.
- **Full C++ suite** every stage; grep the log for `error:` yourself, because `build.sh` has
  reported false passes.
- **Hardware per stage:** Pro 24 DSP start, 20-minute soak (the `DICE_STABILITY_REGRESSION.md`
  §2 health markers), rate change, cable pull and replug, CoreAudio rate probing. S2 and S5 also
  need the 1814, PHASE 88 and Duet.
- **Logging:** one `[Session]` line per restart (reasons coalesced, step reached, duration),
  anomaly-only otherwise:
  `/usr/bin/log show --last 10m --info --debug --predicate 'eventMessage CONTAINS "[Session]"'`.

## 8. Open questions

1. **Wire policy for non-DICE families.** What does Apple's AV/C audio driver do? The source is
   needed before choosing.
2. **Debounce vs `StartIO` latency.** TCAT's ~400 ms quiet period suits device events. The first
   start after `StartIO` on a "follows CoreAudio" family should probably bypass it. How exactly?
3. **Blocking primitive.** Is there a DriverKit wait primitive better than 10 ms polling? Check
   the SDK headers.
4. **Where `DesiredState` lives** relative to route tokens (`TOKEN_BASED_LIFECYCLE.md`): per
   GUID across generations, or per route?
5. **Faulted exit policy.** Which events clear `Faulted`, and is a user-visible reset needed?
6. **`ApogeeInterleaved`.** Does the Duet really need interleaved host/device starts, or would the
   neutral order work? This needs Duet hardware and the reason behind the original ordering.
