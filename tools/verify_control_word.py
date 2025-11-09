#!/usr/bin/env python3
"""
Verify OHCI 1.2 control word building logic.
Compares our implementation against Apple's known-good value.
"""

def build_control_word(reqCount, cmd, key, i, b, ping=False):
    """
    Build OHCI descriptor control word.
    Current implementation from OHCI_HW_Specs.hpp
    """
    # Shift constants - CORRECTED VALUES
    kCmdShift = 12
    kKeyShift = 8      # Fixed: was 9
    kPingShift = 7
    kIntShift = 4      # Fixed: was 6
    kBranchShift = 2   # Fixed: was 4
    kControlHighShift = 16
    
    # Mask inputs per OHCI 1.2 field widths
    cmd_masked = cmd & 0xF
    key_masked = key & 0x7
    i_masked = i & 0x3
    b_masked = b & 0x3
    
    high = (
        (cmd_masked << kCmdShift) |
        (key_masked << kKeyShift) |
        (i_masked << kIntShift) |
        (b_masked << kBranchShift) |
        ((1 << kPingShift) if ping else 0)
    )
    
    control = ((high & 0xFFFF) << kControlHighShift) | (reqCount & 0xFFFF)
    return control

def decode_control_word(control):
    """Decode control word to show field values."""
    reqCount = control & 0xFFFF
    high = (control >> 16) & 0xFFFF
    
    # Extract fields using CORRECTED shifts
    cmd = (high >> 12) & 0xF
    key = (high >> 8) & 0x7   # Fixed: was >> 9
    ping = (high >> 7) & 0x1  # Fixed: was >> 8
    i = (high >> 4) & 0x3     # Fixed: was >> 6
    b = (high >> 2) & 0x3     # Fixed: was >> 4
    wait = (high >> 0) & 0x3
    
    return {
        'reqCount': reqCount,
        'cmd': cmd,
        'key': key,
        'ping': ping,
        'i': i,
        'b': b,
        'wait': wait,
        'high': high
    }

def test_apple_reference():
    """Test against Apple's known-good control word."""
    print("=" * 70)
    print("TEST 1: Apple Reference Case (quadlet read)")
    print("=" * 70)
    
    # Apple's actual control word from decompilation
    apple_control = 0x123C000C
    
    # Parameters for quadlet read
    reqCount = 12  # 12-byte header
    cmd = 1        # OUTPUT_LAST
    key = 2        # Immediate
    i = 3          # Always interrupt
    b = 3          # Always branch (Apple uses this!)
    ping = False
    
    our_control = build_control_word(reqCount, cmd, key, i, b, ping)
    
    print(f"\nApple's control word: 0x{apple_control:08X}")
    print(f"Our control word:     0x{our_control:08X}")
    print(f"Match: {'✅ YES' if our_control == apple_control else '❌ NO'}")
    
    print("\nApple's decoded fields:")
    apple_decoded = decode_control_word(apple_control)
    for field, value in apple_decoded.items():
        if field == 'high':
            print(f"  {field:10s} = 0x{value:04X}")
        else:
            print(f"  {field:10s} = {value}")
    
    print("\nOur decoded fields:")
    our_decoded = decode_control_word(our_control)
    for field, value in our_decoded.items():
        if field == 'high':
            print(f"  {field:10s} = 0x{value:04X}")
        else:
            print(f"  {field:10s} = {value}")
    
    return our_control == apple_control

def test_logged_case():
    """Test the actual logged value from kernel logs."""
    print("\n" + "=" * 70)
    print("TEST 2: Actual Logged Case (from kernel)")
    print("=" * 70)
    
    # What we're actually logging
    logged_control = 0x14C0000C
    
    print(f"\nLogged control word: 0x{logged_control:08X}")
    
    print("\nLogged decoded fields:")
    logged_decoded = decode_control_word(logged_control)
    for field, value in logged_decoded.items():
        if field == 'high':
            print(f"  {field:10s} = 0x{value:04X}")
        else:
            print(f"  {field:10s} = {value}")
    
    # What we're trying to build
    reqCount = 12
    cmd = 1        # OUTPUT_LAST
    key = 2        # Immediate
    i = 3          # Always interrupt
    b = 0          # BranchNever (EOL)
    ping = False
    
    expected = build_control_word(reqCount, cmd, key, i, b, ping)
    print(f"\nExpected (b=0 EOL):  0x{expected:08X}")
    print(f"Match: {'✅ YES' if expected == logged_control else '❌ NO'}")

def analyze_bit_positions():
    """Analyze where each field actually is in the control word."""
    print("\n" + "=" * 70)
    print("TEST 3: Bit Position Analysis")
    print("=" * 70)
    
    print("\nApple's 0x123C:")
    print("  Binary: 0001 0010 0011 1100")
    print("  Bits:   15-12   11-8    7-4     3-0")
    print("          cmd=1   ????    ????    ????")
    
    # Let's try to deduce the correct shifts
    print("\nTrying different key shifts:")
    for key_shift in range(7, 11):
        high = (1 << 12) | (2 << key_shift)
        print(f"  key @ shift {key_shift}: 0x{high:04X} (cmd=1, key=2)")
    
    print("\nTrying to match 0x123C with cmd=1, key=2, i=3, b=3:")
    # We know cmd=1 is at shift 12, so bits [15:12] = 0001
    # Remaining 0x23C needs to encode key=2, i=3, b=3
    
    # Try all combinations
    for key_shift in range(6, 12):
        for i_shift in range(4, 9):
            for b_shift in range(0, 7):
                if i_shift == b_shift or key_shift == i_shift or key_shift == b_shift:
                    continue  # Skip overlaps
                
                high = (1 << 12) | (2 << key_shift) | (3 << i_shift) | (3 << b_shift)
                if high == 0x123C:
                    print(f"\n✅ MATCH FOUND:")
                    print(f"   key_shift = {key_shift}")
                    print(f"   i_shift = {i_shift}")
                    print(f"   b_shift = {b_shift}")
                    print(f"   Formula: (1<<12) | (2<<{key_shift}) | (3<<{i_shift}) | (3<<{b_shift}) = 0x{high:04X}")

def main():
    print("\nOHCI 1.2 Control Word Verification Tool")
    print("========================================\n")
    
    test1_pass = test_apple_reference()
    test_logged_case()
    analyze_bit_positions()
    
    print("\n" + "=" * 70)
    print("SUMMARY")
    print("=" * 70)
    if test1_pass:
        print("✅ Control word building logic matches Apple's implementation")
    else:
        print("❌ Control word building logic DOES NOT match Apple")
        print("   Check the bit position analysis above for correct shift values")

if __name__ == '__main__':
    main()
