#!/usr/bin/env python3
"""
Music Subunit Descriptor Parser
Parses AV/C Music Subunit Status Descriptors (TA 1999045)

Usage: python3 parse_music_descriptor.py <descriptor.bin> [--skip-header] [--verbose]
"""

import sys
import struct
import os
from typing import Optional, Dict, Any

# Info Block Types (MusicSubunitInfoBlockTypes)
INFO_BLOCK_TYPES = {
    0x8100: "GeneralMusicSubunitStatusArea",
    0x8101: "MusicOutputPlugStatusArea",
    0x8102: "SourcePlugStatus",
    0x8103: "AudioInfo",
    0x8104: "MIDIInfo",
    0x8105: "SMPTETimeCodeInfo",
    0x8106: "SampleCountInfo",
    0x8107: "AudioSyncInfo",
    0x8108: "RoutingStatus",
    0x8109: "SubunitPlugInfo",
    0x810A: "ClusterInfo",
    0x810B: "MusicPlugInfo",
    0x000B: "Name",
    0x000A: "RawText"
}

# Music Port Types (MusicPortTypes)
PORT_TYPES = {
    0x00: "Speaker",
    0x01: "HeadPhone",
    0x02: "Microphone",
    0x03: "Line",
    0x04: "Spdif",
    0x05: "Adat",
    0x06: "Tdif",
    0x07: "Madi",
    0x08: "Analog",
    0x09: "Digital",
    0x0A: "Midi",
    0x0B: "AesEbu",
    0x80: "Sync (Vendor Specific)",  # Extension observed in Apogee devices
    0xFF: "NoType"
}

# Music Cluster Formats (MusicClusterFormats / Stream Formats)
STREAM_FORMATS = {
    0x00: "IEC60958-3",
    0x01: "IEC61937-3",
    0x02: "IEC61937-4",
    0x03: "IEC61937-5",
    0x04: "IEC61937-6",
    0x05: "IEC61937-7",
    0x06: "MBLA",           # Multi-Bit Linear Audio (PCM)
    0x07: "DVDAudio",
    0x08: "OneBit",
    0x09: "OneBitSACD",
    0x0A: "OneBitEncoded",
    0x0B: "OneBitSACDEncoded",
    0x0C: "HiPrecisionMBLA",
    0x0D: "MidiConf",       # MIDI Conformant
    0x0E: "SMPTETimeCode",
    0x0F: "SampleCount",
    0x10: "AncillaryData",
    0x40: "SyncStream",     # Sync stream
    0xFF: "DontCare"
}

# Music Plug Types (MusicPlugTypes)
MUSIC_PLUG_TYPES = {
    0x00: "Audio",
    0x01: "Midi",
    0x02: "Smpte",
    0x03: "SampleCount",
    0x80: "Sync"
}

# Music Plug Routing Support (MusicPlugRoutingSupport)
ROUTING_SUPPORT = {
    0x00: "Fixed",
    0x01: "Cluster",
    0x02: "Flexible",
    0xFF: "Unknown"
}

# Music Plug Location (MusicPlugLocation)
PLUG_LOCATIONS = {
    0x01: "LeftFront",
    0x02: "RightFront",
    0x03: "CenterFront",
    0x04: "LFE",
    0x05: "LeftSurround",
    0x06: "RightSurround",
    0x07: "LeftOfCenter",
    0x08: "RightOfCenter",
    0x09: "Surround",
    0x0A: "SideLeft",
    0x0B: "SideRight",
    0x0C: "Top",
    0x0D: "Bottom",
    0x0E: "LeftFrontEffect",
    0x0F: "RightFrontEffect",
    0xFF: "Unknown"
}

# Subunit Plug Usages (MusicSubunitPlugUsages)
PLUG_USAGES = {
    0x00: "IsochStream",
    0x01: "AsynchStream",
    0x02: "Midi",
    0x03: "Sync",
    0x04: "Analog",
    0x05: "Digital"
}

# FDF Format Hierarchy Root (first byte of signal format)
FDF_HIERARCHY = {
    0x00: "DVCR",           # DV/HDV video
    0x10: "Audio/Music",    # Audio
    0x20: "SDI",            # Serial Digital Interface
    0x80: "Vendor",         # Vendor-specific
    0x90: "AM824",          # AM824 (IEC 61883-6) - most common for audio
    0xA0: "Audio Raw",      # Raw audio
    0xFF: "DontCare"
}

# AM824 Subtypes (second byte when hierarchy is 0x90)
AM824_SUBTYPES = {
    0x00: "Simple",         # Simple AM824 (no rate info in 2nd byte)
    0x01: "Simple-1",       # Simple with rate
    0x02: "Simple-2",
    0x04: "Simple-4",
    0x08: "Simple-8",
    0x40: "Compound",       # Compound stream (multiple formats)
    0xFF: "DontCare"
}

# Sample Rate Codes (used in FDF format)
FDF_SAMPLE_RATES = {
    0x00: (32000, "32 kHz"),
    0x01: (44100, "44.1 kHz"),
    0x02: (48000, "48 kHz"),
    0x03: (88200, "88.2 kHz"),
    0x04: (96000, "96 kHz"),
    0x05: (176400, "176.4 kHz"),
    0x06: (192000, "192 kHz"),
    0x07: (22050, "22.05 kHz"),  # Some devices
    0x08: (24000, "24 kHz"),     # Some devices
    0x0F: (0, "Don't Care")
}

# Global verbose flag
VERBOSE = False

# Global registry for collecting channel info (populated during parsing)
CHANNEL_REGISTRY = {
    'music_plugs': {},      # musicPlugID -> {name, port_type, ...}
    'clusters': [],         # [{plug_id, format, port_type, signals: [{musicPlugID, position, ...}]}]
    'subunit_plugs': [],    # [{plug_id, name, direction, format, clusters: [...]}]
}

def reset_channel_registry():
    """Reset the channel registry for a new parse"""
    global CHANNEL_REGISTRY
    CHANNEL_REGISTRY = {
        'music_plugs': {},
        'clusters': [],
        'subunit_plugs': [],
    }

def print_channel_summary():
    """Print a summary of cluster-to-channel mapping"""
    global CHANNEL_REGISTRY
    
    music_plugs = CHANNEL_REGISTRY['music_plugs']
    subunit_plugs = CHANNEL_REGISTRY['subunit_plugs']
    
    if not music_plugs and not subunit_plugs:
        return
    
    print("\n" + "=" * 60)
    print("📋 CHANNEL MAPPING SUMMARY")
    print("=" * 60)
    
    # Show MusicPlugInfo list (channel names)
    if music_plugs:
        print("\n🎵 Music Plug Channels:")
        for plug_id in sorted(music_plugs.keys()):
            info = music_plugs[plug_id]
            name = info.get('name', '(unnamed)')
            port_type = info.get('port_type_name', 'Unknown')
            print(f"  ID 0x{plug_id:04X}: {name} [{port_type}]")
    
    # Show Subunit Plugs with their cluster mappings
    if subunit_plugs:
        print("\n🔌 Subunit Plugs → Channels:")
        for plug in subunit_plugs:
            plug_id = plug.get('plug_id', '?')
            name = plug.get('name', '(unnamed)')
            direction = plug.get('direction', '?')
            direction_arrow = '→' if direction == 'Dest' else '←' if direction == 'Src' else '?'
            
            print(f"\n  Plug {plug_id} {direction_arrow} {name or '(no name)'}:")
            
            clusters = plug.get('clusters', [])
            if clusters:
                for cluster in clusters:
                    fmt = cluster.get('format', '?')
                    signals = cluster.get('signals', [])
                    print(f"    Format: {fmt} ({len(signals)} channels)")
                    for sig in signals:
                        mpid = sig.get('musicPlugID', 0)
                        pos = sig.get('position', 0)
                        # Look up name from music_plugs registry
                        mp_info = music_plugs.get(mpid, {})
                        ch_name = mp_info.get('name', f'(MusicPlug 0x{mpid:04X})')
                        print(f"      Ch {pos}: {ch_name}")
            else:
                print(f"    (no cluster info)")
    
    print("\n" + "=" * 60)


def parse_fdf_format(fdf_value):
    """
    Parse FDF (Format Dependent Field) value.
    
    For SubunitPlugInfo, FDF is typically 16-bit:
    - High byte: Format hierarchy (0x90 = AM824)
    - Low byte: Sample rate code OR subtype
    
    Returns dict with parsed info.
    """
    high_byte = (fdf_value >> 8) & 0xFF
    low_byte = fdf_value & 0xFF
    
    result = {
        'raw': fdf_value,
        'hierarchy': FDF_HIERARCHY.get(high_byte, f"Unknown(0x{high_byte:02X})"),
        'hierarchy_code': high_byte
    }
    
    if high_byte == 0x90:  # AM824
        # For simple AM824, low byte is sample rate
        # For compound (0x40), different structure
        if low_byte == 0x40:
            result['subtype'] = 'Compound'
            result['description'] = "AM824 Compound"
        elif low_byte in FDF_SAMPLE_RATES:
            rate_hz, rate_name = FDF_SAMPLE_RATES[low_byte]
            result['subtype'] = 'Simple'
            result['sample_rate_code'] = low_byte
            result['sample_rate_hz'] = rate_hz
            result['sample_rate_name'] = rate_name
            result['description'] = f"AM824 {rate_name}"
        else:
            # Might be encoded differently
            result['subtype_raw'] = low_byte
            result['description'] = f"AM824 (0x{low_byte:02X})"
    else:
        result['description'] = f"{result['hierarchy']} (0x{fdf_value:04X})"
    
    return result


def format_fdf_string(fdf_value):
    """Format FDF value as human-readable string"""
    parsed = parse_fdf_format(fdf_value)
    return f"0x{fdf_value:04X} ({parsed['description']})"


def get_stream_format_name(val):
    """Get stream format name with fallback"""
    return STREAM_FORMATS.get(val, f"0x{val:02X}")


def get_location_name(val):
    """Get plug location name with fallback"""
    return PLUG_LOCATIONS.get(val, f"0x{val:02X}")


def get_port_type_name(val):
    """Get port type name with fallback"""
    return PORT_TYPES.get(val, f"0x{val:02X}")


def decode_capability_bits(value):
    """Return human-readable capability flags from TX/RX capability byte"""
    modes = []
    if value & 0x01:
        modes.append("Non-Blocking")
    if value & 0x02:
        modes.append("Blocking")
    if not modes:
        modes.append("None")
    return ", ".join(modes)


class AVCInfoBlock:
    """Parser for AV/C Info Block structures per TA 1999045"""
    
    def __init__(self, data, offset=0, indent=0, parent_end=None):
        self.data = data
        self.offset = offset
        self.indent = indent
        self.parsed_length = 0
        self.compound_length = 0
        self.type_val = 0
        self.primary_fields_length = 0
        self.nested_blocks = []
        self.valid = False
        self.errors = []
        self.truncated = False
        
        if parent_end is None:
            self.parent_end = len(data)
        else:
            self.parent_end = parent_end
            
        self.parse()

    def parse(self):
        available = len(self.data) - self.offset
        if available < 4:
            self.errors.append(f"Not enough data for header at offset {self.offset}")
            return

        # Header: Compound Length (2 bytes), Type (2 bytes)
        self.compound_length = struct.unpack_from(">H", self.data, self.offset)[0]
        self.type_val = struct.unpack_from(">H", self.data, self.offset + 2)[0]
        
        # Special case for empty block
        if self.compound_length == 0:
            self.parsed_length = 2
            self.valid = True
            return

        # Total bytes this block claims
        self.parsed_length = 2 + self.compound_length
        
        # Check for truncation
        if self.offset + self.parsed_length > len(self.data):
            self.errors.append(f"Block truncated: claims {self.parsed_length} bytes, only {available} available")
            self.truncated = True
            # Parse what we have
            self.parsed_length = available
        
        # Primary Fields Length (2 bytes)
        if available >= 6:
            self.primary_fields_length = struct.unpack_from(">H", self.data, self.offset + 4)[0]
        else:
            self.primary_fields_length = 0

        self.valid = True
        
        # Validate Primary Fields Length
        max_pfl = self.compound_length - 4 if self.compound_length >= 4 else 0
        if self.primary_fields_length > max_pfl:
            self.errors.append(f"Primary Fields Length {self.primary_fields_length} > Max Possible {max_pfl}")
            # Reset PFL to allow scanning for nested blocks
            self.primary_fields_length = 0

        # Parse Nested Blocks
        primary_fields_end = self.offset + 6 + self.primary_fields_length
        
        # Name block (0x000B) has special offset per spec
        if self.type_val == 0x000B: 
            primary_fields_end = self.offset + 10
        
        current = primary_fields_end
        block_end = min(self.offset + 2 + self.compound_length, len(self.data))
        
        while current + 4 <= block_end:
            try:
                pot_len = struct.unpack_from(">H", self.data, current)[0]
                pot_type = struct.unpack_from(">H", self.data, current + 2)[0]
                
                # Only parse known block types to avoid garbage
                if pot_type in INFO_BLOCK_TYPES:
                    nested = AVCInfoBlock(self.data, current, self.indent + 1, block_end)
                    if nested.valid:
                        self.nested_blocks.append(nested)
                        
                        # CRITICAL: Stop if nested block was truncated
                        if nested.truncated:
                            self.errors.append(f"Stopping nested parse due to truncation at offset {current}")
                            break
                            
                        # Advance by parsed length
                        if nested.parsed_length > 0:
                            current += nested.parsed_length
                        else:
                            break
                        continue
            except:
                pass
            
            # Unknown data - stop parsing rather than scanning
            break

        self.validate()

    def validate(self):
        """Validate block structure"""
        if self.type_val == 0x8108:  # RoutingStatus
            if self.primary_fields_length >= 4:
                start = self.offset + 6
                if start + 4 <= len(self.data):
                    in_plugs = self.data[start]
                    out_plugs = self.data[start + 1]
                    music_count = (self.data[start + 2] << 8) | self.data[start + 3]
                    
                    subunit_count = sum(1 for b in self.nested_blocks if b.type_val == 0x8109)
                    music_nested = sum(1 for b in self.nested_blocks if b.type_val == 0x810B)
                    
                    if subunit_count != (in_plugs + out_plugs):
                        self.errors.append(f"SubunitPlugInfo count ({subunit_count}) != Dest+Src ({in_plugs}+{out_plugs})")
                    if music_nested != music_count:
                        self.errors.append(f"MusicPlugInfo count ({music_nested}) != Expected ({music_count})")

    def print_info(self):
        prefix = "  " * self.indent
        type_str = INFO_BLOCK_TYPES.get(self.type_val, f"Unknown(0x{self.type_val:04X})")
        
        print(f"{prefix}Block: {type_str}")
        print(f"{prefix}  Offset: {self.offset} (0x{self.offset:X})")
        print(f"{prefix}  Compound Length: {self.compound_length}")
        print(f"{prefix}  Primary Fields Length: {self.primary_fields_length}")
        
        for err in self.errors:
            print(f"{prefix}  WARNING: {err}")
        
        self.print_primary_fields(prefix)
        
        if self.nested_blocks:
            print(f"{prefix}  Nested Blocks: {len(self.nested_blocks)}")
            for block in self.nested_blocks:
                block.print_info()

    def print_primary_fields(self, prefix):
        start = self.offset + 6
        length = self.primary_fields_length
        if length == 0:
            return

        if start + length > len(self.data):
            trim_len = len(self.data) - start
            print(f"{prefix}  WARNING: Primary fields truncated ({length} -> {trim_len})")
            length = trim_len
            
        if length <= 0:
            return
            
        fields = self.data[start:start + length]
        
        if self.type_val == 0x000A:  # RawText
            try:
                text = fields.decode('utf-8', errors='replace').replace('\x00', '')
                print(f'{prefix}  Text: "{text}"')
            except:
                print(f"{prefix}  Text: <Binary>")

        elif self.type_val == 0x8100:  # GeneralMusicSubunitStatusArea
            if len(fields) >= 6:
                tx = fields[0]
                rx = fields[1]
                print(f"{prefix}  Transmit Capability: 0x{tx:02X} ({decode_capability_bits(tx)})")
                print(f"{prefix}  Receive Capability: 0x{rx:02X} ({decode_capability_bits(rx)})")
                latency = struct.unpack_from(">I", fields, 2)[0]
                print(f"{prefix}  Latency: {latency} (0x{latency:08X})")
                
        elif self.type_val == 0x8108:  # RoutingStatus
            if len(fields) >= 4:
                print(f"{prefix}  Dest Plugs: {fields[0]}")
                print(f"{prefix}  Source Plugs: {fields[1]}")
                print(f"{prefix}  Music Plugs: {(fields[2] << 8) | fields[3]}")
                
        elif self.type_val == 0x8109:  # SubunitPlugInfo
            if len(fields) >= 1:
                print(f"{prefix}  Plug ID: {fields[0]}")
            if len(fields) >= 3:
                sig_fmt = (fields[1] << 8) | fields[2]
                # Use comprehensive FDF parser
                print(f"{prefix}  Signal Format: {format_fdf_string(sig_fmt)}")
            if len(fields) >= 4:
                usage = fields[3]
                print(f"{prefix}  Plug Usage: {PLUG_USAGES.get(usage, f'0x{usage:02X}')}")
            if len(fields) >= 6 and VERBOSE:
                num_clusters = (fields[4] << 8) | fields[5]
                print(f"{prefix}  Num Clusters: {num_clusters}")
            if len(fields) >= 8 and VERBOSE:
                num_channels = (fields[6] << 8) | fields[7]
                print(f"{prefix}  Num Channels: {num_channels}")
                
        elif self.type_val == 0x810A:  # ClusterInfo
            if len(fields) >= 3:
                fmt = fields[0]
                port = fields[1]
                num_signals = fields[2]
                print(f"{prefix}  Stream Format: {get_stream_format_name(fmt)} (0x{fmt:02X})")
                print(f"{prefix}  Port Type: {get_port_type_name(port)}")
                print(f"{prefix}  Num Signals: {num_signals}")
                # Parse signal entries (4 bytes each)
                for i in range(num_signals):
                    idx = 3 + i * 4
                    if idx + 4 <= len(fields):
                        plug_id = (fields[idx] << 8) | fields[idx + 1]
                        ch = fields[idx + 2]
                        loc = fields[idx + 3]
                        loc_name = get_location_name(loc)
                        print(f"{prefix}    Signal {i}: PlugID=0x{plug_id:04X}, Ch={ch}, Loc={loc_name}")
                        
        elif self.type_val == 0x810B:  # MusicPlugInfo
            # Structure per FWA AVCInfoBlock.cpp (lines 467-484):
            # byte 0: music plug type
            # byte 1-2: music plug ID (big-endian)
            # byte 3: routing support
            # Source connection (5 bytes):
            # byte 4: plugFunctionType (e.g., 0xF0 = subunit dest plug)
            # byte 5: plugId
            # byte 6: plugFunctionBlockId
            # byte 7: streamPosition
            # byte 8: streamLocation
            # Destination connection (5 bytes):
            # byte 9: plugFunctionType (e.g., 0xF1 = audio output)
            # byte 10: plugId
            # byte 11: plugFunctionBlockId
            # byte 12: streamPosition
            # byte 13: streamLocation
            if len(fields) >= 1:
                plug_type = fields[0]
                print(f"{prefix}  Plug Type: {MUSIC_PLUG_TYPES.get(plug_type, get_port_type_name(plug_type))}")
            if len(fields) >= 3:
                plug_id = (fields[1] << 8) | fields[2]
                print(f"{prefix}  Music Plug ID: 0x{plug_id:04X} ({plug_id})")
            if len(fields) >= 4:
                routing = fields[3]
                print(f"{prefix}  Routing: {ROUTING_SUPPORT.get(routing, f'0x{routing:02X}')}")
            # Source connection (bytes 4-8)
            if len(fields) >= 9:
                src_func_type = fields[4]
                src_plug = fields[5]
                src_block = fields[6]
                src_pos = fields[7]
                src_loc = fields[8]
                func_type_str = f"0x{src_func_type:02X}"
                if src_func_type == 0xF0:
                    func_type_str = "SubunitDestPlug (0xF0)"
                elif src_func_type == 0xF1:
                    func_type_str = "AudioOutput (0xF1)"
                print(f"{prefix}  Source: FuncType={func_type_str}, Plug={src_plug}, Block=0x{src_block:02X}, Pos={src_pos}, Loc={get_location_name(src_loc)}")
            # Destination connection (bytes 9-13)
            if len(fields) >= 14:
                dst_func_type = fields[9]
                dst_plug = fields[10]
                dst_block = fields[11]
                dst_pos = fields[12]
                dst_loc = fields[13]
                func_type_str = f"0x{dst_func_type:02X}"
                if dst_func_type == 0xF0:
                    func_type_str = "SubunitDestPlug (0xF0)"
                elif dst_func_type == 0xF1:
                    func_type_str = "AudioOutput (0xF1)"
                print(f"{prefix}  Dest: FuncType={func_type_str}, Plug={dst_plug}, Block=0x{dst_block:02X}, Pos={dst_pos}, Loc={get_location_name(dst_loc)}")
            
            # Register this MusicPlugInfo in the global registry
            if len(fields) >= 3:
                plug_id = (fields[1] << 8) | fields[2]
                plug_type = fields[0] if len(fields) >= 1 else 0
                # Try to extract name from nested RawText block
                name = "(unnamed)"
                for nested in self.nested_blocks:
                    if nested.type_val == 0x000A:  # RawText
                        name_fields = nested.data[nested.offset + 6 : nested.offset + 6 + nested.primary_fields_length]
                        if name_fields:
                            name = bytes(name_fields).decode('utf-8', errors='ignore').strip('\x00')
                            break
                
                global CHANNEL_REGISTRY
                CHANNEL_REGISTRY['music_plugs'][plug_id] = {
                    'name': name,
                    'port_type': plug_type,
                    'port_type_name': MUSIC_PLUG_TYPES.get(plug_type, get_port_type_name(plug_type)),
                }


def detect_descriptor_prefix(data: bytes) -> Optional[Dict[str, Any]]:
    """Detect and describe a descriptor-length prefix + leading info block"""
    if len(data) < 6:
        return None

    descriptor_length = struct.unpack_from(">H", data, 0)[0]
    first_block_compound = struct.unpack_from(">H", data, 2)[0]
    first_block_type = struct.unpack_from(">H", data, 4)[0]
    expected_end = descriptor_length + 2  # Descriptor length does not include the 2-byte prefix

    # Basic sanity checks to avoid false positives
    first_block_total = first_block_compound + 2
    descriptor_fits = expected_end <= len(data)
    first_block_fits = first_block_total <= descriptor_length
    type_known = first_block_type in INFO_BLOCK_TYPES

    if descriptor_fits and first_block_fits and type_known:
        return {
            "descriptor_length": descriptor_length,
            "first_block_compound": first_block_compound,
            "first_block_total": first_block_total,
            "first_block_type": first_block_type,
            "expected_end": expected_end
        }

    return None


def parse_file(file_path, skip_header=False):
    """Parse a descriptor file"""
    # Reset global channel registry for fresh parse
    reset_channel_registry()
    
    with open(file_path, "rb") as f:
        data = f.read()

    print(f"Parsing {file_path} ({len(data)} bytes)...")

    offset = 0
    valid_end = None
    descriptor_prefix = detect_descriptor_prefix(data)

    if descriptor_prefix:
        first_type_str = INFO_BLOCK_TYPES.get(descriptor_prefix["first_block_type"], f"0x{descriptor_prefix['first_block_type']:04X}")
        print("\nDetected descriptor-length prefix:")
        print(f"  Descriptor length (after prefix): {descriptor_prefix['descriptor_length']} bytes")
        print(f"  Leading block: {first_type_str} (compound_length={descriptor_prefix['first_block_compound']})")
        offset = 2  # Skip the descriptor-length prefix
        valid_end = descriptor_prefix["expected_end"]

        if skip_header:
            print("Skipping leading block (--skip-header) to start at next root block.")
            offset += descriptor_prefix["first_block_total"]
    elif skip_header:
        print("\n--skip-header specified without descriptor prefix detection; skipping first 14 bytes.")
        offset = 14

    print("-" * 40)

    # Determine valid data range if not set by descriptor prefix
    if valid_end is None:
        if offset + 4 > len(data):
            print("Error: Not enough data")
            return

        root_compound_len = (data[offset] << 8) | data[offset + 1]
        valid_end = offset + 2 + root_compound_len
        print(f"Root block compound_length={root_compound_len}, valid data ends at offset {valid_end}")
    else:
        print(f"Valid descriptor data ends at offset {valid_end}")

    print("-" * 40)

    while offset < valid_end and offset < len(data):
        if offset + 4 > len(data):
            break

        block = AVCInfoBlock(data, offset)
        if block.valid:
            block.print_info()
            if block.parsed_length > 0:
                offset += block.parsed_length
            else:
                break
        else:
            break

        print("-" * 20)

    if offset < valid_end:
        print(f"\nWARNING: Parsing stopped at offset {offset}, expected to reach {valid_end}")
    elif offset > valid_end:
        print(f"\nNOTE: Parsed beyond expected end ({offset} > {valid_end})")
    else:
        print(f"\n✓ Parsing complete at expected offset {valid_end}")

    # Check for trailing blocks after main descriptor
    if valid_end < len(data):
        trailing_len = len(data) - valid_end
        print(f"\n{'=' * 40}")
        print(f"TRAILING DATA: {trailing_len} bytes after main descriptor")
        print(f"{'=' * 40}")

        offset = valid_end
        block_num = 0

        while offset + 4 <= len(data):
            block_num += 1
            compound_len = (data[offset] << 8) | data[offset + 1]
            block_type = (data[offset + 2] << 8) | data[offset + 3]

            # Check for valid block
            if compound_len == 0:
                print(f"\nBlock {block_num}: Empty block (end marker)")
                break

            block_end = offset + 2 + compound_len
            if block_end > len(data):
                print(f"\nBlock {block_num}: Partial/Invalid (extends past EOF)")
                break

            # Parse trailing block
            block = AVCInfoBlock(data, offset)
            if block.valid:
                block.print_info()
                offset += block.parsed_length
                print("-" * 20)
            else:
                break
    
    # Print channel mapping summary
    print_channel_summary()


def main():
    global VERBOSE
    
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <descriptor_file> [--skip-header] [--verbose]")
        print()
        print("Options:")
        print("  --skip-header  Skip first 14 bytes (Apogee device quirk)")
        print("  --verbose      Show additional fields")
        sys.exit(1)

    file_path = sys.argv[1]
    skip_header = "--skip-header" in sys.argv
    VERBOSE = "--verbose" in sys.argv
    
    if not os.path.exists(file_path):
        print(f"Error: File '{file_path}' not found.")
        sys.exit(1)

    try:
        parse_file(file_path, skip_header)
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()


if __name__ == "__main__":
    main()
