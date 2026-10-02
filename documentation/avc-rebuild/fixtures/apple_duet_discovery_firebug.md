# AppleFWAudio discovering the Apogee Duet (FireBug capture)

Source: `tools/pydice/isitduet.txt` (added by the user), an Apple FireBug 2.3 log. The device is GUID
`0003DB0A 0000D112` (ROM text "Apogee Electronics" / "Duet"), the same unit as `duet.json`. The host at `ffc0`
is a Mac running Apple's driver. Per the user, the Duet worked fully on AppleFWAudio with no vendor driver.
Parsed with `pydice.protocol.log_parser.parse_log` into `apple_duet_discovery_firebug.json`: 35 commands and
49 responses in order, frames trimmed to their real size, 19 responses marked `duplicate`.

## Apple's order

1. **Unit status descriptor probe:** OPEN `00 FF 08 80 01 FF` → NOT IMPLEMENTED.
2. **Current rate:** OUTPUT PLUG SIGNAL FORMAT `01 FF 18 00 FF FF FF FF` → `90 01` (AM824, SFC 1 = 44.1 kHz).
3. **UNIT INFO `01 FF 30` with NO operands** (a quadlet write `01ff3000`) → `0c ff 30 07 08 00 03 db`.
4. **SUBUNIT INFO** `01 FF 31 07 FF FF FF FF` → `08 60`: an audio subunit AND a music subunit.
5. **Music subunit status descriptor:** OPEN `00 60 08 80 01 FF`, then READ `00 60 09 80 FF 00 <len:2> <off:2>
   00 00`, then CLOSE.
   - The READ is padded to 12 bytes. It carries `FF` (read_result_status) and `00` (reserved); our tool sent
     `FF FF`, which the Duet also accepts.
   - The requested length is `min(0x80, 0x1CE − offset)`, i.e. counted against the length field's value. The
     device returns 0x26 (38) bytes per chunk regardless.
6. **Stream formats on the MUSIC subunit, with opcode `0xBF`:**
   - single `01 60 BF C0 00 01 00 FF FF 00 00 00` → 44.1 kHz, compound 2 × MBLA;
   - list `C1` idx 0..3 → 44.1 / 48 / 88.2 / 96, idx 4 REJECTED;
   - the same queries with plug byte `81` → NOT IMPLEMENTED.
7. **SIGNAL SOURCE**, in Apple's 16-byte frame form (`1a ff ff ff 00 00 ff ff ff ff ff 00 00 00`):
   - to the music subunit → NOT IMPLEMENTED (3-byte reply);
   - to the unit → STABLE; with `…00 81…` → REJECTED.
8. **PLUG INFO unit** → `01 01 01 01`.
9. **Then streaming:**
   - IRM: 84 bandwidth units, channel 0;
   - CMP oPCR[0] 80008036 → 81008036 (p2p 1, channel 0, s400);
   - isoch from the Duet: DBS 2, SFC 1, 72-byte DATA packets (8 frames × 2 ch), MBLA label `0x40`.

## Findings

- **Apple never touches the audio subunit on the Duet**: no descriptor, no function-block query, even though
  SUBUNIT INFO lists it. Everything came from the **music subunit**: the descriptor (channels, names,
  positions), `0xBF` formats per music plug, and the rate from the unit signal format.
- **UNIT INFO:** Apple sends no operands, the same as legacy `AVCUnit.cpp:76-81`. The phase-1 codec sends Linux's
  `07 FF FF FF FF`, listed as an "intended difference". Apple + legacy + hardware-proven now argue for the
  zero-operand form, and phase 1's own rule says hardware-proven ASFW bytes win. **Reopen this in phase 2.**
  Both forms were answered STABLE by the Duet and the Phase 88.
- **Duplicate responses:** the Duet re-sent each FCP response about 85 ms later while the Mac didn't complete
  the write transaction (the device got `ack_pending` and no WrResp; WrResp only appears from `068:7159`).
  Apple's READ loop then advanced on stale duplicates and read one chunk past the end (offset `0x1EE`,
  read_result_status `12`). Our transport must answer each FCP write and must match responses defensively.
  The "loop drifted" part is my inference from the timestamps.
- **No rate CONTROL was sent;** the Duet was already at 44.1 kHz.
