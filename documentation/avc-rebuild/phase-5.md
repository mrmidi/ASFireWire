# Phase 5: BeBoB on the new layer (outline)

Source: `00-overview.md` stages B1, B2, B4.

- **B1: generic BeBoB.**
  - Geometry and rates from discovery, per direction and per rate. Rate codes via `StreamFormatRate`; the old
    generic tables used CIP SFC and are wrong.
  - Current rate from signal-format STATUS: the Phase 88 REJECTS the "current format" query; Linux
    `bebob_stream.c:66-81` uses signal format.
  - The model is delivered to the protocol after discovery: today the protocol is created first (ring seq 7605 vs
    7611-7653) and gets an empty `DeviceModel{}`.
  - A generic publisher (today's publisher hardcodes Phase 88: 10+1 @ 48k); `emptyPacketsDuringIdle = true`
    (Phase 88 hardware bring-up, `946be2c1`).
- **B2: Phase 88 onto the generic protocol.**
  - Its mixer unmute becomes a hook; delete `Phase88Protocol` and `Phase88Profile`.
  - Rates: the device reports 32-96 kHz. Decide after a hardware rate-switch test on the user's Phase 88.
- **B4: Prism Orpheus** (`0x001198` / `0x010048`; Linux `bebob.c:423`, `spec_normal`).
  - Generic protocol plus an output-source hook (vendor-dependent, Prism OUI `00 11 98`, cmd `0xB1` value `0x01`),
    following the start sequence in `grandviewsound/ASFireWire-Orpheus` (clone: `tmp/orpheus-fork`); behaviour
    only, no code.
  - Open question for the user: force the output source on every start, or not.
  - Marked not run on this code; ask the fork's author for a capture-tool run.
