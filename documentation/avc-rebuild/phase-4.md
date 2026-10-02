# Phase 4 — AV/C discovery, descriptors and publication rewrite

Landing: **one PR, four commits** (user correction, 2026-10-02). Base:
`feat/avc-transaction-engine` at `03dd1e2a`. Preserve wide CoreAudio streams;
record clusters without changing AudioDriverKit publication. GUID redaction is
separate privacy work. Phases 5–6 and broader 1814 probing are excluded.

## Engineering contract

- Per-unit slot: `variant<Idle, Running, Cancelling>`. Reject a second request
  as busy; terminal results live independently of the active session.
- Pure discovery reducer: `Step(state, event) -> {state, actions}`; events own
  reply bytes and carry session ID, operation serial and full route token.
  Adapter alone submits through IAvcUnit; reducer has no bus, timers or locks.
- Descriptor operation: Opening -> Reading -> Closing{primary result} -> Done.
  Successful OPEN requires CLOSE on a valid route, including read failure and
  cancellation. Reset/removal never sends cleanup to another route.
- Reuse Common/Lifetime.hpp. Callback access is through LiveRef on the same
  serial queue as destruction. Invalidate serial before cancellation/reset.
- Move-only completion: move disarms source, invoke consumes callback, duplicate
  calls log and are ignored, abandonment completes cancelled. Phase moves noexcept.
- Build mutable data privately, freeze shared_ptr<const DiscoverySnapshot>.
  Consumers acquire snapshot leases, never raw mutable subunit references.
- Copy every response span before callback return.
- Parsers: span input, expected<T, ParseError{offset,kind}> output; bounded
  readers, named size/chunk/list/depth budgets, visited text-list identities.
  Device bytes never trigger assertions or unreachable branches.
- Widen arithmetic before bounds checks; saturation must not make bad lengths
  acceptable. Use verified C++26 features where useful; no move_only_function,
  inplace_vector, reflection, contracts or #embed.

## Commit 1: baseline and descriptors (4.1 + 4.2)

Move plan/source fixtures into tracked documentation with provenance; update
links. Existing parsers, graph, report and descriptor API are regression evidence,
not missing features. Preserve parsed/wire goldens. UNIT INFO is optional and uses
five operands; do not attribute old response-ordering failures to the Phase 88.
The naked READ fallback was already removed at f44cb6e2: test no READ after failed
OPEN rather than list its removal as new work.

Rewrite owned descriptor access and music/audio/info-block/text-list parsing.
Keep command bytes and 128-byte requests. Validate specifier, offsets, lengths,
progress and total size. Declared length determines completion despite inaccurate
read-result status. Keep primary/cleanup errors separate. Handle only the captured
Duet short field layout, preserve unknown bounded blocks and valid optional results.
Behavior sources: FFADO avc_descriptor.cpp:156–290; Apple
MusicSubunitController.cpp:853–1008; local Audio/Music/Descriptor/Info Block specs.
Cite actual checked source lines, never copy reference implementations.

## Commit 2: discovery and subunit contents (4.3)

Keep AVCUnit as transport/lifecycle owner; replace nested callback discovery and
mutable subunit hierarchy with reducer + adapter + plain music/audio contents.
Order: policy/route/preparation -> unit/subunit/plug inventory -> formations and
current signal format -> descriptors/text lists -> SIGNAL SOURCE -> confirmed
STATUS controls and INQUIRY sources -> read-only extension results -> snapshot.

Recheck policy before every frame. Preserve restricted/profile-owned/Fireworks
skips, full subunit identities and learned/fixed opcode policies. Unknown subunits
retain counts. Optional unsupported results do not block independent discovery;
transport/route loss cancels. Attach and diagnostics use the same read-only reducer.
Rate/clock/mixer controls stay in runtime/device ownership. Adapt legacy getters
internally so the intermediate commit remains buildable.

## Commit 3: graph and publication/preparation (4.4)

Pure graph builder preserves clusters, music plugs, PCM slots, MIDI, names and
routing evidence. Capture selection is SIGNAL SOURCE-based; ambiguity is explicit.
Current rate comes from signal format; formations provide geometry/rates even
without music descriptor. Valid mapping preference: descriptor -> BridgeCo ->
reference identity. Advertised controls are separate from STATUS-confirmed ones.
Selector declarations alone cannot imply selectable clock sources.

Move audio conversion/publication, startup policy and bootloader preparation
outside generic AV/C, wired through composition. 1814 preparation must succeed
and route be revalidated **before the first enabled AV/C producer/frame**. Failed
preparation blocks traffic; pin order with a restricted-policy golden.

Publication states: Ready, WaitingForDiscovery, Failed{reason}. Waiting subscribes
to required discovery/extension completion and reevaluates on that event. Terminal
unusable geometry is visible failure; no retries against unchanged data. Manual
diagnostics never republishes or applies startup controls.

## Commit 4: diagnostics, user client, MCP and report (4.5)

Serialize immutable snapshots to unchanged legacy layouts/selectors/ordering/error
semantics. Descriptor APIs remain cached. New versioned paged diagnostics covers
status, graph, descriptors/text lists and timed exchanges within 4 KiB per page;
bind pages to session/route and expose dropped/incomplete results.

Report follows the same driver session, enforces streaming exclusion in driver
and app with coordinated admission, exports replay-compatible exchanges plus
versioned metadata, imports old reports, and retains previous report on failed
refresh. Replay feeds owned events into the same reducer. Stale/cancelled captures
never replace the report. No second live probe path and no GUID privacy work.

## Gates and acceptance

Each commit: focused tests, full C++ suite, signed app/dext build --no-bump;
Swift tests for changed app/connector APIs and pydice integration checks. Regenerate
from project.yml; verify arm64e. Delete replaced paths as callers switch.

Required tests: captured parsing/maps/names; five-operand UNIT INFO success and
optional failure; no READ without OPEN; CLOSE on read error; malformed/zero-progress/
oversized/chunk/depth/list limits; reset/cancel/destruction at each phase; exactly
once, no UAF/stale commit/new-route cleanup; descriptor-free BridgeCo geometry;
visible terminal publication failure; late extension event reevaluation; 1814
preparation/route/frame ordering and forbidden-frame absence; report/stream race,
busy session, mixed pages/dropped logs; export -> same reducer -> same graph.
Run meaningful parser/lifecycle mutation checks.

Hardware after commit 3: Duet and Phase 88 attach, 48 kHz play, stop/start/reconnect.
After commit 4: idle report capture and replay on both. **1814 evidence is goldens
only**, no attach/probing in this phase; state this limitation in the PR.

## Progress

- Commit 1: implemented and validated; 2,703 registered C++ cases, six existing skips;
  signed app/dext build and arm64e verified. Four mutations detected/restored.
- Commit 2: discovery now runs through the owned session/reducer; immutable snapshots
  back the compatibility projections. 2,710 registered C++ cases pass (six existing
  skips), signed app/dext and arm64e verified. Owned-event replay reproduces commands
  and contents; every recorded boundary covers cancellation/route loss. Removing
  reply identity admission was detected by the lifecycle mutation test. Original
  wire traces remain; new phase-4 attach traces pin the intended discovery order.
- Commit 3: implemented. Publication, startup policy and bootloader preparation
  moved to `Audio/Protocols/AVC/` (`DiscoveryCoordinator` behind `DiscoveryOwner`);
  no AV/C unit exists before preparation confirms the firmware. Preparation keeps
  one record per incarnation, re-reads after a cue on a new route and never cues
  twice (`LoaderStillActiveAfterCue` fails visibly). Old nested subunit discovery
  (`ParseCapabilities`, `ReadIdentifierDescriptor`, `Query*`, `Set*`, Camera
  subunit, `ProbeUnitInfo`, `GetPlugInfo`) deleted; subunits are snapshot
  projections. Reducer: optional probes survive a timeout or refusal (two
  consecutive timeouts end discovery); opcode learned only from a plug's first
  query; mute+volume STATUS always asked per feature channel (Duet golden now
  matches the measured FB1 frames); selector INQUIRY dropped (no reference);
  sync-candidate INQUIRY cites FFADO avc_plug.cpp:670-690. 2,708 C++ cases pass,
  signed app/dext built, arm64e verified. Mutations caught: second cue allowed,
  manual refresh republishing. Hardware gate pending (batched).
- Commit 4: pending, hardware report gate pending.

Hardware validation is deferred until all software stages are ready, per the user’s
request to batch validation of the single PR. No Phase 4 hardware claim is made.
