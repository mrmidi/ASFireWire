# Phase 2: Characterize the old stack, then move every caller

**Status 2026-10-02: DONE** on `feat/avc-transaction-engine`. The last legacy path (AVCCdb / AVCCommand /
IAVCCommandSubmitter / MAudioSpecialCommand) is deleted; the 1814 clock command is `Extensions/MAudioSpecialClock.hpp`;
MusicSubunit takes IAvcUnit&; the unit model is `Core/AvcUnitModel.hpp`. AvcDifferentialTests and the frozen legacy
copies are gone (the user: no legacy copies in tests). Continuations hold units through `Common::LiveRef`.

Detailed by the supervisor after the phase-1 review. Source: `00-overview.md` (stages A0-goldens and A1).

## 0. Carry-over from phase 1 (do these first)

Phase 1 was signed off on 2026-09-27 at `81058d15`. The supervisor verified 2,573/2,573 host tests and the
three AV/C targets (28 + 15 + 16).

**Status 2026-09-28: items 1–4 DONE at `a2792a30`** (22/22 differential tests, 2,579/2,579 host tests, dext
build clean; the supervisor spot-checked the 1814 probe wording and the clock-flag gate). **Item 5 DECIDED:** see
below. Original items:

1. **Differential tests for the two remaining old duplicates.** Both have live callers, so their bytes must
   match before the callers move:
   - root `Protocols/AVC/AVCSignalFormatCommand.hpp` ← `AVCUnit.cpp:12`;
   - root `Protocols/AVC/AVCStreamFormatCommand.hpp` ← `Audio/Protocols/Oxford/OxfwStreamFormats.cpp:9` and
     `Protocols/AVC/Audio/AudioSubunit.hpp:11`. The Oxford path is hardware-proven on the Duet and the Onyx-i:
     byte parity is required, not just "intended".
   Also call the old code for UNIT INFO and SUBUNIT INFO decode; those two tests rebuild the legacy side inline.
2. **Declare the channel-position strictness difference.** Old `ParseChannelPositionSections` requires the
   payload to be consumed exactly; new `BridgeCo::ParseChannelPositions` ignores trailing bytes. Add a test for
   each and list the difference in `documentation/reports/avc_differential_report.md` as intended or open.
3. **`tools/avc/avc_probe_1814.py`, before the user runs it on the 1814:**
   - Remove the plug-1 signal-format queries (`input_plug_1_…`, `output_plug_1_…`). The driver filter allows any
     plug id, but the evidence covers plug 0 only (Linux `bebob_maudio.c:302-313`, ALSA `special.rs:101-119`).
   - `--include-control` changes device state; don't call it "non-destructive".
     - Read the current rate (plug-0 STATUS) first and restore it at the end. Today the sweep leaves the device
       at 192 kHz, or 96 kHz on the ProjectMix.
     - The clock write sets internal clock and resets `dig_in_fmt`/`dig_out_fmt`, which select the formation
       table (hazard doc H5). Print what it will change and require a second flag
       (`--i-understand-this-changes-clock`) for the clock write.
     - Keep the default run (no flag) read-only.
4. **The hardware now proves the feature-block form.** On 2026-09-27 the Phase 88 ACCEPTED both the spec form
   (selector length `02`, FB1 ch1/ch2 volume) and the old driver's form (length `05`), and read back the value.
   Record this in the differential report next to the feature-block entry: "new form hardware-proven on the
   Phase 88".
5. **Reopen UNIT INFO's bytes.** Apple's own driver sends UNIT INFO with **no operands** (`01 FF 30`, a quadlet
   write; `fixtures/apple_duet_discovery_firebug.md`), the same as legacy `AVCUnit.cpp:76-81`. The phase-1 codec
   sends Linux's `07 FF FF FF FF` and calls it an intended difference. Phase 1's rule is that hardware-proven
   ASFW bytes win, and Apple agrees with them. Both forms get STABLE from the Duet and the Phase 88, so this is
   a choice, not a bug. **Decided 2026-09-28:** `BuildUnitInfoStatus()` defaults to the zero-operand form
   `01 FF 30` (Apple + legacy; the goldens capture it). The Linux 5-operand form stays available as an explicit
   option. Test both encodings, and that replies to both decode. Update the differential report entry from
   "intended difference" to "matches Apple and legacy; the Linux form is optional".
   Measured 2026-09-28 on the Duet: `01 FF 30 07 FF FF FF FF` and `01 FF 30 FF FF FF FF FF` get the identical
   reply `0C FF 30 07 08 00 03 DB`, so it ignores the first operand's value (`tmp/avc-desc/discover_0003db…_1221*`
   vs `…_1241*`). That's one device; send only the documented forms.

## 2a. The unit seam and characterization (no behaviour change)

1. **`IAvcUnit`, the unit seam** (runtime interface).
   - `Submit(frame, generation, completion)`, `Identity()`, `CurrentGeneration()`.
   - Asynchronous and callback-based; nothing blocks (see the CMP work-loop deadlock). Keeping one command
     outstanding is the engine's job, not the caller's.
   - Implementations: the real one (today's `FCPTransport`, later the phase-3 engine), `SimulatedAvcUnit`, and a
     recording wrapper for goldens.
   - Typed commands go through a **concept**, not inheritance. A command type provides `Encode()` and
     `static Decode(span) -> expected<Reply, AvcError>`; a template `Send<Cmd>(unit, cmd, generation, cb)` does
     the rest.
   - Check what the existing `IAVCCommandSubmitter` covers; extend or replace it. Don't add a second seam.
   - Concept source: Apple `IOFireWireAVCUnit` (behaviour only; see `00-overview.md`, concept map).
2. **`SimulatedAvcUnit`** (`tests/support/`).
   - Answers FCP writes to `0xFFFFF0000B00` from the phase-1 device images (`AvcDeviceImages.inc`).
   - Move `RecordingFireWireBus` out of `tests/devices/DICEDuplexTestSupport.hpp:225` into `tests/support`.
3. **Goldens of TODAY's behaviour**, before anything changes: `tests/golden/avc/<device>__<scenario>.trace`.
   - Devices: Duet, Phase 88, Onyx-i (from its documented capture), 1814 (allowlist path).
   - Scenarios: attach discovery, start/stop, bus reset mid-discovery, INTERIM, timeout, NOT IMPLEMENTED.

## 2b. API review and reshape of the phase-1 codecs (bytes unchanged, no caller moves)

Added 2026-09-28 after the user found a wrong abstraction in `FunctionBlockCommand`. Done **before** callers move,
so no caller is moved twice. Proof: the differential tests and the phase-1 hardware bytes stay identical
(including both selector-length forms proven on the Phase 88).

**Checklist, applied to every header in `Core/`, `Commands/`, `Extensions/`:**
1. **One type per spec command family, not per use case.**
2. **The ctype is chosen at send time,** not baked into function names. Today: `BuildSelectorStatus` /
   `BuildSelectorControl`, `BuildFeatureMuteStatus` / `…Control`, `BuildUnitInfoStatus`, … A command is one value;
   `unit.Status(cmd)` / `Control(cmd)` / `Inquiry(cmd)` picks the ctype (ta1394 `AvcStatus` / `AvcControl`
   traits). This is what the 2a `Send<Cmd>` concept expects.
3. **Response-code policy lives only in the engine.** Today every parser takes `ResponseCode expected`
   (`ParseSelector`, `ParseFeatureMute`, `ParseVendorDependent`, …). A codec decodes operands only.
4. **Typed values, not raw integers:** rates (already `StreamFormatRate`), volume in 1/256 dB with `0x7FFF`
   invalid and `0x8000` = −∞, plug addresses, booleans.
5. **A table where the spec has a table.**
6. **Vendor codecs are payloads on the one generic VENDOR-DEPENDENT command** (placement rule in `00-overview.md`).

**Known findings:**
- **`FunctionBlockCommand`: one function per control** (mute and volume × status/control/parse). The spec's shape
  is FUNCTION BLOCK `0xB8` → block type (Selector `0x80`, Feature `0x81`, Processing `0x82`) → control. Volume
  is one control of the feature block, not a block. Reshape into:
  - one generic function-block frame codec: `{type, id, attribute, selector bytes, data bytes}`;
  - typed commands per block type: `Selector {id, inputPlug}`; `Feature {id, channel, control, attribute,
    value}` with a per-control data-width table (mute 1, volume 2, LR/FR balance 2, bass/mid/treble 1, …;
    measured on the Duet in `fixtures/duet_fb_status.py`); `Processing` (mixer) when phase 4 needs it;
  - mute and volume helpers may stay as one-line wrappers.
- **VENDOR-DEPENDENT:** the new stack parses it once (`GeneralCommands.cpp:233` build, `:253` parse). The old stack
  has three separate implementations: `Audio/Protocols/Oxford/OxfwVendorDependent.hpp` (company ID + magic +
  code template, used by `Apogee/ApogeeVendorCodec.hpp` / `ApogeeTransport.cpp`), `MAudioSpecialCommand.hpp:53-54`
  (hand-built frame), and `AVCCommandFilter.hpp`'s own recognition. When they are replaced:
  - "company ID + optional magic + code" is a vendor-codec shape, not OXFW-specific (the Apogee Ensemble is BeBoB,
    no magic). It moves to `Extensions/<Vendor>/` on top of the generic parser.
  - **Keep the TASCAM quirk:** the unit echoes company ID `FF FF FF` instead of its own and doesn't answer with
    the spec's ACCEPTED (`OxfwVendorDependent.hpp:62-73`, Linux `tascam.rs:405-406`). The generic parser must not
    fail on a company-ID mismatch; the echo check is each vendor codec's choice.
- More are expected; the audit's job is to find them. List each finding and its fix in the commit.

**Review of the first 2b attempt (2026-09-28): wrappers, not a reshape. Redo.**
- Found in `GeneralCommands.hpp`: structs forward to the old free `Build…`/`Parse…` (two layers); `Encode(type)`
  ignores `type` (CONTROL silently builds STATUS); `Decode(Response&)` still passes an expected response code;
  "compatibility wrappers for tests" (a second path); `VendorDependentCommand` stores a `std::span` payload
  (dangles once queued; FW-60 pattern).
- **Target shape:** one generic `Command<Op>` (frame: ctype, address, opcode, padding; written once) + an
  `AvcOperands` type per command (`kOpcode`, `Reply`, `Write(writer, ctype)`, `static Read(span)`). ctype chosen
  at send time on `IAvcUnit`; response codes checked by the engine per ctype (optional per-type exception trait);
  unit-only addressing as a compile-time trait; operand types own their bytes; vendor commands are
  `VendorDependent<Payload>` (company-ID echo check = payload trait, TASCAM exception). Same shape as ta1394
  (`AvcOp` + `AvcStatus`/`AvcControl`) and Apple `IOFireWireAVCCommand`.
- Templates where a mistake should fail to compile (commands); a virtual interface where things swap at run time
  (the unit).
- **Timebox:** reshape the command families in use now; record other audit findings and fix them when their
  caller moves in 2c.
- Done when: no free `Build…`/`Parse…` in `Commands/`/`Extensions/`; every command is `Command<Op>`; no codec
  mentions a response code; no queued type holds a `span`; bytes unchanged.

## 2c. The unit model, then move every caller

4. **The unit model as plain data:** identity, the subunit list (type + id), and unit/subunit plug counts. It is
   filled from UNIT INFO / SUBUNIT INFO / PLUG INFO, the same commands this phase moves. It replaces the model part
   of `AVCUnit`.
   - **Not here (phase 4):** what's inside a subunit (music plugs, clusters, function blocks), descriptors, the
     discovery state machine.
5. **Move every caller onto the phase-1 codecs and the unit seam:**
   - BeBoB plug-0 discovery;
   - BeBoB protocol (signal format, function blocks);
   - Oxford stream formats;
   - Duet vendor commands;
   - AVCUnit / AVCDiscovery UNIT INFO / SUBUNIT INFO / PLUG INFO;
   - the user-client raw FCP path.

   The goldens must stay byte-identical. Any difference is declared in the commit and traced to a
   phase-1 differential-report entry.
6. **Delete:**
   - the duplicate command files (both signal-format copies, the name-clashing `StreamFormats/AVCSignalFormatCommand`,
     both stream-format copies);
   - BeBoB discovery's private frame building;
   - the dead `PCRSpace`, `AVCSignalFormatProbe` and `AVCAddress`, with their tests. (Apple's `IOFireWirePCRSpace`
     is a different concept, the Mac's own plugs; see `00-overview.md`.)
7. **Fix or delete the wrong `AVCDefs.hpp` values:** music `0x1C`, panel/bulletin board, the signal-format
   opcodes. Anything still needed moves to `Core/AvcTypes.hpp`.

**Done when:**
- no code outside `Core/`, `Commands/`, `Extensions/` builds an AV/C frame;
- every caller reaches the device through `IAvcUnit`;
- `AVCUnit`'s model is the new plain-data unit;
- goldens are unchanged or the difference is declared;
- the old files are gone.
