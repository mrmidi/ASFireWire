#!/usr/bin/env python3
"""
FireWire Isochronous Bandwidth Calculator

Calculates bandwidth allocation units required for audio streams per IEEE 1394 / IEC 61883.

Bandwidth is measured in Allocation Units, where:
- Total available bandwidth = 6144 units per 125µs cycle (at S400)
- Each unit ≈ 20.345ns of bus time at S400

The driver uses these values when calling IRMClient::AllocateResources().

References:
- IEEE 1394-1995, Section 8.3.2.3.5 (Bandwidth allocation)
- IEC 61883-1, Section 4.2 (CIP header format)
- IEC 61883-6, Section 6.2 (AM824 audio format)
"""

import argparse
import math
from dataclasses import dataclass
from enum import IntEnum
from typing import Optional


class BusSpeed(IntEnum):
    """FireWire bus speeds in Mbps."""
    S100 = 100
    S200 = 200
    S400 = 400
    S800 = 800


@dataclass
class StreamParams:
    """Parameters for an isochronous audio stream."""
    sample_rate: int          # Hz (e.g., 44100, 48000, 96000, 192000)
    channels: int             # Number of audio channels
    bits_per_sample: int      # Usually 24 for professional audio
    bus_speed: BusSpeed       # Bus speed in Mbps
    syt_interval: int = 8     # SYT interval (samples between timestamps)


# Bus speed characteristics
SPEED_PARAMS = {
    BusSpeed.S100: {"bytes_per_unit": 4,    "max_payload": 512},
    BusSpeed.S200: {"bytes_per_unit": 8,    "max_payload": 1024},
    BusSpeed.S400: {"bytes_per_unit": 16,   "max_payload": 2048},
    BusSpeed.S800: {"bytes_per_unit": 32,   "max_payload": 4096},
}

# Total bandwidth available per 125µs cycle
TOTAL_BANDWIDTH_UNITS = 6144

# Fixed overhead per isochronous packet (IEEE 1394)
# Includes: gap, prefix, header, header CRC, data CRC
ISO_OVERHEAD_BYTES = 16  # Conservative estimate

# CIP header size (IEC 61883-1)
CIP_HEADER_BYTES = 8  # Two quadlets


def calculate_samples_per_packet(sample_rate: int) -> tuple[int, int]:
    """
    Calculate samples per packet for a given sample rate.
    
    Returns (base_samples, extra_samples_per_N_packets).
    For 44.1kHz: 5 or 6 samples per packet (averaging to 5.5125)
    For 48kHz: exactly 6 samples per packet
    For 96kHz: exactly 12 samples per packet
    For 192kHz: exactly 24 samples per packet
    """
    # 8000 packets per second (125µs cycle)
    samples_per_second = sample_rate
    packets_per_second = 8000
    
    base_samples = samples_per_second // packets_per_second
    remainder = samples_per_second % packets_per_second
    
    return base_samples, remainder


def calculate_max_packet_size(params: StreamParams) -> int:
    """
    Calculate the maximum packet payload size in bytes.
    
    AM824 format: Each sample is one 32-bit quadlet (4 bytes)
    containing label byte + 24-bit audio.
    """
    base_samples, remainder = calculate_samples_per_packet(params.sample_rate)
    
    # Use ceiling for max packet size calculation
    if remainder > 0:
        max_samples = base_samples + 1
    else:
        max_samples = base_samples
    
    # Each channel gets one quadlet (4 bytes) per sample
    # AM824: 1 label byte + 3 audio bytes = 4 bytes per channel per sample
    audio_data_bytes = max_samples * params.channels * 4
    
    # Add CIP header
    total_payload = CIP_HEADER_BYTES + audio_data_bytes
    
    return total_payload


def calculate_bandwidth_units(params: StreamParams) -> int:
    """
    Calculate bandwidth allocation units required for the stream.
    
    Formula (per IEEE 1394-1995 §8.3.2.3.5):
    bandwidth_units = ceiling((packet_size + overhead) / bytes_per_unit)
    
    This is the value passed to IRMClient::AllocateResources().
    """
    max_payload = calculate_max_packet_size(params)
    total_bytes = max_payload + ISO_OVERHEAD_BYTES
    
    speed_info = SPEED_PARAMS[params.bus_speed]
    bytes_per_unit = speed_info["bytes_per_unit"]
    
    # Ceiling division
    bandwidth_units = math.ceil(total_bytes / bytes_per_unit)
    
    return bandwidth_units


def calculate_bandwidth_percentage(units: int) -> float:
    """Calculate percentage of total available bandwidth."""
    return (units / TOTAL_BANDWIDTH_UNITS) * 100


def format_stream_info(params: StreamParams) -> str:
    """Generate a formatted info string for the stream parameters."""
    base_samples, remainder = calculate_samples_per_packet(params.sample_rate)
    max_payload = calculate_max_packet_size(params)
    bandwidth = calculate_bandwidth_units(params)
    percentage = calculate_bandwidth_percentage(bandwidth)
    
    lines = [
        "=" * 60,
        "FireWire Isochronous Bandwidth Calculation",
        "=" * 60,
        "",
        "Stream Parameters:",
        f"  Sample Rate:      {params.sample_rate:,} Hz",
        f"  Channels:         {params.channels}",
        f"  Bits per Sample:  {params.bits_per_sample}",
        f"  Bus Speed:        S{params.bus_speed.value}",
        "",
        "Packet Analysis:",
        f"  Samples/packet:   {base_samples}" + (f"-{base_samples + 1} (varies)" if remainder else " (fixed)"),
        f"  Audio data:       {(base_samples + (1 if remainder else 0)) * params.channels * 4} bytes max",
        f"  CIP header:       {CIP_HEADER_BYTES} bytes",
        f"  Max payload:      {max_payload} bytes",
        f"  ISO overhead:     {ISO_OVERHEAD_BYTES} bytes",
        f"  Total per packet: {max_payload + ISO_OVERHEAD_BYTES} bytes",
        "",
        "Bandwidth Allocation:",
        f"  Units required:   {bandwidth} (0x{bandwidth:02X})",
        f"  Percentage:       {percentage:.2f}% of available bandwidth",
        f"  Available:        {TOTAL_BANDWIDTH_UNITS} total units",
        "",
        "Driver Usage:",
        f"  IRMClient::AllocateResources(channel, {bandwidth})",
        "=" * 60,
    ]
    
    return "\n".join(lines)


def print_common_configurations():
    """Print bandwidth for common audio configurations."""
    print("\n" + "=" * 70)
    print("Common Audio Stream Bandwidth Requirements (S400)")
    print("=" * 70)
    print(f"{'Configuration':<30} {'Payload':>10} {'Units':>8} {'Hex':>8} {'%':>8}")
    print("-" * 70)
    
    configs = [
        ("Stereo 44.1kHz", 44100, 2),
        ("Stereo 48kHz", 48000, 2),
        ("Stereo 96kHz", 96000, 2),
        ("Stereo 192kHz", 192000, 2),
        ("8ch 44.1kHz", 44100, 8),
        ("8ch 48kHz", 48000, 8),
        ("8ch 96kHz", 96000, 8),
        ("18ch 48kHz (Duet)", 48000, 18),
        ("18ch 96kHz (Duet)", 96000, 18),
        ("32ch 48kHz", 48000, 32),
    ]
    
    for name, rate, channels in configs:
        params = StreamParams(
            sample_rate=rate,
            channels=channels,
            bits_per_sample=24,
            bus_speed=BusSpeed.S400
        )
        payload = calculate_max_packet_size(params)
        units = calculate_bandwidth_units(params)
        pct = calculate_bandwidth_percentage(units)
        print(f"{name:<30} {payload:>10} {units:>8} {f'0x{units:02X}':>8} {pct:>7.2f}%")
    
    print("-" * 70)
    print(f"{'Total available bandwidth:':<30} {'':<10} {TOTAL_BANDWIDTH_UNITS:>8}")
    print()


def main():
    parser = argparse.ArgumentParser(
        description="Calculate FireWire isochronous bandwidth requirements",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s -r 48000 -c 18              # 18 channels at 48kHz (Apogee Duet)
  %(prog)s -r 96000 -c 8 -s 400        # 8 channels at 96kHz, S400
  %(prog)s --common                     # Show common configurations
"""
    )
    
    parser.add_argument("-r", "--rate", type=int, default=48000,
                        help="Sample rate in Hz (default: 48000)")
    parser.add_argument("-c", "--channels", type=int, default=2,
                        help="Number of audio channels (default: 2)")
    parser.add_argument("-b", "--bits", type=int, default=24,
                        help="Bits per sample (default: 24)")
    parser.add_argument("-s", "--speed", type=int, default=400,
                        choices=[100, 200, 400, 800],
                        help="Bus speed: 100, 200, 400, or 800 (default: 400)")
    parser.add_argument("--common", action="store_true",
                        help="Show bandwidth for common configurations")
    
    args = parser.parse_args()
    
    if args.common:
        print_common_configurations()
        return
    
    # Map speed value to enum
    speed_map = {100: BusSpeed.S100, 200: BusSpeed.S200, 
                 400: BusSpeed.S400, 800: BusSpeed.S800}
    
    params = StreamParams(
        sample_rate=args.rate,
        channels=args.channels,
        bits_per_sample=args.bits,
        bus_speed=speed_map[args.speed]
    )
    
    print(format_stream_info(params))


if __name__ == "__main__":
    main()
