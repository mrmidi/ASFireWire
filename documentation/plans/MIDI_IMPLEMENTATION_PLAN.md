# MIDI over AMDTP: implementation plan

Status: implementation plan, 2026-09-13, against ASFW `dce2fec4`. No hardware
action is authorized by this document. Companion to
[MIDI_WIRING.md](../MIDI_WIRING.md), which remains the design and evidence
record; this document is the build order.

Scope is the DICE Saffire first milestone at its verified 48 kHz blocking
formation, stream 0 in each direction. BeBoB plug discovery, Oxford port policy,
secondary-stream MIDI and MIDI 2.0 translation stay out.

## 1. Why this is a separate document

`MIDI_WIRING.md` settles the wire and the references. What it does not do is
give a build order, and three properties of the work make that gap expensive:

- **Most of the cost is not MIDI.** The largest and riskiest work packages —
  extracting a shared stream session out of `ASFWAudioDriverZts.cpp`, bounding
  the TX fill horizon, wiring the offer doorbell — are changes to the audio TX
  path that MIDI merely *requires*. They need audio regression strategy, not
  MIDI acceptance criteria, and they can be built and validated before any MIDI
  byte exists.
- **Three unresolved blockers change the architecture, not just the code.**
  Entitlement grantability, the MIDI provider class, and whether MIDIServer
  schedules for a MIDIDriverKit driver each have an outcome that invalidates
  part of the design. A staged table cannot express "stop here until this
  answers".
- **The design doc deliberately leaves values unselected.** The fill window,
  the silence deadline, the ring capacities and the retry policy are marked "to
  be selected and measured". An implementation plan has to say who selects them,
  from what evidence, and what is blocked until they do.

`MIDI_WIRING.md` §12 is a roadmap with acceptance gates. This is the dependency
graph, the verified starting state, and the per-package contracts.

## 2. Starting state verified for this plan

Re-checked in the working tree at `dce2fec4`. Anything not listed here is
carried from `MIDI_WIRING.md` unverified in this pass — in particular the Linux
line numbers, the Focusrite addresses, the DICE register parse sites and the
MIDIDriverKit header line numbers. Recheck those before relying on them.

**Confirmed present and usable:**

- `MIDIDriverKit.framework` exists in both installed DriverKit SDKs
  (`DriverKit.sdk` and `DriverKit25.5.sdk`), with the full `.iig`/`.h` set
  including `IOUserMIDIEndpoint`. The framework is not a blocker.
- `ASFWDriver` sources are declared recursively over the `ASFWDriver` directory
  (`project.yml:99-115`). **A new `ASFWDriver/Midi/` tree needs no per-file
  registration** — this corrects `MIDI_WIRING.md` §12.1. What does need editing
  is `OTHER_LDFLAGS` (`project.yml:144`), the entitlements file, and
  `ASFWDriver/Info.plist`.
- `ASFWDriver/ASFWDriver.entitlements` carries `driverkit`, `transport.pci`,
  `transport.pci.bridge`, `family.audio`, `family.scsicontroller`. No MIDI
  family.
- `ASFWDriver/Info.plist` already has the nub-properties pattern in use twice
  (`ASFWAudioNubProperties`, `ASFWSBP2NubProperties`), so a third is mechanical
  *if* the provider question (§3.2) resolves that way.
- `Audio/Ports/` holds four seam interfaces today: `IAmdtpTxSlotProvider`,
  `ICycleTimeline`, `IDiagSink`, `ITxPcmSource`. `ITxPcmSource` already models
  the exact failure taxonomy the MIDI work needs to respect —
  `NotYetPublished`, `Expired`, `WrongEpoch`, `ConcurrentRewrite`,
  `InvalidRequest` — so §12.2(3) of the design doc is about *consuming* that
  enum correctly, not inventing one.
- `Audio/Core/AudioEndpointRuntime.hpp` and `AudioCoordinator` exist, so the
  shared-session target named in `MIDI_WIRING.md` §9.4 has a real home.
- Host test registration is `add_audio_test(<Name> <sources...>)` in
  `tests/audio/CMakeLists.txt` (pattern at `:117`).

**Confirmed still missing or still broken:**

- **The offer doorbell has a consumer but no producer.** `33414fc2` landed the
  coalescing doorbell, the split binding sweep, `ServiceOfferedPayloads` and its
  tests. But `IsochTxQueueControl::PublishOfferBatch`
  (`Isoch/Core/IsochTxQueue.hpp:663`) has **zero call sites**, and
  `IsochTransmitContext::ServiceLatePayloadOffers`
  (`Isoch/Transmit/IsochTransmitContext.cpp:887`) has **no callers**.
  Consequently `HasUnservicedOffers()` is permanently false and the claim path
  in `DoRefillOnce` (`IsochTransmitContext.cpp:513-515`) is unreachable in
  production. This is sharper than `MIDI_WIRING.md` §10.3, which reads as if the
  whole route were absent: half of it is built and tested, and the remaining
  half is small. See WP-5.
- **`AmdtpTxPacketizer` refill is deliberately byte-identical to the armed
  packet.** `AmdtpTxPacketizer.cpp:138-148` documents the invariant: the refill
  rebuilds the header from the armed packet's own DBC/SYT and re-lays defaults
  so that "a fill that loses the race to freeze is indistinguishable from never
  having happened." **Writing a MIDI byte into a candidate image breaks that
  invariant by construction** — a lost race is then distinguishable, because a
  byte was consumed from a queue. This is the code-level reason the
  reserve/commit/cancel protocol is mandatory rather than tidy, and it is the
  single most important constraint in the TX package.
- `WriteDataPacketDefaults` (`AmdtpTxPacketizer.cpp:177-192`) writes
  `defaultNonAudioSlotWord` into **every** slot, PCM included, and
  `WritePcmSnapshot` then overwrites the PCM ones. MIDI composition must run
  after `WritePcmSnapshot`, not merely after the defaults pass.
- `DiceTxStreamEngine::FillTransmitSlot` returns `NotFillable` when
  `!pcmSource_` and guards each packet with a one-shot `armedFilled_[index]`
  (`Audio/Engine/Direct/Tx/DiceTxStreamEngine.cpp:238-255`). Both behaviours are exactly as the design
  doc describes: no PCM client means no fills at all, and a packet filled once
  with silence can never later accept MIDI.
- The ZTS fill loop runs to `committedAfter` with no upper bound relative to
  transport progress (`ASFWAudioDriverZts.cpp:1294-1317`), and the sibling
  commit is sequential, not atomic (`:1304-1314`). Both confirmed.
- No `ASFWDriver/Midi/` tree, no MIDI source file anywhere in the dext.

## 3. Gate G0 — resolve before writing integration code

These are cheap, they are independent of each other, and each one's outcome
changes what gets built. None of them blocks WP-3.

### 3.1 Entitlement grantability — **not a development blocker (checked 2026-09-13)**

Checked on this machine, and the original framing was wrong. `ASFWDriver` is
**ad-hoc signed**: the built dext reports `Signature=adhoc`, `flags=0x2(adhoc)`,
`TeamIdentifier=not set`, and its entitlements are embedded straight from
`ASFWDriver.entitlements` with no provisioning profile involved. There is no
profile for `net.mrmidi.ASFW.ASFWDriver` on disk at all, and none of the 18
installed profiles mentions DriverKit. SIP is disabled, and the ad-hoc dext is
installed and `[activated enabled]` with team ID `-`.

So adding `com.apple.developer.driverkit.family.midi` to the entitlements file
will simply work locally. **Apple's grant gates a distributable, notarized build
— not development, and not WP-2.** Nothing in this plan is blocked on it.

Confirm grantability before promising a shipping build, and re-check if the
machine ever re-enables SIP: the ad-hoc path depends on it.

### 3.2 Provider class for the MIDI service

Whether `IOUserMIDIDriver` can match a custom `ASFWMidiNub` the way
`IOUserAudioDriver` matches `ASFWAudioNub`, or whether it must root at
`IOUserResources` as Apple's sample does. Settle it by building the skeleton in
WP-2 both ways if necessary; it is a matching experiment, not a design
argument. Outcome selects between the nub shape and the fallback in
`MIDI_WIRING.md` §9.1, and therefore decides how discovery reaches the MIDI
service. It does **not** affect ring ownership or any wire behaviour, so WP-4
onward can proceed under either answer.

### 3.3 Does MIDIServer schedule for a MIDIDriverKit driver?

The probe in `MIDI_WIRING.md` §13.1. Run it as part of WP-2, on real hardware-
free virtual endpoints. Record the OS build, the API used, the property values,
any utility words observed, and the full latency distribution alongside the
verdict.

- Deadlines preserved → the bytes-only ring in WP-4 is viable as designed.
- Early or coalesced callbacks → the provisional design in §7.3 failed. **Do not
  invent a deadline API to paper over it.** Stop and re-decide the TX contract
  before WP-8; WP-7 (RX) is unaffected and can continue.

Treat a single delayed callback as no evidence. The probe must show that a batch
of independently timestamped events preserves its spacing.

## 4. Work packages

Each package states its goal, the files it owns, the contract it must satisfy,
its tests, and what "done" means. Dependencies are in §5.

### WP-1 — Capability projection — **landed for DICE, hardware-unverified**

**Goal.** A `MidiEndpointCapabilities` record projected from the DICE per-stream
runtime caps that are already parsed, published on whichever nub §3.2 selects.

**Files.** `Audio/Protocols/DICE/Core/DICEDuplexBringupController.cpp` (consume
existing values only — no new register semantics),
`Audio/Families/Common/CommonProfileBuilder.cpp`, `Audio/DriverKit/Config/`,
and the new capability header.

**Contract.** Explicit host-facing direction names at the seam: DICE `tx` is
device→host and becomes a CoreMIDI *source*; ASFW's TX packetizer is
host→device and serves a CoreMIDI *destination*. Carry port count, stream index
(0 for this milestone), MIDI slot index, DBC alignment flag, stable identity
derived from device GUID + direction + port index, and the stream epoch. Reject
rather than normalize: counts > 8, more than one MPX slot, `midiSlotIndex >=
dbs`, or a MIDI slot overlapping a PCM slot make the formation MIDI-unsupported
while leaving audio capability publication untouched. Do not reuse
`DICE::StreamConfig::TotalMidiPorts` / `ActiveMidiPorts` for endpoint
enumeration — they aggregate across streams, which contradicts the stream-0-only
routing policy.

**Tests.** Host fixtures: zero counts, asymmetric in/out, nonzero counts on a
non-zero stream, bad DBS, slot overlap, identity stability across a synthetic
reset. No guessed 1/1 publication anywhere.

**Done when** the projection is exercised by host tests and the real Saffire
model, GUID and 48 kHz stream table are recorded in a report, including which
physical jack each direction represents.

**What landed.** `Midi/Capabilities/MidiEndpointCapabilities.{hpp,cpp}`, a pure
projection from `AudioStreamRuntimeCaps` -- which already carries per-stream
`midiPorts`, `pcmChannels` and `am824Slots` per direction, populated by
`DICEDuplexBringupController`. No DICE register semantics were touched. Every
name is host-facing (`deviceToHost` / `hostToDevice`); tx/rx appears nowhere,
because it inverts across the seam.

Rejections are per-direction and never normalise: over 8 ports, more than one
MPX slot, MIDI on a stream this milestone does not route, and inconsistent DBS.
A rejected direction leaves the other usable and leaves audio publication
untouched.

One simplification worth recording. With a trailing MIDI slot the PCM-overlap
check *is* the DBS check: the slot index is `pcmChannels`, so overlap means
`dbs <= pcmChannels` and out-of-range means `dbs < pcmChannels + midiSlots`, and
both are `dbs != pcmChannels + midiSlots`. A device reporting 8 PCM, 1 MIDI port
and DBS 8 is saying MIDI shares a PCM slot. `kSlotOutOfRange` and
`kSlotOverlapsPcm` remain in the enum but are unreachable today; whoever adds
BridgeCo position discovery must make them live again rather than assume they
already guard anything.

`MidiPortKey(guid, direction, portIndex)` mixes the GUID before folding in
direction and port so two units from one production run cannot alias; it
deliberately excludes the stream epoch, since anything that changes across a
restart would defeat the point.

**Still open.** 17 host tests, all four mutations caught. But every fixture is
synthetic: the real Saffire model, GUID and 48 kHz stream table are still
unrecorded, and which physical jack each direction drives is still unproven.
That is the remaining half of this package's acceptance and it needs hardware.

### WP-2 — MIDI service skeleton and host feasibility — **landed, runtime-unverified**

**Goal.** A loadable `ASFWMIDIDriver` publishing one device with one source and
one destination wired in UMP loopback, plus the two probes from G0.

**Files.** New `ASFWDriver/Midi/DriverKit/` (`ASFWMIDIDriver.iig`, and
`ASFWMidiNub.iig` if §3.2 selects the nub shape); `ASFWDriver/Info.plist` for
the personality and `IOUserMIDIDriverUserClientProperties`;
`ASFWDriver/ASFWDriver.entitlements` for `family.midi`; `project.yml:144` for
`-framework MIDIDriverKit`.

**Contract.** Follow the sample's idioms, not the headers' apparent surface:
`OSTypeAlloc` + `init` for subclasses rather than `Create`; entities built in
`init`; `NewUserClient` forwards `kIOUserMIDIDriverUserClientType` to super;
`Driver::StartIO` calls super first then each device, `StopIO` mirrors it
device-first. Publish `MIDIProtocol_1_0` as a deliberate choice, not the
sample's `_2_0`. Leave `AdvanceScheduleTimeMuSec` unset. Check every return
value the sample leaves unchecked. Do not copy the sample's swapped
`kAddPort`/`kRemovePort` FourCC values.

**Tests.** Signed build and install; source and destination appear exactly once
in CoreMIDI; add/remove entity and reconfiguration are safe in both the running
and stopped branches.

**Done when** the loopback works, §3.2 is answered with a working match, and
§3.3 has a recorded verdict with its data. Also record process placement of all
three services and the actual callback serialization observed — `MIDIIOBlock`
runs on the framework's RT thread and that must be confirmed, not assumed from
the class comment.

**What landed.** `Midi/DriverKit/` holds `ASFWMidiNub`, `ASFWMidiDevice`
(an `IOUserMIDIDevice` subclass) and `ASFWMIDIDriver` (an `IOUserMIDIDriver`
subclass); `Midi/Core/` holds the nub property contract and the publisher.
`ASFWMIDIDriverService` matches `ASFWNubType == "MIDI"`, the exact mirror of
`ASFWAudioDriverService` matching `"Audio"` — which is the §3.2 answer in
design, still pending in runtime. `family.midi` is on the entitlements file,
`-framework MIDIDriverKit` on `project.yml:144`, and the dext links it.

Capability crosses to the MIDI service as registry properties set before the nub
starts, the way `AudioNubPublisher` already populates `ASFWAudioNub`. The service
therefore reads its own shape from its provider in `Start()` and never calls back
across the seam to discover it. `AudioCoordinator` publishes both nubs from the
same resolved profile; a MIDI publication failure deliberately does **not** unwind
the audio endpoint.

Two deliberate divergences from Apple's sample: `MIDIProtocol_1_0`, because the
wire carries a MIDI 1.0 byte stream and claiming 2.0 would promise resolution the
hardware cannot carry; and an endpoint reporting no usable MIDI publishes no nub
at all rather than an empty CoreMIDI device.

A `Midi` log category was added (ID 24), including the positional name array in
`ASFW/DriverConnector+LogRing.swift` — that array is indexed by the C++ enum
value, so adding the name there is half the wire contract, not cosmetic.

**Still open, and it is most of the acceptance.** Nothing here has run. The nub
publishes on endpoint resolution, so the loopback, the nub match (§3.2), the
scheduling probe (§3.3), process placement and callback serialization all need
hardware. `InstallLoopbackForBringUp` is scaffolding and must be deleted in
WP-4 — a loopback left in place makes a broken wire path look like a working
one.

### WP-3 — UMP ↔ MIDI 1.0 byte converter

**Goal.** A pure, bounded, per-port, stateful converter in both directions.

**Files.** New `ASFWDriver/Midi/Ump/`. No DriverKit dependency, no allocation,
no logging.

**Contract.** RX (bytes → UMP): running status tracking; Channel Voice buffered
until complete then emitted as one MT 0x2 word; System Real Time (0xF8–0xFF)
emitted immediately as MT 0x1 without disturbing a partial message; SysEx
accumulated into MT 0x3 packets of ≤ 6 bytes with correct
Complete/Start/Continue/End; accepts a run of up to 3 bytes per call because one
AM824 quadlet can carry three. TX (UMP → bytes): the inverse with **no**
running-status compression; validate word count before reading a multi-word UMP;
reject unsupported message types and groups by a documented policy rather than
reinterpreting their remaining words. Define MT 0x0 utility handling explicitly
as ignore-with-count; do not imply JR scheduling support.

**Tests.** Independent byte and UMP vectors, not a Note On/Off loopback. Cover:
System Common lengths, running-status reset points, SysEx delimiter
insertion/removal, empty and exactly-six-byte SysEx, Real Time interleaved
mid-message, malformed and truncated UMP, and stream-loss recovery that must
invalidate partial parser state so bytes across a gap cannot form a fabricated
message.

**Done when** the vectors pass and the parser is proven to make no allocation
and take no lock on any path.

**This package has no dependencies and no blockers. It is where work starts.**

### WP-4 — Transport block and byte seam — **landed, runtime-unverified**

**Goal.** A `MidiTransportBlock` — 8 SPSC byte rings per direction — in an
`IOBufferMemoryDescriptor` owned by the core driver, handed across the nub(s)
the way `CopyDirectAudioMemory` already hands `outControlMemory`
(`Audio/DriverKit/ASFWAudioNub.iig:44-53`).

**Files.** New port interface under `Audio/Ports/`; nub method(s); allocation in
the core driver.

**Contract.** This is the FW-60 shape — three services, three queues, one buffer
— so the rules are not negotiable. Retain the descriptor and each service's
mapping until that service's queue is quiescent. Acquire/release cursor
publication with documented ordering. Bounded capacity with explicit overflow
behaviour: never overwrite unread bytes; a rejected TX enqueue must not publish
half a message; RX loss is counted and marks a discontinuity at the correct
queue position. SPSC is only valid with one producer and one consumer *per
port* — that must be established from the callback serialization recorded in
WP-2, not inferred from the service count. Push notifications coalesce
race-safely: drain, clear pending, recheck before sleeping. Teardown cancels or
drains queued notification blocks that capture service state.

The seam carries port-indexed bytes plus the reservation contract from
`MIDI_WIRING.md` §9.2 — begin, snapshot positions and staged limiter state,
commit once on successful publication, cancel on anything else, invalidate on
epoch change. A bare `PeekByte`/`ConsumeThrough` pair is insufficient and must
not be built.

**Note.** Do not add the transport-memory accessor to the audio nub if WP-6
makes the extracted session the sole TX consumer. Decide that before writing
the nub method, not after.

**Tests.** Host tests for cursor protocol, overflow, wrap, epoch invalidation,
and the reservation state machine including cancel-then-later-commit rejection.

**What landed.** `Midi/Transport/MidiTransportBlock.hpp` — eight SPSC byte rings
per direction, 1024 bytes each (about 328 ms of MIDI 1.0 backlog against a worst
recorded dispatch stall of 5.25 ms), plus `MidiTxReservation.hpp` for the
packet-local reserve/commit/cancel protocol. Both pure and host-tested.

Ring indices are free-running and only the byte access is masked, so "full" and
"empty" stay distinguishable. `TryWrite` is all-or-nothing: a truncated MIDI
message leaves the device desynchronised until the next status byte, so a
rejected message is strictly better. Overflow counts `droppedBytes` and marks a
discontinuity; the receive converter resets its parser at that mark so bytes
either side of a gap cannot form a message nobody sent.

The nub allocates and owns the `IOBufferMemoryDescriptor`; the MIDI service maps
it and retains both descriptor and mapping for its whole lifetime. `Stop` unbinds
the rings *before* the mapping is released, and `Quiesce` refuses further use
without unmapping, so a callback already running sees a quiesced block rather
than unmapped memory.

**The decision this package owed:** the transport accessor went on the MIDI nub
only, **not** the audio nub. RX extraction runs in the core driver, which reaches
the rings through the nub's LOCALONLY accessor, so nothing about WP-7 needs the
audio service to map anything. Whether the TX consumer maps it directly or
inherits it from the extracted session is now WP-6's to settle, which is where it
belongs.

**The loopback is gone**, replaced by the real path: each destination's I/O block
converts UMP to bytes on the RT thread and enqueues them; `DrainReceiveRings`
goes the other way on the service's work queue. Nothing feeds the wire ends yet
(WP-7/WP-8), so a device published today shows endpoints that stay silent — which
is honest, where a loopback would have made a dead wire path look alive.

**Still open.** 25 host tests and five mutations caught, but only the pure layer
has executed. The allocation, mapping, RT-thread callback and teardown ordering
are verified by inspection and an Xcode build only. SPSC remains a claim about
callers: it holds while each ring has exactly one producer and one consumer, and
the callback serialization that guarantees it is still unconfirmed (§WP-2).

### WP-5 — Offer notification producer wiring — **landed, hardware-unverified**

**Goal.** Close the remaining half of the offer/service gap: give
`PublishOfferBatch` a producer call site and `ServiceLatePayloadOffers` a
caller.

**Files.** `Isoch/Core/IsochTxQueue.hpp` (no change expected),
`Isoch/Transmit/IsochTransmitContext.cpp`, and the content-side publication
path that currently offers late payloads.

**Contract.** One notification per successful offer batch, delivered on the
transport owner queue, cancelled on reset and stop. Stays payload-opaque —
`Isoch/` must not learn that a payload contains MIDI. Any change to OHCI
servicing gets separately validated against the local Linux and Apple transport
references.

**This is an audio-only change with audio-only acceptance.** It is a MIDI
prerequisite because a bounded fill window is worthless if an offer waits for
the next completion interrupt, but it stands alone, ships alone, and is
validated by the existing audio tests plus the `IsochTxDmaRingTests` suite that
`33414fc2` already added. **Recommended second package after WP-3**, precisely
because it is independently valuable.

**Done when** offered payloads demonstrably cause a prompt transport wake, the
existing coalescing and lost-wakeup tests still pass, and audio regression is
clean.

**What landed.** The producer batch call sits at the end of the single fill loop
in `ASFWAudioDriverZts.cpp`'s `TxPreparationReady` -- the only `CommitFill` call
site in the tree -- tracking per-stream whether anything was actually offered,
and notifying each stream's own control block separately (primary is stream 0,
secondary stream 1, matching `AllocateTxIsochResources` and
`IsochService::TransmitContext`). Delivery is a new synchronous nub method,
`ASFWAudioNub::ServiceLatePayloadOffers(streamIndex)`, which resolves the core
`ServiceContext` and calls the transmit context on the core's own queue.

One addition beyond the "no change expected" note above: `IsochTxQueueControl`
gained `AbandonOfferNotification()` and an `offerNotifyDroppedCount`. Without
it, a failed cross-service hop leaves `offerNotifyPending` set forever, every
later batch coalesces into a notification nobody will deliver, and the doorbell
dies silently instead of degrading. Abandoning clears the flag *without* marking
the generation handled, so the offers stay outstanding for the next completion
interrupt -- exactly the pre-doorbell behaviour.

**Still open.** Host stubs cannot compile `ASFWAudioDriverZts.cpp` or
`ASFWAudioNub.cpp`, so the three new tests cover only the control-block
protocol; the cross-service path itself is verified by inspection and an Xcode
build, not by execution. Hardware must still confirm (a) that the wake actually
arrives before the frontier passes, and (b) what one synchronous cross-service
call per fill batch costs the audio preparation queue. Measure
`offerServiceCount`, `offerNotifyCoalescedCount`, `offerNotifyDroppedCount` and
`latePayloadLostPublicationCount` together: the qualification bar is that
offered payloads stop being discarded, not merely that the call happens.

### WP-6 — Shared stream session and leases

**Goal.** One endpoint stream-session owner holding the shared runtime and
content pump, with audio and MIDI as independent leases.

**Files.** `Audio/Core/AudioEndpointRuntime.hpp`, `Audio/Core/AudioCoordinator`,
a new session component under `Audio/Engine/`, and the callers in
`Audio/DriverKit/ASFWAudioDevice.cpp`, `ASFWAudioDriverLifecycle.cpp`,
`ASFWAudioDriverZts.cpp`, `ASFWAudioNub.cpp`.

**Contract.** Extract the arming, timing-observation and fill work out of
`ASFWAudioDriverZts.cpp` into the session. **Relocating a function that still
dereferences audio ivars solves nothing** — the extraction must come with a
retained slot-provider and timeline contract. There must remain exactly one TX
consumer and one pump; an audio-active engine and a MIDI-only engine competing
is a failure of this package, not a configuration. The audio service supplies or
removes a PCM source as its lease changes. Leases acquire and release on a
serialized owner queue, failed starts roll back, hardware stops only on last
release. Specify and test audio-open, audio-close, rate change, bus reset and
unplug while a MIDI lease is held. Teardown quiesces dependent queues and drops
cross-seam views before freeing buffers or detaching hardware.

Do not fabricate HAL activity to keep the pump running.

**This is the largest and riskiest package, and it is not MIDI work.** It is an
audio-architecture refactor whose entire acceptance is audio regression plus
"a MIDI lease alone starts the DICE duplex path with valid silence". If it grows
past a reviewable change it should get its own plan document under
`documentation/plans/` and this plan should depend on that one. Per the
repository's commit-hygiene rule, commit before it gets large.

**Done when** a MIDI-only lease brings the existing DICE duplex path up with
valid silence, audio open/close neither stops it nor creates a second pump, the
last release stops hardware, a failed start rolls back, and existing audio-only
behaviour passes regression unchanged.

### WP-7 — RX extraction and delivery — **landed end to end, hardware-unverified**

**Goal.** MIDI bytes off the wire into the RX rings and out as UMP, working with
CoreAudio closed.

**Files.** `Audio/Engine/Direct/Rx/RxAudioPacketProcessor.cpp`, a validated
AM824 packet view in the content layer, and the `Midi/` delivery path.

**Contract.** Build a validated AM824 packet view and use it for both PCM and
MIDI. Extraction must run **before** the PCM-bound early return, because the
writer is legitimately unbound in MIDI-only operation. `hasValidCip` only means
`CIPHeader::Decode` accepted the EOH markers — it validates neither the AM824
format nor the geometry. Before extracting: check transfer status, minimum
header length, supported FMT/FDF and packet form, nonzero supported DBS,
expected formation, `midiSlotIndex < dbs`, and payload length divisible by
`4 * dbs`. Data blocks start at offset **16** — an 8-byte receive prefix
followed by the 8-byte CIP header. Header-only NO-DATA has no blocks; do not
derive MIDI from leftover storage. Take the event count from the bounded wire
payload, never from the number of PCM samples successfully written, and use the
DBC from that same packet.

`b[0] == 0x80` is legitimate empty, not an error. `len` may be 1–3: a single
quadlet can carry a whole 3-byte Note On. Default `dbcAligned` to true and make
it a per-family profile flag.

The sink copies a bounded run into its own storage and must not retain a pointer
into a receive packet.

**Tests.** Host vectors for labels 0x80–0x83, invalid labels, every mux
position, DBC wrap, short and remainder payloads, NO-DATA, plus the WP-3 parser
vectors driven through the extraction path.

**Done when** physical Saffire MIDI input works with CoreAudio both closed and
open.

**What landed.** `Audio/Ports/IMidiByteSink.hpp` is the seam — port-indexed
bytes and a discontinuity mark, nothing else. `Audio/Wire/AM824/MpxMidiDemux.hpp`
does the demultiplexing as content-layer logic, and
`Midi/Transport/MidiRingByteSink.hpp` adapts it onto the device→host rings.

Extraction runs in `RxAudioPacketProcessor::ProcessPacket` **before** the
`writer_.IsBound()` early return, which is the whole point: in MIDI-only
operation no CoreAudio client is open, the writer is legitimately unbound, and
extracting after that check would make MIDI work only while audio happened to be
running. It is an opt-in parameter defaulted off, so every existing audio-only
caller is byte-for-byte unchanged.

Validation before extraction: the payload must be a whole number of data blocks
(a new `ragged` result field — PCM decode has always truncated silently here, but
a MIDI slot index derived from the wrong DBS reads a different slot entirely), the
geometry's DBS must match the packet's own, and the slot index must be inside the
data block. Labels are range-checked to 0x80–0x83 rather than masked: Focusrite's
`label & 3` agrees for valid input but would also accept an audio label as MIDI.
Event count comes from the wire payload, never from PCM samples written.

**Routing.** `AudioCoordinator` arms the nub's transport for the endpoint's
stream epoch, then hands the block plus the device→host geometry to
`IsochDuplexHostTransport`, which owns the `MidiRingByteSink` and gives the
extraction to stream 0's receive consumer only — a secondary slice shares the
packet stream but not the MIDI slot, so giving it the same extraction would
deliver every byte twice.

The wake is an `OSAction`: the consumer raises one callback per receive batch
that carried bytes (on the first such packet, not at the end — the MIDI service
reads on its own queue, so telling it early costs nothing), the coordinator's
lambda calls `NotifyMidiReceived` on the nub, and the action lands on the MIDI
service's queue where `DrainReceiveRings` converts and Sends. Conversion never
happens on the receive queue.

Teardown detaches the extraction before the nub is terminated, because the sink
borrows the nub's rings.

**Still open.** None of it has executed. Hardware has to confirm that bytes
actually arrive, that they arrive with CoreAudio closed, and that the wake lands
on the MIDI service's queue rather than the receive queue.

### WP-8 — TX composition — **landed for audio-active; MIDI-only needs WP-6**

**Goal.** Host MIDI onto the wire, once, with correct accounting for the bytes
that do not make it.

**Files.** `Audio/Wire/AM824/` (new MPX mux and rate limiter),
`Audio/Wire/AMDTP/AmdtpTxPacketizer.cpp` (final candidate-image composition),
`Audio/DriverKit/ASFWAudioDriverZts.cpp` or its WP-6 successor (bounded fill
window).

**Contract.** Three things, in this order of difficulty.

*Reservation.* Reserve for `{streamEpoch, packetIndex}`, snapshot per-port read
positions and stage the limiter decision, write only reserved bytes, commit
exactly once on successful publication, cancel on rejection, sibling
unreadiness or a missed frontier. Cancellation retains bytes and does not spend
emission credit, but elapsed wire time still advances the limiter. **The
byte-identical-refill invariant at `AmdtpTxPacketizer.cpp:138-148` is what makes
this mandatory.** Compose MIDI after `WritePcmSnapshot`, never merely after the
defaults pass. Leave `PrepareDataPacket` alone: it already writes a byte-correct
empty `0x80000000` into every non-PCM slot.

*Accounting.* A successful offer is a software retirement point under an
at-most-once policy, not physical delivery. `Isoch/Core/IsochTxQueue.hpp:201-230`
permits `kLateImageReady` to become `kFinalOnArmedImage`, recorded as
`SealedDiscardingAlternative` / `latePayloadLostPublicationCount`
(`Isoch/Transmit/IsochTxDmaRing.cpp:263-272`). So `CommitFill == true` does not
mean the bytes reached the bus. Count those as loss, never replay an
already-offered byte behind later bytes, and carry bounded per-port provenance
so loss is attributable — the existing aggregate transport counter cannot
identify a lost MIDI byte. The sibling commit is sequential, not atomic: for
this milestone MIDI rides stream 0 only and retires when stream 0 publishes,
regardless of a later sibling failure.

*Bounded horizon.* The fill loop currently runs to `committedAfter`, up to about
126 ms ahead. With an always-ready silence source it fills the whole horizon and
`armedFilled_` then locks MIDI out of every one of those packets — recreating
arm-time latency despite writing in `RefillPcm`. Introduce an explicit window
relative to transport progress, beyond the freeze frontier with dispatch slack,
and leave more-distant armed packets unfilled. Within the window use PCM when
ready and silence at a selected finalization deadline, always including MIDI.
Note that `substituteSilenceOnPcmUnavailable` is stored and copied but **not
consulted** by `FillTransmitSlot` or `RefillPcm` — flipping it implements
nothing. Trace the effect on PCM cache retention and sibling commit before
changing shared fill policy.

Rate limiting follows Linux's accumulator, written fresh: age once per eligible
port opportunity independent of callback batching, roll back an emitted-byte
debit without undoing elapsed time, and never double-age on NO-DATA or cancelled
packets. Keep the first-eight-block cap independently of the limiter.

**Tests.** A hardware-independent fill-window simulator under `tools/` plus host
suites proving: no byte retired on a rejected or cancelled fill, no retry
duplicates a published byte, later transport discards counted as loss, limiter
aging correct across skipped opportunities.

**Done when** physical Saffire output works MIDI-only, audio-active and during
PCM starvation, with PCM frame assignment and small-buffer operation unchanged.

**What landed.** `Audio/Wire/AM824/MpxMidiMux.hpp` composes the slot (rotation,
eight-block cap, label 0x81) and `MpxMidiRateLimiter.hpp` models the device UART
as Linux's fractional accumulator does. The limiter is staged inside
`MidiTxReservationScope`, so a cancelled fill returns the emission credit while
the elapsed wire time stands.

`AmdtpTxPacketizer::ComposeMidi` runs after `RefillPcm`, never before: defaults
are laid into every slot and PCM then overwrites its own, so MIDI written earlier
would be erased. `DiceTxStreamEngine` reserves at fill, composes, and commits
only when `PublishLatePayload` succeeds; a lost race cancels.

The seam reaches the audio service through a relay on the audio nub. That
reverses WP-4's note, correctly: the note was conditional on the extracted
session being the sole TX consumer, and **WP-6 has not landed**, so
`ASFWAudioDriver` *is* the host→device consumer today. The relay and the audio
service's mapping both disappear when WP-6 moves the pump.

**Two bugs found and fixed while wiring, both mine.** `CommitFill` dereferenced
the transport block unconditionally, and every filled packet reaches it including
on streams with no MIDI — a null dereference on the audio hot path. And a fill
abandoned before `CommitFill` (the ZTS loop skips it when a sibling stream is not
ready) left the reservation outstanding, so every later `Begin` was refused and
MIDI would have stopped for good at the first unready sibling.

**Not done, and it is §10.2's first gate.** MIDI-only transmit still cannot work:
`FillTransmitSlot` returns `NotFillable` with no PCM source bound, so with no
CoreAudio client open no packet is ever filled and no MIDI is composed. That is
WP-6's stream lease. The bounded fill horizon and silence substitution of
§10.3(b) are also untouched, and belong with it — they only bite once an
always-ready silence source exists.

### WP-9 — Saffire qualification

Discovered supported rates, sustained duplex MIDI, long SysEx, overflow,
repeated open/close, bus resets, TX recovery and unplug under load. Collect
bounded timing telemetry through the MCP control plane — arrival, reservation,
publication, target cycle and RX delivery, each labelled with epoch and clock
domain. Report median, upper percentiles, maximum, loss and duplicate counts
separately for MIDI-only and audio-active runs.

**Normal-load qualification requires zero offered-MIDI discards, not merely zero
failed offers.** No stale epoch replay, no stuck leases, no cross-service UAF.
Qualify each additional rate and model explicitly.

## 5. Ordering

```
WP-3 (UMP converter) ────────────────────────────────────┐  no blockers
                                                          │
G0.1 entitlement ─┐                                       │
G0.2 provider ────┼─▶ WP-2 (service skeleton) ─┬──────────┤
G0.3 scheduling ──┘                            │          │
                                               │          │
WP-5 (offer producer) ─── audio-only ──────────┤          │
                                               ▼          ▼
WP-1 (capabilities) ──▶ WP-4 (transport block + seam) ──▶ WP-7 (RX) ──┐
                                               │                       │
                        WP-6 (shared session) ─┴──▶ WP-8 (TX) ─────────┴─▶ WP-9
```

Practical reading:

- **Start with WP-3.** It is pure, host-testable, blocked by nothing, and it is
  needed identically whatever G0 returns.
- **WP-5 next**, because it is a real audio defect today — the doorbell rings for
  nobody — and it ships and validates on its own.
- **G0 in parallel with both**, since §3.1 may stop the host half entirely and
  §3.3 may invalidate the TX contract.
- WP-6 can begin any time after WP-5 and is the long pole; it gates WP-8 only.
- WP-7 before WP-8, so the first wire change is the one that cannot corrupt an
  audio packet.

## 6. Decisions that must become concrete, with their evidence

| Decision | Owner package | Evidence required before choosing |
|---|---|---|
| Fill-window upper bound and silence deadline | WP-8 | Simulator over 48 kHz blocking cadence with dispatch stalls, skipped packets and PCM retention at today's 3-cycle finality. Express in packet/cycle coordinates plus epoch. Do not bake 375 µs into a promise. |
| Ring capacity per port, both directions | WP-4 | Backlog measured at 3093 B/s against the worst observed dispatch stall; the repo already records 5.0–5.25 ms DriverKit queue stalls. |
| Retry policy on transport discard | WP-8 | At-most-once until an ordered terminal-outcome protocol exists. Reserve/commit pseudocode does not provide one. |
| MIDI provider class | WP-2 / G0.2 | A working match, not an argument. |
| TX contract if scheduling probe fails | WP-8 / G0.3 | Re-decide; do not invent a deadline API. |
| Whether WP-6 gets its own plan | WP-6 | Size of the first reviewable slice. |

## 7. Test and build commands

Host suites first, with the actual introduced suite names:

```bash
./build.sh --test-only --test-filter <SuiteName>
./build.sh --test-only                       # full C++ suite for WP-5/6/8
./build.sh --no-bump                         # IIG + signing integration for WP-2
```

New suites register with `add_audio_test(<Name> <sources...>)` in
`tests/audio/CMakeLists.txt`; pure `Midi/Ump/` tests may warrant their own
directory and CMake list. Driver sources need no `project.yml` edit (§2), but
`OTHER_LDFLAGS`, entitlements and `Info.plist` do.

`build.sh` is known to print a Swift-test success line even on failure — grep
the output for `error:` rather than trusting it.

Host stubs cannot validate service matching, dispatch, or hardware publication.
Batch physical checks at each milestone and read the driver through the ASFW MCP
control plane rather than the unified log.

## 8. What would invalidate this plan

- ~~`family.midi` not grantable on the account~~ — resolved: ad-hoc signing with
  SIP disabled accepts the entitlement locally (§3.1). Only a distributable
  build depends on the grant.
- The scheduling probe showing early or coalesced delivery (§3.3) — WP-8's
  contract is rewritten and the latency model in `MIDI_WIRING.md` §14 is void.
- WP-6 turning out to require changing completion geometry — that needs separate
  audio-timing validation and is not authorized by this plan.
- Saffire's actual 48 kHz stream table disagreeing with the assumed single jack
  pair on stream 0 — WP-1 rejects rather than guesses, and the milestone device
  may have to change.

## 9. Out of scope

BeBoB BridgeCo plug-type and channel-position commands, Phase 88 logical-port
normalization, Oxford port policy, secondary-stream MIDI routing, other Saffire
variants, MIDI 2.0 translation, JR timestamps, and any driver-side future
scheduling. Each is recorded in `MIDI_WIRING.md`; none is a prerequisite here.
