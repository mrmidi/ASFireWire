"""Tests for phase-0 parity markdown export."""
from __future__ import annotations

import struct
from pathlib import Path

from pydice.protocol.log_parser import parse_log
from pydice.protocol.parity_markdown import (
    STYLE_BOTH,
    STYLE_PHASES,
    STYLE_TIMELINE,
    export_parity_markdown,
    render_parity_markdown,
)
from pydice.protocol.semantic_analysis import analyze_session


FIXTURE_DIR = Path(__file__).resolve().parents[1]


def _bus_reset(ts: str) -> str:
    return f"{ts}  BUS RESET ---------------------------------------------------------------------------"


def _self_id(ts: str, node: int) -> str:
    return f"{ts}  Self-ID 0  Node={node}"


def _cycle_start(ts: str) -> str:
    return f"{ts}  CycleStart from ffc0"


def _qread(ts: str, dest: str, tlabel: int, speed: str = "s100") -> str:
    return f"{ts}  Qread from ffc0 to {dest}, tLabel {tlabel} [ack 2] {speed}"


def _qrresp(ts: str, src: str, tlabel: int, value: int, speed: str = "s100") -> str:
    return (
        f"{ts}  QRresp from {src} to ffc0, tLabel {tlabel}, "
        f"value {value:08x} [ack 1] {speed}"
    )


def _qwrite(ts: str, dest: str, value: int, tlabel: int, speed: str = "s100") -> str:
    return (
        f"{ts}  Qwrite from ffc0 to {dest}, value {value:08x}, "
        f"tLabel {tlabel} [ack 2] {speed}"
    )


def _bread(ts: str, dest: str, size: int, tlabel: int, speed: str = "s100") -> str:
    return f"{ts}  Bread from ffc0 to {dest}, size {size}, tLabel {tlabel} [ack 2] {speed}"


def _brresp(ts: str, src: str, tlabel: int, payload: bytes, speed: str = "s100") -> str:
    lines = [
        f"{ts}  BRresp from {src} to ffc0, tLabel {tlabel}, size {len(payload)} "
        f"[actual {len(payload)}] [ack 1] {speed}"
    ]
    for offset in range(0, len(payload), 16):
        chunk = payload[offset : offset + 16]
        quadlets = " ".join(
            f"{struct.unpack('>I', chunk[idx : idx + 4])[0]:08x}"
            for idx in range(0, len(chunk), 4)
        )
        lines.append(f"               {offset:04x}   {quadlets}")
    return "\n".join(lines)


def _layout_payload() -> bytes:
    return struct.pack(">10I", 10, 95, 105, 142, 247, 282, 0, 0, 0, 0)


def _synthetic_session():
    text = "\n".join(
        [
            _bus_reset("001:0000:0000"),
            _self_id("001:0000:0001", 2),
            _cycle_start("001:0000:0002"),
            _qread("001:0000:0003", "ffc2.ffff.f000.0400", 1),
            _qrresp("001:0000:0004", "ffc2", 1, 0x0404A54B),
            _bread("001:0000:0005", "ffc2.ffff.e000.0000", 40, 2),
            _brresp("001:0000:0006", "ffc2", 2, _layout_payload()),
            _qwrite("001:0000:0007", "ffc2.ffff.e000.0078", 1, 3),
        ]
    )
    return analyze_session(parse_log(text), "synthetic.txt")


def test_phase_render_omits_config_rom_and_low_signal_noise():
    session = _synthetic_session()
    rendered = render_parity_markdown(
        session,
        source_label="synthetic.txt",
        ignore_config_rom=True,
        style=STYLE_PHASES,
    )[STYLE_PHASES]

    assert "# Phase 0 Reference Parity Checklist" in rendered
    assert "## Bus Reset" in rendered
    assert "## DICE Layout Discovery" in rendered
    assert "ffff.f000.0400" not in rendered
    assert "`SelfID`" not in rendered
    assert "`CycleStart`" not in rendered
    assert "`Bread` `ffff.e000.0000` `DICE_GLOBAL_OFFSET` `40B`" in rendered


def test_timeline_render_is_deterministic_and_tagged():
    session = _synthetic_session()
    rendered_a = render_parity_markdown(
        session,
        source_label="synthetic.txt",
        ignore_config_rom=True,
        style=STYLE_TIMELINE,
    )[STYLE_TIMELINE]
    rendered_b = render_parity_markdown(
        session,
        source_label="synthetic.txt",
        ignore_config_rom=True,
        style=STYLE_TIMELINE,
    )[STYLE_TIMELINE]

    assert rendered_a == rendered_b
    assert "## Ordered Timeline" in rendered_a
    assert "[layout]" in rendered_a
    assert "[enable]" in rendered_a


def test_leading_irm_compare_verify_prelude_is_trimmed():
    text = "\n".join(
        [
            _bus_reset("001:0000:0000"),
            _qread("001:0000:0001", "ffc2.ffff.f000.0228", 1),
            _qrresp("001:0000:0002", "ffc2", 1, 0xFFFFFFFF),
            "001:0000:0003  LockRq from ffc0 to ffc2.ffff.f000.0228, size 8, tLabel 2 [ack 2] s100\n"
            "               0000   ffffffff ffffffff",
            "001:0000:0004  LockResp from ffc2 to ffc0, size 4, tLabel 2 [ack 1] s100\n"
            "               0000   ffffffff",
            _qread("001:0000:0005", "ffc2.ffff.e000.007c", 3),
            _qrresp("001:0000:0006", "ffc2", 3, 0x00000201),
            _qwrite("001:0000:0007", "ffc2.ffff.e000.0078", 1, 4),
        ]
    )
    session = analyze_session(parse_log(text), "prelude.txt")
    timeline = render_parity_markdown(
        session,
        source_label="prelude.txt",
        ignore_config_rom=True,
        style=STYLE_TIMELINE,
    )[STYLE_TIMELINE]

    assert "`ffff.f000.0228`" not in timeline
    assert "- [ ] 002 [global] `Qread` `ffff.e000.007c` `GLOBAL_STATUS` `-` — read request" in timeline


def test_export_both_writes_expected_filenames(tmp_path):
    session = _synthetic_session()
    written = export_parity_markdown(
        session,
        "synthetic.txt",
        tmp_path,
        ignore_config_rom=True,
        style=STYLE_BOTH,
    )

    assert written["phases"].name == "reference-phase0-phases.md"
    assert written["timeline"].name == "reference-phase0-timeline.md"
    assert written["phases"].exists()
    assert written["timeline"].exists()


def test_real_ref_full_render_contains_key_phase0_entries():
    text = FIXTURE_DIR.joinpath("ref-full.txt").read_text(encoding="utf-8")
    session = analyze_session(parse_log(text), "ref-full.txt")
    outputs = render_parity_markdown(
        session,
        source_label="ref-full.txt",
        ignore_config_rom=True,
        style=STYLE_BOTH,
    )
    phases = outputs[STYLE_PHASES]
    timeline = outputs[STYLE_TIMELINE]

    assert "## IRM Reservation" in phases
    assert "GLOBAL_CLOCK_SELECT" in phases
    assert "IRM_BANDWIDTH_AVAILABLE" in phases
    assert "GLOBAL_ENABLE" in phases
    assert "ffff.f000.0400" not in phases
    assert "[irm]" in timeline
    assert "[enable]" in timeline
    assert "`ffff.f000.0228` `IRM_CHANNELS_AVAILABLE_LO` `8B` — 8B IRM_CHANNELS_AVAILABLE_LO: old=0xffffffff, new=0xffffffff | → no change (compare-verify)" not in timeline
    assert "- [ ] 002 [global] `Qread` `ffff.e000.007c` `GLOBAL_STATUS` `-` — read request" in timeline
    assert "`LockRq` `ffff.f000.0220` `IRM_BANDWIDTH_AVAILABLE` `8B`" in timeline
