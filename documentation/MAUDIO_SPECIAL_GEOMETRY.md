# M-Audio special-firmware stream geometry

The 1814 and ProjectMix profiles publish the S/PDIF 10-channel
capture / 6-channel playback geometry. They advertise 44.1, 48, 88.2, and
96 kHz: Linux's `bebob_maudio.c` formation table has the same S/PDIF channel
counts in the first two rate bands (`special_stream_formation_set`, lines
227–254). ProjectMix's reference clock list ends at 96 kHz. The 1814's
176.4/192 kHz formation is 2-channel capture / 4-channel playback and remains
unavailable through HAL.

ADAT is also unavailable through HAL. At 44.1/48 kHz it changes the formation
to 16-channel capture / 12-channel playback; at 88.2/96 kHz it is 12/8. The
format fields are independent for capture and playback. Linux changes them
through stopped-stream digital-interface controls and recalculates both stream
formations after each accepted control write (`bebob_maudio.c`, lines
458–582). The ASFW special-firmware profile has no corresponding user control.

## Work required before exposing ADAT or 176.4/192 kHz

The current ADK graph fixes input/output channel counts at publication. It
builds every advertised ASBD with those counts, sizes the direct audio buffers
from them, and keeps those counts when changing sample rate. A protocol-only
formation change would therefore make the wire DBS disagree with the HAL
stream and shared buffer geometry.

Add a stopped-only configuration transaction that:

1. Stops and confirms both isoch streams are quiescent.
2. Applies the requested clock rate and independent input/output digital
   formats to the device, then resolves the exact formation from the
   reference table.
3. Rebuilds both ADK stream format lists and current formats, channel names,
   direct input/output buffers, and the neutral transport control block from
   that formation as one graph generation.
4. Starts duplex streaming only after the new graph and protocol state agree;
   on any failure, restores the prior device settings and graph or leaves the
   endpoint stopped and unavailable.

Until that transaction exists, keep the fixed S/PDIF geometry and do not offer
ADAT or the 1814's 176.4/192 kHz rates.

## Duplex readiness gate

The special profile gives `ASFWAudioDevice::StartIO` the BeBoB 4-second
initial RX clock-anchor budget. StartIO waits for a timestamp seeded by a
data-bearing receive packet; if none arrives before the budget expires, its
`failStart` path calls `StopAudioStreaming` and releases the allocated TX
resources. The special profile test pins this timeout. This covers RX data
readiness and existing coordinator rollback, including the startup NO-DATA
period described by Linux `bebob_stream.c:10,636-666`.

There is not yet a host-IT completion/liveness signal that can gate duplex
success. `IsochTransmitContext` reports refill/preparation work, but that does
not prove a packet reached the wire. Validate host TX packet liveness during
the 1814 hardware run before considering duplex bring-up fully validated.
