# M-Audio FireWire 1814 / ProjectMix — clock configuration, start choreography, and the host time base

Status: research, with the internal 48 kHz TX and vendor blank-slate clock
sequence now implemented in ASFW.
Decoded from the vendor kext (`M-AudioFireWireBeBoB`, fully symbolised). **The
kext is authoritative here; Linux is supporting evidence.** Where they differ,
the divergence is called out rather than averaged.

Companion to `MAUDIO_1814_STREAM_SYNC.md`, which covers SYT modes, packet
cadence, CIP templates and the RX cursor. This document covers what happens
*before and around* those: how the device is told which clock to run on, the
order the streams are started in, and where the host time base comes from.

`com_m_audio_FWProjectMixDevice` reuses `__ZTV24com_m_audio_FW1814Device`, so all
of this covers both devices. Addresses are file offsets in the IDA database at
`~/DEV/FirWireDriver/OTHER/KEXTs/M-AUDIO/M-AudioFireWireBeBoB.i64`. Behaviour
only — no code reproduced.

---

## 0. The headline

**The kext's audio clock does not depend on the receive stream in any way.**

It reads the FireWire cycle timer, correlates it to host uptime with a latched
paired read, and publishes engine timestamps from the **transmit** DCL callback.
A device that never transmitted a single packet would still produce a running,
correctly-timed playback clock.

ASFW derives its ZTS anchor from the IR (receive) interrupt. That is a coupling
the vendor driver does not have, and it is why a silent receive stream stops
playback here and would not have there. See §6.

---

## 1. Clock configuration — before anything streams

Two entry points, and they are not interchangeable.

### 1.1 Initialisation: `com_m_audio_FW1814Device::SetBlankSlateClockSource` @ `0xe4f0`

```
arg = (cachedClockWord & 0x1C) + 0x40000003
vtable[+2912](this, arg, /*flag=*/1)      // -> SetClockSourceInternal:
    send 16-byte vendor clock frame
    IOSleep(0x12C)                        // 300 ms
    map clock word -> selector FB 4/value 0
    AVCSelectorCommand(4, 0)
    IOSleep(0x12C)                        // 300 ms
IOSleep(0x9C4)                            // outer 2500 ms
```

Three things matter:

- The full initialisation delay is the two inner 300 ms waits plus the outer
  2500 ms blank-slate settle.
- `& 0x1C` preserves the existing digital-format bits while forcing the clock
  source, so a blank-slate reset does not clobber a user's S/PDIF/ADAT choice.
- The generic base call is not a sample-rate write: for this model it emits the
  exact Audio-subunit selector frame
  `00 08 B8 80 04 10 02 00 01 00 00 00`.

### 1.2 User-driven change: `com_m_audio_FW1814Device::SetClockSourceInternal` @ `0xe25c`

```
build 16-byte AV/C frame  ->  vendor clock command
IOSleep(0x12C)            // 300 ms
MapChannelAndFunctionBlock(clock word)
AVCSelectorCommand(mapped FB, mapped input)
IOSleep(0x12C)            // 300 ms
```

SetBlankSlateClockSource calls this whole path and then adds its own 2500 ms
settle. These 300 ms waits do **not** describe the OUTPUT→INPUT plug-signal-format
interlock; Linux's `special_set_rate()` gives that separate delay as 100 ms.

### 1.3 The frame

```
00 FF 00 04 00 04 <clk_src> <dig_in_fmt> <dig_out_fmt> <clk_lock> 00 00 00 00 00 00
```

The first 12 bytes are byte-identical to Linux `avc_maudio_set_special_clk`
(`bebob_maudio.c:171-198`); the kext sends a 16-byte frame with four additional
zero pad bytes. Operand construction in the kext:

| byte | source |
|---|---|
| 6 `clk_src` | `switch (arg & 3)`: `1→1`, `2→2`, `3→0`, default `→3` |
| 7 `dig_in_fmt` | `(arg & 0x08) != 0` |
| 8 `dig_out_fmt` | `(arg & 0x10) != 0` |
| 9 `clk_lock` | `(arg & 0x20) != 0` |

Guard: `arg <= 0x3FFFFFFF` returns `0xE00002C2` (`kIOReturnBadArgument`), so bit
30 must be set — hence the `0x40000003` constant.

### 1.4 Divergence: which clock source

Linux's clock list, with labels (`bebob_maudio.c:343-364`):

| value | label |
|---|---|
| 0 | Internal with Digital Mute |
| 1 | Digital (S/PDIF or ADAT) |
| 2 | Word Clock |
| 3 | Internal |

- **Linux discovery sends `3`** — `avc_maudio_set_special_clk(bebob, 0x03, 0, 0, 0)`
  at `bebob_maudio.c:276`, and treats failure as fatal to discovery.
- **The kext's blank slate sends `0`** — `0x40000003 & 3 == 3` hits the `case 3:
  → 0` arm above.

Same physical clock family, different firmware mode. The original-driver
FireBug capture now resolves the ASFW policy choice: it sends source `0` in a
16-byte frame and follows it with selector FB 4/value 0; capture subsequently
reaches full-size packets. ASFW therefore follows the vendor blank-slate path.
Linux's source `3` remains a known, working-stack divergence rather than the
selected compatibility policy here.

### 1.5 FireBug cross-validation, 2026-08-14

The original driver trace records the sequence directly:

1. channel 1 begins with `02020000 9001ffff` (header-only NO-DATA);
2. `00ff0004 00040000 00000000 00000000` is accepted;
3. about 300 ms later, `0008b880 04100200 01000000` is accepted;
4. the device-output stream restarts, and later runs report full-size packets
   (`Largest: 552` for the prior ADAT formation, then `Largest: 360` for the
   selected S/PDIF formation).

ASFW previously sent a 12-byte source-`3` vendor frame and omitted step 3.

---

## 2. Start choreography

### 2.1 `m_audio_b_FWBaseEngine::StartAudio` @ `0x4bca`

```
Bus->getCycleTime(&now)                                   // FireWire cycle timer
clock_interval_to_deadline(20, 1000000, &deadline)        // 20 ms, host
absolutetime_to_nanoseconds(deadline, this+88)
future = AddFWCycleTimeToFWCycleTime(now, 0xA0000)        // +160 cycles = 20 ms

pass 1: for each DCL program
            arg = isOutputProgram(p) ? 0 : future
            p->Start(arg)                                 // TX now, RX at +20 ms
pass 2: for each DCL program
            p->vtable[+312]()                             // arm
        engine->vtable[+2784](0, &deadline)               // takeTimeStamp(false, t)
```

**Transmit starts immediately; receive starts on a scheduled cycle 20 ms later.**
The lead is computed against the bus cycle timer, not wall clock. `0xA0000 >> 12`
= 160 cycles × 125 µs = 20 ms, matching the host deadline exactly.

The final call seeds the engine's timeline with a timestamp 20 ms in the future
and `increment = false`.

ASFW's V1 M-Audio path keeps the asymmetric transport choreography already
wired for this family, then seeds its *audio packet policy* at TX zero after
the safe all-NO-DATA prefill. It does not wait for RX SYT/replay before it can
produce the first TX DATA packet. Runtime phase correction is later driven by
TX completion timestamps, not by an audio-layer MMIO cycle-time read.

### 2.2 `m_audio_b_FWDCLOutputProgram::Start` @ `0x26900`

```
reset counters
for (i = 0; i < 4; ++i)
    OutputDCLCallback(this, i, /*isPrefill=*/1)     // fill all 4 segments
FWDCLProgram::Start(this)                            // only then start DMA
```

**The transmit ring is fully prefilled before DMA runs.** Four segments, filled
by invoking the driver's own output callback with the prefill flag set. Packets
exist from cycle zero.

Consequence, and it settles a question that is otherwise easy to speculate about:
**the device is never waiting for host audio content.** There is no handshake.
The same pattern is recorded for the Saffire on the DICE side.

### 2.3 `m_audio_b_FWDCLInputProgram::Start` @ `0x2500e`

```
clear counters; shared[+136] = 63; FWDCLProgram::Start(this)
```

No prefill — receive has nothing to prefill.

### 2.4 `m_audio_b_FWDCLProgram::Start` @ `0x250e4`

Idempotence guard on `+141`, then `port->vtable[+344]()`. Returns
`kIOReturnBadArgument` if already started.

---

## 3. The host time base — `CycleTimeToHostNanos` @ `0x26284`

The core of the design, and the reason RX is irrelevant to it.

```
do {
    clock_get_uptime(&t0)
    Bus->getCycleTime(&nowCycles)
    clock_get_uptime(&t1)
} while (nanos(t1) - nanos(t0) > 0x249EF)     // 149,999 ns ≈ 150 µs

host = nanos(t0) + (nanos(t1) - nanos(t0)) / 2      // midpoint of the window

delta = targetCycleField - nowCycleField            // bits [24:12], 0..7999
   wrap: if target <= now  and  -delta > 2000  ->  delta += 8000
         if target >  now  and   delta > 2000  ->  delta -= 8000

return host + 125000 * delta                        // 125 µs per cycle
```

Three properties worth naming:

- **Latched paired read.** Host time is sampled either side of the cycle-timer
  read and the pair is rejected if the window exceeds ~150 µs, then the midpoint
  is taken as the host time corresponding to that cycle reading. This bounds the
  correlation error to ±75 µs by construction rather than by hope.
- **Wrap-safe.** The ±2000-cycle (250 ms) threshold disambiguates a delta that
  crosses the 8000-cycle second boundary.
- **No device data.** Bus cycle timer and host uptime only.

---

## 4. Timestamp publication and warm-up

In `m_audio_b_FWDCLOutputProgram::OutputDCLCallback` @ `0x263c2`, first half,
non-prefill path only:

```
ts = segmentTimestamp[a2]                       // hardware DCL timestamp
if (a2 == 3) ++groupCounter                     // once per 4-segment wrap
hostNanos = CycleTimeToHostNanos(this, ts)      // @ 0x26475 — the ONLY caller
publish into a double-buffered slot pair (+165/+166, alternating on +1360)

if (a2 == 3) {
    if (groupCounter == 2)  shared[+12] = ts; shared[+17] = 1;   // plant RX reference
    if (groupCounter >= 3)  nanoseconds_to_absolutetime(hostNanos, &abs)
                            engine->vtable[+2784](1, &abs)       // takeTimeStamp(true, t)
}
```

- `vtable[+2784]` matches `IOAudioEngine::takeTimeStamp(bool, unsigned long long*)`
  — the import at `0x4c590` — and is the same slot `StartAudio` calls with
  `false`.
- **Timestamps are published once per four-segment group, from the transmit
  callback**, not per packet and not from receive.
- **The first three groups are warm-up.** `< 3` publishes nothing; `== 2` plants
  the cycle-time reference the RX cursor later resyncs against
  (`ResyncInputBuffer` @ `0x2610e`, see the sync doc §5).

Note the direction of dependency: **the RX cursor depends on a reference planted
by TX**, not the other way round.

---

## 5. Three possible clock sources, and which this device's driver uses

| # | source | needs RX? | used by |
|---|---|---|---|
| 1 | SYT from received CIP headers | yes | ASFW today |
| 2 | timestamp in the receive descriptor/interrupt | yes | ASFW today |
| 3 | **cycle timer register, correlated to host uptime** | **no** | **the kext** |

The kext uses (3) exclusively for the audio clock. (1) and (2) cannot start a
stream that has not yet received anything, which makes them unusable as the
*initial* anchor for a playback-only path — the device may legitimately take
~1 s to begin transmitting (`bebob_stream.c:661-662`), and on this family it
will not transmit at all until clocked.

---

## 6. What ASFW does differently, and the cost

ASFW publishes its ADK zero-timestamp anchor from the IR interrupt. That yields a
hard dependency chain with no equivalent in the vendor driver:

```
no RX packets → no ZTS anchor → HAL never runs the IO cycle
              → producer stages nothing → playback impossible
```

Observed on hardware 2026-08-13: every start stage succeeded — clock config,
CMP connect on both plugs, PCR verify, DMA start, post-start signal-format
re-send — and the session was torn down at 4 s by
`ASFWAudioDevice: initial hardware ZTS timed out after 4000 ms`, then retried on
a loop. IT reported ~38,850 packets per cycle; IR reported none.

Two independent consequences:

1. **A silent receive stream stops playback here and would not have in the
   kext.** Under the kext's model the transmit clock runs regardless.
2. **The 4 s ZTS gate has no counterpart in the kext.** It never waits for the
   device to prove it is transmitting. Treating a ZTS timeout as evidence of
   device misbehaviour is therefore unsound on its own.

`MAUDIO_1814_STREAM_SYNC.md` §12 already flagged this: *"The RX cursor genuinely
differs — cycle-timer derived against a reference planted by the TX callback,
rather than SYT/descriptor derived. This is the one part that needs real design
work rather than specialisation."* That assessment was correct and was not acted
on before the hardware run.

---

## 7. Implications for ASFW, in dependency order

1. **A cycle-timer host clock is the missing primitive.** `CycleTimeToHostNanos`
   is small, self-contained, and needs only `getCycleTime` plus a monotonic host
   clock — both of which ASFW already has. It is a better first anchor than
   anything RX-derived because it is available before the device streams.
2. **Prefill the IT ring before RUN.** Verify whether ASFW's BeBoB path does;
   the kext fills all four segments first, unconditionally.
3. **Start order: TX immediately, RX at +20 ms** on a scheduled cycle. Verify
   ASFW's current order against this before assuming it is equivalent.
4. **Warm-up discard is three groups**, with the RX reference planted at group 2.
   Any ZTS timeout shorter than the warm-up plus the device's own transmission
   delay will fire on a healthy device.
5. **Clock configuration is mandatory and slow.** The vendor path is a 16-byte
   clock frame, 300 ms, selector FB 4, 300 ms, and an outer 2500 ms settle. It
   must not sit inside a stream-start timeout budget.

---

## 8. Verified vs inferred

**Verified by decompilation:** the two clock-configuration paths and their
distinct settle constants; the 16-byte vendor frame layout and operand
construction; the FB 4 selector call; the `0x3FFFFFFF` guard; the start order
and its 20 ms lead; the four-segment prefill;
`CycleTimeToHostNanos` in full including the 150 µs window, midpoint and wrap
rule; the single call site of that function; the `takeTimeStamp` slot and its two
call sites; the warm-up gating at group counters 2 and 3.

**Cross-checked against Linux:** the first 12 vendor-frame bytes match
`avc_maudio_set_special_clk`; the clock-source value list and labels; FB 4 is
the digital-input-interface selector; the signal-format interlock is 100 ms.

**Not resolved:** whether the 20 ms start lead is required or generous (already
noted as unresolved in the sync doc), and whether the outer 2500 ms settle is
required in full when the clock is already correctly configured. The source
`0`/`3` divergence remains documented, but the ASFW compatibility policy is now
deliberately source `0` because the vendor kext and captured wire sequence agree.

**Not attempted:** the second-pass `vtable[+312]` arm step in `StartAudio`, and
`AdjustOutputDCL` / `SetValidPackets` / `InitGroupCIPsForOutput` in the back half
of `OutputDCLCallback`, which belong to packet fill rather than to start
sequencing.
