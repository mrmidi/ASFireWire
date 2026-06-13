#!/usr/bin/env python3
"""
zts_sim.py

Executable verification of the ZTS (zero-timestamp) clock model emitted by the
[Zts] telemetry subsystem (IsochReceiveContext -> WatchdogCoordinator drain).
It also contains an executable port of the Saffire.kext RX cadence ring and TX
phase servo decompiled from ReadFirewireBuffers, FillFirewireBuffers, and
adjustOutputPhase, then replays [TxSyt] records against those exact branches.

Hand analysis of the clock relationships is error prone, so this turns every
claim into a check that runs against the real log and either passes with numbers
or fails loudly:

  * back-correction        rawHost == drainHost - FWticks->machticks(age)   (exact integer model)
  * cycle-delta identity   age     == drainCycle_abs - rxCycle_abs          (OHCI cycle-timer domain)
  * host<->bus coherence   drainHost(mach) vs drainCycle(FW), same-instant   -> slope = MACH/FW
  * grid rate              host(mach)      vs sampleFrame                    -> slope = MACH/48000
  * device packet cadence  rxCycle(FW)     vs sampleFrame                    -> slope = FW/48000
  * grid step              d(frame) == d(count) * ZTS_PERIOD ; frame % P == 0
  * interpolation bound     0 <= host - rawHost <= framesDecoded/48000
  * RX SYT recovery         extend(packet cycle, SYT) -> recovered phase/lead
  * TX servo replay         cadence-grid correction, deadband, governor, SYT
  * sparse TX phase chain   cadence read-index commits -> carried output phase
  * TX DATA-slot continuity emitted commits match the absolute N,D,D,D cadence
  * runtime lock health     distinguishes exact execution from useful sync
  * monotonicity / cadence stability / seed sanity

`selftest` builds synthetic records from a forward model of the driver math and
proves each check has teeth: clean data passes, and an injected fault (clock
skew, broken back-correction, wrong grid step) fails the matching check.

Usage:
    python3 zts_sim.py verify [path/to/zts_telemetry.log]
    python3 zts_sim.py firebug [path/to/firebug.txt ...]
    python3 zts_sim.py selftest
"""

import re
import sys
from collections import Counter
from dataclasses import dataclass
from typing import Callable, Dict, Iterable, List, Optional, Tuple

# ---------------------------------------------------------------------------
# Constants (single source of truth: ASFWDriver/Common/TimingUtils.hpp,
# TxTimingModel.hpp, and the Apple-Silicon mach timebase 24 MHz = 125/3 ns).
# ---------------------------------------------------------------------------
FW_HZ = 24_576_000          # FireWire bus / OHCI cycle-timer rate (kTicksPerSecond)
TICKS_PER_CYCLE = 3072
CYCLES_PER_SEC = 8000
SECONDS_FIELD_WRAP = 128     # cycle-timer seconds field is 7 bits
MACH_HZ = 24_000_000        # Apple-Silicon mach_absolute_time rate
SAMPLE_RATE = 48_000
ZTS_PERIOD = 192            # kHalZeroTimestampPeriodFrames
NANOS_PER_SEC = 1_000_000_000

CYCLE_TIMER_SECONDS_SHIFT = 25
CYCLE_TIMER_CYCLES_SHIFT = 12
CYCLE_TIMER_OFFSET_MASK = 0xFFF
CYCLE_TIMER_CYCLES_MASK = 0x1FFF

FULL_DOMAIN_TICKS = SECONDS_FIELD_WRAP * CYCLES_PER_SEC * TICKS_PER_CYCLE

# Saffire.kext SYT clock-recovery constants, decompiled from:
#   StartStreams          0x1132c
#   ReadFirewireBuffers   0xcf24
#   FillFirewireBuffers   0xe778
#   adjustOutputPhase     0xc9c2
OFFSET_DOMAIN_TICKS = 8 * FW_HZ
SYT_FIELD_DOMAIN_TICKS = 16 * TICKS_PER_CYCLE
SYT_INTERVAL_FRAMES = 8
SYT_PACKET_STEP_TICKS = 4096
RX_CADENCE_ENTRIES = 512
RX_CADENCE_READ_DELAY = RX_CADENCE_ENTRIES // 2
RX_CADENCE_WARMUP = RX_CADENCE_ENTRIES + 1
TX_INITIAL_LEAD_TICKS = TICKS_PER_CYCLE
TX_PHASE_DEADBAND_FRAMES = 409
TX_ACCEPT_LEAD_TICKS = 7620
TX_ESCALATE_LEAD_TICKS = 12287
TX_XMIT_TRANSFER_DELAY_TICKS = 12800

TX_FLAG_SEEDED = 0x1
TX_FLAG_FORCE_ADJUST = 0x2
TX_FLAG_RESEEDED = 0x4
TX_FLAG_COMMITTED = 0x8

TX_HEALTH_NOT_SEEDED = 0
TX_HEALTH_ACCEPTED = 1
TX_HEALTH_TIGHT = 2
TX_HEALTH_LATE = 3
TX_HEALTH_GATE = 4
TX_HEALTH_ESCALATE = 5


# ---------------------------------------------------------------------------
# Cycle-timer arithmetic (mirrors IsochRxTiming.hpp / TimingUtils.hpp)
# ---------------------------------------------------------------------------
def decode_cycle_timer(raw: int) -> Tuple[int, int, int]:
    sec = (raw >> CYCLE_TIMER_SECONDS_SHIFT) & 0x7F
    cyc = (raw >> CYCLE_TIMER_CYCLES_SHIFT) & CYCLE_TIMER_CYCLES_MASK
    off = raw & CYCLE_TIMER_OFFSET_MASK
    return sec, cyc, off


def cycle_timer_abs_ticks(raw: int, sec_epoch: int = 0) -> int:
    """Absolute ticks, with `sec_epoch` adding whole 128 s wraps for unwrapping."""
    sec, cyc, off = decode_cycle_timer(raw)
    return ((sec_epoch + sec) * CYCLES_PER_SEC + cyc) * TICKS_PER_CYCLE + off


def signed_domain_diff(a: int, b: int) -> int:
    """Shortest signed (a - b) across the 128 s cycle-timer domain."""
    d = (a - b) % FULL_DOMAIN_TICKS
    if d > FULL_DOMAIN_TICKS // 2:
        d -= FULL_DOMAIN_TICKS
    return d


def wrapped_diff(a: int, b: int, domain: int) -> int:
    """Shortest signed (a - b) in a wrapping domain."""
    d = (a - b) % domain
    if d > domain // 2:
        d -= domain
    return d


def ext_offset_diff(a: int, b: int) -> int:
    return wrapped_diff(a, b, OFFSET_DOMAIN_TICKS)


def normalize_offset_domain(ticks: int) -> int:
    return ticks % OFFSET_DOMAIN_TICKS


def c_div(a: int, b: int) -> int:
    """C/C++ signed integer division (truncation toward zero)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q


def syt_to_field_ticks(syt: int) -> int:
    return ((syt >> 12) & 0xF) * TICKS_PER_CYCLE + (syt & 0xFFF)


def syt_diff_in_offsets(new_syt: int, old_syt: int) -> int:
    return wrapped_diff(
        syt_to_field_ticks(new_syt),
        syt_to_field_ticks(old_syt),
        SYT_FIELD_DOMAIN_TICKS,
    )


def encode_syt(phase_ticks: int) -> int:
    domain_tick = phase_ticks % SYT_FIELD_DOMAIN_TICKS
    cycle = domain_tick // TICKS_PER_CYCLE
    offset = domain_tick % TICKS_PER_CYCLE
    return ((cycle & 0xF) << 12) | (offset & 0xFFF)


def extend_syt_from_cycle_timer(cycle_timer: int, syt: int) -> int:
    """Port of Saffire extendTstamp (0x1456b), returned in the 8 s domain."""
    seconds, base_cycle, _ = decode_cycle_timer(cycle_timer)
    syt_cycle = (syt >> 12) & 0xF
    cycle = (base_cycle & ~0xF) | syt_cycle
    if cycle < base_cycle:
        cycle += 16
        if cycle >= CYCLES_PER_SEC:
            cycle -= CYCLES_PER_SEC
            seconds = (seconds + 1) & 0x7F
    return normalize_offset_domain(
        (seconds * CYCLES_PER_SEC + cycle) * TICKS_PER_CYCLE +
        (syt & 0xFFF)
    )


def fw_ticks_to_mach(age: int) -> int:
    """Driver's two-step integer back-correction: FW ticks -> ns -> mach ticks.

    FireWireTicksToNanos: ns = floor(|age| * 1e9 / FW_HZ)
    nanosToHostTicks:     mach = floor(ns * MACH_HZ / 1e9)
    Sign preserved (the IR path branches on age sign).
    """
    mag = abs(age)
    nanos = (mag * NANOS_PER_SEC) // FW_HZ
    mach = (nanos * MACH_HZ) // NANOS_PER_SEC
    return -mach if age < 0 else mach


# ---------------------------------------------------------------------------
# IDA-derived Saffire TX/RX synchronization model
# ---------------------------------------------------------------------------
@dataclass
class RxCadenceSnapshot:
    write_index: int
    valid_updates: int
    rolling_cadence_ticks: int
    recovered_phase_ticks: int
    established: bool


class SaffireRxSytSync:
    """Exact 512-entry RX SYT cadence ring from ReadFirewireBuffers."""

    def __init__(self) -> None:
        self.reset()

    def reset(self) -> None:
        self.entries = [0] * RX_CADENCE_ENTRIES
        self.write_index = RX_CADENCE_READ_DELAY
        self.aging_index = 0
        self.valid_updates = 0
        self.rolling_cadence_ticks = 0
        self.previous_syt = 0xFFFF
        self.recovered_phase_ticks = 0
        self.established = False

    def observe(self, syt: int, packet_cycle_timer: int) -> bool:
        if syt == 0xFFFF:
            return False

        delta = SYT_PACKET_STEP_TICKS
        if self.previous_syt != 0xFFFF:
            delta = syt_diff_in_offsets(syt, self.previous_syt)
        if delta <= 0 or delta > 0xFFFF:
            self.previous_syt = 0xFFFF
            return False

        outgoing = self.entries[self.aging_index]
        self.entries[self.write_index] = delta
        self.write_index = (self.write_index + 1) & (RX_CADENCE_ENTRIES - 1)
        self.aging_index = (self.aging_index + 1) & (RX_CADENCE_ENTRIES - 1)
        self.rolling_cadence_ticks += delta - outgoing
        self.previous_syt = syt
        self.valid_updates += 1
        self.established = self.valid_updates >= RX_CADENCE_WARMUP
        self.recovered_phase_ticks = extend_syt_from_cycle_timer(
            packet_cycle_timer, syt)
        return True

    def snapshot(self) -> RxCadenceSnapshot:
        return RxCadenceSnapshot(
            write_index=self.write_index,
            valid_updates=self.valid_updates,
            rolling_cadence_ticks=self.rolling_cadence_ticks,
            recovered_phase_ticks=self.recovered_phase_ticks,
            established=self.established,
        )


@dataclass
class TxServoReplay:
    rx_phase_delay_free: int
    phase_error: int
    frame_error: int
    correction_ticks: int
    force_adjust: bool
    phase_post: int
    lead_ticks: int
    wire_lead_ticks: int
    health: int
    syt: int
    gated: bool


def classify_tx_lead(lead_ticks: int) -> int:
    if lead_ticks < 0:
        return TX_HEALTH_LATE
    if lead_ticks <= TX_INITIAL_LEAD_TICKS - 1:
        return TX_HEALTH_TIGHT
    if lead_ticks < TX_ACCEPT_LEAD_TICKS:
        return TX_HEALTH_ACCEPTED
    if lead_ticks < TX_ESCALATE_LEAD_TICKS:
        return TX_HEALTH_GATE
    return TX_HEALTH_ESCALATE


def replay_tx_servo(
    *,
    anchor: int,
    phase_pre: int,
    recovered_phase: int,
    rolling_cadence: int,
    seeded_this_call: bool,
    syt_interval_frames: int = SYT_INTERVAL_FRAMES,
    phase_deadband: int = TX_PHASE_DEADBAND_FRAMES,
    xmit_delay: int = TX_XMIT_TRANSFER_DELAY_TICKS,
) -> TxServoReplay:
    """Port of adjustOutputPhase plus FillFirewireBuffers' lead governor."""
    rx_free = normalize_offset_domain(recovered_phase - xmit_delay)
    phase_error = ext_offset_diff(phase_pre, rx_free)
    cadence_scale = syt_interval_frames << 8

    remainder = 0
    complement = 0
    if cadence_scale != 0 and rolling_cadence != 0:
        if phase_error >= 0:
            remainder = (phase_error * cadence_scale) % rolling_cadence
            complement = rolling_cadence - remainder
        else:
            remainder = ((-phase_error) * cadence_scale) % rolling_cadence
            complement = remainder

    correction = 0
    frame_error = 0
    if remainder != 0:
        correction = complement // cadence_scale
        signed_remainder = remainder
        if remainder > rolling_cadence // 2:
            signed_remainder -= rolling_cadence
        frame_error = c_div(signed_remainder, cadence_scale)

    # A seed/reset arms forceAdjust. Otherwise the Saffire deadband holds the
    # carried phase whenever the fractional-frame error is within 409 frames.
    force_adjust = seeded_this_call or abs(frame_error) > phase_deadband
    phase_post = phase_pre
    if force_adjust:
        phase_post = normalize_offset_domain(anchor + correction)

    lead = ext_offset_diff(phase_post, anchor)
    wire_lead = lead + xmit_delay
    health = classify_tx_lead(lead)
    gated = health in (TX_HEALTH_GATE, TX_HEALTH_ESCALATE)
    syt = 0xFFFF if gated else encode_syt(phase_post + xmit_delay)
    return TxServoReplay(
        rx_phase_delay_free=rx_free,
        phase_error=phase_error,
        frame_error=frame_error,
        correction_ticks=correction,
        force_adjust=force_adjust,
        phase_post=phase_post,
        lead_ticks=lead,
        wire_lead_ticks=wire_lead,
        health=health,
        syt=syt,
        gated=gated,
    )


# ---------------------------------------------------------------------------
# Record model + parser
# ---------------------------------------------------------------------------
@dataclass
class ZtsRecord:
    kind: str           # "SEED" | "UPD"
    count: int
    frame: int
    host: int
    rawHost: int
    drainHost: int
    drainCycle: int
    rxCycle: int
    age: int
    rawRxTs: int
    syt: int
    sytLead: int
    rollCad: int
    recPhase: int
    desc: int
    dec: int
    # filled during unwrap pass:
    drainCycleAbs: int = 0
    rxCycleAbs: int = 0


@dataclass
class TxSytRecord:
    pkt: int
    flags: int
    health: int
    anchor: int
    phasePre: int
    phasePost: int
    recPhase: int
    rxFree: int
    pErr: int
    fErr: int
    corr: int
    lead: int
    wire: int
    rollCad: int
    pend: int
    ridx: int
    syt: int


@dataclass
class FireBugPacket:
    seconds: int
    cycle: int
    offset: int
    channel: int
    tag: int
    sy: int
    size: int
    actual_size: int
    speed: str
    q0: int
    q1: int
    line_number: int

    @property
    def sid(self) -> int:
        return (self.q0 >> 24) & 0x3F

    @property
    def dbs(self) -> int:
        return (self.q0 >> 16) & 0xFF

    @property
    def dbc(self) -> int:
        return self.q0 & 0xFF

    @property
    def eoh(self) -> int:
        return (self.q1 >> 30) & 0x3

    @property
    def fmt(self) -> int:
        return (self.q1 >> 24) & 0x3F

    @property
    def fdf(self) -> int:
        return (self.q1 >> 16) & 0xFF

    @property
    def syt(self) -> int:
        return self.q1 & 0xFFFF

    @property
    def is_no_data(self) -> bool:
        return self.size == 8

    @property
    def frames(self) -> Optional[int]:
        if self.is_no_data:
            return 0
        if self.dbs == 0 or self.size < 8:
            return None
        payload_bytes = self.size - 8
        bytes_per_frame = self.dbs * 4
        if payload_bytes % bytes_per_frame != 0:
            return None
        return payload_bytes // bytes_per_frame

    @property
    def cycle_time(self) -> int:
        return self.seconds * CYCLES_PER_SEC + self.cycle


_KV = re.compile(r"(\w+)=(0x[0-9a-fA-F]+|-?\d+)")
_KIND_ZTS = re.compile(r"\[Zts\]\s+(SEED|UPD)\b")
_KIND_TXSYT = re.compile(r"\[TxSyt\]")
_FIREBUG_HEADER = re.compile(
    r"^\s*(\d+):(\d+):(\d+)\s+"
    r"Isoch channel (\d+), tag (\d+), sy (\d+), size (\d+)"
    r"(?: \[actual (\d+)\])?\s+(\S+)"
)
_FIREBUG_CIP = re.compile(
    r"^\s+0000\s+([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})(?:\s|$)"
)

_ZTS_INT_FIELDS = ("count", "frame", "host", "rawHost", "drainHost", "drainCycle",
                   "rxCycle", "age", "rawRxTs", "syt", "sytLead", "rollCad",
                   "recPhase", "desc", "dec")

_TXSYT_INT_FIELDS = ("pkt", "flags", "health", "anchor", "phasePre", "phasePost",
                     "recPhase", "rxFree", "pErr", "fErr", "corr", "lead",
                     "wire", "rollCad", "pend", "ridx", "syt")


def parse_zts_line(line: str) -> Optional[ZtsRecord]:
    km = _KIND_ZTS.search(line)
    if not km:
        return None
    kv: Dict[str, int] = {}
    for k, v in _KV.findall(line):
        if k in _ZTS_INT_FIELDS and k not in kv:
            kv[k] = int(v, 16) if v.lower().startswith("0x") else int(v)
    # Trailing fields (period, rate) are often truncated by os_log; tolerate that.
    required = ("count", "frame", "host", "rawHost", "drainHost",
                "drainCycle", "rxCycle", "age")
    if any(r not in kv for r in required):
        return None
    return ZtsRecord(
        kind=km.group(1),
        count=kv["count"], frame=kv["frame"], host=kv["host"],
        rawHost=kv["rawHost"], drainHost=kv["drainHost"],
        drainCycle=kv["drainCycle"], rxCycle=kv["rxCycle"], age=kv["age"],
        rawRxTs=kv.get("rawRxTs", 0), syt=kv.get("syt", 0),
        sytLead=kv.get("sytLead", 0), rollCad=kv.get("rollCad", 0),
        recPhase=kv.get("recPhase", 0), desc=kv.get("desc", 0),
        dec=kv.get("dec", 8),
    )


def parse_txsyt_line(line: str) -> Optional[TxSytRecord]:
    if not _KIND_TXSYT.search(line):
        return None
    kv: Dict[str, int] = {}
    for k, v in _KV.findall(line):
        if k in _TXSYT_INT_FIELDS and k not in kv:
            kv[k] = int(v, 16) if v.lower().startswith("0x") else int(v)
    required = ("pkt", "flags", "health", "anchor", "phasePre", "phasePost", "syt")
    if any(r not in kv for r in required):
        return None
    return TxSytRecord(
        pkt=kv["pkt"], flags=kv["flags"], health=kv["health"],
        anchor=kv["anchor"], phasePre=kv["phasePre"], phasePost=kv["phasePost"],
        recPhase=kv.get("recPhase", 0), rxFree=kv.get("rxFree", 0),
        pErr=kv.get("pErr", 0), fErr=kv.get("fErr", 0),
        corr=kv.get("corr", 0), lead=kv.get("lead", 0),
        wire=kv.get("wire", 0), rollCad=kv.get("rollCad", 0),
        pend=kv.get("pend", 0), ridx=kv.get("ridx", 0),
        syt=kv["syt"]
    )


def parse_log(path: str) -> Tuple[List[ZtsRecord], List[TxSytRecord]]:
    zts_recs: List[ZtsRecord] = []
    txsyt_recs: List[TxSytRecord] = []
    with open(path, "r", errors="replace") as f:
        for line in f:
            z = parse_zts_line(line)
            if z:
                zts_recs.append(z)
            else:
                t = parse_txsyt_line(line)
                if t:
                    txsyt_recs.append(t)
    return unwrap_cycle_seconds(zts_recs), txsyt_recs


def parse_firebug_lines(lines: Iterable[str]) -> List[FireBugPacket]:
    packets: List[FireBugPacket] = []
    pending: Optional[Tuple[int, ...]] = None
    pending_speed = ""
    pending_line = 0

    for line_number, line in enumerate(lines, 1):
        header = _FIREBUG_HEADER.match(line)
        if header:
            values = header.groups()
            actual_size = (
                int(values[7]) if values[7] is not None else int(values[6])
            )
            pending = (
                int(values[0]), int(values[1]), int(values[2]),
                int(values[3]), int(values[4]), int(values[5]),
                int(values[6]), actual_size,
            )
            pending_speed = values[8]
            pending_line = line_number
            continue

        if pending is None:
            continue
        cip = _FIREBUG_CIP.match(line)
        if not cip:
            continue

        packets.append(FireBugPacket(
            seconds=pending[0],
            cycle=pending[1],
            offset=pending[2],
            channel=pending[3],
            tag=pending[4],
            sy=pending[5],
            size=pending[6],
            actual_size=pending[7],
            speed=pending_speed,
            q0=int(cip.group(1), 16),
            q1=int(cip.group(2), 16),
            line_number=pending_line,
        ))
        pending = None

    return packets


def parse_firebug(path: str) -> List[FireBugPacket]:
    with open(path, "r", errors="replace") as stream:
        return parse_firebug_lines(stream)


def split_firebug_segments(
    packets: List[FireBugPacket],
    max_gap_cycles: int = 16,
) -> List[List[FireBugPacket]]:
    if not packets:
        return []

    domain_cycles = SECONDS_FIELD_WRAP * CYCLES_PER_SEC
    segments: List[List[FireBugPacket]] = []
    current = [packets[0]]
    previous = packets[0]

    for packet in packets[1:]:
        delta = (packet.cycle_time - previous.cycle_time) % domain_cycles
        if delta > domain_cycles // 2:
            delta -= domain_cycles
        if delta < 0 or delta > max_gap_cycles:
            segments.append(current)
            current = []
        current.append(packet)
        previous = packet

    segments.append(current)
    return segments


def unwrap_cycle_seconds(recs: List[ZtsRecord]) -> List[ZtsRecord]:
    """Unwrap the 7-bit cycle-timer seconds field across the record sequence so
    drainCycleAbs / rxCycleAbs are monotonic for regression."""
    epoch = 0
    prev_sec = None
    for r in recs:
        sec = (r.drainCycle >> CYCLE_TIMER_SECONDS_SHIFT) & 0x7F
        if prev_sec is not None and sec < prev_sec - 1:
            epoch += SECONDS_FIELD_WRAP
        prev_sec = sec
        r.drainCycleAbs = cycle_timer_abs_ticks(r.drainCycle, epoch)
        r.rxCycleAbs = cycle_timer_abs_ticks(r.rxCycle, epoch)
    return recs


# ---------------------------------------------------------------------------
# Pure-stdlib least squares (centered on first sample for float precision)
# ---------------------------------------------------------------------------
@dataclass
class Fit:
    slope: float
    intercept_resid: float
    r2: float
    max_abs_resid: float
    n: int


def linfit(xs: List[int], ys: List[int]) -> Fit:
    n = len(xs)
    x0, y0 = xs[0], ys[0]
    xc = [float(x - x0) for x in xs]
    yc = [float(y - y0) for y in ys]
    sx = sum(xc); sy = sum(yc)
    sxx = sum(x * x for x in xc); sxy = sum(x * y for x, y in zip(xc, yc))
    denom = n * sxx - sx * sx
    if denom == 0:
        return Fit(0.0, 0.0, 0.0, 0.0, n)
    slope = (n * sxy - sx * sy) / denom
    intercept = (sy - slope * sx) / n
    resids = [y - (slope * x + intercept) for x, y in zip(xc, yc)]
    max_resid = max(abs(r) for r in resids)
    mean_y = sy / n
    ss_tot = sum((y - mean_y) ** 2 for y in yc)
    ss_res = sum(r * r for r in resids)
    r2 = 1.0 - (ss_res / ss_tot) if ss_tot > 0 else 1.0
    return Fit(slope, intercept, r2, max_resid, n)


def ppm(measured: float, ideal: float) -> float:
    return (measured / ideal - 1.0) * 1e6


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
@dataclass
class CheckResult:
    name: str
    passed: bool
    detail: str


def _counter_text(values: Counter) -> str:
    if not values:
        return "none"

    def sort_key(item: Tuple[object, int]) -> Tuple[int, object]:
        value = item[0]
        if isinstance(value, int):
            return (0, value)
        return (1, str(value))

    return ", ".join(
        f"{value}:{count}"
        for value, count in sorted(values.items(), key=sort_key)
    )


def _firebug_data_fdf_by_channel(
    packets: List[FireBugPacket],
) -> Dict[int, int]:
    result: Dict[int, int] = {}
    channels = sorted(set(packet.channel for packet in packets))
    for channel in channels:
        counts = Counter(
            packet.fdf for packet in packets
            if packet.channel == channel and not packet.is_no_data
        )
        if counts:
            result[channel] = counts.most_common(1)[0][0]
    return result


def chk_firebug_cip_structure(
    packets: List[FireBugPacket],
) -> CheckResult:
    size_mismatch = sum(
        packet.size != packet.actual_size for packet in packets)
    header_bad = sum(
        packet.eoh != 2 or packet.fmt != 0x10 for packet in packets)
    geometry_bad = sum(
        packet.frames is None or
        (not packet.is_no_data and packet.frames != SYT_INTERVAL_FRAMES)
        for packet in packets
    )
    data_no_info = sum(
        not packet.is_no_data and packet.syt == 0xFFFF
        for packet in packets
    )
    no_data_syt = sum(
        packet.is_no_data and packet.syt != 0xFFFF
        for packet in packets
    )
    bad = (
        size_mismatch + header_bad + geometry_bad +
        data_no_info + no_data_syt
    )
    return CheckResult(
        "FireBug CIP structure and packet geometry",
        bad == 0,
        f"packets={len(packets)}, sizeMismatch={size_mismatch}, "
        f"badEOH/FMT={header_bad}, badGeometry={geometry_bad}, "
        f"DATA-with-ffff={data_no_info}, NO-DATA-with-SYT={no_data_syt}")


def chk_firebug_data_fdf(
    packets: List[FireBugPacket],
) -> CheckResult:
    details: List[str] = []
    bad = 0
    for channel in sorted(set(packet.channel for packet in packets)):
        counts = Counter(
            packet.fdf for packet in packets
            if packet.channel == channel and not packet.is_no_data
        )
        if not counts:
            continue
        expected = counts.most_common(1)[0][0]
        mismatches = sum(
            count for value, count in counts.items() if value != expected)
        bad += mismatches
        rendered = Counter({f"0x{value:02x}": count
                            for value, count in counts.items()})
        details.append(
            f"ch{channel} modal=0x{expected:02x} "
            f"values={_counter_text(rendered)}")
    return CheckResult(
        "DATA FDF is stable per channel",
        bad == 0,
        "; ".join(details) if details else "no DATA packets")


def chk_firebug_no_data_fdf(
    packets: List[FireBugPacket],
) -> CheckResult:
    """Saffire compatibility check, not a claim about every IEC 61883 peer."""
    expected_by_channel = _firebug_data_fdf_by_channel(packets)
    details: List[str] = []
    bad = 0
    checked = 0
    for channel in sorted(expected_by_channel):
        expected = expected_by_channel[channel]
        no_data = [
            packet for packet in packets
            if packet.channel == channel and packet.is_no_data
        ]
        mismatches = [packet for packet in no_data if packet.fdf != expected]
        checked += len(no_data)
        bad += len(mismatches)
        values = Counter(f"0x{packet.fdf:02x}" for packet in no_data)
        details.append(
            f"ch{channel} expected=0x{expected:02x}, "
            f"observed={_counter_text(values)}, "
            f"mismatch={len(mismatches)}/{len(no_data)}")
    return CheckResult(
        "NO-DATA FDF matches stream FDF (Saffire compatibility)",
        bad == 0,
        "; ".join(details) if checked else "no NO-DATA packets")


def chk_firebug_syt_cadence(
    segments: List[List[FireBugPacket]],
) -> CheckResult:
    deltas_by_channel: Dict[int, Counter] = {}
    bad = 0
    checked = 0
    for segment in segments:
        channels = sorted(set(packet.channel for packet in segment))
        for channel in channels:
            data_packets = [
                packet for packet in segment
                if packet.channel == channel and not packet.is_no_data
            ]
            counter = deltas_by_channel.setdefault(channel, Counter())
            for previous, current in zip(data_packets, data_packets[1:]):
                delta = syt_diff_in_offsets(current.syt, previous.syt)
                counter[delta] += 1
                checked += 1
                if delta != SYT_PACKET_STEP_TICKS:
                    bad += 1
    details = [
        f"ch{channel} deltas={_counter_text(counter)}"
        for channel, counter in sorted(deltas_by_channel.items())
    ]
    return CheckResult(
        "DATA SYT cadence is +4096 ticks (+8 frames)",
        bad == 0,
        f"{checked - bad}/{checked} transitions exact; " + "; ".join(details))


def chk_firebug_dbc(
    segments: List[List[FireBugPacket]],
) -> CheckResult:
    bad = 0
    checked = 0
    by_channel: Dict[int, Tuple[int, int]] = {}
    channels = sorted({
        packet.channel for segment in segments for packet in segment
    })
    for channel in channels:
        channel_bad = 0
        channel_checked = 0
        for segment in segments:
            channel_packets = [
                packet for packet in segment if packet.channel == channel
            ]
            for previous, current in zip(
                    channel_packets, channel_packets[1:]):
                frames = previous.frames
                if frames is None:
                    continue
                expected = (previous.dbc + frames) & 0xFF
                channel_checked += 1
                if current.dbc != expected:
                    channel_bad += 1
        bad += channel_bad
        checked += channel_checked
        by_channel[channel] = (channel_bad, channel_checked)
    details = "; ".join(
        f"ch{channel} {count - failures}/{count}"
        for channel, (failures, count) in sorted(by_channel.items())
    )
    return CheckResult(
        "DBC advances by DATA frames and holds on NO-DATA",
        bad == 0,
        f"{checked - bad}/{checked} transitions exact; {details}")


def _firebug_epoch_offsets(
    segment: List[FireBugPacket],
    tx_channel: int = 0,
    rx_channel: int = 1,
) -> Counter:
    tx = {
        (packet.seconds, packet.cycle): packet
        for packet in segment
        if packet.channel == tx_channel and not packet.is_no_data
    }
    rx = {
        (packet.seconds, packet.cycle): packet
        for packet in segment
        if packet.channel == rx_channel and not packet.is_no_data
    }
    offsets: Counter = Counter()
    for key in sorted(tx.keys() & rx.keys()):
        delta_ticks = syt_diff_in_offsets(tx[key].syt, rx[key].syt)
        if delta_ticks % (FW_HZ // SAMPLE_RATE) == 0:
            offsets[delta_ticks // (FW_HZ // SAMPLE_RATE)] += 1
        else:
            offsets[f"{delta_ticks}ticks"] += 1
    return offsets


def chk_firebug_duplex_epoch(
    segments: List[List[FireBugPacket]],
) -> CheckResult:
    offsets: Counter = Counter()
    for segment in segments:
        offsets.update(_firebug_epoch_offsets(segment))
    numeric_offsets = [
        value for value, count in offsets.items()
        for _ in range(count) if isinstance(value, int)
    ]
    nonzero = sum(value != 0 for value in numeric_offsets)
    nonframe = sum(
        count for value, count in offsets.items()
        if not isinstance(value, int)
    )
    checked = sum(offsets.values())
    return CheckResult(
        "duplex epoch ch0-ch1 matches Saffire golden (0 frames)",
        checked == 0 or (nonzero == 0 and nonframe == 0),
        f"same-cycle DATA pairs={checked}, "
        f"offsetFrames={_counter_text(offsets)}")


def firebug_segment_summaries(
    segments: List[List[FireBugPacket]],
) -> List[str]:
    summaries: List[str] = []
    for index, segment in enumerate(segments):
        first = segment[0]
        last = segment[-1]
        channels = Counter(packet.channel for packet in segment)
        channel_text = ",".join(
            f"ch{channel}={count}" for channel, count in sorted(channels.items())
        )
        offsets = _firebug_epoch_offsets(segment)
        epoch = _counter_text(offsets) if offsets else "n/a"
        summaries.append(
            f"segment {index}: "
            f"{first.seconds:03d}:{first.cycle:04d}.."
            f"{last.seconds:03d}:{last.cycle:04d} "
            f"packets={len(segment)} ({channel_text}) "
            f"ch0-ch1 frames={epoch}")
    return summaries


def analyze_firebug(
    packets: List[FireBugPacket],
) -> Tuple[List[CheckResult], List[str]]:
    segments = split_firebug_segments(packets)
    results = [
        chk_firebug_cip_structure(packets),
        chk_firebug_data_fdf(packets),
        chk_firebug_no_data_fdf(packets),
        chk_firebug_syt_cadence(segments),
        chk_firebug_dbc(segments),
        chk_firebug_duplex_epoch(segments),
    ]
    return results, firebug_segment_summaries(segments)


def chk_seed(recs: List[ZtsRecord]) -> CheckResult:
    seeds = [r for r in recs if r.kind == "SEED"]
    ok = len(seeds) == 1 and seeds[0].count == 1
    detail = f"{len(seeds)} seed(s); " + (
        f"seed.count={seeds[0].count} rawHost={seeds[0].rawHost} (nonzero={seeds[0].rawHost != 0})"
        if seeds else "NO SEED captured")
    return CheckResult("seed sanity (exactly one, count=1, real host)",
                       ok and seeds[0].rawHost != 0, detail)


def chk_backcorrection(recs: List[ZtsRecord], tol: int = 1) -> CheckResult:
    worst = 0
    bad = 0
    for r in recs:
        expect = fw_ticks_to_mach(r.age)
        got = r.drainHost - r.rawHost
        err = abs(got - expect)
        worst = max(worst, err)
        if err > tol:
            bad += 1
    return CheckResult(
        "back-correction rawHost == drainHost - age->mach",
        bad == 0,
        f"{len(recs) - bad}/{len(recs)} exact (<= {tol} tick); worst err = {worst} ticks")


def chk_cycle_delta(recs: List[ZtsRecord], tol: int = 1) -> CheckResult:
    worst = 0
    bad = 0
    for r in recs:
        got = signed_domain_diff(r.drainCycleAbs, r.rxCycleAbs)
        err = abs(got - r.age)
        worst = max(worst, err)
        if err > tol:
            bad += 1
    return CheckResult(
        "cycle-delta age == drainCycle - rxCycle (FW domain)",
        bad == 0,
        f"{len(recs) - bad}/{len(recs)} exact (<= {tol} tick); worst err = {worst} ticks")


def chk_grid_step(recs: List[ZtsRecord]) -> CheckResult:
    bad_step = 0
    bad_align = sum(1 for r in recs if r.frame % ZTS_PERIOD != 0)
    for a, b in zip(recs, recs[1:]):
        if (b.frame - a.frame) != (b.count - a.count) * ZTS_PERIOD:
            bad_step += 1
    ok = bad_step == 0 and bad_align == 0
    return CheckResult(
        "grid step d(frame) == d(count)*P and frame %% P == 0",
        ok,
        f"{bad_step} step mismatch(es), {bad_align} unaligned frame(s) [P={ZTS_PERIOD}]")


def chk_monotonic(recs: List[ZtsRecord]) -> CheckResult:
    f_bad = sum(1 for a, b in zip(recs, recs[1:]) if b.frame <= a.frame)
    h_bad = sum(1 for a, b in zip(recs, recs[1:]) if b.host <= a.host)
    return CheckResult("monotonic frame & host (published grid)",
                       f_bad == 0 and h_bad == 0,
                       f"{f_bad} frame regressions, {h_bad} host regressions")


def chk_interp_bound(recs: List[ZtsRecord]) -> CheckResult:
    bad = 0
    worst = 0.0
    for r in recs:
        d = r.host - r.rawHost
        max_ticks = r.dec * MACH_HZ / SAMPLE_RATE + 2  # framesDecoded frames + slack
        if d < -1 or d > max_ticks:
            bad += 1
        worst = max(worst, d)
    return CheckResult(
        "interpolation 0 <= host - rawHost <= dec/48000 (mach)",
        bad == 0,
        f"{len(recs) - bad}/{len(recs)} in-bound; worst host-rawHost = {worst:.0f} ticks "
        f"(~{worst / (MACH_HZ / SAMPLE_RATE):.2f} frames)")


def chk_rx_syt_recovery(recs: List[ZtsRecord]) -> CheckResult:
    bad_phase = 0
    bad_lead = 0
    checked = 0
    for r in recs:
        if r.syt == 0xFFFF:
            continue
        checked += 1
        expected_phase = extend_syt_from_cycle_timer(r.rxCycle, r.syt)
        if r.recPhase != expected_phase:
            bad_phase += 1
        packet_ticks = cycle_timer_abs_ticks(r.rxCycle) % OFFSET_DOMAIN_TICKS
        if r.sytLead != ext_offset_diff(expected_phase, packet_ticks):
            bad_lead += 1
    return CheckResult(
        "rx SYT recovery: extend(packet cycle, SYT) -> phase and lead",
        bad_phase == 0 and bad_lead == 0,
        f"{checked - bad_phase}/{checked} recovered phases exact; "
        f"{checked - bad_lead}/{checked} presentation leads exact")


def chk_txsyt_monotonicity(recs: List[TxSytRecord]) -> CheckResult:
    bad = 0
    for a, b in zip(recs, recs[1:]):
        if b.pkt <= a.pkt:
            bad += 1
    return CheckResult(
        "txsyt packet monotonicity",
        bad == 0,
        f"{bad} packet index regression(s)"
    )


def chk_txsyt_seeding(
    recs: List[TxSytRecord],
    initial_lead: int = TX_INITIAL_LEAD_TICKS,
) -> CheckResult:
    bad = 0
    seeded_count = 0
    for r in recs:
        if r.flags & TX_FLAG_SEEDED:
            seeded_count += 1
            expected = normalize_offset_domain(r.anchor + initial_lead)
            if r.phasePre != expected:
                bad += 1
    worst = max(
        (abs(ext_offset_diff(r.phasePre, r.anchor + initial_lead))
         for r in recs if r.flags & TX_FLAG_SEEDED),
        default=0,
    )
    return CheckResult(
        "txsyt seeding phase: phasePre == (anchor + initialLead) % 8s",
        bad == 0,
        f"{seeded_count - bad}/{seeded_count} captured seed(s) match; "
        f"worst error = {worst} ticks"
    )


def chk_txsyt_sparse_phase_chain(recs: List[TxSytRecord]) -> CheckResult:
    """Replay phase continuity across sampled-out telemetry.

    The TX telemetry ring emits sparse bursts, so adjacent log records are
    rarely adjacent packets. The cadence read index reveals the number of DATA
    commits modulo 512; unwrap it near the 48 kHz blocking rate (3 DATA packets
    per 4 isoch slots), then advance phase by the recorded cadence entry.
    """
    bad = 0
    checked = 0
    total_slots = 0
    total_commits = 0
    for a, b in zip(recs, recs[1:]):
        if (b.flags & TX_FLAG_SEEDED) or (a.flags & TX_FLAG_RESEEDED):
            continue
        if a.pend == 0 or b.pend != a.pend or b.rollCad != a.rollCad:
            continue

        slots = b.pkt - a.pkt
        if slots <= 0:
            continue
        index_delta = (b.ridx - a.ridx) & (RX_CADENCE_ENTRIES - 1)
        expected_commits = (
            slots * SAMPLE_RATE /
            (CYCLES_PER_SEC * SYT_INTERVAL_FRAMES)
        )
        wraps = round(
            (expected_commits - index_delta) / RX_CADENCE_ENTRIES)
        commits = index_delta + wraps * RX_CADENCE_ENTRIES
        if commits < 0:
            continue

        checked += 1
        total_slots += slots
        total_commits += commits
        expected_phase = normalize_offset_domain(
            a.phasePost + commits * a.pend)
        if b.phasePre != expected_phase:
            bad += 1

    ideal_commits = (
        total_slots * SAMPLE_RATE /
        (CYCLES_PER_SEC * SYT_INTERVAL_FRAMES)
    )
    deficit = total_commits - ideal_commits
    return CheckResult(
        "txsyt sparse phase chain: read-index commits advance carried phase",
        bad == 0,
        f"{checked - bad}/{checked} sampled gaps replay exactly; "
        f"{total_commits} DATA commits / {total_slots} slots "
        f"({total_commits / total_slots if total_slots else 0:.6f}, "
        f"ideal 0.750000, deficit {deficit:+.1f} packets)"
    )


def chk_txsyt_data_slot_continuity(
    recs: List[TxSytRecord],
) -> CheckResult:
    """Detect post-lock DATA opportunities that were emitted as NO-DATA.

    Packetizer cycle zero is NO-DATA and the absolute blocking cadence is
    N,D,D,D. The cadence read index advances only for committed DATA packets,
    so sparse telemetry can still reveal holes by comparing its unwrapped
    delta with the exact number of DATA slots between packet indices.
    """
    actual_commits = 0
    expected_commits = 0
    checked = 0
    for a, b in zip(recs, recs[1:]):
        if (b.flags & TX_FLAG_SEEDED) or (a.flags & TX_FLAG_RESEEDED):
            continue

        slots = b.pkt - a.pkt
        if slots <= 0:
            continue
        index_delta = (b.ridx - a.ridx) & (RX_CADENCE_ENTRIES - 1)
        approximate = slots * 3 / 4
        wraps = round((approximate - index_delta) / RX_CADENCE_ENTRIES)
        commits = index_delta + wraps * RX_CADENCE_ENTRIES
        if commits < 0:
            continue

        # Number of multiples of four in [a.pkt, b.pkt) are NO-DATA slots.
        no_data_slots = ((b.pkt + 3) // 4) - ((a.pkt + 3) // 4)
        expected = slots - no_data_slots
        actual_commits += commits
        expected_commits += expected
        checked += 1

    missed = expected_commits - actual_commits
    return CheckResult(
        "txsyt DATA-slot continuity: no post-lock N,D,D,D holes",
        missed == 0,
        f"{actual_commits}/{expected_commits} expected DATA slots committed "
        f"across {checked} sampled gaps; missed={missed}")


def chk_txsyt_servo(
    recs: List[TxSytRecord],
    xmit_delay: int = TX_XMIT_TRANSFER_DELAY_TICKS,
    deadband: int = TX_PHASE_DEADBAND_FRAMES,
) -> CheckResult:
    bad_rx_free = 0
    bad_pe = 0
    bad_servo = 0
    bad_force = 0
    bad_post = 0
    bad_lead = 0
    bad_wire = 0
    bad_health = 0
    bad_gate = 0
    bad_syt = 0

    for r in recs:
        replay = replay_tx_servo(
            anchor=r.anchor,
            phase_pre=r.phasePre,
            recovered_phase=r.recPhase,
            rolling_cadence=r.rollCad,
            seeded_this_call=bool(r.flags & TX_FLAG_SEEDED),
            phase_deadband=deadband,
            xmit_delay=xmit_delay,
        )

        if r.rxFree != replay.rx_phase_delay_free:
            bad_rx_free += 1
        if r.pErr != replay.phase_error:
            bad_pe += 1
        if (r.corr != replay.correction_ticks or
                r.fErr != replay.frame_error):
            bad_servo += 1
        if bool(r.flags & TX_FLAG_FORCE_ADJUST) != replay.force_adjust:
            bad_force += 1
        if r.phasePost != replay.phase_post:
            bad_post += 1
        if r.lead != replay.lead_ticks:
            bad_lead += 1
        if r.wire != replay.wire_lead_ticks:
            bad_wire += 1
        if r.health != replay.health:
            bad_health += 1
        expected_reseed = replay.gated
        if (bool(r.flags & TX_FLAG_RESEEDED) != expected_reseed or
                (replay.gated and (r.flags & TX_FLAG_COMMITTED))):
            bad_gate += 1
        if r.syt != replay.syt:
            bad_syt += 1

    details = []
    if bad_rx_free: details.append(f"rxFree mismatch in {bad_rx_free} pkts")
    if bad_pe: details.append(f"pErr mismatch in {bad_pe} pkts")
    if bad_servo: details.append(f"corr/fErr mismatch in {bad_servo} pkts")
    if bad_force: details.append(f"force/deadband mismatch in {bad_force} pkts")
    if bad_post: details.append(f"phasePost mismatch in {bad_post} pkts")
    if bad_lead: details.append(f"lead mismatch in {bad_lead} pkts")
    if bad_wire: details.append(f"wire mismatch in {bad_wire} pkts")
    if bad_health: details.append(f"health mismatch in {bad_health} pkts")
    if bad_gate: details.append(f"gate/reseed mismatch in {bad_gate} pkts")
    if bad_syt: details.append(f"syt mismatch in {bad_syt} pkts")

    passed = (bad_rx_free == 0 and bad_pe == 0 and bad_servo == 0 and
              bad_force == 0 and bad_post == 0 and bad_lead == 0 and
              bad_wire == 0 and bad_health == 0 and bad_gate == 0 and
              bad_syt == 0)
    detail_str = (
        ", ".join(details) if details else
        f"all IDA-derived servo/governor branches replay exactly across "
        f"{len(recs)} records"
    )
    return CheckResult("txsyt exact Saffire servo replay",
                       passed, detail_str)


def chk_txsyt_runtime_lock(recs: List[TxSytRecord]) -> CheckResult:
    late = sum(r.health == TX_HEALTH_LATE for r in recs)
    gated = sum(
        r.health in (TX_HEALTH_GATE, TX_HEALTH_ESCALATE) for r in recs)
    adjusted = sum(bool(r.flags & TX_FLAG_FORCE_ADJUST) for r in recs)
    raw_min = min(r.lead for r in recs)
    raw_max = max(r.lead for r in recs)
    frame_error_min = min(r.fErr for r in recs)
    frame_error_max = max(r.fErr for r in recs)

    wire_phases = sorted(set(r.wire % SYT_FIELD_DOMAIN_TICKS for r in recs))
    if len(wire_phases) <= 1:
        wire_span = 0
    else:
        gaps = [
            b - a for a, b in zip(wire_phases, wire_phases[1:])
        ]
        gaps.append(
            wire_phases[0] + SYT_FIELD_DOMAIN_TICKS - wire_phases[-1])
        wire_span = SYT_FIELD_DOMAIN_TICKS - max(gaps)

    return CheckResult(
        "txsyt runtime lock: no late/gated DATA decisions",
        late == 0 and gated == 0,
        f"late={late}/{len(recs)}, gated={gated}/{len(recs)}; "
        f"forceAdjust={adjusted}, frameError=[{frame_error_min},"
        f"{frame_error_max}]; "
        f"raw lead=[{raw_min},{raw_max}] ticks "
        f"([{raw_min / 512:.1f},{raw_max / 512:.1f}] frames); "
        f"on-wire 16-cycle phase covers {wire_span / 512:.1f} frames")


def chk_coherence(recs: List[ZtsRecord]) -> List[CheckResult]:
    out: List[CheckResult] = []

    # Primary host<->bus: drainHost(mach) vs drainCycle(FW), sampled same instant.
    fit = linfit([r.drainCycleAbs for r in recs], [r.drainHost for r in recs])
    ideal = MACH_HZ / FW_HZ
    dppm = ppm(fit.slope, ideal)
    out.append(CheckResult(
        "host<->bus coherence: d(drainHost)/d(drainCycle) == MACH/FW",
        abs(dppm) < 1000.0,
        f"slope={fit.slope:.8f} ideal={ideal:.8f} drift={dppm:+.1f} ppm "
        f"R2={fit.r2:.6f} maxResid={fit.max_abs_resid:.0f} mach-ticks"))

    # Grid rate: published host(mach) vs sampleFrame.
    fit = linfit([r.frame for r in recs], [r.host for r in recs])
    ideal = MACH_HZ / SAMPLE_RATE
    dppm = ppm(fit.slope, ideal)
    out.append(CheckResult(
        "grid rate: d(host)/d(frame) == MACH/48000 (=500)",
        abs(dppm) < 1000.0,
        f"slope={fit.slope:.6f} ideal={ideal:.1f} drift={dppm:+.1f} ppm "
        f"R2={fit.r2:.8f} maxResid={fit.max_abs_resid:.0f} mach-ticks"))

    # Device cadence: rxCycle(FW) vs sampleFrame.
    fit = linfit([r.frame for r in recs], [r.rxCycleAbs for r in recs])
    ideal = FW_HZ / SAMPLE_RATE
    dppm = ppm(fit.slope, ideal)
    out.append(CheckResult(
        "device cadence: d(rxCycle)/d(frame) == FW/48000 (=512)",
        abs(dppm) < 1000.0,
        f"slope={fit.slope:.6f} ideal={ideal:.1f} drift={dppm:+.1f} ppm "
        f"R2={fit.r2:.8f} maxResid={fit.max_abs_resid:.0f} FW-ticks"))
    return out


def chk_cadence_stability(recs: List[ZtsRecord]) -> CheckResult:
    rc = sorted(set(r.rollCad for r in recs))
    sl = sorted(set(r.sytLead for r in recs))
    return CheckResult("recovered cadence stability (rollCad / sytLead spread)",
                       True,  # informational
                       f"rollCad distinct={len(rc)} range=[{rc[0]},{rc[-1]}]; "
                       f"sytLead distinct={len(sl)} range=[{sl[0]},{sl[-1]}]")


def run_checks(recs: List[ZtsRecord]) -> List[CheckResult]:
    results = [
        chk_seed(recs),
        chk_backcorrection(recs),
        chk_cycle_delta(recs),
        chk_rx_syt_recovery(recs),
        chk_grid_step(recs),
        chk_monotonic(recs),
        chk_interp_bound(recs),
    ]
    results += chk_coherence(recs)
    results.append(chk_cadence_stability(recs))
    return results


# ---------------------------------------------------------------------------
# Forward model (drives selftest; proves the checks have teeth)
# ---------------------------------------------------------------------------
def synthesize(n: int, *, skew_ppm: float = 0.0, break_backcorr: bool = False,
               break_grid: bool = False, stride: int = 4,
               base_age: int = 1900, age_jitter: int = 2800) -> List[ZtsRecord]:
    """Generate ZtsRecords from the driver's grid/back-correction math.

    A device bus clock (FW_HZ) drives frames; the host mach clock runs at
    MACH_HZ*(1+skew_ppm) -- an independent oscillator. Each published anchor
    samples drainCycle/drainHost at the same instant, then derives rawHost/host
    exactly as IsochReceiveContext does.
    """
    fw0 = 45 * CYCLES_PER_SEC * TICKS_PER_CYCLE      # start near the real seed
    mach0 = 2_233_000_000_000
    frame0 = 4224
    skew = 1.0 + skew_ppm / 1e6
    recs: List[ZtsRecord] = []
    step = ZTS_PERIOD + (8 if break_grid else 0)
    rng = _Lcg(0xC0FFEE)
    for k in range(n):
        if (k % stride) != 0 and k != 0:
            continue
        frame = frame0 + step * k
        bt = fw0 + frame * (FW_HZ // SAMPLE_RATE)          # bus tick at this frame
        rx_abs = (bt // TICKS_PER_CYCLE) * TICKS_PER_CYCLE  # cycle-aligned (offset 0)
        age = base_age + rng.span(age_jitter)              # drain after packet (+/-)
        drain_abs = rx_abs + age
        drain_host = mach0 + round(drain_abs / FW_HZ * MACH_HZ * skew)
        if break_backcorr:
            raw_host = drain_host - int(fw_ticks_to_mach(age) * 1.05)  # wrong factor
        else:
            raw_host = drain_host - fw_ticks_to_mach(age)
        fffps = 8
        host = raw_host + (fffps * MACH_HZ) // SAMPLE_RATE
        rx_cycle_timer = _encode_ct(rx_abs)
        syt = encode_syt(rx_abs + 7344)
        recovered_phase = extend_syt_from_cycle_timer(rx_cycle_timer, syt)
        r = ZtsRecord(
            kind="SEED" if k == 0 else "UPD", count=k + 1, frame=frame,
            host=host, rawHost=raw_host, drainHost=drain_host,
            drainCycle=_encode_ct(drain_abs), rxCycle=rx_cycle_timer,
            age=age, rawRxTs=(rx_cycle_timer & 0xFFFF), syt=syt,
            sytLead=7344, rollCad=1048576,
            recPhase=recovered_phase, desc=(k * 32) % 512, dec=8)
        recs.append(r)
    return unwrap_cycle_seconds(recs)


def _encode_ct(abs_ticks: int) -> int:
    abs_ticks %= FULL_DOMAIN_TICKS
    off = abs_ticks % TICKS_PER_CYCLE
    total_cyc = abs_ticks // TICKS_PER_CYCLE
    sec = (total_cyc // CYCLES_PER_SEC) % SECONDS_FIELD_WRAP
    cyc = total_cyc % CYCLES_PER_SEC
    return (sec << CYCLE_TIMER_SECONDS_SHIFT) | (cyc << CYCLE_TIMER_CYCLES_SHIFT) | off


class _Lcg:
    """Tiny deterministic PRNG so selftest is reproducible without `random`."""
    def __init__(self, seed: int):
        self.s = seed & 0xFFFFFFFF

    def next(self) -> int:
        self.s = (1103515245 * self.s + 12345) & 0x7FFFFFFF
        return self.s

    def span(self, half: int) -> int:
        return (self.next() % (2 * half + 1)) - half


# ---------------------------------------------------------------------------
# Reporting / CLI
# ---------------------------------------------------------------------------
def report(title: str, results: List[CheckResult]) -> bool:
    print(f"\n=== {title} ===")
    all_pass = True
    for r in results:
        mark = "PASS" if r.passed else "FAIL"
        if not r.passed:
            all_pass = False
        print(f"  [{mark}] {r.name}\n         {r.detail}")
    print(f"  ---> {'ALL PASS' if all_pass else 'FAILURES PRESENT'}")
    return all_pass


def chk_saffire_rx_ring_model() -> CheckResult:
    model = SaffireRxSytSync()
    for k in range(RX_CADENCE_ENTRIES):
        packet_cycle = (4 * k) // 3
        model.observe(
            encode_syt(k * SYT_PACKET_STEP_TICKS + 7344),
            _encode_ct(packet_cycle * TICKS_PER_CYCLE),
        )
    early_lock = model.established

    k = RX_CADENCE_ENTRIES
    packet_cycle = (4 * k) // 3
    model.observe(
        encode_syt(k * SYT_PACKET_STEP_TICKS + 7344),
        _encode_ct(packet_cycle * TICKS_PER_CYCLE),
    )
    snap = model.snapshot()
    read_index = (
        snap.write_index + RX_CADENCE_READ_DELAY
    ) & (RX_CADENCE_ENTRIES - 1)
    expected_rolling = RX_CADENCE_READ_DELAY * SYT_PACKET_STEP_TICKS
    ok = (
        not early_lock and
        snap.established and
        snap.valid_updates == RX_CADENCE_WARMUP and
        snap.rolling_cadence_ticks == expected_rolling and
        model.entries[read_index] == SYT_PACKET_STEP_TICKS
    )
    return CheckResult(
        "IDA RX cadence ring bootstrap (513 updates, TX reads 256 behind)",
        ok,
        f"earlyLock={early_lock}, updates={snap.valid_updates}, "
        f"rolling={snap.rolling_cadence_ticks}, write={snap.write_index}, "
        f"initialTxRead={read_index}, entry={model.entries[read_index]}")


def chk_firebug_parser_model() -> CheckResult:
    lines = [
        "010:0100:0100  Isoch channel 1, tag 1, sy 0, "
        "size 552 [actual 552] s400\n",
        "               0000   02110000 900210b0\n",
        "010:0100:0200  Isoch channel 0, tag 1, sy 0, "
        "size 296 [actual 296] s400\n",
        "               0000   00090000 900210b0\n",
        "010:0101:0100  Isoch channel 1, tag 1, sy 0, "
        "size 8 [actual 8] s400\n",
        "               0000   02110008 9002ffff\n",
        "010:0101:0200  Isoch channel 0, tag 1, sy 0, "
        "size 8 [actual 8] s400\n",
        "               0000   00090008 9002ffff\n",
        "010:0102:0100  Isoch channel 1, tag 1, sy 0, "
        "size 552 [actual 552] s400\n",
        "               0000   02110008 900224b0\n",
        "010:0102:0200  Isoch channel 0, tag 1, sy 0, "
        "size 296 [actual 296] s400\n",
        "               0000   00090008 900224b0\n",
    ]
    packets = parse_firebug_lines(lines)
    results, _ = analyze_firebug(packets)
    failures = [result.name for result in results if not result.passed]
    return CheckResult(
        "FireBug parser and golden-compatible packet analysis",
        len(packets) == 6 and not failures,
        f"packets={len(packets)}, failures={failures or 'none'}")


def synthesize_txsyt(n: int, *, break_servo: bool = False,
                     break_continuity: bool = False,
                     break_data_slots: bool = False) -> List[TxSytRecord]:
    recs: List[TxSytRecord] = []
    anchor0 = 10_000_000
    phase = normalize_offset_domain(anchor0 + TX_INITIAL_LEAD_TICKS)
    roll_cad = 1048576

    for k in range(n):
        packet_slot = (4 * k) // 3
        if break_data_slots and k >= n // 2:
            packet_slot += 4
        anchor = normalize_offset_domain(
            anchor0 + packet_slot * TICKS_PER_CYCLE)
        rec_phase = normalize_offset_domain(
            anchor + TX_INITIAL_LEAD_TICKS +
            TX_XMIT_TRANSFER_DELAY_TICKS)
        phase_pre = phase
        rx_free = normalize_offset_domain(
            rec_phase - TX_XMIT_TRANSFER_DELAY_TICKS)
        pe = ext_offset_diff(phase_pre, rx_free)

        cadence_scale = SYT_INTERVAL_FRAMES << 8
        if pe >= 0:
            rem = (pe * cadence_scale) % roll_cad
            comp = roll_cad - rem
        else:
            rem = ((-pe) * cadence_scale) % roll_cad
            comp = rem

        if rem != 0:
            corr = comp // cadence_scale
            s_rem = rem
            if rem > roll_cad // 2:
                s_rem -= roll_cad
            expected_fe = c_div(s_rem, cadence_scale)
        else:
            corr = 0
            expected_fe = 0

        if break_servo and k == 10:
            corr += 10

        force_adjust = (
            (k == 0) or
            (abs(expected_fe) > TX_PHASE_DEADBAND_FRAMES)
        )
        if not force_adjust:
            phase_post = phase_pre
        else:
            phase_post = normalize_offset_domain(anchor + corr)

        lead = ext_offset_diff(phase_post, anchor)
        wire = lead + TX_XMIT_TRANSFER_DELAY_TICKS
        syt = encode_syt(phase_post + TX_XMIT_TRANSFER_DELAY_TICKS)

        flags = TX_FLAG_COMMITTED
        if k == 0:
            flags |= TX_FLAG_SEEDED
        if force_adjust:
            flags |= TX_FLAG_FORCE_ADJUST

        pend = SYT_PACKET_STEP_TICKS

        recs.append(TxSytRecord(
            pkt=packet_slot + 1, flags=flags,
            health=classify_tx_lead(lead), anchor=anchor,
            phasePre=phase_pre, phasePost=phase_post, recPhase=rec_phase,
            rxFree=rx_free, pErr=pe, fErr=expected_fe, corr=corr, lead=lead,
            wire=wire, rollCad=roll_cad, pend=pend, ridx=0, syt=syt
        ))
        recs[-1].ridx = k & (RX_CADENCE_ENTRIES - 1)

        phase = normalize_offset_domain(phase_post + pend)
        if break_continuity and k == 20:
            phase = normalize_offset_domain(phase + 100)

    return recs


def cmd_verify(path: str) -> int:
    zts_recs, txsyt_recs = parse_log(path)
    if not zts_recs and not txsyt_recs:
        print(f"No [Zts] or [TxSyt] records parsed from {path}", file=sys.stderr)
        return 2

    ok = True
    if zts_recs:
        span_s = (zts_recs[-1].host - zts_recs[0].host) / MACH_HZ
        print(f"Parsed {len(zts_recs)} [Zts] records spanning {span_s:.3f} s "
              f"(count {zts_recs[0].count}..{zts_recs[-1].count}, "
              f"frame {zts_recs[0].frame}..{zts_recs[-1].frame})")
        ok &= report(f"verify {path} [Zts]", run_checks(zts_recs))

    if txsyt_recs:
        print(f"Parsed {len(txsyt_recs)} [TxSyt] records "
              f"(pkt {txsyt_recs[0].pkt}..{txsyt_recs[-1].pkt})")
        txsyt_results = [
            chk_txsyt_monotonicity(txsyt_recs),
            chk_txsyt_seeding(txsyt_recs),
            chk_txsyt_sparse_phase_chain(txsyt_recs),
            chk_txsyt_data_slot_continuity(txsyt_recs),
            chk_txsyt_servo(txsyt_recs),
            chk_txsyt_runtime_lock(txsyt_recs),
        ]
        ok &= report(f"verify {path} [TxSyt]", txsyt_results)

    return 0 if ok else 1


def cmd_firebug(paths: List[str]) -> int:
    if not paths:
        paths = ["tools/firebug.txt"]

    overall = True
    for path in paths:
        packets = parse_firebug(path)
        if not packets:
            print(f"No FireBug isoch packets parsed from {path}",
                  file=sys.stderr)
            overall = False
            continue

        segments = split_firebug_segments(packets)
        channels = Counter(packet.channel for packet in packets)
        channel_text = ", ".join(
            f"ch{channel}={count}"
            for channel, count in sorted(channels.items())
        )
        print(
            f"Parsed {len(packets)} FireBug isoch packets from {path} "
            f"across {len(segments)} segment(s): {channel_text}")

        results, summaries = analyze_firebug(packets)
        file_ok = report(f"firebug {path}", results)
        print("  Capture segments:")
        for summary in summaries:
            print(f"    {summary}")

        by_name = {result.name: result for result in results}
        cadence_ok = by_name[
            "DATA SYT cadence is +4096 ticks (+8 frames)"
        ].passed
        epoch_ok = by_name[
            "duplex epoch ch0-ch1 matches Saffire golden (0 frames)"
        ].passed
        fdf_ok = by_name[
            "NO-DATA FDF matches stream FDF (Saffire compatibility)"
        ].passed
        if cadence_ok and not epoch_ok:
            print(
                "  Diagnosis: DATA SYTs are coherent, but channel 0 is on "
                "the wrong RX/device epoch; this is not random SYT data.")
        if not fdf_ok:
            print(
                "  Compatibility: NO-DATA uses a different FDF than DATA. "
                "Saffire.kext preserves the stream FDF, so this capture is "
                "not Saffire-wire-compatible.")
        overall &= file_ok

    return 0 if overall else 1


def cmd_selftest() -> int:
    print("Self-test: clean synthetic data must pass; each fault must trip its check.")
    overall = True

    # 1. Decompiled Saffire synchronization primitive.
    overall &= report(
        "IDA-derived Saffire RX/TX synchronization model",
        [chk_saffire_rx_ring_model(), chk_firebug_parser_model()],
    )

    # 2. ZTS selftests
    clean = synthesize(1000)
    overall &= report("clean Zts model (expect ALL PASS)", run_checks(clean))

    def expect_fail(name_substr: str, recs: List[ZtsRecord], label: str) -> bool:
        res = run_checks(recs)
        hit = [r for r in res if name_substr in r.name and not r.passed]
        ok = bool(hit)
        print(f"\n=== Zts fault: {label} (expect FAIL on '{name_substr}') ===")
        print(f"  ---> {'CAUGHT' if ok else 'MISSED (check has no teeth!)'}")
        return ok

    overall &= expect_fail("host<->bus coherence",
                           synthesize(1000, skew_ppm=5000.0),
                           "host/bus clock skew +5000 ppm")
    overall &= expect_fail("back-correction",
                           synthesize(1000, break_backcorr=True),
                           "broken back-correction factor")
    overall &= expect_fail("grid step",
                           synthesize(1000, break_grid=True),
                           "wrong ZTS grid step (200 != 192)")

    # 3. TxSyt selftests
    tx_clean = synthesize_txsyt(100)
    tx_results = [
        chk_txsyt_monotonicity(tx_clean),
        chk_txsyt_seeding(tx_clean),
        chk_txsyt_sparse_phase_chain(tx_clean),
        chk_txsyt_data_slot_continuity(tx_clean),
        chk_txsyt_servo(tx_clean),
        chk_txsyt_runtime_lock(tx_clean),
    ]
    overall &= report("clean TxSyt model (expect ALL PASS)", tx_results)

    def expect_tx_fail(check_func: Callable[[List[TxSytRecord]], CheckResult],
                       recs: List[TxSytRecord], label: str) -> bool:
        r = check_func(recs)
        ok = not r.passed
        print(f"\n=== TxSyt fault: {label} (expect FAIL) ===")
        print(f"  ---> {'CAUGHT' if ok else 'MISSED (check has no teeth!)'}")
        return ok

    overall &= expect_tx_fail(chk_txsyt_servo,
                              synthesize_txsyt(100, break_servo=True),
                              "broken servo math")
    overall &= expect_tx_fail(chk_txsyt_sparse_phase_chain,
                              synthesize_txsyt(100, break_continuity=True),
                              "broken continuity")
    overall &= expect_tx_fail(chk_txsyt_data_slot_continuity,
                              synthesize_txsyt(
                                  100, break_data_slots=True),
                              "skipped post-lock DATA slots")

    print(f"\n==== SELFTEST {'OK' if overall else 'FAILED'} ====")
    return 0 if overall else 1


def main(argv: List[str]) -> int:
    if len(argv) >= 2 and argv[1] == "verify":
        default = "tools/zts_telemetry.log"
        return cmd_verify(argv[2] if len(argv) > 2 else default)
    if len(argv) >= 2 and argv[1] == "firebug":
        return cmd_firebug(argv[2:])
    if len(argv) >= 2 and argv[1] == "selftest":
        return cmd_selftest()
    print(__doc__)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
