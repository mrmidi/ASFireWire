# MOTU stack

AudioDeviceHost owns discovery/publication; Session owns sequencing; the shared
MOTU register adapter owns device control; Audio/Wire/MOTU owns packed PCM and SPH.
Transport receives opaque packets. Model identity is Unit_Sw_Version, not root model_id.

## Scope and model checklist

The implementation on `feature/motu-stack` supplies these FireWire-only identities.
V1 activation requires `ASFW_MOTU_V1_VALIDATION=1` (default 0).
A checked row means implemented and host-tested, **not hardware-validated**.

| Implemented | Device | Protocol | Offered rates (kHz) |
|---|---|---|---|
| [x] opt-in | Original 828 | V1 | 44.1, 48 |
| [x] opt-in | Original 896 | V1 | 44.1, 48; 2x withheld |
| [x] | 828mk2 | V2 | 44.1, 48, 88.2, 96 |
| [x] | 896HD | V2 | 44.1, 48, 88.2, 96, 176.4, 192 |
| [x] | Traveler | V2 | 44.1, 48, 88.2, 96, 176.4, 192 |
| [x] | UltraLite | V2 | 44.1, 48, 88.2, 96 |
| [x] | 8pre | V2 | 44.1, 48, 88.2, 96 |
| [x] | 828mk3 (FireWire) | V3 | 44.1, 48, 88.2, 96, 176.4, 192 |
| [x] | 896mk3 (FireWire) | V3 | 44.1, 48, 88.2, 96, 176.4, 192 |
| [x] | UltraLite mk3 (FireWire) | V3 | 44.1, 48, 88.2, 96, 176.4, 192 |
| [x] | Traveler mk3 | V3 | 44.1, 48, 88.2, 96, 176.4, 192 |

USB/hybrid models are excluded by request: UltraLite mk3 Hybrid, Audio Express,
828mk3 Hybrid, 896mk3 Hybrid, Track16 and 4pre. They remain recognized, without
activation or generic geometry fallback. Their unresolved geometry and response
quirks remain recorded in the model table/research.

No MOTU hardware is available for this implementation. PR #114/#116 contributor
reports supply V2 evidence; PR #172 supplies 828mk3 48 kHz observations, not a
multi-model guarantee. Hardware validation remains false in every formation.

## Behavioral sources

All paths below are local behavioral references, not code copied into the driver.

- `references/linux-sound-firewire-stack/firewire/motu/motu.c:164-180`: identities.
- `motu-protocol-v1.c:10-118,186-248,336-461`: V1 clock, fetching, framing and widths.
- `motu-protocol-v2.c:168-225,246-320`: fetching and mode/chunk tables.
- `motu-protocol-v3.c:62-113,185-239,274-320`: clock notification, optical banks and widths.
- `motu-transaction.c:37-67,95-133`: notification region and registration.
- `motu-stream.c:62-105,135-225,227-307`: activation, stop, bandwidth and packet format.
- Archived PRs: `tmp/motu-pr-evidence/README.md`, pinned sources and discussions.
- Vendor analysis: `tmp/motu-research/vendor-four-gaps.md`,
  `vendor-controller-followup.md`, and `tmp/motu-controller/gap-0x3f080.txt`.

The 896mk3 vendor branch at 0x3f118 compares provider Gestalt with 0x31333934
("1394"). FireWire selects 18 playback chunks at 1x, agreeing with Linux; the
16-chunk branch belongs to other providers. The non-hybrid ambiguity is resolved.

## Delivered behavior

The shared `MotuProtocol` and `MotuProfile` supply model-specific formations,
actual packed block stride, and correct IRM packet bandwidth. Host channel
capacity accommodates 34 capture chunks with both V3 ADAT banks enabled at 1x.
The 32-slot AM824 map and its masks retain their separate limits. Channels and
buffer capacities survive serialization and rate changes without remapping ADK memory.
Padding chunks are retained; chunk counts do not promise physical port counts.

V1 PCM begins immediately after SPH at byte 4. Original 828 capture has two trailing
status chunks; playback has none. Original 896 has neither. Their clock and fetching
registers differ from V2/V3. Original 828 waits 100 ms on Session after host streaming
starts, before fetching; original 896 stop preserves its irreversible output-on bits.
The original 896 reserves 18 chunks in both directions at 1x. Its 2x modes are
withheld because Linux and vendor optical behavior disagree. V1 is recognized
but not offered to users in default builds; the build flag enables validation only.

V2 keeps asymmetric 8pre formations and Traveler fetching flags. Fetch-enable errors
participate in start confirmation. Stop awaits mute and deactivation; failed stop
writes remain retryable. MOTU sets SPH, FMT 0x02, FDF 0x22 and end-event DBC;
DATA SYT stays NO_INFO.

V3 uses both optical banks and every model-supported rate. Each protocol owns a
lifetime-held notification mailbox and an aligned host quadlet. Registration is
refreshed explicitly by discovery after a route change, including idle devices.
Geometry/rate work also verifies registration before proceeding. Clock switching clears
fetch, writes the rate, awaits both write ACK and matching CLK_CHANGED, then verifies
clock readback. Missing notifications time out after four seconds. Route changes,
foreign source nodes, stale generations and shutdown cannot complete the wrong wait.
Shutdown's two address-release writes own their IO state through completion, even
if the protocol object has been destroyed. All MOTU publication refuses a failed
geometry read instead of presenting fixed counts that may omit optical channels.
The receiver handles notifications for requested clock changes; unsolicited front-panel
clock/optical changes do not yet drive graph rebuilding.

Capture accepts the observed `22ffffff` V3 word only in the MOTU codec; strict IEC
decoding is unchanged elsewhere. The observed 828mk3 activation pre-stop and zero
low half remain model-scoped; its stream-config word is limited to 48 kHz. Other
models/rates use the reference register sequence, preserving unrelated low bits.
The captured vendor prepare script's hard-coded pseudo-addresses are not replayed.
This is best-effort activation, not a claim of equivalence to PR #172's full bring-up.

Default builds preserve the per-block observed-offset replay policy for every
model. Each model selects timing and activation policy in `MotuModel.hpp`; there
is no MOTU rate-admission branch in Session. Admission consumes offered formations.
V3 synthesis is an explicit per-model experiment enabled only by
`ASFW_MOTU_SYNTH_VALIDATION=1` (default 0). This flag does not claim equivalence
to PR #172 or vendor hardware behavior.

The experimental TX-owned Q32 SPH synthesizer observes the first replay-rebased SPH per DATA
packet, updates after at least 512 elapsed frames, uses full acquisition/quarter
locked gain and clamps to +/-2000 ppm. Valid phase discontinuities re-anchor
the presentation controller inside the stream, keeping packet cadence and audio
frame accounting. Lock is reacquired before timing readiness is restored; sustained
unusable/missing timing still uses session recovery. Epoch changes reset acquisition.
It retains RX packet cadence. Initial presentation lead comes from the existing
duplex replay seam; PR #172's absolute-phase conditioning and measured 510-tick
setpoint are not ported. It is vendor-inspired, not a bit-exact vendor clone.
Simulator phase error against a synthetic clock is not measured DAC jitter.

The generic Session readiness gate waits after both host contexts start and before
fetch/unmute confirmation. Replay readiness requires a successfully stamped packet;
experimental synthesis requires phase lock. The TX producer runs on its independent
preparation queue while StartIO waits. Timeout and teardown unwind startup before
fetch is enabled. Original 828 also retains its 100 ms host-packet settling interval.

## Remaining validation and work

- Hardware checks for activation, audible playback/capture, all supported rates,
  optical combinations, and unplug/reset recovery for each newly enabled model.
- Automatic graph reconfiguration for unsolicited clock/optical notifications.
- Model-specific DSP, MIDI, volume and physical labels beyond existing port maps.
- Original 828 status interpretation beyond reserving the documented tail bytes.
- Reconcile original 896 optical behavior and complete the vendor SPH phase bridge.

## Verification (2026-10-08 review fixes)

- Full host CTest: 3,105 tests enumerated, zero failures, six existing skips.
- ASan + UBSan: 173 tests pass across eight protocol, payload, synthesis,
  buffer, nub serialization, family-adapter and session suites.
- TSan: 112 tests pass across control, notifications, timing and session suites.
- Opt-in build (`ASFW_MOTU_V1_VALIDATION=1`, `ASFW_MOTU_SYNTH_VALIDATION=1`):
  35 profile, policy, timing and payload tests pass.
- Python timing/probe tools: all 20 tests pass.
- `./build.sh --no-bump`: app/dext build succeeds with x86_64 and arm64e slices.
  The script disables code signing; this output requires signing before installation.
  Existing analyzer warnings in SCSI, bus timing and AV/C are outside this change.

Regression coverage includes uneven per-block SPH replay, phase reacquisition while
DATA continues, epoch reset, producer timing readiness before confirmation, timeout
rollback and teardown cancellation, generic formation admission, idle discovery
re-registration, stale registration completions, and failed-read publication refusal.

The build flags can be passed with `./build.sh --no-bump --set
ASFW_MOTU_V1_VALIDATION=1 --set ASFW_MOTU_SYNTH_VALIDATION=1` for validation.
Default builds leave both flags at zero.

For hardware tracing, use `/usr/bin/log stream --level debug --predicate
'eventMessage CONTAINS "[SessionTiming]" OR eventMessage CONTAINS "[MotuGeometry]"
OR eventMessage CONTAINS "MotuProtocol"'`. The readiness line is emitted once per
start; a timeout identifies `TransmitTimingReady` in the session rollback log.

These checks validate host logic and compilation, not hardware interoperability.
