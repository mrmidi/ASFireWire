# Zero Timestamp (ZTS) and SYT Timing Architecture

This document provides a comprehensive guide to the clock recovery, synchronization, and presentation timing architectures in the ASFW Driver. It explains how the driver bridges the asynchronous Apple CoreAudio host domain with the synchronous, cycle-aligned IEEE 1394 (FireWire) bus domain.

> [!IMPORTANT]
> **Document state and authority:** Sections 4–8 preserve defect analysis from
> the pre-FW-53 working tree. Sections 10F and 11 preserve the intermediate
> research conclusion that favored short DMA groups; that conclusion is now
> explicitly historical. Section 12 records the implementation chronology.
> **Section 13 is the authoritative current geometry** and supersedes older
> ASFW geometry recommendations elsewhere in this document.

---

## 1. High-Level Architecture Overview

Audio devices require synchronous timing to prevent underruns or overruns. Because macOS AudioDriverKit (ADK) operates in user space and CoreAudio paces its I/O callbacks based on host time (Mach absolute time), the driver must map the physical hardware clock of the FireWire bus to the host's system clock.

We achieve this using a two-part timing system:
1. **Zero Timestamp (ZTS) Pipeline**: Back-corrects the cycle-granular OHCI
   packet timestamp against a paired cycle-timer/host-time observation, then
   publishes periodic phase anchors to CoreAudio that track the hardware rate.
2. **SYT Presentation Timing Servo**: Recovers the input device's sample-clock
   cadence and drives the transmit (TX) engine's packet presentation timestamps
   using a deadbanded, hold-and-nudge software control loop. This driver-side
   servo operates on top of, and does not replace, the device's physical audio
   clock PLL/jitter-rejection domain.

```
 ┌────────────────────────────────────────────────────────┐
 │                   FIREWIRE BUS DOMAIN                  │
 └───────────────────────────┬────────────────────────────┘
                             │ (Rx Packets with Raw HW Timestamps)
                             ▼
 ┌────────────────────────────────────────────────────────┐
 │               ISOCH RECEIVE DMA INTERRUPT              │
 ├────────────────────────────────────────────────────────┤
 │ 1. Read master CYCLE_TIMER register.                   │
 │ 2. Compute packet Age: delta between timer & HW stamp. │
 └───────────────────────────┬────────────────────────────┘
                             │
                             ▼
 ┌────────────────────────────────────────────────────────┐
 │                ZTS PIPELINE (Host Time)                │
 ├────────────────────────────────────────────────────────┤
 │ 1. Convert Age in 1394 ticks to Nanoseconds.          │
 │ 2. Scale Nanoseconds to Mach absolute ticks.           │
 │ 3. Subtract from interrupt time -> stamped cycle start.│
 │ 4. Project forward to ZTS grid frame boundary.         │
 │ 5. Seed CoreAudio: UpdateCurrentZeroTimestamp().       │
 └───────────────────────────┬────────────────────────────┘
                             │
                             ▼
 ┌────────────────────────────────────────────────────────┐
 │                 COREAUDIO I/O ENGINE                   │
 └────────────────────────────────────────────────────────┘
```

---

## 2. Zero Timestamp (ZTS) Architecture

### Purpose in AudioDriverKit
CoreAudio paces its virtual audio engine by expecting the driver to periodically call:
```cpp
audioDevice->UpdateCurrentZeroTimestamp(uint64_t sampleTime, uint64_t hostTime);
```
*   `sampleTime`: The monotonically increasing frame counter (e.g. `0`, `192`, `384`...).
*   `hostTime`: The Mach absolute time corresponding to the driver's
    hardware-derived estimate of that sample frame on the device stream
    timeline. It is not a direct observation of the physical converter.

If ZTS anchors are delayed, jittery, or missing, CoreAudio will experience sync loss, resulting in audio glitches or complete silence.

---

### Step-by-Step ZTS Host Time Calculation

During each packet-drain interrupt, the driver calculates `hostTime` using the following pipeline:

#### Step 1: Decode the OHCI Packet Status Timestamp
The OHCI hardware writes a 16-bit `timeStamp` into the descriptor status
writeback. This is **not** the low 16 bits of the 32-bit bus `CYCLE_TIMER`.

The IEEE 1394 cycle timer is formatted as a 7-bit `cycleSeconds`, a 13-bit
`cycleCount`, and a 12-bit `cycleOffset`. Its literal low 16 bits would
therefore contain all 12 offset bits and only four cycle-count bits, which is
not the descriptor format.

OHCI 1.1 instead defines the descriptor timestamp as:

```text
timeStamp[15:13] = cycleSeconds[2:0]
timeStamp[12:0]  = cycleCount[12:0]
```

The 12-bit `cycleOffset` is omitted. Software can identify the bus cycle, but
cannot recover the event's position within that 125 microsecond cycle from the
descriptor timestamp alone. ASFW therefore expands the timestamp with an
intra-cycle offset of zero.

The event represented by the stamp is context-specific:

*   **Isochronous Transmit (IT), `OUTPUT_LAST`**: OHCI 1.1 Table 9-3 says it
    identifies the cycle for which the IT DMA controller queued transmission
    of the packet. The value is written when `xferStatus` is written.

    > "indicates the cycle for which the IT DMA controller queued the transmission of this packet"

*   **Asynchronous Transmit (AT)**: It represents the packet transmission time.
*   **Asynchronous Receive (AR)**: It is sampled at some time during packet
    receipt.
*   **Isochronous Receive (IR) in the ZTS path**: ASFW uses it as a
    cycle-granular receive anchor. The resulting host-time estimate refers to
    the beginning of the indicated bus cycle because no sub-cycle offset is
    available.

The fields wrap independently as follows:

*   `seconds` (bits 15–13): wraps every 8 seconds.
*   `cycle` (bits 12–0): valid cycle counts are 0–7999 and repeat each second.

#### Step 2: Compute Packet Age in FireWire Ticks
At the start of the interrupt handler, the driver reads the master `CYCLE_TIMER` register (`drainCycleTimer`) and records the entry host time using `mach_absolute_time()` (`drainHostTicks`).
$$\text{ageTicks} = \text{drainCycleTimer} - \text{rxCycleTimer}$$
Where $1\text{ cycle} = 3072\text{ ticks}$ at $24.576\text{ MHz}$.

#### Step 3: Convert Age in Ticks to Nanoseconds
$$\text{nanos} = \text{ageTicks} \times \frac{1,000,000,000\text{ ns}}{24,576,000\text{ Hz}}$$

#### Step 4: Convert Nanoseconds to Mach Host Ticks
Using the system timebase parameters (`mach_timebase_info`):
$$\text{ageHostTicks} = \text{nanos} \times \frac{\text{denom}}{\text{numer}}$$
*   *Apple Silicon Macs*: The host clock runs at $24.000\text{ MHz}$ ($\text{numer}=125, \text{denom}=3$).
*   *Intel Macs*: The host clock typically runs at $1.000\text{ GHz}$ ($\text{numer}=1, \text{denom}=1$).

Subtracting this age from the interrupt entry time yields the estimated host
time of the beginning of the bus cycle identified by the descriptor:
$$\text{packetReceiveHostTicks} = \text{drainHostTicks} - \text{ageHostTicks}$$

Despite the historical variable name, this is not an exact intra-cycle wire
arrival timestamp. Its precision is limited by the descriptor's omission of
`cycleOffset`.

#### Step 5: Grid Projection to ZTS Frame Boundary
CoreAudio requires ZTS anchors to align exactly to the ZTS period boundary (typically $P = 192\text{ frames}$).

1.  Calculate the next grid sample frame:

```math
\mathrm{gridFrame} = \left(\frac{\mathrm{packetFirstFrame}}{P} + 1\right) \times P
```

2.  Calculate the frame difference:

```math
\Delta\mathrm{frames} = \mathrm{gridFrame} - \mathrm{packetFirstFrame}
```

3.  Project the host time forward to this boundary:

```math
\Delta\mathrm{nanos} = \Delta\mathrm{frames} \times \frac{1{,}000{,}000{,}000}{\mathrm{sampleRateHz}}
```

```math
\Delta\mathrm{hostTicks} = \Delta\mathrm{nanos} \times \frac{\mathrm{denom}}{\mathrm{numer}}
```

```math
\mathrm{gridHostTicks} = \mathrm{packetReceiveHostTicks} + \Delta\mathrm{hostTicks}
```

The pair `(gridFrame, gridHostTicks)` is then published to CoreAudio.

The formula above illustrates the next-boundary case. The implementation uses
a grid-crossing loop so packets that start exactly on a boundary, skipped grid
points, and drains that cross multiple boundaries remain monotonic and on-grid.

Because the descriptor timestamp is cycle-granular, the resulting ZTS host
time may contain a fixed sub-cycle bias. This is not a sample-rate error.
Presentation-latency metadata and safety offsets account for constant transport
delay; ZTS primarily supplies the sample-time-to-host-time rate mapping.

---

## 3. SYT Presentation Timestamps (TxTimingModel)

### Role of SYT in IEEE 1394
In the IEC 61883-6 protocol, isochronous packets carry an **SYT** field in their CIP headers. The SYT field tells the receiver (the audio interface's hardware DAC) the exact bus cycle and offset at which the audio samples in that packet should be played.

To maintain synchronization, the driver must generate outgoing SYT values that tightly match the incoming clock recovered from the device.

---

### The Deadbanded Software Timing Servo (`AdjustOutputPhase`)
The transmitter phase is managed by a deadbanded hold-and-nudge software
control loop, conventionally describable as a software PLL and ported from the
original Saffire driver. It steers packet timing; it does not generate the
device's physical converter clock:

```cpp
int64_t TxTimingModel::AdjustOutputPhase(
    int64_t executionPhaseTicks,
    int64_t candidatePhaseTicks,
    const RxSytCadence::Snapshot& rx,
    Decision& decision) noexcept;
```

#### The Servo Algorithm:

1.  **Calculate Phase Error**: Compare the proposed phase candidate against the recovered delay-free phase from the receiver.

```math
\mathrm{phaseError} = \mathrm{candidatePhaseTicks} - (\mathrm{rx.recoveredPhaseTicks} - \mathrm{transferDelay})
```

2.  **Divide by Cadence Scale**: The scale is determined by the SYT interval (8 frames for blocking mode).

```math
\mathrm{cadenceScale} = \mathrm{sytIntervalFrames} \ll 8 = 2048
```

3.  **Compute Remainder and Complement**:
    *   If $\mathrm{phaseError} \ge 0$:

```math
\mathrm{remainder} = (\mathrm{phaseError} \times \mathrm{cadenceScale}) \pmod{\mathrm{rollingCadenceTicks}}
```

```math
\mathrm{complement} = \mathrm{rollingCadenceTicks} - \mathrm{remainder}
```

    *   If $\mathrm{phaseError} < 0$:

```math
\mathrm{remainder} = (-\mathrm{phaseError} \times \mathrm{cadenceScale}) \pmod{\mathrm{rollingCadenceTicks}}
```

```math
\mathrm{complement} = \mathrm{remainder}
```

4.  **Determine Frame Error and Correction**:
    If $\mathrm{remainder} \ne 0$:

```math
\mathrm{correctionTicks} = \frac{\mathrm{complement}}{\mathrm{cadenceScale}}
```

```math
\mathrm{signedRemainder} = \mathrm{remainder} > \frac{\mathrm{rollingCadenceTicks}}{2} \ ? \ (\mathrm{remainder} - \mathrm{rollingCadenceTicks}) : \mathrm{remainder}
```

```math
\mathrm{frameError} = \frac{\mathrm{signedRemainder}}{\mathrm{cadenceScale}}
```

5.  **Apply Deadband (Hold-and-Nudge)**:
    To prevent jitter under normal micro-fluctuations, the model will **hold** the phase unchanged unless the `frameError` exceeds the deadband configuration threshold (typically `409`) or `forceAdjust` is armed:
    ```cpp
    if (!forceAdjust_ && std::abs(frameError) <= config_.phaseDeadband) {
        return candidatePhaseTicks; // HOLD
    }
    ```
    If it exceeds the deadband, it applies the correction:
    ```cpp
    forceAdjust_ = false;
    return Normalize(candidatePhaseTicks + correctionTicks); // NUDGE
    ```

`frameError` is a historical implementation name. It is a normalized cadence
residual produced by the modulo-and-scale calculation, not a count of ordinary
PCM frames and not a raw OHCI tick delta. `correctionTicks`, by contrast, is
the phase correction applied in the bus-tick domain.

**Phase-domain invariant:** `rx.recoveredPhaseTicks`, `rxFree`, `SYTdiff`, and
`transferDelay` must each be documented as receive-time, presentation-time, or
delay-free phase. A transfer delay may be removed or restored exactly once.
Double-subtracting or double-adding it creates a persistent duplex phase error
of one or more bus cycles.

---

## 4. Continuity & Safety Mechanisms

TX wire timing follows Linux-style sequence replay. RX supplies the captured
data-block cadence and delay-free SYT offset; the latest completed TX packet
supplies the output bus-cycle anchor. No separate callback-phase continuity
tracker overrides or invalidates this replay timeline.

Actual failures remain explicit: missing execution anchor, unavailable replay
entry, invalid SYT on a DATA packet, or packet preparation failure.

### NO-DATA, NO-INFO, and DBC Are Not Interchangeable

These terms must not be collapsed into one generic "empty packet" rule:

*   A **scheduled NO-DATA packet** occupies one bus cycle and advances the
    blocking cadence, but carries no PCM frames/data blocks.
*   `SYT = 0xFFFF` means **NO-INFO**. It does not by itself prove that the
    packet has no payload. Linux uses `CIP_UNAWARE_SYT` for devices that send
    data packets with `SYT = 0xFFFF`, and documents devices that ignore such
    payloads.
*   A **forced underrun fallback** replaces a packet that was expected to
    carry data. It is not automatically equivalent to the cadence's scheduled
    NO-DATA slot and must not silently consume that cadence position.

Under the normal IEC 61883 start-event interpretation used by Linux AMDTP and
ASFW, DBC identifies the first data block in a packet. A zero-block packet
therefore does not increment DBC. For the captured Saffire blocking cadence,
the scheduled NO-DATA packet carries the DBC of the following DATA packet, and
that DATA packet repeats the same value before DBC advances by eight:

```text
packet:  NO-DATA  DATA  DATA  DATA  NO-DATA  DATA  DATA  DATA
DBC:        C0     C0    C8    D0      D8     D8    E0    E8
```

Thus "DBC unchanged on NO-DATA" means that no data blocks were consumed. It
does **not** mean "copy the previous packet's DBC": after a DATA packet with
DBC `D0`, the cadence-appropriate NO-DATA value above is `D8`.

The exact receive and transmit policy remains device-profile driven. Linux's
broader FireWire stack has explicit quirks for empty packets with wrong DBC,
end-event DBC, DBC measured in payload quadlets, skipped zero-DBC checks, and
wrong DBS. Those flags prove that no universal empty-packet rule is safe; they
are not a list of quirks ASFW should apply to every device.

For the current ASFW target scope, the relevant evidence is narrower:

*   **Saffire Pro 24:** Linux DICE uses `CIP_BLOCKING` without an
    empty-packet/wrong-DBC override. The captured repeated-DBC sequence is the
    applicable profile.
*   **Apogee Duet:** Linux applies `CIP_BLOCKING` plus
    `SND_OXFW_QUIRK_IGNORE_NO_INFO_PACKET`. Despite its name, the latter means
    the Duet does not render audio payload carried with `SYT = 0xFFFF`;
    DATA packets require a non-NO-INFO SYT. It is not a DBC quirk.

ASFW must preserve this separation:

| State | Bus-cycle index | PCM/data-block cursor | DBC/CIP header |
|---|---:|---:|---|
| DATA | advances | advances by payload blocks | packetizer/profile decision |
| Scheduled NO-DATA | advances | does not advance | cadence-appropriate packetizer/profile decision |
| Forced fallback | advances on wire | must not be inferred from cadence alone | explicit underrun policy; no generic DMA synthesis |

The packetizer and device profile therefore own the complete CIP header and
the corresponding DBC/cadence state transition. The DMA ring may transmit a
fully prepared fallback packet, but it must not create one by copying an
arbitrary previous Q0, forcing `SYT = 0xFFFF`, and assuming the result has
valid DBC semantics.

---

## 5. Performance and Logging Constraints

Because the timing pipeline runs inside the DCL hardware interrupt callback (the hot path), strict constraints are enforced:
*   **Zero Console Spanning**: No system logging (`os_log` / `ASFW_LOG`) is performed in the receive or transmit hot paths.
*   **Telemetry Ring Buffers**: Timing values (`ZtsTelemetryRecord`) are written to the lock-free `ztsTelemetry_` ring buffer. A separate watchdog/diagnostics worker thread periodically drains this buffer off the hot path to output debug messages safely.
*   **Discontinuity Rate-Limiting**: Discontinuity resets are logged via rate-limited macros (`ASFW_LOG_RL` at 5-second intervals) to prevent kernel log storms during a hardware dropout.

---

## 6. Telemetry Log Breakdowns and Calculations

To debug real-time synchronization, the driver outputs periodic telemetry lines for the receive path (`[Zts]`) and the transmit path (`[TxSyt]`). This section breaks down these logs and walks through the math of how raw values are converted.

### A. The Receive Telemetry log `[Zts]`

Example log line:
```text
[Zts] SEED count=1 frame=4224 host=239100154044 rawHost=239100150045 drainHost=239100152504 drainCycle=0x8e9d89d7 rxCycle=0x8e9d8000 age=2519 rawRxTs=0xe9d8 syt=0xaab0 sytLead=8880 rollCad=1048576 recPhase=179782320 desc=191 dec=8 period=192
```

#### Field Breakdown:
*   `SEED` or `UPD`: Indicates if this is the initial clock synchronization (`SEED`) or a subsequent update (`UPD`).
*   `count=1`: The ZTS publication sequence number (`rxZtsPublishCount_`).
*   `frame=4224`: The sample frame boundary projected onto the ZTS grid (multiple of the period, e.g., 192).
*   `host=239100154044`: The host time (in Mach absolute ticks) published to CoreAudio corresponding to `frame`.
*   `rawHost=239100150045`: The estimated Mach host time of the beginning of
    the bus cycle identified by the packet's OHCI status timestamp.
*   `drainHost=239100152504`: The host time when the interrupt was entered and the reference register was read.
*   `drainCycle=0x8e9d89d7`: The value of the 32-bit hardware `CYCLE_TIMER` register read at interrupt entry.
*   `rxCycle=0x8e9d8000`: The expanded 32-bit cycle-timer value for offset zero
    of the bus cycle identified by the packet timestamp.
*   `age=2519`: The difference (in FireWire ticks at 24.576 MHz) between the
    reference read and offset zero of the bus cycle identified by the packet
    timestamp.
*   `rawRxTs=0xe9d8`: The raw 16-bit status timestamp written by the OHCI DMA engine.
*   `syt=0xaab0`: The presentation timestamp from the CIP header.
*   `sytLead=8880`: The presentation lead time (in ticks) of the syt stamp: `presentationTicks - receiveTicks`.
*   `rollCad=1048576`: The recovered average cadence period in ticks.
*   `recPhase=179782320`: The recovered phase of the device clock in ticks.
*   `desc=191`: The DMA receive ring slot index (`descriptorIndex`).
*   `dec=8`: **Frames Decoded** — the number of audio frames carried in this packet.
*   `period=192`: The CoreAudio ZTS period in frames.

---

### B. Mathematical Walkthrough of ZTS Calculations

Using the values from the `SEED` log line above:
*   $\text{drainCycle} = \text{0x8e9d89d7}$
*   $\text{rawRxTs} = \text{0xe9d8}$
*   $\text{drainHost} = 239100152504\text{ host ticks}$

#### 1. Decoding `drainCycle` (CYCLE_TIMER register)
The register is split as:

```text
seconds = ((0x8e9d89d7 & 0xFE000000) >> 25) = 0x47 = 71 s
cycle   = ((0x8e9d89d7 & 0x01FFF000) >> 12) = 0x09d8 = 2520 cycles
offset  =  (0x8e9d89d7 & 0x00000FFF)        = 0x9d7  = 2519 ticks
```

Collapsing this to 24.576 MHz ticks (where $1\text{ cycle} = 3072\text{ ticks}$):
$$\text{referenceTicks} = 3072 \times (2520 + 8000 \times 71) + 2519 = 1,753,637,479\text{ ticks}$$

#### 2. Decoding and Expanding `rawRxTs`
The raw 16-bit timestamp $\text{0xe9d8}$ is decoded:

```text
timestampSeconds = (0xe9d8 >> 13) & 7       = 7 s
timestampCycle   =  0xe9d8       & 0x1FFF   = 2520 cycles
timestampOffset  =  0                        ticks
```

Aligning the 3-bit seconds field with the reference seconds ($71$):

```text
candidateSeconds = (71 & ~7) | 7 = 64 | 7 = 71 s
```

*   $\text{candidateTicks} = 3072 \times (2520 + 8000 \times 71) + 0 = 1,753,634,960\text{ ticks}$

#### 3. Calculating Age in FireWire Ticks
$$\text{age} = \text{referenceTicks} - \text{candidateTicks} = 1,753,637,479 - 1,753,634,960 = 2519\text{ ticks}$$
This matches the `age=2519` logged.

#### 4. Converting Age to Mach Absolute Host Ticks
On Apple Silicon, the host clock frequency is $24.000\text{ MHz}$ ($\text{numer}=125, \text{denom}=3$), while the FireWire clock frequency is $24.576\text{ MHz}$.

The conversion ratio is:
$$\frac{24.000\text{ MHz}}{24.576\text{ MHz}} = \frac{125}{128}$$

Calculating the host age ticks directly:
$$\text{ageHostTicks} = 2519 \times \frac{125}{128} \approx 2459.96 \approx 2459\text{ ticks}$$

Subtracting this from `drainHost` gives the cycle-granular packet host anchor:
$$\text{rawHost} = \text{drainHost} - \text{ageHostTicks} = 239100152504 - 2459 = 239100150045\text{ ticks}$$
This matches the `rawHost=239100150045` logged.

Because the descriptor does not report `cycleOffset`, this value corresponds
to offset zero in the indicated receive cycle. It must not be interpreted as
the exact sub-cycle instant at which the packet reached the link or converter.

#### 5. Calculating Decoded Frames (`dec`)
The raw frames decoded is calculated by taking the packet's payload size and dividing it by the data block size (DBS):

```math
\mathrm{dec} = \frac{\mathrm{packetLength} - 16}{\mathrm{dataBlockSize} \times 4}
```

*   Overhead is $16\text{ bytes}$ ($8\text{ bytes}$ for OHCI context/isoch headers + $8\text{ bytes}$ for CIP header).
*   For blocking 48 kHz mode, $\mathrm{dataBlockSize} = 8$ and the packet carries $8\text{ audio frames}$.

```math
\mathrm{dec} = \frac{272 - 16}{8 \times 4} = \frac{256}{32} = 8\text{ frames}
```

This matches `dec=8` logged.

#### 6. Calculating the Published Host Time Grid Anchor
CoreAudio requires the anchor to align to the grid boundary (e.g. $4224$, which is a multiple of $192$):
*   $\mathrm{packetFirstFrame} = 4216$ (since $4224 - 8 = 4216$, which is $8\text{ frames}$ behind the grid frame).
*   $\Delta\mathrm{frames} = 4224 - 4216 = 8\text{ frames}$
*   Converting $\Delta\mathrm{frames}$ to nanoseconds:

```math
\Delta\mathrm{ns} = 8 \times \frac{1{,}000{,}000{,}000}{48{,}000} \approx 166{,}666.67\text{ ns}
```

*   Converting $\Delta\mathrm{ns}$ to host ticks:

```math
\Delta\mathrm{hostTicks} = 166{,}666.67 \times \frac{3}{125} = 4000\text{ host ticks}
```
*   $$\text{host} = \text{rawHost} + \Delta\text{hostTicks} = 239100150045 + 4000 = 239100154045\text{ ticks}$$
(The actual log shows `239100154044` due to sub-nanosecond integer rounding in the exact timebase conversions).

---

### C. The Transmit Telemetry Log `[TxSyt]`

Example log line:
```text
[TxSyt] pkt=45963 flags=0x08 health=1 anchor=43228837 phasePre=43233456 phasePost=43233456 recPhase=42109616 rxFree=42096816 pErr=1136640 fErr=0 corr=0 lead=4619 wire=17419 rollCad=1048576 pend=4096 ridx=416 syt=0xd6b0 tgtZts=0 tgtDev=0 tgtDiff=0
```

#### Field Breakdown:
*   `pkt=45963`: The transmit packet index (`packetIndex`).
*   `flags=0x08`: Status flags:
    *   `0x01`: Timing model was seeded this packet.
    *   `0x02`: Force adjust was fired.
    *   `0x04`: Timing model was reseeded.
    *   `0x08`: Packet contains data payload (committed).
*   `health=1`: The timing model's lead health state:
    *   `0`: NotSeeded
    *   `1`: Healthy
    *   `2`: Warning (approaching margins)
    *   `3`: Late (late / underrun imminent)
*   `anchor=43228837`: The host time (in ticks) when this packet is scheduled to transmit.
*   `phasePre=43233456`: Output phase (in ticks) before slewing corrections are applied.
*   `phasePost=43233456`: Output phase (in ticks) after corrections are applied.
*   `recPhase=42109616`: The raw recovered device phase from the receiver.
*   `rxFree=42096816`: The recovered phase free of transmit transfer delay: $\text{rx.recoveredPhaseTicks} - \text{xmitTransferDelayTicks}$.
*   `pErr=1136640`: Phase error (in ticks): $\text{phasePost} - \text{rxFree}$.
*   `fErr=0`: Frame error (residual offset) computed by the software timing
    servo.
*   `corr=0`: The correction nudge applied to the phase.
*   `lead=4619`: Refill lead time (in ticks) — how far ahead the software prepared this packet relative to its transmission.
*   `wire=17419`: The estimated lead time (in ticks) when the packet physically hits the wire: $\text{lead} + \text{xmitTransferDelayTicks}$.
*   `rollCad=1048576`: The rolling cadence period in ticks.
*   `pend=4096`: Pending cadence ticks.
*   `ridx=416`: The index of the cadence entry.
*   `syt=0xd6b0`: The computed 16-bit SYT value written to this packet.
*   `tgtZts`: Target frame index computed from host ZTS.
*   `tgtDev`: Target frame index computed from device phase.
*   `tgtDiff`: Target frame offset difference: $\text{tgtZts} - \text{tgtDev}$.

---

## 7. Seeding Transmit Phase directly from Hardware (Device Clock)

### The Problem with Host-Time Seeding
In early versions of the driver, the transmit timing model initialized its phase (`phaseTicks_`) based on `packetAnchorTicks` (which represents the host-aligned transmit time of the packet being prepared). Because `packetAnchorTicks` is derived from the host's system clock, the initial difference between the host time at start and the physical hardware clock of the device (`rx.recoveredPhaseTicks`) contained a random fractional cycle offset. Since the deadbanded software servo locks onto the nearest cadence boundary from this initial seed, it froze a random cycle count offset (e.g. 2–5 cycles) modulo 16 between the transmit and receive SYT.

### The Solution: Hardware-Aligned Seeding
To reproduce the observed Saffire-style SYT epoch alignment, where transmitted
and received SYT cycle counts share the same modulo-16 epoch, ASFW seeds the
initial phase directly from `rx.recoveredPhaseTicks` rather than from the
host-paced `packetAnchorTicks`.

1. **Calculate the physical distance** between the scheduling anchor and the recovered receive phase:

```math
\mathrm{distanceTicks} = \mathrm{extOffsetDiff}(\mathrm{packetAnchorTicks},\ \mathrm{rx.recoveredPhaseTicks})
```

2. **Round this distance** to the nearest multiple of the 16-cycle SYT epoch ($49{,}152\text{ ticks}$):

```math
\mathrm{alignedDistance} = \mathrm{round}\left(\frac{\mathrm{distanceTicks}}{49{,}152}\right) \times 49{,}152
```

3. **Initialize the phase** using the receiver phase plus the aligned distance and the safety lead:

```math
\mathrm{phaseTicks\_} = \mathrm{rx.recoveredPhaseTicks} + \mathrm{alignedDistance} + \mathrm{initialLeadTicks}
```

Because `alignedDistance` is a multiple of $16\text{ cycles}$ ($0\pmod{16}$),
the cycle offset between the transmit and receive timelines is a whole
modulo-16 epoch. The encoded transmit SYT therefore preserves the intended
Saffire-style epoch alignment. This establishes the seed relationship; it does
not by itself claim complete wire-level equivalence with `Saffire.kext`.

---

## 8. Architectural Defects and Required Corrections

### Defect A: Mischaracterizing the ZTS `OSAction` as Timestamp Jitter
The current ZTS path is:

```text
IR drain
  -> publish (sampleFrame, hostTicks) to shared memory
  -> ZtsAnchorReady OSAction
  -> AudioDriverKit process snapshots the latest generation
  -> UpdateCurrentZeroTimestamp(sampleFrame, hostTicks)
```

`ASFWAudioNub.iig` explicitly defines the nub as a **cross-process** bridge
between the hardware driver and `ASFWAudioDriver`. Therefore a direct C++
callback from the hardware IR context to the `IOUserAudioDevice` is not
available in the current architecture. The earlier proposal to "bypass
OSAction and call inline" was based on the false assumption that both objects
execute in one dext task.

The `OSAction` also does not add error to the timestamp value itself. The
cycle-derived `hostTicks` is captured before notification; delayed
delivery changes when CoreAudio receives the anchor, not the
`(sampleFrame, hostTicks)` pair. The real costs are:

*   cross-process wakeup and scheduling overhead;
*   delayed visibility of an otherwise valid anchor;
*   intermediate observations being replaced if the consumer stalls.

The required design is therefore to keep the timestamp creation entirely in
the IR path, preserve the latest captured value unchanged, and let the
AudioDriverKit consumer publish each generation it observes. Skipped
generations are valid; CoreAudio's configured clock algorithm owns timestamp
history and smoothing. Eliminating `OSAction` would require a larger
architectural change that moves ZTS production into the AudioDriverKit process
or collapses the current process boundary. It is not a local callback
optimization.

### Defect B: TX Exposure Frontier Lags the CoreAudio Write Window (Under-Exposure)

> **Correction (2026-06-15).** Earlier revisions of this section described
> Defect B as *over-exposure* — a deep "fill-in-place" window of zeroed slots
> exposed far ahead of the CoreAudio write horizon (`E ≫ W`), shipping silence
> because data could not exist yet. **Direct measurement shows the opposite,
> and the prior framing was actively misleading.** The real failure is
> *under-exposure*: the producer's exposure frontier lags the CoreAudio write
> window. That description applied to the dead pre-FW-53 direct path and was
> never re-derived for the current AMDTP timeline path. The corrected analysis
> follows.

#### The three-cursor model

The TX path has three frame-domain cursors, all nominally 48 kHz:

| Cursor | Meaning |
|--------|---------|
| **T** | hardware transmit position (what the OHCI IT DMA ships on the bus now) |
| **W** | CoreAudio write frontier = `sampleTime + frameCount` |
| **E** | producer exposure frontier = `AmdtpPacketTimeline::ExposedFrameEnd()` |

A PCM frame reaches the wire intact **iff `T ≤ W ≤ E`**:

* `T ≤ W` — *written-before-transmit*. **Guaranteed by construction**: CoreAudio
  always writes ahead of the playout position, so a written packet always
  transmits correctly.
* `W ≤ E` — the packet exists when the writer reaches it.

The **only** failure mode is **`W > E` (under-exposure)** — CoreAudio delivers a
frame whose packet the producer has not exposed yet. This is the
`framesWithoutPacket` counter in `AmdtpPayloadWriter`.

#### Why "over-exposure" is harmless (and the old framing was wrong)

Because `T < W` always holds, any packet between `W` and `E` sits **above** the
hardware cursor. The writer (at `W`) fills it before the hardware (at `T`) ever
transmits it. **Exposing far ahead cannot ship zeros.** The old "capacity is not
latency / expose less" prescription solved a problem that does not exist in the
timeline path, while missing the one that does: an exposure frontier that does
not stay *ahead* of `W`.

#### What the measurement shows

`tools/analyze_payloadwriter.py` over 808 `[PayloadWriter]` records (~5.5 h):

* Miss buckets **`withoutPkt : outsidePkt = 89 : 1`**, `raced = 0`. The misses are
  overwhelmingly "writer ran ahead of exposure," not "writer arrived late."
* Lead margin `E − W_end`: mean **−6**, median **−8**, **min −56**, **58.5 %** of
  calls negative.
* `E − W_start ≈ +120 frames` against a **128-frame IO window** → the tail of
  each IO window spills ~8 frames (worst 56) past the exposure frontier. The
  deficit is *small and structural*, driven by Default-queue scheduling jitter.
* `W − HWcompletion ≈ 4883 frames` (~102 ms) is a **stable** pipeline offset
  (range 70), a different anchor from `E` — **not** the dropout driver. Do not
  conflate it with the `E − W` margin.

#### Root cause: a missing TX cushion that RX already has

The RX path raises its safety offset to `outputSafety + maxClientIO + jitter`
(`RequiredInputSafetyFrames`, ≈ 624 frames at 48 kHz, visible in the HAL device
properties) precisely to absorb scheduling jitter. **The TX exposure path had no
equivalent cushion** — it ran with ≈ 0 lead margin. That asymmetry *is* Defect B:
RX is protected from the same Default-queue jitter that starves TX.

#### The fix

The frames lost to `withoutPkt` were **already present in the host ring** when
the producer finally exposed the packet (that is what `W > E` means). Two
options, in order of preference:

1. **Cheap push fix (recommended, magnitude-justified).** Give the producer's
   audio-frame exposure lead a cushion symmetric to RX's:
   `RequiredOutputExposureFrames = maxClientIO + jitter`, named
   `AudioTimingGeometry::kTxExposureLeadFrames` (576 frames). Because `E` and `W`
   are clock-locked (the offset is constant; see the stable `W − HWcompletion`),
   raising the producer lead moves `E − W_start` one-for-one. Closing an 8-frame
   median / 56-frame worst deficit needs only ≈ +64 frames (~11 packets) of
   exposure lead — it fits the existing rings with room to spare. The producer
   path must be changed to **enforce** `kTxExposureLeadFrames`.
2. **Structural pull (clean, but not mandated by the data).** Have the packetizer
   fill PCM from the host ring *at prepare time*, gated on `W`, and emit an
   explicit cadence-correct NO-DATA/hold packet only on genuine source underrun.
   This is the Linux `amdtp-stream.c` / Saffire `FillFirewireBuffers` model and
   removes the producer↔writer rendezvous entirely. It is the right long-term
   shape, but the measured deficit no longer requires it.

> **Note on the simulator.** `tools/tx_payload_ownership_sim.py` initially
> concluded push needed an infeasibly large lead (> shared ring) and favored
> pull. That result over-modeled the cursors as independent relative to `T`;
> measurement showed `E` and `W` are clock-locked, which is why the cheap push
> fix works. Always calibrate the sim with `analyze_payloadwriter.py` before
> trusting its absolute rates.

#### Guardrails now in place

`ASFWDriver/Shared/Isoch/AudioTimingGeometry.hpp` now **names** the previously
implicit quantity (`kTxExposureLeadFrames`) and adds the compile-time invariant
whose absence hid this defect:

```cpp
static_assert(kTxExposureLeadFrames >= kHalIoPeriodFrames + kSchedulingJitterFrames,
              "TX exposure lead must cover one full IO window plus scheduling jitter");
```

The rate-dependent partner `RequiredOutputExposureFrames` lives beside
`RequiredInputSafetyFrames` in `AudioGeometryPolicy.hpp`.

#### Measurement caveat

In the analyzed run the source was **~99 % digital silence** (`nonZero` = 1.0 %
of `written`), so the `[Isoch] IT WIRE PCM dropout` *edge* counter is heavily
inflated by legitimate source silence. Re-measure the true dropout rate with
*continuous* nonzero audio. (Separately: the dext crashed with `fatal 309` at the
end of that log, and `withoutPkt` spiked from 1.14 % lifetime to 8.8 % in the
final window — a pre-crash degradation worth investigating on its own.)

Independent of all the above, the packet-ring **capacity** may remain deep for
wrap/recovery headroom: capacity is not latency, and (per the harmless-over-
exposure result) a deep ring never causes dropouts on its own.

### Defect C: Host-Time Seeding of Transmit Phase
*   **The Flaw**: Seeding the initial transmit phase using `packetAnchorTicks` imports the arbitrary host scheduling start phase relative to the device's clock. This results in a random cycle-count offset modulo 16, preventing the transmit and receive SYT cycle counts from aligning.
*   **The Solution**: Seed the transmit phase candidate directly from the recovered hardware receive phase (`rx.recoveredPhaseTicks`) shifted by the configured preparation lead distance and quantized to the nearest multiple of the 16-cycle SYT epoch (49,152 ticks). This locks the transmit and receive cycle counts to the individual cycle level.

---

## 9. Reference Implementations: Linux ALSA and libffado

This comparison is scoped to the two current target devices:

*   Focusrite Saffire Pro 24 / Pro 24 DSP (`DICE`);
*   Apogee Duet FireWire (`OXFORD` / OXFW).

Other Linux and FFADO quirks are used only to explain why packet policy must
be device-specific. They are not ASFW implementation requirements.

### A. Device-Specific Stream-Mode Verdict

#### Saffire Pro 24

The Saffire evidence is consistent:

*   FFADO's `configuration` selects the `DICE` driver. Its DICE path uses the
    ordinary AMDTP receiver and transmitter.
*   At 48 kHz, the FFADO transmitter emits fixed eight-frame DATA packets with
    valid SYT, interspersed with CIP-only NO-DATA packets. DATA advances DBC by
    eight; NO-DATA advances it by zero.
*   FFADO explicitly disables payload in DICE NO-DATA packets because DICE-II
    rejects it.
*   Linux `dice-stream.c` initializes both directions with `CIP_BLOCKING`.
    It starts the duplex domain with sequence replay enabled so playback
    follows the device's captured packet cadence.

**Adopted conclusion:** Saffire Pro 24 is a blocking AMDTP profile. At 48 kHz,
use eight-frame DATA packets, scheduled CIP-only NO-DATA gaps, valid SYT on
DATA, and the captured start-event DBC sequence.

#### Apogee Duet: Device-to-Host Capture

FFADO assigns the Duet to its `OXFORD` driver, whose receive adapter was built
for Oxford streams that can be non-blocking and have unreliable SYT. It:

*   accepts variable payload lengths;
*   ignores the received SYT and DBC for timing;
*   accumulates payload until it can expose an eight-frame pseudo-packet;
*   derives capture timing from packet arrival and a DLL.

This is meaningful evidence that an Oxford-compatible capture path should
tolerate non-blocking segmentation and bad timestamps. It is **not** proof
that the Duet itself transmits non-blocking packets: the source comment names
the FCA202 and says "possibly other" Oxford devices, while the same adapter is
installed for every FFADO Oxford device.

The newer Linux OXFW path has a Duet-specific entry and applies
`SND_OXFW_QUIRK_BLOCKING_TRANSMISSION` to both stream directions. ASFW's
existing device note also records the observed Duet output cadence as
blocking.

**Adopted conclusion:** treat Duet capture as blocking unless a Duet wire
capture proves otherwise. A robust parser may accept variable data-block
counts, but ASFW must not infer the operating mode solely from FFADO's shared
Oxford compatibility adapter.

#### Apogee Duet: Host-to-Device Playback

The playback evidence is stronger:

*   FFADO does not use a non-blocking Oxford transmitter. It uses the ordinary
    AMDTP transmitter: eight-frame DATA packets at 48 kHz, valid SYT on DATA,
    and CIP-only NO-DATA gaps with no DBC increment.
*   FFADO explicitly disables payload in Oxford NO-DATA packets because it
    causes distorted sound.
*   Linux forces blocking mode for the Duet.
*   Linux's Duet-specific `IGNORE_NO_INFO_PACKET` quirk says the device ignores
    audio payload when `SYT = 0xFFFF`, even though its level meter can move.
    Therefore a DATA packet with NO-INFO is not a valid silence or underrun
    substitute.
*   Linux enables sequence replay for the Duet, so host playback follows the
    data-block and SYT cadence captured from the device stream.

The two Linux Duet quirks are causally related. At 48 kHz, Linux's ideal
non-blocking generator emits six data blocks in every 125 us cycle, while its
normal SYT schedule contains a `NO_INFO` position every fourth cycle. With the
Duet's ignore-NO-INFO behavior, that generic non-blocking sequence would lose
the six blocks carried in each such position. Blocking mode instead produces
three eight-block DATA packets and one zero-block NO-DATA packet per four
cycles, preserving the same 48 kHz average while ensuring that payload appears
only with valid SYT.

The Duet may advertise non-blocking capability, and ASFW records that host
playback can function in that mode. That makes non-blocking a legitimate
future experiment, not the default duplex geometry.

**Adopted conclusion:** use blocking playback with valid SYT on every DATA
packet. Use `FDF = 0xFF`, `SYT = 0xFFFF`, and no payload only for an explicitly
scheduled or profile-generated NO-DATA packet.

Changing the Duet profile to non-blocking requires device-specific wire
evidence. For capture, verify six data blocks per cycle and DBC advancing by
six, rather than an eight/eight/eight/zero blocking sequence with repeated DBC
on the zero-block cycle. For playback, a controlled test must establish the
SYT pattern the Duet accepts without discarded audio; capability advertisement
and FFADO's generic Oxford receive tolerance are insufficient.

### B. Linux Kernel ALSA "Sequence Replay"
Instead of computing transmit timestamps dynamically using a mathematical PLL or host-time tracking, the Linux driver maps the playback stream (`OUT_STREAM`) directly to the capture stream (`IN_STREAM`) as its `replay_target`:
1. **Cadence Caching**: As capture packets arrive, the driver logs their exact cadence (number of data blocks and computed relative SYT offsets) in a ring buffer.
2. **Replayed Output**: When generating packets for playback, the driver retrieves the cached descriptor properties and copies them directly (`pool_replayed_seq`).
3. **Offset Cancellation**:
   * On capture, `compute_syt_offset` extracts the SYT relative ticks and subtracts the configured `transfer_delay` (presentation latency).
   * On playback, `compute_syt` retrieves this offset, adds the `transfer_delay` back, and re-encodes the 16-bit CIP SYT.
   * Consequently, the playback stream reuses the captured data-block and
     relative-SYT pattern, preserving its hardware-derived cadence and
     modulo-16 phase relationship.

This is not merely generic Linux machinery for our purposes. The DICE path
enables replay when starting the Saffire duplex domain. The OXFW path also
enables it for the Duet because the Duet has neither the jumbo-payload nor
voluntary-recovery exception.

### C. libffado-2.5.0 User-Space Driver
For the ordinary AMDTP path used by Saffire, `libffado` operates in user-space
and uses a `TimestampedBuffer` combined with a Delay-Locked Loop (DLL):
1. **Absolute Tick Expansion**: Upon packet receipt, it expands the 16-bit status cycle timer (`pkt_ctr`) and 16-bit CIP SYT to a 64-bit absolute cycle-tick timestamp (`sytRecvToFullTicks2`). This resolves modulo-16 wraps by comparing the SYT cycle to the arrival cycle.
2. **DLL Buffering**: Audio frames are written into a ring buffer tagged with this 64-bit timestamp.
3. **Transmit Gating**: The transmit engine reads the buffer head's presentation timestamp, subtracts the `transfer_delay` to find the target transmission cycle, and writes the encoded SYT back to the packet header if it falls within the transmit window.

The Oxford receive path used for Duet is an explicit exception: it does not
trust CIP SYT. It reconstructs an eight-frame internal timeline from payload
arrival time and a DLL. The transmit side still uses the ordinary fixed-block
AMDTP transmitter with valid SYT.

### D. DMA Program Construction in Linux ALSA & libffado
Unlike Apple's driver, which enforces a hardcoded static DCL ring size (100 groups of 8 packets), both the Linux ALSA driver and user-space `libffado` dynamically size the DMA descriptor ring and interrupt interval based on the user's ALSA period and buffer size settings.

#### 1. Linux ALSA DMA Geometry (`amdtp-stream.c`)
*   **Queue Size (DMA Buffer Depth)**: The total number of packets queued in the context is dynamically calculated from the ALSA buffer size:

```math
\mathrm{queue\_size}
=
\mathrm{DIV\_ROUND\_UP}
\left(
\mathrm{CYCLES\_PER\_SECOND}
\times
\frac{\mathrm{events\_per\_buffer}}{\mathrm{sample\_rate}}
\right)
```

Where $\mathrm{CYCLES\_PER\_SECOND} = 8000$ packets/s. For a buffer size of 192 frames (3 periods of 64 frames) at 48 kHz:

```math
\mathrm{queue\_size}
=
\mathrm{DIV\_ROUND\_UP}
\left(
8000 \times \frac{192}{48000}
\right)
= 32\text{ packets}
```

*   **Interrupt Interval (`idle_irq_interval`)**: The hardware interrupt cadence is determined by the ALSA period size:

```math
\mathrm{idle\_irq\_interval}
=
\mathrm{DIV\_ROUND\_UP}
\left(
\mathrm{CYCLES\_PER\_SECOND}
\times
\frac{\mathrm{events\_per\_period}}{\mathrm{sample\_rate}}
\right)
```

For a 64-frame period at 48 kHz:

```math
\mathrm{idle\_irq\_interval}
=
\mathrm{DIV\_ROUND\_UP}
\left(
8000 \times \frac{64}{48000}
\right)
= 11\text{ packets} \approx 1.375\text{ ms}
```

For a 32-frame period:

```math
\mathrm{idle\_irq\_interval} = 6\text{ packets} = 0.75\text{ ms}
```

*   **Minimum Cadence Limit**: In blocking mode, ALSA enforces a minimum period time of **250 microseconds** (2 cycles). Thus, the absolute minimum hardware interrupt interval under Linux is **2 packets (0.25 ms)**.

#### 2. libffado User-Space DMA Geometry (`IsoHandlerManager.cpp`)
*   **Buffers (Queue size)**: Configured using a maximum buffer limit (`max_nb_buffers_recv` or `max_nb_buffers_xmit`).
*   **Interrupt Cadence (`irq_interval`)**: Dynamically computed from the user's ALSA period size (`packets_per_period`) and target minimum interrupts per period:

```math
\mathrm{irq\_interval}
=
\frac{\mathrm{packets\_per\_period} - 1}{\mathrm{min\_interrupts\_per\_period}}
```

It is capped at `buffers / 2` to ensure at least two interrupts per wrap.
For a 64-frame period (1.33 ms) at 48 kHz, this produces a hardware interrupt interval of **4 to 8 packets (0.5 ms to 1.0 ms)** depending on configuration.

### E. Major Takeaways for ASFW
* **Hardware-Domain Lock**: Both reference drivers avoid absolute host time pacing for wire presentation. They calculate offsets and phases entirely in the physical 1394 cycle-timer/tick domain.
* **Ring Depth Is Not Active Lead**: Reference implementations may keep a
  deeper descriptor or packet ring, but they do not justify committing live
  audio 384 cycles ahead. Their refill/interrupt geometry is period-derived
  and close to the wire. ASFW must likewise separate backing-ring capacity
  from the number of packet slots actively exposed ahead of transmission.
* **Capabilities Are Not Operating Mode**: A Duet capability advertisement or
  FFADO's generic Oxford tolerance does not override device-specific observed
  cadence. Keep capability discovery separate from the selected per-direction
  runtime profile.
* **Do Not Merge the Two Profiles**: Saffire and Duet can share blocking
  start-event DBC utilities, but their SYT validity, receive tolerance, and
  underrun policies remain separate profile decisions.

### F. Linux Has No Direct AudioDriverKit ZTS Equivalent
Linux's general ALSA timestamping API can return a PCM hardware position
together with a system timestamp and, when implemented by a low-level driver,
an `audio_tstamp` derived from a DMA, link, or hardware clock. That is
conceptually similar to observing a sample-clock/system-clock relationship,
but it is a pull-based PCM status interface rather than a periodic driver
publication contract.

The Linux FireWire AMDTP PCM drivers expose
`amdtp_domain_stream_pcm_pointer()` through the ALSA `.pointer` callback. They
do not implement a FireWire-specific `.get_time_info`/`audio_tstamp` that
publishes an OHCI-cycle-to-monotonic-time anchor comparable to
`UpdateCurrentZeroTimestamp(sampleTime, hostTime)`. ALSA therefore falls back
to DMA time based on the PCM hardware pointer.

Consequences for ASFW:

*   Linux validates keeping SYT and wire presentation in the 1394 clock
    domain.
*   Linux does **not** provide evidence for a particular AudioDriverKit
    `zero_timestamp_period`.
*   A 48-frame or 192-frame ZTS period must be justified from the
    AudioDriverKit contract and ASFW's own DMA/ring geometry, not inferred from
    ALSA period wakeups or Linux interrupt intervals.

---

## 10. Reference Implementation: Apple's AppleFWAudio.kext

> [!NOTE]
> The findings in this section are derived from reverse-engineering and decompiling the binary implementation of `AppleFWAudio.kext` (specifically `AM824DCLWrite`) on x86 using IDA Pro. While the architectural flow, control loop structures, and register-to-field mappings represent our findings accurately, minor implementation details, exact type widths, and compiler-optimized invariants may vary from the original source code.

Reverse engineering of Apple's official `AppleFWAudio.kext` reveals a
driver-level SYT/phase control loop executed in the isochronous completion
callback. It is driven by hardware cycle timestamps and the device's recovered
audio-clock domain, but it is software running in the kext.

This is distinct from the device's physical audio-clock PLL and jitter
rejection. On DICE/TCAT-class devices, that hardware domain includes the
on-chip JetPLL clock-recovery system and hardware IEC 61883-6 streaming engine.
The hardware PLL owns converter-clock quality and synchronization; the driver
servo observes cycle/SYT timing and slews packet presentation to remain aligned
with that domain. In short:

```text
JetPLL / device PLL            = physical audio clock and jitter rejection
CheckSYT / fSYTOffset          = driver-side packet timing servo
SYT                            = presentation-time contract between them
```

TCAT's DICE overview describes JetPLL as on-chip clock cleaning and jitter
elimination, while THAT Corporation's later JetPLL IC family likewise
identifies it as audio clock-generator PLL technology:
[TCAT DICE overview](https://datasheet.datasheetarchive.com/originals/crawler/tctechnologies.tc/9e1a175deedf90a82cefb3662de4d53c.pdf);
[THAT JetPLL family](https://thatcorp.com/pll/).

### A. DCL Program Callback Configuration
In `SetupSendBufferForExternalSync`, the driver allocates play buffer groups and chains them into the DCL program.
* A callback command is inserted at the end of each group, with **`DCLSyncOutputCallback`** bound as the service routine.
* The pointer to the `PlayBufferGroup` structure is stored in the command's operand space (`v16[4]`).
* A `DCLSync` command is appended, which captures the hardware arrival cycle timer at completion.

### B. Hardware Clock and SYT Recovery
Inside `DCLSyncOutputCallback`, the driver:
1. **Gates execution** to run only when `groupIndex == 0` (once per DCL program loop) to minimize CPU overhead in the interrupt context.
2. **Extracts the received packet’s CIP Q1 header** from the last packet in the group to get the incoming SYT.
3. **Reconstructs the big-endian SYT** to host-endian:
   ```cpp
   syt = ((Q1 & 0xFF0000) >> 8) | (Q1 >> 24);
   ```
4. **Reads the cycle-only hardware timestamp** from the `DCLSync` command
   status word. As with OHCI descriptor timestamps, this identifies a cycle
   and does not supply a 12-bit intra-cycle offset.
5. If the SYT is valid (`!= 0xFFFF`), it calls `CheckSYT(arrivalCycle, syt)`.

### C. The Driver-Side Slewing Servo (`CheckSYT`)
In `CheckSYT`, the driver compares the cycle index reconstructed from the
cycle-only hardware timestamp with the cycle index encoded in the received
SYT. The SYT also contains a 12-bit cycle offset, but the hardware descriptor
timestamp does not:
```cpp
diff = syt_cycle - arrival_cycle;
```
* **Target Latency**: The target latency is **2 or 3 cycles** (representing the safe hardware/DAC presentation delay).
* **Control Loop**:
  * If the loop is stable and `diff == 2` or `3`, the software servo reports
    `*LOCKED*`.
  * If the latency drifts (`diff != 2, 3, 4`), it sets `fSYTUnstable = 10` and enters a proportional slewing loop.
  * It adjusts **`fSYTOffset`** (the transmit phase offset):
    * If `diff > 3` (latency too high), it decrements `fSYTOffset` (slew earlier).
    * If `diff < 2` (latency too low), it increments `fSYTOffset` (slew later).

### D. Outbound Packet Translation
In `EncodePacket`, the transmitter translates the incoming packet's SYT to the outbound packet's SYT using `fSYTOffset`:
```cpp
output_syt_offset = rx_syt_offset;
output_syt_cycle = (rx_syt_cycle + fSYTOffset) % 16;
```
This ensures the outbound SYT's cycle index is aligned to the incoming stream, locking the transmission rate to the hardware converters' physical clock.

### E. DMA Program Geometry & Interrupt Cadence (48 kHz Case)
When the driver initializes, `AM824DCLWrite::Init` sets the default DMA ring structure parameters:
*   `fNumBufferGroups = 100`
*   `fNumPacketsPerBufferGroup = 8`

For a **48 kHz** sample rate, the selector `SetupSendBuffer` delegates to `SetupSendBufferFor8NkHz`.
*   The average transport rate is:

```math
\mathrm{averageFramesPerCycle} = \frac{48{,}000\text{ Hz}}{8{,}000\text{ cycles/s}} = 6\text{ frames/cycle}
```

In IEC 61883-6 blocking mode this does not mean every packet contains six
    frames. The cadence is represented by DATA packets carrying the SYT
    interval (8 frames at 48 kHz) interspersed with NO-DATA packets. For the
    observed eight-cycle group, six DATA packets and two NO-DATA packets carry
    48 frames total, averaging six frames per cycle.
*   The DCL program constructs a ring of **100 groups**, with **8 packets per group** (each packet programmed with a `DCLTransferPacket` command).
*   Since each group is terminated with a completion callback, the hardware triggers an interrupt once every **8 packets** (exactly **1.0 millisecond** at 8000 packets/s).
*   **Observed Software-Servo Update Frequency**: In the decompiled external-sync path,
    `CheckSYT` appears gated to `groupIndex == 0`. If `groupIndex` spans the
    full 100-group ring in this mode, that implies one correction per ring
    wrap:

```math
\mathrm{Servo\ Update\ Period} = 100\text{ groups} \times 8\text{ packets/group} \times 125\ \mu\text{s} = 100\text{ milliseconds}
```

This is a reverse-engineering observation, not a general requirement.
    Other modes or callback paths may use different gating.

### F. Historical Short-Group Interpretation

> [!CAUTION]
> This subsection records the pre-adoption argument for separating DMA
> interrupt cadence from the ZTS grid. That architectural observation remains
> true, but its recommendation to replace 32-packet groups with shorter groups
> was not adopted. See Section 13 for the current geometry.

AppleFWAudio's observed eight-packet DCL groups establish that its completion
callback ran every 1 ms in the analyzed mode. They do not establish an
AudioDriverKit zero-timestamp period, and they do not make eight packets a
universal pro-audio requirement. DMA completion cadence, HAL I/O period, ZTS
period, preparation lead, and end-to-end latency are related but distinct
quantities.

The research-stage ASFW design coupled a 32-packet interrupt group to the
192-frame ZTS period. ZTS correctness does not require this coupling. At
48 kHz:

$$
\frac{192\text{ frames}}{6\text{ average frames/cycle}}
= 32\text{ FireWire cycles per ZTS period}
$$

This means ZTS anchors are separated by 32 bus cycles on average. In isolation,
that arithmetic does not require the driver to wait 32 packets before
servicing DMA.

The historical proposed architecture separated the two cadences:

1.  **Interrupt frequently** using a short DMA completion group chosen for
    refill latency and DriverKit scheduling tolerance.
2.  **Process every drained bus cycle**, including DATA and NO-DATA packets,
    while advancing the decoded sample cursor only by the number of data
    blocks actually carried.
3.  **Publish ZTS only when that cursor crosses the next 192-frame grid
    boundary.** Most interrupts therefore publish no ZTS.
4.  **Back-project the crossed grid point** from the relevant packet's
    cycle-granular OHCI timestamp rather than assigning the interrupt entry
    time directly to the grid frame.
5.  If a delayed/coalesced drain crosses more than one grid boundary, publish
    each crossed 192-frame anchor in order.

For example, a six-packet interrupt interval is 0.75 ms at 8000 cycles/s.
Because 32 bus cycles are not an integer multiple of six, ZTS publication
would naturally fall on a repeating pattern of the fifth or sixth interrupt.
That is expected; cursor crossing, rather than interrupt count, determines
publication. An eight-packet interval happens to produce exactly four
interrupts per 192-frame ZTS period in the nominal 48 kHz cadence, but this
arithmetic convenience is not a correctness requirement.

The local reverse-engineering notes contain more than one legacy geometry:
the analyzed AppleFWAudio path uses eight-packet groups, while the Saffire
receive/transmit DCL research records 12-packet groups at 48 kHz. Neither
establishes a six-packet Apple requirement. The common architectural lesson is
that these drivers service DMA more frequently than ASFW's former 32-packet
batch while publishing host-clock anchors on an independent timeline.

The adopted design deliberately chose the other valid trade: 32 packets
contain eight whole D/D/D/N cadence blocks and therefore advance a nominal
192 frames. This makes the interrupt-group and ZTS-grid boundaries coincide
without making their equality a universal protocol requirement. The
grid-crossing loop remains mandatory for drift, relock, and coalesced drains.

---

## 11. Historical Research Verdict

> [!CAUTION]
> This verdict predates the adopted FW-53+ geometry in Section 13. Its findings
> about host/wire clock separation and OSAction timestamp preservation remain
> valid. Its rejection of 32-packet groups and its proposed 8–16 packet
> preparation window are superseded.
>
> **Its "fill-in-place ownership" finding has been CORRECTED — see Section 8,
> Defect B.** That framing (deep over-exposure of zeroed slots shipping silence,
> `E ≫ W`) was backwards for the current timeline path. Direct measurement shows
> the real defect is *under-exposure* (`W > E`): the exposure frontier lags the
> CoreAudio write window. Over-exposing is harmless because `T < W`. Treat the
> "fill-in-place" language below as historical and read Section 8 for the truth.

The timing geometry under investigation at that point was not a balanced
tradeoff or a conservative starting point. Its core ownership and clock-domain
couplings were architecturally wrong:

1.  **The short-group proposal treated 32 packets as too coarse.** That
    conclusion was later superseded by the whole-cadence, fixed-advance
    geometry adopted in Section 13.
2.  **The old 384-packet "fill-in-place bridge" framing was wrong — see
    Section 8, Defect B (corrected).** The deep mutable window was blamed for
    shipping silence via over-exposure (`E ≫ W`); measurement later showed that
    over-exposure is harmless (`T < W`) and the real dropout is *under-exposure*
    (`W > E`). The deep window did complicate frame-to-packet mapping, but it was
    not the silence cause. Retained here only as a record of the superseded
    hypothesis.
3.  **Deep packet storage was confused with latency margin.** A ring can be
    deep without committing its contents 48 ms early. Capacity and active
    lead must be independent.
4.  **`OSAction` delivery latency was confused with timestamp error.** The
    anchor value is captured before the cross-process notification. The
    notification path must be bounded and lossless, but removing it is not a
    same-task callback optimization in the present driver topology.
5.  **Host and wire clock responsibilities must remain separate.** ZTS maps
    the device sample timeline to host time. RX SYT/cycle timing governs TX
    presentation. Neither should be used as a substitute for the other.

The research-stage replacement direction was:

*   consider interrupting and refilling on a short packet cadence;
*   keep the HAL ZTS grid independent and publish only on cursor
    crossings;
*   use the packet's OHCI cycle stamp to project each crossed ZTS boundary;
*   reduce active TX preparation to a small bounded near-wire window;
*   keep startup NO-DATA prefill and deep ring capacity as separate mechanisms,
    with packetizer/profile-owned CIP and DBC semantics;
*   preserve captured ZTS anchors across the required cross-process
    notification path.

The current implementation retained the clock-domain and ownership
corrections while adopting a six-packet interrupt group. Because six is not a
complete four-packet cadence multiple, a group advances by either 32 or 40
frames depending on cadence phase.

---

## 12. Implementation Chronology

The geometry changed in stages. Reading these states as simultaneous design
requirements is the source of several apparent contradictions:

1.  **Pre-FW-46/FW-53 broken state:** experiments mixed an eight-packet DMA
    group, a 48-frame ZTS idea, 512-frame rings, and a large mutable TX
    fill-in-place bridge. These values did not form one valid ADK geometry.
2.  **FW-46 through FW-52 intermediate state:** eight-packet DMA groups were
    retained while the HAL IO size, frame ring, and ZTS period were made
    512 frames. ZTS grid points had to be reconstructed inside a sequence of
    48-frame nominal group advances.
3.  **Current adopted state:** the transport uses six-packet timing groups,
    a phase-dependent 32/40-frame group advance, and a 1536-frame ADK ZTS
    period equal to the 1536-frame HAL ring. Packet-ring ownership and startup
    prefill remain separate from the HAL frame-ring geometry.

Reference-driver sections describe evidence, not configuration inheritance.
Apple's eight-packet groups, Linux's period-derived queues, and FFADO's
dynamic interrupt choices remain useful comparisons but do not override the
current ASFW constants below.

---

## 13. Current Adopted Geometry

This section is the authority for current ASFW timing geometry. The constants
live in `ASFWDriver/Shared/Isoch/AudioTimingGeometry.hpp`.

### A. Authoritative Values

| Quantity | Current value | Meaning |
|---|---:|---|
| Sample rate | 48,000 Hz | Current supported timing profile |
| Blocking cadence block | 4 packets | D/D/D/N, phase may rotate |
| Frames per DATA packet | 8 | AMDTP SYT interval at 48 kHz |
| Frames per cadence block | 24 | Three DATA packets |
| RX/TX timing group | 6 packets | 0.75 ms interrupt target |
| Group advance | 32 or 40 frames | Depends on D/D/D/N starting phase; 36 average |
| ADK ZTS period | 1536 frames | Equal to the mapped ADK stream-ring length |
| Maximum HAL IO transfer | 512 frames | Client-transfer upper bound |
| HAL frame ring | 1536 frames | One ZTS period, three max IO transfers |
| Frame alignment | 32 frames | Shared alignment contract |
| IR descriptor ring | 504 packets | Packet-domain receive storage |
| TX hardware ring | 48 packets | OHCI-owned transmit program |
| TX preparation slack | 96 packets | Sixteen groups / 12 ms of producer scheduling tolerance |
| TX coverage lead | 144 packets | Hardware ring plus scheduling slack; refill-safety sub-budget |
| TX frame-exposure window | 192 packets | Packetized cushion for `WriteEnd + kTxExposureLeadFrames` |
| TX preparation lead | 336 packets | Coverage lead plus frame-exposure window |
| TX shared packet ring | 384 packets | Preparation lead plus one hardware-ring reuse guard |
| Input safety floor | 104 frames | Maximum 40-frame group plus 64-frame jitter floor |

The HAL has one frame-domain sample ring per direction. Packet-domain IR
descriptor buffers and TX payload/metadata slots also exist, but they do not
define HAL frame-wrap timing.

### B. Load-Bearing Invariants

The design requires the following relationships:

```text
rxDescriptorPackets % cadenceBlockPackets == 0
rxDescriptorPackets % timingGroupPackets == 0
frameRing == ztsPeriod
frameRing % maxIO == 0
frameRing % frameAlignment == 0
txSharedSlots % timingGroupPackets == 0
txSharedSlots % cadenceBlockPackets == 0
txHardwarePackets % timingGroupPackets == 0
```

For the adopted values:

```text
504 / 4    = 126 complete cadence blocks
504 / 6    = 84 interrupt groups
1536       = 1536-frame ZTS period
1536 / 512 = 3 maximum IO transfers
1536 / 32  = 48 alignment quanta
384 / 6    = 64 shared-ring groups
384 / 4    = 96 complete cadence blocks
48 / 6     = 8 hardware-ring groups
```

The frame ring equals the declared ZTS period because AudioDriverKit wraps the
mapped stream buffer on that contract. The six-packet DMA cadence remains
independent from the frame-domain ZTS grid.

### C. ZTS Publication

Host anchors are RX-interrupt-derived ADK ZTS-grid anchors. A six-packet group
advances by 32 or 40 decoded frames, so many receive drains occur between
1536-frame anchors. The receive path advances the absolute frame cursor by the
decoded data-block count. It publishes only when a real DATA packet begins
exactly on the 1536-frame grid, using that packet's hardware-derived receive
host time unchanged. It does not synthesize a boundary timestamp by adding
nominal frame durations to another packet observation.

Each packet's eight-byte receive prefix is decoded for its own OHCI timestamp
and isochronous header. The single cycle-timer/host-time pair captured at drain
entry is only the expansion reference. This per-packet timestamp correlation
is essential during coalesced drains: assigning one drain timestamp to all
packets would make older packets stale.

**Verification priority: high after the first stable duplex run.** Exercise
multi-group and wraparound drains and confirm that every published anchor uses
the DATA packet whose first decoded frame is the grid point.

### D. NO-DATA, DBC, and Startup Prefill

NO-DATA emits a CIP-only packet with `SYT = 0xFFFF` and consumes no PCM frames.
DBC follows the device profile's observed blocking cadence; the PCM cursor
must not advance merely because a NO-DATA packet occupied a bus cycle. For
Saffire, the gap carries the cadence-appropriate already-advanced DBC and the
following DATA packet repeats it.

Before IT RUN, the packetizer seeds the entire 384-slot shared TX ring with
committed NO-DATA packets. This is 48 ms of valid packet-domain backing. It is
deliberately larger than the 336-packet total preparation lead because action
delivery and the startup handoff can be delayed. `TxPreparationReady` uses a
dedicated DriverKit dispatch queue, so the synchronous `StartIO` wait for the
first hardware ZTS does not starve the producer.

Once the action runs, the producer has two targets. It must always reach the
refill-coverage target (`completion + 144`) so the core never sees an
uncommitted slot. It may continue up to the total preparation limit
(`completion + 336`) until `AmdtpPacketTimeline::ExposedFrameEnd()` covers the
latest CoreAudio `WriteEnd + kTxExposureLeadFrames`. This separates the
packet-domain underrun budget from the audio-frame under-exposure budget.

The DMA layer must not invent fallback CIP state by copying an arbitrary
previous Q0. Forced fallback is a packetizer/profile transition and must
reconcile DBC and cadence before normal DATA resumes.

### E. Safety Offsets

The implemented input safety rule currently raises the selected device-profile
value to at least:

```text
maximumTimingGroupFrames + jitterMargin = 40 + 64 = 104 frames
```

For general client IO sizes, the required runtime rule is:

```text
inputSafetyFrames =
    max(profileInputSafety,
        outputSafetyFrames + actualClientIOFrames + jitterMargin,
        timingGroupFrames + jitterMargin)
```

Thus 104 frames is a floor, not a universally sufficient value for a
512-frame client transfer. Runtime/profile logic must raise it when the active
client IO geometry requires more headroom.

This rule must be enforced when the device profile is activated and whenever
the HAL client IO size changes. The current 48-frame output safety and
512-frame maximum client transfer raise the registered input safety to 624
frames. A runtime assertion or focused test must reject any configuration in
which the registered input safety is below the computed requirement;
documentation alone is not protection against a stale hardcoded floor.

### F. Current Residual Work

1.  Validate per-packet timestamp/grid correlation during coalesced
    multi-group drains and cycle-timer wraparound.
2.  Enforce and test the dynamic input-safety rule at profile activation and
    HAL client-buffer-size changes; do not treat the 104-frame floor as the
    final value.
3.  Remove any DMA-owned generic NO-DATA synthesis that copies a previous CIP
    header. Prefill and underrun packets must come from the packetizer/profile.
4.  Verify the prefill-to-live handoff on both Saffire Pro 24 and Apogee Duet,
    including DBC repetition and valid-SYT DATA requirements.
