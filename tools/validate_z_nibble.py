#!/usr/bin/env python3
"""
validate_z_nibble.py

Tool to test and validate OHCI AT CommandPtr Z-nibble logic.

What it validates:
- Old buggy rule: Z derived from "first descriptor is immediate" (gives Z=2 for CAS).
- Correct rule: Z equals total packet blocks (e.g., CAS uses 3 blocks).

Also optionally parses ASFW kernel logs to verify:
    cmdPtr_low_nibble == blocks
for PATH1 arming.

Usage:
  python3 validate_z_nibble.py
  python3 validate_z_nibble.py --blocks 3 --iova 0x80000420 --first-immediate 1
  python3 validate_z_nibble.py --logfile /path/to/kernel.log

Exit code:
  0 if all validations pass, 1 otherwise.
"""

from __future__ import annotations

import argparse
import dataclasses
import re
import sys
from typing import List, Optional, Tuple


@dataclasses.dataclass(frozen=True)
class DescriptorChain:
    """
    Minimal model of your DescriptorBuilder::DescriptorChain fields
    relevant to Z/cmdPtr.
    """
    txid: int
    first_iova32: int
    first_blocks: int   # 2 for immediate, 1 for standard
    last_blocks: int    # 2 for immediate-last, 1 for standard-last
    payload_size: int = 0

    def total_blocks(self) -> int:
        # In your code TotalBlocks() = firstBlocks + lastBlocks when two-descriptor,
        # or just firstBlocks when header-only.
        if self.payload_size == 0:
            return self.first_blocks
        return self.first_blocks + self.last_blocks


def compute_z_old(first_is_immediate: bool) -> int:
    """
    Your buggy ATSubmitPolicy::ComputeZ(firstIsImmediate).
    """
    return 0x2 if first_is_immediate else 0x0


def compute_z_new(total_blocks: int) -> int:
    """
    Correct ComputeZ(totalBlocks).
    OHCI expects Z == number of 16B blocks in the packet.
    """
    return total_blocks & 0xF


def command_ptr_from_iova(first_iova32: int, z: int) -> int:
    """
    Mimic DescriptorRing::CommandPtrWordFromIOVA(iova, z):
    lower nibble holds Z, upper bits are 16B-aligned address.
    """
    return (first_iova32 & 0xFFFFFFF0) | (z & 0xF)


def validate_chain(chain: DescriptorChain) -> Tuple[bool, str]:
    """
    Validate old vs new Z and show outcome.
    """
    total = chain.total_blocks()
    first_is_immediate = (chain.first_blocks == 2)

    z_old = compute_z_old(first_is_immediate)
    z_new = compute_z_new(total)

    cmd_old = command_ptr_from_iova(chain.first_iova32, z_old)
    cmd_new = command_ptr_from_iova(chain.first_iova32, z_new)

    ok_old = (cmd_old & 0xF) == total
    ok_new = (cmd_new & 0xF) == total

    lines = []
    lines.append(f"txid={chain.txid} first_iova32=0x{chain.first_iova32:08X} "
                 f"first_blocks={chain.first_blocks} last_blocks={chain.last_blocks} "
                 f"payload_size={chain.payload_size} total_blocks={total}")
    lines.append(f"  OLD: first_is_immediate={int(first_is_immediate)} z_old={z_old} "
                 f"cmdPtr_old=0x{cmd_old:08X} lowNibble={cmd_old & 0xF} "
                 f"=> {'OK' if ok_old else 'FAIL'}")
    lines.append(f"  NEW: z_new={z_new} cmdPtr_new=0x{cmd_new:08X} lowNibble={cmd_new & 0xF} "
                 f"=> {'OK' if ok_new else 'FAIL'}")

    if ok_new and not ok_old:
        lines.append("  ✅ Fix validated: old rule breaks, new rule correct.")
        return True, "\n".join(lines)

    if ok_new and ok_old:
        lines.append("  ✅ Both rules happen to work for this chain (not CAS).")
        return True, "\n".join(lines)

    lines.append("  ❌ New rule failed — check TotalBlocks, ring alignment, or cmdPtr packing.")
    return False, "\n".join(lines)


def run_builtin_tests() -> bool:
    """
    Built-in vectors that reproduce your CAS bug and validate the fix.
    """
    tests: List[DescriptorChain] = []

    # 1) Header-only immediate packet (read quadlet / write quadlet / phy packet)
    # Immediate descriptor only => 2 blocks; old rule OK here.
    tests.append(DescriptorChain(
        txid=1, first_iova32=0x80001000, first_blocks=2, last_blocks=0, payload_size=0
    ))

    # 2) CAS lock request => immediate header (2 blocks) + standard payload (1 block) = 3 blocks
    # Old rule yields z=2 => FAIL; new yields z=3 => OK.
    tests.append(DescriptorChain(
        txid=2, first_iova32=0x80000420, first_blocks=2, last_blocks=1, payload_size=8
    ))

    # 3) Standard-only packet (rare for your AT, but legal model case)
    tests.append(DescriptorChain(
        txid=3, first_iova32=0x80002000, first_blocks=1, last_blocks=0, payload_size=0
    ))

    all_ok = True
    print("=== Built-in Z-nibble validation tests ===")
    for ch in tests:
        ok, msg = validate_chain(ch)
        print(msg)
        print()
        all_ok = all_ok and ok

    return all_ok


LOG_BLOCKS_RE = re.compile(r"blocks=(\d+)")
LOG_CMDPTR_RE = re.compile(r"cmdPtr=0x([0-9a-fA-F]+)")
LOG_PATH1_RE = re.compile(r"PATH1|P1_ARM|path1_armed|path1_start")


def parse_logfile(path: str) -> bool:
    """
    Parse kernel log and validate that when PATH1 arms:
       cmdPtr_low_nibble == blocks
    """
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.readlines()

    failures = 0
    total_checks = 0

    # We search windows around PATH1 lines, because blocks and cmdPtr may be on nearby lines.
    for i, line in enumerate(lines):
        if not LOG_PATH1_RE.search(line):
            continue

        # Search backward/forward a small window for blocks and cmdPtr.
        window = lines[max(0, i - 5): min(len(lines), i + 6)]
        blocks_val: Optional[int] = None
        cmdptr_val: Optional[int] = None

        for w in window:
            m_b = LOG_BLOCKS_RE.search(w)
            if m_b and blocks_val is None:
                blocks_val = int(m_b.group(1))

            m_c = LOG_CMDPTR_RE.search(w)
            if m_c and cmdptr_val is None:
                cmdptr_val = int(m_c.group(1), 16)

        if blocks_val is None or cmdptr_val is None:
            continue

        total_checks += 1
        low = cmdptr_val & 0xF

        if low != blocks_val:
            failures += 1
            print("=== PATH1 Z mismatch ===")
            print(f"Line {i+1}: {line.strip()}")
            print(f"  Detected blocks={blocks_val}, cmdPtr=0x{cmdptr_val:08X}, lowNibble={low}")
            print("  Expected lowNibble == blocks. This reproduces the CAS-style bug.")
            print()

    if total_checks == 0:
        print("No PATH1 arming checks found in log (no blocks/cmdPtr pairs near PATH1 lines).")
        return True

    if failures == 0:
        print(f"All PATH1 log checks passed ({total_checks} checks). ✅")
        return True

    print(f"{failures}/{total_checks} PATH1 checks FAILED. ❌")
    return False


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate OHCI AT CommandPtr Z-nibble logic.")
    parser.add_argument("--blocks", type=int, default=None,
                        help="Total packet blocks to validate (e.g., 3 for CAS).")
    parser.add_argument("--first-immediate", type=int, choices=[0, 1], default=None,
                        help="Whether first descriptor is immediate (1) or standard (0).")
    parser.add_argument("--iova", type=str, default=None,
                        help="First descriptor IOVA32 (hex), e.g. 0x80000420.")
    parser.add_argument("--logfile", type=str, default=None,
                        help="Kernel log file to scan for PATH1 cmdPtr/Z mismatches.")

    args = parser.parse_args()

    ok = True

    # Built-in tests always run unless user explicitly only wants logfile.
    if args.logfile is None and args.blocks is None and args.iova is None:
        ok = run_builtin_tests()

    # Manual single-case validation.
    if args.blocks is not None and args.iova is not None:
        iova_val = int(args.iova, 16)
        blocks_val = args.blocks
        # Infer first_blocks from first-immediate if provided, else from blocks.
        if args.first_immediate is None:
            first_blocks = 2 if blocks_val >= 2 else 1
        else:
            first_blocks = 2 if args.first_immediate == 1 else 1

        last_blocks = max(0, blocks_val - first_blocks)
        payload_size = 0 if last_blocks == 0 else 8  # assume CAS-like payload for 2-descriptor case

        chain = DescriptorChain(
            txid=999,
            first_iova32=iova_val,
            first_blocks=first_blocks,
            last_blocks=last_blocks,
            payload_size=payload_size
        )

        ok_case, msg = validate_chain(chain)
        print(msg)
        ok = ok and ok_case

    # Logfile validation.
    if args.logfile is not None:
        ok_log = parse_logfile(args.logfile)
        ok = ok and ok_log

    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
