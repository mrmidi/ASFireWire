# SYT Anchoring on the M-Audio 1814

Research note, 2026-08-16. Where the AMDTP transmit timestamp comes from in
M-Audio's kext, in Linux, and in ASFW — and what that does and does not explain
about a capture stream that never leaves NO-DATA.

> **Scope: M-Audio "special" firmware only** — FireWire 1814 (`0x000D6C` /
> `0x00010071`) and ProjectMix I/O (`0x00010091`), i.e. the devices driven by
> `MAudioSpecialProtocol` and the timing code in this directory. Several findings
> here are *not* general AMDTP behaviour and must not be generalised to other
> families: the sync constraints in §6 come from this firmware's vendor command
> set, and the vendor engine in §4 is one 2007 kext's implementation. The
> family-agnostic parts are flagged where they appear — §2's offset arithmetic
> and §5's Linux transfer delay hold for any 48 kHz blocking-mode AM824 stream.
>
> Related: `Protocols/AVC/AVC_DEVICE_HAZARDS.md` (H1 command surface, H8 start
> choreography), `Protocols/BeBoB/MAudioSpecialFormation.hpp` (geometry).

**No code was changed for this note.** Everything below is measurement,
decompilation, or reference reading, each labelled as such.

---

## Standing conclusions

| Claim | Status | Basis |
|---|---|---|
| ASFW's SYT phase was wrong, and was fixed | **Settled** | `firebug-asfw.txt` 13.79–14.46 cyc → `last.txt` 4.69–5.37 cyc, across `8aefed6a` |
| SYT offsets always ending `0x00` is correct, not drift | **Settled** | Linux emits the identical three offsets — see [SYT byte pattern](#the-syt-byte-pattern-is-arithmetic-not-drift) |
| ASFW composes SYT anchored to the packet's transmit cycle | **Settled** | `ASFWAudioDriverZts.cpp:296` |
| DBC advancing across the cadence NO-DATA is a real protocol error | **Settled** | Present in every capture we have; fixed in tree by `91f7ad0d`, **not yet re-captured** |
| The vendor kext also advances DBC across NO-DATA | **Open** | Decompilation only; the vendor capture contains no channel-0 isoch packets |
| The 1814 is always the sync master; our SYT is not its clock | **Settled** | `special_clk_types[]` has no SYT entry; `SND_BEBOB_CLOCK_TYPE_SYT` is never read — see [§6.5](#65-nothing-is-syt-clocked-and-the-clock-source-is-not-acted-on-at-start) |
| The vendor clock frame is illegal while streams run | **Settled** | `-EBUSY` guard, `bebob_maudio.c:179-181` — see [§6.1](#61-the-clock-command-is-illegal-while-either-stream-runs) |
| The vendor re-syncs two ways: HW re-anchor (internal) and SYT replay (external sync) | **Settled** | `OutputDCLCallback` window check; shared SYT ring at `shared+24` — see [§7.2](#72-the-vendor-has-two-re-sync-mechanisms-not-one) |
| The transmit SYT accumulator is seeded to 0, not from the clock | **Settled** | `StartAudio` passes 0 to the output `ResetPort`; vtable slot 40 confirmed — see [§7.1](#71-where-the-start-time-cycle-read-actually-goes) |
| CIP header *content* is probably not why the device withholds | **Strong inference** | Linux starts this device with `header_length = 0` packets — see [§7.4](#74-the-decisive-one-linux-starts-the-device-with-empty-packets) |
| The firmware has a `Waiting for sync` streaming state that emits NO-DATA | **Settled** | `sub_2011EC00` state enum — see [§9.2](#92-the-firmware-names-the-state-we-are-stuck-in) |
| Our `clk_src = 0x00` operand matches the vendor's blank-slate frame | **Settled** | `SetBlankSlateClockSource` → internal selector 3 → wire operand 0; mapping is swapped (§8.4) |
| The vendor runs the clock sequence twice; the second pass ships the mixer levels and we never send it | **Settled** | `if (!a3)` branch calls `FWSettingsLevels::SendToDevice`; matches the `MAUDIO_PARAM` burst after the second frame (§8.4) |
| Why the device withholds its own stream | **Open** | Content hypotheses weakened by §7.4; `Waiting for sync` + `clk_src` now the leading mechanism |

---

## 0. Defect register — what is wrong in the ASFW 1814 path

The actionable summary. Detail in the sections referenced.

| # | Defect | Status | Evidence |
|---|---|---|---|
| D1 | SYT lead was ~14 cycles instead of ~4 | **fixed and verified** (`8aefed6a`) | `firebug-asfw.txt` 13.79–14.46 → `last.txt` 4.69–5.37 (§1, §2) |
| D2 | DBC advanced by 8 across the cadence NO-DATA | **fixed, NOT verified on the wire** (`91f7ad0d`) | every committed capture predates the fix (§3) |
| D3 | Only the **blank-slate half** of the vendor's clock choreography is performed; the completing pass that ships the mixer levels is never sent | **leading candidate; verified against kext + wire** | `SetClockSourceInternal` `if (!a3)` branch calls `FWSettingsLevels::SendToDevice`, seen on the wire as the `MAUDIO_PARAM` burst after the *second* clock frame (§8.4) |
| D4 | The `MAUDIO_PARAM` window is never written | **same defect as D3, seen from the other side** | vendor writes 42 registers; FFADO writes the whole `0x00`–`0x9c` block because they are write-only with unknown power-on state (§8.4) |
| D5 | Device state is never read; health is reported as belief | **blind spot** | `ReadClockHealth` returns cached values; the non-FCP lock byte at `0xffc700600000+82` is never read (§6.4) |
| D6 | Rate is asserted, not read back first | **ordering difference vs Linux** | Linux `special_get_rate` then re-applies that value (§8.2) |
| D7 | No acceptance gate existed for captures | **closed** | `tools/1814/firebug_syt_check.py` + `amdtp_tx_reference.py` |

### The pattern behind D2 and D3

Both come from the same failure mode: **a behaviour was copied from one reference
without the context that made it correct there.**

- **D2** took Linux's `SND_BEBOB_QUIRK_WRONG_DBC` — documented *"Only for
  in-stream"*, read only in `parse_ir_ctx_header`, a tolerance for what the device
  does **to us** — and applied it as our **transmit** behaviour.
- **D3** took the vendor kext's `clk_src = 0` operand without the 42
  `MAUDIO_PARAM` writes, 32 Processing-FB crosspoints and repeated clock command
  that accompany it in the vendor's own sequence.

Neither was careless; both cite their source accurately. The error in each case is
*direction and context*, not the reading. Worth a standing check when adopting
anything from a reference: which side of the wire does this describe, and what
else in that reference's sequence makes it safe?

---

## 1. Measured state of the two ASFW captures

Both from `tools/1814/`, same analyser, bus-cycle offset recovered from
FireBug's own `(CT s:cccc)` anchor lines by `firebug_parse.py --syt`.

| Capture | ch0 SYT lead | SYT step | DBC across NO-DATA | Device stream (ch1) |
|---|---|---|---|---|
| `firebug-asfw.txt` (older) | 13.79 – 14.46 cyc | 4096 ✓ | +8 | NO-DATA, DBC `0x00` |
| `last.txt` (current) | 4.69 – 5.37 cyc | 4096 ✓ | +8 | NO-DATA, DBC `0x00` |

Reference bands: Linux 4.17–4.83 cycles, vendor kext 3–5 cycles (§4).

The lead moved into band and **the symptom did not change**. That eliminates
SYT phase as the explanation. It does not implicate anything else on its own.

> **Both captures predate `91f7ad0d`.** The DBC fix is in the tree and has never
> been observed on the wire. Nothing else in this note is worth acting on before
> a fresh capture exists.

---

## 2. The SYT byte pattern is arithmetic, not drift

A reasonable-looking objection: every SYT in `last.txt` ends in `00` —
`0x3a00`, `0x5200`, `0x6600`, `0x7a00` — which reads like a synthetic counter
rather than a real timestamp.

It is forced by the arithmetic. At 48 kHz the SYT step is 4096 ticks and a cycle
is 3072, so `gcd(4096, 3072) = 1024` and the offset field can only ever take
three values. Linux's transfer delay is ≡ 512 (mod 1024), which places those
three at 512 / 1536 / 2560 — `0x200`, `0x600`, `0xA00`. Low byte `00` in all
three.

Derived from `initial_state[CIP_SFC_48000] = { 6, 1024 }` (`amdtp-stream.c:1751`),
`calculate_syt_offset()` (`:410-446`) and `compute_syt()` (`:1004-1011`):

```
 n  syt_offset  ->  +delay   cyc carry  offset  low byte
 0      2048   ->   14848   +4           2560  0x00
 1  NO_INFO      (cadence empty)
 2         0   ->   12800   +4            512  0x00
 3      1024   ->   13824   +4           1536  0x00
 4      2048   ->   14848   +4           2560  0x00
 5  NO_INFO      (cadence empty)
 ...

Linux 48k SYT offsets: [512, 1536, 2560]
ASFW  48k SYT offsets: [512, 1536, 2560]     (0x5200, 0x6600, 0x3a00)
```

Same three values, same 3-data : 1-empty cadence, same `+4` cycle carry. The
observed `0x3a00 → 0x5200` step is `2560 + 4096 = 6656 = 2×3072 + 512`: cycle
+2, offset 512. Correct.

**ASFW's composition is anchored, not free-running.** `ASFWAudioDriverZts.cpp:296`
builds the value with `ComputeInternalTxSyt(sytPhaseTicks, transmitCycle)`, where
the cycle comes from `txExecutionTimeline.AnchorForPacket()` — the packet's own
scheduled transmit cycle. That is the same shape as Linux's
`compute_syt(syt_offset, cycle, transfer_delay)`: accumulator supplies the
sub-cycle phase, hardware supplies the cycle.

---

## 3. The live defect: DBC across the cadence NO-DATA

Visible in `last.txt` at 48 kHz, channel 0:

```
114:0871  00070090   DBC 0x90   DATA     (232 B = 8 blocks × 7 quadlets)
114:0872  00070098   DBC 0x98   NO-DATA  (8 B, 0 blocks)
114:0873  000700a0   DBC 0xa0   DATA
```

Linux for the same cadence emits `0x90(D), 0x98(N), 0x98(D)` — the empty carries
the DBC that the *next* data packet will use, because
`pool_blocking_data_blocks()` sets `data_blocks = 0` for it
(`amdtp-stream.c:359-363`) and DBC advances by `data_blocks`. FFADO agrees:
`fillNoDataPacketHeader` returns 0 under the comment *"DBC is not increased"*
(`AmdtpTransmitStreamProcessor.cpp:394`).

The receiving device computes expected-next as `0x90 + 8 = 0x98`, receives
`0xa0`, and sees an 8-block discontinuity — once per group, 2000 times a second.
That is the "inadequate sequence" `bebob_stream.c:637` names as a reason a device
withholds its own transmit stream.

Fixed by `91f7ad0d`. Unverified on the wire.

---

## 4. The vendor kext's SYT engine (decompiled)

M-Audio does not use Apple's engine — the kext ships
`com_m_audio_MakeAppleFWAudioGoAway` to out-match it and carries zero
`AM824NuDCLWrite` symbols. Its own stack is `m_audio_b_FWDCLOutputProgram` over
`m_audio_FWb::TPacketPool`. Four functions carry the timestamp story.

### Seed — `FWBaseEngine::StartAudio` @ 0x4bca

```c
Bus->getCycleTime(Bus, cycleTime1);
clock_interval_to_deadline(20, 1000000, &result);      // 20 ms
v3 = AddFWCycleTimeToFWCycleTime(cycleTime1[0], 0xA0000);
                                   // 0xA0000 >> 12 = 160 cycles = 20 ms
```

Reads the live bus clock, schedules the DCL program 160 cycles out, and records
the matching host deadline for the CoreAudio timestamp.

### Per-rate constants — `FWDCLProgram::SetPacketParameters` @ 0x272f4

IDA already carried the parameter types (`SYTInterval`, `SYTIncrement`,
`FDFValue`), so this table is unambiguous.

| Rate | SYT_INTERVAL | SYTIncrement | SFC | exact ticks = 24576000·N/rate |
|---|---|---|---|---|
| 32000 | 8 | 6144 | 0 | 6144 exact |
| 44100 | 8 | 4458 | 1 | 4458.2312925… |
| 48000 | 8 | 4096 | 2 | 4096 exact |
| 88200 | 16 | 4458 | 3 | 4458.2312925… |
| 96000 | 16 | 4096 | 4 | 4096 exact |
| 176400 | 32 | 4458 | 5 | 4458.2312925… |
| 192000 | 32 | 4096 | 6 | 4096 exact |

### Two rational generators — `FWDCLProgram::SetupCIP` @ 0x27102

The 44.1 kHz family cannot use an integer tick step, so the vendor runs a
Bresenham accumulator per SFC class, plus a second independent accumulator that
decides which packets are NO-DATA.

| SFC class | NO-DATA gen | SYT frac gen | implies |
|---|---|---|---|
| 0 — 32k | 1 / 2 | 1 / 1 | 4000 data pkt/s × 8 = 32000 ✓ |
| 1,3,5 — 44.1k family | 199 / 640 | 339 / 441 | 5512.5 × 8 = 44100 ✓ |
| 2,4,6 — 48k family | 1 / 4 | 1 / 1 | 6000 × 8 = 48000 ✓ |

Both check out. NO-DATA fraction 199/640 = 0.3109375 leaves 0.6890625 × 8000 =
5512.5 data packets/s, at 8 blocks each = exactly 44100. The SYT accumulator
steps +339 mod 441, so 102 of every 441 packets take `base+1`, and
4458 + 102/441 = 4458.2312925… — the exact tick rate, nothing rounded away.

### Hardware re-anchor — `FWDCLOutputProgram::OutputDCLCallback` @ 0x263c2

The accumulator is a full 32-bit FireWire cycle-time value, checked every
callback against the DCL segment's own hardware timestamp:

```c
v16 = field307 + ((timestamp >> 12) & 0x1FFF);   // one buffer ahead (683 cyc @ 48k)
v18 = accumulatorCycle - v16;                    // wrapped to ±8000
if ( (unsigned)(v18 - 3) >= 3 )                  // outside {3,4,5}?
    field309 = (timestamp & 0xFE000000)
             | (((v16 + 4) << 12) & 0x1FFF000)   // re-seed to +4 cycles
             | (accumulator & 0xFFF);            // keep sub-cycle phase
```

So the vendor holds its timestamp **3–5 cycles ahead of the packet's real
transmit cycle**, re-seeding to +4 whenever it drifts out. The SYT field is the
low 16 bits of that accumulator — `CalcIsochPacketHeaders` @ 0x2519c takes it as
a `uint16` and byte-swaps it into CIP quadlet 1, which is exactly the
4-bit-cycle + 12-bit-offset layout.

### Open: the vendor's DBC on empty packets

In `OutputDCLCallback` the DBC byte is bumped outside the data/no-data branch:

```c
LABEL_48:
    CalcIsochPacketHeaders(this, &cip[i], syt16, *(BYTE*)(this+1232), hasData);
    ...
    *(BYTE *)(this + 1232) += v50;      // v50 = SYT_INTERVAL = 8, unconditional
```

Read literally that means the vendor advances DBC by 8 on every packet including
the cadence empty — the opposite of Linux, FFADO, and `91f7ad0d`.

**This is not established.** It rests on inferred Hex-Rays field semantics
(`this+1232` identified as DBC only by its use as the fourth argument to
`CalcIsochPacketHeaders`, which ORs it into the low byte of CIP quadlet 0), and
`tools/1814/firebug.txt` contains **no channel-0 isoch packets at all** — only
four channel-1 NO-DATA packets — so the vendor's DBC was never observed. It must
not be used to argue against the DBC fix, which does have direct wire evidence.

Unresolved sub-question: whether `SetValidPackets` / `AdjustOutputDCL` cause
those no-data slots to be transmitted at all, or to be skipped in the DCL
program.

---

## 5. Linux computes the same lead, then replays instead

Nominal figure (`amdtp-stream.c:29, :288, :292`):

```c
#define TRANSFER_DELAY_TICKS  0x2e00          // 11776 ticks, 479.17 µs
s->transfer_delay = TRANSFER_DELAY_TICKS - TICKS_PER_CYCLE;   // 8704
if (s->flags & CIP_BLOCKING)
    s->transfer_delay += TICKS_PER_SECOND * s->syt_interval / rate;

// 48 kHz : 8704 + 4096 = 12800 ticks = 4.167 cycles
// 44.1kHz: 8704 + 4458 = 13162 ticks = 4.284 cycles
```

For BeBoB that is only the fallback. `bebob_stream.c:644` starts the domain with
`replay_seq = true`, so Linux derives the host's transmit SYT from the device's
own received sequence: `make_association()` (`amdtp-stream.c:2009`) pairs each
OUT stream with an IN stream, `pool_seq_descs()` (`:560`) switches from
`pool_ideal_seq_descs` to `pool_replayed_seq` once the device's sequence is
cached, and `irq_target_callback_skip()` (`:1588`) withholds real transmission
until that cache is more than half full.

**Neither reference free-runs.** The vendor anchors to its own DMA hardware
timestamp; Linux anchors to the device's timestamps. A correct constant lead is
necessary but is not the same as being anchored — though §2 confirms ASFW is in
fact anchored per packet.

Relevant to the symptom, `bebob_stream.c:636-643`:

> *"In the early stage of packet streaming, any device transfers NODATA packets.
> After several hundred cycles, it begins to multiplex event into the packet…
> Some devices are strictly to generate any discontinuity in the sequence of tx
> packet when they receives inadequate sequence of value in syt field."*

Several hundred cycles is tens of milliseconds. Our device is still empty
**11.5 seconds** after channel 1 goes active.

---

## 6. The special-firmware sync machinery in Linux

This is more constrained than a list of clock sources suggests. Five separate
mechanisms, and the ordering rules between them are load-bearing.

### 6.1 The clock command is illegal while either stream runs

`avc_maudio_set_special_clk()` refuses up front (`bebob_maudio.c:179-181`):

```c
if (amdtp_stream_running(&bebob->rx_stream) ||
    amdtp_stream_running(&bebob->tx_stream))
	return -EBUSY;
```

So `clk_src`, `dig_in_fmt`, `dig_out_fmt` and `clk_lock` can only be changed with
**both** streams stopped. Every ALSA control that touches them — Clock Source,
Digital Input Interface, Digital Output Interface — inherits that constraint, and
each one re-sends the whole four-operand frame because the four fields share a
single command.

*ASFW complies*: `last.txt` puts the `04 00 04` frame at `088:2605`, roughly 14
seconds before channel 0 goes active at `102:5046` — by accident of ordering, not
by a guard.

**Correction — this is Linux policy, not a device restriction.** The vendor kext
sends the same frame *while the device is streaming*: `072:1766` and `076:2690`,
both after channel 1 goes ACTIVE at `071:2957`, and the device `ACCEPTED` both
(+11.2 ms, +6.5 ms). So the guard above is `avc_maudio_set_special_clk`'s own
conservatism. Practically this means clock parameters can be changed live for
experiments (§8.3) without tearing the stream down.

### 6.2 Linux fakes "not running" to make its own discover-time call legal

`amdtp_stream_running(s)` is just `!IS_ERR(s->context)`
(`amdtp-stream.h:244-247`). At discover the streams have never been initialised,
so the guard above would read uninitialised memory. `snd_bebob_maudio_special_discover`
writes a poison value into both contexts first (`bebob_maudio.c:273-276`):

```c
/* initialize these parameters because driver is not allowed to ask */
bebob->rx_stream.context = ERR_PTR(-1);
bebob->tx_stream.context = ERR_PTR(-1);
err = avc_maudio_set_special_clk(bebob, 0x03, 0x00, 0x00, 0x00);
```

The comment is about a different thing (the driver cannot query the device), but
the two lines exist purely to satisfy §6.1's guard on the very first call.

### 6.3 The clock source is never read back — it is belief

`special_clk_get()` returns the cached `params->clk_src` (`:347-352`); so does
`special_dig_out_iface_ctl_get()` for `dig_out_fmt` (`:533-542`). The only field
genuinely queried from the device is the digital *input* interface, via audio
selector function block 4 (`:457`). Everything else is driver-side state that may
only change alongside a clock command that actually succeeded — which is exactly
what `MAudioSpecialProtocol.hpp`'s member comment already asserts.

Sources (`:341-361`): 0 = Internal with digital mute, 1 = Digital (S/PDIF or
ADAT), 2 = Word Clock, 3 = Internal. Linux's discover writes `clk_src=3`;
**ASFW and the vendor both send `clk_src=0`** — confirmed in both traces, so we
match the vendor here rather than Linux. `clk_lock` ("lock these settings") is 0
everywhere.

### 6.4 The meter block is the only real device-state read

`check_clk_sync()` (`:145-164`) block-reads 84 bytes from `0xffc700600000` and
tests `buf[82] != 0xff`. Not merely a boolean — when locked, that byte equals the
SFC of the FDF the device is running, so it reports lock **and** rate.

It backs the read-only "Sync Status" control and is polled by userspace on
demand. **Linux never polls it itself and never gates streaming on it.** Sync
status is additionally marked stale by `snd_ctl_notify(... ctl_id_sync)` after a
successful clock command (`:218-220`) and after `special_set_rate` (`:333-335`).

### 6.5 Nothing is SYT-clocked, and the clock source is not acted on at start

`bebob_maudio.c:22-24` — *"The single stream is OK for the other devices even if
the clock source is not SYT-Match (I note no devices use SYT-Match)."*

Stronger than the comment: `SND_BEBOB_CLOCK_TYPE_SYT` is assigned exactly once
(`bebob_stream.c:204`) and **never read anywhere in the driver**. And in
`snd_bebob_stream_start_duplex` the result of `snd_bebob_stream_get_clock_src()`
is stored in `src` at `:619` and never used again — it is an early-failure probe,
not a decision input. For special firmware that probe cannot even fail, because
`special_clk_get` just copies a cached byte.

Also note what `special_clk_types[]` contains: two `INTERNAL` entries and two
`EXTERNAL` entries, no `SYT`. So `get_clock_src` can never return SYT for the
1814 under any user selection.

### What this means for the empty capture stream

**The 1814 is always the synchronisation master.** Our SYT is a presentation
timestamp it consumes for buffer alignment, not a clock it locks to. A device
that is its own master should be transmitting its own events regardless of our
stream's timing — which sharpens the puzzle rather than explaining it, and raises
the value of §6.4: if `buf[82] == 0xff` the device has no lock and will produce
no events no matter what our CIP stream looks like.

That read is the cheapest unused diagnostic available — a plain block read on the
freeze-prone device's **non**-FCP path.

---

## 7. Cross-validation: Linux against the vendor kext

### 7.1 Where the start-time cycle read actually goes

`FWBaseEngine::StartAudio` reads the hardware clock and propagates it — but not
where you would expect. Resolved from the vtable rather than inferred: the vptr
points at `vtable+16`, so the call at `0x4c7b` (`call qword ptr [rax+130h]`) is
slot 40 = `ResetPort(uint)`, and `0x4cd8` (`[rax+138h]`) is slot 41 = `Start()`.

```c
Bus->getCycleTime(Bus, cycleTime1);
clock_interval_to_deadline(20, 1000000, &result);            // 20 ms
v3 = AddFWCycleTimeToFWCycleTime(cycleTime1[0], 0xA0000);    // now + 160 cycles

for each DCL program:
    v8 = safeMetaCast(prog, FWDCLOutputProgram) ? 0 : v3;    // note the polarity
    prog->ResetPort(v8);                                     // slot 40
for each DCL program:
    prog->Start();                                           // slot 41
```

and `FWDCLProgram::ResetPort(bool talking, uint startCycle)` opens with:

```c
*((_DWORD *)this + 309) = a3;      // field309 — the SYT accumulator — seeded here
```

So the hardware cycle time **does** seed a SYT accumulator, but only the *input*
program's; the **output** program is seeded with `0`. The receive port is
additionally armed to that cycle (`startState = a3 >> 12`, `startMask = 0x7FFF`),
while the transmit port gets `startState = a3 = 0`, `startMask = 0x8004000`.

The transmit accumulator is therefore **not** seeded from the clock. It starts at
zero, `Start()` primes four packet groups by calling `OutputDCLCallback(i, 1)`
with the priming flag set — which skips the re-anchor block entirely — and the
first *non*-priming callback then slams field309 onto the hardware timestamp.
The seed is deliberately not load-bearing; the re-anchor is.

> The `createLocalIsochPort` argument semantics (`startEvent/startState/startMask`)
> are inferred. `references/IOFireWireFamily.kmodproj/` is **not** present in this
> checkout, so they could not be checked against Apple's source.

### 7.2 The vendor has two re-sync mechanisms, not one

**A — internal-mode hardware re-anchor** (default; `this+1560 == 0`). The window
check in §4: hold field309 3–5 cycles ahead of `(DCL timestamp cycle + field307)`,
re-seed to +4 otherwise. Runs on every non-priming callback.

**B — external-sync SYT replay** (`this+1560 == 1`, set by
`FWEngine2::SetExternalSyncEnabled`, which writes through to the output program).
This is a genuine replay path and it is structurally Linux's:

- `FWDCLProgram::Initialize(device, FWDCLSharedSyncData*)` stores a **shared
  struct at `this+128`** that both DCL programs hold.
- The **input** program captures every received SYT into a ring at `shared+24` —
  2048 entries of `uint16`, masked `& 0x7FF`, byte-swapped from the wire with
  `__ROL2__(v, 8)` — and publishes its write index to `shared+32`.
- The **output** program reads at `shared+36` and advances field309 by the
  *device's* inter-SYT delta rather than the nominal rate:

```c
v27 = deviceSyt;  v28 = field310;                    // previous device SYT
v30 = (v28 <= v27) ? Subtract(v27, v28)
                   : Add(Subtract(0xFC00, v28), v27);   // wrapped delta
field309 = AddFWCycleTimeToFWCycleTime(field309, v30);
field310 = v27;
```

- **Engage gate:** replay is used only if `writeIdx - readIdx >= packets + 4`;
  otherwise `v53 = 0` and it falls back to the internal generator.
- **Catch-up:** if more than `3 × packets` behind, the read index jumps forward to
  `writeIdx - 4 - packets`.
- **Realign:** it then forward-searches the ring for a device SYT whose cycle
  lands in an acceptable window relative to `(timestamp cycle + field307)`.
- A device `0xFFFF` passes straight through as a NO-DATA output packet.

### 7.3 Side by side

| Concern | Linux | M-Audio kext | Agree |
|---|---|---|---|
| Nominal lead @48k | `transfer_delay` 12800 ticks = 4.17 cyc | held 3–5 cyc, re-seed +4 | ✓ |
| 44.1 k fractional rate | phase accumulator, `+1386` with a 13/147 correction (`amdtp-stream.c:429-437`) | Bresenham 339/441 on base 4458 | ✓ same exact rate, different mechanics |
| Cadence generator | `syt_offset >= 3072` → `CIP_SYT_NO_INFO` → `data_blocks = 0` | independent accumulator, 199/640 @44.1k, 1/4 @48k | ✓ same ratios |
| SYT composition | `compute_syt(offset, packet cycle, delay)` | low 16 bits of a cycle-time accumulator re-anchored to the DCL timestamp | ✓ both hardware-anchored |
| Replay of device SYT | `replay_seq`, `make_association`, `pool_replayed_seq` | shared ring + delta replay | ✓ same idea |
| Replay engage gate | `cached_cycles > count && > cache_size/2` | `writeIdx - readIdx >= packets + 4` | ✓ same idea |
| Replay fallback | `pool_ideal_seq_descs` | internal rational generator | ✓ |
| **When replay is on** | **always**, for every BeBoB device (`bebob_stream.c:644` passes `true` unconditionally) | **only** when external sync is selected | ✗ **diverge** |

The divergence is coherent rather than a contradiction. On internal clock the
device runs at nominal rate, so the vendor free-runs and lets the hardware
re-anchor absorb drift; on an external clock (S/PDIF, ADAT, Word Clock) the rate
is not nominal and the host must follow the device, so replay engages. Linux
takes the simpler route of always replaying.

**Relevant to us:** ASFW runs `clk_src = 0` (Internal with digital mute), which
under the vendor's model is internal mode — free-run plus hardware re-anchor, no
replay. That is the path `ASFWAudioDriverZts.cpp:296` takes, so our mode
selection matches the vendor for this clock setting.

### 7.4 The decisive one: Linux starts the device with empty packets

`irq_target_callback_skip()` withholds real transmission until the device's
sequence cache is half full, and during that phase `skip_rx_packets()`
(`amdtp-stream.c`) queues:

```c
struct fw_iso_packet params = {
	.header_length = 0,
	.payload_length = 0,
};
```

**No CIP header at all** — not a NO-DATA header, nothing. So under Linux the 1814
begins transmitting its own SYT-bearing stream while receiving nothing but empty
isochronous packets from the host.

That is a strong constraint on the open question. If the device self-starts
against literally empty packets, then **no detail of our CIP header content — not
SYT phase, not DBC continuity, not the cadence — can be what makes it withhold
its stream.** Both remaining content hypotheses are weakened at once, and the
non-content candidates move up: clock lock (§6.4), CMP/plug state, or the
signal-format handshake.

Caveat worth keeping: this is an inference from Linux's ordering being known to
work on this device, not a direct observation of our device starting under empty
packets.

---

## 8. Synthesis: why the device→host stream never starts

Reasoning from the async bring-up and the isoch packets together, using
`last.txt`.

### 8.1 What the trace eliminates

Every layer below the device's audio engine is provably healthy, and most of it
is byte-identical to the vendor's.

| Layer | Evidence | Verdict |
|---|---|---|
| IRM | ch0 + ch1 allocated, 756 + 884 bandwidth units granted (`102:4727`–`102:4827`) | fine |
| CMP — playback | `iPCR[0] 80000000 → 81000000` (p2p=1, ch=0) | fine |
| CMP — capture | `oPCR[0] 80000080 → 81018080` (p2p=1, ch=1, S400) — **byte-identical to the vendor's lock** | fine |
| AV/C | clock/format, Selector FB 4 ×2, OUT/IN signal format ×4 — all `ACCEPTED` | fine |
| Start trigger | ch1 goes ACTIVE at `102:7754`, **116 ms after** our post-DMA IN signal format at `102:7638` | **worked** |
| Device idle packet | `02020000 9002ffff` — DBS=2, DBC=0x00, AM824, FDF = negotiated rate, SYT=`0xFFFF`; **byte-identical to the vendor's idle packet** | normal |
| Our transmit stream | SYT lead 4.69–5.37 cyc, exact 4096 step, 3:1 cadence, DBS=7 (6 PCM + 1 MIDI = correct 48 kHz S/PDIF playback geometry) | well-formed |

Two of those deserve emphasis. **The Linux-documented "unusual" start trigger
worked** — the device began transmitting because of our post-DMA signal-format
re-send, exactly as `bebob_stream.c:648-659` describes. And **DBS=2 in the idle
stream is not a misconfiguration**: the vendor's device emits the identical
header, so this is simply what a 1814 transmits when it has no events.

§7.4 removes the remaining content hypotheses: Linux starts this device while
sending packets with `header_length = 0`, so neither our SYT phase nor the DBC
defect of §3 can be what keeps it silent. The DBC defect is real and worth having
fixed; it is not this bug.

### 8.2 What that leaves

The device is connected, transmitting, and at the right rate — its NO-DATA
packets carry `FDF = 0x02`, so it accepted 48 kHz — with DBC frozen at `0x00` for
12 seconds. Per IEC 61883-6 a transmitter emits NO-DATA when it has no events to
send. So the **stream is alive and the audio engine is producing nothing**. The
fault is device-side state, set during bring-up, not a packet-formatting error.

Three things ASFW does differently from both references, ranked by how well each
explains "stream alive, no events":

**1 — We select the clock source that has "mute" in its name.**
`MAudioSpecialProtocol::InitializeClock` sends
`MAudioClockSource::InternalDigitalMute` = `clk_src 0x00`, chosen deliberately to
copy the vendor kext. Linux never selects it: its discover writes `0x03`, plain
Internal (`bebob_maudio.c:276`), and Linux drives this device to working audio
with **no mixer initialisation whatsoever**. Index 0 is labelled
`SND_BEBOB_CLOCK_TYPE_INTERNAL /* With digital mute */` (`:342`). This is the
only divergence that names a mute, costs one byte to test, and needs no
additional mechanism to explain the symptom.

The counter-argument is that the vendor also sends `0x00` — but the vendor pairs
it with 42 `MAUDIO_PARAM` writes, 32 Processing-FB mixer crosspoints, and a
repeated clock command, none of which we do. Copying one operand out of that
sequence is not the same as reproducing it.

**2 — We never write the `MAUDIO_PARAM` window at all.**
The vendor writes 42 registers. FFADO writes the entire `0x00`–`0x9c` block at
startup (`special_mixer.cpp:74-106`) *precisely because these registers are
write-only with unknown power-on state* — the driver cannot read them back, so it
must assert every one. We assert none. Weaker than #1 because a capture stream
ought to carry raw converter data regardless of mixer routing, but that
assumption is untested on this firmware.

**3 — We assert the rate rather than reading it.**
Linux reads the device's current rate first (`special_get_rate`, STATUS on input
plug 0) and then re-applies *that* value (`bebob_stream.c:613-617, 651-659`). We
command 48 kHz outright. Weakest of the three — the device accepted it and
reports `FDF = 0x02` — but it is a real ordering difference from the reference.

### 8.3 Discriminating tests, cheapest first

1. **Read `0xffc700600000 + 82` while the stream is up.** `0xff` means no clock
   lock, and then nothing about our packets matters; any other value is the SFC
   the device is actually running. One block read, no FCP exposure. This splits
   the whole problem in half and should be done before changing anything.
2. **Send `clk_src = 0x03` (plain Internal) instead of `0x00`.** One enum value in
   `InitializeClock`, matching the configuration Linux is known to drive this
   device with — and Linux needs no mixer init to make it work.
3. **If neither, write FFADO's init block** to `MAUDIO_PARAM` `0x00`–`0x9c` before
   streaming, reproducing `Mixer::initialize()`.

Note that test 2 does not require a restart: the vendor sends the clock/format
command **while streaming** (`072:1766` and `076:2690`, both after ch1 goes ACTIVE
at `071:2957`) and the device `ACCEPTED` it both times. See the correction in
§6.1.

### 8.4 The vendor's clock choreography has two phases; we do one

Recovered from `com_m_audio_FW1814Device::SetClockSourceInternal` @0xe25c and
`SetBlankSlateClockSource` @0xe4f0, and confirmed against the wire.

**The wire operand mapping is swapped**, which is worth knowing before comparing
anyone's `clk_src` against anyone else's:

```c
switch (a2 & 3) {          // internal selector  ->  wire operand buf[6]
  case 1:  buf[6] = 1;
  case 2:  buf[6] = 2;
  case 3:  buf[6] = 0;     // internal 3 emits operand 0
  default: buf[6] = 3;     // internal 0 emits operand 3
}
```

The rest of `a2` is a packed parameter word: bit 3 → `dig_in_fmt` (buf[7]),
bit 4 → `dig_out_fmt` (buf[8]), bit 5 → `clk_lock` (buf[9]), bit 30 a validity
tag rejected below `0x40000000`.

`SetBlankSlateClockSource` passes `0x40000003 | (field81 & 0x1C)` — so internal
selector 3, wire operand **0**, digital formats carried over from the previous
state, and `clk_lock` deliberately dropped (the mask `0x1C` excludes bit 5).
**ASFW's `0x00` operand therefore does match the vendor**; that part of
`MAudioSpecialProtocol`'s comment is correct.

What does *not* match is what follows. The tail of `SetClockSourceInternal` is:

```c
  send 16-byte vendor frame
  IOSleep(300);
  field81 = a2;
  v5 = <selector FB pass>(a2, a3);
  IOSleep(300);
  if ( !a3 ) {                                   // NOT blank slate
      field81 = a2;
      <vtable+3040>(this, a2);
      FWSettingsLevels::SendToDevice(this + 1336);   // ships MAUDIO_PARAM
  }
```

So the vendor runs the sequence **twice**, and only the second pass configures the
device:

| | clock frame | selector FB | `MAUDIO_PARAM` |
|---|---|---|---|
| phase 1 — blank slate (`a3 = true`) | `072:1766` | `072:4265` | — |
| phase 2 — normal (`a3 = false`) | `076:2690` | `076:5153` | **`076:7569`+** |

`SetBlankSlateClockSource` adds `IOSleep(2500)` after phase 1 and stops. The
`MAUDIO_PARAM` burst in the capture is `FWSettingsLevels::SendToDevice` firing in
the `if (!a3)` branch of phase 2.

**ASFW implements phase 1 only.** `InitializeClock` reproduces the blank-slate
path faithfully — frame, 300 ms, selector FB, 300 ms, 2500 ms settle — and then
stops. The completing pass never runs, so the device is left in the state the
vendor treats as *not yet configured*, and no level or routing register is ever
written. D3 and D4 are therefore one defect seen from two sides, not two.

This also explains why copying the operand alone was not enough: operand `0` is
correct, but in the vendor's design it is the *opening* move of a two-phase
sequence, not a complete configuration.

---

## 9. The device firmware itself (`fw1814.bcd`)

The BridgeCo firmware image loads cleanly and is the most direct evidence
available, because it names the device's own internal states.

### 9.1 Load parameters

`fw1814.bcd` is a `bCoD` (bridgeCo Download) container. Header layout from FFADO
`bebob_dl_bcd.cpp:213-219`:

| Field | Offset | Value |
|---|---|---|
| image offset | `+0x30` | `0x70` |
| image base address | `+0x34` | **`0x20080000`** |
| image length | `+0x38` | `0xe4a9c` (936 604 B) |

Architecture is **ARM32 little-endian, ARMv5TE / ARM9E-S class** — the word at
file `+0x70` is `ee100f10` = `MRC p15,0,r0,c0,c0,0`, and the CP15 dump strings
show `R6_ProtectionRegion[]` and `R9_CodeTCMSize`, i.e. MPU + TCM, no MMU.

Two independent confirmations that the file matches the hardware we captured: its
build stamp is `20070713080440`, byte-for-byte the `BEBOB_INFO+0x20` value read
off the wire in §4, and `0x20080000` is exactly the previously-unnamed
`BEBOB_INFO+0x3c` field — the device reports its own load address.

Reproducing the database (IDA's binary loader defaults to AArch64 in batch mode,
so wrap the image in a minimal ELF32-ARM header rather than fighting the prompt):

```
image = bcd[0x70 : 0x70+0xe4a9c]        # then prepend a 84-byte ELF32/EM_ARM
                                        # header with p_vaddr = 0x20080000
```

Analysis via `idalib` (shipped as `idalib/python/idapro-*.whl`; activate with
`py-activate-idalib.py -d <IDA>/Contents/MacOS`). Result: processor ARM, 32-bit,
one segment `20080000-20164a9c`, **3270 functions**, Hex-Rays works.

### 9.2 The firmware names the state we are stuck in

`sub_2011EC00` is the status printer behind the `sys` shell commands. It decodes a
**streaming state machine**:

| value | string |
|---|---|
| 0 | `Stop` |
| 1 | `Idle` |
| 2 | **`Waiting for sync`** |
| 3 | `Running` |

and, from the same function, the sync-source enum:

```
Internal Sync
Internal Digital Input Sync
Adat External Sync
Spdif External Sync
Word Clock Sync
```

A device parked in state 2 transmits NO-DATA indefinitely — exactly our symptom,
now with a name and a specific mechanism rather than an inference.

**This sharpens §8.2 hypothesis 1 considerably.** Linux labels `clk_src = 0` as
*"Internal with Digital Mute"*, but the firmware's own vocabulary offers
**`Internal Digital Input Sync`** — internal rate, *digital input as the sync
reference*. If `clk_src = 0` selects that, then with nothing connected to S/PDIF
the device would sit in `Waiting for sync` forever and emit NO-DATA. That is a
complete mechanism for the observed behaviour, and it predicts that `clk_src = 3`
(plain `Internal Sync`) fixes it.

> **Not yet proven.** The mapping from the AV/C `04 00 04` operand to this
> sync-source enum has not been traced through the firmware's command handler.
> The hypothesis is strong and cheap to test (§8.3 test 2) but it is a hypothesis.

### 9.3 Device-side error flags and where they live

The `avstat` handler is `sub_201222F8`. Contrary to their `%08X` formatting these
are **single status bits**, not counters, read through pointer indirection:

| Flag | Pointer | Bit |
|---|---|---|
| `DBCMismatch` | `[[0x04000070]]` (framer block) | 0 |
| `CIPMismatch` | same word | 5 |
| `HeaderMismatch` | same word | 4 |
| `SetTgInLock` | `[[0x0400002C]]` (TGEN block) | 2 |
| `SetTgSytMiss` | same word | 3 |

`SetTgInLock` and `CIPMismatch`/`DBCMismatch` are precisely the device's own
verdict on our stream. AV1/AV2 clock source is likewise a bit test on `[R4+0x14]`
(bits 4 and 5 select Internal vs External).

**Open question worth pursuing:** whether these physical addresses are reachable
over 1394 async reads. BridgeCo already maps `BEBOB_INFO` at `0xffffc8020000` and
the M-Audio windows at `0xffc700xxxxxx`, so a mapping may exist. If it does, the
device's own lock state and CIP/DBC verdict become a block read — no UART, no
hardware modification.

### 9.4 How the device validates SYT

Partially recovered. The shape is clear; the numeric bounds are not.

**Validation is two-tier.**

*Tier 1 — silicon.* The DM1000's framer and timing generator do the conformance
checking and latch sticky status bits; the firmware only reads them (§9.3).
`SetTgSytMiss` is a timing-generator flag, and `CIPMismatch` / `DBCMismatch` /
`HeaderMismatch` are framer flags. So CIP/DBC/SYT conformance is not a software
loop the firmware could be argued out of.

*Tier 2 — the streaming driver.* A `/syt/` debug namespace of 32-bit fields,
registered by `sub_200E1E64` via `sub_200EE1C4` / `sub_200EE098`
(`R0`=class, `R1`=name, `R2`=struct offset, `R3`=size):

| Name | Offset | Class | Reading |
|---|---|---|---|
| `SytOffset` | `0x44` | `'S'` | expected offset |
| `SytCorr` | `0x48` | `'S'` | correction applied |
| `pktSytDiff` | `0x84` | `'E'` | per-packet SYT difference |
| `SytDiffErr` | `0x94` | `'E'` | difference out of range |
| `CtrDiffErr` | — | `'E'` | counter difference out of range |
| `linStartSyt` | `0xA8` | `' '` | stream start SYT |
| `outStartSyt` | `0xAC` | `' '` | stream start SYT |

alongside a matched pair of classifiers, **`pkt Future`** and **`pkt Past`**.

That pair is the tell. The device computes a per-packet difference between the
received SYT and its own expected presentation time, and classifies the packet as
too far ahead or already elapsed — a **windowed presentation-time check**, the same
model as Linux's transfer-delay reasoning, with `SytCorr` suggesting it corrects
rather than simply discards. Configuration knobs `playOutSytOffset` (printed
`%#.4x`, so a tick value) and `dbcOffsetCode` set the expectation; SYT interval
(8/16/32) and SYT match source (AV1/AV2) are separately configurable and reported
by `sub_20121B54`.

**Not recovered:** the acceptance window itself. That needs tracing writes to
struct `+0x84` / `+0x94` in the isochronous receive path, which means identifying
the struct base register there first.

> **Scope note — this cannot explain our symptom.** SYT validation governs what the
> device does with packets *we send it* (our playback). It has no bearing on
> whether the device transmits its own stream, which is governed by the sync state
> machine of §9.2. The two are separate mechanisms and should not be conflated:
> even a total SYT rejection on the receive side would not park the transmit side
> in `Waiting for sync`.

---

## 10. Open items, by value per unit of effort

1. **Re-capture with the current driver.** Every ASFW trace in the repo predates
   `91f7ad0d`. Nothing else is worth doing first.
2. **Read `0xffc700600000+82` while the stream is up.** One block read, no FCP
   exposure. Separates "device has no clock" from "device dislikes our packets" —
   the two halves the investigation cannot currently tell apart.
3. **Capture the vendor kext with isoch recording enabled.** Settles both the
   vendor's real SYT lead (the 3–5 cycle window is decompiled, never observed)
   and the vendor DBC question in §4.
4. **Check whether our lead is held or merely started correctly.** §2 shows the
   composition is anchored, so this is likely fine, but a longer capture would
   show a slowly walking lead if the anchor is not actually closing the loop.

---

## Method and provenance

- Decompilation: IDA Professional 9.3 headless (`idat -A -S`) over a copy of
  `M-AudioFireWireBeBoB.i64`. The `ida-pro-mcp:idalib` MCP server was not
  reachable from the session, so this ran through the same Hex-Rays engine via
  script rather than MCP. All field offsets are Hex-Rays indices into the object
  and were **not** cross-checked against a symbolized struct.
- Linux citations: `references/linux-sound-firewire-stack` (sparse
  `torvalds/linux` clone). FFADO: `references/libffado-2.5.0`.
- Trace figures: `tools/1814/firebug_parse.py`, bus-cycle calibration from
  FireBug's own `(CT s:cccc)` anchor lines.
