"""Tests for phase-0 parity C++ fixture export."""
from __future__ import annotations

import struct
from pathlib import Path

from pydice.protocol.log_parser import parse_log
from pydice.protocol.parity_cpp_fixture import (
    export_parity_cpp_fixture,
    render_parity_cpp_fixture,
)
from pydice.protocol.semantic_analysis import analyze_session


FIXTURE_DIR = Path(__file__).resolve().parents[1]


def _bus_reset(ts: str) -> str:
    return f"{ts}  BUS RESET ---------------------------------------------------------------------------"


def _qread(ts: str, src: str, dest: str, tlabel: int, speed: str = "s100") -> str:
    return f"{ts}  Qread from {src} to {dest}, tLabel {tlabel} [ack 2] {speed}"


def _qrresp(ts: str, src: str, dst: str, tlabel: int, value: int, speed: str = "s100") -> str:
    return (
        f"{ts}  QRresp from {src} to {dst}, tLabel {tlabel}, "
        f"value {value:08x} [ack 1] {speed}"
    )


def _qwrite(ts: str, src: str, dest: str, value: int, tlabel: int, speed: str = "s100") -> str:
    return (
        f"{ts}  Qwrite from {src} to {dest}, value {value:08x}, "
        f"tLabel {tlabel} [ack 2] {speed}"
    )


def _bread(ts: str, src: str, dest: str, size: int, tlabel: int, speed: str = "s100") -> str:
    return f"{ts}  Bread from {src} to {dest}, size {size}, tLabel {tlabel} [ack 2] {speed}"


def _brresp(ts: str, src: str, dst: str, tlabel: int, payload: bytes, speed: str = "s100") -> str:
    lines = [
        f"{ts}  BRresp from {src} to {dst}, tLabel {tlabel}, size {len(payload)} "
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


def _lockrq(ts: str, src: str, dest: str, payload: bytes, tlabel: int, speed: str = "s100") -> str:
    quadlets = " ".join(
        f"{struct.unpack('>I', payload[idx : idx + 4])[0]:08x}"
        for idx in range(0, len(payload), 4)
    )
    return (
        f"{ts}  LockRq from {src} to {dest}, size {len(payload)}, tLabel {tlabel} [ack 2] {speed}\n"
        f"               0000   {quadlets}"
    )


def _lockresp(ts: str, src: str, dst: str, payload: bytes, tlabel: int, speed: str = "s100") -> str:
    quadlets = " ".join(
        f"{struct.unpack('>I', payload[idx : idx + 4])[0]:08x}"
        for idx in range(0, len(payload), 4)
    )
    return (
        f"{ts}  LockResp from {src} to {dst}, size {len(payload)}, tLabel {tlabel} [ack 1] {speed}\n"
        f"               0000   {quadlets}"
    )


def _layout_payload() -> bytes:
    return struct.pack(">10I", 10, 95, 105, 142, 247, 282, 0, 0, 0, 0)


def _synthetic_session():
    text = "\n".join(
        [
            _bus_reset("001:0000:0000"),
            _qread("001:0000:0001", "ffc0", "ffc2.ffff.f000.0228", 1),
            _qrresp("001:0000:0002", "ffc2", "ffc0", 1, 0xFFFFFFFF),
            _lockrq(
                "001:0000:0003",
                "ffc0",
                "ffc2.ffff.f000.0228",
                struct.pack(">II", 0xFFFFFFFF, 0xFFFFFFFF),
                2,
            ),
            _lockresp("001:0000:0004", "ffc2", "ffc0", struct.pack(">I", 0xFFFFFFFF), 2),
            _qread("001:0000:0005", "ffc0", "ffc2.ffff.e000.007c", 3, "s400"),
            _qrresp("001:0000:0006", "ffc2", "ffc0", 3, 0x00000201, "s400"),
            _bread("001:0000:0007", "ffc0", "ffc2.ffff.e000.0000", 40, 4, "s400"),
            _brresp("001:0000:0008", "ffc2", "ffc0", 4, _layout_payload(), "s400"),
            _qwrite("001:0000:0009", "ffc2", "ffc0.0001.0000.0000", 0x20, 5, "s400"),
            _qwrite("001:0000:0010", "ffc0", "ffc2.ffff.e000.0078", 1, 6, "s400"),
        ]
    )
    return analyze_session(parse_log(text), "synthetic.txt")


def test_cpp_fixture_render_is_deterministic_and_skips_prelude_and_incoming_writes():
    session = _synthetic_session()
    rendered_a = render_parity_cpp_fixture(
        session,
        source_label="synthetic.txt",
        ignore_config_rom=True,
    )
    rendered_b = render_parity_cpp_fixture(
        session,
        source_label="synthetic.txt",
        ignore_config_rom=True,
    )

    assert rendered_a == rendered_b
    assert "namespace ReferencePhase0ParityFixture" in rendered_a
    assert "kPrepareExpectedRequests" in rendered_a
    assert "0xE000007CU" in rendered_a
    assert "0xF0000400U" not in rendered_a
    assert "0x0001U, 0x00000000U" not in rendered_a


def test_cpp_fixture_export_writes_requested_path(tmp_path):
    session = _synthetic_session()
    out_path = tmp_path / "ReferencePhase0ParityFixture.inc"
    written = export_parity_cpp_fixture(
        session,
        "synthetic.txt",
        out_path,
        ignore_config_rom=True,
    )

    assert written == out_path
    assert written.exists()
    assert "kFullExpectedRequests" in written.read_text(encoding="utf-8")


def test_real_ref_full_cpp_fixture_contains_expected_stage_arrays():
    text = FIXTURE_DIR.joinpath("ref-full.txt").read_text(encoding="utf-8")
    session = analyze_session(parse_log(text), "ref-full.txt")
    rendered = render_parity_cpp_fixture(
        session,
        source_label="ref-full.txt",
        ignore_config_rom=True,
    )

    assert 'inline constexpr char kSourceLog[] = "ref-full.txt";' in rendered
    assert "kPrepareExpectedRequests" in rendered
    assert "kIrmPlaybackExpectedRequests" in rendered
    assert "kProgramRxExpectedRequests" in rendered
    assert "kIrmCaptureExpectedRequests" in rendered
    assert "kProgramTxEnableExpectedRequests" in rendered
    assert "0xE000007CU" in rendered
    assert "0xF0000220U" in rendered
    assert "0xE0000078U" in rendered
    assert "0xF0000400U" not in rendered
    assert "0x0001U, 0x00000000U" not in rendered
