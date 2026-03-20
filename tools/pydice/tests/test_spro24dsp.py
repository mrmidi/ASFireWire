"""Tests ported from focusrite/spro24dsp.rs."""
import pytest
import struct
from pydice.protocol.focusrite.spro24dsp import (
    Spro24DspCompressorState, Spro24DspEqualizerState, Spro24DspEqualizerFrequencyBandState,
    Spro24DspReverbState, Spro24DspEffectGeneralParams,
    serialize_compressor, deserialize_compressor,
    serialize_equalizer, deserialize_equalizer,
    serialize_reverb, deserialize_reverb,
    serialize_effect_general_params, deserialize_effect_general_params,
)
from pydice.protocol.constants import COEF_BLOCK_SIZE


def approx_list(lst, expected, rel=1e-6):
    assert len(lst) == len(expected)
    for a, b in zip(lst, expected):
        assert a == pytest.approx(b, rel=rel), f"{a} != ~{b}"


def test_compressor_serdes():
    state = Spro24DspCompressorState(
        output=[0.04, 0.05],
        threshold=[0.16, 0.17],
        ratio=[0.20, 0.21],
        attack=[0.32, 0.33],
        release=[0.44, 0.45],
    )
    raw = serialize_compressor(state)
    assert len(raw) == COEF_BLOCK_SIZE * 2

    s = deserialize_compressor(raw)
    approx_list(s.output, state.output)
    approx_list(s.threshold, state.threshold)
    approx_list(s.ratio, state.ratio)
    approx_list(s.attack, state.attack)
    approx_list(s.release, state.release)


def test_equalizer_serdes():
    state = Spro24DspEqualizerState(
        output=[0.06, 0.07],
        low_coef=[
            Spro24DspEqualizerFrequencyBandState(coefs=[0.00, 0.01, 0.02, 0.03, 0.04]),
            Spro24DspEqualizerFrequencyBandState(coefs=[0.10, 0.11, 0.12, 0.13, 0.14]),
        ],
        low_middle_coef=[
            Spro24DspEqualizerFrequencyBandState(coefs=[0.20, 0.21, 0.22, 0.23, 0.24]),
            Spro24DspEqualizerFrequencyBandState(coefs=[0.30, 0.31, 0.32, 0.33, 0.34]),
        ],
        high_middle_coef=[
            Spro24DspEqualizerFrequencyBandState(coefs=[0.40, 0.41, 0.42, 0.43, 0.44]),
            Spro24DspEqualizerFrequencyBandState(coefs=[0.50, 0.51, 0.52, 0.53, 0.54]),
        ],
        high_coef=[
            Spro24DspEqualizerFrequencyBandState(coefs=[0.60, 0.61, 0.62, 0.63, 0.64]),
            Spro24DspEqualizerFrequencyBandState(coefs=[0.70, 0.71, 0.72, 0.73, 0.74]),
        ],
    )
    raw = serialize_equalizer(state)
    assert len(raw) == COEF_BLOCK_SIZE * 2

    s = deserialize_equalizer(raw)
    approx_list(s.output, state.output)
    for ch in range(2):
        approx_list(s.low_coef[ch].coefs, state.low_coef[ch].coefs)
        approx_list(s.low_middle_coef[ch].coefs, state.low_middle_coef[ch].coefs)
        approx_list(s.high_middle_coef[ch].coefs, state.high_middle_coef[ch].coefs)
        approx_list(s.high_coef[ch].coefs, state.high_coef[ch].coefs)


def test_reverb_serdes():
    state = Spro24DspReverbState(
        size=0.04,
        air=0.14,
        enabled=False,
        pre_filter=-0.1,
    )
    raw = serialize_reverb(state)
    assert len(raw) == COEF_BLOCK_SIZE

    s = deserialize_reverb(raw)
    assert s.size == pytest.approx(state.size, rel=1e-6)
    assert s.air == pytest.approx(state.air, rel=1e-6)
    assert s.enabled == state.enabled
    assert s.pre_filter == pytest.approx(state.pre_filter, rel=1e-6)


def test_effect_general_params_serdes():
    params = Spro24DspEffectGeneralParams(
        eq_after_comp=[False, True],
        comp_enable=[True, False],
        eq_enable=[False, True],
    )
    raw = serialize_effect_general_params(params)
    assert len(raw) == 4

    p = deserialize_effect_general_params(raw)
    assert p.eq_after_comp == params.eq_after_comp
    assert p.comp_enable == params.comp_enable
    assert p.eq_enable == params.eq_enable
