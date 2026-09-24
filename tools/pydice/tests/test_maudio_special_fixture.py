"""Capture-driven M-Audio 1814 special bring-up fixture export."""
from pathlib import Path
import re

from pydice.protocol.maudio_special_fixture import (
    _all_events,
    _happy_run,
    load_and_export_maudio_special_fixture,
)


CAPTURE = Path(__file__).resolve().parents[1] / "1814-mockme.txt"


def test_real_capture_selects_second_happy_run_with_both_isoch_streams():
    all_events = _all_events(CAPTURE.read_text(encoding="utf-8"))
    wrap_index = next(i for i, event in enumerate(all_events)
                      if event.kind == "Reset" and event.timestamp == "025:1195:0893")
    wrap_delta = all_events[wrap_index].elapsed_cycles - all_events[wrap_index - 1].elapsed_cycles
    assert 1_000_000 < wrap_delta < 2_000_000

    events = _happy_run(all_events)
    kinds = [event.kind for event in events]
    assert events[0].kind == "Reset"
    assert events[0].timestamp == "028:0085:2530"
    assert not any(event.timestamp == "025:1195:0893" for event in events)
    assert "LockRq" in kinds and "LockResp" in kinds
    assert "Bwrite" in kinds and "WrResp" in kinds
    assert {event.dst for event in events if event.kind == "IsochPacket"} >= {0, 1}
    assert any(event.payload == bytes.fromhex("020200009002ffff") for event in events
               if event.kind == "IsochPacket")
    tx_sample = next(i for i, event in enumerate(events)
                     if event.kind == "IsochPacket" and event.dst == 0 and event.payload)
    rx_sample = next(i for i, event in enumerate(events)
                     if event.kind == "IsochPacket" and event.dst == 1 and event.payload)
    accepted_start = [i for i, event in enumerate(events)
                      if event.kind == "Bwrite" and event.payload[:3] == bytes.fromhex("09ff19")
                      and i > max(tx_sample, rx_sample)]
    assert accepted_start
    assert any(event.elapsed_cycles == 0 for event in events)
    assert all(a.elapsed_cycles <= b.elapsed_cycles for a, b in zip(events, events[1:]))


def test_exporter_emits_capture_order_and_payload_backed_event_records(tmp_path):
    target = tmp_path / "MAudioSpecialHappyPathFixture.inc"
    load_and_export_maudio_special_fixture(CAPTURE, target)
    rendered = target.read_text(encoding="utf-8")
    assert 'kSourceLog[] = "1814-mockme.txt"' in rendered
    assert "EventKind::LockRq" in rendered
    assert "EventKind::IsochPacket" in rendered
    assert "kPayload" in rendered
    assert re.search(r"EventKind::IsochPacket[^\n]*0x0000000000000000ULL, 8U", rendered)
    # The real observed output plug is allocated before its 232-byte stream sample.
    assert rendered.index("EventKind::LockRq") < rendered.index("EventKind::IsochPacket")
