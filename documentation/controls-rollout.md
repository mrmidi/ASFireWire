# Controls rollout — FW-197

Branch: `feat/controls-sync`, based on `071f9f79` (0.6.0, build 24).

Keep implementation checkpoints tied to a phase and ticket. Record software
validation separately from hardware acceptance. A checkpoint does not establish
that its phase is complete.

## Phase 1 — FW-259

Implementation checkpoint: `319bb346`.

Confirmed AV/C changes update the graph and fan out to CoreAudio and the app.
The app consumes pushed values instead of polling snapshots for value changes.

Software validation: 109 relevant C++ tests and 26 Swift ControlSnapshotTests
passed. The affected rejection fixture was corrected and rerun. Xcode's targeted
test build succeeded with signing disabled. Generated project reproducibility
and whitespace checks passed. Version remains 0.6.0, build 24.

Hardware acceptance remains pending: Phase 88 mute changes in both directions
between the app and Audio MIDI Setup, AV/C volume parity, and reconnect behavior.

## Phase 2 — FW-260, items 1, 4, 5

Initial model checkpoint: `0889891e`. Software implementation is now complete
for items 1, 4, and 5: snapshot serialization and wire decoding support access,
actions, state range overrides, and applicability. The Swift model and controls
honor the current range and block read-only/inactive writes. Actions carry no
remembered result and duplicate in-flight triggers are not coalesced for replay.

Validation: 14 TopologyModelTests, 9 AvcControlProjectionTests, and 30 Swift
ControlSnapshotTests passed. The targeted Xcode test build succeeded with signing
disabled. New host tests cover changing Mic/Inst ranges with stable identity,
invalid metadata, read-only/inactive writes, and action serialization. Swift tests
cover decoding, effective range snapping, action encoding, and refusing writes
before calling the driver. An initially missing action JSON tag was found by the
host test and corrected. These are software checks only.

## Phase 3 — FW-205, generic CoreAudio projection

Checkpoint: `1e5ecb39` (Codex work as found), fixes on top.

`CoreAudioProjection` turns model parameters into CoreAudio controls by
semantic (volume, mute, phantom, phase, nominal level, source, clock source,
high-pass). A control's scope/element comes from walking channel-preserving
paths to a host stream; an ambiguous path publishes nothing rather than a
guessed channel. Mixer crosspoints stay app-only. Three generic classes
(`ASFWParameterLevel/Boolean/SelectorControl`) replace the AV/C-specific ones
in the graph; `ControlService` is the one write path for CoreAudio and the
user client. A mode-dependent range (Duet gain) is a normalized 0..1 level
translated against the current range, so the device is never republished.
The composite Input Type writes the inactive connector's level first, then
switches the source.

Fixes after the checkpoint:
- A pushed or re-read level outside the published range is not shown in the
  HAL (the AV/C path had this check; the generic one had dropped it).
- A failure reading provider controls at publish costs the controls, not the
  audio device.
- Host restore (`SetWantsControlsRestored`) only for AV/C feature controls. A
  provider-backed device keeps its own settings; the host must not re-apply
  phantom/gain to it at publish.
- Host-test mocks for the new driver/nub API; the HAL push test now covers
  level, boolean, selector and the re-read-all invalidation.

Superseded paths removed: the AV/C-only CoreAudio path (`ASFWAvcLevelControl`,
`SetAvcFeatureControl`/`ApplyAvcFeatureControl`, the `ASFWAvcControls` nub
property and the driver's copies of it) and the protocol-boolean path
(`ASFWProtocolBooleanControl`, `AudioControlBuilder`, `IDeviceProtocol`
boolean virtuals), which nothing had published since FW-132. The AV/C config
keeps its internal master list; it chooses the generic master volume/mute.
The one behaviour of the old write path still missing was restored: a
CoreAudio write whose 2 s wait timed out is not sent to the device late.

Not done here: the composite Input Type is two writes, not atomic. If the
source switch fails after the level write, the readback reports the device's
real state.

## Phase 4 — FW-261, Duet provider

`DuetControlModel` maps the Duet onto the model (ALSA `apogee.rs` ranges:
input gain raw 10..75, the instrument input shown as 0..65 dB). Input Type is
Mic / +4 dBu / -10 dBV / Inst. The provider polls the knob state every 500 ms
(ALSA's runtime reads the same state on a 50 ms timer when measuring,
`runtime/oxfw/src/main.rs:222`) and pushes only changes.

Fix after the checkpoint: control I/O is serialized. A write or source switch
arriving while the poll (or another write) is in flight waits in a bounded
queue instead of being refused `kIOReturnNotReady`; a route change answers
every waiter. Mutation-checked: refusing again fails the new host test.

Validation for phases 3–4: full C++ host suite 3730/3730, Swift 537/537,
dext and app build (signing disabled). Hardware acceptance pending: Duet controls in
Logic/AMS and the app both ways, knob sync, Input Type switching; Phase 88
volume/mute parity through the generic path.

## Subsequent phases

5. FW-203 stage 1: Saffire basic controls; clock source initially read-only.
6. FW-203 stage 2 / FW-207: Saffire mixer and app matrix; exact mute/solo restore.
7. FW-203 stage 3 / FW-260 items 2–3: router commit/readback and app patchbay.
8. FW-203 stage 4 / remaining FW-260: DSP blocks, raw ranges first; recover real
   units from reference evidence before publishing conversions.
9. FW-198 / FW-201: remove superseded paths and unused model parts; writable
   DICE clock source requires the lock/rate transition review.

Each phase needs its own implementation and validation record. Hardware checks
remain pending until actually run; host tests do not establish hardware behavior.
