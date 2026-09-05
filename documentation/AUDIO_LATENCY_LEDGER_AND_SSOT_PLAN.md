# Audio latency ledger and timing SSOT — plan

**Status:** plan, updated 2026-09-05 with provisional Duet electrical measurements.
The original ledger reviewed branch `ctrls` at `5e5991b1`; the bench investigation
reviewed `dad154d5`. Phase 0 has landed; Phase 1 remains open.
**Premise:** every latency number in this driver is currently either a
structural constant nobody can trace to a physical stage, a measurement of one
stage presented as if it covered the path, or a literal copied from another
driver. The fix is not a better constant. It is a *ledger*.

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

Evidence and retained run logs:
[Duet latency investigation, 2026-09-05](reports/latency-investigation-2026-09-05/README.md).

**Client buffer sizing behaved correctly in the bench runs.** At 48 kHz,
requested and observed callback sizes agreed. After the user restarted the
hardware, the 64 → 128 → 256 → 64 sweep measured 12.145 → 14.811 → 20.145 →
12.145 ms RTL: each buffer increase added twice that increase to raw RTL.
The configured 512-frame sizing budget is not a fixed callback batch.

**The next investigation is startup timing and accumulated queue delay.** An
earlier session measured 54.145 ms at the same 64-frame client size, about
42 ms more. Within the restarted session, subtracting measured scheduling
leaves approximately 355 frames / 7.40 ms at every buffer size. Against the
107 declared latency frames, that leaves approximately 248 frames / 5.17 ms
unexplained. Neither remainder is established hardware latency, and a restart
changing the result does not locate the cause in ASFW versus interface internals.
The fresh runs have weak SNR and no-signal rejections; the reporting-only
self-check is still outstanding. These are investigation inputs, not a completed
Phase 1 baseline or a reason to change a latency declaration.

### Next implementation block — Phases 3 and 5 before more bench work

Pause further latency sweeps while the known implementation defects are being
repaired. The existing runs are enough to prioritize this work; repeating
them now cannot resolve the attribution gaps in the instruments.

1. **Phase 3 correctness repairs:** fix the lost-publication race first, then
   completion-history draining and CommandPtr freshness. Repair the RTL
   preflight and transport-status projection as well.
2. **Phase 5 recovery coordination:** implement a coherent epoch transition
   across RX, TX, and published PCM, and make the two-stream commit contract
   atomic. These fixes do not depend on choosing the latency reference plane.
3. **Phase 3 trace support:** implement the bounded marker trace below and the
   interval measurements, so the next hardware run can locate the delay.
4. **Verify the implementation before returning to hardware:** use reference
   review, host tests, and simulators to exercise publication/finality races,
   completion ordering, epoch transitions, and secondary-stream failure. These
   checks prepare the bench pass; they do not establish actual DMA timing or
   device behavior.

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
logging or new audio/transport layer coupling. Repair the Phase 3 instruments
that this trace relies on, keeping the lost-publication race first. Two further
gaps from the bench review belong in that work: the RTL tool must surface
buffer setter/getter failures, and the transport-status projection needs a live
producer before its default `stopped` label can establish transport state.

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
| `E0` | HAL client writes the frame into the shared output buffer | yes |
| `E1` | the packet carrying it reaches payload finality (`finalizedEnd` passes it) | yes |
| `E2` | OHCI IT DMA transmits that packet onto the wire | yes |
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
| `F4` | frame decoded into the capture ring by the completion batch | yes |
| `F5` | HAL client reads it | yes |

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
| `A5` client IO buffer | 32 | **Yes** | accounted separately by CoreAudio | user-set |

Logic's "5.6 ms roundtrip" is `(A5+A2+A1) + (A5+A4+A3)` = 271. **A sum of
accounting terms, not of physical intervals**, displayed because we declared it.

### 1d. Depths that are not latency at all

| Constant | Value | Why it is not latency |
|----------|------:|----------------------|
| `kTxPreparedTargetCycleSlots` | 120 pkt / 720 fr / 15 ms | Arm horizon. Packets are armed with **silence**; content overwrites them later. A late producer costs content, never a holed ring. **Largest depth in the system.** |
| ↳ `kTxOwnershipGuardCycleSlots` | 48 pkt | component of the above |
| ↳ `kTxDispatchSlackCycleSlots` | 72 pkt | component of the above |
| `kPcmPublicationCacheFrames` | 8192 fr (~170 ms) | Retention window for republication, not delay |

Restating this wherever the constants finally live is mandatory. Losing it is
how the arm horizon became latency the first time.

### What the ledger already shows

- **`A1` = 67 while `I3` alone stamps 105.** Whatever the reconciliation is, it
  is not "these describe the same thing."
- **`I4` and `J1` are unknown and unclaimed.** Apple's 67/40 presumably include
  their converters; ours do not. Our numbers and Apple's are therefore **not the
  same quantity** even where numerically equal — which undercuts "parity with
  Apple" as a correctness target.
- **Two events are never observed** (`E3`, `F2`), so four spans collapse into
  two. Any future number that attributes delay across them is invented.
- **Three intervals are nominal geometry, not measured** (`I2`, `J3`, plus the
  variable pair `I1`/`J4`). Their distributions are Phase 3 work.

---

## Part 2 — The blocking question

**Which plane is the HAL sample clock anchored in?**

Verified in code; no compensation anywhere along the chain:

```
DirectAudioReceiveConsumer.cpp:467  presentationBusTicks = packetBusTicks
                                        + sytOffset + rxTransferDelayTicks
HardwareSampleTimeline.hpp:292      boundaryBusTicks = presentationBusTicks + …
AudioTransportControlBlock.hpp:839  PublishHostClockAnchor(…)   ← pass-through
ASFWAudioDriverZts.cpp:372          UpdateCurrentZeroTimestamp(sampleFrame,
                                                               hostTicks)
```

So the HAL's "now" is the **stamped presentation** instant (`E4`), while
`finalizedEnd` is anchored on the DMA command pointer (`E2`). Two planes, one set
of numbers.

| If the ZTS reference is… | `A1` should cover | `A2` bounds `I1` against a deadline of |
|---|---|---|
| `E4`, stamped presentation (what the code does) | `I4` only — the **unknown** DAC residual | `I2`+`I3` ahead of the reference |
| `E2`, transmission | `I3`+`I4` ≈ 105 + unknown | `I2` |
| Today | 67 | 50 |

**Note the residual.** Even under the presentation reading, `A1` is not zero —
it is the unknown `I4`. `E4` is a stamped instant at the device's decode point,
not the analog jack. Any exit criterion demanding a residual-free sum would force
someone to invent a number for `I4`.

One caveat that must survive: the shortfall implied by the presentation reading
is a **model**. The 10 s acoustic capture on this exact configuration was clean —
no dropouts, no drift, no silent runs. Something absorbs it, most plausibly the
HAL's published frontier running further ahead than `A2` alone requires.
**Decide the plane; do not assume the number must grow.**

---

## Part 3 — Phases

### Phase 0 — live bug, do immediately

**Generic AVC input safety is 16 frames.** `GenericAvcProfileBuilder.cpp:20`
calls `AddDefaultTiming` and sets nothing after, keeping `inputSafetyFrames =
16`. Commit `5d1ea98c` removed the floor that previously raised it to 128.

16 frames is below the frame count a single completion batch carries (`J3`,
32–40), so the declared visibility margin does not cover the interval it exists
to cover. Whether that *manifests* as a glitch depends on capture position
relative to the chosen ZTS reference — stated as unjustified, not as a predicted
failure.

Output is unaffected (`RequiredOutputSafetyFrames` still floors 16 → 48); the
Duet is unaffected (sets 50 explicitly), which is why the suite stayed green.

**Exit:** a generic-AVC profile test asserts input safety ≥ one completion
batch, with the requirement expressed once rather than as a second floor.

**Landed** in `2d1c584a`: `AddDefaultTiming` derives both safety defaults from
`AudioGeometryPolicy::CompletionBatchFrames(rate)` — 48/96/192 frames at
48/96/192 kHz — giving the first real caller of that header. Reported latency
was deliberately left at 32/32; it needs the Phase 2 plane decision before it
means anything.

### Phase 1 — uncompensated electrical baseline

First among the real work: depends on nothing, and constrains Phase 2.

Measure round-trip **electrically** (output→input cable), with **no DAW in the
measurement path**. This yields one number covering `I1`…`I4` and `J1`…`J4` plus
the scheduling terms — **including both converters**, since the signal passes
DAC then ADC.

It immediately settles the ambiguity left by the Logic captures: physical RTL is
either ~270 samples (5.6 ms) or ~541 (11.3 ms), and only an uncompensated
measurement can say which. The 4.2× improvement over baseline is robust either
way; the absolute is not, and 11.3 ms would be audible for monitoring.

**Bench self-check.** Vary a **reporting-only** field — `outputLatencyFrames` or
`inputLatencyFrames`, which feed `SetOutputLatency`/`SetInputLatency` and change
no scheduling — by a known amount and confirm the measurement does not move. Do
**not** probe with safety offsets or the client buffer: those feed
`SetOutputSafetyOffset`/`SetInputSafetyOffset` and legitimately change physical
timing, so a moving measurement would prove nothing.

Verified chain for the probe field: `OxfwProfileBuilder.cpp:72` sets the Duet's
48 kHz `outputLatencyFrames = 67`, which reaches `SetOutputLatency` untouched at
`ASFWAudioDriverGraph.cpp:686`. Only output *safety* passes through
`RequiredOutputSafetyFrames`, so this field is purely a declaration.

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
- **`RESIDUAL` = `RTL_ts` − declared hardware latency.** The signed amount by
  which our declarations misstate the physical path. **This feeds Phase 2
  directly** — a measured answer to the reference-plane question, where Part 2
  offers only a model.

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

**Bench status:** electrical measurements and a buffer sweep are recorded in the
[investigation](reports/latency-investigation-2026-09-05/README.md). The fresh
64-frame result is provisionally 12.145 ms; the earlier 54.145 ms result and the
weak return signal prevent treating it as a stable baseline. Remaining work is
the correlated trace and repeat-start comparison above, a stronger return
signal, and the reporting-only self-check. Procedure, setup, and invalidation
conditions are in [`tools/rtl/README.md`](../tools/rtl/README.md).

**Exit:** an absolute electrical RTL figure from a path with no compensation,
validated by the self-check.

### Phase 2 — decide the reference plane

Resolve Part 2, informed by Phase 1's number. Deliverable is a written decision
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
   must read `filled − lost`, not `filled`.** One behaviour change to note when
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
3. **Stale CommandPtr at rebind** (finding 1). **Landed in the same series.**
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
4. **Measure the intervals the ledger marks nominal.** **Instrumented; awaiting
   the bench run for the distributions themselves.** All four now report on the
   `[Ledger]` heartbeat, each line carrying sample count, unresolved count, min,
   mean, max and an eight-bucket µs ladder starting at one isoch cycle.

   | Interval | Endpoints observed at | Domain |
   |---|---|---|
   | `I1` E0→E1 | `WriteEnd` publication → frontier crossing at the TX observer | host |
   | `I2` E1→E2 | frontier crossing → the packet's own completion timestamp | bus |
   | `J3` F3→F4 | the RX drain's back-dating of the packet, which *is* the elapsed time | host |
   | `J4` F4→F5 | capture decode → `BeginRead` for the newest frame requested | host |

   Two things to read carefully. `I2` starts at the frontier crossing **as
   observed**, so the observer's own dispatch lag is inside it deliberately —
   that lag is part of how late a content decision is and geometry does not
   bound it. And `unresolved` is part of each measurement, not an error channel:
   it counts samples whose first endpoint aged out of the stamp ring or arrived
   out of order, so a thin histogram can never be read as a well-behaved
   interval that merely occurred rarely.

   These are **not** anomaly-gated, unlike the fault telemetry: a distribution
   that only appears once it is already bad cannot establish what normal looks
   like, which is the single thing these exist to do. They ride the existing
   coarse heartbeat and add no per-packet logging.
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
`RequiredOutputPublicationFrames`. **Nothing calls any of them.** They are
referenced only by their own static_asserts, and look live in a grep only
because `ResolvedAudioStreamProfile` has accessors with identical names reading
the profile table instead. Make them the derivation path or delete them.

**Consolidate the five builders**, which today each restate everything:

| Builder | in/out latency, in/out safety | Style |
|---|---|---|
| `CommonProfileBuilder::AddDefaultTiming` | 32/32/16/16 | base for all |
| `OxfwProfileBuilder` | 46/55/46/46 @44.1, 40/67/50/50 @48 | literals, base rates only |
| `DiceProfileBuilder` | 29/59/119; `(16+a)*fpp`, `(6+a)*fpp` | derived |
| `BeBoBProfileBuilder` | 128/128/64/64 | literals + M-Audio special |
| `MAudioSpecialTiming` | `roundTrip/2` | derived from a round-trip figure |

Afterwards each declares only its deviations, every asserted value carrying a
provenance comment (measurement, vendor plist, reference stack).

**Exit:** one resolver; every constant traceable to a ledger interval; no second
floor; rule 1 enforced in code; no dead ladder.

### Phase 5 — recovery coordination

Review findings 5 and 6. Neither blocks Phase 4.

- **Epoch transition** changes the sample origin via
  `NextBoundaryAfter(lastBoundary)` without translating RX cursors or queued TX
  ranges — a 7384-frame jump in the repro. Needs one recovery transaction that
  either preserves the physical coordinate or resets every participant.
- **Two-stream commit is not atomic** — the secondary `CommitFill` result is
  `(void)`-discarded under a comment promising all-or-nothing. Source-level
  finding; not reproduced on a dual-stream device.

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

Phase 0 has landed. The immediate work is the implementation and offline
verification of Phases 3 and 5, with the lost-publication race first. Defer
further routine hardware metering until the fixes and trace support are ready,
then batch their hardware verification with the Phase 1 repeat-start baseline
and reporting-only self-check. Phase 5 addresses established recovery defects;
it is not yet proven to explain the observed latency difference.

Phase 2 waits for that validated baseline and timing attribution; Phase 4 follows
the plane decision and trustworthy instruments. Phase 6 requires the electrical
baseline. Reduce the scheduling budget against measured deadlines after these
timing contracts are established.
