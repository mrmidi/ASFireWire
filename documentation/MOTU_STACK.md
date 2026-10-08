# MOTU stack

AudioDeviceHost owns discovery/publication; Session owns sequencing; the shared
MOTU register adapter owns device control; Audio/Wire/MOTU owns packed PCM and SPH.
Transport receives opaque packets. Model identity is Unit_Sw_Version, not root model_id.

## Scope and model checklist

The implementation on `feature/motu-stack` enables these FireWire-only identities.
A checked row means implemented and host-tested, **not hardware-validated**.

| Implemented | Device | Protocol | Offered rates (kHz) |
|---|---|---|---|
| [x] | Original 828 | V1 | 44.1, 48 |
| [x] | Original 896 | V1 | 44.1, 48, 88.2, 96 |
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
- `motu-stream.c:62-105,135-225,376-420`: activation, stop, bandwidth and packet format.
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
The original 896 reserves 18 chunks in both directions at 1x and 2x like Linux;
the vendor's optical-at-1x policy still needs hardware reconciliation.

V2 keeps asymmetric 8pre formations and Traveler fetching flags. Fetch-enable errors
participate in start confirmation. Stop awaits mute and deactivation; failed stop
writes remain retryable. MOTU sets SPH, FMT 0x02, FDF 0x22 and end-event DBC;
DATA SYT stays NO_INFO.

V3 uses both optical banks and every model-supported rate. Each protocol owns a
lifetime-held notification mailbox and an aligned host quadlet. Registration is
refreshed before geometry/rate work after a route change. Clock switching clears
fetch, writes the rate, awaits both write ACK and matching CLK_CHANGED, then verifies
clock readback. Missing notifications time out after four seconds. Route changes,
foreign source nodes, stale generations and shutdown cannot complete the wrong wait.
Shutdown's two address-release writes own their IO state through completion, even
if the protocol object has been destroyed. V1/V3 publication refuses a failed
geometry read instead of presenting fixed counts that may omit optical channels.
The receiver handles notifications for requested clock changes; unsolicited front-panel
clock/optical changes do not yet drive graph rebuilding.

Capture accepts the observed `22ffffff` V3 word only in the MOTU codec; strict IEC
decoding is unchanged elsewhere. The observed 828mk3 activation pre-stop and zero
low half remain model-scoped; its stream-config word is limited to 48 kHz. Other
models/rates use the reference register sequence, preserving unrelated low bits.
The captured vendor prepare script's hard-coded pseudo-addresses are not replayed.
This is best-effort activation, not a claim of equivalence to PR #172's full bring-up.

The TX-owned Q32 SPH synthesizer observes the first replay-rebased SPH per DATA
packet, updates after at least 512 elapsed frames, uses full acquisition/quarter
locked gain, clamps to +/-2000 ppm and latches discontinuities for session recovery.
It retains RX packet cadence. Initial presentation lead comes from the existing
duplex replay seam; PR #172's absolute-phase conditioning and measured 510-tick
setpoint are not ported. It is vendor-inspired, not a bit-exact vendor clone.
Simulator phase error against a synthetic clock is not measured DAC jitter.

## Remaining validation and work

- Hardware checks for activation, audible playback/capture, all supported rates,
  optical combinations, and unplug/reset recovery for each newly enabled model.
- Automatic graph reconfiguration for unsolicited clock/optical notifications.
- Model-specific DSP, MIDI, volume and physical labels beyond existing port maps.
- Original 828 status interpretation beyond reserving the documented tail bytes.
- Reconcile original 896 optical behavior and complete the vendor SPH phase bridge.

## Verification

- Full host CTest: 3,095 tests enumerated, zero failures, six existing skips.
- ASan + UBSan: 126 tests pass across protocol, payload, synthesis, buffer,
  nub serialization and family-adapter suites. Includes delayed shutdown completions
  after destroying the protocol object and the 34-channel rate-change regression.
- TSan: 10 tests pass for V1/V3 control, notification concurrency and shutdown ownership.
- Python timing/probe tools: all 20 tests pass.
- `./build.sh --no-bump`: app/dext build succeeds with x86_64 and arm64e slices.
  The script disables code signing; this output requires signing before installation.

These checks validate host logic and compilation, not hardware interoperability.
