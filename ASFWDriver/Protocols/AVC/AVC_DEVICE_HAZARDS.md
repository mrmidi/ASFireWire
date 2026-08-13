# AV/C Device Hazards

Status: living document. Read this **before** adding a device identity, widening a
probe, or changing FCP/transaction timeout behaviour.

Some FireWire audio devices are damaged — in the sense of "needs a power cycle" or
"stops answering transactions" — by commands that are perfectly legal AV/C. Others
wedge *our* stack rather than themselves. This file records both kinds, what the
evidence is, and the rule that follows.

A hazard here is only listed when it is backed by a reference stack or a measurement
on real hardware. Where a claim is ours and unverified, it says so.

### Two severities, two guards — do not conflate them

| surface | worst case | recovery | guard |
|---|---|---|---|
| **AV/C command path** (H1) | firmware hangs | **power cycle** | per-device permitted-frame table, `Protocols/AVC/AVCCommandFilter.hpp`, enforced at `FCPTransport::SubmitCommand` |
| **Bootloader register window** (H4, `0xFFFF_C802_1000`) | permanent, in flash | **possibly none** | command code frozen to `0x11` in `BeBoBBootloaderCue.hpp` + user-client refusal of writes into the window |

Neither reference claims permanent damage from AV/C: Linux says *"freezed"*
(`bebob_maudio.c:30-31`) and FFADO the same (`special_avdevice.h:34-37`). Do not
escalate that to "bricks the device" — a maintainer who believes an FCP mistake is
unrecoverable will refuse to test on hardware at all, and a power cycle is a risk worth
taking to get the probe validated.

The bootloader window is the one where "brick" is the right word, because it is the
firmware upload channel: `0x0a ProgramGUID`, `0x0b ProgramMAC` and `0x10
ProgramHWIdVersion` write flash permanently, and `0x04`/`0x05`/`0x06`
`DownloadStart/Block/End` overwrite the firmware itself
(`references/libffado-2.5.0/src/bebob/bebob_dl_codes.h:39-54`). Our cue and
`ProgramGUID` are the same 12-byte block write to the same address, differing in **one
byte at offset 6**.

Deeper background lives in the local-only notes (gitignored `docs/`):
`docs/BEBOB_BRIDGECO_REFERENCE.md` and `docs/MAUDIO_1814_KEXT_RE.md`.

---

## Rule 0 — the probe tier is decided from identity, before a byte is sent

Config ROM identity is known from discovery. Choose what may be sent **from that
alone**. Do not probe-and-back-off on timeout: a freeze is not a timeout, and no
timeout value recovers a device that needs its power cycled.

The production authority is the safety-rule section of
`DeviceProfiles/Audio/AudioDeviceCatalog.cpp`. It runs before positive matching or
generic AV/C fallback. Discovery publishes the resulting quarantine state to the app;
there is no second Swift deny-list to drift out of sync.

The FireWire 1814 bootloader (`0x00010070`), operational firmware (`0x00010071`),
and ProjectMix I/O (`0x00010091`) produce no family adapter and no audio nub, and
nothing is sent to them automatically.

The two operational personas were previously quarantined outright. They now carry
`ProbePolicyId::BeBoBFilteredCommandSet`, the "separately reviewed named operation
policy" this section used to defer to. The change is a narrowing, not a loosening:
quarantine was a device-level kill switch that refused even the command these devices
tolerate, while proving nothing about the commands that freeze them. What replaces it
bounds every frame individually, at the point frames are sent, including the
user-client raw path.

**Widening that table is the reviewed act.** Adding a row means adding its evidence to
H1 first. Adding a *family provider* to those definitions is a separate decision again,
and is gated on H5 and H8.

---

## H1 — M-Audio "special firmware" freezes on unimplemented AV/C

**Affects:** FireWire 1814 (`0x000D6C` / `0x00010071`), ProjectMix I/O
(`0x000D6C` / `0x00010091`) — the **booted** identities.

Linux states it plainly: *"Firewire 1814 and ProjectMix I/O uses special firmware. It
will be freezed when receiving any commands which the firmware can't understand."*
— `bebob_maudio.c:30-31`. Recovery is a power cycle.

FFADO independently skips these two model IDs before its BeBoB probe
(`bebob/bebob_avdevice.cpp:88-91`). ALSA's userspace stack agrees for the extension
set: *"Special models doesn't support any bridgeco extension."*
(`protocols/bebob/src/maudio/special.rs:100`).

M-Audio's own kext compiles `AVCGetAudioSubUnitInfo` (`01 08 31 07 FF FF FF FF`) and
never calls it — **zero cross-references**. It also ships a null driver,
`com_m_audio_MakeAppleFWAudioGoAway`, whose entire body probes at score `0x7FFFFF` to
out-match Apple's generic FireWire audio driver. The vendor deliberately kept generic
AV/C drivers away from this hardware.

### What is actually safe

Not "no AV/C". The safe surface is narrower *and* wider than a blanket ban:

| Command | Safe? | Evidence |
|---|---|---|
| AV/C General signal format (`0x18`/`0x19`), get and set | **Yes** | Linux `special_get_rate` uses `avc_general_get_sig_fmt` on **input plug 0** — *"Input plug shows actual rate"* (`bebob_maudio.c:302-313`); ALSA `cache_freq` uses `OutputPlugSignalFormat` STATUS (`maudio/special.rs:101-119`) |
| Vendor-dependent with company ID `04 00 04` (clock/format) | **Yes** | `bebob_maudio.c:187-189` |
| Vendor-dependent with company ID `03 00 01` (LED) | **Yes** | `maudio/special.rs:127`; matches the vendor kext |
| `UNIT_INFO` (`0x30`) | **No** | not sent by Linux, FFADO, or the vendor kext |
| `SUBUNIT_INFO` (`0x31`) | **No** | as above; vendor kext has it compiled but unreferenced |
| `PLUG_INFO` (`0x02`) | **No** | as above |
| AV/C extended stream format (`0x2F` + `0xC0`/`0xC1`) | **No** | `maudio/special.rs:100`; and the vendor kext binds `FWRev1AudioDevice::AVCControlPlugSignalFormat` (blind `0x18`/`0x19` CONTROL) into the 1814's vtable at `0x37510`, never the `FWRev2AudioDevice` builder that queries the `0xC1` list |
| AV/C SIGNAL SOURCE (`0x1A`) | **No** | vendor kext calls it only from Audiophile/FW410/Solo/Lightbridge `SetClockSourceInternal`; the 1814 uses the `04 00 04` vendor command |

This table is research for that later operation policy, not an authorization surface.
The normalized pass suppresses all transactions for these identities deliberately;
streaming support is out of scope.

---

## H2 — a device that never answers FCP wedges our transport

**Affects:** everything. The 1814 **bootloader** (`0x00010070`) is just the first
device we have seen trigger it.

Measured 2026-08-10 on real hardware, with `droppedRecords: 0` so absence of records
is authoritative:

- `AVCUnit: Initializing...` at `t = 299,204.756 s`, then nothing.
- AR responses jump **tLabel 37 → 39**. tLabel 38 is the FCP command write; it was
  allocated and never completed. The device never answered the write to its FCP
  command register at `0xFFFF_F000_0B00`.
- Because `FCPTransport` arms its response deadline **from write completion**
  (deliberately — see H2.1), no timer was ever armed.
- The command held the single `pending_` slot for **33.6 minutes**. Every later FCP
  command queued behind it (`FCPQueuePolicy::kFifo`), including an MCP-issued
  STATUS that simply never returned.
- It was released only by the bus reset from a physical replug, completing with
  `AVCResult::kBusReset` (9) at `t = 301,223.539 s`.

**Rule:** never assume a device answers FCP just because it enumerated and declares
the AV/C unit spec ID (`0x00A02D`). A silent device must be bounded by a timer, not by
a bus reset.

### H2.1 — where the timeout belongs

Arming the AV/C response deadline from write completion is correct and matches Apple.
`IOFireWireAVCCommand.cpp:70-73` documents the state table:

```
Write submitted:  fTimeout = 0        (no AV/C timer while the write is outstanding)
Write complete:   fTimeout = 250000   (250 ms response deadline)
Interim received: fTimeout = 10000000 (10 s)
```

and the retry guard at `:242` is `state == kIOReturnTimeout && fTimeout != 0 && ...`
— Apple does not replay a command whose write has not landed.

That is safe for Apple because the **transaction** is bounded one layer down:
`IOFWAsyncCommand.cpp:100` and `:152` set `fTimeout = 1000*125` (125 ms) on every
async request command. M-Audio's kext raises its own write to `10000000` µs,
confirming it is a per-command knob over that default.

**Do not add a write watchdog at the FCP layer.** It duplicates a bound that belongs
to the transaction layer, and CLAUDE.md forbids leaving two mechanisms for one
behaviour. ASFW's equivalent is `Transaction::deadlineUs_`, checked in
`Async/Track/Tracking.hpp:313`:

```cpp
if (deadline > 0 && nowUsec >= deadline) { ... }   // deadline 0 => immortal
```

set by `OnTxPosted` from `Async/Commands/AsyncCommandImpl.hpp:163-164` (500 ms) and
ticked by `Scheduling/WatchdogCoordinator.cpp:144`.

**Measured 2026-08-10, second capture** (`droppedRecords: 0`, ring sequences 1–562):
the deadline *was* armed, and the expiry never ran.

- `OnTxPosted` calls `SetDeadline` unconditionally after a successful submit
  (`Tracking.hpp:250`) — it is outside the read/write strategy branch. Both submit
  failure paths log `ASFW_LOG_ERROR` (`AsyncCommandImpl.hpp:146-156`), and a missing
  transaction logs `"OnTxPosted: Transaction tLabel=%u not found"` (`Tracking.hpp:261`).
  **None of those records exist.** So the 500 ms deadline was set.
- `asfw://telemetry/snapshot` reports `async.timeouts: 0` with tLabel 38 outstanding
  for **~695 s**. Nothing in the ring mentions tLabel 38 after its AT request — no
  completion, no cancel, no timeout.

The fault is therefore in the expiry path, not in a missing deadline. Three guards can
silence it, and they are not equally bad:

1. **`AdmitsNormalWork()` went false.** `ScheduleAsyncWatchdog`
   (`ASFWDriver.cpp:928-937`) returns without arming, and **nothing else re-arms the
   chain** — the only re-arm is the timer rescheduling itself from
   `AsyncWatchdogTimerFired_Impl` (`:953`), with the initial arm at `:424`. One false
   reading kills async timeouts permanently for the life of the driver.
2. **`is_bus_reset_in_progress_` stuck true.** `AsyncSubsystem::OnTimeoutTick`
   early-returns (`AsyncSubsystemLifecycle.cpp:689-691`) while the timer keeps ticking
   — timeouts silently disabled with no other symptom.
3. **`isRunning_` false** (`:686-688`). Least likely: submissions were succeeding.

`watchdogTickCount` separates them, and it already crosses the wire — `StatusPublisher`
publishes it (`StatusPublisher.cpp:115`, `:181`) and the app decodes it at status offset
136 (`ASFW/Models/DriverConnectorModels.swift:109`). It is simply never surfaced
anywhere: no UI, no MCP field. Mapping it into the telemetry snapshot is an app-only
change (relaunch, no dext reinstall) and is the next step.

Note the severity: **this is not a BeBoB bug.** What was measured is that the expiry
path did not run for at least 695 s of this generation — long enough that any device
failing to answer any request would wedge the same way. The 1814 bootloader is only
what made it visible. Whether the tick runs in other generations is exactly what
`watchdogTickCount` would tell us.

### H2.2 — a command that reaches `pending_` must always reach its completion

`FCPTransport::StartPendingWrite()` used to `return false` without completing when
`shuttingDown_ || !routeRegistry_`, stranding the command with no timer. Fixed; keep
it that way. Every non-success exit from that function either completes the command or
provably does not own one.

---

## H3 — Linux's forced bus reset is a Linux workaround. Do not port it.

Linux forces a bus reset immediately after registering these two models
(`bebob.c:284-293`):

> *"This is a workaround. This bus reset seems to have an effect to make devices
> correctly handling transactions. Without this, the devices have gap_count mismatch.
> This causes much failure of transaction."*

An earlier revision of this file read that as a device requirement and flagged it as
work we owed. **That was wrong**, and the correction matters because the wrong reading
leads somewhere harmful: a per-model bus reset in a device class, which is global state
affecting every device on the bus.

**The vendor kext does not do it.** Cross-checked in IDA against
`M-AudioFireWireBeBoB` (`FireWireBeBoB_01.11.002`, fully symbolized): `resetBus` has
**zero** occurrences in the binary, and the string `gap` does not appear anywhere,
including comments and data. `com_m_audio_FWMetaNub::start` (`0x1cf68`) — the exact
analogue of Linux's post-`snd_card_register` hook — does `safeMetaCast` →
`getController` → `getNodeIDGeneration` → copy five properties →
`FirmwareCacheInfoRegisters` → register, and nothing else. The kext imports
`IOFireWireNub::getBus()` and `getController()`, so it *could* reach `resetBus()`
through the vtable; it never does.

**Because IOFireWireFamily handles gap mismatch generically**, for every device:

| site | behaviour |
|---|---|
| `IOFireWireController.cpp:2137-2155` | `processSelfIDs` compares gap counts across self-IDs; on mismatch sends a PHY config forcing gap `0x3F` and sets `fGapCountMismatch` |
| `:1605-1620` | `doBusReset` promotes to **IBR instead of ISBR** while `fGapCountMismatch`, with a comment describing exactly the latch-and-reset loop Linux is compensating for |
| `:3404-3418` | `finishedBusScan`, on inconsistent gaps, stages the PHY config packet and calls `resetBus()` |

So Apple *does* issue a driver-initiated bus reset for gap count — from the controller,
as bus policy, never from a device driver. Linux's device-driver kick exists because its
core converges more weakly.

**ASFW already implements all three:** `kConservativeMismatchGapCount = 0x3F`
(`Bus/BusResetCoordinatorActions.cpp:25`, forced at `:444`),
`GapPolicyDecision::GapMismatchRequiresLongReset`
(`Bus/BusManager/GapPolicyCoordinator.cpp:155`, driving long-reset selection at
`:61-80` — the mismatch case overrides `useShortResetForPureOptimization`, matching
Apple's `useIBR`), and PHY config dispatch at `:635`.

**Rule:** if these devices show transaction failures with gap-count symptoms, the fix
belongs in bus policy, not in a BeBoB device class. Confirming our mismatch path fires
with a 1814 attached is a log check, not new code.

Linux also defers registration to the next `update()` so user space does not start I/O
before that reset lands. That part is about Linux's own registration ordering and has no
ASFW analogue either.

---

## H4 — the M-Audio bootloader cue

Send **once**. Twelve-byte **block** write to `0xFFFF_C802_1000`
(`BEBOB_ADDR_REG_REQ`) — *not* the info block at `…C802_0000`.

Payload is little-endian in memory, so the wire bytes are:

```
01 00 00 00 | 00 00 11 01 | 00 00 00 00
```

i.e. as big-endian quadlets `0x01000000, 0x00001101, 0x00000000`. ASFW defaults to
`ToBusOrder`; sending these as ordinary BE quadlets is wrong. Cross-validated three
ways: Linux `bebob_maudio.c:119-127`, the vendor kext's `FirmwareStart`, and the
`IOBufferMemoryDescriptor::withBytes` path in that kext performing no swap.

**The first quadlet is not the literal `0x00000001`.** The vendor kext sends the
BootROM **protocol version** field (info block `+0x08`), read live from the device.
Linux hardcodes `1` under the comment *"Bootloader Protocol Version 1"*
(`bebob_maudio.c:40-49`); echoing the device's own value is the correct general
implementation.

Gated on the BootROM **software build date** (`+0x20`) being `>= "20070401"`
(firmware 5058). Older units expect a full firmware blob upload at every power-on,
which ASFW does not implement (`bebob_maudio.c:99-113`).

CUE2's *"initializing configuration to factory settings"* annotation is **not** a
destructive warning: the vendor kext sends the identical constant on every cold boot
and keeps a separate `ResetToFactorySettings` method for the booted device.

After the cue the device bus-resets and returns as `0x00010071` — which is H1.

---

## H5 — special-firmware stream geometry cannot be queried

Linux: *"initialize these parameters because driver is not allowed to ask"*
(`bebob_maudio.c:273`, in `snd_bebob_maudio_special_discover` at `:258`). The channel table is
hardcoded (`bebob_maudio.c:227-254`) and the vendor kext hardcodes the identical
numbers. Capture count is selected by `dig_in_fmt`, playback independently by
`dig_out_fmt`, so S/PDIF-in with ADAT-out is a legal combination.

`map_data_channels` is skipped entirely for this quirk (`bebob_stream.c:417`).

---

## H6 — bad SYT can corrupt CMP teardown and generate a bus reset

Linux, `bebob_stream.c:636-643`:

> *"Some devices are strictly to generate any discontinuity in the sequence of tx
> packet when they receives inadequate sequence of value in syt field of CIP header.
> In the case, the request to break CMP connection is often corrupted, then any
> transaction results in unrecoverable error, sometimes generate bus-reset."*

A SYT bug is therefore not contained to audio quality — it can escalate to a failed
CMP break and a bus reset. Treat SYT-cadence regressions as bus-affecting.

Related: BeBoB devices send NODATA for several hundred cycles before the first real
event, and *"Some devices postpone start of transmission mostly for 1 sec after
receives packets firstly"* (`bebob_stream.c:661-662`). Do not interpret an early
NODATA run as a fault.

---

## H7 — the bootloader mirrors its response register into host space

Observed 2026-08-10:

```
[Async] AR request UNCLAIMED tLabel=1 tCode=0x1 src=0xFFC0
        addr=0xffffc8029000 len=8 → addr_error
```

`0xFFFF_C802_9000` is **not an unknown address**. It is the BridgeCo bootloader
*response* register — libffado's `AddrRegResp` (`bebob/bebob_dl_mgr.cpp:48`, with
`AddrRegRespBuf` at `+0x40` on `:49`) — the counterpart of the request register
`0xFFFF_C802_1000` that the boot cue is written to.

Both references treat it as a **device-side register the host reads**:

- FFADO's `readResponse()` reads `getRespSizeInQuadlets()` quadlets from it *on the
  device* (`bebob_dl_mgr.cpp:623-627`).
- The vendor kext does the same in `com_m_audio_FWMetaNub::FirmwareReadResponse`
  (`0x1c20f`): `addressHi = -1`, `addressLo = 0xC8029000`, a **read**, three retries,
  `IOSleep(0x7D0)` = 2000 ms between attempts.

The 1814 *also* pushes that register to the host at the same numeric address, and the
8-byte length identifies the payload exactly. A bootloader response header is protocol
version (1 quadlet) plus `commandId` lo/hi + `commandCode` + `operandSize` (1 quadlet),
and `getRespSizeInQuadlets() = 2 + operandSizeResponse` (`bebob_dl_codes.h:95`,
`bebob_dl_codes.cpp:68-80`). Eight bytes is therefore the bare header with no operands —
a "a response is ready" notification, not the response body.

**Answering `address_error` is correct, and it is what Apple's stack does.** The vendor
kext claims no host address space beyond CMP: the only address-space symbol referenced
anywhere in the binary is `IOFireWirePCRSpace::getPCRAddressSpace` (three sites, all in
`m_audio_b_FWAVCConnectionManager`). M-Audio's own driver rejects this write exactly as
ASFW does, and the device tolerates it. **Do not claim the range** — the attested way to
collect a bootloader response is to read the device register, and claiming it would be
synthesized wire behaviour with no reference behind it.

### What prompts it: our own FCP command write

Measured 2026-08-10 from `asfw://transactions/recent` and the driver ring
(`droppedRecords: 0`, sequences 1–562, exactly **one** UNCLAIMED record in the whole
run). Times are relative to the bus reset that started the generation:

```
+0.011007 s  AT tx  tLabel=37  readQuadlet → 0xFFFFF0000498          (last ROM quadlet)
+0.011742 s  AR rx  tLabel=37  response                              ack_complete
+0.012908 s  AT tx  tLabel=38  writeBlock  → 0xFFFFF0000B00  len=8   (the FCP command)
                    ... no response for tLabel 38, ever ...
+0.133376 s  AR rx  tLabel=1   writeBlock  ← 0xFFFFC8029000  len=8   (device → host)
+59.563458 s AT tx  tLabel=39  readBlock   → 0xFFFFC8020000          (BootROM, app)
```

The push follows our FCP command write by **120.5 ms** and is the only bus event
between them. It is not a beacon — one record in the entire run. It is not triggered by
the BootROM read, which happens 59 s *later*. And ASFW has never written a bootloader
request: nothing under `ASFWDriver/` writes `0xFFFF_C802_1000`, and `bootCueSupported`
(`ASFW/Models/BridgeCoReportModels.swift:43`) is report-only.

**The BridgeCo bootloader has no AV/C stack.** It takes the write to the FCP command
register at `0xFFFF_F000_0B00`, never sends a write response for it, and answers on its
own channel with a bootloader response header instead. That is the explanation for this
hazard *and* the device side of H2. The host side is H2.1.

Operationally: a BridgeCo device in bootloader state must never be sent FCP. Boot it
first, or leave it alone.

---

## H8 — the start choreography is inverted for special firmware

Linux starts the AMDTP domain **first**, then re-sends the rate:

> *"The firmware customized by M-Audio uses these commands to start transmitting
> stream. This is not usual way."* — `bebob_stream.c:644-659`

`special_set_rate` itself is OUT, then a settle delay, then IN
(`bebob_maudio.c:315-338`). Linux uses `msleep(100)`; the vendor kext uses
`IOSleep(300)` twice around its rate set. **Prefer 300 ms** — the vendor driver is
authoritative for what the hardware needs.

ASFW's `BeBoBProtocol` base does sig-fmt *before* CMP, so this needs a post-start
hook, not a reordering of the existing one.

---

## Checklist for adding a device identity

1. Is it in Linux `bebob.c` / FFADO with a quirk or a `NULL` spec? A `NULL` spec means
   bootloader — it cannot stream.
2. Does any reference refuse to send it a command? Mirror that refusal.
3. Does it need a forced bus reset after registration (H3)?
4. Is its geometry queryable, or must it be hardcoded (H5)?
5. Land the probe gate in the **same** change as the identity, never after.
