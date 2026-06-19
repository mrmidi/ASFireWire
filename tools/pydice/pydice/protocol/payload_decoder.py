"""Payload decoder for DICE/TCAT block transfers."""
import struct

from .tcat.global_section import (
    _deserialize_labels,
    CLOCK_SOURCE_LABEL_TABLE,
    deserialize as _deserialize_global,
    MIN_SIZE as _GLOBAL_MIN_SIZE,
)
from .tcat.router_entry import deserialize_router_entries, ROUTER_ENTRY_SIZE
from .codec import unpack_label
from .dice_address_map import _decode_iso_channel, _decode_speed


def decode_payload(
    address: str | None, payload: bytes, size: int | None
) -> list[str]:
    """Return list of human-readable description lines for a block payload."""
    if not payload:
        return []

    if address:
        parts = address.split(".")
        region = ".".join(parts[1:]) if len(parts) >= 2 else address
    else:
        region = None

    if region == "ffff.e000.0000":
        return _decode_section_layout(payload)
    if region == "ffff.e000.0028":
        if len(payload) == 16:
            return _decode_cas(payload)
        return _decode_global_section(payload)
    if region == "ffff.e000.0034":
        return _decode_nick_name(payload)
    if region == "ffff.e000.0090":
        return _decode_clock_source_names(payload)
    if region == "ffff.e000.01a4":
        return _decode_tx_section(payload)
    if region == "ffff.e000.01bc":
        return _decode_channel_names(payload, "TX[0]")
    if region == "ffff.e000.03dc":
        return _decode_rx_section(payload)
    if region == "ffff.e000.03f4":
        return _decode_channel_names(payload, "RX[0]")
    if region == "ffff.e020.0000":
        return _decode_tcat_ext_header(payload)
    if region == "ffff.f000.0220":
        return _decode_irm_bandwidth(payload)
    if region in ("ffff.f000.0224", "ffff.f000.0228"):
        base_ch = 0 if region == "ffff.f000.0224" else 32
        return _decode_irm_channels(payload, base_ch)
    if region == "ffff.e020.06e8":
        return _decode_router_entries(payload)
    if region == "ffff.e020.6d70":
        return [f"[INVESTIGATE] {len(payload)}B raw"] + _hex_dump(payload)
    if region == "ffff.e020.0d28":
        return [f"[INVESTIGATE] playlist {len(payload)}B"] + _hex_dump(payload)

    return _hex_dump(payload)


# ── section decoders ──────────────────────────────────────────────────────────

_SECTION_LAYOUT_PAIRS = [
    ("DICE_GLOBAL_OFFSET", "DICE_GLOBAL_SIZE"),
    ("DICE_TX_OFFSET",     "DICE_TX_SIZE"),
    ("DICE_RX_OFFSET",     "DICE_RX_SIZE"),
    ("DICE_EXT_SYNC_OFFSET", "DICE_EXT_SYNC_SIZE"),
]


def _decode_section_layout(payload: bytes) -> list[str]:
    lines = []
    for i, (offset_name, size_name) in enumerate(_SECTION_LAYOUT_PAIRS):
        base = i * 8
        if base + 8 > len(payload):
            break
        offset_val, size_val = struct.unpack(">II", payload[base : base + 8])
        lines.append(f"{offset_name}: {offset_val} quadlets ({offset_val * 4:#x} bytes)")
        lines.append(f"{size_name}: {size_val} quadlets ({size_val * 4:#x} bytes)")
    return lines


def _decode_global_section(payload: bytes) -> list[str]:
    lines = []
    if len(payload) < _GLOBAL_MIN_SIZE:
        lines.append(f"[too short: {len(payload)}B, need {_GLOBAL_MIN_SIZE}B]")
        return lines + _hex_dump(payload)
    try:
        params = _deserialize_global(payload)
        lines.append(f"OWNER: {_format_owner(params.owner)}")
        lines.append(f"NOTIFICATION: 0x{params.latest_notification:08x}")
        lines.append(f"NICK_NAME: {params.nickname!r}")
        lines.append(
            f"CLOCK_SELECT: {params.clock_config.rate.name}, {params.clock_config.src.name}"
        )
        lines.append(f"ENABLE: {params.enable}")
        lines.append(
            f"STATUS: locked={params.clock_status.src_is_locked}, {params.clock_status.rate.name}"
        )
        lines.append(f"SAMPLE_RATE: {params.current_rate} Hz")
        if params.version:
            lines.append(f"VERSION: 0x{params.version:08x}")
        if params.avail_rates:
            lines.append(f"CLOCK_CAPS rates: {[r.name for r in params.avail_rates]}")
        if params.avail_sources:
            lines.append(f"CLOCK_CAPS srcs: {[s.name for s in params.avail_sources]}")
        for src, lbl in params.clock_source_labels:
            lines.append(f"  src label: {src.name} = {lbl!r}")
    except Exception as exc:
        lines.append(f"[decode error: {exc}]")
        lines.extend(_hex_dump(payload))
    return lines


def _format_owner(owner: int) -> str:
    if owner == 0xFFFF000000000000:
        return "No owner"
    node = (owner >> 48) & 0xFFFF
    addr = owner & 0x0000FFFFFFFFFFFF
    return f"node 0x{node:04x} notify@0x{addr:012x}"


def _decode_cas(payload: bytes) -> list[str]:
    if len(payload) >= 16:
        old_hi, old_lo, new_hi, new_lo = struct.unpack(">IIII", payload[:16])
        old_val = (old_hi << 32) | old_lo
        new_val = (new_hi << 32) | new_lo
        return [
            f"CAS old_val: 0x{old_val:016x}",
            f"CAS new_val: 0x{new_val:016x}",
        ]
    return _hex_dump(payload)


def _decode_nick_name(payload: bytes) -> list[str]:
    """Decode 64-byte byte-swapped DICE nickname label."""
    name = unpack_label(payload[:64])
    return [f"Nickname: {name!r}"]


def _decode_clock_source_names(payload: bytes) -> list[str]:
    """Decode 256-byte backslash-separated clock source name labels."""
    labels = _deserialize_labels(payload[:256])
    lines = []
    for i, src in enumerate(CLOCK_SOURCE_LABEL_TABLE):
        label = labels[i] if i < len(labels) else "(none)"
        lines.append(f"  {src.name}: {label!r}")
    return lines if lines else _hex_dump(payload)


def _decode_tx_section(payload: bytes) -> list[str]:
    """Decode full TX section block (starts at ffff.e000.01a4)."""
    if len(payload) < 8:
        return [f"[too short: {len(payload)}B]"] + _hex_dump(payload)
    tx_number, tx_size = struct.unpack(">II", payload[0:8])
    lines = [
        f"TX_NUMBER: {tx_number}",
        f"TX_SIZE: {tx_size} quadlets ({tx_size * 4:#x} bytes)",
    ]
    stream_stride = tx_size * 4
    for i in range(tx_number):
        base = 8 + i * stream_stride
        if base + 16 > len(payload):
            lines.append(f"  TX[{i}] [truncated at {len(payload)}B]")
            break
        iso_ch, audio_ch, midi_ports, speed = struct.unpack(">IIII", payload[base : base + 16])
        lines.append(f"  TX[{i}] ISO channel: {_decode_iso_channel(iso_ch)}")
        lines.append(f"  TX[{i}] audio channels: {audio_ch}")
        lines.append(f"  TX[{i}] MIDI ports: {midi_ports}")
        lines.append(f"  TX[{i}] speed: {_decode_speed(speed)}")
        names_start = base + 16
        names_end = min(names_start + 256, len(payload))
        if names_start < len(payload):
            labels = _deserialize_labels(payload[names_start:names_end])
            for j, label in enumerate(labels):
                lines.append(f"  TX[{i}] ch {j}: {label!r}")
    return lines


def _decode_rx_section(payload: bytes) -> list[str]:
    """Decode full RX section block (starts at ffff.e000.03dc)."""
    if len(payload) < 8:
        return [f"[too short: {len(payload)}B]"] + _hex_dump(payload)
    rx_number, rx_size = struct.unpack(">II", payload[0:8])
    lines = [
        f"RX_NUMBER: {rx_number}",
        f"RX_SIZE: {rx_size} quadlets ({rx_size * 4:#x} bytes)",
    ]
    stream_stride = rx_size * 4
    for i in range(rx_number):
        base = 8 + i * stream_stride
        if base + 16 > len(payload):
            lines.append(f"  RX[{i}] [truncated at {len(payload)}B]")
            break
        iso_ch, seq_start, audio_ch, midi_ports = struct.unpack(">IIII", payload[base : base + 16])
        lines.append(f"  RX[{i}] ISO channel: {_decode_iso_channel(iso_ch)}")
        lines.append(f"  RX[{i}] seq start: {seq_start}")
        lines.append(f"  RX[{i}] audio channels: {audio_ch}")
        lines.append(f"  RX[{i}] MIDI ports: {midi_ports}")
        names_start = base + 16
        names_end = min(names_start + 256, len(payload))
        if names_start < len(payload):
            labels = _deserialize_labels(payload[names_start:names_end])
            for j, label in enumerate(labels):
                lines.append(f"  RX[{i}] ch {j}: {label!r}")
    return lines


def _decode_tcat_ext_header(payload: bytes) -> list[str]:
    """Decode TCAT extension section header (variable pairs of offset/size quadlets)."""
    lines = []
    for i in range(len(payload) // 8):
        base = i * 8
        offset_val, size_val = struct.unpack(">II", payload[base : base + 8])
        lines.append(f"TCAT_SECT_{i}_OFFSET: {offset_val} quadlets ({offset_val * 4:#x} bytes)")
        lines.append(f"TCAT_SECT_{i}_SIZE: {size_val} quadlets ({size_val * 4:#x} bytes)")
    return lines if lines else _hex_dump(payload)


def _decode_irm_bandwidth(payload: bytes) -> list[str]:
    """Decode IRM BANDWIDTH_AVAILABLE LockRq (8B) or LockResp (4B)."""
    if len(payload) == 4:
        bw = struct.unpack(">I", payload)[0]
        return [f"IRM_BANDWIDTH_AVAILABLE returned: {bw} units"]
    if len(payload) >= 8:
        old_bw, new_bw = struct.unpack(">II", payload[:8])
        delta = old_bw - new_bw
        if delta > 0:
            action = f"allocate {delta} (0x{delta:x}) units"
        elif delta < 0:
            action = f"release {-delta} (0x{-delta:x}) units"
        else:
            action = "no change"
        return [
            f"IRM_BANDWIDTH_AVAILABLE: old={old_bw}, new={new_bw}",
            f"  → {action}",
        ]
    return _hex_dump(payload)


def _decode_irm_channels(payload: bytes, base_ch: int) -> list[str]:
    """Decode IRM CHANNELS_AVAILABLE LockRq (8B) or LockResp (4B).

    Bit layout (IEEE 1394): MSB (bit 31) = lowest channel in range.
    For HI register base_ch=0 (channels 0–31); LO base_ch=32 (channels 32–63).
    """
    reg = "IRM_CHANNELS_AVAILABLE_HI" if base_ch == 0 else "IRM_CHANNELS_AVAILABLE_LO"
    if len(payload) == 4:
        bitmap = struct.unpack(">I", payload)[0]
        return [f"{reg} returned: 0x{bitmap:08x}"]
    if len(payload) >= 8:
        old_bitmap, new_bitmap = struct.unpack(">II", payload[:8])
        lines = [f"{reg}: old=0x{old_bitmap:08x}, new=0x{new_bitmap:08x}"]
        changed = old_bitmap ^ new_bitmap
        if changed == 0:
            lines.append("  → no change (compare-verify)")
        else:
            for bit in range(32):
                if changed & (1 << bit):
                    ch = base_ch + (31 - bit)
                    if old_bitmap & (1 << bit) and not (new_bitmap & (1 << bit)):
                        lines.append(f"  → allocate channel {ch}")
                    else:
                        lines.append(f"  → release channel {ch}")
        return lines
    return _hex_dump(payload)


def _decode_channel_names(payload: bytes, prefix: str) -> list[str]:
    labels = _deserialize_labels(payload)
    if not labels:
        return [f"[no labels decoded]"] + _hex_dump(payload)
    return [f"{prefix} ch {i}: {label!r}" for i, label in enumerate(labels)]


def _decode_router_entries(payload: bytes) -> list[str]:
    count = len(payload) // ROUTER_ENTRY_SIZE
    if count == 0:
        return ["[no entries]"]
    entries = deserialize_router_entries(payload, count)
    return [
        f"  [{i:3d}] {e.dst.id.name}:{e.dst.ch} <- {e.src.id.name}:{e.src.ch}  peak=0x{e.peak:04x}"
        for i, e in enumerate(entries)
    ]


def _hex_dump(payload: bytes, bytes_per_line: int = 16) -> list[str]:
    lines = []
    for offset in range(0, len(payload), bytes_per_line):
        chunk = payload[offset : offset + bytes_per_line]
        hex_part = " ".join(f"{b:02x}" for b in chunk)
        lines.append(f"  {offset:04x}  {hex_part}")
    return lines
