# TerraTec PHASE 88 Rack FW — Hardware Topology Research

This record captures read-only, hardware-confirmed discovery against the
TerraTec PHASE 88 Rack FW on 2026-07-16. It is implementation evidence for the
ASFW BeBoB backend, not a replacement for a streaming test.

## Identity and transport

| Field | Observed value |
|---|---|
| Vendor / model | `0x000AAC` / `0x000003` |
| Name | TerraTec PHASE 88 Rack FW |
| GUID | `0x000AAC0300B1D1F7` |
| AV/C specifier | `0x00A02D` |
| Link | S400 |
| FCP command / response registers | `0xfffff0000b00` / `0xfffff0000d00` |

The device does not need generic AV/C `UNIT_INFO` or `SUBUNIT_INFO` before
BridgeCo discovery. It responds cleanly to generic Unit `PLUG_INFO` and
BridgeCo extension STATUS commands.

## Unit plug inventory

The generic AV/C Unit `PLUG_INFO` response reports:

| Unit plug class | Input count | Output count |
|---|---:|---:|
| Isochronous | 2 | 2 |
| External | 8 | 7 |

ISO plug 0 is type `0x00` (isochronous audio) in both directions. ISO plug 1
is type `0x03` (sync) in both directions; it is not an AMDTP audio stream.

## Audio stream geometry

Both directions advertise the same five format entries, each with 10 PCM
channels, one MIDI data slot, and `DBS=11`:

| AV/C rate code | Meaning |
|---|---|
| `0x02` | 32 kHz |
| `0x03` | 44.1 kHz |
| `0x04` | 48 kHz |
| `0x0A` | 88.2 kHz |
| `0x05` | 96 kHz |

At the target 48 kHz rate, the negotiated packet geometry is therefore
`10 PCM + 1 MIDI = 11 AM824 data blocks per AMDTP packet`.

### Channel maps

BridgeCo uses the following one-based stream positions. The MIDI section has
two logical channel locations, both referencing stream position 11; it remains
one MIDI data block.

| Direction | Sections | PCM positions | MIDI position |
|---|---|---|---:|
| BridgeCo IN | 8-channel line, 2-channel SPDIF/AC3, MIDI | `2, 7, 3, 8, 4, 9, 5, 10, 1, 6` | 11 |
| BridgeCo OUT | Line 1/2, 3/4, 5/6, 7/8, SPDIF, MIDI | `2, 7, 3, 8, 4, 9, 5, 10, 1, 6` | 11 |

Section names supplied by the device:

- IN: `PHASE88 FW Multichannel Out`, `SPDIF/AC3 Out`, `MidiSection.0`.
- OUT: `Line In 1/2`, `Line In 3/4`, `Line In 5/6`, `Line In 7/8`,
  `SPDIF In`, `MidiSection.0`.

## External plug inventory

| Direction | Plug IDs / reported BridgeCo types |
|---|---|
| IN | `0–4`: ISO, `5–6`: MIDI, `7`: SYNC |
| OUT | `0–4`: ISO, `5–6`: MIDI |

The four MIDI endpoint count reads are asymmetric: IN 5/6 each report `2`;
OUT 5/6 each report `0`. Preserve these raw values until the MIDI-device
mapping is validated with active hardware streaming.

## Music Subunit clock topology

The Music Subunit reports 10 input plugs and 6 output plugs. Its input plugs
are `0–5`: ISO, `6–7`: MIDI, `8–9`: SYNC. Both SYNC input source descriptors
are all `0xff`, BridgeCo's no-upstream-input encoding. Linux's clock-source
logic classifies this as the internal clock.

`asfw_bebob_get_clock_topology` follows Linux's first-SYNC-input behavior and
will therefore report input plug 8 as the current internal-clock path while
retaining all probed FCP receipts.

## BootROM block

MCP successfully read 104 bytes at `0xffffc8020000`; the block starts with
`bridgeCo`. The current decoder rejects it because this device pads the
`HHMMSS` timestamp tail with NUL bytes and stores its GUID as two little-endian
quadlets in high-word/low-word order.

The raw record indicates protocol version 1, hardware model 3, hardware
revision 1, software image ID 3, image base `0x20080000`, maximum image size
`0x00180000` (1.5 MiB), software timestamp `2005-12-15 16:37:13 UTC`, and
bootloader timestamp `2004-07-19 13:40:46 UTC`. No debugger record is present.

## Remaining work

- Surface the stored channel permutation in the runtime stream profile.
- Implement live CMP iPCR/oPCR inspection in the MCP adapter and compare it
  against Linux’s reserve/connect order.
- Reserve CMP resources and start/stop 48 kHz AMDTP only in a dedicated,
  explicitly authorized hardware-stream test.
