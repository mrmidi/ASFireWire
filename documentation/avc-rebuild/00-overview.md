> Historical design and decisions. Current phase-4 implementation contract:
> [phase-4.md](phase-4.md). Plans and fixtures were moved into tracked documentation
> on 2026-10-02. The historical phase-1 stop instruction below is superseded.

# Plan: Rebuild the AV/C stack (then BeBoB on top of it)

## Deliverables of this planning step (user, 2026-09-27)
Another, cheaper agent writes the code under my supervision. My deliverables are these:
1. **Plan docs**, stored as-is under `documentation/avc-rebuild/`. `docs/` is effectively untracked; nothing is committed.
   - `00-overview.md`: this whole plan.
   - `phase-1.md` … `phase-6.md`: one file per phase. Phase 1 in full detail; the others as outlines, detailed when
     their turn comes.
2. **Phase-1 headers** (my job; the other agent writes the `.cpp` and tests).
   - Namespace `ASFW::AVC`, new and unused. Existing unprefixed names (`CommandType`, `ResponseCode`, `PlugInfo`,
     `StreamFormat`…) would collide while old and new coexist.
   - `ASFWDriver/Protocols/AVC/Core/`:
     - `AvcTypes.hpp`: ctype, response code, subunit type/address, opcodes;
     - `AvcError.hpp`;
     - `AvcFrame.hpp`: command frame build, response parse/match, quadlet padding;
     - `OperandPack.hpp`: C++26 structured-binding-pack helpers for fixed byte layouts.
   - `.../Commands/`:
     - `GeneralCommands.hpp`: UNIT INFO `0x30`, SUBUNIT INFO `0x31`, PLUG INFO `0x02`, INPUT/OUTPUT PLUG SIGNAL
       FORMAT `0x19`/`0x18`, VENDOR-DEPENDENT `0x00`;
     - `StreamFormatCommand.hpp`: `0x2F`/`0xBF`, C0/C1, plug address, compound AM824, typed rate;
     - `SignalSourceCommand.hpp`: `0x1A`;
     - `FunctionBlockCommand.hpp`: `0xB8`, selector/feature.
   - `.../Extensions/BridgeCoPlugInfo.hpp`: PLUG INFO C0.
   - Each declaration cites its layout source: `ta1394` (MIT) `file:line`, Linux `bebob_command.c`, the specs, Apple.
   - The headers compile standalone with `-std=c++26 -fsyntax-only`.
3. **Stop after phase 1.** Review the other agent's work, then detail the next phase.

Toolchain confirmed on 2026-09-27: Xcode 27.0 (27A266a), Apple Clang `2100.3.34.2`. Structured-binding packs, pack
indexing and `std::expected` compile with `-std=c++26` for the host and for DriverKit (`arm64e`, `-fno-rtti
-fno-exceptions`, `driverkit25.0`).

## Phases (the stages below, grouped)
- **Phase 1 — Foundation, additive only** (no caller changes, no behaviour change): T (C++26), the core codec + A1
  command codecs as NEW files with unit tests, and the A0 capture tool + hardware support matrix (Duet → Phase 88 →
  1814 last).
- **Phase 2** (reordered 2026-09-28):
  - **2a:** the `IAvcUnit` seam, `SimulatedAvcUnit`, goldens of the current code;
  - **2b:** API review and reshape of the phase-1 codecs (ctype at send time, response codes only in the engine,
    one command per spec family); bytes unchanged, no caller moves;
  - **2c:** the unit model as plain data (identity, subunits, plug counts; moved here from A3), then move every
    caller onto the phase-1 codecs and the seam, delete the duplicate/dead files, fix the `AVCDefs` values.
- **Phase 3:** transaction engine (A2).
- **Phase 4:** subunit contents + discovery + descriptors (the rest of A3, and A4): the discovery state machine, music
  and audio subunit contents, descriptors, the graph builder, publishers moving out, the AV/C Report tab. It
  builds on the phase-2c unit.
- **Phase 5:** BeBoB (B1, B2, B4).
- **Phase 6:** Oxford + M-Audio (B3).

## Context

The AV/C layer (`ASFWDriver/Protocols/AVC`, 12,614 lines) is old and grew by accretion. Its object shape is
right (unit, subunits, plugs, commands, one transport); its implementation is not.

**Evidence gathered on 2026-09-27**, from the specs, Apple's `AppleFWAudio` and `AVCVideoServices`, Linux, and the
user's Phase 88 via MCP:
- **Duplicated commands.**
  - Signal format `0x18`/`0x19` exists twice: `AVCSignalFormatCommand.hpp` and
    `StreamFormats/AVCUnitPlugSignalFormatCommand.hpp`.
  - A third class shares the name: `StreamFormats/AVCSignalFormatCommand.hpp`, for `0xA0`/`0xA1`.
  - Stream format exists twice: `AVCStreamFormatCommand.hpp` and `StreamFormats/AVCStreamFormatCommands.hpp`.
  - BeBoB discovery builds its own frames.
- **Unused by the driver.** `PCRSpace` (tests only), `AVCSignalFormatProbe` (tests only), `AVCAddress` (nothing).
- **Wrong definitions, spec-confirmed** (`AVCDefs.hpp`):
  - The signal-format opcodes are listed as `0xBF`/`0xFF`. They are `0x18`/`0x19`, unit only (TA 2004006 General 4.2
    §10.10–10.11).
  - Music is listed as `0x1C`, which is Vendor unique. Music is `0x0C`: Linux `ta1394/general/src/lib.rs:36-49`,
    Music Subunit 1.0 operand tables, and AppleFWAudio's SubUnit_Type 12 match.
  - Panel, Bulletin Board and Camera Storage are off by one: Linux `lib.rs:44-46` has `0x09`/`0x0a`/`0x0b`
    (General 4.2 Table 11 agrees).
- **Wrong rate table.** The generic BeBoB code (`GenericBeBoBProtocol.cpp`, `BeBoBProfile.cpp`) decodes stream-format
  rate codes with the CIP SFC table. The shared `StreamFormatTypes.hpp:59-70` is right, and agrees with Apple
  (`MusicSubunitController.cpp:1262-1299`) and Linux (`bebob_stream.c:38-46`, `oxfw-stream.c:32-39`).
- **Device policy inside discovery.** `AVCDiscovery.cpp` is 1,848 lines. About 450 of them publish specific devices:
  Duet prefetch, M-Audio, "BeBoB" (actually Phase 88 only), and two Onyx publishers.
- **The generic BeBoB path has never streamed.**
  - The factory passes an empty `DeviceModel{}`.
  - The protocol is created before discovery runs (ring records 7605 vs 7611–7653).
  - The publisher hardcodes 10+1 @ 48k.
- **Phase 88 on hardware.**
  - Plug-0 formations: 5 per direction, each 10 PCM + 1 MIDI, at 32/44.1/48/88.2/96 kHz. The PCM is
    8 MBLA + 2 IEC 60958.
  - The "current format" query is REJECTED.
  - It is `0x2F`-only: `0xBF` answers NOT IMPLEMENTED, and the device stays healthy.
- **Apple's model is one generic AV/C path.**
  - AppleFWAudio matches any music or audio subunit and carries no BridgeCo or Oxford code.
  - It tries `0xBF` first and falls back to `0x2F` on each call. AVCVideoServices remembers the fallback.
  - `LegacySupport` only chooses the host's isoch engine (`CheckDeviceForNuDCLSupport`).

**Goal.** A clean-room, modular, testable AV/C stack. It is general, with pluggable subunit modules: music and audio
first; tape and panel later, as in AVCVideoServices. BeBoB then shrinks to BridgeCo extras plus vendor hooks. Phase 88
moves onto the generic BeBoB protocol, and the Prism Orpheus becomes a catalog row.

**User decisions.**
- Rebuild **in place, per component**. Each stage deletes what it replaces, so no second copy lives on.
- **Rewrite FCPTransport too.** Its current tests plus new frame-level goldens are the bar.
- Clean-room: the references are behaviour only.
- Language: C++23, or C++26 once Xcode 27 lands (Stage T).

## References (behaviour only; cite section or `file:line`; never read whole PDFs)
**Linux is the golden reference.** The user doesn't have every spec (the extended stream format draft, among
others). Where a spec is missing or silent, Linux decides; Apple is the cross-check.
- **Linux AV/C, primary:** `references/alsa-userspace-control-protocols-impl/protocols/ta1394`.
  - Crates `general`, `audio`, `ccm`, `stream-format`; ~5.2k lines of Rust; MIT.
  - It has no AV/C descriptor code.
- **Linux descriptors and info blocks:** FFADO `src/libavc/descriptors/`, `musicsubunit/avc_descriptor_music.cpp`,
  `audiosubunit/avc_descriptor_audio.cpp`.
- **BridgeCo:** `alsa-userspace …/protocols/bebob/src/bridgeco.rs` (~2.6k lines, typed; LGPL) plus Linux
  `bebob_command.c`. Local notes: `docs/BEBOB_BRIDGECO_REFERENCE.md`. Committed rules:
  `ASFWDriver/Protocols/AVC/AVC_DEVICE_HAZARDS.md`.
- **Specs:** `/Volumes/SDExt/DEV/FirWireDriver/1papers`
  - 2004006 General 4.2; 1999008 Audio Subunit 1.0; 2001007 Music Subunit 1.0; 2001002 Stream Format Information;
    2002013 Descriptor 1.2; 1999045 Information Blocks; 61883-1.
  - Workflow: `pdftotext` into `tmp/specs/`, grep, then read the single page that answers the question.
- **`references/IOFireWireAVC`** (`IOFireWireAVCCommand.cpp`, `IOFireWireAVCUnit.cpp`, `IOFireWireAVCTargetSpace.cpp`):
  transport, unit model, reset ordering, target space.
- **`/Volumes/SDExt/DEV/FirWireDriver/AVCVideoServices-42`:** Apple's full user-space AV/C: music, tape and panel
  controllers, and the virtual (target-role) subunits.
- **`tmp/re/AppleFWAudio.x86_64.i64`** (idalib, `run_auto_analysis=false`): shipped audio behaviour.
- **Linux** `firewire/fcp.c`, `cmp.c`, `bebob/`, `oxfw/`; **FFADO** `src/libavc/` (layering), `src/bebob/`.

## Target architecture (inside `ASFWDriver/Protocols/AVC`)
1. **Frame codec.**
   - Contents: ctype/response enum, subunit address (5-bit type from the spec table + 3-bit id), opcode, operands.
   - `std::span` in, `std::expected<…, AvcError>` out, layouts pinned with `static_assert`.
2. **Commands.**
   - One small encode/decode type per spec command:
     - UNIT INFO, SUBUNIT INFO, PLUG INFO;
     - INPUT/OUTPUT PLUG SIGNAL FORMAT;
     - STREAM FORMAT (one codec, opcode `0x2F` or `0xBF` as a value; subfunctions C0 single, C1 list);
     - SIGNAL SOURCE `0x1A`, VENDOR-DEPENDENT `0x00`;
     - audio function blocks (feature, selector, processing).
   - BridgeCo EXTENDED PLUG INFO (`0x02`/C0 subfunctions) lives in its own BridgeCo namespace.
   - **Every rate field gets its own type:** `StreamFormatRate` (TA 2001002 codes) vs `CipSfc` (IEC 61883-6). The
     compiler refuses to mix them.
   - With C++26, codecs iterate an operand struct's fields via structured-binding packs (P1061 + P2662). Without it,
     each command spells out its fields by hand. The API is the same either way.
3. **Transaction engine** (FCPTransport rewrite).
   - One outstanding command per unit.
   - INTERIM and IN TRANSITION handling; generation/route tokens.
   - The allowlist is enforced at submit.
   - The stream-format opcode is per-unit data: fixed by the catalog, or learned once from a NOT IMPLEMENTED answer
     where the catalog allows it. Never a blind probe on each call.
   - Errors map to `IOReturn` in exactly one place.
   - The `FCPResponseRouter` contract is kept.
4. **Model and discovery.**
   - Unit → plugs, subunits → music subunit (plugs, clusters, formations) / audio subunit (function blocks).
   - Plain data, filled by a pure state machine driven by the engine and gated per unit by `ProbePolicyId`.
   - No `if (vendor)` in this layer.
5. **Subunit modules** register with the core: music and audio now; tape and panel later. The target role (the Mac as
   an AV/C device) stays possible but is not built.

**Outside this layer:**
- Device publishers move out of `AVCDiscovery` into the family/device code.
- CMP (IEC 61883-1, `CMP/CMPClient`) is not AV/C and is not rebuilt.
- DV/MPEG streaming is content framing (like CIP) and is not part of this plan.

## Stages (one commit each; full suite + dext build every stage; goldens unchanged unless declared)

**D0 — Inventory + design doc** (`documentation/AVC_STACK.md`). Every existing file/class gets:
- keep / rewrite / move out / delete;
- its callers;
- the spec section and reference behaviour it maps to;
- the line counts before, to measure against.

**T — Toolchain to C++26** (gated on Xcode 27, being installed).
- Switch the dext (`project.yml:132`), the project base (`project.yml:36`) and the tests (`tests/CMakeLists.txt:8,92`,
  root `CMakeLists.txt:17`).
- Verify:
  - the full suite and dext build pass, still `arm64e`;
  - `nm -u` shows no new libc++ imports;
  - the host tests still build against `HostDriverKitStubs` (the user's matrix saw a `<new>` clash only with
    MacOSX.sdk + DriverKit.h).
- What we would use: structured-binding packs, pack indexing, saturating arithmetic.
- What we must not rely on: P2996 reflection, contracts, `#embed` (all absent in Apple Clang 21, per
  `/Volumes/SDExt/DEV/reflection/README.md`).
- If Xcode 27 is late, A1 proceeds in C++23.

**A0 — Hardware command-support matrix + wire characterization** (no production change; the bar for everything
after).
- **Capture tool** (`tools/avc/`, like pydice):
  - runs a battery through MCP `asfw_fcp_send_command`: every read-only STATUS/INQUIRY form of every command we
    implement or plan to;
  - covers unit and subunit, general, music, audio, CCM, both stream-format opcodes, and descriptors/info blocks;
  - records each answer (response code + payload), writes a support-matrix report, and exports a C++ device image.
  - OPEN DESCRIPTOR for read is a CONTROL command: allowed, but it goes through the developer tool, and each run is
    logged.
- **Order, per user:**
  1. Duet (user has it);
  2. Phase 88;
  3. **only then the 1814**. The user accepts that it may freeze; the goal is to learn what works and what doesn't,
     and to replace today's assumed allowlist with measured data.

  Onyx-i: from its documented capture unless the device is available.
- **`SimulatedAvcUnit`** in `tests/support/`: answers FCP writes to `0xFFFFF0000B00` from a captured image. Move
  `RecordingFireWireBus` out of `tests/devices/DICEDuplexTestSupport.hpp:225` into `tests/support`.
- **Goldens** `tests/golden/avc/<device>__<scenario>.trace`: attach discovery, start/stop, bus reset mid-discovery,
  INTERIM, timeout, NOT IMPLEMENTED.

**A1 — Frame codec + commands.**
- New codec and command files.
- Switch every caller: BeBoB discovery's private frames, the signal-format and stream-format copies, the audio FB
  command.
- Delete the replaced files and the dead ones (`PCRSpace`, `AVCSignalFormatProbe`, `AVCAddress`).
- Fix the enum values from the specs.
- Tests: spec examples plus captured frames. Goldens unchanged.

**A2 — Transaction engine.**
- Rewrite FCPTransport against `IOFireWireAVCCommand.cpp` and Linux `fcp.c`. The per-unit opcode policy arrives here.
- Bar: `FCPTransportTests`, `FCPResponseRouterTests` and the A0 goldens.
- Delete the old transport.

**A3 — Model + discovery.** (2026-09-28: the unit model part moved to phase 2c.)
- Replace `AVCUnit`, `Subunit`, `MusicSubunit`, `AudioSubunit` and the generic part of `AVCDiscovery`.
- Move the device publishers out.
- Keep `UserClient/WireFormats/AVCWireFormats.hpp` byte-compatible through a serializer, so the app is unchanged.
- Goldens unchanged.

**A4 — Descriptors and information blocks** (2002013, 1999045). Main line, not deferred.
- Rewrite OPEN/READ/CLOSE and parse the music and audio subunit status descriptors and info blocks.
- Reference: FFADO `libavc` descriptors (Linux) first, then IOFireWireAVC and AVCVideoServices.
- Answers from Duet and Phase 88 captured in A0.
- Wire it to `GetSubunitDescriptor` and the MCP READ DESCRIPTOR tool, which today returns "not implemented".

**B1 — Generic BeBoB on the new layer.**
- Geometry and rates from discovery, per direction and per rate, with the right codes.
- Current rate from signal-format STATUS.
- The model delivered after discovery.
- A generic publisher; `emptyPacketsDuringIdle = true`.

**B2 — Phase 88 onto the generic BeBoB protocol.** The mixer unmute becomes a hook; delete `Phase88Protocol` and
`Phase88Profile`. Rate policy is decided by a hardware rate-switch test on the user's Phase 88.

**B3 — Oxford (Duet, Onyx-i) and M-Audio moved** onto the new layer. Goldens unchanged. The 1814's allowlist is
rebuilt from the A0 1814 matrix (measured) instead of today's assumptions.

**B4 — Prism Orpheus row.** Generic BeBoB plus an output-source hook: vendor-dependent `0xB1`=1, following the
start sequence in the fork `grandviewsound/ASFireWire-Orpheus` (clone in `tmp/orpheus-fork`). Marked not run on this
code.

**Later (not in this plan):** tape and panel modules, the target role, DV capture.

## Verification
- **Every stage:**
  - `ctest` on the full suite, grepping `error:` (never trust `build.sh`'s summary);
  - `./build.sh --no-bump` for the dext;
  - pydice tests;
  - `tests/golden/{avc,session,dice,dice-profiles}` unchanged unless declared in the commit.
- **New codecs:** mutation checks (break a field, expect a failure). After `git checkout` restores, run `sleep 1;
  touch` (make compares mtimes at 1 s).
- **Hardware, batched at the end of B.** Devices: Duet and Phase 88 (both on the user's desk), then the 1814.
  - Re-run the A0 matrix on each device and diff it against the A0 capture (same answers).
  - Attach, 48k play, rate switch where supported.
  - 1814: attach with only measured-safe frames (check the FCP ring);
  - query the driver ring (`asfw_log_query` FCP/AVC/Audio) and `asfw_fcp_get_recent_responses`.

## Apple's IOFireWireAVC as the concept map (user, 2026-09-28)
Concepts only, no code. `IOFireWireAVCCommand` → phases 1 + 3 (codec + transaction engine); `IOFireWireAVCUnit` →
phase 2 (the `IAvcUnit` seam, the plain-data unit model) and phase 4 (subunit contents, discovery, reset handling); `IOFireWireAVCTargetSpace`/`RequestSpace`/`LocalNode` → the Mac as
an AV/C target (out of scope, keep the door open); `IOFireWirePCRSpace` → the **Mac's own** PCRs, which remote
devices lock-write (allocate plug + callback on a successful lock). That is a different thing from our dead
`PCRSpace` (a client-side PCR helper that duplicates `CMP/`), so deleting ours stays correct. A local PCR space,
if we ever need one, belongs with CMP (IEC 61883-1), not the AV/C layer.

## Placement rule: vendor-dependent and device-specific code (user, 2026-09-28)
The question is "bytes, or knowledge about a device?"
- **Generic AV/C** (`Core/`, `Commands/`): includes VENDOR-DEPENDENT `0x00` as company ID + opaque payload.
- **Vendor codecs** (`Protocols/AVC/Extensions/<Vendor>/`, by company, never by model): typed encode/decode on top
  of the VENDOR-DEPENDENT codec. Bytes and cited sources only; no model names, no policy.
  `BridgeCo/` (exists), `Apogee/` (born in phase 2c when the Duet callers move), `MAudio/`.
- **Device knowledge** (device layer, outside AV/C): which vendor commands a model uses, what they mean, the
  `ParameterId` → command bindings, the safety list, quirks. This is the overlay on the discovered base. It reaches
  the device only through `IAvcUnit` + the typed codecs. No `if (model)` inside `Protocols/AVC`.
- **Non-AV/C vendor protocols** (DICE/TCAT extensions, Fireworks EFC, MOTU, RME registers) are sibling protocol
  families, not children of AV/C, with the same codec-vs-device split inside each.

### Example: Apogee has two command sets on one frame (Linux userspace, `protocols/`)
Both use VENDOR-DEPENDENT (`0x00`) to the **unit** address (`FF`) with the Apogee OUI `00 03 DB`. After that they
share nothing, so `Extensions/Apogee/` holds two codecs (Duet, Ensemble), each named after its command set:
- **Duet (OXFW)** `oxfw/src/apogee.rs`: payload starts with the signature `50 43 4D` ("PCM") (`:871`), then a
  command code (`MIC_POLARITY 0x00` … `OUT_MUTE 0x09`, `MIXER_SRC 0x10`, `OUT_VOLUME 0x15`, …) and arguments.
  **STATUS and CONTROL both work** (`:58`, `:85`; test frames at `:1324-1375`).
- **Ensemble (BeBoB)** `bebob/src/apogee/ensemble.rs`: no "PCM" signature; the command code comes right after the
  OUI (`0xE4`–`0xF6`, `HW 0xEB` with sub-codes, `HW_STATUS 0xFF`; `:1247-1263`, `:1581-1587`). **CONTROL only**
  (only `AvcControl` is implemented, `:1678`). State and meters are read with `HW_STATUS` as a CONTROL
  (`:1011`, `:1394-1395`), and the host caches everything else. **The payload is padded with `FF` to at least 6
  bytes (3 quadlets); the source comment says the unit freezes otherwise** (`:1683-1686`).
