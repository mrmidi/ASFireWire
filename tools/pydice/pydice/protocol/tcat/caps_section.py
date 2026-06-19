"""TCAT extension capabilities section."""
from dataclasses import dataclass
from enum import Enum
from ..codec import pack_u32, unpack_u32


class AsicType(Enum):
    DiceII = 0
    Tcd2210 = 1
    Tcd2220 = 2


@dataclass
class RouterCaps:
    is_exposed: bool = False
    is_readonly: bool = False
    is_storable: bool = False
    maximum_entry_count: int = 0

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, RouterCaps):
            return NotImplemented
        return (self.is_exposed == other.is_exposed and
                self.is_readonly == other.is_readonly and
                self.is_storable == other.is_storable and
                self.maximum_entry_count == other.maximum_entry_count)


@dataclass
class MixerCaps:
    is_exposed: bool = False
    is_readonly: bool = False
    is_storable: bool = False
    input_device_id: int = 0
    output_device_id: int = 0
    input_count: int = 0
    output_count: int = 0

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, MixerCaps):
            return NotImplemented
        return (self.is_exposed == other.is_exposed and
                self.is_readonly == other.is_readonly and
                self.is_storable == other.is_storable and
                self.input_device_id == other.input_device_id and
                self.output_device_id == other.output_device_id and
                self.input_count == other.input_count and
                self.output_count == other.output_count)


@dataclass
class GeneralCaps:
    dynamic_stream_format: bool = False
    storage_avail: bool = False
    peak_avail: bool = False
    max_tx_streams: int = 0
    max_rx_streams: int = 0
    stream_format_is_storable: bool = False
    asic_type: AsicType = AsicType.DiceII

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, GeneralCaps):
            return NotImplemented
        return (self.dynamic_stream_format == other.dynamic_stream_format and
                self.storage_avail == other.storage_avail and
                self.peak_avail == other.peak_avail and
                self.max_tx_streams == other.max_tx_streams and
                self.max_rx_streams == other.max_rx_streams and
                self.stream_format_is_storable == other.stream_format_is_storable and
                self.asic_type == other.asic_type)


@dataclass
class ExtensionCaps:
    router: RouterCaps = None  # type: ignore
    mixer: MixerCaps = None  # type: ignore
    general: GeneralCaps = None  # type: ignore

    def __post_init__(self) -> None:
        if self.router is None:
            self.router = RouterCaps()
        if self.mixer is None:
            self.mixer = MixerCaps()
        if self.general is None:
            self.general = GeneralCaps()

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, ExtensionCaps):
            return NotImplemented
        return (self.router == other.router and
                self.mixer == other.mixer and
                self.general == other.general)


SIZE = 12  # 3 × 4 bytes


def serialize_router_caps(caps: RouterCaps) -> bytes:
    val = 0
    if caps.is_exposed:
        val |= 0x00000001
    if caps.is_readonly:
        val |= 0x00000002
    if caps.is_storable:
        val |= 0x00000004
    val |= (caps.maximum_entry_count & 0xFFFF) << 16
    return pack_u32(val)


def deserialize_router_caps(raw: bytes) -> RouterCaps:
    val = unpack_u32(raw)
    return RouterCaps(
        is_exposed=bool(val & 0x00000001),
        is_readonly=bool(val & 0x00000002),
        is_storable=bool(val & 0x00000004),
        maximum_entry_count=(val >> 16) & 0xFFFF,
    )


def serialize_mixer_caps(caps: MixerCaps) -> bytes:
    val = 0
    if caps.is_exposed:
        val |= 0x00000001
    if caps.is_readonly:
        val |= 0x00000002
    if caps.is_storable:
        val |= 0x00000004
    val |= (caps.input_device_id & 0x0F) << 4
    val |= (caps.output_device_id & 0x0F) << 8
    val |= (caps.input_count & 0xFF) << 16
    val |= (caps.output_count & 0xFF) << 24
    return pack_u32(val)


def deserialize_mixer_caps(raw: bytes) -> MixerCaps:
    val = unpack_u32(raw)
    return MixerCaps(
        is_exposed=bool(val & 0x00000001),
        is_readonly=bool(val & 0x00000002),
        is_storable=bool(val & 0x00000004),
        input_device_id=(val >> 4) & 0x0F,
        output_device_id=(val >> 8) & 0x0F,
        input_count=(val >> 16) & 0xFF,
        output_count=(val >> 24) & 0xFF,
    )


def serialize_general_caps(caps: GeneralCaps) -> bytes:
    val = 0
    if caps.dynamic_stream_format:
        val |= 0x00000001
    if caps.storage_avail:
        val |= 0x00000002
    if caps.peak_avail:
        val |= 0x00000004
    val |= (caps.max_tx_streams & 0x0F) << 4
    val |= (caps.max_rx_streams & 0x0F) << 8
    if caps.stream_format_is_storable:
        val |= 0x00001000
    asic_val = caps.asic_type.value
    val |= (asic_val & 0xFFFF) << 16
    return pack_u32(val)


def deserialize_general_caps(raw: bytes) -> GeneralCaps:
    val = unpack_u32(raw)
    asic_v = (val >> 16) & 0xFFFF
    try:
        asic = AsicType(asic_v)
    except ValueError:
        raise ValueError(f"Unknown ASIC type: {asic_v}")
    return GeneralCaps(
        dynamic_stream_format=bool(val & 0x00000001),
        storage_avail=bool(val & 0x00000002),
        peak_avail=bool(val & 0x00000004),
        max_tx_streams=(val >> 4) & 0x0F,
        max_rx_streams=(val >> 8) & 0x0F,
        stream_format_is_storable=bool(val & 0x00001000),
        asic_type=asic,
    )


def serialize(caps: ExtensionCaps) -> bytes:
    return (
        serialize_router_caps(caps.router)
        + serialize_mixer_caps(caps.mixer)
        + serialize_general_caps(caps.general)
    )


def deserialize(raw: bytes) -> ExtensionCaps:
    assert len(raw) >= SIZE
    return ExtensionCaps(
        router=deserialize_router_caps(raw[0:4]),
        mixer=deserialize_mixer_caps(raw[4:8]),
        general=deserialize_general_caps(raw[8:12]),
    )
