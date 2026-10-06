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
