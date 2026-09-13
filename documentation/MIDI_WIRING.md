# MIDI over AMDTP → MIDIDriverKit

How MIDI rides inside the isochronous audio stream on the wire, what
MIDIDriverKit will and will not accept at the host end, and how to wire the two
together in ASFW.

Four sources inform this design. API facts, observed reference behaviour and
proposed ASFW behaviour have different levels of certainty:

- **Linux** — `references/linux-sound-firewire-stack/firewire/` (ALSA `snd-firewire`).
  Authoritative for wire behaviour. Cited as `file:line`.
- **Apple SDK (original investigation)** — `DriverKit27.0.sdk/…/MIDIDriverKit.framework/Headers/` and
  `MacOSX27.0.sdk/…/CoreMIDI.framework/Headers/`. Authoritative for the API
  surface. `ctx7` returned no MIDIDriverKit entry during this review; the local
  headers and Apple's online documentation are available.
- **Focusrite** — decompilation of the shipping CoreMIDI-era `Saffire` kext and
  `SaffireMIDIDriver` plugin (v4.1.4.18735, 2014-06-04, x86_64). A complete
  working implementation of exactly this feature, against DICE hardware we own.
  Cited as `binary!0xADDR`.
- **Apple sample** — `CreatingAMIDIDeviceDriver`, Apple's own MIDIDriverKit
  sample: a virtual driver publishing one device with one source and one
  destination. Authoritative for *idiom* — how the classes are actually
  subclassed, dispatched and reconfigured — and for the shipping personality and
  entitlement set. Cited as `sample:<file>`, relative to
  `references/CreatingAMIDIDeviceDriver/CreatingMIDIDriverSampleAppExtension/`.

> **Why this document exists.** The wire framing is small and was settled twice
> over — Linux and Focusrite agree byte for byte. The parts that are *not*
> obvious are (a) where in ASFW's two-phase TX pipeline a MIDI byte can legally
> be written, (b) that MIDIDriverKit's I/O surface has no timestamp and what
> that forces, and (c) that MIDI must not be modelled as a sub-feature of the
> audio driver. Each of those is a decision that is expensive to reverse.

**Status:** design, not yet implemented. Nothing in `ASFWDriver/` writes or
reads a MIDI byte today.

**Review checkpoint (2026-09-13, ASFW `dce2fec4`):** cross-checked the
current ASFW integration points, local Linux AM824/DICE sources, installed
`DriverKit25.5.sdk` headers, and the Apple sample now supplied in
`references/CreatingAMIDIDeviceDriver/`. The original SDK 27 citations remain
historical provenance; the installed headers confirm `Send`, `MIDIIOBlock`,
lifecycle signatures and the property enum. Context7 again returned no
MIDIDriverKit library. The sample confirms the virtual provider, object setup,
loopback and stopped/running configuration branch; it does not settle hardware
nub matching or host scheduling. The Focusrite addresses were **not
independently re-decompiled in this review**.

**Verdict:** the AM824 framing and separate MIDI-service direction make sense.
This is an implementable design direction, **not yet a settled lifecycle or
latency contract**. The main work is shared stream ownership, safe bounded
content publication, and the host scheduling probe.

**Initial scope: DICE-based Focusrite Saffire.** Use Saffire Pro 24 DSP as the
provisional first test model because this tree has its dedicated protocol and
profile; record the actual unit/model/GUID before hardware validation. “Saffire”
also names older BeBoB devices, which are outside this first milestone. Begin
with the device's verified 48 kHz blocking formation, stream 0 in each
direction, and its discovered physical port counts (expect one jack pair;
do not hardcode it from the Pro 40 reference). Add 44.1/96 kHz only where the
model and current engine support them. BeBoB, Oxford, secondary-stream MIDI,
and unsupported rate formations are follow-up work, not stage-0 dependencies.
The timing figures in §14 remain a model to test, not measured guarantees.

---

## Contents

1. [What already exists in ASFW](#1-what-already-exists-in-asfw)
2. [The wire: AM824 MPX-MIDI framing](#2-the-wire-am824-mpx-midi-framing)
3. [Rate limiting](#3-rate-limiting)
4. [Discovery: port counts and slot position](#4-discovery-port-counts-and-slot-position)
5. [The host API: MIDIDriverKit](#5-the-host-api-mididriverkit)
6. [UMP](#6-ump)
7. [Timestamps, and why there are none](#7-timestamps-and-why-there-are-none)
8. [Reference implementation: Focusrite](#8-reference-implementation-focusrite)
9. [ASFW design](#9-asfw-design)
10. [TX injection: the arm/fill problem](#10-tx-injection-the-armfill-problem)
11. [RX extraction](#11-rx-extraction)
12. [Staged plan](#12-staged-plan)
13. [Open questions](#13-open-questions)
14. [Timing budget](#14-timing-budget)
15. [Appendix: constants and offsets](#appendix-constants-and-offsets)

---

## 1. What already exists in ASFW

The discovery and geometry layers already account for MIDI slots. Port
publication, independent stream lifetime and byte plumbing are still missing.

| Piece | Location | State |
|---|---|---|
| `midiSlots` in the stream config | `Audio/Wire/AMDTP/AmdtpTypes.hpp:43` | present |
| Derived as `am824Slots − pcmChannels` | `Audio/DriverKit/Config/ResolvedAudioStreamProfile.cpp:96` | present |
| DBS includes MIDI blocks | `Audio/Protocols/BeBoB/MAudioSpecialProtocol.cpp:96-101`, `Phase88Protocol.cpp:34`, `GenericBeBoBProtocol.cpp:64` | correct |
| DICE MIDI port counts parsed | `Audio/Protocols/DICE/Core/DICETypes.hpp:491,503` (`kNumberMidi`), read at `DICEDuplexBringupController.cpp:1095,1151`; stream entries parsed at `DICETransaction.cpp:585,592` | **already read** |
| Oxford MIDI slot counts | `Audio/Protocols/Oxford/OxfwStreamFormats.cpp:60,77` (format code `0x0D`) | parsed |
| `TxMidiSlots()` / `RxMidiSlots()` | `Audio/DriverKit/Config/IAudioDeviceProfile.hpp:38,41` | plumbed |
| Empty MIDI quadlet on the wire | `AmdtpTxPolicy::defaultNonAudioSlotWord = 0x80000000` → `AmdtpTxPacketizer.cpp:181-188` | **already byte-correct** |
| MIDI label detection on RX | `Audio/Wire/AM824/AM824Decoder.hpp:47` `IsMIDI()` | present, unused |

The last two matter. `WriteDataPacketDefaults` already fills every non-PCM slot
with `0x80000000`, which is a byte-exact IEC 61883-6 "MIDI conformant data, zero
valid bytes" quadlet — identical to Linux's `b[0] = 0x80; b[1..3] = 0`. **ASFW
already transmits well-formed empty MIDI.** The work is to put bytes in it.

The DICE path also already copies `StreamFormatEntry::midiPorts` into
`AudioStreamRuntimeCaps::{deviceToHostStreams,hostToDeviceStreams}` in
`Audio/Protocols/DICE/Core/DICEDuplexBringupController.cpp:71-95`.
`Audio/Families/Common/CommonProfileBuilder.cpp` retains these caps in
`ResolvedAudioEndpointProfile::runtimeCaps`. Start from that per-stream data;
adding a second register-discovery path would duplicate existing work.
`ResolvedAudioStreamProfile::MakeStreamConfig` reduces the wire geometry to
`dbs`, `pcmChannels`, and `midiSlots`; that slot count is not an endpoint count.
A MIDI-specific capability projection/publication is still needed.

Missing:

1. MIDI **port counts** for BeBoB (Saffire pre-DICE, Phase 88, M-Audio). Only
   the DICE path has them.
2. MIDI **slot position** — implicitly `pcmChannels` today; correct for DICE,
   not guaranteed elsewhere.
3. All byte plumbing, UMP conversion, and the MIDI service itself.

---

## 2. The wire: AM824 MPX-MIDI framing

MIDI rides as **MPX-MIDI conformant data** in one AM824 slot of each data block,
alongside the PCM slots. One slot carries up to 8 MIDI ports, time-multiplexed
across successive data blocks.

### 2.1 Quadlet format

```
 31..24   23..16   15..8    7..0
 label    byte0    byte1    byte2
```

`label = 0x80 | n`, where `n` ∈ 0..3 is the count of valid MIDI bytes in this
quadlet. `0x80` = no bytes (the idle filler). Unused byte positions are zero.

Big-endian on the wire, like all IEEE 1394 payload.

Linux emits **at most one byte per quadlet** (`label = 0x81`) —
`amdtp-am824.c:309-315`. Focusrite does the same:

```c
// Saffire!0xe502  writeMIDIQuadlet, decompiled
v21 = byte << 16;
v22 = v21 - 0x7F000000;        // unsigned wrap: == 0x81000000 | (byte << 16)
return _byteswap_ulong(v22);
// ...and when nothing is due:
v22 = 0x80000000;
return _byteswap_ulong(v22);
```

Receivers must still accept `n` = 2 or 3. Linux's reader does
(`amdtp-am824.c:340-341`), and Focusrite's peels up to three:

```c
// Saffire!0xcdfc  readMIDIQuadlet
v7 = _byteswap_ulong(quadlet);
v8 = HIBYTE(v7) & 3;                  // length
v10 = 16;                             // shift: 16, 8, 0
while (v8--) { byte = v7 >> v10; ...; if (v10) v10 -= 8; }
```

Note Focusrite masks `label & 3` where Linux computes `b[0] - 0x80`. For valid
input (`0x80`–`0x83`) these agree. Masking alone does not validate the MIDI
label: it could also accept a different AM824 label. ASFW should retain Linux's
explicit range check; the excerpt does not establish Focusrite's upstream
validation.

### 2.2 Port multiplexing

```
port = (DBC + f) % 8
```

where `f` is the data-block index within the packet and `DBC` is the packet's
CIP data-block counter.

**Linux, TX** — `amdtp-am824.c:295-320`:
```c
for (f = 0; f < frames; f++) {
    b = (u8 *)&buffer[p->midi_position];
    port = (data_block_counter + f) % 8;
    if (f < MAX_MIDI_RX_BLOCKS && midi_ratelimit_per_packet(s, port) &&
        p->midi[port] != NULL &&
        snd_rawmidi_transmit(p->midi[port], &b[1], 1) == 1) {
        midi_rate_use_one_byte(s, port);
        b[0] = 0x81;
    } else {
        b[0] = 0x80;
        b[1] = 0;
    }
    b[2] = 0; b[3] = 0;
    buffer += s->data_block_quadlets;
}
```

**Linux, RX** — `amdtp-am824.c:323-344`:
```c
unsigned int port = f;
if (!(s->flags & CIP_UNALIGHED_DBC))
    port += data_block_counter;
port %= 8;
len = b[0] - 0x80;
if ((1 <= len) && (len <= 3) && (p->midi[port]))
    snd_rawmidi_receive(p->midi[port], b + 1, len);
```

**Focusrite, TX** — `Saffire!0xe778` `FillFirewireBuffers`, MIDI loop at
`0xf449`–`0xf48f`:
```c
v78 = *(uint8_t *)(packet + 3);              // CIP quadlet 0, byte 3 = DBC
v82 = (uint32_t *)(packet + 8 + 4*pcmSlots); // first MIDI slot
for (n = blocksInPacket; n; --n) {
    *v82 = writeMIDIQuadlet(this, stream, stream->midiBuf[v78 & 7], cycle);
    v82 = (char *)v82 + 4*(pcmSlots + 1);    // stride one data block
    ++v78;
}
```

**Focusrite, RX** — `Saffire!0xcf24` `ReadFirewireBuffers`. Three geometry
variants (call sites `0xe14e`, `0xe279`, `0xe332`); two seed the counter
explicitly and both read the same slot:

```asm
d39a  movzx ecx, byte ptr [rbx+0Bh]   ; CIP q0 byte 3 = DBC
d39e  mov   [rbp+var_A8], rcx
...
e11d  mov   rax, [rbp+var_A8]         ; DBC
e124  mov   r15d, eax                 ; port counter := DBC
e130  and   eax, 7                    ; port = (DBC + f) & 7
e137  mov   rdx, [rcx+rax*8+28h]      ; stream->midiBuf[port]
e14e  call  readMIDIQuadlet
e158  inc   r15d
```

Variant 3 (`0xe2bd`–`0xe2c4`) seeds identically.

**Conclusion: RX uses the same `(DBC + f) & 7` as TX. Focusrite implements no
unaligned-DBC quirk in the inspected paths.** The ordinary mapping agrees;
Linux's family-specific quirk remains a real difference.

> **False lead, recorded so it is not re-chased:** there is a fourth `and …, 7`
> at `Saffire!0xd3db`, but it masks the FDF byte and feeds
> `FDFToSampleRate(fdf & 7)` — the SFC sample-rate code, nothing to do with
> MIDI ports.

### 2.3 Slot position

The MIDI slot is one AM824 slot within each data block: `dbs = pcmSlots + 1`
for a single MPX-MIDI channel.

- **Linux** defaults `midi_position = pcm_channels` (`amdtp-am824.c:102`) —
  i.e. immediately after the PCM slots — and lets the device family override it
  via `amdtp_am824_set_midi_position()`.
- **Focusrite** hardcodes the last slot: first MIDI slot at
  `packet + 8 + 4*pcmSlots`, stride `4*(pcmSlots + 1)`
  (`Saffire!0xf2cc`-`0xf2e2`, `0xf449`).

BeBoB devices report it: the BridgeCo channel-position response carries section
type `0x0a` for MIDI conformant data. The returned position is **one-based**;
Linux subtracts one to obtain `stm_pos`, the zero-based slot index —
`bebob/bebob_stream.c:319-341`. M-Audio firmware is noted there as putting the
MIDI *location* where other devices put a channel index.

Moving MIDI away from the trailing slot also requires the corresponding PCM
slot map. Validate that the MIDI slot is below DBS and does not overlap any PCM
slot; otherwise adding MIDI would overwrite an audio channel.

### 2.4 Capacity ceiling

`AM824_MAX_CHANNELS_FOR_MIDI = 1` (`amdtp-am824.h:28`) — **one MPX-MIDI data
channel, up to 8 ports.** `midi_channels = DIV_ROUND_UP(midi_ports, 8)`
(`amdtp-am824.c:69`), and more than one channel is rejected.

Every device in scope fits comfortably:

| Device | Ports | Source |
|---|---|---|
| M-Audio 1814 | 1 per direction | `bebob/bebob_maudio.c:289-295` |
| M-Audio ProjectMix | 2 per direction | same; model selection at `bebob/bebob.c:245-249` |
| Terratec Phase 88 | 1 MIDI data block | `Audio/Protocols/BeBoB/Phase88Protocol.cpp:30` |
| Focusrite Saffire Pro 40 TCD3070 variant | 1 per direction | `dice/dice-focusrite.c:8-20`; query other DICE models |

### 2.5 Byte-slot budget

For **blocking mode with one PCM frame per data block**, the Linux first-eight
rule gives each port exactly one byte opportunity per DATA packet. Focusrite's
uncapped loop gives additional opportunities at higher rates:

| Rate | SYT interval | Blocks/packet | MIDI-eligible blocks | Opportunities per port per DATA packet | DATA packets/s |
|---|---|---|---|---|---|
| 44.1 / 48 kHz | 8 | 8 | 8 | 1 | 5512.5 / 6000 |
| 88.2 / 96 kHz | 16 | 16 | 8 (Linux) / 16 (Focusrite) | 1 / 2 | 5512.5 / 6000 |
| 176.4 / 192 kHz | 32 | 32 | 8 (Linux) / 32 (Focusrite) | 1 / 4 | 5512.5 / 6000 |

(Geometry from `Audio/Wire/AMDTP/AmdtpRateGeometry.hpp:58-64`.) This table is not
universal: `AmdtpCadence.cpp` also has a nonblocking 48 kHz cadence with six
blocks per packet, and Linux supports DICE's double-PCM-frame quirk
(`amdtp-am824.c:82-90`). Port rotation counts **wire data blocks**, not HAL
frames. Validate each supported formation independently.

MIDI 1.0 wire rate is 3125 bytes/s, so even the Linux-capped 6000/s is ~2× the
requirement. The rate limiter, not the slot budget, is what throttles.

### 2.6 Divergence: blocks carrying MIDI

Linux caps MIDI to the first 8 data blocks of a packet (`MAX_MIDI_RX_BLOCKS`,
`amdtp-am824.c:28,306`). Focusrite writes MIDI in **every** data block.

At 48 kHz (8 blocks/packet) the two are identical. At ≥ 88.2 kHz they diverge:
Focusrite cycles the port rotation two or four times per packet.

The cap is also a **device compatibility requirement**: Linux explicitly says
some receivers inspect only the first eight blocks (`amdtp-am824.c:24-28`).
Focusrite's credit limiter does not establish that other receivers accept MIDI
in later blocks.

**ASFW should follow Linux and cap at 8.** It is the conservative choice, it
costs nothing (MIDI 1.0 cannot use the extra bandwidth), and it keeps one
behaviour across all rates.

---

## 3. Rate limiting

Both stacks model the receiving device's MIDI UART FIFO. Neither omits it. A
device driven at 6000 bytes/s against a 3125 bytes/s UART will drop or garble.

### 3.1 Linux: fractional FIFO accumulator

`amdtp-am824.c:262-292`. Values are held in *bytes × sample rate* to avoid
fractions.

```c
#define MIDI_BYTES_PER_SECOND 3093   // nominally 3125; margin for clock drift

p->midi_fifo_limit = rate - MIDI_BYTES_PER_SECOND * s->syt_interval + 1;

static bool midi_ratelimit_per_packet(struct amdtp_stream *s, unsigned int port)
{
    int used = p->midi_fifo_used[port];
    if (used == 0) return true;
    used -= MIDI_BYTES_PER_SECOND * s->syt_interval;   // drain, per packet
    used = max(used, 0);
    p->midi_fifo_used[port] = used;
    return used < p->midi_fifo_limit;
}

static void midi_rate_use_one_byte(struct amdtp_stream *s, unsigned int port)
{
    p->midi_fifo_used[port] += amdtp_rate_table[s->sfc];   // += rate
}
```

Note the deliberate 3093 rather than 3125 (`amdtp-am824.c:19-22`): *"Nominally
3125 bytes/second, but the MIDI port's clock might be a bit slow."*

### 3.2 Focusrite: in-flight credit window

Per-port state in the TX ring header (§8.2):

| Offset | Field |
|---|---|
| +0x08 | `u32 history[16]` — the isoch cycle stamp of each emitted byte |
| +0x48 | `credits` — bytes still allowed in flight |
| +0x4C | ring mask (`N − 1`) |
| +0x50 / +0x54 | history head / tail |
| +0x58 | in-flight threshold, in cycle-offset units |

Emission decrements `credits` and records the send cycle
(`Saffire!0xe502`, `--a3[18]` and `a3[v12 + 2] = cycle`). Before each packet, a
sweep in `FillFirewireBuffers` (`0xf34f`–`0xf42f`) walks the history and returns
credits for bytes whose elapsed cycle offset exceeds the threshold — i.e. bytes
that have had time to clock out at 31.25 kbaud. Credits exhausted →
`writeMIDIQuadlet` early-outs to `0x80000000`.

`credits` and `mask` are initialised in `AllocateStreams` (`Saffire!0x12130`,
`0x12142`) as `N` and `N − 1`; the history array reserves 16 entries, so
`N ≤ 16`. The runtime source of `N` and of the threshold at `+0x58` were not
traced — they are tuning constants, not semantics.

### 3.3 Which to implement

**Implement Linux-equivalent accumulator behaviour in fresh C++**, following
the repository's read-only reference rule. The excerpts above are behavioural
evidence, not code to paste into the dext. Linux explicitly assumes a two-byte
receiver FIFO (`amdtp-am824.c:104-110`); it is a conservative model, not a
measurement of every device.

Drain against wire opportunities, not dispatch wall time: one wake can prepare
several packets. A failed fill must neither spend a byte nor permanently spend
its limiter credit. Conversely, skipped wire intervals must still advance the
model. Test the staged/committed limiter state alongside byte reservations
(§10.3). Retain the first-eight cap independently of the limiter.

---

## 4. Discovery: port counts and slot position

### 4.1 DICE — register parsing already present

Port count is the sum, per direction, of each stream's `number_midi` register:

```c
// Saffire!0x8a4c  Saffire::GetMIDIInfo
for (s = 0; s < txStreamCount; s++)  txPorts += txStream[s].numMidi;
for (s = 0; s < rxStreamCount; s++)  rxPorts += rxStream[s].numMidi;
```

Linux takes the max rather than the sum (`dice/dice-midi.c:114-119`) and only
ever drives MIDI on stream 0 (`dice/dice-midi.c:59-63,76-80`). For a single-stream
device the two agree; for multi-stream, prefer Linux's "stream 0 only" rule
until hardware says otherwise.

ASFW already reads this register: `DICETypes.hpp:491` (TX `kNumberMidi = 0x10`),
`:503` (RX `kNumberMidi = 0x14`), read at
`DICEDuplexBringupController.cpp:1095,1151` (those two callbacks discard the
returned value). The usable stream-entry parsing carries
the count into `DICE::StreamFormatEntry::midiPorts` (`DICETransaction.cpp:585,592`).
No new register command is needed, but publication is not done:
`DICE::StreamConfig::TotalMidiPorts` / `ActiveMidiPorts` sum counts
(`DICETypes.hpp:719-734`), so do not reuse those totals for Linux-style endpoint
enumeration without adapting the aggregation and stream routing together.

For the first Saffire milestone, validate stream 0's count and slot geometry
against the complete reported stream table. Linux uses the maximum count, not
a sum; if a nonzero count cannot be represented on stream 0, report the
formation as unsupported for MIDI rather than silently publishing unreachable
ports. Keep audio capability publication independent of this MIDI rejection.
DICE `tx` means device→host (CoreMIDI source), while ASFW's TX packetizer means
host→device (CoreMIDI destination). Use explicit direction names at the seam.

### 4.2 BeBoB — port and position discovery

Linux walks the unit's *external* plugs, asks each its type, and counts channels
of the ones that are MIDI — `bebob/bebob_stream.c:822-867`:

```c
for (i = 0; i < plug_count; ++i) {
    avc_bridgeco_fill_unit_addr(addr, plug_dir, AVC_BRIDGECO_PLUG_UNIT_EXT, i);
    err = avc_bridgeco_get_plug_type(bebob->unit, addr, &plug_type);
    if (plug_type != AVC_BRIDGECO_PLUG_TYPE_MIDI) continue;
    err = avc_bridgeco_get_plug_ch_count(bebob->unit, addr, &ch_count);
    // Yamaha GO44/GO46, Terratec Phase 24/x24 report 0 channels on external
    // output plug 3 (MIDI type) despite having a pair of physical MIDI jacks.
    if (ch_count == 0) ch_count = 1;
    *midi_ports += ch_count;
}
```

The plug counts come from `PLUG_INFO`: `plugs[2]` (external in) and `plugs[3]`
(external out) — `bebob/bebob_stream.c:942-948`. **ASFW already has these**, as
`UnitPlugCounts::extInputPlugs` / `extOutputPlugs`
(`Protocols/AVC/AVCUnitPlugInfoCommand.hpp:20-25`).

What ASFW lacks is the BridgeCo vendor-dependent extension pair:
`avc_bridgeco_get_plug_type` and `avc_bridgeco_get_plug_ch_count`. Grepping
`ASFWDriver/` for `0003db` / BridgeCo extension commands returns only comments.
These must be added to `Protocols/AVC/`.

Linux applies the zero-count workaround in this generic query, but the cited
comment names Phase 24/x24, **not Phase 88**. Do not treat it as evidence of a
Phase 88 firmware defect. Linux also skips this discovery when no advertised
formation contains MIDI (`bebob_stream.c:833-840`).

Per-family fallbacks when the query is refused (`BeBoBProfileBuilder.cpp:121-128`
already notes that some firmware refuses BridgeCo queries):

| Device | Ports in / out |
|---|---|
| M-Audio 1814 | 1 / 1 |
| M-Audio ProjectMix | 2 / 2 |
| Terratec Phase 88 | unresolved logical mapping; do not infer 1 / 1 from one slot |

The existing [Phase 88 hardware record](TERRATEC_TOPOLOGY_RESEARCH.md) already
reports two MIDI external plugs per direction and two MIDI logical locations
at the same stream position. Its channel-count responses are inconsistent:
IN plugs 5/6 report 2 each; OUT plugs 5/6 report 0 each. Applying the generic
sum/zero-count rule mechanically would produce 4/2, so neither a 1/1 fallback
nor a silent normalization is justified. Preserve the raw discovery values
and validate which logical indices reach the physical jacks before publishing
the final endpoints.

### 4.3 Direction mapping

`bebob/bebob_stream.c:500-520`: the **tx** stream (device→host, capture) is
parameterised with `midi_input_ports`; the **rx** stream (host→device,
playback) with `midi_output_ports`. Easy to invert; don't.

### 4.4 Oxford — slots are not physical port counts

`OxfwStreamFormats.cpp` parses MIDI slot counts, not a physical jack count.
Linux's generic OXFW discovery exposes one port in a direction when its
formation has MIDI (`oxfw/oxfw-stream.c:821-823,849-851`). If Oxford is in the
initial scope, encode that family policy explicitly; do not expose eight jacks
merely because one MPX slot can multiplex eight ports.

---

## 5. The host API: MIDIDriverKit

Originally read from `DriverKit27.0.sdk/System/DriverKit/System/Library/Frameworks/MIDIDriverKit.framework/Headers/`.
The current local verification uses the corresponding headers in
`/Applications/Xcode.app/Contents/Developer/Platforms/DriverKit.platform/Developer/SDKs/DriverKit25.5.sdk/`.
`ctx7` returned no entry for this framework during review. Apple's
[framework documentation](https://developer.apple.com/documentation/mididriverkit)
and [sample guide](https://developer.apple.com/documentation/mididriverkit/creating-a-midi-device-driver)
are additional API and sample sources; neither replaces a lifecycle probe.

### 5.1 Object graph

The MIDI device/entity/endpoint classes are `LOCALONLY`; the driver itself
is an `IOService` with service RPCs and local MIDI methods. The object graph
uses in-process C++ calls; this does not prove where separate services run.

```
IOUserMIDIDriver : IOService          your subclass, one per personality
 └─ IOUserMIDIDevice                  Create(driver, name, model, manufacturer)
     └─ IOUserMIDIEntity              Create(driver, device, name, protocol, numSources, numDestinations)
         ├─ IOUserMIDISource          device→host   Send(const IOUserMIDIUMPWord*, size_t)
         └─ IOUserMIDIDestination     host→device   SetIOBlock(MIDIIOBlock)
```

Objects managed by the host must belong to the driver's object graph
(`IOUserMIDIDriver.iig:122-137`). Follow the sample's actual construction:
`driver->AddObject(device)` and `device->AddEntity(entity)`, with matching
removal and checked return values. Do not infer that every descendant requires
a second explicit `AddObject` call. `Entity::Create` allocates its sources and
destinations; retrieve them with `GetSource(i)` / `GetDestination(i)`.

### 5.2 Plumbing requirements

From the `IOUserMIDIDriver` class comment:

Entitlements, from `sample:CreatingMIDIDriverSampleAppDriver.entitlements`:

| Entitlement | On | Required? |
|---|---|---|
| `com.apple.developer.driverkit` | dext | yes — ASFW already has it |
| `com.apple.developer.driverkit.family.midi` | dext | **yes — new** |
| `com.apple.developer.driverkit.allow-any-userclient-access` | dext | only if apps open a custom user client (macOS only) |
| `com.apple.developer.driverkit.userclient-access` | app | the narrower alternative to the above |

The sample's personality, summarized from `sample:Info.plist` — note the provider,
which is discussed in §9.1:

```
CFBundleIdentifierKernel            com.apple.kpi.iokit
IOClass                             IOUserService
IOProviderClass                     IOUserResources
IOResourceMatch                     IOKit
IOMatchCategory                     <driver class name>
IOUserClass                         <driver class name>
IOUserServerName                    <dext bundle id>
IOUserMIDIDriverUserClientProperties { IOClass = IOUserUserClient,
                                       IOUserClass = IOUserMIDIDriverUserClient }
```

The `IOUserMIDIDriverUserClientProperties` dictionary is what lets CoreMIDI
connect. A second, custom dictionary is needed only if apps also open a user
client on the dext.

`NewUserClient` must handle `type == kIOUserMIDIDriverUserClientType`
(`1129149796`, `MIDIDriverKitTypes.h:42`) by forwarding to super, and may create
its own client for any other type (`sample:CreatingMIDIDriverSampleAppDriver.cpp`).

`-framework MIDIDriverKit` goes in `OTHER_LDFLAGS` in `project.yml:144`.

MIDIDriverKit is available on macOS and on iPadOS 18+ with an M-series chip.

### 5.3 Lifecycle

| Method | Where |
|---|---|
| `IOUserMIDIDriver::StartIO(OSArray* deviceList)` / `StopIO()` | `IOUserMIDIDriver.iig` |
| `IOUserMIDIDevice::StartIO()` / `StopIO()` | `IOUserMIDIDevice.iig` |
| `RequestDeviceConfigurationChange(action, info)` → `PerformDeviceConfigurationChange` / `AbortDeviceConfigurationChange` | `IOUserMIDIDevice.iig` |
| `GetWorkQueue()` → `OSSharedPtr<IODispatchQueue>` | `IOUserMIDIDriver.iig`, `IOUserMIDIObject.iig` |

While I/O is running, structural changes use
`RequestDeviceConfigurationChange`; the framework stops I/O before the perform
callback. The sample directly calls its perform method when stopped (§5.6).
Do not infer a universal request requirement for the stopped case.

### 5.4 Properties

`IOUserMIDIObject::SetProperty(IOUserMIDIProperty, OSObject*)` /
`CopyProperty`. The `IOUserMIDIProperty` enum
(`MIDIDriverKitTypes.h:151-205`) mirrors CoreMIDI's property set: `Name`,
`Manufacturer`, `Model`, `UniqueID`, `DeviceID`, `Offline`, `Private`,
`DisplayName`, `ProtocolID`, `AdvanceScheduleTimeMuSec`, the
`Receives*` / `Transmits*` capability flags, and the UMP-era
`UMPActiveGroupBitmap`, `UMPCanTransmitGroupless`, `AssociatedEndpoint`.

`AdvanceScheduleTimeMuSec` is a trap — see §7.3.

### 5.5 What is absent

- **`IOUserMIDIUtils.h` is an empty stub** — `#ifndef` / `#endif`, no content.
- **No UMP ↔ MIDI 1.0 conversion helpers anywhere.** Grepping the framework for
  `MIDI1UP|ToBytes|FromBytes|PacketList` returns nothing.
- **No timestamp on any I/O path.** See §7.
- **No public `EnableSource` override.** `_HandleEnableSource(sourceID, bool)`
  exists only in the private `EXTENDS (IOUserMIDIDriver)` block. Do enable-time
  work in `IOUserMIDIDevice::StartIO`, or track enablement yourself.

### 5.6 Implementation idioms, from Apple's sample

These are not inferable from the headers, and several are easy to get wrong.

**Subclass with `OSTypeAlloc` + `init`, not `Create`.** `IOUserMIDIDevice::Create`
is for the un-subclassed case only — the header says so and the sample confirms
it. A subclass allocates itself and chains to the base initialiser:

```cpp
ivars->mDevice = OSSharedPtr(OSTypeAlloc(MyDevice), OSNoRetain);
ivars->mDevice->init(this, deviceName.get(), modelUID.get(), manufacturerUID.get());
AddObject(ivars->mDevice.get());
RegisterService();
```

`IOUserMIDIEntity::Create` *is* used directly, because the sample does not
subclass it.

**Entities are built in `init`, long before IO starts.** The sample creates its
entity, installs the IO blocks and sets `Offline` inside `Device::init`.

**The sample dispatches device I/O transitions and control actions to its work
queue.** `GetWorkQueue()` is available on both driver and device. Device
`StartIO`/`StopIO` wrap their super calls in `DispatchSync`, and driver control
handlers dispatch add/remove/offline actions. This is not proof that every
mutation or MIDI callback runs there: `StartIO` sets `Offline` outside that
block and the MIDI I/O block runs on the framework's RT thread. Avoid a nested
`DispatchSync` onto the same queue; verify ASFW's actual call contexts.

**`Driver::StartIO(OSArray* deviceList)` is the fan-out point.** Call
`super::StartIO(deviceList)` first, then each device's `StartIO()`. `StopIO()`
mirrors it, device first then super.

**`StartIO` clears `Offline`.** The sample sets `IOUserMIDIProperty::Offline` to
0 on a successful start — the natural hook for FireWire device presence.

**Gate configuration changes on `GetDeviceIsRunning()`:**

```cpp
if (GetDeviceIsRunning())
    return RequestDeviceConfigurationChange(kAddPortAction, changeInfo.get());
else
    return PerformDeviceConfigurationChange(kAddPortAction, changeInfo.get());
```

This is the sample's stopped/running branch. The claim that `Request…` while
stopped necessarily hangs is not established by the headers or sample; verify
that behaviour if ASFW needs to rely on it.

**Install I/O blocks for new entities.** `SetupEntities()` iterates all entities
and installs their loopback blocks. The sample calls it during `init` and after
**adding** an entity in `PerformDeviceConfigurationChange`; the removal branch
only removes the entity. It does not demonstrate universal reinstallation after
every change or safe lifetime for ASFW's cross-service callbacks. ASFW must
handle allocation/API failures and explicit callback teardown; the sample leaves
some return values unchecked.

**The loopback is Apple's own pattern**, and is exactly the stage-1 bring-up:

```cpp
auto ioBlock = ^kern_return_t(IOUserMIDIUMPWord const* umpWords, size_t numWords) {
    return source->Send(umpWords, numWords);
};
destination->SetIOBlock(ioBlock);
```

**Protocol choice.** The sample publishes `MIDIProtocol_2_0`. For an AM824
device carrying a MIDI 1.0 byte stream, `MIDIProtocol_1_0` is the honest
declaration — but make it a deliberate choice rather than inheriting the
sample's.

> **Sample-code curiosity.** `kAddPortConfigChangeAction = 'remp'` and
> `kRemovePortConfigChangeAction = 'addp'` — the FourCC values are swapped
> relative to their names. Harmless, because the tokens are opaque and only ever
> compared against themselves, but don't copy the confusion.

**No timestamp handling appears anywhere in the sample.** Grepping it for
`timeStamp`, `AdvanceSchedule` and `mach_absolute` returns nothing. Apple's own
reference implementation makes no attempt to work around the absence described
in §7.

---

## 6. UMP

MIDIDriverKit's I/O is **UMP words only**:

```c
typedef uint32_t IOUserMIDIUMPWord;
kern_return_t IOUserMIDISource::Send(IOUserMIDIUMPWord const* umpWords, size_t numWords);
typedef kern_return_t (^MIDIIOBlock)(IOUserMIDIUMPWord const* umpWords, size_t numWords);
```

The wire carries a MIDI 1.0 **byte stream**. A stateful per-port converter in
both directions is mandatory work.

### 6.1 Message types

`CoreMIDI/MIDIMessages.h:31-40`:

| MT | Meaning | Words |
|---|---|---|
| 0x0 | Utility (NOOP, JR Clock, JR Timestamp, Delta Clockstamp) | 1 |
| 0x1 | System Common / Real Time | 1 |
| 0x2 | **MIDI 1.0 Channel Voice** | 1 |
| 0x3 | **Data / SysEx7** | 2 |
| 0x4 | MIDI 2.0 Channel Voice | 2 |
| 0x5 | Data128 (SysEx8, Mixed Data Set) | 4 |
| 0xD | Flex Data | 4 |
| 0xF | UMP Stream | 4 |

MT is `word0 >> 28`. The byte bridge supports MT 0x1, 0x2 and 0x3.
MT 0x0 is transport/utility metadata, not DIN bytes: define which utility
messages are ignored or handled, without silently promising JR scheduling.
Declaring `MIDIProtocol_1_0` does not replace input validation. Check the word
count before reading a multiword UMP and reject unsupported types/groups by a
documented policy; never reinterpret their remaining words as new messages.

### 6.2 Word layouts

Read directly off the inline `constexpr` builders in `MIDIMessages.h`.

**MT 0x2 — MIDI 1.0 Channel Voice** (`MIDIMessages.h:276`):
```
 31..28  27..24  23..20   19..16    15..8          7..0
  0x2    group   status   channel   data1 (7-bit)  data2 (7-bit)
```

**MT 0x1 — System Common / Real Time** (`MIDIMessages.h:311`) — `status` is a
**full byte** here, not a nibble:
```
 31..28  27..24  23..16          15..8   7..0
  0x1    group   status (0xF8…)  byte1   byte2
```

**MT 0x3 — SysEx7** (`MIDIMessages.h:315`), max 6 payload bytes per packet
(`kMIDI1UPMaxSysexSize = 6`):
```
word0:   31..28  27..24  23..20   19..16      15..8   7..0
          0x3    group   status   numBytes    byte1   byte2
word1:   31..24  23..16  15..8    7..0
          byte3   byte4   byte5   byte6
```
`MIDISysExStatus` (`MIDIMessages.h:97-107`):
`Complete = 0x0`, `Start = 0x1`, `Continue = 0x2`, `End = 0x3`.

**MT 0x0 — Utility** (`MIDIMessages.h:557-574`):
```
 31..28  27..24  23..20   19..0
  0x0      0     status   data
```
`MIDIUtilityStatus` (`MIDIMessages.h:109-115`): `NOOP = 0x0`,
`JitterReductionClock = 0x1`, `JitterReductionTimestamp = 0x2`,
`DeltaClockstampTicksPerQuarterNote = 0x3`, `TicksSinceLastEvent = 0x4`.

### 6.3 Porting the builders

`MIDIMessages.h` exposes `MIDI1UPNoteOn`, `MIDI1UPNoteOff`,
`MIDI1UPPolyPressure`, `MIDI1UPControlChange`, `MIDI1UPProgramChange`,
`MIDI1UPChannelPressure`, `MIDI1UPPitchBend`, `MIDI1UPSystemCommon`,
`MIDI1UPSysEx`, `MIDI1UPSysExArray` — all `CF_INLINE CM_CONSTEXPR`, pure
bit-packing with no library dependency.

A dext cannot link CoreMIDI, but reimplementing these against §6.2 is
mechanical. They take **already-parsed fields**, so they are the easy half.

### 6.4 The hard half: the byte-stream parser

Per port, per direction, stateful:

**RX (bytes → UMP)**
- Track running status.
- Buffer a Channel Voice message until complete, then emit one MT 0x2 word.
- System Real Time bytes (`0xF8`–`0xFF`) may arrive **mid-message**: emit
  immediately as MT 0x1 without disturbing the partial message.
- SysEx accumulates into MT 0x3 packets of ≤ 6 bytes with
  `Start` / `Continue` / `End` (or a single `Complete`).
- A single AM824 quadlet can deliver up to 3 bytes, so the parser must accept
  a run, not assume one byte per call.

**TX (UMP → bytes)**
- The inverse, expanded to a byte stream with **no running-status compression**
  — safest for arbitrary devices.

Before implementation, specify System Common lengths and running-status reset,
SysEx delimiter insertion/removal, empty and exact-six-byte SysEx boundaries,
Real Time interleaving, malformed/truncated UMP and stream-loss recovery. The
bit-packing helpers alone do not specify those parser transitions. Add
independent byte/UMP vectors for them; do not call the converter complete after
only a Note On/Off loopback.

This is the same state machine as Focusrite's `MIDIParser::FeedBytes`
(`SaffireMIDIDriver!0x48bc`), which emits `MIDIPacketList` instead of UMP.

### 6.5 Groups vs. ports

Map **one entity per physical MIDI jack pair**:
`Entity("… MIDI 1", MIDIProtocol_1_0, numSources: 1, numDestinations: 1)`.
That gives CoreMIDI the shape Linux gives ALSA (`bebob/bebob_midi.c:75-82`) and
keeps the UMP `group` field at 0 throughout. For asymmetric port counts, create
only the source or destination that exists. Derive stable identity from device
GUID, direction and port index so a replug does not create new logical ports.

---

## 7. Timestamps, and why there are none

### 7.1 UMP does have timestamps — the wrong ones

```c
CF_INLINE CM_CONSTEXPR MIDIMessage_32
MIDIJitterReductionTimestampMessage(UInt16 senderClockTimestamp)
{ return (UInt32)kMIDIUtilityStatusJitterReductionTimestamp << 20u
       | (UInt32)senderClockTimestamp; }
```

JR Clock and JR Timestamp are a **16-bit relative sender clock for jitter
reduction across a transport** — wrapping, not absolute. (The UMP spec sets the
tick at 1/31250 s, giving a ~2.1 s wrap; the header does not carry the rate.)
They are also a negotiated MIDI 2.0 endpoint capability —
`MIDI2EndpointInfoNotificationMessage` carries `receiveJRTimestamp` /
`transmitJRTimestamp` flags (`MIDIMessages.h:466`), and
`MIDI2StreamConfigurationRequestMessage` negotiates them (`:509`).

A wrapping relative counter alone does not provide an absolute host deadline;
it requires an agreed clock correlation and receiver behaviour. The headers do
not establish whether MIDIDriverKit interprets JR words for scheduling. Do not
assume either that JR solves the missing timestamp argument or that wrapping
makes clock correlation impossible. Keep it out of the initial byte bridge
unless that contract is verified.

### 7.2 The real timestamp is outside the words

```c
// CoreMIDI/MIDIServices.h:481
struct MIDIEventPacket {
    MIDITimeStamp timeStamp;   // "Zero means now. The time stamp applies to
                               //  each UMP in the word stream."
    UInt32 wordCount;
    UInt32 words[64];
};
```

The timestamp lives in the **packet header**. MIDIDriverKit's `Send` and
`MIDIIOBlock` take `words` + `wordCount` — the packet payload with its header
removed. **The timestamp is structurally excluded, not accidentally omitted.**

### 7.3 `AdvanceScheduleTimeMuSec` — the trap

`CoreMIDI/MIDIServices.h:913-932` documents the contract:

> If it is non-zero, then it is a recommendation of how many microseconds in
> advance clients should schedule output. … **For devices with a non-zero
> advance schedule time, drivers will receive outgoing messages to the device at
> the time they are sent by the client, via MIDISend, and the driver is
> responsible for scheduling events to be played at the right times according to
> their timestamps.**
>
> … When a client sends to a virtual destination with an advance schedule time
> of 0, the virtual destination receives its messages at their scheduled
> delivery time.

The zero-advance delivery statement explicitly describes **virtual
destinations**. Extending it to a MIDIDriverKit hardware endpoint is a hypothesis,
not a documented guarantee for that callback:

| Advance time | MIDIServer delivers | Driver must |
|---|---|---|
| non-zero | classic documented path: immediate, with timestamps | schedule output itself; MIDIDriverKit's words-only path remains unverified |
| 0 / unset | documented for virtual destinations: scheduled delivery | verify the MIDIDriverKit callback timing before treating its words as "now" |

**MIDIDriverKit exposes `AdvanceScheduleTimeMuSec = 'mast'` in its property
enum. Leave it zero/unset for initial bring-up.** If nonzero causes early
delivery on this path, emitting those words immediately would play events too
early. The property enum alone says nothing about MIDIDriverKit's scheduling
implementation.

**Provisional design: let MIDIServer schedule.** A TX ring with bytes and no
deadlines is conditional on passing §13.1. No deadline API should be invented
to fill the gap if that probe fails.

### 7.4 What this costs

Focusrite's 4 ms lookahead permits early submission (§8.4); it cannot recover a
deadline already missed. Under the provisional MIDIServer model, ASFW adds
completion batching, dispatch delay, the bounded fill lead, port opportunities,
limiter delay and queue backlog after the host delivery. The mean DATA-packet
spacing alone is not the latency or jitter budget; see §14.

RX timestamps (§8.5) likewise have no destination. Compute them into the ring
for diagnostics only when a valid cycle/host correlation exists. Keep the
clock domain and stream epoch explicit; this does not restore client timestamps.

---

## 8. Reference implementation: Focusrite

`Saffire` kext + `SaffireMIDIDriver` plugin, v4.1.4.18735 (2014-06-04, x86_64),
for DICE-based Saffire hardware. A complete working implementation of this exact
feature.

### 8.1 Architecture

`SaffireMIDIDriver` is **not a kext**. It is a user-space CoreMIDI CFPlugIn
(`MIDIDriverInterface`, the classic `MIDIDriver` SDK base class) that opens a
user client on the audio kext.

```
CoreMIDI (MIDIServer)
      │  MIDIDriverStart / Send / EnableSource / Monitor
┌─────▼──────────────────────┐
│ SaffireMIDIDriver          │  user space
│   DiceMIDIDriver           │  MIDIDeviceCreate / AddEntity / MIDIReceived
│   MIDIParser (per port)    │  byte stream → MIDIPacketList
└─────┬──────────────────────┘
      │ IOServiceOpen("Saffire", type 0)              0x28e0 ConnectToDriver
      │ IOConnectMapMemory(type 0, 1491200 bytes)     0x42d8 Start
      │ IOConnectCallStructMethod(sel 0x15) GetMIDIInfo        0x2402
      │ IOConnectCallStructMethod(sel 0x16) + IOConnectSetNotificationPort  0x2206
┌─────▼──────────────────────┐
│ Saffire.kext               │  kernel
│   FillFirewireBuffers      │  0xe778 → writeMIDIQuadlet 0xe502
│   ReadFirewireBuffers      │  0xcf24 → readMIDIQuadlet  0xcdfc
└────────────────────────────┘
```

The MIDI driver never touches FireWire. The audio driver produces and consumes
MIDI bytes in the shared buffer whenever it is streaming.

### 8.2 Shared memory layout

`IOConnectMapMemory(connect, 0, task, &addr, &size = 1491200, 1)` —
`SaffireMIDIDriver!0x42d8`. Backed by
`SaffireUserClient::clientMemoryForType` (`Saffire!0x798e`, `withLength 0x16C100`).

`AllocateStreams` (`Saffire!0x11c40`) carves it in two, and the regions are
exactly contiguous (`0xC5D90 − 0xFD10 = 0xB6080 = 745600`):

```
this + 0x0FD10 (64784)   RX region   5 devices × 16 ports × 9320 B = 745600
this + 0xC5D90 (810384)  TX region   5 devices × 16 ports × 9320 B = 745600
                                     total = 1491200 = 0x16C100
```

Per-port block, **9320 bytes, identical shape in both directions**:

| Offset | Size | Field |
|---|---|---|
| +0x0000 | u32 | `writeIndex` (producer) |
| +0x0004 | u32 | `readIndex` (consumer) |
| +0x0008 | u32[16] | in-flight history (TX only) |
| +0x0048 | u32 | `credits` (TX only) |
| +0x004C | u32 | ring mask `N − 1` (TX only) |
| +0x0050 | u32 | history head (TX only) |
| +0x0054 | u32 | history tail (TX only) |
| +0x0058 | u32 | in-flight threshold (TX only) |
| +0x0060 | u64 | last emitted timestamp (TX only) |
| +0x0068 | u64[1024] | **timestamp per byte** |
| +0x2068 | u8[1024] | **byte ring** |

Both carve-up loops are guarded `cmp …, 10h / jnb` — 16 ports per device, max 5
devices, matching the plugin's own bounds checks (`SaffireMIDIDriver!0x3a7c`
rejects device ≥ 5, port ≥ 0x10).

**RX blocks get only `writeIndex` / `readIndex` zeroed** (`Saffire!0x128bf`).
The credit machinery exists only on TX (`Saffire!0x1210b`–`0x1215d`) — the kext
is the one feeding a device UART that can overrun.

### 8.3 Notification path

`SaffireMIDIDriver!0x2206 SetMIDIDataReceivedNotification`:

```c
IONotificationPortCreate(masterPort);
CFRunLoopAddSource(MIDIGetDriverIORunLoop(), IONotificationPortGetRunLoopSource(port), …);
IOConnectSetNotificationPort(connect, 0, IONotificationPortGetMachPort(port), 0);
inputStruct = { MIDIDataReceivedHandler, this, 64 };
IOConnectCallStructMethod(connect, 0x16, inputStruct, 0x18, nullptr, nullptr);
```

RX is **push, not poll**. `readMIDIQuadlet` sets a flag (`*a6 = 1`) which drives
`SaffireUserClient::SendMIDIDataReceivedNotification` (`Saffire!0x67aa`); the
plugin's handler drains every device × port ring
(`SaffireMIDIDriver!0x3d7c MIDIDataReceived`).

### 8.4 TX scheduling — the 4 ms window

`nm -u SaffireMIDIDriver` shows exactly three MIDI properties imported:
`kMIDIPropertyName`, `kMIDIPropertyOffline`, and
**`kMIDIPropertyAdvanceScheduleTimeMuSec`**.

The import is consistent with the non-zero branch of §7.3, but does not by
itself prove the property value. The recorded decompilation below demonstrates
driver-side timestamp scheduling; recheck the property setter/value before
claiming a particular MIDIServer delivery policy for the legacy plugin.

`SaffireMIDIDriver!0x3a7c DiceMIDIDriver::Send` stores a deadline per byte:

```c
bytes[w]      = packet->data[i];
timeStamp    += (timeStamp == 1);   // 1 is a reserved "consumed" sentinel
timestamps[w] = timeStamp;          // the MIDIPacket's host time; 0 == "now"
```

`Saffire!0xe502 writeMIDIQuadlet` consumes on deadline:

```c
clock_get_uptime(&now);
now += 4000000;                       // +4 ms lookahead
while (read != write) {
    ts = timestamps[read];
    if (ts == 1)                    { skip; }                  // already consumed
    else if (ts == 0 || ts <= now)  { emit; timestamps[read] = 1; break; }
    else                            { skip to next; }          // not due yet
}
// read cursor advances only over a contiguous run of consumed entries
```

Out-of-order emission, in-order retirement. A byte scheduled far ahead does not
block a later-queued byte that is due now.

**None of this transfers to MIDIDriverKit** (§7.3). It is recorded because it
explains every field in the ring layout and because it is the design to revisit
if a timestamped `Send` ever appears.

### 8.5 RX timestamping

`ReadFirewireBuffers` advances the timestamp it passes to `readMIDIQuadlet` by a
fixed amount per packet (`Saffire!0xe3fa`):

```asm
add qword ptr [rbp+var_74+4], 1E848h     ; 125000 ns = one isochronous cycle
```

RX byte timestamps are synthesised from the **isoch cycle grid in nanoseconds**,
not from a host clock read per byte. User space converts:

```c
v6 = AudioConvertNanosToHostTime(timestamps[r]);
MIDIParser::FeedBytes(parser, v6, &byte, 1);
```

Clean domain separation: kext works in ns on the cycle grid, plugin converts to
host time.

### 8.6 Enable is consumer-side only

`SaffireMIDIDriver!0x40a4 DiceMIDIDriver::EnableSource` makes **no IOConnect
call at all**. It sets `readIndex = writeIndex` to discard backlog, and lazily
allocates a `MIDIParser` for that (device, port). The kext's rings run
unconditionally with the audio stream.

Contrast Linux, where `midi_open` starts the isoch stream
(`bebob/bebob_midi.c:10-31`) and shares a `substreams_counter` with PCM.

### 8.7 Device discovery and hot-plug

`GetMIDIInfo` (`SaffireMIDIDriver!0x2402`) calls selector `0x15` for a 488-byte
struct: device count, per-device 88-byte records (GUID etc.), and per-device
port counts. It **retries up to 25 times with 50 ms sleeps** waiting for GUIDs
to stabilise. This establishes a retry policy in that implementation, not a
requirement for those constants in ASFW. Prefer ASFW's generation-aware
discovery completion; add bounded retries only for an observed transient.

`DiceMIDIDeviceManager` (`0x1ba4`) subclasses `IOServiceClient` and drives
`CheckForAddedDevices` / `CheckForRemovedDevices` /`RemoveOldMIDIDevices` off
IOKit publish/terminate notifications, using `MIDISetupAddDevice`,
`MIDIDeviceAddEntity`, `MIDIDeviceRemoveEntity` and
`kMIDIPropertyOffline`.

### 8.8 Summary: what transfers

| Concern | Focusrite | Transfers to ASFW? |
|---|---|---|
| Separate service for MIDI | user-space CFPlugIn over a user client | **yes** — proposed ASFW topology (§9.1) |
| Shared ring buffer | `IOConnectMapMemory`, 1024-byte rings | **yes**, with retained mappings; verify process placement |
| Push notification | mach port → runloop | **yes** → `IODispatchQueue` |
| Wire framing, port mux | `0x81`/`0x80`, `(DBC + f) & 7` | **yes**, equivalent behaviour in fresh code |
| Rate limiting | credit window | yes (or Linux's accumulator) |
| MIDI in all data blocks | no 8-block cap | **no** — follow Linux |
| Per-byte TX deadlines, 4 ms window | requires non-zero advance schedule time | **no** (§7.3) |
| Per-byte RX timestamps | 125 µs cycle grid | compute, but nowhere to send (§7.4) |
| Enable = consumer-side only | no call into the kext | only once a shared stream lease is held (§9.4) |
| Port count from `number_midi` | sum per direction | parsing yes; summing no — use the scoped routing policy in §4.1 |

---

## 9. ASFW design

### 9.1 Service topology — proposed separation

`IOUserAudioDriver` and `IOUserMIDIDriver` are both direct `IOService`
subclasses. `ASFWAudioDriver` should not combine both service hierarchies;
use a separate MIDI service. The current PCI provider of `ASFWDriver` is not
itself proof that a MIDI driver cannot use a hardware provider: Apple's guide
explicitly discusses USB/PCI hardware. Separation here follows ASFW's existing
service responsibilities; the custom-nub match still needs a probe.

Use the audio nub as a candidate publication shape, subject to the probe below:

```
ASFWDriver (IOPCIDevice)  ── owns OHCI, isoch, discovery, shared buffers
 ├─ ASFWAudioNub  (ASFWNubType=Audio) ← ASFWAudioDriver : IOUserAudioDriver
 └─ ASFWMidiNub   (ASFWNubType=MIDI)  ← ASFWMIDIDriver  : IOUserMIDIDriver
```

Add `ASFWMidiNubProperties` to `ASFWDriver/Info.plist` beside
`ASFWAudioNubProperties`, and publish with
`driver_->Create(driver_, "ASFWMidiNubProperties", &nub)` — template at
`Audio/Core/AudioNubPublisher.cpp:85-89`.

> **Unconfirmed: can `IOUserMIDIDriver` match a custom nub?** Apple's sample uses
> `IOProviderClass = IOUserResources` / `IOResourceMatch = IOKit` — the MIDI
> driver starts unconditionally at boot, with no hardware provider, and creates
> its device in `Driver::Start`. The audio-nub pattern above is the mirror of
> what `ASFWAudioDriver` already does, but nothing in the sample or the headers
> demonstrates it working for MIDI.
>
> **Fallback if the nub match does not take:** root `ASFWMIDIDriver` at
> `IOUserResources` like the sample, start it with zero devices, and have
> `ASFWDriver` drive device creation and teardown over a user client —
> `AddObject` / `RemoveObject` for whole devices, and
> `RequestDeviceConfigurationChange` for entity changes within one. This is
> analogous to Focusrite's plugin (§8.7), but does not prove a dext-to-dext
> connection or same-process placement. Validate discovery, connection,
> placement and mapping lifetime for this shape before choosing it.
>
> Resolve this at stage 1 — it is cheap to test and it decides how discovery
> reaches the MIDI service.

The existing personalities set `IOUserServerOneProcess = True`. Carry the
intended server configuration into the new personality and verify all three
services' process placement at stage 1. A proposed personality is not evidence
that they already share a process. Use retained descriptors/mappings regardless.

Proposed nub capability record: `{ deviceToHostMidiPorts,
hostToDeviceMidiPorts, deviceToHostStreamIndex, hostToDeviceStreamIndex,
deviceToHostMidiSlot, hostToDeviceMidiSlot, dbcAligned, deviceName,
stableDeviceIdentity, endpointId, streamEpoch }`. Keep persistent identity
separate from runtime endpoint IDs and reset epochs.

### 9.2 Layering

MPX-MIDI framing is AM824 label work — it belongs beside CIP under
`Audio/Wire/`, and **must never migrate into transport**. UMP and CoreMIDI
semantics are the MIDI analogue of `Audio/` and get their own subsystem.

```
CoreMIDI (MIDIServer)
      │
  Midi/DriverKit/   ASFWMIDIDriver.iig (: IOUserMIDIDriver), ASFWMidiNub.iig
      │
  Midi/Ump/         MIDI 1.0 byte stream ↔ UMP, per port, stateful
      │
  ── seam ──        Audio/Ports/IMidiPortIO.hpp — bytes + port index only
      │
  Audio/Wire/AM824/ Am824MidiMux: (DBC+f)%8 cadence, 0x80|len labels, rate limiter
      │
  Audio/Wire/AMDTP/ AmdtpTxPacketizer (writes slot) / RxAudioPacketProcessor (reads slot)
      │
  Isoch/            payload-opaque; no MIDI parsing, queues or endpoint knowledge
```

The byte seam carries port-indexed data and an explicit reservation contract.
A bare `PeekByte(port)` plus `ConsumeThrough(packetIndex)` is insufficient:
neither call says which queue positions were selected for the packet. Specify:

- Begin one reservation for `{streamEpoch, packetIndex}` on the TX consumer.
- Snapshot selected per-port read positions and byte counts, and stage the
  limiter decision. No producer-visible queue position advances yet.
- Write only those reserved bytes into the candidate packet.
- After successful payload publication, retire exactly that reservation once.
  Before a successful offer, rejection, sibling unreadiness or a missed
  frontier cancels it and retains bytes for a later packet. After a successful
  offer, never requeue automatically; count any later discard as loss (§10.3).
  A later commit must not retire canceled bytes.
- Reset/invalidate outstanding reservations at a stream epoch change; packet
  indices are reused on restart and eventually wrap.

Keep the ring's storage contract separate from service callbacks. The RX sink
copies a bounded run into its owned ring; it must not retain a pointer into a
receive packet. These are design requirements, not existing API methods.

### 9.3 Ring ownership — the FW-60 constraint

The TX write point and the RX read point are in **different IOServices**:

| Direction | Runs in | Queue |
|---|---|---|
| TX write (`RefillPcm`) | `ASFWAudioDriver` — `Audio/DriverKit/ASFWAudioDriverZts.cpp:1294-1314` | audio ZTS/IO thread |
| RX read (`ConsumePacket`) | `ASFWDriver` — via `Audio/Duplex/IsochDuplexHostTransport`, `Audio/Core/AudioCoordinator` | core driver receive queue |

Plus `ASFWMIDIDriver` as a third. Raw pointers between three services on three
queues is exactly the FW-60 shape.

**Allocate a `MidiTransportBlock` — 8 SPSC byte rings per direction — in an
`IOBufferMemoryDescriptor` owned by the core driver**, and hand it across both
nubs the way `ASFWAudioNub::CopyDirectAudioMemory` already hands over
`outControlMemory` (`Audio/DriverKit/ASFWAudioNub.iig:44-53`). Add
`CopyMidiTransportMemory` to both nubs.

```
ASFWMIDIDriver                                    ASFWAudioDriver
  Destination::SetIOBlock (RT thread)               ZTS thread
        │ UMP → bytes                                    │
        └──────▶ [ MidiTransportBlock.tx[port] ] ◀────────┘  peek in RefillPcm
                                                            consume in CommitFill
        ┌──────▶ [ MidiTransportBlock.rx[port] ] ◀──── ASFWDriver receive queue
        │ bytes → UMP                                        ExtractMpxMidi
  Source::Send
```

Both hot paths become lock-free ring accesses with no allocation and no logging
— which `MIDIIOBlock` requires anyway: *"called on the real-time thread, the
client should not do any blocking operations and make only real-time safe
calls."*

SPSC is conditional on one producer and one consumer **per port**. Verify the
callback serialization contract rather than inferring it from the service
count. Specify acquire/release cursor publication, capacity and overflow
handling, and retain both the descriptor and each service's mapping until its
queue is quiescent. Do not overwrite unread bytes. A rejected TX enqueue must
not publish half a message; RX loss must be counted and invalidate partial
parser state so bytes across the gap cannot form a fabricated message.

Push notifications should be coalesced and race-safe: drain, clear the pending
flag, then recheck before sleeping. Teardown must also cancel or drain queued
notification blocks that capture service state. Same-process memory does not
remove these lifetime obligations.

### 9.4 Stream lifetime

The one thing the MIDI service must reach across for is stream start/stop.
`IOUserMIDIDevice::StartIO` must ask `ASFWDriver` to bring the isoch streams up
(and bind the silence PCM source, §10.3); `StopIO` releases. Refcount against
the audio side's own start/stop — the DICE `substreams_counter` pattern from
`dice/dice-midi.c:9-48` (reserve/start, rollback on failure, release on close).

This is a **prerequisite for hardware RX/TX**, not a final-stage enhancement.
The current content pump is `ASFWAudioDriver::TxPreparationReady`, gated on
`runtime.txActive` and audio-owned control state. Starting OHCI alone does not
start that pump. Define the owner of the content engine, timing state and
completion action while CoreAudio is closed, and keep it alive under a MIDI
lease. Do not fabricate HAL activity just to keep the pump running.

**Target ownership:** one endpoint stream-session owner, coordinated through
`Audio/Core/AudioEndpointRuntime.hpp` and `AudioCoordinator`, holds the shared
runtime and content pump for both audio and MIDI leases. Extract the necessary
arming, timing-observation and fill work from `ASFWAudioDriverZts.cpp` into an
`Audio/Engine/` session component; keep it out of `Isoch/`. The audio service
supplies or removes a PCM source as its lease changes; the MIDI service supplies
bytes through owned storage. There must remain exactly one TX consumer/pump,
not competing audio-active and MIDI-only engines. The ownership change also
needs a retained slot-provider/timeline contract; merely relocating a function
that still dereferences audio ivars does not solve lifetime.

The diagrams in §9.3 show today's TX owner and a possible intermediate memory
handoff, not a requirement to keep the final pump in the audio service. Do not
add `CopyMidiTransportMemory` to the audio nub if the extracted session is the
sole TX consumer and the audio service no longer needs that mapping.

Acquire/release leases on a serialized owner queue, roll back failed starts,
and stop hardware only when the last lease is released. Specify audio-open,
audio-close, rate-change, bus-reset and unplug behaviour while MIDI is active.
The absence of a public `EnableSource` override also means `StartIO` must not
be assumed to correspond one-for-one to an application's endpoint open.

Teardown ordering follows the existing rule: quiesce dependent queues and drop
cross-seam views **before** freeing buffers or detaching hardware.

---

## 10. TX injection: the arm/fill problem

This is the part with no precedent in either reference, because ASFW's TX
pipeline is two-phase and neither Linux nor Focusrite has that shape.

### 10.1 The geometry

```
        126 ms out                                   near wire        WIRE
            │                                             │             │
   ┌────────▼─────────┐                          ┌─────────▼────────┐    │
   │ PrepareDataPacket │ ······ 1008 packets ····│  RefillPcm       │───▶│
   │ arms from SILENCE │                          │ real PCM content │    │
   └───────────────────┘                          └──────────────────┘
   DiceTxStreamEngine.cpp:143-169                 DiceTxStreamEngine.cpp:320-338
```

From `Audio/Shared/AudioTimingGeometry.hpp:266-267,286`:
`kTxSharedSlotPackets = 1512` = 3 × ring(504) = 189 ms, of which **126 ms is the
preparation lead** and 63 ms the ownership guard.

The arming comment is explicit (`DiceTxStreamEngine.cpp:143-149`): *"Arming
never consults the content source… real content arrives later through
FillTransmitSlot."*

Therefore:

- **MIDI at arm time → 126 ms of MIDI latency.** Unusable.
- **MIDI at a bounded near-wire fill point → potentially low latency.** Merely
  moving it into `RefillPcm` does not bound how far ahead it is written.

### 10.2 The three gates

`FillTransmitSlot` is PCM-gated in three ways, each of which drops MIDI:

| Gate | Code | Effect on MIDI |
|---|---|---|
| No PCM source bound (MIDI-only, no CoreAudio client) | `DiceTxStreamEngine.cpp:243-247` → `NotFillable` | no fills ever happen; MIDI never transmits |
| `ContentUnavailable` | `ASFWAudioDriverZts.cpp:1299` — `break` | packet retried later; MIDI stalls behind PCM |
| `Filled` but sibling stream not ready | `ASFWAudioDriverZts.cpp:1304-1314` — `CommitFill` skipped | `RefillPcm`'s writes discarded; popped MIDI bytes vanish |

Linux's `process_it_ctx_payloads` writes PCM **or silence**, then MIDI
unconditionally (`amdtp-am824.c:353-368`). That is the reference for content
independence; it does not validate ASFW's two-phase publication mechanics.

### 10.3 The fix — one path, not two

**(a) Reserve/commit/cancel.** The current loop skips a packet after a failed
sibling fill; `ContentUnavailable` instead breaks before advancing the cursor.
Neither behaviour gives a byte queue enough information for safe retirement.
Use the packet-local reservation in §9.2, including staged limiter state:

```cpp
// Illustrative transaction flow, not existing API.
auto reservation = midiMux.ReserveForPacket(epoch, packetIndex, dbc, blocks);
WriteReservedMidi(slot, reservation);
if (siblingReady && PublishLatePayload(packetIndex)) {
    midiMux.Commit(reservation);
} else {
    midiMux.Cancel(reservation);
}
```

Cancellation retains bytes and does not spend their emission credit. Elapsed
wire time still advances. A successful offer is a software queue-retirement
point under the initial **at-most-once submission policy**, not physical
delivery. This distinction matters even without a reset.

The current `CommitFill` calls `PublishLatePayload` and marks `armedFilled_`
only if it succeeds (`DiceTxStreamEngine.cpp:345-357`). The provider uses
`OfferLateTxPayload` arbitration (`ASFWAudioDriverPrivate.hpp:232-255`), not
just a check of `finalizedEnd`. Retire on that successful offer; do not retire
at acquire, encode, or arm time. Once offered, do not modify that packet image.
`RefillPcm` resets non-PCM slots to defaults before writing PCM
(`AmdtpTxPacketizer.cpp:139-146`), so MIDI must be composed **after** this reset
in the same candidate-image pass; a second PCM fill would erase it.

**An accepted offer can still be lost.**
`Isoch/Core/IsochTxQueue.hpp:201-230` permits `kLateImageReady` to become
`kFinalOnArmedImage`; `Isoch/Transmit/IsochTxDmaRing.cpp:263-272` records this as
`SealedDiscardingAlternative` / `latePayloadLostPublicationCount`. Therefore
`CommitFill == true` is not a guarantee that MIDI reaches the bus. The initial
policy must count these as losses and never replay an already-offered byte
behind later bytes. Track packet/epoch/port/byte counts in bounded content-side
bookkeeping and expose generic terminal payload outcomes through the seam;
the existing aggregate transport counter alone cannot identify lost MIDI bytes.
A stronger retry policy needs an ordered terminal-outcome protocol before later
bytes are allowed through; reserve/commit pseudocode alone does not provide it.
Normal-load qualification requires zero offered-MIDI discards, not merely zero
failed offers. A reset after binding still makes delivery uncertain.

**Offer notification is only partially wired in this snapshot.**
`IsochTxQueueControl::PublishOfferBatch` and
`IsochTransmitContext::ServiceLatePayloadOffers` exist, including generation
coalescing and a bounded service sweep, but there are no producer call sites
for `PublishOfferBatch` or callers of `ServiceLatePayloadOffers` in
`ASFWDriver/`. Do not assume that a content offer already causes a prompt
transport wake. Complete/verify the generic notification route as a prerequisite
of the bounded MIDI fill policy: one notification per successful batch, on the
transport owner queue, with reset/stop cancellation. Keep it payload-opaque.
Completion-time servicing remains available, but is not the missing explicit
notification route. Any change to OHCI servicing must be separately validated
against the local Linux/Apple transport references and audio tests.

The sibling code checks readiness and then commits sequentially; it is **not
an atomic two-stream transaction**. Primary publication may succeed before
secondary publication fails. For stage 3, MIDI is carried only on stream 0:
retire its reservation exactly when that stream publishes, regardless of a
later sibling failure. Never roll back/requeue already-published bytes. Keep
this limitation visible in audio regression tests; do not claim all-or-nothing
publication from the current comment in `TxPreparationReady`.

**(b) Bound the fill horizon before supplying always-ready silence.**
`ASFWAudioDriverZts.cpp:1294-1317` fills until `committedAfter`, which may be
about 126 ms ahead. With an always-ready silence source it can fill that whole
horizon; `armedFilled_` then prevents later MIDI from entering those packets
(`DiceTxStreamEngine.cpp:254-262`). A byte arriving afterward waits for the
cursor far ahead. This recreates arm-time latency despite writing in `RefillPcm`.

The proposed common content pass therefore needs an explicit bounded window
relative to transport progress, beyond the freeze frontier with enough dispatch
slack. Leave more-distant armed packets unfilled. Choose and validate that
window before assigning latency numbers; the frontier is a safety boundary,
not a fill scheduler. Trace the effect on PCM cache retention and sibling
commit behaviour before changing the shared fill policy.

Within that window, use PCM when ready and silence at the selected finalization
deadline when it is unavailable, while still including MIDI. Apply this during
audio underruns as well as MIDI-only operation. The existing
`substituteSilenceOnPcmUnavailable` flag is stored/copied but **not consulted by
`FillTransmitSlot` or `RefillPcm`**; flipping it does not implement this behaviour.
Do not immediately finalize all future audio as silence merely because it has
not yet been produced. The proposed result is one MIDI write path, with its
pump kept alive by the stream lease in §9.4.

**(c) Leave `PrepareDataPacket` alone.** It already writes `0x80000000` into
every non-PCM slot (`AmdtpTxPacketizer.cpp:181-188`). An armed packet that never
gets filled transmits valid "no MIDI byte this block", which is correct.

### 10.4 The write

One proposed method on `AmdtpTxPacketizer`, called after the selected PCM or
silence has been written. The following is **pseudocode**: the reservation is
already rate-limited, bound to this packet, and contains at most one byte per
port. Validate capacity, DBS, slot position and port count before taking any
payload view:

```cpp
void AmdtpTxPacketizer::WriteMidiSlot(uint8_t* packetBytes,
                                      const PreparedTxPacket& packet,
                                      const MidiPacketReservation& reserved) noexcept {
    if (streamConfig_.midiSlots == 0) return;
    uint8_t* payload = packetBytes + kCipHeaderBytes;
    // MAX_MIDI_RX_BLOCKS: amdtp-am824.c:28,306
    const uint32_t blocks = std::min<uint32_t>(packet.framesInPacket, 8);
    for (uint32_t f = 0; f < blocks; ++f) {
        const uint8_t port = static_cast<uint8_t>((packet.dbc + f) % 8);
        uint8_t byte = 0;
        const uint32_t word = reserved.GetByte(port, byte)
            ? (0x81u << 24) | (static_cast<uint32_t>(byte) << 16)
            : (0x80u << 24);
        WriteBE32(payload + (f * packet.dbs + midiSlotIndex_) * 4, word);
    }
}
```

`midiSlotIndex_` comes from the profile — defaults to `pcmChannels`, overridden
per family (§4.2).

---

## 11. RX extraction

`RxAudioPacketProcessorResult` already returns `dbc`, `dbs` and a wire block
count in `framesDecoded` (`RxAudioPacketProcessor.cpp:49-65`). These fields are
set **before** PCM binding and geometry validation. `hasValidCip` only means
`CIPHeader::Decode` accepted the EOH markers; it does not validate the AM824
format, MIDI slot, or complete payload geometry.

Create a validated AM824 packet view in the content layer and use it for both
PCM and MIDI. Its MIDI extraction must not depend on successful HAL writes:
the writer can legitimately be unbound in MIDI-only operation. The current
receive image has an **8-byte receive prefix followed by an 8-byte CIP header**;
data blocks start at offset **16**, not an ambiguous `kHeaderBytes`
(`RxAudioPacketProcessor.cpp:13,33-63`).

Before extraction, validate transfer status, minimum header length, supported
FMT/FDF and packet form, nonzero supported DBS, expected formation,
`midiSlotIndex < dbs`, and payload length divisible by `4 * dbs`. Reject or
explicitly handle family quirks before producing the view. In particular,
header-only NO-DATA has no blocks; do not derive MIDI from leftover storage.
Take the event count from the bounded wire payload, not the number of PCM
samples successfully written. Retain the DBC from that same packet.

The inner extraction then mirrors `read_midi_messages`
(`amdtp-am824.c:323-344`). This pseudocode assumes all bounds were checked:

```cpp
for (uint32_t f = 0; f < events; ++f) {
    const uint8_t* b = blocks + (f * dbs + midiSlotIndex) * 4;   // big-endian
    const uint8_t port = dbcAligned ? ((dbc + f) % 8) : (f % 8);
    const int len = static_cast<int>(b[0]) - 0x80;
    if (port < midiPortCount && len >= 1 && len <= 3)
        sink.DeliverBytes(port, b + 1, static_cast<uint8_t>(len));
}
```

Two things to get right:

- `b[0] == 0x80` is legitimate (empty) — skip, do not treat as an error.
- `len` can be 1–3, so a single quadlet may carry a whole 3-byte Note On. Do not
  assume one byte per quadlet on receive merely because both references only
  *send* one.

`dbcAligned` is the `CIP_UNALIGHED_DBC` equivalent. Default **true** — Focusrite
implements no unaligned path (§2.2), and Linux applies the quirk only to
specific device families. Make it a per-family profile flag and check Linux's
flags before trusting it on any new device.

---

## 12. Staged plan

Each stage should be a reviewable change with its evidence recorded before the
next hardware milestone. This plan targets the DICE Saffire first; no BridgeCo
commands or Phase 88 normalization are required to start.

| Stage | Concrete change | Acceptance gate |
|---|---|---|
| **0 — Capability contract** | Project a `MidiEndpointCapabilities` record from DICE per-stream runtime caps. Include explicit host directions, stream index 0, port count, trailing slot, DBC alignment, stable identity and epoch. Validate counts ≤8 and exactly one MPX slot when MIDI exists; reject overlaps/unsupported formations. | Host fixtures cover zero/asymmetric counts, duplicate counts across streams, bad DBS/slot and reset identity. Record the actual Saffire model and 48 kHz stream table; prove which jack each direction represents. No guessed 1/1 publication. |
| **1 — Host feasibility** | Add the separate MIDIDriverKit service and candidate nub, framework link, MIDI entitlement and user-client personality; initially exercise internal UMP loopback without hardware MIDI. Update `project.yml`, never the generated project. | Signed build/install works; source/destination appear once; reconfiguration/removal is safe. Record process placement, callback serialization, StartIO/StopIO ordering and the scheduling probe in §13.1. Resolve custom-nub placement or select one proven alternative before proceeding. |
| **1a — Shared session** | Extract one content pump/timing owner from audio ivars; introduce serialized audio/MIDI leases, retained storage and epoch invalidation. Wire both audio and MIDI start/stop to it. | MIDI lease alone starts the existing DICE duplex path with valid silence; audio open/close does not stop it or create a second pump; last release stops; start failure rolls back; teardown quiesces callbacks before freeing mappings. Existing audio-only behaviour passes regression checks. |
| **2 — RX and conversion** | Add bounded per-port storage, validated AM824 packet view and extraction before the PCM-bound early return. Implement MIDI-byte→UMP parsing and deliver on the MIDI work queue. | Physical Saffire input works with CoreAudio closed/open. Host vectors cover labels 0x80–0x83, invalid labels, all mux positions, DBC wrap, short/remainder payloads, NO-DATA, running status, System Common, interleaved Real Time, SysEx boundaries and stream-loss recovery. |
| **3 — TX composition** | Add UMP→bytes validation, per-port limiter and packet/epoch reservations. Bound the content horizon; select PCM or deadline silence; write MIDI after defaults/PCM and retire only on successful stream-0 publication with explicit later-discard accounting. | Simulations and host tests prove no byte is retired on rejected/canceled fills, no retry duplicates published bytes, later transport discards are counted as loss, and limiter aging remains correct across skipped opportunities. Physical Saffire output works MIDI-only, audio-active and during PCM starvation; PCM frame assignments and small-buffer operation remain correct. |
| **4 — Saffire qualification** | Exercise discovered supported rates, sustained duplex MIDI, long SysEx, overflow, repeated open/close, resets, TX recovery and unplug under load; collect bounded timing telemetry through MCP. | Record loss/duplicates, backlog, publication rejection, parser resets and latency distribution for MIDI-only and audio-active runs. No stale epoch replay, stuck leases or cross-service UAF. Qualify each additional rate/model explicitly. |

### 12.1 Proposed file ownership

- `Audio/Protocols/DICE/Core/DICEDuplexBringupController.cpp` and
  `Audio/Devices/`: consume the existing raw capabilities and create the MIDI
  projection without changing DICE register semantics or the audio-only profile.
- `Midi/DriverKit/`: new MIDI driver/nub and CoreMIDI object lifecycle;
  `Midi/Ump/`: pure bounded parser/serializer, independently host-testable.
- `Audio/Ports/`: lifetime-owned byte storage and reservation/session contracts;
  `Audio/Core/` + `Audio/Engine/`: shared stream-session ownership and pumping.
- `Audio/Wire/AM824/`: pure MPX MIDI mux/demux and limiter;
  `Audio/Wire/AMDTP/AmdtpTxPacketizer.cpp`: final candidate-image composition;
  `Audio/Engine/Direct/Rx/RxAudioPacketProcessor.cpp`: validated RX integration.
- `Audio/DriverKit/ASFWAudioDevice.cpp`, `ASFWAudioDriverLifecycle.cpp`,
  `ASFWAudioDriverZts.cpp`, and `ASFWAudioNub.cpp`: switch current start/stop,
  recovery and completion ownership to the shared session; remove superseded
  paths once validated.
- `tests/`: pure framing/conversion/reservation/session tests; `tools/`: a
  hardware-independent fill-window simulator and a host MIDI scheduling probe.
  Add source/build registration in `project.yml` and the relevant test CMake
  list when files are introduced. `Isoch/` remains payload-opaque.

### 12.2 Decisions to make concrete before hardware TX

1. **Fill window:** express bounds in packet/cycle coordinates and the stream
   epoch. Simulate 48 kHz blocking cadence, dispatch stalls, skipped packets and
   PCM retention with today's 3-cycle finality configuration. Choose the upper
   bound and silence deadline from evidence; do not bake 375 µs into a promise.
2. **Offer service and outcomes:** finish the existing generic batch-notification
   route and verify offer→service latency. Carry bounded MIDI reservation
   provenance through terminal payload selection for loss attribution; do not
   infer delivery from `CommitFill`. Preserve at-most-once ordering on loss.
3. **PCM availability:** `NotYetPublished` can wait until the selected deadline;
   expired content can become silence; wrong epoch invalidates the operation;
   a concurrent rewrite gets a bounded retry/deadline policy. Invalid geometry
   is an error. Do not flatten all failures into successful silence.
4. **Queue loss:** TX validates and reserves capacity before publishing any
   message's bytes. RX overflow marks a discontinuity at the correct queue
   position and resets parser state there. Define bounded streaming SysEx
   recovery so an overflow cannot join fragments into a fabricated message.
5. **Reset:** cancel unpublished reservations, drop/count bytes whose delivery
   is uncertain, reset parser/limiter state and reject stale callbacks by epoch.
   Never replay previously published bytes automatically. Rate changes use the
   same explicit quiesce/reconfigure boundary while retaining logical identity.
6. **Reference limiter:** age once per eligible port opportunity, independently
   of callback batching; rollback emitted-byte debit without undoing elapsed
   time. NO-DATA and canceled packets must not accidentally double-age state.

Run targeted host suites first (`./build.sh --test-only --test-filter ...` with
the actual introduced suite names), then the full C++ suite for shared-session
or TX changes and `./build.sh --no-bump` for IIG/signing integration. Host stubs
cannot validate service matching, dispatch or hardware publication. Batch
physical checks at each milestone and read the driver through the ASFW MCP
control plane; the current documentation review did not start streams or alter
hardware state.

**Deferred:** BeBoB plug discovery and channel-position commands (§4.2), Oxford
port policy, secondary-stream MIDI routing, other Saffire variants, MIDI 2.0
translation and driver-side future scheduling. RX precedes TX to isolate the
first wire change, but still requires audio regression and lifetime checks.

---

## 13. Open questions

### 13.1 Does MIDIServer schedule for a MIDIDriverKit driver? — **test at stage 1**

§7.3 is the documented CoreMIDI contract (`MIDIServices.h:913-932`). It is *not*
verified for the DriverKit-era path. Per the ground-truth rule in `CLAUDE.md`,
behavioural contracts are validated empirically, never from recall.

**Probe:** publish an endpoint with zero/unset advance time. Send several
distinct messages with separate future timestamps (for example +100, +300 and
+500 ms), both through `MIDISend` and the UMP event-list API. Record callback
host ticks and received words into preallocated telemetry, then inspect them
off the RT thread. Compare against the client's deadlines in the same host
clock domain. Run immediate, late and future cases, at idle and under load.

- Callbacks preserve each future deadline within a measured tolerance → supports
  host scheduling for the tested OS/API path; the bytes-only ring remains viable.
- Early/coalesced callbacks → inspect utility words and actual host behaviour;
  the current scheduling assumption failed. Resolve a supported deadline route
  before building the hardware TX path.

Keep the test OS build, API, property values, utility words and latency
distribution with the result. One delayed callback is not proof that a batch
of independently timestamped events preserves its spacing.

### 13.2 Provider class for the MIDI service — **settle at stage 1**

Whether `IOUserMIDIDriver` can match a custom nub the way `IOUserAudioDriver`
does, or must root at `IOUserResources` as Apple's sample does. See §9.1 for both
shapes and the fallback. Does not affect ring ownership or any wire behaviour.

### 13.3 Untraced Focusrite constants

The credit count `N` and the in-flight threshold at ring offset `+0x58`
(§3.2/§8.2) were not traced to their runtime source. They are tuning constants,
not semantics, and are moot if Linux's accumulator is used instead.

### 13.4 Apple's MIDI-plug policy

`references/IOFireWireAVC/` and `references/IOFireWireFamily.kmodproj/` are
present in this checkout; the previous absence claim was stale. Linux covers the AM824 MIDI framing described here;
that does not settle host scheduling, parser edge cases or ASFW lifecycle. Apple's view on MIDI plug
*policy* (which external plugs to expose, naming) remains unexamined; the
Focusrite decompilation partially substitutes.

### 13.5 Entitlement provisioning

`com.apple.developer.driverkit.family.midi` must be on the provisioning profile.
The dext currently carries `family.audio` and `family.scsicontroller`
(`ASFWDriver/ASFWDriver.entitlements`). The local sample confirms its set (§5.2), and the installed
`IOUserMIDIDriver.iig` confirms the MIDI-family entitlement; what is unconfirmed is whether the MIDI family is grantable on
this account. Check **before** stage 1, not during it.

---

## 14. Timing budget

The proposed MIDI pump uses transport completion wakes. That does not by itself
bound latency: the current fill cursor is also constrained by PCM availability
and may run far ahead (§10). HAL frame counts are a separate timing domain;
see [CoreAudio HAL timing domains](COREAUDIO_HAL_TIMING_DOMAINS.md).

### 14.1 Where the time goes

| Term | Value | Interpretation |
|---|---|---|
| Completion group | 8 cycles = **1.0 ms** | `Shared/Isoch/IsochQueueGeometry.hpp:23`; nominal cadence, not a dispatch guarantee |
| Freeze frontier lead | 3 cycles = **375 µs** | `IsochQueueGeometry::kPayloadFinalityLeadPackets` = repoint guard (2) + 1; configured boundary, not a measured wall-clock delay |
| Port opportunity gap, blocking 48 kHz | **125 or 250 µs** | `AmdtpCadence.cpp:8-13`: three DATA packets and one NO-DATA per four cycles; mean is 166.7 µs |
| Rate-limiter average | about **323 µs/byte** at saturation | 3093 B/s; discrete eligibility and FIFO credit make actual spacing nonuniform |
| Content fill lead | **to be selected and measured** | Must be bounded as in §10.3(b) |
| Ring backlog | load-dependent | An additional 100 bytes at 3093 B/s represents about 32 ms of serialization |

**TX chain:** host delivery → ring backlog → completion/dispatch wait →
selection into the bounded fill window → wire opportunity and limiter → device
FIFO/UART. Do not simply add independent-looking maxima: fill lead and port
selection refer to the same packet timeline. Simulate that timeline, then
measure callback-to-wire and end-to-end latency separately.

The former **1.4–2.4 ms** estimate is not established. It assumed a near-wire
fill cursor, treated mean DATA spacing as a maximum, and omitted dispatch
stalls and backlog. The repository already records 5.0–5.25 ms DriverKit
queue stalls (`IsochQueueGeometry.hpp:28-34`), so 1 ms completion grouping
cannot be a hard jitter bound.

**RX chain:** device/UART → packetization → nominal 0–1 ms completion batching
→ receive-queue delay → extraction → MIDI work-queue delay → parser completion
→ `Source::Send`. The batching term alone is not the end-to-end delay. A
three-byte DIN message itself takes 960 µs at 31.25 kbaud, and a parser may
need several packets before it can emit the complete message. Focusrite's 4 ms
lookahead is an emission policy, not a measured latency to compare against.

### 14.2 Scheduling policy

- Drive the content pass and RX draining from existing completion notifications;
  avoid adding a periodic MIDI poll and its extra batching interval.
- Notify the MIDI work queue promptly, with coalescing that cannot lose a wake
  (§9.3). Do not accumulate bytes merely to reach an arbitrary batch size.
- Keep conversion, notifications and ring operations bounded. A separate work
  queue does not eliminate scheduling stalls or guarantee RT priority.
- Retain partial messages until complete, while preserving Real Time handling.
- Use one MIDI composition path in MIDI-only and audio-active operation, and
  measure both rather than assuming their scheduling is identical.

### 14.3 Failure cases to validate

Exercise PCM unavailable/expired/wrong-epoch, an unready sibling stream, a
missed freeze frontier, silence available across the whole prepared horizon,
queue overflow and reset during a reservation. None may cause MIDI to be
silently retired without publication or repeated by a later commit. PCM must
retain its intended frame assignment and must not be prematurely replaced with
silence. Freeze races and resets can still lose physically undelivered data;
report them explicitly instead of claiming delivery from a software commit.

### 14.4 Measurement and constraints

Keep the existing completion geometry while designing MIDI. Changing it needs
a separate audio-timing validation; neither 1 ms nor any other period is
inherently below a universal MIDI “noise floor.” Keep the rate limiter and the
first-eight-block compatibility cap.

Record arrival, reservation, publication, target cycle and RX delivery times
with epoch and clock-domain labels in bounded telemetry. Report median, upper
percentiles, maximum, loss and duplicate counts under MIDI-only, audio-active,
underrun, multi-port and SysEx load. A valid cycle/host correlation can support
RX timing diagnostics, but the current `Source::Send` has no explicit host-time
argument. Do not promise timestamp preservation to clients until a supported
path is demonstrated.

---

## Appendix: constants and offsets

### AM824 MIDI

| Constant | Value | Source |
|---|---|---|
| MIDI conformant label base | `0x80` | `Audio/Wire/AM824/AM824Encoder.hpp:36` |
| One-byte label | `0x81` | `amdtp-am824.c:311`; `Saffire!0xe502` |
| Empty quadlet | `0x80000000` | `amdtp-am824.c:313-316`; `Saffire!0xe502` |
| Max MPX-MIDI channels | 1 | `amdtp-am824.h:28` |
| Ports per channel | 8 | `(dbc + f) % 8` |
| Max MIDI blocks per packet (Linux) | 8 | `amdtp-am824.c:28` |
| Nominal MIDI byte rate | 3125 B/s | MIDI 1.0 |
| Linux modelled rate | 3093 B/s | `amdtp-am824.c:22` |
| SysEx7 payload per UMP packet | 6 bytes | `MIDIMessages.h:322` |

### Timing

| Constant | Value | Source |
|---|---|---|
| Completion group | 8 packets / 1.0 ms | `IsochQueueGeometry.hpp:23` |
| Payload repoint guard | 2 packets | `IsochQueueGeometry::kPayloadRepointGuardPackets` |
| Freeze frontier | 3 packets / 375 µs | `IsochQueueGeometry::kPayloadFinalityLeadPackets` |
| TX preparation lead | 1008 packets / 126 ms | `AudioTimingGeometry.hpp:266-267` |
| Isoch cycle | 125 µs | IEEE 1394 |
| MIDI TX path | unmeasured; bounded fill policy required | §14.1 |
| MIDI RX batching | nominal 0–1 ms, plus dispatch/parser/host delay | §14.1 |

### MIDIDriverKit

| Constant | Value | Source |
|---|---|---|
| User client type | `1129149796` | `MIDIDriverKitTypes.h:42` |
| Entitlement | `com.apple.developer.driverkit.family.midi` | `IOUserMIDIDriver.iig` class comment |
| Driver object ID | `kIOUserMIDIObjectIDDriver = 1` | `MIDIDriverKitTypes.h:68` |
| Advance schedule property | `'mast'` | `MIDIDriverKitTypes.h` — **do not set** |
| Sample protocol | `MIDIProtocol_2_0` | `sample:…Device.cpp` — ASFW should use `_1_0` |
| Sample provider class | `IOUserResources` | `sample:Info.plist` — see §9.1 |

### Focusrite ring (per port, 9320 bytes)

| Offset | Field |
|---|---|
| +0x0000 | `writeIndex` |
| +0x0004 | `readIndex` |
| +0x0008 | in-flight history `u32[16]` (TX) |
| +0x0048 | credits (TX) |
| +0x004C | mask `N−1` (TX) |
| +0x0050 / +0x0054 | history head / tail (TX) |
| +0x0058 | in-flight threshold (TX) |
| +0x0060 | last emitted timestamp (TX) |
| +0x0068 | `u64 timestamp[1024]` |
| +0x2068 | `u8 byte[1024]` |

Device stride `0x24680` (149120) = 16 × 9320. Region stride `0xB6080` (745600)
= 5 × 149120. Total map `0x16C100` (1491200) = 2 × 745600.

### Focusrite function map

| Address | Symbol |
|---|---|
| `SaffireMIDIDriver!0x2206` | `DiceMIDIDriver::SetMIDIDataReceivedNotification` |
| `SaffireMIDIDriver!0x2402` | `DiceMIDIDriver::GetMIDIInfo` |
| `SaffireMIDIDriver!0x28e0` | `DiceMIDIDriver::ConnectToDriver` |
| `SaffireMIDIDriver!0x3a7c` | `DiceMIDIDriver::Send` |
| `SaffireMIDIDriver!0x3d7c` | `DiceMIDIDriver::MIDIDataReceived` |
| `SaffireMIDIDriver!0x40a4` | `DiceMIDIDriver::EnableSource` |
| `SaffireMIDIDriver!0x42d8` | `DiceMIDIDriver::Start` |
| `SaffireMIDIDriver!0x48bc` | `MIDIParser::FeedBytes` |
| `Saffire!0x798e` | `SaffireUserClient::clientMemoryForType` |
| `Saffire!0x8a4c` | `Saffire::GetMIDIInfo` |
| `Saffire!0xc156` | `Saffire::initHardware` |
| `Saffire!0xcdfc` | `Saffire::readMIDIQuadlet` |
| `Saffire!0xcf24` | `Saffire::ReadFirewireBuffers` (MIDI at `0xe14e`, `0xe279`, `0xe332`) |
| `Saffire!0xe502` | `Saffire::writeMIDIQuadlet` |
| `Saffire!0xe778` | `Saffire::FillFirewireBuffers` (MIDI at `0xf449`–`0xf48f`) |
| `Saffire!0x11c40` | `Saffire::AllocateStreams` (ring carve-up at `0x120f8`, `0x128ac`) |

### Linux reference index

All paths relative to `references/linux-sound-firewire-stack/firewire/`.

| Topic | Location |
|---|---|
| Rate constants, block cap | `amdtp-am824.c:19-28` |
| Max MIDI channels | `amdtp-am824.h:28` |
| `set_parameters`, channel count | `amdtp-am824.c:53-110` |
| Default `midi_position` | `amdtp-am824.c:102` |
| `set_midi_position` | `amdtp-am824.c:134-146` |
| `midi_trigger` | `amdtp-am824.c:243-260` |
| Rate limiter | `amdtp-am824.c:262-292` |
| `write_midi_messages` | `amdtp-am824.c:295-320` |
| `read_midi_messages` | `amdtp-am824.c:323-344` |
| MIDI written unconditionally | `amdtp-am824.c:353-368` |
| BeBoB channel position / section type `0x0a` | `bebob/bebob_stream.c:330-341` |
| BeBoB direction mapping | `bebob/bebob_stream.c:500-520` |
| `detect_midi_ports` | `bebob/bebob_stream.c:822-867` |
| External plug counts | `bebob/bebob_stream.c:942-948` |
| `midi_open` starts the stream | `bebob/bebob_midi.c:10-31` |
| Substream naming | `bebob/bebob_midi.c:75-82` |
| M-Audio formation `midi = 1` | `bebob/bebob_maudio.c:249-252` |
| M-Audio port counts | `bebob/bebob_maudio.c:290-294` |
| DICE port count | `dice/dice-midi.c:114-119` |
| DICE stream 0 only | `dice/dice-midi.c:59-63,76-80` |
| DICE/Focusrite port counts | `dice/dice-focusrite.c:13-20` |

### Apple sample reference index

Paths relative to
`references/CreatingAMIDIDeviceDriver/CreatingMIDIDriverSampleAppExtension/`.
These source checks establish what the example does, not measured ASFW lifecycle.

| Topic | Location |
|---|---|
| Subclass allocation and graph insertion | `CreatingMIDIDriverSampleAppDriver.cpp:81-97` |
| MIDI user-client forwarding | `CreatingMIDIDriverSampleAppDriver.cpp:123-150` |
| Driver I/O fan-out | `CreatingMIDIDriverSampleAppDriver.cpp:163-181` |
| Control handlers dispatch synchronously | `CreatingMIDIDriverSampleAppDriver.cpp:185-210` |
| Entity protocol and initial setup | `CreatingMIDIDriverSampleAppDevice.cpp:57-65` |
| Device I/O dispatch and offline property | `CreatingMIDIDriverSampleAppDevice.cpp:81-118` |
| UMP loopback callback | `CreatingMIDIDriverSampleAppDevice.cpp:122-140` |
| Reinstall after entity addition | `CreatingMIDIDriverSampleAppDevice.cpp:147-164` |
| Running/stopped configuration branch | `CreatingMIDIDriverSampleAppDevice.cpp:205-224` |
| Provider and CoreMIDI user-client dictionary | `Info.plist` |
| MIDI family entitlement | `CreatingMIDIDriverSampleAppDriver.entitlements` |
