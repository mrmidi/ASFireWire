# Audio subunit: FUNCTION BLOCK and dependent information (TA 1999008 §8-§11)

Spec: `1papers/1999008 AVC Audio Subunit Specification 1.0.pdf` (24 Oct 2000). Text extract used while writing:
`pdftotext -layout` into `tmp/` (headings are letter-spaced; read the PDF page for any figure you rely on).

## What exists

| Piece | File | Spec |
|---|---|---|
| Control catalogue: which `control_selector` means what in which block, and what value it carries | `Commands/AudioControlTypes.hpp` | Table A.4, §10 |
| Typed values (`ControlValue`: boolean, volume, balance, tone, delay, percent, 8.8 fixed, scale pairs), `Steps` | same | Tables 10.3-10.17 |
| Generic FUNCTION BLOCK codec, all block types, and the named requests | `Commands/AudioFunctionBlock.hpp` | §10, Figures 10.1-10.96 |
| CHANGE CONFIGURATION (`0xC0`) | `Commands/ChangeConfigurationCommand.hpp` | §11.1 |
| Spec names for logs (controls, values, replies, process / CODEC types) | `Commands/AudioNames.hpp`, `Commands/CommandNames.hpp` | Tables A.2, A.3, A.4 |
| Processing and CODEC dependent information in the identifier descriptor | `Descriptors/AudioSubunitDescriptor.hpp` | §8.4, §8.5 |
| Which control each Controls-bitmap bit is | `Descriptors/AudioControlBits.hpp` | Tables 8.3, 8.9, 8.10, 8.12, 8.15, 8.18-8.22 |
| Tests | `AudioFunctionBlockTests.cpp` (28), `AudioSubunitDescriptorTests.cpp` (+8) | |

The proven `SelectorOperands` / `FeatureOperands` (what discovery sends) are unchanged. The tests build the same
request both ways and compare bytes, so there is one wire format with the proven path as the reference.
The previous generic `FunctionBlockOperands` was dead code (no callers) and also left out the CODEC length byte; it is
replaced.

## What is proven and what is not

- **Seen on a device:** Selector and Feature mute / volume (Phase 88, Duet, discovery); Mixer dependent information
  (`01 00 00`: mixer, no programmable controls) on all five Phase 88 mixers.
- **Spec-only:** every other control, the Mixer command (Phase 88's mixers have no programmable controls, so there is
  nothing to set), all CODEC and type-specific Processing layouts, CHANGE CONFIGURATION. Nothing in the driver sends
  any of them; discovery's Audio probes are unchanged.

## Spec problems found

1. **Chorus Level has no selector.** §10.5.7 defines it and Table 8.12 gives it Controls bit 1, but Table A.4 lists
   only Rate (03) and Depth (04). It has no `ControlId` and cannot be sent; it still prints by name in the bitmap.
2. **DTS CODEC bits.** Table 8.18 gives bits 2 and 3 (`LFE_Select`, `Matrixed_Stereo_Select`); §10.6.4.1 says there
   are no DTS-specific controls and Table A.4 has none.
3. **Reverberation bit order is not the selector order** (Table 8.10 vs Table A.4). The bit table is its own table.
4. **Mode control layouts differ.** Processing Mode carries `Size_of_modes` then the mode bytes (Figure 10.52); CODEC
   Mode carries the mode bytes alone (Figure 10.99).
5. **High/Low scaling byte order differs.** MPEG: LowScale then HighScale (Figure 10.111). AC-3: HighScale then LowScale
   (Figure 10.119). Two value kinds, so a swapped pair is a refused request.
6. **Graphic equalizer bitmaps.** Table 10.13 gives bit numbers only; the four bytes are read as one big-endian number,
   bit 0 least significant. STATUS use of the BandsPresent fields is not spelled out, so there is no query builder.
7. **AC-3 status enumerations** (frame error, sample rate, data rate, room type, dual mono) are named in the text but
   their numeric values are not in the extracted text, so they stay raw bytes with the spec's selector name.
8. **Control bitmap bit order.** Spec Table 8.3 does not fix the direction. MSB-first matches Phase 88; the Duet
   disagrees (see `magic-numbers-audit.md` F8). The bitmap stays a hint; STATUS decides.

## Opcode numbers

`CHANGE CONFIGURATION` is `0xC0`, the same number as the Music subunit's `MUSIC PLUG INFO`. Opcode names are therefore
by subunit type: `Describe(SubunitType, Opcode)`, used by `FCPTransport` and the discovery log.

## Naming change

Feature control selectors print as the spec's identifiers (`MUTE_CONTROL(0x01)`, was `mute(0x01)`) and control
attributes in capitals (`CURRENT(0x10)`, Table 9.3). One table (the catalogue) names every control; the old
`kFeatureControls` table is gone. 60 golden lines changed, all pure renames; 14 more now show the process type and
control bits by name.

## Not done

- Reading Audio replies for controls other than mute / volume into the graph or the app.
- Probing any of the new controls at attach. First candidates, once a device with them is connected: a Feature
  control the descriptor advertises (bass, treble), STATUS only.
