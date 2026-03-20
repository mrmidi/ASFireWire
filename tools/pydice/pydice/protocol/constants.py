"""Shared DICE protocol constants and enumerations."""
from enum import Enum


class ClockRate(Enum):
    R32000 = 0x00
    R44100 = 0x01
    R48000 = 0x02
    R88200 = 0x03
    R96000 = 0x04
    R176400 = 0x05
    R192000 = 0x06
    AnyLow = 0x07
    AnyMid = 0x08
    AnyHigh = 0x09
    NONE = 0x0A

    @classmethod
    def from_byte(cls, v: int) -> "ClockRate":
        for m in cls:
            if m.value == v:
                return m
        return cls.NONE


class ClockSource(Enum):
    Aes1 = 0x00
    Aes2 = 0x01
    Aes3 = 0x02
    Aes4 = 0x03
    AesAny = 0x04
    Adat = 0x05
    Tdif = 0x06
    WordClock = 0x07
    Arx1 = 0x08
    Arx2 = 0x09
    Arx3 = 0x0A
    Arx4 = 0x0B
    Internal = 0x0C

    @classmethod
    def from_byte(cls, v: int) -> "ClockSource":
        for m in cls:
            if m.value == v:
                return m
        raise ValueError(f"Unknown ClockSource byte: {v:#04x}")


class SrcBlkId(Enum):
    Aes = 0
    Adat = 1
    Mixer = 2
    Ins0 = 4
    Ins1 = 5
    ArmAprAudio = 10
    Avs0 = 11
    Avs1 = 12
    Mute = 15

    @classmethod
    def from_nibble(cls, v: int) -> "SrcBlkId":
        for m in cls:
            if m.value == v:
                return m
        raise ValueError(f"Unknown SrcBlkId nibble: {v}")


class DstBlkId(Enum):
    Aes = 0
    Adat = 1
    MixerTx0 = 2
    MixerTx1 = 3
    Ins0 = 4
    Ins1 = 5
    ArmApbAudio = 10
    Avs0 = 11
    Avs1 = 12

    @classmethod
    def from_nibble(cls, v: int) -> "DstBlkId":
        for m in cls:
            if m.value == v:
                return m
        raise ValueError(f"Unknown DstBlkId nibble: {v}")


# Saffire PRO 24 DSP application section offsets (relative to app section base 0x6DD4)
FW_BASE = 0xFFFFF0000000
APP_SECTION_BASE = 0x6DD4

SW_NOTICE_REG = 0x05EC

OUTPUT_GROUP_BASE = 0x000C
INPUT_PARAMS_OFFSET = 0x0058
DSP_ENABLE_OFFSET = 0x0070
CH_STRIP_FLAG_OFFSET = 0x0078

COEF_OFFSET = 0x0190
COEF_BLOCK_SIZE = 0x88

# Software notice values
SW_NOTICE_SRC = 0x00000001
SW_NOTICE_DIM_MUTE = 0x00000002
CH_STRIP_FLAG_SW_NOTICE = 0x00000005
COMP_CH0_SW_NOTICE = 0x00000006
COMP_CH1_SW_NOTICE = 0x00000007
EQ_OUTPUT_CH0_SW_NOTICE = 0x09
EQ_OUTPUT_CH1_SW_NOTICE = 0x0A
EQ_LOW_FREQ_CH0_SW_NOTICE = 0x0C
EQ_LOW_FREQ_CH1_SW_NOTICE = 0x0D
EQ_LOW_MIDDLE_FREQ_CH0_SW_NOTICE = 0x0F
EQ_LOW_MIDDLE_FREQ_CH1_SW_NOTICE = 0x10
EQ_HIGH_MIDDLE_FREQ_CH0_SW_NOTICE = 0x12
EQ_HIGH_MIDDLE_FREQ_CH1_SW_NOTICE = 0x13
EQ_HIGH_FREQ_CH0_SW_NOTICE = 0x15
EQ_HIGH_FREQ_CH1_SW_NOTICE = 0x16
REVERB_SW_NOTICE = 0x0000001A
DSP_ENABLE_SW_NOTICE = 0x1C
