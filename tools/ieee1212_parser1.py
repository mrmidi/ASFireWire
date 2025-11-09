
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple, Any, Iterable
import struct
import io
import json

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

def swap_words(data: bytes) -> bytes:
    """Swap every 4-byte word (common when ROM is dumped from host memory)."""
    swapped = bytearray()
    for i in range(0, len(data), 4):
        swapped.extend(data[i:i+4][::-1])
    return bytes(swapped)

def split_bus_info_header(header_quadlet: int) -> Tuple[int, int, int]:
    """
    Bus Info Block header format (IEEE 1212):
      bits[31:24] = bus_info_length (number of quadlets following the header)
      bits[23:16] = crc_length (number of quadlets CRC covers)
      bits[15:0]  = CRC value
    """
    bus_info_length = (header_quadlet >> 24) & 0xFF
    crc_length = (header_quadlet >> 16) & 0xFF
    crc = header_quadlet & 0xFFFF
    return bus_info_length, crc_length, crc

def split_crc_len(header_quadlet: int) -> Tuple[int, int]:
    crc = (header_quadlet >> 16) & 0xFFFF
    length = header_quadlet & 0xFFFF
    return crc, length

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

def parse_bus_info_block(rom: bytes, base_units_addr: int = CONFIG_ROM_BASE) -> BusInfoBlock:
    start = 0
    header = be32(rom, start)
    bus_info_length, crc_length, crc_header = split_bus_info_header(header)
    total_quadlets = bus_info_length + 1
    quadlets = [be32(rom, start + 4 * i) for i in range(total_quadlets)]
    crc_calc = crc16_over_quadlets(quadlets[1:1 + crc_length])
    fields: Dict[str, Any] = {}
    if len(quadlets) >= 2:
        fields["bus_name_quadlet"] = quadlets[1]
        try:
            fields["bus_name_ascii"] = bytes.fromhex(f"{quadlets[1]:08x}").decode("ascii", errors="ignore")
        except Exception:
            pass
    if len(quadlets) >= 3:
        q2 = quadlets[2]
        fields["max_rom_hint"] = q2 & 0x3
    return BusInfoBlock(
        start_offset=start,
        crc_length=crc_length,
        bus_info_length=bus_info_length,
        crc_header=crc_header,
        crc_calc=crc_calc,
        raw_quadlets=quadlets,
        fields=fields,
    )

def parse_directory(rom: bytes, dir_offset: int) -> Directory:
    header = be32(rom, dir_offset)
    crc_header, length_quadlets = split_crc_len(header)
    entries_raw = [be32(rom, dir_offset + 4 * (i + 1)) for i in range(length_quadlets)]
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

def parse_leaf(rom: bytes, leaf_offset: int) -> Leaf:
    header = be32(rom, leaf_offset)
    crc_header, length_quadlets = split_crc_len(header)
    payload = rom[leaf_offset + 4: leaf_offset + 4 + 4 * length_quadlets]
    quadlets = list(struct.unpack(f">{length_quadlets}I", payload)) if length_quadlets else []
    crc_calc = crc16_over_quadlets(quadlets)
    decoded: Dict[str, Any] = {}
    try:
        if length_quadlets >= 2:
            desc_hdr = quadlets[0]
            desc_crc, desc_len = split_crc_len(desc_hdr)
            if length_quadlets >= (1 + desc_len):
                dword1 = quadlets[1]
                descriptor_type = (dword1 >> 24) & 0xFF
                specifier_id = dword1 & 0xFFFFFF
                if descriptor_type == 0 and specifier_id == 0:
                    remaining_bytes = payload[8: 4 + 4 * desc_len]
                    # Try normal decode
                    text = remaining_bytes.decode("ascii", errors="ignore").strip("\x00")
                    # If empty or garbled, try word-swapped (common host-order quirk)
                    if not text or len(text) < 3:
                        swapped = swap_words(remaining_bytes)
                        text = swapped.decode("ascii", errors="ignore").strip("\x00")
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

def walk_tree(rom: bytes, root_dir_offset: int):
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
            d = parse_directory(rom, off)
        except Exception:
            continue
        dirs[off] = d
        for e in d.entries:
            if e.entry_type == 2 and e.target_rom_offset is not None:
                if 0 <= e.target_rom_offset < len(rom):
                    if e.target_rom_offset not in leaves:
                        leaves[e.target_rom_offset] = parse_leaf(rom, e.target_rom_offset)
            elif e.entry_type == 3 and e.target_rom_offset is not None:
                if 0 <= e.target_rom_offset < len(rom):
                    queue.append(e.target_rom_offset)
    return dirs, leaves

def parse_config_rom(rom_bytes: bytes, base_units_addr: int = CONFIG_ROM_BASE) -> ConfigROM:
    bus = parse_bus_info_block(rom_bytes, base_units_addr)
    root_dir_offset = 4 * (bus.bus_info_length + 1)
    root = parse_directory(rom_bytes, root_dir_offset)
    all_dirs, all_leaves = walk_tree(rom_bytes, root_dir_offset)
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
        parts.append(f"-> units 0x{e.resolved_addr_units:012x}")
    if e.target_rom_offset is not None:
        parts.append(f"-> rom+0x{e.target_rom_offset:04x}")
    if e.comment:
        parts.append(f"({e.comment})")
    return "  - " + "  ".join(parts)

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

def parse_and_dump(rom_bytes: bytes) -> str:
    return dump_config_rom(parse_config_rom(rom_bytes))

# Known vendor OUIs for short brand names
OUI_BRAND = {
    0x0003DB: "Apogee",   # Apogee Electronics
    0x00000F: "Focusrite",
    0x0007F5: "MOTU",
    # add more as needed
}

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

# Main entry point
if __name__ == "__main__":
    import sys
    import argparse
    
    parser = argparse.ArgumentParser(description="Parse and dump IEEE 1212 Configuration ROM")
    parser.add_argument("rom_file", help="Path to binary ROM image file (.img or .bin)")
    parser.add_argument("--no-swap", action="store_true", help="Don't word-swap the input (for wire-format ROMs)")
    args = parser.parse_args()
    
    try:
        with open(args.rom_file, "rb") as f:
            rom_data = f.read()
        
        # Auto-detect if we need to swap by checking for '1394' signature
        q1_normal = be32(rom_data, 4) if len(rom_data) >= 8 else 0
        needs_swap = q1_normal != 0x31333934
        
        if needs_swap and not args.no_swap:
            print(f"Detected host-order ROM dump, word-swapping...")
            rom_data = swap_words(rom_data)
        
        print(f"Parsing {len(rom_data)} bytes from {args.rom_file}...\n")
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
