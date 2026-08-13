# M-Audio FireWire 1814 / ProjectMix — stream sync model

Status: research, nothing implemented. Decoded from the vendor kext
(`M-AudioFireWireBeBoB`, fully symbolised) and cross-checked against Linux and
FFADO. `com_m_audio_FWProjectMixDevice` reuses `__ZTV24com_m_audio_FW1814Device`
outright, so everything here covers both devices.

Addresses are file offsets in the IDA database named in the local-only
`docs/MAUDIO_1814_KEXT_RE.md`. Field references are `this + N` as a **`_DWORD`
index** unless stated otherwise. Behaviour only — no code is reproduced.

---

## 0. The headline for scoping

There are two SYT modes, and the selector is external clock sync
(`com_m_audio_FWEngine2::SetExternalSyncEnabled` @ `0x11bed` writes the engine's
sync state to the DCL output program at byte offset `0x618`, i.e. `_DWORD` index
1560, which `OutputDCLCallback` @ `0x265bb` tests).

- **Internal clock → free-run (Mode B).** This is the v1 path.
- **External clock → slave (Mode A).** Deferrable with external sync itself.

And at 48 kHz internal the free-run model degenerates to almost nothing:

| | 44.1 family | **48 kHz** |
|---|---|---|
| SYT_INCREMENT | 4458 + 102/441 | **4096, exact** |
| SYT fraction (num, den) | (339, 441) | **(1, 1) — never adds +1** |
| NO-DATA cadence (N, D) | (199, 640) | **(1, 4) — every 4th packet** |

So a 48 kHz internal-clock TX is: `SYT += 4096`, NO-DATA every fourth packet,
re-anchor when out of window. No drift estimation exists anywhere in this
design. The RX conversion is exact at 48 k too, since `4096 = 8 × 512`.

---

## 1. Packet parameters per rate

`SetPacketParameters` @ `0x272f4` maps rate → (SYT_INTERVAL, SYT_INCREMENT, FDF)
and calls `SetupCIP` @ `0x27102`. 44100 is also the fall-through default.

| rate | SYT_INTERVAL | SYT_INCREMENT | FDF/SFC |
|---|---|---|---|
| 32000 | 8 | 6144 | 0 |
| 44100 | 8 | 4458 | 1 |
| 48000 | 8 | 4096 | 2 |
| 88200 | 16 | 4458 | 3 |
| 96000 | 16 | 4096 | 4 |
| 176400 | 32 | 4458 | 5 |
| 192000 | 32 | 4096 | 6 |

`SYT_INCREMENT = SYT_INTERVAL × 24576000 / rate`, truncated. The 48 k family is
exact; the 44.1 family carries its fraction in the accumulator below.

## 2. Mode B — free-run, two exact rational accumulators

Both live in `OutputDCLCallback` @ `0x263c2`, in the `else` branches of the
slave-mode test.

**NO-DATA cadence** (`+303` acc, `+304` N, `+305` D):

```
acc += N;  if (acc > D) { acc -= D;  emit NO-DATA }
```

**SYT step** (`+302` acc, `+300` num, `+301` den, `+298` INC):

```
acc += num;
if (acc <= den)  step = INC + 1;
else            { acc -= den;  step = INC; }
```

The step is then applied to the running SYT (`+309`) in three adds —
`INC>>1`, `INC>>1`, `INC&1` — which totals `INC`; the split only avoids overflow
in the cycle-time add helper.

**The load-bearing detail: SYT advances only on DATA packets.** The NO-DATA
branch jumps past the step entirely. Read without that, the arithmetic looks
wrong by ~0.54 ticks/packet; with it, it is exact:

> At 44.1 kHz, 441 of every 640 packets carry data. 640 cycles = 1,966,080 ticks.
> Per DATA packet that is 1,966,080 / 441 = 4458.2313 ticks, and the accumulator
> yields 4458 + 102/441 = 4458.2313, since `102/441 = 1 − 339/441`.
> Frame check: 441 × 8 frames / 0.08 s = 44100. Both exact.

## 3. Mode B — re-anchor

Before the per-packet loop, and only on the first call of a group:

```
target = lead(+307) + cycleNow
delta  = (SYT(+309) >> 12 & 0x1FFF) − target      // wrap-corrected at 8000
if (delta − 3) unsigned>= 3:                       // i.e. outside [3, 6)
    SYT = (cycleTime & 0xFE000000)
        | (((target << 12) + 0x4000) & 0x1FFF000)
        | (SYT & 0xFFF)                            // sub-cycle offset PRESERVED
```

Conceptually our TxAlign. The preserved `SYT & 0xFFF` is the device's constant
sub-cycle offset, which we already model separately.

## 4. Mode A — slave to the device (external sync only)

Mirrors the device's own stream 1:1, so no rate estimation exists at all:

- received SYT `0xFFFF` (device NO-DATA) → emit NO-DATA, advance the read cursor
- otherwise `txSYT += received[n] − received[n−1]`, wrapping at `0xFC00`
  (16 cycles × 3072), and `+310` remembers the last received SYT

Entry conditions, checked per group:

- requires `writeIdx − readIdx >= packetsPerGroup + 4` buffered received SYTs
- if `writeIdx − readIdx > 3 × packetsPerGroup`, drop backlog:
  `readIdx = writeIdx − 4 − packetsPerGroup`
- then phase-align: skip forward (past `0xFFFF` entries) until the chosen
  received SYT lands in the acceptable cycle window relative to `now + lead`

The received-SYT ring is at `shared + 24`, 2048 `uint16` entries indexed
`& 0x7FF`.

## 5. RX cursor — from the cycle timer, not from SYT

`ResyncInputBuffer` @ `0x2610e`. This is the piece that differs most from ASFW,
which derives RX from SYT and descriptor timestamps.

```
reference = shared[+12]                    // planted by the TX callback, see §6
if wrapped/no-advance guard fails: return  // 8000-cycle wrap, 2000-cycle
                                           // small-delta case, delta <= 0 exits
d      = SubtractFWCycleTime(now & 0x1FFFFFF, reference & 0x1FFFFFF)
ticks  = (d & 0xFFF) + 3072 * ((d >> 12) & 0x1FFF) + 24576000 * (d >> 25)
frames = (ticks * SYT_INTERVAL + SYT_INCREMENT/2) / SYT_INCREMENT   & 0x1FFF
shared[+64] = frames * 4 * (pcm + midi)
```

Exact rational with round-half, so it cannot accumulate drift. `24576000` and
`3072` appear as literals, confirming the tick base.

## 6. Timestamps and the reference the RX side uses

In the first half of `OutputDCLCallback`:

- `CycleTimeToHostNanos` @ `0x26284` converts the captured group timestamp, and
  the pair is published into a double-buffered slot pair
- **at group 3 with the group counter exactly 2**, the cycle time is planted into
  `shared+12` and `shared+17` is set — this is the reference §5 resyncs against,
  and it is planted by the **transmit** callback
- from group counter ≥ 3 the host nanos go through `nanoseconds_to_absolutetime`
  into device vtable slot `2784`, the same slot `StartAudio` uses — the engine
  timestamp

`AddTimestampedTransferPackets` @ `0x6ad8` is selected in `BuildPacketDCLSegment`
@ `0x2587a` whenever a callback is supplied.

## 7. CIP templates

`SetupCIP` @ `0x27102` builds both quadlets once, then patches per packet:

- `+1176` = `(localNodeID & 0x3F) << 24 | (pcm + midi) << 16` — SID and DBS.
  DBC is OR'd in per packet; `+1232` is the DBC, incremented by SYT_INTERVAL.
- `+1180` = `0x90000000` — second-quadlet marker plus FMT `0x10` (AM824)
- `+1184` rate, `+1188` FDF, `+1192` SYT_INCREMENT, `+1196` SYT_INTERVAL

`CalcIsochPacketHeaders` @ `0x2519c` emits, byte-assembled into wire order:

```
hdrs[0] = bswap32(CIP0template | dbc)
hdrs[1] = 0x90 | FDF | SYT_hi | SYT_lo     (data)
hdrs[1] = 0x90   FF     FF       FF        (NO-DATA)
```

The NO-DATA branch is literally `HIBYTE(0x90000000) - 256`, landing FDF `0xFF`
and SYT `0xFFFF` — IEC 61883-6 NO-DATA, constructed rather than inferred.

## 8. Start scheduling

`StartAudio` @ `0x4bca`: read `getCycleTime`, add `0xA0000` (160 cycles = 20 ms),
arm a matching 20 ms deadline. Every DCL program that is **not** an
`FWDCLOutputProgram` starts at that future cycle time; output programs start with
0, i.e. immediately. A second pass arms each program (vtable `+312`), then the
engine's `+2784`. `AddSetTagSyncBits(builder, 1, 0)` — tag 1 (CIP), sync 0 — is
emitted for the **input** direction only.

## 9. Buffer geometry and reported latency

`SetupCIP` also stores cycles-per-2048-frames = `16,384,000 / rate` with a ±2
window at `+1244`/`+1248`, and ceilings for 2048 and 4096 frames at
`+1224`/`+1228`.

`GetRoundTripLatencyForFDF` @ `0xcf30`, indexed by FDF: 210 (32 k/44.1 k default),
225 (48 k), 358 (88.2 k), 378 (96 k), 642 (176.4 k), 698 (192 k). This is a
CoreAudio-reported round trip of roughly 3.6–4.7 ms at every rate — **not** the
CIP transfer delay.

## 10. Channel geometry (sync-adjacent)

`GetDeviceChannelsForSampleRate` @ `0xc998` picks one of four tables from two
bits of the cached clock/format word (`this+324`, byte offset): bit `0x08`
(`dig_in_fmt`) selects capture, bit `0x10` (`dig_out_fmt`) selects playback,
independently.

| table | capture | playback | MIDI |
|---|---|---|---|
| S/PDIF 44.1–96 k | 10 | 6 | 1 |
| ADAT 44.1–48 k | 16 | 12 | 1 |
| ADAT 88.2–96 k | 12 | 8 | 1 |
| 176.4/192 k | 2 | 4 | 1 |

All twelve numbers match Linux `ch_table` exactly. `SetupAudioEngine` @ `0xed34`
also writes the base-rate counts inline from the same word, so the geometry is
hardcoded in two places, not one. `SwitchDigitalSignals` @ `0xe5dc` is the
re-derivation path on a digital-format change.

---

## 11. Field map

`_DWORD` index on the DCL output program unless noted.

| offset | meaning |
|---|---|
| `+298` | SYT_INCREMENT |
| `+299` | SYT_INTERVAL (also the DBC/frame step per packet) |
| `+300` / `+301` | SYT fraction numerator / denominator |
| `+302` | SYT fraction accumulator |
| `+303` / `+304` / `+305` | NO-DATA accumulator / N / D |
| `+307` | TX lead, in cycles |
| `+309` | running TX SYT |
| `+310` | last received SYT (slave mode) |
| `+312` | packets per group |
| `+1232` | DBC (byte) |
| `+1560` | slave-mode flag (byte `0x618`) |
| `shared+12` | RX reference cycle time, planted by the TX callback |
| `shared+24` | received-SYT ring, 2048 × uint16 |
| `shared+32` / `+36` | ring write / read index |
| `shared+64` | RX cursor, in bytes |

---

> **Companion:** `MAUDIO_1814_START_CHOREOGRAPHY.md` covers clock configuration,
> the start order, and the cycle-timer host time base. The RX-cursor concern
> raised in §12 below is developed there against a hardware run that hit it.

## 12. What this means for ASFW

- **Mode B is close to what we already do**: SYT = transmit cycle + lead with a
  periodic re-anchor that preserves the sub-cycle offset. The exact rational
  accumulators are *simpler* than our current drift handling, because there is
  none to do.
- **Mode A has no ASFW equivalent** and is arguably better for a device-clocked
  stream, since it removes rate estimation entirely. Out of scope until external
  sync is.
- **The RX cursor genuinely differs** — cycle-timer derived against a reference
  planted by the TX callback, rather than SYT/descriptor derived. This is the
  one part that needs real design work rather than specialisation.
- **48 kHz internal is the easy corner of this device.** Scope v1 there.

## 13. Verified vs inferred

Verified by decompilation, and where noted cross-checked against Linux or FFADO:
the mode selector, both accumulators and their constants, the DATA-only SYT
advance, the re-anchor window and preserved offset, the slave-mode delta rule and
its entry gate, the RX conversion, the CIP templates and NO-DATA construction,
the rate table, the channel tables (match Linux `ch_table`), and the start
scheduling.

Not verified: whether the 20 ms start lead is required or merely generous;
whether the `[3, 6)` re-anchor window is tuned or arbitrary; how the device
behaves if the host never re-anchors. None of these can be settled without
hardware.
