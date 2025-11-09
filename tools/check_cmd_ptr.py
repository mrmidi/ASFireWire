val = 0x80000022
addr = val & 0xFFFFFFF0
z = val & 0xF
print(f'CommandPtr Value: 0x{val:08X}')
print(f'  Address bits [31:4]: 0x{addr:08X}')
print(f'  Z field [3:0]: {z} (0x{z:X})')
print()
print('Analysis:')
aligned = "16-byte aligned" if (addr & 0xF) == 0 else "NOT ALIGNED"
print(f'  - Address: 0x{addr:08X} ({aligned})')
print(f'  - Z value: {z}')
if z == 0:
    print('    -> End of list marker')
elif z == 1:
    print('    -> RESERVED (invalid per OHCI spec Table 7-5)')
elif 2 <= z <= 8:
    print(f'    -> Valid descriptor size: {z} blocks = {z*16} bytes')
else:
    print(f'    -> RESERVED (invalid per OHCI spec Table 7-5)')
print()
print('CRITICAL ANALYSIS:')
print(f'  Bits [31:4] = 0x{addr >> 4:07X} (address)')
print(f'  Bits [3:0]  = 0x{val & 0xF:X} (Z field = {val & 0xF})')
print(f'  Bits [1:0]  = 0x{val & 0x3:X} (reserved, should be 0)')
print()
if (val & 0x3) != 0:
    print('BUG DETECTED:')
    print(f'  Reserved bits [1:0] are {val & 0x3}, should be 0!')
    print(f'  This violates OHCI spec Table 7-3')
    print(f'  Correct value would be: 0x{addr | ((z & 0xC) >> 0):08X}')
else:
    print('VALUE LOOKS CORRECT per OHCI spec')