#!/usr/bin/env python3
"""Compare Linux-relative SPH replay with a vendor-style synthesis model.

Independent behavioral implementation; no reference code is copied.
Replay: references/linux-sound-firewire-stack/firewire/motu/amdtp-motu.c:303-329,
373-393; packet cadence: amdtp-stream.c replay sequence.
Synthesis: tmp/motu-research/vendor-motu-gap-research.md sections 1.2-1.4.
Controller/test evidence: PR #172, head 858f07582c9dac937dab492036097a40f104cead;
MotuSphClockServo.hpp, MotuSphClockServoTests.cpp, MotuV3SphWireParityTests.cpp.
Vendor gains/clamps and exact phase convention are NOT fully recovered: this is
a PR #172-informed loop model, not a bit-exact MOTU implementation.
Uses only the Python standard library. Never accesses hardware.
"""
import argparse
import csv
from dataclasses import dataclass
import html
import json
import math
from pathlib import Path
import random
import statistics

TICKS_PER_CYCLE = 3072
CYCLES_PER_SECOND = 8000
TICKS_PER_SECOND = TICKS_PER_CYCLE * CYCLES_PER_SECOND


def encode_sph(ticks):
    tick = round(ticks) % TICKS_PER_SECOND
    return ((tick // TICKS_PER_CYCLE) << 12) | (tick % TICKS_PER_CYCLE)


def decode_sph(sph):
    cycle, offset = (sph >> 12) & 0x1fff, sph & 0xfff
    if cycle >= CYCLES_PER_SECOND or offset >= TICKS_PER_CYCLE:
        raise ValueError("invalid SPH cycle or offset")
    return cycle * TICKS_PER_CYCLE + offset


def unwrap_near(tick, reference):
    return tick + round((reference - tick) / TICKS_PER_SECOND) * TICKS_PER_SECOND


@dataclass
class Packet:
    cycle: int
    frame: int
    stamps: list


class Replay:
    """Buffered per-packet cadence and per-frame offsets, translated to TX cycles."""
    def emit(self, packet, output_cycle):
        base = (packet.cycle % CYCLES_PER_SECOND) * TICKS_PER_CYCLE
        offsets = [(decode_sph(sph) - base) % TICKS_PER_SECOND for sph in packet.stamps]
        return [encode_sph(output_cycle * TICKS_PER_CYCLE + offset) for offset in offsets]


Q32 = 1 << 32
ORACLE_REL_SETPOINT_TICKS = 510  # PR #172's official-driver capture; not universal model proof.


def trunc_div(numerator, denominator):
    """C++ signed division: truncate toward zero, including negative feedback."""
    return (abs(numerator) // denominator) * (-1 if numerator < 0 else 1)


def absolute_phase_seed(phase_ticks, center_ticks=None):
    """Distance to PR #172's observed setpoint, with a movable wrap branch cut."""
    def fold(ticks):
        value = ticks % TICKS_PER_CYCLE
        return value-TICKS_PER_CYCLE if value > TICKS_PER_CYCLE//2 else value

    error = phase_ticks-ORACLE_REL_SETPOINT_TICKS
    if center_ticks is None:
        return -fold(error)
    center_error = fold(center_ticks-ORACLE_REL_SETPOINT_TICKS)
    return -(center_error + fold(error-center_error))


class SphServo:
    """PR #172 behavioral model: cumulative RX/TX feedback, elapsed-frame gain.

    Observation ticks are unwrapped; correction includes actual TX actuation.
    This models the pure loop, not PR's complete phase-conditioning bridge.
    """
    def __init__(self, rate, clamp_ppm=2000, locked_shift=2, acquisition_shift=0):
        self.nominal = (TICKS_PER_SECOND << 32) // rate
        self.lower = int(self.nominal * (1-clamp_ppm*1e-6))
        self.upper = int(self.nominal * (1+clamp_ppm*1e-6))
        self.locked_shift, self.acquisition_shift = locked_shift, acquisition_shift
        self.reference = None
        self.previous = None
        self.locked = False
        self.step = self.nominal
        self.error = 0
        self.hard_resync = False
        self.clamped = False
        self.cause = 'bootstrap'

    def update(self, generation, frames, ticks, tx_correction, seed_ticks=None,
               absolute_phase_ticks=None, phase_center_ticks=None):
        # These flags describe this observation, rather than the previous update.
        self.hard_resync = False
        self.clamped = False
        if absolute_phase_ticks is not None:
            seed_ticks = absolute_phase_seed(absolute_phase_ticks, phase_center_ticks)
        cause = None
        if self.reference is None:
            cause = 'bootstrap'
        elif generation != self.reference[0]:
            cause = 'generation-change'
        elif seed_ticks is not None:
            cause = 'absolute-phase'
        elif frames < self.previous[0]:
            cause = 'frame-regression'
        elif ticks < self.previous[1]:
            cause = 'tick-regression'
        if cause:
            self.reference = (generation, frames, ticks, tx_correction, (seed_ticks or 0)*Q32)
            self.previous = (frames, ticks)
            self.step = self.nominal
            self.error = (seed_ticks or 0)*Q32
            self.cause = cause
            return False
        if frames == self.previous[0]:
            self.cause = 'bridge-stall'
            return False
        elapsed = frames - self.previous[0]
        delta = ticks - self.previous[1]
        if delta <= 0:
            self.reference = (generation, frames, ticks, tx_correction, 0)
            self.previous = (frames, ticks)
            self.step = self.nominal
            self.error = 0
            self.cause = 'non-advancing-ticks'
            return False
        _, ref_frames, ref_ticks, ref_tx, seed = self.reference
        measured = (delta << 32) // elapsed
        self.error = ((ticks-ref_ticks) << 32) - self.nominal*(frames-ref_frames) - (tx_correction-ref_tx) + seed
        self.hard_resync = abs(self.error) > 4*TICKS_PER_CYCLE*Q32
        shift = self.locked_shift if self.locked else self.acquisition_shift
        requested = measured if self.hard_resync else measured + trunc_div(self.error, elapsed << shift)
        self.step = max(self.lower, min(self.upper, requested))
        self.clamped = self.step != requested
        if self.hard_resync:
            self.locked = False
        elif abs(self.error) < (TICKS_PER_CYCLE*Q32)//50:
            self.locked = True
        self.previous = frames, ticks
        self.cause = 'tracking'
        return True


class Synthesizer:
    """Free-running Q32 accumulator driven by buffer-window servo observations.

    Frames/ticks and TX correction are referenced on the same sample index.
    Actual PR bridge scheduling, absolute-phase conditioner and repair placement
    are not reproduced. Window and clamp are exposed simulation parameters.
    """
    def __init__(self, rate, window_frames, clamp_ppm, locked_shift, acquisition_shift):
        self.servo = SphServo(rate, clamp_ppm, locked_shift, acquisition_shift)
        self.window = window_frames
        self.next_q32 = None
        self.next_frame = 0
        self.reference_frame = 0
        self.reference_tick = 0
        self.last_observed_frame = 0
        self.correction = 0
        self.correction_at_frame = {}
        self.resyncs = 0
        self.locked = False
        self.lock_frame = None

    def observe(self, packet, delay):
        if not packet.stamps:
            return
        first = unwrap_near(decode_sph(packet.stamps[0]), packet.cycle*TICKS_PER_CYCLE)
        target = first + delay*TICKS_PER_CYCLE
        if self.next_q32 is None:
            self.next_q32, self.next_frame = target*Q32, packet.frame
            self.reference_frame, self.reference_tick = packet.frame, first
            self.last_observed_frame = packet.frame
            self.servo.update(1, 0, 0, 0)
            return
        if packet.frame-self.last_observed_frame < self.window:
            return
        correction = self.correction_at_frame.get(packet.frame)
        if correction is None:
            # Nominal extrapolation only at the packet boundary before its emission.
            if packet.frame != self.next_frame:
                return
            correction = self.correction
        frames = packet.frame-self.reference_frame
        if self.servo.update(1, frames, first-self.reference_tick, correction):
            if self.servo.hard_resync:
                # Model a one-shot repair at the next emission; count its actuation.
                self.next_q32 += self.servo.error
                self.correction += self.servo.error
                self.resyncs += 1
            self.locked = self.servo.locked
            if self.locked and self.lock_frame is None:
                self.lock_frame = frames
        self.last_observed_frame = packet.frame

    def emit(self, cycle, block):
        if self.next_q32 is None or self.next_q32 >= (cycle+1)*TICKS_PER_CYCLE*Q32:
            return [], []
        stamps, frames = [], []
        for _ in range(block):
            self.correction_at_frame[self.next_frame] = self.correction
            stamps.append(encode_sph(self.next_q32 // Q32))
            frames.append(self.next_frame)
            self.next_q32 += self.servo.step
            self.correction += self.servo.step-self.servo.nominal
            self.next_frame += 1
        # Keep only the latest two measurement windows of bridge history.
        oldest = self.next_frame-2*self.window
        self.correction_at_frame = {f:c for f,c in self.correction_at_frame.items() if f >= oldest}
        return stamps, frames


def simulate(args, scenario):
    rng = random.Random(args.seed)
    cycles = round(args.seconds * CYCLES_PER_SECOND)
    block = 8 * (1 if args.rate <= 48000 else 2 if args.rate <= 96000 else 4)
    origin = args.start_cycle * TICKS_PER_CYCLE
    first_tick = origin + args.lead_cycles * TICKS_PER_CYCLE
    frame, next_tick = 0, first_tick
    packets, truth = {}, {}
    for relative in range(cycles):
        cycle = args.start_cycle + relative
        stamps = []
        start_frame = frame
        if next_tick < (cycle + 1) * TICKS_PER_CYCLE:
            for _ in range(block):
                truth[frame] = next_tick
                noise = rng.gauss(0, args.jitter_ticks) if scenario == 'jitter' else 0
                stamps.append(encode_sph(next_tick + noise))
                ppm = args.drift_ppm
                if scenario == 'drift-step' and relative >= cycles // 2:
                    ppm = -args.drift_ppm
                next_tick += TICKS_PER_SECOND / (args.rate * (1 + ppm * 1e-6))
                frame += 1
        # Drop a whole packet: a discontinuity, including its cadence information.
        if scenario == 'drop' and relative == cycles // 2:
            packets[cycle] = None
        else:
            packets[cycle] = Packet(cycle, start_frame, stamps)

    replay = Replay()
    synth = Synthesizer(args.rate, args.window_frames, args.clamp_ppm, args.locked_shift, args.acquisition_shift)
    rows, counts = [], {'replay': [], 'synthesis': []}
    stopped = False
    for cycle in range(args.start_cycle, args.start_cycle + cycles + args.delay_cycles):
        observed = packets.get(cycle - args.delay_cycles)
        in_window = args.start_cycle <= cycle - args.delay_cycles < args.start_cycle + cycles
        if not in_window:
            continue
        if observed is None:
            # Linux reports discontinuity and stops. Apply the same safety policy
            # to synthesis: open-loop continuation would be a separate experiment.
            stopped = True
        if stopped:
            break
        synth.observe(observed, args.delay_cycles)
        replay_stamps = replay.emit(observed, cycle)
        synth_stamps, synth_frames = synth.emit(cycle, block)
        counts['replay'].append(len(replay_stamps))
        counts['synthesis'].append(len(synth_stamps))
        for algorithm, stamps, frames in (
            ('replay', replay_stamps, range(observed.frame, observed.frame + len(replay_stamps))),
            ('synthesis', synth_stamps, synth_frames)):
            for sph, sample in zip(stamps, frames):
                if sample not in truth:
                    continue
                ideal = truth[sample] + args.delay_cycles * TICKS_PER_CYCLE
                actual = unwrap_near(decode_sph(sph), ideal)
                rows.append(dict(algorithm=algorithm, cycle=cycle, frame=sample,
                                 time_s=(cycle - args.start_cycle) / CYCLES_PER_SECOND,
                                 sph=f'0x{sph:08x}', error_ticks=actual - ideal,
                                 error_us=(actual - ideal) / 24.576))
    stats = {}
    for algorithm in counts:
        errors = [r['error_us'] for r in rows if r['algorithm'] == algorithm]
        stats[algorithm] = dict(frames=len(errors), mean_error_us=statistics.fmean(errors) if errors else None,
                               rms_error_us=math.sqrt(statistics.fmean(e*e for e in errors)) if errors else None,
                               peak_error_us=max(map(abs, errors), default=None),
                               empty_packets=counts[algorithm].count(0))
    stats.update(stopped_on_discontinuity=stopped, synthesis_resyncs=synth.resyncs,
                 synthesis_lock_frame=synth.lock_frame, cadence_mismatches=sum(a != b for a, b in zip(counts['replay'], counts['synthesis'])))
    return rows, stats


def write_plot(path, rows, scenario):
    colors = {'replay': '#58a6ff', 'synthesis': '#f6ad55'}
    bound = max(0.05, max((abs(r['error_us']) for r in rows), default=0)) * 1.1
    duration = max((r['time_s'] for r in rows), default=1) or 1
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1000" height="420" viewBox="0 0 1000 420">',
             '<rect width="1000" height="420" fill="#101820"/>',
             f'<text x="65" y="28" fill="white" font-family="sans-serif">{html.escape(scenario)} — SPH error relative to device timeline (µs)</text>']
    for value in (-bound, 0, bound):
        y = 205 - value / bound * 150
        parts.append(f'<path d="M65 {y} H970" stroke="#394651"/><text x="5" y="{y}" fill="white" font-size="12">{value:.3f}</text>')
    for algorithm, color in colors.items():
        series = [r for r in rows if r['algorithm'] == algorithm]
        stride = max(1, len(series)//4000)
        points = ' '.join(f"{65+r['time_s']/duration*905:.2f},{205-r['error_us']/bound*150:.2f}" for r in series[::stride])
        parts.append(f'<polyline points="{points}" fill="none" stroke="{color}" stroke-width="1"/>')
    parts.append(f'<text x="65" y="390" fill="white" font-family="sans-serif">0 → {duration:.3f} seconds   ·   blue: replay   ·   orange: synthesis</text></svg>')
    path.write_text('\n'.join(parts))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--rate', type=int, choices=[44100,48000,88200,96000,176400,192000], default=48000)
    parser.add_argument('--seconds', type=float, default=2)
    parser.add_argument('--drift-ppm', type=float, default=100)
    parser.add_argument('--jitter-ticks', type=float, default=8)
    parser.add_argument('--delay-cycles', type=int, default=32)
    parser.add_argument('--lead-cycles', type=int, default=2)
    parser.add_argument('--start-cycle', type=int, default=7990, help='Default exercises one-second SPH wrap')
    parser.add_argument('--window-frames', type=int, default=512, help='RX observation interval; actual elapsed frames determine gain')
    parser.add_argument('--locked-shift', type=int, default=2, help='Quarter gain while locked, as PR #172')
    parser.add_argument('--acquisition-shift', type=int, default=0, help='Full acquisition gain, as PR #172')
    parser.add_argument('--clamp-ppm', type=float, default=2000, help='Simulation clamp; not claimed to be vendor limit')
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--scenario', choices=['all','steady','drift-step','jitter','drop'], default='all')
    parser.add_argument('--output', type=Path, default=Path('tmp/motu-sph-comparison'))
    args = parser.parse_args()
    if args.seconds <= 0 or args.delay_cycles < 0 or args.lead_cycles < 0 or args.window_frames <= 0 or not 0 <= args.locked_shift <= 16 or not 0 <= args.acquisition_shift <= 16 or not 0 <= args.clamp_ppm < 1e6 or abs(args.drift_ppm) >= 1e6 or args.jitter_ticks < 0:
        parser.error('invalid duration, delay, lead, controller or clock parameters')
    args.output.mkdir(parents=True, exist_ok=True)
    report = {'parameters': {k: str(v) if isinstance(v, Path) else v for k,v in vars(args).items()},
              'limitations': 'PR #172 pure-loop model and Q32 accumulator; observation/phase bridge and actuator scheduling remain simplified. Loss stops both algorithms. Not hardware validation.', 'scenarios': {}}
    for scenario in ['steady','drift-step','jitter','drop'] if args.scenario == 'all' else [args.scenario]:
        rows, stats = simulate(args, scenario)
        report['scenarios'][scenario] = stats
        with (args.output / f'{scenario}.csv').open('w', newline='') as file:
            writer = csv.DictWriter(file, fieldnames=list(rows[0]) if rows else ['algorithm','cycle','frame','time_s','sph','error_ticks','error_us'])
            writer.writeheader()
            writer.writerows(rows)
        write_plot(args.output / f'{scenario}.svg', rows, scenario)
        print(f'{scenario}: {json.dumps(stats, sort_keys=True)}')
    (args.output / 'summary.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Artifacts: {args.output.resolve()}')


if __name__ == '__main__':
    main()
