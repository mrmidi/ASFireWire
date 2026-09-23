# Resolved audio-device policy boundary (FW-161)

**Status:** design decision. The implementation is recorded in local ticket
branches FW-162 through FW-165; this note remains the FW-161 ownership contract.

## Producer and lifetime

`DeviceRegistry::UpsertFromROM` collects the Config-ROM evidence for all units of
one physical device and calls `AudioDeviceCatalog::Resolve` for its
device-level classification. The target resolved result is a value: selected unit, match
provenance/candidates, support, family, pre-traffic probe policy, protocol
implementation, profile builder, static stream traits, and descriptive names.
The catalog remains pure and host-testable. It neither sends a frame nor owns a
protocol object.

The pure catalog plan is bound to one device incarnation and observed route in
`ResolvedDevicePolicy`; it holds a `DeviceRouteToken` but owns no route
lifetime. Rebind after a reset resolves from new ROM evidence. A selected unit
offset alone cannot authorize traffic: the registry must still accept the
token when an operation executes. A safe family probe may later refine an
ambiguous candidate and produce a separate wire/configuration snapshot, but
this branch has no generic safe-probe executor or configured constraint. An
ambiguity remains unadmitted until an evidenced, bounded probe exists.

## Distinct decisions

| Decision | Owner | Current or proposed representation |
| --- | --- | --- |
| Physical identity, units, route validity | Discovery registry/controller | `DeviceRecord`, `DeviceRouteToken` |
| Support and pre-traffic policy | Audio catalog, enforced at the FCP submit gate | `SupportDisposition`, `ProbePolicyId`, command filter |
| Concrete protocol class | Audio catalog | Separate selector (FW-162 proposes `ProtocolImplementationId`) |
| Endpoint/profile shape | Audio catalog | `ProfileBuilderId` |
| Validated wire configuration | Family probe/backend | Family runtime caps/profile |
| Nub/HAL geometry and timing | Audio publication and later timing work | Nub profile, Epic 3 |
| Start/stop/recovery | Coordinator and backend | Mutable runtime state |

The concrete protocol selector should be assigned by the catalog definition,
not inferred from `ProfileBuilderId`. A supported definition must name both.
The constructor should switch on the implementation choice; profile builders
may vary while the protocol class stays the same. This requires no change to
the DICE or MOTU wire sequence.

## Consumer migration sequence

1. FW-162 separates the protocol implementation selector from the profile
   builder and adds projections from one catalog plan.
2. FW-164 binds that plan to the observed route and carries it from the discovery/controller handoff to the
   protocol registry, AV/C bootstrap, coordinator, backends, duplex planning,
   and nub publication. Generic runtime paths then stop resolving identity.
3. FW-165 removes superseded lookup helpers and the unused safe-probe
   constraint scaffolding after agreement tests cover the existing catalogue
   and hazardous identities. FW-166 adds route and family contract tests.

The implemented route-bound carrier is intentionally small: one immutable
static plan and a token. It does not own the measured wire geometry, nub,
protocol, or in-flight work. Discovery invalidates its binding on reset, loss,
or duplicate-GUID quarantine.

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
