#!/usr/bin/env python3
"""
IR Descriptor Analysis Tool - per OHCI 1.1 §10.1.1

IR descriptor control word layout (bits 31:16 of first quadlet):
  [31:28] cmd     - 4 bits: 2=INPUT_MORE, 3=INPUT_LAST
  [27]    s       - 1 bit:  status update (1=update xferStatus/resCount)
  [26:24] key     - 3 bits: must be 0
  [23:22] (reserved)
  [21:20] i       - 2 bits: interrupt (0=never, 3=always)
  [19:18] b       - 2 bits: branch (0=never, 3=always)
  [17:16] w       - 2 bits: wait for sync (0=don't wait)
  [15:0]  reqCount - 16 bits: buffer size

branchWord layout:
  [31:4]  branchAddress - 28 bits: 16-byte aligned next descriptor address
  [3:0]   Z             - 4 bits: 0=last, 1-8=count of descriptors to fetch

For buffer-fill mode continuous ring:
  - b = 3 (branch always)
  - Z = 1 (fetch next single descriptor)
  - s = 1 (update status)
"""

import struct
from dataclasses import dataclass

@dataclass
class IRDescriptor:
    """OHCI Isochronous Receive descriptor (16 bytes, little-endian)."""
    control: int
    dataAddress: int
    branchWord: int
    statusWord: int
    
    @classmethod
    def from_bytes(cls, data: bytes) -> 'IRDescriptor':
        control, dataAddr, branch, status = struct.unpack('<IIII', data[:16])
        return cls(control, dataAddr, branch, status)
    
    # Control word fields (high 16 bits)
    @property
    def cmd(self) -> int:
        return (self.control >> 28) & 0xF
    
    @property
    def s(self) -> int:
        """Status update bit."""
        return (self.control >> 27) & 0x1
    
    @property
    def key(self) -> int:
        return (self.control >> 24) & 0x7
    
    @property
    def i(self) -> int:
        """Interrupt control."""
        return (self.control >> 20) & 0x3
    
    @property
    def b(self) -> int:
        """Branch control."""
        return (self.control >> 18) & 0x3
    
    @property
    def w(self) -> int:
        """Wait control."""
        return (self.control >> 16) & 0x3
    
    @property
    def reqCount(self) -> int:
        return self.control & 0xFFFF
    
    # Branch word fields
    @property
    def branchAddress(self) -> int:
        return self.branchWord & 0xFFFFFFF0
    
    @property
    def Z(self) -> int:
        return self.branchWord & 0xF
    
    # Status word fields
    @property
    def xferStatus(self) -> int:
        return (self.statusWord >> 16) & 0xFFFF
    
    @property
    def resCount(self) -> int:
        return self.statusWord & 0xFFFF
    
    def __str__(self):
        cmd_names = {2: 'INPUT_MORE', 3: 'INPUT_LAST'}
        cmd_name = cmd_names.get(self.cmd, f'CMD_{self.cmd}')
        
        issues = []
        if self.key != 0:
            issues.append(f"key={self.key} (should be 0)")
        if self.b != 3:
            issues.append(f"b={self.b} (should be 3 for branch)")
        if self.Z == 0:
            issues.append("Z=0 (STOPS context! Should be 1)")
        if self.s == 0:
            issues.append("s=0 (no status update)")
        
        issue_str = " ⚠️ ISSUES: " + ", ".join(issues) if issues else " ✓ OK"
        
        return (f"IR Desc: ctl=0x{self.control:08x} cmd={cmd_name} s={self.s} key={self.key} "
                f"i={self.i} b={self.b} w={self.w} req={self.reqCount}\n"
                f"         data=0x{self.dataAddress:08x} branch=0x{self.branchAddress:08x} Z={self.Z}\n"
                f"         status=0x{self.statusWord:08x} xfer=0x{self.xferStatus:04x} res={self.resCount}"
                f"{issue_str}")

    @staticmethod
    def build_control(reqCount: int, cmd: int = 2, s: int = 1, i: int = 0, b: int = 3, w: int = 0) -> int:
        """Build IR descriptor control word per OHCI §10.1.1."""
        key = 0  # Always 0 for IR
        high = ((cmd & 0xF) << 12) | ((s & 0x1) << 11) | ((key & 0x7) << 8) | \
               ((i & 0x3) << 4) | ((b & 0x3) << 2) | (w & 0x3)
        return (high << 16) | (reqCount & 0xFFFF)
    
    @staticmethod
    def build_branch(addr: int, Z: int = 1) -> int:
        """Build branchWord. Z=1 for continuous ring, Z=0 for last descriptor."""
        return (addr & 0xFFFFFFF0) | (Z & 0xF)


def analyze_from_log():
    """Analyze descriptor from driver logs."""
    # From log: ctl=0x200c1000 data=0x803b8000 br=0x803a8010 stat=0x00001000
    print("=" * 70)
    print("Analyzing descriptor from driver logs")
    print("=" * 70)
    
    desc_bytes = struct.pack('<IIII', 0x200c1000, 0x803b8000, 0x803a8010, 0x00001000)
    desc = IRDescriptor.from_bytes(desc_bytes)
    print(desc)
    
    # Decode what the current BuildControl produces
    print("\n" + "-" * 70)
    print("Current BuildControl() interpretation (assuming AT layout):")
    print("-" * 70)
    ctl = 0x200c1000
    # AT layout: cmd(4) key(3) i(2) b(2) ... 
    at_cmd = (ctl >> 28) & 0xF
    at_key = (ctl >> 24) & 0x7
    at_i = (ctl >> 20) & 0x3
    at_b = (ctl >> 18) & 0x3
    print(f"  AT decode: cmd={at_cmd} key={at_key} i={at_i} b={at_b}")
    print(f"  This looks correct for AT, but IR has different layout!")

    print("\n" + "-" * 70)
    print("Correct IR descriptor values for buffer-fill mode:")
    print("-" * 70)
    correct_ctl = IRDescriptor.build_control(
        reqCount=4096, 
        cmd=2,    # INPUT_MORE
        s=1,      # Update status
        i=0,      # No interrupt (or 3 for every 8th)
        b=3,      # Branch always
        w=0       # Don't wait
    )
    correct_branch = IRDescriptor.build_branch(0x803a8010, Z=1)
    print(f"  control:     0x{correct_ctl:08x} (current: 0x200c1000)")
    print(f"  branchWord:  0x{correct_branch:08x} (current: 0x803a8010)")
    
    # Check if current matches expected
    print("\n" + "-" * 70)
    print("DIAGNOSIS:")
    print("-" * 70)
    current_z = 0x803a8010 & 0xF
    print(f"  Current branchWord Z value: {current_z}")
    if current_z == 0:
        print("  ❌ Z=0 means 'last descriptor' - context STOPS after this one!")
        print("  ✅ Fix: Set Z=1 to continue to next descriptor")
    else:
        print(f"  ✓ Z={current_z} is correct for continuation")


def main():
    analyze_from_log()
    
    print("\n" + "=" * 70)
    print("IR Control Word Bit Layout (OHCI §10.1.1)")
    print("=" * 70)
    print("""
    Bits 31:16 (high word):
    ┌───────┬───┬───────┬─────┬───┬───┬───┐
    │cmd(4) │s(1)│key(3) │res(2)│i(2)│b(2)│w(2)│
    └───────┴───┴───────┴─────┴───┴───┴───┘
    
    Bits 15:0 (low word):
    ┌─────────────────────────────────────┐
    │           reqCount (16)             │
    └─────────────────────────────────────┘
    
    For buffer-fill continuous ring:
      cmd = 2 (INPUT_MORE)
      s   = 1 (update status)  ← CRITICAL for polling!
      key = 0 (required)
      i   = 0 or 3 (interrupt control)
      b   = 3 (branch always)  ← CRITICAL for ring!
      w   = 0 (don't wait)
      Z   = 1 (in branchWord)  ← CRITICAL! Z=0 STOPS context!
    """)


if __name__ == '__main__':
    main()
