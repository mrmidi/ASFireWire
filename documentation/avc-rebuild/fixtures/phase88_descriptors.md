# Phase 88 descriptors: decoded (expected graph for tests)

Raw exchanges: `phase88_descriptors.json`, in the same record format as `documentation/fixtures/AVC/phase88.json`.
Captured 2026-09-27 over MCP (node 1, generation 1) **during active playback. Audio stayed clean** (user).
Tools: `desc_read.py` (OPEN / chunked READ / always CLOSE), `infoblocks.py` (info-block tree).

## Commands (all answered ACCEPTED `09`)

| Step | Frame | Notes |
|---|---|---|
| OPEN for read | `00 <sub> 08 <spec> 01 FF` | subfunction `01` = read open |
| READ | `00 <sub> 09 <spec> FF FF <len:2> <off:2>` | first read: len 0, off 0; then ask for the **remaining** length (Apple) |
| CLOSE | `00 <sub> 08 <spec> 00 FF` | always sent, even after a failed read |

- **READ reply:** `[09 sub 09 <spec>] [status] [FF] [chunk_len:2] [offset:2] [data…]`, with data at `3 + len(spec) + 6`.
  Status `11` = more to read, `10` = complete. The device sends at most `0x8E` (142) data bytes per chunk.
- **Specifiers:**
  - `80` = subunit status descriptor;
  - `00` = subunit identifier descriptor;
  - `10 <listID:2>` = list by ID (TA 2002013 §6.2.2 Figure 25).
- **Frame sources:** AppleFWAudio's discovery, as replayed in the Orpheus fork (`AppleDiscoverySequence.cpp`).

## What was read

| Subunit | Specifier | Bytes | Result |
|---|---|---|---|
| music `60` | `80` status | 2410 | info blocks (TA 1999045 + Music Subunit 1.0) |
| audio `08` | `80` status | — | OPEN answered **NOT IMPLEMENTED** `08` |
| audio `08` | `00` identifier | 658 | 1 configuration, 22 function blocks with edges |
| audio `08` | `10 18 00` | 35 | root list, type `86` = Text Database List (Audio Subunit 1.0 line 2082); 1 entry → child list `0x1801` |
| audio `08` | `10 18 01` | 3895 | 80 text entries, indexed by `function_block_name` / channel name IDs |

## Music subunit status descriptor (60 / 80)

- **Routing Status `8108`** primary `0a 04 00 19`: 10 subunit destination plugs, 4 source plugs, 25 music plugs.
- **Subunit dest plug 0** (host → device playback, 11 channels, 3 clusters). Stream positions are 0-based, per Cluster Info `810A`:
  - "PHASE88 FW Multichannel Out": MBLA (`06`), line (`03`), 8 signals at positions **1, 6, 2, 7, 3, 8, 4, 9**;
  - "SPDIF/AC3 Out": IEC 60958 (`00`), S/PDIF (`04`), positions **0, 5**;
  - "MidiSection.0": MIDI (`0d`), port `0a`, position **10**.

  This is identical to BridgeCo `C0` channel positions (1-based there).
- **Subunit source plug 0** (device → host capture):
  - "Line In 1/2" at 1, 6; "3/4" at 2, 7; "5/6" at 3, 8; "7/8" at 4, 9;
  - "SPDIF In" at 0, 5;
  - MIDI at 10.
- **Per-channel names** (Name Info `000B` under Audio Info `8103`):
  - capture: "Line_1/2 left PHASE88 FW" … "Line_7/8 right …", "SPDIF left/right PHASE88 FW";
  - playback: "Multichannel 1…8 PHASE88 FW", "SPDIF/AC3 left/right PHASE88 FW".
- **Sync:** plugs 8, 9 (dest) and 5 (source) are format `40` (sync stream), named "Synch".
- **Music Plug Info `810B` × 25:** per-channel source → destination. The layout is **confirmed by Apple's
  encoder** (AVCVideoServices-42 `VirtualMusicSubunit.cpp:2269-2297`):
  `type, musicPlugID:2, routingSupport, F0, sourcePlugID, FF, srcStreamPosition, srcStreamLocation, F1,
  destPlugID, FF, destStreamPosition, destStreamLocation`. Example: music plug 0 goes from dest plug 0
  position 1 to source plug 1 position 0, i.e. FireWire slot 1 → analog Out 1. Cluster Info signal entries are
  `musicPlugID:2, streamPosition, streamLocation` (`:2303-2333`).

## Audio subunit identifier descriptor (08 / 00): function-block graph

- **Header:** `generation_ID 2`, `size_of_list_ID 2`, `size_of_object_ID 0`, `size_of_object_position 2`, 1 root list
  `0x1800`. One configuration `0x0001`: master cluster 2 channels.
- **Function block layout:** spec Table 8.1 (`tmp/specs/avc-audio-subunit-1.0.txt:2185-2245`).
- **`source_ID`:** `F0 n` = subunit destination plug n; `FE` = not connected.

| Block | Name (text DB) | Inputs |
|---|---|---|
| Feature 1 | **Mixer Output Level** | Processing 1 |
| Feature 2 | Mixer Input LineIn 1/2 Level | dest plug 2 |
| Feature 3 | Mixer Input LineIn 3/4 Level | dest plug 3 |
| Feature 4 | Mixer Input LineIn 5/6 Level | dest plug 4 |
| Feature 5 | Mixer Input LineIn 7/8 Level | dest plug 5 |
| Feature 6 | Mixer Input S/PDIF In Level | dest plug 6 |
| Feature 7 | Mixer Input Waveplay Level | Selector 7 |
| Selector 1–4 | LineOut 1/2, 3/4, 5/6, 7/8 Selector | Processing 2–5, or Selector 6 |
| Selector 5 | S/PDIF Out Selector | dest plug 1, or Selector 6 |
| Selector 6 | Mixer Output Selector FB | Feature 1 (×6) |
| Selector 7 | Waveplay Selector | dest plug 1, Processing 2–5 |
| Selector 8 | external Clocksource Selector | dest plugs 6, 7 |
| Selector 9 | Clock Selector | not connected, Selector 8 |
| Selector 10 | Input 7/8 Selector | dest plug 5, not connected |
| Processing 1 | Main Mixer | Feature 7, Features 2–6 |
| Processing 2–5 | Channel 1/2, 3/4, 5/6, 7/8 | dest plug 0 |

- **Feature 1** has an 8-channel cluster. The type-dependent bytes list `c0` per channel. My reading is
  mute + volume present (bits 7, 6); check against spec Table 8.3.
- **Subunit source plug links (11):** `F0 02`, `F0 03`, `F0 04`, `80 0a`, `F0 06`, `80 09`, `80 01`…`80 05`. Each is
  the block that feeds each audio subunit source plug.

## Cross-checks with earlier measurements

- The channel map matches BridgeCo `C0` (`phase88.json`) and the fix in PR #160, which was verified by ear.
- **Feature 1 = Master:** confirmed by the device's own name and wiring, by FFADO `phase88control.py:43-44`, and by
  the volume changes heard on 2026-09-27.
- **Feature block 0 doesn't exist:** there's none in the descriptor, and STATUS answers NOT IMPLEMENTED.
- **Selector 6 = 1 ("Line Out 1/2")** matches FFADO's "Out Assign". The descriptor shows it selects among 6 inputs,
  all from Feature 1.
