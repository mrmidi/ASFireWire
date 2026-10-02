# Phase 1: Foundation (additive only)

Read `00-overview.md` first for the why. This file is the work order for phase 1.

**Scope in one line:** add the new AV/C codec (`ASFW::AVC`) beside the old code, test it byte-for-byte, move
the toolchain to C++26, and capture what real devices answer. **No existing caller changes and no behaviour
changes.** Phase 2 moves callers; phase 1 only makes that move safe.

Supervisor: Claude (Opus). The implementer is another agent. After each step, stop and report (see "Reporting").

---

## 0. Ground rules (non-negotiable)

- **Worktree:** `/Volumes/SDExt/DEV/ASFireWire/tmp/avc-wt`, branch `refactor/avc-stack` (off `main` `6b51d1a9`). The headers are commit `6d08861f`.
  Work only there. The main checkout (`rme` branch) holds the user's uncommitted work: do not touch it.
- **Never `git stash`** in this repo. Scratch files go in the repo's `./tmp`, never in `/tmp`.
- **Do not push.** Commit locally, one commit per step. The user decides on pushes and PRs.
- **Do not edit the headers** listed in §2 without asking the supervisor. They are the contract. If one is wrong
  or unimplementable, stop and report; don't work around it.
- **Do not touch the old AV/C code** (`ASFWDriver/Protocols/AVC/*` outside `Core/`, `Commands/`, `Extensions/`) or
  any caller. The one exception is step 1.1's build settings.
- **Clean-room:** references are behaviour only (CLAUDE.md "References are read-only"). Read them, cite
  `file:line`, write fresh C++. Never paste reference code. The Linux AV/C crate `ta1394` is MIT, but the rule is
  the same.
- **Wire truth order:** Linux (`ta1394`, kernel `sound/firewire`) > Apple (`IOFireWireAVC`, `AVCVideoServices`,
  `AppleFWAudio`) > specs. **Today's hardware-proven ASFW bytes beat all of them.** Where old ASFW code already
  sends a command that works on a device, the new codec must send the same bytes.
- **Never trust `build.sh`'s summary:** grep its output for `error:`.
- **After restoring a mutated file:** `sleep 1; touch <file>` before rebuilding (make compares mtimes at 1 s; a
  same-second restore leaves the mutated binary in place).
- **Specs:** `/Volumes/SDExt/DEV/FirWireDriver/1papers`. **Never read a whole PDF.** Run `pdftotext -layout` into
  `tmp/specs/` (three are already there: `avc-general-4.2.txt`, `avc-stream-format-info.txt`,
  `avc-music-subunit-1.0.txt`), grep, and read only the lines that answer the question.

## 1. References you will need

| What | Where |
|---|---|
| Linux AV/C (primary) | `references/alsa-userspace-control-protocols-impl/protocols/ta1394/{general,ccm,stream-format,audio}/src/*.rs` |
| BridgeCo extension | Linux `references/linux-sound-firewire-stack/firewire/bebob/bebob_command.c`, `bebob.h`, `bebob_stream.c`; `…/protocols/bebob/src/bridgeco.rs` |
| Linux FCP | `references/linux-sound-firewire-stack/firewire/fcp.c` |
| Apple | `references/IOFireWireAVC/`, `/Volumes/SDExt/DEV/FirWireDriver/AVCVideoServices-42/` |
| Local notes | `docs/BEBOB_BRIDGECO_REFERENCE.md`, committed `ASFWDriver/Protocols/AVC/AVC_DEVICE_HAZARDS.md` |
| MCP client | `.claude/skills/asfw-mcp-control-plane/scripts/asfw_mcp.py` (read the skill `SKILL.md` next to it) |

## 2. The contract: headers (written by the supervisor, already in the worktree)

All in `ASFWDriver/Protocols/AVC/`. Each header documents its byte layout with citations; read it fully before
implementing.

| Header | You implement |
|---|---|
| `Core/AvcTypes.hpp` | nothing (header-only) |
| `Core/AvcError.hpp` | nothing (header-only) |
| `Core/RateCodes.hpp` | nothing (header-only) |
| `Core/OperandPack.hpp` | nothing (header-only, C++26 structured-binding packs; self-tests are `static_assert`s) |
| `Core/AvcFrame.hpp` | `Core/AvcFrame.cpp`: `CommandFrame::Make`, `ParseResponse`, `ParseResponseFor` |
| `Commands/GeneralCommands.hpp` | `Commands/GeneralCommands.cpp` |
| `Commands/StreamFormatCommand.hpp` | `Commands/StreamFormatCommand.cpp` (incl. `PlugAddress::Decode`) |
| `Commands/SignalSourceCommand.hpp` | `Commands/SignalSourceCommand.cpp` |
| `Commands/FunctionBlockCommand.hpp` | `Commands/FunctionBlockCommand.cpp` |
| `Extensions/BridgeCoPlugInfo.hpp` | `Extensions/BridgeCoPlugInfo.cpp` |

Implementation rules:
- Everything `noexcept`; failures only through `Expected<T>`. No allocation, no logging, no DriverKit includes.
  These files must build unchanged in the dext and in host tests.
- Use `Pack::ToBytes` / `Pack::FromBytes` (OperandPack.hpp) for fixed runs of bytes instead of hand offsets where
  it fits: PLUG INFO counts, the plug signal format (plug, FMT, FDF), UNIT INFO fields.
- Parsers start with `OperandsIf(response, expected)` and ignore trailing bytes past the defined layout
  (quadlet padding).
- Builders reject what a command cannot carry with `kInvalidArgument` (e.g. a subunit address for UNIT INFO).
- XcodeGen compiles every `.cpp` under `ASFWDriver/` into the dext automatically, so the dext build is part of
  your verification. There is no `project.yml` source list to edit.

## 3. Steps

### Step 1.1: Toolchain to C++26

Xcode 27.0 (27A266a, Apple Clang `2100.3.34.2`) is installed. The supervisor verified that structured-binding
packs, pack indexing and `std::expected` compile for host and DriverKit (`arm64e`, `-fno-rtti -fno-exceptions`).

Change:
- `project.yml:36` (project base) and `project.yml:132` (ASFWDriver target) `CLANG_CXX_LANGUAGE_STANDARD` to
  `gnu++26`. Read the surrounding settings first: if the app/test targets inherit line 36 and a Swift or ObjC++
  target breaks, set the base back and change only the targets that compile our C++. Report which.
- `tests/CMakeLists.txt:8` (`CMAKE_CXX_STANDARD 26`) and `:92` (`cxx_std_26`); root `CMakeLists.txt:17`.
- Regenerate `compile_commands.json` (`./build.sh --commands`). The old one points at the removed
  `DriverKit25.5.sdk`.

Verify:
- the full host suite builds and passes (grep `error:`), with the same test count as before the change;
- `./build.sh --no-bump` builds the dext (grep `error:`), and it is still `arm64e`
  (`lipo -archs` on the built binary);
- `nm -u` on the dext binary: the list of undefined `libc++` symbols is identical before and after (save both
  lists to `tmp/avc-phase1/`);
- warnings: report any new warning class the C++26 switch introduces, and fix none of them in this step.

Commit: `build: compile the dext and host tests as C++26`.

### Step 1.2: Frame codec (`Core/AvcFrame.cpp`)

Tests: new file `tests/protocols/AvcCodecTests.cpp`, registered in `tests/protocols/CMakeLists.txt` with
`add_protocols_test(AvcCodecTests AvcCodecTests.cpp "${ASFW_DRIVER_DIR}/Protocols/AVC/Core/AvcFrame.cpp" …)`.
Add each new `.cpp` to this target as you write it.

Cases:
- **`Make`:**
  - header bytes;
  - operands up to exactly `kMaxOperandBytes` succeed, one more fails with `kFrameTooLong`;
  - an extended-type address fails with `kUnsupported`;
  - `WireBytes()` pads with zeros to a quadlet (3→4, 8→8, 11→12) and `Bytes()` does not.
- **`ParseResponse`:**
  - `< 3` bytes → `kFrameTooShort`; `> 512` → `kFrameTooLong`;
  - low nibble `< 8` → `kNotAResponse`; nonzero CTS nibble → `kNotAResponse`;
  - each response code maps to its enum.
- **`ParseResponseFor`:** address and opcode mismatch.
- **Real frames:** the three Phase 88 answers below parse to the expected code, address, opcode and operands.

### Step 1.3: Command codecs

One `.cpp` per header. Each command gets three kinds of test.

**(a) Reference vectors.**
- `ta1394` has Rust unit tests with literal byte arrays (e.g. `ccm/src/lib.rs:281`). Turn the relevant ones into
  C++ tests, citing the Rust test's `file:line`; the byte values are facts, and the test code is ours.
- Add spec-derived cases where a spec table gives bytes.

**(b) Hardware captures.** These frames come from the user's Phase 88, taken on 2026-09-27 with
`asfw_fcp_send_command` (node 1, generation 8):

```
cmd  01 FF 2F C1 00 00 00 00 FF FF 00
resp 0C FF 2F C1 00 00 00 00 FF FF 00 90 40 02 01 03 08 06 02 00 01 0D
     -> list entry 0 of input plug 0: compound AM824, 32 kHz (0x02), sync 0, rate control "don't care",
        entries 8 x MBLA (0x06), 2 x IEC 60958 (0x00), 1 x MIDI (0x0D)
        PcmChannels()=10, MidiChannels()=1, OnlyPcmAndMidi()=true
cmd  01 FF BF C1 00 00 00 00 FF FF 00      resp 08 FF BF C1 00 00 00 00 FF FF 00   (NOT IMPLEMENTED)
cmd  01 FF BF C0 00 00 00 00 FF FF         resp 08 FF BF C0 00 00 00 00 FF FF      (NOT IMPLEMENTED)
PLUG INFO unit (resp): 0C FF 02 00 02 02 08 07   -> iso in 2, iso out 2, ext in 8, ext out 7
```

The driver ring also recorded the Phase 88's full format list: 5 formations per direction, rate codes `02 03 04 0A
05` (32/44.1/48/88.2/96 kHz), each 10 PCM + 1 MIDI. Step 1.4 captures all of these as fixtures; until then use the
four frames above.

**(c) Differential tests against today's code.** This is what makes phase 2 safe. For every command old code
already builds, build the same request with the old code and the new, and `EXPECT_EQ` the bytes. Where an old
parser exists, feed both the same response bytes and compare the decoded values.

| Old (keep untouched, just call it) | New |
|---|---|
| `Protocols/AVC/AVCCommands.hpp` UNIT INFO / SUBUNIT INFO `BuildCdb` | `BuildUnitInfoStatus`, `BuildSubunitInfoStatus` |
| `Protocols/AVC/AVCUnitPlugInfoCommand.hpp` | `BuildUnitPlugInfoStatus` |
| `Protocols/AVC/StreamFormats/AVCUnitPlugSignalFormatCommand.hpp` (query and set) | `BuildPlugSignalFormatStatus` (`kAllWildcard`), `BuildPlugSignalFormatControl` + `Am824SignalFormat` |
| `Protocols/AVC/AVCSignalFormatCommand.hpp` (root) | same new functions |
| `Protocols/AVC/AVCStreamFormatCommand.hpp`, `StreamFormats/AVCStreamFormatCommands.hpp` | `BuildStreamFormat*` |
| `Protocols/AVC/AudioFunctionBlockCommand.{hpp,cpp}` (Phase 88 mixer; hardware-proven) | `BuildSelector*`, `BuildFeatureMute*`, `BuildFeatureVolume*` |
| `Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.cpp` `BuildReadOnlyProbeCommand` (every `ReadOnlyProbeCommand`) | `BuildUnitPlugInfoStatus`, `BridgeCo::BuildExtendedPlugInfoStatus`, `BuildStreamFormatListStatus` (opcode `0x2F`) |
| `Audio/Protocols/BeBoB/BeBoBPlug0StreamDiscovery.cpp` `ParseChannelPositionSections`, `ParseStreamFormation`, `ParseExtendedStreamFormat*Response` | `BridgeCo::ParseChannelPositions`, `ParseStreamFormatBlock`, `ParseStreamFormatList`/`Single` |
| Apogee Duet vendor-dependent frames (`Audio/Protocols/Oxford/Apogee/`) | `BuildVendorDependent` |

- **Opcode, address or ctype differences:** if the old bytes differ from the new, **stop and report**. Do not
  "fix" either side. A difference is either an old bug (phase 2 declares it) or a header mistake (the supervisor
  fixes it).
- **Known intentional difference:** the old `AVCDefs.hpp` enum values are wrong (music `0x1C`, panel `0x0A`,
  bulletin board `0x0B`, signal format `0xBF`/`0xFF`). New code uses the right ones. List every such case in the
  report.

**(d) Mutation check** per `.cpp`: change one byte or offset and confirm a test fails, then restore it (`sleep 1;
touch`). Record which mutations you ran.

Commit per header (`avc: <command> codec`), or one commit for step 1.3 if it stays small.

### Step 1.4: Capture tool and hardware support matrix

Goal: learn what each device actually answers, before any code relies on it. Read-only: STATUS (`0x01`) and
SPECIFIC INQUIRY (`0x02`) only, through MCP `asfw_fcp_send_command`, which refuses anything else. **No CONTROL
frames in phase 1** (OPEN DESCRIPTOR is CONTROL and belongs to phase 4).

**Tool:** `tools/avc/avc_probe.py`, Python 3, calling the MCP client script. Per run:
- **Before:** `health` must be `ready`. Read `summary` for the node id and generation. Refuse if the device is
  streaming (`asfw_get_audio_stream_health` → `streaming: false`).
- **Battery, in this order; stop at the first transport timeout** (that is a wedge; report it):
  - UNIT INFO;
  - SUBUNIT INFO pages 0..7, stopping at the first REJECTED / NOT IMPLEMENTED;
  - PLUG INFO: unit `00` and `01`; each subunit found;
  - INPUT and OUTPUT PLUG SIGNAL FORMAT for every unit iso plug, **both** query forms (`kAllWildcard`,
    `kAm824Wildcard`);
  - STREAM FORMAT with **both opcodes**, for every unit iso plug and every music-subunit plug:
    - single;
    - list entries 0.. until a non-STABLE answer (cap 32);
  - SIGNAL SOURCE STATUS, both first bytes (`0xFF`, `0x0F`), for each music-subunit sync destination plug;
  - BridgeCo extended PLUG INFO, **only if the unit answered `0x2F`**: every `InfoType` for unit iso plugs, and
    cluster info for section ids 1..N (N from channel positions);
  - FUNCTION BLOCK STATUS (selector current; feature mute and volume current/min/max) for fb ids 0..15 on the
    audio subunit, if one exists; stop per type at the first NOT IMPLEMENTED;
  - SPECIFIC INQUIRY of each CONTROL form above (signal format, stream format single, signal source, selector,
    feature). INQUIRY asks "would you accept this?" without doing it.
- **After:** `health` again; the same generation; PLUG INFO still answers.
- **Output:**
  - `documentation/fixtures/AVC/<device>.json`: every frame sent, every answer (hex), response code, time,
    node/generation, driver version (`asfw_get_driver_version`);
  - `documentation/fixtures/AVC/<device>.md`: a support matrix;
  - an exporter `tools/avc/export_cpp.py` that writes `tests/support/AvcDeviceImages.inc`, in the style of
    `tests/support/DiceDeviceImages.inc` (generated header comment, stable keys).

**Device order (user decision):**
1. **Apogee Duet**;
2. **TerraTec Phase 88**;
3. **M-Audio 1814, only after 1 and 2 are reviewed, and only with the user present.** The user accepts that it may
   freeze; the goal is to measure what works. Use the same battery, but stop at the first timeout and record it.

The user must connect each device. Ask the supervisor to coordinate: don't run the tool against a device the user
didn't hand you.

**Tests:** a fixture test (in `AvcCodecTests` or a separate `AvcFixtureTests`) parses **every** captured answer
with the new codecs. A STABLE answer must parse without error. NOT IMPLEMENTED / REJECTED must give
`kUnexpectedResponse` with that code.

Commit: `avc: hardware capture tool + Duet/Phase 88 fixtures`.

## 4. Definition of done (phase 1)

- [ ] C++26 everywhere; host suite green (same test count + the new tests); dext builds, `arm64e`; `nm -u` libc++
      list unchanged.
- [ ] Six `.cpp` files implemented; every header function covered by tests (a), (b) and (c) where applicable; a
      mutation check recorded per file.
- [ ] Differential report: every old/new byte difference listed, each marked **intended** (the spec-confirmed enum
      fixes) or **open** (for the supervisor to decide).
- [ ] Duet and Phase 88 fixtures and matrices committed; every captured answer parses in the fixture test.
- [ ] 1814 matrix: done with the user present, or explicitly deferred by the user.
- [ ] No old AV/C file and no caller modified (`git diff main --stat -- ASFWDriver/Protocols/AVC` shows only
      `Core/`, `Commands/`, `Extensions/`; `git diff main --stat -- ASFWDriver/Audio` is empty).

## 5. Reporting (after every step)

Send the supervisor:
- the commit hash;
- the test counts before and after;
- the exact commands you ran;
- anything that surprised you, especially any byte difference in (c) and any header you think is wrong, with the
  reference line that says so.

Short and factual. No "should work": say what ran and what it showed.
