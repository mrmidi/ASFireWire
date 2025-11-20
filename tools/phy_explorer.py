#!/usr/bin/env python3
"""
phy_explorer.py
================

Quick-and-dirty PHY packet inspector used to validate what we emit on the wire.
Feed it the two quadlets that FireBug reported (or what we logged locally) and
it will decode the Alpha PHY configuration layout, verify the inverted quadlet,
and highlight suspicious fields such as gap_count=0 with T=1.

Examples:
    ./phy_explorer.py 0x00800000 0xff7fffff
    ./phy_explorer.py --little-endian 0x00008000
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass


@dataclass
class PhyPacket:
    quadlet: int  # host order (msb = bit 31)

    @staticmethod
    def from_int(raw: int, little_endian: bool) -> "PhyPacket":
        raw &= 0xFFFFFFFF
        if little_endian:
            raw = int.from_bytes(raw.to_bytes(4, "little"), "big")
        return PhyPacket(raw)

    @property
    def packet_id(self) -> int:
        return (self.quadlet >> 30) & 0x3

    @property
    def root_id(self) -> int:
        return (self.quadlet >> 24) & 0x3F

    @property
    def force_root(self) -> bool:
        return bool((self.quadlet >> 23) & 0x1)

    @property
    def gap_opt(self) -> bool:
        return bool((self.quadlet >> 22) & 0x1)

    @property
    def gap_count(self) -> int:
        return (self.quadlet >> 16) & 0x3F

    @property
    def payload(self) -> int:
        """Lower 16 bits (used by extended/global-resume packets)."""
        return self.quadlet & 0xFFFF

    def describe(self) -> str:
        pid = self.packet_id
        if pid == 0:
            kind = "PHY Config"
        elif pid == 2:
            kind = "Self ID"
        else:
            kind = f"Reserved({pid})"
        flags = []
        if self.force_root:
            flags.append("R=1 (force root)")
        if self.gap_opt:
            flags.append("T=1 (gap update)")
        if not flags:
            flags.append("R=0 T=0")
        flag_str = ", ".join(flags)
        return (
            f"{kind}: rootId={self.root_id:02d} gapCount={self.gap_count} "
            f"{flag_str} payload=0x{self.payload:04X}"
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Decode IEEE 1394 PHY packets")
    parser.add_argument(
        "quadlets",
        nargs="+",
        help="Quadlets to decode (hex like 0x00c3f000 or decimal). "
        "Provide two values to check the inverted second quadlet.",
    )
    group = parser.add_mutually_exclusive_group()
    group.add_argument(
        "--bus-order",
        action="store_true",
        help="Treat inputs as already in bus order (default).",
    )
    group.add_argument(
        "--little-endian",
        action="store_true",
        help="Inputs are little-endian host order (bytes will be swapped).",
    )
    parser.add_argument(
        "--raw",
        action="store_true",
        help="Also print the raw integer values after normalization.",
    )
    parser.add_argument(
        "--test-endianness",
        action="store_true",
        help="Show both little-endian and big-endian interpretations to diagnose byte-swap bugs.",
    )
    return parser.parse_args()


def parse_int(token: str) -> int:
    token = token.strip()
    base = 16 if token.lower().startswith("0x") else 10
    return int(token, base)


def main() -> int:
    args = parse_args()
    if not args.quadlets:
        print("No quadlets provided", file=sys.stderr)
        return 1

    little = args.little_endian
    raw_values = [parse_int(tok) for tok in args.quadlets]

    # Endianness testing mode - show both interpretations
    if args.test_endianness:
        print("=" * 80)
        print("ENDIANNESS DIAGNOSTIC MODE")
        print("=" * 80)
        for idx, raw in enumerate(raw_values):
            print(f"\n--- Quadlet[{idx}] = 0x{raw:08X} ---")

            # Interpretation 1: As-is (big-endian / bus order)
            print("\n  Interpretation A: AS-IS (big-endian / bus order)")
            pkt_be = PhyPacket(raw)
            print(f"    0x{raw:08X}")
            print(f"    {pkt_be.describe()}")
            print(f"    Bytes: {(raw >> 24) & 0xFF:02X} {(raw >> 16) & 0xFF:02X} {(raw >> 8) & 0xFF:02X} {raw & 0xFF:02X}")

            # Interpretation 2: Byte-swapped (little-endian → big-endian)
            swapped = int.from_bytes(raw.to_bytes(4, "little"), "big")
            print("\n  Interpretation B: BYTE-SWAPPED (little-endian → big-endian)")
            pkt_le = PhyPacket(swapped)
            print(f"    0x{swapped:08X}")
            print(f"    {pkt_le.describe()}")
            print(f"    Bytes: {(swapped >> 24) & 0xFF:02X} {(swapped >> 16) & 0xFF:02X} {(swapped >> 8) & 0xFF:02X} {swapped & 0xFF:02X}")

            # Analysis
            print("\n  🔍 ANALYSIS:")
            if pkt_be.gap_count != 0 and pkt_le.gap_count == 0:
                print("    ❌ ENDIANNESS BUG DETECTED!")
                print(f"       - AS-IS has gap={pkt_be.gap_count} (likely intended)")
                print(f"       - SWAPPED has gap={pkt_le.gap_count} (what PHY might see!)")
                print("       → You're sending host-order bytes to bus (need byte swap!)")
            elif pkt_be.gap_count == 0 and pkt_le.gap_count != 0:
                print("    ⚠️  Possible byte-swap issue")
                print(f"       - AS-IS has gap={pkt_be.gap_count}")
                print(f"       - SWAPPED has gap={pkt_le.gap_count}")
                print("       → Check if you swapped bytes when you shouldn't have")
            elif pkt_be.gap_count == pkt_le.gap_count:
                print(f"    ✓ Gap count is same in both: {pkt_be.gap_count}")
            else:
                print(f"    ℹ️  AS-IS gap={pkt_be.gap_count}, SWAPPED gap={pkt_le.gap_count}")

        print("\n" + "=" * 80)
        print("RECOMMENDATION:")
        if any((PhyPacket(raw).gap_count != 0 and
                PhyPacket(int.from_bytes(raw.to_bytes(4, "little"), "big")).gap_count == 0)
               for raw in raw_values):
            print("  Your code is encoding packets correctly in HOST order,")
            print("  but NOT converting to BUS order (big-endian) before transmission!")
            print("  → Use EncodeBusOrder() or ToBusOrder() before sending to OHCI")
        print("=" * 80)
        return 0

    # Normal mode
    packets = [PhyPacket.from_int(raw, little) for raw in raw_values]

    for idx, pkt in enumerate(packets):
        print(f"Quadlet[{idx}] raw=0x{pkt.quadlet:08X}")
        if args.raw:
            print(f"  int={pkt.quadlet}")
        print(f"  {pkt.describe()}")
        if pkt.packet_id != 0:
            print("  ⚠️  Not a PHY Config packet (packet identifier bits != 00)")
        if pkt.gap_opt and pkt.gap_count == 0:
            print("  ❌  Gap optimization requested with gap_count=0 (invalid!)")
        if not pkt.gap_opt and pkt.gap_count != 0:
            print("  ℹ️  gap_count field present but T=0, will be ignored")
        if not pkt.force_root and not pkt.gap_opt and pkt.payload not in (0x0000, 0x3C00, 0x3C02):
            print("  ℹ️  Looks like an extended PHY packet / vendor command")

    if len(packets) == 2:
        complement = packets[1].quadlet == (~packets[0].quadlet & 0xFFFFFFFF)
        status = "PASS" if complement else "FAIL"
        print(f"Second quadlet complement check: {status}")
    elif len(packets) > 2:
        print("Note: only the first two quadlets are checked for complement.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
