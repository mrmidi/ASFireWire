import sys
import struct
import argparse
import re
from dataclasses import dataclass, field
from typing import List, Optional, Tuple, Any
from enum import IntEnum

# --- Constants and Enums ---

class FMT(IntEnum):
    AM824 = 0x10  # Audio & Music format

FDF_SFC_MAP = {
    0x00: (32000, "32 kHz"),
    0x01: (44100, "44.1 kHz"),
    0x02: (48000, "48 kHz"),
    0x03: (88200, "88.2 kHz"),
    0x04: (96000, "96 kHz"),
    0x05: (176400, "176.4 kHz"),
    0x06: (192000, "192 kHz"),
}

# IT DMA constants
CMD_OUTPUT_MORE = 0x0
CMD_OUTPUT_LAST = 0x1
KEY_STANDARD = 0x0
KEY_IMMEDIATE = 0x2
BRANCH_NEVER = 0b00
BRANCH_ALWAYS = 0b11
IRQ_ALWAYS = 0b11
BLOCK_SIZE = 16

# --- IT DMA Descriptor Classes ---

@dataclass
class ITDescriptor:
    """Base class for IT DMA descriptors"""
    size: int = 16
    def to_bytes(self) -> bytes:
        raise NotImplementedError

@dataclass
class OutputMoreImmediate(ITDescriptor):
    """OUTPUT_MORE-Immediate (32 bytes) for CIP Q0+Q1"""
    size: int = 32
    cip_q0: int = 0
    cip_q1: int = 0
    def to_bytes(self) -> bytes:
        # 16-byte descriptor header + 16-byte immediate data
        control = (CMD_OUTPUT_MORE << 28) | (KEY_IMMEDIATE << 24) | (8 & 0xFFFF)
        common = struct.pack('<IIII', control, 0, 0, 0)
        imm = struct.pack('>II', self.cip_q0, self.cip_q1) + b'\x00' * 8
        return common + imm

@dataclass
class OutputLast(ITDescriptor):
    """OUTPUT_LAST (16 bytes) for payload data"""
    size: int = 16
    req_count: int = 0
    data_address: int = 0
    branch_address: int = 0
    branch_z: int = 0
    irq: bool = False
    def to_bytes(self) -> bytes:
        i_bits = IRQ_ALWAYS if self.irq else 0
        # NOTE: bit 27 (status) is reserved/0 for Transmit descriptors
        control = (CMD_OUTPUT_LAST << 28) | (KEY_STANDARD << 24) | (i_bits << 20) | (BRANCH_ALWAYS << 18) | (self.req_count & 0xFFFF)
        branch_ptr = (self.branch_address & 0xFFFFFFF0) | (self.branch_z & 0xF)
        return struct.pack('<IIII', control, self.data_address, branch_ptr, 0)

@dataclass
class OutputLastImmediate(ITDescriptor):
    """OUTPUT_LAST-Immediate (32 bytes) for NO-DATA packets"""
    size: int = 32
    cip_q0: int = 0
    cip_q1: int = 0
    branch_address: int = 0
    branch_z: int = 0
    irq: bool = False
    def to_bytes(self) -> bytes:
        i_bits = IRQ_ALWAYS if self.irq else 0
        # NOTE: bit 27 (status) is reserved/0 for Transmit descriptors
        control = (CMD_OUTPUT_LAST << 28) | (KEY_IMMEDIATE << 24) | (i_bits << 20) | (BRANCH_ALWAYS << 18) | (8 & 0xFFFF)
        branch_ptr = (self.branch_address & 0xFFFFFFF0) | (self.branch_z & 0xF)
        common = struct.pack('<IIII', control, 0, branch_ptr, 0)
        imm = struct.pack('>II', self.cip_q0, self.cip_q1) + b'\x00' * 8
        return common + imm

@dataclass
class DescriptorBlock:
    """A complete descriptor block (1-2 descriptors) representing one packet"""
    descriptors: List[ITDescriptor] = field(default_factory=list)
    address: int = 0
    @property
    def z_value(self) -> int:
        return (sum(d.size for d in self.descriptors) + 15) // 16
    def to_bytes(self) -> bytes:
        return b''.join(d.to_bytes() for d in self.descriptors)

# --- CIP Helpers ---

def build_cip_q0(sid: int, dbs: int, fn: int, qpc: int, sph: int, dbc: int, rsv: int = 0) -> int:
    return ((0b00 & 0x3) << 30) | ((sid & 0x3F) << 24) | ((dbs & 0xFF) << 16) | \
           ((fn & 0x03) << 14) | ((qpc & 0x07) << 11) | ((sph & 0x01) << 10) | \
           ((rsv & 0x03) << 8) | (dbc & 0xFF)

def build_cip_q1(fmt: int, fdf: int, syt: int) -> int:
    # 10b prefix for AM824/IEC61883-6
    return (0b10 << 30) | ((fmt & 0x3F) << 24) | ((fdf & 0xFF) << 16) | (syt & 0xFFFF)

def decode_cip_q0(q0: int) -> Tuple[int, int, int, int, int, int, int]:
    # returns sid, dbs, fn, qpc, sph, rsv, dbc
    sid = (q0 >> 24) & 0x3F
    dbs = (q0 >> 16) & 0xFF
    fn  = (q0 >> 14) & 0x03
    qpc = (q0 >> 11) & 0x07
    sph = (q0 >> 10) & 0x01
    rsv = (q0 >> 8)  & 0x03
    dbc = q0 & 0xFF
    return sid, dbs, fn, qpc, sph, rsv, dbc

def decode_cip_q1(q1: int) -> Tuple[int, int, int, int]:
    # returns fmt, fdf, syt, prefix_ok
    prefix = (q1 >> 30) & 0x3
    fmt = (q1 >> 24) & 0x3F
    fdf = (q1 >> 16) & 0xFF
    syt = q1 & 0xFFFF
    return fmt, fdf, syt, (1 if prefix == 0b10 else 0)

# --- Data Classes ---

@dataclass
class AmdtpPacket:
    # Isochronous Header fields (only if header_present=True)
    data_length: int = 0
    tag: int = 0
    channel: int = 0
    tcode: int = 0
    sy: int = 0
    header_crc: int = 0

    # CIP Header fields (Quadlet 1)
    sid: int = 0
    dbs: int = 0
    fn: int = 0
    qpc: int = 0
    sph: int = 0
    dbc: int = 0

    # CIP Header fields (Quadlet 2)
    fmt: int = 0
    fdf: int = 0
    syt: int = 0

    # Payload
    payload_size: int = 0
    payload_data: bytes = field(default_factory=bytes)
    
    # Derived info
    packet_type: str = "UNKNOWN" # "DATA", "NO-DATA"
    sample_rate_hz: Optional[int] = None
    sample_rate_name: Optional[str] = None
    header_present: bool = False

    def get_cip_quadlets(self) -> Tuple[int, int]:
        q0 = build_cip_q0(self.sid, self.dbs, self.fn, self.qpc, self.sph, self.dbc)
        q1 = build_cip_q1(self.fmt, self.fdf, self.syt)
        return q0, q1

    def __str__(self) -> str:
        lines = []
        if self.header_present:
            lines.append(f"--- Isochronous Packet (Channel {self.channel}) ---")
            lines.append(f"  Data Length:   {self.data_length} bytes")
            lines.append(f"  Tag:           {self.tag} ({'CIP' if self.tag == 1 else 'Other'})")
            lines.append(f"  Channel:       {self.channel}")
            lines.append(f"  TCode:         0x{self.tcode:X} ({'ISOCH_BLOCK' if self.tcode == 0xA else 'Other'})")
            lines.append(f"  SY:            {self.sy}")
            lines.append(f"  Header CRC:    0x{self.header_crc:08X}")
        else:
            lines.append(f"--- Isochronous Payload (No Packet Header) ---")
            
        lines.append(f"--- CIP Header ---")
        lines.append(f"  SID:           0x{self.sid:X}")
        lines.append(f"  DBS:           {self.dbs} quadlets ({self.dbs * 4} bytes)")
        lines.append(f"  FN:            {self.fn}")
        lines.append(f"  QPC:           {self.qpc}")
        lines.append(f"  SPH:           {self.sph}")
        lines.append(f"  DBC:           {self.dbc}")
        lines.append(f"  FMT:           0x{self.fmt:X} ({FMT(self.fmt).name if self.fmt == FMT.AM824 else 'Other'})")
        lines.append(f"  FDF:           0x{self.fdf:02X}")
        
        if self.sample_rate_hz:
            lines.append(f"    (Sample Rate: {self.sample_rate_name} / {self.sample_rate_hz} Hz)")
        lines.append(f"  SYT:           0x{self.syt:04X} ({'NO-DATA' if self.syt == 0xFFFF else 'Timestamp'})")
        
        lines.append(f"--- Payload ---")
        lines.append(f"  Packet Type:   {self.packet_type}")
        lines.append(f"  Payload Size:  {self.payload_size} bytes")
        if self.payload_size > 0:
            lines.append(f"  Payload Data:  {self.payload_data.hex()}")
        
        return "\n".join(lines)

# --- IT Program Builder ---

class ITProgramBuilder:
    def __init__(self, base_address: int = 0x80000000):
        self.base_address = base_address
        self.blocks: List[DescriptorBlock] = []
        self.payload_base = base_address + 0x10000
        self.payload_offset = 0

    def add_packet(self, packet: AmdtpPacket, irq: bool = False):
        q0, q1 = packet.get_cip_quadlets()
        addr = self.base_address if not self.blocks else self.blocks[-1].address + self.blocks[-1].z_value * BLOCK_SIZE
        
        if packet.packet_type == "NO-DATA":
            block = DescriptorBlock(descriptors=[OutputLastImmediate(cip_q0=q0, cip_q1=q1, irq=irq)], address=addr)
        else:
            p_addr = self.payload_base + self.payload_offset
            self.payload_offset = (self.payload_offset + packet.payload_size + 15) & ~15
            block = DescriptorBlock(descriptors=[
                OutputMoreImmediate(cip_q0=q0, cip_q1=q1),
                OutputLast(req_count=packet.payload_size, data_address=p_addr, irq=irq)
            ], address=addr)
        self.blocks.append(block)

    def finalize(self) -> bytes:
        if not self.blocks: return b''
        # Set branch addresses to create a ring
        for i, block in enumerate(self.blocks):
            next_block = self.blocks[(i + 1) % len(self.blocks)]
            last_desc = block.descriptors[-1]
            if hasattr(last_desc, 'branch_address'):
                last_desc.branch_address = next_block.address
                last_desc.branch_z = next_block.z_value
        return b''.join(b.to_bytes() for b in self.blocks)

# --- Helper Functions ---

def hex_to_bytes(hex_string: str) -> bytes:
    cleaned = ''.join(c for c in hex_string if c in '0123456789ABCDEFabcdef')
    if len(cleaned) % 2 != 0:
        raise ValueError("Hex string has odd length.")
    return bytes.fromhex(cleaned)

def parse_amdtp_packet(data: bytes, has_header: bool = False, le_header: bool = False, le_payload: bool = False) -> AmdtpPacket:
    offset = 0
    fmt_header = "<I" if le_header else ">I"
    fmt_payload = "<I" if le_payload else ">I"
    
    # Defaults
    data_length = len(data)
    tag = 0; channel = 0; tcode = 0xA; sy = 0; header_crc = 0

    if has_header:
        if len(data) < 8: raise ValueError("Packet too short for header")
        q0_isoch = struct.unpack_from(fmt_header, data, offset)[0]
        offset += 4
        data_length = (q0_isoch >> 16) & 0xFFFF
        tag = (q0_isoch >> 14) & 0x3
        channel = (q0_isoch >> 8) & 0x3F
        tcode = (q0_isoch >> 4) & 0xF
        sy = q0_isoch & 0xF
        header_crc = struct.unpack_from(fmt_header, data, offset)[0]
        offset += 4
    
    if len(data) - offset < 8: raise ValueError("Packet too short for CIP headers")
    
    # Read raw CIP quadlets
    cip_q0_raw = struct.unpack_from(fmt_payload, data, offset)[0]; offset += 4
    cip_q1_raw = struct.unpack_from(fmt_payload, data, offset)[0]; offset += 4

    # Decode bitfields
    sid, dbs, fn, qpc, sph, rsv, dbc = decode_cip_q0(cip_q0_raw)
    fmt, fdf, syt, prefix_ok = decode_cip_q1(cip_q1_raw)

    payload_data = data[offset:]
    payload_size = len(payload_data)
    
    # Correct classification: packet without payload is NO-DATA, regardless of SYT
    packet_type = "NO-DATA" if payload_size == 0 else "DATA"

    # Derive sample rate
    sample_rate_hz = None; sample_rate_name = None
    if fmt == FMT.AM824:
        n_flag = (fdf >> 3) & 0x1; sfc = fdf & 0x7
        if n_flag == 0 and sfc in FDF_SFC_MAP:
            sample_rate_hz, sample_rate_name = FDF_SFC_MAP[sfc]

    return AmdtpPacket(
        data_length=data_length, tag=tag, channel=channel, tcode=tcode, sy=sy, header_crc=header_crc,
        sid=sid, dbs=dbs, fn=fn, qpc=qpc, sph=sph, dbc=dbc,
        fmt=fmt, fdf=fdf, syt=syt,
        payload_size=payload_size, payload_data=payload_data,
        packet_type=packet_type, sample_rate_hz=sample_rate_hz, sample_rate_name=sample_rate_name,
        header_present=has_header
    )

def parse_firebug_log(file_path: str, filter_channel: int = 0) -> List[AmdtpPacket]:
    packets = []
    with open(file_path, 'r') as f:
        content = f.read()
    
    current_packet_bytes = bytearray()
    current_channel = -1
    expected_size = 0
    expecting_data = False
    
    for line in content.splitlines():
        # Match header: 033:2673:2243  Isoch channel 0, tag 1, sy 0, size 72 [actual 72] s400
        m = re.search(r'Isoch channel (\d+),.*size (\d+)', line)
        if m:
            # Process previous packet if it was for our channel
            if current_packet_bytes and current_channel == filter_channel:
                if len(current_packet_bytes) >= expected_size:
                    packets.append(parse_amdtp_packet(bytes(current_packet_bytes[:expected_size]), has_header=False))
            
            current_channel = int(m.group(1))
            expected_size = int(m.group(2))
            current_packet_bytes = bytearray()
            expecting_data = True
            continue
        
        if expecting_data:
            dm = re.search(r'^\s+[0-9a-fA-F]{4}\s+((?:[0-9a-fA-F]{8}\s*)+)', line)
            if dm:
                hex_str = dm.group(1).replace(" ", "")
                current_packet_bytes.extend(bytes.fromhex(hex_str))
            else:
                if line.strip() == "" or "Isoch channel" in line:
                    expecting_data = False
    
    # Last packet
    if current_packet_bytes and current_channel == filter_channel:
        if len(current_packet_bytes) >= expected_size:
            packets.append(parse_amdtp_packet(bytes(current_packet_bytes[:expected_size]), has_header=False))
        
    return packets

# --- Output Formatters ---

def hexdump(data: bytes, base_addr: int = 0) -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_part = ' '.join(f'{b:02X}' for b in chunk)
        lines.append(f'{base_addr + i:08X}  {hex_part}')
    return '\n'.join(lines)

def generate_mermaid_diagram(builder: ITProgramBuilder) -> str:
    lines = ['graph LR']
    for i, block in enumerate(builder.blocks):
        kind = "DATA" if len(block.descriptors) > 1 else "NO-DATA"
        style = "fill:#90EE90" if kind == "DATA" else "fill:#FFB6C1"
        lines.append(f'    B{i}["{kind}<br/>Z={block.z_value}<br/>0x{block.address:08X}"]')
        lines.append(f'    style B{i} {style}')
    for i in range(len(builder.blocks)):
        lines.append(f'    B{i} --> B{(i + 1) % len(builder.blocks)}')
    return '\n'.join(lines)

def export_cpp_header(builder: ITProgramBuilder, name: str = "kITProgram") -> str:
    program = builder.finalize()
    lines = [f'static constexpr uint8_t {name}[] = {{']
    for i in range(0, len(program), 16):
        chunk = program[i:i+16]
        lines.append('    ' + ', '.join(f'0x{b:02X}' for b in chunk) + ',')
    lines.append('};')
    return '\n'.join(lines)

# --- Main ---

def main():
    parser = argparse.ArgumentParser(description="Analyze AMDT packets and generate IT DMA programs.")
    parser.add_argument('hex_packets', nargs='*', help='Hex strings of AMDT packets.')
    parser.add_argument('--from-log', help='Parse packets from FireBug log file.')
    parser.add_argument('--channel', type=int, default=0, help='Filter log by channel.')
    parser.add_argument('--it-base', type=lambda x: int(x, 0), default=0x80000000, help='Base address for IT descriptors.')
    parser.add_argument('--it-program', action='store_true', help='Generate IT DMA program.')
    parser.add_argument('--export-cpp', action='store_true', help='Export program as C++ header.')
    parser.add_argument('--diagram', action='store_true', help='Output Mermaid diagram.')
    parser.add_argument('--header', action='store_true', help='Input has 8-byte isoch header prefix.')
    parser.add_argument('--le-header', action='store_true', help='LE isoch headers.')
    parser.add_argument('--le-payload', action='store_true', help='LE CIP/payload.')
    parser.add_argument('--no-debug', action='store_true', help='Disable detailed packet breakdown.')
    parser.add_argument('--limit', type=int, help='Limit number of packets processed.')

    args = parser.parse_args()
    debug = not args.no_debug
    
    packets = []
    if args.from_log:
        packets = parse_firebug_log(args.from_log, args.channel)
        if args.limit:
            packets = packets[:args.limit]
    else:
        for hex_str in args.hex_packets:
            packets.append(parse_amdtp_packet(hex_to_bytes(hex_str), has_header=args.header, le_header=args.le_header, le_payload=args.le_payload))

    if debug:
        for i, p in enumerate(packets):
            print(f"\nPacket {i+1}:\n{p}")
            if i > 0:
                expected = (packets[i-1].dbc + (packets[i-1].payload_size // (packets[i-1].dbs*4 if packets[i-1].dbs else 1))) & 0xFF
                check_icon = '✅' if p.dbc == expected else f'❌ Expected {expected}'
                print(f"DBC check: {check_icon}")

    if args.it_program or args.export_cpp or args.diagram:
        builder = ITProgramBuilder(base_address=args.it_base)
        for i, p in enumerate(packets):
            builder.add_packet(p, irq=(i == len(packets)-1))
        
        if args.diagram:
            print("\nMermaid Diagram:\n" + generate_mermaid_diagram(builder))
        elif args.export_cpp:
            print("\nC++ Export:\n" + export_cpp_header(builder))
        else:
            prog = builder.finalize()
            print(f"\nIT DMA Program ({len(prog)} bytes):\n" + hexdump(prog, args.it_base))

if __name__ == "__main__":
    main()
