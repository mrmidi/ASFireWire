#!/usr/bin/env python3
"""
ASFW Saffire-shaped TX simulator.

Forward simulator for the proposed ASFW DICE TX refactor.

It is NOT the old ASFW model:
  - no free-running TX audio timeline as the source of audio truth
  - no huge txOutputOffsetFrames_
  - no epoch
  - no PendingSource
  - no packet generation checks

Core model:
  CoreAudio write side:
      reported playhead -> write-ahead target -> TxPcmPacketRing.populate([begin,end))

  Isoch send side:
      recovered device phase -> TxOutputPhaseLoop -> DATA/NO-DATA
      DATA: outputPhaseTicks -> absolute audio frame -> packet slot -> coverage check
      NO-DATA: CIP-only in real driver terms, SYT=0xffff, no payload consumed

Typical usage:
  python3 tools/debug/asfw_saffire_tx_sim.py
  python3 tools/debug/asfw_saffire_tx_sim.py --show 12
  python3 tools/debug/asfw_saffire_tx_sim.py --drift-ppm 100
  python3 tools/debug/asfw_saffire_tx_sim.py --glitch-at 5000 --glitch-jump 5000
  python3 tools/debug/asfw_saffire_tx_sim.py --write-ahead-frames 16
  python3 tools/debug/asfw_saffire_tx_sim.py --seed-bias-frames 4096 --show-faults 5
  python3 tools/debug/asfw_saffire_tx_sim.py --sweep-write-ahead 0:256:8
"""

from __future__ import annotations

import argparse
import dataclasses
import math
import random
import statistics
from typing import Optional


TICKS_PER_CYCLE = 3072
TICKS_PER_FRAME = 512
FRAMES_PER_CYCLE = TICKS_PER_CYCLE // TICKS_PER_FRAME
PHASE_MOD = 0x0BB80000

LEAD_TIGHT = TICKS_PER_CYCLE
LEAD_ACCEPT = 7620
LEAD_REJECT = 12287
TARGET_LEAD = TICKS_PER_CYCLE + TICKS_PER_CYCLE // 2
DEADBAND_TICKS = 256

DEFAULT_RING_CAPACITY = 4096
DEFAULT_FRAMES_PER_DATA_PACKET = 8
DEFAULT_AM824_SLOTS = 9


def align_down(value: int, quantum: int) -> int:
    return value - (value % quantum)


def align_up(value: int, quantum: int) -> int:
    return ((value + quantum - 1) // quantum) * quantum


def ticks_to_frames(ticks: int) -> float:
    return ticks / TICKS_PER_FRAME


def ext_offset_diff(a: int, b: int, mod: int = PHASE_MOD) -> int:
    d = (a - b) % mod
    if d > mod // 2:
        d -= mod
    return d


def field_encode_syt(phase_ticks: int) -> int:
    cyc = (phase_ticks // TICKS_PER_CYCLE) & 0xF
    sub = phase_ticks % TICKS_PER_CYCLE
    return (cyc << 12) | sub


@dataclasses.dataclass
class TxPacketSlotStamp:
    begin_frame: int = 0
    end_frame: int = 0
    write_serial: int = 0
    has_audio_write: bool = False

    def covers(self, begin: int, end: int) -> bool:
        return self.has_audio_write and self.begin_frame <= begin and self.end_frame >= end


@dataclasses.dataclass
class TxPcmPacketRing:
    capacity: int = DEFAULT_RING_CAPACITY
    frames_per_packet: int = DEFAULT_FRAMES_PER_DATA_PACKET
    am824_slots: int = DEFAULT_AM824_SLOTS
    base_frame: int = 0

    def __post_init__(self) -> None:
        if self.capacity <= 0 or self.frames_per_packet <= 0:
            raise ValueError("capacity and frames_per_packet must be positive")
        if self.capacity % self.frames_per_packet != 0:
            raise ValueError("capacity must be divisible by frames_per_packet")
        self.packet_slots = self.capacity // self.frames_per_packet
        self.stamps = [TxPacketSlotStamp() for _ in range(self.packet_slots)]
        self.write_serial = 0
        # Payload memory is assumed to be valid AM824 silence by default.
        # Stamps mean real CoreAudio writes only.
        self.silence_initialized = True

    def relative_frame(self, frame: int) -> int:
        return (frame - self.base_frame) % self.capacity

    def slot_for_frame(self, frame: int) -> int:
        return (self.relative_frame(frame) // self.frames_per_packet) % self.packet_slots

    def stamp_for_frame(self, frame: int) -> TxPacketSlotStamp:
        return self.stamps[self.slot_for_frame(frame)]

    def populate_range(self, begin: int, end: int) -> int:
        if end <= begin:
            return 0

        first_packet = align_up(begin, self.frames_per_packet)
        last_packet_exclusive = align_down(end, self.frames_per_packet)
        if last_packet_exclusive <= first_packet:
            return 0

        count = 0
        f = first_packet
        while f < last_packet_exclusive:
            slot = self.slot_for_frame(f)
            self.write_serial += 1
            self.stamps[slot] = TxPacketSlotStamp(
                begin_frame=f,
                end_frame=f + self.frames_per_packet,
                write_serial=self.write_serial,
                has_audio_write=True,
            )
            count += 1
            f += self.frames_per_packet
        return count


@dataclasses.dataclass
class OutputPhaseResult:
    emit_data: bool
    output_phase_ticks: int
    lead_ticks: int
    syt: int
    tight_lead: bool
    reason: str


@dataclasses.dataclass
class TxOutputPhaseLoop:
    target_lead_ticks: int = TARGET_LEAD
    deadband_ticks: int = DEADBAND_TICKS
    data_advance_ticks: int = DEFAULT_FRAMES_PER_DATA_PACKET * TICKS_PER_FRAME

    out_phase: Optional[int] = None
    holds: int = 0
    rebases: int = 0
    nodata: int = 0
    tight: int = 0

    def reset(self) -> None:
        self.out_phase = None

    def emit(self, device_phase_ticks: int) -> OutputPhaseResult:
        """
        Cadence-aware Saffire-shaped phase emitter.

        Important ASFW/DICE point:
          At 48 kHz the wire cycle advances 6 audio frames per isoch cycle,
          but the current DATA packet carries 8 frames. So a correct model must
          naturally produce roughly 3 DATA packets per 4 callbacks.

        Therefore this loop does NOT rebase merely because DATA_ADVANCE_TICKS
        makes the next candidate lead grow by 1024 ticks. That growth is the
        desired D/D/D/N cadence pressure. We only rebase on impossible/stale
        lead, otherwise we carry the phase and let the DATA/NO-DATA gate handle
        cadence.
        """
        if self.out_phase is None:
            candidate = (device_phase_ticks + self.target_lead_ticks) % PHASE_MOD
            seed_reason = "seed=dev+target"
        else:
            candidate = self.out_phase
            seed_reason = "carry"

        lead = ext_offset_diff(candidate, device_phase_ticks)

        # Hard sanity correction only. Normal cadence lead growth must not
        # trigger rebase, otherwise we emit DATA every cycle and overrun 48 kHz.
        if lead <= 0 or lead > LEAD_REJECT:
            adjusted = (device_phase_ticks + self.target_lead_ticks) % PHASE_MOD
            self.rebases += 1
            pll_reason = f"rebase badLead={lead:+d}"
        else:
            adjusted = candidate
            self.holds += 1
            err = lead - self.target_lead_ticks
            pll_reason = f"hold leadErr={err:+d}"

        lead = ext_offset_diff(adjusted, device_phase_ticks)
        tight = lead <= LEAD_TIGHT - 1

        if lead < LEAD_ACCEPT:
            emit_data = True
            syt = field_encode_syt(adjusted)
            self.out_phase = (adjusted + self.data_advance_ticks) % PHASE_MOD
            if tight:
                self.tight += 1
            gate_reason = "DATA" + (" tight" if tight else "")
        else:
            emit_data = False
            syt = 0xFFFF
            # Keep adjusted phase across NO-DATA. The device phase advances by
            # one cycle while output phase stays put, bringing lead back into
            # the DATA window on the next callback.
            self.out_phase = adjusted
            self.nodata += 1
            if lead > LEAD_REJECT:
                gate_reason = "NODATA reject>4cyc"
            else:
                gate_reason = "NODATA cadence"

        return OutputPhaseResult(
            emit_data=emit_data,
            output_phase_ticks=adjusted,
            lead_ticks=lead,
            syt=syt,
            tight_lead=tight,
            reason=f"{seed_reason}; {pll_reason}; {gate_reason}",
        )


@dataclasses.dataclass
class OutputPhaseToAudioMap:
    phase_base_ticks: int = 0
    audio_base_frame: int = 0
    seeded: bool = False

    def seed(self, phase_ticks: int, audio_frame: int) -> None:
        self.phase_base_ticks = phase_ticks % PHASE_MOD
        self.audio_base_frame = audio_frame
        self.seeded = True

    def frame_for_phase(self, phase_ticks: int) -> int:
        if not self.seeded:
            raise RuntimeError("OutputPhaseToAudioMap is not seeded")
        dt = ext_offset_diff(phase_ticks % PHASE_MOD, self.phase_base_ticks)
        return self.audio_base_frame + int(dt // TICKS_PER_FRAME)


@dataclasses.dataclass
class CoreAudioProducer:
    ring: TxPcmPacketRing
    phase_map: OutputPhaseToAudioMap
    output_offset_frames: int = 64
    write_ahead_frames: int = 128
    io_buffer_frames: int = 128
    jitter_frames: int = 0
    rng: random.Random = dataclasses.field(default_factory=lambda: random.Random(1))

    written_end: int = 0
    populate_calls: int = 0
    populated_packets: int = 0

    def reset(self, initial_written_end: int = 0) -> None:
        self.written_end = align_down(initial_written_end, self.ring.frames_per_packet)
        self.populate_calls = 0
        self.populated_packets = 0

    def service_until_phase(self, device_phase_ticks: int) -> None:
        current_device_frame = self.phase_map.frame_for_phase(device_phase_ticks)
        reported_playhead = current_device_frame - self.output_offset_frames

        jitter = 0
        if self.jitter_frames:
            jitter = self.rng.randint(-self.jitter_frames, self.jitter_frames)

        target_end = reported_playhead + self.write_ahead_frames + jitter
        target_end = max(0, align_down(target_end, self.ring.frames_per_packet))

        while self.written_end + self.io_buffer_frames <= target_end:
            begin = self.written_end
            end = self.written_end + self.io_buffer_frames
            n = self.ring.populate_range(begin, end)
            self.populate_calls += 1
            self.populated_packets += n
            self.written_end = end

        if self.written_end < target_end:
            begin = self.written_end
            end = target_end
            n = self.ring.populate_range(begin, end)
            self.populate_calls += 1
            self.populated_packets += n
            self.written_end = target_end

    @property
    def oldest_valid(self) -> int:
        return self.written_end - self.ring.capacity


@dataclasses.dataclass
class FaultSample:
    callback: int
    packet_first: int
    packet_end: int
    slot: int
    stamp: TxPacketSlotStamp
    written_end: int
    oldest_valid: int
    lead_ticks: int
    syt: int
    reason: str

    def format(self) -> str:
        miss = self.packet_first - self.stamp.begin_frame if self.stamp.has_audio_write else None
        return (
            f"UNCOVERED cb={self.callback} slot={self.slot} "
            f"packet=[{self.packet_first},{self.packet_end}) "
            f"stamp=[{self.stamp.begin_frame},{self.stamp.end_frame}) "
            f"hasAudio={int(self.stamp.has_audio_write)} "
            f"miss={miss} written=[{self.oldest_valid},{self.written_end}) "
            f"lead={self.lead_ticks} ticks ({ticks_to_frames(self.lead_ticks):.1f}f) "
            f"syt=0x{self.syt:04x} reason={self.reason}"
        )


@dataclasses.dataclass
class SimStats:
    callbacks: int = 0
    data_packets: int = 0
    nodata_packets: int = 0
    covered_packets: int = 0
    uncovered_packets: int = 0
    future_packets: int = 0
    overwritten_packets: int = 0
    holes: int = 0
    one_lap_misses: int = 0
    glitches: int = 0

    lead_ticks: list[int] = dataclasses.field(default_factory=list)
    data_frame_leads: list[int] = dataclasses.field(default_factory=list)
    packet_phase_frames: list[int] = dataclasses.field(default_factory=list)
    faults: list[FaultSample] = dataclasses.field(default_factory=list)

    @property
    def coverage_pct(self) -> float:
        if self.data_packets == 0:
            return 0.0
        return 100.0 * self.covered_packets / self.data_packets

    @property
    def nodata_pct(self) -> float:
        if self.callbacks == 0:
            return 0.0
        return 100.0 * self.nodata_packets / self.callbacks


def percentile(values: list[int], q: float) -> float:
    if not values:
        return 0.0
    xs = sorted(values)
    pos = (len(xs) - 1) * q
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return float(xs[lo])
    frac = pos - lo
    return xs[lo] * (1.0 - frac) + xs[hi] * frac


def summarize_series(values: list[int], *, scale: float = 1.0) -> str:
    if not values:
        return "n/a"
    return (
        f"min={min(values) / scale:.1f} "
        f"p50={statistics.median(values) / scale:.1f} "
        f"p95={percentile(values, 0.95) / scale:.1f} "
        f"max={max(values) / scale:.1f}"
    )


@dataclasses.dataclass
class SimConfig:
    callbacks: int = 20000
    capacity: int = DEFAULT_RING_CAPACITY
    frames_per_packet: int = DEFAULT_FRAMES_PER_DATA_PACKET
    am824_slots: int = DEFAULT_AM824_SLOTS

    write_ahead_frames: int = 128
    output_offset_frames: int = 64
    io_buffer_frames: int = 128
    write_jitter_frames: int = 0

    drift_ppm: float = 0.0
    glitch_at: int = -1
    glitch_jump_ticks: int = 5000
    seed: int = 1

    target_lead_ticks: int = TARGET_LEAD
    deadband_ticks: int = DEADBAND_TICKS
    seed_bias_frames: int = 0

    show: int = 0
    show_faults: int = 0


def run_sim(cfg: SimConfig) -> SimStats:
    rng = random.Random(cfg.seed)
    ring = TxPcmPacketRing(
        capacity=cfg.capacity,
        frames_per_packet=cfg.frames_per_packet,
        am824_slots=cfg.am824_slots,
        base_frame=0,
    )

    phase_map = OutputPhaseToAudioMap()
    phase_map.seed(0, cfg.seed_bias_frames)

    producer = CoreAudioProducer(
        ring=ring,
        phase_map=phase_map,
        output_offset_frames=cfg.output_offset_frames,
        write_ahead_frames=cfg.write_ahead_frames,
        io_buffer_frames=cfg.io_buffer_frames,
        jitter_frames=cfg.write_jitter_frames,
        rng=rng,
    )
    producer.reset(initial_written_end=0)

    loop = TxOutputPhaseLoop(
        target_lead_ticks=cfg.target_lead_ticks,
        deadband_ticks=cfg.deadband_ticks,
        data_advance_ticks=cfg.frames_per_packet * TICKS_PER_FRAME,
    )

    stats = SimStats()
    device_phase_float = 0.0
    nominal_advance = TICKS_PER_CYCLE * (1.0 + cfg.drift_ppm / 1.0e6)
    previous_expected_device_phase: Optional[int] = None

    for cb in range(cfg.callbacks):
        if cb == 0:
            device_phase_float = 0.0
        else:
            device_phase_float += nominal_advance
        if cfg.glitch_at >= 0 and cb == cfg.glitch_at:
            device_phase_float += cfg.glitch_jump_ticks
            loop.reset()
            stats.glitches += 1

        device_phase_ticks = int(round(device_phase_float)) % PHASE_MOD

        if previous_expected_device_phase is not None:
            derr = ext_offset_diff(device_phase_ticks, previous_expected_device_phase)
            if abs(derr) > (TICKS_PER_CYCLE // 2) and abs(derr) != TICKS_PER_CYCLE:
                loop.reset()
                stats.glitches += 1
        previous_expected_device_phase = (device_phase_ticks + TICKS_PER_CYCLE) % PHASE_MOD

        producer.service_until_phase(device_phase_ticks)

        res = loop.emit(device_phase_ticks)
        stats.callbacks += 1
        stats.lead_ticks.append(res.lead_ticks)

        if not res.emit_data:
            stats.nodata_packets += 1
            if cfg.show and cb < cfg.show:
                print(
                    f"cb={cb:05d} NODATA lead={res.lead_ticks} "
                    f"({ticks_to_frames(res.lead_ticks):.1f}f) syt=0xffff {res.reason}"
                )
            continue

        stats.data_packets += 1

        audio_frame = phase_map.frame_for_phase(res.output_phase_ticks)
        packet_first = align_down(audio_frame, cfg.frames_per_packet)
        packet_end = packet_first + cfg.frames_per_packet
        slot = ring.slot_for_frame(packet_first)
        stamp = ring.stamp_for_frame(packet_first)
        covered = stamp.covers(packet_first, packet_end)

        frame_lead_to_written = producer.written_end - packet_end
        phase_frame_lead = audio_frame - phase_map.frame_for_phase(device_phase_ticks)
        stats.data_frame_leads.append(frame_lead_to_written)
        stats.packet_phase_frames.append(phase_frame_lead)

        if covered:
            stats.covered_packets += 1
        else:
            stats.uncovered_packets += 1
            if packet_end > producer.written_end:
                stats.future_packets += 1
            elif packet_first < producer.oldest_valid:
                stats.overwritten_packets += 1
            else:
                stats.holes += 1

            if stamp.has_audio_write and abs(packet_first - stamp.begin_frame) == cfg.capacity:
                stats.one_lap_misses += 1

            if len(stats.faults) < cfg.show_faults:
                stats.faults.append(FaultSample(
                    callback=cb,
                    packet_first=packet_first,
                    packet_end=packet_end,
                    slot=slot,
                    stamp=stamp,
                    written_end=producer.written_end,
                    oldest_valid=producer.oldest_valid,
                    lead_ticks=res.lead_ticks,
                    syt=res.syt,
                    reason=res.reason,
                ))

        if cfg.show and cb < cfg.show:
            print(
                f"cb={cb:05d} DATA   phase={res.output_phase_ticks:9d} "
                f"lead={res.lead_ticks:5d} ({ticks_to_frames(res.lead_ticks):4.1f}f) "
                f"syt=0x{res.syt:04x} audioFrame={audio_frame:7d} "
                f"packet=[{packet_first},{packet_end}) slot={slot:03d} "
                f"stamp=[{stamp.begin_frame},{stamp.end_frame}) "
                f"covered={int(covered)} writtenEnd={producer.written_end:7d} "
                f"frameLead={frame_lead_to_written:4d} {res.reason}"
            )

    stats._producer = producer  # type: ignore[attr-defined]
    stats._loop = loop          # type: ignore[attr-defined]
    return stats


def print_report(cfg: SimConfig, stats: SimStats) -> None:
    producer = stats._producer  # type: ignore[attr-defined]
    loop = stats._loop          # type: ignore[attr-defined]

    print("=== ASFW Saffire-shaped TX simulation ===")
    print(
        f"rate=48000 capacity={cfg.capacity} fpp={cfg.frames_per_packet} "
        f"slots={cfg.capacity // cfg.frames_per_packet} am824Slots={cfg.am824_slots}"
    )
    print(
        f"writeAhead={cfg.write_ahead_frames} outputOffset={cfg.output_offset_frames} "
        f"ioBuffer={cfg.io_buffer_frames} jitter={cfg.write_jitter_frames} "
        f"seedBias={cfg.seed_bias_frames}"
    )
    print(
        f"phaseLoop targetLead={cfg.target_lead_ticks} ticks "
        f"({ticks_to_frames(cfg.target_lead_ticks):.1f} frames) "
        f"deadband={cfg.deadband_ticks} drift={cfg.drift_ppm:g}ppm"
    )
    print()

    print("=== Results ===")
    print(f"callbacks        : {stats.callbacks}")
    print(f"DATA / NO-DATA   : {stats.data_packets} / {stats.nodata_packets} "
          f"({stats.nodata_pct:.1f}% NO-DATA)")
    print(f"coverage         : {stats.covered_packets}/{stats.data_packets} "
          f"({stats.coverage_pct:.2f}%)")
    print(f"uncovered        : {stats.uncovered_packets} "
          f"(future={stats.future_packets}, old={stats.overwritten_packets}, "
          f"holes={stats.holes}, oneLap={stats.one_lap_misses})")
    print(f"producer writes  : calls={producer.populate_calls} packetStamps={producer.populated_packets} "
          f"writtenEnd={producer.written_end} oldest={producer.oldest_valid}")
    print(f"phase loop       : holds={loop.holds} rebases={loop.rebases} "
          f"nodata={loop.nodata} tightWarns={loop.tight} glitches={stats.glitches}")
    print(f"lead ticks       : {summarize_series(stats.lead_ticks)}")
    print(f"lead frames      : {summarize_series(stats.lead_ticks, scale=TICKS_PER_FRAME)}")
    print(f"packet phase f   : {summarize_series(stats.packet_phase_frames)} "
          f"(selected audio frame minus current device frame)")
    print(f"written margin f : {summarize_series(stats.data_frame_leads)} "
          f"(writtenEnd - packetEnd)")
    print()

    if stats.faults:
        print("=== First uncovered packets ===")
        for f in stats.faults:
            print(f.format())
        print()

    if stats.covered_packets == stats.data_packets and stats.data_packets > 0:
        print("VERDICT: OK - all DATA packets selected covered audio frame ranges.")
    else:
        print("VERDICT: BROKEN - DATA selection can read unwritten/old/hole packet ranges.")


def parse_sweep(s: str) -> tuple[int, int, int]:
    a, b, c = s.split(":")
    return int(a, 0), int(b, 0), int(c, 0)


def run_sweep_write_ahead(args: argparse.Namespace) -> None:
    start, end, step = parse_sweep(args.sweep_write_ahead)
    print("=== Sweep writeAheadFrames ===")
    print("ahead  coverage  data/nodata  future old holes oneLap  margin[min/p50/max]")
    print("-----  --------  -----------  ------ --- ----- ------  -------------------")
    best: Optional[int] = None
    for ahead in range(start, end + 1, step):
        cfg = SimConfig(
            callbacks=args.callbacks,
            capacity=args.capacity,
            frames_per_packet=args.frames_per_packet,
            am824_slots=args.am824_slots,
            write_ahead_frames=ahead,
            output_offset_frames=args.output_offset_frames,
            io_buffer_frames=args.io_buffer_frames,
            write_jitter_frames=args.write_jitter_frames,
            drift_ppm=args.drift_ppm,
            glitch_at=args.glitch_at,
            glitch_jump_ticks=args.glitch_jump,
            seed=args.seed,
            target_lead_ticks=args.target_lead_ticks,
            deadband_ticks=args.deadband_ticks,
            seed_bias_frames=args.seed_bias_frames,
        )
        st = run_sim(cfg)
        if best is None and st.covered_packets == st.data_packets and st.data_packets > 0:
            best = ahead
        margins = st.data_frame_leads
        margin_txt = "n/a" if not margins else f"{min(margins)}/{int(statistics.median(margins))}/{max(margins)}"
        print(
            f"{ahead:5d}  {st.coverage_pct:7.2f}%  "
            f"{st.data_packets:5d}/{st.nodata_packets:<5d}  "
            f"{st.future_packets:6d} {st.overwritten_packets:3d} "
            f"{st.holes:5d} {st.one_lap_misses:6d}  {margin_txt}"
        )
    print()
    if best is not None:
        print(f"smallest fully-covered writeAheadFrames in sweep: {best}")
    else:
        print("no fully-covered writeAheadFrames in sweep")


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--callbacks", type=int, default=20000)
    p.add_argument("--capacity", type=int, default=DEFAULT_RING_CAPACITY)
    p.add_argument("--frames-per-packet", type=int, default=DEFAULT_FRAMES_PER_DATA_PACKET)
    p.add_argument("--am824-slots", type=int, default=DEFAULT_AM824_SLOTS)

    p.add_argument("--write-ahead-frames", type=int, default=128,
                   help="CoreAudio/HAL write target ahead of reported playhead, frames")
    p.add_argument("--output-offset-frames", type=int, default=64,
                   help="reported playhead = device frame - this offset")
    p.add_argument("--io-buffer-frames", type=int, default=128)
    p.add_argument("--write-jitter-frames", type=int, default=0)

    p.add_argument("--drift-ppm", type=float, default=0.0)
    p.add_argument("--glitch-at", type=int, default=-1)
    p.add_argument("--glitch-jump", type=int, default=5000)
    p.add_argument("--seed", type=int, default=1)

    p.add_argument("--target-lead-ticks", type=int, default=TARGET_LEAD)
    p.add_argument("--deadband-ticks", type=int, default=DEADBAND_TICKS)
    p.add_argument("--seed-bias-frames", type=int, default=0,
                   help="intentionally offset phaseBaseTicks<->audioBaseFrame seeding")

    p.add_argument("--show", type=int, default=0,
                   help="print first N callbacks")
    p.add_argument("--show-faults", type=int, default=0,
                   help="print first N uncovered DATA packets")
    p.add_argument("--sweep-write-ahead",
                   help="sweep write-ahead frames, format start:end:step")
    return p


def main() -> int:
    args = build_arg_parser().parse_args()

    if args.sweep_write_ahead:
        run_sweep_write_ahead(args)
        return 0

    cfg = SimConfig(
        callbacks=args.callbacks,
        capacity=args.capacity,
        frames_per_packet=args.frames_per_packet,
        am824_slots=args.am824_slots,
        write_ahead_frames=args.write_ahead_frames,
        output_offset_frames=args.output_offset_frames,
        io_buffer_frames=args.io_buffer_frames,
        write_jitter_frames=args.write_jitter_frames,
        drift_ppm=args.drift_ppm,
        glitch_at=args.glitch_at,
        glitch_jump_ticks=args.glitch_jump,
        seed=args.seed,
        target_lead_ticks=args.target_lead_ticks,
        deadband_ticks=args.deadband_ticks,
        seed_bias_frames=args.seed_bias_frames,
        show=args.show,
        show_faults=args.show_faults,
    )
    stats = run_sim(cfg)
    print_report(cfg, stats)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
