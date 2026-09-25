# DICE / TCAT: evidence, current state, and direction

**Status:** evidence base + accepted direction (2026-09-20). The direction in §4
is agreed. **Steps A, B and C have landed** (A 2026-09-20; B and C 2026-09-25,
audio-session stage S6): per-stream geometry crosses the nub, rates come from
the device, and one `DiceProfile` replaces the seven classes. **D is deferred**
to the work that raises the 48 kHz ceiling (§4.2).

**Scope.** This doc owns DICE **geometry** (profiles, resolver, rates). The DICE
**bring-up and session lifecycle** (owner, clock, stream start/stop, recovery),
and the replacement of `AudioDuplexCoordinator`, are designed in
[`AUDIO_SESSION_REDESIGN.md`](AUDIO_SESSION_REDESIGN.md) (2026-09-24), which also
extends §2.1 below to all five vendor kexts.

**Relationship to other docs.** [`DEVICE_BACKEND_UNIFICATION.md`](DEVICE_BACKEND_UNIFICATION.md)
is the protocol-neutral-interface direction, of which this is the DICE-family
instance. [`SAMPLE_RATE_EXPANSION.md`](SAMPLE_RATE_EXPANSION.md) owns rate
enablement; §2.4 here supplies the DICE geometry facts that gate it.

**Reference pinning.** Vendor kext addresses are from IDA databases under
`/Users/mrmidi/DEV/FirWireDriver/OTHER/KEXTs/` (local-only), read 2026-09-20 via
idalib. Linux/FFADO citations refer to `references/` (gitignored, local-only).
Reference stacks are GPL/LGPL — behavioural truth only, never copied code.

---

## 1. Why this document exists

DICE is the family where ASFW carries the most per-model code and where the
vendors carry the least. Three decompiled TC SDK drivers plus four recorded
register dumps now say the same thing from two directions, and that agreement is
what makes the direction in §4 safe to commit to rather than merely plausible.

---

## 2. What we know

### 2.1 The vendor drivers are one code path

`MidasFW.kext`, `AlesisFirewire.kext` and `PaeFireStudio.kext` are all rebranded
TC Applied Technologies DICE reference drivers — same symbol tree
(`tcat::dice::*`, `_DICE_STREAM_STRUCT`, `DICE_DEVICE_STRUCT`), same function
names, same control flow. **Update 2026-09-24:** `WeissFirewire.kext` (4.3.1) and
Focusrite's `Saffire.kext` (4.1.4) are the same SDK too, and MidasFW and
PaeFireStudio share 347 of 353 function bodies instruction-for-instruction. See
[`AUDIO_SESSION_REDESIGN.md`](AUDIO_SESSION_REDESIGN.md) §2.1 for the method.

**`probe` decides accept/reject and nothing else.** All three derive a model
index from the GUID — bits 39:32 must be `0x04`, then `guid >> 22` indexes a
name table — and the name is used **only in `IOLog`**:

```c
// AlesisFirewireAudio::probe  (AlesisFirewire.i64  0x1af0)
if (BYTE4(v7) != 4) goto reject;
v10 = "Alesis MultiMix Firewire";          // index 0
v11 = (unsigned int)v7 >> 22;
if (((v7 >> 22) & 0x3FF) != 0) {
    v10 = "Alesis iO Firewire";            // index 1
    if (v11 != 1) { if (v11 != 2) goto reject; v10 = "Alesis MasterControl"; }
}
IOLog("AlesisFirewireAudio: %s guid:%llx connected.\n", v10, v7);
```

`PaeFireStudioAudio::probe` (`0x1228`) is the same shape across **14** PreSonus
models. **There is no per-model behavioural branch anywhere in any of the five
drivers.** `WeissFirewireAudio::probe` is the one variation: it accepts GUID
category byte `0x00` where the others require `0x04`.

### 2.2 Geometry is read from the device, every time

`PopulateDeviceStruct` (Midas `0xc89a`, Alesis `0xd920`, PreSonus `0xc61a`)
reads the global block, then `TX_NUMBER` and `RX_NUMBER`, then loops:

```c
while (PopulateTxStruct(this, dev, i)) { if (++i >= TX_NUMBER) break; }
while (PopulateRxStruct(this, dev, j)) { if (++j >= RX_NUMBER) break; }
```

`Populate{Tx,Rx}Struct` reads, per stream and section-relative: `SIZE`,
`ISOCHRONOUS`, `SEQ_START` (RX only), `NUMBER_AUDIO`, `NUMBER_MIDI`, then the
channel-name block. Offsets match Linux `dice-interface.h` exactly.

**No host-side geometry constant exists in any of them.**

Stream-count bounds differ and are worth knowing:

| driver | DICE TX (host capture) | DICE RX (host playback) |
|---|---|---|
| Midas | `>= 3` → hard error | `> 4` → hard error |
| PreSonus | `>= 3` → hard error | `> 4` → hard error |
| Alesis | **no bound at all** | **no bound at all** |

Both bounded drivers **reject** rather than clamp. ASFW's
`kMaxAudioStreamsPerDirection = 4` is a superset of both.

**Channel-name block has two layouts**, selected by stream `SIZE`: below 326
quadlets → 256 bytes at section-relative `+0x018`; at or above → 1024 bytes at
`+0x120`. (70 quadlets standard block + 256 quadlets of names = 326.) ASFW reads
the standard offset unconditionally — see §3.3.

### 2.3 A rate change re-reads everything

There is **no rate → channel-count table** in any vendor driver. The chain is:

```
SetNewSamplingRate            (Midas 0x8ec8, Alesis 0xb4b0)
  validate index against the device-read rate mask   (_bittest)
  → RequestStreamingRestart
      → RestartStreaming      (Midas 0xdb62, Alesis 0x15c80)
          → PopulateDeviceStruct   (Midas xref 0xe56e; Alesis 0x17104-0x17146)
              → re-read TX_NUMBER / RX_NUMBER and every per-stream block
```

The device reports different channel counts at the new rate mode; the driver
simply reads them again. Linux's per-rate-mode tables (`dice-alesis.c`,
`dice-focusrite.c`) are **Linux's** workaround for caching formats, not vendor
behaviour.

### 2.4 Rate-mode geometry, from hardware

The Saffire Pro 24 DSP dump (`fixtures/DICE/spro24dsp.txt`) carries a TCAT
extension whose `current_config` enumerates **every** rate mode:

| mode | capture (DICE TX) | playback (DICE RX) |
|---|---|---|
| low (32–48k) | pcm=16 midi=1 | pcm=8 midi=1 |
| middle (88.2–96k) | pcm=**12** midi=1 | pcm=8 midi=1 |
| high (176.4–192k) | pcm=**8** midi=1 | pcm=8 midi=1 |

The channel names show the mechanism: `IP 1-4` and `SPDIF L/R` persist, **ADAT
halves then quarters** (8→4→2, S/MUX), and `Loop 1/2` drops at high. Playback is
constant. The dump states the rule directly:

> These describe the layout at EVERY rate mode. The plain TX/RX registers above
> only describe the mode the device is in now.

**Geometry is therefore rate-mode-scoped, not absolute.** Any resolved geometry
object must be keyed by rate mode.

### 2.5 There are three format-discovery sources

| source | which devices | what it yields |
|---|---|---|
| **EAP `current_config`** | Pro 24 DSP (`dynamicStreamFormat=true`, `maxTx/RxStreams=2`, TCD2210) | all three modes, read once, no rate switch needed |
| **plain TX/RX registers** | Venice, StudioLive, MultiMix — all report `extension space <absent>` | the current mode only; must re-read after a switch |
| **supplied table** | TCD3070 Pro 40 — no EAP, so registers give the current mode only | `dice-focusrite.c:8-22`, keyed by rate mode |

**Correction (2026-09-25): the TCD3070 Pro 40's registers are not known to
fail.** This table used to say they "do not answer"; nothing supports that.
Focusrite's own driver supports the device from Saffire.kext 4.3.0 / MixControl
3.9 (the 2019 installer; the 4.1.4 / 3.5 we first read has no entry):
- `SaffireAudio::probe` accepts GUID product `19` (`0x13`) as a second
  "Saffire Pro40". It is the same device as Linux's ROM model `0x0000de`
  (`dice.c:385-387` documents the GUID/ROM mismatch).
- The kext has no geometry table for it. `PopulateDeviceStruct` reads its
  TX/RX registers like every other model; its only product check is for
  another vendor (OUI `0x000166`).
- MixControl 3.9 adds `Pro40DiceIIIDescriptor` (device type 7, firmware
  `Pro40d3Firmware`) and `Pro40DiceIII_IpSigTab/OpSigTab`. Rates are 44.1, 48,
  88.2 and 96 kHz: no 32 kHz and no high-rate table. The tables are identical
  to the original Pro 40's except where the host-stream channels sit on router
  blocks 11/12 at low rate. The original splits them 12 + 8 across the two
  blocks, one per stream. The DICE III runs them 16 then 4, i.e. one 20-channel
  stream addressed across both blocks, which agrees with Linux's one stream of
  20 (low) / 16 (middle) plus MIDI.

Linux's table is therefore best read as covering the missing extension: without
it, registers describe only the current rate mode. Evidence tools live in
`tmp/dicere/` (`probe_430.c`, `pro40_sigtabs.txt`).

The earlier plan modelled two cases. It is three, and the EAP case is *better*
than the base case, not a degradation. **No device is known to report
registers that answer but answer wrongly**; do not add that state speculatively.

### 2.6 Supported rates are a device fact too

`CLOCK_CAPABILITIES` (global `+0x64`) carries a rate bitmask. Midas stores it
straight into the device struct and validates every rate request against it:

```c
// MidasFW::PopulateDeviceStruct 0xc89a
*(_DWORD *)(v2 + 12604) = ratesMask | (sourcesMask << 16);
```

The **only** host-side constant for the caps is a fallback for a GLOBAL
section too short to hold them, not a per-model one. `PopulateDeviceStruct`
reads `min(GLOBAL size, 0x17C)` bytes; when that is `<= 0x64` it synthesizes the
caps word from driver defaults before the read:

```c
if ( v3 <= 0x64 )                              // GLOBAL ends at or before CLOCK_CAPS
    *(_DWORD *)(a2 + 12604) = defaultRates | (defaultSources << 16);
```

Linux has the same rule: it reads `CLOCK_CAPABILITIES` only when GLOBAL is
longer than `0x18` quadlets and otherwise assumes 44.1 + 48 kHz
(`dice-transaction.c:318-326`, `dice.c:73-92`). ASFW follows Linux
(`DiceDeviceRateMask`).

**Correction (2026-09-25).** This section used to call the
`GLOBAL_VERSION < 1.0.11.0` branch (`0xc89a`, `+12600 < 0x1000B00`) a caps
fallback. It is not: it writes fields at GLOBAL `+0x168..+0x17B` (struct
offsets 12864–12876), well past the caps at `+0x64`. The caps are never
overridden by firmware version.

### 2.7 The vendors publish per-stream, and the only sum is the channel base

`CreateStreams` (Alesis `0x59a0`, Midas `0x35c4`) creates **one `IOAudioStream`
per DICE stream** — `createNewAudioStream(direction, _DICE_STREAM_STRUCT*,
startingChannelID)`, named `"Input Stream %d"`, each taking its channel count
from its own per-stream struct. **No aggregate channel count exists in these
drivers**: CoreAudio is shown `Input Stream 1` (12ch) and `Input Stream 2` (2ch).

The one running total is the channel-ID base:

```c
v39 = 1;                                      // base, 1-based
...
createNewAudioStream(...);
v16 = *(_DWORD *)(v12 + 1507908);             // THIS stream's channel count
v39 += v16;                                   // running sum of preceding widths
```

ASFW publishes **one** `IOUserAudioStream` per direction and slices it with
`AudioStreamConfig::sourceChannelOffset`. That is a legitimate and simpler
HAL-side choice and we are keeping it — the totals agree either way. But it
means `sourceChannelOffset` **is** the vendor's channel base, and the rule it
must obey is *running sum of preceding stream widths*, never
`index × width-of-stream-0`. See §3.5.

### 2.8 The recorded devices

Five dumps in [`fixtures/`](fixtures/), four vendors, five distinct shapes. These
drive `tests/audio/DiceFixtureGeometryTests.cpp`. (The Venice F32 dump was added
after this section was first written; it and the F24 agree on every identifying
register and differ only in GUID, serial and geometry.)

| device | ASIC / EAP | capture (DICE TX) | playback (DICE RX) | clockCaps | rates |
|---|---|---|---|---|---|
| Focusrite Saffire Pro 24 DSP | TCD2210, **EAP** | 1: 16 PCM + 1 MIDI (dbs 17) | 1: 8 PCM + 1 MIDI (dbs 9) | `0x112C001E` | 44.1/48/88.2/96 |
| Midas Venice F24 | no EAP | 2: 16+8 = **24** | 2: 16+8 = **24** | `0x13000006` | 44.1/48 |
| Midas Venice F32 | no EAP | 2: 16+16 = **32** | 2: 16+16 = **32** | `0x13000006` | 44.1/48 |
| PreSonus StudioLive 24.4.2 | no EAP | 2: 16+16 = **32** | 2: 16+10 = **26** | `0x13000006` | 44.1/48 |
| Alesis MultiMix | no EAP | 2: 12+2 = **14** | **1**: 2 | `0x11000006` | 44.1/48 |

Each covers something the others cannot:

- **Pro 24 DSP** — the only hardware-verified row, the only EAP, the only one
  advertising 2x, and the only one with MIDI ≠ 0 (so `am824Slots ≠ pcmChannels`,
  which is the only case exercising the slot-mismatch branch of the resolver).
  Single-stream, so it does *not* discriminate summing from multiplication.
- **Venice F24** — asymmetric streams in both directions. Its own model string
  reads "Venice F32", so identity cannot name the variant; an F32 at 16+16 could
  not discriminate at all.
- **StudioLive 24.4.2** — asymmetric *between* directions (32 vs 26).
- **MultiMix** — the only unequal stream **counts** per direction (2 vs 1), and
  the sharpest aggregate case: `stream0 × count` = 24 against a true 14.

All four note *"RX section is allocated for N stream block(s) but NUMBER reports
M."* Allocation is not the count; `NUMBER` is authoritative.

> The MultiMix dump is user-contributed on an older build — indicative, not
> verified. It is the only MultiMix evidence in the tree.

### 2.9 Where the references disagree with us

libffado forces `m_nb_rx = 1` for Alesis model `0x000000`/`0x000001` and
Focusrite Saffire PRO 26 (`dice_avdevice.cpp:1686-1700`), because the device
"announces two receive transmitters, but only has one". Two facts about this:

- `m_nb_rx` is **host playback**. `dice_avdevice.cpp:1057-1062` is decisive:
  `m_nb_tx → E_Capture`, `m_nb_rx → E_Playback`. libffado never clamps `m_nb_tx`.
- **The Alesis vendor driver clamps neither** — `PopulateDeviceStruct` loops the
  full `TX_NUMBER` and full `RX_NUMBER` with no bound whatsoever.

So the clamp is libffado's own workaround, with no vendor basis, and libffado's
adjacent `FIXME` proposes the general rule instead: *ignore a stream announced
with zero channels*.

**Our copy of it is disabled** (§3.3). The hazard libffado guards — arming an
iso channel and reserving bandwidth for a stream the device never consumes — is
real in principle, so the code is retained under `#if 0` rather than deleted.

**What hardware already says.** The contributed dump
(`fixtures/alesismultimix.txt`) reports DICE `TX_NUMBER = 2` (12 + 2) and DICE
`RX_NUMBER = 1` (2 PCM). **That unit does not over-report playback at all** — so
libffado's clamp would be a no-op on it, while ours removed a capture stream it
genuinely has.

**TODO(FW-DICE-ALESIS): one variant remains, and it is the accused one.**
libffado names the MultiMix **16** specifically — *"Same is true for Alesis
Multimix16 and Focusrite Saffire PRO 26."* The dump we hold is a 12-input unit
(`MIC_LINE_1..4`, `LINE_5..12`, `MAIN_IN L/R`), and 8/12/16 all publish vendor
`0x000595` model `0x000000`, so it cannot speak for the range.

1. Obtain a MultiMix **16** dump — `TX_NUMBER`, `RX_NUMBER`, per-stream
   `NUMBER_AUDIO`. A Saffire PRO 26 would corroborate independently.
2. If it reports `RX_NUMBER > 1` with only one real playback stream, re-enable
   against **playback** (`playbackStreamCount`), never capture, scoped so the 12
   is unaffected.
3. Prefer libffado's own suggested general form over a per-model rule: ignore a
   stream announced with zero channels. No vendor/model table, no direction to
   get wrong. Not implemented yet because no recorded device shows such a
   stream, and implementing it on spec alone would be inventing wire behaviour.
4. If the 16 also reports `RX_NUMBER = 1`, **delete the block and the trait**.
   libffado's workaround would then have no basis on any hardware we have seen,
   and Alesis's own driver clamps nothing in either direction (§2.9).

---

## 3. What we have

### 3.1 The geometry resolver (landed)

`Audio/Protocols/StreamGeometryResolver.hpp` — plain scalars, no DriverKit,
fully host-testable. `ResolvedDeviceGeometry{capture, playback}` with
`StreamCount()`, `TotalPcmChannels()`, `Usable()`. The device wins where it
states a geometry; the profile is an expectation; a disagreement is a refusal,
never a silent reconciliation.

`StreamGeometryAuthority{kSeed, kAsserted}` distinguishes a profile constant
that is *evidence* from one that is a *pre-caps placeholder*, per direction.
Without it, enforcing the verdict made unverified profiles unpublishable — the
MultiMix seeds one capture stream of 16 where the device reports two of 12+2.

**This is scaffolding.** Once profiles carry no geometry (§4), there is nothing
left to seed or assert and the whole distinction deletes itself.
**Deleted 2026-09-25 (stage C):** the resolver now takes the device's geometry
alone. The one profile input left is the Alesis MultiMix's single playback
stream (§4.2 C).

### 3.2 The seven profile classes, measured

> **Historical (collapsed 2026-09-25, stage C).** Kept as the evidence behind
> `DiceProfileSpec`; the classes no longer exist.

26 DICE catalog rows → 10 builders → 7 profile classes, 1,284 lines. What is
actually in them:

**Safety offsets and reported latency — six profiles, three spellings, one set
of numbers.** Alesis, Midas, StudioLive, StudioLive 2442 use
`(6|16 + rateAddend) * framesPerPacket`; Focusrite writes the same via a
`delayPackets` local; Weiss writes `6U|16U * FramesPerPacket()`. All six use the
29/59/119 latency ladder. Only `GenericDiceProfile` differs (flat 64/128).
Weiss omits the rate addend — identical at every rate we publish, divergent at
2x/4x.

**Quirks — one baseline plus two single-field deltas.** Focusrite, Midas,
StudioLive and StudioLive 2442 are byte-identical. Alesis differs only in
`initializeNonAudioSlots = false`; Saffire Pro 40 only in `tx = kAM824`. Weiss
and Generic return a default-constructed `DiceDeviceQuirks{}`.

**Update 2026-09-24:** the Pro 40 delta is removed. It now inherits the Saffire
baseline (raw 24-in-32 TX), matching Focusrite's own `Saffire.kext`, which drives
every model it supports through one `Float32ToSwapInt24_In_32` path. Weiss and
Generic still default to AM824 TX, although `WeissFirewire.kext` uses the same
raw path. Weiss has never run on hardware (README), so that flip is a declared,
unverified delta for stage C, not a silent change.

Everything else in those 1,284 lines is device-readable (geometry, rates) or an
identical copy. The irreducible per-device content is: **two one-field quirk
deltas, Weiss's capture-visibility policy, Generic's flat offsets, and names.**

### 3.3 Known gaps

| gap | detail |
|---|---|
| **Resolved object has one consumer** | only `DiceAudioBackend::EnsureNubForGuid`. Bandwidth reservation reads `caps` directly (`DuplexStreamProfile::ResolveChannels`); CIP framing reads the profile (`ASFWAudioDevice::StartIO`). |
| **StartIO is host-sourced and 2-stream** | builds streams 0 and 1 from `profile->BuildTxStreamConfig`, hardcoded. `Model::ASFWAudioDevice` carries only aggregate channel counts, so per-stream geometry cannot cross the nub. **Consequence:** playback geometry may not be seeded — see below. |
| ~~**Rates are a host constant**~~ | **Closed 2026-09-25 (stage B).** `CLOCK_CAPABILITIES` reaches `AudioStreamRuntimeCaps::deviceRateMask`; the nub publishes `DicePublishedRates(mask)` with `ASFWDeviceSampleRates`, and `Configure`/`ApplyClockIdle` refuse an unadvertised rate before any bus traffic. |
| **Caps re-read, but the HAL device is not republished** | *Corrected 2026-09-25:* caps do re-read. `Prepare` (`FinishPrepare → RefreshRuntimeCaps`) and the idle clock apply (`CompleteClockApply → RefreshRuntimeCaps`) both refresh them. What is missing is stage D: a rate-mode change that alters geometry does not republish the CoreAudio device. Not observable while `kDiceMaxSupportedRateHz = 48000`, because 32/44.1/48 kHz are all rate mode *low*. |
| **Extended channel-name block** | `DiceFamilyDriver::DiscoverStreams` reads the standard names offset unconditionally; a device with stream `SIZE >= 326` uses `+0x120` (§2.2). Cosmetic — wrong or empty labels, not a streaming fault. |

### 3.4 Every geometry consumer now reads the device

Step A has landed. All three consumers read the same resolved geometry:

| path | source |
|---|---|
| bandwidth / transport, both directions | device (`DuplexStreamProfile::Build` reads `caps`) |
| **capture** encoding | device (`IsochDuplexHostTransport` → `DirectAudioReceiveConsumer`) |
| **playback** encoding + packet allocation | **device** (nub per-stream properties → `BuildResolvedTxStreamConfig`) |

The nub carries `ASFWPlaybackStreams` / `ASFWCaptureStreams` — an array per
direction of `{PCM, Slots, MIDI, Offset}`, where `Offset` is the running sum of
preceding stream widths (§3.5). `ASFWAudioDevice::StartIO` builds each stream
from it, keeping only the constants the DICE registers do not hold: `fdf`,
`fmt`, frames per data packet, stream mode, `sid`. Packet allocation shares the
same derivation (`TxPacketBytesForStreamConfig`), so the buffer allocated and
the geometry framed from cannot disagree.

The recorded Venice F24 now resolves end to end: published as 24 in / 24 out,
and framed as **16 at offset 0 then 8 at offset 16**, with DBS 16 and 8 and
packet sizes to match — where the F32 profile would have said 16 + 16.

`TreatProfileAsAsserted(profileAsserts, encodingIsDeviceSourced)` in
`StreamGeometryResolver.hpp` is the rule, and `DiceAudioBackend` now supplies
`true` for both directions. The constants are kept rather than deleted: they
state *which* directions have been migrated, and the next family to move off
profile constants needs the same question asked of it. Both are properties of
the **driver**, not of any device, and must not become per-device policy.

A profile's own declaration is left untouched by this: it states what its
constants *mean*; the gate decides what the driver can safely act on.

**Capacity.** This build allocates one primary and one secondary playback
stream. A device carrying more is refused — at publication in
`EnsureNubForGuid` and again at `StartIO`, so it never appears as an endpoint
that fails every start. Nothing in the fixtures exceeds two.

**Refresh.** The audio driver caches geometry during graph creation; changing
nub properties alone does not reconfigure it. The publisher retains the original
configuration and compares both stream directions, offsets, HAL channel counts,
visibility, supported rates and stream mode. An unchanged observation is a no-op.
A mismatch latches start/recovery/clock rejection until the nub is terminated
and recreated, even if a later observation matches again. The rejected snapshot
does not replace the endpoint runtime configuration. Ordinary stop remains
available. Live geometry reconfiguration is deliberately not implemented.

**Loss across the boundary cannot be silent.** `ASFWResolvedGeometryRequired`
says the audio side must not substitute profile constants. Serialization failure
fails the whole publication (`PopulateNubProperties` returns false, and
`AudioNubPublisher` discards the nub); an array arriving malformed or
over-length is rejected whole rather than truncated; and `StartIO` fails with
`MissingResolvedGeometry` if the flag is set but no streams arrived. Without
that flag, losing the arrays would be indistinguishable from a family that never
had geometry to publish, and the fallback would quietly reinstate the F24
mismatch.

### 3.5 The `sourceChannelOffset` landmine

`AudioStreamProfile.hpp:113` computes:

```cpp
outConfig.sourceChannelOffset = streamIndex * outConfig.pcmChannels;
```

This is self-consistent **today only because** `BuildDefaultTxStreamConfig`
returns stream 0's shape for every index — all widths are equal, so
`index × w0` coincides with the running sum.

The moment per-stream widths become device-derived (12 then 2, not 12 then 12),
that expression puts the MultiMix's second capture stream at offset 2 instead of
12, on top of the first. **The correct rule is the vendor's running sum (§2.7),
and it must be adopted in the same change that makes widths device-derived.**
Pinned by `ChannelBaseIsARunningSum` in the fixture tests.

### 3.6 Publication has no unvalidated path

`EnsureNubForGuid` reaches `EnsureNub` only after the device has been compared
against the profile. Three refusals guard it: a missing protocol, a failed
geometry load (refused in the callback, because caps cached by an earlier
successful load survive a later failure and would otherwise publish against a
stale description), and an unusable verdict.

---

## 4. Direction

### 4.1 Target

```
catalog row (identity + name + ~4 scalars)
        +
device registers (geometry, rates, clock, channel names)
        ↓
   one DICE profile builder  →  resolved profile, keyed by rate mode
        ↓
   every consumer: nub publication, StartIO framing, IRM bandwidth, capture layout
```

The catalog keeps identity and names — that is exactly what the vendors' own
name tables are, and the one thing their `probe` retains. What it gains is the
small set of facts the device cannot report:

| scalar | why it cannot be read | who needs it |
|---|---|---|
| `txEncoding` | wire format is not in any register | **Likely deletable:** all five TCAT kexts send raw 24-in-32 for every model. Weiss/Generic `kAM824` is the only remaining deviation, and it is unverified (§3.2 update) |
| `initializeNonAudioSlots` | host framing policy | Alesis is the only `false` |
| `preserveFdfInNoDataPackets` | host framing policy | the five DICE rows |
| `hideCaptureFromCoreAudio` | product decision, not a device fact | Weiss (already a runtime policy; move its source) |

What stays a host constant, legitimately: **`kDiceMaxSupportedRateHz`**. It is
one global ceiling with a stated reason (2x/4x changes frames-per-packet and
per-stream splits and is unverified end to end), not a per-model quirk.

Published rates become `deviceAdvertised(clockCaps) ∩ hostValidated(≤ ceiling)`,
with the short-GLOBAL fallback (§2.6).

### 4.2 Staging

The order is forced by one fact: **`StartIO` runs on the audio side, which has
no bus access.** Device geometry must cross the nub before anything host-side
can be deleted.

- **A — per-stream geometry crosses the nub.** Extend the nub snapshot beyond
  aggregate counts; `StartIO` loops `StreamCount()` instead of hardcoding 0/1;
  `sourceChannelOffset` becomes a running sum (§3.5). *Touches the serialized
  nub contract between two IOService objects — sequence it, do not just edit it.*
  Flips `kPlaybackEncodingIsDeviceSourced` to true in the same change, which
  retires the seeding gate in §3.4. Afterwards `Tx/RxStreamCount` and the
  per-stream channel constants have no consumer.
  **Landed 2026-09-20** — see §3.4.
- **B — rates from the device.** `clockCaps` onto `AudioStreamRuntimeCaps`;
  intersect with the ceiling; short-GLOBAL fallback (§2.6). Independent of A.
  Afterwards `SupportedSampleRates()` has no DICE consumer.
  **Landed 2026-09-25** (`a4e5148d`). Every recorded device still publishes
  {44.1, 48} kHz. Declared delta: a device advertising 32 kHz now offers it,
  and one lacking 44.1 or 48 kHz no longer offers that rate.
- **C — collapse seven classes into one builder + the §4.1 scalars.** Only
  possible once A and B have removed their consumers.
  **Landed 2026-09-25** (`3a856d9d` records every builder's answers in
  `tests/golden/dice-profiles/`, `7b0585ba` collapses). One `DiceProfile`
  built from a `DiceProfileSpec` (name, Venice range names, TX encoding,
  `preserveFdfInNoDataPackets`, `initializeNonAudioSlots`,
  `assertedPlaybackStreams`); latency from `AudioGeometryPolicy`. Capture
  visibility needed no scalar: the Weiss protocol already publishes
  `hostInputPcmChannels = 0`. Declared deltas are listed in §4.4.
- **D — invalidate on rate change**, per discovery source (§2.5): EAP devices
  read all modes once; register-only devices re-read after the switch, as
  `RestartStreaming → PopulateDeviceStruct` does. Unblocks raising the ceiling.
  **Deferred (2026-09-25)** to the ceiling raise. It cannot trigger below
  2x rates, and verifying it needs 2x/4x hardware.

Doing C first is the tempting error: without A, deleting profile geometry only
moves the constants, because `StartIO` still needs numbers from the host side.

### 4.3 Explicitly not doing

- **Not** adopting the vendors' per-stream `IOAudioStream` model. One aggregate
  plus `sourceChannelOffset` is simpler on the HAL side and the totals agree.
  Changing it is a HAL-visible rewrite with no evidence behind it.
- **Not** adding an "incorrect live registers" state. No catalog device is known
  to be in that class (§2.5).
- **Not** porting a provider/registration framework. A family-keyed construction
  function gives the same selection and testability; an extensible registry has
  to earn itself against that baseline.

### 4.4 Migration invariant

Only the Pro 24 DSP and the StudioLive 24.4.2 are hardware-attested. The other
rows must resolve **exactly** as before, except where a stage declares a change
and gives its evidence. `tests/audio/DiceFixtureGeometryTests.cpp` is the
executable form of this: the four recorded devices' resolved answers are what
must not move. Collapsing the seven classes additionally needs a generated
equivalence check across all 26 rows — eyeballing the table in §3.2 is not it.

Two deltas must be **declared, not absorbed**, when C lands: Weiss's missing rate
addend, and `GenericDiceProfile`'s flat 64/128 offsets, which are a different
model from the packet-scaled one and sit on the fallback path.

**As landed (2026-09-25).** The equivalence check is
`tests/audio/DiceProfileEquivalenceTests.cpp`, recorded from the classes before
the collapse. Rows share builders, so the 10 builders plus the fallback cover
all 26 rows. Its diff moved only these lines:
- geometry (channels, stream counts, per-stream configs) is zero: profiles no
  longer state it, and a device whose registers differ from an old constant is
  no longer refused;
- the Alesis MultiMix keeps one asserted playback stream;
- the generic fallback now uses the ladder (48/128, latency 29 at 1x);
- Weiss is unchanged at every published rate (the addend only matters at 2x/4x).

Names, TX policy, framing constants and clock source did not move, and
`DiceFixtureGeometryTests` still resolves every recorded device unchanged.

---

## 5. Open questions

1. **Nub contract shape.** Per-stream geometry must cross as serialized
   properties. Does it go as a packed array, or does the snapshot change shape?
   This is the only piece in §4.2 that is not internal.
2. **EAP as the preferred source.** When a device has a TCAT extension, should
   `current_config` be authoritative over the plain registers for the *current*
   mode too, or only for the modes we are not in? The Pro 24 DSP agrees with
   itself across both, so the question is currently unforced.
3. **Zero-channel streams.** libffado's `FIXME` proposes ignoring a stream
   announced with zero channels, which would generalise its Alesis clamp. No
   recorded device exhibits one, so this stays a hypothesis.
4. **Extended channel-name layout.** Needs a device with stream `SIZE >= 326` to
   confirm whether the standard block is still populated on such a device.
