# Adding FireWire audio hardware

This guide describes the device resolution and bring-up path in the driver. A
catalog row is a statement about observed Config ROM evidence, not proof that a
device streams correctly. Keep a new device `RecognizedUnsupported` until its
protocol, geometry, and startup behavior have evidence and tests.

## Where each fact belongs

| Fact | Owner | What to supply |
| --- | --- | --- |
| Observed identity and selected unit | [`DeviceIdentityEvidence`](../ASFWDriver/Discovery/DiscoveryTypes.hpp), [`AudioDeviceCatalog`](../ASFWDriver/DeviceProfiles/Audio/AudioDeviceCatalog.hpp) | Config ROM GUID, root keys, and each unit directory with its offset, specifier, and version. A root `model_id` of zero is present evidence, not an absent key. |
| Static decision | [`Definitions/`](../ASFWDriver/DeviceProfiles/Audio/Definitions), [`DeviceRegistry::UpsertFromROM`](../ASFWDriver/Discovery/DeviceRegistry.cpp) | Match clause, support state, family, probe policy, profile builder, protocol implementation, and any justified stream traits. Registry resolves once and binds the result to a route in [`ResolvedDevicePolicy`](../ASFWDriver/DeviceProfiles/Audio/ResolvedDevicePolicy.hpp). |
| Pre-traffic safety | Definition probe policy or [`AudioSafetyRule`](../ASFWDriver/DeviceProfiles/Audio/AudioDeviceCatalog.hpp), [`CommandFilterFor`](../ASFWDriver/DeviceProfiles/Audio/AudioDeviceResolver.cpp), [`FCPTransport`](../ASFWDriver/Protocols/AVC/FCPTransport.cpp) | A definition carries permitted traffic for a recognized persona; a safety rule can quarantine a hazardous identity. Audio support and command permission are separate decisions. |
| Bootstrap and protocol class | [`SelectProbeBootstrap`](../ASFWDriver/Audio/Protocols/SelectProbeBootstrap.hpp), [`FamilyProtocolConstruction`](../ASFWDriver/Audio/Protocols/FamilyProtocolConstruction.cpp) | Reuse an existing family implementation when its wire behavior fits. Add a new protocol implementation only for a real protocol difference. |
| Profile and wire geometry | [`AudioProfileRegistry`](../ASFWDriver/Audio/DriverKit/Config/AudioProfileRegistry.cpp), family profile, [`DiceAudioBackend`](../ASFWDriver/Audio/Protocols/Backends/DiceAudioBackend.cpp), [`MotuAudioBackend`](../ASFWDriver/Audio/Protocols/Backends/MotuAudioBackend.cpp) | Document the source of per-stream channels, slots, sample rates, and codec. DICE publication requires validated runtime stream geometry. The supported MOTU 828mkII and UltraLite use their known fixed chunk layout for publication before live caps exist; other named MOTU models remain unsupported. |
| Stream lifecycle policy | [`DeviceStreamTraits`](../ASFWDriver/DeviceProfiles/Audio/AudioDeviceCatalog.hpp), [`DuplexStreamProfileResolver`](../ASFWDriver/Audio/Protocols/Backends/DuplexStreamProfile.hpp) | Only evidenced differences such as start order, CMP-owned channels, or conditional wire format. The family backend retains the actual prepare/start/stop sequence. |
| HAL and timing geometry | [`Audio/Runtime/`](../ASFWDriver/Audio/Runtime), [`Audio/DriverKit/`](../ASFWDriver/Audio/DriverKit) | Buffer timing, CoreAudio format, and service lifetime are later concerns. Do not encode them as Config ROM identity or wire geometry. |

The selected catalog plan carries static facts. Live registers and probe replies
carry runtime facts. `DeviceRouteToken` ties the plan to one generation and node;
reset, loss, or duplicate-GUID quarantine invalidates that binding. Consumers
must use the current policy rather than resolving the identity again. A stale
callback must not publish a nub or send a device command to a rebound route.
There is currently no generic safe-probe refinement step for ambiguous catalog
matches. A new probe must name its permitted commands, cancellation boundary,
and evidence before it can resolve an ambiguity; missing evidence leaves the
device unadmitted.
The `AudioSafetyRule` table is currently empty. It is a quarantine mechanism;
per-frame AV/C restrictions come from the matched definition's probe policy
through `CommandFilterFor` and `FCPTransport`.

## Add a member of an existing family

1. Save the device's Config ROM and identify the matching **unit directory**.
   Record the GUID, root vendor/model keys, unit specifier/version, and unit
   offset. Note absent keys separately from keys with value zero. Include the
   source of any inferred model ID. Add a fixture using that evidence in
   [`AudioDeviceCatalogTests.cpp`](../tests/devices/AudioDeviceCatalogTests.cpp).
2. Add one definition under [`Definitions/`](../ASFWDriver/DeviceProfiles/Audio/Definitions).
   Reuse a family, probe policy, and protocol implementation only when the
   reference stack or device capture supports that choice. For example,
   [`Focusrite.hpp`](../ASFWDriver/DeviceProfiles/Audio/Definitions/Focusrite.hpp)
   shows ordinary DICE models sharing `ProtocolImplementationId::DiceTcat`
   while selecting distinct profile builders. If there is no implemented
   profile yet, keep the row `RecognizedUnsupported` with no builder or
   protocol implementation. Register a new definitions array in
   [`AudioDeviceCatalog.cpp`](../ASFWDriver/DeviceProfiles/Audio/AudioDeviceCatalog.cpp)
   and update its count assertion. Add a definition or builder ID in
   [`AudioDeviceCatalog.hpp`](../ASFWDriver/DeviceProfiles/Audio/AudioDeviceCatalog.hpp)
   when an existing ID cannot describe it.
3. Supply a profile or geometry rule only where the device needs one. The
   profile builder is an endpoint/geometry selection, while
   `ProtocolImplementationId` selects the concrete protocol class. Update
   [`AudioProfileRegistry`](../ASFWDriver/Audio/DriverKit/Config/AudioProfileRegistry.cpp)
   when adding a builder. A row may reuse both existing selectors. Do not add
   a vendor/model branch to discovery, the coordinator, a generic backend, or
   the duplex resolver. A genuinely new protocol implementation requires an
   enum value and construction arm in
   [`FamilyProtocolConstruction.cpp`](../ASFWDriver/Audio/Protocols/FamilyProtocolConstruction.cpp);
   update its exhaustive checks and add protocol tests.
4. Add a stream trait only with evidence. For example, the Saffire Pro 24 DSP
   row records a conditional raw 24-bit PCM format, and
   [`DuplexStreamProfileResolver`](../ASFWDriver/Audio/Protocols/Backends/DuplexStreamProfile.hpp)
   applies it only when the runtime slot geometry is eight PCM channels in
   nine slots. A catalog identity alone cannot determine that live format.
5. Run the host agreement and geometry tests below. Then validate discovery,
   publication, StartIO, StopIO, reset/rebind, and teardown on the actual
   hardware before claiming support. Host tests cannot establish a safe wire
   sequence or prove that a device reports truthful geometry.

### A device that needs special preparation

The M-Audio rows in [`MAudio.hpp`](../ASFWDriver/DeviceProfiles/Audio/Definitions/MAudio.hpp)
show why support, traffic permission, and preparation are independent. The
1814 bootloader persona is `RecognizedUnsupported`, has
`NoAutomaticTraffic`, and blocks every FCP command. `BootloaderCuePolicy` is
descriptive and also enforces `BlockAll`; this branch does **not** execute a
firmware cue. The special-firmware 1814 and ProjectMix personas remain
unsupported as audio but carry a narrow FCP allowlist. A future preparation
path must validate the required command and hardware response, bind the attempt
to the route token, cancel on reset/removal, and keep failure from falling
through to generic AV/C probing. Do not infer a cue sequence from a catalog
field or mark these personas playable to make discovery advance.

## Tests and review

Use the existing tests as contracts, extending a fixture only for a new fact:

- [`AudioDeviceCatalogTests.cpp`](../tests/devices/AudioDeviceCatalogTests.cpp)
  checks match evidence, ambiguity, support, and safety rules.
- [`CatalogMatcherAgreementTests.cpp`](../tests/devices/CatalogMatcherAgreementTests.cpp)
  checks that one plan drives backend, bootstrap, protocol, profile builder,
  and command filter consistently across DICE, BeBoB, OXFW, Fireworks, and MOTU.
- [`DeviceIdentityEvidenceTests.cpp`](../tests/discovery/DeviceIdentityEvidenceTests.cpp)
  checks route binding and invalidation on reset, loss, and duplicate GUID.
- [`DiceRuntimeDeviceConfigTests.cpp`](../tests/devices/DiceRuntimeDeviceConfigTests.cpp),
  [`DuplexStreamProfileTests.cpp`](../tests/devices/DuplexStreamProfileTests.cpp), and
  [`NubGeometryRoundTripTests.cpp`](../tests/audio/NubGeometryRoundTripTests.cpp)
  check stream geometry, wire traits, and nub geometry changes.

Run `./build.sh --test-only` for host tests and `./build.sh --no-bump` for the
DriverKit build. Hardware validation remains a separate gate. For wire-visible
changes, compare the local read-only reference stacks in `references/` before
implementing; those sources are not copied into the driver.

Review a contribution for these failure modes:

- A Supported row without a concrete protocol and profile builder, or a
  generic fallback presented as playable audio.
- A hazardous or ambiguous identity reaching unrestricted AV/C traffic.
- A second vendor/model decision in generic runtime code, or a cached policy
  surviving its route token.
- Profile-only DICE geometry published as if it were validated live geometry,
  or a changed nub geometry silently reused.
- A startup or stop sequence changed to accommodate a model without a device
  capture or reference-stack basis. Preserve known-good DICE ordering.
