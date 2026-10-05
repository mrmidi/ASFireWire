# FW-221 multi-rate port on current main

Status: **in progress; additional production rates remain gated**.

The clean 48 kHz AV/C endpoint is the regression baseline. The implementation
must not remove the single-rate HAL publication until the production transaction
and ADK layout experiment are complete. Passing host tests does not establish
hardware support.

## Implemented foundation

- NonBlockingCadence uses ceil-of-cumulative rational frame counts for all seven
  AM824 rates. Its fractional sequences were compared with Linux
  `references/linux-sound-firewire-stack/firewire/amdtp-stream.c:383-422`.
- The packetizer accepts both cadence modes and rejects configurations whose
  packet capacity cannot hold the maximum frames at the selected rate.
- ResolveAudioConfiguration combines a specific formation, timing policy and
  allocation capacity. It rejects missing/ambiguous rates, invalid slot maps,
  unsupported protocols, unvalidated hardware and insufficient capacity.
- ResolveTimingGeometry accepts allocation capacity explicitly; existing
  callers retain their current capacity and 48 kHz behavior.
- The variant configuration reducer was adapted from midi into Audio/Runtime.
  Accepted candidates retain immutable resolved snapshots, and publication is
  delayed until hardware confirmation and successful ADK projection. Competing
  intents are busy; unknown hardware or failed projection enters recovery.
- AV/C discovery retains different-width formations separately from the legacy
  same-shape supported-rate projection. This adds no discovery transactions.

## Current-main rate assumption inventory

| Producer/consumer | Current contract | Disposition / remaining work |
|---|---|---|
| AvcAudioConfig::BuildGraphAudioConfig | Startup rate only exposed to HAL | Replace after transaction/lab validation; retain all discovery formations |
| DiscoveryGraph::Geometry / ExtensionGeometry | Same-shape rates plus retained full formations | Complete per-rate descriptor/map selection at candidate resolution |
| GenericAvcProtocol::SupportedRates / DeviceCaps | Adopts published rates and one discovered geometry | Consume per-rate formations rather than replacing only the scalar rate |
| BeBoBProtocol::ApplyClockConfig | Output signal format, input signal format, settle | Preserve Linux bebob_stream.c:96-115 ordering; add confirmed outcome reporting |
| ApogeeDuetDuplex::ApplyClockConfig | Oxford-specific rate programming | Select reported formation; preserve vendor mute controls and ordering |
| MAudioFormationFor / MAudioSpecialProtocol::SupportedRates | Formation table covers six rates; runtime restricted to 48 | Resolve both digital directions independently; preserve hazardous-probe allowlist |
| DiceAudioBackend::RebuildEndpointForNewGeometry | Returns false for changed layouts | Replace with coordinated same-device ADK projection |
| ApplyDiceRuntimeCapsToDeviceConfig | Adopts observed channel counts only | Resolve the requested rate-mode configuration before apply, verify after apply |
| FamilyDriver::ApplyClockIdle / Configure / ReadHealth | Scalar clock, runtime caps returned separately | Add confirmed configuration outcome; errors must not imply hardware unchanged |
| SessionScheduler::ChangeClock / ApplyClockIdle | Reconciliation owns device lifecycle | Execute transaction effects; retain token/generation and verified rollback |
| AudioCoordinator::RequestClockConfig | Updates endpoint scalar on session success | Publish resolved configuration only after coordinated commit |
| ASFWAudioDevice::HandleChangeSampleRate | Pending scalar; rejects active IO | Replace with reducer and automatic host stop/change/restart |
| ASFWAudioDevice::PerformDeviceConfigurationChange / CommitSampleRate | Sequential setters, incomplete failure recovery | Separate apply/confirm/project/commit, restore only verified configurations |
| IOUserAudioStream default format callback | Independently sets stream format | Guard and route through the device transaction; prove in ADK lab |
| RequestExternalRateResync / persisted rate restoration | Separate scalar paths | Common intents with explicit origins; persist only committed configuration |
| ResolveTimingGeometry / HalBufferProfileForRate | Tier-dependent active ring; default 2x allocation | Endpoint-specific maximum validated capacity, including 4x when eligible |
| AudioEndpointRuntime::EnsureDirectAudioMemoryLocked | Allocates current channel width | Allocate maximum eligible width once; distinguish capacity from active stride |
| AudioGraphBinding / UpdateDirectAudioGeometry | Active memory view and scalar rate | Bind one committed revision; quiesce before changing views |
| HardwareSampleTimeline / HostClockAnchor | Existing rational clock conversion | Audit rounding, overflow and epoch reset for all rates |
| ASFWAudioDriverZts::PrepareTransmitSlots | RX replay plus special-family timing | Preserve source policy; audit all packet/frame bounds and rate gates |
| RationalBlockingCadence / NonBlockingCadence | Parameterized exact schedules | Host tested; validate wire seed and timing-source behavior on hardware |
| AmdtpTxPacketizer::Configure / PrepareNextPacket | Rate-specific FDF/cadence, bounded frames | New capacity checks; retain DBC/NO-DATA quirks |
| RxSytCadence / RxSequenceReplay | RX-derived timing | Test fractional phase, high-tier SYT, reset and wrap with one revision |
| ResolvedStreamConfig / RestartRoutine | Current formation determines transport resources | Build packet sizes/bandwidth from candidate, preserve CMP/IRM cleanup |
| Avc controls / channel labels | Discovery-scoped coordinates | Remap or withdraw on rate-mode changes; never reuse an invalid token |

## Required integration and validation sequence

1. Prove nominal-rate and stream-format callback ordering, failed projection,
   channel-count changes and stable device identity in ADKVirtualAudioLab.
   Its current dext still has a 48 kHz-only timing model; host tests alone cannot
   answer these runtime questions.
2. Connect candidate resolution and reducer effects to main's scheduler/nub/ADK
   boundary, replacing pendingSampleRateHz rather than keeping a second path.
3. Implement family formation catalogs and confirmed hardware outcomes; connect
   endpoint allocation, active geometry, controls and persisted preferences.
4. Run full host tests and signed build, then batch hardware validation on
   Phase88, Duet, 1814 and Saffire Pro 24 DSP. No hardware controls or installers
   are run as part of the source implementation.
5. Validate 48 baseline, 96, 44.1, 32, 88.2 and eligible 4x rates. Exercise idle
   and active changes, topology changes, rollback, reset/replug and restoration.
6. Unlock only configurations validated on the corresponding family/model.

Runtime transaction records must carry endpoint, origin, token, bus generation,
prior/requested/confirmed rate, formation revision and result. Hot-path telemetry
remains coarse heartbeat plus anomalies. Final acceptance also requires physical
continuity/RTL, correct clock slope, and no slow frame-buffer walk.
