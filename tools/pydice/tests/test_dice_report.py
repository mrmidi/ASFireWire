"""Tests for the ASFW DICE device report parser and its C++ export.

Expected values are the ones already transcribed by hand in
tests/audio/DiceFixtureGeometryTests.cpp, so the parser and the hand-written
table must agree.
"""
from pathlib import Path

import pytest

from pydice.protocol.dice_report import (
    ADAT_NO_DATA,
    DEFAULT_REPORTS,
    load_dice_report,
    render_dice_images_cpp,
)

REPO_ROOT = Path(__file__).resolve().parents[3]


def _load(key: str):
    rel = dict(DEFAULT_REPORTS)[key]
    return load_dice_report(REPO_ROOT / rel)


# key -> (guid, clockCaps, capture (pcm, midi) per stream, playback (pcm, midi) per stream)
EXPECTED = {
    "saffire-pro24-dsp": (0x00130E0402004713, 0x112C001E, [(16, 1)], [(8, 1)]),
    "venice-f24": (0x10C73F040040C7B6, 0x13000006, [(16, 0), (8, 0)], [(16, 0), (8, 0)]),
    "venice-f32": (0x10C73F04004011DF, 0x13000006, [(16, 0), (16, 0)], [(16, 0), (16, 0)]),
    "studiolive-2442": (0x000A9204049204CB, 0x13000006, [(16, 0), (16, 0)], [(16, 0), (10, 0)]),
    "multimix": (0x00059504000005FE, 0x11000006, [(12, 0), (2, 0)], [(2, 0)]),
}


@pytest.mark.parametrize("key", sorted(EXPECTED))
def test_geometry_matches_hand_transcription(key):
    image = _load(key)
    guid, clock_caps, capture, playback = EXPECTED[key]
    assert image.guid == guid
    assert image.clock_caps == clock_caps
    # DICE TX = device transmits = host capture.
    assert [(s.pcm, s.midi) for s in image.tx_streams] == capture
    assert [(s.pcm, s.midi) for s in image.rx_streams] == playback
    for stream in image.tx_streams + image.rx_streams:
        assert len(stream.names) == stream.pcm
    assert image.tx_entry_quadlets == 70
    assert image.rx_entry_quadlets == 70
    assert len(image.clock_source_names) == 13


def test_saffire_section_table_is_shifted_by_its_larger_global():
    saffire = _load("saffire-pro24-dsp")
    venice = _load("venice-f24")
    assert (saffire.sections["global"].offset_quadlets, saffire.sections["global"].size_quadlets) == (0x0A, 0x5F)
    assert saffire.sections["tx"].offset_quadlets == 0x69
    assert saffire.sections["rx"].offset_quadlets == 0xF7
    assert (venice.sections["global"].size_quadlets, venice.sections["tx"].offset_quadlets) == (0x5A, 0x64)
    assert saffire.has_extension
    assert not venice.has_extension


def test_saffire_was_captured_streaming_and_others_idle():
    saffire = _load("saffire-pro24-dsp")
    assert saffire.enable == 1
    assert saffire.owner == 0xFFC0000100000000
    assert saffire.tx_streams[0].iso == 1
    assert saffire.rx_streams[0].iso == 0
    venice = _load("venice-f24")
    assert venice.enable == 0
    assert venice.owner == 0xFFFF000000000000
    assert all(s.iso == -1 for s in venice.tx_streams + venice.rx_streams)


def test_speed_and_seq_start_are_direction_specific():
    image = _load("studiolive-2442")
    assert all(s.speed == 2 and s.seq_start is None for s in image.tx_streams)  # S400
    assert all(s.seq_start == 0 and s.speed is None for s in image.rx_streams)


def test_alesis_has_no_ext_sync_and_a_selected_vs_achieved_rate_split():
    image = _load("multimix")
    assert "ext_sync" in image.sections and image.sections["ext_sync"].size_quadlets == 0
    assert image.ext_sync is None
    assert (image.clock_select >> 8) & 0xFF == 2  # requests 48 kHz ...
    assert (image.status >> 8) & 0xFF == 1        # ... achieved 44.1 kHz
    assert image.sample_rate == 44100


def test_ext_sync_decodes_no_data():
    ext = _load("venice-f32").ext_sync
    assert ext is not None
    assert ext.clock_source == 12 and ext.locked and ext.rate_index == 2
    assert ext.adat_user_data == ADAT_NO_DATA


def test_saffire_rate_mode_formats_from_eap():
    modes = _load("saffire-pro24-dsp").rate_modes
    assert [s.pcm for s in modes["low"].tx] == [16]
    assert [s.pcm for s in modes["middle"].tx] == [12]
    assert [s.pcm for s in modes["high"].tx] == [8]
    assert all([s.pcm for s in modes[m].rx] == [8] for m in ("low", "middle", "high"))
    assert modes["middle"].tx[0].names[-1] == "Loop 2"


def test_nickname_and_names():
    image = _load("venice-f24")
    assert image.nickname == "Venice"
    assert image.tx_streams[1].names[0] == "OUTPUT ST CH1L"
    assert image.clock_source_names[12] == "Internal"


def test_rendered_cpp_is_deterministic_and_complete():
    images = [(key, load_dice_report(REPO_ROOT / rel)) for key, rel in DEFAULT_REPORTS]
    first = render_dice_images_cpp(images)
    second = render_dice_images_cpp(images)
    assert first == second
    for key, _ in DEFAULT_REPORTS:
        assert f'.key = "{key}"' in first
    assert "kAll{{" in first
    # Label blocks: backslash-separated, double-backslash terminated, C++-escaped.
    assert r'"IP 1\\IP 2\\' in first


def test_checked_in_include_is_current():
    include = REPO_ROOT / "tests" / "support" / "DiceDeviceImages.inc"
    images = [(key, load_dice_report(REPO_ROOT / rel)) for key, rel in DEFAULT_REPORTS]
    assert include.read_text(encoding="utf-8") == render_dice_images_cpp(images), (
        "tests/support/DiceDeviceImages.inc is stale: run "
        "`python main.py export-dice-images-cpp`")
