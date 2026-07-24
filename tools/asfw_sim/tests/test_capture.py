"""The capture importer must not lose or mislabel driver telemetry."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from asfw_sim.capture import (
    SCHEMA,
    Capture,
    capture_from_responses,
    load_capture,
    parse_message,
    parse_mcp_response,
)

REAL = Path(__file__).resolve().parents[2].parent / "captures" / "2026-07-19-duet-coldstart-to-silence.json"


def test_parses_nested_category_and_tag():
    """Regression: a greedy DOTALL tag regex matched only the outer category
    bracket and silently labelled every record 'DirectAudio'."""
    tag, subtag, fields = parse_message(
        "[DirectAudio] [PayloadWriter] deficit sample=102234948 write=102235460 "
        "exposed=102158336 d=77124 comp=17033508 target=102237860 gen=110360/51078 wake=0"
    )
    assert tag == "PayloadWriter"
    assert subtag == "deficit"
    assert fields["write"] == 102235460
    assert fields["exposed"] == 102158336
    assert fields["d"] == 77124


def test_expands_slash_separated_pairs():
    _, _, fields = parse_message(
        "[DirectAudio] [PayloadWriter] last prepared=12769831/4265357/17035188 "
        "aligned=1 epoch=3 ring=102240580/102241092"
    )
    assert fields["prepared_0"] == 12769831
    assert fields["prepared_2"] == 17035188
    assert fields["prepared"] == "12769831/4265357/17035188"
    assert fields["epoch"] == 3


def test_parses_replay_failure_name_and_signed_distance():
    tag, _, fields = parse_message(
        "[DirectAudio] [TxReplay] fail=overwritten pkt=451442 cur=450792 "
        "prod=451400 d=-608 ep=4/4 slot=0/0 est=1"
    )
    assert tag == "TxReplay"
    assert fields["fail"] == "overwritten"
    assert fields["d"] == -608


def test_reclamped_is_a_subtag_not_a_failure():
    _, subtag, _ = parse_message(
        "[DirectAudio] [TxReplay] reclamped pkt=1 cur=2 prod=3 ok=1"
    )
    assert subtag == "reclamped"


def test_round_trips_through_json(tmp_path):
    payload = {
        "structuredContent": {
            "data": {
                "records": [
                    {
                        "sequence": 1,
                        "timestampNs": 100,
                        "message": "[DirectAudio] [TxPrepFrame] target=10 before=4 "
                        "after=6 deficit=4 write=5 replay=1",
                    }
                ]
            }
        }
    }
    cap = capture_from_responses([payload], device="Duet", notes="unit")
    path = cap.save(tmp_path / "c.json")
    again = load_capture(path)
    assert again.device == "Duet"
    assert len(again.records) == 1
    assert again.records[0].fields["write"] == 5
    assert json.loads(path.read_text())["schema"] == SCHEMA


def test_rejects_a_foreign_schema(tmp_path):
    p = tmp_path / "x.json"
    p.write_text(json.dumps({"schema": "something.else"}))
    with pytest.raises(ValueError, match="expected schema"):
        load_capture(p)


def test_deduplicates_across_per_tag_responses():
    rec = {
        "sequence": 7,
        "timestampNs": 1,
        "message": "[DirectAudio] [TxPrep] margin=672 lead=678",
    }
    payload = {"structuredContent": {"data": {"records": [rec]}}}
    cap = capture_from_responses([payload, payload], record_geometry=False)
    assert len(cap.records) == 1


def test_empty_response_is_not_an_error():
    cap = capture_from_responses(
        [{"structuredContent": {"data": {"records": []}}}], record_geometry=False
    )
    assert cap.records == []
    assert cap.deficit_slope_per_s() is None
    assert cap.cursor_rates() is None


# --- the committed real capture -----------------------------------------------


@pytest.mark.skipif(not REAL.exists(), reason="real capture not present")
def test_real_capture_parses_into_known_tags():
    cap = load_capture(REAL)
    tags = {r.tag for r in cap.records}
    assert "TxPrepFrame" in tags and "PayloadWriter" in tags
    assert "DirectAudio" not in tags, "category bracket must not become a tag"


@pytest.mark.skipif(not REAL.exists(), reason="real capture not present")
def test_real_capture_shows_the_deficit_ramp():
    """The committed Duet run: E loses ground against W, monotonically enough
    that a least-squares slope is unambiguously positive."""
    cap = load_capture(REAL)
    slope = cap.deficit_slope_per_s()
    assert slope is not None and slope > 20, f"expected a ramp, got {slope}"
    rates = cap.cursor_rates()
    assert rates is not None
    w_rate, e_rate = rates
    assert w_rate > e_rate, "E must be the slow cursor"
