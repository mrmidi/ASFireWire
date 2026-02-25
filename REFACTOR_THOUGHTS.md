# REFACTOR_THOUGHTS.md
# Audio Constants & Config File Consolidation

## Status: proposal — no code changed

---

## 1. What Prompted This

During the S5028 macro refactor session (2026-02-25), three `#define` guards were
replaced with proper `constexpr` constructs in `TxBufferProfiles.hpp`,
`AudioRingBuffer.hpp`, and `StreamProcessor.hpp`.

While verifying those changes, two separate problems surfaced:

1. A compile-time limit (`kMaxSupportedChannels = 16`) is duplicated in three files
   with inconsistent names, lives inside a class that shouldn't own it, and is
   numerically wrong for high-channel-count devices.

2. The `Isoch/Config/` directory contains files whose names do not reveal their
   audio-specific purpose, and the RX profile file still carries the old macro
   pattern that was just fixed in its TX sibling.

These are independent of each other but share a common fix: one well-named header
per concern, placed where readers expect to find it.

---

## 2. Problem Inventory

### 2.1 — The "16 channel" cap is wrong and in the wrong place

**Current state:**

| Location | Symbol | Value | Scope |
|----------|--------|-------|-------|
| `Encoding/PacketAssembler.hpp:38` | `kMaxSupportedChannels` | 16 | namespace `ASFW::Encoding` |
| `Encoding/AudioRingBuffer.hpp:44` | `kMaxSupportedChannels` | 16 | class `AudioRingBuffer<>` |
| `Receive/StreamProcessor.hpp:28` | `kMaxSupportedPcmChannels` | 16 | class `StreamProcessor` |

Three independent definitions, three different names, same magic number, no shared
reference between them. A future change to the cap requires touching all three files
and knowing they're related.

**Why 16 is wrong:**

The limit was chosen thinking "audio channels", but AMDTP DBS (data block size) is
the actual wire concept — it counts all AM824 slots: PCM audio *plus* MIDI *plus*
control data. PCM channel count is bounded above by DBS, not the other way around.

Real FireWire audio devices that exceed 16 PCM channels:
- MOTU 828mk3 — up to 28 channels
- RME Fireface 800 — 28 channels
- Focusrite Saffire PRO 40 — 20 channels
- Presonus FireStudio Project — 20 channels

The driver already has `kMaxSupportedAm824Slots = 32` in `PacketAssembler` and
`StreamProcessor` for the wire-level DBS cap. A device with DBS=20 (18 PCM + 2 MIDI)
would pass the DBS check, then silently truncate to 16 PCM channels in the ring
buffer. The truncation is currently silent — no assertion, no log.

**Why 16 is in the wrong place:**

`AudioRingBuffer` is a generic lock-free SPSC ring buffer. It has no business knowing
about AMDTP format limits. Its `kMaxSupportedChannels` leaks domain knowledge into
a utility class. The cap belongs in the layer that knows about audio format
constraints — i.e., the same place `kMaxSupportedAm824Slots` lives.

### 2.2 — `kMaxSupportedAm824Slots = 32` is also duplicated

| Location | Symbol | Value |
|----------|--------|-------|
| `Encoding/PacketAssembler.hpp:41` | `kMaxSupportedAm824Slots` | 32 |
| `Receive/StreamProcessor.hpp:30` | `kMaxSupportedAm824Slots` | 32 |

TX and RX each define the same wire cap independently. The invariant
`kMaxSupportedPcmChannels ≤ kMaxSupportedAm824Slots` is stated in a comment on the
TX side but not enforced anywhere by `static_assert`.

### 2.3 — `TxBufferProfiles.hpp` name does not reveal audio scope

The file contains audio isochronous TX DMA tuning parameters: startup wait targets,
ring buffer sizing, safety offsets, prime data packet counts. Every field is
audio-specific. Nothing in the name indicates this — `TxBufferProfiles` could just
as well describe a network driver or a storage controller.

Same problem for `RxBufferProfiles.hpp`.

### 2.4 — `RxBufferProfiles.hpp` still has the old S5028 macro pattern

The TX file was refactored (2026-02-25) to replace `#define ASFW_TX_PROFILE_A/B/C`
with an `enum class` and a `constexpr` selection function. The RX file was not
touched and still carries the identical pre-refactor pattern:

```cpp
#define ASFW_RX_PROFILE_A 1
#define ASFW_RX_PROFILE_B 2
#define ASFW_RX_PROFILE_C 3
#ifndef ASFW_RX_TUNING_PROFILE
#define ASFW_RX_TUNING_PROFILE ASFW_RX_PROFILE_B
#endif
// ...
#if ASFW_RX_TUNING_PROFILE == ASFW_RX_PROFILE_A
inline constexpr RxBufferProfile kRxBufferProfile = kRxProfileA;
#elif ...
#error "Invalid ASFW_RX_TUNING_PROFILE value."
#endif
```

SonarQube will flag this as three new S5028 violations the next time the RX path is
analyzed. The fix is the same enum-class + `SelectRxProfile()` pattern applied to TX.

---

## 3. Proposed File Structure

All changes are within `ASFWDriver/Isoch/Config/`.

### 3.1 — New file: `AudioConstants.hpp`

Single source of truth for AMDTP format limits. No dependencies — zero includes
beyond `<cstdint>`.

```
ASFWDriver/Isoch/Config/AudioConstants.hpp
```

Contents:

```cpp
/// Maximum AM824 slots per isochronous data block (CIP DBS).
/// This is the wire-level container size: PCM audio + MIDI + control slots combined.
/// IEC 61883-6 / AMDTP allows up to 255 but practical FireWire audio devices cap at 32.
inline constexpr uint32_t kMaxAmdtpDbs = 32;

/// Maximum PCM audio channels the driver can handle (host-facing, CoreAudio side).
/// Must be ≤ kMaxAmdtpDbs because PCM channels occupy a subset of AM824 DBS slots.
inline constexpr uint32_t kMaxPcmChannels = kMaxAmdtpDbs;

static_assert(kMaxPcmChannels <= kMaxAmdtpDbs,
              "PCM channel cap cannot exceed AMDTP DBS — PCM slots are a subset of DBS");
```

**Why `kMaxPcmChannels == kMaxAmdtpDbs`:**

We set them equal because we do not currently know, at compile time, what fraction
of any device's DBS slots carry PCM vs MIDI. Setting PCM max = DBS max is safe and
conservative — it prevents truncation. At runtime, `IsochAudioTxPipeline` clamps to
the actual queue channel count, and `StreamProcessor` clamps to the actual wire DBS.
The compile-time arrays are worst-case sized.

Memory cost of raising PCM cap from 16 → 32:
- `AudioRingBuffer<4096>`: `4096 × 32 × 4 = 512 KB` (was 256 KB). Static, allocated
  once per audio engine instance. Acceptable in a DriverKit dext.
- `StreamProcessor::eventSamples_[32]`: 128 bytes on the class (was 64). Negligible.
- Stack buffers in `IsochAudioTxPipeline` (three sites): `256 × 32 × 4 = 32 KB`
  each (was 16 KB). Within normal stack budgets for non-interrupt context.

### 3.2 — Rename: `TxBufferProfiles.hpp` → `AudioTxProfiles.hpp`

No content changes beyond the `#include` path update in consumers. The namespace
`ASFW::Isoch::Config` stays the same.

Rename surfaces the audio-specific nature of the file immediately. The current name
reads like a DMA ring buffer configuration for any protocol; the new name is
unambiguous.

### 3.3 — Rename + refactor: `RxBufferProfiles.hpp` → `AudioRxProfiles.hpp`

Same rename rationale as TX.

Additionally, apply the same S5028 macro fix that was applied to `TxBufferProfiles`:

- Replace `#define ASFW_RX_PROFILE_A/B/C` with `enum class RxProfileId : uint8_t`
- Replace `ASFW_RX_TUNING_PROFILE` default from `ASFW_RX_PROFILE_B` to `1` (integer)
- Add `static_assert(ASFW_RX_TUNING_PROFILE <= 2, ...)`
- Replace `#if/#elif/#error` selection block with `SelectRxProfile(RxProfileId)`
- Add `gActiveRxProfile` / `GetActiveRxProfile()` / `SetActiveRxProfile()` — mirrors
  the runtime toggle infrastructure added to the TX side

---

## 4. Consumer Changes After the Refactor

### `Encoding/PacketAssembler.hpp`
- Remove: `constexpr uint32_t kMaxSupportedChannels = 16`
- Remove: `constexpr uint32_t kMaxSupportedAm824Slots = 32`
- Add: `#include "../Config/AudioConstants.hpp"`
- Replace all uses: `kMaxSupportedChannels` → `kMaxPcmChannels`,
  `kMaxSupportedAm824Slots` → `kMaxAmdtpDbs`

### `Encoding/AudioRingBuffer.hpp`
- Remove: `static constexpr uint32_t kMaxSupportedChannels = 16` (class member)
- Add: `#include "../Config/AudioConstants.hpp"`
- `kMaxTotalSamples` becomes `FrameCount * kMaxPcmChannels` (or `kMaxAmdtpDbs` —
  same value, but see open question §5.1)

### `Receive/StreamProcessor.hpp`
- Remove: `static constexpr size_t kMaxSupportedPcmChannels = 16`
- Remove: `static constexpr size_t kMaxSupportedAm824Slots = 32`
- Add: `#include "../Config/AudioConstants.hpp"`
- Replace: `kMaxSupportedPcmChannels` → `kMaxPcmChannels`,
  `kMaxSupportedAm824Slots` → `kMaxAmdtpDbs`

### `Transmit/IsochAudioTxPipeline.cpp`
- `Encoding::kMaxSupportedChannels` → `Config::kMaxPcmChannels`
  (three stack buffer sites + one validation site)
- `Encoding::kMaxSupportedAm824Slots` → `Config::kMaxAmdtpDbs`

### `Transmit/IsochTxVerifier.hpp` / `.cpp`
- `Encoding::kMaxSupportedAm824Slots` → `Config::kMaxAmdtpDbs`

### `Audio/ASFWAudioDriver.cpp`
- `ASFW::Encoding::kMaxSupportedChannels` → `ASFW::Isoch::Config::kMaxPcmChannels`

### `tools/calc_buffer_sizes.py`
- No changes needed for the constant refactor (it parses TX profile values, not
  channel counts). Already updated in the S5028 session for the TX macro rewrite.
- Will need the RX profile include path updated if it parses `RxBufferProfiles.hpp`.

---

## 5. Open Questions

### 5.1 — Should `AudioRingBuffer` use `kMaxPcmChannels` or `kMaxAmdtpDbs`?

They are equal (`kMaxPcmChannels = kMaxAmdtpDbs = 32`), so numerically it makes no
difference. The semantic question is: what does the ring buffer max represent?

**Option A — use `kMaxPcmChannels`:**
Communicates "this buffer holds PCM audio, bounded by the PCM channel cap". More
accurate for the ring buffer's role (it receives PCM from CoreAudio, not AM824 quads).

**Option B — use `kMaxAmdtpDbs`:**
Communicates "this buffer is sized for the worst-case AMDTP container". Defensive
against a future where PCM channels are derived from DBS differently.

Recommendation: **Option A** (`kMaxPcmChannels`). The ring buffer is a PCM buffer.
The fact that `kMaxPcmChannels` is currently equal to `kMaxAmdtpDbs` is an
implementation detail. If we ever decide PCM max should be less than DBS max (e.g.,
cap at 24 to limit stack usage), Option A gives a clean override point.

### 5.2 — Should `kMaxAmdtpDbs = 32` be raised?

IEC 61883-6 allows DBS up to 255. The value 32 was chosen pragmatically — it covers
all known consumer/pro FireWire audio devices. DICE-II (the common chip in MOTU,
TC Electronic, etc.) is internally limited to 32 DBS.

A value of 64 would future-proof for hypothetical multi-stream aggregation but would
double stack buffer sizes again. Leave at 32 until a device demands more.

### 5.3 — `kSamplesPerDataPacket = 8` in `PacketAssembler.hpp`

This is also an AMDTP-specific constant (48 kHz blocking mode: 8 audio samples per
125 µs isochronous cycle). It is only referenced within `PacketAssembler` and its
consumers. It is NOT a general audio constant. It should stay in `PacketAssembler.hpp`
(or move to `AudioConstants.hpp` only if a second consumer appears). Leave for now.

---

## 6. Implementation Order

The changes are low-risk (constant renames + file renames + one value change 16→32)
but touch many files. Suggested sequence to keep each step buildable:

1. **Create `AudioConstants.hpp`** — new file, no consumers yet, zero breakage.

2. **Update `PacketAssembler.hpp`** — add include, replace the two namespace-level
   constants with references to `AudioConstants.hpp`. Add `static_assert` that
   `kMaxPcmChannels <= kMaxAmdtpDbs`. Build + test.

3. **Update `AudioRingBuffer.hpp`** — remove class-level constant, include
   `AudioConstants.hpp`, update `kMaxTotalSamples`. Build + test.
   *(This is the step that changes the buffer size from 16→32 — watch for stack
   usage changes in `IsochAudioTxPipeline` once it's updated.)*

4. **Update `StreamProcessor.hpp`** — remove class-level constants, include
   `AudioConstants.hpp`. Build + test.

5. **Update `IsochAudioTxPipeline.cpp`, `IsochTxVerifier.*`, `ASFWAudioDriver.cpp`**
   — mechanical reference updates. Build + test.

6. **Rename `TxBufferProfiles.hpp` → `AudioTxProfiles.hpp`** — update all `#include`
   paths. Build + test. Confirm `calc_buffer_sizes.py` parses the new path.

7. **Rename + refactor `RxBufferProfiles.hpp` → `AudioRxProfiles.hpp`** — rename,
   apply the same S5028 enum-class + SelectRxProfile fix. Update all `#include`
   paths. Build + test.

8. **Run full test suite**: `./build.sh --test-only` — all 306 tests must pass.

9. **Run SonarQube scan** — confirm zero new S5028 issues in the touched files.

---

## 7. What This Does NOT Change

- No logic changes anywhere. This is purely naming, placement, and the 16→32 cap.
- `kTxBufferProfile` and `kRxBufferProfile` remain `constexpr` — all existing callers
  compile unchanged.
- The runtime toggle API (`GetActiveTxProfile`, `SetActiveTxProfile`) added in the
  previous session is unaffected.
- The TX profile macro `ASFW_TX_TUNING_PROFILE` remains a plain integer (0/1/2) as
  set in the S5028 session.
- `calc_buffer_sizes.py` profile-selection logic is already updated (S5028 session).
  Only the `#include` path may need updating for the file rename.

---

## 8. Risk Assessment

| Change | Risk | Mitigation |
|--------|------|------------|
| 16→32 cap | Stack size doubles at 3 sites in TxPipeline | Sizes are 32 KB each — acceptable; verify no stack overflow in DriverKit |
| File renames | All `#include` paths must be updated | Grep-verifiable; compile catches misses |
| RxProfile macro refactor | Same as TX refactor — proven pattern | Copy TX approach exactly |
| Constant renames | Mechanical — compiler enforces | Any missed reference is a build error |

All changes are trivially reversible (no data format or protocol changes).
