# How AppleFWAudio builds the device graph (behaviour, from the shipped kext)

Source: IDA database `tmp/re/AppleFWAudio.x86_64.i64` (opened without auto-analysis), 2026-09-28. These are
behaviour notes for a clean-room implementation. No code is copied. Function names are the kext's own symbols.

## Entry point

`AppleFWAudioDevice::InitializeDeviceInfo` (2,184 decompiled lines):
- reads the subunit descriptor into an `AVCInfoBlock` tree;
- runs visitors over one plug block with `AVCInfoBlock::GetFromPlug(isInput, plugIndex, visitor, ctx)`:
  `GetChannelsPerStream`, `GetAudioStreamCount`, `GetMIDIStreamCount`, `GetChannelNamesFromPlug`;
- calls `GetChannelMapping(isInput, plugIndex, out[])` for the slot order;
- its log strings include "device has not stream format support. Bailing out of discovery." and a sanity check
  that rejects two different counts reported for the same subunit type.

## Block types used

| Constant | Block |
|---|---|
| `0x8100` | General Music Subunit Status (root child) |
| `0x8108` | Routing Status |
| `0x8109` | Subunit Plug Info |
| `0x810A` | Cluster Info |
| `0x0C0C0100` | synthetic root Apple wraps around a music subunit descriptor |
| `0x0C0C0003` | synthetic wrapper for the audio-subunit form (count = primary byte 0; stream count 1) |

A third synthetic value, `0x0C0C0010`, also appears in `GetChannelMapping`.

## Rules

1. **Finding a plug** (`GetFromPlug`, `GetChannelMapping(bool, u8, u8*)`):
   - Go root → `8108` Routing Status → the n-th `8109` child.
   - Destination plugs come first, then source plugs. The number of destination plugs is Routing Status
     primary byte 0.
   - `GetFromPlug(isInput=1, i)` selects destination plug i; `isInput=0` selects source plug `i + numDest`.
   - **`GetChannelMapping` uses the opposite convention:** `1` selects source plug `i + numDest`, `0` selects
     destination plug i. Record both; don't "fix" one to match the other.
2. **Slot map** (`GetChannelMapping(u8*)`, per `8109`):
   - Clusters = Subunit Plug Info primary bytes 4–5 (big-endian).
   - For each `810A` child in order, for each signal i, append the byte at cluster primary `5 + 4·i` (the
     signal's **stream position**).
   - So `map[k]` = the AM824 slot of channel k, concatenated in cluster order. MIDI is not filtered here.
3. **Channels per stream** (`GetChannelCounts`): one entry per cluster = Cluster Info primary byte 2 (signal
   count). **Apple's CoreAudio stream unit is the cluster.**
4. **Stream count** (`GetClusterStreams`): the cluster count, taken from Subunit Plug Info bytes 4–5, MIDI
   clusters included.
5. **MIDI** (`GetMIDIStream`): sum of the signal counts of clusters whose **port type** (Cluster Info primary byte
   1) is `0x0A`.

## Checked against our fixtures (`fixtures/graph_build.py`)

- **Phase 88, capture plug (music source 0):** map 1, 6, 2, 7, 3, 8, 4, 9, 0, 5, 10, 10. The same as BridgeCo
  `C0` and PR #160 (verified by ear). Clusters: Line In 1/2, 3/4, 5/6, 7/8, SPDIF In, MIDI, i.e. 5 audio
  clusters × 2 + MIDI.
- **Duet:** one 2-channel cluster per direction, map 0, 1.

## Clock sources (`AppleFWAudioDevice::DiscoverSyncSources`)

- **Precondition:** a music subunit and a stored **sync destination plug** on it. With `0xFF` (none), no sources
  are offered.
- **Candidates are asked, not read.** For each candidate Apple calls an `AM824AVC` method (vtable +600; its
  arguments match `QuerySyncPlugReconnect`: sync subunit, sync dest plug, source subunit, source plug) and keeps
  the accepted ones:
  - every **unit iso input plug** and every **external input plug** (`0x80 + n`). The iso plug the Mac transmits
    into is named **"Mac"** (and a "could sync to Mac" flag is set). Others are named from the descriptor
    (`GetFromPlug` + `GetPlugName`), else from the format ("SPDIF" for < 6, "MBLA" for 6, "Sync Stream" for
    `0x40`), else "Ext Plug #n" / "Unit Isoch #n (unsupported)";
  - every **subunit source plug whose format is `0x40`** (sync stream). The music subunit's own is
    **"Internal"**; another subunit's is "Subunit #n".
- **Devices that can't answer the question** get two fixed entries, "Device" and "Mac" (a flag suppresses
  "Mac").
- Our devices: the Duet is currently fed by its own sync source (Internal). On the Phase 88 the music sync
  plugs are unconnected and the clock is in audio-subunit Selectors 8/9. Apple's rule would only find sources
  that the device accepts into the music sync plug. Asking the Phase 88 that exact question (SPECIFIC INQUIRY)
  is a read-only test we haven't run yet.

## Channel names (`GetChannelNamesFromPlug` → `GetChannelNamesFromClusters`)

- **Cluster name:** Raw Text inside Name Info (`000B` → `000A`), or Raw Text directly under the cluster. Both
  are accepted.
- **Channel name:** its **music plug's** name (`GetChannelNameFromPlugId(musicPlugID)`), else the **cluster
  name**. With no cluster name, that cluster's channels get none. Apple appends three spaces to each name
  (cosmetic; don't copy).
- **The per-plug name lists under `8101`/`8103` (Phase 88: "Line_1/2 left PHASE88 FW") are NOT used.** Apple
  would show the Phase 88's inputs as "Line In 1/2" twice. The Duet shows "Analog Out 1/2" (music-plug names).

## Fallback (`InitializeDeviceInfo`, `DefaultDeviceInfo`, `DefaultChannelMap`)

- **It always starts from the defaults.** `DefaultDeviceInfo` (vtable +0xF30, called first at `0x3a5f`) sets
  music subunit = none, sync dest plug = none, "could sync to Mac" = false, then calls `DefaultChannelMap`
  (+0xF38): identity slot maps (256 entries per direction) and no names.
- **Data-block size from the current stream format** of the plug the Mac uses: format code `≤ 6` = PCM,
  `0x0D` = MIDI, slots = PCM + ⌈MIDI / 8⌉. **Apple's PCM rule (0x00–0x06) is wider than Linux's (0x00, 0x06).**
- **The descriptor map is used only if** the plug's subunit is a **music subunit** (type `0x0C`) with a parsed
  descriptor.
- **Identity again if** `GetChannelMapping` fails **or any mapped slot ≥ the data-block size** (`0x5924`–`0x595c`,
  and the same for the other direction at `0x5d4e`/`0x5d77`).
- Then names (`GetChannelNamesFromPlug`) and the MIDI stream count (`GetMIDIStreamCount`) from the descriptor.

## Consequences for our builder

1. **Validate before use:** reject a descriptor map with any slot ≥ the stream-format data-block size, then fall
   back (Apple: identity; for BeBoB we have `C0` first).
2. **Names:** Apple's order is music plug, then cluster. The per-plug name lists are an extra we may use, but
   they go beyond Apple; decide deliberately.
3. **Clock:** ask (SPECIFIC INQUIRY) which sources the music sync plug accepts. Don't infer the options from the
   current route. BeBoB devices like the Phase 88 also need the audio-subunit clock selectors.
4. **Streams = clusters** if we want Apple's CoreAudio shape.

## Measured: Apple's clock question on the Phase 88 (2026-09-28, gen 4)

- SPECIFIC INQUIRY `02 FF 1A FF <src> 60 <8|9>` for every Apple candidate (unit iso in 0–1, ext in 0x80–0x87,
  music src 5) plus audio src 5: **all NOT IMPLEMENTED** on both sync plugs.
- **Controls:** the inquiry answers `0C` for routes that already exist (music dest 0 ← iso in 0, music dest 1 ←
  audio src 0) and NOT IMPLEMENTED for a non-current source (music dest 1 ← ext in 0x80). **The Phase 88's
  SIGNAL SOURCE routing is fixed.** It only confirms current routes.
- **So Apple's music-sync-plug method finds no clock sources on the Phase 88.**
  - **CAVEAT (2026-10-03): this was measured with the wrong first byte.** Our INQUIRY frames carry `operand[0] = FF`.
    Apple's `QuerySyncPlugReconnect` (`0x10f18`) sends `02 FF 1A 0F <src> <dest>`, and the spec, FFADO and the Prism
    fork use `0F` too (see "SIGNAL SOURCE first byte" below). The Phase 88 answered `0C` for existing routes even with
    `FF`, so the device probably ignores the upper nibble, but that is untested. **Re-run the sync-plug inquiries with
    `0F` before relying on "Apple would offer the Phase 88 no clock sources".**
- **The capability check (vtable +2776 = +0xAD8) is `GetExtendedStreamSupport()`,** which returns a flag set by
  `CheckForExtendedStreamFormatListSupport` (`0xf37e`). That function asks for stream-format **list entry 0 on
  unit plug 0**, input and output (an `AM824AVC` method at +736; its arguments match
  `GetExtendedStreamFormatListElement`). **Either success sets the flag** and a second "use the full method"
  flag.
  - The Phase 88 answers the `0x2F` list (and NOT IMPLEMENTED to `0xBF`; Apple falls back from `0xBF` to
    `0x2F`), so the flag is set and Apple takes the inquiry path. **Apple would have offered the Phase 88 NO
    clock sources.**
  - The fixed "Device" + "Mac" pair is only for devices that fail even the format-list query.
- **The real clock switch is in the audio subunit and is named in the descriptor:**
  - Selector 9 "Clock Selector": inputs = not connected, Selector 8;
  - Selector 8 "external Clocksource Selector": inputs = dest plugs 6, 7.

  This matches Linux `phase88_rack_clk_src_get` (FB9 internal/external, FB8 which external). Records are in
  `fixtures/phase88_descriptors.json` (`inquiry_*`).

## Apple's host-plug and sync-plug selection (IDA, 2026-10-03; open item 4)

Source: `InitializeDeviceInfo` (`0x3a34`, size `0x2667`) in `tmp/re/AppleFWAudio.x86_64.i64`, decompiled; stores found
with `search_text` over the listing. Addresses below are inside that function unless noted. Field names are mine; Apple
has none in the binary.

**Data model it builds** (all arrays use a 4268-byte stride per plug):
- Unit PLUG INFO gives iso-in (`+284`), iso-out (`+280`), ext-in (`+288`), ext-out (`+292`) counts.
- `+296` = unit OUTPUT plugs (iso-out + ext-out). `+304` = unit INPUT plugs (iso-in + ext-in).
- `+320` = subunit table, 48-byte stride: `+16` = subunit source (output) plugs, `+24` = subunit dest (input) plugs.
- Per plug: `+0` valid flag, `+1` connected subunit address byte (`0xFF` = a unit plug), `+2` subunit index, `+4` plug
  number on that subunit, `+20` = **slot count** (sum of the per-field channel counts of the stream format), `+87..` =
  one format code per slot (`<=6` PCM, `13` MIDI).
- The connection fields come from `GetPlugRouting` (`0x2e8e`, vtable `+3560`), which calls a method on the AV/C unit
  object (`+784`, outside this binary) and fills source subunit and plug. That is the SIGNAL SOURCE query. Subunit
  index comes from `GetSubUnitIndex` (`0x2e50`, vtable `+3552`).

**`+0x18C` (396): the capture plug, a unit iso OUTPUT plug index** (stores at `0x4fc3` clear, `0x503d` set).
- Candidates: unit iso-out plugs that are valid, are fed by a subunit (`+1 != 0xFF`), and whose subunit type bits are
  music (`(addr & 0xF8) == 0x60`).
- Winner: the one whose music-subunit source plug has the **most slots**. Strict `<`, so ties go to the lowest index.

**`+0x18D` (397): the playback plug, a unit iso INPUT plug index** (stores at `0x4fcb` clear, `0x50e2` set).
- Candidates: unit iso-in plugs that are valid and consumed by a subunit (`+1 != 0xFF`). **No music-type test here.**
- Winner: most slots on the consuming subunit dest plug. Ties go to the lowest index.
- The unit-input entries are filled by reversing the subunit dest-plug sources (loop at `0x4d8d`). A data plug
  overwrites a sync (`0x40`) plug that already claimed the entry.
- Fallback: with exactly one unit input plug and one subunit, plug 0 is used without any routing answer.

**`+0x166B` / `+0x166C` (5739 / 5740): the sync destination plug** (subunit index, plug number; flag at `+0x1668`).
- Stored at `0x4c62`/`0x4c69` (compound AM824 form) and `0x4cf6`/`0x4cfd` (simple form).
- Rule: while walking every subunit's dest plugs in order, STATUS-read the stream format (vtable `+728`, input
  direction). If it is the AM824 **sync** format, store this plug.
  - Compound: root `0x40`, one field, field format `0x40`.
  - Simple: root `0x00`, format code `0x40`.
- There is no `break`, so **the last** matching plug wins.
- `DefaultDeviceInfo` (`0x2de8`) initialises both to `0xFF`. Readers: `DiscoverSyncSources` and `ChangeClockSource`
  (`0xaa5d`/`0xaa72`).

**What this predicts for our two devices (derived from the fixtures, not run):**
- Phase 88 capture plug: unit iso-out 0 (music src 0, 10 PCM + 1 MIDI) beats iso-out 1 (music src 5, sync).
- Phase 88 sync destination: music dest 9 (the last of the sync plugs 8 and 9), provided both answer `0x40`.
- Our existing derivation (plug 0 for capture and playback) agrees with Apple on both. The rule only matters on a
  unit with several iso plugs of different widths.

**The SIGNAL SOURCE reply byte (open item 3), decoded by Apple** (`AM824AVC::GetSignalSourceInfo`, `0x11d82`,
called through vtable `+784` from `GetPlugRouting`; corrects an earlier note here that said the call left this binary):
- Command sent: STATUS (ctype 1), subunit `FF`, opcode `1A`, operands `FF  FF FE  <dest subunit> <dest plug>`.
  This is byte for byte what our STATUS path writes.
- Accepted only if the response code is `0C` (STABLE/IMPLEMENTED).
- Reply byte 3 (= our operand 0, `firstByte`) is split into three fields:
  - bits 3..0: `PlugSignalStatus` (written to `a4+12`);
  - bit 4: a boolean (written through `v16`/`a6`);
  - bits 7..5: `PlugStreamStatus` (written to `a4+8`).
- Reply bytes 4 and 5 are the source subunit and source plug.
- Apple's own `AVCVideoServices-42/MusicSubunitController.cpp:1644-1710` sends the same frame, treats `0C` as
  connected and stores byte 3 as `SourcePlugStatus` (it presets `0xEE` for "no connection").
- Our measured bytes decode as: `0x10` = signal 0, flag 1, stream 0; `0x30` = signal 0, flag 1, stream 1;
  `0x70` = signal 0, flag 1, stream 3. **The flag is set in all three and the signal nibble is 0 in all three.**

**The spec: TA Document 2002010, AV/C Connection and Compatibility Management (CCM) Specification 1.1, 19 Mar
2003** (`/Volumes/SDExt/DEV/FirWireDriver/1papers/2002010-2.pdf`; the file is untitled, and the ta1394 `ccm` README
names this document). Text extract: `tmp/specs/2002010.txt` (read lines 1553-1735 only).
- **§7.1.5 SIGNAL SOURCE status response format, Figure 7.8:** `operand[0]` = `output_status` (3 bits, 7..5) |
  `conv` (bit 4) | `signal_status` (4 bits, 3..0). `operand[1..2]` = signal_source, `operand[3..4]` = destination.
  Apple and FFADO split the byte the same way. It is **not** the signal-format command (`0x18`/`0x19`); `conv` only
  says whether *that* command may change the format.
- **`output_status`** depends on what the destination plug is:
  - serial bus oPCR (**Table 7.7**): 0 effective (data packets are flowing through the oPCR), 1 not effective (no
    internal connection or no data from the source), 2 insufficient resource (oPCR sends empty packets for lack of
    bandwidth), 3 ready (signal reaches the oPCR but no isochronous connection exists, so no packets go out),
    4 virtual output, 5..7 reserved. Decision flow: Figure 7.9.
  - external output plug (**Table 7.8**) and subunit destination plug (**Table 7.9**): only 0 effective (signal
    flows into the plug) and 1 not effective; 2..F reserved.
- **`conv`** (**Table 7.10**): 1 = the transfer format can be changed at the signal_destination plug. Defined for
  serial bus oPCRs only; for ext output and subunit destination plugs it "is set to zero in the response".
- **`signal_status`** (**Figure 7.10**): four flag bits, `processed | filtered | converted | OSD overlaid`; all zero
  means the signal at the destination is identical to the source. An audio function block that modifies the signal
  should set `processed`.

**What the fixtures show** (`fixtures/phase88_descriptors.json`, `duet_descriptors.json`, STATUS replies, 33 records):

| Byte | output_status | Plugs that carried it |
|---|---|---|
| `0x10` | 0 effective | Phase 88: unit iso-out 0 (capture stream) and music dest 0 (fed by unit iso-in 0, the playback stream) |
| `0x70` | 3 ready | Phase 88: iso-out 1 and every other connected plug. Duet: every connected plug, queried idle |
| `0x30` | 1 not effective | Phase 88: only plugs whose source is `FF FE` (not connected): audio dest 7, music dest 8 and 9 |

- **Spec-correct reading for the oPCRs:** Phase 88 iso-out 0 is `effective` = packets flowing, and iso-out 1 is
  `ready` = signal present but no isochronous connection. The idle Duet's iso-out 0 is `ready`. Both fit Table 7.7
  and the old note "`0x10` at gen 4 vs `0x70` at gen 1" (streaming vs idle). The fixtures still do not record
  whether audio ran during each query, so the streaming link is an inference, not a measurement.
- **Both devices deviate from the spec on the non-oPCR plugs** (declared, not fixed):
  - they return `ready` (3), a reserved value, for connected ext-out and subunit-dest plugs (Tables 7.8, 7.9);
  - they set `conv` = 1 on every reply, including ext-out and subunit-dest plugs where it must be 0;
  - `signal_status` is 0 everywhere, even where the audio subunit's mixer sits on the path (spec: `processed`).
  So only `effective` / `not effective` can be trusted on those plugs, and `ready` there just means "connected,
  idle".
- **Practical use (untested on hardware):**
  - "is the device sending" = STATUS on the unit iso-out plug, `output_status == 0`;
  - "is the device receiving our stream" = STATUS on the music dest fed by the unit iso-in plug, `== 0`.
- An INQUIRY reply uses a different first-byte meaning (FFADO: `resultStatus`, low nibble): the Phase 88 answered
  `0x30` to the INQUIRY for its existing music dest 0 route.
- FFADO (`references/libffado-2.5.0/src/libavc/ccm/avc_signal_source.h`, `eOutputStatus`) uses the same names for
  0..4, which is where I first got them.

**Still unknown:**
- Whether Apple acts on any of these values. `DiscoverSyncSources` and `InitializeDeviceInfo` do not branch on
  them in the code read so far.
- Annex A.2 (plug state model, referenced from §7.1.5) and Annex C Tables C.1 / C.6 (example status frames) not read.

## CCM Annex A.2 and Annex C read (2026-10-03, TA 2002010, `tmp/specs/2002010.txt` lines 3292-3705 and 4271-4621)

**A.2 output plug state model (Figure A.2)** gives the meaning behind `output_status`:
- Serial bus oPCR, O0 **effective**: one or more isochronous connections exist (PCR-active) *and* the device sends
  effective packets (a continuous stream that carries contents).
- O3 **ready**: the device could send effective packets if an isochronous connection existed (PCR-ready). It is the
  state after "all isochronous connections broken" (O0:O3).
- O1 **not effective**: no effective packets even if a connection exists: no internal connection to the plug, or one
  exists but no signal flows. Any PCR state is possible.
- O2 insufficient resource: PCR-active but only empty packets (bandwidth claim failed). O4 virtual output.
- **External output plugs have only two states:** always "effective" if they can output a signal, else "not
  effective". That confirms `ready` (3) on the Phase 88 and Duet ext-out plugs is not spec behaviour.
- Power off may show as a REJECTED reply to the status command rather than a state (A.2, O0:O1 trigger).
- Annex A.1 (input plugs) belongs to INPUT SELECT, which we do not use. Annex B is figures only.

**`conv` is the link to the signal-format commands.** Table 7.10: `conv = 1` means the transfer format at that oPCR can
be changed by OUTPUT PLUG SIGNAL FORMAT (`0x18` CONTROL, TA 2004006 §10.10). The Phase 88 reports `conv = 1` on both
iso oPCRs, and its `0x18`/`0x19` CONTROL to 96 kHz was ACCEPTED (open item 1); the Duet reports `conv = 1` on iso-out
0 too. So `conv` on a unit iso-out plug is a cheap "does this device take a rate CONTROL" flag, without sending one.
The spec's own Table C.1 example reply has `conv = 0` ("cannot be changed").

**SIGNAL SOURCE first byte, per spec** (Figure 7.1, Figure 7.8, Tables C.1 and C.2):
- STATUS command: `output_status` 7, `conv` 1, `signal_status` F as defaults, so `operand[0] = FF`. (Table C.1:
  command `FF`; the example reply is `output_status` 3 ready, `conv` 0, `signal_status` 0, i.e. `60`.)
- CONTROL command: reserved = 0, `result_status` = F, so **`operand[0] = 0F`**. The text says the reserved field
  "shall be set to 0 and shall not take F in any future extension" so that REJECTED responses to control and status
  commands can be told apart (§7.1.1). CONTROL ACCEPTED reply: `result_status` 0 "not source" (Table C.2).
- INQUIRY uses the control format.
- **Apple matches:** `SyncPlugReconnect` (`0x10fb6`, CONTROL, ctype 0) and `QuerySyncPlugReconnect` (`0x10f18`,
  INQUIRY, ctype 2) both put `0x0F` in `operand[0]`; `GetSignalSourceInfo` (STATUS) puts `0xFF`. They treat `09`
  (ACCEPTED) and `0C` (STABLE) as success. FFADO `serialize` writes `resultStatus & 0xF` (default `0x0F`) for CONTROL
  and INQUIRY. ta1394 `ccm/src/lib.rs:180` writes `0xFF` for both, which departs from the spec.
- **Ours departed, now fixed (2026-10-03):** the codec used to default `firstByte` to `0xFF` for STATUS, CONTROL and
  INQUIRY alike and called `0x0F` "Prism". `SignalSourceCommand.hpp` now has no first-byte field: the codec writes
  `SignalSourceStatusField::Request()` (`FF`) for STATUS/NOTIFY and `SignalSourceControlField::Request()` (`0F`) for
  CONTROL/INQUIRY, from named constants. The 28 Phase 88 clock-probe INQUIRY frames in the attach golden changed from
  `02 FF 1A FF ..` to `02 FF 1A 0F ..`. The inquiry results recorded above were captured with `FF` and are not yet
  re-run with `0F`; that needs the device.
