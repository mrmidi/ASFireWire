# Discovered AV/C controls and Core Audio

Discovery asks CURRENT for mute/volume on master channel 0 and every cluster channel. A successful volume CURRENT schedules MINIMUM, MAXIMUM, and RESOLUTION for that same subunit/block/channel. Snapshot feature records carry the attribute; commands and actual reply codes remain in graph probeResults. Graph featureChannels contains the confirmed state and signed raw volume attributes. No bitmap interpretation or another channel's range supplies missing attributes.

TA 1999008 10.3.2 defines signed 1/256 dB and the CURRENT/MINIMUM/MAXIMUM/RESOLUTION queries. Wire CONTROL/STATUS behavior was cross-checked against references/alsa-userspace-control-protocols-impl/protocols/bebob/src/lib.rs:312-350. Those reference implementations use device-specific limits; our generic publication requires device-reported limits instead.

ASFWAudioDevice publishes GUID-bound control descriptors through the nub. Each descriptor retains its AV/C subunit/block/channel token, presentation scope/element, name, initial values, and range. Volume requires a finite ordered range and positive resolution; missing, silence-sentinel, or invalid ranges do not become writable sliders. Mute is independent of volume and can publish alone.

Volume uses IOUserAudioLevelControl (VolumeControl), in dB. The scalar callback converts using AudioDriverKit's generated SDK _GetDecibelFromScalarValue interface, so ADK's scalar readback and writes use the same curve. A scalar slider position is not a linear PCM amplitude or an AV/C integer. AvcVolumeRange clamps dB and rounds to the reported resolution grid anchored at minimum. Non-finite inputs are rejected. Boolean mute maps to the AV/C 0x70/0x60 encodings.

The nub dispatches graph lookup and AV/C commands to the controller queue and waits on its own queue. The transaction owns its unit/route across CONTROL then CURRENT STATUS. Only a stable matching readback updates HAL and the graph. Rejected writes, wrong-channel replies, transition responses, or stale routes fail. A timed-out caller retires the continuation; already-submitted device commands cannot be undone. No transport/audio-buffer pointers cross this control seam.

Core Audio publishes only output-master channel 0 volume and mute. Generic
mapping accepts a fixed, channel-preserving playback master or an output-facing
feature reached through selectors from non-capture Audio source plugs. The latter
requires a complete capture channel identity map so a capture gain cannot be
mistaken for output gain, and stops at processing blocks instead of crossing a
mixer into input gains. Descriptor general_tag master purpose prefers a declared
output master among candidates. There are no Duet/Phase88 model or block-ID
checks. Missing/malformed routing can leave a control unpublished, including
Duet's broken feature reference when its playback boundary is absent. Each control class publishes only when exactly one eligible
master has confirmed state (and, for volume, valid device-reported limits and
step). Per-channel, input and internal mixer controls stay in discovery but are
not published to HAL. Competing master candidates are omitted independently
for volume and mute. Phase88's mixer master does not attenuate direct routes
that bypass the mixer; publication does not change selectors.

These controls affect hardware only. Volume and mute callbacks send AV/C commands and confirm device readback; they do not scale or silence the PCM output buffer. There is no second software attenuation stage. A monitor-mixer gain may affect monitoring without changing recorded samples or the direct playback path.

The GUID-based device UID is unchanged. Before publication, the driver enables SetWantsControlsRestored for discovered AV/C controls and SetWantsStreamFormatsRestored for stream formats. These request host-managed durable restoration, not storage in device firmware. Restoration uses the same checked callbacks. A confirmed HAL control write marks its feature block as user-owned; the BeBoB startup mixer skips defaults for that block so restart/rate changes cannot erase those settings. Other families retain their prior control-restore policy.

Duet and Phase88 offer discovered same-PCM/slot-shape rate intersections in the build's streamable 32/44.1/48 kHz range. GenericAvcProtocol adopts those published rates and reports its applied rate after clock programming. Single-rate generic profiles remain restricted. Unsupported saved formats remain subject to normal validation.

Stream mode prefers blocking in the supported transmit/receive intersection (TA 2001007 Tables 5.5/5.6). The identifier supplies supported flags when available; status supplies current capability flags otherwise. Missing flags retain catalog policy; validated device overrides take precedence over advertised flags (Duet blocking quirk: references/linux-sound-firewire-stack/firewire/oxfw/oxfw.c:164-167). One shared duplex mode cannot represent disjoint transmit/receive modes.

Validation: AvcFeatureControlTests covers conversion grids, invalid inputs, mute encoding, rejected control, stable readback, malformed/transition replies, reset and expired continuations. AvcAudioConfigTests covers range-gated publication, scopes/elements, routing ambiguity, reordered and partial streams, collisions and blocking preference. AvcGoldenTests covers per-channel limits, exact replies, immutable graph updates and stale-route rejection. Session characterization and dashboard render/report tests cover affected integration.

Hardware checks after installing this build: inspect all three volume attributes on Duet/Phase88; enumerate their Core Audio volume/mute objects; change one level and mute and compare CURRENT readback; select 44.1/48 kHz; verify values survive stop/start, replug, and reboot. Also check front-panel knob interaction: this bridge confirms host writes but does not add polling for external knob changes. No new live CONTROL was issued during implementation.

API sources: https://developer.apple.com/documentation/audiodriverkit/iouseraudiolevelcontrol and https://developer.apple.com/documentation/audiodriverkit/iouseraudioclockdevice/setwantscontrolsrestored and https://developer.apple.com/documentation/audiodriverkit/iouseraudiodevice/setwantsstreamformatsrestored . AddControl retains the control on success (SDK IOUserAudioClockDevice.iig:792-806).

Control diagnostics use `[AvcControl]` in the Audio log category. Initialization records discovered values/ranges and host restoration opt-ins. Every scalar, dB, and protocol boolean callback records input; AV/C transactions record readback or error kind/response and final IOReturn. Host restoration and manual HAL writes use the same callbacks, and the API does not identify their origin. Discovery reads device parameters before publication; ASFW does not directly read the host preference store. A restoration request is not proof that the host saved or restored a preference; verify callbacks and readback across replug/reboot.

Initialization emits an `[AvcControl] init-state` summary with GUID/sample rate and every descriptor's name, known-state flags, dB value, and mute state. This is the device-discovery/nub baseline, not a claimed completed host restoration. Subsequent callback/readback records identify what was actually applied; restoration has no completion/source notification in this API.


The ASFW Controls tab remains read-only, using the grouped discovery cards.
Blocks sort by subunit type/ID and numeric block ID; channels sort numerically,
with master first. No HAL enumeration, periodic refresh, write buttons or
progress indicators are installed in this view. Hardware changes are made
through Core Audio output-master controls. All discovery feature data remains
available for diagnostics.


TX wake independence (2026-10-04): Core Audio output-write and TX preparation
follow-up wakes send `TxPreparationReady(action, generation)` directly to the
retained OSAction. Generated IIG sets `kIORPCMessageOneway` and targets the
action, which is bound to the dedicated `TxPreparation` queue. The synchronous
`RequestTxPreparation` nub RPC was removed: it could wait behind a synchronous
AV/C control request on the nub queue. The same direct action path already
serves hardware refill wakes. Coalescing and teardown ownership remain intact.
AV/C control setters still return confirmed hardware state or an error; their
wait no longer gates those audio wake notifications.

`[AvcControlTrace]` records a request ID and wait-begin/controller-begin/
device-complete/wait-end with elapsed microseconds. It identifies controller
queue delay separately from device response time. `[TxPrepStall]` is emitted
only for queue or packet-preparation delays of at least 5 ms, with generation,
queueDelayUs, preparationUs, prepared count and PCM client frame/host cursor.
Client host ticks are the HAL cursor timestamp, not a measured callback-entry
time. Compare cursor advancement and queue delay to distinguish missing PCM
from an unscheduled producer; the packetizer already arms encoded silence.
Use the MCP driver ring query for `[AvcControlTrace]` in Audio and
`[TxPrepStall]` in DirectAudio, and preserve Isoch fatal records. A clean run
should have normal control traces and no preparation-stall records. Hardware
reproduction is still required after installation; host tests do not execute
DriverKit dispatch queues.


Generic master discovery no longer selects Duet/Phase88 by protocol ID or block
number. Audio destination plugs declared in the descriptor are included in
SIGNAL SOURCE STATUS discovery even if PLUG_INFO omits them (duplicate declared
plugs are queried once). Captured Duet Audio destination 0 to Music source 1
establishes its playback boundary without repairing its broken source-link
reference. The captured Phase88 graph identifies its output-facing mixer
feature through selectors while rejecting mixer input features. Explicit
input/output trim purpose does not become system master volume. Ambiguous
masters remain unpublished rather than being selected by label or model.

The Phase88 startup map no longer writes -35 dB to FB1 channels 1/2. It retains
those device levels and leaves master-volume writes to Core Audio. This removes
the preset, not existing stored attenuation; previously applied channel levels
are not automatically reset to unity. WavePlay setup and selector routing are
unchanged.
