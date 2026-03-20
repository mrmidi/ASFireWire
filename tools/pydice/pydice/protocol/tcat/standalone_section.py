"""TCAT standalone configuration section."""
from dataclasses import dataclass
from enum import Enum
from ..constants import ClockSource, ClockRate
from ..codec import pack_u32, unpack_u32, pack_bool, unpack_bool

MIN_SIZE = 20


class AdatParam(Enum):
    Normal = 0
    SMUX2 = 1
    SMUX4 = 2
    Auto = 3


class WordClockMode(Enum):
    Normal = 0
    Low = 1
    Middle = 2
    High = 3


@dataclass
class WordClockRate:
    numerator: int = 1
    denominator: int = 1

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, WordClockRate):
            return NotImplemented
        return self.numerator == other.numerator and self.denominator == other.denominator


@dataclass
class WordClockParam:
    mode: WordClockMode = WordClockMode.Normal
    rate: WordClockRate = None  # type: ignore

    def __post_init__(self) -> None:
        if self.rate is None:
            self.rate = WordClockRate()

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, WordClockParam):
            return NotImplemented
        return self.mode == other.mode and self.rate == other.rate


@dataclass
class StandaloneParameters:
    clock_source: ClockSource = ClockSource.Internal
    aes_high_rate: bool = False
    adat_mode: AdatParam = AdatParam.Auto
    word_clock_param: WordClockParam = None  # type: ignore
    internal_rate: ClockRate = ClockRate.R48000

    def __post_init__(self) -> None:
        if self.word_clock_param is None:
            self.word_clock_param = WordClockParam()

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, StandaloneParameters):
            return NotImplemented
        return (self.clock_source == other.clock_source and
                self.aes_high_rate == other.aes_high_rate and
                self.adat_mode == other.adat_mode and
                self.word_clock_param == other.word_clock_param and
                self.internal_rate == other.internal_rate)


def serialize(params: StandaloneParameters) -> bytes:
    raw = bytearray(MIN_SIZE)

    # clock source as u8 in LSB of big-endian quadlet
    raw[0:4] = pack_u32(params.clock_source.value & 0xFF)

    # aes_high_rate as bool quadlet
    raw[4:8] = pack_bool(params.aes_high_rate)

    # adat mode
    adat_val = params.adat_mode.value
    raw[8:12] = pack_u32(adat_val)

    # word clock
    if params.word_clock_param.rate.numerator < 1 or params.word_clock_param.rate.denominator < 1:
        raise ValueError(
            f"Invalid word clock rate: {params.word_clock_param.rate.numerator}/"
            f"{params.word_clock_param.rate.denominator}"
        )
    wc_val = params.word_clock_param.mode.value & 0x03
    wc_val |= ((params.word_clock_param.rate.numerator - 1) & 0x0FFF) << 4
    wc_val |= ((params.word_clock_param.rate.denominator - 1) & 0xFFFF) << 16
    raw[12:16] = pack_u32(wc_val)

    # internal rate as u8 in LSB of big-endian quadlet
    raw[16:20] = pack_u32(params.internal_rate.value & 0xFF)

    return bytes(raw)


def deserialize(raw: bytes) -> StandaloneParameters:
    assert len(raw) >= MIN_SIZE

    src_byte = unpack_u32(raw[0:4]) & 0xFF
    clock_source = ClockSource.from_byte(src_byte)

    aes_high_rate = unpack_bool(raw[4:8])

    adat_val = unpack_u32(raw[8:12])
    adat_map = {0: AdatParam.Normal, 1: AdatParam.SMUX2, 2: AdatParam.SMUX4, 3: AdatParam.Auto}
    adat_mode = adat_map.get(adat_val, AdatParam.Normal)

    wc_val = unpack_u32(raw[12:16])
    mode_map = {0: WordClockMode.Normal, 1: WordClockMode.Low, 2: WordClockMode.Middle, 3: WordClockMode.High}
    wc_mode = mode_map.get(wc_val & 0x03, WordClockMode.Normal)
    wc_num = 1 + ((wc_val >> 4) & 0x0FFF)
    wc_den = 1 + ((wc_val >> 16) & 0xFFFF)

    rate_byte = unpack_u32(raw[16:20]) & 0xFF
    internal_rate = ClockRate.from_byte(rate_byte)

    return StandaloneParameters(
        clock_source=clock_source,
        aes_high_rate=aes_high_rate,
        adat_mode=adat_mode,
        word_clock_param=WordClockParam(
            mode=wc_mode,
            rate=WordClockRate(numerator=wc_num, denominator=wc_den),
        ),
        internal_rate=internal_rate,
    )
