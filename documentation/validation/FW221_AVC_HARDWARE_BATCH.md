# FW-221: AV/C production hardware batch

The production AV/C path now uses the host configuration window, complete
per-rate formations, route-bound STATUS confirmation, and one bounded verified
rollback attempt. DICE remains on its existing path. This is a validation batch,
not a claim that every descriptor-supported rate has passed hardware testing.

## Build

Normal builds keep the existing baseline advertisement. Enable candidates for
this batch explicitly:

```sh
./build.sh --no-bump --verbose \
  --set ASFW_AVC_MULTIRATE_VALIDATION=1 \
  --set CODE_SIGNING_ALLOWED=YES --set CODE_SIGNING_REQUIRED=YES \
  --set CODE_SIGN_IDENTITY=-
```

The dext must contain arm64e and have a verified signature. The hardware opt-in
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
per device/protocol/rate/mode, not from the generic unit-test table. Keep high rates
gated in normal builds until those records exist.
