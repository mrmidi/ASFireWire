#!/usr/bin/env python3
"""
Analyze Isochronous packet data and OHCI descriptor formats.

Endianness notes:
- OHCI descriptors: LITTLE-ENDIAN (host CPU order on x86/ARM)
- CIP headers (Q0, Q1): BIG-ENDIAN (wire order)
- AM824 audio samples: BIG-ENDIAN (wire order)
- FireBug displays everything in LITTLE-ENDIAN (confusing!)

Usage:
    python3 analyze_isoch.py
"""

import struct
from dataclasses import dataclass
from typing import Optional

# ============================================================================
# OHCI Descriptor (16 bytes, LITTLE-ENDIAN)
# ============================================================================
@dataclass
class OHCIDescriptor:
    """OHCI descriptor as seen by CPU (little-endian)."""
    control: int      # offset 0: reqCount[15:0], cmd[31:28], key[27:25], i[25:24], b[23:22], etc.
    dataAddress: int  # offset 4: DMA address of buffer
    branchWord: int   # offset 8: next descriptor address | Z
    statusWord: int   # offset 12: xferStatus[15:0] | resCount[15:0]
    
    @classmethod
    def from_bytes(cls, data: bytes) -> 'OHCIDescriptor':
        """Parse 16 bytes as OHCI descriptor (little-endian)."""
        control, dataAddr, branch, status = struct.unpack('<IIII', data[:16])
        return cls(control, dataAddr, branch, status)
    
    @property
    def reqCount(self) -> int:
        return self.control & 0xFFFF
    
    @property
    def cmd(self) -> int:
        return (self.control >> 28) & 0xF
    
    @property
    def xferStatus(self) -> int:
        return (self.statusWord >> 16) & 0xFFFF
    
    @property
    def resCount(self) -> int:
        return self.statusWord & 0xFFFF
    
    @property
    def bytesReceived(self) -> int:
        return self.reqCount - self.resCount if self.resCount <= self.reqCount else 0
    
    def __str__(self):
        cmd_names = {0: 'OUTPUT_MORE', 1: 'OUTPUT_LAST', 2: 'INPUT_MORE', 3: 'INPUT_LAST'}
        cmd_name = cmd_names.get(self.cmd, f'CMD_{self.cmd}')
        return (f"OHCI Desc: ctl=0x{self.control:08x} ({cmd_name}, req={self.reqCount}) "
                f"data=0x{self.dataAddress:08x} br=0x{self.branchWord:08x} "
                f"stat=0x{self.statusWord:08x} (xfer=0x{self.xferStatus:04x} res={self.resCount} recv={self.bytesReceived})")


# ============================================================================
# IEC 61883-1 CIP Header (8 bytes, BIG-ENDIAN)
# ============================================================================
@dataclass
class CIPHeader:
    """Common Isochronous Packet header (big-endian wire format)."""
    q0: int  # Quadlet 0: SID[5:0], DBS[7:0], FN[1:0], QPC[2:0], SPH, DBC[7:0]
    q1: int  # Quadlet 1: FMT[5:0], FDF[23:0] or FDF[7:0]+SYT[15:0]
    
    @classmethod
    def from_bytes(cls, data: bytes) -> 'CIPHeader':
        """Parse 8 bytes as CIP header (big-endian)."""
        q0, q1 = struct.unpack('>II', data[:8])
        return cls(q0, q1)
    
    @property
    def sid(self) -> int:
        """Source ID (node that created the packet)."""
        return (self.q0 >> 24) & 0x3F
    
    @property
    def dbs(self) -> int:
        """Data Block Size (in quadlets)."""
        return (self.q0 >> 16) & 0xFF
    
    @property
    def fn(self) -> int:
        """Fraction Number."""
        return (self.q0 >> 14) & 0x03
    
    @property
    def qpc(self) -> int:
        """Quadlet Padding Count."""
        return (self.q0 >> 11) & 0x07
    
    @property
    def sph(self) -> int:
        """Source Packet Header flag."""
        return (self.q0 >> 10) & 0x01
    
    @property
    def dbc(self) -> int:
        """Data Block Counter (8-bit, wraps at 256)."""
        return self.q0 & 0xFF
    
    @property
    def fmt(self) -> int:
        """Format field."""
        return (self.q1 >> 24) & 0x3F
    
    @property
    def fdf(self) -> int:
        """Format Dependent Field (for AM824: contains SFC)."""
        return (self.q1 >> 16) & 0xFF
    
    @property 
    def syt(self) -> int:
        """Synchronization timestamp."""
        return self.q1 & 0xFFFF
    
    @property
    def is_empty(self) -> bool:
        """Check if this is an empty (NO-DATA) packet."""
        return self.syt == 0xFFFF
    
    def __str__(self):
        status = "EMPTY" if self.is_empty else f"SYT=0x{self.syt:04x}"
        return (f"CIP: Q0=0x{self.q0:08x} Q1=0x{self.q1:08x} | "
                f"SID={self.sid} DBS={self.dbs} DBC=0x{self.dbc:02x} FMT=0x{self.fmt:02x} FDF=0x{self.fdf:02x} {status}")


# ============================================================================
# AM824 Audio Sample (4 bytes per channel, BIG-ENDIAN)
# ============================================================================
@dataclass
class AM824Sample:
    """AM824 audio sample (1 quadlet per channel)."""
    raw: int  # Full 32-bit quadlet
    
    @classmethod
    def from_bytes(cls, data: bytes) -> 'AM824Sample':
        """Parse 4 bytes as AM824 sample (big-endian)."""
        raw, = struct.unpack('>I', data[:4])
        return cls(raw)
    
    @property
    def label(self) -> int:
        """Label byte (bits 31:24)."""
        return (self.raw >> 24) & 0xFF
    
    @property
    def audio_24bit(self) -> int:
        """24-bit audio sample (bits 23:0), signed."""
        val = self.raw & 0xFFFFFF
        # Sign extend from 24 to 32 bits
        if val & 0x800000:
            val |= 0xFF000000
        return val - 0x100000000 if val & 0x80000000 else val
    
    @property
    def label_type(self) -> str:
        """Decode label type."""
        if self.label == 0x40:
            return "PCM"
        elif self.label == 0x00:
            return "MIDI/Empty"
        elif self.label == 0x80:
            return "Raw MIDI"
        elif (self.label & 0xF0) == 0x80:
            return f"MIDI ch{self.label & 0x0F}"
        else:
            return f"Label=0x{self.label:02x}"
    
    def __str__(self):
        return f"AM824: 0x{self.raw:08x} ({self.label_type}, audio={self.audio_24bit})"


# ============================================================================
# Analysis Functions
# ============================================================================

def parse_isoch_packet(data: bytes) -> tuple:
    """Parse a complete isochronous packet (CIP + payload)."""
    if len(data) < 8:
        return None, []
    
    cip = CIPHeader.from_bytes(data[:8])
    samples = []
    
    if not cip.is_empty and len(data) > 8:
        # Parse AM824 samples after CIP header
        payload = data[8:]
        num_samples = len(payload) // 4
        for i in range(num_samples):
            sample_data = payload[i*4:(i+1)*4]
            if len(sample_data) == 4:
                samples.append(AM824Sample.from_bytes(sample_data))
    
    return cip, samples


def hex_to_bytes(hex_str: str) -> bytes:
    """Convert hex string (with or without spaces) to bytes."""
    hex_clean = hex_str.replace(' ', '').replace('\n', '')
    return bytes.fromhex(hex_clean)


# ============================================================================
# Test with FireBug captured packets
# ============================================================================

if __name__ == '__main__':
    print("=" * 60)
    print("Isochronous Packet Analyzer")
    print("=" * 60)
    
    # Example packet from FireBug (already in WIRE order, which is BIG-ENDIAN)
    # FireBug shows: 02020050 9002ffff (8 bytes, empty packet)
    print("\n--- Empty Packet Example ---")
    empty_pkt = hex_to_bytes("02020050 9002ffff")
    cip, samples = parse_isoch_packet(empty_pkt)
    print(cip)
    print(f"  Empty packet (NO-DATA): {cip.is_empty}")
    
    # Example with audio samples
    # FireBug shows: 02020050 9002863b 4000002a 40000049 40000049 40000079 ...
    print("\n--- Audio Packet Example ---")
    audio_pkt = hex_to_bytes("02020050 9002863b 4000002a 40000049 40000049 40000079 40000036 40000047")
    cip, samples = parse_isoch_packet(audio_pkt)
    print(cip)
    print(f"  Data samples: {len(samples)}")
    for i, s in enumerate(samples):
        print(f"    [{i}] {s}")
    
    # Decode expected ContextMatch value
    print("\n--- ContextMatch Register Analysis ---")
    ctx_match = 0xF0000000  # From logs
    tag_mask = (ctx_match >> 28) & 0xF
    channel = (ctx_match >> 6) & 0x3F
    print(f"ContextMatch = 0x{ctx_match:08x}")
    print(f"  Tag mask: 0x{tag_mask:x} (accepts tags: {[i for i in range(4) if tag_mask & (1 << i)]})")
    print(f"  Channel: {channel}")
    
    # OHCI descriptor example (from log: ctl=0x200c1000 data=0x803b8000 br=0x803a8010 stat=0x00001000)
    print("\n--- OHCI Descriptor Analysis ---")
    desc_bytes = struct.pack('<IIII', 0x200c1000, 0x803b8000, 0x803a8010, 0x00001000)
    desc = OHCIDescriptor.from_bytes(desc_bytes)
    print(desc)
    print(f"  cmd nibble=0x{desc.cmd:x} (expect 2 for INPUT_MORE)")
    print(f"  reqCount={desc.reqCount} (expect 4096)")
    print(f"  resCount={desc.resCount} xferStatus=0x{desc.xferStatus:04x}")
    print(f"  Status interpretation: NO DATA RECEIVED (resCount == reqCount)")
    
    # Key insight about run=1 active=0
    print("\n--- IR Context State Analysis ---")
    ctl = 0x40008000  # From logs
    run = (ctl >> 15) & 1
    active = (ctl >> 10) & 1
    dead = (ctl >> 11) & 1
    isoch_header = (ctl >> 30) & 1
    print(f"ContextControl = 0x{ctl:08x}")
    print(f"  run={run} active={active} dead={dead} isochHeader={isoch_header}")
    print()
    if run == 1 and active == 0:
        print("  ⚠️ ISSUE: run=1 but active=0 means context is waiting!")
        print("  Possible causes:")
        print("    1. No matching packets on the wire (wrong channel/tag?)")
        print("    2. Context is waiting for first packet arrival")
        print("    3. CommandPtr not pointing to valid descriptor")
        print("    4. DMA coherency issue (descriptor not visible to hardware)")
