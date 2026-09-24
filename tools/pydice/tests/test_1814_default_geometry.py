"""Sanity check the displayed 48 kHz FireBug samples against the 1814 profile."""
from pathlib import Path

from pydice.protocol.maudio_special_fixture import _all_events


CAPTURE = Path(__file__).resolve().parents[1] / "1814-default-geom-48k.txt"


def test_default_48k_isoch_samples_match_spdif_formation_geometry():
    packets = [event for event in _all_events(CAPTURE.read_text(encoding="utf-8"))
               if event.kind == "IsochPacket"]

    # Default SPDIF formation: playback is 6 PCM + MIDI (DBS 7), capture is
    # 10 PCM + MIDI (DBS 11). Each sample has two CIP quadlets followed by
    # complete AM824 data blocks.
    playback = [event for event in packets if event.dst == 0 and len(event.payload) > 8]
    capture = [event for event in packets if event.dst == 1 and len(event.payload) > 8]
    empty_capture = [event for event in packets if event.dst == 1 and len(event.payload) == 8]
    assert len(packets) == 12
    assert len(playback) == 6
    assert len(capture) == 4
    assert len(empty_capture) == 2
    # This firmware uses DBS 2 and FDF 0xff in header-only packets; they do
    # not describe the active 11-slot data geometry.
    assert all(event.payload[1] == 2 and event.payload[7] == 0xff
               for event in empty_capture)

    for samples, expected_dbs in ((playback, 7), (capture, 11)):
        assert all(event.payload[1] == expected_dbs for event in samples)
        rows = {(len(event.payload) - 8) // (expected_dbs * 4)
                for event in samples
                if (len(event.payload) - 8) % (expected_dbs * 4) == 0}
        assert rows == {8}
        assert all((len(event.payload) - 8) % (expected_dbs * 4) == 0
                   for event in samples)

    assert {(event.dst, len(event.payload)) for event in playback + capture} == {
        (0, 232), (1, 360),
    }
