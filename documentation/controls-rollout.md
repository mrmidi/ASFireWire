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

Status: work-in-progress model checkpoint; not validated or complete.

The initial changes introduce read-only access, one-shot action domains, and
state-carried scalar range overrides and applicability. Write validation starts
checking these constraints before dispatching a provider write.

Remaining work includes snapshot serialization, wire value decoding, Swift model
and UI support, error mapping, and host tests. Existing serializers and exhaustive
variant visitors still need adjustment for the action variant. This checkpoint
must not be treated as a buildable release or as completed Phase 2 acceptance.

## Subsequent phases

3. FW-205: generic CoreAudio projection and AV/C migration; delete superseded
   AV/C control classes after parity validation.
4. FW-261: Duet provider and generic app controls, including knob synchronization.
5. FW-203 stage 1: Saffire basic controls; clock source initially read-only.
6. FW-203 stage 2 / FW-207: Saffire mixer and app matrix; exact mute/solo restore.
7. FW-203 stage 3 / FW-260 items 2–3: router commit/readback and app patchbay.
8. FW-203 stage 4 / remaining FW-260: DSP blocks, raw ranges first; recover real
   units from reference evidence before publishing conversions.
9. FW-198 / FW-201: remove superseded paths and unused model parts; writable
   DICE clock source requires the lock/rate transition review.

Each phase needs its own implementation and validation record. Hardware checks
remain pending until actually run; host tests do not establish hardware behavior.
