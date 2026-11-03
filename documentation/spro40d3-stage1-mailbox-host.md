# FW-203 Stage 1 — TCD3070 (Saffire Pro 40 D3) DCP Vendor Mailbox: Host-Only Implementation

**Status: COMPLETE (host-only). STOPPING before any hardware execution, per the Stage 1 brief.**
Branch: `feat/fw203-spro40d3-mailbox-host` (worktree, based on origin/main)
Scope: pure descriptor/message codecs, a transport-independent serialized mailbox
engine, a deterministic simulated TCD3070, and host-side tests. No driver-target
changes, no live CAS registration, no hardware writes, no LGPL code (behavioral
facts from the ALSA reference are cited per item; all code is fresh C++).

Evidence base for every modeled behavior:
- **Stage 0 live capture** (2026-11-03): 48-byte DCP descriptor, byte-exact,
  `documentation/evidence/stage0-tcd3070/mailbox-descriptor-raw.txt`
- **ALSA reference** (LGPL, facts only):
  `references/snd-firewire-ctl-services/protocols/dice/src/focusrite/spro40d3.rs`
- **Firmware analysis**: `tools/firmware_analysis/Pro40d3-DCP.md` (transport FSM
  0x1B688, async request handler 0x1BAB4, unknown-category status 4)

---

## 1. Changed files (exact)

Production (driver tree; compiled **only** into host-test targets — no
`project.yml`/pbxproj changes, so the shipping driver is untouched):

| File | Kind | Contents |
|---|---|---|
| `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3MailboxTypes.hpp` | new | DCP descriptor codec, 16-byte message framing, command serializers (set_volume, get_meter, get/set_control, write_sw_notice, routing low/high), route-entry encoding, control-store offsets, output-volume inverted packing, `ValueProvenance` (Unknown/Observed/Cached) + control-state mirror |
| `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3MailboxTypes.cpp` | new | Routing table serializers (byte offsets per reference) |
| `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3MailboxEngine.hpp` | new, reworked by review | `ISPro40D3Transport` seam, `MailboxError`, serialized engine API; pump model (§2.2a) |
| `ASFWDriver/Audio/Protocols/DICE/Focusrite/SPro40D3MailboxEngine.cpp` | new, reworked by review | Engine: ownership lifecycle, command lifecycle, bounded waits, counter management + reset ritual, notification attribution, provenance store, uncertain-write invalidation |

Test support / tests:

| File | Kind | Contents |
|---|---|---|
| `tests/support/SPro40D3SimulatedDevice.hpp` | new, reworked by review | Deterministic TCD3070: real Stage 0 descriptor bytes (owner field overlaid from live storage, quadlet-0-low word order), command semantics, fault knobs (response status, dropped/queued notifications, counter desync, forced owner, owner race, bus reset, write failures, header-echo corruption, notification-length overrides), request-lifetime tripwire, invariant observers (response-area read counters, reset-frame sighting) |
| `tests/devices/SPro40D3MailboxCodecTests.cpp` | new, reworked by review round 3 | 14 tests: codec validation from independently derived layouts (§2.4a) |
| `tests/devices/SPro40D3MailboxEngineTests.cpp` | new, expanded by review | 50 tests: engine behavior vs the sim |
| `tests/devices/CMakeLists.txt` | modified | Two `add_device_test` targets wiring the new sources |

## 2. Architecture

### 2.0 Review corrections applied

#### Round 4 (2026-11-02): one finding, corrected

nick's round 3 review accepted both round-2 corrections (foreign-owner CAS
takeover prevention, routing source-domain validation) and required one
final correction before PR approval:

| # | Review finding | Correction |
|---|---|---|
| Stale registration state after detected ownership loss | `RegisterNotificationAddress` returned `kOwnershipConflict` on a registered-vs-observed mismatch while RETAINING `kRegistered` — subsequent `Send*()` passed their precondition despite known ownership loss | Confirmed mismatch now invalidates the registration state immediately (§2.3a): `kOwnershipConflict` + `ForeignOwner()` preserved for diagnostics + `registration_ = kUnregistered`. Subsequent commands fail `kNotRegistered` with zero transport ops and no callback. No reacquisition CAS, no release CAS — re-claiming is an explicit caller decision. In-flight review (§2.3a): detection can never coincide with an in-flight command (`Register` returns `kBusy` while `active_`/`inFlight_`), so no cancellation path exists or is needed |

#### Round 3 (2026-11-02): two findings, corrected

nick's round 2 review returned TWO CORRECTIONS REQUIRED BEFORE PR. The
architecture (pump, bounded reads, exactly-once, sim) was accepted and was
not redesigned. Corrections implemented:

| # | Review finding | Correction |
|---|---|---|
| A (P0) | `RegisterNotificationAddress` used the fresh descriptor's notification address as the CAS expected operand — CAS(foreign→ours) succeeds against a steady foreign owner and silently steals the mailbox | Three-state ownership classification (§2.3a): the CAS expected operand is the CONSTANT `kDcpOwnerNone` sentinel, never a device-read value. Foreign-owned (including a field that merely equals our intended address without engine-state corroboration) → `kOwnershipConflict` + `ForeignOwner()`, ZERO CAS operations. Self-ownership requires THIS engine's prior successful CAS plus device corroboration; idempotent re-registration preserves both. Races between read and CAS fail atomically and report the racer |
| A (P0) | The sim's descriptor read never reflected the live owner storage (the hazard was invisible to tests); the `foreignOwner` knob was decoupled from the CAS | Sim descriptor read overlays the CURRENT owner into bytes [8..16) (quadlet-0-low word order, Stage 0 capture); `foreignOwner` knob removed; `ForceOwner()` models any pre-existing claimant; one-shot `ArmOwnerRace()` models the read→CAS race window with the racer's claim committing |
| B (P1) | `SendSetRouting`/`RouteSourceId`: no upper bound on the mixer branch — source IDs past the device domain overflow the route entry's 12-bit source field and contaminate the destination bits | `IsValidRouteSource` / `kMaxSymbolicRouteSource = 54` (16 mix buses, spro40d3.rs:29) in the types header; `SendSetRouting` validates all 42 entries BEFORE serialization → `kInvalidArgument`, zero transport ops, no callback. Boundary tests: valid max 54, first invalid 55, large unsigned values including the `(source << 12)` wrap threshold where a huge id masquerades as a small source class |
| Cleanup | Mechanically extracted GPL-derived byte arrays (`SPro40D3AlsaVectors.inc`) | File deleted; the 5 vector-matching codec tests replaced with 4 independently derived tests (values chosen here, layouts from the documented protocol model); independently derived tests retained (§2.4a) |
| Cleanup | Uncertain-transaction recovery documented as verified | §5.7 now marks recovery (power-cycle / bus reset) as hardware-unverified |
| Cleanup | Notification-address lifecycle for production integration undocumented | §2.5b: lifecycle requirements for Stage 2 |

#### Round 2 (2026-11-02): 11 findings, corrected

nick's Stage 1 review returned CHANGES REQUIRED. Corrections implemented:

| # | Review finding | Correction |
|---|---|---|
| P1 | Timeout left a WRITE's cached value standing (ambiguous completion) | `CompleteFailure` marks a write's targets `Unknown` for every failure whose outcome is uncertain (§2.2b); no auto-replay anywhere |
| P1 | Re-entrancy was undefined (sync notifications during `WriteBlock`, callbacks re-launching) | Pump model (§2.2a): re-entrant calls queue; `Pump()` drains iteratively; callbacks may launch/reset but never destroy the engine |
| P1 | Late/stale notification attribution was loose | Deadline checked at ingress (a late frame is never read); response counter matched against the in-flight command and a 4-entry recent-counter ring (§2.2c) |
| P2 | Request buffer lifetime was unspecified | Request storage is engine-owned for the whole in-flight call (documented at the seam); the sim re-reads the request after the notification sink returns and asserts it did not move or mutate (ASan-verified) |
| P2 | Bounded reads not proven | Response-area reads are structurally paired (notification-gated) and bounds-checked before issue: `rlen+16 > responseAreaSize` → malformed (keep waiting); `rlen ≠ expected` → `kResponseShapeMismatch`, **no read** |
| P2 | Chained-step failures reported as the primary result | `kPartialCompletion` (§2.2b): the primary step was confirmed, the chain did not finish |
| P3 | Release (unregister) CAS unverified on hardware | `Options::allowOwnershipRelease{false}` default; `Unregister` returns `kReleaseDisabled` unless explicitly enabled |
| P3 | Post-bus-reset descriptor drift unmodeled | `RegisterNotificationAddress` re-reads and re-validates the descriptor (addresses, sizes, version) after every reset; mismatch → `kBadDescriptor` |
| P3 | Input ranges unchecked at the engine boundary | All `Send*` validate ranges (`kInvalidArgument`, nothing sent) |
| P3 | ALSA vectors provenance mislabeled (LGPL) | Reference is GPL-3.0-or-later (repo `COPYING`); provenance block rewritten (§2.4a) |
| P3 | Exactly-once completion not asserted | Every failure-mode test asserts the completion fired exactly once (counting captures); acceptance errors assert it never fired |

### 2.1 Layering

```
SPro40D3 family driver (Stage 2+, NOT built here)
   │  serializes callers; owns the real clock
   ▼
SPro40D3MailboxEngine        (serialized state machine, no I/O)
   │  ISPro40D3Transport: WriteBlock / ReadBlock / CompareSwap64
   │  (absolute 1394 addresses; std::expected<_, IOReturn>)
   ▼
DiceDeviceIo adapter         (Stage 2: adds DICE base, forwards)
   ▼
ProtocolRegisterIO → dext transactions
```

- **Types layer** (`SPro40D3MailboxTypes.*`) is pure byte math: no transport,
  no DriverKit, no bus concepts. Every wire fact carries a citation.
- **Engine** owns all protocol state: registration, counter, in-flight command,
  provenance mirror. It never performs I/O outside the transport seam, and it
  reads the response area (+0x30) **only** paired with a completed notification
  — the Stage 0 §7a rule is structural, not convention.
- **Sim** (`tests/support/`) implements the transport and pushes notifications
  through a sink the test wires to `OnNotification`. It knows nothing about the
  engine. Fault knobs script every failure interleaving.

### 2.2 Command lifecycle (per command)

1. `Send*` validates registration + serialization (kBusy/kNotRegistered).
   **Acceptance contract**: `Send*` returns acceptance errors only
   (`kBusy`/`kNotRegistered`/`kInvalidArgument`) — the completion callback is
   NOT invoked for these. Once a command is accepted (return value has a
   value), its callback fires **exactly once**: success or one of the errors
   below. This is asserted by every failure-mode test.
2. Engine stamps the 16-byte header (counter, payload length) — serializers
   wrote only opcode+payload, so counter management lives in exactly one place.
3. `WriteBlock(writeAddress, frame)` — request area only.
4. Await notification (device block-write to our registered host address).
   Deadline = `now + responseTimeoutMs` (default 200 ms, per reference call
   sites); `Poll(now)` converts expiry into `kTimeout`. No hidden timers.
5. On notification: decode rlen/rstatus.
   - rstatus ≠ 0 and not yet retried → **counter-reset ritual** (reference
     behavior): send the 16-byte reset frame (0x80…), counter back to 0,
     retry the same command once. Second failure → `kResponseStatus`.
   - rstatus = 0 → **the one paired read**: `ReadBlock(readAddress, rlen+16)`.
6. Validate the echoed counter (`kCounterMismatch` otherwise), decode per
   command kind, update the provenance mirror, fire the completion.
7. Chained commands model the reference exactly: `set_control` →
   `write_sw_notice`; `set_routing` → low-rate table then high-rate table.

### 2.2a Re-entrancy: the pump

The engine has no threads and no locks; it is a serialized state machine that
can be *entered* re-entrantly from three directions: the transport delivering
a notification synchronously inside `WriteBlock` (the DICE notification
router's path), a completion callback that immediately launches the next
command or triggers a bus reset, and `Poll`/`OnBusReset` arriving from a timer
while a callback is on the stack. A re-entrant entry never runs inline; it
**queues** (notifications, launches, poll, bus-reset each have a queue slot)
and returns. `Pump()` — called on exit from every top-level entry — drains
the queues iteratively in a fixed order (bus reset → poll → notifications →
launches) until all are empty. Consequences:

- A completion callback may freely call `Send*` or `OnBusReset`; its work
  runs after the current stack unwinds, in a defined order.
- A queued launch that finds the slot still occupied completes `kBusy`
  (exactly once); one that finds no registration completes `kBusReset`.
- Callbacks must not destroy the engine or throw (`Pump` is `noexcept`;
  throwing is a `std::terminate` by design — the engine is not recoverable
  from an exception mid-transaction).

### 2.2b Failure semantics (what each error means for state)

| Completion | When | Provenance effect on a WRITE target |
|---|---|---|
| success | every chained step confirmed | `Cached` (writes) / `Observed` (reads) |
| `kTransport` | initial write or ritual write failed | none — never landed |
| `kPartialCompletion` | a chained step failed after the primary was confirmed | primary stays `Cached`; routing tables → `Unknown` (mixed state) |
| `kTimeout` | deadline passed (checked at ingress and in `Poll`) | **`Unknown`** — may or may not have landed |
| `kResponseShapeMismatch` | notification's rlen ≠ expected (checked **before** any read) | **`Unknown`** |
| `kMalformedResponse` | header echo inconsistent (after the paired read) | **`Unknown`** |
| `kCounterMismatch` | echo matches no live/recent command (after the paired read) | **`Unknown`** |
| `kShortResponse` | paired read returned fewer bytes | **`Unknown`** |
| `kTransport` (paired read) | the confirmation was unreadable | **`Unknown`** |
| `kResponseStatus` | device returned status ≠ 0 twice (after the reset ritual) | none — the device explicitly rejected the command |
| `kBusReset` | reset while in flight | **all** targets `Unknown` (`OnBusReset`) |
| `kBusy`/`kNotRegistered`/`kInvalidArgument` | acceptance errors | none — callback never fires |

No code path auto-replays a write whose fate is unknown; recovery is always
the caller's decision, and the provenance store forces a re-read before the
value is trusted again.

### 2.2c Notification attribution

A response is attributed to a command by the **counter echoed in its header**,
matched against the in-flight command first, then a 4-entry ring of recently
completed counters. A frame matching neither is stale and ignored (counted,
logged, never read). The deadline is enforced **at ingress**: a notification
arriving at or after the deadline completes `kTimeout` without touching the
response area — a late response is never read, so a late-but-valid frame can
never be mistaken for the next command's answer (the response area may have
been reused by then). Malformed frames (< 12 bytes) are dropped and the
command keeps waiting.

### 2.3 Ownership / notification registration (Stage 0 §7a)

The descriptor's +0x08 field is both the ownership CAS word and the
notification address. `RegisterNotificationAddress` claims it only from the
unowned state (§2.3a); a foreign owner yields `kOwnershipConflict` with the
observed owner exposed.
`OnBusReset` drops registration, zeroes the counter, fails any in-flight
command with `kBusReset`, and invalidates every Cached control value
(the hardware never confirmed them).

After a reset, `RegisterNotificationAddress` **re-reads the descriptor** and
re-validates it against the Stage 0 capture (notification/write/read
addresses, response-area and max-request sizes, version) before CASing; a
descriptor that changed across the reset (different unit, firmware swap)
yields `kBadDescriptor` and no CAS is attempted. The release CAS
(ours → none) is **disabled by default** (`Options::allowOwnershipRelease`):
`Unregister` returns `kReleaseDisabled` unless the integrator explicitly
enables it, because the reference never releases and firmware acceptance of
the release CAS is unverified (§5.2).

### 2.3a Ownership classification (round 3, finding A)

The round 2 engine CASed `fresh.notificationAddress → hostAddress` — using a
value freshly READ from the device as the CAS expected operand. That design
has a P0 hazard: against a steady foreign owner, CAS(foreign→ours) SUCCEEDS
and silently steals the mailbox. (The old sim hid this: its descriptor read
never reflected the live owner storage, so no test could see it.)

Register now classifies from the fresh owner field **plus the engine's own
registration state**, and only ever CASes with the CONSTANT `kDcpOwnerNone`
sentinel as the expected operand:

| State | Fresh field | Result | CAS ops |
|---|---|---|---|
| registered | == our address | idempotent success (state + device corroborate) | 0 |
| registered | ≠ our address | `kOwnershipConflict`, `ForeignOwner()` = observed (covers stolen, and cleared-behind-our-back where ForeignOwner may be `kDcpOwnerNone`). **Confirmed ownership loss INVALIDATES the registration state immediately (round 4):** subsequent `Send*()` fail `kNotRegistered` with zero transport ops — a stale `kRegistered` must never let a command pass its precondition against a mailbox we know we no longer own. No reacquisition CAS, no release CAS; re-claiming is an explicit caller decision | 0 |
| unregistered | ≠ none (incl. == our intended address) | `kOwnershipConflict`, `ForeignOwner()` = observed. Equality without engine-state corroboration is NOT self-ownership: a stale value from a previous process, garbage, and a foreign claimant at that address are indistinguishable. Recovery is the operator's (power-cycle, or a bus reset IF the firmware drops ownership — unverified, §5.7) | 0 |
| unregistered | == none | CAS(`kDcpOwnerNone` → ours); a racer claiming in the read→CAS window fails the CAS atomically and is reported | 1 |

Re-registration to a different address while registered is refused
(`kOwnershipConflict`; release first — the release CAS is gated, §2.3).
Descriptor revalidation (§2.3) runs before classification in all cases.

**In-flight interaction (round 4 review, requirement 7).** Ownership-loss
detection happens only inside `Register`, which returns `kBusy` whenever a
command is `active_` or queued behind one (`inFlight_`) — so detection can
never coincide with an in-flight operation and no cancellation path is
needed. If ownership is lost *while* a command is in flight, the engine
cannot observe it (the DCP mailbox has no ownership-change notification);
the command then completes through the normal paths, and if the device
rejects or ignores it the existing uncertain-completion machinery applies
(explicit reject → `kResponseStatus`, no invalidation; timeout/malformed →
write-target invalidation per §2.2b). The control-state mirror's `Cached`
values are NOT invalidated on ownership loss — a foreign owner may have
changed device state, but the engine cannot know when; re-reads refresh
provenance. Noted as a Stage 2 integration consideration, unchanged per the
round 4 instruction to keep the patch minimal.

### 2.4 Provenance

Family-local `ValueProvenance {Unknown, Observed, Cached}` per the Stage 1
brief. It maps onto `ASFW::AudioModel::StateOrigin` at integration time
(Observed→Observed, Cached→Cached, Unknown→absent); the AudioModel enum has
`Derived` where this family needs `Unknown`, so the family keeps its own
projection rather than bending shared infrastructure.

### 2.4a Reference-derived test vectors: license position

`references/snd-firewire-ctl-services` is licensed **GPL-3.0-or-later** (its
`COPYING` is the full GPLv3 text; no per-file SPDX, no Cargo `license` field).
Two kinds of content were taken from it, under different rationales — and
round 3 resolved the second kind (removal):

1. **Facts (cited, not copied):** wire layouts, offsets, opcodes, limits,
   and behavioral sequences. Each carries a file:line citation to
   `protocols/dice/src/focusrite/spro40d3.rs` in comments. Facts are not
   copyrightable subject matter, and the engine's expression is independent.
2. **Test vectors (mechanically extracted bytes): REMOVED (round 3).**
   `tests/devices/SPro40D3AlsaVectors.inc` held 8 arrays extracted by regex
   from the reference's in-file test module. Per the round 3 review
   direction (remove unless maintainer approval), the file is deleted and
   the 5 vector-matching codec tests were replaced with 4 independently
   derived tests (`SetVolumeFrameLayoutIndependentlyDerived`,
   `GetMeterFrameLayoutIndependentlyDerived`,
   `MeterDecodeHandlesNegativeValues`, `RoutingEntriesLandAtDocumentedOffsets`):
   values chosen in the test, expectations computed from the documented
   protocol model, no reference bytes involved. Independently derived tests
   (descriptor vs the live Stage 0 capture, route-class mapping, header
   round-trip, notification decode, response-header rejection) are
   unchanged. The simulated device still round-trips every command
   end-to-end.

### 2.5 Integration points (Stage 2, after hardware validation)

- **Transport adapter**: `DiceDeviceIo` already exposes
  `ReadBlock/WriteBlock/CompareSwap64` with `std::expected<_, IOReturn>` —
  the seam mirrors it 1:1; the adapter adds the DICE base offset and forwards.
- **Notification ingress**: the dext's host-address write handler calls
  `OnNotification(frame, now)` (same path `DiceNotificationRouter` uses for
  DICE section notifications).
- **Family driver**: a `SPro40D3Protocol` class following
  `SPro24DspProtocol.hpp`'s constructor pattern (bus ops, registry, router,
  wait clock), owning one engine instance and serializing its callers.
- **Clock**: every engine entry point takes `nowMs` explicitly; the driver
  passes the real clock, tests pass a deterministic one.

### 2.5b Notification-address lifecycle (production integration requirements)

The owner field doubles as the notification address, so its lifecycle is a
production-integration concern, not just an engine detail:

1. **Address allocation**: the notification address must be stable for the
   process lifetime and unique per host (it is the CAS identity — two hosts
   registering the same address are indistinguishable to the device and to
   each other). Derive it from a per-boot allocation, never a hard-coded
   constant.
2. **Startup ordering**: read the descriptor, classify ownership (§2.3a),
   and only then advertise the device as usable. A `kOwnershipConflict` at
   startup must surface to the user (another owner holds the mailbox), not
   be retried in a loop — the engine performs zero CAS against a known
   foreign owner, and no retry policy changes that.
3. **Crash recovery**: after a host crash the field may still name the dead
   host's address. A new process cannot distinguish "stale value that
   happens to equal my address" from "foreign claimant" — the engine
   conservatively reports `kOwnershipConflict` (§2.3a row 3). The integrator
   decides recovery: device power-cycle, or a bus reset **if** the firmware
   drops ownership on reset (hardware-unverified, §5.7). Until verified,
   surface the conflict.
4. **Tear-down**: release is gated behind `Options::allowOwnershipRelease`
   (§2.3) because firmware acceptance of the release CAS is unverified.
   If Stage 2 hardware verification confirms it, enable release on clean
   shutdown so the next owner does not inherit our stale address.
5. **Bus-reset handling**: on reset, re-read and revalidate the descriptor
   before re-registering (§2.3). If the field still names an address after
   the reset (retention), the engine refuses to re-register (§2.3a row 3
   applies post-reset too: `RegistrationAfterResetRefusesRetainedOwnership`).

## 3. Test coverage (Stage 1 brief → tests)

| Required scenario | Tests |
|---|---|
| Success paths | `SetVolumeCompletesAndCaches`, `SetControlChainsSoftwareNotice`, `GetControlObserves`, `MasterMeterLowRatePosition`, `MixerMeterHighRatePosition`, `RoutingSendsBothTablesThenCaches`, `DescriptorReadFromTransportMatchesStage0Capture` |
| Failure handling | `PersistentStatusFailsAfterOneRetry`, `CounterMismatchFailsTheCommand`, `CommandsRequireRegistration` |
| Bounded timeouts | `TimeoutIsBoundedAndFailsTheCommand` (deadline boundary at 199/200 ms; zero response-area reads while unsignalled) |
| Retries (reference reset ritual) | `NonzeroStatusTriggersResetRitualAndRetry` (transparent success, reset frame sighted, counter resync verified on the wire) |
| Stale notifications | `LateNotificationAfterTimeoutIsStale`, `UnsolicitedNotificationIsCountedAndIgnored` (malformed ingress dropped) |
| Bus resets | `BusResetInvalidatesRegistrationStateAndCounter`, `BusResetFailsInFlightCommand` |
| Ownership conflicts | `RegistrationConflictReportsForeignOwner`, `RegistrationClaimsOwnership`, `UnregisterReleasesOwnershipWhenEnabled` |
| Counter wraparound | `CounterWraparound` (0xFFFE→0xFFFF→0x0000 on the wire) |
| Provenance modeling | `ProvenanceTransitionsUnknownCachedObservedUnknown` |
| Response-area pairing invariant (§7a) | `ResponseAreaReadIsAlwaysPaired` (N commands → exactly N paired reads, 0 unpaired) |
| Byte-exact codecs | `SetVolumeFrameLayoutIndependentlyDerived`, `GetMeterFrameLayoutIndependentlyDerived`, `MeterDecodeHandlesNegativeValues`, `RoutingEntriesLandAtDocumentedOffsets` (entries at documented offsets via `EncodeRouteEntry`), descriptor tests vs the live Stage 0 capture |
| Serialization contract | `BusyRejectsConcurrentCommand` |
| **Review round 2 additions** | |
| Re-entrancy / pump | `SyncNotificationCompletesDuringWriteWithoutRequestMutation` (sync ingress inside `WriteBlock`; request bytes stable across the sink — the sim's tripwire), `ChainedCommandNotificationsDrainThroughThePump`, `CallbackCanLaunchTheNextCommand`, `CallbackCanTriggerBusReset`, `QueuedLaunchBehindInFlightCommandCompletesBusy` (kBusy exactly-once) |
| Notification attribution | `LateNotificationDuringNextCommandIsHarmless` (re-delivered frame for a completed command can never satisfy the next one), `StaleCounterResponseKeepsCommandWaiting` (out-of-band response with stale counter ignored; live counter completes) |
| Exactly-once completion | `InitialWriteFailureFiresExactlyOnceWithTransport` |
| Partial completion (§2.2b) | `NoticeChainTransportFailureIsPartialCompletion` (primary stays Cached), `RoutingHighTableFailureIsPartialCompletion` (router → Unknown) |
| Uncertain-write invalidation | `TimeoutInvalidatesWriteTargets` (volume/mute/routing: value kept, provenance Unknown) |
| Bounded reads (§7a) | `OversizedNotificationRejectedWithoutRead` (rlen+16 > 400 → malformed, no read), `WrongLengthNotificationRejectedWithoutRead` (rlen ≠ expected → kResponseShapeMismatch, no read), `ResponseOpcodeEchoMismatchIsMalformed`, `ResponseLengthEchoMismatchIsMalformed` (both after exactly one paired read), `DeadlineAtIngressNeverReadsLateResponse` (late frame → kTimeout, zero reads) |
| Malformed ingress | `MalformedFrameIgnoredCommandStillCompletes` |
| Input validation | `InputValidationRejectsOutOfRangeWithoutSending` (channel/mix/volume/rate/notice ranges; boundaries legal; zero requests for rejects) |
| Post-reset descriptor revalidation | `RegisterRevalidatesDescriptorAfterBusReset` (version drift → kBadDescriptor, no CAS; cleared → registers) |
| Release gating | `UnregisterIsDisabledByDefault` (kReleaseDisabled, engine still operational), `UnregisterReleasesOwnershipWhenEnabled` (opt-in; CAS ours→none verified) |
| **Review round 3 additions** | |
| Ownership classification (§2.3a, finding A) | `RegistrationConflictReportsForeignOwner` (reworked: pre-existing foreign owner → conflict, **zero CAS**, owner untouched), `OwnershipRaceBetweenReadAndCasReportsRacer` (one-shot race knob; racer's claim commits, ours fails atomically, exactly 1 CAS with expected = none sentinel), `StaleSelfAddressWithoutEngineStateIsForeign` (field equals our intended address, engine never registered → conflict, zero CAS), `IdempotentReRegistrationRequiresDeviceCorroboration` (state+device agree → idempotent, zero new CAS; stolen mid-registration → conflict, zero new CAS), `RegistrationAfterResetReclaimsWhenDeviceDropsOwnership`, `RegistrationAfterResetRefusesRetainedOwnership` (firmware keeps owner across reset → conservative refusal, zero new CAS) |
| Routing source domain (finding B) | `RoutingSourceDomainIsValidated` (boundary: all 42 entries at max valid 54 complete; first invalid 55, 0x1000, 2²⁰, the (source << 12) wrap threshold, 0xFFFFFFFF — each array checked independently → kInvalidArgument, zero transport ops beyond the accepted chain, zero callbacks) |
| Sim honesty (finding A) | Descriptor read overlays the live owner (quadlet-0-low word order per the Stage 0 capture); `ForceOwner`/`ArmOwnerRace` replace the decoupled `foreignOwner` knob |
| Codec vectors removed (cleanup) | 5 GPL-derived vector tests replaced by 4 independently derived tests (§2.4a) |
| **Review round 4 additions** | |
| Registration invalidation on ownership loss (§2.3a) | `OwnershipLossInvalidatesRegistrationForSubsequentCommands` (register → owner stolen externally → Register detects conflict → subsequent `SendSetVolume`/`SendSetRouting` fail `kNotRegistered`, callback never fired, **zero command writes**, no reacquisition/release CAS), `ClearedOwnershipBehindEngineBackInvalidatesRegistration` (owner field cleared to none while registered → conflict with `ForeignOwner()` = `kDcpOwnerNone`, registration invalidated, zero writes; explicit caller-driven re-claim then succeeds via the normal claim path), `IdempotentReRegistrationRequiresDeviceCorroboration` (extended: post-conflict state is `kUnregistered` and a subsequent command fails `kNotRegistered`, zero writes) |

## 4. Results

New tests: **66/66 pass** (`SPro40D3MailboxCodecTests` 14 — 10 retained
independently derived + 4 new; `SPro40D3MailboxEngineTests` 52 — 44
round-2 + 6 round-3 + 2 round-4 ownership-loss invalidation tests).
Both targets also pass clean under **AddressSanitizer + UBSan**
(`./build.sh --test-only --asan`), including the sync-notification path
where the simulated device re-reads the request bytes after the sink returns
(use-after-free / request-mutation tripwire).

Full-suite regression: see §4.1.

### 4.1 Regression comparison

`./build.sh --test-only` (Release config, worktree `fw203-stage1-mailbox`,
review round 4):

- **3984/3984 tests passed, 0 failed** (includes the 66 SPro40D3 tests;
  round 4 added 2 ownership-loss invalidation engine tests, all passing,
  zero regressions elsewhere).
- 6 pre-existing skips (Linux fixture-dependent SelfID/AT-descriptor/CRC
  reference tests) — unrelated to this change, skipped on main as well.
- The full suite passed before this branch on the same machine (Stage 0
  session, 2026-11-02); the cumulative delta of this branch is +66 passing,
  −0 failing.
- `./build.sh --test-only --asan`: full suite passes clean under
  AddressSanitizer + UBSan (engine 52/52, codec 14/14, zero sanitizer
  reports).

## 5. Unverified hardware assumptions

Everything below is modeled from the ALSA reference / firmware analysis and
**must be confirmed on hardware before the engine is trusted against it**
(Stage 2). The sim encodes these assumptions; tests prove the engine handles
them, not that the device behaves this way.

1. **Meter value encoding.** `get_meter` returns BE i32 per channel (reference
   `deserialize_meter`), but the scale (dB fixed-point? peak/RMS?) is unknown.
   The sim returns documented synthetic values. First live `get_meter` output
   must be captured before any UI consumes it.
2. **`UnregisterNotificationAddress` (CAS ours→none).** The ALSA reference
   never unregisters (its drop path just frees the response region). Whether
   the TCD3070 firmware accepts the release CAS — or ignores it — is unverified.
   The engine therefore ships with the release CAS **disabled by default**
   (`Options::allowOwnershipRelease{false}` → `kReleaseDisabled`); enabling
   it is a Stage 2 hardware decision.
3. **Device-side counter semantics.** The reference never validates a counter
   device-side; the engine assumes the device echoes the request counter in
   the response header. If the device tracks its own counter, first live
   transaction may fail `kCounterMismatch` — that is diagnostic, not a bug.
4. **Notification frame shape.** Modeled as ≥12 bytes with rlen at [6..8] and
   rstatus at [8..12] (reference `resp.connect_requested`). Total frame
   length and any additional fields are unverified — Stage 0 captured no
   notification (no command was ever sent). First live notification must be
   hex-logged in full.
5. **Reset-ritual acceptance.** The ritual (reset frame + counter 0 + one
   retry) mirrors `send_command`; whether the real firmware requires exactly
   counter 0 on the retry, or any counter, is unverified.
6. **rstatus = 4 for unknown categories.** From firmware analysis
   (`Pro40d3-DCP.md`), not hardware-observed.
7. **Ownership drop on bus reset.** Inferred from TCAT re-claiming ownership
   every generation (see `SimulatedDiceDevice.hpp` rationale); not observed
   on TCD3070. **Recovery from a foreign or retained owner is likewise
   hardware-unverified**: when the engine refuses registration
   (§2.3a), the operator's recovery options are a device power-cycle or a
   bus reset — and the bus-reset option only works if this very assumption
   (the firmware drops the owner on reset) holds. Do not script automatic
   recovery until first hardware contact confirms the drop.
8. **Descriptor +0x10 field (= 2).** Captured, undecoded, unused.
9. **sw_notice values (1=volumes, 2=mute/dim, 3=pad, 4=adat).** From reference
   call sites; device-side effect unverified.
10. **One response read per notification.** The engine reads exactly once per
    notification (sim enforces). Whether the firmware re-arms or clears the
    response area differently is unverified.
11. **Rate boundary.** High-rate meter positions/tables are used for
    `rate > 48000` (reference comparison); 88.2/96/176.4/192 kHz all take the
    high-rate table. The device's actual rate-switch behavior is unverified.
12. **Mixer crosspoint volume scale.** `set_volume` writes a 16-bit value;
    the reference passes 0..0x7fff-ish values from its mixer model. Absolute
    scale/clip behavior on hardware unknown. (The inverted 0..255 scale is the
    *control-store output volume* path, a different mechanism.)

## 6. Proposed safety checklist — first live mailbox transaction (Stage 2 gate)

1. **Preconditions.** Stage 0 evidence reviewed; device powered, idle (no
   streaming); playback muted at the monitor; ASFW Release app attached;
   hex-logging armed for every frame both directions.
2. **Re-read the descriptor.** 48-byte block read at 0xFFFFE0400000 must match
   the Stage 0 capture byte-for-byte (magic, owner=none, addresses). Any
   mismatch → STOP.
3. **Owner check.** Owner field must be `0xFFFF000000000000` (none). A foreign
   owner → STOP (do not CAS over it).
4. **Register.** One CAS at base+8 (none → our host address). Read back the
   descriptor; owner must equal our address. Exactly one CAS attempt — no
   retry loop.
5. **First command: read-only.** `get_control(mute)` — no device state change.
   Verify: notification arrives ≤ 200 ms; the paired response read returns
   rlen+16; the echoed counter matches; rstatus = 0. Log the full notification
   frame (assumption 4).
6. **No speculative reads.** The engine structurally never reads +0x30 except
   paired; if hand-driving any test, honor the same rule (Stage 0 §7a).
7. **Second command: still read-only.** `get_meter` (master, low-rate position)
   to capture the real meter encoding (assumption 1) before anything consumes
   it.
8. **First write.** Only after 5–7 pass: `set_control(mute, 1)` + notice —
   mute ON as the safest first write; confirm by `get_control` readback
   (Observed) and by ear.
9. **Volume discipline.** Before any volume-affecting write (set_volume,
   output volumes), start from mute/minimum; raise in small steps. Headphone
   routing milestone (per project plan) before monitor-path routing.
10. **Bus reset watch.** If the bus generation changes at any point: STOP,
    re-read the descriptor, re-register, restart the checklist at step 5.
11. **Teardown.** Leave ownership as-is on the first run (release CAS is
    assumption 2 — do not exercise it on run one); power-cycle the device
    afterward and confirm the owner reads `none` on the next session.
12. **Abort criteria.** Any `kCounterMismatch`, `kTimeout` ×2 in a row, or
    unexpected rstatus → STOP and capture evidence. Do not improvise retries
    beyond the reference ritual the engine already implements.

## 7. STOP

Per the Stage 1 brief and the round 4 review instruction: host-only
implementation is validated, including the round 2, round 3 (findings A and
B, GPL vector removal), and round 4 (stale-registration invalidation)
review corrections; **no hardware execution, no commits, no PR.** This
worktree holds the corrected implementation, tests, and this report for
nick's final PR-approval review. The round-2 GPL-provenance decision is
resolved: the mechanically extracted vectors were removed per the round 3
direction (§2.4a); if nick prefers byte-exact cross-validation against the
reference's vectors despite the GPL derivation, they can be restored from
the round 2 evidence zip with maintainer approval.
