# MOTU research probe

Ongoing research, not a final protocol specification. Correct and extend these
tools and fixtures as stronger IDA, reference or hardware evidence becomes available.

The Python probe uses the existing ASFW loopback MCP client. No third-party
Python packages are required. Enable the MCP Control Plane in the ASFW app.
The probe discovers the target by GUID and pins each read to its bus generation.
It accepts only a MOTU discovery vendor identity. `--model` and `--firmware` are
operator declarations: the current node summary does not verify the unit version.
Check them against the device and cached Config ROM before interpreting results.

## Snapshot and export

```sh
python3 tools/motu/motu_probe.py snapshot \
  --guid 0xYOUR_GUID --model 828mk3-fw --firmware unknown \
  --phase idle --notes '48 kHz, internal clock, optical banks disabled' \
  --output tmp/motu-research/captures/mk3-idle.json

python3 tools/motu/motu_probe.py export \
  tmp/motu-research/captures/mk3-idle.json \
  --output tmp/motu-research/fixtures/mk3-idle.registers.json
```

Replace the GUID placeholder with the actual discovered GUID. Use a unique output
filename each time; existing files are never overwritten. Override the endpoint
with `--endpoint` or `ASFW_MCP_ENDPOINT` if the app uses another port.

Each JSON snapshot preserves discovery identity, generation, running driver build,
stream health, raw register responses, minimal decoded fields and operator notes.
Failed reads or route changes produce a partial snapshot with `complete: false`
and a nonzero exit status. Partial or telemetry-only snapshots cannot become
register fixtures. Successful exports remain `validatedGolden: false` until reviewed.
The source hash covers canonical sorted-key JSON, not the input file formatting.

During any active audio stream, register reads are skipped and only cached health
is collected. `--telemetry-only` also avoids register transactions when idle.
Idle checks are best effort: do not start audio concurrently with an idle probe.
Snapshots are sequential observations, not atomic device state. Front-panel changes
during probing can mix configurations even without a generation change.

## Contributor capture matrix

1. Record physical model, firmware, connection type, host OS and running driver build.
2. Collect idle state at each supported/tested rate and clock source. Change settings
   manually; the probe never writes settings or asserts unsupported combinations.
3. Repeat for independent optical bank/mode configurations that are safe in the
   installed driver. Record the full configuration in `--notes`.
4. Collect `--phase playing` and `--phase recording` snapshots during normal audio
   use; these contain cached telemetry, not active-stream register reads.
5. Collect `--phase stopped`, `--phase after-reset` and `--phase after-wake` once the
   driver is ready and audio is idle. GUID selection resolves the new node ID.
6. Supply packet dumps separately from the existing capture tool or vendor passive
   capture setup. Optionally attach an existing UTF-8 dump with `--packet-capture`
   and `--packet-source asfw|vendor-driver|unknown`. Attachments are preserved and
   hashed, not parsed, timestamp-correlated or validated by this initial probe.

This script does not arm packet capture, start/stop streams, force resets or change
clock/optical settings. A register snapshot cannot prove SPH trajectory or controller
equivalence. Packet capture integration requires the finalized tooling PR interface.

## Fixture rules

- Hardware observations keep source/model/firmware/configuration provenance and raw results.
- Reference-derived vectors cite the exact IDA or Linux/FFADO evidence separately.
- Synthetic loss/drift/wrap/failure cases must be explicitly labeled synthetic.
- Never generate a missing model's "hardware golden" from another model or from our
  implementation's own output. Missing-device synthesis remains pending model proof.
- Live mailbox addresses are generation/topology-specific observations, not constants
  to replay. Register fixtures are readback inputs; they are not write recipes.

Reference entry points: local `motu-protocol-v1.c`, `motu-protocol-v2.c`,
`motu-protocol-v3.c`, `motu-stream.c:23`, and the vendor evidence under
`tmp/motu-research/motu-vendor`. Model-specific geometry is deliberately not
reconstructed by this first probe.

## Hardware-free checks

```sh
python3 -m unittest discover -s tools/motu -p 'test_*.py' -v
```

Checks cover stale routes, active-stream admission, failed transactions, provenance,
generation zero, endian decoding and independent optical directions. They issue no
hardware requests. Run the contributor matrix before treating observations as goldens.

## SPH timing comparison

`compare_sph.py` is a standard-library-only, hardware-free timing simulator:

```sh
python3 tools/motu/compare_sph.py
python3 tools/motu/compare_sph.py --rate 44100 --seconds 10 --drift-ppm 200
python3 -m unittest discover -s tools/motu -p test_compare_sph.py
```

Default output is `tmp/motu-sph-comparison/`: per-scenario CSV traces, SVG error
plots and `summary.json`. Scenarios are steady clock drift, a drift reversal,
SPH timestamp noise, and a missing packet. The default start crosses the
one-second timestamp wrap. Supported rates are 44.1 through 192 kHz.

Replay preserves incoming blocking cadence and translates each frame's SPH
relative offset to the outgoing packet cycle. Synthesis now uses the controller
behavior tested in PR #172: cumulative RX ticks and frames, feedback from the
correction actually emitted on TX, and a Q32 fractional sample-time accumulator.
It measures over buffer windows (`--window-frames`, default 512), rather than
reacting to each audio block. Phase correction divides by the frames that
actually elapsed, so a delayed update does not increase the loop gain.

Acquisition uses full gain (`--acquisition-shift 0`); locked tracking uses quarter
gain (`--locked-shift 2`). Lock requires error below 61.44 ticks and stays latched
until error exceeds four bus cycles. Step limits remain a simulation policy
(`--clamp-ppm`, default 2000), not a recovered vendor limit. The pure-controller
API also accepts an absolute phase seed: it targets the captured +510-tick
operating point and folds around the supplied phase center to avoid branch-cut
jumps. The comparison itself starts at zero relative error; it does not model
the telemetry conditioner that discovers an absolute operating point.

Both algorithms use an identical fixed observation/buffering delay and are
compared to the noiseless device timeline translated by that delay.
Timestamp quantization is one tick.
CSV frame indices identify samples, allowing comparison even when packet cadence
differs. `cadence_mismatches` counts cycles with differing frame counts.

**Scope:** replay matches the referenced timing arithmetic, not the entire Linux
stream engine or its startup cache. Synthesis implements PR #172's tested loop,
not a bit-exact clone of the vendor algorithm. The observation bridge, absolute
phase conditioner and hard-resync placement remain simplified. Jitter means
independent noise on SPH values, not DMA arrival latency. Noise results therefore
cannot rank the actual drivers. There is no audio, DMA, HAL, clock-source
selection, or recovery choreography simulated.
Missing packets stop both models; synthesis continuation after loss is not claimed
to reproduce vendor behavior. Startup begins from the first observed audio block;
lock flags are tracked internally but output muting is not simulated.

Regression checks include 24 captured official-driver SPH quadlets, host seconds
excluded from wire stamps, second wrap, fractional 44.1 kHz periods, actual TX
feedback, elapsed-interval gain, late updates, acquisition/lock boundaries,
strict hard-resync threshold, absolute-seed sign and branch-cut folding,
reference regressions, and clean-clock/loss simulations at all six rates.
Captured quadlet round trips verify representation; they do not prove that our
controller produces the vendor's trajectory.

Behavioral sources (read-only; independently implemented):

- `references/linux-sound-firewire-stack/firewire/motu/amdtp-motu.c:303–329,373–393`
- `tmp/motu-research/linux-motu-research.md`, A1
- `tmp/motu-research/vendor-motu-gap-research.md`, §§1.2–1.4
- [PR #172 controller](https://github.com/cube666999/ASFireWire/blob/858f07582c9dac937dab492036097a40f104cead/ASFWDriver/Audio/Protocols/MOTU/MotuSphClockServo.hpp)
- [PR #172 servo tests](https://github.com/cube666999/ASFireWire/blob/858f07582c9dac937dab492036097a40f104cead/tests/audio/MotuSphClockServoTests.cpp)
- [PR #172 captured SPH tests](https://github.com/cube666999/ASFireWire/blob/858f07582c9dac937dab492036097a40f104cead/tests/audio/MotuV3SphWireParityTests.cpp)

A 1-second run at 48 kHz and +100 ppm produces replay RMS error about 0.012 µs
and synthesis about 0.093 µs. With 8-tick SPH noise, replay RMS is about 0.326 µs
and synthesis about 0.398 µs (the previous per-block model was about 2.379 µs).
The clean run locks at frame 512 without resyncs. Forty-two cycles have different
packet frame counts, as small timing differences cross packet boundaries; total
frame counts agree. These are simulator results, not hardware measurements.
