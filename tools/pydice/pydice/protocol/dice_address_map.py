"""DICE/TCAT address annotation: address + value → (register_name, decoded_string)."""
import struct
from typing import Callable

from .tcat.global_section import _deserialize_clock_config, _deserialize_clock_status


def annotate(address: str | None, value: int | None) -> tuple[str, str]:
    """Return (register_name, decoded_value_string) for a FireWire address."""
    if not address:
        return ("", "")
    parts = address.split(".")
    # Region is everything after the node part: "ffff.e000.0074"
    region = ".".join(parts[1:]) if len(parts) >= 2 else address
    return _lookup(region, value)


def _val_bytes(value: int) -> bytes:
    return struct.pack(">I", value & 0xFFFFFFFF)


def _decode_clock_config(value: int) -> str:
    try:
        cfg = _deserialize_clock_config(_val_bytes(value))
        return f"{cfg.rate.name}, {cfg.src.name}"
    except Exception:
        return f"0x{value:08x}"


def _decode_clock_status(value: int) -> str:
    try:
        st = _deserialize_clock_status(_val_bytes(value))
        return f"locked={st.src_is_locked}, {st.rate.name}"
    except Exception:
        return f"0x{value:08x}"


def _decode_bool(value: int) -> str:
    return "True" if value else "False"


def _decode_rate(value: int) -> str:
    return f"{value} Hz"


def _decode_decimal(value: int) -> str:
    return str(value)


def _decode_hex(value: int) -> str:
    return f"0x{value:08x}"


def _decode_configrom(region: str, value: int | None) -> str:
    if value is None:
        return ""
    raw = _val_bytes(value)
    try:
        if all(0x20 <= b < 0x7F or b == 0 for b in raw):
            text = raw.decode("ascii").rstrip("\x00")
            if text:
                return f"0x{value:08x} ({text!r})"
    except Exception:
        pass
    return f"0x{value:08x}"


def _decode_iso_channel(value: int) -> str:
    if value == 0xFFFFFFFF:
        return "unused (-1)"
    return f"channel {value}"


def _decode_speed(value: int) -> str:
    return {0: "s100", 1: "s200", 2: "s400", 3: "s800"}.get(
        value, f"0x{value:x} [INVESTIGATE]"
    )


def _decode_quadlets_offset(value: int) -> str:
    return f"{value} quadlets ({value * 4:#x} bytes)"


def _decode_owner(value: int) -> str:
    if value == 0xFFFF0000:
        return "No owner"
    return f"node 0x{value >> 16:04x}"


_NOTIFY_BITS = [
    (0x00000001, "RX_CFG_CHG"),
    (0x00000002, "TX_CFG_CHG"),
    (0x00000010, "LOCK_CHG"),
    (0x00000020, "CLOCK_ACCEPTED"),
    (0x00000040, "EXT_STATUS"),
]


def _decode_notify_bits(value: int) -> str:
    bits = [name for mask, name in _NOTIFY_BITS if value & mask]
    return "|".join(bits) if bits else f"0x{value:08x}"


_EXT_STATUS_BITS = [
    (1 << 0,  "AES1_LOCKED"), (1 << 1,  "AES2_LOCKED"),
    (1 << 2,  "AES3_LOCKED"), (1 << 3,  "AES4_LOCKED"),
    (1 << 4,  "ADAT_LOCKED"), (1 << 5,  "TDIF_LOCKED"),
    (1 << 6,  "ARX1_LOCKED"), (1 << 7,  "ARX2_LOCKED"),
    (1 << 8,  "ARX3_LOCKED"), (1 << 9,  "ARX4_LOCKED"),
    (1 << 16, "AES1_SLIP"),   (1 << 17, "AES2_SLIP"),
    (1 << 18, "AES3_SLIP"),   (1 << 19, "AES4_SLIP"),
    (1 << 20, "ADAT_SLIP"),   (1 << 21, "TDIF_SLIP"),
    (1 << 22, "ARX1_SLIP"),   (1 << 23, "ARX2_SLIP"),
    (1 << 24, "ARX3_SLIP"),   (1 << 25, "ARX4_SLIP"),
]


def _decode_ext_status(value: int) -> str:
    bits = [name for mask, name in _EXT_STATUS_BITS if value & mask]
    return "|".join(bits) if bits else f"0x{value:08x}"


_CLOCK_RATE_BITS = [
    (0x0001, "R32000"), (0x0002, "R44100"), (0x0004, "R48000"),
    (0x0008, "R88200"), (0x0010, "R96000"), (0x0020, "R176400"), (0x0040, "R192000"),
]
_CLOCK_SRC_BITS = [
    (0x0001, "Aes1"),  (0x0002, "Aes2"),  (0x0004, "Aes3"),  (0x0008, "Aes4"),
    (0x0010, "AesAny"), (0x0020, "Adat"), (0x0040, "Tdif"),  (0x0080, "WordClock"),
    (0x0100, "Arx1"),  (0x0200, "Arx2"),  (0x0400, "Arx3"),  (0x0800, "Arx4"),
    (0x1000, "Internal"),
]


def _decode_clock_caps(value: int) -> str:
    rate_bits = value & 0xFFFF
    src_bits = (value >> 16) & 0xFFFF
    rates = [name for mask, name in _CLOCK_RATE_BITS if rate_bits & mask]
    srcs = [name for mask, name in _CLOCK_SRC_BITS if src_bits & mask]
    parts = []
    if rates:
        parts.append("rates=[" + ",".join(rates) + "]")
    if srcs:
        parts.append("srcs=[" + ",".join(srcs) + "]")
    return " ".join(parts) if parts else f"0x{value:08x}"


# Register map: region string → (name, decoder_fn | None)
_DICE_REGS: dict[str, tuple[str, Callable[[int], str] | None]] = {
    # Section layout header (10 quadlets at e000.0000)
    "ffff.e000.0000": ("DICE_GLOBAL_OFFSET",              _decode_quadlets_offset),
    "ffff.e000.0004": ("DICE_GLOBAL_SIZE",                _decode_quadlets_offset),
    "ffff.e000.0008": ("DICE_TX_OFFSET",                  _decode_quadlets_offset),
    "ffff.e000.000c": ("DICE_TX_SIZE",                    _decode_quadlets_offset),
    "ffff.e000.0010": ("DICE_RX_OFFSET",                  _decode_quadlets_offset),
    "ffff.e000.0014": ("DICE_RX_SIZE",                    _decode_quadlets_offset),
    "ffff.e000.0018": ("DICE_EXT_SYNC_OFFSET",            _decode_quadlets_offset),
    "ffff.e000.001c": ("DICE_EXT_SYNC_SIZE",              _decode_quadlets_offset),
    # Global section (base 0x0028)
    "ffff.e000.0028": ("GLOBAL_OWNER",                    _decode_owner),
    "ffff.e000.002c": ("GLOBAL_OWNER +4",                 _decode_hex),
    "ffff.e000.0030": ("GLOBAL_NOTIFICATION",             _decode_notify_bits),
    "ffff.e000.0034": ("GLOBAL_NICK_NAME",                None),
    "ffff.e000.0074": ("GLOBAL_CLOCK_SELECT",             _decode_clock_config),
    "ffff.e000.0078": ("GLOBAL_ENABLE",                   _decode_bool),
    "ffff.e000.007c": ("GLOBAL_STATUS",                   _decode_clock_status),
    "ffff.e000.0080": ("GLOBAL_EXTENDED_STATUS",          _decode_ext_status),
    "ffff.e000.0084": ("GLOBAL_SAMPLE_RATE",              _decode_rate),
    "ffff.e000.0088": ("GLOBAL_VERSION",                  _decode_hex),
    "ffff.e000.008c": ("GLOBAL_CLOCK_CAPABILITIES",       _decode_clock_caps),
    "ffff.e000.0090": ("GLOBAL_CLOCK_SOURCE_NAMES",       None),
    # TX section (base 0x01a4)
    "ffff.e000.01a4": ("TX_NUMBER",                       _decode_decimal),
    "ffff.e000.01a8": ("TX_SIZE",                         _decode_quadlets_offset),
    "ffff.e000.01ac": ("TX[0] ISOCHRONOUS channel",       _decode_iso_channel),
    "ffff.e000.01b0": ("TX[0] audio channels",            _decode_decimal),
    "ffff.e000.01b4": ("TX[0] MIDI ports",                _decode_decimal),
    "ffff.e000.01b8": ("TX[0] speed",                     _decode_speed),
    "ffff.e000.01bc": ("TX[0] channel names",             None),
    # RX section (base 0x03dc)
    "ffff.e000.03dc": ("RX_NUMBER",                       _decode_decimal),
    "ffff.e000.03e0": ("RX_SIZE",                         _decode_quadlets_offset),
    "ffff.e000.03e4": ("RX[0] ISOCHRONOUS channel",       _decode_iso_channel),
    "ffff.e000.03e8": ("RX[0] seq start",                 _decode_decimal),
    "ffff.e000.03ec": ("RX[0] audio channels",            _decode_decimal),
    "ffff.e000.03f0": ("RX[0] MIDI ports",                _decode_decimal),
    "ffff.e000.03f4": ("RX[0] channel names",             None),
    # TCAT extension (e020.xxxx — partially undocumented)
    "ffff.e020.0000": ("TCAT ext section header",         None),
    "ffff.e020.005c": ("TCAT TX[0] ISO channel (ext)",    _decode_iso_channel),
    "ffff.e020.0060": ("TCAT RX[0] ISO channel (ext)",    _decode_iso_channel),
    "ffff.e020.06e8": ("TCAT router entries",             None),
    "ffff.e020.0d24": ("TCAT playlist count",             _decode_decimal),
    "ffff.e020.0d28": ("TCAT playlist descriptors [INVESTIGATE]", None),
    "ffff.e020.6d70": ("TCAT mixer/routing state [INVESTIGATE]",  None),
    "ffff.e020.7350": ("[INVESTIGATE e020.7350]",         _decode_hex),
    # IRM (Isochronous Resource Manager) — IEEE 1394 standard registers
    "ffff.f000.0220": ("IRM_BANDWIDTH_AVAILABLE",         _decode_decimal),
    "ffff.f000.0224": ("IRM_CHANNELS_AVAILABLE_HI",       _decode_hex),
    "ffff.f000.0228": ("IRM_CHANNELS_AVAILABLE_LO",       _decode_hex),
    # Misc
    "00ff.0000.d1cc": ("SW notify latch",                 _decode_hex),
    "0001.0000.0000": ("FW notification address",         _decode_hex),
}


def _lookup(region: str, value: int | None) -> tuple[str, str]:
    # Config ROM: ffff.f000.04XX (offset 0x0000 ... 0x03FF from base 0xf0000400)
    if region.startswith("ffff.f000.04"):
        try:
            last_hex = region.split(".")[-1]
            offset = int(last_hex, 16) - 0x0400
            name = f"ConfigROM +0x{offset:03x}"
            decoded = _decode_configrom(region, value)
        except Exception:
            name = f"ConfigROM ({region})"
            decoded = f"0x{value:08x}" if value is not None else ""
        return (name, decoded)

    if region in _DICE_REGS:
        name, decoder = _DICE_REGS[region]
        if decoder is not None and value is not None:
            decoded = decoder(value)
        elif value is not None:
            decoded = f"0x{value:08x}"
        else:
            decoded = ""
        return (name, decoded)

    # Unknown address — show region as name, hex value
    val_str = f"0x{value:08x}" if value is not None else ""
    return (region, val_str)
