"""TCAT global section protocol."""
import struct
from dataclasses import dataclass, field
from ..constants import ClockSource, ClockRate
from ..codec import pack_u32, unpack_u32, pack_bool, unpack_bool, pack_label, unpack_label

NICKNAME_SIZE = 64
LABEL_COUNT = 13

CLOCK_CAPS_RATE_TABLE = [
    ClockRate.R32000, ClockRate.R44100, ClockRate.R48000, ClockRate.R88200,
    ClockRate.R96000, ClockRate.R176400, ClockRate.R192000,
    ClockRate.AnyLow, ClockRate.AnyMid, ClockRate.AnyHigh, ClockRate.NONE,
]

CLOCK_CAPS_SRC_TABLE = [
    ClockSource.Aes1, ClockSource.Aes2, ClockSource.Aes3, ClockSource.Aes4,
    ClockSource.AesAny, ClockSource.Adat, ClockSource.Tdif, ClockSource.WordClock,
    ClockSource.Arx1, ClockSource.Arx2, ClockSource.Arx3, ClockSource.Arx4,
    ClockSource.Internal,
]

CLOCK_SOURCE_LABEL_TABLE = CLOCK_CAPS_SRC_TABLE

EXTERNAL_CLOCK_SOURCE_TABLE = [
    ClockSource.Aes1, ClockSource.Aes2, ClockSource.Aes3, ClockSource.Aes4,
    ClockSource.Adat, ClockSource.Tdif,
    ClockSource.Arx1, ClockSource.Arx2, ClockSource.Arx3, ClockSource.Arx4,
    ClockSource.WordClock,
]

CLOCK_SOURCE_STREAM_LABEL_TABLE = [
    (ClockSource.Arx1, "Stream-1"),
    (ClockSource.Arx2, "Stream-2"),
    (ClockSource.Arx3, "Stream-3"),
    (ClockSource.Arx4, "Stream-4"),
]

MIN_SIZE = 96
EXT_SIZE = 360


@dataclass
class ClockConfig:
    rate: ClockRate = ClockRate.R48000
    src: ClockSource = ClockSource.Internal

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, ClockConfig):
            return NotImplemented
        return self.rate == other.rate and self.src == other.src


@dataclass
class ClockStatus:
    src_is_locked: bool = False
    rate: ClockRate = ClockRate.R48000

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, ClockStatus):
            return NotImplemented
        return self.src_is_locked == other.src_is_locked and self.rate == other.rate


@dataclass
class ExternalSourceStates:
    sources: list = field(default_factory=list)
    locked: list = field(default_factory=list)
    slipped: list = field(default_factory=list)

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, ExternalSourceStates):
            return NotImplemented
        return (self.sources == other.sources and
                self.locked == other.locked and
                self.slipped == other.slipped)


@dataclass
class GlobalParameters:
    owner: int = 0
    latest_notification: int = 0
    nickname: str = ""
    clock_config: ClockConfig = None  # type: ignore
    enable: bool = False
    clock_status: ClockStatus = None  # type: ignore
    external_source_states: ExternalSourceStates = None  # type: ignore
    current_rate: int = 0
    version: int = 0
    avail_rates: list = field(default_factory=list)
    avail_sources: list = field(default_factory=list)
    clock_source_labels: list = field(default_factory=list)

    def __post_init__(self) -> None:
        if self.clock_config is None:
            self.clock_config = ClockConfig()
        if self.clock_status is None:
            self.clock_status = ClockStatus()
        if self.external_source_states is None:
            self.external_source_states = ExternalSourceStates()


def _from_ne(buf: bytearray) -> bytearray:
    """Convert native-endian to big-endian per quadlet (reverses bytes per quad on LE machines)."""
    for i in range(0, len(buf) // 4 * 4, 4):
        v = struct.unpack("<I", buf[i:i+4])[0]
        buf[i:i+4] = struct.pack(">I", v)
    return buf


def _to_ne(buf: bytearray) -> bytearray:
    """Convert big-endian to native-endian per quadlet (reverses bytes per quad on LE machines)."""
    for i in range(0, len(buf) // 4 * 4, 4):
        v = struct.unpack(">I", buf[i:i+4])[0]
        buf[i:i+4] = struct.pack("<I", v)
    return buf


def _serialize_labels(labels: list[str], size: int = 256) -> bytes:
    """Serialize backslash-separated labels with from_ne byte-swap."""
    buf = bytearray(size)
    pos = 0
    for label in labels:
        encoded = label.encode("ascii", errors="replace")
        end = pos + len(encoded)
        if end + 1 >= size:
            break
        buf[pos:end] = encoded
        buf[end] = 0x5C  # backslash
        pos = end + 1
    if pos < size:
        buf[pos] = 0x5C  # terminating backslash
    _from_ne(buf)
    return bytes(buf)


def _deserialize_labels(raw: bytes) -> list[str]:
    """Deserialize backslash-separated labels with to_ne byte-swap."""
    buf = bytearray(raw)
    _to_ne(buf)
    labels = []
    i = 0
    while i < len(buf):
        end = buf.find(0x5C, i)
        if end == -1 or end == i:  # double-backslash or end = terminator
            break
        chunk = buf[i:end]
        labels.append(chunk.decode("ascii", errors="replace"))
        i = end + 1
    return labels


def _serialize_clock_config(config: ClockConfig) -> bytes:
    # val = (rate << 8) | src, stored big-endian
    val = ((config.rate.value & 0xFF) << 8) | (config.src.value & 0xFF)
    return pack_u32(val)


def _deserialize_clock_config(raw: bytes) -> ClockConfig:
    val = unpack_u32(raw)
    src_byte = val & 0xFF
    rate_byte = (val >> 8) & 0xFF
    return ClockConfig(
        rate=ClockRate.from_byte(rate_byte),
        src=ClockSource.from_byte(src_byte),
    )


def _serialize_clock_status(status: ClockStatus) -> bytes:
    val = 0
    if status.src_is_locked:
        val |= 0x00000001
    val |= (status.rate.value & 0xFF) << 8
    return pack_u32(val)


def _deserialize_clock_status(raw: bytes) -> ClockStatus:
    val = unpack_u32(raw)
    locked = bool(val & 0x00000001)
    rate_byte = (val >> 8) & 0xFF
    return ClockStatus(src_is_locked=locked, rate=ClockRate.from_byte(rate_byte))


def serialize(params: GlobalParameters) -> bytes:
    raw = bytearray(MIN_SIZE)

    hi = (params.owner >> 32) & 0xFFFFFFFF
    lo = params.owner & 0xFFFFFFFF
    raw[0:4] = pack_u32(hi)
    raw[4:8] = pack_u32(lo)
    raw[8:12] = pack_u32(params.latest_notification)

    nick_bytes = pack_label(params.nickname, NICKNAME_SIZE)
    raw[12:76] = nick_bytes

    raw[76:80] = _serialize_clock_config(params.clock_config)
    raw[80:84] = pack_bool(params.enable)
    raw[84:88] = _serialize_clock_status(params.clock_status)

    locked_bits = 0
    slipped_bits = 0
    for i, src in enumerate(EXTERNAL_CLOCK_SOURCE_TABLE):
        if src in params.external_source_states.sources:
            idx = params.external_source_states.sources.index(src)
            if params.external_source_states.locked[idx]:
                locked_bits |= (1 << i)
            if params.external_source_states.slipped[idx]:
                slipped_bits |= (1 << i)
    raw[88:92] = pack_u32((slipped_bits << 16) | locked_bits)
    raw[92:96] = pack_u32(params.current_rate)

    return bytes(raw)


def serialize_extended(params: GlobalParameters) -> bytes:
    """Serialize to full extended 360-byte format."""
    base = bytearray(serialize(params))
    base += bytearray(EXT_SIZE - MIN_SIZE)

    base[96:100] = pack_u32(params.version)

    rate_bits = 0
    for i, rate in enumerate(CLOCK_CAPS_RATE_TABLE):
        if rate in params.avail_rates:
            rate_bits |= (1 << i)

    src_bits = 0
    for i, src in enumerate(CLOCK_CAPS_SRC_TABLE):
        if src in params.avail_sources:
            src_bits |= (1 << i)
        # Also set bit for stream sources that appear in labels
        for s, _ in CLOCK_SOURCE_STREAM_LABEL_TABLE:
            if s == src and any(sl == src for sl, _ in params.clock_source_labels):
                src_bits |= (1 << i)
    base[100:104] = pack_u32((src_bits << 16) | rate_bits)

    # Build ordered label list for CLOCK_SOURCE_LABEL_TABLE
    labels_to_write = []
    for src in CLOCK_SOURCE_LABEL_TABLE:
        label = "Unused"
        for s, l in params.clock_source_labels:
            if s == src:
                label = l
                break
        labels_to_write.append(label)
    base[104:360] = _serialize_labels(labels_to_write, 256)

    return bytes(base)


def deserialize(raw: bytes) -> GlobalParameters:
    params = GlobalParameters()

    extended = len(raw) > MIN_SIZE

    if extended:
        # Parse labels from [104:360] = 256-byte backslash-separated region
        label_strings = _deserialize_labels(raw[104:360])

        # Pad to 13 if needed
        while len(label_strings) < LABEL_COUNT:
            label_strings.append("unused")

        cap_val = unpack_u32(raw[100:104])
        rate_bits = cap_val & 0xFFFF
        src_bits = (cap_val >> 16) & 0xFFFF

        avail_rates = [r for i, r in enumerate(CLOCK_CAPS_RATE_TABLE) if rate_bits & (1 << i)]

        # Build (src, label) pairs
        src_labels = list(zip(CLOCK_SOURCE_LABEL_TABLE, label_strings))

        # For stream sources (Arx*) that have src_bits set, replace label with stream name
        final_src_labels = []
        for src, lbl in src_labels:
            i = CLOCK_CAPS_SRC_TABLE.index(src)
            if src_bits & (1 << i):
                stream_name = next((n for s, n in CLOCK_SOURCE_STREAM_LABEL_TABLE if s == src), None)
                if stream_name:
                    final_src_labels.append((src, stream_name))
                    continue
            final_src_labels.append((src, lbl))

        # Available sources: bits set AND label not "unused", and not stream-only
        avail_sources = []
        for i, src in enumerate(CLOCK_CAPS_SRC_TABLE):
            if not (src_bits & (1 << i)):
                continue
            if any(s == src for s, _ in CLOCK_SOURCE_STREAM_LABEL_TABLE):
                continue
            lbl = next((l for s, l in final_src_labels if s == src), "unused")
            if lbl.lower() != "unused":
                avail_sources.append(src)

        # Final clock_source_labels: only relevant entries
        clock_source_labels = [
            (src, lbl) for src, lbl in final_src_labels
            if lbl.lower() not in ("unused",) and (
                any(s == src for s, _ in CLOCK_SOURCE_STREAM_LABEL_TABLE) or
                src in avail_sources
            )
        ]

        params.version = unpack_u32(raw[96:100])
        params.avail_rates = avail_rates
        params.avail_sources = avail_sources
        params.clock_source_labels = clock_source_labels
    else:
        params.version = 0
        params.avail_rates = [ClockRate.R44100, ClockRate.R48000]
        params.avail_sources = [ClockSource.Internal]
        params.clock_source_labels = [
            (ClockSource.Arx1, "Stream-1"),
            (ClockSource.Internal, "internal"),
        ]

    params.owner = (unpack_u32(raw[0:4]) << 32) | unpack_u32(raw[4:8])
    params.latest_notification = unpack_u32(raw[8:12])
    params.nickname = unpack_label(raw[12:76])
    params.clock_config = _deserialize_clock_config(raw[76:80])
    params.enable = unpack_bool(raw[80:84])
    params.clock_status = _deserialize_clock_status(raw[84:88])

    ext_val = unpack_u32(raw[88:92])
    locked_bits = ext_val & 0xFFFF
    slipped_bits = (ext_val >> 16) & 0xFFFF

    src_labels_for_ext = params.clock_source_labels
    srcs = [
        src for src in EXTERNAL_CLOCK_SOURCE_TABLE
        if any(s == src for s, _ in src_labels_for_ext)
    ]
    locked = [bool(locked_bits & (1 << EXTERNAL_CLOCK_SOURCE_TABLE.index(src))) for src in srcs]
    slipped = [bool(slipped_bits & (1 << EXTERNAL_CLOCK_SOURCE_TABLE.index(src))) for src in srcs]

    params.external_source_states = ExternalSourceStates(
        sources=srcs, locked=locked, slipped=slipped
    )
    params.current_rate = unpack_u32(raw[92:96])

    return params
