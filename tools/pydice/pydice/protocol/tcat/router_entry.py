"""TCAT router entry protocol — SrcBlk, DstBlk, RouterEntry serialization."""
from dataclasses import dataclass
from ..constants import SrcBlkId, DstBlkId
from ..codec import pack_u32, unpack_u32


@dataclass
class SrcBlk:
    id: SrcBlkId = SrcBlkId.Mute
    ch: int = 0

    def serialize_byte(self) -> int:
        return ((self.id.value << 4) & 0xF0) | (self.ch & 0x0F)

    @classmethod
    def deserialize_byte(cls, val: int) -> "SrcBlk":
        id_nibble = (val & 0xF0) >> 4
        ch = val & 0x0F
        return cls(id=SrcBlkId.from_nibble(id_nibble), ch=ch)

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, SrcBlk):
            return NotImplemented
        return self.id == other.id and self.ch == other.ch

    def __lt__(self, other: "SrcBlk") -> bool:
        return self.serialize_byte() < other.serialize_byte()


@dataclass
class DstBlk:
    id: DstBlkId = DstBlkId.Aes
    ch: int = 0

    def serialize_byte(self) -> int:
        return ((self.id.value << 4) & 0xF0) | (self.ch & 0x0F)

    @classmethod
    def deserialize_byte(cls, val: int) -> "DstBlk":
        id_nibble = (val & 0xF0) >> 4
        ch = val & 0x0F
        return cls(id=DstBlkId.from_nibble(id_nibble), ch=ch)

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, DstBlk):
            return NotImplemented
        return self.id == other.id and self.ch == other.ch

    def __lt__(self, other: "DstBlk") -> bool:
        return self.serialize_byte() < other.serialize_byte()


@dataclass
class RouterEntry:
    dst: DstBlk = None  # type: ignore
    src: SrcBlk = None  # type: ignore
    peak: int = 0

    def __post_init__(self) -> None:
        if self.dst is None:
            self.dst = DstBlk()
        if self.src is None:
            self.src = SrcBlk()

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, RouterEntry):
            return NotImplemented
        return self.dst == other.dst and self.src == other.src and self.peak == other.peak


ROUTER_ENTRY_SIZE = 4


def serialize_router_entry(entry: RouterEntry) -> bytes:
    dst_val = entry.dst.serialize_byte()
    src_val = entry.src.serialize_byte()
    val = (
        ((entry.peak & 0xFFFF) << 16)
        | ((src_val & 0xFF) << 8)
        | (dst_val & 0xFF)
    )
    return pack_u32(val)


def deserialize_router_entry(raw: bytes) -> RouterEntry:
    val = unpack_u32(raw)
    dst_byte = (val & 0x000000FF) >> 0
    src_byte = (val & 0x0000FF00) >> 8
    peak = (val & 0xFFFF0000) >> 16
    return RouterEntry(
        dst=DstBlk.deserialize_byte(dst_byte),
        src=SrcBlk.deserialize_byte(src_byte),
        peak=peak,
    )


def serialize_router_entries(entries: list[RouterEntry]) -> bytes:
    return b"".join(serialize_router_entry(e) for e in entries)


def deserialize_router_entries(raw: bytes, count: int) -> list[RouterEntry]:
    entries = []
    for i in range(count):
        offset = i * ROUTER_ENTRY_SIZE
        entries.append(deserialize_router_entry(raw[offset : offset + ROUTER_ENTRY_SIZE]))
    return entries
