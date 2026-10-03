# AV/C stack: magic numbers, where they come from (2026-10-03)

Spec hardening and alignment. Scope: `ASFWDriver/Protocols/AVC` (core, commands, descriptors, discovery, graph,
transport), the AV/C vendor code in `Audio/Protocols/{BeBoB,Oxford}`, and the AV/C profiles in
`Audio/DriverKit/Config/AVC`. CMP (IEC 61883-1) and the non-AV/C families (DICE, RME, MOTU, Fireworks) are out.

Rule (user, 2026-10-03): requests are built from names taken from the spec, never from raw bytes at call sites.
The wire bytes exist only inside a codec, produced from constants that cite their table or figure.

## Method

`tmp/magic_scan.py` finds every hex literal in code (comments excluded) and separates declarations of a named
constant from inline uses. Result for the scope above, before the pass: **282 hex literals in named declarations and
227 source lines with an inline hex literal.** After the pass: **345 named, 97 inline lines.** Of the 97, 40 are the
device and CIP-payload tables in section C (each with its source) and 57 are enumerator values, the definitions of the
named constants themselves, or test data. Each group was traced to a spec table, a reference file or a hardware
capture. Spec text was extracted with `pdftotext` into `tmp/specs/` and read one section at a time.

Spec documents used (all in `1papers`): TA 2004006 General 4.2, TA 2002010 CCM 1.1, TA 2001002 Stream Format
Information, TA 1999008 Audio Subunit 1.0, TA 2001007 Music Subunit 1.0, TA 2002013 Descriptor Mechanism 1.2,
TA 1999045 Information Blocks, IEC 61883-6 (the Russian GOST R edition; its OCR damages some digits, row order is
intact). Not available locally: the extended stream format draft (0xBF, compound AM824), the Tape
Recorder/Player subunit spec, IEEE 1212.

How to read the status column: **spec** = value read in the cited local spec; **ref** = taken from a reference
implementation or Apple's source and not found in a local spec; **capture** = proven on hardware; **unverified** =
no source found yet.

---

## A. Converted in this pass (the bytes on the wire did not change)

Suite after the change: 2756 cases pass (2748 before, plus 8 new pins), all goldens untouched; the Debug dext builds
for arm64e and x86_64.

| Constant | Value | What it is | Source | Status |
|---|---|---|---|---|
| `kFirstResponseCode` | 08 | ctype codes are 0-4; 8-F are response codes | TA 2004006 §5.3.1 Table 8 | spec |
| `kUnspecifiedOperand` | FF | "all FF16" in STATUS commands | TA 2004006 Figs 26, 28, 36, 57 | spec |
| `kSubunitTypeShift/Mask`, `kSubunitIdMask`, `kUnitAddressByte` | 3 / 1F / 07 / FF | address byte layout; unit = type 1F, id 7 | TA 2004006 §5.3.4.1 Tables 11, 13 | spec |
| `kSpecifierIdMask` | 00FFFFFF | Config ROM specifier_ID is 24 bits | IEEE 1212 (not local) | ref |
| `HighByte`, `LowByte` | | big-endian split of a 16-bit operand | AV/C operands are big-endian | spec |
| `kUnitInfoFirstOperand` | 07 | UNIT INFO operand[0] (we send it too) | response: Fig 27; command: ta1394 `general.rs:29` | spec / ref, see F1 |
| `kUnitInfoUnitField` | 07 | "unit" field of the command | TA 2004006 Table 28 | spec |
| `kUnspecifiedCompanyId` | FF FF FF | company_ID of the command | TA 2004006 Table 28 | spec |
| `kSubunitInfoExtensionCode` | 07 | "shall presently have a value of 7" | TA 2004006 §9.3.1.1 | spec |
| `kSubunitInfoPageShift/Mask`, `kSubunitInfoExtensionMask` | 4 / 07 / 07 | operand[0] = page(3) / extension_code(3) | TA 2004006 Fig 28 | spec |
| `kSubunitInfoEntriesPerPage`, `kSubunitInfoEmptyEntry` | 4 / FF | four entries per page; empty slot FF | TA 2004006 §9.3.1.1, §9.3.1.2 | spec |
| `kPlugInfoSubfunction*` | 00 iso+external, 01 async, subunit 00 | PLUG INFO subfunction | TA 2004006 §10.1.1.1 Table 39 | spec |
| `kFmtEohBit/FormBit/CodeMask` | 80 / 40 / 3F | fmt byte = eoh \| form \| FMT | TA 2004006 Figs 56, 57, Tables 64, 65 | spec |
| `kFmtAudioMusic` | 10 | FMT of audio and music streams | IEC 61883-6 Table 2 | spec |
| `kFmtAm824` | 90 | composed: eoh 1, form 0, FMT 10 | derived; `static_assert`ed | spec |
| `kFmtStatusWildcard`, `kFdfStatusWildcard` | FF / FF | STATUS asks with fmt 3F (byte FF), fdf all FF | TA 2004006 Table 65 | spec |
| `kFdfSfcMask` | 07 | SFC is bits 2..0 of the AM824 FDF | IEC 61883-6 Fig 30, Tables 16, 19 | spec |
| `kUnusedPlugAddressByte`, `kPlugAddressBytes` | FF / 5 | padding of the 5-byte plug address | ta1394 `lib.rs:785-1060`; "don't care FF16" in TA 2001002 | ref |
| `kCompoundSyncSourceBit/RateControlMask` | 04 / 03 | compound AM824 flags byte | ta1394 `lib.rs:545-549, 570-572` (draft) | ref, see F4 |
| `kCompoundHeaderBytes/EntryBytes` | 5 / 2 | compound AM824 block layout | ta1394 `lib.rs:454-462` | ref |
| `kFormatRootAm` (citation added) | 90 | format hierarchy root "Audio & Music" | TA 2001002 Table 5.1 | spec |
| `kFormatLevel1Am824` (citation added) | 00 | level 1 AM824 | TA 2001002 Table 5.4 | spec |
| `kSelectorBlockSelectorLength`, `kFeatureBlockSelectorLength` | 2 / 2 | selector_length of Selector and Feature blocks | TA 1999008 §10, Figs 10.1, 10.2, §10.3 | spec |
| `kInputPlugUnspecified` | FF | selector STATUS input fb-plug; never in CONTROL | TA 1999008 §10.2 | spec |
| `kUnspecifiedControlData` | FF | Feature STATUS control_data placeholder | TA 1999008 §10.3.1 (FF = invalid for mute) | spec, see F3 |
| `FunctionBlockType::kCodec` | 83 | CODEC function block | TA 1999008 Table 10.2 | spec, see F2 |
| `DescriptorSpecifierType` | 00, 10, 11, 80-BF | descriptor_specifier_type | TA 2002013 §6.1 Table 14 | spec |
| `kOpenDescriptorReservedByte` | FF | byte after the OPEN subfunction | Table 30 says 00; Apple and the devices use FF | capture, see F11 |
| `kReadResultStatusRequest`, `kReadDescriptorReservedByte` | FF / 00 | READ DESCRIPTOR command fields | TA 2002013 §7.5 Table 35 | spec |
| `kTapeTransportStateOpcode`, `kTapeTransportModeOpcodeFirst/Last` | D0 / C1 / C4 | tape TRANSPORT STATE is answered with a mode opcode | Apple `IOFireWireAVCCommand.cpp:122-125, 136-140` | ref |
| `kResponseOpcodeCompareMask` | 7F | opcodes compared without bit 7 | legacy ASFW; Apple compares all 8 bits | unverified, see F6 |
| `FW::kNodeNumberMask`, `FW::NodeNumberOf` | 3F | node number inside a node_ID | `FWTypes.hpp` (bus[15:10] \| node[5:0]) | ref |
| `FW::Unpack` in place of hand-written masks | | 48-bit address split into hi/lo | existing helper, same result (node ID 0) | n/a |
| `kInfoBlockRawText/Name` | 000A / 000B | Raw Text and Name info blocks | TA 1999045 §4.8, §4.9 | spec |
| `kMusicInfoBlock*` 8100-8107 | | music status descriptor blocks | TA 2001007 §6.2.1-§6.2.3.5 | spec |
| `kMusicInfoBlock*` 8108-810B | | routing status, subunit plug, cluster, music plug | Apple `MusicSubunitController.h:68-79`; Phase 88 and Duet captures | ref + capture |
| `kMusicPortTypeMidi/None` | 0A / FF | cluster port type | Apple `MusicSubunitController.h:107-122`; no port type table in the local TA 2001007 | ref |
| `kMusicCapabilityNonBlocking/BlockingBit` | 01 / 02 | transmit/receive capability bits | TA 2001007 §5.2.1 Tables 5.5, 5.6 | spec |
| `kAudioListTypeTextDatabase` | 86 | text database list type | TA 1999008 §7.1 Table 7.1 | spec |
| `kAudioEntryTypeChildDirectory/TextDatabase` | 90 / 93 | audio object entry types | TA 1999008 §6.1 Table 6.1 | spec |
| `kListAttributeEntriesHaveObjectId`, `kEntryAttributeHasChildId`, `kAttributeSkip`, `kAttributeUpToDate`, `kAttributeHasMoreAttributes` | 10 / 20 / 40 / 08 / 80 | list and entry attribute bits | TA 2002013 Tables 11, 12 | spec |
| `kSourceIdSubunitDestinationPlug`, `kSourceIdNotConnected` | F0 / FE | source_ID function_block_type | TA 1999008 Tables 8.2, 9.1 | spec |
| `kFunctionBlockTypeClassMask/Class` | F0 / 80 | 80-8F = audio function blocks | TA 1999008 Table 9.1 | spec |
| BeBoB and M-Audio signal formats (`BeBoBProtocol.cpp` x2, `MAudioSpecialProtocol.cpp`) | | hand-built `{fmt 0x90, fdf {sfc, FF, FF}}` | now `Am824SignalFormat(plug, sfc)` | spec |
| `GenericAvcProfile`, `MAudioSpecialProfile`: `fdf`, `fmt` | `CipSfc::k48000`, `kFmtAudioMusic` | placeholder TX FDF and FMT | IEC 61883-6 Table 20, Table 2 | spec |
| `CipSfcFromHz` loop bound | `CipSfc::k192000` | last SFC code | IEC 61883-6 Table 20 | spec |
| Apogee `MixerSrc` gain bytes; INPUT SELECT reply `case` labels | `HighByte/LowByte`; `InputSelectResult` | byte split; result_status values | big-endian operands; TA 2002010 Table 7.18 | spec |
| Graph builder: `Am824Format::kMidiConformant`, `kSyncStream`, `kMusicCapabilityBlockingBit`, `kMusicPortTypeMidi` | | replaced bare 0D, 40, 02, 0A | as above | spec / ref |
| `kUnset*` markers | FF / FFFF | our own "not set" values, not wire values | n/a | n/a |

The CCM codecs added earlier today (commit `46e4ae8f`) already follow the rule; their sources are in
`phase-7-ccm.md`.

---

## B. Findings: where the code and a reference disagree

None of these is changed in this pass. Each needs a decision, and every one that touches the wire needs a
capture first (`AVC_DEVICE_HAZARDS.md`; "unrecognised is unsafe").

**F1. UNIT INFO operand[0]: we send `07`, the spec and Apple send `FF`.**
TA 2004006 Figure 26 says all five command operands are FF. Apple's kext sends all five FF
(`IOFireWireAVCUnit.cpp:946-950`). ta1394 sends `07` then four FF (`general.rs:29`), as we do. The Duet and the
Phase 88 answer `07 FF FF FF FF`. Do not change it without a capture on each device.

**F2. `FunctionBlockType` lacked CODEC `0x83`.** TA 1999008 Table 10.2 lists it. Added (no code uses it).

**F3. STATUS placeholders versus the spec's "invalid" values.**
Mute invalid is FF (TA 1999008 §10.3.1), which matches our fill. Volume invalid is 7FFF (§10.3.2), but a volume
STATUS sends FF FF, and FFFF is a real value there (-0.0039 dB). The ta1394 and Apple convention works on the Duet
and Phase 88, so it is a de-facto convention, not a spec value. Recorded in the constant's comment.

**F4. Compound AM824 is not in TA 2001002.** Table 5.4 defines level 1 values 00 (AM824), 01, 02 only; 03-FE are
reserved. The 40 level-1 value, the flags byte, and the entry formats 0F, 10 and 40 (TA 2001002 Table 5.5 stops
at 0D) come from the unpublished extended stream format draft through ta1394 and Apple. They are hardware-proven
on the Phase 88 only.

**F5. Sample-rate codes: three sets, one with a value the spec calls reserved.**
`StreamFormatRate` matches TA 2001002 Table 5.6 for 22.05k=0, 24k=1, 32k=2, 44.1k=3, 48k=4, 96k=5, 176.4k=6,
192k=7, but **88.2 kHz = 0x0A is not in that table** (8-E are reserved). It comes from BridgeCo through Linux
`bebob_stream.c:38-46` and Apple `MusicSubunitController.cpp:1262-1299`, and the Phase 88 lists it. `CipSfc` matches
IEC 61883-6 Table 20 (0..6 = 32, 44.1, 48, 88.2, 96, 176.4, 192 kHz). Apple's third set (music subunit field) is not
modelled. `CipSfcFromHz` loops to a bare `0x06`; it should end at the last `CipSfc` value.

**F6. Opcode comparison ignores bit 7 (`kResponseOpcodeCompareMask`).**
It comes from the pre-phase-3 `FCPTransport` and nothing supports it: Apple compares the whole byte
(`IOFireWireAVCCommand.cpp:154-157`). A response whose opcode differs only in bit 7 is accepted. Candidate fix:
compare all eight bits, then run the goldens and a hardware attach.

**F7. The music status-descriptor parser reads `8101`-`8105` as capability blocks (unverified).**
Two descriptors, two layouts (TA 2001007 1.0). The *identifier* descriptor (§5) holds the capabilities as plain fields
inside `music_subunit_specific_information` (Figure 5.3), switched on by the bits of a `capability_attributes` byte
(Table 5.4: bit 0 general, 1 audio, 2 MIDI, 3 SMPTE, 4 sample count, 5 audio SYNC); they have no info block type
numbers. The *status* descriptor (§6) is made of typed info blocks: `8100` general status area (§6.2.1), `8101` music
output plug status area (§6.2.2: `[number_of_source_plugs]` + nested `8102`), `8102` source plug status (§6.2.3:
`[source_plug_number]` + nested `8103`-`8107`), `8103` audio info, `8104` MIDI info, `8105` SMPTE, `8106` sample count,
`8107` audio SYNC (§6.2.3.1-§6.2.3.5). The captures match the spec exactly: the Phase 88 status descriptor is `8100`
(`02 03 FF FF FF FF`), `8101` (primary `04` = 4 source plugs) containing `8102` (primary `00`) containing `8103`
(primary `0A` = 10 audio streams) and `8104` (primary `02` = 2 MIDI streams), then the non-spec `8108` routing block;
the Duet has only `8100` and `8108`.

The parser walks that nesting correctly, but it also reads top-level `8101`-`8105` with the *identifier-descriptor*
capability layouts (audio capability: a first-entry channel count; MIDI capability: version, adaptation layer, port
counts; SMPTE, sample count, audio SYNC: Tx/Rx and Ex/Bus bits). What is and is not known:
- On the Phase 88 the audio read fails for lack of bytes (the real `8101` primary field is one byte), and `8102`-`8105`
  appear only nested in both captures, never at top level.
- FFADO's status-descriptor loop handles only `8100`, `8101` and `8108` at top level and skips every other type;
  Apple's header names `8101` the Music Output Plug Status Area.
- So the reads are **unreachable on the two captured devices and on any spec-conforming device. Nothing is known about
  other devices**, and no reference shows any device sending the identifier layouts at top level of a status descriptor.
- They were written on 2026-09-30 (`af12d89b`, the new status parser, from the January snapshot's labels), not taken
  from a reference.
- The app's capability flags and counts do not depend on them: `BuildMusicCapabilities` also derives audio and MIDI
  presence and counts from the discovered plugs. Only `8100` (tx/rx capability, same bit layout as Tables 5.5 and 5.6)
  is exercised and used.
- **The one real risk is a misread, not an empty value:** a device whose `8101` carried a primary field longer than the
  spec's one byte would have the audio read take those bytes as "max channels".

The case labels carry the spec names and an `AUDIT F7` comment. Options, none applied: (a) keep everything and record
unrecognised top-level blocks in the AV/C Report so a third device shows itself; (b) as (a), plus skip the capability
read of `8101` whenever it has nested `8102` children (the spec shape), which removes the misread on conforming
devices and keeps the guess for the rest; (c) delete the reads once more devices are captured.

**F8. The feature-control bitmap bit order has four disagreeing sources.**
TA 1999008 Table 8.3 says "Bit 0: Mute, Bit 1: Volume, ..." without fixing the numbering direction.
(1) `FeatureControlMask` in `AudioSubunitDescriptor.hpp` is LSB-first (`kMute = 0x0001`) and only the tests use it.
(2) The Phase 88 capture `00 15 | 00 02 | 00 | c0 00 ...` reads as Mute + Volume only if bit 0 is the most
significant bit. (3) The discovery reducer tests `0x8000 >> bit`, MSB-first, on the same value; for the Duet's
one-byte elements that test is wrong. (4) The Duet's bitmap fits neither order
(`fixtures/duet_descriptors.md`). Mute and volume are always probed with STATUS, so audio works, but every other
control is probed or skipped by an order the code cannot justify. `kControlBitmapFirstBit` names the reducer's
assumption. Rule already recorded: confirm controls by STATUS, never by the descriptor.

**F9. Duplicated definitions.**
`AudioFunctionBlockType` (Descriptors) and `Cmd::FunctionBlockType` hold the same values and cite different section
numbers (§8.x versus §10.x). `kSubunitDestinationPlug = 0xF0` exists in `MusicPlugEndpoint` and as
`kSourceIdSubunitDestinationPlug` in `AudioSourceId`. `kBooleanTrue/False` and the feature control selectors exist in
the command codec only. Candidate: one definition each.

**F10. SIGNAL SOURCE first byte.** Fixed in `46e4ae8f` (CONTROL and INQUIRY now send `0F`, STATUS `FF`); the Phase 88
inquiry re-run on hardware is open (`open-items.md` item 8).

**F11. OPEN DESCRIPTOR reserved byte.** TA 2002013 Table 30 says 00 and FFADO sends 00; Apple and our captures use FF
and both devices accept it. Kept, documented in the constant's comment.

---

## C. Not converted: where each remaining literal comes from

### C1. `AVCCommandFilter.hpp` (18 inline lines, 136 bytes)

Whole-frame byte tables for the M-Audio special firmware allowlist. Each row already cites its source:
- sig-fmt STATUS input / output: Linux `bebob_maudio.c:302-313`; ALSA `bebob/src/maudio/special.rs:101-119`.
- sig-fmt CONTROL input / output: Linux `bebob_maudio.c:315-338`; vendor kext `AVCControlPlugSignalFormat` (vtable `0x37510`).
- vendor clock/format: Linux `bebob_maudio.c:186-198`; vendor kext `SetClockSourceInternal` (`0xe25c`) and FireBug.
- blank-slate input selector: vendor kext (`0xe45a` to `0x23148`); Linux `bebob_maudio.c:461-462, 514`.
- LED: vendor kext `AVCControlSetLEDStatus` (`0xdaaa`); ALSA `special.rs:121-167`.

Candidate (needs a decision, because this is a safety allowlist): generate each row from the named encoders
(`PlugSignalFormatCommand`, the selector codec, the vendor-dependent codec) so a byte cannot drift from the codec,
and keep the golden `fw1814__allowlist_enforcement.trace` as the guard.

### C2. Vendor codecs

- **Apogee Duet** (`ApogeeVendorCodec.hpp`): the 21 command codes (`MicPolarity 0x00` ... `DisplayFollowToKnob 0x22`)
  are already a named enum (the scan counts them as inline because the enumerators have no `k` prefix). Source:
  `snd-firewire-ctl-services protocols/oxfw/src/apogee.rs`; OUI and the "PCM" signature `50 43 4D` at
  `apogee.rs:127, 870-871`. `kApogeeArgDefault FF` and `kApogeeArgIndexed 80` are named. Nothing to convert.
- **Phase 88 mixer** (`Phase88MixerData.hpp`): function block ids 06, 07, 01 are literals inside `SelectorRoute{...}`
  and `ChannelMute{...}`. Source: FFADO `phase88control.py:42-64` and the Phase 88 descriptor capture (22 named
  blocks, `fixtures/phase88_descriptors.md`), confirmed on hardware 2026-09-27. Candidate: name the three ids.
- **M-Audio special routing** (`MAudioSpecialRouting.hpp`): register `FFC7:00700094` and the twelve route bytes.
  Source: measured 1814 playback route and the ALSA special mixer defaults (`special.rs`). No local spec exists for
  the vendor register map.

### C3. CIP and AM824 constants that live outside the command layer

| Where | Literal | Meaning | Source | Status |
|---|---|---|---|---|
| `MackieOnyx400FProfile.cpp` | `fdf = 0x01`, `fmt = 0x10` | SFC 44.1 kHz, CIP FMT | IEC 61883-6 Table 20, Table 2 (Fireworks/EFC, not AV/C) | left: converting it would make an EFC profile depend on an AV/C header; wants a shared CIP constants header |
| `Audio/Wire/AMDTP/AmdtpTypes.hpp:70,79` | `0x80000000`, `0xCF000000` | non-audio slot word (label 80 = MIDI conformant), cadence slot word (label CF = ancillary data) | TA 2001007 §5.2.2 Table 5.8 (AM824 label values: 80-83 MIDI, C0-EF ancillary) | spec for the labels; why these exact words is unverified |
| `PcmSlotCodec.cpp`, `AM824Encoder.hpp` | `0x40000000` | AM824 label 40 = multi-bit linear audio | TA 2001007 Table 5.8 (40-4F) | spec |

### C4. Positional offsets and sizes (decimal)

124 decimal layout literals remain in the AV/C codecs: reply fields read as `in[N]`, minimum sizes such as
`in.size() < 5`, and `Take(N)` widths. They are the byte positions of the spec figures. They are listed by file
because they are the next category to convert (for example with named offset constants or the `ParseReader` used in
the descriptor code):

| File | Offsets / sizes |
|---|---|
| `Commands/GeneralCommands.hpp` | 29 |
| `Commands/FunctionBlockCommand.hpp` | 24 |
| `Commands/StreamFormatCommand.cpp` | 19 (the list/single reply offsets are now named) |
| `Extensions/BridgeCoPlugInfo.cpp` | 10 |
| `FCPTransport.cpp` | 9 |
| `Commands/StreamFormatCommand.hpp`, `SignalSourceCommand.hpp` (and the since-deleted `InputSelectCommand.hpp`, `OutputPresetCommand.hpp`: 8, 5) | 6, 5 |
| `Descriptors/*.cpp` | 7 |

### C5. Not magic

`Descriptors/ParseReader.hpp` (21 literals) is `static_assert` test data. `RateCodes.hpp` enumerators are named
(the scan could not see them because `k32000` has a digit after `k`). Everything else in the scan was a field default
that is now a named `kUnset*` marker.

---

## D. Verification of this pass

- Host suite: 2756 cases pass; no golden changed; Debug dext build succeeds (arm64e, x86_64).
- **Mutation checks, 23 of 23 caught** (20 by tests, 3 at compile time by `static_assert`). Each mutation changes one
  new constant to a wrong value: UNIT INFO first operand, PLUG INFO async subfunction, SUBUNIT INFO page shift and
  extension code, FDF SFC mask, compound flag bits, plug address padding, selector and feature lengths, selector STATUS
  plug, OPEN and READ DESCRIPTOR bytes, specifier types, tape TRANSPORT STATE opcode, opcode compare mask, global
  unspecified-operand value, subunit id mask, blocking capability bit, audio text list type, routing status block
  type, source-id "not connected". The harness is `tmp/magic_mutation.py`.
- **Eight constants were not pinned by any test** on the first run and now are (new tests in `AvcCodecTests`,
  `FCPTransportTests`, `AvcGraphBuilderTests`, `AudioSubunitDescriptorTests`): SUBUNIT INFO page placement, the SFC
  mask against the rate-control bit, the compound flags byte, the tape TRANSPORT STATE quirk (using the wire literal,
  not the constant), the opcode bit-7 behaviour (a characterization test for F6), the blocking capability bit, and
  the source-id sentinels.
- **Harness lessons.** The first attempt listed six test targets that no longer exist as CMake targets (leftover
  executables in the build directory). `make` stops at the first bad target, so every target after it was silently
  not rebuilt, and my filter looked for `error:` and missed "No rule to make target". Results for those targets were
  invalid and were discarded; the whole batch was rerun after every target name was checked against
  `cmake --build --target help`. The script now treats any non-compile build failure as an infrastructure error
  instead of a result, and rebuilds after each restore.
- Not run: hardware. The conversions are byte-identical by construction and by the unchanged goldens, but no device
  was attached.

## E. Runtime visibility: every discovered value is logged by name (2026-10-03)

Naming the constants in source is half of the hardening; the other half is that a log reader sees the
same names. Before this pass a finished discovery reached the driver ring as `result=completed`, the
discovery document dropped the SIGNAL SOURCE status byte, and `FCPTransport` logged raw `ctype=0x%02x`.

**One path, no duplicates.** `Discovery/DiscoveryLog.{hpp,cpp}` turns a committed `DiscoverySnapshot` (and
the graph built from it) into lines; `AVCUnit::LogDiscovery` is the single caller and writes them to the
ring. Every line starts with `[AvcCaps]`. Query it with
`asfw_log_query {"categories":["AVC"],"contains":"[AvcCaps]","maxRecords":200}` (the dext has no os_log
categories, so the tag is the filter).

**The contract** (`Core/AvcNames.hpp`): a value a table knows prints as `name(0xNN)`; a value it does not
prints as `UNKNOWN(<table>:0xNN)`. It is never dropped and never guessed. A log that contains `UNKNOWN(` is
a device sending a value nobody has named. The tables are in `Core/AvcNames.hpp`,
`Commands/CommandNames.hpp` and `Descriptors/DescriptorNames.hpp`, and each cites the spec table (or, where
the local TA copy is silent, the Apple/FFADO source) that defines its values.

What is logged: unit and plug counts, subunits, every plug's signal format / current format / supported
formats (compound AM824 entry by entry), every route with the SIGNAL SOURCE status byte split into
`output_status`, `conv`, `signal_status` plus a `route_check` line naming any departure from TA 2002010
Tables 7.7-7.10, every descriptor read with its errors, every top-level info block of the music status
descriptor **by type, whether or not the parser reads it**, music plug usage / port type / plug type /
routing support, audio function blocks and their sources, feature and selector status, failed probes
(aggregated with counts), extension formations, and the resulting graph. Lines carry the subunit
address byte once (on the `subunit` line) and `Type(0xNN)#id` after that.

Also changed in the same pass:
- `FCPTransport` now logs `STATUS(0x1) SIGNAL SOURCE(0x1a)` and, new, the named response code of each
  accepted response, in place of raw `ctype=0x%02x opcode=0x%02x`.
- The discovery document's route object gained `firstOperand`, `sourceName`, `destinationName`, `status`
  and `deviations` (additive; the schema version is unchanged).
- Music plug usage (`!= 3`, `== 0x04 || 0x05`) and music plug type (`kAudioMusicPlug`) are named constants;
  the 810B `routing_support` byte, previously skipped as "reserved", is read (FFADO
  `avc_descriptor_music.cpp:376-470`, Apple `MusicSubunitController.h:148-165`).

**What the new log showed on the two captured devices** (goldens `tests/golden/avc/*__discovery_log.txt`):
- No `UNKNOWN(` on either device: every value they send is named.
- Duet: every subunit-plug route answers `output_status=ready, conv=1`. TA 2002010 allows only
  `effective` / `not effective` on a subunit destination plug and `conv` only on an oPCR, so
  `route_check` flags both on each of the five plugs. This is the device, not the decoder.
- Phase 88: its music status descriptor carries a top-level **0x8101 "music output plug status area"
  block of 705 bytes** that the parser recognises by type and does not read (audit F7). Its
  routing-status block says 4 source plugs; the unit's SUBUNIT INFO says 6.

**Not covered, on purpose:** the bytes inside an info block whose type is known but whose layout the
parser does not read (8101's 705 bytes) are logged by type and size only. Dumping them would be a second
decoder with no spec behind it.

Verification: `DiscoveryLogTests` (16 cases: every table has a known and an unknown case, unknown top-level
block / plug usage / port type are flagged, a 400-character name is wrapped without loss, the route status
and its deviations are logged), the two golden logs, and `DuetDiscoveryDocumentCarriesTheDecodedRouteStatus`.
Dropping one table entry fails the unit test and both goldens. Host suite 2775/2775; dext builds. Not yet
run on hardware.
