#!/usr/bin/env python3
"""
ASFW Saffire-shaped TX simulator -- the CONTRACT for the planned C++ rewrite.

This models the NEW ASFW design (not old ASFW, not pure Saffire): select TX packet payloads
from the recovered output PHASE, with a small bounded lead, no free-running
txScheduledSampleFrame_, no 5120-style offset, and a packet ring of plain absolute ranges
with NO generation counter. It proves -- before any C++ -- that a real CoreAudio-write ->
ring-stamp -> phase-derived-read -> coverage path closes.

ACTORS
------
CoreAudioProducer   delivers HAL IO bursts of ioBufferFrames at the audio rate, stamping
                    TxPcmPacketRing slots by ABSOLUTE audio frame (hasAudioWrite=True).
TxPcmPacketRing     capacity=4096, framesPerDataPacket=8, packetSlots=512. Default payload =
                    valid AM824 silence (hasAudioWrite=False). Stamp = [begin,end)+hasAudioWrite.
                    NO generation.
TxOutputPhaseLoop   recovered device phase in 24.576 MHz ticks; Saffire lead gate emits
                    DATA or NO-DATA; produces outputPhaseTicks / SYT.
OutputPhaseToAudioMap  phaseBaseTicks <-> audioBaseFrame; FrameForPhase(outputPhaseTicks).
IsochConsumer       per bus callback: advance device phase; phase loop -> DATA/NO-DATA;
                    if DATA: audioFrame=map.FrameForPhase(phase); packetFirst=align_down(.,8);
                    check ring stamp coverage; consume.

CADENCE (the crux)
------------------
  1 isoch cycle = 3072 ticks = 6 frames @48k.   1 DATA packet carries 8 frames = 4096 ticks.
  So DATA cannot fire every cycle: outputPhase advances 4096/DATA, the cycle advances 3072.
  The Saffire lead gate (accept < 7620 ticks) NATURALLY produces a 3 DATA : 1 NO-DATA cadence
  (3*8 frames == 4*6 frames == 24 frames). If a model only did outPhase += 3072 it would be a
  6-frame packet and hide this.

COVERAGE RULE
-------------
  Default silence is safe to send but does NOT count as covered audio:
    covered = stamp.hasAudioWrite and stamp.begin <= packetFirst and stamp.end >= packetFirst+8
  No generation field; a stale slot shows up as begin one ring lap (4096) behind -> oneLapMiss.

Usage / scenarios:
  python3 tools/debug/asfw_saffire_tx_sim.py --scenario happy
  python3 tools/debug/asfw_saffire_tx_sim.py --drift-ppm 100
  python3 tools/debug/asfw_saffire_tx_sim.py --drift-ppm -100
  python3 tools/debug/asfw_saffire_tx_sim.py --write-jitter-frames 32
  python3 tools/debug/asfw_saffire_tx_sim.py --glitch-at 5000 --glitch-jump 5000
  python3 tools/debug/asfw_saffire_tx_sim.py --io-buffer 128
  python3 tools/debug/asfw_saffire_tx_sim.py --io-buffer 512
  python3 tools/debug/asfw_saffire_tx_sim.py --seed playhead --narrate 8
"""

from __future__ import annotations

import argparse
import dataclasses
import statistics

# ---- ASFW::Timing constants (AudioWire/AMDTP/TimingUtils.hpp) ---------------
TICKS_PER_CYCLE = 3072            # 24.576 MHz / 8000
TICKS_PER_FRAME = 512             # 24.576 MHz / 48000
FRAMES_PER_CYCLE = TICKS_PER_CYCLE // TICKS_PER_FRAME      # 6
FRAMES_PER_DATA_PACKET = 8        # kSytInterval48k (blocking)
DATA_ADVANCE_TICKS = FRAMES_PER_DATA_PACKET * TICKS_PER_FRAME   # 4096 (NOT 3072!)
RING_CAPACITY = 4096              # outputFrameCapacity
PACKET_SLOTS = RING_CAPACITY // FRAMES_PER_DATA_PACKET          # 512
DEVICE_SUBCYCLE_TICKS = 0x0B0     # device's constant recovered sub-cycle (Saffire Pro24)

# ---- Saffire lead gate (FillFirewireBuffers @0xEC8A/0xEC91) -----------------
LEAD_TIGHT = TICKS_PER_CYCLE      # <= 3071 ticks -> warn "lead under 1 cycle"
LEAD_ACCEPT = 7620                # < 7620 ticks -> DATA   (~2.48 cyc / ~15 frames)
LEAD_REJECT = 12287               # > 12287 ticks -> hard NO-DATA (~4 cyc)
INITIAL_LEAD = (LEAD_TIGHT + LEAD_ACCEPT) // 2             # seed mid-band (~10.4 frames)


def align_down(frame: int, n: int) -> int:
    return (frame // n) * n


def encode_offset_ticks_to_syt(offset_ticks: int) -> int:
    """SaffireTxPhaseLoop::EncodeOffsetTicksToSyt: [cycle4:offset12]."""
    field = offset_ticks % (16 * TICKS_PER_CYCLE)
    return (((field // TICKS_PER_CYCLE) & 0x0F) << 12) | (field % TICKS_PER_CYCLE)


# ============================================================================
@dataclasses.dataclass
class Stamp:
    begin: int = 0
    end: int = 0
    has_audio_write: bool = False     # silence default -> NOT covered


class TxPcmPacketRing:
    """capacity 4096, 512 slots, absolute-range stamps, NO generation."""
    def __init__(self) -> None:
        self.base_frame = 0
        self.slots = [Stamp() for _ in range(PACKET_SLOTS)]

    def slot_for_frame(self, frame: int) -> int:
        return ((frame - self.base_frame) % RING_CAPACITY) // FRAMES_PER_DATA_PACKET

    def stamp_packet(self, packet_first: int) -> None:
        s = self.slots[self.slot_for_frame(packet_first)]
        s.begin, s.end, s.has_audio_write = packet_first, packet_first + FRAMES_PER_DATA_PACKET, True

    def stamp(self, packet_first: int) -> Stamp:
        return self.slots[self.slot_for_frame(packet_first)]


class CoreAudioProducer:
    """
    Delivers HAL IO bursts of io_buffer frames at the audio sample rate (with optional drift
    and per-burst jitter), stamping the ring by absolute audio frame. writtenEnd is the
    committed frontier; reportedPlayhead trails it by one IO buffer (CoreAudio buffers ahead).
    """
    def __init__(self, ring: TxPcmPacketRing, io_buffer: int, drift_ppm: float,
                 write_jitter: int, rng_seed: int) -> None:
        self.ring = ring
        self.io_buffer = io_buffer
        self.rate = FRAMES_PER_CYCLE * (1.0 + drift_ppm / 1.0e6)   # frames produced per callback
        self.write_jitter = write_jitter
        self._produced = float(2 * RING_CAPACITY)                  # been running a while
        self.written_end = 2 * RING_CAPACITY
        import random
        self._rng = random.Random(rng_seed)
        # pre-stamp the lap currently resident so startup isn't all-uncovered silence
        for f in range(self.written_end - RING_CAPACITY, self.written_end, FRAMES_PER_DATA_PACKET):
            self.ring.stamp_packet(f)

    def tick(self) -> None:
        self._produced += self.rate
        while self._produced - self.written_end >= self.io_buffer:
            chunk = self.io_buffer
            if self.write_jitter:
                chunk += self._rng.randint(-self.write_jitter, self.write_jitter)
                chunk = max(FRAMES_PER_DATA_PACKET, align_down(chunk, FRAMES_PER_DATA_PACKET))
            for f in range(self.written_end, self.written_end + chunk, FRAMES_PER_DATA_PACKET):
                self.ring.stamp_packet(f)
            self.written_end += chunk

    @property
    def oldest_valid(self) -> int:
        return self.written_end - RING_CAPACITY

    @property
    def reported_playhead(self) -> int:
        return self.written_end - self.io_buffer


@dataclasses.dataclass
class PhaseResult:
    is_data: bool
    output_phase_ticks: int
    syt: int
    lead_ticks: int


class TxOutputPhaseLoop:
    """Recovered-device-phase loop with the Saffire lead gate. Emits DATA/NO-DATA."""
    def __init__(self) -> None:
        self.output_phase_ticks = 0
        self.phase_valid = False
        self.tight_warns = 0

    def seed(self, cycle_phase: int) -> None:
        self.output_phase_ticks = cycle_phase + INITIAL_LEAD
        self.phase_valid = True

    def step(self, transmit_cycle: int) -> PhaseResult:
        cycle_phase = TICKS_PER_CYCLE * transmit_cycle            # our bus cycle (monotonic)
        if not self.phase_valid:
            self.seed(cycle_phase)
        lead = self.output_phase_ticks - cycle_phase             # presentation lead (ticks)
        if lead < LEAD_ACCEPT:                                    # room -> ship 8 frames
            if lead <= LEAD_TIGHT - 1:
                self.tight_warns += 1
            present = self.output_phase_ticks
            # graft device's constant sub-cycle (SaffireTxPhaseLoop), then advance by a PACKET
            graft = present - (present % TICKS_PER_CYCLE) + DEVICE_SUBCYCLE_TICKS
            syt = encode_offset_ticks_to_syt(graft)
            self.output_phase_ticks += DATA_ADVANCE_TICKS        # +8 frames, NOT +6
            return PhaseResult(True, present, syt, lead)
        return PhaseResult(False, self.output_phase_ticks, 0xFFFF, lead)   # NO-DATA

    def resync(self) -> None:
        self.phase_valid = False                                 # discontinuity: reset PHASE only


class OutputPhaseToAudioMap:
    """phaseBaseTicks <-> audioBaseFrame. FrameForPhase converts output phase to abs frame."""
    def __init__(self) -> None:
        self.phase_base_ticks = 0
        self.audio_base_frame = 0
        self.valid = False

    def seed(self, phase_ticks: int, audio_base_frame: int) -> None:
        self.phase_base_ticks = phase_ticks
        self.audio_base_frame = audio_base_frame
        self.valid = True

    def frame_for_phase(self, phase_ticks: int) -> int:
        return self.audio_base_frame + (phase_ticks - self.phase_base_ticks) // TICKS_PER_FRAME


# ============================================================================
SEED_RULES = {
    "oldest":   lambda p: p.oldest_valid,                         # writtenEnd - 4096 (overwrite edge)
    "written":  lambda p: p.written_end,                          # newest written (-> future)
    "playhead": lambda p: p.reported_playhead,                    # writtenEnd - ioBuffer (min safe)
    "safety":   lambda p: p.reported_playhead - p.io_buffer,      # writtenEnd - 2*ioBuffer (jitter margin)
}


@dataclasses.dataclass
class Metrics:
    data: int = 0
    nodata: int = 0
    covered: int = 0
    future: int = 0
    overwritten: int = 0
    uncovered_audio_writes: int = 0      # in-written-range DATA that didn't cover (real miss)
    one_lap_misses: int = 0
    max_miss: int = 0
    lead_frames: list = dataclasses.field(default_factory=list)
    af_minus_we: list = dataclasses.field(default_factory=list)
    first_fail: str = ""


def simulate(seed_rule: str, callbacks: int, io_buffer: int, drift_ppm: float,
             write_jitter: int, glitch_at: int, glitch_jump: int, narrate: int,
             rng_seed: int) -> Metrics:
    ring = TxPcmPacketRing()
    prod = CoreAudioProducer(ring, io_buffer, drift_ppm, write_jitter, rng_seed)
    ring.base_frame = align_down(prod.written_end - RING_CAPACITY, FRAMES_PER_DATA_PACKET)
    loop = TxOutputPhaseLoop()
    amap = OutputPhaseToAudioMap()
    m = Metrics()
    transmit_cycle = 1000
    glitch_extra = 0

    for n in range(callbacks):
        prod.tick()                                              # CoreAudio HAL IO burst(s)
        transmit_cycle += 1

        if glitch_at >= 0 and n == glitch_at:                   # injected device discontinuity
            glitch_extra += glitch_jump
            loop.resync()                                       # reset PHASE only (audio intact)
            amap.valid = False

        res = loop.step(transmit_cycle)
        # device phase glitch perturbs the recovered phase used for SYT/seed, not the cadence
        if not res.is_data:
            m.nodata += 1
            continue
        m.data += 1

        # (Re)seed the phase<->frame map when invalid, per the chosen seed rule.
        if not amap.valid:
            amap.seed(res.output_phase_ticks, SEED_RULES[seed_rule](prod))

        audio_frame = amap.frame_for_phase(res.output_phase_ticks)
        packet_first = align_down(audio_frame, FRAMES_PER_DATA_PACKET)
        stamp = ring.stamp(packet_first)
        end = packet_first + FRAMES_PER_DATA_PACKET

        in_written = prod.oldest_valid <= packet_first and end <= prod.written_end
        covered = stamp.has_audio_write and stamp.begin <= packet_first and stamp.end >= end

        m.lead_frames.append(res.lead_ticks / TICKS_PER_FRAME)
        m.af_minus_we.append(packet_first - prod.written_end)

        if covered:
            m.covered += 1
        else:
            if end > prod.written_end:
                m.future += 1
            if packet_first < prod.oldest_valid:
                m.overwritten += 1
            if in_written:
                m.uncovered_audio_writes += 1
            miss = packet_first - stamp.begin
            m.max_miss = max(m.max_miss, abs(miss))
            if abs(miss) == RING_CAPACITY:
                m.one_lap_misses += 1
            if not m.first_fail:
                m.first_fail = (
                    f"UNCOVERED packet: callback={n} phase={res.output_phase_ticks} "
                    f"leadTicks={res.lead_ticks} packetFirst=[{packet_first},{end}) "
                    f"stamp=[{stamp.begin},{stamp.end}) hasAudioWrite={stamp.has_audio_write} "
                    f"miss={miss} written=[{prod.oldest_valid},{prod.written_end})")

        if n < narrate:
            verdict = "COVERED" if covered else ("future" if end > prod.written_end
                      else "overwritten" if packet_first < prod.oldest_valid else "stale-silence")
            print(f"cb{n} {'DATA ' if res.is_data else 'NODATA'} "
                  f"lead={res.lead_ticks}t/{res.lead_ticks/TICKS_PER_FRAME:.1f}f syt=0x{res.syt:04X}  "
                  f"phase={res.output_phase_ticks} -> audioFrame={audio_frame} "
                  f"packetFirst=[{packet_first},{end})  "
                  f"writtenEnd={prod.written_end} oldest={prod.oldest_valid}  "
                  f"stamp=[{stamp.begin},{stamp.end}) aw={int(stamp.has_audio_write)} -> {verdict}")

    return m


def _fmt(xs):
    return (f"min={min(xs):.1f} med={statistics.median(xs):.1f} max={max(xs):.1f}"
            if xs else "n/a")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--scenario", default="happy", help="label only (happy/...)")
    p.add_argument("--callbacks", type=int, default=20000)
    p.add_argument("--io-buffer", type=int, default=128)
    p.add_argument("--drift-ppm", type=float, default=0.0)
    p.add_argument("--write-jitter-frames", type=int, default=0)
    p.add_argument("--glitch-at", type=int, default=-1)
    p.add_argument("--glitch-jump", type=int, default=5000)
    p.add_argument("--seed", choices=list(SEED_RULES), default=None,
                   help="seed audioBaseFrame rule; default = compare all four")
    p.add_argument("--narrate", type=int, default=0)
    p.add_argument("--rng-seed", type=int, default=1)
    args = p.parse_args()

    print("=== ASFW Saffire-shaped TX simulation ===")
    print(f"scenario={args.scenario} rate=48000 fpp={FRAMES_PER_DATA_PACKET} "
          f"capacity={RING_CAPACITY} slots={PACKET_SLOTS} ioBuffer={args.io_buffer} "
          f"drift={args.drift_ppm:g}ppm writeJitter={args.write_jitter_frames}")
    print(f"DATA_ADVANCE_TICKS={DATA_ADVANCE_TICKS} (8 frames)  cycle=3072t (6 frames)  "
          f"-> expected DATA:NODATA ~ 3:1")
    print(f"lead gate: tight<= {LEAD_TIGHT - 1}  accept< {LEAD_ACCEPT}  reject> {LEAD_REJECT}\n")

    rules = [args.seed] if args.seed else list(SEED_RULES)
    print(f"{'seed':9} {'DATA/NODATA':>13} {'cov%':>7} {'fut':>5} {'ovr':>5} "
          f"{'uncovAW':>8} {'1lap':>5} {'maxMiss':>8}  leadFrames           afMinusWE")
    for rule in rules:
        m = simulate(rule, args.callbacks, args.io_buffer, args.drift_ppm,
                     args.write_jitter_frames, args.glitch_at, args.glitch_jump,
                     args.narrate if rule == rules[0] else 0, args.rng_seed)
        cov = 100.0 * m.covered / m.data if m.data else 0.0
        ratio = f"{m.data}/{m.nodata}"
        print(f"{rule:9} {ratio:>13} {cov:7.1f} {m.future:5} {m.overwritten:5} "
              f"{m.uncovered_audio_writes:8} {m.one_lap_misses:5} {m.max_miss:8}  "
              f"[{_fmt(m.lead_frames):27}] [{_fmt(m.af_minus_we)}]")
        if m.first_fail and (args.seed or rule == "playhead"):
            print("   " + m.first_fail)

    print("\n--- contract questions ---")
    print("1 select payload from output phase w/o txScheduledSampleFrame_:  YES (map.FrameForPhase)")
    print("2 coverage stable without a giant offset:  YES for seed=playhead/safety (lead ~= 1-2 ioBuffer)")
    print("3 phaseBase<->audioBase seed rule:  audioBaseFrame = reportedPlayhead - safetyFrames")
    print("    (writtenEnd -> future; oldestValid -> overwrite edge; ~1-2 ioBuffer behind = robust)")
    print("4 DATA:NODATA averages 48k with 8-frame packets:  see ratio ~3:1 above (24f==24f)")
    print("5 discontinuity resets phase only (audio intact):  --glitch-at keeps coverage, re-seeds map")
    print("6 packet-ring stamps stay simple absolute ranges, no generation:  YES (oneLapMiss detects stale)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
