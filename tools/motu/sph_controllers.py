#!/usr/bin/env python3
"""Three MOTU transmit-SPH controllers against one device model, 48 kHz.

Scored the way passive-observer captures are measured (sph_wire_capture.py):
the host's transmit SPH minus the device's capture SPH on the same bus cycle,
and the host's SPH lead over its own transmit cycle.

Measured target, MOTU 828mk3 with the official driver (observer captures,
2026-07-30 and 2026-08-22): after its start-up the same-cycle offset holds
13 frames (6656 ticks) +0 or +21 ticks, +/-1 tick, for the whole capture; the
first DATA packet leads its transmit cycle by exactly 9216 ticks (3 cycles)
and the lead then stays within 9216..12287.

Controllers:
  pr191     cumulative RX/TX servo (compare_sph.SphServo, PR #172/#191 law),
            seeded at transmit time + 3-cycle lead; one absolute-phase seed
            from a measured offset window, folded to one cycle, then relative.
  synth     the pre-#191 SphSynthesizer (MotuSphSynthesizer.hpp at 69887ef5^1),
            observing the replay-rebased device SPH, seeded from it.
  restored  the same SphSynthesizer, seeded at transmit time + 3-cycle lead,
            observing the device SPH plus a constant chosen to hold the
            measured same-cycle offset: the setpoint as continuous feedback.

Model limits: one device, one rate; host processing latency, IT/IR DMA and
host clock noise are not modelled; the device accepts any lead. A good score
here is reason to test on hardware, not proof of clean audio.
"""
import argparse
import json
import random
import statistics
from dataclasses import dataclass, field

from compare_sph import Q32, SphServo, trunc_div

TICKS_PER_CYCLE = 3072
TICKS_PER_SECOND = TICKS_PER_CYCLE * 8000
RATE = 48000
FRAME_TICKS = TICKS_PER_SECOND // RATE  # 512
FRAMES_PER_PACKET = 8
LEAD_TICKS = 3 * TICKS_PER_CYCLE        # first DATA lead, both drivers on the wire
VENDOR_OFFSET_TICKS = 13 * FRAME_TICKS  # 6656, 90 s capture
LOCK_TICKS = TICKS_PER_CYCLE // 50      # 61.44 -> 61, the lock threshold both use


def fold(value, period):
    value %= period
    return value - period if value >= period / 2 else value


@dataclass
class DeviceParams:
    period_ppm: float = 14.8          # ticks per frame vs nominal, 828mk3 captures
    stamp_jitter_ticks: float = 0.25  # 4095/4096/4097 = 5.7/82.6/11.8%; capture 6.2/81.5/12.3%
    max_lead_ticks: int = 6322        # largest capture lead seen; sets DATA cadence
    phase_step_ticks: int = 0         # device timeline jump (re-anchor) ...
    phase_step_at_s: float = 0.0      # ... at this time
    ppm_after_s: float = 0.0          # optional rate step
    ppm_after: float = 0.0


class Device:
    """Device clock and capture stream: one DATA packet of 8 frames when due."""
    def __init__(self, params, rng, start_cycle, start_tick):
        self.p, self.rng = params, rng
        self.start_cycle = start_cycle
        self.next_true = float(start_tick)
        self.frame = 0
        self.stepped = False

    def seconds(self, cycle):
        return (cycle - self.start_cycle) / 8000

    def period(self, cycle):
        ppm = self.p.ppm_after if self.p.ppm_after_s and self.seconds(cycle) >= self.p.ppm_after_s else self.p.period_ppm
        return FRAME_TICKS * (1 + ppm * 1e-6)

    def packet(self, cycle):
        if self.p.phase_step_ticks and not self.stepped and self.seconds(cycle) >= self.p.phase_step_at_s:
            self.next_true += self.p.phase_step_ticks
            self.stepped = True
        if self.next_true > cycle * TICKS_PER_CYCLE + self.p.max_lead_ticks:
            return None
        stamp = round(self.next_true + self.rng.uniform(-self.p.stamp_jitter_ticks, self.p.stamp_jitter_ticks))
        first = self.frame
        self.next_true += FRAMES_PER_PACKET * self.period(cycle)
        self.frame += FRAMES_PER_PACKET
        return first, stamp


class SphSynthesizer:
    """Behavioral port of MotuSphSynthesizer.hpp (69887ef5^1), unwrapped ticks."""
    def __init__(self, tolerance_ppm=2000):
        self.nominal = FRAME_TICKS * Q32
        self.tolerance = self.nominal * tolerance_ppm // 1_000_000
        self.ready = self.locked = False
        self.step = self.nominal
        self.phase = 0
        self.frames = self.reference_frames = 0
        self.reference_tick = self.observed_tick = 0
        self.error_ticks = 0
        self.resyncs = 0
        self.reference_pending = False

    def seed(self, tick, external=False):
        """Start the clock at `tick`. An external seed (transmit time + lead)
        is not an observation: the first observation after it only sets the
        rate reference, and the phase error is then removed by the loop gain
        (restored design; the C++ synthesizer seeds from an observation)."""
        self.phase = tick * Q32
        self.reference_tick = self.observed_tick = tick
        self.reference_frames = self.frames
        self.ready = True
        self.reference_pending = external

    def observe(self, tick):
        """`tick`: where the frame about to be emitted should be presented."""
        if not self.ready:
            self.seed(tick)
            return
        if self.reference_pending:
            self.reference_tick = self.observed_tick = tick
            self.reference_frames = self.frames
            self.reference_pending = False
            return
        self.observed_tick = tick
        error = tick * Q32 - self.phase
        self.error_ticks = error // Q32
        if abs(error) > 4 * TICKS_PER_CYCLE * Q32:
            return self.reanchor(tick)
        elapsed = self.frames - self.reference_frames
        if elapsed < 512:
            return
        measured = ((self.observed_tick - self.reference_tick) * Q32) // elapsed
        if not self.nominal - self.tolerance <= measured <= self.nominal + self.tolerance:
            return self.reanchor(tick)
        gain = 4 if self.locked else 1
        self.step = max(self.nominal - self.tolerance,
                        min(self.nominal + self.tolerance, measured + trunc_div(error, elapsed) // gain))
        self.locked = abs(error) < 6144 * Q32 // 100
        self.reference_frames, self.reference_tick = self.frames, self.observed_tick

    def reanchor(self, tick):
        self.phase = tick * Q32
        self.reference_tick = self.observed_tick = tick
        self.reference_frames = self.frames
        self.step, self.locked = self.nominal, False
        self.resyncs += 1

    def next_stamp(self):
        stamp = self.phase // Q32
        self.phase += self.step
        self.frames += 1
        return stamp


@dataclass
class Run:
    offsets: list = field(default_factory=list)   # (time_s, same-cycle offset)
    leads: list = field(default_factory=list)     # (time_s, tx lead ticks)
    resyncs: int = 0


def simulate(controller, seconds=4.0, seed=1, device=None, cadence_delay=4,
             target_offset=VENDOR_OFFSET_TICKS, observe_every_frames=512,
             observe_jitter_frames=0, pr191_seed_at_s=0.25, pr191_seed_window_s=0.1):
    """Run one controller; returns a Run with same-cycle offsets and TX leads.

    cadence_delay: TX DATA/NO-DATA replays the device cadence this many cycles
    later (both PR #191 and the synthesizer replay the RX cadence).
    """
    rng = random.Random(seed)
    params = device or DeviceParams()
    start_cycle = 1000
    dev = Device(params, rng, start_cycle, start_cycle * TICKS_PER_CYCLE + 4000)
    rx = {}  # cycle -> (first frame, stamp)
    run = Run()
    synth = SphSynthesizer() if controller in ('synth', 'restored') else None
    servo = SphServo(RATE) if controller == 'pr191' else None
    tx_phase = None                       # pr191 accumulator, Q32 ticks
    tx_frames = 0
    tx_correction = 0                     # Q32, sum of (step - nominal)
    rx_ref = None                         # (frames, stamp) of first RX sample
    next_observe = 0
    seeded_absolute = False
    window = []                           # (cycle, offset) for the pr191 seed
    total_cycles = int(seconds * 8000)
    for relative in range(total_cycles):
        cycle = start_cycle + relative
        packet = dev.packet(cycle)
        if packet:
            rx[cycle] = packet
        mirrored = rx.get(cycle - cadence_delay)
        if mirrored is None:
            continue
        # The controller sees device timing up to this cycle.
        if servo is not None and packet:
            frames, stamp = packet
            if rx_ref is None:
                rx_ref = (frames, stamp)
                servo.update(1, 0, 0, 0)
            elif frames - rx_ref[0] >= next_observe:
                servo.update(1, frames - rx_ref[0], stamp - rx_ref[1], tx_correction)
                next_observe = frames - rx_ref[0] + observe_every_frames + \
                    (rng.randint(0, observe_jitter_frames) if observe_jitter_frames else 0)
        # Transmit one DATA packet on this cycle.
        if controller == 'pr191':
            if tx_phase is None:
                tx_phase = (cycle * TICKS_PER_CYCLE + LEAD_TICKS) * Q32
            if servo.hard_resync:
                tx_phase += servo.error
                tx_correction += servo.error
                run.resyncs += 1
                servo.hard_resync = False
            stamps = []
            for _ in range(FRAMES_PER_PACKET):
                stamps.append(tx_phase // Q32)
                tx_phase += servo.step
                tx_correction += servo.step - servo.nominal
                tx_frames += 1
        else:
            first_frame, rx_stamp = mirrored
            replay = rx_stamp + cadence_delay * TICKS_PER_CYCLE
            if controller == 'synth':
                synth.observe(replay)
            else:
                if not synth.ready:
                    synth.seed(cycle * TICKS_PER_CYCLE + LEAD_TICKS, external=True)
                # Device frames covered by the cadence delay, at its own period.
                elapsed = (cadence_delay * TICKS_PER_CYCLE) / (FRAME_TICKS * (1 + params.period_ppm * 1e-6))
                frames_in_delay = round(elapsed / FRAMES_PER_PACKET) * FRAMES_PER_PACKET
                synth.observe(rx_stamp + target_offset + frames_in_delay * FRAME_TICKS)
            stamps = [synth.next_stamp() for _ in range(FRAMES_PER_PACKET)]
        time_s = relative / 8000
        run.leads.append((time_s, stamps[0] - cycle * TICKS_PER_CYCLE))
        if packet:
            offset = stamps[0] - packet[1]
            run.offsets.append((time_s, offset))
            if controller == 'pr191' and not seeded_absolute:
                window.append((time_s, offset))
                if time_s >= pr191_seed_at_s:
                    recent = [o for t, o in window if t >= time_s - pr191_seed_window_s]
                    measured = statistics.median(recent)
                    seed_ticks = round(fold(target_offset - measured, TICKS_PER_CYCLE))
                    frames, stamp = packet
                    servo.update(1, frames - rx_ref[0], stamp - rx_ref[1], tx_correction, seed_ticks=seed_ticks)
                    seeded_absolute = True
    if synth is not None:
        run.resyncs = synth.resyncs
    return run


def score(run, target_offset=VENDOR_OFFSET_TICKS, settle_s=None, after_s=0.0):
    """Lock time and the settled offset error against the measured target."""
    errors = [(t, o - target_offset) for t, o in run.offsets if t >= after_s]
    lock_s = None
    for i, (t, _) in enumerate(errors):
        if all(abs(e) <= LOCK_TICKS for _, e in errors[i:]):
            lock_s = t - after_s
            break
    tail_from = settle_s if settle_s is not None else (errors[-1][0] * 0.5 if errors else 0)
    tail = [e for t, e in errors if t >= tail_from]
    leads = [lead for _, lead in run.leads]
    return {
        'lock_s': lock_s,
        'settled_mean_error_ticks': round(statistics.fmean(tail), 2) if tail else None,
        'settled_max_abs_error_ticks': max(map(abs, tail), default=None),
        'settled_residue_mod512': statistics.median([(e + target_offset) % FRAME_TICKS for e in tail]) if tail else None,
        'tx_lead_min_ticks': min(leads, default=None),
        'tx_lead_max_ticks': max(leads, default=None),
        'first_tx_lead_ticks': leads[0] if leads else None,
        'resyncs': run.resyncs,
    }


SCENARIOS = {
    'steady': {},
    'drift-plus15ppm': {'device': DeviceParams(period_ppm=29.8)},
    'drift-minus15ppm': {'device': DeviceParams(period_ppm=-0.2)},
    'rate-step': {'device': DeviceParams(ppm_after_s=2.0, ppm_after=-15.0)},
    'device-phase-step': {'device': DeviceParams(phase_step_ticks=500, phase_step_at_s=2.0)},
    'noisy-stamps': {'device': DeviceParams(stamp_jitter_ticks=4.0)},
    'late-observations': {'observe_jitter_frames': 64},
}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--seconds', type=float, default=4.0)
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--cadence-delay', type=int, default=4)
    parser.add_argument('--target-residue', type=int, default=0, help='Ticks above 13 frames; the 0.5 s capture held +21')
    args = parser.parse_args()
    target = VENDOR_OFFSET_TICKS + args.target_residue
    report = {}
    for name, overrides in SCENARIOS.items():
        report[name] = {}
        for controller in ('pr191', 'synth', 'restored'):
            run = simulate(controller, args.seconds, args.seed, cadence_delay=args.cadence_delay,
                           target_offset=target, **overrides)
            after = 2.0 if name in ('device-phase-step', 'rate-step') else 0.0
            report[name][controller] = score(run, target, settle_s=max(args.seconds * 0.5, after + 0.5), after_s=after)
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
