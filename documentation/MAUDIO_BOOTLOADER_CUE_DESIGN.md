# M-Audio bootloader cue — subsystem design

Status: implemented. Scope is the FireWire 1814 / ProjectMix I/O bootloader
persona only. No audio, no adapter, no nub.

Wire authority, all three independently agreeing:

- `references/libffado-2.5.0/src/bebob/bebob_dl_codes.h:39-54` — command codes
- `references/libffado-2.5.0/src/bebob/bebob_dl_mgr.cpp:45-49` — address map
- `references/linux-sound-firewire-stack/firewire/bebob/bebob_maudio.c:41-49,119-121` — the cue quadlets
- `docs/MAUDIO_1814_KEXT_RE.md` §1 — vendor kext behaviour (local-only notes)

Operational rules live in `ASFWDriver/Protocols/AVC/AVC_DEVICE_HAZARDS.md`.

---

## 0. Naming

Reference stacks name these commands `eCmdC_Go`, `eSM_Application`, `eBPV_V1`.
That notation is not reproduced. ASFW uses descriptive identifiers and cites the
reference in a comment, per the house rule that spec and vendor names belong in
comments rather than identifiers — and because copying a reference's naming
scheme is the thin end of copying its code.

| this design | reference name | value |
|---|---|---|
| `StartApplicationFirmware` | `eCmdC_Go` | `0x11` |
| `ApplicationStartMode` | `eSM_Application` | `0` |
| `DebuggerStartMode` | `eSM_Debugger` | `2` |
| `BootloaderProtocolVersion` | `EBootloaderProtocolVersion` | from info+0x08 |

"Cue" is retained: it is the established local term for this specific
three-quadlet write and reads better than "start command".

## 1. What the cue is

One command: **start the application firmware** — bootloader command `0x11` with
start mode `0`. It tells a BeBoB bootloader to jump into the application
firmware. It writes nothing persistent. A malformed cue costs a power cycle,
nothing more.

```
AddrRegInfo  0xFFFF_C802_0000   read 80 bytes  — BootROM info block
AddrRegReq   0xFFFF_C802_1000   write 12 bytes — the cue
AddrRegResp  0xFFFF_C802_9000   firmware-download response register; not read by the start cue
```

```
quadlet[0] = protocol version    read live from info+0x08 (Linux hardcodes 1)
quadlet[1] = 0x01110000          carries command code 0x11, start application firmware
quadlet[2] = 0x00000000          operand: start mode, 0 = application (2 = debugger)
```

Written little-endian, matching Linux `cpu_to_le32` and the kext's deliberate
no-swap on x86.

`bootloaderActive = info[+0x0C] != 0`. That field is the BootROM "bootloader
version", **used as a state flag**. If it is zero the device is already running
application firmware and **no cue is sent**.

The normal start-cue path ends at the acknowledged write. It does **not** poll
`AddrRegResp`: Linux sends the cue and waits for the device's bus reset and fresh
detection (`references/linux-sound-firewire-stack/firewire/bebob/
bebob_maudio.c:87-92,119-134`); the original M-Audio kext's `FirmwareStart` does
the same, scheduling re-registration immediately after its write. Its
`FirmwareReadResponse` helper belongs to the firmware-download call path, not
normal application start.

## 2. Why this needs a guard, not just care

The cue's `0x11` is one opcode from `0x10`, which rewrites the hardware ID, and
shares its byte field with `0x0a`, which rewrites the GUID. The same 12-byte
write, with one byte wrong, permanently reprograms device identity in flash.

| code | what it does | reference name | effect |
|---|---|---|---|
| `0x04` `0x05` `0x06` | begin / send / end firmware download | `DownloadStart` / `Block` / `End` | rewrites firmware image |
| `0x0a` | write GUID to flash | `ProgramGUID` | rewrites EUI-64 in flash |
| `0x0b` | write MAC to flash | `ProgramMAC` | rewrites MAC |
| `0x0c` `0x0d` | reset persistent config | `InitPersParams` / `InitConfigToFactorySetting` | wipes persistent config |
| `0x0f` | write debug GUID | `SetDebugGUID` | overwrites identity |
| `0x10` | write hardware ID/version | `ProgramHWIdVersion` | rewrites hardware ID/version |
| **`0x11`** | **start application firmware** | **`Go`** | **the cue — benign** |

`ProgramGUID` existing in flash is very likely how a batch of ProFire 2626 units
shipped sharing one EUI-64. Treat every code in this table except `0x11` as
permanently destructive and unreachable by construction.

## 3. The guard — three independent interlocks

**(a) No opcode exists in the type system.** The cue is a closed value type with
no command parameter anywhere in its interface. `ProgramGUID` and friends are
not "unused constants" — they have *no representation in the driver*.

```cpp
class BeBoBBootloaderCue final {
public:
    static constexpr size_t kCueBytes = 12;

    // The only constructor. There is no opcode parameter, here or anywhere.
    explicit BeBoBBootloaderCue(uint32_t protocolVersionFromInfoBlock) noexcept;
    [[nodiscard]] std::span<const uint8_t, kCueBytes> Bytes() const noexcept;

private:
    // Bootloader command 0x11, start mode 0: start the application firmware.
    // Cross-validated with Linux bebob_maudio.c:41-49 (CUE1/CUE2/CUE3) and
    // libffado bebob_dl_codes.h:54,285-292.
    static constexpr uint32_t kStartApplicationFirmware = 0x01110000;
    static_assert((kStartApplicationFirmware & 0x00FF0000U) >> 16 == 0x11,
                  "command code must remain start-application-firmware; 0x10 "
                  "and 0x0a in this field permanently reprogram device identity");
    std::array<uint8_t, kCueBytes> bytes_{};
};
```

**(b) No generic write reaches the region.** The bootloader client exposes
exactly two operations — `ReadInfo()` and `SendCue()`. There is no parameterised
write. A single choke point validates that any write to
`0xFFFF_C802_xxxx` targets exactly `AddrRegReq`, is exactly 12 bytes, and has
quadlets [1] and [2] bit-identical to the frozen constants; quadlet [0] is the
only variable. Violations are a hard failure, not a log line.

**(c) Policy decides before the bus is touched.** The cue is attempted only for
catalog identities carrying an explicit cue policy:

```
.bootloaderCue = BootloaderCuePolicy::BeBoBStartFirmware   // 0x00010070 only
```

The three identities are not equivalent and should stop being treated as one
group. H1 is about AV/C commands reaching *operational firmware*; a bootloader is
not running that firmware and has no AV/C surface to freeze. Linux and FFADO both
write the cue to it as the normal path.

| identity | probe policy | cue policy | quarantine |
|---|---|---|---|
| `0x00010070` bootloader | `NoAutomaticTraffic` | `BeBoBStartFirmware` | none — it is a bootloader, not a hazardous AV/C target |
| `0x00010071` operational | `NoAutomaticTraffic` | `None` | `HazardousNoProbe` until a reviewed operation policy exists |
| `0x00010091` ProjectMix | `NoAutomaticTraffic` | `None` | `HazardousNoProbe`, as above |

Zero FCP is sent to any of the three. Splitting them this way removes the need
for a carve-out in the quarantine rule: the persona that gets prepared is simply
not the persona that is quarantined.

## 4. Placement

Family-local, under the BeBoB family that owns it:

```
ASFWDriver/Audio/Families/BeBoB/Bootloader/
    BeBoBBootloaderCue.{hpp,cpp}          the frozen 12-byte value type
    BeBoBBootloaderClient.{hpp,cpp}       ReadInfo / SendCue + choke point
    BeBoBBootloaderPreparation.{hpp,cpp}  the state machine
```

Mechanism and policy split the way the reference stacks split them: FFADO keeps
`bebob_dl_mgr` / `bebob_dl_codes` at `src/bebob/` with `maudio/`, `focusrite/`,
`terratec/` as sibling vendor directories. The BridgeCo bootloader protocol is
**BeBoB-generic**; what is M-Audio-specific is shipping a bootloader persona that
needs cueing at all. So the protocol lives in the BeBoB family and the identities
that get cued live in the catalog — no vendor branch anywhere in between.

A top-level `Firmware/` peer of `Discovery/` was considered and rejected: it
would advertise a generality that does not exist, and it would put device-family
policy outside the family that owns it, against the normalization pass's own
acceptance criterion.

## 5. State machine

Preparation is a session lifecycle stage, not a separate subsystem. The existing
`AudioSessionState` gains one state ahead of `StaticResolved`, and the BeBoB
family provider owns it. A device whose static plan carries a cue policy enters
`Preparing` instead of `Probing` and never reaches `Ready`:

```
Observed(0x00010070, cue policy = BeBoBStartFirmware)
  → Preparing
      ReadingInfo        read 80 B @ AddrRegInfo
      EvaluatingInfo     bootloaderActive = info[+0x0C] != 0
             if inactive → Retired(NoCueNeeded)
      SendCue            write the frozen 12 B ONCE, version from info+0x08
      AwaitingReenumeration
                         no more bus traffic in this generation
  → device resets; the Preparing session is retired on suspension
  → new generation, current operational persona → the ordinary path
```

The session is deliberately terminal: preparation never becomes an audio
endpoint. `AwaitingReenumeration` holds the old-generation gate only long enough
to prevent a duplicate cue; reset suspension removes it before the resumed
device is resolved from its current Config ROM identity (§6).

### The states are a `std::variant`, not an enum plus flags

```cpp
struct ReadingInfo      { uint8_t attempt{0}; };
struct EvaluatingInfo   { BootRomInfo info; };            // owns the only parsed block
struct AwaitingReenumeration {
    uint32_t cuedProtocolVersion;                          // what we actually sent
};
struct Retired          { RetireReason reason; };

using PreparationState = std::variant<ReadingInfo, EvaluatingInfo,
                                      AwaitingReenumeration, Retired>;
```

This is not stylistic. Two of the safety rules become type invariants instead of
checks someone can forget:

- **The cue's protocol version cannot be stale or defaulted.** `SendCue` is
  reachable only from `EvaluatingInfo`, the sole state that owns a `BootRomInfo`.
  There is no member holding a "last known" version for a later state to reuse.
- **"Cue already sent" is not a bool.** `AwaitingReenumeration` *is* the
  post-cue state. The once-only rule of §5 is expressed by the state's existence,
  so no callback can re-send by mistaking a flag; re-sending requires a fresh
  `EvaluatingInfo` in a new generation.

`ReadingInfo` alone carries a retry counter, so the bounded pre-cue retry budget
cannot leak into the post-cue state.

**The cue is written at most once per bootloader-active observation.** A failed
write is never retried in the same generation: an ambiguous failure may have
landed and the device may already be mid-boot. An acknowledged cue then leaves
the session inert until the reset is observed.

## 6. Correlation across the reset — deliberately not load-bearing

The device can return with a different model ID while retaining its
`DeviceInstanceId`; its observed GUID is not reliable enough to use as a required
correlation key. The session manager therefore removes a `Preparing` session in
`OnDeviceSuspended`, before `OnDeviceResumed` resolves the current registry record.
This prevents the old unit-to-endpoint mapping from masking the operational
persona after a successful cue.

Correctness comes from the current device state instead: the info-block read is
authoritative, and a device already running firmware reports
`bootloaderActive == 0` and is never cued. Re-running resolution for the resumed
record is therefore safe and idempotent.

Correlation is best-effort: match on observed GUID when it is non-zero and unique
within the generation; otherwise record an expiring "cue in flight" marker purely
for logging. Never gate an action on it.

## 7. Observability

Add `LogCategory::Firmware` as a new frozen ID (currently `Oxfw = 22`,
`Count = 23`, so `Firmware = 23`, `Count = 24`), with its catalog entry, its
`os_log` accessor, and the matching append to `ASFWLogRingCategories` in the app.
IDs are frozen and positional across the dext/app boundary — append only, never
renumber.

This is a rare, cold path, so log every state transition, not anomalies only:
`[Bootloader] instance=… persona=… bootloaderActive=… action=…`. The hot-path
suppression rule does not apply here.

## 8. Tests

- Cue bytes are exactly Linux `CUE1/CUE2/CUE3` for a given protocol version.
- The write-choke-point rejects: wrong address in range, wrong length, any
  mutation of quadlet [1] or [2].
- State machine: inactive bootloader sends nothing; cue is written exactly once;
  an acknowledged cue reaches `AwaitingReenumeration` with no response-register
  transaction; a bus reset retires the old mapping before the resumed persona is
  resolved.
- Architectural test: the constants `0x04`–`0x10` from the command table appear
  nowhere in `ASFWDriver/` as bootloader command codes.

## 9. Everything else in the command space

Two tiers, and the difference is whether the damage is recoverable.

**Never — destroys identity or the firmware image.** These have no legitimate
use for us at any point. Absent, not disabled.

| code | command | why never |
|---|---|---|
| `0x0a` | `ProgramGUID` | destroys the unique EUI-64; unrecoverable |
| `0x0b` | `ProgramMAC` | same class |
| `0x0f` | `SetDebugGUID` | same class |
| `0x10` | `ProgramHWIdVersion` | rewrites the ID the catalog matches on |
| `0x04` `0x05` `0x06` | `DownloadStart` / `Block` / `End` | rewrites the firmware image; shipping a firmware updater is a separate decision of much larger scope, not an incremental addition |

**Not in v1 — potentially useful, add deliberately.** These are recoverable and
some are plainly wanted later.

| code | command | note |
|---|---|---|
| `0x0d` | `InitConfigToFactorySetting` | a real user-facing "reset to factory settings" feature. Before reaching for the bootloader path, check whether the kext's `ResetToFactorySettings` @ `0xd190` is in fact an AV/C vendor command on the live firmware — the 1814's operational AV/C surface does include vendor commands, and doing it there would not require bootloader mode at all |
| `0x0c` | `InitPersParams` | same family, narrower |
| `0x03` | `ReadImageCRC` | read-only; useful for reporting installed firmware |
| `0x01` `0x02` | `Halt` / `Reset` | recoverable by power cycle |
| `0x07`–`0x09` | 1394 shell | diagnostic curiosity, no known need |

**How to add one safely.** The interlocks in §3 are designed so that widening the
surface is a deliberate act, never an accident. Each new command needs its own
closed value type with no opcode parameter, its own explicit catalog policy, and
its own entry in the write choke point's allowlist. There must never be a generic
`SendBootloaderCommand(opcode, …)`; that single API would dissolve all three
interlocks at once and put `ProgramGUID` one integer away from a caller again.
