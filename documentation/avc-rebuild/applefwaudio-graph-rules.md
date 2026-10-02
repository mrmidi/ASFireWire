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
