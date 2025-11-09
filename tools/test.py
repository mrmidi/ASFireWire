# This script tests the packing of OHCI command/control fields
# according to both OHCI 1.2 and OHCI 1.1 specifications.
# The issue is that all of my attempts with OHCI 1.1 mapping
# produced unexpected results for status words

from dataclasses import dataclass

@dataclass
class Fields:
    cmd:int=1      # OUTPUT_LAST
    status:int=0   # explicit in OHCI 1.2
    key:int=2      # IMMEDIATE
    p:int=0        # ping bit
    yy:int=0       # reserved duo bits (should be 0)
    irq:int=3      # ALWAYS
    branch:int=3   # ALWAYS (required for Immediate)
    wait:int=0     # 0
    req:int=12     # 12 bytes

def pack_ohci12(f: Fields) -> tuple[int, int]:
    # OHCI 1.2 (Linux/Apple): [15:12]=cmd, [11]=status, [10:8]=key, [7]=p, [6]=yy, [5:4]=irq, [3:2]=branch, [1:0]=wait
    hi = (f.cmd   << 12) \
       | (f.status<< 11) \
       | (f.key   <<  8) \
       | (f.p     <<  7) \
       | (f.yy    <<  6) \
       | (f.irq   <<  4) \
       | (f.branch<<  2) \
       | (f.wait  <<  0)
    return (hi << 16) | f.req, hi

def pack_ohci11(f: Fields) -> tuple[int, int]:
    # OHCI 1.1 (legacy/incorrect for Linux): [15:12]=cmd, [11:9]=key, [8]=p, [7:6]=irq, [5:4]=branch, [1:0]=wait
    hi = (f.cmd   << 12) \
       | (f.key   <<  9) \
       | (f.p     <<  8) \
       | (f.irq   <<  6) \
       | (f.branch<<  4) \
       | (f.wait  <<  0)
    return (hi << 16) | f.req, hi

def hx(x: int) -> str:
    return f"0x{x:08X}"

# Case A: OHCI 1.2 w/o STATUS (Linux/Apple typical for immediate)
f = Fields(status=0)
c12_no_status, hi12_no_status = pack_ohci12(f)

# Case B: OHCI 1.2 with STATUS=1
f_stat = Fields(status=1)
c12_status, hi12_status = pack_ohci12(f_stat)

# Case C: OHCI 1.1 (legacy mapping)
c11, hi11 = pack_ohci11(Fields())

print("OHCI 1.2 (no STATUS):")
print("  high    =", f"0x{hi12_no_status:04X}")
print("  control =", hx(c12_no_status))

print("\nOHCI 1.2 (with STATUS=1):")
print("  high    =", f"0x{hi12_status:04X}")
print("  control =", hx(c12_status))

print("\nOHCI 1.1 (legacy mapping):")
print("  high    =", f"0x{hi11:04X}")
print("  control =", hx(c11))

# CommandPtr example
firstPhys = 0x80000020
z = 2
command_ptr = firstPhys | z
print("\nCommandPtr example: firstPhys=0x80000020, z=2")
print("  CommandPtr =", hx(command_ptr))