# Audio latency ledger and timing SSOT — plan

**September 7 ownership update:** frozen evidence identified retirement beyond
`mappedEnd`. The working tree now reads descriptor completion status, uses a
finite DMA chain, and retains the continuation anchor until its successor
completes. See [contract, verification and next bench step](TX_FINITE_QUEUE_OWNERSHIP.md)
and the [Instruments capture](reports/instruments-failure-2026-09-07/README.md).
The descriptor-replay interleaving is covered by a DMA-graph regression.
Coordinated recovery and the separate interrupt/AT-completion failure remain
open. The full host suite and DriverKit build passed; no installation or new
hardware measurement is claimed. Commit this verified ownership change before
mixing in the recovery work.

**Status: reconciled 2026-09-06 against `0cd228bf`.** The timing contract landed
in `d11c2fbf`, with its epoch-staleness correction in `1946d6ae`; alignment probes
landed in `d88e3feb`. Runtime tuning and the app panel are now implemented through
`0cd228bf`, but their effective readback and apply transaction need repairs.
Phase 0 and several Phase 3 repairs have landed.
The timing contract is host-side verification; runtime alignment/recovery and
the validated electrical baseline remain open. This is source status, not a
claim that the installed dext matches this checkout.

The original ledger reviewed `5e5991b1`; the first bench report used `dad154d5`.
Its geometry and measurements below retain that provenance unless updated
explicitly. The remaining problem is to distinguish measured intervals,
configured requirements and unexplained timing displacement before resolving
the declarations into one policy.

Three rules govern the ledger. Violating them is what produced the current mess:

1. **Adjacent physical intervals add.** A frame's journey is a chain, and
   consecutive spans of it sum normally. What must **never** be added is an
   *accounting allowance* on top of the physical interval it already covers —
   a safety offset is the guarantee that a write lands before a deadline, not a
   delay stacked on that deadline.
2. **An interval is defined by its start and end events, not by its name.** If
   an intermediate event is never observed, the spans on either side of it are
   **one** interval, not two — splitting it invents an attribution the
   measurement does not support.
3. **A configured frame count is not a measured duration.** Geometry constants
   describe nominal leads and batch contents. The actual delay distribution is
   a separate, currently unmeasured quantity.

The SSOT comes after. A single source of truth for numbers whose meaning is
undecided just centralises the ambiguity.

## Current decision — explain the delay, then reduce it

### Where we are

| Work | Current status | Remaining / dependency |
|---|---|---|
| Phase 0 — default safety | Landed: `2d1c584a` | Geometry default established; measured deadline margin still belongs to later work |
| Phase 1 — electrical baseline | Instrument hardened and several provisional runs captured | Stronger return, repeat-start provenance, correlated trace and reporting-only self-check |
| Phase 2 — reference plane | Open | Validated timing attribution from Phases 1/3/5; a residual alone does not select the plane |
| Phase 3 — instruments/progress | Payload arbitration, completion draining, live-pointer recheck, preflight/status, and Stage 1 TX metering ($E_0 \to E_2$) landed | RX capture dating ($F_4$), progress evidence and marker trace |
| Executable timing contract | Landed; 21 tests and six mutation cases documented after the epoch correction | Host-only; join packet/content evidence and validate timing inside a range, beyond its first-frame offset |
| Runtime tuning panel | Transmit-depth consumer wired; request/bridge/UI/logging added | Effective-state readback, publication safety and apply semantics fail review; declarations/HAL geometry are not applied |
| Phase 4 — resolution policy | Open; common safety defaults already derive from geometry | Final latency values wait for Phase 2; policy consolidation remains |
| Phase 5 — recovery/alignment | Defects identified; contract tests detect injected timeline slips | Runtime admission, coordinated epoch transition and atomic two-stream commit |
| Phase 6 — monitoring/acoustic | Open | Validated electrical baseline and stable timing; software monitoring remains unmeasured |

**Completed repairs are not the next task.** The lost-publication race
(`c912231e`), newest-only completion read (`e4464ae2`), stale CommandPtr recheck
(`c9e31d67`) and RTL preflight/transport-status projection (`f0e724fa`) are in
the tree. Later instrumentation commits through `f4a9bb84` improved coverage and
accounting but left the event-dating and snapshot defects listed in Phase 3.

### What the measurements and research establish

Retained evidence:
[initial investigation](reports/latency-investigation-2026-09-05/README.md),
[Phase 3 hardware report](reports/phase3-hw-validation-2026-09-05/README.md), and
[September 6 RTL follow-up](reports/latency-followup-2026-09-06/README.md).
Earlier reports describe their reviewed versions; their causal conclusions and
completion claims are superseded by this status and the current contract.

**Client buffer sizing behaved correctly in the bench runs.** At 48 kHz,
requested and observed callback sizes agreed. After the user restarted the
hardware, the 64 → 128 → 256 → 64 sweep measured 12.145 → 14.811 → 20.145 →
12.145 ms RTL: each buffer increase added twice that increase to raw RTL.
The configured 512-frame sizing budget is not a fixed callback batch.

**There is no stable absolute baseline yet.** September 6 runs returned about
84.145 ms raw RTL at 64 frames and, later, 8.811 ms twice at 128 frames. The
128-frame declarations predict 9.646 ms round trip and 5.104 ms output, matching
Logic's rounded 9.6/5.1 display. All four logs report the requested callback
span and no callback timing anomalies, but weak returns accepted only 6–11 of
20 trials. Installed binary and epoch continuity between those sessions were
not established. These are observations, not a controlled buffer-size comparison
or proof of an implementation improvement. The older ~355-frame remainder is
therefore not a demonstrated floor.

**The alignment probes found small relative divergence in one session.**
`3a133a09` records 43.7 s within one packet of the receive-derived projection,
an internally consistent seed calculation, and a same-session 20/20 accepted
RTL run at 34.9 dB SNR: raw 1158.95 frames / 24.145 ms, timestamp-domain 930.95,
measured/declared scheduling 228, residual +823.95. This narrows the search for
large *relative* drift in that run. It does not independently establish the
seed's physical correctness, exclude common-mode errors or close the cause of
other sessions. Preserve the raw trace, exact settings and loaded build with
the next comparison; the four retained follow-up logs above are separate runs.

**Prepared depth is now a testable variable, not an attributed RTL interval.**
120 packets is a 15 ms planning horizon (720 frames at 48 kHz), and the reported
seed lead was 716 frames. This agreement is not evidence that a marker waited
720 frames there: arming can precede PCM publication. Do not subtract 720 from
930.95 and call the remainder device/converter latency. The new panel can change
transmit depth through a requested ADK window without rebuilding the dext, but
the [runtime tuning review](reviews/runtime-tuning-2026-09-06/README.md) found
incorrect effective readback and apply/validation defects. Repair those before
using it for a controlled sweep. Declaration overrides and HAL geometry still
have no runtime consumer; a successful panel readback does not make them active.

**A 288-frame lattice is a clue, not a closed cause.** At 48 kHz, one 48-packet
ring traversal takes 6 ms when each cycle advances a packet and carries 288
frames on average. The September 6 scheduling-normalized results differ by
approximately 3744 frames, or 13 such traversals. A lost lap, a configured lead
and an impulse repeated by an unrefilled ring can share this spacing. A weak
correlator can select a different repeat. Neither the grid nor a seed log alone
distinguishes these explanations.

Two software sensitivities are independently demonstrated by the
[executable contract](AUDIO_TIMING_CONTRACT.md):

- A receive-derived TX seed shifts by 288 frames when its supplied presentation
  time is shifted by one synthetic lap. That does not prove that the real
  completion path supplies the error; both indices in an extrapolation must be
  traced, since a common index offset can cancel.
- After initialization, `PreviewTxRange` continues allocating consecutive
  content while accepting a shifted presentation time. Alignment therefore
  needs an invariant across the epoch, not only a corrected first seed.

**Progress is not known from a modulo pointer alone.** `ComputeDeltaConsumed`
returns 0–47 for the 48-packet ring, so `maxCompletionDelta > 48` cannot detect a
lost traversal. Elapsed cycles and `TxPacketIndexLift` supply a conditional
estimate; self-linked skips consume cycles without descriptor advance. Preserve
raw observations and justified progress bounds, and report ambiguity when more
than one lap fits. Neither elapsed time nor a nearest-lap guess establishes
actual execution by itself.

The [HAL timing research](COREAUDIO_HAL_TIMING_DOMAINS.md) and
[Apple driver mechanics](APPLE_DRIVER_TIMESTAMP_MECHANICS.md) are behavioral
references, not tuning prescriptions. Client buffers, descriptor rings and ZTS
periods are separate quantities. Apple's configured period does not validate
ASFW's measured anchor cadence or convergence. Do not copy an interrupt cadence
or startup gate across device families: completion grouping also changes
finality and RX service, and valid SYT/FDF/DBC behavior is capability-specific.

### Next implementation block — Phases 3 and 5 before more bench work

Pause further latency sweeps while the known implementation defects are being
repaired. The existing runs are enough to prioritize this work; repeating
them now cannot resolve the attribution gaps in the instruments.

1. **Make the tuning tool trustworthy.** Resolve the
   [runtime tuning review](reviews/runtime-tuning-2026-09-06/README.md): expose
   effective values separately from requests, disable unsupported groups,
   serialize request/snapshot publication, merge only selected groups, report
   terminal apply/abort outcomes, and fix validation and rate conversion. Add
   tests through the apply/readback path; the seven value-helper tests alone do
   not cover it. This is the next bounded change for rebuild-free experiments.
2. **Finish Phase 3 event evidence.** Date finality at the actual seal decision,
   retain bounded event history with a sound publication/read protocol, and
   remove the fallback that substitutes observer time for a missing event.
   Capture F4 at actual successful PCM publication. Port the existing review
   reproductions into permanent tests before treating these distributions as
   measured intervals.
3. **Implement runtime progress and presentation admission.** Feed a production
   adapter independently justified packet-progress bounds and a fresh
   frame/presentation relation with explicit uncertainty. Join execution and
   content identities and verify timing over the range; the current helper
   checks their progress separately and tests only the first-frame offset. The host contract
   already rejects ambiguous laps and shifted content; now make production
   correct or explicitly enter recovery. Do not turn a cycle estimate into
   exact evidence or impose a permanent nominal-rate slope.
4. **Complete Phase 5 recovery as part of that integration.** Quiesce and move
   RX, TX, PCM identities and pending observations to a coherent epoch; preserve
   the intended source/coordinate policy and make two-stream commit atomic.
   Test stalls, skipped cycles, old-record replay and secondary-commit failure.
   Matching epoch IDs in the host contract is not proof of atomic execution.
5. **Close startup admission and trace coverage.** Specify interrupted SYT
   warm-up behavior and profile-appropriate FDF/DBC/SYT validation, then test it
   through the actual consumer. Add the bounded marker trace below, including
   both the startup seed and later alignment. The current contract does not
   yet test that receive admission path.
6. **Batch integrated verification, then the bench.** Turn the contract's
   production defect detectors into conformance tests for the adapter, retain
   independent expectations and mutation checks, and verify the reference
   behavior before rebuilding/installing for the correlated electrical run.

Keep the work in reviewable changes. Return to hardware once this implementation
block is ready for integrated verification. An earlier targeted hardware check
is warranted only if an unresolved behavior blocks implementation and local
references or the hardware-free lab cannot settle it.

**Still deferred:** Phase 1's final baseline and self-check, Phase 2's
reference-plane decision, and Phase 4's final resolution policy and timing
values. Phase 3's measured distributions also remain open until the bench pass.
Do not mark those phases complete based on the new instrumentation alone.

### Next bench pass — one impulse traced across the path

Follow the same marker through **HAL write → PCM publication → TX packet
finalization/transmission → returned RX packet → capture publication → HAL
read**. Retain the epoch, absolute sample coordinates, and correlated host/bus
times. Record requested SYT presentation separately from observed transmission;
do not treat completion-handler execution time as the packet's wire time.

Use a bounded trace captured in memory and read after the run, with no per-packet
logging or new audio/transport layer coupling. Finish the Phase 3 endpoint and
snapshot repairs before trusting that trace. RTL setter/getter diagnostics and
the transport-status mirror are already implemented; remaining MCP liveness
checks need counter deltas and freshness rather than cumulative activity.

After the implementation block, repeat the trace across starts, keeping rate,
buffer size, cable, gain, and routing fixed and recording whether the event was
an audio stop/start, recovery,
or hardware restart. Improve the returned signal before accepting a baseline;
retain rejected trials and their reasons. Then perform the reporting-only
latency perturbation self-check described in Phase 1.

**Success:** locate the measured host-side waits and any frame/time displacement,
show which term changes between starts, and retain the unobserved device span
as an explicit unknown. That evidence must support a specific fix or identify
the next missing observation. Do not absorb the difference into declarations.

### Then reduce the known scheduling budget

At 48 kHz with 64-frame callbacks, measured scheduling is:

| Component | Frames | Duration |
|-----------|-------:|---------:|
| Input + output client buffers | 128 | 2.67 ms |
| Input + output safety offsets | 100 | 2.08 ms |
| Total scheduling distance | 228 | 4.75 ms |

This is the observed accounting split `RTL_raw − RTL_ts`, not an additional
delay to add on top of the physical path ledger. **A 2–3 ms RTL target requires
reducing this scheduling budget too.** After establishing the reference plane,
measure actual publication deadlines and capture visibility under load, then
reduce client size and safety only where those measurements support it. Record
raw RTL, trial acceptance, underruns, and recovery behavior at each setting.
Reporting-only latency changes correct compensation; they do not reduce
monitoring delay. No current result establishes that this hardware can meet
the target.

---

## Part 1 — The path ledger

All figures at 48 kHz. One isoch cycle = 125 µs; blocking AMDTP cadence D,D,D,N
gives 8 frames per DATA packet, **6 frames per packet averaged**. One bus tick =
1/24.576 MHz, so **512 ticks per frame**.

### 1a. TX physical intervals

Follow one frame. Events:

| Event | Meaning | Observed? |
|-------|---------|-----------|
| `E0` | HAL client writes the frame into the shared output buffer | publication bracket $[P_{\text{earliest}}, P_{\text{latest}}]$; observed across commit interval |
| `E1` | the packet carrying it reaches payload finality (`finalizedEnd` passes it) | decision exists; timestamp/history still need repair |
| `E2` | OHCI IT DMA transmits that packet onto the wire | per-packet completion evidence; absolute execution identity must be justified |
| `E3` | the packet is received by the device | **no — never observed** |
| `E4` | the device's nominal presentation instant for the frame (SYT) | stamped by us, not observed |
| `E5` | analog signal at the output jack | no |

| Interval | Span | Magnitude @48k | What we actually know | Composition & accounting |
|----------|------|---------------:|----------------------|--------------------------|
| `I1` | E0→E1 | **variable, ≥0** | Wait between the write landing and the packet freezing. Not fixed — set by how far ahead of finality the HAL scheduled the write. | Adds to `I2` normally. **Accounted by `A2`; do not add `A2` on top of it.** |
| `I2` | E1→E2 | **nominal 48 fr (1 ms)** | `kTxContentFreezeCycleSlots` = 8 pkt = `kPacketsPerCompletionGroup` 6 + `kPayloadRepointGuardPackets` 2. This is the **nominal frontier lead, not a per-packet duration** — packets cross finality at different phases within a completion group. Actual distribution **unmeasured**. | Guard component (12 fr) rests on an **unverified** 32-byte OHCI prefetch assumption. |
| `I3` | E2→**E4** | **stamped 105 fr** | `[TxLead]` = `sytOffset + txTransferDelayTicks` = 53876 ticks (min 105, max 111), applied to `transmitBusTicks`. It spans transmission→stamped presentation **as one quantity**: `E3` is never observed, so the wire/device split is unattributable. And it is what we *ask* the device to do, not what it does. | **Wire transit is already inside this.** Do not add a separate transit term — that was an overcount in the previous draft. |
| `I4` | E4→E5 | **unknown** | Device DAC. Not claimed by us. Not derivable from software geometry. | — |

### 1b. RX physical intervals

| Event | Meaning | Observed? |
|-------|---------|-----------|
| `F0` | analog signal at the input jack | no |
| `F1` | ADC emits the digital frame | no |
| `F2` | device transmits the packet carrying it | **no — never observed** |
| `F3` | packet received by host OHCI | yes |
| `F4` | decoded frame is successfully published as available in the capture ring | publication exists; current timestamp is taken later |
| `F5` | HAL client reads it | BeginRead hook exists; use actual read event, not scheduled hostTime |

| Interval | Span | Magnitude @48k | What we actually know | Composition & accounting |
|----------|------|---------------:|----------------------|--------------------------|
| `J1` | F0→F1 | **unknown** | Device ADC. Not claimed. Not derivable. | — |
| `J2` | F1→**F3** | **not separately observed** | `F2` is never observed, so ADC-to-transmit and wire transit are one unattributable span. `rxTransferDelayTicks` (12800) is a **configured constant used in timeline construction**, not a measurement of any device interval — it should not be read as "25 frames of device buffering". | — |
| `J3` | F3→F4 | **nominal: batch carries 32–40 fr** | One completion group (`kRxPacketsPerGroup` = 6 pkt, 0.75 ms). The 32–40 figure is **how many frames a batch contains**, not a bound on elapsed receive-to-decode time: dispatch delay is not included and is not bounded by geometry. Distribution **unmeasured**. | — |
| `J4` | F4→F5 | **variable, ≥0** | Capture-ring wait. **Real waiting when the reader lags the writer.** | Adds to `J3` normally. **Accounted by `A4`; do not add `A4` on top of it.** |

### 1c. Accounting terms — what we declare to CoreAudio

A *different object* from 1a/1b. Each is a declaration; only some also change
scheduling.

| Term | Value (Duet @48k) | Changes scheduling? | Intended to cover | Source |
|------|------------------:|---------------------|-------------------|--------|
| `A1` output latency | 67 | **No** — pure declaration | delay from the HAL timestamp plane to audible: part of `I3`+`I4` depending on the plane | asserted (Apple plist) |
| `A2` output safety offset | 50 | **Yes** — `SetOutputSafetyOffset` | the requirement that E0 precedes E1, i.e. bounds `I1` ≥ 0 | asserted (Apple plist) |
| `A3` input latency | 40 | **No** | `J1`+`J2`, and part of `J3` depending on the plane | asserted (Apple plist) |
| `A4` input safety offset | 50 | **Yes** — `SetInputSafetyOffset` | visibility margin: bounds `J4` ≥ 0 | asserted (Apple plist) |
| `A5` client IO buffer | client-specific; latest comparison 128 | **Yes** | accounted separately by CoreAudio | user-set |

At 128 frames, `(A5+A2+A1) + (A5+A4+A3)` = 463 frames / 9.646 ms, and
`A5+A2+A1` = 245 frames / 5.104 ms output: Logic's current rounded 9.6/5.1 ms.
The earlier 32-frame 5.6 ms display was 271 frames. These are sums of declarations
and scheduling allowances; neither display is an electrical measurement.

### 1d. Depths that are not latency at all

| Constant | Value | Why it is not latency |
|----------|------:|----------------------|
| `kTxPreparedTargetCycleSlots` | 120 pkt / 720 fr / 15 ms | Arm horizon. Packets are armed with **silence**; content overwrites them later. A late producer costs content rather than leaving an unarmed packet. Capacity alone does not establish playback delay. |
| ↳ `kTxOwnershipGuardCycleSlots` | 48 pkt | component of the above |
| ↳ `kTxDispatchSlackCycleSlots` | 72 pkt | component of the above |
| `kPcmPublicationCacheFrames` | 8192 fr (~170 ms) | Retention window for republication, not delay |

Restating this wherever the constants finally live is mandatory. Losing it is
how the arm horizon became latency the first time.

### What the ledger already shows

- **`A1` = 67 while `I3` alone stamps 105.** Whatever the reconciliation is, it
  is not "these describe the same thing."
- **`I4` and `J1` are not separately established.** Copied 67/40 declarations
  may have included converters in Apple's reference plane; their numerical
  equality does not establish the same coverage in ASFW. Electrical loopback
  includes both converters but cannot isolate them.
- **Two events are never observed** (`E3`, `F2`), so four spans collapse into
  two. Any future number that attributes delay across them is invented.
- **Four intervals need trustworthy distributions:** `I2`/`J3` have nominal
  geometry, while `I1`/`J4` vary with publication and reading. Instrumentation
  exists, but Phase 3's remaining event-dating defects prevent closing them.

---

## Part 2 — The blocking question

**Which plane is the HAL sample clock anchored in?**

The receive-clock path constructs a presentation coordinate and projects it
into host time. Relevant symbols, rather than stale line numbers:

```
DirectAudioReceiveConsumer::ConsumePacket  packetBusTicks + SYT offset + RX delay
HardwareSampleTimeline::Observe        project a boundary inside the packet
PublishHostClockAnchor                 publish the sample/host pair
UpdateCurrentZeroTimestamp             supply the pair to ADK
```

This establishes how the RX-derived timestamp is constructed. It does not
alone establish that TX content reaches its requested `E4` on that same
coordinate, or where either analog converter lies relative to it. TX-sourced
devices take a different path. The following is a conditional accounting model,
not a completed reference-plane decision.

| If the ZTS reference is… | `A1` should cover | `A2` bounds `I1` against a deadline of |
|---|---|---|
| `E4`, if the shared timeline matches TX presentation | `I4` only — the **unknown** DAC residual | `I2`+`I3` ahead of the reference |
| `E2`, transmission | `I3`+`I4` ≈ 105 + unknown | `I2` |
| Today | 67 | 50 |

**Note the residual.** Even under the presentation reading, `A1` is not zero —
it is the unknown `I4`. `E4` is a stamped instant at the device's decode point,
not the analog jack. Any exit criterion demanding a residual-free sum would force
someone to invent a number for `I4`.

The earlier clean 10 s acoustic capture does not validate the plane or identify
what supplied sufficient lead. Measure the publication/deadline relation before
concluding that a safety declaration must grow.

---

## Part 3 — Phases

### Phase 0 — default safety repair (landed)

The former generic AVC input default was 16 frames after `5d1ea98c` removed an
input floor. **Fixed in `2d1c584a`:** `AddDefaultTiming` derives both defaults from
`AudioGeometryPolicy::CompletionBatchFrames(rate)` — 48/96/192 frames at
48/96/192 kHz. The Duet still supplies 50/50 at 48 kHz. This is a geometry
default, not a measured bound on dispatch delay or proof of input visibility.
Reported latency remains 32/32 in the common builder and requires Phase 2
attribution. Do not re-open this completed literal-removal task as the next fix.

### Phase 1 — uncompensated electrical baseline

Instrument and provisional measurements exist; final acceptance remains open.

Measure round-trip **electrically** (output→input cable), with **no DAW in the
measurement path**. It covers the physical waits in `I1`…`I4` and `J1`…`J4`,
including both converters. Scheduling affects those waits; do not add its
accounting sum to the already measured raw RTL. The old choice between 270 and
541 frames inferred from compensated Logic captures is superseded by the
electrical runs and their explicit admission/provenance limits.

**Bench self-check.** Vary a **reporting-only** field — `outputLatencyFrames` or
`inputLatencyFrames`, which feed `SetOutputLatency`/`SetInputLatency` and change
no scheduling — by a known amount and confirm the measurement does not move. Do
**not** probe with safety offsets or the client buffer: those feed
`SetOutputSafetyOffset`/`SetInputSafetyOffset` and legitimately change physical
timing, so a moving measurement would prove nothing.

The baseline chain is `OxfwProfileBuilder`'s output latency 67 at 48 kHz through
the resolved profile to `SetOutputLatency` in `ASFWAudioDriverGraph`. Only output
*safety* passes through `RequiredOutputSafetyFrames`. The tuning panel does not
yet apply declaration overrides to that graph, so it cannot currently perform
this self-check despite reporting the requested values back as active. Complete
that path or use a verified reporting-only build change for the test. Match
restart conditions in both runs so a re-arm effect is not blamed on the field.

Note what the self-check may and may not assert. Since IOProc timestamps do not
carry hardware latency, moving this field must leave **both** `RTL_raw` and
`RTL_ts` unchanged, and move only the declared figure — and hence the residual.
An expectation that `RTL_ts` tracks the field would be wrong.

**Instrument — `tools/rtl/rtl_loopback.c`** (built, offline-verified). Emits
impulses through a HAL IOProc. Its arithmetic follows Apple's contract, not our
own: IOProc timestamps carry the safety offset but **not** hardware latency
(Moore, coreaudio-api 2002/Aug/msg00055; confirmed 2004/Oct/msg00266), so the
two halves separate cleanly.

- `RTL_raw` — frames between writing a sample into an output buffer and seeing it
  in an input buffer, counted by accumulating each callback's frame count. It
  never reads `mSampleTime`, so it is immune to the reporting-only fields. It is
  the whole thru time. **The exit number.**
- `RTL_ts` — the same event pair in the sample-time domain. With a validated
  reference plane and truthful timing, it represents
  `in_hw_latency + out_hw_latency`, converters included. Until that contract is
  established, call it the **remaining path delay after measured scheduling**;
  the tool's hardware-latency label does not prove hardware attribution.
- `RTL_raw − RTL_ts` — the scheduling distance, reconciling with
  `2×io + safety`. It carries no latency term and therefore proves nothing about
  whether a declared latency reached the HAL.
- **`RESIDUAL` = `RTL_ts` − declared hardware latency.** A signed discrepancy
  to explain. It informs Phase 2 after timestamp/content alignment and detector
  identity are validated; alone it neither chooses the plane nor proves a
  converter/device delay.

Trial admission fails closed (`cd5568d6`). A gap, ambiguous timing, re-anchor,
missing timestamps, or absent signal rejects the whole trial. Only `Accepted`
reaches any aggregate; `RTL_raw` and `RTL_ts` use identical populations, with
scheduling differences computed per accepted trial. Re-anchored trials are
not salvaged for raw RTL. The shared hardware/simulator engine is exercised
with independently injected faults, including combined drops and re-anchors.

`--selftest` covers the analysis without hardware: known delays recovered exactly
at integer positions and within 0.16 frames at fractional ones, an empty window
rejected rather than fitted to noise, a zero residual retained rather than
mistaken for a missing value, and a varying callback size not read as a timeline
break.

**Bench status:** the initial sweep, September 6 follow-up and separately
reported alignment session are summarized above. There is no accepted universal
12.145 ms baseline. Remaining work is the correlated trace and repeat-start
comparison, a strong identifiable return, exact effective configuration and
loaded-build provenance, and the reporting-only self-check. Procedure, setup, and invalidation
conditions are in [`tools/rtl/README.md`](../tools/rtl/README.md).

**Exit:** an absolute electrical RTL figure from a path with no compensation,
validated by the self-check.

### Phase 2 — decide the reference plane

Resolve Part 2 using the validated baseline and event attribution, not just one
residual. Deliverable is a written decision
plus the code change making one plane true everywhere: either move the ZTS
anchor to `E2` and declare `I3` as latency, or keep `E4` and drop it from `A1`.

Do not ship a compensating constant in one place and leave the other.
`AUDIOENGINEV3_DRAFT.md`'s "Safety offset — what belongs in it" is the decision
record's home.

**Exit:** every interval in 1a/1b is attributed to exactly one side of the
reference plane, and each accounting term in 1c names the intervals it covers.
**Documented unknowns (`I4`, `J1`) and unattributable spans (`I3`, `J2`) are
permitted and expected** — the criterion is completeness of attribution, not a
residual-free sum.

### Phase 3 — repair and extend the instruments

Ordered by how badly each corrupts measurement. Items 1–3 from
`documentation/reviews/audioengine-v3-2026-09-05/`.

1. **Lost-publication race** (finding 4). **Landed in `c912231e`.**
   `PublishLatePayload` re-read `finalizedEnd` *before* the scan stored it, so
   the return value that exists to report a lost race could not fire in the
   window it guarded. It corrupted `filled`-vs-silence attribution — the
   telemetry Phases 2 and 6 read.

   A generation-tagged arbitration word per packet is now the single
   linearization point: the producer may only offer, transport may only bind or
   seal, and both move it by CAS. Transport seals every packet it abandons,
   per packet and before publishing the frontier, walking the frontier delta so
   a command pointer that jumps more than a completion group leaves nothing
   undecided. The frontier is now advisory telemetry rather than the decision.

   Because a producer cannot know at call time whether its image will transmit,
   `latePayloadLostPublicationCount` counts accepted publications that transport
   then sealed on the armed image, and `[TxFill]` reports `lost=`. **The ledger
   must read `filled − lost`, not `filled`, for the same epoch and finalized
   cohort; pending offers are not proof of transmitted content.** One behaviour change to note when
   reading older captures: `rejected=` now counts rejected packets rather than
   rejection attempts, because a packet rejected on geometry is sealed instead
   of retried once per pass.
2. **Newest-only completion read** (finding 3). **Landed in `e4464ae2`.** It
   published 2 ZTS boundaries where per-packet observation publishes 12, on the
   TX-sourced timeline, and corrupted the clock the ledger is written against.
   The observer now keeps a cursor and drains every completion stamp it owes;
   the cursor arithmetic is `TxCompletionStampDrain`, which rewinds on a re-arm
   and reports rather than reads stamps that aged out of the ring. The M-Audio
   path stays one observation per wake by design — its warm-up machine counts
   wakes — but is given the wake's newest DATA packet, so a trailing NO-DATA no
   longer hides the audio beside it.

   **Reading older captures:** the completion-latency histogram and cycle trace
   now record per packet rather than per wake, so that distribution is not
   comparable across `e4464ae2`.
3. **Stale CommandPtr at rebind** (finding 1). **Landed in `c9e31d67`.**
   `minRebindDistance` was sampled before completion processing and the whole
   scan, so it overstated the margin it reported. Transport now re-reads the
   live command position immediately before the descriptor store, abandons the
   rebind if the controller has reached the packet, and reports the distance it
   actually verified. A missed deadline is permanent for that packet — the
   controller only moves forward — so it seals on the armed image rather than
   being retried and re-counted, and the producer's image is booked as a lost
   publication. A failure to read MMIO decides nothing and leaves the packet
   open, so a transient loss of access cannot discard content.

   **This addresses snapshot age only.** Controller prefetch and controller
   progress between the recheck and the store remain separate, unresolved
   concerns — `I2`'s guard component still rests on an unverified assumption
   after this fix, and settling it needs reference or hardware evidence. The
   review fixture for writing that reproduction is now
   `IsochTxPayloadArbitrationTest`.
4. **Measure the intervals the ledger marks nominal.** **Stage 1 (TX metering $E_0 \to E_2$) in tree (bench verification pending); RX capture dating ($F_4$) remains open.**
   Prior distribution additions (`9740d4b5`, `f5779a51`, `3a6cf466`, `f4a9bb84`)
   attempted to measure $I_1$ ($E_0 \to E_1$) and $I_2$ ($E_1 \to E_2$) using an
   intermediate "finality seal" ($E_1$), which was prone to stale sampling,
   torn multi-field reads across queues, and ungrounded observer timestamps.

   **Stage 1 Resolution ($E_0 \to E_2$ Direct Metering):**
   Rather than attempting to date a phantom intermediate seal, Stage 1 directly
   measures the true span from complete PCM payload readiness ($E_0$) to hardware
   wire transmission bounds ($E_2$):
   - **$E_0$ Dating:** `PerformIO` records the exact host timestamp and playback
     ring frame range for every committed output block in `publicationHistory`
     (`PublicationRangeRing`, a 256-slot lock-free atomic ring).
   - **$E_2$ Hardware Ground Truth:** Extracted from the OHCI descriptor
     `OUTPUT_LAST` writeback `hwTimestamp` (`sec[2:0]:cycle[12:0]`) on packet
     completion. Mapped to host time using the correlated `IsochTxClockPairSample`
     (`cycleTimer` / `hostTimeMid`) with explicit bracket width `bracketTicks`.
   - **4 Uncertainty Terms ($U$):** Every measurement reports an exact nominal
     duration $\Delta t$ and an uncertainty interval:
     $$U = U_{\text{bracket}} + U_{\text{granularity}} + U_{\text{round}} + U_{\text{drift}}$$
     where $U_{\text{bracket}} = \pm \text{bracketTicks} / 2$, $U_{\text{granularity}} = \pm 62.5\,\mu\text{s}$ (1394 cycle),
     $U_{\text{round}} = \pm 0.5\,\mu\text{s}$, and $U_{\text{drift}} = \text{duration} \times \text{driftPpm} \times 10^{-6}$.
   - **Payload-Opaque Provenance:** Transport remains strictly payload-opaque.
     `PacketTimelineSlot` carries `ImageProvenance images[2]` (populated in
     `PrepareTransmitSlot` and `FillTransmitSlot`), and completion metadata packs
     the active image index (`selectedPayloadImage`). The audio completion
     observer correlates the packet's playback range with `publicationHistory`
     to establish the exact $E_0$ origin.
   - **Non-Interfering Session Capture:** Stratified pseudo-random sampling
     ($S \in [16, 64]$) captures up to 4096 samples without lock contention or
     allocations. A quiescent drain protocol (`writerActive` sequence) prevents
     races between the realtime audio observer and control plane reads.
   - **Diagnostics & Tooling:** UserClient selectors 1036/1037 wire pagination
     (32 samples / 2768-byte pages), decoded in Swift (`DriverConnector+TxLatency`),
     presented in the ASFW app (`TxLatencyView`) with range statistics and CSV
     export, and exposed via MCP tools (`asfw_start_tx_latency_session`,
     `asfw_stop_tx_latency_session`, `asfw_get_tx_latency_results`).

   | Evidence defect | Current status | Required repair |
   |---|---|---|
   | $E_1$ seal record receives stale `refillCycleTimer` | **Addressed in tree (bench pending)** | Bypassed: direct $E_0 \to E_2$ measurement renders intermediate $E_1$ unnecessary for TX latency bounds |
   | $I_1$ / $I_2$ observer timestamp fallback | **Addressed in tree (bench pending)** | Bypassed: strict classification rejects ungrounded samples without manufacturing endpoints |
   | Latest-only seal race / torn reads | **Addressed in tree (bench pending)** | Replaced by `PublicationRangeRing` + `IsochTxClockPairSample` atomic bracket |
   | $F_4$ sampled after processing, not at PCM publication | **Open** | Timestamp actual successful `PublishProducedEnd`; do not date logical progress without PCM as availability |
5. **Make bench preflight and transport status trustworthy.** **Landed.**

   `txTransportStatus` had a live producer all along and simply was not being
   written: the value-owned transport mirror refreshes the completion cursor and
   committed end on every preparation pass and omitted the status beside them,
   so MCP read the initialisation value and reported `stopped` while the cursors
   it sits next to advanced. It now mirrors `IsochTxQueueStatus`, which shares
   the diagnostic encoding's numbering.

   RTL preflight now reports both directions. A buffer request that is refused
   or silently clamped is named before any measurement (the Duet clamps 7 to
   15), and a declared property that will not read is listed as `NOT READ,
   counted as 0` — with `RESIDUAL` withheld rather than computed, because a
   residual taken against an incomplete declaration is wrong in a way that looks
   exactly like an answer. The measurements themselves still stand.

   `[TxFill]` now also carries `missedDeadline=` and `stampsMissed=`.

   **Still open in this item:** MCP judges progress from cumulative activity
   rather than counter deltas and freshness. **And a gap worth knowing before
   the bench pass:** the new counters are readable only in the log, not in the
   MCP snapshot — the Swift telemetry decoder uses a hand-written byte-offset
   table with no assertion tying it to the C++ layout, so extending it is a
   separate, deliberate change. Capture `[TxFill]` during the hardware pass.

Use these repairs to support the correlated marker trace in the current
decision above. Aggregate histograms remain useful, but cannot attribute a
particular impulse or a displacement introduced at startup.

**Exit:** `[TxFill]` counters and `minRebindDistance` mean what they say, and
every "nominal" cell in Part 1 has a measured distribution beside it.

### Phase 4 — one resolution policy

The goal is **one authoritative resolution path**, not literally one header.
Constants stay layered where they belong — transport geometry in
`IsochQueueGeometry`, audio geometry in `AudioTimingGeometry` — because
collapsing them into one file would breach the transport/audio boundary that
CLAUDE.md makes load-bearing. What must be singular is the *policy that turns
them into declared values*.

**Resolver, not passthrough.** An `std::optional` per field distinguishes unset
from specified:

```cpp
struct RateTimingPolicy {
    std::optional<uint32_t> inputSafetyFrames;   // nullopt = derive
    std::optional<uint32_t> outputSafetyFrames;
    std::optional<uint32_t> inputLatencyFrames;
    std::optional<uint32_t> outputLatencyFrames;
};
```

But **a specified value does not bypass the implementation's requirement.** An
Apple-derived safety offset may be below what ASFW's transport actually needs —
exactly the Phase 2 question. So the resolver *validates or combines* profile
intent with transport requirement, logging which fields were derived, which
asserted, and which raised. That is still one authoritative path; it is not the
"second floor" this refactor removes, because the combination happens in one
place under one rule.

Two limits to encode explicitly:

- **`nullopt` cannot mean "derive" for converter-inclusive fields.** `I4` and
  `J1` are not obtainable from software geometry. A latency field meant to
  include them is measured, vendor-documented, or explicitly declared
  incomplete — never derived.
- **Rule 1 is a resolver rule, not a comment.** `A2` accounts for `I1`; adding
  `A2` on top of `I1` is the double-count. The resolver should make that
  unrepresentable.

**Wire up what is already written.** `AudioGeometryPolicy` already contains a
complete generic per-rate ladder — `TxSafetyOffsetFrames` (48 @48k),
`RxSafetyOffsetFrames` (128), `ReportedLatencyFrames` (29) — plus
`RequiredOutputPublicationFrames`. **None has a production caller.** They are
referenced only by their own static_asserts, and look live in a grep only
because `ResolvedAudioStreamProfile` has accessors with identical names reading
the profile table instead. Make them the derivation path or delete them.
The header as a whole is live: `CompletionBatchFrames` now supplies common
safety defaults, and `RequiredOutputSafetyFrames` is used by the graph.

**Consolidate the five builders**, which today each restate everything:

| Builder | in/out latency, in/out safety | Style |
|---|---|---|
| `CommonProfileBuilder::AddDefaultTiming` | latency 32/32; safety 48/48 @48k, 96/96 @96k, 192/192 @192k | common safety derived from `CompletionBatchFrames` |
| `OxfwProfileBuilder` | 46/55/46/46 @44.1, 40/67/50/50 @48 | literals, base rates only |
| `DiceProfileBuilder` | 29/59/119; `(16+a)*fpp`, `(6+a)*fpp` | derived |
| `BeBoBProfileBuilder` | 128/128/64/64 | literals + M-Audio special |
| `MAudioSpecialTiming` | `roundTrip/2` | derived from a round-trip figure |

Afterwards each declares only its deviations, every asserted value carrying a
provenance comment (measurement, vendor plist, reference stack).

**Exit:** one resolver; every constant traceable to a ledger interval; no second
floor; rule 1 enforced in code; no dead ladder.

### Phase 5 — recovery coordination

Open runtime work. Policy refactoring can proceed independently, but credible
final values and integrated timing verification require coherent recovery.

- **Epoch transition** changes the sample origin via
  `NextBoundaryAfter(lastBoundary)` without translating RX cursors or queued TX
  ranges — a 7384-frame jump in the repro. Needs one recovery transaction that
  either preserves the physical coordinate or resets every participant.
- **Two-stream commit is not atomic** — the secondary `CommitFill` result is
  `(void)`-discarded under a comment promising all-or-nothing. Source-level
  finding; not reproduced on a dual-stream device.
- **Progress ambiguity and presentation alignment:** a modulo cursor cannot
  identify whole traversals at startup or after a stall. `[TxLapSeed]` is a
  conditional estimate, and `[TxSeed]`/`[TxAlign]` expose relative coordinates;
  neither establishes physical progress by itself. The executable contract
  demonstrates seed sensitivity and later presentation-slip admission in the
  real class, but does not prove either caused the measured RTL.

  Preserve evidence sufficient to identify one absolute execution position or
  declare ambiguity and enter coordinated recovery. Do not rewrite only the
  completion count: descriptor generations, queued content, PCM identities and
  published anchors must agree. Repeated payload already transmitted cannot be
  undone by relabelling a counter. Whether it occurred in a particular run must
  come from the trace, not the lattice.
- **Startup admission:** FDF is parsed without a configured-rate comparison,
  DBC is accumulated without a continuity gate, and interrupted SYT warm-up
  retains cumulative history. Specify the supported profile's policy and add
  consumer-level tests; do not impose Apple AV/C checks blindly on every family.

**Exit:** runtime conformance under delayed/missing progress, coherent recovery
across participants, no partial two-stream commit, and validated startup
evidence. Agreement checks in a single-threaded host helper are not proof of
the cross-service transaction.

### Phase 6 — remaining measurement

With the electrical baseline established in Phase 1:

- **Acoustic measurement** adds speaker, air, and microphone to the electrical
  path. Acoustic minus electrical isolates **speaker/air/mic — not the
  converters**, which the electrical loopback already contains. `I4` and `J1`
  are not separable this way; they need vendor data or a reference device of
  known latency.
- **Software monitoring** — input → turnaround → output. Never measured, and it
  is the path actually complained about. A single capture containing both the
  physical event and the monitored output is the cheapest form.

**Exit:** each unknown, unattributable, or nominal cell in Part 1 is measured,
sourced, or explicitly accepted as a property we do not claim.

---

## Ordering rationale

Phase 0 and Phase 3's first three repairs have landed. The immediate supporting
task is to repair runtime tuning's effective readback and transaction so it can
support rebuild-free experiments. Then finish Phase 3 event evidence and the
Phase 5 runtime contract/recovery integration described above. Defer further
routine hardware metering until the relevant fixes and trace support are ready,
then batch their hardware verification with the Phase 1 repeat-start baseline
and reporting-only self-check. Phase 5 addresses established recovery defects;
it is not yet proven to explain the observed latency difference.

Phase 2 waits for that validated baseline and timing attribution; Phase 4 follows
the plane decision and trustworthy instruments. Phase 6 requires the electrical
baseline. Reduce the scheduling budget against measured deadlines after these
timing contracts are established.
