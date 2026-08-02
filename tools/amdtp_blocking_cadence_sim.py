#!/usr/bin/env python3
"""
amdtp_blocking_cadence_sim.py -- 44.1-family blocking cadence/SYT simulator.

Executable validation of the blocking-mode packet cadence math for the
complete IEC 61883-6 rate ladder -- 32, 44.1, 48, 88.2, 96, 176.4, and
192 kHz with SYT intervals 8/8/8/16/16/32/32 -- covering both the 44.1 and
48k families, per documentation/SAMPLE_RATE_EXPANSION.md (Phase 2 golden
sequences) and documentation/44100.md (Apple AppleFWAudio RE notes).

Three independent models are simulated and cross-checked:

  1. RationalBlockingEngine -- the ASFW-candidate design: one exact
     integer deadline DDA in the FireWire tick domain, state kept in
     "subticks" (ticks x denominator), parameterized by
     (sampleRate, sytInterval). No floats, no per-rate magic numbers.
     A data packet is emitted iff the next SYT deadline lands inside the
     current 125 us cycle; the SYT offset is the deadline's position in
     that cycle. Data/no-data, SYT, and DBC all derive from this one state.

  2. AppleNuDcl441 -- a port of the implementation-neutral pseudocode in
     documentation/44100.md section 7 with the exact IDA-recovered
     constants (packet phase step 30,720,000; next-SYT seed 117,220,000;
     SYT step 44,582,312; modulus 491,520,000; all FireWire ticks x 10^4).
     The resynchronization branch is deliberately absent (as in the doc).

  3. Oracles -- invariants documented by independent, hardware-proven
     stacks (behavior only; no code copied):
       * Linux sound/firewire/amdtp-stream.c:410-447 (calculate_syt_offset
         comment): mod-cycle SYT increments 1386/1387 with an exact
         147-event period; pool_blocking_data_blocks (:350-366): data iff
         SYT deadline due. Reference commit 2c7c88a412aa.
       * Saffire kext (44100.md section 9): full SYT deltas 4458/4459 with
         exactly 34 long deltas per 147 events.
       * 441 data packets per 640 cycles; 3528 frames per 640 cycles.

Also checked:
  * 48k / 32k degenerate cases (D,D,D,N and D,N patterns; constant deltas).
  * Whole-family cadence identity: 88.2/16 and 176.4/32 produce the exact
    same D/N + deadline sequence as 44.1/8 (only frames-per-packet scales).
  * 6-cycle interrupt-group frame bounds (AudioTimingGeometry cross-check):
    exhaustive over all window phases.
  * Ring-wrap phase: the 640-cycle 44.1 period does not divide the 512-slot
    timeline ring (unlike the 4-cycle 48k period), proving the cadence must
    be stateful, not slot-index-derived.
  * DBC continuity: +sytInterval mod 256 on data, unchanged on no-data.
  * Replay path (SAMPLE_RATE_EXPANSION.md section 3): a device-clocked
    generator at +/-ppm feeds a 512-entry ring consumed 256 behind the
    writer; TX replays dataBlocks 1:1. Asserts exact rate lock and exact
    preservation of the device's SYT-delta pattern at every tested ppm.

Notes:
  * SYT presentation lead / transfer delay is a constant added at encode
    time; it cancels in every delta checked here, so the sim works with
    raw deadline ticks.
  * The driver-side engine would reduce the subtick accumulator modulo
    (1 s x den) to stay in uint64; Python ints are unbounded so the sim
    keeps absolute values.

Also modeled: ZTS anchor publication (boundary observability at the
1536-frame profile, anchor spacing, frames<->bus-time slope, Q8
nanos-per-sample quantization, and the anchor-thinning counterexample for a
misaligned period), plus per-second packet-rate accounting (8000 pps at every
rate) and a headline-figures table at the end.

The replay trial runs through Python ports of the PRODUCTION functions
(ComputeReplaySytOffset / ComputeReplaySytFromTicks from
RxSequenceReplay.hpp, cycle-timer fields, 16-cycle SYT extension), including
real 16-bit SYT encode/decode and both branches of the +16-cycle guard --
not just ring preservation.

SCOPE: this sim validates MATH ONLY; it does not enable 44.1 in the driver.
The run ends with a "Production wiring status" section listing exactly what
must change (file:line) before 44.1 is real, and the zero-seed golden prefix
is an engine regression anchor, NOT a wire contract -- startup seed/lead/
reset policy is a separate, capture-gated deliverable
(SAMPLE_RATE_EXPANSION.md sections 6 and 8).

Every claim either passes with numbers or fails loudly (exit code 1). All
"any window" checks are exhaustive, including the final window. Output is
colored when stdout is a TTY (NO_COLOR and --color=never disable).
Runtime: a few seconds at default --cycles.
"""

from __future__ import annotations

import argparse
import os
import random
import sys
import textwrap
from collections import Counter
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path
from typing import List, Optional, Tuple

TICKS_PER_CYCLE = 3072
CYCLES_PER_SECOND = 8000
TICKS_PER_SECOND = TICKS_PER_CYCLE * CYCLES_PER_SECOND  # 24_576_000

# 44.1-family blocking period: 441 data packets per 640 cycles.
FAMILY_441_PERIOD_CYCLES = 640
FAMILY_441_DATA_PER_PERIOD = 441
SYT_DELTA_PERIOD_EVENTS = 147  # exact: 147 * (24576000*8/44100) = 655,360 ticks
SYT_LONG_DELTAS_PER_PERIOD = 34

TIMELINE_SLOTS = 512  # AudioTimingGeometry::kTimelineSlots

# Apple NuDCL constants (documentation/44100.md section 7, IDA-recovered).
APPLE_SCALE = 10_000
APPLE_CYCLE_PHASE = 30_720_000          # 3072 ticks * 10^4
APPLE_PHASE_MODULO = 491_520_000        # 16 cycles * 10^4
APPLE_HALF_RING = APPLE_PHASE_MODULO // 2
APPLE_SYT_STEP_441 = 44_582_312         # ~4458.2312 ticks * 10^4
APPLE_NEXT_SYT_SEED = 117_220_000       # 11,722 ticks * 10^4
APPLE_STARTUP_PREFIX = "NNNDNDDNDDNDDDND"  # 44100.md section 7
APPLE_STARTUP_WINDOW_DATA = 439            # 44100.md section 7


@dataclass
class Decision:
    cycle: int
    is_data: bool
    frames: int
    syt_offset_ticks: Optional[int]  # offset within the cycle, data only
    deadline_ticks_floor: Optional[int]  # absolute floor(deadline), data only
    dbc: int


class RationalBlockingEngine:
    """Exact integer deadline DDA in ticks x step_den ("subticks").

    step_num/step_den = SYT event spacing in ticks. For the nominal engine
    step_num = TICKS_PER_SECOND * sytInterval, step_den = sampleRate.
    A device oscillator at a ppm offset is modeled by scaling the step
    exactly (see make_device_engine)."""

    def __init__(self, step_num: int, step_den: int, syt_interval: int,
                 seed_ticks_num: int = 0, dbc0: int = 0) -> None:
        assert step_num >= TICKS_PER_CYCLE * step_den, \
            "at most one SYT event per cycle (event rate <= 8000/s)"
        self.step_num = step_num
        self.step_den = step_den
        self.syt_interval = syt_interval
        self.deadline_sub = seed_ticks_num  # units of (1/step_den) ticks
        self.cycle = 0
        self.dbc = dbc0

    @classmethod
    def nominal(cls, rate: int, syt_interval: int,
                seed_ticks: int = 0, dbc0: int = 0) -> "RationalBlockingEngine":
        return cls(TICKS_PER_SECOND * syt_interval, rate, syt_interval,
                   seed_ticks * rate, dbc0)

    def next_cycle(self) -> Decision:
        cycle_end_sub = (self.cycle + 1) * TICKS_PER_CYCLE * self.step_den
        if self.deadline_sub < cycle_end_sub:
            deadline_ticks = self.deadline_sub // self.step_den
            offset = deadline_ticks - self.cycle * TICKS_PER_CYCLE
            assert 0 <= offset < TICKS_PER_CYCLE
            d = Decision(self.cycle, True, self.syt_interval, offset,
                         deadline_ticks, self.dbc)
            self.dbc = (self.dbc + self.syt_interval) & 0xFF
            self.deadline_sub += self.step_num
        else:
            d = Decision(self.cycle, False, 0, None, None, self.dbc)
        self.cycle += 1
        return d

    def run(self, cycles: int) -> List[Decision]:
        return [self.next_cycle() for _ in range(cycles)]


# ---------------------------------------------------------------------------
# Generated C++ golden vector: 44.1 kHz / SYT_INTERVAL 8, zero seed
# ---------------------------------------------------------------------------

def _format_cpp_values(values: List[int], suffix: str, per_line: int) -> str:
    lines = []
    for begin in range(0, len(values), per_line):
        line = values[begin:begin + per_line]
        lines.append("    " + ", ".join(f"{value}{suffix}" for value in line) + ",")
    return "\n".join(lines)


def render_441_golden_header() -> str:
    """Returns the checked-in oracle for the detached C++ cadence primitive.

    This is intentionally the zero-deadline synthetic seed. It pins the exact
    rational arithmetic only; it does not specify Apple's startup lead or a
    future ASFW wire-visible seed policy.
    """
    decisions = RationalBlockingEngine.nominal(44100, 8).run(
        FAMILY_441_PERIOD_CYCLES)
    data = [d for d in decisions if d.is_data]
    assert len(data) == FAMILY_441_DATA_PER_PERIOD

    words = [0] * ((FAMILY_441_PERIOD_CYCLES + 63) // 64)
    for d in data:
        words[d.cycle // 64] |= 1 << (d.cycle % 64)

    offsets = [d.syt_offset_ticks for d in data[:SYT_DELTA_PERIOD_EVENTS]]
    assert all(offset is not None for offset in offsets)
    deltas = [data[index + 1].deadline_ticks_floor -
              data[index].deadline_ticks_floor
              for index in range(SYT_DELTA_PERIOD_EVENTS)]
    assert all(delta is not None for delta in deltas)

    words_cpp = _format_cpp_values(
        [int(f"0x{word:016X}", 16) for word in words], "ULL", 2)
    offsets_cpp = _format_cpp_values([int(offset) for offset in offsets], "u", 12)
    deltas_cpp = _format_cpp_values([int(delta) for delta in deltas], "u", 12)

    return f"""// Generated by tools/amdtp_blocking_cadence_sim.py --write-golden.
// Do not edit manually; use --verify-golden in CI or review.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ASFW::Test::Golden {{

inline constexpr std::uint32_t k441SampleRateHz = 44'100;
inline constexpr std::uint8_t k441SytInterval = 8;
inline constexpr std::size_t k441CadencePeriodCycles = 640;
inline constexpr std::size_t k441DataPacketsPerPeriod = 441;
inline constexpr std::size_t k441SytDeltaPeriodEvents = 147;

// Bit i is set when cycle i carries one 8-block AMDTP data packet.
inline constexpr std::array<std::uint64_t, {len(words)}> k441DataCycleBits = {{
{words_cpp}
}};

inline constexpr std::array<std::uint16_t, {len(offsets)}> k441SytOffsets = {{
{offsets_cpp}
}};

inline constexpr std::array<std::uint16_t, {len(deltas)}> k441SytDeltaTicks = {{
{deltas_cpp}
}};

[[nodiscard]] constexpr bool Is441DataCycle(std::size_t cycle) noexcept {{
    if (cycle >= k441CadencePeriodCycles) {{
        return false;
    }}
    return (k441DataCycleBits[cycle / 64] &
            (std::uint64_t{{1}} << (cycle % 64))) != 0;
}}

}} // namespace ASFW::Test::Golden
"""


def write_441_golden_header(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(render_441_golden_header(), encoding="ascii")


def verify_441_golden_header(path: Path) -> bool:
    if not path.is_file():
        return False
    return path.read_text(encoding="ascii") == render_441_golden_header()


class AppleNuDcl441:
    """Port of documentation/44100.md section 7 pseudocode (exact constants).

    Returns per cycle: (is_data, next_syt_phase_before_advance) so the
    deadline phase itself can be compared against the rational engine."""

    def __init__(self) -> None:
        self.packet_phase = 0
        self.next_syt = APPLE_NEXT_SYT_SEED

    def next_cycle(self) -> Tuple[bool, int]:
        self.packet_phase = (self.packet_phase + APPLE_CYCLE_PHASE) % APPLE_PHASE_MODULO
        lag = (self.packet_phase + APPLE_PHASE_MODULO - self.next_syt) % APPLE_PHASE_MODULO
        if lag != 0 and lag <= APPLE_HALF_RING:
            deadline = self.next_syt
            self.next_syt = (self.next_syt + APPLE_SYT_STEP_441) % APPLE_PHASE_MODULO
            return True, deadline
        return False, self.next_syt


# ---------------------------------------------------------------------------
# Check harness (colored, wrap-friendly output; respects NO_COLOR / --color)
# ---------------------------------------------------------------------------

FAILURES: List[str] = []
CHECKS_RUN = 0
_COLOR = False


def _c(code: str, s: str) -> str:
    return f"\x1b[{code}m{s}\x1b[0m" if _COLOR else s


def init_color(mode: str) -> None:
    global _COLOR
    if mode == "always":
        _COLOR = True
    elif mode == "never":
        _COLOR = False
    else:  # auto
        _COLOR = sys.stdout.isatty() and os.environ.get("NO_COLOR") is None


def check(name: str, ok: bool, detail: str) -> None:
    global CHECKS_RUN
    CHECKS_RUN += 1
    mark = _c("32", "PASS") if ok else _c("1;31", "FAIL")
    print(f"  {mark}  {_c('1', name) if not ok else name}")
    for line in textwrap.wrap(detail, width=86):
        print(_c("2", f"        {line}"))
    if not ok:
        FAILURES.append(name)


def section(title: str) -> None:
    print()
    line = f"── {title} " + "─" * max(3, 74 - len(title))
    print(_c("1;36", line))


# ---------------------------------------------------------------------------
# Checks: 44.1-family exact schedule
# ---------------------------------------------------------------------------

def check_441_schedule(decisions: List[Decision], rate: int,
                       syt_interval: int) -> None:
    data = [d for d in decisions if d.is_data]
    n_cycles = len(decisions)

    # Golden cadence prefix from the zero seed. Unlike 48k there is no short
    # repeat (full period = 640 cycles), so pin the first 32 decisions as the
    # golden value for the future C++ engine tests. 88.2/176.4 share this
    # exact sequence (see the family-identity section).
    #
    # REGRESSION ANCHOR ONLY -- the zero seed is NOT a wire contract. The
    # start phase is a wire-visible policy that must be derived from the bus
    # epoch or a capture (SAMPLE_RATE_EXPANSION.md section 8): Apple's
    # recovered seed opens N,N,N,D...; an accumulate-then-test accumulator
    # opens N,D,D.... All are rate-identical; they differ only in seed phase.
    prefix32 = "".join("D" if d.is_data else "N" for d in decisions[:32])
    golden32 = "DDDNDDNDDNDDNDDDNDDNDDNDDNDDNDDD"
    check(f"{rate}: cadence prefix, zero seed (engine regression anchor)",
          prefix32 == golden32,
          f"first 32 = {prefix32} (period 640, no short repeat; NOT a wire "
          "contract -- startup seed/lead policy is a separate, capture-gated "
          "deliverable)")

    # Sliding 640-cycle windows all carry exactly 441 data packets: 640
    # cycles equal exactly 441 SYT steps, so every window count is exact.
    prefix = [0]
    for d in decisions:
        prefix.append(prefix[-1] + (1 if d.is_data else 0))
    win = FAMILY_441_PERIOD_CYCLES
    counts = {prefix[i + win] - prefix[i] for i in range(n_cycles - win + 1)}
    check(f"{rate}: 441 data per any 640-cycle window",
          counts == {FAMILY_441_DATA_PER_PERIOD},
          f"window counts observed = {sorted(counts)} "
          f"(exhaustive, {n_cycles - win + 1} windows incl. final)")

    frames_per_period = FAMILY_441_DATA_PER_PERIOD * syt_interval
    expected_fps = rate * FAMILY_441_PERIOD_CYCLES // CYCLES_PER_SECOND
    check(f"{rate}: frames per 640 cycles",
          frames_per_period == expected_fps,
          f"{FAMILY_441_DATA_PER_PERIOD} x {syt_interval} = {frames_per_period}"
          f" (expected {expected_fps}; scales to {rate}/s)")

    # Full-domain SYT deltas: 4458/4459, exactly 34 long per any 147 events.
    deltas = [b.deadline_ticks_floor - a.deadline_ticks_floor
              for a, b in zip(data, data[1:])]
    dset = set(deltas)
    check(f"{rate}: SYT deltas in {{4458,4459}}", dset == {4458, 4459},
          f"observed = {sorted(dset)}")
    long_prefix = [0]
    for x in deltas:
        long_prefix.append(long_prefix[-1] + (1 if x == 4459 else 0))
    p = SYT_DELTA_PERIOD_EVENTS
    win34 = {long_prefix[i + p] - long_prefix[i]
             for i in range(len(deltas) - p + 1)}
    check(f"{rate}: 34x 4459 per any 147 events (Saffire section 9 / Linux)",
          win34 == {SYT_LONG_DELTAS_PER_PERIOD},
          f"exhaustive over {len(deltas) - p + 1} sliding windows incl. "
          f"final: counts = {sorted(win34)}")
    period_sum = sum(deltas[:SYT_DELTA_PERIOD_EVENTS])
    check(f"{rate}: 147-event period sum", period_sum == 655_360,
          f"sum = {period_sum} ticks (147 x 24576000x{syt_interval}/{rate})")

    # Mod-cycle increment pattern documented by Linux amdtp-stream.c:420-436:
    # successive data-packet syt_offsets differ by 1386 or 1387 (mod 3072).
    mod_deltas = {(b.syt_offset_ticks - a.syt_offset_ticks) % TICKS_PER_CYCLE
                  for a, b in zip(data, data[1:])}
    check(f"{rate}: mod-cycle SYT increments 1386/1387 (Linux :420-436)",
          mod_deltas == {1386, 1387}, f"observed = {sorted(mod_deltas)}")

    # Data spacing: gaps of 1 or 2 cycles only (never two no-data in a row);
    # per 441 consecutive gaps: 242 one-gaps + 199 two-gaps = 640 cycles.
    gaps = [b.cycle - a.cycle for a, b in zip(data, data[1:])]
    gset = set(gaps)
    check(f"{rate}: data gaps in {{1,2}} cycles", gset == {1, 2},
          f"observed = {sorted(gset)}")
    g = gaps[:FAMILY_441_DATA_PER_PERIOD]
    ones, twos = g.count(1), g.count(2)
    check(f"{rate}: per-period gap mix 242x1 + 199x2",
          (ones, twos) == (242, 199) and ones + 2 * twos == 640,
          f"ones = {ones}, twos = {twos}, cycles = {ones + 2 * twos}")


def check_dbc(decisions: List[Decision], syt_interval: int, label: str) -> None:
    ok = True
    prev_data_dbc = None
    for d in decisions:
        if d.is_data:
            if prev_data_dbc is not None and \
               d.dbc != (prev_data_dbc + syt_interval) & 0xFF:
                ok = False
                break
            prev_data_dbc = d.dbc
    check(f"{label}: DBC advances by {syt_interval} on data, held on no-data",
          ok, f"checked {len(decisions)} packets")


# ---------------------------------------------------------------------------
# Checks: 48k-family exact schedules (same engine, no special cases)
# ---------------------------------------------------------------------------

def check_48k_family(cycles: int) -> None:
    # rate, sytInterval, expected zero-seed prefix, constant SYT delta,
    # (ratio window cycles, data per window).
    cases = [
        (32000, 8, "DNDNDNDN", 6144, 2, 1),
        (48000, 8, "DDDNDDDN", 4096, 4, 3),
        (96000, 16, "DDDNDDDN", 4096, 4, 3),
        (192000, 32, "DDDNDDDN", 4096, 4, 3),
    ]
    for rate, interval, want_prefix, want_delta, win, want_data in cases:
        eng = RationalBlockingEngine.nominal(rate, interval)
        ds = eng.run(cycles)
        pattern = "".join("D" if d.is_data else "N" for d in ds[:8])
        check(f"{rate}: cadence {'/'.join(want_prefix[:4])} from zero seed",
              pattern == want_prefix, f"first 8 = {pattern}")
        data = [d for d in ds if d.is_data]
        deltas = {b.deadline_ticks_floor - a.deadline_ticks_floor
                  for a, b in zip(data, data[1:])}
        check(f"{rate}: constant SYT delta {want_delta} ticks",
              deltas == {want_delta}, f"observed = {sorted(deltas)}")
        prefix = [0]
        for d in ds:
            prefix.append(prefix[-1] + (1 if d.is_data else 0))
        counts = {prefix[i + win] - prefix[i]
                  for i in range(len(ds) - win + 1)}
        check(f"{rate}: {want_data} data per any {win}-cycle window",
              counts == {want_data},
              f"exhaustive incl. final window: {sorted(counts)}")


# ---------------------------------------------------------------------------
# Checks: packet rate -- one packet every 125 us cycle (8000 pps)
# ---------------------------------------------------------------------------

def check_packet_rate(cycles: int) -> None:
    """IEC 61883-6 blocking mode never skips a bus cycle: every cycle carries
    exactly one packet (data, or CIP-header-only no-data), so the isoch
    stream is always 8000 packets/s at every sample rate; the rate only moves
    the data/no-data split. 1 s windows are exact for the 48k family but not
    the 44.1 family (8000 cycles = 12.5 periods of 640); 2 s windows
    (16000 cycles = 25 periods) are exact for both. The driver-side analogue
    is the txPackets counter advancing 1:1 with the elapsed cycle timer."""
    cases = {
        (32000, 8): ({4000}, 8000),
        (44100, 8): ({5512, 5513}, 11025),
        (48000, 8): ({6000}, 12000),
        (88200, 16): ({5512, 5513}, 11025),
        (96000, 16): ({6000}, 12000),
        (176400, 32): ({5512, 5513}, 11025),
        (192000, 32): ({6000}, 12000),
    }
    # Exact-total run length: multiple of lcm(640-cycle period, 8000) = 16000.
    n = max(16_000, (cycles // 16_000) * 16_000)
    one = CYCLES_PER_SECOND
    two = 2 * CYCLES_PER_SECOND
    for (rate, interval), (want_1s, want_2s) in cases.items():
        eng = RationalBlockingEngine.nominal(rate, interval)
        ds = eng.run(n)
        data_pps = rate / interval
        ok_seq = len(ds) == n and all(d.cycle == i for i, d in enumerate(ds))
        check(f"{rate}: one packet per 125 us cycle (8000 pps)",
              ok_seq,
              f"{len(ds)} packets in {n} cycles, no skipped/doubled cycles "
              f"({data_pps:.1f} data + {8000 - data_pps:.1f} no-data pps)")

        pre_d = [0]
        pre_f = [0]
        for d in ds:
            pre_d.append(pre_d[-1] + (1 if d.is_data else 0))
            pre_f.append(pre_f[-1] + d.frames)

        data_1s = {pre_d[i + one] - pre_d[i] for i in range(n - one + 1)}
        check(f"{rate}: data packets per any 1 s window = {sorted(want_1s)}",
              data_1s == want_1s,
              f"observed = {sorted(data_1s)} (exhaustive incl. final window)")

        frames_1s = {pre_f[i + one] - pre_f[i] for i in range(n - one + 1)}
        want_frames_1s = {c * interval for c in want_1s}
        check(f"{rate}: frames per any 1 s window = {sorted(want_frames_1s)}",
              frames_1s == want_frames_1s,
              f"observed = {sorted(frames_1s)} (mean = {rate})")

        data_2s = {pre_d[i + two] - pre_d[i] for i in range(n - two + 1)}
        check(f"{rate}: exact 2 s accounting (any 16000-cycle window)",
              data_2s == {want_2s} and want_2s * interval == 2 * rate,
              f"data per 16000 cycles = {sorted(data_2s)}; "
              f"{want_2s} x {interval} frames = {want_2s * interval} "
              f"= 2 x {rate}")

        total_frames = pre_f[-1]
        want_total = n * rate // CYCLES_PER_SECOND
        check(f"{rate}: long-run frame total exact",
              total_frames == want_total,
              f"{total_frames} frames in {n} cycles "
              f"({n // CYCLES_PER_SECOND} s) == {want_total}")


# ---------------------------------------------------------------------------
# Checks: ZTS anchor observability and cadence (CoreAudio zero timestamps)
# ---------------------------------------------------------------------------

# Active HAL buffer profile "dice-working-1536" (AudioHalBufferProfiles.hpp).
ZTS_PERIOD_FRAMES = 1536


def check_zts(cycles: int) -> None:
    """Models IsochReceiveContext ZTS anchor publication: an anchor is
    published when a DATA packet's *first* audio frame lands exactly on a
    kHalZeroTimestampPeriodFrames boundary. The anchor pair is
    (countedFrame, packet time) -- no rate constant participates;
    nanosPerSampleQ8 = (1e9 << 8) / sampleRateHz is computed live
    (IsochReceiveContext.cpp:558). Verifies at every rate: 100% boundary
    observability (needs ZTS period % framesPerDataPacket == 0), anchor
    spacing, frames<->bus-time slope bounded by one cycle, Q8 quantization
    under 0.2 ppm -- and demonstrates the anchor-thinning failure mode a bad
    period would cause (the static-assert residual in
    SAMPLE_RATE_EXPANSION.md section 6)."""
    cases = [(32000, 8), (44100, 8), (48000, 8), (88200, 16), (96000, 16),
             (176400, 32), (192000, 32)]
    for rate, interval in cases:
        eng = RationalBlockingEngine.nominal(rate, interval)
        ds = eng.run(cycles)
        frame = 0
        anchors: List[Tuple[int, int]] = []  # (sampleFrame, cycle)
        for d in ds:
            if d.is_data:
                if frame % ZTS_PERIOD_FRAMES == 0:
                    anchors.append((frame, d.cycle))
                frame += interval
        want_count = ((frame - interval) // ZTS_PERIOD_FRAMES) + 1
        check(f"{rate}: every {ZTS_PERIOD_FRAMES}-frame ZTS boundary observed",
              len(anchors) == want_count,
              f"{len(anchors)} anchors == {want_count} boundaries "
              f"({ZTS_PERIOD_FRAMES} % {interval} == 0)")

        spacings = {b[1] - a[1] for a, b in zip(anchors, anchors[1:])}
        num = ZTS_PERIOD_FRAMES * CYCLES_PER_SECOND
        lo = num // rate
        want_sp = {lo} if num % rate == 0 else {lo, lo + 1}
        check(f"{rate}: anchor spacing {sorted(want_sp)} cycles",
              spacings == want_sp,
              f"observed = {sorted(spacings)} "
              f"(~{ZTS_PERIOD_FRAMES * 1000 / rate:.2f} ms nominal)")

        # frames<->bus-time slope: anchors quantize to cycle starts, so the
        # accumulated deviation from the exact rate must stay under 1 cycle.
        (f0, c0), (fn, cn) = anchors[0], anchors[-1]
        lhs = (fn - f0) * TICKS_PER_SECOND
        rhs = (cn - c0) * TICKS_PER_CYCLE * rate
        check(f"{rate}: anchor slope == sample rate within one cycle",
              abs(lhs - rhs) <= TICKS_PER_CYCLE * rate,
              f"|error| = {abs(lhs - rhs) / (TICKS_PER_CYCLE * rate):.3f} "
              f"cycles across {len(anchors)} anchors")

        q8 = (10**9 << 8) // rate
        exact = Fraction(10**9 << 8, rate)
        err_ppm = float((exact - q8) / exact) * 1e6
        # Floor quantization grows with rate: 0.25 ppm at 96k/192k is the
        # true production value ((1e9 << 8) / rate integer division).
        check(f"{rate}: nanosPerSampleQ8 quantization < 0.3 ppm",
              0.0 <= err_ppm < 0.3,
              f"Q8 = {q8} ({q8 / 256:.3f} ns/sample), err = {err_ppm:.3f} ppm")

    # Failure-mode demo: a ZTS period that is not a multiple of
    # frames-per-packet (1544 at 176.4k/32) puts 3 of 4 boundaries mid-packet
    # where they are never a packet-first frame: anchors silently thin 4x.
    bad_period = 1544
    eng = RationalBlockingEngine.nominal(176400, 32)
    ds = eng.run(cycles)
    frame = 0
    hits = 0
    for d in ds:
        if d.is_data:
            if frame % bad_period == 0:
                hits += 1
            frame += 32
    boundaries = ((frame - 32) // bad_period) + 1
    frac = hits / boundaries
    check("counterexample: ZTS period 1544 @ 176.4k observes ~1/4 boundaries",
          0.2 < frac < 0.3,
          f"{hits}/{boundaries} boundaries observable ({frac:.1%}) -> "
          "zeroTimestampPeriodFrames % framesPerDataPacket == 0 needs a "
          "static assert")


# ---------------------------------------------------------------------------
# Checks: family identity (88.2/176.4 share the 44.1 deadline sequence)
# ---------------------------------------------------------------------------

def check_family_identity(cycles: int) -> None:
    families = {
        "44.1": ((44100, 8), (88200, 16), (176400, 32), 5512.5),
        "48": ((48000, 8), (96000, 16), (192000, 32), 6000.0),
    }
    for name, (base, x2, x4, event_rate) in families.items():
        seqs = {}
        for rate, interval in (base, x2, x4):
            eng = RationalBlockingEngine.nominal(rate, interval)
            seqs[rate] = eng.run(cycles)
        base_rate = base[0]
        others = (x2[0], x4[0])
        dn_equal = all(
            [d.is_data for d in seqs[base_rate]] ==
            [d.is_data for d in seqs[r]] for r in others)
        off_equal = all(
            [d.syt_offset_ticks for d in seqs[base_rate]] ==
            [d.syt_offset_ticks for d in seqs[r]] for r in others)
        check(f"family {name}: {x2[0]}/{x2[1]} and {x4[0]}/{x4[1]} emit the "
              f"{base_rate}/{base[1]} D/N sequence",
              dn_equal, f"{cycles} cycles compared element-wise")
        check(f"family {name}: identical SYT deadline sequence at 1x/2x/4x",
              off_equal,
              f"event rate {event_rate}/s is family-invariant; only "
              "frames-per-packet scales")


# ---------------------------------------------------------------------------
# Checks: 6-cycle interrupt-group frame bounds (AudioTimingGeometry)
# ---------------------------------------------------------------------------

def check_group_windows(cycles: int) -> None:
    # Note: 2x/4x bounds are tier-uniform across BOTH families (event counts
    # per 6-cycle window are {4,5} at 5512.5/s and 6000/s alike), so per-tier
    # kMin/MaxNominalFramesPerInterrupt values cover 44.1 and 48 together.
    expect = {
        (32000, 8): {24},  # exact: 6 cycles = 3 events x 8 frames
        (44100, 8): {32, 40},
        (48000, 8): {32, 40},
        (88200, 16): {64, 80},
        (96000, 16): {64, 80},
        (176400, 32): {128, 160},
        (192000, 32): {128, 160},
    }
    for (rate, interval), want in expect.items():
        eng = RationalBlockingEngine.nominal(rate, interval)
        ds = eng.run(cycles)
        frames = [d.frames for d in ds]
        prefix = [0]
        for f in frames:
            prefix.append(prefix[-1] + f)
        got = {prefix[i + 6] - prefix[i] for i in range(len(frames) - 6 + 1)}
        check(f"{rate}: frames per 6-cycle window = {sorted(want)}",
              got == want,
              f"exhaustive over {len(frames) - 5} window phases incl. final: "
              f"{sorted(got)}")


# ---------------------------------------------------------------------------
# Checks: ring-wrap phase (why the cadence must be stateful)
# ---------------------------------------------------------------------------

def check_ring_wrap() -> None:
    period_441 = FAMILY_441_PERIOD_CYCLES
    period_48k = 4
    check("ring wrap: 48k period divides the 512-slot ring",
          TIMELINE_SLOTS % period_48k == 0,
          f"{TIMELINE_SLOTS} % {period_48k} == 0 "
          "(slot-index-derived phase was possible at 48k)")
    check("ring wrap: 44.1 period does NOT divide the 512-slot ring",
          TIMELINE_SLOTS % period_441 != 0,
          f"{TIMELINE_SLOTS} % {period_441} = {TIMELINE_SLOTS % period_441}; "
          "cadence state must live in the engine, not the slot index "
          "(SAMPLE_RATE_EXPANSION.md section 6, item 4)")


# ---------------------------------------------------------------------------
# Checks: Apple NuDCL model vs rational engine
# ---------------------------------------------------------------------------

def check_apple_model(cycles: int) -> None:
    apple = AppleNuDcl441()
    apple_dn: List[bool] = []
    apple_deadlines: List[int] = []  # phase units at each data decision
    for _ in range(cycles):
        is_data, deadline = apple.next_cycle()
        apple_dn.append(is_data)
        if is_data:
            apple_deadlines.append(deadline)

    prefix = "".join("D" if x else "N" for x in apple_dn[:16])
    check("Apple: startup prefix (44100.md section 7)",
          prefix == APPLE_STARTUP_PREFIX,
          f"first 16 = {prefix}")
    first_window = sum(1 for x in apple_dn[:FAMILY_441_PERIOD_CYCLES] if x)
    check("Apple: 439 data packets in first 640-cycle startup window",
          first_window == APPLE_STARTUP_WINDOW_DATA,
          f"count = {first_window}")
    steady = sum(1 for x in apple_dn[FAMILY_441_PERIOD_CYCLES:
                                     11 * FAMILY_441_PERIOD_CYCLES] if x)
    check("Apple: steady 441/640 after the startup lead is absorbed",
          steady == 10 * FAMILY_441_DATA_PER_PERIOD,
          f"{steady} data in the next 10 periods")

    # Rational engine seeded with the same deadline (11,722 ticks exactly).
    rat = RationalBlockingEngine.nominal(44100, 8,
                                         seed_ticks=APPLE_NEXT_SYT_SEED // APPLE_SCALE)
    rat_dn: List[bool] = []
    rat_deadline_sub: List[int] = []  # subticks (ticks x 44100) per data event
    sub = rat.deadline_sub
    for _ in range(cycles):
        d = rat.next_cycle()
        rat_dn.append(d.is_data)
        if d.is_data:
            rat_deadline_sub.append(sub)
            sub += rat.step_num
    first_div = next((i for i, (a, b) in enumerate(zip(apple_dn, rat_dn))
                      if a != b), None)

    # The two streams cannot agree forever: Apple's step is short by
    # deficit_num/44100 phase units per event, so its deadlines precess
    # earlier until one crosses a cycle edge. Closed-form prediction of the
    # first flip: smallest n where the rational deadline's offset into its
    # cycle (in phase units) is less than n * deficit. At that point Apple
    # emits the event one cycle early, so the observed divergence cycle is
    # (rational event cycle - 1).
    seed_sub = (APPLE_NEXT_SYT_SEED // APPLE_SCALE) * 44100
    step_sub = TICKS_PER_SECOND * 8
    cycle_sub = TICKS_PER_CYCLE * 44100
    deficit_num = TICKS_PER_SECOND * 8 * APPLE_SCALE - APPLE_SYT_STEP_441 * 44100
    predicted = None
    for n in range(1, len(rat_deadline_sub)):
        offset_sub = (seed_sub + n * step_sub) % cycle_sub
        if offset_sub * APPLE_SCALE < n * deficit_num:
            predicted = (seed_sub + n * step_sub) // cycle_sub - 1
            break
    check("Apple vs rational: first D/N flip exactly where the step "
          "deficit predicts",
          first_div is not None and first_div == predicted,
          f"measured cycle {first_div}, predicted {predicted} "
          f"(~{(first_div or 0) / CYCLES_PER_SECOND:.2f} s of bus time; "
          "identical before that; Apple then precesses ~0.02 ppm fast "
          "forever while the rational engine stays exact)")

    # Exact drift identity. Apple's step is short of the exact value by
    # (24576000*8/44100)*10^4 - 44,582,312 phase units per event; verify the
    # accumulated phase difference matches that Fraction exactly.
    exact_step_units = Fraction(TICKS_PER_SECOND * 8 * APPLE_SCALE, 44100)
    per_event_drift = exact_step_units - APPLE_SYT_STEP_441
    n_events = min(len(apple_deadlines), len(rat_deadline_sub))
    ok_exact = True
    for n in (1000, 10_000, n_events - 1):
        rat_units = Fraction(rat_deadline_sub[n] * APPLE_SCALE, 44100)
        diff = (rat_units - apple_deadlines[n]) % APPLE_PHASE_MODULO
        if diff > APPLE_HALF_RING:
            diff -= APPLE_PHASE_MODULO
        if diff != (per_event_drift * n) % APPLE_PHASE_MODULO:
            ok_exact = False
            break
    drift_ppm = float(per_event_drift / exact_step_units) * 1e6
    ticks_per_hour = float(per_event_drift) / APPLE_SCALE * 5512.5 * 3600
    check("Apple: fixed-point drift == exact-step deficit (Fraction identity)",
          ok_exact,
          f"deficit = {float(per_event_drift):.6f} units/event "
          f"=> Apple runs {drift_ppm:.4f} ppm fast, "
          f"{ticks_per_hour:.0f} ticks/hour ({ticks_per_hour / 24.576:.1f} us/h); "
          "rational engine is exact")


# ---------------------------------------------------------------------------
# Ports of PRODUCTION replay/timing math (ASFW's own code, re-expressed in
# Python so the replay trial exercises the real functions, not just ring
# preservation):
#   compute_replay_syt_offset / compute_replay_syt / _from_ticks:
#     ASFWDriver/Audio/Wire/AMDTP/RxSequenceReplay.hpp:26-93
#   cycle-timer fields (sec[31:25], cycle[24:12], offset[11:0]) and the
#   16-cycle SYT extension: Common/TimingUtils.hpp + AmdtpTiming.hpp:36-44
# ---------------------------------------------------------------------------

SYT_NO_INFO16 = 0xFFFF
REPLAY_NO_INFO = (1 << 32) - 1  # RxSequenceReplayState::kNoInfo


def encode_cycle_timer(seconds: int, cycle: int, offset: int) -> int:
    return ((seconds & 0x7F) << 25) | ((cycle & 0x1FFF) << 12) | (offset & 0xFFF)


def cycle_timer_cycle(cycle_timer: int) -> int:
    return (cycle_timer >> 12) & 0x1FFF


def compute_replay_syt_offset(syt: int, source_cycle_timer: int,
                              transfer_delay_ticks: int) -> int:
    # Port of ComputeReplaySytOffset (RxSequenceReplay.hpp:26-53).
    if syt == SYT_NO_INFO16:
        return REPLAY_NO_INFO
    source_cycle_low = cycle_timer_cycle(source_cycle_timer) & 0xF
    syt_cycle_low = (syt >> 12) & 0xF
    if syt_cycle_low < source_cycle_low:
        syt_cycle_low += 16
    offset = (syt_cycle_low - source_cycle_low) * TICKS_PER_CYCLE + (syt & 0xFFF)
    if offset < transfer_delay_ticks:
        offset += 16 * TICKS_PER_CYCLE  # the +16-cycle guard branch
    return offset - transfer_delay_ticks


def compute_replay_syt(syt_offset: int, output_cycle_timer: int,
                       transfer_delay_ticks: int) -> int:
    # Port of ComputeReplaySyt (RxSequenceReplay.hpp:55-73).
    if syt_offset == REPLAY_NO_INFO:
        return SYT_NO_INFO16
    output_cycle = cycle_timer_cycle(output_cycle_timer)
    presentation = syt_offset + transfer_delay_ticks
    cycle = output_cycle + presentation // TICKS_PER_CYCLE
    return ((cycle & 0xF) << 12) | (presentation % TICKS_PER_CYCLE)


def compute_replay_syt_from_ticks(syt_offset: int, output_cycle_ticks: int,
                                  transfer_delay_ticks: int) -> int:
    # Port of ComputeReplaySytFromTicks (RxSequenceReplay.hpp:75-93).
    normalized = output_cycle_ticks % (8 * TICKS_PER_SECOND)
    seconds = normalized // TICKS_PER_SECOND
    within = normalized % TICKS_PER_SECOND
    return compute_replay_syt(
        syt_offset,
        encode_cycle_timer(seconds, within // TICKS_PER_CYCLE,
                           within % TICKS_PER_CYCLE),
        transfer_delay_ticks)


def syt_ticks_after_cycle(syt: int, base_cycle: int) -> int:
    # Relative form of extendTstamp (AmdtpTiming.hpp:36-44): presentation
    # ticks after the start of base_cycle, resolved on the 16-cycle ring.
    d = (((syt >> 12) & 0xF) - (base_cycle & 0xF)) % 16
    return d * TICKS_PER_CYCLE + (syt & 0xFFF)


# ---------------------------------------------------------------------------
# Checks: replay path (device-clocked cadence through the production math)
# ---------------------------------------------------------------------------

def make_device_engine(rate: int, syt_interval: int, ppm: int,
                       seed_ticks: int) -> RationalBlockingEngine:
    # Device oscillator at (1 + ppm*1e-6): event spacing shrinks by that
    # factor. Exact: step = 24576000*interval*1e6 / (rate*(1e6+ppm)) ticks.
    num = TICKS_PER_SECOND * syt_interval * 1_000_000
    den = rate * (1_000_000 + ppm)
    return RationalBlockingEngine(num, den, syt_interval, seed_ticks * den)


def run_replay_trial(cycles: int, ppm: int, phase: int,
                     device_lead_ticks: int, rx_delay: int, tx_delay: int,
                     rate: int = 44100, interval: int = 8):
    """One duplex trial through the production math. The device presents
    frames device_lead_ticks after its packet cycle; RX reconstructs the
    delay-free offset via compute_replay_syt_offset; the 512/256 ring
    replays it; TX re-anchors via compute_replay_syt_from_ticks. Returns
    per-packet records for invariant checks."""
    dev = make_device_engine(rate, interval, ppm, phase)
    dev_ds = dev.run(cycles)
    ring_size, read_delay = 512, 256
    ring: List[Optional[Tuple[int, int, int]]] = [None] * ring_size
    tx_packets = 0
    tx_frames = 0
    carries = 0
    records = []  # (dev_pres_abs, tx_pres_abs)
    for c, d in enumerate(dev_ds):
        if d.is_data:
            pres_abs = d.cycle * TICKS_PER_CYCLE + d.syt_offset_ticks + \
                device_lead_ticks
            syt16 = (((pres_abs // TICKS_PER_CYCLE) & 0xF) << 12) | \
                (pres_abs % TICKS_PER_CYCLE)
            src_ct = encode_cycle_timer((d.cycle // CYCLES_PER_SECOND) & 0x7F,
                                        d.cycle % CYCLES_PER_SECOND, 0)
            off = compute_replay_syt_offset(syt16, src_ct, rx_delay)
            raw = pres_abs - d.cycle * TICKS_PER_CYCLE
            # Count guard-branch hits only for packets TX will consume: the
            # final read_delay cycles are published but never replayed.
            if raw < rx_delay and c < cycles - read_delay:
                carries += 1
            ring[c % ring_size] = (d.frames, off, pres_abs)
        else:
            ring[c % ring_size] = (0, REPLAY_NO_INFO, 0)
        if c < read_delay:
            continue
        entry = ring[(c - read_delay) % ring_size]
        assert entry is not None
        tx_packets += 1
        frames, off, dev_pres = entry
        tx_frames += frames
        if frames:
            out_ticks = c * TICKS_PER_CYCLE
            tx_syt = compute_replay_syt_from_ticks(off, out_ticks, tx_delay)
            rel = syt_ticks_after_cycle(tx_syt, c % CYCLES_PER_SECOND)
            tx_pres = out_ticks + rel
            records.append((dev_pres, tx_pres))
    dev_window = dev_ds[:cycles - read_delay]
    dev_frames = sum(d.frames for d in dev_window)
    return records, tx_packets, tx_frames, dev_frames, carries


def check_replay(cycles: int, seed: int) -> None:
    rng = random.Random(seed)
    ppms = [-500, -100, 0, 100, 500] + [rng.randint(-300, 300) for _ in range(3)]
    read_delay = 256
    rx_delay = tx_delay = 12800  # AudioTransportControlBlock.hpp:158-159
    lead = 5 * TICKS_PER_CYCLE + 1000  # device presents ~5 cycles ahead
    for ppm in ppms:
        phase = rng.randint(0, TICKS_PER_CYCLE - 1)
        records, tx_packets, tx_frames, dev_frames, carries = \
            run_replay_trial(cycles, ppm, phase, lead, rx_delay, tx_delay)

        ok_pps = tx_packets == cycles - read_delay
        ok_lock = tx_frames == dev_frames
        # End-to-end presentation lag through the production math must be the
        # ring delay plus the delay asymmetry, exactly, on every packet.
        want_lag = read_delay * TICKS_PER_CYCLE + (tx_delay - rx_delay)
        lags = {tx - dev for dev, tx in records}
        ok_lag = lags == {want_lag}
        # The device's realized SYT-delta pattern must survive the 16-bit
        # field encode -> ComputeReplaySytOffset -> ring -> re-anchor chain.
        dev_deltas = [b[0] - a[0] for a, b in zip(records, records[1:])]
        tx_deltas = [b[1] - a[1] for a, b in zip(records, records[1:])]
        ok_pattern = tx_deltas == dev_deltas
        hist = dict(sorted(Counter(tx_deltas).items()))
        check(f"replay @ {ppm:+d} ppm: production SYT math round-trip",
              ok_pps and ok_lock and ok_lag and ok_pattern and carries == 0,
              f"txPackets = {tx_packets} (1/cycle, 8000 pps after "
              f"{read_delay}-cycle warmup); txFrames = {tx_frames} == "
              f"deviceFrames = {dev_frames}; presentation lag constant = "
              f"{want_lag} ticks on all {len(records)} packets; "
              f"delta histogram = {hist}")

    # Cross-family trials: the replay math is family-invariant; prove it at
    # one 48-family and one 4x 44.1-family rate through the same round trip.
    for rate, interval, ppm2 in ((96000, 16, 100), (176400, 32, -100)):
        records, tx_packets, tx_frames, dev_frames, carries = \
            run_replay_trial(cycles, ppm2, 555, lead, rx_delay, tx_delay,
                             rate, interval)
        want_lag = read_delay * TICKS_PER_CYCLE + (tx_delay - rx_delay)
        lags = {tx - dev for dev, tx in records}
        dev_deltas = [b[0] - a[0] for a, b in zip(records, records[1:])]
        tx_deltas = [b[1] - a[1] for a, b in zip(records, records[1:])]
        hist = dict(sorted(Counter(tx_deltas).items()))
        check(f"replay @ {rate} {ppm2:+d} ppm: family-invariant round-trip",
              tx_packets == cycles - read_delay and tx_frames == dev_frames
              and lags == {want_lag} and tx_deltas == dev_deltas
              and carries == 0,
              f"txFrames = {tx_frames} == deviceFrames; lag constant = "
              f"{want_lag} ticks on all {len(records)} packets; "
              f"delta histogram = {hist}")

    # Carry-branch trial: device lead (2 cycles) below rxTransferDelay makes
    # ComputeReplaySytOffset take the +16-cycle guard on every packet; a
    # smaller txTransferDelay keeps the re-encoded SYT inside the 16-cycle
    # horizon. The lag constant then includes the 49152-tick guard exactly.
    records, tx_packets, tx_frames, dev_frames, carries = \
        run_replay_trial(cycles, 100, 777, 2 * TICKS_PER_CYCLE, 12800, 3072)
    want_lag = read_delay * TICKS_PER_CYCLE + (3072 - 12800) + \
        16 * TICKS_PER_CYCLE
    lags = {tx - dev for dev, tx in records}
    dev_deltas = [b[0] - a[0] for a, b in zip(records, records[1:])]
    tx_deltas = [b[1] - a[1] for a, b in zip(records, records[1:])]
    check("replay carry branch: +16-cycle guard exercised on 100% of packets",
          carries == len(records) and len(records) > 0 and
          lags == {want_lag} and tx_deltas == dev_deltas,
          f"{carries}/{len(records)} packets took the guard "
          f"(RxSequenceReplay.hpp:48-51); lag constant = {want_lag} ticks "
          "includes the 49152-tick guard; delta pattern still exact")


# ---------------------------------------------------------------------------
# Checks: seed independence (any start phase preserves all invariants)
# ---------------------------------------------------------------------------

def check_seed_independence(cycles: int, seed: int) -> None:
    rng = random.Random(seed)
    ok = True
    tried = []
    for _ in range(8):
        phase = rng.randint(0, 2 * TICKS_PER_CYCLE)
        tried.append(phase)
        eng = RationalBlockingEngine.nominal(44100, 8, seed_ticks=phase)
        ds = eng.run(cycles)
        prefix = [0]
        for d in ds:
            prefix.append(prefix[-1] + (1 if d.is_data else 0))
        win = FAMILY_441_PERIOD_CYCLES
        counts = {prefix[i + win] - prefix[i]
                  for i in range(len(ds) - win + 1)}
        if counts != {FAMILY_441_DATA_PER_PERIOD}:
            ok = False
            break
    check("44100: 441/640 invariant holds from arbitrary seed phases",
          ok, f"seed ticks tried = {tried} (exhaustive windows per seed)")


# ---------------------------------------------------------------------------
# Production wiring status
# ---------------------------------------------------------------------------

def print_wiring_status() -> None:
    """This sim validates MATH ONLY. Print the outstanding production work so
    a green run is never mistaken for 44.1 being enabled in the driver."""
    section("Production wiring status -- this sim does NOT enable 44.1")
    items = [
        ("AmdtpTxPacketizer.cpp:43",
         "Configure() still rejects sampleRate != 48000; accept the 44.1 "
         "family and select cadence from the rational engine validated "
         "here."),
        ("ASFWAudioDriverZts.cpp:349-358",
         "frame projection divides by kTicksPerSample48k (512); replace "
         "with frames = deltaTicks * rate / 24576000 in u64 (44.1 has no "
         "integer ticks/sample)."),
        ("RxSytCadence.hpp:19-20",
         "bootstrap step is the 48k constant (4096); use "
         "round(24576000 * sytInterval / rate) -> 4458 at 44.1."),
        ("AudioTimingGeometry.hpp:85-124",
         "frame<->packet budgets assume 6 frames/cycle; at 44.1 (441/80): "
         "exposure lead 96 -> 105 pkt, window 192 -> 210, prep lead "
         "336 -> 354, shared slots 384 -> 402 (fits 512 timeline slots)."),
        ("DICETcatProtocol.cpp:240-249 + IDeviceProtocol.hpp:63-83",
         "bring-up is hardwired PrepareDuplex48k/kDiceClockSelect48kInternal;"
         " parameterize by DICE rate code and read GLOBAL_CLOCK_CAPS (0x64);"
         " wire HAL PerformDeviceConfigurationChange -> duplex restart."),
        ("AudioHalBufferProfiles.hpp",
         "add static assert zeroTimestampPeriodFrames % framesPerDataPacket"
         " == 0 (see the 1544-period counterexample above)."),
        ("startup seed / lead / reset policy",
         "capture-gated (SAMPLE_RATE_EXPANSION.md section 8); the zero-seed "
         "prefix asserted here is an engine regression anchor, not the wire "
         "contract."),
    ]
    for where, what in items:
        print(f"  {_c('33', 'TODO')}  {_c('1', where)}")
        for line in textwrap.wrap(what, width=86):
            print(_c("2", f"        {line}"))


# ---------------------------------------------------------------------------
# Headline figures
# ---------------------------------------------------------------------------

def print_headline_summary() -> None:
    section("Headline figures (informational)")
    header = (f"  {'rate':>7}  {'pps':>5}  {'data pps':>9}  {'no-data':>8}  "
              f"{'f/pkt':>5}  {'ZTS anchor spacing':<26}  {'ns/sample':>10}")
    print(_c("1", header))
    for rate, interval in [(32000, 8), (44100, 8), (48000, 8), (88200, 16),
                           (96000, 16), (176400, 32), (192000, 32)]:
        data_pps = rate / interval
        num = ZTS_PERIOD_FRAMES * CYCLES_PER_SECOND
        lo = num // rate
        cyc = f"{lo}" if num % rate == 0 else f"{lo}-{lo + 1}"
        anchor = f"{cyc} cyc ({ZTS_PERIOD_FRAMES * 1000 / rate:.2f} ms)"
        ns = ((10**9 << 8) // rate) / 256
        print(f"  {rate:>7}  {8000:>5}  {data_pps:>9.1f}  "
              f"{8000 - data_pps:>8.1f}  {interval:>5}  {anchor:<26}  "
              f"{ns:>10.3f}")
    print(_c("2", "        pps is rate-invariant (one packet per 125 us "
                  "cycle); ZTS anchors use the 1536-frame profile"))


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("--cycles", type=int, default=256_000,
                    help="main simulation length in 125 us cycles "
                         "(default 256000 = 32 s of bus time)")
    ap.add_argument("--apple-cycles", type=int, default=1_280_000,
                    help="cycle count for the Apple-vs-rational comparison")
    ap.add_argument("--seed", type=int, default=1394,
                    help="PRNG seed for phase/ppm trials")
    ap.add_argument("--color", choices=("auto", "always", "never"),
                    default="auto",
                    help="colorize output (default: auto = TTY and no "
                         "NO_COLOR env)")
    golden_group = ap.add_mutually_exclusive_group()
    golden_group.add_argument("--write-golden", type=Path,
                              metavar="PATH",
                              help="write the deterministic 44.1 C++ golden "
                                   "header to PATH")
    golden_group.add_argument("--verify-golden", type=Path,
                              metavar="PATH",
                              help="fail when PATH differs from the "
                                   "deterministic 44.1 C++ golden header")
    args = ap.parse_args()
    init_color(args.color)

    if args.write_golden is not None:
        write_441_golden_header(args.write_golden)
        print(f"Wrote 44.1 C++ golden header: {args.write_golden}")
    elif args.verify_golden is not None:
        section("Generated C++ golden data")
        check("44.1 C++ golden header is current",
              verify_441_golden_header(args.verify_golden),
              f"{args.verify_golden} matches the zero-seed rational engine")

    section("44.1 kHz blocking schedule (rational engine, zero seed)")
    eng = RationalBlockingEngine.nominal(44100, 8)
    ds = eng.run(args.cycles)
    check_441_schedule(ds, 44100, 8)
    check_dbc(ds, 8, "44100")

    section("48k-family schedules: 32/48/96/192 (same engine, no special "
            "cases)")
    check_48k_family(min(args.cycles, 64_000))

    section("Packet rate: one packet per 125 us cycle (8000 pps, all rates)")
    check_packet_rate(min(args.cycles, 64_000))

    section("ZTS anchors: observability, spacing, slope (CoreAudio clock)")
    check_zts(min(args.cycles, 64_000))

    section("Family identity: 88.2/176.4 mirror 44.1; 96/192 mirror 48")
    check_family_identity(min(args.cycles, 64_000))
    eng882 = RationalBlockingEngine.nominal(88200, 16)
    check_dbc(eng882.run(min(args.cycles, 64_000)), 16, "88200")
    eng192 = RationalBlockingEngine.nominal(192000, 32)
    check_dbc(eng192.run(min(args.cycles, 64_000)), 32, "192000")

    section("6-cycle interrupt-group frame bounds (AudioTimingGeometry)")
    check_group_windows(min(args.cycles, 64_000))

    section("Ring-wrap phase alignment")
    check_ring_wrap()

    section("Apple NuDCL fixed-point model vs rational engine")
    check_apple_model(args.apple_cycles)

    section("Seed independence")
    check_seed_independence(min(args.cycles, 128_000), args.seed)

    section("Replay path: device-clocked cadence replayed 1:1 (+/- ppm)")
    check_replay(min(args.cycles, 64_000), args.seed)

    print_wiring_status()
    print_headline_summary()

    print()
    if FAILURES:
        print(_c("1;31",
                 f"RESULT: {len(FAILURES)}/{CHECKS_RUN} checks FAILED: "
                 f"{FAILURES}"))
        return 1
    print(_c("1;32", f"RESULT: all {CHECKS_RUN} checks passed"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
