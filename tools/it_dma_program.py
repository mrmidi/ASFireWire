#!/usr/bin/env python3
"""
it_dma_program.py - IT DMA Descriptor Program Builder

Generates and visualizes Isochronous Transmit (IT) DMA descriptor programs
for OHCI 1.1 compliant FireWire controllers.

ARCHITECTURE (matches ohci.c):
- Descriptor layout: 16-byte struct with le16 req_count, le16 control, le32 data_address,
  le32 branch_address, le16 res_count, le16 transfer_status
- Immediate quadlets: IT header Q0 (speed/tag/channel/tcode/sy) + Q1 (dataLength<<16)
- CIP header is part of the PAYLOAD (big-endian), not in the immediate quadlets

Features:
- Full descriptor support (STORE_VALUE, OUTPUT_MORE, OUTPUT_LAST)
- Configurable skip strategies (Next, Self, Sentinel)
- Multi-fragment packet support (Scatter-Gather)
- Automated validation

Reference: docs/Isoch/IT_DMA.md, docs/protocols/61883-1.md

Usage:
    python it_dma_program.py --cycles 8 --channel 0 --rate 48000
    python it_dma_program.py --diagram
    python it_dma_program.py --cycles 4 --fragments 2 --validate
    python it_dma_program.py --analyze-log logfile.txt
"""

import argparse
import struct
import re
from dataclasses import dataclass, field
from enum import IntEnum, Enum
from typing import List, Tuple, Optional, Dict

# =============================================================================
# Constants
# =============================================================================

class Speed(IntEnum):
    S100 = 0
    S200 = 1
    S400 = 2
    S800 = 3

class TCode(IntEnum):
    ISOCH_DATA = 0xA

# Descriptor command bits (in 16-bit control field)
# Control field layout: [15:12]=cmd, [11]=s, [10:8]=key, [7:6]=unused, [5:4]=i, [3:2]=b, [1:0]=w
class ITCmd(IntEnum):
    OUTPUT_MORE = 0x0
    OUTPUT_LAST = 0x1
    STORE_VALUE = 0x8

class ITKey(IntEnum):
    STANDARD  = 0x0
    IMMEDIATE = 0x2
    STORE     = 0x6

class ITIrq(IntEnum):
    NONE   = 0x0
    ALWAYS = 0x3

class ITBranch(IntEnum):
    NEVER  = 0x0
    ALWAYS = 0x3

class ITWait(IntEnum):
    NEVER = 0x0

# Block alignment
BLOCK_SIZE = 16  # 16 bytes per descriptor block

# =============================================================================
# CIP Header Constants (IEC 61883-1 / AM824)
# =============================================================================

# AM824 format constants
FMT_AM824 = 0x10  # Audio & Music format
FDF_AM824_48K = 0x02  # 48kHz
FDF_AM824_44K = 0x00  # 44.1kHz
FDF_AM824_96K = 0x04  # 96kHz

# Default values
DEFAULT_SID = 0x3F  # Source ID placeholder (set by device)
DEFAULT_DBS = 8     # Data Block Size (8 quadlets = 8 channels)
DEFAULT_SPH = 0     # Source Packet Header (0 = not present)

# =============================================================================
# Control Field Builder (16-bit, matches ohci.c)
# =============================================================================

def make_control(cmd: ITCmd,
                 status_write: bool = False,
                 key: ITKey = ITKey.STANDARD,
                 irq: ITIrq = ITIrq.NONE,
                 branch: ITBranch = ITBranch.NEVER,
                 wait: ITWait = ITWait.NEVER) -> int:
    """
    Build 16-bit control field matching ohci.c struct descriptor.
    """
    s = 1 if status_write else 0
    control = ((int(cmd) & 0xF) << 12) | ((s & 0x1) << 11) | ((int(key) & 0x7) << 8)
    control |= ((int(irq) & 0x3) << 4) | ((int(branch) & 0x3) << 2) | (int(wait) & 0x3)
    return control & 0xFFFF

def pack_desc16(req_count: int,
                control: int,
                data_address: int,
                branch_address_with_z: int,
                res_count: int = 0,
                transfer_status: int = 0) -> bytes:
    """
    Pack 16-byte descriptor matching ohci.c struct descriptor.
    """
    return struct.pack(
        "<HHIIHH",
        req_count & 0xFFFF,
        control & 0xFFFF,
        data_address & 0xFFFFFFFF,
        branch_address_with_z & 0xFFFFFFFF,
        res_count & 0xFFFF,
        transfer_status & 0xFFFF,
    )

def pack_ptr_with_z(addr: int, z: int) -> int:
    """Pack address with Z value in low 4 bits."""
    return (addr & 0xFFFFFFF0) | (z & 0xF)

# =============================================================================
# IT Immediate Header Quadlets (matches ohci.c queue_iso_transmit)
# =============================================================================

def build_it_header_q0(speed: int, tag: int, channel: int, sy: int = 0, tcode: int = 0xA) -> int:
    """Build IT header quadlet 0 (internal OHCI format)."""
    q0 = 0
    q0 |= (speed & 0x7) << 16       # spd at bits 18:16
    q0 |= (tag & 0x3) << 14         # tag at bits 15:14
    q0 |= (channel & 0x3F) << 8     # chanNum at bits 13:8
    q0 |= (tcode & 0xF) << 4        # tcode at bits 7:4
    q0 |= (sy & 0xF)                # sy at bits 3:0
    return q0 & 0xFFFFFFFF

def build_it_header_q1(data_length_bytes: int) -> int:
    """Build IT header quadlet 1 (internal OHCI format)."""
    return ((data_length_bytes & 0xFFFF) << 16) & 0xFFFFFFFF

def pack_it_immediate16(it_q0: int, it_q1: int) -> bytes:
    """Pack 16 bytes of immediate data for OUTPUT_MORE-Immediate."""
    return struct.pack("<II", it_q0 & 0xFFFFFFFF, it_q1 & 0xFFFFFFFF) + b"\x00" * 8

# =============================================================================
# CIP Header Builders (IEC 61883-1, matches 61883-1.md)
# =============================================================================

def build_cip_q0(sid: int = DEFAULT_SID, dbs: int = DEFAULT_DBS, dbc: int = 0,
                 fn: int = 0, qpc: int = 0, sph: int = 0) -> int:
    """Build CIP header quadlet 0 per IEC 61883-1."""
    q0 = 0
    q0 |= (0b00 & 0x3) << 30        # EOH = 00
    q0 |= (sid & 0x3F) << 24        # SID
    q0 |= (dbs & 0xFF) << 16        # DBS
    q0 |= (fn & 0x3) << 14          # FN
    q0 |= (qpc & 0x7) << 11         # QPC
    q0 |= (sph & 0x1) << 10         # SPH
    q0 |= (0 & 0x3) << 8            # RSV
    q0 |= (dbc & 0xFF)              # DBC
    return q0 & 0xFFFFFFFF

def build_cip_q1(fmt: int = FMT_AM824, fdf: int = FDF_AM824_48K, 
                 syt: int = 0xFFFF) -> int:
    """Build CIP header quadlet 1 per IEC 61883-1."""
    q1 = 0
    q1 |= (0b10 & 0x3) << 30        # EOH = 10
    q1 |= (fmt & 0x3F) << 24        # FMT
    q1 |= (fdf & 0xFF) << 16        # FDF
    q1 |= (syt & 0xFFFF)            # SYT
    return q1 & 0xFFFFFFFF

def pack_cip_payload(cip_q0: int, cip_q1: int, audio_bytes: bytes = b"") -> bytes:
    """Pack CIP header + audio payload for DMA (big-endian)."""
    payload = struct.pack(">II", cip_q0 & 0xFFFFFFFF, cip_q1 & 0xFFFFFFFF)
    if audio_bytes:
        payload += audio_bytes
    return payload

def fdf_for_rate(sample_rate: int) -> int:
    """Get FDF (Format Dependent Field) for sample rate per IEC 61883-6."""
    fdf_map = {
        32000: 0x03,
        44100: FDF_AM824_44K,
        48000: FDF_AM824_48K,
        88200: 0x01,
        96000: FDF_AM824_96K,
        176400: 0x05,
        192000: 0x06,
    }
    if sample_rate not in fdf_map:
        raise ValueError(f"Unsupported sample rate for FDF: {sample_rate}")
    return fdf_map[sample_rate]

# =============================================================================
# Descriptor Data Classes
# =============================================================================

@dataclass
class ITDescriptor:
    """Base class for IT DMA descriptors."""
    size: int = 16
    
    def to_bytes(self) -> bytes:
        raise NotImplementedError

@dataclass
class StoreValue(ITDescriptor):
    """
    STORE_VALUE (16 bytes = 1 block).
    Writes a 32-bit value to host memory for status tracking.
    Must be the FIRST descriptor in a block.
    """
    size: int = 16
    
    store_value: int = 0
    store_address: int = 0      # Physical address to write to
    
    skip_address: int = 0       # Jump here if cycle lost
    skip_z: int = 0
    
    irq: bool = False
    
    def to_bytes(self) -> bytes:
        control = make_control(
            cmd=ITCmd.STORE_VALUE,
            status_write=False,
            key=ITKey.STORE,
            irq=ITIrq.ALWAYS if self.irq else ITIrq.NONE,
            branch=ITBranch.NEVER,
            wait=ITWait.NEVER,
        )
        return pack_desc16(
            req_count=self.store_value & 0xFFFF,     # Low 16 bits of value (Wait... OHCI might use special field)
            # Actually, per OHCI 1.1 §9.2.2.1: 
            # reqCount = lower 16 bits of value to store? 
            # Wait, 9.2.2.1 says:
            #   Offset 0: storeDoublet (upper 16 bits)
            #   Offset 4: dataAddress
            # No, STORE_VALUE is CMD=8, KEY=6
            # Layout: [cmd][key][i][storeDoublet] ?
            # IT_DMA.md says: [cmd=8][key=6][i][res][storeDoublet]
            # AND "storeDoublet" is just 16 bits.
            # So let's stick to IT_DMA.md definition which claims it writes a 32-bit value 0x0000|storeDoublet?
            # Or maybe just writes 16 bits? OHCI spec says "Store Value descriptor writes a 4-byte value...
            # The value written is the concatenation of 16-bits of zeros and the 16-bit storeDoublet field."
            # So pack_desc16 req_count maps to 'storeDoublet' field position.
            control=control,
            data_address=self.store_address,
            branch_address_with_z=pack_ptr_with_z(self.skip_address, self.skip_z),
            res_count=0,
            transfer_status=0,
        )

@dataclass
class OutputMoreImmediate(ITDescriptor):
    """OUTPUT_MORE-Immediate (32 bytes = 2 blocks)."""
    size: int = 32
    it_q0: int = 0
    it_q1: int = 0
    skip_address: int = 0
    skip_z: int = 0
    irq_on_skip: bool = False
    
    def to_bytes(self) -> bytes:
        # Custom Packing for Immediate (Key=2)
        # Offset 0: reqCount | Control
        # Offset 4: SkipAddress | Z  <-- CRITICAL FIX
        # Offset 8: IT Header Q0
        # Offset C: IT Header Q1
        
        control = make_control(
            cmd=ITCmd.OUTPUT_MORE,
            key=ITKey.IMMEDIATE,
            irq=ITIrq.ALWAYS if self.irq_on_skip else ITIrq.NONE,
        )
        
        desc = struct.pack(
            "<IIII",
            (8 & 0xFFFF) | ((control & 0xFFFF) << 16),
            pack_ptr_with_z(self.skip_address, self.skip_z),
            self.it_q0 & 0xFFFFFFFF,
            self.it_q1 & 0xFFFFFFFF
        )
        return desc + b"\x00" * 16 # Padding for second block

@dataclass
class OutputLastImmediate(ITDescriptor):
    """OUTPUT_LAST-Immediate (32 bytes = 2 blocks)."""
    size: int = 32
    it_q0: int = 0
    it_q1: int = 0
    branch_address: int = 0
    branch_z: int = 0
    write_status: bool = True
    irq_on_complete: bool = False
    
    def to_bytes(self) -> bytes:
        # Custom Packing for Immediate (Key=2)
        # Offset 0: reqCount | Control
        # Offset 4: BranchAddress | Z
        # Offset 8: IT Header Q0
        # Offset C: IT Header Q1
        # Offset 10: Status/Timestamp (Written by HW)
        
        control = make_control(
            cmd=ITCmd.OUTPUT_LAST,
            status_write=self.write_status,
            key=ITKey.IMMEDIATE,
            irq=ITIrq.ALWAYS if self.irq_on_complete else ITIrq.NONE,
            branch=ITBranch.ALWAYS,
        )
        
        desc = struct.pack(
            "<IIII",
            (8 & 0xFFFF) | ((control & 0xFFFF) << 16),
            pack_ptr_with_z(self.branch_address, self.branch_z),
            self.it_q0 & 0xFFFFFFFF,
            self.it_q1 & 0xFFFFFFFF
        )
        return desc + b"\x00" * 16 # Padding for second block



@dataclass
class OutputMore(ITDescriptor):
    """
    OUTPUT_MORE (16 bytes = 1 block).
    Standard scatter-gather descriptor (Key=0).
    """
    size: int = 16
    req_count: int = 0
    data_address: int = 0
    
    def to_bytes(self) -> bytes:
        control = make_control(
            cmd=ITCmd.OUTPUT_MORE,
            key=ITKey.STANDARD,
            irq=ITIrq.NONE,
            branch=ITBranch.NEVER,
        )
        return pack_desc16(
            req_count=self.req_count,
            control=control,
            data_address=self.data_address,
            branch_address_with_z=0, # Reserved for OUTPUT_MORE
        )

@dataclass
class OutputLast(ITDescriptor):
    """OUTPUT_LAST (16 bytes = 1 block)."""
    size: int = 16
    req_count: int = 0
    data_address: int = 0
    branch_address: int = 0
    branch_z: int = 0
    write_status: bool = True
    irq_on_complete: bool = False
    
    def to_bytes(self) -> bytes:
        control = make_control(
            cmd=ITCmd.OUTPUT_LAST,
            status_write=self.write_status,
            key=ITKey.STANDARD,
            irq=ITIrq.ALWAYS if self.irq_on_complete else ITIrq.NONE,
            branch=ITBranch.ALWAYS,
        )
        return pack_desc16(
            req_count=self.req_count,
            control=control,
            data_address=self.data_address,
            branch_address_with_z=pack_ptr_with_z(self.branch_address, self.branch_z),
        )

@dataclass
class OutputLastSkip(ITDescriptor):
    """OUTPUT_LAST (reqCount=0) - Skip Cycle."""
    size: int = 16
    branch_address: int = 0
    branch_z: int = 0
    irq_on_complete: bool = False
    
    def to_bytes(self) -> bytes:
        control = make_control(
            cmd=ITCmd.OUTPUT_LAST,
            status_write=True,
            key=ITKey.STANDARD,
            irq=ITIrq.ALWAYS if self.irq_on_complete else ITIrq.NONE,
            branch=ITBranch.ALWAYS,
        )
        return pack_desc16(
            req_count=0,
            control=control,
            data_address=0,
            branch_address_with_z=pack_ptr_with_z(self.branch_address, self.branch_z),
        )

# =============================================================================
# Helper Classes
# =============================================================================

class SkipStrategy(Enum):
    NEXT = "next"
    SELF = "self"
    SENTINEL = "sentinel"

@dataclass
class DescriptorBlock:
    """A complete descriptor block."""
    descriptors: List[ITDescriptor] = field(default_factory=list)
    address: int = 0
    
    @property
    def z_value(self) -> int:
        total_bytes = sum(d.size for d in self.descriptors)
        return (total_bytes + 15) // 16
    
    def to_bytes(self) -> bytes:
        return b''.join(d.to_bytes() for d in self.descriptors)

# =============================================================================
# Program Builder
# =============================================================================

class ITProgramBuilder:
    """Build IT DMA program with ring buffer."""
    
    def __init__(self, base_address: int = 0x80000000, 
                 channel: int = 0, speed: Speed = Speed.S400,
                 sample_rate: int = 48000,
                 skip_strategy: SkipStrategy = SkipStrategy.NEXT):
        self.base_address = base_address
        self.channel = channel
        self.speed = speed
        self.sample_rate = sample_rate
        self.skip_strategy = skip_strategy
        self.blocks: List[DescriptorBlock] = []
        self.dbc = 0
        
        self.payload_base = base_address + 0x10000
        self.payload_offset = 0
        
        # Sentinel block (for SENTINEL skip strategy)
        self.sentinel_addr = base_address + 0xFF00 # arbitrary end area
    
    def _next_block_address(self) -> int:
        if not self.blocks:
            return self.base_address
        last = self.blocks[-1]
        return last.address + last.z_value * BLOCK_SIZE
    
    def _alloc_payload(self, size: int) -> int:
        addr = self.payload_base + self.payload_offset
        self.payload_offset += (size + 15) & ~15
        return addr

    def add_data_packet(self, samples: int, channels: int = 2, # Default Stereo
                        fragments: int = 1, store_val: Optional[int] = None,
                        irq: bool = False) -> DescriptorBlock:
        """
        Add DATA packet.
        Supports:
        - Optional STORE_VALUE
        - Multiple fragments (Scatter-Gather) via OUTPUT_MORE
        """
        # Calculate payload
        audio_bytes = samples * channels * 4
        payload_len = 8 + audio_bytes  # CIP + audio
        
        # Build headers
        it_q0 = build_it_header_q0(int(self.speed), 1, self.channel)
        it_q1 = build_it_header_q1(payload_len)
        cip_q0 = build_cip_q0(dbs=channels, dbc=self.dbc)
        cip_q1 = build_cip_q1(fdf=fdf_for_rate(self.sample_rate))
        self.dbc = (self.dbc + samples) & 0xFF
        
        descriptors: List[ITDescriptor] = []
        
        # 1. Store Value (Optional)
        if store_val is not None:
            # We'll write to a dummy status address for viz
            descriptors.append(StoreValue(
                store_value=store_val,
                store_address=self.payload_base - 4, # Just somewhere
            ))
            
        # 2. OUTPUT_MORE-Immediate (IT Header)
        # Note: CIP header + audio is payload.
        # We need to construct the payload buffer carefully if fragments > 1
        # For simplicity, we assume fragments just split the ONE payload buffer
        
        payload_addr = self._alloc_payload(payload_len)
        
        header = OutputMoreImmediate(it_q0=it_q0, it_q1=it_q1)
        descriptors.append(header)
        
        # 3. Fragments (Scatter-Gather)
        # We have payload_len bytes at payload_addr.
        # If fragments=1: One OUTPUT_LAST
        # If fragments>1: (N-1) OUTPUT_MORE + 1 OUTPUT_LAST
        
        frag_size = payload_len // fragments
        rem_size = payload_len % fragments
        
        # Validation: Enforce 4-byte alignment
        if frag_size % 4 != 0:
            raise ValueError(f"Fragment size {frag_size} not 4-byte aligned!")
        
        current_addr = payload_addr
        
        for i in range(fragments - 1):
            size = frag_size
            descriptors.append(OutputMore(
                req_count=size,
                data_address=current_addr
            ))
            current_addr += size
            
        # Last fragment
        last_size = frag_size + rem_size
        descriptors.append(OutputLast(
            req_count=last_size,
            data_address=current_addr,
            irq_on_complete=irq
        ))
        
        block = DescriptorBlock(
            descriptors=descriptors,
            address=self._next_block_address()
        )
        self.blocks.append(block)
        return block

    def add_nodata_packet(self, channels: int = 8, irq: bool = False) -> DescriptorBlock:
        """Add NO-DATA packet."""
        payload_len = 8
        payload_addr = self._alloc_payload(payload_len)
        
        it_q0 = build_it_header_q0(int(self.speed), 1, self.channel)
        it_q1 = build_it_header_q1(payload_len)
        
        cip_q0 = build_cip_q0(dbs=channels, dbc=self.dbc)
        cip_q1 = build_cip_q1(fdf=fdf_for_rate(self.sample_rate))
        
        descriptors = [
            OutputMoreImmediate(it_q0=it_q0, it_q1=it_q1),
            OutputLast(req_count=payload_len, data_address=payload_addr, irq_on_complete=irq)
        ]
        
        block = DescriptorBlock(descriptors=descriptors, address=self._next_block_address())
        self.blocks.append(block)
        return block # Should return block
        
    def finalize_ring(self) -> bytes:
        """Link blocks and set skip addresses based on strategy."""
        if not self.blocks: return b''
        
        count = len(self.blocks)
        for i, block in enumerate(self.blocks):
            next_idx = (i + 1) % count
            next_block = self.blocks[next_idx]
            
            # Determine skip target
            if self.skip_strategy == SkipStrategy.NEXT:
                skip_addr = next_block.address
                skip_z = next_block.z_value
            elif self.skip_strategy == SkipStrategy.SELF:
                skip_addr = block.address
                skip_z = block.z_value
            else: # SENTINEL
                skip_addr = self.sentinel_addr # Should point to a valid sentinel block
                skip_z = 1 # Minimal Z
            
            # Update descriptors
            for desc in block.descriptors:
                if isinstance(desc, StoreValue):
                    desc.skip_address = skip_addr
                    desc.skip_z = skip_z
                elif isinstance(desc, OutputMoreImmediate):
                    desc.skip_address = skip_addr
                    desc.skip_z = skip_z
                elif isinstance(desc, (OutputLast, OutputLastSkip)):
                    desc.branch_address = next_block.address
                    desc.branch_z = next_block.z_value
                    
        return b''.join(b.to_bytes() for b in self.blocks)
        
    def validate(self) -> List[str]:
        """Validate program correctness per OHCI rules."""
        errors = []
        for i, block in enumerate(self.blocks):
            # 1. Check StoreValue position
            has_store = any(isinstance(d, StoreValue) for d in block.descriptors)
            if has_store and not isinstance(block.descriptors[0], StoreValue):
                errors.append(f"Block {i}: STORE_VALUE is not first descriptor")
                
            # 2. Check OUTPUT_MORE count
            more_count = sum(1 for d in block.descriptors if isinstance(d, (OutputMore, OutputMoreImmediate)))
            if more_count > 8: # Arbitrary limit, OHCI allows more but practical limit needed
                errors.append(f"Block {i}: Too many OUTPUT_MORE descriptors ({more_count})")
            
            # 3. Check OUTPUT_LAST existence
            has_last = any(isinstance(d, (OutputLast, OutputLastSkip)) for d in block.descriptors)
            if not has_last:
                errors.append(f"Block {i}: Missing OUTPUT_LAST descriptor")
                
            # 4. Alignment
            if block.address % 16 != 0:
                errors.append(f"Block {i}: Address 0x{block.address:X} not 16-byte aligned")
                
            # 5. Immediate Descriptor Structure (Deep Check)
            # Ensure correct packing of Q0, Q1, Skip/Branch
            for d in block.descriptors:
                if isinstance(d, (OutputMoreImmediate, OutputLastImmediate)):
                    # Just sanity check internal consistency if possible
                    # Here we trust to_bytes() but could check sizing
                    if d.size != 32:
                         errors.append(f"Block {i}: Immediate descriptor size mismatch ({d.size})")

        return errors

# ...


    def add_nodata_packet(self, channels: int = 8, irq: bool = False) -> DescriptorBlock:

        """Add NO-DATA packet."""
        payload_len = 8
        payload_addr = self._alloc_payload(payload_len)
        
        it_q0 = build_it_header_q0(int(self.speed), 1, self.channel)
        it_q1 = build_it_header_q1(payload_len)
        
        cip_q0 = build_cip_q0(dbs=channels, dbc=self.dbc)
        cip_q1 = build_cip_q1(fdf=fdf_for_rate(self.sample_rate))
        
        descriptors = [
            OutputMoreImmediate(it_q0=it_q0, it_q1=it_q1),
            OutputLast(req_count=payload_len, data_address=payload_addr, irq_on_complete=irq)
        ]
        
        block = DescriptorBlock(descriptors=descriptors, address=self._next_block_address())
        self.blocks.append(block)
        return block
    
    def finalize_ring(self) -> bytes:
        """Link blocks and set skip addresses based on strategy."""
        if not self.blocks: return b''
        
        count = len(self.blocks)
        for i, block in enumerate(self.blocks):
            next_idx = (i + 1) % count
            next_block = self.blocks[next_idx]
            
            # Determine skip target
            if self.skip_strategy == SkipStrategy.NEXT:
                skip_addr = next_block.address
                skip_z = next_block.z_value
            elif self.skip_strategy == SkipStrategy.SELF:
                skip_addr = block.address
                skip_z = block.z_value
            else: # SENTINEL
                skip_addr = self.sentinel_addr # Should point to a valid sentinel block
                skip_z = 1 # Minimal Z
            
            # Update descriptors
            for desc in block.descriptors:
                if isinstance(desc, StoreValue):
                    desc.skip_address = skip_addr
                    desc.skip_z = skip_z
                elif isinstance(desc, OutputMoreImmediate):
                    desc.skip_address = skip_addr
                    desc.skip_z = skip_z
                elif isinstance(desc, (OutputLast, OutputLastSkip)):
                    desc.branch_address = next_block.address
                    desc.branch_z = next_block.z_value
                    
        return b''.join(b.to_bytes() for b in self.blocks)
        
    def validate(self) -> List[str]:
        """Validate program correctness per OHCI rules."""
        errors = []
        for i, block in enumerate(self.blocks):
            # 1. Check StoreValue position
            has_store = any(isinstance(d, StoreValue) for d in block.descriptors)
            if has_store and not isinstance(block.descriptors[0], StoreValue):
                errors.append(f"Block {i}: STORE_VALUE is not first descriptor")
                
            # 2. Check OUTPUT_MORE count
            more_count = sum(1 for d in block.descriptors if isinstance(d, (OutputMore, OutputMoreImmediate)))
            if more_count > 8: # Arbitrary limit, OHCI allows more but practical limit needed
                errors.append(f"Block {i}: Too many OUTPUT_MORE descriptors ({more_count})")
            
            # 3. Check OUTPUT_LAST existence
            has_last = any(isinstance(d, (OutputLast, OutputLastSkip)) for d in block.descriptors)
            if not has_last:
                errors.append(f"Block {i}: Missing OUTPUT_LAST descriptor")
                
            # 4. Alignment
            if block.address % 16 != 0:
                errors.append(f"Block {i}: Address 0x{block.address:X} not 16-byte aligned")
                
        return errors

# =============================================================================
# Log Analyzer (Enhanced)
# =============================================================================




EVENT_CODE_NAMES: Dict[int, str] = {
    0x00: "evt_no_status",
    0x02: "ack_complete",
    0x06: "evt_descriptor_read",
    0x07: "evt_data_read",
    0x0A: "evt_timeout",       # Corrected per Expert
    0x0E: "evt_unknown",       # Corrected per Expert
    0x0F: "evt_flushed",       # Corrected per Expert
    0x11: "ack_pending", 
    0x21: "evt_skip_overflow", # Imp-specific
}

def analyze_it_log(log_text: str) -> List[str]:
    """Enhanced log analyzer with Deep Diagnosis."""
    out = []
    
    # Regexes
    pat_event = re.compile(r"(eventCode|evt|status)\s*[:=]\s*(0x[0-9a-fA-F]+|\d+)", re.I)
    pat_cmdp = re.compile(r"(CommandPtr|cmdPtr)\s*[:=]\s*(0x[0-9a-fA-F]+)", re.I)
    pat_dead = re.compile(r"\bdead\s*[:=]\s*1\b", re.I)
    
    # Deep Diagnosis Regexes
    pat_base = re.compile(r"base\s*=\s*(0x[0-9a-fA-F]+)", re.I)
    pat_desc = re.compile(r"IT:\s*@(\d+)\s+ctl=(0x[0-9a-fA-F]+)\s+dat=(0x[0-9a-fA-F]+)\s+br=(0x[0-9a-fA-F]+)", re.I)
    
    events = []
    descriptors = {} # index -> (ctl, dat, br)
    base_address = None
    
    for line in log_text.splitlines():
        # 1. Parse Events
        ev_code = None
        cmd_ptr = None
        is_dead = False
        
        m = pat_event.search(line)
        if m: ev_code = int(m.group(2), 16) if '0x' in m.group(2).lower() else int(m.group(2))
        
        m = pat_cmdp.search(line)
        if m: cmd_ptr = int(m.group(2), 16)
        
        if pat_dead.search(line): is_dead = True
        
        if ev_code is not None or cmd_ptr is not None or is_dead:
            events.append((ev_code, cmd_ptr, is_dead, line))
            
        # 2. Parse Base Address
        m = pat_base.search(line)
        if m and base_address is None:
            base_address = int(m.group(1), 16)
            
        # 3. Parse Descriptor Dumps
        m = pat_desc.search(line)
        if m:
            idx = int(m.group(1))
            ctl = int(m.group(2), 16)
            dat = int(m.group(3), 16)
            br = int(m.group(4), 16)
            descriptors[idx] = (ctl, dat, br)

    if not events:
        return ["No relevant IT DMA events found."]
    
    # Find Critical Event
    critical_ev = None
    for ev in events:
        code = ev[0]
        dead = ev[2]
        is_error_code = code is not None and code not in [0x00, 0x02, 0x11]
        if dead or is_error_code:
            critical_ev = ev
            break
            
    target_ev = critical_ev if critical_ev else events[-1]
    
    out.append(f"Analyzing Log ({len(events)} events found)...")
    out.append("-" * 60)
    out.append(f"Critical Event Found: {'Yes' if critical_ev else 'No'}")
    out.append(f"Log Line: {target_ev[3].strip()}")
    
    code = target_ev[0]
    dead_ctx = target_ev[2]
    cmd_ptr = target_ev[1]
    
    # Basic Diagnosis
    if code is not None:
        name = EVENT_CODE_NAMES.get(code, f"Unknown(0x{code:X})")
        out.append(f"Event Code: 0x{code:X} ({name})")
        if code == 0x0A:
             out.append("Diagnosis: Timeout (Cycle Lost?). Hardware stuck or skip overflow.")
        elif code == 0x21:
             out.append("Diagnosis: Skip Processing Overflow.")

    if cmd_ptr: out.append(f"CommandPtr: 0x{cmd_ptr:08X}")
    if dead_ctx: out.append("Context Status: DEAD (Hardware halted)")
    
    # Deep Diagnosis
    out.append("-" * 60)
    out.append("Deep Diagnosis:")
    
    if cmd_ptr and base_address:
        cmd_ptr_addr = cmd_ptr & 0xFFFFFFF0
        offset = cmd_ptr_addr - base_address
        if offset < 0:
            out.append(f"⚠️  CmdPtr (0x{cmd_ptr:X}) is before Base Address (0x{base_address:X})!")
        elif offset % 16 != 0:
             out.append(f"⚠️  CmdPtr illegal alignment (offset={offset}). Must be 16-byte aligned.")
        else:
            idx = offset // 16
            desc = descriptors.get(idx)
             
            out.append(f"Failure at Descriptor Index: {idx}")
            
            # Check if previous descriptor was 32-byte and covers this one
            prev_desc = descriptors.get(idx - 1)
            if prev_desc:
                p_ctl = prev_desc[0]
                p_key = (p_ctl >> 8) & 0x7
                if p_key == 2: # IMMEDIATE (32-byte)
                    out.append(f"ℹ️  Note: This index is the second half of descriptor {idx-1} (IMMEDIATE).")
                    
            if desc:
                d_val, d_dat, d_br = desc
                out.append(f"Descriptor Content: ctl=0x{d_val:08X} dat=0x{d_dat:08X} br=0x{d_br:08X}")
                
                # Unpack 32-bit word 0 (reqCount | control<<16)
                req_count = d_val & 0xFFFF
                control = (d_val >> 16) & 0xFFFF
                
                # Check 1: Null Branch
                br_z = d_br & 0xF
                br_addr = d_br & 0xFFFFFFF0
                
                # Check descriptor type from 16-bit control
                desc_cmd = (control >> 12) & 0xF
                desc_key = (control >> 8) & 0x7
                desc_b = (control >> 2) & 0x3
                
                desc_name = "UNKNOWN"
                if desc_cmd == 0 and desc_key == 0: desc_name = "OUTPUT_MORE"
                elif desc_cmd == 0 and desc_key == 2: desc_name = "OUTPUT_MORE-Immediate"
                elif desc_cmd == 1 and desc_key == 0: desc_name = "OUTPUT_LAST"
                elif desc_cmd == 1 and desc_key == 2: desc_name = "OUTPUT_LAST-Immediate"
                elif desc_cmd == 8 and desc_key == 6: desc_name = "STORE_VALUE"
                
                out.append(f"Type: {desc_name} (ResCount/ReqCount={req_count})")
                
                if d_val == 0xDEDEDEDE:
                    out.append("❌ FAULT: Uninitialized Memory (0xDE pattern). Descriptor not written!")
                elif br_z == 0:
                     if desc_cmd == 1: # OUTPUT_LAST
                        out.append("ℹ️  Branch Z=0. Context stopped naturally (if intended).")
                elif br_addr == 0:
                     # For OUTPUT_MORE (Standard), branch field is reserved/unused usually, so 0 is OK?
                     if desc_cmd == 0 and desc_key == 0:
                         out.append("✅ Standard OUTPUT_MORE (branch ignored).")
                     else:
                         out.append("❌ FAULT: Null Branch Address with Z!=0. Context has nowhere to go!")
                         if desc_cmd == 0 and desc_key == 2:
                             out.append("   (This is the 'skipAddress' for OUTPUT_MORE-Immediate. Cycle was likely lost/skipped, but recovery address is NULL.)")
                else:
                    out.append("✅ Branch address looks valid.")
            else:
                out.append("⚠️  Descriptor dump not found for this index.")
    else:
        out.append("Cannot perform deep diagnosis (missing Base Address or CmdPtr).")

    return out

# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(description='IT DMA Program Builder (Enhanced)')
    parser.add_argument('--cycles', type=lambda x: int(x, 0), default=8, help='Number of cycles')
    parser.add_argument('--channel', type=lambda x: int(x, 0), default=0, help='Isochronous channel')
    parser.add_argument('--rate', type=lambda x: int(x, 0), default=48000, help='Sample rate')
    parser.add_argument('--fragments', type=lambda x: int(x, 0), default=1, help='Data fragments per packet (Scatter-Gather)')
    parser.add_argument('--store-value', type=lambda x: int(x, 0), default=None, help='Add STORE_VALUE with value N')
    parser.add_argument('--skip-strategy', type=str, choices=['next', 'self', 'sentinel'], default='next', help='Skip strategy')
    parser.add_argument('--diagram', action='store_true', help='Output Mermaid diagram')
    parser.add_argument('--validate', action='store_true', help='Validate generated program')
    parser.add_argument('--analyze-log', type=str, help='Log file to analyze')
    parser.add_argument('--output', '-o', type=str, help='Output file')
    
    args = parser.parse_args()
    
    if args.analyze_log:
        with open(args.analyze_log) as f:
            print('\n'.join(analyze_it_log(f.read())))
        return

    # Generation
    builder = ITProgramBuilder(
        channel=args.channel, 
        sample_rate=args.rate,
        skip_strategy=SkipStrategy(args.skip_strategy)
    )
    
    # Schedule (simplified uniform for now)
    samples_per_cycle = 8 # Default for 48k
    
    for i in range(args.cycles):
        irq = (i == args.cycles - 1)
        store = args.store_value + i if args.store_value is not None else None
        
        builder.add_data_packet(
            samples=samples_per_cycle,
            fragments=args.fragments,
            store_val=store,
            irq=irq
        )
        print(f"Cycle {i}: DATA packet, Z={builder.blocks[-1].z_value}")
        
    if args.validate:
        errors = builder.validate()
        if errors:
            print("\nValidation Errors:")
            for e in errors: print(f"- {e}")
        else:
            print("\nValidation Passed!")
            
    program = builder.finalize_ring()
    
    if args.diagram:
        print("```mermaid")
        print("%%{init: {'theme': 'base'}}%%")
        print("graph LR")
        
        for i, block in enumerate(builder.blocks):
            # Determine block type and color
            has_audio = any(isinstance(d, (OutputLast, OutputMore)) and d.req_count > 8 for d in block.descriptors)
            has_store = any(isinstance(d, StoreValue) for d in block.descriptors)
            
            if has_audio:
                btype = "DATA"
                style = "fill:#90EE90" # Light Green
            elif has_store:
                btype = "STORE+DATA"
                style = "fill:#FFE4B5" # Moccasin
            else:
                btype = "NO-DATA"
                style = "fill:#FFB6C1" # Light Pink
                
            label = f"{btype}<br/>Z={block.z_value}<br/>Addr=0x{block.address:X}"
            
            # Show internal structure if verbose
            details = []
            for d in block.descriptors:
                if isinstance(d, StoreValue): details.append("STORE")
                elif isinstance(d, OutputMoreImmediate): details.append("HDR")
                elif isinstance(d, OutputMore): details.append(f"MORE({d.req_count})")
                elif isinstance(d, OutputLast): details.append(f"LAST({d.req_count})")
            
            label += "<br/>" + "+".join(details)
            
            print(f"    B{i}[\"{label}\"]")
            print(f"    style B{i} {style}")
            
            next_i = (i + 1) % len(builder.blocks)
            print(f"    B{i} --> B{next_i}")
            
        print("```")
    else:
        print(f"\nProgram Size: {len(program)} bytes")

if __name__ == '__main__':
    main()
