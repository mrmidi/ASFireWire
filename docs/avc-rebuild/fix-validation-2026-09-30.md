# AV/C corrective implementation and review — 2026-09-30

Worktree: `/Volumes/SDExt/DEV/ASFireWire/tmp/avc-wt`, branch `refactor/avc-stack`.

Three Luna agents implemented corrections to the earlier review; the parent reviewed their diffs, requested revisions, and ran combined validation. Changes remain uncommitted. This report supersedes the original review's correctness findings for the corrected paths, but does not mark every planned phase complete.

## Corrected behavior

- Apogee STATUS replies retain the actual returned company ID and payload through an explicit generic reply type. The Apogee parser validates that returned ID; generic vendor framing has no Apogee policy.
- Typed FCP submissions bind the requested bus generation at admission and queued-write issuance. Stale requests fail once without writing. Generation rebinding remains confined to the existing explicit idempotent reset-retry path. Busy and other migrated AVC errors retain their common IOReturn mapping.
- Audio identifier/list reads use OPEN, READ, CLOSE sessions. Named traversal state retains the accessor through deferred operations; Music fallback callbacks also retain their accessor and unit. Child list IDs come from descriptor entry fields, with bounded traversal. Complete Phase 88 fixtures resolve names from child 0x1801.
- Descriptor parsers reject declared-length mismatch, truncated nested extents, invalid text blocks, malformed READ lengths/status/offset, and premature empty chunks. The simulator owns deferred response bytes and enforces descriptor sessions.
- Graphs distinguish sync destinations, selectors and endpoint identities, confirmed clock sources, advertised control masks, and STATUS-confirmed controls. Names no longer establish clock or volume semantics. Production caching leaves stream selection unresolved until route evidence is supplied; unavailable slot-map geometry is explicit.
- AVCUnit retains mixed Audio and Music subunits, updates identity, caches the graph, and records known plug counts without extra generic/camera probes. Duet and Phase 88 attach traces were updated only for the resulting Audio discovery sequence.
- Generic BeBoB rates are the duplex intersection; selected-rate formations supply each direction's geometry independently. Conflicting current rates or unavailable formations report caps unavailable. Profile configuration rejects geometry that cannot fit the wire DBS field.

## Reviewer revisions

The parent rejected duplicated vendor framing, a self-retaining callback cycle, RTTI-dependent casts incompatible with the dext, speculative plug probes, ambiguous malformed-text success, unchecked geometry narrowing, and success returns for unavailable runtime caps. A new analyzer dead-store warning was removed before the final build.

## Validation

- Full CMake host-test build passed.
- Full CTest run: 2,618 registered, 2,612 passed, six skipped, zero failures. Skips are existing fixture/dependency-dependent tests.
- `./build.sh --no-bump` passed for the app and dext. Only the existing multiple-destination Xcode warning remains.
- The descriptor agent ran deferred complete Phase 88 traversal under ASan and UBSan successfully.
- Focused transport, vendor, graph, descriptor, profile, simulation, and golden tests passed; `git diff --check` passed.
- No hardware installation or runtime hardware validation was performed. Host tests do not prove DriverKit dispatch-queue or hardware behavior.

## Remaining plan work

Production does not yet populate SIGNAL SOURCE selections, selectable-clock inquiry results, or STATUS-confirmed feature controls into the graph. The cached graph intentionally leaves these unavailable. Graph publication/UI consumers and broad legacy-command replacement remain unfinished phase work. The graph's pre-existing dependency on the audio slot-map type still deserves a separate layer-boundary cleanup. Generic-device factory integration is not completed merely by correcting the discovery model and profile geometry.

The earlier Python prototype remains under `docs/avc-rebuild/fixtures/graph_build.py`; both fixture runs were checked during the original review. No rewrite of the prototype or hardware-specific clock/mixer programming was introduced here.
