"""Tests ported from tcat/extension/standalone_section.rs."""
from pydice.protocol.tcat.standalone_section import (
    StandaloneParameters, AdatParam, WordClockParam, WordClockMode, WordClockRate,
    serialize, deserialize,
)
from pydice.protocol.constants import ClockSource, ClockRate


def test_standalone_params_serdes():
    params = StandaloneParameters(
        clock_source=ClockSource.Tdif,
        aes_high_rate=True,
        adat_mode=AdatParam.SMUX4,
        word_clock_param=WordClockParam(
            mode=WordClockMode.Middle,
            rate=WordClockRate(numerator=12, denominator=7),
        ),
        internal_rate=ClockRate.R88200,
    )

    raw = serialize(params)
    assert len(raw) >= 20

    p = deserialize(raw)
    assert p == params
