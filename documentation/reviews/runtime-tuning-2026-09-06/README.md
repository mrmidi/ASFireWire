# Runtime audio tuning review — 2026-09-06

Reviewed `5e39de08` through `0cd228bf` (base `3a133a09`): value validation,
user-client wire surface, nub bridge, configuration window, app panel and logs.
No production edits, driver installation, hardware control or new RTL run.

The transmit-depth consumer is wired into preparation. Declarations and HAL
geometry are not applied to CoreAudio yet. The current panel is therefore not
ready to serve as evidence of effective configuration during a latency sweep.

## Findings

### 1. P1 — Requested values are reported as effective configuration

[`ApplyPendingRuntimeTuning`](../../../ASFWDriver/Audio/DriverKit/ASFWAudioDevice.cpp:1260)
explicitly leaves declarations and HAL geometry unconsumed, then stores the
entire candidate as active and increments the applied sequence. A ZTS request
for 4096 consequently reads back 4096 even though device creation and the
timeline still use 8192; a latency override reads back without a corresponding
`SetOutputLatency`/`SetInputLatency`. The UI offers both as working operations;
the caveat exists only in the driver log.

Even before the first request,
[`CopyRuntimeTuningSnapshot`](../../../ASFWDriver/Audio/Core/AudioCoordinator.cpp:397)
returns the four zero override sentinels as resolved declarations. The Duet
panel at 128 frames therefore computes 256 frames / 5.333 ms, whereas the graph
declares 463 / 9.646 ms with its profile's 67/40 latency and 50/50 safety.

Publish effective graph values separately from requested overrides. Until the
consumers/republication exist, reject unsupported groups and disable their
controls; logging that they did not apply cannot make a successful readback true.

### 2. P1 — Repeated publication races with cross-service readers

The nub's
[`SetActiveRuntimeTuning` / `CopyActiveRuntimeTuning`](../../../ASFWDriver/Audio/DriverKit/ASFWAudioNub.cpp:591)
exchange plain fields across the audio and core queues. A release increment and
one acquire load publish a completed write but do not protect a reader from the
next overwrite. There is no lock, immutable version or validation after copying.
Outcome fields have the same problem and are updated after the sequence.

A native two-thread harness using the unmodified setter/copier bodies extracted
from this revision accepted 19,637 mixed snapshots in one second: for example,
slack 48 and latency 12 while each writer publication paired equal values.
This harness stubs the nub shell; it does not exercise DriverKit dispatch.

The [pending path](../../../ASFWDriver/Audio/DriverKit/ASFWAudioNub.cpp:547)
has the same overwrite race. It also loses requests: after `CopyPending` copies
A, a producer can publish B, then `TakePending`'s unconditional zero store
clears B. Repeated Apply is allowed while a window is pending, and the constant
configuration token does not identify which request a callback consumes.

Use one serialized/locked transaction or immutable, retained request records
with a request ID. Copy-and-consume must be one operation; apply/abort must
complete that specific request. These are control paths, so there is no need
to invent an unproven lock-free mailbox here.

### 3. P2 — The group mask does not restrict the mutation

[`ApplyPendingRuntimeTuning`](../../../ASFWDriver/Audio/DriverKit/ASFWAudioDevice.cpp:1222)
initializes from active state, but `TakePendingRuntimeTuning` overwrites every
field, and the whole candidate is assigned at line 1274. `groups` controls only
the zero check and a log; `CostOf` is used only in tests.

For example, a declarations-only request carrying an older snapshot's 72-packet
slack overwrites the currently active 48-packet slack, although transmit depth
was not selected. The driver must merge selected groups into current state,
validate the merged result and calculate its actual cost. Reject unknown masks
and avoid requesting an ADK window for an effective no-op.

### 4. P2 — Unsigned overflow bypasses the shared-slot bound

[`ValidateTuning`](../../../ASFWDriver/Audio/Shared/AudioRuntimeTuning.hpp:178)
adds the target and guard in `uint32_t`. With `txDispatchSlackPackets = UINT32_MAX`
and the valid 48-packet guard, the real validator returns applicable:

```text
applicable=1 rejection=0 target=47 required=95 slots=168
```

The malformed wire request passes both validation sites and can replace the
runtime target with less than one hardware ring. Validate the slack directly
against the remaining storage capacity or perform checked/widened sums before
narrowing. The current UI presets do not generate this input; the user-client
boundary still accepts it.

### 5. P2 — Packet-to-frame conversion is fixed at 48 kHz

[`PreparedLeadFrames`](../../../ASFWDriver/Audio/Shared/AudioRuntimeTuning.hpp:310)
and the snapshot's `framesPerPacketAverage` always use six frames per cycle.
At 96/192 kHz, the average is 12/24; the 120-packet horizon remains 15 ms.
Current output is 720 frames displayed as 7.5/3.75 ms instead of 1440/2880
frames and 15 ms. Pass the rate into the conversion or derive duration directly
from cycles; test all supported rates. This is a reporting error, not proof of
the horizon's contribution to physical RTL.

### 6. P2 — A valid UInt32 edit can crash the app before validation

[`coreAudioSection`](../../../ASFW/Views/AudioTuningView.swift:183) parses editable
strings as `UInt32` and immediately adds them with trapping arithmetic. Typing
`4294967295` into an enabled latency field succeeds in parsing, but adding the
128-frame preview buffer overflows while SwiftUI evaluates the body. Driver
validation is never reached. Check ranges before preview arithmetic and use
wider checked calculations; invalid text should produce a field error rather
than silently falling back to the old value.

### 7. P2 — Blank/reset cannot clear an existing declaration override

[`resetToDefaults`](../../../ASFW/Views/AudioTuningView.swift:323) blanks the
fields, but [`resolved`](../../../ASFW/Views/AudioTuningView.swift:380) replaces
blank with the snapshot value. After storing override 200, Reset plus Apply
therefore submits 200 again (or disables Apply as unchanged), not the driver's
zero sentinel for reverting to the profile. Encode override absence explicitly
and use resolved values only for display. Also distinguish safety from latency
in the section text: safety changes scheduling and is not a reporting-only
self-check variable.

### 8. P2 — An asynchronous refusal never reaches a terminal UI outcome

[`RequestRuntimeTuning`](../../../ASFWDriver/Audio/DriverKit/ASFWAudioDevice.cpp:1202)
logs an immediate `RequestDeviceConfigurationChange` failure but leaves the
candidate pending. The app already received success from queuing the action;
no rejection/status update reaches its snapshot. Abort clears pending without
recording a rejected outcome either. The panel can remain parked indefinitely
or show an old outcome with no way to identify the failed request.

Associate a request ID with queued, requested, applied, rejected and aborted
states; publish the terminal error for both refusal paths. A later callback
must not apply or discard another request's candidate.

## Validation and limits

- `AudioRuntimeTuningTests`: 7/7 pass. These cover value helpers, not the nub,
  effective graph readback, group merge or asynchronous apply/abort path.
- [Native probe source](evidence/runtime-tuning-probe.cpp) and
  [output](evidence/runtime-tuning-probe.txt): real validator and rate helpers;
  active snapshot stress using extracted production method bodies and ivars.
- Swift checked arithmetic confirms `128 + UInt32.max` overflows. No GUI crash
  or live tuning request was induced.
- No full Xcode build or empirical ADK lifecycle validation in this review.

The latest alignment logs are useful observations of small relative divergence
in the sampled session. Their shared projection is not an independent physical
oracle, and a 120-packet prepared horizon does not by itself allocate 720 frames
of measured RTL to driver waiting. Keep that attribution as an experiment until
effective settings, actual events and repeated runs can be correlated.
