#!/usr/bin/env python3
"""
validate_startup_math.py

A Python simulation tool to validate the timing math and model the startup sequence
of the macOS DriverKit-based FireWire audio driver. It simulates the packet-by-packet
DMA transmission, interrupt dispatching, and user-space thread scheduling (with jitter)
to find the exact underrun boundaries and headroom.
"""

import sys
import random
from dataclasses import dataclass
from typing import List, Tuple, Optional

# Constants
PACKET_RATE_HZ = 8000  # IEEE 1394 cycle frequency
PACKET_DURATION_MS = 1000.0 / PACKET_RATE_HZ  # 0.125 ms per packet

@dataclass
class SimConfig:
    ring_size: int          # N: Shared metadata ring size (kTxSharedSlotPackets)
    prep_lead: int          # L: Preparation lead packets (kTxPreparationLeadPackets)
    hw_ring_size: int       # H: Hardware ring size (kTxHardwareRingPackets)
    group_size: int        # G: Interrupt group size (kTxPacketsPerGroup)
    sleep_delay_ms: float   # Actual sleep/wakeup latency of the user-space thread (ms)
    jitter_std_ms: float = 0.0 # Scheduling jitter standard deviation (ms)

@dataclass
class SimStep:
    time_ms: float
    event: str
    completion_cursor: int
    expose_cursor: int
    fill_cursor: int
    details: str

def expected_commit_gen(packet_index: int, ring_size: int) -> int:
    return packet_index // ring_size + 1

def run_startup_simulation(config: SimConfig, log_trace: bool = False) -> Tuple[bool, Optional[int], float, List[SimStep]]:
    """
    Simulates the driver startup sequence.
    Returns:
        ok (bool): True if the driver successfully starts (or runs for 250ms without underrun)
        fatal_index (int or None): The packet index where underrun occurred, if any
        time_elapsed_ms (float): The time elapsed when simulation stopped/failed
        trace (list): List of trace steps
    """
    trace: List[SimStep] = []
    
    # Initialize state
    expose_cursor = config.ring_size  # Prefill seeds the entire ring (0 to N-1)
    completion_cursor = 0
    fill_cursor = config.hw_ring_size # Prime sets softwareFillAbsIdx_ to H
    
    # metadata ring slot generations
    committed_gen = [1] * config.ring_size
    
    time_ms = 0.0
    next_interrupt_time = config.group_size * PACKET_DURATION_MS
    
    # Model user-space thread wakeup time.
    # Initially, the thread is starting up. Its first check happens after it sleeps.
    next_user_thread_wakeup = 0.0
    
    underrun = False
    fatal_index = None
    
    # Run simulation for up to 250 ms (the Saffire ZTS prime timeout window)
    sim_duration_ms = 250.0
    
    if log_trace:
        trace.append(SimStep(
            time_ms=0.0,
            event="STARTUP_PREFILL",
            completion_cursor=completion_cursor,
            expose_cursor=expose_cursor,
            fill_cursor=fill_cursor,
            details=f"Prefilled {config.ring_size} slots with gen=1. exposeCursor={expose_cursor}."
        ))

    while time_ms < sim_duration_ms:
        # Determine the next event: either an interrupt (hardware progress) or a user-space thread wakeup
        next_event_time = min(next_interrupt_time, next_user_thread_wakeup)
        
        # Advance time
        time_ms = next_event_time
        
        if time_ms >= sim_duration_ms:
            break
            
        if next_event_time == next_interrupt_time:
            # 1. Hardware progress / Interrupt group boundary
            # The hardware has consumed G packets
            completion_cursor += config.group_size
            next_interrupt_time += config.group_size * PACKET_DURATION_MS
            
            # The interrupt triggers the refill loop on the interrupt path
            # Refill attempts to keep `prep_lead` packets filled ahead of the completion cursor
            # Wait, refill loop attempts to refill `deltaConsumed` (G) packets starting from `fill_cursor`
            refill_ok = True
            underrun_idx = None
            
            for i in range(config.group_size):
                idx_to_refill = fill_cursor + i
                slot = idx_to_refill % config.ring_size
                expected_gen = expected_commit_gen(idx_to_refill, config.ring_size)
                actual_gen = committed_gen[slot]
                
                if actual_gen != expected_gen:
                    refill_ok = False
                    underrun_idx = idx_to_refill
                    break
            
            if not refill_ok:
                underrun = True
                fatal_index = underrun_idx
                if log_trace:
                    trace.append(SimStep(
                        time_ms=time_ms,
                        event="FATAL_UNDERRUN",
                        completion_cursor=completion_cursor,
                        expose_cursor=expose_cursor,
                        fill_cursor=fill_cursor,
                        details=f"IT FATAL UNDERRUN: Attempted to refill index {underrun_idx} (slot {underrun_idx % config.ring_size}). Expected gen {expected_commit_gen(underrun_idx, config.ring_size)}, committed gen {committed_gen[underrun_idx % config.ring_size]}."
                    ))
                break
            
            # Refill succeeded for this group
            fill_cursor += config.group_size
            
            if log_trace:
                trace.append(SimStep(
                    time_ms=time_ms,
                    event="REFILLED_GROUP",
                    completion_cursor=completion_cursor,
                    expose_cursor=expose_cursor,
                    fill_cursor=fill_cursor,
                    details=f"Refilled {config.group_size} packets up to index {fill_cursor - 1}."
                ))
                
        else:
            # 2. User-space thread wakeup
            # Schedule the next wakeup
            sleep_time = config.sleep_delay_ms
            if config.jitter_std_ms > 0:
                sleep_time += random.normalvariate(0, config.jitter_std_ms)
                sleep_time = max(0.1, sleep_time) # sleep cannot be negative or zero
            next_user_thread_wakeup += sleep_time
            
            # The user thread checks if targetPacketIndex > expose_cursor
            target_packet_index = completion_cursor + config.prep_lead
            
            if target_packet_index > expose_cursor:
                # User-space prepares new slots
                prepared_count = 0
                start_prep = expose_cursor
                while expose_cursor < target_packet_index:
                    slot = expose_cursor % config.ring_size
                    committed_gen[slot] = expected_commit_gen(expose_cursor, config.ring_size)
                    expose_cursor += 1
                    prepared_count += 1
                
                if log_trace:
                    trace.append(SimStep(
                        time_ms=time_ms,
                        event="USER_PREPARED",
                        completion_cursor=completion_cursor,
                        expose_cursor=expose_cursor,
                        fill_cursor=fill_cursor,
                        details=f"Woke up and prepared {prepared_count} packets from {start_prep} to {expose_cursor - 1}. Target was {target_packet_index}."
                    ))
            else:
                if log_trace:
                    trace.append(SimStep(
                        time_ms=time_ms,
                        event="USER_IDLE",
                        completion_cursor=completion_cursor,
                        expose_cursor=expose_cursor,
                        fill_cursor=fill_cursor,
                        details=f"Woke up. Target {target_packet_index} <= expose {expose_cursor}. Nothing to prepare."
                    ))
                    
    return not underrun, fatal_index, time_ms, trace

def print_config(config: SimConfig):
    print(f"Configuration:")
    print(f"  Shared Ring Size (N)   : {config.ring_size} packets")
    print(f"  Preparation Lead (L)   : {config.prep_lead} packets")
    print(f"  Hardware Ring Size (H) : {config.hw_ring_size} packets")
    print(f"  Interrupt Group (G)    : {config.group_size} packets ({config.group_size * PACKET_DURATION_MS:.3f} ms)")
    print(f"  Wakeup Sleep Delay     : {config.sleep_delay_ms:.3f} ms (jitter std={config.jitter_std_ms:.3f} ms)")
    theoretical_budget = (config.ring_size - config.prep_lead) * PACKET_DURATION_MS
    print(f"  Theoretical Startup Headroom (T_budget = (N - L) / R): {theoretical_budget:.3f} ms")
    print(f"  Theoretical Max Packets Consumed Before Prep Needed : {config.ring_size - config.prep_lead}")

def main():
    print("=" * 80)
    print("ASFW FIREWIRE AUDIO DRIVER - STARTUP TIMING & DEADLOCK VALIDATOR")
    print("=" * 80)
    
    # 1. Simulate the failing case (Commit 62a927e)
    # Ring=512, Lead=384, Sleep = 20ms (macOS user-space thread wakeup delay under scheduling load)
    print("\n--- CASE A: FAILING STARTUP (Commit 62a927e) ---")
    config_fail = SimConfig(
        ring_size=512,
        prep_lead=384,
        hw_ring_size=192,
        group_size=32,
        sleep_delay_ms=20.0,  # 20ms scheduler wakeup latency
        jitter_std_ms=2.0
    )
    print_config(config_fail)
    ok, fatal_idx, elapsed, trace = run_startup_simulation(config_fail, log_trace=True)
    print(f"\nSimulation Result: {'SUCCESS' if ok else 'FAILED (FATAL UNDERRUN)'}")
    print(f"  Elapsed Time : {elapsed:.3f} ms")
    if not ok:
        print(f"  Fatal Index  : {fatal_idx}")
        print(f"  Trace:")
        for step in trace:
            print(f"    [{step.time_ms:6.3f} ms] {step.event:<15} | C={step.completion_cursor:<4} | E={step.expose_cursor:<4} | F={step.fill_cursor:<4} | {step.details}")
            
    # 2. Simulate Option A: Expand ring size to 1024
    print("\n" + "=" * 80)
    print("--- CASE B: FIXED STARTUP (Option A - Expand Ring to 1024) ---")
    config_fixed_ring = SimConfig(
        ring_size=1024,
        prep_lead=384,
        hw_ring_size=192,
        group_size=32,
        sleep_delay_ms=20.0,
        jitter_std_ms=2.0
    )
    print_config(config_fixed_ring)
    ok, fatal_idx, elapsed, trace = run_startup_simulation(config_fixed_ring, log_trace=True)
    print(f"\nSimulation Result: {'SUCCESS' if ok else 'FAILED (FATAL UNDERRUN)'}")
    print(f"  Elapsed Time : {elapsed:.3f} ms")
    if not ok:
        print(f"  Fatal Index  : {fatal_idx}")
    else:
        print("  First 15 steps of success trace:")
        for step in trace[:15]:
            print(f"    [{step.time_ms:6.3f} ms] {step.event:<15} | C={step.completion_cursor:<4} | E={step.expose_cursor:<4} | F={step.fill_cursor:<4} | {step.details}")
        print("    ...")

    # 3. Simulate Option C: Lower preparation lead back to 224
    print("\n" + "=" * 80)
    print("--- CASE C: FIXED STARTUP (Option C - Reduce Lead to 224) ---")
    config_fixed_lead = SimConfig(
        ring_size=512,
        prep_lead=224,
        hw_ring_size=192,
        group_size=32,
        sleep_delay_ms=20.0,
        jitter_std_ms=2.0
    )
    print_config(config_fixed_lead)
    ok, fatal_idx, elapsed, trace = run_startup_simulation(config_fixed_lead, log_trace=True)
    print(f"\nSimulation Result: {'SUCCESS' if ok else 'FAILED (FATAL UNDERRUN)'}")
    print(f"  Elapsed Time : {elapsed:.3f} ms")
    if not ok:
        print(f"  Fatal Index  : {fatal_idx}")
    else:
        print("  First 15 steps of success trace:")
        for step in trace[:15]:
            print(f"    [{step.time_ms:6.3f} ms] {step.event:<15} | C={step.completion_cursor:<4} | E={step.expose_cursor:<4} | F={step.fill_cursor:<4} | {step.details}")
        print("    ...")

    # 4. Sweep sleep latency vs Ring size & Lead to find the exact failure threshold
    print("\n" + "=" * 80)
    print("--- PARAMETER SWEEP: THREAD WAKEUP LATENCY VS STARTUP SUCCESS ---")
    print("=" * 80)
    print(f"{'Ring (N)':<10} | {'Lead (L)':<10} | {'Theoretical T_budget':<22} | {'Max Safe Latency (Sim)':<25}")
    print("-" * 78)
    
    sweeps = [
        (512, 384),
        (512, 256),
        (512, 224),
        (1024, 384),
        (1024, 512),
        (2048, 384)
    ]
    
    for ring, lead in sweeps:
        theory = (ring - lead) * PACKET_DURATION_MS
        # Find maximum sleep delay that succeeds
        max_safe_delay = 0.0
        for delay in range(1, 200):
            # run multiple times to average out jitter
            success = True
            for _ in range(5):
                cfg = SimConfig(
                    ring_size=ring,
                    prep_lead=lead,
                    hw_ring_size=192,
                    group_size=32,
                    sleep_delay_ms=float(delay),
                    jitter_std_ms=0.0
                )
                ok, _, _, _ = run_startup_simulation(cfg, log_trace=False)
                if not ok:
                    success = False
                    break
            if success:
                max_safe_delay = float(delay)
            else:
                break
        print(f"{ring:<10} | {lead:<10} | {theory:18.3f} ms | {max_safe_delay:21.1f} ms")

if __name__ == "__main__":
    main()
