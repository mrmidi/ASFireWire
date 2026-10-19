# FF800 status: RME 3.41 reverse-engineering findings

Recorded 2026-10-18. This describes observed vendor code, not a complete firmware
register specification. The implementation and remote-validation sections below
describe the subsequent choreography change; the RE evidence remains separate.

## Evidence and provenance

Inspected the x86_64 slices of `FirefaceAudioDriver.kext` and `Fireface Settings.app`
from the locally supplied `driver_fw_mac_341/Fireface_really_latest.pkg`. The kext
bundle version is 3.41. SHA-256 of the complete universal kext executable:
`a04ce313a4e27b4476213e84373e19b0c41a2523339ca6faab571d6165087074`.
Addresses below are IDA virtual addresses with kext image base 0 and Settings app
image base `0x100000000`; they are not runtime addresses. The arm64e slices and
device firmware have not been checked here.

The vendor app preserves Objective-C names and UI strings. Original register
macro/enum names are not recoverable from these binaries. Masks below come from
instruction tests connected to the preserved labels, not invented vendor names.

## Four raw status quadlets

Values are little endian. The address has different read and write meanings:
block writes to `0x801c0000` control PCM fetching, not the read-side status fields.

| Raw word | Address | Established meaning |
| --- | --- | --- |
| q0 | `0x801c0000` | Input lock/sync, AutoSync reference/rate, coarse rate field |
| q1 | `0x801c0004` | Clock configuration and mirrored settings; TCO lock/sync |
| q2 | `0x801c0008` | FF800 transmit-channel result: `0xffffffff` means pending; otherwise channel is low 6 bits |
| q3 | `0x801c000c` | Unresolved; do not name it another channel or an error code |

For q2, vendor `hwStart` (`0x3d40`) reads four raw words at `0x3fa2`, tests
q2 against `0xffffffff` at `0x4149`, retries every 5 ms, and masks it with `0x3f`
at `0x42ae`. Cross-check: Linux `firewire/fireface/ff-protocol-former.c:294-332`
and FFADO `src/rme/rme_avdevice.cpp:760-795` in the local reference copies.

### Follow-up search for q3 consumers

The inspected x86_64 vendor drivers provide no semantic consumer of raw q3:

- 3.41: all four direct `hwReadStatus` call sites are in `UserClientReadStatus`
  (`0x1c4d`), `set_latencies` (`0x3802`), and the initial/retry reads in `hwStart`
  (`0x3f9b`, `0x417c`). The first reads q0/q1, the latency call reads one word at
  byte offset 4 (q1), and startup reads four words but consumes q2 only for the
  channel. Assembly at `0x413a` loads the low 32 bits of the q2/q3 stack slot;
  no corresponding q3 load was found. Other direct FWRead/FWReadQuad callers
  inspected address level meters, flash, TCO, revisions, or q0 rate detection.
- 2.75 (from local `Fireface_275_x86/Fireface_latest.pkg`, bundle version 2.75):
  `hwStart` (`0x2d1a`, read at `0x2ef2`) likewise reads four words, uses q2 at
  `0x2f16-0x2f34`, and uses q0/q1 for clock/rate decisions, without consuming q3.
  Its other direct `hwReadStatus` callers are `UserClientReadStatus` (`0x2246`)
  and `initHardware` (`0x3c37`, byte offset 4, one word: q1).
- FFADO `rme_avdevice.cpp:769` prints q3 in the diagnostic tuple but does not use
  it for the allocation decision. Linux's allocator reads q2 directly.

This narrows the uncertainty: the fourth word is fetched by these vendor startup
paths but is not required by their host-side decisions. It does not establish
that the register is reserved, a mirror of q2, or a boolean readiness flag.
The observed pair `1,1` versus `0xffffffff,0xffffffff` cannot distinguish those
hypotheses; a capture where q2 has a different valid channel would be useful.
Coverage is static x86_64 host code for two releases, not firmware or arm64e.

## Vendor names and lock/sync tests

Settings `-[Controller awakeFromNib]` (`0x1000035bc`, array creation at
`0x100003647`) defines `syncCheckStatusStrings`: **No Lock**, **Lock**, **Sync**.
The decoder at `0x100005f52` returns Sync when both bits are set, otherwise Lock
when the lock bit is set, otherwise No Lock. A sync bit alone is No Lock.

| Input | Word | Lock mask | Sync mask |
| --- | --- | --- | --- |
| Word Clock | q0 | `0x40000000` | `0x20000000` |
| ADAT1 | q0 | `0x00000400` | `0x00001000` |
| ADAT2 | q0 | `0x00000800` | `0x00002000` |
| SPDIF | q0 | `0x00100000` | `0x00040000` |
| LTC/TCO fifth status row | q1 | `0x00800000` | `0x00400000` |

Evidence: Settings `0x100006014-0x100006067` uses SIMD comparison-mask lanes
`[0x60000000, 0x1400, 0x2800, 0x140000]` and lock fallback shifts
`[30, 10, 11, 20]`. TCO is decoded at `0x10000606b-0x100006084` with mask
`0x00c00000` and lock fallback shift 23. `timerAction:` (`0x100003a24`, FF800
branch starting at `0x100004044`) connects the results to the UI rows and labels.

These masks agree with FFADO `src/rme/fireface_def.h:270-289,344-351`.
Linux `ff-protocol-former.c:159-170` instead uses `0x00080000` for SPDIF lock;
that is not the lock bit used by this vendor decoder. FFADO names that other bit
`SR0_OVER`, but this vendor decoder does not establish its meaning.
ALSA userspace `protocols/fireface/src/former/ff800.rs:113-155` reverses several
lock/sync labels relative to this vendor code. Do not choose those labels merely
because the file is FF800-specific.

## Vendor AutoSync reference labels

`awakeFromNib` creates `autoSyncReferenceStatusStrings` at `0x100003741`:
**ADAT1**, **ADAT2**, **SPDIF**, **Word**, **LTC**, **VITC**, **No Lock**, empty.
The decoder indexes `(q0 >> 22) & 7` through the table at `0x100006fb0`,
whose entries are `[0, 1, 6, 2, 3, 4]`. Indices 6 and 7 default to No Lock.

| `q0 & 0x01c00000` | Vendor display |
| --- | --- |
| `0x00000000` | ADAT1 |
| `0x00400000` | ADAT2 |
| `0x00800000` | No Lock |
| `0x00c00000` | SPDIF |
| `0x01000000` | Word |
| `0x01400000` | LTC |
| `0x01800000` | No Lock |
| `0x01c00000` | No Lock |

At `0x1000060be-0x1000060df`, the app also forces No Lock when the external-rate
field `q0 & 0x1e000000` is zero. The VITC string exists but is not selected by
this source lookup. This display does not label `0x01800000` as TCO or
`0x01c00000` as Internal; it does not prove the firmware meaning of every code.

The external-rate field is converted to the labels 32, 44.1, 48, 64, 88.2, 96,
128, 176.4, 192 kHz in order, with codes 1 through 9 (`0x100006108-0x10000612f`,
frequency strings at `0x1000036dd`). This agrees with FFADO and Linux's table.
Configured rate in q1 is a different field: mask `0x1e`, with 48 kHz code `0x06`
(Linux `parse_clock_bits`, `ff-protocol-former.c:14-73`).

## Driver-to-Settings API is not four raw status words

Kext `UserClientReadStatus` (`0x1be8`) reads only q0 and q1 for FF800, and returns
four scalar outputs: q0 with bit 0 replaced by engine-running state; q1; software
engine sample rate; a software engine counter. Do not interpret the third and
fourth Settings API outputs as raw q2 and q3. Startup uses a separate raw read.

## ASFW status names and unresolved differences

`FirefaceSettings.hpp` names the concepts with `ClockSource` and `LockState`.
Log names use `none`, `lock`, `sync`, `word-clock`, `timecode`, rather than the
vendor's exact No Lock / Word / LTC strings. Individual masks remain literals.

`FirefaceSettings.cpp::DecodeStatus` uses the vendor-confirmed q0 lock/sync masks.
Its source decoder does not apply the vendor UI's external-rate gate, and returns
unknown for source code 7. `FirefaceClockStatus.hpp::DecodeClockStatus` reverses
lock/sync labels relative to that decoder and expects active TCO at `0x01800000`.
These differences are recorded, not repaired here. Requiring both lock/sync bits
makes a pair reversal immaterial to that boolean test, but source matching can
affect external-clock admission. Internal-clock admission bypasses those tests.

## Startup difference found before the choreography change

Vendor `hwStart`: enable fetching (`0x3f05`); allocate host transmit channel
(`0x3f2d`); send complete init (`0x3f66`); poll raw q2; allocate receiver
(`0x3fa8`); enable device (`0x4000`); sleep 5 ms; start host RX then TX.
`hwInitDevice` (`0x7850`, write call `0x7919`) sends one three-quadlet block at
`0x20000001c`: rate, `(channels << 11) + hostChannel`, channels plus S800 flag.
The previous ASFW path split frequency and the format tail, and opened fetching in Confirm.
Those are proven host-sequence differences, not proof of how firmware handles a
partial command or which difference causes the missing transmit-channel result.

## Implemented choreography

FF800 now uses the existing driver-owned `IOTimerDispatchSource` through
`ITimerScheduler` (production binding: `Service/DriverContext.cpp` passes the
`DriverKitSessionScheduler` to the audio registry). `FF800TransmitStart` owns
three callback stages, with cancelable timers, bounded IO, and generation checks:

1. Configure opens fetching with one 112-byte zero block before host reservation.
2. After reservation, AssignChannels submits the complete 12-byte init command.
3. Its completion starts one 500 ms budget for four-word status reads. Pending
   channel or clock-not-ready results schedule the next read in 5 ms. A valid
   channel and a clock ready at 48 kHz are both required before proceeding.
4. After host preparation, enable submits the start word, then completes only
   after a timer fires at least 5 ms after write completion. The family reports
   zero additional session delay, so the session skips its generic IOSleep.
5. The recipe starts host RX then TX; Confirm verifies health without repeating
   fetch-enable. Stop still disables the device before muting PCM fetching.

The split-init constants and speculative stop/reset/retry path were removed.
Missing allocation now produces one bounded failure and session rollback. If host
reservation fails after fetch-enable, rollback mutes without sending an init or
device-stop command. An uncertain partial init/enable still requires device stop.
FF400 retains its existing startup order and delay.

The synchronous family/session boundary still awaits stage completion on a
separate queue; neither the channel-poll continuation nor the FF800 post-enable
delay sleeps on the driver callback queue. This change does not convert the
entire session/health machinery into an asynchronous API.

## Remote hardware validation and log handoff

No FF800 hardware is available to the implementer. Host tests and an Xcode build
can validate structure and cancellation, not establish that firmware accepts the
sequence or that audible streaming works. Send the tested build to the hardware
owner; no installation or hardware success is implied by a build passing.

Ask the hardware owner to record the build number, connect the FF800, select it
as output, start playback once, then export the complete ASFW driver log after
success or failure. If playback starts, stop and start it once more and export
again; this exercises mute/stop and re-init as well as first start. Keep the
surrounding Session/IRM/Isoch records, not only filtered RME lines. Note whether
sound played, whether startup stalled, and whether any disconnect/reset occurred.

The fetch stage announces `recipe=vendor341-full-init-v1` so returned logs identify
this choreography even when comparing local builds with the same version number.

`[RME][Choreo]` records, at the default persisted level, include:

- Human operation names, GUID, generation, register address, result and duration.
- Exact little-endian payload bytes in chunks with total length and byte offset.
  Fetch/mute payloads are included; HexDumps need not be enabled.
- Raw q0/q1/q2/q3, decoded clock/input-lock states, channel pending/allocated,
  poll count and elapsed time. q3 remains uninterpreted.
- Poll snapshots initially, at most every 100 ms, on success, and on timeout.
  The 5 ms polls themselves do not flood the ring. All new records fit its
  232-byte message limit; raw and decoded status use separate lines.
- Explicit submission/completion of init and enable, timer-settle start/end,
  and final phase completion. Late replies cannot resume a canceled stage.

Expected progression: enable PCM fetching -> IRM reservation -> initialize stream
(complete command) -> channel and clock ready -> enable device transmit -> timer
settle complete -> host RX/TX and streaming confirmation. A failure should identify
the last submitted operation, its result, and the last received raw status tuple;
no status reply is reported explicitly rather than shown as zero status.

For an additional unified-log capture on the hardware owner's machine:

```sh
/usr/bin/log show --last 10m --info --debug --style compact \
  --predicate 'eventMessage CONTAINS "[RME][Choreo]"'
```

The driver-ring export is the preferred artifact. Do not filter by the dext
process name; DriverKit messages can surface under `kernel`.

## Host validation for this change

- 62 Fireface family/async/sequence tests passed, including byte-exact vendor
  commands, fetch-only rollback, timer refusal, pending/missing/short status,
  cancellation, generation invalidation and late completions.
- Those 62 tests also passed with AddressSanitizer and UBSan enabled.
- 23 Fireface settings tests and 180 session characterization tests passed.
- Final unsigned Debug DriverKit build passed for x86_64 and arm64e. This was a
  driver-only build (`ASFWDriver`, generic DriverKit destination), with signing
  and project-wide static analysis disabled for the final compile/link check.
  The earlier broader build completed driver analysis (warnings in untouched
  SCSI and bus-timing code) and was stopped during unrelated app dependencies.
- The log-export test checks exact little-endian init/status bytes, q3 retention,
  human clock/channel meaning, bounded log volume and absence of record truncation.

These results do not establish FF800 firmware acceptance or audible playback.

## Remote hardware result — 2026-10-19

The hardware owner reported working playback and returned
`ASFW_Driver_Logs_2026-10-19.txt`. This is hardware evidence for the combined
choreography on FF800 GUID `000a3500efafcd8a`, at 48 kHz, 28 channels and S800;
it does not isolate which ordering change was necessary.

- Both starts identify `recipe=vendor341-full-init-v1` (records #390 and #625).
- First start submits the complete 12-byte init command with payload
  `80 bb 00 00 00 e0 00 00 1c 08 00 00` (#415). The first status read has
  q2/q3=`ffffffff`; the second, 5 ms later, has q2/q3=`00000001` (#425).
  Channel 1 and the configured 48 kHz clock are accepted after two polls.
- The post-enable timer completes (#474), then streaming is confirmed (#493).
- After stop/restart, channel 1 is ready on the first poll (#656); the timer
  completes (#673) and streaming is confirmed again (#692).
- The second stream runs from approximately 162.911 to 245.263 seconds of host
  monotonic time (about 82 seconds). Its TX preparation heartbeats report
  `forcedNoData=0` and `missedFinality=0`; the device subsequently stops.
- The export contains 941 records, reports complete coverage at snapshot, and
  reports zero dropped records. It is a bounded run on one device, not evidence
  for other rates, models, hot-unplug recovery, or transport ring-wrap coverage.

q3 still cannot be named from this run: it transitions together with q2, but
the only allocated channel observed is 1, leaving mirror versus state semantics
unresolved. Playback success is the owner's report; startup, timing and telemetry
above are directly visible in the returned log.
