
from dataclasses import dataclass, field
from typing import List, Dict, Optional, Tuple, Any, Iterable
import struct
import io
import json
import sys
import argparse

# ==============================
# Core data structures
# ==============================

@dataclass
class DirectoryEntry:
    offset_in_rom: int              # byte offset of this entry within the ROM blob
    raw: int                        # 32-bit raw quadlet (big-endian)
    key: int                        # 8-bit "key" field (MSB byte of raw)
    entry_type: int                 # top 2 bits of key: 0=Immediate,1=CSR,2=Leaf,3=Directory
    key_id: int                     # lower 6 bits of key
    value: int                      # low 24 bits of raw
    resolved_addr_units: Optional[int] = None  # absolute Units-space address (for CSR offset)
    target_rom_offset: Optional[int] = None    # byte offset in ROM this entry points to (for Leaf/Directory)
    comment: Optional[str] = None

@dataclass
class Leaf:
    start_offset: int               # byte offset of the first quadlet of the leaf header
    length_quadlets: int            # number of data quadlets following the header
    crc_header: int                 # CRC stored in the leaf header
    crc_calc: int                   # CRC computed from data quadlets
    data: bytes                     # payload bytes (4 * length_quadlets)
    decoded: Dict[str, Any] = field(default_factory=dict)  # best-effort decoded view (e.g., textual descriptor)

@dataclass
class Directory:
    start_offset: int               # byte offset of the first quadlet of the directory header
    length_quadlets: int            # number of entry quadlets following the header
    crc_header: int                 # CRC stored in the directory header
    crc_calc: int                   # CRC computed from entry quadlets
    entries: List[DirectoryEntry]   # parsed entries (length == length_quadlets)

@dataclass
class BusInfoBlock:
    start_offset: int
    crc_length: int                 # number of quadlets the CRC covers (per spec 7.2/7.3 context)
    bus_info_length: int            # number of following quadlets in the bus info block (root dir follows after this+1)
    crc_header: int                 # CRC value stored in first quadlet (upper 16 bits)
    crc_calc: int                   # computed CRC across crc_length quadlets
    raw_quadlets: List[int]         # the whole bus info block (including header quadlet)
    fields: Dict[str, Any]          # best-effort parsed known fields (bus_name, max_rom, etc.)

@dataclass
class ConfigROM:
    raw: bytes
    base_units_addr: int                           # FFFF F000 0400h by default
    bus_info: BusInfoBlock
    root_dir: Directory
    all_dirs: Dict[int, Directory]                 # map ROM offset -> Directory
    all_leaves: Dict[int, Leaf]                    # map ROM offset -> Leaf

# ==============================
# Utility functions
# ==============================

def be32(data: bytes, off: int) -> int:
    """Read a big-endian quadlet."""
    return struct.unpack_from(">I", data, off)[0]

def le32(data: bytes, off: int) -> int:
    """Read a little-endian quadlet."""
    return struct.unpack_from("<I", data, off)[0]

def detect_endianness(rom: bytes) -> str:
    """Detect ROM endianness by looking for '1394' signature in quadlet 1."""
    if len(rom) < 8:
        return "big"  # default
    q1_be = be32(rom, 4)
    q1_le = le32(rom, 4)
    # IEEE 1394 signature is 0x31333934 ('1394' in ASCII)
    if q1_be == 0x31333934:
        return "big"
    elif q1_le == 0x31333934:
        return "little"
    return "big"  # default

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
    """
    Headers for Bus Info Block, Directories and Leaves encode:
      bits[31:16] = CRC (doublet)
      bits[15:0]  = length (# of following quadlets)
    """
    crc = (header_quadlet >> 16) & 0xFFFF
    length = header_quadlet & 0xFFFF
    return crc, length

def crc16_quadlet_step(crc: int, data_quadlet: int) -> int:
    """
    CRC-16 nibble-sequential algorithm from IEEE 1212 Table 5.
    Operates on a 16-bit 'crc' state and a 32-bit 'data' quadlet.
    Returns a 16-bit crc.
    """
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

# ==============================
# Spec constants
# ==============================

UNITS_BASE = 0xFFFF_F000_0000  # "units space" base for CSR offset entries
CONFIG_ROM_BASE = 0xFFFF_F000_0400  # conventional Config ROM base in units space

# Entry types per Table 7
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

# ==============================
# Parsers
# ==============================

def parse_bus_info_block(rom: bytes, base_units_addr: int = CONFIG_ROM_BASE, endianness: str = "big") -> BusInfoBlock:
    start = 0  # assuming the provided `rom` blob begins at 0xFFFFF0000400
    if len(rom) < 4:
        raise ValueError(f"ROM too small: {len(rom)} bytes, need at least 4")
    
    read32 = le32 if endianness == "little" else be32
    header = read32(rom, start)
    
    # Bus info header has different format than directory/leaf headers
    bus_info_length, crc_length, crc_header = split_bus_info_header(header)

    # Total quadlets in the bus info block (header + data)
    total_quadlets = bus_info_length + 1
    
    # Bounds check: ensure we don't read beyond the ROM
    max_quadlets = len(rom) // 4
    if total_quadlets > max_quadlets:
        print(f"Warning: bus_info_length claims {bus_info_length} quadlets, but ROM only has {max_quadlets} total. Truncating.")
        total_quadlets = max_quadlets
        bus_info_length = total_quadlets - 1
    
    quadlets = [read32(rom, start + 4 * i) for i in range(total_quadlets)]
    # CRC covers the number of quadlets specified in crc_length field (usually same as bus_info_length)
    crc_calc = crc16_over_quadlets(quadlets[1:1 + crc_length])

    fields: Dict[str, Any] = {}
    fields["endianness"] = endianness

    # Try to parse common IEEE-1394 bus info fields if present
    if len(quadlets) >= 3:
        # Quadlet[1] often encodes ASCII '1394' (0x31333934)
        fields["bus_name_quadlet"] = quadlets[1]
        try:
            fields["bus_name_ascii"] = bytes.fromhex(f"{quadlets[1]:08x}").decode("ascii", errors="ignore")
        except Exception:
            pass

    if len(quadlets) >= 4:
        q2 = quadlets[2]
        fields["max_rom_hint"] = q2 & 0x3  # per spec table, a small encoding

    return BusInfoBlock(
        start_offset=start,
        crc_length=bus_info_length,
        bus_info_length=bus_info_length,
        crc_header=crc_header,
        crc_calc=crc_calc,
        raw_quadlets=quadlets,
        fields=fields,
    )

def parse_directory(rom: bytes, dir_offset: int, endianness: str = "big") -> Directory:
    read32 = le32 if endianness == "little" else be32
    header = read32(rom, dir_offset)
    crc_header, length_quadlets = split_crc_len(header)
    entries_raw = [read32(rom, dir_offset + 4 * (i + 1)) for i in range(length_quadlets)]
    crc_calc = crc16_over_quadlets(entries_raw)

    entries: List[DirectoryEntry] = []
    for i, raw in enumerate(entries_raw):
        key = (raw >> 24) & 0xFF
        entry_type = (key >> 6) & 0x3      # top 2 bits
        key_id = key & 0x3F                # lower 6 bits
        value = raw & 0xFFFFFF
        entry_offset = dir_offset + 4 * (i + 1)

        # Resolve addresses according to type
        resolved_units = None
        target_off = None
        if entry_type == CSR_OFFSET:
            resolved_units = UNITS_BASE + 4 * value
        elif entry_type in (LEAF_OFFSET, DIR_OFFSET):
            target_off = entry_offset + 4 * value  # relative to the entry itself

        # Helpful comments for familiar keys
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

def parse_leaf(rom: bytes, leaf_offset: int, endianness: str = "big") -> Leaf:
    read32 = le32 if endianness == "little" else be32
    header = read32(rom, leaf_offset)
    crc_header, length_quadlets = split_crc_len(header)
    payload = rom[leaf_offset + 4: leaf_offset + 4 + 4 * length_quadlets]
    quadlets = list(struct.unpack(f">{length_quadlets}I", payload)) if length_quadlets else []
    crc_calc = crc16_over_quadlets(quadlets)

    decoded: Dict[str, Any] = {}

    # Best-effort textual descriptor recognition (descriptor_type=0, specifier_ID=0)
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
                    text = remaining_bytes.decode("ascii", errors="ignore").strip("\x00")
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

def walk_tree(rom: bytes, root_dir_offset: int, endianness: str = "big") -> Tuple[Dict[int, Directory], Dict[int, Leaf]]:
    """Breadth-first walk of the ROM starting from root_dir_offset, following relative pointers."""
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
    """
    Parse an IEEE 1212 Configuration ROM blob that begins at the conventional ROM base.
    Steps:
      1) Detect endianness (little or big-endian)
      2) Parse Bus Info Block (first (bus_info_length+1) quadlets).
      3) Locate & parse Root Directory that follows.
      4) Walk all relative pointers (directories/leaves) reachable within the blob.
    """
    endianness = detect_endianness(rom_bytes)
    print(f"Detected endianness: {endianness}")
    bus = parse_bus_info_block(rom_bytes, base_units_addr, endianness)
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

# ==============================
# Pretty-print helpers
# ==============================

IMMEDIATE = 0
CSR_OFFSET = 1
LEAF_OFFSET = 2
DIR_OFFSET = 3

def format_entry(e: DirectoryEntry) -> str:
    name = KEY_NAMES.get(e.key_id, f"key_{e.key_id:02x}")
    tname = {IMMEDIATE: "Imm", CSR_OFFSET: "CSR", LEAF_OFFSET: "Leaf", DIR_OFFSET: "Dir"}.get(e.entry_type, f"T{e.entry_type}")
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

# Convenience: quick parse+dump from bytes
def parse_and_dump(rom_bytes: bytes) -> str:
    return dump_config_rom(parse_config_rom(rom_bytes))

# ==============================
# Main entry point
# ==============================

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description="Parse and dump IEEE 1212 Configuration ROM")
    parser.add_argument("rom_file", help="Path to binary ROM image file (.img or .bin)")
    args = parser.parse_args()
    
    try:
        with open(args.rom_file, "rb") as f:
            rom_data = f.read()
        print(f"Parsing {len(rom_data)} bytes from {args.rom_file}...\n")
        result = parse_and_dump(rom_data)
        print(result)
    except FileNotFoundError:
        print(f"Error: File '{args.rom_file}' not found")
        sys.exit(1)
    except Exception as e:
        print(f"Error parsing ROM: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
