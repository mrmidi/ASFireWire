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
- **B4: Prism Orpheus: SKIPPED (user decision, 2026-10-02).** No catalog row, no vendor hook until a user
  reports that it is needed. Reasons:
  - The fork's output-source write (vendor-dependent `0xB1`=1 plus a `0xBF` whole-state read-modify-write,
    Prism OUI `00 11 98`, audio subunit `0x08`) landed together with its stream-format CONTROL change, so which
    one made audio play is unproven.
  - Linux (`bebob.c:423`, `spec_normal`), FFADO and Apple's driver send no vendor command; the source is most
    likely a stored device setting.
  - Forcing it every start would override the user's front-panel choice and needs CONTROL writes to a device we
    cannot test.

  If a report arrives: first the generic row; then the source as a profile control (read-only STATUS at attach);
  a guarded "write only if not FireWire" step only on evidence of silence, never the `0xBF` bulk write-back.
  Facts and fork location: memory `prism-orpheus-port-thread`, clone `tmp/orpheus-fork`.
