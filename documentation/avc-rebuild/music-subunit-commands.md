# Music subunit commands and identifier descriptor (TA 2001007 §5, §7)

Status: codecs and parser written and host-tested. **Nothing sends them and nothing reads them.**
No reference stack implements any of this, and no capture holds one, so every layout below is the
spec text alone. Treat it as probably wrong on the wire until a device answers.

## What exists

| Piece | File | Spec |
|---|---|---|
| Opcodes + names | `Core/AvcTypes.hpp`, `Core/AvcNames.hpp` | Table 7.1 |
| Vocabulary (plug type, plug IDs, stream position, sub-functions, results) | `Commands/MusicTypes.hpp` | Tables 7.2-7.8, 7.19, 7.20; Figures 7.3-7.5 |
| DESTINATION / SOURCE PLUG CONFIGURE (0x40, 0x41) | `Commands/MusicPlugCommands.hpp` | §7.1, §7.2 |
| DESTINATION / SOURCE CONFIGURATIONS (0x42, 0x43) | same | §7.3, §7.4 |
| MUSIC PLUG INFO (0xC0) | same | §7.5 |
| CURRENT CAPABILITY (0xC1) | same | §7.6 |
| Spec names for logs | `Commands/MusicNames.hpp` | same contract as `AvcNames.hpp` |
| Identifier descriptor (specifier 0x00) parser | `Descriptors/MusicSubunitIdentifier.hpp` | §5, Figures 5.1-5.17 |
| Tests | `tests/protocols/MusicCommandTests.cpp` (36 cases) | Annex A frames, Tables 7.15, 7.17 |

Requests are built with named factories (`QueryMusicPlugInfo`, `QueryCurrentCapability`,
`QueryDestinationConfigurations`, `PlugConfigureEntry::Connect`, ...). Replies keep every wire value; a
named view returns `nullopt` for a reserved one. Encoding refuses what Table 7.1 does not allow
(CONTROL on anything but DESTINATION PLUG CONFIGURE, NOTIFY and INQUIRY everywhere, a non-Music subunit
address, 0 or more than 72 subcommands, a subfunction in a STATUS entry).

## What is deliberately not wired

- Discovery does not probe any of these commands and does not read the identifier descriptor. It reads the
  status descriptor (0x80) only (`DiscoveryReducer.cpp`).
- Per `avc-attach-sends-only-captured-frames`: an unproven frame can freeze Phase 88 / M-Audio firmware.
  These frames are unproven. A device with a command allowlist refuses them (`kRefused`).
- Adding a probe means: pick a device that is not Phase 88, send one MUSIC PLUG INFO STATUS, capture the
  reply, and turn that capture into a fixture before widening.

MUSIC PLUG INFO (0xC0) is the live equivalent of the plug counts in the 810B blocks, so it is the natural
first probe. The identifier descriptor read is ordinary READ DESCRIPTOR with specifier 0x00, as the Audio
subunit already gets.

## Spec problems found while writing the codecs

1. **CURRENT CAPABILITY entry size.** Figure 7.22 says `music_plug_format_info` is 3 bytes; Figure 7.23
   lays out `music_plug_ID (2) + format_info (2)`. The fields add up to 4, so the codec reads 4.
2. **Entry counts.** DESTINATION / SOURCE CONFIGURATIONS and CURRENT CAPABILITY give `start` and `end`
   plug IDs but no entry count, and the worked Table 7.17 mixes plug types under one start/end pair. The
   codec counts whole entries and ignores up to three trailing bytes as the response's quadlet padding.
   It records `start` / `end` raw and does not check them against the count.
3. **`compound` attribute.** Table 7.20 says each plug then has its own type, but a format entry carries
   no type. Format info is therefore decoded by the reply's single `music_plug_type`; a compound reply is
   kept raw.
4. **Result tables differ.** CONTROL replies use Table 7.6 (six values), STATUS replies Table 7.8 (four).
   `first` of a `PlugConfigureEntry` is the subfunction, or the result, so it has two readings
   (`Subfunction()`, `StatusResult()`).
5. **FF is overloaded.** Reserved in Table 7.3, "no value" in DISCONNECT_ALL / DEFAULT_CONFIGURE and
   queries, "all kind of plugs" in Table 7.18. It prints `none(0xff)` in replies and `all kinds(0xff)` for
   a MUSIC PLUG INFO request.
6. **Annex A needs INPUT SELECT.** Procedure 4 connects a serial bus input plug to a subunit destination
   plug with INPUT SELECT (CCM 0x1B). That codec was deleted in 83440bdf (unwired). Recover it from
   46e4ae8f if connection management is ever built.

## Identifier descriptor

Nesting follows Figures 5.1 and 5.2 and is read the way the Audio subunit identifier is
(descriptor length, then a type-dependent section that opens with its own fields length). Every length is a
bounded section; the parser stops at the first error and reports the absolute offset.

- `capability_attributes` (Table 5.4) says which of general / audio / MIDI / SMPTE / sample count / audio
  SYNC fields follow, in that order, each led by a one-byte length.
- A field longer than its known layout is accepted; a shorter one is `Truncated`.
- `attributes` and `capability_attributes` may chain (bit 7); later bytes are kept, not interpreted.
- Optional info blocks (§5.1, "for future expansion") and manufacturer bytes are counted, not read.
  A descriptor that ends before the manufacturer length is accepted.

**Captured 2026-10-03 from a Phase 88** (`fixtures/phase88_music_identifier.json`): OPEN, one READ of 67 bytes, CLOSE,
all ACCEPTED, no wedge. Generation 2, version 1.0, capability attributes `27` (general, audio, MIDI, audio SYNC):
general = transmit blocking / receive non-blocking+blocking (same as `8100`); audio = 5 formats, 10 in / 10 out
channels, FDFs 00-04 (the five rates of its plugs), label 0x40; MIDI 1.0, adaptation layer 0, **max input ports 4, max
output ports 0** (odd for a device with MIDI in and out; as reported, not yet explained); audio SYNC `03`, the same
value the status descriptor's `8107` reports as activity. The 10 / 10 channels match the nub's published geometry.

This descriptor is where the **capability** fields live (max audio channels, MIDI version, SMPTE / sample
count / sync). The status parser no longer guesses them (audit F7 resolved); `MusicCapabilities` holds only `8100`'s
fields. The identifier parser exists and is tested; discovery does not read the descriptor yet.

## Naming change

`music_plug_type` 0x80 now prints `audio SYNC(0x80)` (Table 7.3's word) instead of `sync(0x80)`. Two
golden lines changed (`duet__`, `phase88__discovery_log.txt`); nothing else.
