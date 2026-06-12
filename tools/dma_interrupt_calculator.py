#!/usr/bin/env python3
"""
DMA Interrupt and Core Audio Period Alignment Calculator for ASFW.
Helps design DMA programs and timing groups to match Core Audio's ZTS expectations.
"""

import sys
from typing import List, Tuple

def analyze_alignment():
    print("========================================================================")
    print(" DMA INTERRUPT & CORE AUDIO ALIGNMENT CALCULATOR (48kHz Blocking)")
    print("========================================================================")
    print("\n--- The Mathematical Foundation ---")
    print("1. FireWire operates at a strict packet rate of 8,000 packets/sec (125 µs cycle).")
    print("2. At 48,000 Hz, the average frames per packet is 48000 / 8000 = 6.0.")
    print("3. In blocking mode, each data packet carries exactly 8 frames (or 0 for empty).")
    print("4. This creates a repeating 4-packet cadence: [D, D, D, E] (3 data, 1 empty).")
    print("   Total frames in 4 packets = 3 * 8 + 1 * 0 = 24 frames.")
    print("   Therefore, any integer P packets (multiples of 4) will contain exactly 6 * P frames.")

    print("\n--- Why Power-of-Two ZTS Periods require Interpolation ---")
    print("To align a hardware interrupt exactly with a ZTS period boundary without interpolation,")
    print("the frame advance of the packet group (6 * P) must equal the ZTS period (F).")
    print("If F is a power of two (2^k), then:")
    print("   6 * P = 2^k  =>  2 * 3 * P = 2^k")
    print("Because 6 contains the prime factor 3, it is MATHEMATICALLY IMPOSSIBLE for 6 * P")
    print("to equal a power of two for any integer packet count P.")
    print("Therefore, standard Core Audio power-of-two ZTS periods (64, 128, 256, 512) WILL ALWAYS")
    print("require sub-packet host time interpolation unless a non-power-of-two grid is used.")

    # Core Audio expectations (power of two)
    p2_periods = [32, 64, 128, 256, 512, 1024, 2048]
    # Aligned expectations (multiples of 6 / 24)
    aligned_periods = [48, 96, 192, 384, 576, 768, 1152, 1536]

    print("\n--- Analysis of Packet Group Sizes (Interrupt Cadences) ---")
    print(f"{'Group Packets':<15} | {'Frame Advance':<15} | {'Duration (ms)':<15} | {'Power of Two Match':<22} | {'Aligned Period Match'}")
    print("-" * 90)

    # We evaluate packet group sizes from 4 to 64 (multiples of 4 to keep cadence phase locked)
    for p in range(4, 68, 4):
        frames = 6 * p
        duration_ms = p * 0.125
        
        # Check power of two division
        p2_matches = []
        for period in p2_periods:
            if frames == period:
                p2_matches.append(f"EXACT({period})")
            elif period % frames == 0:
                p2_matches.append(f"Divides({period})")
        p2_str = ", ".join(p2_matches) if p2_matches else "None"

        # Check aligned division
        aligned_matches = []
        for period in aligned_periods:
            if frames == period:
                aligned_matches.append(f"EXACT({period})")
            elif period % frames == 0:
                aligned_matches.append(f"Divides({period})")
        aligned_str = ", ".join(aligned_matches) if aligned_matches else "None"

        print(f"{p:<15} | {frames:<15} | {duration_ms:<15.3f} | {p2_str:<22} | {aligned_str}")

    print("\n--- Practical Recommendations ---")
    print("1. If keeping Power-of-Two Ring Buffers and ZTS (e.g., 512 frames):")
    print("   - Using group size = 8 packets (48 frames) or 32 packets (192 frames) is ideal.")
    # 512 % 64 == 0 (which is 10.67 ms / 64 packets group)
    print("   - Note that 512 / 48 is not an integer (10.67), meaning an 8-packet group (48 frames)")
    print("     does not evenly divide a 512-frame ring. This is why the working tree uses")
    print("     a ZTS period of 512, but does sub-packet back-interpolation inside the 8-packet drain.")
    print("   - If we want to eliminate interpolation while keeping the 512-frame ring, we would need")
    print("     to interrupt every 64 packets (512 frames / 10.67 ms). This increases latency.")
    print("   - If we want low latency (1.33 ms / 8 packets group = 48 frames), we could set the ZTS Period")
    print("     and Ring Buffer to 384 frames (since 384 % 48 == 0, and 384 is multiple of 32).")
    print("\n2. If using Aligned Non-Power-of-Two Rings (Multiples of 32 & 24):")
    print("   - Best candidate: ZTS Period = 192 frames (4 ms), Ring Buffer = 384 or 768 frames.")
    print("   - Interrupt every 32 packets (192 frames / 4 ms) or every 8 packets (48 frames / 1 ms).")
    print("   - 192 and 384 are both multiples of 32 (32-frame alignment contract) and 24.")
    print("   - In this setup, the hardware interrupt aligns perfectly with the ZTS boundary,")
    print("     meaning we can publish raw host timestamps directly from the interrupt without interpolation.")

if __name__ == "__main__":
    analyze_alignment()
