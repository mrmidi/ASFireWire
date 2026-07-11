# PR #41 Code Review — Rate-Generic DICE Sample-Rate Support (Phase 0: 44.1 kHz)

**PR:** [#41 — (DICE-4) 44k Sample Rate Implementation Switcher](https://github.com/mrmidi/ASFireWire/pull/41)

**Reviewed commit:** `3285b13378d3db721d1b98557d02defa0688d9b4`

**Goal:** build normal, multi-sample-rate support across the audio stack. This
PR's Phase 0 scope is DICE 44.1 kHz enablement; it must establish the
rate-generic configuration and ownership model that later enables 32/48,
88.2/96, and 176.4/192 kHz without another architecture fork.

**Verdict:** **Request changes.** The Phase 0 44.1 kHz cadence math is sound,
but the CoreAudio/device-clock transaction is incomplete and can leave the HAL,
direct binding, and DICE clock in different states. That prevents this PR from
being a safe foundation for normal multi-rate support.

## Requirement cross-check

| Requirement | PR status |
|---|---|
| Rate-generic blocking/replay arithmetic | Pass for Phase 0 (44.1 kHz) |
| Per-device supported-rate set derived from DICE capabilities | Missing |
| Host-coordinated ADK configuration transaction | Missing |
| Atomic HAL + stream-format + transport publish | Missing |
| Rate-tier geometry and capture-gated wire policy | Incomplete / stale |

## Findings

### P0 — The new rate switch bypasses the required AudioDriverKit configuration transaction

[`HandleChangeSampleRate`](https://github.com/mrmidi/ASFireWire/blob/3285b13378d3db721d1b98557d02defa0688d9b4/ASFWDriver/Audio/DriverKit/ASFWAudioDevice.cpp#L616)
immediately calls the cross-service DICE RPC, mutates the endpoint binding, then
calls `SetSampleRate`. The PR adds no `RequestDeviceConfigurationChange`,
`PerformDeviceConfigurationChange`, or `AbortDeviceConfigurationChange`
implementation; the IIG declares only `StartIO`, `StopIO`, and
`HandleChangeSampleRate`.

This violates the required stop → hardware commit → publish → resume barrier
in [SAMPLE_RATE_EXPANSION.md](SAMPLE_RATE_EXPANSION.md#L225). When I/O is
active it merely returns `kIOReturnBusy`; it never asks the host to quiesce I/O
and perform the change safely.

Implement the pending-change state machine specified in the design: validate
and queue a request, call `RequestDeviceConfigurationChange`, commit DICE only
from `PerformDeviceConfigurationChange` after `StopIO`, publish all ADK
state, and discard the pending record in `AbortDeviceConfigurationChange`.

### P1 — A successful rate request does not update stream formats, and a failed ADK publish leaves the device already changed

The graph initially selects a stream format once at
[input](https://github.com/mrmidi/ASFireWire/blob/3285b13378d3db721d1b98557d02defa0688d9b4/ASFWDriver/Audio/DriverKit/ASFWAudioDriverGraph.cpp#L495)
and [output](https://github.com/mrmidi/ASFireWire/blob/3285b13378d3db721d1b98557d02defa0688d9b4/ASFWDriver/Audio/DriverKit/ASFWAudioDriverGraph.cpp#L530).
The new handler only calls `SetSampleRate`; it never calls
`DeviceSampleRateChanged` for either stream.

DICE and the direct binding have already been updated before `SetSampleRate`
can fail. The design requires device rate, both stream formats, direct-memory
metadata, replay/DBC state, and transport state to publish as one successful
commit; on failure, hardware must remain at or recover to the previous committed
state. See [the required contract](SAMPLE_RATE_EXPANSION.md#L240).

The `audioNub` null branch is also unsafe: it skips hardware programming but
still calls `SetSampleRate`, allowing CoreAudio to accept a rate change that
never reached the device.

### P1 — The PR does not establish a per-device, rate-generic capability contract

DICE profiles unconditionally publish `{44100, 48000}` at
[DiceDeviceProfile.hpp:51](https://github.com/mrmidi/ASFireWire/blob/3285b13378d3db721d1b98557d02defa0688d9b4/ASFWDriver/Audio/DriverKit/Config/DICE/DiceDeviceProfile.hpp#L51).
The graph overwrites nub-provided rates with that profile list at
[ASFWAudioDriverGraph.cpp:165](https://github.com/mrmidi/ASFireWire/blob/3285b13378d3db721d1b98557d02defa0688d9b4/ASFWDriver/Audio/DriverKit/ASFWAudioDriverGraph.cpp#L165).

The code reads `GLOBAL_CLOCK_CAPABILITIES`, and even defines
`DiceClockCapsSupportRate`, but never uses it to advertise or validate a
selection. `HandleChangeSampleRate` also casts any `double` to `uint32_t`
with no exact-membership check. Consequently, 32 kHz can pass the DICE selector
and restart validation even though CoreAudio was advertised only 44.1/48 kHz,
producing a device/format mismatch.

For the broader goal, the supported-rate list must be the intersection of
device caps, validated stream-geometry tiers, and profile quirks; Phase 0 can
limit that intersection to 44.1/48 kHz, but it must not hardcode a universal
two-rate list. This conflicts with the design's requirement to validate the
exact requested rate against nub-advertised rates and current device caps
([SAMPLE_RATE_EXPANSION.md](SAMPLE_RATE_EXPANSION.md#L240)). The local Linux
DICE reference uses clock capabilities to gate usable rates:
[dice.c](../references/linux-upstream/sound/firewire/dice/dice.c#L74) and
[dice-stream.c](../references/linux-upstream/sound/firewire/dice/dice-stream.c#L32).

### P1 — A failed DICE clock change poisons the next start

[`DICETcatProtocol::ApplyClockConfig`](https://github.com/mrmidi/ASFireWire/blob/3285b13378d3db721d1b98557d02defa0688d9b4/ASFWDriver/Audio/Protocols/DICE/TCAT/DICETcatProtocol.cpp#L190)
stores `selectedClock_` before the asynchronous hardware operation succeeds,
with no rollback on the failure callback. A subsequent `StartIO` uses that
stored value through
[`PrepareDuplex48k`](https://github.com/mrmidi/ASFireWire/blob/3285b13378d3db721d1b98557d02defa0688d9b4/ASFWDriver/Audio/Protocols/DICE/TCAT/DICETcatProtocol.cpp#L253).

A rejected 44.1 kHz request can therefore leave CoreAudio at 48 kHz while the
next start retries 44.1 kHz. Set `selectedClock_` only after confirmed success,
or restore the previous applied clock on every error path.

### P2 — The simulator and design note do not describe the multi-rate roadmap accurately

The simulator passes but still prints that production rejects 44.1 kHz, has a
48 kHz ZTS conversion, lacks rate-aware timing geometry, lacks HAL wiring, and
needs the ZTS-period assertion. Those statements are no longer accurate for
the code changed by this PR.

Likewise, [SAMPLE_RATE_EXPANSION.md](SAMPLE_RATE_EXPANSION.md#L1) still says
implementation has not started. Update the documents to distinguish Phase 0
(44.1 kHz on DICE) from the rate-generic endpoint, including the 2x/4x geometry
and capture gates. Do not describe the accumulator seed as hardware-verified
without the capture required by [44100.md](44100.md#L834).

## Verification performed

- Reviewed PR head `3285b13378d3db721d1b98557d02defa0688d9b4` against base
  `3b7bbb68e4b9409a4de48c3bcdf89f39b8ba31a0`.
- `git diff --check` completed without whitespace errors.
- Ran `python3 tools/amdtp_blocking_cadence_sim.py`: **119/119 checks passed**.
- Verified that `tests/audio/generated/AmdtpBlockingCadence441Golden.hpp`
  matches the current generator output.

The simulator validates the arithmetic/model, not the missing AudioDriverKit
transaction, capability filtering, or hardware capture gates.

### Simulator results confirmed

- Phase 0: 441 data packets per 640 cycles, carrying 3528 frames per period.
- SYT deltas are 4458/4459 ticks, with 34 long deltas in every 147 events and
  a 655360-tick period sum.
- DBC advances by 8 on data packets and remains unchanged on no-data packets.
- One isochronous packet is sent per FireWire cycle at every tested rate.
- The model also verifies family-invariant blocking sequences for
  88.2/176.4 and 96/192 kHz; this is evidence for the rate-generic engine, not
  proof that those DICE rate tiers are ready to advertise.
- 44.1 kHz has 32/40 frames in every six-cycle timing group.
- Replay round-trips preserve the presentation lag at tested clock offsets from
  -500 ppm through +500 ppm.

## Scope note

No implementation code, tests, GitHub review comments, or PR state were
modified during this review.
