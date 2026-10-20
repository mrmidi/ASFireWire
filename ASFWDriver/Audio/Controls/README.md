# Confirmed AV/C control changes (FW-259)

The immutable AV/C discovery graph remains authoritative. App and CoreAudio
writes still share WriteAvcFeature: CONTROL, STATUS readback, then
AVCUnit::RememberConfirmedFeature. The latter replaces the graph before
emitting one FeatureChange when its value actually changed. Repeated values,
unknown feature addresses and obsolete routes emit nothing.

FeatureChange contains AV/C facts only. ProjectAvcChange produces the same
pointer-free ParameterChanged payload for both consumers, with GUID,
discovery revision, sequence, parameter id, typed value and observed origin.
Volume values are the device's confirmed signed 1/256 dB converted to dB.

* Nub: sends ControlParameterChanged to the audio driver by OSAction-targeted
  one-way RPC. The driver matches its existing published AV/C control and calls
  SetDecibelValue / SetControlValue. It does not call HandleChange*.
* User client: sends the payload on the existing status action. Eight scalars:
  sequence, 0x4354524c, guid, revision, parameterId, kind, bits, origin.
  Kind and value bits match selectors 81/82; origin 0 means observed.
* App: subscribes to pushed changes, rejecting other devices, stale revisions
  and old sequences. It does not write back or fetch a snapshot for a value
  change. Selector 82 still collects an app write's asynchronous result.
  A later push wins over an older write result.

Snapshots add an optional sequence watermark, stored with the immutable
graph's values. A queued event already included in a fetched snapshot cannot
roll the display back. A new revision causes one snapshot refresh. This is
separate from topology revision: a value change does not invalidate ids.

The nub replays current graph values after registering, without bus traffic,
to close the discovery-to-registration window. These baseline messages use
sequence zero; a live value already delivered takes precedence.

## Lifetime and queues

FeatureChangeNotifications::Unregister drains callbacks under a control-plane
lock before a listener's action or ivars are released. Listener callbacks may
only enqueue notifications; they must not do synchronous queue dispatch, bus
I/O or registry re-entry. The controller never waits on the nub queue. The
generated IIG sender sets kIORPCMessageOneway and has no reply buffer.

Failed Start, Stop, repeated user-client Start, and free withdraw registrations.
Queued audio actions check their action identity and live graph before use.
The audio driver's retained control references are dropped during graph teardown.

## Verification

Host tests cover shared write/readback fan-out, different confirmed volume,
failed STATUS, repeated values, obsolete routes, sequence ordering, draining an
in-flight callback, the real audio-driver update handler and teardown.
Swift tests cover push decoding, updates without polling or echo writes, stale
messages, snapshot watermarks and a newer push overtaking a write result.

Hardware acceptance is still required: a published Phase 88 master mute changed
in Device Controls must flip in Audio MIDI Setup, and the reverse, without
Refresh. Check a device with a valid advertised AV/C volume range/resolution
for unchanged volume behavior and restore after reconnect. The captured Phase
88 data has no volume resolution, so host volume tests deliberately supply one.

This phase adds no AV/C NOTIFY subscription or device polling. A future provider
that observes external feature changes must publish confirmed observations
through the same graph-update/event path. Generic CoreAudio publication and
the AV/C-specific class migration remain FW-205.
