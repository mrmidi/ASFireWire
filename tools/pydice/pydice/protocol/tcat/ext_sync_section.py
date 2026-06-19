"""TCAT extended synchronization section (read-only)."""
from dataclasses import dataclass
from typing import Optional
from ..constants import ClockSource, ClockRate
from ..codec import unpack_u32


@dataclass
class ExtendedSyncParameters:
    clk_src: ClockSource = ClockSource.Internal
    clk_src_locked: bool = False
    clk_rate: ClockRate = ClockRate.R48000
    adat_user_data: Optional[int] = None

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, ExtendedSyncParameters):
            return NotImplemented
        return (self.clk_src == other.clk_src and
                self.clk_src_locked == other.clk_src_locked and
                self.clk_rate == other.clk_rate and
                self.adat_user_data == other.adat_user_data)


ADAT_USER_DATA_MASK = 0x0F
ADAT_USER_DATA_UNAVAIL = 0x10
MIN_SIZE = 16


def serialize(params: ExtendedSyncParameters) -> bytes:
    # All fields are read-only; no-op
    return b"\x00" * MIN_SIZE


def deserialize(raw: bytes) -> ExtendedSyncParameters:
    assert len(raw) >= MIN_SIZE

    # In Rust, deserialize_u8 reads the LSB of a big-endian quadlet
    src_byte = unpack_u32(raw[0:4]) & 0xFF
    clk_src = ClockSource.from_byte(src_byte)

    locked_val = unpack_u32(raw[4:8])
    clk_src_locked = locked_val > 0

    rate_byte = unpack_u32(raw[8:12]) & 0xFF
    clk_rate = ClockRate.from_byte(rate_byte)

    adat_val = unpack_u32(raw[12:16])
    if adat_val & ADAT_USER_DATA_UNAVAIL:
        adat_user_data = None
    else:
        adat_user_data = adat_val & ADAT_USER_DATA_MASK

    return ExtendedSyncParameters(
        clk_src=clk_src,
        clk_src_locked=clk_src_locked,
        clk_rate=clk_rate,
        adat_user_data=adat_user_data,
    )
