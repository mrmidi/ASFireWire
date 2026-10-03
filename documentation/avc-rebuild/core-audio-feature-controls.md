# Discovered AV/C controls and Core Audio

Discovery asks CURRENT for mute/volume on master channel 0 and every cluster channel. A successful volume CURRENT schedules MINIMUM, MAXIMUM, and RESOLUTION for that same subunit/block/channel. Snapshot feature records carry the attribute; commands and actual reply codes remain in graph probeResults. Graph featureChannels contains the confirmed state and signed raw volume attributes. No bitmap interpretation or another channel's range supplies missing attributes.

TA 1999008 10.3.2 defines signed 1/256 dB and the CURRENT/MINIMUM/MAXIMUM/RESOLUTION queries. Wire CONTROL/STATUS behavior was cross-checked against references/alsa-userspace-control-protocols-impl/protocols/bebob/src/lib.rs:312-350. Those reference implementations use device-specific limits; our generic publication requires device-reported limits instead.

ASFWAudioDevice publishes GUID-bound control descriptors through the nub. Each descriptor retains its AV/C subunit/block/channel token, presentation scope/element, name, initial values, and range. Volume requires a finite ordered range and positive resolution; missing, silence-sentinel, or invalid ranges do not become writable sliders. Mute is independent of volume and can publish alone.

Volume uses IOUserAudioLevelControl (VolumeControl), in dB. The scalar callback converts using AudioDriverKit's generated SDK _GetDecibelFromScalarValue interface, so ADK's scalar readback and writes use the same curve. A scalar slider position is not a linear PCM amplitude or an AV/C integer. AvcVolumeRange clamps dB and rounds to the reported resolution grid anchored at minimum. Non-finite inputs are rejected. Boolean mute maps to the AV/C 0x70/0x60 encodings.

The nub dispatches graph lookup and AV/C commands to the controller queue and waits on its own queue. The transaction owns its unit/route across CONTROL then CURRENT STATUS. Only a stable matching readback updates HAL and the graph. Rejected writes, wrong-channel replies, transition responses, or stale routes fail. A timed-out caller retires the continuation; already-submitted device commands cannot be undone. No transport/audio-buffer pointers cross this control seam.

Scope assignment is generic across AV/C devices. The graph joins Music channel positions to confirmed Audio/Music SIGNAL SOURCE boundaries, retaining logical channel, plug and subunit identities. Fixed, channel-preserving feature chains map to playback output or capture input elements. A master becomes the main element only when it covers the whole stream. Missing or duplicate positions, cycles, processors, mixers, multiple-input selectors, shared input/output gain and colliding controls remain named play-through controls with distinct addresses. The verified single-feature Duet override remains available when boundary evidence is missing. Phase88 monitor-mixer features are separate from its direct capture paths and remain internal; AMIS need not show those as input/output sliders.

These controls affect hardware only. Volume and mute callbacks send AV/C commands and confirm device readback; they do not scale or silence the PCM output buffer. There is no second software attenuation stage. A monitor-mixer gain may affect monitoring without changing recorded samples or the direct playback path.

The GUID-based device UID is unchanged. Before publication, the driver enables SetWantsControlsRestored for discovered AV/C controls and SetWantsStreamFormatsRestored for stream formats. These request host-managed durable restoration, not storage in device firmware. Restoration uses the same checked callbacks. A confirmed HAL control write marks its feature block as user-owned; the BeBoB startup mixer skips defaults for that block so restart/rate changes cannot erase those settings. Other families retain their prior control-restore policy.

Duet and Phase88 offer discovered same-PCM/slot-shape rate intersections in the build's streamable 32/44.1/48 kHz range. GenericAvcProtocol adopts those published rates and reports its applied rate after clock programming. Single-rate generic profiles remain restricted. Unsupported saved formats remain subject to normal validation.

Stream mode prefers blocking in the supported transmit/receive intersection (TA 2001007 Tables 5.5/5.6). The identifier supplies supported flags when available; status supplies current capability flags otherwise. Missing flags retain catalog policy; validated device overrides take precedence over advertised flags (Duet blocking quirk: references/linux-sound-firewire-stack/firewire/oxfw/oxfw.c:164-167). One shared duplex mode cannot represent disjoint transmit/receive modes.

Validation: AvcFeatureControlTests covers conversion grids, invalid inputs, mute encoding, rejected control, stable readback, malformed/transition replies, reset and expired continuations. AvcAudioConfigTests covers range-gated publication, scopes/elements, routing ambiguity, reordered and partial streams, collisions and blocking preference. AvcGoldenTests covers per-channel limits, exact replies, immutable graph updates and stale-route rejection. Session characterization and dashboard render/report tests cover affected integration.

Hardware checks after installing this build: inspect all three volume attributes on Duet/Phase88; enumerate their Core Audio volume/mute objects; change one level and mute and compare CURRENT readback; select 44.1/48 kHz; verify values survive stop/start, replug, and reboot. Also check front-panel knob interaction: this bridge confirms host writes but does not add polling for external knob changes. No new live CONTROL was issued during implementation.

API sources: https://developer.apple.com/documentation/audiodriverkit/iouseraudiolevelcontrol and https://developer.apple.com/documentation/audiodriverkit/iouseraudioclockdevice/setwantscontrolsrestored and https://developer.apple.com/documentation/audiodriverkit/iouseraudiodevice/setwantsstreamformatsrestored . AddControl retains the control on success (SDK IOUserAudioClockDevice.iig:792-806).

Control diagnostics use `[AvcControl]` in the Audio log category. Initialization records discovered values/ranges and host restoration opt-ins. Every scalar, dB, and protocol boolean callback records input; AV/C transactions record readback or error kind/response and final IOReturn. Host restoration and manual HAL writes use the same callbacks, and the API does not identify their origin. Discovery reads device parameters before publication; ASFW does not directly read the host preference store. A restoration request is not proof that the host saved or restored a preference; verify callbacks and readback across replug/reboot.

Initialization emits an `[AvcControl] init-state` summary with GUID/sample rate and every descriptor's name, known-state flags, dB value, and mute state. This is the device-discovery/nub baseline, not a claimed completed host restoration. Subsequent callback/readback records identify what was actually applied; restoration has no completion/source notification in this API.


The ASFW Controls tab enumerates HAL control objects on the exact GUID-bound
`ASFW-%016llX` device UID. Volume and mute objects are paired by scope/element;
ambiguous duplicate objects are disabled. The tab displays hardware names and
scope, reported dB limits, a dB slider, and mute buttons. A slider commits on
release. Writes serialize on an actor away from the main UI actor, re-resolve
the device/control identity, check writability, and use the existing driver
callbacks. Failed writes show an error and reload confirmed HAL values. A
one-second refresh reads HAL state only; it does not add device STATUS polling
or detect front-panel changes the driver has not received. Read-only discovery
cards remain available when no published HAL controls can be enumerated.

Phase88 selector-dependent physical output routing remains internal. Cross-check:
`references/alsa-userspace-control-protocols-impl/protocols/bebob/src/terratec/phase88.rs:248-296`
changes output selectors together with mixer selection. A permanent output scope
assignment would be wrong after such changes. The generic fixed-chain mapper is
retained; routing-dependent assignments require routing change notification and
linked-control synchronization before they can safely appear as AMIS sliders.
