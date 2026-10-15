# Pro40 DICE III: DCP receiver-diagnostic read-path investigation

2026-10-15. Source: the maintainer's packed IDA database,
`tools/extracted_firmware/Pro40d3Firmware.elf.i64` in the main checkout
(local only; it need not exist in a worktree). Addresses below are firmware
virtual addresses, not host addresses. No firmware code was copied into ASFW.

## Result

The `0xFFFFE0400000` window is a category/command RPC transport. The normal
handlers inspected do **not** expose an address-based read of the DICE III
receiver registers at `0x60001000` / `0x60001020`. Do not implement a receiver
dump by treating the application GET or flash READ commands as a general PEEK.
This conclusion covers this Pro40 image; it does not establish which commands
PreSonus firmware implements. No live DCP requests were sent.

## Transport and dispatch evidence

- `dcp_RegisterWindowE0400000`, `0x1BF64`: registers the 0x350-byte window.
  Its literal pool at `0x1C0BC` contains the request address
  `0xFFFFE04001C0`, response address `0xFFFFE0400030`, and range end
  `0xE0400350`. Advertised logical-message maximum is 0x400 bytes; the
  request/response transport carries it in segments.
- `dcp_HandleAsyncRequest`, `0x1BAB4`: admits quadlet/block reads/writes and
  compare-swap locking at offset +0x08. Requests start at +0x1C0; response
  reads start at +0x30. An ordinary window read is not a CPU-address read.
- `dcp_TransportStateMachine`, `0x1B688`: checks segment sequence and direction,
  limits logical request length to 0x400, assembles payloads, and signals the
  dispatch worker. A request has a 16-byte transport header. Response segments
  carry at most 0x180 bytes of payload (`0x1B938`–`0x1B980`).
- Dispatch worker `0x1BE8C`: at `0x1BEC0`–`0x1BEC4` extracts category from
  header bits 23:12, and at `0x1BEF0`–`0x1BEF8` extracts command bits 11:0.
  It walks the linked handler list and calls the matching category handler at
  `0x1BF08`. Unknown categories return status 4.
- Category registration `0x1BF4C`: prepends one 16-byte node (next pointer,
  category at +4, callback at +8, context at +12). Direct callers are
  `0x3E6C`, three registrations in `0x16404`, and `0x1E21C`. The core category
  is inserted directly by the window initializer (`0x1C064`–`0x1C070`).
- Response helper `0x1BCD8`: publishes status/data/length to the response
  transport and waits for consumption. This is an RPC response, not a raw
  address-range mapping.

## Registered categories examined

| Category | Registry node | Handler | Observed normal command behavior |
| --- | --- | --- | --- |
| 0 | `0x2DE9C` | `0x1BD80` | Command 1 probes category availability; 2 returns an 88-byte information record; 3 completes and may invoke a callback. No memory-address command. |
| 1 | `0x2DE0C` | `0x15F68` | Command 0 returns meter geometry; 1 reads a bounded meter window at `0x50008800 + 4 * index`. Index/count are 16-bit; count is capped at 0x100 and bounded by configured meter count. An optional flag also resets a meter timer: do not call it a universally side-effect-free dump. |
| 2 | `0x2DE1C` | `0x16040` | Mixer information, coefficient reads/writes, and a version/value query. Read address derives from a configured coefficient buffer and small row/column indices, not a caller-supplied CPU address. |
| 3 | `0x2DE30` | `0x161EC` | Routing information and reads/writes of one of three configured route tables. Reads index a selected table; there is no normal full-width address argument. |
| 4 | `0x2DF78` | `0x1D684` | Flash information, region information, erase/progress, write, read, checksum, and another erase/control operation. Command 5 reads within a selected flash region or, with region index -1, the configured whole-flash range. It calls flash access `0xD080`; this is not a receiver-register read. |
| 0x800 | `0x2D9C8` | `0x3CE4` | Application GET/SET/NOTIFY. GET/SET use offset and length relative to fixed application buffer `0x363B0`, checked against 0x206C. Debug strings at `0x22F6E`/`0x22F8B` identify GET and SET. |

This analysis describes intended command inputs and ordinary read paths. It
does not attempt to turn malformed requests or insufficient bounds checks into
a memory-disclosure exploit, and those are not a suitable driver diagnostic API.

## Receiver map cross-check and latch ownership

The packed database's literal pool at `0xECB4` contains ARX1 `0x60001000`
and ARX2 `0x60001020`. CFG0 writer `0xE9AC` puts the channel in bits 21:16.
`0xEA20` writes delay at +0x08; **+0x0C is CFG3**, written by `0xEA6C`,
not a second delay register. The C++ catalog uses these observed names.
`avs_RxIsLocked` at `0xFACC` reads status +0x1C bit 0.

`avs_RxPollStatusEvents`, `0xFBD0`, reads status at `0xFBF4` and writes the
value back at `0xFBF8`, clearing the event latches. At `0xFC28`–`0xFC50` it
counts the 14 high-half event bits into firmware counters rooted at `0x305B4`.
The clock task at `0x12C54` calls this for both receivers every 20 ms.
Consequently, even a verified late MMIO read could miss the fault that prevented
lock. The counters are outside the application GET buffer rooted at `0x363B0`;
the inspected DCP handlers do not provide a normal read of that counter table.
These private firmware addresses are evidence for this image, not portable
per-chip register addresses or an implemented access path.

## Implemented receiver-lock watch

The module lives in `ASFWDriver/Audio/Diagnostics/DICE`, independently of the
stream-control code. `DiceReceiverRegisters.hpp` catalogs the chip-specific CPU
registers and DCP window coordinates; the standard DICE protocol map stays in
`DICETypes.hpp`. The address catalog is not an automatic-read allowlist.

`ASFW_ENABLE_DICE_RECEIVER_DIAGNOSTICS` defaults to 1 in `project.yml`. Pass
`ASFW_ENABLE_DICE_RECEIVER_DIAGNOSTICS=0` directly to `xcodebuild` to disable it. With the flag
at 0, the watcher is a compile-time stub: no lock allocation, timer scheduling,
callbacks, or register reads. Cached diagnostics explicitly report "disabled".
The separate `DiceDiagnosticsDisabledTests` target checks this build path.

`DiceFamilyDriver::CompleteConfirm` starts the standard-register watch for each
assigned device-RX stream. It issues asynchronous STATUS/EXT_STATUS quadlet
reads at +100, +500, +2000, and +5000 ms. A +7000 ms deadline produces a verdict
if a bus completion is lost. Existing notification-driven health block reads
feed the same session accumulator without an extra notification reader.

The lock time is the **first observation**, not exact hardware acquisition
latency. The snapshot retains first-lock time per receiver, observed lock loss,
last raw values, accumulated slips, sample count, errors, GUID, and generation.
Stops, route changes, shutdown, and destruction fence submissions and cancel
timers; teardown closes admission to future watches. A completed or interrupted
verdict remains until the next start or protocol removal.

Reference cross-check: Linux `firewire/dice/dice-interface.h:138-170` defines
ARX1–4 lock/slip positions and states that a quadlet read clears slip history.
The driver's sparse watch owns those quadlet reads during its window. The
existing health probe is a block read; the app's DICE Report now splits GLOBAL
reads around EXT_STATUS, including its quadlet fallback, and uses the cached
watch value. Selector 1015 returns a copied, 96-byte v1 family-specific
snapshot by GUID; it does not enlarge generic audio telemetry v4. The DICE
Report and Audio Telemetry view display the retained verdict.

After rebuilding/installing and starting playback, capture the bounded watch:

```sh
/usr/bin/log show --last 2m --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[DiceLock]"'
```

Verify the assigned receivers lock, then stop/restart and exercise a bus reset.
Host tests establish state-machine behavior only; they do not establish hardware
lock, clear-on-read behavior on every firmware, or the presence of DCP on PreSonus.
Layer B hardware receiver dumps remain unimplemented pending a verified read path.
