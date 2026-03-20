"""Factory for AppState with sensible dummy defaults."""
from dataclasses import dataclass, field
from .protocol.focusrite.spro24dsp import (
    OutputGroupState, OutputChannelState,
    Spro24DspCompressorState, Spro24DspEqualizerState,
    Spro24DspEqualizerFrequencyBandState, Spro24DspReverbState,
    Spro24DspEffectGeneralParams,
)
from .protocol.tcat.router_entry import RouterEntry, SrcBlk, DstBlk
from .protocol.constants import SrcBlkId, DstBlkId


# Routing matrix dimensions for Saffire PRO 24 DSP
NUM_SOURCES = 46
NUM_DESTINATIONS = 46

# Source row labels
ROUTING_SOURCE_LABELS = (
    ["Analog In {}".format(i) for i in range(1, 7)]
    + ["S/PDIF Coax {}".format(i) for i in range(1, 3)]
    + ["S/PDIF Opt {}".format(i) for i in range(3, 5)]
    + ["ADAT {}".format(i) for i in range(1, 9)]
    + ["Stream In {}".format(i) for i in range(1, 9)]
    + ["Mixer Out {}".format(i) for i in range(1, 17)]
    + ["Ch-Strip Out {}".format(i) for i in range(1, 3)]
    + ["Reverb Out {}".format(i) for i in range(1, 3)]
)

# Destination column labels
ROUTING_DEST_LABELS = (
    ["Analog Out {}".format(i) for i in range(1, 7)]
    + ["S/PDIF Out {}".format(i) for i in range(1, 3)]
    + ["Stream Out {}".format(i) for i in range(1, 17)]
    + ["Mixer In {}".format(i) for i in range(1, 19)]
    + ["Ch-Strip In {}".format(i) for i in range(1, 3)]
    + ["Reverb In {}".format(i) for i in range(1, 3)]
)

# Mixer labels
MIXER_INPUT_LABELS = (
    ["Analog In {}".format(i) for i in range(1, 7)]
    + ["S/PDIF In {}".format(i) for i in range(1, 3)]
    + ["ADAT In {}".format(i) for i in range(1, 9)]
    + ["Ch-Strip In {}".format(i) for i in range(1, 3)]
)
MIXER_OUTPUT_LABELS = ["Mixer Out {}".format(i) for i in range(1, 17)]


@dataclass
class AppState:
    output_group: OutputGroupState = field(default_factory=OutputGroupState)
    compressor: Spro24DspCompressorState = field(default_factory=Spro24DspCompressorState)
    equalizer: Spro24DspEqualizerState = field(default_factory=Spro24DspEqualizerState)
    reverb: Spro24DspReverbState = field(default_factory=Spro24DspReverbState)
    effect_params: Spro24DspEffectGeneralParams = field(default_factory=Spro24DspEffectGeneralParams)
    dsp_enable: bool = False
    # routing[dst][src] = True/False
    routing: list = field(default_factory=list)
    # mixer[out][inp] = float dB (-inf to 0)
    mixer: list = field(default_factory=list)


def make_dummy_state() -> AppState:
    state = AppState()

    # Output: pairs at 100/100, 80/80, 60/60 — all unmuted
    volumes = [100, 100, 80, 80, 60, 60]
    state.output_group = OutputGroupState(
        master_mute=False,
        master_dim=False,
        channels=[OutputChannelState(volume=v, muted=False) for v in volumes],
    )

    # Compressor: mid-range values
    state.compressor = Spro24DspCompressorState(
        output=[0.5, 0.5],
        threshold=[0.3, 0.3],
        ratio=[0.5, 0.5],
        attack=[0.2, 0.2],
        release=[0.4, 0.4],
    )

    # Equalizer: flat (all zeros = unity)
    state.equalizer = Spro24DspEqualizerState(
        output=[0.5, 0.5],
        low_coef=[Spro24DspEqualizerFrequencyBandState() for _ in range(2)],
        low_middle_coef=[Spro24DspEqualizerFrequencyBandState() for _ in range(2)],
        high_middle_coef=[Spro24DspEqualizerFrequencyBandState() for _ in range(2)],
        high_coef=[Spro24DspEqualizerFrequencyBandState() for _ in range(2)],
    )

    # Reverb: enabled, medium room
    state.reverb = Spro24DspReverbState(
        size=0.5,
        air=0.3,
        enabled=True,
        pre_filter=0.0,
    )

    # Effect general params: comp and EQ enabled on both channels
    state.effect_params = Spro24DspEffectGeneralParams(
        eq_after_comp=[False, False],
        comp_enable=[True, True],
        eq_enable=[True, True],
    )

    state.dsp_enable = True

    # Routing: diagonal — source N → dest N for first 6 pairs (Analog In → Analog Out)
    n_src = len(ROUTING_SOURCE_LABELS)
    n_dst = len(ROUTING_DEST_LABELS)
    state.routing = [[False] * n_src for _ in range(n_dst)]
    for i in range(min(6, n_src, n_dst)):
        state.routing[i][i] = True
    # Stream In N → Stream Out N (stream outs start at dst index 8)
    stream_in_start = 14   # Analog 6 + SPDIF 2 + SPDIF 2 + ADAT 8 = index 14... 6+2+2+8=18? no
    # Sources: Analog 6, SPDIF Coax 2, SPDIF Opt 2, ADAT 8 = 18, then Stream In 1-8 = indices 18-25
    # Dests: Analog 6, SPDIF 2 = 8, then Stream Out 1-16 = indices 8-23
    for i in range(8):
        src_idx = 18 + i  # Stream In 1-8
        dst_idx = 8 + i   # Stream Out 1-8
        if src_idx < n_src and dst_idx < n_dst:
            state.routing[dst_idx][src_idx] = True

    # Mixer: diagonal at 0 dB (represented as 0.0), off-diagonal at -inf (None)
    n_in = len(MIXER_INPUT_LABELS)
    n_out = len(MIXER_OUTPUT_LABELS)
    state.mixer = [[None] * n_in for _ in range(n_out)]
    for i in range(min(n_in, n_out)):
        state.mixer[i][i] = 0.0

    return state
