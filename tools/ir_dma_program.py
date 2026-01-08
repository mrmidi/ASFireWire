#!/usr/bin/env python3
"""
ir_dma_program.py - IR DMA Descriptor Program Builder

Generates and visualizes Isochronous Receive (IR) DMA descriptor programs
for OHCI 1.1 compliant FireWire controllers.

Reference: docs/Isoch/IR_DMA.md

Usage:
    python ir_dma_program.py --buffers 8 --size 512 --mode input-more
    python ir_dma_program.py --diagram
    python ir_dma_program.py --export-cpp > ir_program.hpp
"""

import argparse
import struct
from dataclasses import dataclass, field
from enum import IntEnum
from typing import List, Optional

# =============================================================================
# Constants
# =============================================================================

# Descriptor command bits
CMD_INPUT_MORE = 0x2
CMD_INPUT_LAST = 0x3

KEY_STANDARD = 0x0

# Branch control
BRANCH_NEVER = 0b00
BRANCH_ALWAYS = 0b11

# Interrupt control  
IRQ_NONE = 0b00
IRQ_ALWAYS = 0b11

# Wait control
WAIT_NO = 0b00
WAIT_YES = 0b11

# Block alignment
BLOCK_SIZE = 16  # 16 bytes per descriptor block

# =============================================================================
# Descriptor Data Classes
# =============================================================================

@dataclass
class IRDescriptor:
    """Base class for IR DMA descriptors"""
    size: int = 16
    
    def validate(self):
        raise NotImplementedError

    def to_bytes(self) -> bytes:
        raise NotImplementedError

@dataclass
class InputLast(IRDescriptor):
    """
    INPUT_LAST (16 bytes)
    Used in Packet-per-Buffer mode.
    """
    size: int = 16
    req_count: int = 0
    data_address: int = 0
    branch_address: int = 0
    branch_z: int = 0
    write_status: bool = True
    irq: bool = False
    wait: bool = False
    initial_res_count: int = 0
    
    def validate(self):
        if self.req_count > 0xFFFF:
            raise ValueError(f"req_count 0x{self.req_count:X} exceeds 16 bits")
        if self.branch_address & 0xF:
            raise ValueError(f"branch_address 0x{self.branch_address:X} not 16-byte aligned")
        if self.data_address & 0x3: # Quadlet alignment recommended for headers
             pass # Warn?

    def to_bytes(self) -> bytes:
        self.validate()
        s_bit = 1 if self.write_status else 0
        i_bits = IRQ_ALWAYS if self.irq else IRQ_NONE
        w_bits = WAIT_YES if self.wait else WAIT_NO
        
        # In Packet-per-Buffer, b is always 0x3 (BRANCH_ALWAYS)
        control = (
            (CMD_INPUT_LAST << 28) |
            (s_bit << 27) |
            (KEY_STANDARD << 24) |
            (i_bits << 20) |
            (BRANCH_ALWAYS << 18) |
            (w_bits << 16) |
            (self.req_count & 0xFFFF)
        )
        
        branch_ptr = (self.branch_address & 0xFFFFFFF0) | (self.branch_z & 0xF)
        status_res = self.initial_res_count & 0xFFFF
        
        return struct.pack('<IIII', control, self.data_address, branch_ptr, status_res)

@dataclass
class InputMore(IRDescriptor):
    """
    INPUT_MORE (16 bytes)
    Used in Buffer-Fill mode or multi-buffer packets.
    """
    size: int = 16
    req_count: int = 0
    data_address: int = 0
    branch_address: int = 0
    branch_z: int = 0
    buffer_fill: bool = False 
    write_status: bool = True
    irq: bool = False
    wait: bool = False
    initial_res_count: int = 0

    def validate(self):
        if self.req_count > 0xFFFF:
            raise ValueError(f"req_count 0x{self.req_count:X} exceeds 16 bits")
        if self.branch_address & 0xF:
            raise ValueError(f"branch_address 0x{self.branch_address:X} not 16-byte aligned")

    def to_bytes(self) -> bytes:
        self.validate()
        s_bit = 1 if self.write_status else 0
        i_bits = IRQ_ALWAYS if self.irq else IRQ_NONE
        w_bits = WAIT_YES if self.wait else WAIT_NO
        b_bits = BRANCH_ALWAYS if self.buffer_fill else BRANCH_NEVER
        
        control = (
            (CMD_INPUT_MORE << 28) |
            (s_bit << 27) |
            (KEY_STANDARD << 24) |
            (i_bits << 20) |
            (b_bits << 18) |
            (w_bits << 16) |
            (self.req_count & 0xFFFF)
        )
        
        branch_ptr = (self.branch_address & 0xFFFFFFF0) | (self.branch_z & 0xF)
        status_res = self.initial_res_count & 0xFFFF
        
        return struct.pack('<IIII', control, self.data_address, branch_ptr, status_res)

# =============================================================================
# Program Builder
# =============================================================================

@dataclass
class DescriptorBlock:
    descriptors: List[IRDescriptor] = field(default_factory=list)
    address: int = 0
    
    @property
    def z_value(self) -> int:
        return (sum(d.size for d in self.descriptors) + 15) // 16
    
    def to_bytes(self) -> bytes:
        return b''.join(d.to_bytes() for d in self.descriptors)

class IRProgramBuilder:
    def __init__(self, base_address: int = 0x80000000, 
                 buffer_base: int = 0x90000000,
                 mode: str = "input-last"):
        self.base_address = base_address
        self.buffer_base = buffer_base
        self.buffer_offset = 0
        self.blocks: List[DescriptorBlock] = []
        self.mode = mode.lower()
        
    def _next_block_address(self) -> int:
        if not self.blocks:
            return self.base_address
        last = self.blocks[-1]
        return last.address + last.z_value * BLOCK_SIZE
    
    def _alloc_buffer(self, size: int) -> int:
        addr = self.buffer_base + self.buffer_offset
        self.buffer_offset += (size + 15) & ~15
        return addr
    
    def add_buffer(self, size: int, irq: bool = False):
        buf_addr = self._alloc_buffer(size)
        desc = None
        
        if self.mode == "input-last": # Packet-per-Buffer
            desc = InputLast(
                req_count=size,
                data_address=buf_addr,
                irq=irq,
                initial_res_count=size,
                write_status=True
            )
        else: # input-more (Buffer-Fill)
            desc = InputMore(
                req_count=size,
                data_address=buf_addr,
                irq=irq,
                initial_res_count=size,
                write_status=True,
                buffer_fill=True
            )
        
        block = DescriptorBlock(
            descriptors=[desc],
            address=self._next_block_address(),
        )
        self.blocks.append(block)

    def finalize_ring(self) -> bytes:
        if not self.blocks:
            return b''
        
        for i, block in enumerate(self.blocks):
            next_idx = (i + 1) % len(self.blocks)
            next_block = self.blocks[next_idx]
            
            last_desc = block.descriptors[-1]
            # Use Z=0 for ring loop (standard IR/AR practice)
            if isinstance(last_desc, (InputLast, InputMore)):
                last_desc.branch_address = next_block.address
                last_desc.branch_z = 0 
        
        program = b''
        for block in self.blocks:
            program += block.to_bytes()
        return program

# =============================================================================
# Output Formatters
# =============================================================================

def hexdump(data: bytes, base_addr: int = 0) -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_part = ' '.join(f'{b:02X}' for b in chunk)
        lines.append(f'{base_addr + i:08X}  {hex_part}')
    return '\n'.join(lines)

def generate_mermaid_diagram(builder: IRProgramBuilder) -> str:
    lines = [
        '%%{init: {"theme": "base", "flowchart": {"htmlLabels": true, "curve": "monotoneX"}}}%%',
        'graph LR'
    ]
    for i, block in enumerate(builder.blocks):
        desc = block.descriptors[0]
        desc_type = "LAST" if isinstance(desc, InputLast) else "MORE"
        style = "fill:#ADD8E6"
        node_id = f'B{i}'
        label = f'{desc_type}<br/>req={desc.req_count}<br/>0x{block.address:08X}'
        lines.append(f'    {node_id}["{label}"]')
        lines.append(f'    style {node_id} {style}')
    for i in range(len(builder.blocks)):
        lines.append(f'    B{i} --> B{(i + 1) % len(builder.blocks)}')
    return '\n'.join(lines)

def export_cpp_header(builder: IRProgramBuilder, name: str = "kIRProgram") -> str:
    program = builder.finalize_ring()
    lines = [
        f'// Auto-generated IR DMA program',
        f'// Mode: {builder.mode}, Blocks: {len(builder.blocks)}, Total: {len(program)} bytes',
        f'static constexpr uint8_t {name}[] = {{'
    ]
    for i in range(0, len(program), 16):
        chunk = program[i:i+16]
        vals = ', '.join(f'0x{b:02X}' for b in chunk)
        lines.append(f'    {vals},')
    lines.append('};')
    lines.append(f'static constexpr size_t {name}Size = sizeof({name});')
    return '\n'.join(lines)

# =============================================================================
# Main
# =============================================================================

def main():
    parser = argparse.ArgumentParser(
        description='IR DMA Descriptor Program Builder',
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument('--buffers', type=int, default=8, help='Number of buffers')
    parser.add_argument('--size', type=int, default=512, help='Buffer size in bytes')
    parser.add_argument('--mode', choices=['input-last', 'input-more'], default='input-last', 
                        help='Descriptor mode: input-last (packet-per-buffer) or input-more (buffer-fill)')
    parser.add_argument('--diagram', action='store_true', help='Output Mermaid diagram')
    parser.add_argument('--export-cpp', action='store_true', help='Export as C++ header')
    parser.add_argument('--base', type=lambda x: int(x, 0), default=0x80000000, help='Descriptor base addr')
    parser.add_argument('--buf-base', type=lambda x: int(x, 0), default=0x90000000, help='Buffer base addr')
    
    args = parser.parse_args()
    
    print(f'IR DMA Program Builder')
    print(f'Mode: {args.mode}, Buffers: {args.buffers}, Size: {args.size} bytes')
    print()
    
    builder = IRProgramBuilder(base_address=args.base, buffer_base=args.buf_base, mode=args.mode)
    
    for i in range(args.buffers):
        irq = (i == args.buffers - 1)
        builder.add_buffer(args.size, irq=irq)
        
    program = builder.finalize_ring()
    
    if args.diagram:
        print(generate_mermaid_diagram(builder))
    elif args.export_cpp:
        print(export_cpp_header(builder))
    else:
        print(hexdump(program, args.base))

if __name__ == '__main__':
    main()