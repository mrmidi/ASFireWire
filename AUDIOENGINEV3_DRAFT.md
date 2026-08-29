# Audio Engine V3 — Hardware Timeline and Immutable PCM Publication

## Status

V3 is implemented in the working tree and has passed the host/software gates.
Hardware acceptance remains pending and is intentionally batched at the end.

This document is the current implementation contract and handoff. It replaces
the earlier research notebook, provisional lab gates, and stale latency
theories.

The former 2,400-frame exposure/catch-up value is not an SYT presentation
delay, does not explain the CoreAudio/HAL contract failure, and is not part of
V3.

## Problem and correction

The old engine allowed several software components to act as clocks:

- `WriteEnd` and the staging horizon;
- a packetizer-owned `nextAudioFrame_`;
- content catch-up and stale-range rebasing;
- RX replay projection;
- a separate M-Audio frame cursor.

That could change the physical meaning of an absolute HAL sample coordinate.
V3 fixes the contract by making hardware presentation time the only timing
authority:

```text
hardware presentation observation
             ↓
HardwareSampleTimeline {epoch, absolute sample coordinate}
             ↓
TxPresentationPlan
             ↓
backend cadence / DBC / SYT conversion
             ↓
packet encoder and isoch transport
```

PCM availability is orthogonal:

```text
CoreAudio writes ADK ring
             ↓
WriteEnd(S, N)
             ↓
PcmPublicationCache {epoch, absolute frame, immutable words}
             ↓
bounded packet scratch copy + identity revalidation
```

The cache publishes content. It never schedules packets, advances physical
time, retries a range, or rebases a sample coordinate.

## Invariants

1. Hardware presentation time is the sole owner of the absolute HAL sample
   coordinate.
2. `WriteEnd` is the final content-publication boundary for its absolute range,
   not a timing event.
3. Within an epoch, sample coordinates never clamp, jump, retry, or rebase.
4. A genuine hardware discontinuity creates a new explicit epoch.
5. A packetizer encodes a supplied plan and owns no HAL frame cursor.
6. A missed DATA deadline permanently consumes that physical sample range and
   emits the backend's NO-DATA representation.
7. Normal cadence NO-DATA consumes zero sample frames.
8. RX replay may supply backend cadence, block-count sequence, or SYT behavior;
   it never assigns or advances HAL frames.
9. All backends use the same timeline, immutable cache, physical planner, and
   8,192-frame HAL geometry.
10. M-Audio differs only in device control, startup choreography, presentation
    observation conversion, and channel policy.

## Hardware timeline and ZTS

`HardwareSampleTimeline` owns:

- the current epoch and discontinuity reason;
- the current absolute HAL sample coordinate;
- the latest hardware presentation observation;
- refreshed FireWire-bus↔host-clock correlation;
- the exact nominal local geometry of 512/256/128 bus ticks per frame at
  48/96/192 kHz;
- the next TX frame and last published ZTS boundary.

RX observations are preferred while valid. Backend-qualified TX observations
provide output-only operation and take over explicitly after RX presentation
loss. A TX completion time is not itself presentation time: backend conversion
applies the relevant cycle/SYT/transfer offset before submitting an observation.

There is no driver PLL and no startup-derived long-term nominal projection.
Nominal ticks per frame are used only inside an observed packet or similarly
bounded interval. Successive hardware-derived ZTS anchors expose the actual
media-clock rate to HAL.

ZTS is published exactly at every 8,192-frame boundary, including a boundary
inside a received or transmitted packet. RX and TX observations cannot publish
the same `{epoch, boundary}` twice.

Epoch transitions are explicit for:

- fresh `StartIO`;
- FireWire bus-generation change;
- sample-rate or clock-source change;
- hardware restart;
- unrecoverable presentation discontinuity or RX presentation loss.

A fresh `StartIO` begins at frame zero. A midstream epoch starts at the next
8,192-frame boundary after the last published boundary. Cache publication and
hardware observation use non-blocking active-operation fencing so a transition
cannot race an in-flight RX observation or `WriteEnd` publication.

## Immutable PCM publication cache

`PcmPublicationCache` has a fixed capacity of 8,192 frames. It replaces both
the timing-bearing staging FIFO and delayed reads from the mutable ADK ring.

`WriteEnd(S, N)` performs only:

1. a wrap-aware copy from the ADK output ring;
2. publication under `{epoch, absoluteFrame}`;
3. a scheduler wake-up.

Every cached PCM word is atomic. Each frame has a sequence and identity:

1. publish an odd sequence to mark rewrite in progress;
2. store epoch, absolute frame, and PCM words;
3. release-publish the even completed sequence;
4. the consumer acquire-checks identity, copies to packet scratch, and
   rechecks the sequence.

The copy result is explicit:

```cpp
enum class PcmCopyResult {
    Ready,
    NotYetPublished,
    Expired,
    WrongEpoch,
    ConcurrentRewrite,
    InvalidRequest,
};
```

Duplicate `WriteEnd` publication is diagnosed without modifying the timeline
or replacing already-final content for the same identity. There is no
allocation, locking, packet construction, logging, or clock movement in the
real-time callback.

The immutable cache removes the undocumented in-progress ADK-ring overwrite
race. The old direct-ring lab gate and snapshot fallback are deleted.

## Physical TX planning

Every planned cycle uses:

```cpp
struct TxPresentationPlan {
    uint64_t epoch;
    uint64_t cycleOrdinal;
    uint64_t firstAudioFrame;
    uint32_t frameCount;
    uint64_t presentationBusTicks;
    PacketDisposition disposition;
};
```

The timeline and backend physical cadence preview the plan. The packetizer
receives its absolute frame and presentation time explicitly. Cadence and DBC
commit only after durable packet-slot publication, or after a deadline-expired
NO-DATA packet has durably represented the missed range.

Initial cycle policy:

| Quantity | Cycle slots | Time |
|---|---:|---:|
| Ownership guard | 48 | 6 ms |
| Dispatch slack | 96 | 12 ms |
| Prepared target | 144 | 18 ms |
| Durable packet store | 192 | 24 ms |

These are policy values, not derived truths. Telemetry measures actual
headroom, ownership, store high-water, and completion latency so the values can
be tuned from hardware evidence.

Content availability never gates packet production. When the cache cannot
satisfy a planned DATA range and the profile sets
`substituteSilenceOnPcmUnavailable`, the range is encoded as silence (a zero
sample through the configured slot encoding, which is `0x40000000` for AM824
MBLA) and transmitted as an ordinary DATA packet that consumes its frames.

This replaces the original "retry while the physical transport deadline remains
schedulable" rule, which starved the IT descriptor ring on hardware until the
context faulted on an uncommitted slot. Both reference stacks fill rather than
withhold: Linux `sound/firewire/amdtp-am824.c:358-363` calls
`write_pcm_silence()` when no PCM is available (a path shared by snd-bebob and
snd-dice), and Apple's `AppleFWAudio` has no availability check at all —
`AM824DCLWrite::HandleDCLCallback` unconditionally refills the next buffer group.
A NO-DATA packet is not a substitute: it consumes no frames, so it stalls the
data-block cadence the device is clocked on.

The policy is per-profile. It is enabled for the AV/C class, where both
references attest it; DICE keeps the previous behaviour until its own vendor
driver is checked.

For a profile without silence substitution, a construction failure may still
retry while the physical transport deadline remains schedulable without
consuming cadence, DBC, or frame state. Once the deadline passes:

- the old PCM range is never transmitted later;
- the exact missed `{epoch, firstFrame, frameCount}` is recorded;
- the precise cache failure and transport cursors are latched;
- the backend's NO-DATA representation is published;
- the hardware timeline advances by the missed DATA plan's frame count.

DICE primary and secondary streams receive the same presentation plan. Their
only difference is the channel slice encoded into each stream.

## M-Audio scope

Special handling applies only to the FireWire 1814 and ProjectMix special
firmware.

Retained device-specific behavior:

- the filtered control-command surface;
- vendor clock and mixer initialization;
- rate-command interlocks and post-host-start rate reassertion;
- TX-first start, RX at +160 cycles, and TX prefill;
- TX callback warm-up: group 2 capture reference and group 3 first eligible
  playback observation;
- header-only capture transition handling;
- channel permutations, input skew, formations, MIDI counts, and latency
  tables.

`MAudioPresentationObserver` replaces the one-origin nominal-rate clock
adapter. Each eligible hardware callback correlates completion cycle with host
interrupt time, applies the M-Audio presentation offset, and submits a common
`HardwarePresentationObservation`.

`MAudioInternalTxTiming` is now a backend cadence/DBC/SYT adapter. It accepts
the common plan's cycle and absolute frame and owns no HAL cursor. M-Audio uses
the same cache, planner, missed-deadline semantics, and HAL geometry as DICE.
There is no selectable legacy M-Audio scheduler.

## HAL geometry and timing properties

V3 supports 48, 96, and 192 kHz. Unsupported rates are filtered at the HAL
boundary and rejected during runtime configuration.

- ADK output/input ring capacity: 8,192 frames.
- Zero-timestamp period: 8,192 frames.
- The geometry is configured when constructing the device; no artificial live
  configuration-change transaction is manufactured.
- Output safety is derived from physical scheduling lead, backend transfer
  delay, and packet granularity. It is not ring capacity.
- Device/stream latency remains a separate handoff↔presentation or
  acquisition↔delivery measurement.

## Measured latency targets (Apogee Duet)

Reference numbers for the one device measured so far. Captured 2026-08-29 from
Logic on an Intel Mac running Apple's original FireWire audio driver, with the
same Apogee Duet (GUID `0x0003DB0A0000D112`) later used on macOS 27.

Logic reports `output = (reportedLatency + safetyOffset + ioBuffer) / rate`, so
the buffer-independent cost is recovered as `output_ms x rate - buffer`. At each
rate the three buffer sizes (512/128/32) solve to the same value to the last
decimal, which is what makes these trustworthy rather than eyeballed:

| rate | Apple output fixed | Apple input fixed | output in cycles | output ms |
|---|---:|---:|---:|---:|
| 44.1 kHz | 101 frames | 91 frames | 18.3 | 2.29 |
| 48 kHz | **117 frames** | **88 frames** | 19.5 | 2.43 |
| 96 kHz | 160 frames | 218 frames | 13.3 | 1.67 |

Against V3 as it stands at 48 kHz:

| | Apple | ASFW (`7ca8d6e3`) | ratio |
|---|---:|---:|---:|
| output fixed | 117 frames (19.5 cycles) | 896 frames (149 cycles) | 7.6x |
| input fixed | 88 frames (14.7 cycles) | 256 frames (42.7 cycles) | 2.9x |

Matching the output figure would put Logic at **5.1 ms output / 9.6 ms
roundtrip** at 48 kHz / 128 frames — which is exactly what Apple's driver
reports, and level with a modern USB interface (Audient iD14: 9.0 ms).

### What the numbers constrain

720 of our 768 safety frames are *committed DMA lead*: PCM is fixed at
preparation time, `kTxPreparedTargetCycleSlots` = 120 packets ahead, and the
safety offset must cover it or we would be lying to CoreAudio about what we can
still honour.

Apple reaches 19.5 cycles **on the same OHCI silicon**. Subtracting the IEC
61883-6 transfer delay (12800 ticks, ~4.2 cycles) leaves ~15 cycles of driver
and DMA lead. Our ownership guard alone is 48 packets. So the guard is a policy
choice, not a hardware floor, and roughly 3x more conservative than what is
demonstrably sufficient.

The other half of the evidence points the same way: Apple runs a *deep* IT ring
(~800 packets, 100 ms TX interrupt cadence) with this *small* content lead.
Ring depth and content lead are independent quantities. V3 currently makes them
the same number, which is why the safety offset is large.

The implied change is to split them:

- keep a long **cadence** lead — plan, DBC, SYT, descriptors — sized by the
  producer-lateness budget, which is what dispatch stalls actually threaten;
- write **PCM payload** as late as the DMA allows, back-filling prepared but
  unfetched slots when fresher content arrives;
- report only the content lead as output safety.

The unknown is where the true content boundary sits — how far ahead OHCI has
actually fetched payload. Apple's ~15 cycles is the empirical upper bound on how
conservative it needs to be. That is measurable rather than derivable, and it
touches the transport/audio seam, so it wants its own instrumented step.

### Caveats

- One device. Other devices are not captured yet; DICE in particular may differ,
  and its TX policy is deliberately still on the pre-`59f3b501` behaviour.
- The reference machine is Intel, on an OS old enough to use IOAudioFamily
  rather than AudioDriverKit. The *semantics* of the safety offset are the same
  so the frame counts compare directly, but Apple's scheduling headroom was
  chosen against a kext-era dispatch model, not DriverKit's.
- The 96 kHz input figure (218 frames) is nearly 2.5x the 48 kHz one while
  output moves the other way. Recorded as measured; not explained.

## Instrumentation

Audio telemetry wire version 6 is 1,072 bytes per endpoint. The C++ snapshot,
Swift decoder, fixed-size validation, and MCP projection agree on that layout.

Telemetry includes counters and fixed histograms for:

- timeline epochs, source changes, observations, ZTS publications, and
  duplicate suppression;
- PCM publication spans, callback duration, duplicates, expiry, epoch changes,
  and every `PcmCopyResult`;
- preparation/ownership depth, deadline headroom, retries, exact missed ranges,
  and cache failure attribution;
- backend DATA/NO-DATA output, DBC/SYT discontinuity, replay state, and
  observation conversion failures;
- packet-store high-water and completion latency;
- M-Audio warm-up groups, TX-derived observations, and transition state.

The continuously overwritten 512-entry `TxCycleTraceRecord` ring contains:

- epoch and cycle ordinal;
- first absolute audio frame and frame count;
- PCM result and packet disposition;
- presentation bus time;
- prepare, publish, ownership, and completion cycles;
- deadline headroom.

There are no per-packet or per-`WriteEnd` logs. Normal output is one five-second
`[TxV3]` liveness/margin heartbeat. Anomaly records use first-occurrence plus
power-of-two repetition limiting:

- `[PcmCache]`
- `[TxDeadline]`
- `[TimelineEpoch]`
- `[ZTS]`
- `[BackendTiming]`
- `[TxOwnership]`
- `[MAudioTiming]`

Capture the V3 trace from a user shell with:

```bash
log stream --style compact --info --debug --predicate 'process == "kernel" AND (eventMessage CONTAINS "[TxV3]" OR eventMessage CONTAINS "[PcmCache]" OR eventMessage CONTAINS "[TxDeadline]" OR eventMessage CONTAINS "[TimelineEpoch]" OR eventMessage CONTAINS "[ZTS]" OR eventMessage CONTAINS "[BackendTiming]" OR eventMessage CONTAINS "[TxOwnership]" OR eventMessage CONTAINS "[MAudioTiming]")'
```

## Software validation completed

The current working tree passes:

- production Xcode Debug build with no warnings or errors;
- Swift/XCTest suite, including telemetry-v6 decoding and MCP compatibility;
- all 1,805 C++ host tests; seven hardware/environment or Debug-only tests are
  intentionally reported as skipped;
- optimized Release benchmark for maximum-channel 4,096-frame `WriteEnd`
  publication, below 10% of that operation's audio duration;
- exact 8,192 ZTS boundary projection inside packets at 48/96/192 kHz;
- synthetic ±25/±100 ppm and drifting hardware observations;
- delayed worker bus/host backdating;
- explicit epoch transitions and duplicate-anchor suppression;
- cache wrap, 4,096-frame operations, concurrent rewrite, expiry, duplicates,
  and epoch invalidation;
- pre-deadline retry versus irreversible post-deadline NO-DATA;
- zero physical-plan gaps or overlaps;
- DICE shared-plan equality;
- M-Audio start choreography, callback-group qualification, presentation
  conversion, and cadence commit without a private frame cursor.

## Batched hardware acceptance pending

Hardware validation must confirm:

- HAL reads back the 8,192 ring and ZTS period;
- anchors are monotonic within each epoch and visibly reseed across real
  discontinuities;
- playback/capture are stable at 48/96/192 kHz;
- output-only and RX-loss TX observation fallback remain clock-correct;
- no stale audio is emitted after deadline misses;
- measured preparation and ownership margins support 48/96/144;
- DICE primary/secondary wire timing agrees;
- M-Audio performs its asymmetric startup, produces TX-derived anchors, and
  transitions capture without moving the playback timeline;
- reported safety and latency match observed hardware-relative behavior.

The implementation is software-complete, but V3 is not release-accepted until
this hardware batch passes. There is no runtime legacy fallback if it fails;
the V3 implementation must be corrected.

## Assumptions in force

- `WriteEnd` is HAL's final publication boundary for completed output content,
  not a timing event.
- The immutable cache is the cross-service content boundary; the transport
  does not read mutable ADK output-ring slots later.
- Nominal FireWire ticks-per-frame are bounded packet math, not long-term
  media-clock authority.
- M-Audio is special in control, startup, and observation conversion only—not
  in HAL timeline ownership.
- Initial cycle depths are conservative policies subject to telemetry-driven
  hardware tuning.
