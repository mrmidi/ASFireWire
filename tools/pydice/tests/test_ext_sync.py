"""Tests ported from tcat/ext_sync_section.rs."""
from pydice.protocol.tcat.ext_sync_section import deserialize, ExtendedSyncParameters
from pydice.protocol.constants import ClockSource, ClockRate


def test_ext_sync_params_serdes():
    raw = bytes([0, 0, 0, 0xa, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0, 7])
    params = deserialize(raw)

    assert params.clk_src == ClockSource.Arx3
    assert params.clk_src_locked is True
    assert params.clk_rate == ClockRate.R176400
    assert params.adat_user_data == 7
