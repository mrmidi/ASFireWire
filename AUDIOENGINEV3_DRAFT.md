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

### Host-side filtering of our anchors (open decision)

Removing the driver PLL does not make the anchor stream unfiltered. The host
applies its own filter unless told otherwise
(`AudioDriverKitTypes.h`, `IOUserAudioClockAlgorithm`):

| value | behaviour |
|---|---|
| `Raw` (`'raww'`) | timestamps used as-is, no filtering |
| `SimpleIIR` (`'iirf'`) | simple IIR filter — **the default when the device does not set the property** |
| `TwelvePtMovingWindowAverage` (`'mavg'`) | 12-point moving window average |

So "anchors expose the actual media-clock rate to HAL" is only true with `Raw`.
By default a host-side IIR sits between our anchors and the HAL's rate estimate,
which is the same smoothing we deliberately removed from the driver.

This is the mechanism Apple described long before ADK: inaccurate time stamps
"keep the HAL's clock from locking on to the true sample rate"
(Jeff Moore, `coreaudio-api` 2004/Oct/msg00166). The filter *is* the locking.

Two consequences to settle before hardware acceptance:

- If our anchors are as accurate as V3 intends, the default filter only adds lag
  to genuine rate tracking, and `Raw` is the consistent choice.
- **Epoch interaction.** We reseed the timeline on discontinuity. A stateful IIR
  carrying pre-epoch state across that reseed converges from a stale estimate;
  nothing in the SDK says the host resets its filter on a ZTS discontinuity.
  Under `Raw` the question does not arise.

`SetClockAlgorithm()` is declared on `IOUserAudioClockDevice` and reaches us by
inheritance (`class IOUserAudioDevice: public IOUserAudioClockDevice`).

Anchor density is the other half of this: an 8,192-frame period at 48 kHz is one
anchor per 170 ms, so a 12-point window would span roughly two seconds.

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

Starting at zero is the documented device-timeline behaviour, not a V3 choice:
`AudioDeviceGetCurrentTime()` returns a timeline that "starts at 0 when the
hardware starts and monotonically increases while the hardware is running"
(Jeff Moore, `coreaudio-api` 2005/Sep/msg00207). Our epoch trigger list also
matches the discontinuity causes the archive enumerates — configuration change,
sample-rate change, engine restart, overload.

### An epoch is invisible to the HAL (open decision)

Invariant 4 says a genuine hardware discontinuity creates a new *explicit*
epoch. That is explicit to V3 only. There is no way to tell the host:

> "the archive does not provide the exact driver API, flag, property, or callback
> by which a driver explicitly announces 'the sample clock has restarted.' No
> such mechanism should be inferred."
>
> "At the system level, a configuration change may instead be represented by
> stopping the IO engine, updating device state, and restarting it."
> — `coreaudio-api`, discontinuity thread (2005/Sep, 2008/Apr)

The HAL only ever sees a stream of `{sample time, host time}` pairs. It has no
concept of our epoch and receives no notification when one opens. Recovery above
the HAL is interpolation, not resynchronisation: "the output unit uses the host
times and rate scalar in its most recent HAL timestamps to interpolate a new
sample time."

So the question V3 has to answer explicitly is: **across a midstream epoch, does
the absolute HAL sample coordinate remain continuous?**

- If it does, an epoch is internal bookkeeping — discontinuity reason, source
  arbitration, duplicate suppression — the host observes an unbroken monotonic
  timeline, and nothing further is required. This should be stated as an
  invariant rather than left implied.
- If it does not, we are introducing an unannounced discontinuity. The host will
  chase it through whatever clock algorithm is in force, and under the
  `SimpleIIR` default it will filter *across* the jump rather than reset at it.
  The only sanctioned representation of a real clock restart is
  `StopIO` → reconfigure → `StartIO`, which is a heavier transition than "start
  at the next 8,192-frame boundary" currently implies.

The present wording — a midstream epoch starting at the next boundary while IO
continues — reads as the first case, but does not say whether the coordinate
itself reseeds. Hardware acceptance cannot settle this; it is a design statement
we owe the reader.

### Prewarm and the "fresh StartIO" rule (unresolved)

`StartIO` is not called once per session. It carries flags
(`AudioDriverKitTypes.h`):

```c
enum class IOUserAudioStartStopFlags : uint64_t { None = 0, Prewarm = (1L << 0) };
enum class IOUserAudioDeviceTransportState : uint64_t { Stopped = 0, Prewarmed = 1, Running = 2 };
```

`Prewarm` means "enable the minimal hardware to minimize transition to normal IO
operation", and the device advertises support through `in_supports_prewarming`
on `IOUserAudioDevice::Create()`, readable via `GetSupportsPrewarming()`.

The transport is therefore a three-state machine, and a session can legitimately
be `StartIO(Prewarm)` → `StartIO(None)`. "A fresh `StartIO` begins at frame zero"
does not say which one. Taken literally it opens two epochs per session, the
second resetting the coordinate after the first already published anchors.

V3 must state explicitly either that:

- we advertise `in_supports_prewarming = false`, so only one `StartIO` occurs; or
- a `Prewarm` start is epoch-neutral and only the `None` start opens the epoch.

This is not currently addressed anywhere in the timeline, epoch, or M-Audio
startup sections, and it interacts directly with the asymmetric M-Audio
choreography (TX-first, RX at +160 cycles, TX prefill).

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
| Dispatch slack | 72 | 9 ms |
| Prepared target | 120 | 15 ms |
| Durable packet store | 168 | 21 ms |

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
- Output safety is derived from the physical payload-finality lead and the
  device-profile floor. Backend transfer delay is reported separately as
  latency; it is not counted twice in safety.
- Device/stream latency remains a separate handoff↔presentation or
  acquisition↔delivery measurement.

Setting the geometry at construction is a deliberate refusal of an available
path, not an absence of one: `IOUserAudioReservedConfigChangeAction` defines
`RingBufferFrameSize = 2` alongside `SampleRate = 1` and `StreamFormat = 3`, so
the ring can be resized live through the config-change mechanism. We do not.

### Client buffer size range is synthesized, not chosen

The driver does not advertise the client IO buffer range; the HAL derives it.
Jeff Moore stated the formula twice, seven years apart:

> "The buffer frame size range is synthesized by the HAL based on the smallest
> schedulable time quantum (roughly 300 microseconds) to 3/8ths the size of the
> hardware ring buffer."
> — `coreaudio-api` 2001/Nov/msg00047

> "Currently for IOAudio-based devices, the minimum is always however many frames
> fit into 300 microseconds (for example 14 frames at 44100). The maximum is
> always 3/8ths the number of frames in the ring buffer. […] The only influence
> the driver has is through its control of the size of the ring buffer."
> — `coreaudio-api` 2008/Aug/msg00171

Both qualifiers matter: "currently", and "IOAudio-based". This is an
implementation detail of a family we are not in, and no equivalent constraint
appears anywhere in the AudioDriverKit headers — there is no documented minimum,
maximum, or ring ratio.

If the ratio carried over, our 8,192-frame ring caps clients at 3,072 frames,
and the 4,096-frame `WriteEnd` benchmark exercises a size the HAL would never
request. That is not a defect either way, but it means the benchmark's headroom
claim is unanchored.

**Action.** The driver cannot read this range — the HAL synthesizes it *from our
ring buffer* and never hands it back to us. Two ways to observe it from where we
actually sit:

- *Driver-side, passive.* We already see the answer: the frame count in every
  `WriteEnd(S, N)` is a size the HAL chose from that range. Record min/max/
  distribution of `N` in the existing telemetry histograms. If `N` never exceeds
  3,072 with an 8,192-frame ring, ADK inherited the ratio; if it reaches 4,096,
  it did not. This costs one counter and answers the question during ordinary
  playback.
- *Userspace, one shot.* A HAL client reading
  `kAudioDevicePropertyBufferFrameSizeRange` on our published device prints the
  synthesized range directly. Worth doing once to confirm the passive result and
  to record the number here.

The passive form is authoritative for what actually happens; the userspace read
is authoritative for what is *permitted*, including sizes no client requested.

### Safety offset — what belongs in it

The split V3 uses (payload-finality lead plus device-profile floor in safety;
backend transfer delay reported as latency) matches Apple's stated purpose:

> "The safety offset is there primarily to allow for physical transfer issues.
> […] the USB Audio driver has to be some number of packets ahead of where the
> USB hardware is because of how the USB hardware works. This requirement is
> reflected in the safety offset the driver reports."
> — Jeff Moore, `coreaudio-api` 2004/Oct/msg00166

Our payload-finality lead is the FireWire analogue of "N packets ahead", so it
belongs in safety; presentation delay belongs in the latency property. The
50-frame floor is likewise consistent with "there really isn't any real hardware
that would have a safety offset of 0."

**But** the same message gives safety offset a second purpose we have designed
out:

> "The safety offset secondarily allows a driver to add some padding […] useful
> for drivers whose hardware has trouble generating accurate time stamps.
> Inaccurate time stamps keep the HAL's clock from locking on to the true sample
> rate […] which leads to reading/writing data in places where the driver isn't
> prepared which causes glitching."

V3 carries no jitter padding by design — accuracy comes from hardware-derived
anchors rather than margin. That is the better engineering position, but it
means the 50-frame floor has no jitter reserve, and the failure mode is
glitching rather than a clean error. Hardware acceptance should measure observed
anchor jitter against the floor explicitly; the telemetry already records what is
needed.

### Reported latency is a sum of three properties

Apple's accounting is:

```text
minimum stream latency = kAudioDevicePropertyLatency      (device presentation)
                       + kAudioStreamPropertyLatency      (stream presentation)
                       + kAudioDevicePropertySafetyOffset

output = minimum stream latency + kAudioDevicePropertyBufferFrameSize
```

The `output = (reportedLatency + safetyOffset + ioBuffer) / rate` form used below
collapses the two latency properties into one term. That is fine as shorthand,
but the 117-frame and 90-frame figures must be checked against the sum of *both*
device and stream latency, not one of them.

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
| 44.1 kHz | 101 frames | 92 frames | 18.3 | 2.29 |
| 48 kHz | **117 frames** | **90 frames** | 19.5 | 2.43 |
| 88.2 kHz | 149 frames | 200 frames | 13.5 | 1.69 |
| 96 kHz | 159 frames | 219 frames | 13.3 | 1.66 |

The Gate A2 working tree now reproduces the 48-kHz HAL split exactly:

| | Apple | ASFW | difference |
|---|---:|---:|---:|
| output fixed | 117 frames | 117 frames | 0 |
| input fixed | 90 frames | 90 frames | 0 |
| total fixed | 207 frames | 207 frames | 0 |

At a 128-frame client buffer this property model predicts **5.1 ms output /
9.6 ms roundtrip**, matching Apple's reported model. It does not prove physical
loopback latency; Logic displays the properties supplied by the driver.

### What the numbers constrain

The split is now implemented. The cadence/silence preparation runway remains
120 cycle slots. Descriptor mapping remains a 48-packet physical ownership
window. Payload finality is a separate eight-slot frontier: one six-packet
completion interval plus a two-packet live-command repoint guard.

A planned DATA packet is armed as complete silence in image 0. Audio publishes
a complete image 1 without touching hardware-addressed bytes. A1 places the
invariant eight-byte prefix in descriptor 2 and the mutable tail in descriptor
3. Gate A2 lets transport select image 1 for an already-bound packet by one
aligned `descriptor3.dataAddress` store after DMA publication. `mappedEnd` is
descriptor ownership; `finalizedEnd` is the producer's irreversible boundary.

At 48 kHz, eight cycle slots are 48 nominal frames, so the Duet profile's
50-frame output-safety floor wins. The old formula's backend transfer and
packet addends, plus 32-frame alignment, are gone from safety. The 67-frame
output latency remains separate profile policy.

Host tests prove complete-image race resolution and descriptor-field
invariants. Hardware still must confirm that the two-packet repoint guard is
sufficient on the real controller and wire.

### Caveats

- One device. Other device property tables are not inferred from the Duet;
  DICE retains its own per-rate safety/latency policy.
- The reference machine is Intel, on an OS old enough to use IOAudioFamily
  rather than AudioDriverKit. The *semantics* of the safety offset are the same
  so the frame counts compare directly, but Apple's scheduling headroom was
  chosen against a kext-era dispatch model, not DriverKit's.
- The 96 kHz input figure (219 frames) is over 2.4x the 48 kHz one while
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
`[TxV3]` liveness/margin heartbeat plus `[TxFill]`, which reports payload
finality, mapping, rebind acceptance/rejection, and minimum rebind distance.
Anomaly records use first-occurrence plus power-of-two repetition limiting:

- `[PcmCache]`
- `[TxDeadline]`
- `[TimelineEpoch]`
- `[ZTS]`
- `[BackendTiming]`
- `[TxOwnership]`
- `[MAudioTiming]`

Capture the V3 trace from a user shell with:

```bash
log stream --style compact --info --debug --predicate 'process == "kernel" AND (eventMessage CONTAINS "[TxV3]" OR eventMessage CONTAINS "[TxFill]" OR eventMessage CONTAINS "[PcmCache]" OR eventMessage CONTAINS "[TxDeadline]" OR eventMessage CONTAINS "[TimelineEpoch]" OR eventMessage CONTAINS "[ZTS]" OR eventMessage CONTAINS "[BackendTiming]" OR eventMessage CONTAINS "[TxOwnership]" OR eventMessage CONTAINS "[TxPayloadSeal]" OR eventMessage CONTAINS "[MAudioTiming]")'
```

## Software validation completed

The current working tree passes:

- production Xcode Debug build with no warnings or errors;
- Swift/XCTest suite, including telemetry-v6 decoding and MCP compatibility;
- all 1,839 C++ host tests; seven hardware/environment or Debug-only tests are
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
- alternate-image rebind at the two-packet command-pointer guard, rejection
  without an invariant prefix, and independent monotonic finality.

## Batched hardware acceptance pending

Hardware validation must confirm:

- HAL reads back the 8,192 ring and ZTS period;
- anchors are monotonic within each epoch and visibly reseed across real
  discontinuities;
- playback/capture are stable at 48/96/192 kHz;
- output-only and RX-loss TX observation fallback remain clock-correct;
- no stale audio is emitted after deadline misses;
- measured preparation and ownership margins support 48/72/120;
- `[TxFill]` shows rebind progress, zero rejected image selections, and a
  minimum accepted distance of at least two packets;
- DICE primary/secondary wire timing agrees;
- M-Audio performs its asymmetric startup, produces TX-derived anchors, and
  transitions capture without moving the playback timeline;
- reported safety and latency match observed hardware-relative behavior;
- the observed `WriteEnd` frame-count distribution is recorded, settling whether
  ADK inherits the IOAudio 300 µs / 3-8ths synthesis and whether a 4,096-frame
  client buffer is ever actually requested;
- the effective `IOUserAudioClockAlgorithm` is confirmed, and if it is left at
  the `SimpleIIR` default, that rate tracking still converges across an epoch
  reseed rather than carrying stale filter state;
- observed anchor jitter is measured against the 50-frame safety floor, which
  carries no jitter reserve by design;
- `StartIO(Prewarm)` either does not occur (prewarming unadvertised) or is
  demonstrably epoch-neutral.

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
- The host does not filter our ZTS anchors. This holds only under
  `IOUserAudioClockAlgorithm::Raw`; the default is `SimpleIIR`. Until
  `SetClockAlgorithm` is called explicitly, this assumption is false.
- An epoch is a V3-internal construct. The HAL is never told one has opened and
  has no API to be told, so the sample coordinate the host observes must remain
  monotonic and continuous across a midstream epoch. Any reseed that is visible
  to the host is an unannounced discontinuity.
- Initial cycle depths are conservative policies subject to telemetry-driven
  hardware tuning.
