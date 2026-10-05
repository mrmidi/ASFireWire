# FW-221: AV/C production hardware batch

The production AV/C path now uses the host configuration window, complete
per-rate formations, route-bound STATUS confirmation, and one bounded verified
rollback attempt. DICE remains on its existing path. This is a validation batch,
not a claim that every descriptor-supported rate has passed hardware testing.

## Build

Ordinary Xcode builds enable complete AV/C rate candidates by default through
`project.yml`. No multi-rate build override is required:

```sh
./build.sh --no-bump --verbose \
  --set CODE_SIGNING_ALLOWED=YES --set CODE_SIGNING_REQUIRED=YES \
  --set CODE_SIGN_IDENTITY=-
```

The dext must contain arm64e and have a verified signature. The advertisement policy
never changes the catalog's `hardwareValidated` evidence. The existing lab-only
HAL probe remains restricted to its virtual UID; do not use its synthetic
16/12/8 channel expectations on a physical device.

## What this batch exercises

- Phase88 and Duet use complete duplex descriptor formations. Clock programming
  retains each protocol's existing wire order and settle behavior.
- 1814/ProjectMix retain their vendor clock command, existing S/PDIF-in/out
  initialization, independent formation tables, probe allowlist, and capture
  permutation/delay policy. This batch does not add digital-format selectors or
  issue generic descriptor probes to those devices.
- CoreAudio nominal-rate requests and stream-format callbacks stage the same
  transaction. Active requests ask the host to stop/change/restart IO.
- STATUS observations classify unchanged, requested, other known formation, and
  unknown/mismatched duplex state. A refused CONTROL is not evidence of unchanged
  hardware. A confirmed other offered formation can be published coherently.
- Endpoint memory objects stay allocated at catalog capacity. Active channel
  widths, ring, ZTS, formats, labels, declarations, packet sizing, FDF and epochs
  follow the selected formation. The audio layer owns CIP content.
- Failed confirmation or projection gets one rollback attempt. The prior state
  becomes available only after hardware readback and complete HAL projection.
  Unknown state stays stopped. A route change cannot authorize installation of
  an old transaction's formation.
- Compatible master volume/mute remains on its existing non-blocking path.
  CoreAudio format restoration continues under the same GUID-based UID; the
  superclass commits only a confirmed successful projection.

## One install, meaningful sequence

First leave Phase88 connected. Begin at 48 kHz and establish normal duplex
playback/capture. Then run only offered rates in this order:

1. 48 → 96 → 48.
2. 48 → 44.1 → 48 → 32 → 48 → 88.2 → 48.
3. Offered 176.4/192 rates → 48, on hardware that actually supports them.

Repeat while idle and while audio is actively playing. Verify input and output
formats together after each transition. Keep the same device UID and stream
objects. Exercise mute/volume while playing, and return to 48 before switching
hardware. Repeat on Duet, then 1814. A Saffire/DICE test is outside this batch.

After the ordinary sweep, test power-cycle restoration and a reset/unplug during
an active transition. A stale transition must fail or remain stopped, never
publish mixed hardware/HAL geometry. Route invalidation can require endpoint
republishing before another transaction; automatic recovery after such an abort
must be established by this batch rather than inferred from host tests.

Where possible, record physical loopback to assess continuous audio, channel
order, clock slope and actual latency. A clean configuration trace or resumed
CoreAudio callbacks alone does not prove audio continuity.

## Capture and acceptance

```sh
/usr/bin/log show --last 20m --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[RateTxn]" OR eventMessage CONTAINS "[SessionClock]" OR eventMessage CONTAINS "[TxPrep]" OR eventMessage CONTAINS "[Timing]"'
```

`[RateTxn]` reports origin, token, route generation, prior/requested/observed rate,
projection widths, revision, commit or rollback status. Hardware observations
also report the route epoch and both plug rates. Existing transport anomaly
telemetry and coarse heartbeat stay in place.

Accept a configuration only when the hardware's confirmed clock, HAL duplex
formats, active transport geometry and timing agree; audio is continuous; buffers
and clock slope stay stable; and return to 48 is deterministic. Mark validation
per device/protocol/rate/mode, not from the generic unit-test table. Default
advertisement does not replace those hardware-validation records. To build with
baseline-only advertisement, set `ASFW_AVC_MULTIRATE_VALIDATION=0` explicitly.

## Duet 96 kHz milestone — 2026-10-05

**User-confirmed clean 96 kHz playback after removing redundant timeline scans
from the CoreAudio IO hot path.** This is a playback result for the attached
Duet, not validation of every family, rate, capture path or soak duration.

The initial 96 kHz run had growing `missedFinality`. Preserving the 48 kHz
scheduling budget in time at higher rates removed those misses, but the user
still heard clicks. The subsequent trace had zero deadline misses, healthy RX
geometry/CIP counters and ample packet preparation margin. Increasing buffering
alone had therefore not resolved the audible problem.

The PCM writer performed a bounded scan of up to 1,512 timeline slots for every
sample frame, repeating the same packet lookup 16 times per 96 kHz DATA packet.
It now reuses a packet snapshot for consecutive frames while checking its
seqlock generation and exposed state on every reuse. Crossing a packet boundary
or observing invalidation falls back to lookup. This removes redundant hot-path
work without relaxing finality, retirement or channel/ring mapping checks.
After this change, the user reported clean 96 kHz audio.

**Performance rule: remove repeated work from IO callbacks before increasing
buffers or introducing another scheduling path.** A zero missed-finality counter
is necessary evidence, but does not prove the entire callback met its timing
budget. The existing coarse `[TxPrep]` heartbeat now includes `fillUs`, the
maximum PCM fill duration in the interval; no per-callback logging is added.

Validation: 2,932 C++ tests passed (six skipped), including a 96 kHz packet/ring
boundary and finality regression; signed production Debug build and signature
verification passed, with x86_64/arm64e slices. Continue the remaining hardware
matrix and measure fill duration/latency before claiming complete multi-rate
validation.

### First idle rate request after replug

Phase88 run on 2026-10-05: the first two 96 kHz requests returned
`kIOReturnNotReady` (0xe00002d8) before CONTROL, with both plugs still at
44.1 kHz. After 44.1 playback started and refreshed the live protocol context,
44.1 → 48 succeeded in the same generation/session. The subsequent 48 → 96
transaction confirmed both plugs at 96 kHz and committed revision 3; the user
reported playback without clicks. `ApplyAvcRate` now binds
the current discovery unit and validated route before requesting clock apply,
so it does not depend on a previous StartIO. The bounded `phase=bind` record
identifies missing context separately from a hardware rejection.

Regression batch: after fresh replug, change rate while idle before any
playback; confirm bind/apply/duplex readback/commit, then begin playback. Repeat
44.1 → 96 and return to 48. Missing or stale route must leave clocks unchanged.
