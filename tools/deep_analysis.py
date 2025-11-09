#!/usr/bin/env python3
"""
Deep analysis of Apple's 0x123C000C control word.
"""

def analyze_apple_control():
    """Analyze Apple's exact control word bit by bit."""
    
    apple = 0x123C000C
    high = (apple >> 16) & 0xFFFF
    reqCount = apple & 0xFFFF
    
    print("Apple's Control Word: 0x123C000C")
    print("=" * 70)
    print(f"High word: 0x{high:04X} = 0b{high:016b}")
    print(f"Low word:  0x{reqCount:04X} = {reqCount} (reqCount)")
    
    print("\nBit-by-bit breakdown of high word 0x123C:")
    print("  Bits [15:12] = 0b0001 = 1 (cmd = OUTPUT_LAST) ✓")
    print("  Bits [11:8]  = 0b0010 = 2")
    print("  Bits [7:4]   = 0b0011 = 3")
    print("  Bits [3:0]   = 0b1100 = 12")
    
    print("\n" + "=" * 70)
    print("INTERPRETATION OPTIONS:")
    print("=" * 70)
    
    print("\nOption 1: OHCI 1.1 bit positions")
    print("  [15:12] cmd=1 ✓")
    print("  [11:9]  key=2 → bits[11:9]=0b001=1 ❌ (we need key=2)")
    print("  [8]     ping=0 ✓")
    print("  [7:6]   i=3 → bits[7:6]=0b01=1 ❌ (we need i=3)")
    print("  [5:4]   b=3 → bits[5:4]=0b11=3 ✓")
    print("  DOESN'T MATCH!")
    
    print("\nOption 2: Our current (wrong) shifts")
    print("  [15:12] cmd=1 ✓")
    print("  [11:9]  key @ 9 → 0b001=1 ❌ (we set key=2)")
    print("  [8]     ping @ 8 → 0")
    print("  [7:6]   i @ 6 → 0b00=0 ❌ (we set i=3)")
    print("  [5:4]   b @ 4 → 0b11=3 ✓ (but we're setting b=0!)")
    
    print("\nOption 3: Working backwards from 0x123C")
    # We know:
    # - cmd=1 at [15:12] → 0x1000
    # - Remaining is 0x023C
    # Let's see what fields that could be
    
    remaining = 0x023C
    print(f"  After cmd=1: remaining = 0x{remaining:04X} = 0b{remaining:012b}")
    
    # Try: bits[11:10] unused, [9:8] key, [7:6] ??, [5:4] i, [3:2] b, [1:0] wait
    print("\n  IF key at [9:8], i at [5:4], b at [3:2]:")
    key_test = (remaining >> 8) & 0x3
    i_test = (remaining >> 4) & 0x3
    b_test = (remaining >> 2) & 0x3
    wait_test = remaining & 0x3
    print(f"    key=[9:8]={key_test} (need 2, have {key_test}) ❌")
    print(f"    i=[5:4]={i_test} (need 3, have {i_test}) ✓")
    print(f"    b=[3:2]={b_test} (need 3, have {b_test}) ✓")
    print(f"    wait=[1:0]={wait_test}")
    
    # Try: bits[10:8] key, [7:6] unused, [5:4] i, [3:2] b
    print("\n  IF key at [10:8], i at [5:4], b at [3:2]:")
    key_test = (remaining >> 8) & 0x7
    i_test = (remaining >> 4) & 0x3
    b_test = (remaining >> 2) & 0x3
    print(f"    key=[10:8]={key_test} (need 2, have {key_test}) ✓")
    print(f"    i=[5:4]={i_test} (need 3, have {i_test}) ✓")
    print(f"    b=[3:2]={b_test} (need 3, have {b_test}) ✓")
    print("    ✅ THIS MATCHES!")
    
    print("\n" + "=" * 70)
    print("CORRECT BIT LAYOUT (within high 16-bit word):")
    print("=" * 70)
    print("  [15:12] cmd   (4 bits) - shift 12")
    print("  [11]    ?? (unused or reserved)")
    print("  [10:8]  key   (3 bits) - shift 8")
    print("  [7:6]   ?? (unused or ping/status)")
    print("  [5:4]   i     (2 bits) - shift 4")
    print("  [3:2]   b     (2 bits) - shift 2")
    print("  [1:0]   wait  (2 bits) - shift 0")
    
    print("\nCORRECT SHIFT CONSTANTS:")
    print("  kCmdShift = 12")
    print("  kKeyShift = 8  (NOT 9!)")
    print("  kIntShift = 4  (NOT 6!)")
    print("  kBranchShift = 2  (NOT 4!)")
    
    print("\nVerification:")
    ctl = (1 << 12) | (2 << 8) | (3 << 4) | (3 << 2)
    print(f"  (1<<12) | (2<<8) | (3<<4) | (3<<2) = 0x{ctl:04X}")
    print(f"  Matches 0x123C: {'✅ YES' if ctl == 0x123C else '❌ NO'}")

if __name__ == '__main__':
    analyze_apple_control()
