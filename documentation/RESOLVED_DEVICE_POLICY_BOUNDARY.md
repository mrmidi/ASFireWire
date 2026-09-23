# Resolved audio-device policy boundary (FW-161 / FW-162)

**Status:** static decision contract; consumer migration remains FW-164.

## Producer and lifetime

`DeviceRegistry::UpsertFromROM` collects the Config-ROM evidence for all units of
one physical device and calls `AudioDeviceCatalog::Resolve` once for its
device-level classification. The result is a value: selected unit, match
provenance/candidates, support, family, pre-traffic probe policy, protocol
implementation, profile builder, static stream traits, and descriptive names.
The catalog remains pure and host-testable. It neither sends a frame nor owns a
protocol object.

The decision is valid for one device incarnation and observed bus generation.
Rebind after a reset must resolve from the new ROM evidence. A selected unit
offset alone cannot authorize traffic: the route token and generation still
have to be current when an operation executes. A safe family probe may later
refine an ambiguous candidate and produce a separate wire/configuration
snapshot for that stream epoch.

## Distinct decisions

| Decision | Owner | Current representation |
| --- | --- | --- |
| Physical identity, units, route validity | Discovery registry/controller | `DeviceRecord`, `DeviceRouteToken` |
| Support and pre-traffic policy | Audio catalog, enforced at the FCP submit gate | `SupportDisposition`, `ProbePolicyId`, command filter |
| Concrete protocol class | Audio catalog | `ProtocolImplementationId` |
| Endpoint/profile shape | Audio catalog | `ProfileBuilderId` |
| Validated wire configuration | Family probe/backend | Family runtime caps/profile |
| Nub/HAL geometry and timing | Audio publication and later timing work | Nub profile, Epic 3 |
| Start/stop/recovery | Coordinator and backend | Mutable runtime state |

`ProtocolImplementationId` is assigned by catalog definition identity, not by
`ProfileBuilderId`. A supported definition must name both. The constructor
switches on the implementation choice; profile builders may vary while the
protocol class stays the same. This changes no DICE or MOTU wire sequence.

## Consumer migration sequence

1. FW-162 completes the coherent static producer and safe refinement contract.
2. FW-164 carries the value from the discovery/controller handoff to the
   protocol registry, AV/C bootstrap, coordinator, backends, duplex planning,
   and nub publication. Generic runtime paths then stop resolving identity.
3. FW-165 removes superseded lookup helpers after agreement tests cover the
   existing catalogue and hazardous identities.

Until step 2, `DeviceRegistry::UpsertFromROM` and downstream consumers still
call `AudioDeviceCatalog::Resolve` independently. That duplication is tracked
work, not a second authority in the target design.

## Rejected fields and publication exceptions

The static decision does not contain mutable caps, clock state, DMA buffers,
protocol/nub ownership, route callbacks, or HAL timing policy. In particular,
it must not copy `midi::ResolvedAudioEndpointProfile` wholesale. DICE retains
its current live-capability comparison before nub publication. MOTU retains
its evidence-backed fixed chunk geometry before streaming because its live
caps are unavailable until the stream is prepared. Neither exception grants a
generic probe permission. M-Audio special firmware remains unsupported as
audio while the per-frame FCP allowlist remains active; bootloader cue execution
requires its separate hardware evidence gate.

The ownership audit and hardware questions are in Linear FW-160 and FW-168.
