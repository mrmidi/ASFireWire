#!/usr/bin/env python3
"""
ASFW Timing Geometry and Callback Simulator.

Validates:
1. Static Geometry alignment (deterministic divisibility checks).
2. CoreAudio ADK IOOperationHandler callback scheduling under jitter,
   enforcing safety offsets, and utilizing an interval-based generational
   FrameRing model to detect capture starvations and playback overwrites.
3. Candidate geometry searching to discover optimal buffer settings.
"""

from __future__ import annotations

import random
from dataclasses import dataclass
from fractions import Fraction
from math import gcd
from typing import List, Tuple, Sequence

DATA_FRAMES = 8
SAMPLE_RATE = 48_000
FIREWIRE_PACKET_RATE = 8_000
PACKET_US = 125
CADENCE = (DATA_FRAMES, DATA_FRAMES, DATA_FRAMES, 0)  # D, D, D, N

# -----------------------------------------------------------------------------
# FrameRing: Generational Ring Buffer
# -----------------------------------------------------------------------------
class FrameRing:
    """
    Simulates a circular buffer by tracking write/read generations per frame.
    Helps detect read-before-write (starvation) and write-before-read (overwrite).
    """
    def __init__(self, size: int):
        self.size = size
        self.valid_generation = [-1] * size

    def reset(self):
        self.valid_generation = [-1] * self.size

    def write_span(self, start_frame: int, count: int, generation: int) -> None:
        for f in range(start_frame, start_frame + count):
            gen = generation
            # If the span crosses a ring boundary, adjust the generation index for wrapped frames
            if (f // self.size) != (start_frame // self.size):
                gen = f // self.size
            self.valid_generation[f % self.size] = gen

    def check_readable(self, start_frame: int, count: int) -> bool:
        for f in range(start_frame, start_frame + count):
            if f < 0:
                continue
            expected_generation = f // self.size
            if self.valid_generation[f % self.size] < expected_generation:
                return False
        return True


# -----------------------------------------------------------------------------
# Geometry Definition
# -----------------------------------------------------------------------------
@dataclass(frozen=True)
class Geometry:
    name: str
    irq_packets: int
    zts_period_frames: int
    io_period_frames: int
    ring_frames: int
    output_safety_frames: int = 192
    input_safety_frames: int = 448
    dma_packets: int = 512
    cadence_phase: int = 0
    groups_to_simulate: int = 128
    jitter_std_us: float = 150.0

    @property
    def frames_per_irq(self) -> int:
        frames = 0
        for p in range(self.irq_packets):
            frames += CADENCE[(p + self.cadence_phase) % len(CADENCE)]
        return frames

    @property
    def irq_duration_ms(self) -> float:
        return self.irq_packets * PACKET_US / 1000.0


@dataclass
class SimResult:
    geometry: Geometry
    errors: list[str]
    warnings: list[str]
    irq_frame_advances: list[int]
    zts_anchors_emitted: int
    zts_anchors_per_irq: list[int]
    callbacks_run: int
    playback_underruns: int
    capture_starvations: int
    ring_overwrites: int
    min_tx_lead_frames: int
    max_tx_lead_frames: int


# -----------------------------------------------------------------------------
# Static Geometry Validation
# -----------------------------------------------------------------------------
def validate_static_geometry(g: Geometry) -> Tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []

    for name in ("irq_packets", "zts_period_frames", "io_period_frames", "ring_frames"):
        if getattr(g, name) <= 0:
            errors.append(f"{name} must be positive")

    if errors:
        return errors, warnings

    if g.irq_packets % len(CADENCE) != 0:
        errors.append(
            f"IRQ group packets ({g.irq_packets}) must be a multiple of {len(CADENCE)} "
            "to prevent cutting through the D/D/D/N blocking cadence."
        )

    if g.dma_packets % g.irq_packets != 0:
        errors.append(
            f"DMA packet ring ({g.dma_packets}) is not an integer multiple of "
            f"IRQ group packets ({g.irq_packets})."
        )

    if g.ring_frames % g.zts_period_frames != 0:
        errors.append(
            f"ring_frames ({g.ring_frames}) is not divisible by zts_period_frames ({g.zts_period_frames})."
        )

    if g.ring_frames % g.io_period_frames != 0:
        errors.append(
            f"ring_frames ({g.ring_frames}) is not divisible by io_period_frames ({g.io_period_frames})."
        )

    frames_per_irq = g.frames_per_irq
    if g.zts_period_frames != frames_per_irq:
        warnings.append(
            f"ZTS period ({g.zts_period_frames}) != frames/IRQ ({frames_per_irq}). "
            "Requires crossed-grid ZTS anchor interpolation."
        )

    if g.io_period_frames != g.zts_period_frames:
        warnings.append(
            f"IO period ({g.io_period_frames}) != ZTS period ({g.zts_period_frames})."
        )

    return errors, warnings


# -----------------------------------------------------------------------------
# Dynamic Callback Simulator
# -----------------------------------------------------------------------------
def simulate(g: Geometry, callback_sizes: Sequence[int] | None = None) -> SimResult:
    errors, warnings = validate_static_geometry(g)
    if errors:
        return SimResult(g, errors, warnings, [], 0, [], 0, 0, 0, 0, 0, 0)

    # Simulation setup
    random.seed(42)
    total_packets = g.groups_to_simulate * g.irq_packets
    
    # Rings
    rx_ring = FrameRing(g.ring_frames)
    tx_ring = FrameRing(g.ring_frames)
    
    # State tracking
    rx_hw_frame_cursor = 0
    tx_hw_frame_cursor = 0
    
    zts_anchors: list[Tuple[int, Fraction]] = [(0, Fraction(0))]  # Prime
    next_zts_frame = g.zts_period_frames
    
    client_sample_time = 0
    client_playback_written_end = 0
    
    playback_underruns = 0
    capture_starvations = 0
    ring_overwrites = 0
    callbacks_run = 0
    
    min_tx_lead = 999999
    max_tx_lead = -999999

    zts_anchors_per_irq: list[int] = []
    irq_frame_advances: list[int] = []

    # Setup safety offsets
    output_safety_us = Fraction(g.output_safety_frames * 1_000_000, SAMPLE_RATE)
    
    # Primary variables for tracking callbacks
    cb_index = 0
    next_client_wakeup_us = Fraction(0)
    pending_wakeup_us: Fraction | None = None
    
    # Group frames accumulator for correct frame advance reporting
    accumulated_group_frames = 0

    # Prime the write ring with initial output silence
    tx_ring.write_span(0, g.output_safety_frames, generation=0)
    client_playback_written_end = g.output_safety_frames

    for p_idx in range(total_packets):
        time_us = p_idx * PACKET_US
        
        # HW Receive & Transmit advance
        frames = CADENCE[(p_idx + g.cadence_phase) % len(CADENCE)]
        
        # 1. HW Receive: Write incoming packet payload into RX ring
        if frames > 0:
            generation = rx_hw_frame_cursor // g.ring_frames
            rx_ring.write_span(rx_hw_frame_cursor, frames, generation)
            rx_hw_frame_cursor += frames
        
        # 2. HW Transmit: Read outgoing packet payload from TX ring
        if frames > 0:
            if not tx_ring.check_readable(tx_hw_frame_cursor, frames):
                playback_underruns += 1
            tx_hw_frame_cursor += frames
            
            lead = client_playback_written_end - tx_hw_frame_cursor
            min_tx_lead = min(min_tx_lead, lead)
            max_tx_lead = max(max_tx_lead, lead)

        # Accumulate frames for this IRQ group
        accumulated_group_frames += frames

        # 3. Check for hardware interrupt (Timing Group Boundary)
        if (p_idx + 1) % g.irq_packets == 0:
            interrupt_host_us = Fraction((p_idx + 1) * PACKET_US)
            
            # Record frame advance for this IRQ group
            irq_frame_advances.append(accumulated_group_frames)
            
            # ZTS Anchor publication
            if g.zts_period_frames == g.frames_per_irq:
                # Aligned Mode: publish strictly at the end of the group
                zts_anchors.append((rx_hw_frame_cursor, interrupt_host_us))
            else:
                # Crossed-grid interpolation mode
                while next_zts_frame <= rx_hw_frame_cursor:
                    # Linearly interpolate between the start and end of the group
                    group_start_frame = rx_hw_frame_cursor - accumulated_group_frames
                    group_start_host = interrupt_host_us - Fraction(g.irq_packets * PACKET_US)
                    
                    progress = Fraction(next_zts_frame - group_start_frame, accumulated_group_frames)
                    interpolated_host = group_start_host + progress * Fraction(g.irq_packets * PACKET_US)
                    
                    zts_anchors.append((next_zts_frame, interpolated_host))
                    next_zts_frame += g.zts_period_frames

            # Reset group accumulator
            accumulated_group_frames = 0

        # 4. CoreAudio Callback Scheduler
        # Each wakeup gets exactly ONE jitter draw, fixed when the wakeup is
        # armed. (Re-drawing per packet while waiting and clamping monotonic
        # ratcheted the wake time up by max-of-draws plus 10 µs per packet —
        # an artifact that fabricated underruns for large IO periods.)
        callbacks_this_packet = 0
        while True:
            # Resolve next callback size
            if callback_sizes:
                current_cb_size = callback_sizes[cb_index % len(callback_sizes)]
            else:
                current_cb_size = g.io_period_frames

            if pending_wakeup_us is None:
                # Arm the next wakeup: ideal time from the latest anchor,
                # one jitter draw, never earlier than the previous wakeup.
                latest_anchor_frame, latest_anchor_host = zts_anchors[-1]
                ideal_playback_time_us = latest_anchor_host + Fraction(
                    (client_sample_time - latest_anchor_frame) * 1_000_000, SAMPLE_RATE
                )
                jitter_us = Fraction(random.gauss(0.0, g.jitter_std_us))
                pending_wakeup_us = max(
                    ideal_playback_time_us - output_safety_us + jitter_us,
                    next_client_wakeup_us + Fraction(10),
                )
            wakeup_host_us = pending_wakeup_us

            # Check if this wakeup falls in the current packet interval [time_us, time_us + PACKET_US)
            if wakeup_host_us < (time_us + PACKET_US):
                pending_wakeup_us = None
                callbacks_this_packet += 1
                if callbacks_this_packet > 4:
                    errors.append("scheduler runaway")
                    break

                callbacks_run += 1
                
                # --- IOOperationHandler::BeginRead ---
                # Client reads input span
                target_read_start = client_sample_time - g.input_safety_frames
                if target_read_start >= 0:
                    if not rx_ring.check_readable(target_read_start, current_cb_size):
                        capture_starvations += 1

                # --- IOOperationHandler::WriteEnd ---
                # Client writes output span
                write_generation = client_sample_time // g.ring_frames
                
                # Verify that client does not overwrite yet-to-be-read hardware data
                # i.e. client_sample_time + current_cb_size - tx_hw_frame_cursor <= g.ring_frames
                if client_sample_time + current_cb_size - tx_hw_frame_cursor > g.ring_frames:
                    ring_overwrites += 1
                
                tx_ring.write_span(client_sample_time, current_cb_size, write_generation)
                client_playback_written_end = max(client_playback_written_end, client_sample_time + current_cb_size)

                # Advance timeline
                client_sample_time += current_cb_size
                cb_index += 1
                next_client_wakeup_us = wakeup_host_us
            else:
                break
                
        if errors:
            break

    if min_tx_lead == 999999:
        min_tx_lead = 0
    if max_tx_lead == -999999:
        max_tx_lead = 0

    if playback_underruns > 0:
        errors.append(f"Detected {playback_underruns} playback underruns.")

    return SimResult(
        geometry=g,
        errors=errors,
        warnings=warnings,
        irq_frame_advances=irq_frame_advances,
        zts_anchors_emitted=len(zts_anchors) - 1,
        zts_anchors_per_irq=zts_anchors_per_irq,
        callbacks_run=callbacks_run,
        playback_underruns=playback_underruns,
        capture_starvations=capture_starvations,
        ring_overwrites=ring_overwrites,
        min_tx_lead_frames=min_tx_lead,
        max_tx_lead_frames=max_tx_lead,
    )


# -----------------------------------------------------------------------------
# Candidate Search
# -----------------------------------------------------------------------------
def find_candidates() -> None:
    print("========================================================================")
    print(" CANDIDATE GEOMETRIES FOR 48kHz BLOCKING MODE")
    print("========================================================================")
    print(f"{'Packets/Group':<15} | {'Frames/Group':<15} | {'ZTS Period':<12} | {'Ring Size':<10} | {'Safety Out':<10} | {'Safety In'}")
    print("-" * 85)
    
    # Candidates must have packets/group divisible by 4 (cadence phase locked)
    for irq_packets in range(4, 129, 4):
        # Calculate frames advanced for these packets
        full, rem = divmod(irq_packets, len(CADENCE))
        frames_per_irq = full * sum(CADENCE) + sum(CADENCE[:rem])
        
        # Ring sizes (multiples of 32 for alignment)
        for ring in [384, 512, 768, 1024, 1152, 1536, 2048]:
            if ring % frames_per_irq == 0:
                # Target ZTS Period is frames_per_irq to keep it 1:1
                zts = frames_per_irq
                
                # Rule: input safety >= output safety + 2 * ZTS period + jitter margin
                safety_out = zts
                safety_in = safety_out + 2 * zts + 64
                
                # Verify that safety in / out sizes and ZTS periods fit in the ring without overwrites or stale reads
                if safety_out + zts <= ring and safety_in <= ring:
                    print(f"{irq_packets:<15} | {frames_per_irq:<15} | {zts:<12} | {ring:<10} | {safety_out:<10} | {safety_in}")


# -----------------------------------------------------------------------------
# Printing Results
# -----------------------------------------------------------------------------
def print_result(r: SimResult) -> None:
    g = r.geometry
    print("=" * 88)
    print(g.name)
    print("=" * 88)
    print(f"IRQ packets:          {g.irq_packets}")
    print(f"Frames per IRQ:       {g.frames_per_irq} ({g.irq_duration_ms:.2f} ms)")
    print(f"ZTS period:           {g.zts_period_frames} frames")
    print(f"IO period:            {g.io_period_frames} frames")
    print(f"Frame ring:           {g.ring_frames} frames")
    print()
    print(f"--- IOOperationHandler Simulation (Jitter StdDev: {g.jitter_std_us} µs) ---")
    print(f"Client IO Callbacks:  {r.callbacks_run}")
    print(f"Playback Underruns:   {r.playback_underruns}")
    print(f"Capture Starvations:  {r.capture_starvations} (Input safety: {g.input_safety_frames} frames)")
    print(f"Ring Overwrites:      {r.ring_overwrites}")
    print(f"TX Buffer Lead Range: [{r.min_tx_lead_frames} .. {r.max_tx_lead_frames}] frames")
    print()

    if r.errors:
        print("ERRORS:")
        for e in r.errors:
            print(f"  - {e}")
    else:
        print("ERRORS: none")

    if r.warnings:
        print("WARNINGS:")
        for w in r.warnings:
            print(f"  - {w}")
    else:
        print("WARNINGS: none")

    print()


# -----------------------------------------------------------------------------
# Assertion-based Testing
# -----------------------------------------------------------------------------
def run_tests() -> None:
    # 1. Aligned 192-frame case must pass completely
    aligned_192 = Geometry(
        name="CLEAN ALIGNED: 32 packets / 192 ZTS / 768 ring",
        irq_packets=32,
        zts_period_frames=192,
        io_period_frames=192,
        ring_frames=768,
        output_safety_frames=192,
        input_safety_frames=448, # output_safety (192) + io_period (192) + jitter margin (64)
    )
    res = simulate(aligned_192)
    assert not res.errors, f"Aligned 192 failed with errors: {res.errors}"
    assert res.playback_underruns == 0
    assert res.capture_starvations == 0
    assert res.ring_overwrites == 0

    # 2. Low latency 96-frame case must pass completely
    low_latency_96 = Geometry(
        name="EXPERIMENTAL LOW-LATENCY: 16 packets / 96 ZTS / 768 ring",
        irq_packets=16,
        zts_period_frames=96,
        io_period_frames=96,
        ring_frames=768,
        output_safety_frames=96,
        input_safety_frames=224, # output_safety (96) + io_period (96) + jitter margin (32)
    )
    res = simulate(low_latency_96)
    assert not res.errors, f"Low latency 96 failed with errors: {res.errors}"
    assert res.playback_underruns == 0
    assert res.capture_starvations == 0
    assert res.ring_overwrites == 0

    # 3. Bad Old Mix must fail static validation
    bad_old_mix = Geometry(
        name="BAD OLD MIX: 8 packets / 48 ZTS / 512 ring",
        irq_packets=8,
        zts_period_frames=48,
        io_period_frames=48,
        ring_frames=512,
        output_safety_frames=48,
        input_safety_frames=128,
    )
    res = simulate(bad_old_mix)
    assert len(res.errors) > 0, "Bad Old Mix did not report divisibility errors"

    # 4. Safe baseline must produce warning about ZTS period
    safe_baseline = Geometry(
        name="SAFE BASELINE: 8 packets / 512 ZTS / 512 ring",
        irq_packets=8,
        zts_period_frames=512,
        io_period_frames=512,
        ring_frames=512,
        output_safety_frames=512,
        input_safety_frames=1088,
    )
    res = simulate(safe_baseline)
    assert not res.errors
    assert any("crossed-grid" in w for w in res.warnings), "Safe baseline missed interpolation warning"

    # 5. Variable size callbacks testing
    var_geom = Geometry(
        name="VARIABLE CALLBACKS: 32 packets / 192 ZTS / 768 ring",
        irq_packets=32,
        zts_period_frames=192,
        io_period_frames=192,
        ring_frames=768,
        output_safety_frames=192,
        input_safety_frames=544, # output_safety (192) + max_cb_size (288) + jitter margin (64)
    )
    res_var = simulate(var_geom, callback_sizes=[96, 192, 288])
    assert not res_var.errors
    assert res_var.playback_underruns == 0
    assert res_var.capture_starvations == 0

    # 6. Saffire profile test case (Decompiled kext properties): 1.33 ms (64 frames) period, 48 frames out safety, 128 frames in safety
    saffire_64 = Geometry(
        name="Saffire Profile (64 frames period): 8 packets / 48 ZTS / 768 ring",
        irq_packets=8,
        zts_period_frames=48,
        io_period_frames=64,
        ring_frames=768,
        output_safety_frames=48,
        input_safety_frames=128,
        jitter_std_us=100.0,
    )
    res_saffire = simulate(saffire_64)
    assert not res_saffire.errors
    assert res_saffire.playback_underruns == 0
    assert res_saffire.capture_starvations == 0

    # 7. ASFW TARGET geometry (AudioTimingGeometry.hpp): 32-packet IRQ groups,
    #    ZTS 192 == per-interrupt advance (aligned 1:1, the interrupt IS the
    #    ZTS callback), ring 1536 (divisible by 192 and by max IO 512).
    #    7a. Worst-case client (512-frame IO):
    #        in_safety = out(48) + io(512) + 64 per the duplex equation.
    asfw_target_max_io = Geometry(
        name="ASFW TARGET (max IO): 32 packets / 192 ZTS / 512 IO / 1536 ring",
        irq_packets=32,
        zts_period_frames=192,
        io_period_frames=512,
        ring_frames=1536,
        output_safety_frames=48,
        input_safety_frames=624,
    )
    res_target = simulate(asfw_target_max_io)
    assert not res_target.errors, f"ASFW target (max IO) failed: {res_target.errors}"
    # ZTS == frames/IRQ (aligned); the only acceptable warning is the client's
    # own IO size differing from the ZTS period.
    assert not any("crossed-grid" in w for w in res_target.warnings), res_target.warnings
    assert res_target.playback_underruns == 0
    assert res_target.capture_starvations == 0
    assert res_target.ring_overwrites == 0

    #    7b. Typical pro-audio client (64-frame IO) with the declared driver
    #        offsets: out = 48 (Saffire profile), in = 256 (driver floor: one
    #        interrupt group 192 + 64 jitter; profile's 128 assumed Saffire's
    #        own 1.5 ms groups).
    asfw_target_typical = Geometry(
        name="ASFW TARGET (typical 64-frame client): 32 packets / 192 ZTS / 1536 ring",
        irq_packets=32,
        zts_period_frames=192,
        io_period_frames=64,
        ring_frames=1536,
        output_safety_frames=48,
        input_safety_frames=256,
        jitter_std_us=100.0,
    )
    res_typical = simulate(asfw_target_typical)
    assert not res_typical.errors, f"ASFW target (typical) failed: {res_typical.errors}"
    assert res_typical.playback_underruns == 0
    assert res_typical.capture_starvations == 0
    assert res_typical.ring_overwrites == 0

    # 8. Cadence phase testing: phase 1, 2, 3 must yield same aligned advances for 32-packet groups
    for phase in (1, 2, 3):
        aligned_phase = Geometry(
            name=f"CLEAN ALIGNED PHASE {phase}: 32 packets / 192 ZTS / 768 ring",
            irq_packets=32,
            zts_period_frames=192,
            io_period_frames=192,
            ring_frames=768,
            output_safety_frames=192,
            input_safety_frames=448,
            cadence_phase=phase
        )
        res_phase = simulate(aligned_phase)
        assert not res_phase.errors
        assert res_phase.geometry.frames_per_irq == 192, f"Phase {phase} broke 192-frame group advance"

    print("All simulator validation checks passed successfully!\n")


def run_all() -> None:
    cases = [
        Geometry(
            name="BAD OLD MIX: 8 packets (48 frames) IRQ / 48 ZTS / 512 ring",
            irq_packets=8,
            zts_period_frames=48,
            io_period_frames=48,
            ring_frames=512,
            output_safety_frames=48,
            input_safety_frames=128, # output_safety (48) + io_period (48) + jitter margin (32)
        ),
        Geometry(
            name="SAFE BASELINE: 8 packets (48 frames) IRQ / 512 ZTS / 512 ring",
            irq_packets=8,
            zts_period_frames=512,
            io_period_frames=512,
            ring_frames=512,
            output_safety_frames=512,
            input_safety_frames=1088, # output_safety (512) + io_period (512) + jitter margin (64)
        ),
        Geometry(
            name="CLEAN ALIGNED: 32 packets (192 frames) IRQ / 192 ZTS / 768 ring",
            irq_packets=32,
            zts_period_frames=192,
            io_period_frames=192,
            ring_frames=768,
            output_safety_frames=192,
            input_safety_frames=448, # output_safety (192) + io_period (192) + jitter margin (64)
        ),
        Geometry(
            name="CLEAN CONSERVATIVE: 64 packets (384 frames) IRQ / 384 ZTS / 768 ring",
            irq_packets=64,
            zts_period_frames=384,
            io_period_frames=384,
            ring_frames=768,
            output_safety_frames=384,
            input_safety_frames=832, # output_safety (384) + io_period (384) + jitter margin (64)
        ),
        Geometry(
            name="EXPERIMENTAL LOW-LATENCY: 16 packets (96 frames) IRQ / 96 ZTS / 768 ring",
            irq_packets=16,
            zts_period_frames=96,
            io_period_frames=96,
            ring_frames=768,
            output_safety_frames=96,
            input_safety_frames=224, # output_safety (96) + io_period (96) + jitter margin (32)
        ),
        Geometry(
            name="Saffire Profile (64 frames period): 8 packets / 48 ZTS / 768 ring",
            irq_packets=8,
            zts_period_frames=48,
            io_period_frames=64,
            ring_frames=768,
            output_safety_frames=48,
            input_safety_frames=128,
            jitter_std_us=100.0,
        ),
        Geometry(
            name="ASFW TARGET (AudioTimingGeometry.hpp): 32 packets / 192 ZTS / 1536 ring, 512-frame client",
            irq_packets=32,
            zts_period_frames=192,
            io_period_frames=512,
            ring_frames=1536,
            output_safety_frames=48,
            input_safety_frames=624,
        ),
        Geometry(
            name="ASFW TARGET (AudioTimingGeometry.hpp): 32 packets / 192 ZTS / 1536 ring, 64-frame client",
            irq_packets=32,
            zts_period_frames=192,
            io_period_frames=64,
            ring_frames=1536,
            output_safety_frames=48,
            input_safety_frames=256,
            jitter_std_us=100.0,
        ),
    ]

    for case in cases:
        print_result(simulate(case))

    # Run variable size callbacks simulation
    aligned_192 = cases[2]
    print("========================================================================")
    print(" VARIABLE CALLBACK SIZE SIMULATION (STANDARD SAFETY): [96, 192, 288] frames")
    print("========================================================================")
    print_result(simulate(aligned_192, callback_sizes=[96, 192, 288]))

    var_geom = Geometry(
        name="VARIABLE CALLBACKS (SAFE INPUT OFFSET): 32 packets / 192 ZTS / 768 ring",
        irq_packets=32,
        zts_period_frames=192,
        io_period_frames=192,
        ring_frames=768,
        output_safety_frames=192,
        input_safety_frames=544,
    )
    print("========================================================================")
    print(" VARIABLE CALLBACK SIZE SIMULATION (SAFE SAFETY): [96, 192, 288] frames")
    print("========================================================================")
    print_result(simulate(var_geom, callback_sizes=[96, 192, 288]))


if __name__ == "__main__":
    run_tests()
    find_candidates()
    run_all()
