# TX latency metering: complete PCM publication to transmission-cycle bounds

Status: implementation plan, 2026-09-09. No hardware action is authorized by this document. Scope is a TX measurement session exposed through the existing control app and MCP, initially validated at 48 kHz.

## 1. Measurement contract

Measure the elapsed time between availability of the complete, identified PCM payload of a packet and that packet's successful hardware transmission cycle.

This is a packet-readiness metric, not the residence time of every individual frame. If a packet spans two publications, the earlier frames waited longer than the packet-ready measurement. Do not call it physical output latency or RTL. It excludes client rendering before publication and device processing after transmission.

Both endpoints have uncertainty:

```
E0 complete-payload availability ∈ [publicationEarliest, publicationLatest]
E2 successful transmission      ∈ [txEarliest, txLatest]

rawWaitMin = txEarliest - publicationLatest
rawWaitMax = txLatest   - publicationEarliest
```

Use checked signed differences. Preserve the raw bounds in exported results. For confirmed matching PCM, the physical wait is nonnegative; a raw interval crossing zero can be displayed intersected with [0, infinity), while preserving its original uncertainty. An interval entirely before zero is inconsistent evidence. Define interval endpoints and conservative rounding consistently; treating the computed upper endpoint as inclusive is acceptable and slightly conservative.

Never insert a predicted packet schedule as evidence of execution. A completed descriptor with a failure event is not successful transmission.

## 2. Source verification before implementation

Recheck source paths and current ABI/selector versions before editing; line numbers and constants in earlier proposals are not stable.

- OHCI mechanism: inspect the local Linux `references/linux-ohci-firewire-low-level-stack/ohci.c` transmit completion handling and `core.h` timestamp conversion, plus ALSA `references/linux-sound-firewire-stack/firewire/amdtp-stream.c`. Cite verified locations in the implementation. Learn behavior; do not copy reference code.
- Current ASFW completion extraction: `ASFWDriver/Isoch/Transmit/IsochTxDmaRing.cpp`. Confirm descriptor status/event validation and ordering before accepting the low 16 timestamp bits. A zero timestamp can be valid at a wrap; it is not the completion-valid flag.
- Correlation and lifting: `Audio/Shared/TxCycleAnchor.hpp`, `Isoch/Core/IsochTxQueue.hpp`, and `Audio/DriverKit/ASFWAudioDriverZts.cpp`.
- Publication and copying: `Audio/Runtime/PcmPublicationCache.{hpp,cpp}` and `Audio/DriverKit/ASFWAudioDriverIO.cpp`.
- Packet identity and image selection: `Audio/Wire/AMDTP/AmdtpPacketTimeline.*`, audio fill paths for both replay and M-Audio timing, and transport payload arbitration.
- Verify owning queues and service lifetime using the current runtime graph and teardown code. This plan adds no raw pointers across the service boundary.

## 3. E0: record actual publication coverage and uncertainty

Do not reuse `LedgerStampRing` end-cursor coverage as proof that a particular PCM range was published.

The current PCM cache suppresses duplicate writes and trims partial overlaps: `copyStart` can be later than the incoming `firstFrame`. Record the actual newly committed suffix `[copyStart, incomingEnd)`, not the entire caller-supplied range. This prevents a later overlapping callback from falsely redating already published frames.

Add a narrow publication receipt describing:

```
publication ID / monotonic sequence
timeline epoch
actual first committed frame and exclusive end
publication earliest/latest host ticks
```

For the first implementation, reuse the existing host clock reads bracketing `PcmPublicationCache::Publish`. A successful publication's visibility transition lies within that call interval. These are conservative publication bounds, not a claim that `publicationEnd` is the exact instant of visibility. Preserve the normal PCM commit behavior; do not delay publication to simplify diagnostics. The caller records a receipt only for a successful publication, after the return value and actual accepted range are known. Duplicate/rejected calls create no new successful receipt.

Verify the receipt against the same epoch checks and visibility gates used by `CopyExact`. If future code permits rewriting a logical frame within one epoch, range identity is no longer sufficient: require publication-version provenance from the copy itself before accepting a match.

Store recent receipts in an audio-owned bounded publication history, retained across measurement sessions within a stream epoch. Initial size: 256 entries, with eviction counters; do not advertise a fixed retention duration because HAL publication sizes vary. Reset/invalidate it through the existing publisher-quiescence path at stream epoch changes, not from an unrelated control callback.

Concurrency: no ordinary C++ payload fields may be concurrently overwritten under a sequence-only seqlock. For this small diagnostic history, use atomic sequence and atomic scalar fields with a documented, tested coherent-snapshot protocol. A fully sequentially consistent initial protocol is acceptable; verify single-writer serialization and lock-free scalar support on target architectures. Bound reader retries. Do not weaken memory ordering without a separate proof and tests.

Resolve a packet only if the union of retained successful receipts covers its entire half-open frame range in the same epoch. Do not require unrelated history entries to abut; require actual coverage of the requested range without holes. For each contributing publication, retain its identity and bounds. Derive complete-payload availability as:

```
publicationEarliest = max(contributing publication earliest times)
publicationLatest   = max(contributing publication latest times)
```

A receipt may not yet have been recorded when another queue observes the PCM commit. Distinguish this transient from eviction: allow a small bounded pending join, resolved before freezing. Never stall the PCM path. At freeze, unresolved joins remain explicit unresolved records.

## 4. Content identity: associate provenance with each image

For each packet generation and each payload image, keep audio-side provenance:

```
packet index and generation
image index
timeline epoch and PCM frame range
content classification: exact PCM / substituted / partial / unknown
publication provenance or immutable-identity evidence
```

Capture the classification from the successful exact-copy/fill operation before that image is published as ready. Do not infer it from sample values: intentional digital silence is valid PCM. A single mutable packet-wide `contentOutcome` is insufficient when the late image differs from the armed image.

Retain provenance until the corresponding completion is consumed, or explicitly report recycling as lost evidence. Readers must validate generation before and after any concurrent slot access using a race-free protocol; extending a struct does not automatically make its publication safe.

Transport completion metadata carries only neutral facts: packet identity/generation, selected image, actual completion timestamp, and completion success/error. It does not know audio frames, silence, CIP semantics, or publication timestamps.

The join uses the image that actually transmitted:

- Armed image containing exact intended PCM: eligible for Matched.
- Late image containing exact intended PCM: eligible for Matched.
- Selected image containing substitution or partial intended content: Substituted.
- Selected image's provenance cannot be proved: Unresolved.

Preserve the terminal image decision at completion before metadata is recycled. Do not infer it later from the latest fill attempt.

## 5. E2: conservative transmission bounds

OUTPUT_LAST identifies a cycle with no intra-cycle offset. Lift its modulo-eight-second timestamp against a hardware/host correlation pair only after validating completion identity, status, and plausible age. Require the age to be unambiguous within the helper's validated window; after long stalls or ambiguous wraps, report unresolved evidence rather than selecting the nearest timestamp blindly.

Propagate the host times bracketing the hardware cycle-timer read, or midpoint plus a conservatively rounded width. Use those endpoints to construct host bounds for the completion cycle. Preserve per-record correlation age and uncertainty for export.

Use rational conversion based on 24,576,000 FireWire ticks/second, with wide intermediates, overflow checks, and outward rounding. The existing divide-by-24 microsecond helper must not be used. If it is still present and incorrect, fix it separately with a focused regression test.

The uncertainty budget includes:

- One 125 µs FireWire cycle of unresolved packet position.
- Correlation read bracket and any known timer-read uncertainty.
- Conversion rounding.
- Bus/host rate-mapping uncertainty over correlation age.

The correlation bracket alone does not bound oscillator-rate error. Use an explicitly justified rate-error bound and age limit, or label the result **estimated transmission bounds** with the mapping assumption. Do not market a guaranteed bound without supporting evidence. Show median and maximum uncertainty, not only the median bracket.

Join each actual stamp to its own packet metadata in both supported audio timing paths. No uncompleted projection path may feed the metric.

## 6. Outcomes and accounting

Every selected sample has exactly one final outcome:

- Matched: successful transmission, selected-image PCM identity proven, complete publication coverage proven, timing bounds consistent.
- Substituted: successful transmission, selected-image provenance proves partial or replacement content.
- Unresolved: missing receipt, publication gap, epoch mismatch, evicted publication/provenance, ambiguous time lift, pending metadata at freeze, or other unprovable evidence. Preserve a reason code.
- TransmitFailed: descriptor reports failure; no successful TX interval is inferred.
- Invalid: all required evidence exists but identities or temporal bounds are inconsistent. Preserve the reason and raw data.

```
selected = matched + substituted + unresolved + transmitFailed + invalid
```

Keep separate counters for completion stamps lost before classification, unknown packet types, capacity exhaustion, and unsampled eligible packets. Missing stamps cannot reliably be assigned to DATA or NO-DATA; label the denominator **observed eligible DATA completions**, not total DATA packets sent on the bus.

The precedence of classifications must be explicit: known failed transmission first, known substitution next, then evidence resolution and timing checks for matching content. Unknown evidence must never be silently upgraded to a match.

## 7. Sampling and population

First release: five-second default, bounded duration and sample budget, fixed storage allocated outside the real-time path. A 4096-record ceiling is reasonable; compute actual memory from the final record layout rather than assuming the old 48/56-byte sizes.

Define the cohort explicitly as eligible completions processed by the audio observer during the session. This can include packets prepared/published before arming. Continuous bounded publication history allows them to join; missing history is reported. Do not claim this is an exact five-second hardware transmission interval or that it contains all final packets at the deadline.

Choose one pseudo-random ordinal within each fixed-size stratum of observed eligible DATA completions. Determine stratum size conservatively from duration, maximum eligible packet rate, and sample budget. Record seed, algorithm version, stratum size, and partial-final-stratum behavior. Reject invalid parameters and avoid zero-seed PRNG traps.

If storage fills, freeze with CapacityReached rather than silently biasing or overwriting the retained population. Keep the actual duration and all counters.

Record completion-group phase for both observed eligible DATA packets and selected packets. Compare those distributions; flatness across all slots is not expected if some slots carry NO-DATA. A seed makes selection reproducible for the same observed sequence, not hardware scheduling itself.

All percentiles describe the sampled, successfully matched population. Show the exclusion and evidence-loss counts beside them.

## 8. Ownership and session lifecycle

Choose a single audio-side owner for all mutable session state and sample storage. The control queue sends retained, bounded commands to this owner; it never clears or writes the active sample ring directly. Avoid synchronous cross-queue waits while holding the runtime registry lock.

```
Idle/Frozen → Arming → Capturing → StopRequested → Frozen
```

- Arm requests are validated and acknowledged on the owner queue. Allocate storage before capture. Latch session ID, endpoint identity, epoch, rate, build/geometry provenance, start time, deadline, and sampling parameters together.
- Stop, timer expiry, epoch change, and teardown request termination. Only the owner acknowledges Frozen after every in-flight sampler write has finished and bounded pending joins have been finalized.
- Use an owner-queue timer plus an in-loop deadline check. Expiry does not depend on continued hardware callbacks or UI polling. If the owner queue is delayed, report finalization delay; do not pretend results were frozen at the requested wall-clock deadline.
- The deadline check needs an actual host-now value. Reuse one already read in the observer if valid; otherwise allow one bounded clock read per observer wake. Do not promise no new reads while depending on a nonexistent `now`.
- No new sample is accepted after the deadline check fails. No result reads occur while Arming, Capturing, or StopRequested.
- Termination reasons: DeadlineExpired, UserStopped, EpochChanged, StreamReset, EndpointRemoved, CapacityReached, InternalError.

At Frozen, transfer the completed storage into an immutable result object and publish a retained handle. Readers retain that object for each page; rearming uses separate storage and cannot overwrite a reader's buffer. Session-ID checks detect stale requests but are not the memory-safety mechanism.

Bound retained result count and total memory. Return an explicit Busy/ResultExpired response when retention limits are reached; do not accumulate abandoned captures indefinitely.

Teardown must quiesce producer/observer queues before freeing runtime memory. If results should survive endpoint removal, transfer the immutable result to a service-owned store whose lifetime is independent of the endpoint. A registry lock alone does not establish this ownership. Shared memory is not intrinsically unsafe, but this diagnostic buffer does not need to cross the audio/transport seam.

## 9. Control API and wire format

Add dedicated versioned user-client operations for start, stop, and paged fetch (start/stop may share a command selector). Inspect the current selector table and allocate unused values; do not assume 1036/1037 are still free.

Requests identify the endpoint explicitly. Start supplies bounded duration, budget, and optional seed, and returns a request/session ID with pending/accepted status. Stop and fetch supply that session ID. Repeated stop is idempotent. Fetch never silently stops an active capture.

Header fields include version/size, session and endpoint identity, epoch, sample rate, state/reason, requested and actual timing, sampling parameters, population/outcome/loss counters, record count, cursor, and uncertainty-model version. Export loaded build provenance and effective geometry with the session.

Each record includes packet identity/generation, selected image, frame range, publication lower/upper host bounds, TX lower/upper host bounds, classification/reason, group phase, and correlation age/uncertainty evidence. Unavailable fields have explicit validity flags, not meaningful-looking zero timestamps.

Use fixed-width POD fields, explicit padding, sizeof/offsetof assertions, checked sizes, and strict Swift version/layout decoding. Calculate page capacity from the final header and record sizes under the inline response limit; do not retain the earlier assumed 96-byte header or 71-record capacity.

Extend the neutral TX queue ABI for completion/image/correlation fields and bump its current version with both sides updated. The audio publication/provenance records remain audio-owned.

MCP surface:

```
asfw_start_tx_latency_session {endpointId, seconds, sampleBudget, seed?}
asfw_stop_tx_latency_session  {sessionId}
asfw_get_tx_latency_results  {sessionId, cursor, maxRecords}
```

Document start/stop as bounded diagnostic mutations, not read-only calls. Do not claim MCP inherently causes VM faults; the reason to avoid polling is observer overhead and controlled measurement. No health/summary/discovery calls during the measurement. Finish first, then fetch pages.

## 10. UI and statistics

Put the first panel alongside existing Audio Geometry/Telemetry; a separate navigation tab is optional. Reuse established components.

- Endpoint and rate, current geometry, duration/budget, Start and Stop.
- Capturing / stopping / frozen states and termination reason.
- Label: **Complete PCM payload ready → bus transmission**.
- Median, P95, minimum, and maximum as ranges derived separately from lower and upper wait bounds using the same percentile convention.
- State that these are uncertainty bounds on sampled statistics, not statistical confidence intervals for all traffic.
- Always-visible observed/selected/matched/substituted/unresolved/failed/invalid counts and lost stamps.
- Precision: 125 µs cycle resolution, publication bracket, correlation/model uncertainty, and whether bounds are conditional on a rate assumption.
- Phase coverage compared with the observed eligible distribution.
- CSV export with raw bounds, provenance, reasons, session metadata, and signed raw waits.

Do not show planned horizon or hardware ring capacity as measured latency. Do not add a raw safety-offset control in this feature. Existing playback is sufficient; a test tone or loopback cable is not required for this internal metric.

Implement export and summary first. Add the time-series wait band and distribution after the records are validated. A five-second capture does not characterize a thirty-minute fault; longer observation needs an explicit bounded sampling policy.

## 11. Implementation map

New audio runtime components: publication receipt/history, pure transmission-bound and classification helpers, per-image provenance, session owner/result storage. Suggested files under `ASFWDriver/Audio/Runtime/`: `PublicationRangeRing.hpp`, `TxLatencyMeasurement.hpp`, and `TxLatencySession.{hpp,cpp}`.

Integrate actual suffix receipts through `PcmPublicationCache.*` and `ASFWAudioDriverIO.cpp`; integrate provenance through packet timeline/fill paths; integrate completed samples through both replay and M-Audio loops in `ASFWAudioDriverZts.cpp`.

Transport changes stay in `IsochTxQueue.hpp` and `IsochTxDmaRing.cpp`: neutral selected-image/generation/status/timestamp and correlation metadata only.

Control wiring belongs in the runtime registry/service lifetime boundary, dedicated wire-format header, user-client handlers/dispatch, app connector, and MCP catalog/schema/dispatch. UI decoding and statistics remain in the app.

Inspect `project.yml` source inclusion and add entries if required; regenerate via the normal build. Never edit the generated project. Update the existing latency ledger to identify this measured combined span and retain I1/I2 as unresolved separately.

## 12. Verification and acceptance

Host tests:

- Publication gaps, duplicate writes, partial-overlap suffixes, packet straddles, epoch changes, delayed receipts, history eviction, and same-frame rewrite rejection/versioning.
- Concurrent publication-history snapshots; verify no data races or torn identity/time tuples, with sanitizer coverage where the host target supports it.
- Both images carrying PCM, either image substituted, partial content, stale packet generation, unknown provenance, and failed transmission.
- Same-cycle publication/transmission crossing zero; impossible negative upper bound; conservative rounding, overflow, zero-valued valid timestamp, and eight-second wrap/ambiguous age.
- Correlation bracket and rate-age uncertainty propagation, including wider bounds for older correlations.
- Sampler stopped mid-write; stop/read/rearm races; outstanding page-reader ownership; timer expiry with no completions; endpoint teardown; all termination reasons; bounded retention.
- Sampling reproducibility, budget exhaustion, partial strata, DATA-only phase eligibility, exclusions, and counter conservation.
- Wire layout/version/cursor/size validation and complete pagination of one immutable result.

Swift tests: malformed/truncated pages, session mismatch, outcome decoding, signed wait handling, percentile bounds, empty/single/even/odd populations, CSV round-trip, and asynchronous start/stop status.

Run targeted tests first, then relevant integration/build checks. Inspect exit status and actual failures. Build with `--no-bump` when appropriate; no installation/reload or hardware reset without explicit user authorization.

Authorized hardware validation: 48 kHz, known client buffer, first idle playback then controlled load. Capture five seconds without competing diagnostic queries, fetch only after Frozen, inspect raw matches and exclusions. Repeat across stream stop/start and recovery only when those actions are explicitly authorized. Do not approve a baseline from low exclusion counts alone: manually cross-check selected-image identities and timestamp conversion for representative records.

Physical loopback is a separate corroboration experiment. Fix device/rate/client/routing, document the RTL tool's timestamp/reference semantics, and compare equivalent paths. Do not assert that every packet-ready wait sample or maximum must be less than a separately measured RTL median; the cohorts, client rendering time, and reference planes differ.

Acceptance: race-free lifetime, proven selected-image PCM association, conservative or explicitly conditional timing bounds, reproducible export, truthful population accounting, and no material regression in the normal audio path. Numeric results then inform the 48 kHz preset investigation; this feature itself does not change latency geometry.
