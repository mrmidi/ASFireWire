# Executable audio timing contract

**Status (2026-09-06): host-side specification and production-class audit.**
This makes the admission rule executable before the Phase 3 progress repair and
Phase 5 recovery implementation. It does **not** repair the running driver,
choose a latency reference plane, or establish the cause of the measured RTL.

- Specification: [`tests/support/AudioTimingContract.hpp`](../tests/support/AudioTimingContract.hpp).
- Tests and independent trace: [`tests/audio/AudioTimingContractTests.cpp`](../tests/audio/AudioTimingContractTests.cpp).
- Semantics and research limits: [CoreAudio HAL timing domains](COREAUDIO_HAL_TIMING_DOMAINS.md).
- Work order: [latency ledger plan](AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md#next-implementation-block--phases-3-and-5-before-more-bench-work).

## The admission rule

A content range is eligible for a timing aggregate only if `Contract::Admit`
returns a successful `std::expected<TxPresentationRange, Rejection>`. There is
no partially accepted result. Rejections neither advance the content cursor
nor provide a range to aggregate.

| Obligation | Executable check |
|---|---|
| RX, TX and PCM belong to one epoch | Nonzero, identical epoch IDs at startup and every observation/admission |
| Packet progress is justified | Exactly one absolute packet position satisfies both the observed ring slot and independently supplied inclusive progress bounds |
| Ring geometry remains stable | A changed ring size requires a new epoch |
| Content is contiguous | The range begins at the next admitted frame; zero-length, overlapping, skipped and overflowing ranges are rejected |
| Content is the intended content | Independent evidence names the same first frame and frame count |
| The first frame has the intended presentation time | Its unwrapped bus time lies inside the independent evidence interval, including endpoints |
| A failed epoch cannot silently resume | Current-epoch ambiguity, missing evidence or a contract violation blocks admission until a strictly newer coherent epoch |

A record is *entirely* old — and so dismissible as merely late, returning
`StaleEpoch` without damaging a new epoch — only when the RX/TX/PCM snapshot and
every epoch stamped on the record itself name the same older epoch. Every mixed
pairing is a broken handoff and blocks: an old range under a current snapshot, a
current range under a stale snapshot, and a range and its evidence disagreeing
with each other. Re-entering the same epoch cannot clear a failure. A failed
partial transition also blocks admission on the previous epoch.

This is a single-threaded verification state machine. Agreement among three
observed IDs is a **necessary condition**, not proof of atomic publication or
safe cross-service teardown. Those still need the production recovery
transaction and concurrency tests in Phase 5.

**Two limits of scope, stated so the table is not read for more than it says.**

- **Packet progress and content progress are verified separately and never
  related.** `Admit` does not read the verified packet index. The contract can
  say that a ring position was justified and that a content range was
  contiguous, and cannot say that the two advanced consistently with each other.
  A frames-per-packet invariant is exactly the kind of relation the current
  investigation needs, and it is not here yet.
- **Only the first frame of a range is bound to a presentation time**, so the
  contract is blind to rate error inside a range: a range with the intended
  start and the wrong interval is admitted. It detects offset, not slope.

## Evidence is part of the contract

`PresentationEvidence` contains an independently established content identity
and an interval for that range's **first frame presentation event**, in unwrapped
24.576 MHz bus ticks. This is the event named by
`TxPresentationRange::presentationBusTicks`; it is not callback entry, descriptor
completion, PCM publication, or analog output. There is no host-clock subtraction
and no new interpretation of the HAL latency properties here.

The test oracle fixes content identity before inspecting the production range.
It advances a separate media clock, supplies fresh per-range evidence, and
includes synthetic phase drift. Both content identity and time are checked:
shifting the candidate's frame labels and its timestamps together must not
create agreement with an independent reference.

The small two-tick interval in the synthetic trace is a **fixture property**.
It is not a proposed hardware tolerance. Real evidence must carry the measured
clock/correlation uncertainty, its valid horizon and the provenance of its
content identity. If those cannot bound the particular range, supply no
evidence and reject it. The helper consumes an interval; it does not estimate a
sample rate, prove a tolerance is reasonable, detect fabricated evidence, or
extrapolate indefinitely from a nominal startup slope.

For progress, bounds mean descriptor advances since the previous verified
position. The candidate set is:

```text
{ p : previous + minimum <= p <= previous + maximum,
      p modulo ringPackets == observedSlot }

0 candidates -> inconsistent evidence
1 candidate  -> verified absolute position
2 or more    -> ambiguous; no position is returned
```

The test oracle enumerates this set independently of the resolver's arithmetic.
An unchanged slot with bounds `[0, 48]` admits both zero progress and one lap
for a 48-packet ring, so it is ambiguous. `[48, 48]` resolves one lap only when
some independent evidence actually establishes those bounds.

Elapsed bus cycles are not such exact execution evidence. ASFW's self-linked
skip behavior can consume a cycle without advancing the packet; cross-checked
against the behavioral explanation in the local read-only reference
`references/linux-ohci-firewire-low-level-stack/ohci.c:3250-3256`.
The skipped-cycle fixture executes 53 packets over 101 cycles. The existing
nearest-lap helper guesses 101; the contract with only `[0, 101]` returns
ambiguity. No runtime capability to obtain the fixture's exact bounds is claimed.

Every relevant progress observation/loss must be delivered to `ObserveProgress`
in order. The helper cannot detect an omitted observation. It can admit several
content ranges after a verified observation when each has its own independent
presentation evidence; it does not claim that one pointer reading proves all
future execution. Capturing trustworthy, complete evidence is Phase 3 work.

## What the tests establish

The suite checks unique/absent/multiple candidates against enumeration,
inclusive boundaries, invalid geometry and integer overflow, delays shorter
than a lap and delays of 1, 2 and 13 laps, and skipped cycles. Admission tests
exercise absent evidence, gaps/overlaps, incorrect content counts, presentation
error of either sign, sticky rejection, mixed epochs and old-record replay.

Tests also call the **real** `HardwareSampleTimeline::PreviewTxRange` and
`CommitTxRange`:

1. Normal streams at 48/96/192 kHz agree with an independent DATA/NO-DATA script
   and fresh drifting presentation evidence.
2. After initialization, the current class accepts a presentation shift of
   ±1 or ±13 laps while returning the same next content frame. The contract
   rejects it as `PresentationMismatch`.
3. A late presentation input to the receive-derived seed moves the candidate
   by 288 frames per synthetic lap at 48 kHz. Independent intended content
   rejects that candidate, even if the verifier's continuity cursor is given
   the same bad initial label.

The last two are **detector tests for known production behavior**. Their passing
means the oracle catches the injected defect, not that production conforms.
They do not establish where a real completion-path error originates, its sign,
or whether a weak loopback correlation selected a repeated impulse. When the
runtime repair lands, turn these into conformance tests for the new adapter:
correct alignment or explicit recovery, never a shifted accepted range.

## Run and use it

```sh
cmake -S tests -B build/tests_build
cmake --build build/tests_build --target AudioTimingContractTests -j 8
ctest --test-dir build/tests_build --output-on-failure -R 'AudioTiming(ProgressContract|Contract)'
```

The new target is part of the normal CMake/CTest suite. It is host-only, so it
does not add a dext source or change `project.yml`.

**Validation on 2026-09-06:** all 21 contract tests passed. The existing timeline,
completion-drain, packet-lift and architecture checks also passed. Mutations were
compiled against temporary copies of the specification header, leaving the
working tree and normal test binary unchanged:

| Deliberate weakening | Failing tests |
|---|---:|
| Permit admission/observation after the epoch is blocked | 6 |
| Return the first candidate when progress is ambiguous | 5 |
| Return an accepted range despite presentation mismatch | 3 |
| Return an accepted range despite content identity mismatch | 2 |
| Allow a partial RX/TX/PCM epoch transition | 1 |
| Judge staleness from the snapshot alone, ignoring the record's own epoch | 1 |

These are mutations of the verification contract, not of a production recovery
implementation. They show that weakening its admission rules is observable.

The last row is a regression guard rather than a hypothetical. The first
revision of this contract judged staleness from the snapshot triple alone, which
dismissed a current-epoch record under a stale snapshot as merely late — leaving
the contract usable where the mirrored pairing correctly demanded recovery. The
fixture ring size is now `static_assert`ed against
`Isoch::Tx::Layout::kNumPackets`, so the lap arithmetic cannot outlive the
geometry it describes.

Next, supply the runtime adapter with defensible progress evidence and fresh
presentation evidence; replace silent continuation after lost progress with a
coordinated epoch transition. Preserve an independent test oracle while doing
that. Do not feed the candidate's own extrapolation back as its expected time,
or turn the nearest-lap estimate into an exact bound. Batch hardware verification
after those repairs and the instrumentation are ready.
