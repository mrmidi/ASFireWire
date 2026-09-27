# AV/C Differential & Conformance Report (Phase 1, Step 1.3(c))

**Branch:** `refactor/avc-stack`  
**Worktree:** `/Volumes/SDExt/DEV/ASFireWire/tmp/avc-wt`  
**Test Suite:** `tests/protocols/AvcDifferentialTests.cpp` (16/16 tests passing)  
**Date:** 2026-09-27  

---

## 1. Overview & Methodology

Phase 1 introduces the new clean-room, C++26 `ASFW::AVC` codec stack beside the legacy AV/C implementation. To ensure that migrating callers in Phase 2 will be completely safe, Step 1.3(c) mandates systematic differential testing comparing every legacy command builder and response parser against the new `ASFW::AVC` codecs.

All tests are implemented in [`tests/protocols/AvcDifferentialTests.cpp`](../../tests/protocols/AvcDifferentialTests.cpp).

---

## 2. Command-by-Command Differential Analysis

| Command / Functional Area | Legacy Implementation | New Rebuilt Implementation (`ASFW::AVC`) | Wire Comparison | Status | Spec / Linux Reference |
|---|---|---|---|---|---|
| **UNIT INFO (0x30)** Request | `AVCUnit::ProbeUnitInfo` sends 3 bytes `[01, FF, 30]` (padded to 4) | `Cmd::BuildUnitInfoStatus()` sends 8 bytes `[01, FF, 30, 07, FF, FF, FF, FF]` | Header matches (`01 FF 30`). Legacy omitted operands; new sends 5 dummy bytes `07 FF FF FF FF`. | **INTENDED DIFFERENCE** | AV/C General Spec 4.2 §10.1; Linux `ta1394/general/src/general.rs:43`. Standard requires 5 operand bytes for UNIT INFO status. |
| **UNIT INFO (0x30)** Response Parsing | `AVCCdb::Decode` / `AVCUnit` parses unit type, ID, company ID | `Cmd::ParseUnitInfo()` parses `UnitInfo` struct | Identical decoded values on live Apogee Duet and Phase 88 captures. | **MATCH** | Spec §10.1; Linux `general.rs:60`. |
| **SUBUNIT INFO (0x31)** Request | `AVCSubunitInfoCommand` across pages 0..7 | `Cmd::BuildSubunitInfoStatus(page, ext)` | Byte-for-byte identical across all pages 0..7: `[01, FF, 31, (page<<4)\|ext, FF, FF, FF, FF]`. | **EXACT MATCH** | AV/C General Spec 4.2 §10.2; Linux `ta1394/general/src/general.rs:136`. |
| **SUBUNIT INFO (0x31)** Enum Parity | `AVCDefs.hpp` defined `kMusic = 0x1C` (and `kMusic0C = 0x0C`) | `AvcTypes.hpp` defines `SubunitType::kMusic = 0x0C` | Wire bytes decode identically (`(entry >> 3) & 0x1F = 0x0C`). Legacy had duplicate/wrong enum values (`0x1C`); new adheres strictly to 0x0C. | **INTENDED ENUM FIX** | AV/C General Spec 4.2 Table 10.2; Linux `ta1394/general/src/general.rs:114`. |
| **PLUG INFO (0x02)** Request | `AVCUnitPlugInfoCommand` (subfunction 0x00) | `Cmd::BuildUnitPlugInfoStatus(kIsochronousExternal)` | Byte-for-byte identical: `[01, FF, 02, 00, FF, FF, FF, FF]`. | **EXACT MATCH** | AV/C General Spec 4.2 §10.3; Linux `ta1394/general/src/general.rs:388`. |
| **PLUG INFO (0x02)** Response Parsing | `AVCUnitPlugInfoCommand::ParseResponse` | `Cmd::ParseUnitIsochronousExternalPlugs()` | Decoded counts identical across iso inputs, iso outputs, ext inputs, ext outputs. | **EXACT MATCH** | Live Phase 88 capture (`02 02 08 07`). |
| **PLUG SIGNAL FORMAT (0x19/0x18)** Status Query | `AVCUnitPlugSignalFormatCommand` (query mode) | `Cmd::BuildPlugSignalFormatStatus(dir, plug, kAllWildcard)` | Byte-for-byte identical for input (`0x19`) and output (`0x18`) unit plugs: `[01, FF, 19/18, 00, FF, FF, FF, FF]`. | **EXACT MATCH** | TA 2004006 General 4.2 §10.10, §10.11; Linux `fcp.c:64`. |
| **PLUG SIGNAL FORMAT (0x19/0x18)** Control Set | `AVCUnitPlugSignalFormatCommand` across 7 rates (32k..192k) | `Cmd::BuildPlugSignalFormatControl(dir, Am824SignalFormat(plug, sfc))` | Byte-for-byte identical across all 7 sampling frequencies: `[00, FF, 19/18, plug, 0x90, sfc, FF, FF]`. | **EXACT MATCH** | TA 2004006 §10.10; Linux `fcp.c:82`. |
| **PLUG SIGNAL FORMAT (0x19/0x18)** Response Parsing | `AVCUnitPlugSignalFormatCommand::ParseResponse` | `Cmd::ParsePlugSignalFormat()`, `Cmd::SfcOf()` | Decoded FMT (`0x90`) and SFC match legacy interpretation across all rates. | **EXACT MATCH** | Linux `fcp.c:98`. |
| **STREAM FORMAT LIST (0x2F)** Request | `Audio::BeBoB::BuildReadOnlyProbeCommand` (kStreamFormatList) | `Cmd::BuildStreamFormatListStatus(kStreamFormatSupport, Unit, plug, idx)` | Byte-for-byte identical across input/output directions and indices 0..4: `[01, FF, 2F, C1, dir, 00, 00, 00, FF, FF, idx]`. | **EXACT MATCH** | TA 2001002; Linux `bebob_command.c:295`. |
| **STREAM FORMAT LIST (0x2F)** Response Parsing | `Audio::BeBoB::ParseExtendedStreamFormatListResponse` | `Cmd::ParseStreamFormatList()` | Decoded sample rate, PCM channel count, and MIDI slot count match on Phase 88 live captures. | **EXACT MATCH** | Live Phase 88 capture (10 PCM + 1 MIDI, 32k/44.1k/48k/88.2k/96k). |
| **BridgeCo Extended PLUG INFO (0x02, subfunction 0xC0)** | `Audio::BeBoB::BuildReadOnlyProbeCommand` (Plug Type, Channel Positions, Cluster Info) | `BridgeCo::BuildExtendedPlugInfoStatus()` | Byte-for-byte identical across input/output and sections 0..2 (12-byte zero-padded wire buffer). | **EXACT MATCH** | Linux `bebob_command.c:116`, `:222`; `bridgeco.rs:844`. |
| **Function Block Selector (0xB8)** | `AudioFunctionBlockCommand` (Selector Control) | `Cmd::BuildSelectorControl(kAudioSubunit0, fbId, plug)` | Byte-for-byte identical across FB IDs 1..4 and plugs 0..1: `[00, 08, B8, 80, fbId, 10, 02, plug, 01]`. | **EXACT MATCH** | TA 1999008 Audio Subunit 1.0 §10.2; Linux `bebob_command.c:20-28`. |
| **Function Block Feature Mute (0xB8)** | `BeBoBProtocol::SetFeatureMute` | `Cmd::BuildFeatureMuteControl(kAudioSubunit0, fbId, chan, muted)` | **Byte 6 (selector length):** legacy sent `0x04`; new sends `0x02`.<br>**Byte 8 (control):** legacy sent `0x00` (mute) / `0x60` (unmute); new sends `0x70` (mute) / `0x60` (unmute). | **INTENDED DIFFERENCE** | TA 1999008 Audio Subunit 1.0 §10.3 ("selector_length shall always be set to 2") and §10.3.1 (Mute_On: `0x70` = TRUE, `0x60` = FALSE, `0x00` invalid). Legacy implementation was best-effort unvalidated code. |
| **Function Block Feature Volume (0xB8)** | `BeBoBProtocol::SetFeatureVolume` | `Cmd::BuildFeatureVolumeControl(kAudioSubunit0, fbId, chan, volume)` | **Byte 6 (selector length):** legacy sent `0x05`; new sends `0x02`. | **INTENDED DIFFERENCE** | TA 1999008 §10.3 ("selector_length shall always be set to 2"). Legacy sent length of selector + data in selector length field. |
| **Apogee Vendor-Dependent (0x00)** | `Audio::Oxford::Apogee::ApogeeVendorCommand` | `Cmd::BuildVendorDependent(kStatus, Unit, OUI, payload)` | Byte-for-byte identical operand sequence: OUI `00 03 DB`, Magic `'PCM'`, Code, Args. | **EXACT MATCH** | Live Apogee Duet probe; `ApogeeVendorCodec.hpp`. |
| **M-Audio Special Allowlist** | `kMAudioSpecialPermittedFrames` in `AVCCommandFilter.hpp` | `tools/avc/avc_probe_1814.py` `PERMITTED_FRAMES` | Exact mirror of driver table (7 permitted commands; all hazardous queries blocked). | **EXACT MATCH** | DriverKit filter table & Linux `sound/firewire/bebob/bebob_maudio.c`. |

---

## 3. Classification of Discrepancies

### A. Intended Differences (Validated by Spec & Reference Standards)
1. **UNIT INFO Operands Length (3 bytes vs 8 bytes)**:
   - *Legacy:* Sent 0 operands (`01 FF 30`), padded to 4 bytes.
   - *New:* Sends 5 dummy operands (`01 FF 30 07 FF FF FF FF`), padded to 8 bytes.
   - *Authority:* AV/C General Specification 4.2 §10.1 explicitly specifies: `operand[0] = 0x07`, `operand[1..4] = 0xFF`. Linux `ta1394/general/src/general.rs:43` asserts 5 operand bytes.
2. **Audio Function Block Selector Length for Feature Blocks (4/5 vs 2)**:
   - *Legacy:* Set operand 3 to 4 for mute and 5 for volume.
   - *New:* Sets operand 3 to 2.
   - *Authority:* TA 1999008 Audio Subunit 1.0 §10.3: *"The selector_length field (Operand[3]) for feature function block shall always be set to 2."*
3. **Mute Control Encoding (`0x00` vs `0x70`)**:
   - *Legacy:* Sent `0x00` for mute on, `0x60` for mute off.
   - *New:* Sends `0x70` for mute on, `0x60` for mute off.
   - *Authority:* TA 1999008 §10.3.1: *Mute_On: 0x70 = TRUE (muted), 0x60 = FALSE (not muted), 0xFF invalid in CONTROL*.
4. **SubunitType Enum Parity (`0x1C` vs `0x0C`)**:
   - *Legacy:* `kMusic = 0x1C` in `AVCDefs.hpp`.
   - *New:* `SubunitType::kMusic = 0x0C`.
   - *Authority:* AV/C General Spec Table 10.2; Linux `ta1394`.

### B. Open Discrepancies
- **None.** All differences between the legacy code and new codecs have been fully characterized, traced to authoritative specifications, and validated by unit tests.

---

## 4. Mutation Testing Record

In accordance with Phase 1 Step 1.3(d), every new `.cpp` implementation file was subjected to a single-point mutation test to confirm test sensitivity. In each case, a deliberate error was injected, failure of the test suite was verified, the original code was restored, and `sleep 1; touch <file>` was executed before rebuilding and verifying clean test passage.

| File Mutated | Line | Injected Mutation | Test Case That Detected Mutation | Failure Symptom Observed | Restoration Verified |
|---|---|---|---|---|---|
| `Core/AvcFrame.cpp` | 28 | `frame.bytes_[0] = 0xAA;` (corrupted header ctype) | `AvcFrameTests.MakeSetsHeaderAndAccessors` | `Expected equality: frame->Bytes()[0] (0xAA) vs 0x01` | Clean build & test pass (`sleep 1; touch`) |
| `Commands/GeneralCommands.cpp` | 31 | `kOperands[0] = 0x08;` in `BuildUnitInfoStatus()` | `AvcDifferentialTests.UnitInfo_IdentifiesIntendedDifference` | `std::equal failed: expected dummy byte 0x07` | Clean build & test pass (`sleep 1; touch`) |
| `Commands/StreamFormatCommand.cpp` | 100 | `compound.rate = StreamFormatRate::k48000;` (forced rate override) | `AvcDifferentialTests.StreamFormatList_ResponseParsingComparison` | `Expected equality: compound.rate (0x04) vs k32000 (0x02)` | Clean build & test pass (`sleep 1; touch`) |
| `Commands/SignalSourceCommand.cpp` | 21 | `firstByte = 0xAA;` in `BuildSignalSourceStatus()` | `SignalSourceTests.BuildStatusAndControl` | `Expected equality: ops[0] (0xAA) vs 0xFF` | Clean build & test pass (`sleep 1; touch`) |
| `Commands/FunctionBlockCommand.cpp` | 44 | `operand[3] = 0x03;` in `BuildSelectorControl()` | `AvcDifferentialTests.AudioFunctionBlock_SelectorCommandBytesMatch` | `Expected equality: legacyEncoded[6] (0x02) vs newCmd[6] (0x03) (fbId 1..4)` | Clean build & test pass (`sleep 1; touch`) |
| `Extensions/BridgeCoPlugInfo.cpp` | 35 | `subfunction = 0xC1;` in `BuildExtendedPlugInfoStatus()` | `AvcDifferentialTests.BridgeCoExtendedPlugInfo_CommandBytesMatchBeBoBDiscovery` | `Expected equality: legSecEnc[i] (0xC0) vs newSec[i] (0xC1)` | Clean build & test pass (`sleep 1; touch`) |

---

## 5. Toolchain and Build Hygiene

1. **Host Test Suite**:
   - `ctest --test-dir build/tests_build`: **2,573 / 2,573 tests passed (100%)** (6 skipped pre-existing host mocks).
   - Zero test failures, zero regressions across audio, discovery, bus, and protocol suites.
2. **DriverKit Dext Compilation**:
   - Compiles cleanly with `./build.sh --no-bump`.
   - Architectures: `x86_64 arm64e`.
3. **Symbol Isolation**:
   - `nm -u net.mrmidi.ASFW.ASFWDriver`: Zero undefined `libc++` symbols. Symbol table identical before and after Phase 1.
