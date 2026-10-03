# Phase 7: the CCM protocol as named requests (2026-10-03)

Source: TA Document 2002010, AV/C Connection and Compatibility Management Specification 1.1
(`1papers/2002010-2.pdf`; text in `tmp/specs/2002010.txt`). Branch `feat/avc-ccm`.

## Rule

No raw numbers at call sites. A request is built from a named factory, an enum or a typed field taken from the spec.
The wire bytes exist only inside the codec, produced from named constants that cite their table or figure. Replies
keep every raw value and offer a named view that returns `nullopt` for a reserved value. A device that sends a reserved
value is not a parse error; `CheckStatusAgainstSpec` reports the departure as data.

## What is in the tree

| File (under `ASFWDriver/Protocols/AVC/Commands/`) | Contents |
|---|---|
| `CcmTypes.hpp` | `UnitPlugId`, `BusNodeId`, `SignalAddress` (+ `SignalAddressKind`), `OutputStatus`, `SignalModifications`, `SignalSourceStatusField`, `SignalSourceControlField`, `SignalSourceResult`, `DestinationPlugKind`, `StatusDeviations`, `CheckStatusAgainstSpec`. `static_assert`s pin Table C.1 / C.2 values. |
| `SignalSourceCommand.hpp` | SIGNAL SOURCE `0x1A`. Requests: `QuerySignalSource` (STATUS), `ConnectSignalSource` (CONTROL), `CanConnectSignalSource` (SPECIFIC INQUIRY), `WatchSignalSource` (NOTIFY). Reply views: `Status()`, `Control()`, `AsVirtualOutput()`. |
| `InputSelectCommand.hpp` | INPUT SELECT `0x1B`. Requests: `QueryInputPlug`, `ConnectInput`, `ChangeInputPath`, `SelectInput`, `DisconnectInput`. Reply views: `AsControl()`, `AsStatus()`. |
| `OutputPresetCommand.hpp` | OUTPUT PRESET `0x1C`. Requests: `QueryPresetCount`, `QueryPreset`, `AddPreset`, `CancelPreset`. |
| `CcmProfileCommand.hpp` | CCM PROFILE `0x1D` (STATUS only). Request: `QueryCcmProfile`. |
| `Core/AvcTypes.hpp` | `Opcode::kInputSelect`, `kOutputPreset`, `kCcmProfile` (Table 7.1). |

Tests: `tests/protocols/CcmCodecTests.cpp` (46 cases): the spec's Annex C frames (Tables C.1, C.2, C.6), every field of
every figure, and Phase 88 / Duet STATUS replies including the departures they show. Existing SIGNAL SOURCE tests and
the discovery reducer use the named requests.

## Declared changes

- **Wire:** SIGNAL SOURCE CONTROL and SPECIFIC INQUIRY frames now carry `operand[0] = 0F` (Figure 7.1; Apple and FFADO
  agree) instead of `FF`. Only the discovery reducer's `ClockProbe` (INQUIRY) sends one: 28 frames in the Phase 88 attach
  golden, updated. STATUS frames are unchanged (`FF`, Table C.1). The Duet golden has no INQUIRY.
- **API:** `SignalSource::firstByte` is gone; `first` holds the raw byte and `Status()` / `Control()` interpret it by
  command type. `SignalAddress::StatusWildcard()` is `NoSignal()`. `kSignalSourceFirstBytePrism` is deleted: `0F` is the
  spec default, not a Prism quirk.
- **Wrong citation fixed:** the golden allow-list justified `02FF1AFF.*` with FFADO `avc_signal_source.cpp:137-141`.
  FFADO writes `resultStatus & 0xF` there, which is `0F`.

## Not built, on purpose

- **No caller sends INPUT SELECT, OUTPUT PRESET or CCM PROFILE.** No supported device is known to implement them, and an
  unproven frame on the wire has frozen firmware (`AVC_DEVICE_HAZARDS.md`). They are codecs with host tests until a
  capture proves a device accepts the frame. The command allowlist is unchanged.
- **Spec §4-6 (connection scenarios and procedures) and the Digital/Analog Changeover profile (§8.2).** They describe a
  controller connecting other devices. Our `CMPClient` plus SIGNAL SOURCE covers what an audio unit needs.
- **Extended subunit_type addressing** in signal address fields (allowed by Figures 7.3, 7.5, 7.13, 7.19): not
  modelled, as elsewhere in this layer (`SubunitAddress::IsExtendedType`).

## Spec observations worth keeping

- Annex C Table C.2 labels result_status `0` "not source"; normative Table 7.6 says `0` is "source". The codec follows
  Table 7.6 (a test comment records it).
- Both measured devices break Tables 7.8-7.10: they report `ready` (3) on external-output and subunit-destination
  plugs and set `conv` on every reply. `CheckStatusAgainstSpec` flags exactly those two; both are in the tests.
- `conv = 1` on a unit iso-out plug advertises that OUTPUT PLUG SIGNAL FORMAT CONTROL works (both devices do).
- The Duet answers `0x70` ("ready") on every plug **while streaming**, identical to idle (read-only STATUS via MCP,
  `fixtures/duet_signal_source_streaming.json`). `output_status` is therefore not a generic "audio is flowing" probe.

## Verification (2026-10-03)

- Host suite: 2748 registered cases pass (six existing skips). Dext Debug build: `DiscoveryReducer.o` rebuilt for arm64e
  and x86_64.
- Mutation checks, 16 of 16 caught: 6 by `static_assert` at compile time (control default `FF`, status conv default, ready and
  effective swapped, conv bit moved, output_status shift, deviation rule dropped), 10 by tests (NOTIFY frame, virtual-output
  prefix, INPUT SELECT plug position / status shift / node bytes, OUTPUT PRESET entry value / self bit / cancel, CCM
  PROFILE bits, opcode value). The first run of the virtual-output mutation passed because the script wrote the header
  inside the same second as the previous build (make compares mtimes at 1 s); the script now waits.
- Not run: pydice (`pytest` is not installed for this Python; no pydice file changed).
- **Hardware:** six STATUS frames ran on the streaming Duet (read-only, MCP, 2026-10-03); the running dext (`b1cb7031`)
  predates this branch. **Not run:** the new `0F` INQUIRY frame on any device, and attach with the new frames. Needed: attach the Phase 88 and the Duet with the new INQUIRY frames and compare with the
  previous attach; then the read-only `0F` re-run of the sync-plug inquiries (open item 8).
