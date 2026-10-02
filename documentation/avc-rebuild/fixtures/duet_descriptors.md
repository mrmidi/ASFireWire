# Apogee Duet descriptors: decoded (expected graph for tests)

Raw exchanges: `duet_descriptors.json`, in the same format as `phase88_descriptors.json`. Captured 2026-09-27 over
MCP (node 0, generation 1), with the same frames as the Phase 88 (see `phase88_descriptors.md`).

## What was read

| Subunit | Specifier | Bytes | Result |
|---|---|---|---|
| music `60` | `80` status | 464 | chunks of **38** data bytes (Phase 88: 142); status `11` → `10` |
| audio `08` | `80` status | — | OPEN answered **REJECTED** `0A` (Phase 88: NOT IMPLEMENTED `08`) |
| audio `08` | `00` identifier | 56 | 1 configuration, 1 function block, **0 root lists** (no text database) |

## Music subunit status descriptor (60 / 80)

- **General Music Subunit Status:** primary `01 01 ff ff ff ff`.
- **Routing Status:** `03 03 00 05` = 3 destination plugs, 3 source plugs, 5 music plugs.
- **Destination plugs:**
  - 0 = "Analog Out": MBLA, line, 2 signals at stream positions **0, 1** (plain order, host playback);
  - 1 = "Analog In";
  - 2 = "Sync": format `40`.
- **Source plugs:**
  - 0 = "Analog In": MBLA, line, positions **0, 1** (plain order, host capture);
  - 1 = "Analog Out";
  - 2 = "Sync".
- **Music plugs:** "Analog Out 1", "Analog Out 2", "Analog In 1", "Analog In 2", and a sync plug (type `80`).
- **Where names live:** they are **Raw Text `000A` directly under the block**. The Phase 88 wraps them in Name Info
  `000B`, so a parser must accept both.
- **Channel order is plain.** This matches Linux's Oxford driver, which assumes it and reads no descriptor.

## Audio subunit identifier descriptor (08 / 00)

- **Header:** generation 0, `size_of_list_ID 2`, `size_of_object_ID 0`, `size_of_object_position 2`, 0 root lists.
- **Configuration `0x0000`:** master cluster `02 02 c0 00` (2 channels). One subunit source-plug link, reading
  `81 00`: a feature block with id 0, although the only block is id 1. Recorded as observed.
- **Feature 1:**
  - name `0xFFFF` (none);
  - input `F0 00` (audio subunit destination plug 0);
  - cluster `02 02 c0 00`;
  - type-dependent `08 02 00 00 03 00 02 00 02`.
- **Controls:** that last field **doesn't parse per Table 8.3 the way the Phase 88's does** (`00 15 | 00 02 | 00 |
  c0 00…`). The coherent reading uses 1-byte length and size fields and bit 0 = LSB:
  - master `0003` = Mute + Volume;
  - channels 1, 2 `0002` = Volume.

  This is unconfirmed. The master part matches the phase-1 probe (FB1 channel-0 mute and volume answer STABLE;
  mute reads `70` = muted; range −64..0 dB).
- **Measured 2026-09-28** (node 0, gen 3; `duet_fb1_status.json`, tool `duet_fb_status.py`; STATUS + INQUIRY only):
  - channels 0, 1, 2: **mute and volume both answer STABLE** (mute `70`, volume `0000` = 0 dB), and INQUIRY
    answers IMPLEMENTED for mute on, mute off and volume 0 dB on all three;
  - channels 1, 2: volume min `C000` (−64 dB), max `0000`, resolution `0001` (1/256 dB);
  - LR/FR balance, bass, mid, treble, AGC, delay, bass boost, loudness: NOT IMPLEMENTED on channels 0–2;
  - channel 3: NOT IMPLEMENTED for everything (the cluster has 2 channels).
- **So the Duet's control bitmap is wrong or unreadable, whichever bit order is used:**
  - Table 8.3 (`tmp/specs/avc-audio-subunit-1.0.txt:2304-2340`) has 2-byte `length` and `size_of_controls`.
    The Phase 88 follows it; the Duet uses 1-byte fields.
  - With bit 0 = LSB, the Duet claims volume only on channels 1, 2, but they also have mute.
  - With bit 0 = MSB (the reading that fits the Phase 88's `C000`), the Duet's `0003`/`0002` are reserved bits.
  - **Rule: never trust the descriptor for which controls exist. Confirm each with STATUS (or INQUIRY).**
- **User's reading (2026-09-28): the low-bit reading is right.** Master = mute + volume, channels = volume; the
  channels answering mute is extra, not a contradiction. This is the same shape Linux uses for the generic OXFW
  output controls: volume on all channels, mute on the master (`alsa-userspace…/protocols/oxfw/src/lib.rs:118-200`,
  Griffin FB 2/1, LaCie FB 1). The consequence: the Phase 88's `C000` then needs the opposite bit order, so one
  parser can't read both. The STATUS rule covers both devices.
- **Linux doesn't use FB1 for the Duet.** Its Duet controls are Apogee vendor-dependent commands at the unit
  address (`protocols/oxfw/src/apogee.rs:880-890`: `OUT_MUTE 0x09`, `OUT_VOLUME 0x15`, line/HP mute 0x16–0x19).

## Lessons for the parser and graph builder

- **Replies vary by vendor:** chunk size (38 vs 142 bytes), the answer to an unsupported status descriptor
  (REJECTED vs NOT IMPLEMENTED), name wrapping (Raw Text vs Name Info), and the feature control bitmap layout.
- **Treat descriptor contents as hints.** Confirm each control with a STATUS query before publishing it.
- **The Duet's feature block has no name and no text database.** Its meaning (input or output path, and why it
  reads muted while audio plays) comes from the edges plus the catalog, not from the device.
