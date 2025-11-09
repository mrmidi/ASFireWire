#!/usr/bin/env python3
"""
IEEE 1212 Configuration ROM Parser
Handles both big-endian (wire format) and little-endian (host dump) ROMs.
"""

from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple, Any, Iterable
import struct
import io
import json
import sys
import argparse

@dataclass
class DirectoryEntry:
    offset_in_rom: int
    raw: int
    key: int
    entry_type: int
    key_id: int
    value: int
    resolved_addr_units: Optional[int] = None
    target_rom_offset: Optional[int] = None
    comment: Optional[str] = None

@dataclass
class Leaf:
    start_offset: int
    length_quadlets: int
    crc_header: int
    crc_calc: int
    data: bytes
    decoded: Dict[str, Any] = field(default_factory=dict)

@dataclass
class Directory:
    start_offset: int
    length_quadlets: int
    crc_header: int
    crc_calc: int
    entries: List[DirectoryEntry]

@dataclass
class BusInfoBlock:
    start_offset: int
    crc_length: int
    bus_info_length: int
    crc_header: int
    crc_calc: int
    raw_quadlets: List[int]
    fields: Dict[str, Any]

@dataclass
class ConfigROM:
    raw: bytes
    base_units_addr: int
    bus_info: BusInfoBlock
    root_dir: Directory
    all_dirs: Dict[int, Directory]
    all_leaves: Dict[int, Leaf]

def be32(data: bytes, off: int) -> int:
    return struct.unpack_from(">I", data, off)[0]

def le32(data: bytes, off: int) -> int:
    return struct.unpack_from("<I", data, off)[0]

def crc16_quadlet_step(crc: int, data_quadlet: int) -> int:
    crc &= 0xFFFF
    data = data_quadlet & 0xFFFFFFFF
    for shift in range(28, -1, -4):
        s = ((crc >> 12) ^ (data >> shift)) & 0xF
        crc = ((crc << 4) & 0xFFFF) ^ ((s << 12) & 0xFFFF) ^ ((s << 5) & 0xFFFF) ^ s
    return crc & 0xFFFF

def crc16_over_quadlets(quadlets: Iterable[int]) -> int:
    crc = 0
    for q in quadlets:
        crc = crc16_quadlet_step(crc, q)
    return crc & 0xFFFF

UNITS_BASE = 0xFFFF_F000_0000
CONFIG_ROM_BASE = 0xFFFF_F000_0400

IMMEDIATE = 0
CSR_OFFSET = 1
LEAF_OFFSET = 2
DIR_OFFSET = 3

KEY_NAMES = {
    0x01: "Descriptor",
    0x02: "Bus_Dependent_Info",
    0x03: "Vendor_ID",
    0x04: "Hardware_Version",
    0x07: "Module",
    0x0C: "Node_Capabilities",
    0x0D: "EUI_64",
    0x11: "Unit",
    0x12: "Specifier_ID",
    0x13: "Version",
    0x14: "Dependent_Info",
    0x15: "Unit_Location",
    0x17: "Model_ID",
    0x18: "Instance",
    0x19: "Keyword",
    0x1A: "Feature",
    0x1B: "Extended_ROM",
    0x1C: "Extended_Key_Specifier_ID",
    0x1D: "Extended_Key",
    0x1E: "Extended_Data",
    0x1F: "Modifiable_Descriptor",
    0x20: "Directory_ID",
    0x21: "Revision",
}

OUI_BRAND = {
    0x0003DB: "Apogee",
    0x00000F: "Focusrite",
    0x0007F5: "MOTU",
}

def detect_endianness(rom: bytes) -> str:
    """Detect ROM endianness by looking for '1394' signature in quadlet 1."""
    if len(rom) < 8:
        return "big"
    q1_be = be32(rom, 4)
    q1_le = le32(rom, 4)
    if q1_be == 0x31333934:
        return "big"
    elif q1_le == 0x31333934:
        return "little"
    return "big"

def parse_bus_info_block(rom: bytes, endianness: str, base_units_addr: int = CONFIG_ROM_BASE) -> BusInfoBlock:
    read32 = le32 if endianness == "little" else be32
    header = read32(rom, 0)
    
    # Bus info header: [info_len:8][crc_len:8][CRC:16] in BE wire format
    # LE: bits become [CRC:16][crc_len:8][info_len:8]
    if endianness == "little":
        crc_header = header & 0xFFFF
        crc_length = (header >> 16) & 0xFF
        bus_info_length = (header >> 24) & 0xFF
    else:
        bus_info_length = (header >> 24) & 0xFF
        crc_length = (header >> 16) & 0xFF
        crc_header = header & 0xFFFF
    
    total_quadlets = bus_info_length + 1
    quadlets = [read32(rom, 4 * i) for i in range(total_quadlets)]
    crc_calc = crc16_over_quadlets(quadlets[1:1 + crc_length])
    
    fields: Dict[str, Any] = {"endianness": endianness}
    if len(quadlets) >= 2:
        fields["bus_name_quadlet"] = quadlets[1]
        try:
            fields["bus_name_ascii"] = bytes.fromhex(f"{quadlets[1]:08x}").decode("ascii", errors="ignore")
        except Exception:
            pass
    if len(quadlets) >= 3:
        fields["max_rom_hint"] = quadlets[2] & 0x3
    
    return BusInfoBlock(
        start_offset=0,
        crc_length=crc_length,
        bus_info_length=bus_info_length,
        crc_header=crc_header,
        crc_calc=crc_calc,
        raw_quadlets=quadlets,
        fields=fields,
    )

def parse_directory(rom: bytes, dir_offset: int, endianness: str) -> Directory:
    read32 = le32 if endianness == "little" else be32
    header = read32(rom, dir_offset)
    
    # Directory header: [CRC:16][length:16] in BE wire format
    # LE: bits become [length:16][CRC:16]
    if endianness == "little":
        length_quadlets = (header >> 16) & 0xFFFF
        crc_header = header & 0xFFFF
    else:
        crc_header = (header >> 16) & 0xFFFF
        length_quadlets = header & 0xFFFF
    
    entries_raw = [read32(rom, dir_offset + 4 * (i + 1)) for i in range(length_quadlets)]
    crc_calc = crc16_over_quadlets(entries_raw)
    
    entries: List[DirectoryEntry] = []
    for i, raw in enumerate(entries_raw):
        key = (raw >> 24) & 0xFF
        entry_type = (key >> 6) & 0x3
        key_id = key & 0x3F
        value = raw & 0xFFFFFF
        entry_offset = dir_offset + 4 * (i + 1)
        
        resolved_units = None
        target_off = None
        if entry_type == CSR_OFFSET:
            resolved_units = UNITS_BASE + 4 * value
        elif entry_type in (LEAF_OFFSET, DIR_OFFSET):
            target_off = entry_offset + 4 * value
        
        comment = None
        name = KEY_NAMES.get(key_id)
        if name == "Vendor_ID" and entry_type == IMMEDIATE:
            comment = f"company_id=0x{value:06x}"
        elif name == "Model_ID" and entry_type == IMMEDIATE:
            comment = f"model_id=0x{value:06x}"
        elif name == "Specifier_ID" and entry_type == IMMEDIATE:
            comment = f"specifier(company_id)=0x{value:06x}"
        elif name == "Version" and entry_type == IMMEDIATE:
            comment = f"version=0x{value:06x}"
        
        entries.append(DirectoryEntry(
            offset_in_rom=entry_offset,
            raw=raw, key=key, entry_type=entry_type, key_id=key_id, value=value,
            resolved_addr_units=resolved_units, target_rom_offset=target_off, comment=comment
        ))
    
    return Directory(
        start_offset=dir_offset,
        length_quadlets=length_quadlets,
        crc_header=crc_header,
        crc_calc=crc_calc,
        entries=entries
    )

def parse_leaf(rom: bytes, leaf_offset: int, endianness: str) -> Leaf:
    read32 = le32 if endianness == "little" else be32
    header = read32(rom, leaf_offset)
    
    # Leaf header: [CRC:16][length:16] in BE wire format
    if endianness == "little":
        length_quadlets = (header >> 16) & 0xFFFF
        crc_header = header & 0xFFFF
    else:
        crc_header = (header >> 16) & 0xFFFF
        length_quadlets = header & 0xFFFF
    
    payload = rom[leaf_offset + 4: leaf_offset + 4 + 4 * length_quadlets]
    quadlets = [read32(rom, leaf_offset + 4 + 4*i) for i in range(length_quadlets)] if length_quadlets else []
    crc_calc = crc16_over_quadlets(quadlets)
    
    decoded: Dict[str, Any] = {}
    
    # Try to decode textual descriptor
    # Format: [desc_crc:16|desc_len:16][type:8|specifier:24][text...]
    try:
        if len(payload) >= 8:
            # First quadlet of payload is descriptor header
            desc_hdr = quadlets[0] if quadlets else struct.unpack(f"{'<' if endianness == 'little' else '>'}I", payload[:4])[0]
            # Second quadlet is type|specifier
            type_spec = quadlets[1] if len(quadlets) > 1 else struct.unpack(f"{'<' if endianness == 'little' else '>'}I", payload[4:8])[0]
            
            descriptor_type = (type_spec >> 24) & 0xFF
            specifier_id = type_spec & 0xFFFFFF
            
            if descriptor_type == 0 and specifier_id == 0:
                # Text starts at byte 8 of payload
                text_bytes = payload[8:]
                text = None
                
                # For little-endian ROMs, text is usually word-swapped
                if endianness == "little":
                    swapped = bytearray()
                    for i in range(0, len(text_bytes), 4):
                        swapped.extend(text_bytes[i:i+4][::-1])
                    text = bytes(swapped).decode("ascii", errors="ignore").strip("\x00")
                
                # If still no text, try normal decode
                if not text or len(text) < 2:
                    text = text_bytes.decode("ascii", errors="ignore").strip("\x00")
                
                if text:
                    decoded["descriptor_type"] = "text"
                    decoded["specifier_id"] = specifier_id
                    decoded["text_ascii"] = text
    except Exception:
        pass
    
    return Leaf(
        start_offset=leaf_offset,
        length_quadlets=length_quadlets,
        crc_header=crc_header,
        crc_calc=crc_calc,
        data=payload,
        decoded=decoded
    )

def walk_tree(rom: bytes, root_dir_offset: int, endianness: str) -> Tuple[Dict[int, Directory], Dict[int, Leaf]]:
    dirs: Dict[int, Directory] = {}
    leaves: Dict[int, Leaf] = {}
    queue = [root_dir_offset]
    seen = set()
    
    while queue:
        off = queue.pop(0)
        if off in seen or off < 0 or off + 4 > len(rom):
            continue
        seen.add(off)
        
        try:
            d = parse_directory(rom, off, endianness)
        except Exception:
            continue
        dirs[off] = d
        
        for e in d.entries:
            if e.entry_type == LEAF_OFFSET and e.target_rom_offset is not None:
                if 0 <= e.target_rom_offset < len(rom):
                    if e.target_rom_offset not in leaves:
                        leaves[e.target_rom_offset] = parse_leaf(rom, e.target_rom_offset, endianness)
            elif e.entry_type == DIR_OFFSET and e.target_rom_offset is not None:
                if 0 <= e.target_rom_offset < len(rom):
                    queue.append(e.target_rom_offset)
    
    return dirs, leaves

def parse_config_rom(rom_bytes: bytes, base_units_addr: int = CONFIG_ROM_BASE) -> ConfigROM:
    endianness = detect_endianness(rom_bytes)
    bus = parse_bus_info_block(rom_bytes, endianness, base_units_addr)
    root_dir_offset = 4 * (bus.bus_info_length + 1)
    root = parse_directory(rom_bytes, root_dir_offset, endianness)
    all_dirs, all_leaves = walk_tree(rom_bytes, root_dir_offset, endianness)
    
    return ConfigROM(
        raw=rom_bytes,
        base_units_addr=base_units_addr,
        bus_info=bus,
        root_dir=root,
        all_dirs=all_dirs,
        all_leaves=all_leaves
    )

def format_entry(e: DirectoryEntry) -> str:
    name = KEY_NAMES.get(e.key_id, f"key_{e.key_id:02x}")
    tname = {0: "Imm", 1: "CSR", 2: "Leaf", 3: "Dir"}.get(e.entry_type, f"T{e.entry_type}")
    parts = [
        f"{name:<22} {tname:<4} 0x{e.value:06x}",
        f"@rom+0x{e.offset_in_rom:04x}",
    ]
    if e.resolved_addr_units is not None:
        parts.append(f"→ units 0x{e.resolved_addr_units:012x}")
    if e.target_rom_offset is not None:
        parts.append(f"→ rom+0x{e.target_rom_offset:04x}")
    if e.comment:
        parts.append(f"({e.comment})")
    return "  - " + " ".join(parts)

def dump_config_rom(cr: ConfigROM) -> str:
    out = io.StringIO()
    b = cr.bus_info
    print(f"BusInfo @rom+0x{b.start_offset:04x}: len={b.bus_info_length}q, CRC hdr=0x{b.crc_header:04x}, CRC calc=0x{b.crc_calc:04x}", file=out)
    if b.fields:
        print("  Fields:", json.dumps(b.fields, indent=2), file=out)
    rd = cr.root_dir
    print(f"RootDir @rom+0x{rd.start_offset:04x}: entries={rd.length_quadlets}, CRC hdr=0x{rd.crc_header:04x}, CRC calc=0x{rd.crc_calc:04x}", file=out)
    for off, d in sorted(cr.all_dirs.items()):
        print(f"\nDirectory @rom+0x{off:04x}: entries={d.length_quadlets}, CRC hdr=0x{d.crc_header:04x}, CRC calc=0x{d.crc_calc:04x}", file=out)
        for e in d.entries:
            print(format_entry(e), file=out)
    for off, leaf in sorted(cr.all_leaves.items()):
        print(f"\nLeaf @rom+0x{off:04x}: len={leaf.length_quadlets}q, CRC hdr=0x{leaf.crc_header:04x}, CRC calc=0x{leaf.crc_calc:04x}", file=out)
        if leaf.decoded:
            print("  Decoded:", json.dumps(leaf.decoded, ensure_ascii=False), file=out)
    return out.getvalue()

def canonical_device_name(cr: ConfigROM) -> str:
    """Return a friendly '<Brand> <Model>' using Vendor OUI and textual descriptor leaves."""
    oui = 0
    vendor_text = ""
    model_text = ""
    root = cr.root_dir
    unit_dir_off = None

    for e in root.entries:
        name = KEY_NAMES.get(e.key_id)
        if name == "Vendor_ID" and e.entry_type == IMMEDIATE:
            oui = e.value
        elif name == "Descriptor" and e.entry_type == LEAF_OFFSET and e.target_rom_offset in cr.all_leaves:
            vendor_text = cr.all_leaves[e.target_rom_offset].decoded.get("text_ascii", "") or vendor_text
        elif name == "Unit" and e.entry_type == DIR_OFFSET:
            unit_dir_off = e.target_rom_offset

    if unit_dir_off is not None and unit_dir_off in cr.all_dirs:
        unit_dir = cr.all_dirs[unit_dir_off]
        for e in unit_dir.entries:
            name = KEY_NAMES.get(e.key_id)
            if name == "Descriptor" and e.entry_type == LEAF_OFFSET and e.target_rom_offset in cr.all_leaves:
                model_text = cr.all_leaves[e.target_rom_offset].decoded.get("text_ascii", "") or model_text

    brand = OUI_BRAND.get(oui) or (vendor_text.split()[0] if vendor_text else "")
    return f"{brand} {model_text}".strip()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Parse and dump IEEE 1212 Configuration ROM")
    parser.add_argument("rom_file", help="Path to binary ROM image file (.img or .bin)")
    args = parser.parse_args()
    
    try:
        with open(args.rom_file, "rb") as f:
            rom_data = f.read()
        
        print(f"Parsing {len(rom_data)} bytes from {args.rom_file}...")
        cr = parse_config_rom(rom_data)
        
        device_name = canonical_device_name(cr)
        if device_name:
            print(f"Device: {device_name}\n")
        
        print(dump_config_rom(cr))
        
    except FileNotFoundError:
        print(f"Error: File '{args.rom_file}' not found")
        sys.exit(1)
    except Exception as e:
        print(f"Error parsing ROM: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
