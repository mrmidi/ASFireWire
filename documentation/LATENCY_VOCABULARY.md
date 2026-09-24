# Latency vocabulary and reference planes

**Status:** shared vocabulary for the timing, ZTS and TX-ownership work
(Linear FW-174, supporting FW-180 / FW-189 / FW-212). It defines what each
latency-shaped number *means*. It does not settle any value: numbers quoted
below are either main's current constants (with their source) or research
figures from the `midi` branch, labelled as such.

Distilled from the `midi` branch's `AUDIO_LATENCY_LEDGER_AND_SSOT_PLAN.md`
(the "ledger") and reconciled with main's existing timing documents:
[`TRANSFER_DELAY_AND_OTHER.md`](TRANSFER_DELAY_AND_OTHER.md) §4–5,
[`ZTS_BUFFERS_AND_IRS.md`](ZTS_BUFFERS_AND_IRS.md) §9.2 and
[`ZTS_AND_SYT.md`](ZTS_AND_SYT.md) §13. Where those documents use a different
word for the same thing, the mapping is in [§10](#10-mapping-to-existing-documents).

---

## 1. Three rules

Violating these is what made earlier latency accounting unreadable.

1. **Adjacent physical intervals add. Accounting allowances do not stack on the
   interval they cover.** A safety offset is the guarantee that a write lands
   before a deadline, not a delay added on top of that deadline.
2. **An interval is defined by its start and end events, not by its name.** If
   an intermediate event is never observed, the spans either side of it are
   *one* interval. Splitting it invents an attribution no measurement supports.
3. **A configured frame count is not a measured duration.** Geometry constants
   describe nominal leads, capacities and batch contents. The real delay
   distribution is a separate quantity, and is unmeasured unless stated.

A corollary: **frames are a unit, not a type.** Two quantities expressed in
frames are not addable merely because their units match.

## 2. Five kinds of number

Every latency-shaped number in the project is exactly one of these. Say which.

| Kind | Examples | Can be added to a physical interval? |
|---|---|---|
| **Measured physical interval** | `RTL_raw`, `RTL_ts` (electrical loopback) | yes, if adjacent (rule 1) |
| **Stamped / requested instant** | SYT presentation time, TX transfer delay | no — it is what we *ask* the device to do |
| **Declaration to CoreAudio** | device latency, stream latency | no — it is a claim, verified only by a residual |
| **Scheduling allowance** | safety offsets, client IO buffer | only as scheduling distance, never on top of the interval it bounds |
| **Depth / capacity** | rings, horizons, preparation leads, caches | **never** — capacity is not delay (§6) |

## 3. The TX path: events and intervals

Follow one output frame. Events:

| Event | Meaning | Observed today? |
|---|---|---|
| `E0` | HAL client writes the frame into the shared output buffer | bracketed by the IO callback (`WriteEnd`) |
| `E1` | the packet carrying it reaches content finality | decision exists in the TX producer; not timestamped on main |
| `E2` | OHCI IT DMA transmits that packet | per-packet completion status |
| `E3` | the device receives the packet | **never observed** |
| `E4` | the device's nominal presentation instant (SYT) | **stamped by us, not observed** |
| `E5` | analog signal at the output jack | no |

| Interval | Span | What is known | Accounting |
|---|---|---|---|
| `I1` | E0 → E1 | variable, ≥ 0: how far ahead of finality the HAL scheduled the write | **covered by the output safety offset (`A2`) — do not add `A2` on top** |
| `I2` | E1 → E2 | nominal lead only (the finality-to-transmit frontier); per-packet distribution unmeasured | — |
| `I3` | E2 → **E4** | the stamped SYT offset: SYT = transmit cycle + TX transfer delay (main: 12800 ticks = 25 frames at 48 kHz, `ResolvedTimingGeometry::txTransferDelayTicks`). One quantity: `E3` is never observed, so wire transit is **inside** it | never add a separate "wire transit" term |
| `I4` | E4 → E5 | device DAC — **unknown**, not derivable from software | — |

## 4. The RX path: events and intervals

| Event | Meaning | Observed today? |
|---|---|---|
| `F0` | analog signal at the input jack | no |
| `F1` | ADC emits the digital frame | no |
| `F2` | device transmits the packet | **never observed** |
| `F3` | host OHCI receives the packet | yes (IR completion) |
| `F4` | decoded frame published as available in the capture ring | yes (publication), timestamp later than the event |
| `F5` | HAL client reads it | IO callback (`BeginRead`) |

| Interval | Span | What is known | Accounting |
|---|---|---|---|
| `J1` | F0 → F1 | device ADC — **unknown** | — |
| `J2` | F1 → **F3** | **not separately observed** (`F2` never is). `rxTransferDelayTicks` (12800) is a constant used to *construct* the receive timeline, **not** a measurement of device buffering | — |
| `J3` | F3 → F4 | nominal batch content: one RX timing group = 6 packets = 32 or 40 frames (`AudioTimingGeometry::kRxPacketsPerGroup`). That is how many frames a batch *contains*, not a bound on elapsed time; dispatch delay is extra and unbounded by geometry | — |
| `J4` | F4 → F5 | variable, ≥ 0: capture-ring wait while the reader lags the writer | **covered by the input safety offset (`A4`) — do not add `A4` on top** |

## 5. Accounting terms: what we declare to CoreAudio

A different kind of object from §3–§4. Only some also change scheduling.

| Term | main source | Changes scheduling? | Intended to cover |
|---|---|---|---|
| `A1` output device latency | `IAudioDeviceProfile::TxReportedLatencyFrames()` → `SetOutputLatency` (`ASFWAudioDriverGraph.cpp`, "Reported HAL latency") | **no** — pure declaration | the part of `I3`+`I4` beyond the HAL timestamp plane (depends on §8) |
| `A2` output safety offset | `TxSafetyOffsetFrames()` → `SetOutputSafetyOffset` | **yes** | the requirement E0 precedes E1, i.e. `I1` ≥ 0 |
| `A3` input device latency | `RxReportedLatencyFrames()` → `SetInputLatency` | **no** | `J1`+`J2` and part of `J3`, depending on the plane |
| `A4` input safety offset | `RxSafetyOffsetFrames()`, floored by `RequiredInputSafetyFrames` (max group 40 + jitter 64 = 104) | **yes** | visibility margin: `J4` ≥ 0 |
| `A5` client IO buffer | set by the client (`kAudioDevicePropertyBufferFrameSize`) | **yes** | counted by CoreAudio itself |
| stream latency | `SetLatency(0)` on both streams today | **no** | — |

Apple's composition of a round trip from these (Jeff Moore / Sven Behne,
coreaudio-api 2004): `2×A5 + A2 + A4` (**declared scheduling**) plus
`A1 + A3 + stream latencies` (**declared hardware latency**). Both
`rtl_loopback` and `hal_geometry` compute exactly this, and withhold it when
any term is unreadable.

The applied values are in the `Reported HAL latency` line and in the `[Timing]`
line, which prints the whole resolved geometry (FW-177). Drivers before FW-177
also printed `TimingCursorPolicy … outSafety=8 inSafety=8`; those were
fallback-policy values, **not** what was applied.

## 6. Depths that are not latency

Capacities, horizons and leads. None of these is a delay, and none may appear
in a latency sum. Restate this wherever these constants live.

| main constant (`AudioTimingGeometry`) | Value | Why it is not latency |
|---|---:|---|
| `kTxHardwareRingPackets` | 48 pkt | OHCI-owned transmit program: ownership capacity |
| `kTxPreparationSlackPackets` | 96 pkt | producer scheduling tolerance |
| `kTxCoverageLeadPackets` | 144 pkt | refill-safety sub-budget |
| `kTxPreparationLeadPackets` | 336 pkt | how far ahead packets are prepared; packets armed early carry silence until content arrives, so capacity alone does not establish playback delay |
| `kTxDataHorizonPackets` | 400 pkt | data horizon |
| HAL frame ring / ZTS period | 1536 fr | one ZTS period; the wrap unit of the ADK ring, not a delay |
| `kRxDescriptorPackets` | 504 pkt | receive storage |

(`midi`-branch equivalents with different names — `kTxPreparedTargetCycleSlots`,
`kPcmPublicationCacheFrames` — are the same category.)

## 7. Measured quantities and their reference planes

| Quantity | Tool | Start event | End event | Plane / meaning |
|---|---|---|---|---|
| **`RTL_raw`** | `tools/rtl/rtl_loopback` | sample written into an output IO buffer | same sample seen in an input IO buffer | **client buffer plane**, counted in delivered frames; the whole thru time `E0…E5 + F0…F5`, both converters included |
| **`RTL_ts`** | `rtl_loopback` | output IOProc timestamp of that frame | input IOProc timestamp of that frame | **HAL sample-time plane**. Timestamps carry safety but not hardware latency, so a truthful device returns `in_hw + out_hw` — **not zero** |
| **scheduling distance** | `rtl_loopback` | — | — | `RTL_raw − RTL_ts`, paired per trial; reconciles with declared scheduling (`2×A5 + A2 + A4`); contains no latency term |
| **residual** | `rtl_loopback` | — | — | `median(RTL_ts) − (A1 + A3 + stream)`; positive = we under-declare. The only number that tests a declaration |
| **HAL timeline slope / ppm** | `tools/halprobe/hal_geometry --clock` | — | — | the **HAL-published** timeline (driven by our ZTS), not the word clock |
| **callback interval** | `hal_geometry --io` | IOProc entry | next IOProc entry | host time between callbacks |

Electrical loopback includes **both** converters (`I4` + `J1`) and cannot
separate them. Any number attributed to one converter is invented unless it
comes from a device specification or a separate measurement.

## 8. The open question: which plane is the ZTS anchored in?

The receive-clock path constructs a presentation coordinate (packet bus ticks
+ SYT offset + RX transfer delay) and projects it into host time for the ZTS.
That establishes how the RX-derived timestamp is *built*; it does not establish
that TX content reaches its requested `E4` on the same coordinate, nor where
either converter lies relative to it.

| If the ZTS reference is… | `A1` should cover | `A2` bounds `I1` against |
|---|---|---|
| `E4` (presentation) | `I4` only — the unknown DAC residual | `I2 + I3` ahead of the reference |
| `E2` (transmission) | `I3 + I4` | `I2` |

Even under the presentation reading `A1` is not zero: it is the unknown `I4`.
An exit criterion demanding a residual-free sum would force someone to invent
a number for `I4`. This decision belongs to FW-189 (timeline authority) and
FW-180 (geometry ownership), and must be taken against a measured residual
(FW-176 baseline), not a model.

## 9. Unknowns — state them, never zero them

| Quantity | Status |
|---|---|
| `I4` DAC delay, `J1` ADC delay | unknown; electrical loopback measures only their sum with everything else |
| `E3`, `F2` | never observed; spans across them are single intervals |
| `I1`, `J4` distributions | variable; observable only as spread (`sd`) in `RTL_raw` today |
| `I2`, `J3` distributions | nominal geometry only |
| absolute RTL on main | **not yet captured** — see [`MEASUREMENT_BASELINE.md`](MEASUREMENT_BASELINE.md) |

## 10. Mapping to existing documents

| Term here | `TRANSFER_DELAY_AND_OTHER.md` | `ZTS_BUFFERS_AND_IRS.md` | `ZTS_AND_SYT.md` §13 |
|---|---|---|---|
| `I3` stamped SYT offset | "TRANSFER_DELAY" (§2, §5) | — | SYT publication |
| `A1`–`A4` | §4 "safety offsets & reported latency" | — | input safety floor |
| depths (§6) | §5 unified timing model | §9.2 "three periods" (ZTS period, HAL IO period, interrupt) | §13 A "Authoritative Values" |
| ZTS plane (§8) | — | §9 ZTS publish cadence | §13 C "ZTS Publication" |

## 11. What was deliberately not ported

The `midi` branch also carried `ASFWDriver/Audio/Runtime/AudioLedgerIntervals.hpp`.
Despite its name it is **not** vocabulary: it is always-on runtime stamping
(atomic interval statistics plus 64-slot seqlock stamp rings) embedded in
`AudioTransportControlBlock` and fed from real-time IO, IR and IT paths. On
`midi` HEAD it is write-only — its only reader sits in a TX path disconnected
since WP-6 (`2033e16`). It is not ported. If interval measurement is needed
again it belongs in the observability substrate (FW-171 / FW-175) as an
optional sidecar, not in a shared ownership structure.
