#!/usr/bin/env python3
"""
SYT (Synchronization Timestamp) Generator Simulator

Simulates Apple's AM824NuDCLWrite SYT generation algorithm for FireWire audio.

Key constants from Apple's decompilation:
- 30,720,000 = one FireWire cycle in "TenThousand" units (3072 × 10000)
- 491,520,000 = 16 cycles wrap (SYT cycle field is 4 bits)
- +3 cycles transfer delay = 375µs presentation offset
- 3072 offsets per cycle
- 8000 cycles per second

Usage:
    python syt.py simulate --packets 100 --rate 48000
    python syt.py decode 0x7bff
    python syt.py verify --log console.txt
"""

import argparse
from dataclasses import dataclass
from typing import List, Tuple, Optional

# =============================================================================
# Constants (matching Apple's implementation)
# =============================================================================

TICKS_PER_CYCLE = 3072
CYCLES_PER_SECOND = 8000
TICKS_PER_SECOND = TICKS_PER_CYCLE * CYCLES_PER_SECOND  # 24,576,000

# Apple's "InTenThousand" scaled units
SCALE_FACTOR = 10000
TICKS_PER_CYCLE_SCALED = TICKS_PER_CYCLE * SCALE_FACTOR  # 30,720,000
SYT_WRAP_SCALED = 16 * TICKS_PER_CYCLE_SCALED  # 491,520,000

# Transfer delay: +3 cycles = 375µs
TRANSFER_DELAY_CYCLES = 3

# SYT format
SYT_NO_INFO = 0xFFFF
SYT_CYCLE_MASK = 0xF000
SYT_OFFSET_MASK = 0x0FFF

# Sample rates and SYT intervals (samples between timestamps)
SYT_INTERVALS = {
    32000: 8,
    44100: 8,
    48000: 8,
    88200: 16,
    96000: 16,
    176400: 32,
    192000: 32,
}

# Cycle offsets per SYT interval (scaled)
# Formula: (SYT_INTERVAL / sample_rate) * TICKS_PER_SECOND * SCALE_FACTOR
def calc_offsets_per_syt_interval(sample_rate: int) -> int:
    syt_interval = SYT_INTERVALS.get(sample_rate, 8)
    # Time per SYT interval in seconds: syt_interval / sample_rate
    # Convert to ticks: * TICKS_PER_SECOND
    # Scale: * SCALE_FACTOR
    return int(syt_interval * TICKS_PER_SECOND * SCALE_FACTOR / sample_rate)


# =============================================================================
# SYT Generator (Apple-style)
# =============================================================================

@dataclass
class SYTState:
    """State of the SYT generator"""
    counter: int = 0               # Scaled counter (in "TenThousand" units)
    bus_cycle: int = 0             # Current FireWire bus cycle (0-7999 within second)
    bus_second: int = 0            # Current bus second (0-127)
    packet_count: int = 0          # Packets generated
    sample_count: int = 0          # Samples processed
    samples_per_packet: float = 0  # Average samples per packet
    sample_rate: int = 48000       # Sample rate


class AppleSYTGenerator:
    """
    Apple-style SYT generator.
    
    Key insight from decompilation:
    - Counter increments by 30,720,000 per packet (one cycle)
    - SYT = (bus_cycle + counter_cycles + 3) & 0xF | offset
    """
    
    def __init__(self, sample_rate: int = 48000):
        self.sample_rate = sample_rate
        self.syt_interval = SYT_INTERVALS.get(sample_rate, 8)
        self.offsets_per_syt = calc_offsets_per_syt_interval(sample_rate)
        
        # State
        self.counter = 0
        self.bus_cycle = 0
        self.packet_count = 0
        
        # For blocking mode: track when to emit SYT
        self.samples_since_syt = 0
        
        print(f"[SYT Init] rate={sample_rate}Hz syt_interval={self.syt_interval} "
              f"offsets_per_syt={self.offsets_per_syt}")
    
    def set_bus_cycle(self, cycle: int, second: int = 0):
        """Set current bus cycle (from hardware read)"""
        self.bus_cycle = cycle % CYCLES_PER_SECOND
    
    def generate_packet(self, samples_in_packet: int, bus_cycle: Optional[int] = None) -> Tuple[int, dict]:
        """
        Generate SYT for a packet.
        
        Args:
            samples_in_packet: Number of audio samples in this packet (0 for NO-DATA)
            bus_cycle: Optional bus cycle override (None = use internal tracking)
            
        Returns:
            (syt_value, debug_info)
        """
        if bus_cycle is not None:
            self.bus_cycle = bus_cycle % CYCLES_PER_SECOND
        
        # Increment counter by one cycle
        self.counter += TICKS_PER_CYCLE_SCALED
        if self.counter >= SYT_WRAP_SCALED:
            self.counter -= SYT_WRAP_SCALED
        
        # Update sample count
        self.samples_since_syt += samples_in_packet
        
        # Check if we should emit SYT (every syt_interval samples)
        emit_syt = self.samples_since_syt >= self.syt_interval
        
        if emit_syt and samples_in_packet > 0:
            self.samples_since_syt -= self.syt_interval
            
            # Compute SYT components
            scaled_ticks = self.counter // SCALE_FACTOR
            offset_cycles = scaled_ticks // TICKS_PER_CYCLE
            offset_ticks = scaled_ticks % TICKS_PER_CYCLE
            
            # Apply transfer delay (+3 cycles)
            syt_cycle = (self.bus_cycle + offset_cycles + TRANSFER_DELAY_CYCLES) & 0xF
            syt = (syt_cycle << 12) | offset_ticks
        else:
            syt = SYT_NO_INFO
            offset_cycles = 0
            offset_ticks = 0
            syt_cycle = 0
        
        # Advance bus cycle (simulated)
        self.bus_cycle = (self.bus_cycle + 1) % CYCLES_PER_SECOND
        self.packet_count += 1
        
        debug = {
            'packet': self.packet_count,
            'samples': samples_in_packet,
            'counter': self.counter,
            'bus_cycle': self.bus_cycle,
            'emit': emit_syt,
            'syt_cycle': syt_cycle if emit_syt else None,
            'syt_offset': offset_ticks if emit_syt else None,
        }
        
        return syt, debug


class SimpleSYTGenerator:
    """
    Simplified SYT generator - always emits SYT for data packets.
    
    This is what the current ASFW code does (incorrectly).
    """
    
    def __init__(self, sample_rate: int = 48000):
        self.sample_rate = sample_rate
        self.counter = 0
        self.packet_count = 0
        
    def generate_packet(self, samples_in_packet: int, bus_cycle: Optional[int] = None) -> Tuple[int, dict]:
        """Generate SYT (always, for any data packet)"""
        # Increment counter
        self.counter += TICKS_PER_CYCLE_SCALED
        if self.counter >= SYT_WRAP_SCALED:
            self.counter -= SYT_WRAP_SCALED
        
        if samples_in_packet > 0:
            scaled = self.counter // SCALE_FACTOR
            cycle = (scaled // TICKS_PER_CYCLE) & 0xF
            offset = scaled % TICKS_PER_CYCLE
            syt = (cycle << 12) | offset
        else:
            syt = SYT_NO_INFO
            cycle = 0
            offset = 0
        
        self.packet_count += 1
        
        return syt, {
            'packet': self.packet_count,
            'counter': self.counter,
            'cycle': cycle,
            'offset': offset,
        }


# =============================================================================
# SYT Decoder
# =============================================================================

def decode_syt(syt: int) -> dict:
    """Decode a 16-bit SYT value"""
    if syt == SYT_NO_INFO:
        return {'raw': syt, 'cycle': None, 'offset': None, 'no_info': True}
    
    cycle = (syt >> 12) & 0xF
    offset = syt & 0xFFF
    time_us = (cycle * 125.0) + (offset * 125.0 / 3072)
    
    return {
        'raw': syt,
        'hex': f"0x{syt:04x}",
        'cycle': cycle,
        'offset': offset,
        'time_us': time_us,
        'no_info': False,
    }


# =============================================================================
# Simulation
# =============================================================================

def simulate_stream(packets: int, sample_rate: int, use_apple: bool = True, 
                    blocking: bool = False, verbose: bool = False) -> List[dict]:
    """
    Simulate SYT generation for a stream.
    
    Args:
        packets: Number of packets to simulate
        sample_rate: Audio sample rate
        use_apple: Use Apple-style generator (True) or simple (False)
        blocking: Use blocking mode (8/0 samples) vs non-blocking (6 samples avg)
        verbose: Print each packet
        
    Returns:
        List of packet info dicts
    """
    if use_apple:
        gen = AppleSYTGenerator(sample_rate)
    else:
        gen = SimpleSYTGenerator(sample_rate)
    
    results = []
    samples_per_cycle = sample_rate / CYCLES_PER_SECOND  # 6.0 for 48kHz
    
    for i in range(packets):
        if blocking:
            # Blocking mode: alternate between SYT_INTERVAL and 0 samples
            syt_interval = SYT_INTERVALS.get(sample_rate, 8)
            # Every 8 samples / 6 samples per packet ≈ 1.33 packets
            # So roughly: 3 packets with 8 samples, 1 with 0
            samples = syt_interval if (i % 4) != 3 else 0
        else:
            # Non-blocking: average 6 samples per packet
            # Alternate 6, 6, 6, 6, 6, 6 pattern (or 5,6,6,6,6,7 for exact 48k)
            # Simplified: always 6
            samples = int(samples_per_cycle)
        
        syt, debug = gen.generate_packet(samples)
        decoded = decode_syt(syt)
        
        result = {
            'packet': i + 1,
            'samples': samples,
            'syt': syt,
            **decoded,
            **debug,
        }
        results.append(result)
        
        if verbose:
            syt_str = f"0x{syt:04x}" if syt != SYT_NO_INFO else "NOINFO"
            print(f"[{i+1:4d}] samples={samples} syt={syt_str} "
                  f"counter={debug.get('counter', 0):>12d}")
    
    return results


def print_summary(results: List[dict]):
    """Print summary statistics"""
    total = len(results)
    with_syt = sum(1 for r in results if not r.get('no_info', True))
    no_data = sum(1 for r in results if r.get('samples', 0) == 0)
    
    print(f"\n{'='*60}")
    print(f"Summary: {total} packets")
    print(f"  - With SYT: {with_syt} ({100*with_syt/total:.1f}%)")
    print(f"  - NO-DATA:  {no_data} ({100*no_data/total:.1f}%)")
    
    # SYT distribution
    cycles = [r['cycle'] for r in results if r.get('cycle') is not None]
    if cycles:
        print(f"  - SYT cycles used: {sorted(set(cycles))}")
    
    # Check for our buggy pattern (always 0xBFF offset)
    offsets = [r['offset'] for r in results if r.get('offset') is not None]
    if offsets:
        unique_offsets = sorted(set(offsets))
        if len(unique_offsets) <= 3:
            print(f"  - ⚠️  Only {len(unique_offsets)} unique offsets: {unique_offsets}")
            print(f"       This suggests a bug in the counter increment!")
        else:
            print(f"  - Offset range: {min(offsets)} - {max(offsets)}")


# =============================================================================
# CLI
# =============================================================================

def main():
    parser = argparse.ArgumentParser(description="SYT Generator Simulator")
    subparsers = parser.add_subparsers(dest='command', help='Commands')
    
    # simulate command
    sim = subparsers.add_parser('simulate', help='Simulate SYT generation')
    sim.add_argument('-n', '--packets', type=int, default=100, help='Number of packets')
    sim.add_argument('-r', '--rate', type=int, default=48000, help='Sample rate')
    sim.add_argument('-b', '--blocking', action='store_true', help='Use blocking mode')
    sim.add_argument('-s', '--simple', action='store_true', help='Use simple generator (buggy)')
    sim.add_argument('-v', '--verbose', action='store_true', help='Print each packet')
    
    # decode command
    dec = subparsers.add_parser('decode', help='Decode SYT value')
    dec.add_argument('syt', help='SYT value (hex or decimal)')
    
    # compare command
    cmp = subparsers.add_parser('compare', help='Compare Apple vs Simple generators')
    cmp.add_argument('-n', '--packets', type=int, default=50, help='Number of packets')
    cmp.add_argument('-r', '--rate', type=int, default=48000, help='Sample rate')
    
    # constants command
    const = subparsers.add_parser('constants', help='Print timing constants')
    const.add_argument('-r', '--rate', type=int, default=48000, help='Sample rate')
    
    args = parser.parse_args()
    
    if args.command == 'simulate':
        results = simulate_stream(
            args.packets, args.rate, 
            use_apple=not args.simple,
            blocking=args.blocking,
            verbose=args.verbose
        )
        print_summary(results)
        
    elif args.command == 'decode':
        syt_val = int(args.syt, 16) if args.syt.startswith('0x') else int(args.syt)
        info = decode_syt(syt_val)
        print(f"SYT: {info['hex']}")
        if info['no_info']:
            print("  NO-INFO (no timestamp)")
        else:
            print(f"  Cycle:  {info['cycle']}")
            print(f"  Offset: {info['offset']} (0x{info['offset']:03x})")
            print(f"  Time:   {info['time_us']:.2f} µs within 16-cycle window")
            
    elif args.command == 'compare':
        print("="*60)
        print("APPLE-STYLE GENERATOR:")
        print("="*60)
        apple_results = simulate_stream(args.packets, args.rate, use_apple=True, verbose=True)
        print_summary(apple_results)
        
        print("\n" + "="*60)
        print("SIMPLE GENERATOR (current buggy code):")
        print("="*60)
        simple_results = simulate_stream(args.packets, args.rate, use_apple=False, verbose=True)
        print_summary(simple_results)
        
    elif args.command == 'constants':
        rate = args.rate
        syt_int = SYT_INTERVALS.get(rate, 8)
        offsets_scaled = calc_offsets_per_syt_interval(rate)
        
        print(f"Timing Constants for {rate} Hz:")
        print(f"  TICKS_PER_CYCLE        = {TICKS_PER_CYCLE}")
        print(f"  CYCLES_PER_SECOND      = {CYCLES_PER_SECOND}")
        print(f"  TICKS_PER_SECOND       = {TICKS_PER_SECOND:,}")
        print(f"  SCALE_FACTOR           = {SCALE_FACTOR}")
        print(f"  TICKS_PER_CYCLE_SCALED = {TICKS_PER_CYCLE_SCALED:,}")
        print(f"  SYT_WRAP_SCALED        = {SYT_WRAP_SCALED:,}")
        print(f"  TRANSFER_DELAY_CYCLES  = {TRANSFER_DELAY_CYCLES} ({TRANSFER_DELAY_CYCLES * 125} µs)")
        print(f"  SYT_INTERVAL           = {syt_int} samples")
        print(f"  OFFSETS_PER_SYT_SCALED = {offsets_scaled:,}")
        print(f"  Samples per cycle (avg)= {rate / CYCLES_PER_SECOND:.2f}")
        
    else:
        parser.print_help()


if __name__ == '__main__':
    main()
