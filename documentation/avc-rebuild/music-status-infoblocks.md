# Music subunit status descriptor: info blocks 8100-8107

Source of truth for what each block carries, cross-checked against a second implementation. Written
2026-10-03 for audit finding F7 (`magic-numbers-audit.md`).

- **Spec:** TA 2001007 AV/C Music Subunit 1.0 (April 2002) §6.2.1-§6.2.3.5, Figures 6.2-6.15, Tables 6.2-6.8.
- **Second implementation:** Apple's AVCVideoServices-42 (`/Volumes/SDExt/DEV/FirWireDriver/AVCVideoServices-42/`),
  `MusicSubunitController.h:66-72` (type enum) and `MusicSubunitController.cpp:2376-2460` (field decode). It is a
  descriptor dumper, so it shows what a shipping Apple tool takes each block to contain. Behaviour only; not copied.

Every block: `[compound_length:2][info_block_type:2][primary_fields_length:2][primary fields][nested blocks]`
(compound_length excludes its own 2 bytes).

## Layout, and whether the spec and Apple agree

| Type | Name (§) | Primary fields | Nested | Apple decodes |
|---|---|---|---|---|
| 8100 | General Music Subunit Status Area (§6.2.1) | `current_transmit_capability:1` (Table 5.5), `current_receive_capability:1` (Table 5.6), `current_latency_capability:4` (= FFFFFFFF) | none | bytes 6, 7, 8-11: same |
| 8101 | Music Output Plug Status Area (§6.2.2) | `number_of_source_plugs:1`, "currently configured by subunit" | `8102` x n | byte 6 = `number_of_source_plugs`: same |
| 8102 | Source Plug Status (§6.2.3) | `source_plug_number:1` | `8103`, `8104`, `8105`, `8106`, `8107` (each optional) | byte 6 = source plug number: same |
| 8103 | Audio Info (§6.2.3.1) | `number_of_audio_streams:1` | `name_info_block` (labels, CR LF separated; empty label = back-to-back CR LF), optional blocks | byte 6: same |
| 8104 | MIDI Info (§6.2.3.2) | `number_of_MIDI_streams:1` | **one `name_info_block` per MIDI stream**, optional blocks | byte 6: same |
| 8105 | SMPTE Time Code Info (§6.2.3.3) | `SMPTE_time_code_activity:1`: bit 0 Rx, bit 1 Tx, rest reserved (Table 6.6) | none | byte 6: same |
| 8106 | Sample Count Info (§6.2.3.4) | `sample_count_activity:1`: bit 0 Rx, bit 1 Tx (Table 6.7) | none | byte 6: same |
| 8107 | Audio SYNC Info (§6.2.3.5) | `audio_SYNC_activity:1`: bit 0 Bus (sync from the 1394 bus), bit 1 Ex (sync from the sync source) (Table 6.8) | none | byte 6: same |

The spec and Apple agree on every field of 8100-8107. The local spec copy is not out of date for these blocks.
`8108`-`810B` (routing status, subunit plug, cluster, music plug) are in neither this spec nor in §6.2; they come
from Apple's headers and the captures, and are documented in `applefwaudio-graph-rules.md`.

Nothing in the spec or in Apple's tool reads `8101`-`8105` at the top level of a status descriptor as a
*capability* block (the identifier-descriptor layouts of §5.2). That read exists only in our parser (audit F7).

## What the other reference stacks do (checked 2026-10-03)

- **Linux** (`references/linux-sound-firewire-stack`, the `snd-firewire-ctl-services` crates): never reads a music
  subunit descriptor. No READ DESCRIPTOR or OPEN DESCRIPTOR anywhere; bebob uses plug info and stream formats
  only. It has nothing on `8100`-`8107`.
- **FFADO** (`avc_descriptor_music.cpp:734-748`): the status-descriptor loop handles `8100`, `8101` and `8108`
  only. `8101` is `AVCMusicOutputPlugStatusInfoBlock`, whose `deserialize` warns "not supported, skipping" and skips
  it (`:84-103`), so FFADO never reads the nested `8102`-`8107` either. Every other type is skipped as unknown.
- **Apple AVCVideoServices**: decodes all of `8100`-`8107` as above, as a dumper.

So outside our own parser, only Apple's dumper decodes `8102`-`8107`. Apple's driver does not use the per-plug
label lists under `8101`/`8103` for channel names; it uses cluster and music-plug names
(`applefwaudio-graph-rules.md`, "The per-plug name lists under `8101`/`8103` ... are NOT used"). That makes
`8101`-`8107` the least-exercised part of the descriptor in every stack we have.

## What our parser does with each (`MusicSubunitDescriptor.cpp`)

| Block | Read? |
|---|---|
| 8100 | Yes: tx / rx capability and latency. |
| 8101 | Nested walk: yes. `number_of_source_plugs` is **not stored** (the 8108 count is used instead). Also read as an identifier-style audio capability (F7). |
| 8102 | `source_plug_number` used as the key for labels. Also read as a MIDI capability at top level (F7). |
| 8103 | Yes: audio stream labels, per source plug. |
| 8104 | **No.** The MIDI labels are not read. |
| 8105 / 8106 / 8107 | **No** (nested). Read only as top-level identifier-style flags (F7). |

## Evidence from the captures

- Phase 88: `8100` `02 03 FF FF FF FF`; `8101` primary `04` (4 source plugs) containing `8102` containing `8103`
  (primary `0A`: 10 audio streams) and `8104` (primary `02`: 2 MIDI streams). The nested `8104` carries two name
  blocks, **"MidiPort_1" and "MidiPort_2"** (`fixtures/phase88_descriptors.json`). The device sends spec-defined
  MIDI labels that nothing reads.
- Duet: `8100` and `8108` only; no `8101`.
- `8101` says 4 source plugs, the unit's SUBUNIT INFO says 6. **Checked live on a Phase 88 (2026-10-03,
  `asfw_avc_get_discovery_document`):** the 4 is the number of nested `8102` blocks, and they are sparse: source plugs
  0 (10 audio + 2 MIDI streams), 1 (8 audio), 2 (2 audio) and 5 (`8107` audio SYNC, activity `03`). Plugs 3 and 4 have
  no status block. So `number_of_source_plugs` counts the plugs the status area describes, not the subunit's
  plug total, and plug numbers are not 0..n-1. One device; do not assume it for others.

## Open (not applied)

1. Read the MIDI labels from nested `8104` (`name_info_block[0..n-1]`), the same way as the `8103` labels.
2. Read the `8105`-`8107` activity bits from their spec position (nested in `8102`).
3. Store `8101` `number_of_source_plugs`.
4. F7 itself: delete or guard the top-level capability reads. Apple's tool shows no use of them.
