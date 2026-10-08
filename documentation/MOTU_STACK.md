# MOTU stack

The current AudioDeviceHost owns discovery/publication; Session owns sequencing;
MOTU register adapters own device control; Audio/Wire/MOTU owns packed PCM and SPH.
Transport receives opaque packets. Model identity is Unit_Sw_Version, not root model_id.

Implementation proceeds on `feature/motu-stack` from the current host architecture.
The existing SPH comparison tools were committed separately as dc03d1f9.

## Delivery plan

1. Model/rate geometry table and V2 lifecycle correctness: asymmetric 8pre,
   Traveler fetch flags, completed stop writes, correct packet bandwidth, rate
   formations with packed rather than AM824 geometry.
2. Shared vendor-inspired SPH controller, driven by observed capture timestamps;
   preserve frame accounting, reject discontinuities, reset at a session boundary.
3. V3 register/framing support, starting with the 828mk3 observations in PR #172;
   extend only where local behavioral sources settle the wire contract.

No MOTU hardware is available for this implementation. Protocol support and hardware
validation are separate facts. PR #114/#116 contributor reports establish useful
V2 evidence; PR #172 supplies 828mk3 48 kHz observations, not a multi-model guarantee.

The model table records all known unit versions. V1 has different packet framing and
is recognized only. Hybrid 4x disagreement (828mk3 playback 12 versus Linux 14),
896mk3 conditional 1x widths, Track16 optical padding and Audio Express write-response
semantics must not be hidden behind a generic fallback. No ignored write errors.

## Behavioral sources

- `references/linux-sound-firewire-stack/firewire/motu/motu.c:164-180`: identities.
- `motu-protocol-v2.c:168-225,246-320`: fetching and mode/chunk tables.
- `motu-protocol-v3.c`: rate-change notification and optical banks.
- `motu-stream.c:135-225,376-420`: bandwidth, packet format and fetching order.
- Archived PRs: `tmp/motu-pr-evidence/README.md`, pinned source trees and discussions.
- Vendor analysis: `tmp/motu-research/vendor-four-gaps.md` and
  `vendor-controller-followup.md` (local research, not shipped dependencies).

SPH synthesis is vendor-inspired, not a bit-exact clone. Simulator error is phase
error against a synthetic noiseless sample clock; it is not measured DAC jitter.

## Delivered behavior and remaining limits

The shared `MotuProtocol` and `MotuProfile` replace the V2-only classes. All five
V2 identities are enabled with model-specific formations; no rate is called
hardware-validated. The graph retains the complete packed block stride even when
DBS is smaller than the PCM chunk count. IRM reserves that actual block size.
MOTU sets SPH, FMT 0x02, FDF 0x22 and end-event DBC; DATA SYT stays NO_INFO.
Fetch-enable errors participate in start confirmation. Stop awaits fetching mute
and deactivation, and a rejected stop remains retryable.

The 828mk3 FW path is experimental at its existing 48 kHz clock. Capture accepts
the observed `22ffffff` vendor word only in that codec; strict IEC decoding is
unchanged elsewhere. It reads both optical banks, uses the observed zero low half
on activation and the 48 kHz stream-config word. Unsupported current clocks refuse
publication. V3 clock changes are refused until a registered notification receiver
can implement CLK_CHANGED. The captured vendor prepare script's hard-coded
pseudo-addresses are not replayed without a host address owner. This path needs
hardware testing and is not claimed equivalent to PR #172's complete bring-up.

The TX-owned Q32 synthesizer observes the first replay-rebased SPH per DATA packet,
updates after at least 512 elapsed frames, uses full acquisition/quarter locked
gain, clamps to +/-2000 ppm and latches discontinuities for session recovery. It
retains RX packet cadence. Its initial presentation lead comes from the existing
duplex replay seam; PR #172's absolute-phase conditioning and measured 510-tick
setpoint are not ported. It is vendor-inspired, not the full vendor phase bridge.

V1, other V3 activation, notification ownership, live optical reconfiguration,
model-specific DSP/MIDI/volume and physical labels beyond known port maps remain
separate work. Those unknowns are recorded, rather than guessed or declared done.

## Verification

- Full host CTest run: 3,077 tests enumerated, no failures, six existing skips.
- Final playback-prefix regression: all 11 `MotuPayloadCodecTests` pass, including
  production-profile DATA/NO-DATA SPH, FDF, SYT and end-event DBC assertions.
- Focused ASan + UBSan run: 94 tests pass (model tables, protocol lifecycle,
  SPH synthesis, payloads and nub geometry serialization).
- `./build.sh --no-bump`: signed app/dext build succeeds. The produced dext
  contains both x86_64 and arm64e slices.

These checks validate host logic and compilation, not hardware interoperability.
No MOTU hardware or TSan run was used for this slice.
