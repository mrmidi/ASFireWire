"""Focusrite Saffire Pro 24 DSP protocol implementation."""
from dataclasses import dataclass, field
from ..codec import pack_f32, unpack_f32, pack_u32, unpack_u32
from ..constants import (
    COEF_OFFSET, COEF_BLOCK_SIZE,
    CH_STRIP_FLAG_OFFSET, CH_STRIP_FLAG_SW_NOTICE,
    COMP_CH0_SW_NOTICE, COMP_CH1_SW_NOTICE,
    EQ_OUTPUT_CH0_SW_NOTICE, EQ_OUTPUT_CH1_SW_NOTICE,
    EQ_LOW_FREQ_CH0_SW_NOTICE, EQ_LOW_FREQ_CH1_SW_NOTICE,
    EQ_LOW_MIDDLE_FREQ_CH0_SW_NOTICE, EQ_LOW_MIDDLE_FREQ_CH1_SW_NOTICE,
    EQ_HIGH_MIDDLE_FREQ_CH0_SW_NOTICE, EQ_HIGH_MIDDLE_FREQ_CH1_SW_NOTICE,
    EQ_HIGH_FREQ_CH0_SW_NOTICE, EQ_HIGH_FREQ_CH1_SW_NOTICE,
    REVERB_SW_NOTICE,
    OUTPUT_GROUP_BASE, SW_NOTICE_SRC, SW_NOTICE_DIM_MUTE,
)
from ..command import FireWireCommand

# Channel strip flag bits
CH_STRIP_FLAG_EQ_ENABLE: int = 0x0001
CH_STRIP_FLAG_COMP_ENABLE: int = 0x0002
CH_STRIP_FLAG_EQ_AFTER_COMP: int = 0x0004

# Compressor offsets within a coefficient block
COMP_OUTPUT_OFFSET = 0x04
COMP_THRESHOLD_OFFSET = 0x08
COMP_RATIO_OFFSET = 0x0C
COMP_ATTACK_OFFSET = 0x10
COMP_RELEASE_OFFSET = 0x14

# Equalizer offsets within a coefficient block
EQ_OUTPUT_OFFSET = 0x18
EQ_LOW_FREQ_OFFSET = 0x20

# Reverb offsets within a coefficient block
REVERB_SIZE_OFFSET = 0x70
REVERB_AIR_OFFSET = 0x74
REVERB_ENABLE_OFFSET = 0x78
REVERB_DISABLE_OFFSET = 0x7C
REVERB_PRE_FILTER_VALUE_OFFSET = 0x80
REVERB_PRE_FILTER_SIGN_OFFSET = 0x84

# Block indices
COEF_BLOCK_COMP = 2
COEF_BLOCK_EQ = 2
COEF_BLOCK_REVERB = 3

EQ_COEF_COUNT = 5


@dataclass
class Spro24DspCompressorState:
    output: list = field(default_factory=lambda: [0.0, 0.0])
    threshold: list = field(default_factory=lambda: [0.0, 0.0])
    ratio: list = field(default_factory=lambda: [0.0, 0.0])
    attack: list = field(default_factory=lambda: [0.0, 0.0])
    release: list = field(default_factory=lambda: [0.0, 0.0])

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Spro24DspCompressorState):
            return NotImplemented
        return (self.output == other.output and
                self.threshold == other.threshold and
                self.ratio == other.ratio and
                self.attack == other.attack and
                self.release == other.release)


@dataclass
class Spro24DspEqualizerFrequencyBandState:
    coefs: list = field(default_factory=lambda: [0.0] * EQ_COEF_COUNT)

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Spro24DspEqualizerFrequencyBandState):
            return NotImplemented
        return self.coefs == other.coefs


@dataclass
class Spro24DspEqualizerState:
    output: list = field(default_factory=lambda: [0.0, 0.0])
    low_coef: list = field(default_factory=lambda: [
        Spro24DspEqualizerFrequencyBandState(),
        Spro24DspEqualizerFrequencyBandState(),
    ])
    low_middle_coef: list = field(default_factory=lambda: [
        Spro24DspEqualizerFrequencyBandState(),
        Spro24DspEqualizerFrequencyBandState(),
    ])
    high_middle_coef: list = field(default_factory=lambda: [
        Spro24DspEqualizerFrequencyBandState(),
        Spro24DspEqualizerFrequencyBandState(),
    ])
    high_coef: list = field(default_factory=lambda: [
        Spro24DspEqualizerFrequencyBandState(),
        Spro24DspEqualizerFrequencyBandState(),
    ])

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Spro24DspEqualizerState):
            return NotImplemented
        return (self.output == other.output and
                self.low_coef == other.low_coef and
                self.low_middle_coef == other.low_middle_coef and
                self.high_middle_coef == other.high_middle_coef and
                self.high_coef == other.high_coef)


@dataclass
class Spro24DspReverbState:
    size: float = 0.0
    air: float = 0.0
    enabled: bool = False
    pre_filter: float = 0.0

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Spro24DspReverbState):
            return NotImplemented
        return (self.size == other.size and
                self.air == other.air and
                self.enabled == other.enabled and
                self.pre_filter == other.pre_filter)


@dataclass
class Spro24DspEffectGeneralParams:
    eq_after_comp: list = field(default_factory=lambda: [False, False])
    comp_enable: list = field(default_factory=lambda: [False, False])
    eq_enable: list = field(default_factory=lambda: [False, False])

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, Spro24DspEffectGeneralParams):
            return NotImplemented
        return (self.eq_after_comp == other.eq_after_comp and
                self.comp_enable == other.comp_enable and
                self.eq_enable == other.eq_enable)


def _set_f32(buf: bytearray, offset: int, v: float) -> None:
    buf[offset : offset + 4] = pack_f32(v)


def _get_f32(buf: bytes, offset: int) -> float:
    return unpack_f32(buf[offset : offset + 4])


# ── Compressor ────────────────────────────────────────────────────────────────

def serialize_compressor(state: Spro24DspCompressorState) -> bytes:
    buf = bytearray(COEF_BLOCK_SIZE * 2)
    for ch in range(2):
        base = COEF_BLOCK_SIZE * ch
        _set_f32(buf, base + COMP_OUTPUT_OFFSET, state.output[ch])
        _set_f32(buf, base + COMP_THRESHOLD_OFFSET, state.threshold[ch])
        _set_f32(buf, base + COMP_RATIO_OFFSET, state.ratio[ch])
        _set_f32(buf, base + COMP_ATTACK_OFFSET, state.attack[ch])
        _set_f32(buf, base + COMP_RELEASE_OFFSET, state.release[ch])
    return bytes(buf)


def deserialize_compressor(raw: bytes) -> Spro24DspCompressorState:
    assert len(raw) >= COEF_BLOCK_SIZE * 2
    state = Spro24DspCompressorState()
    for ch in range(2):
        base = COEF_BLOCK_SIZE * ch
        state.output[ch] = _get_f32(raw, base + COMP_OUTPUT_OFFSET)
        state.threshold[ch] = _get_f32(raw, base + COMP_THRESHOLD_OFFSET)
        state.ratio[ch] = _get_f32(raw, base + COMP_RATIO_OFFSET)
        state.attack[ch] = _get_f32(raw, base + COMP_ATTACK_OFFSET)
        state.release[ch] = _get_f32(raw, base + COMP_RELEASE_OFFSET)
    return state


def compressor_commands(state: Spro24DspCompressorState) -> list[FireWireCommand]:
    raw = serialize_compressor(state)
    cmds = []
    base_app_offset = COEF_OFFSET + COEF_BLOCK_SIZE * COEF_BLOCK_COMP
    for pos in range(0, COEF_BLOCK_SIZE * 2, 4):
        val = unpack_u32(raw[pos : pos + 4])
        if val != 0:
            cmds.append(FireWireCommand(
                description=f"Compressor @+0x{pos:03X}",
                app_offset=base_app_offset + pos,
                value=val,
                sw_notice=COMP_CH0_SW_NOTICE if pos < COEF_BLOCK_SIZE else COMP_CH1_SW_NOTICE,
            ))
    return cmds


# ── Equalizer ─────────────────────────────────────────────────────────────────

def serialize_equalizer(state: Spro24DspEqualizerState) -> bytes:
    buf = bytearray(COEF_BLOCK_SIZE * 2)
    for ch in range(2):
        base = COEF_BLOCK_SIZE * ch
        _set_f32(buf, base + EQ_OUTPUT_OFFSET, state.output[ch])
        all_coefs = (
            state.low_coef[ch].coefs
            + state.low_middle_coef[ch].coefs
            + state.high_middle_coef[ch].coefs
            + state.high_coef[ch].coefs
        )
        for i, coef in enumerate(all_coefs):
            _set_f32(buf, base + EQ_LOW_FREQ_OFFSET + i * 4, coef)
    return bytes(buf)


def deserialize_equalizer(raw: bytes) -> Spro24DspEqualizerState:
    assert len(raw) >= COEF_BLOCK_SIZE * 2
    state = Spro24DspEqualizerState()
    for ch in range(2):
        base = COEF_BLOCK_SIZE * ch
        state.output[ch] = _get_f32(raw, base + EQ_OUTPUT_OFFSET)
        all_coefs = []
        for i in range(EQ_COEF_COUNT * 4):
            all_coefs.append(_get_f32(raw, base + EQ_LOW_FREQ_OFFSET + i * 4))
        state.low_coef[ch] = Spro24DspEqualizerFrequencyBandState(coefs=all_coefs[0:5])
        state.low_middle_coef[ch] = Spro24DspEqualizerFrequencyBandState(coefs=all_coefs[5:10])
        state.high_middle_coef[ch] = Spro24DspEqualizerFrequencyBandState(coefs=all_coefs[10:15])
        state.high_coef[ch] = Spro24DspEqualizerFrequencyBandState(coefs=all_coefs[15:20])
    return state


# ── Reverb ────────────────────────────────────────────────────────────────────

def serialize_reverb(state: Spro24DspReverbState) -> bytes:
    buf = bytearray(COEF_BLOCK_SIZE)
    _set_f32(buf, REVERB_SIZE_OFFSET, state.size)
    _set_f32(buf, REVERB_AIR_OFFSET, state.air)
    enabled_val = 1.0 if state.enabled else 0.0
    disabled_val = 0.0 if state.enabled else 1.0
    _set_f32(buf, REVERB_ENABLE_OFFSET, enabled_val)
    _set_f32(buf, REVERB_DISABLE_OFFSET, disabled_val)
    _set_f32(buf, REVERB_PRE_FILTER_VALUE_OFFSET, abs(state.pre_filter))
    sign_val = 1.0 if state.pre_filter > 0.0 else 0.0
    _set_f32(buf, REVERB_PRE_FILTER_SIGN_OFFSET, sign_val)
    return bytes(buf)


def deserialize_reverb(raw: bytes) -> Spro24DspReverbState:
    assert len(raw) >= COEF_BLOCK_SIZE
    size = _get_f32(raw, REVERB_SIZE_OFFSET)
    air = _get_f32(raw, REVERB_AIR_OFFSET)
    enabled_val = _get_f32(raw, REVERB_ENABLE_OFFSET)
    enabled = enabled_val > 0.0
    pre_val = _get_f32(raw, REVERB_PRE_FILTER_VALUE_OFFSET)
    sign_val = _get_f32(raw, REVERB_PRE_FILTER_SIGN_OFFSET)
    if sign_val == 0.0:
        pre_val = -pre_val
    return Spro24DspReverbState(size=size, air=air, enabled=enabled, pre_filter=pre_val)


def reverb_commands(state: Spro24DspReverbState) -> list[FireWireCommand]:
    raw = serialize_reverb(state)
    base_app_offset = COEF_OFFSET + COEF_BLOCK_SIZE * COEF_BLOCK_REVERB
    cmds = []
    for pos in range(0, COEF_BLOCK_SIZE, 4):
        val = unpack_u32(raw[pos : pos + 4])
        if val != 0:
            cmds.append(FireWireCommand(
                description=f"Reverb @+0x{pos:03X}",
                app_offset=base_app_offset + pos,
                value=val,
                sw_notice=REVERB_SW_NOTICE,
            ))
    return cmds


# ── Effect General Params ────────────────────────────────────────────────────

def serialize_effect_general_params(params: Spro24DspEffectGeneralParams) -> bytes:
    val = 0
    for i in range(2):
        flags = 0
        if params.eq_after_comp[i]:
            flags |= CH_STRIP_FLAG_EQ_AFTER_COMP
        if params.comp_enable[i]:
            flags |= CH_STRIP_FLAG_COMP_ENABLE
        if params.eq_enable[i]:
            flags |= CH_STRIP_FLAG_EQ_ENABLE
        val |= (flags & 0xFFFF) << (16 * i)
    return pack_u32(val)


def deserialize_effect_general_params(raw: bytes) -> Spro24DspEffectGeneralParams:
    assert len(raw) >= 4
    val = unpack_u32(raw)
    params = Spro24DspEffectGeneralParams()
    for i in range(2):
        flags = (val >> (16 * i)) & 0xFFFF
        params.eq_after_comp[i] = bool(flags & CH_STRIP_FLAG_EQ_AFTER_COMP)
        params.comp_enable[i] = bool(flags & CH_STRIP_FLAG_COMP_ENABLE)
        params.eq_enable[i] = bool(flags & CH_STRIP_FLAG_EQ_ENABLE)
    return params


def effect_general_params_commands(params: Spro24DspEffectGeneralParams) -> list[FireWireCommand]:
    raw = serialize_effect_general_params(params)
    val = unpack_u32(raw)
    return [FireWireCommand(
        description="Ch-strip flags",
        app_offset=CH_STRIP_FLAG_OFFSET,
        value=val,
        sw_notice=CH_STRIP_FLAG_SW_NOTICE,
    )]


# ── Output Group ─────────────────────────────────────────────────────────────

@dataclass
class OutputChannelState:
    volume: int = 127  # 0..127, stored as (127 - volume) on wire
    muted: bool = False

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, OutputChannelState):
            return NotImplemented
        return self.volume == other.volume and self.muted == other.muted


@dataclass
class OutputGroupState:
    master_mute: bool = False
    master_dim: bool = False
    channels: list = field(default_factory=lambda: [OutputChannelState() for _ in range(6)])

    def __eq__(self, other: object) -> bool:
        if not isinstance(other, OutputGroupState):
            return NotImplemented
        return (self.master_mute == other.master_mute and
                self.master_dim == other.master_dim and
                self.channels == other.channels)


def command_for_volume(ch: int, volume: int) -> FireWireCommand:
    """Generate a FireWire command for output channel volume."""
    wire_val = 127 - volume
    # Each channel uses one quadlet; layout: [mute_bits, vol0, vol1, ...]
    offset = OUTPUT_GROUP_BASE + 4 + ch * 4  # skip the mute/dim quadlet
    return FireWireCommand(
        description=f"Output ch{ch+1} volume={volume}",
        app_offset=offset,
        value=wire_val,
        sw_notice=SW_NOTICE_SRC,
    )


def command_for_mute(ch: int, muted: bool) -> FireWireCommand:
    offset = OUTPUT_GROUP_BASE
    return FireWireCommand(
        description=f"Output ch{ch+1} mute={muted}",
        app_offset=offset,
        value=1 if muted else 0,
        sw_notice=SW_NOTICE_DIM_MUTE,
    )
