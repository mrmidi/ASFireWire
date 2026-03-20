"""Tests for pydice semantic init analysis."""
from __future__ import annotations

import json
import struct
from pathlib import Path

from pydice.protocol.log_parser import parse_log
from pydice.protocol.semantic_analysis import (
    PHASE_STREAM,
    PHASE_WAIT,
    _merge_transactions,
    analyze_session,
    compare_init_logs,
    compare_init_logs_strict_phase0,
    render_json_report,
    render_strict_phase0_json_report,
    render_strict_phase0_text_report,
    render_text_report,
)
from pydice.protocol.tcat.global_section import _serialize_labels


FIXTURE_DIR = Path(__file__).resolve().parents[1]


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


def _wrresp(ts: str, tlabel: int, speed: str = "s100") -> str:
    return f"{ts}  WrResp from ffc2 to ffc0, tLabel {tlabel}, rCode 0 [ack 1] {speed}"


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


def _lockrq(ts: str, dest: str, payload: bytes, tlabel: int, speed: str = "s100") -> str:
    quadlets = " ".join(
        f"{struct.unpack('>I', payload[idx : idx + 4])[0]:08x}"
        for idx in range(0, len(payload), 4)
    )
    return (
        f"{ts}  LockRq from ffc0 to {dest}, size {len(payload)}, tLabel {tlabel} [ack 2] {speed}\n"
        f"               0000   {quadlets}"
    )


def _lockresp(ts: str, src: str, tlabel: int, payload: bytes, speed: str = "s100") -> str:
    quadlets = " ".join(
        f"{struct.unpack('>I', payload[idx : idx + 4])[0]:08x}"
        for idx in range(0, len(payload), 4)
    )
    return (
        f"{ts}  LockResp from {src} to ffc0, tLabel {tlabel}, size {len(payload)} [ack 1] {speed}\n"
        f"               0000   {quadlets}"
    )


def _bus_reset(ts: str) -> str:
    return f"{ts}  BUS RESET ---------------------------------------------------------------------------"


def _tx_payload() -> bytes:
    payload = bytearray(512)
    payload[0:8] = struct.pack(">II", 1, 70)
    payload[8:24] = struct.pack(">IIII", 1, 16, 1, 2)
    payload[24 : 24 + 256] = _serialize_labels(["IP 1", "IP 2", "IP 3"], 256)
    return bytes(payload)


def _rx_payload() -> bytes:
    payload = bytearray(512)
    payload[0:8] = struct.pack(">II", 1, 70)
    payload[8:24] = struct.pack(">IIII", 0, 0, 8, 1)
    payload[24 : 24 + 256] = _serialize_labels(["Mon 1", "Mon 2"], 256)
    return bytes(payload)


def _fixture_comparison():
    reference = parse_log(FIXTURE_DIR.joinpath("reference.txt").read_text(encoding="utf-8"))
    current = parse_log(FIXTURE_DIR.joinpath("asfw-lessfubar.txt").read_text(encoding="utf-8"))
    return compare_init_logs(reference, current, "reference.txt", "asfw-lessfubar.txt")


def _strict_reference_text() -> str:
    return "\n".join(
        [
            _bus_reset("001:0000:0000"),
            _qread("001:0000:0001", "ffc2.ffff.e000.007c", 1, "s400"),
            _qrresp("001:0000:0002", "ffc2", 1, 0x00000201, "s400"),
            _qread("001:0000:0003", "ffc2.ffff.e000.0084", 2, "s400"),
            _qrresp("001:0000:0004", "ffc2", 2, 48000, "s400"),
            _bread("001:0000:0005", "ffc2.ffff.e000.0028", 8, 3, "s400"),
            _brresp("001:0000:0006", "ffc2", 3, struct.pack(">Q", 0xFFFF000000000000), "s400"),
            _lockrq("001:0000:0007", "ffc2.ffff.e000.0028",
                    struct.pack(">QQ", 0xFFFF000000000000, 0xFFC0000100000000), 4, "s400"),
            _lockresp("001:0000:0008", "ffc2", 4, struct.pack(">Q", 0xFFFF000000000000), "s400"),
            _bread("001:0000:0009", "ffc2.ffff.e000.0028", 8, 5, "s400"),
            _brresp("001:0000:0010", "ffc2", 5, struct.pack(">Q", 0xFFC0000100000000), "s400"),
            _qwrite("001:0000:0011", "ffc2.ffff.e000.0074", 0x0000020C, 6, "s400"),
            _wrresp("001:0000:0012", 6, "s400"),
            _qread("001:0000:0013", "ffc2.ffff.f000.0220", 7),
            _qrresp("001:0000:0014", "ffc2", 7, 4915),
            _qread("001:0000:0015", "ffc2.ffff.f000.0224", 8),
            _qrresp("001:0000:0016", "ffc2", 8, 0xFFFFFFFF),
            _qread("001:0000:0017", "ffc2.ffff.f000.0228", 9),
            _qrresp("001:0000:0018", "ffc2", 9, 0xFFFFFFFF),
            _lockrq("001:0000:0019", "ffc2.ffff.f000.0220", struct.pack(">II", 4915, 4595), 10),
            _lockresp("001:0000:0020", "ffc2", 10, struct.pack(">I", 4915)),
            _lockrq("001:0000:0021", "ffc2.ffff.f000.0224", struct.pack(">II", 0xFFFFFFFF, 0x7FFFFFFF), 11),
            _lockresp("001:0000:0022", "ffc2", 11, struct.pack(">I", 0xFFFFFFFF)),
            _qread("001:0000:0023", "ffc2.ffff.e000.03e0", 12, "s400"),
            _qrresp("001:0000:0024", "ffc2", 12, 0x46, "s400"),
            _qwrite("001:0000:0025", "ffc2.ffff.e000.03e4", 0, 13, "s400"),
            _wrresp("001:0000:0026", 13, "s400"),
            _qwrite("001:0000:0027", "ffc2.ffff.e000.03e8", 0, 14, "s400"),
            _wrresp("001:0000:0028", 14, "s400"),
            _qread("001:0000:0029", "ffc2.ffff.f000.0220", 15),
            _qrresp("001:0000:0030", "ffc2", 15, 4595),
            _qread("001:0000:0031", "ffc2.ffff.f000.0224", 16),
            _qrresp("001:0000:0032", "ffc2", 16, 0x7FFFFFFF),
            _qread("001:0000:0033", "ffc2.ffff.f000.0228", 17),
            _qrresp("001:0000:0034", "ffc2", 17, 0xFFFFFFFF),
            _lockrq("001:0000:0035", "ffc2.ffff.f000.0220", struct.pack(">II", 4595, 4019), 18),
            _lockresp("001:0000:0036", "ffc2", 18, struct.pack(">I", 4595)),
            _lockrq("001:0000:0037", "ffc2.ffff.f000.0224", struct.pack(">II", 0x7FFFFFFF, 0x3FFFFFFF), 19),
            _lockresp("001:0000:0038", "ffc2", 19, struct.pack(">I", 0x7FFFFFFF)),
            _qread("001:0000:0039", "ffc2.ffff.e000.01a8", 20, "s400"),
            _qrresp("001:0000:0040", "ffc2", 20, 0x46, "s400"),
            _qwrite("001:0000:0041", "ffc2.ffff.e000.01ac", 1, 21, "s400"),
            _wrresp("001:0000:0042", 21, "s400"),
            _qwrite("001:0000:0043", "ffc2.ffff.e000.01b8", 2, 22, "s400"),
            _wrresp("001:0000:0044", 22, "s400"),
            _qwrite("001:0000:0045", "ffc2.ffff.e000.0078", 1, 23, "s400"),
            _wrresp("001:0000:0046", 23, "s400"),
        ]
    )


def test_request_response_pairs_merge_with_evidence():
    text = "\n".join(
        [
            _qread("001:0000:0001", "ffc2.ffff.e000.0084", 1),
            _qrresp("001:0000:0002", "ffc2", 1, 48000),
        ]
    )
    txs = _merge_transactions(parse_log(text))
    assert len(txs) == 1
    tx = txs[0]
    assert tx.status == "complete"
    assert tx.request_kind == "Qread"
    assert tx.response_kind == "QRresp"
    assert tx.response_value == 48000
    assert len(tx.evidence) == 2
    assert tx.timestamp_start == "001:0000:0001"
    assert tx.timestamp_end == "001:0000:0002"


def test_repeated_notification_polls_collapse_into_single_wait_step():
    text = "\n".join(
        [
            _bus_reset("001:0000:0000"),
            _qwrite("001:0000:0001", "ffc2.ffff.e000.0074", 0x0000020C, 10),
            _wrresp("001:0000:0002", 10),
            _qread("001:0000:0003", "ffc2.ffff.e000.0030", 11),
            _qrresp("001:0000:0004", "ffc2", 11, 0x00000010),
            _qread("001:0000:0005", "ffc2.ffff.e000.0030", 12),
            _qrresp("001:0000:0006", "ffc2", 12, 0x00000010),
            _qread("001:0000:0007", "ffc2.ffff.e000.0030", 13),
            _qrresp("001:0000:0008", "ffc2", 13, 0x00000010),
            _qwrite("001:0000:0009", "ffc2.ffff.e000.0078", 0x00000001, 14),
            _wrresp("001:0000:0010", 14),
        ]
    )
    session = analyze_session(parse_log(text), "wait.txt")
    wait_phase = next(phase for phase in session.phases if phase.kind == PHASE_WAIT)
    assert wait_phase.details["steps"][0]["poll_count"] == 3
    assert "poll GLOBAL_NOTIFICATION 3x" in wait_phase.summary


def test_block_reads_and_scalar_reads_can_compare_as_equivalent_stream_discovery():
    reference_text = "\n".join(
        [
            _bus_reset("001:0000:0000"),
            _qread("001:0000:0001", "ffc2.ffff.e000.01a4", 1),
            _qrresp("001:0000:0002", "ffc2", 1, 1),
            _qread("001:0000:0003", "ffc2.ffff.e000.01a8", 2),
            _qrresp("001:0000:0004", "ffc2", 2, 70),
            _qread("001:0000:0005", "ffc2.ffff.e000.01ac", 3),
            _qrresp("001:0000:0006", "ffc2", 3, 1),
            _qread("001:0000:0007", "ffc2.ffff.e000.01b0", 4),
            _qrresp("001:0000:0008", "ffc2", 4, 16),
            _qread("001:0000:0009", "ffc2.ffff.e000.01b4", 5),
            _qrresp("001:0000:0010", "ffc2", 5, 1),
            _qread("001:0000:0011", "ffc2.ffff.e000.01b8", 6),
            _qrresp("001:0000:0012", "ffc2", 6, 2),
            _qread("001:0000:0013", "ffc2.ffff.e000.03dc", 7),
            _qrresp("001:0000:0014", "ffc2", 7, 1),
            _qread("001:0000:0015", "ffc2.ffff.e000.03e0", 8),
            _qrresp("001:0000:0016", "ffc2", 8, 70),
            _qread("001:0000:0017", "ffc2.ffff.e000.03e4", 9),
            _qrresp("001:0000:0018", "ffc2", 9, 0),
            _qread("001:0000:0019", "ffc2.ffff.e000.03e8", 10),
            _qrresp("001:0000:0020", "ffc2", 10, 0),
            _qread("001:0000:0021", "ffc2.ffff.e000.03ec", 11),
            _qrresp("001:0000:0022", "ffc2", 11, 8),
            _qread("001:0000:0023", "ffc2.ffff.e000.03f0", 12),
            _qrresp("001:0000:0024", "ffc2", 12, 1),
            _qwrite("001:0000:0025", "ffc2.ffff.e000.0078", 1, 13),
            _wrresp("001:0000:0026", 13),
        ]
    )
    current_text = "\n".join(
        [
            _bus_reset("001:0000:0000"),
            _bread("001:0000:0001", "ffc2.ffff.e000.01a4", 512, 1),
            _brresp("001:0000:0002", "ffc2", 1, _tx_payload()),
            _bread("001:0000:0003", "ffc2.ffff.e000.03dc", 512, 2),
            _brresp("001:0000:0004", "ffc2", 2, _rx_payload()),
            _qwrite("001:0000:0005", "ffc2.ffff.e000.0078", 1, 3),
            _wrresp("001:0000:0006", 3),
        ]
    )
    comparison = compare_init_logs(parse_log(reference_text), parse_log(current_text), "ref", "cur")
    stream_phase = next(phase for phase in comparison.phases if phase.kind == PHASE_STREAM)
    assert stream_phase.classification == "equivalent"


def test_real_pair_reports_partial_global_coverage_and_missing_irm():
    comparison = _fixture_comparison()
    titles = [finding.title for finding in comparison.findings]
    assert "Missing IRM reservation before stream programming" in titles
    assert "Current trace leaves reference-visible state undiscovered" in titles
    assert comparison.current.state["global"]["coverage"]["max_read_size"] == 104
    assert comparison.reference.state["global"]["coverage"]["max_read_size"] == 380


def test_real_pair_reports_completion_strategy_difference_and_unknown_region():
    comparison = _fixture_comparison()
    text = render_text_report(comparison)
    assert "Completion strategy differs from reference" in text
    assert "poll GLOBAL_NOTIFICATION 19x => LOCK_CHG" in text
    assert "async write FW notification address = 0x00000020" in text
    assert "ffff.e020.6dd4" in text


def test_real_pair_reports_extra_configrom_and_tcat_discovery():
    comparison = _fixture_comparison()
    titles = [finding.title for finding in comparison.findings]
    assert "Current trace performs deeper Config ROM probing" in titles
    assert "Current trace performs extra TCAT discovery" in titles


def test_json_report_is_deterministic_and_contains_unknown_region_fingerprint():
    comparison_a = _fixture_comparison()
    comparison_b = _fixture_comparison()
    report_a = render_json_report(comparison_a)
    report_b = render_json_report(comparison_b)
    assert json.dumps(report_a, sort_keys=True) == json.dumps(report_b, sort_keys=True)

    unknown = next(
        entry for entry in report_a["unknown_regions"] if entry["address"] == "ffff.e020.6dd4"
    )
    assert unknown["classification"] == "extra"
    assert len(unknown["fingerprint"]) == 12


def test_text_report_mentions_key_findings_and_programmed_channels():
    comparison = _fixture_comparison()
    text = render_text_report(comparison)
    assert "[HIGH] Missing IRM reservation before stream programming" in text
    assert "program TX[0] = channel 1, speed=s400" in text
    assert "program RX[0] = channel 0, seq=0" in text


def test_strict_phase0_reference_self_passes():
    text = _strict_reference_text()
    comparison = compare_init_logs_strict_phase0(parse_log(text), parse_log(text), "ref", "cur")
    assert comparison.passed is True
    assert comparison.failure is None
    rendered = render_strict_phase0_text_report(comparison)
    assert "Status:    PASS" in rendered


def test_strict_phase0_flags_extra_read_only_noise_outside_whitelist():
    reference = _strict_reference_text()
    current = reference.replace(
        _qread("001:0000:0013", "ffc2.ffff.f000.0220", 7),
        "\n".join(
            [
                _qread("001:0000:00125", "ffc2.ffff.e020.0d24", 24, "s400"),
                _qrresp("001:0000:00126", "ffc2", 24, 0x00000001, "s400"),
                _qread("001:0000:0013", "ffc2.ffff.f000.0220", 7),
            ]
        ),
    )
    comparison = compare_init_logs_strict_phase0(parse_log(reference), parse_log(current), "ref", "cur")
    assert comparison.passed is False
    assert comparison.failure is not None
    assert comparison.failure.code == "unexpected_read_noise"
    assert comparison.failure.current_step is not None
    assert comparison.failure.current_step.region == "ffff.e020.0d24"


def test_strict_phase0_flags_rx_programming_delay():
    reference = _strict_reference_text()
    delayed = "\n".join(
        [
            _bus_reset("001:0000:0000"),
            _qread("001:0000:0001", "ffc2.ffff.e000.007c", 1, "s400"),
            _qrresp("001:0000:0002", "ffc2", 1, 0x00000201, "s400"),
            _qread("001:0000:0003", "ffc2.ffff.e000.0084", 2, "s400"),
            _qrresp("001:0000:0004", "ffc2", 2, 48000, "s400"),
            _bread("001:0000:0005", "ffc2.ffff.e000.0028", 8, 3, "s400"),
            _brresp("001:0000:0006", "ffc2", 3, struct.pack(">Q", 0xFFFF000000000000), "s400"),
            _lockrq("001:0000:0007", "ffc2.ffff.e000.0028",
                    struct.pack(">QQ", 0xFFFF000000000000, 0xFFC0000100000000), 4, "s400"),
            _lockresp("001:0000:0008", "ffc2", 4, struct.pack(">Q", 0xFFFF000000000000), "s400"),
            _bread("001:0000:0009", "ffc2.ffff.e000.0028", 8, 5, "s400"),
            _brresp("001:0000:0010", "ffc2", 5, struct.pack(">Q", 0xFFC0000100000000), "s400"),
            _qwrite("001:0000:0011", "ffc2.ffff.e000.0074", 0x0000020C, 6, "s400"),
            _wrresp("001:0000:0012", 6, "s400"),
            _qread("001:0000:0013", "ffc2.ffff.f000.0220", 7),
            _qrresp("001:0000:0014", "ffc2", 7, 4915),
            _qread("001:0000:0015", "ffc2.ffff.f000.0224", 8),
            _qrresp("001:0000:0016", "ffc2", 8, 0xFFFFFFFF),
            _qread("001:0000:0017", "ffc2.ffff.f000.0228", 9),
            _qrresp("001:0000:0018", "ffc2", 9, 0xFFFFFFFF),
            _lockrq("001:0000:0019", "ffc2.ffff.f000.0220", struct.pack(">II", 4915, 4595), 10),
            _lockresp("001:0000:0020", "ffc2", 10, struct.pack(">I", 4915)),
            _lockrq("001:0000:0021", "ffc2.ffff.f000.0224", struct.pack(">II", 0xFFFFFFFF, 0x7FFFFFFF), 11),
            _lockresp("001:0000:0022", "ffc2", 11, struct.pack(">I", 0xFFFFFFFF)),
            _qread("001:0000:0023", "ffc2.ffff.f000.0220", 12),
            _qrresp("001:0000:0024", "ffc2", 12, 4595),
            _qread("001:0000:0025", "ffc2.ffff.f000.0224", 13),
            _qrresp("001:0000:0026", "ffc2", 13, 0x7FFFFFFF),
            _qread("001:0000:0027", "ffc2.ffff.f000.0228", 14),
            _qrresp("001:0000:0028", "ffc2", 14, 0xFFFFFFFF),
            _lockrq("001:0000:0029", "ffc2.ffff.f000.0220", struct.pack(">II", 4595, 4019), 15),
            _lockresp("001:0000:0030", "ffc2", 15, struct.pack(">I", 4595)),
            _lockrq("001:0000:0031", "ffc2.ffff.f000.0224", struct.pack(">II", 0x7FFFFFFF, 0x3FFFFFFF), 16),
            _lockresp("001:0000:0032", "ffc2", 16, struct.pack(">I", 0x7FFFFFFF)),
            _qread("001:0000:0033", "ffc2.ffff.e000.03e0", 17, "s400"),
            _qrresp("001:0000:0034", "ffc2", 17, 0x46, "s400"),
            _qwrite("001:0000:0035", "ffc2.ffff.e000.03e4", 0, 18, "s400"),
            _wrresp("001:0000:0036", 18, "s400"),
            _qwrite("001:0000:0037", "ffc2.ffff.e000.03e8", 0, 19, "s400"),
            _wrresp("001:0000:0038", 19, "s400"),
            _qread("001:0000:0039", "ffc2.ffff.e000.01a8", 20, "s400"),
            _qrresp("001:0000:0040", "ffc2", 20, 0x46, "s400"),
            _qwrite("001:0000:0041", "ffc2.ffff.e000.01ac", 1, 21, "s400"),
            _wrresp("001:0000:0042", 21, "s400"),
            _qwrite("001:0000:0043", "ffc2.ffff.e000.01b8", 2, 22, "s400"),
            _wrresp("001:0000:0044", 22, "s400"),
            _qwrite("001:0000:0045", "ffc2.ffff.e000.0078", 1, 23, "s400"),
            _wrresp("001:0000:0046", 23, "s400"),
        ]
    )
    comparison = compare_init_logs_strict_phase0(parse_log(reference), parse_log(delayed), "ref", "cur")
    assert comparison.passed is False
    assert comparison.failure is not None
    assert comparison.failure.code == "rx_programming_delayed"
    assert "RX programming was delayed" in comparison.failure.message


def test_strict_phase0_flags_extra_write():
    reference = _strict_reference_text()
    current = reference.replace(
        _qwrite("001:0000:0045", "ffc2.ffff.e000.0078", 1, 23, "s400"),
        "\n".join(
            [
                _qwrite("001:0000:00445", "ffc2.ffff.e000.01b4", 1, 30, "s400"),
                _wrresp("001:0000:00446", 30, "s400"),
                _qwrite("001:0000:0045", "ffc2.ffff.e000.0078", 1, 23, "s400"),
            ]
        ),
    )
    comparison = compare_init_logs_strict_phase0(parse_log(reference), parse_log(current), "ref", "cur")
    assert comparison.passed is False
    assert comparison.failure is not None
    assert comparison.failure.code == "unexpected_state_change"


def test_strict_phase0_flags_extension_space_read():
    reference = _strict_reference_text()
    extension_read = "\n".join(
        [
            _bread("001:0000:00445", "ffc2.ffff.e020.0000", 72, 30, "s400"),
            _brresp("001:0000:00446", "ffc2", 30, bytes(72), "s400"),
        ]
    )
    current = reference.replace(
        _qwrite("001:0000:0045", "ffc2.ffff.e000.0078", 1, 23, "s400"),
        "\n".join(
            [
                extension_read,
                _qwrite("001:0000:0045", "ffc2.ffff.e000.0078", 1, 23, "s400"),
            ]
        ),
    )
    comparison = compare_init_logs_strict_phase0(parse_log(reference), parse_log(current), "ref", "cur")
    assert comparison.passed is False
    assert comparison.failure is not None
    assert comparison.failure.code == "unexpected_read_noise"


def test_strict_phase0_flags_duplicate_irm_verify_block():
    reference = _strict_reference_text()
    duplicate_verify = "\n".join(
        [
            _qread("001:0000:00345", "ffc2.ffff.f000.0220", 24),
            _qrresp("001:0000:00346", "ffc2", 24, 4595),
            _qread("001:0000:00347", "ffc2.ffff.f000.0224", 25),
            _qrresp("001:0000:00348", "ffc2", 25, 0x7FFFFFFF),
            _qread("001:0000:00349", "ffc2.ffff.f000.0228", 26),
            _qrresp("001:0000:00350", "ffc2", 26, 0xFFFFFFFF),
        ]
    )
    current = reference.replace(
        _lockrq("001:0000:0035", "ffc2.ffff.f000.0220", struct.pack(">II", 4595, 4019), 18),
        "\n".join(
            [
                duplicate_verify,
                _lockrq("001:0000:0035", "ffc2.ffff.f000.0220", struct.pack(">II", 4595, 4019), 18),
            ]
        ),
    )
    comparison = compare_init_logs_strict_phase0(parse_log(reference), parse_log(current), "ref", "cur")
    assert comparison.passed is False
    assert comparison.failure is not None
    assert comparison.failure.code == "state_changing_mismatch"


def test_strict_phase0_flags_changed_value():
    reference = _strict_reference_text()
    current = reference.replace(
        _qwrite("001:0000:0043", "ffc2.ffff.e000.01b8", 2, 22, "s400"),
        _qwrite("001:0000:0043", "ffc2.ffff.e000.01b8", 3, 22, "s400"),
    )
    comparison = compare_init_logs_strict_phase0(parse_log(reference), parse_log(current), "ref", "cur")
    assert comparison.passed is False
    assert comparison.failure is not None
    assert comparison.failure.code == "state_changing_mismatch"


def test_strict_phase0_flags_pre_state_mismatch():
    reference = _strict_reference_text()
    current = reference.replace(
        _qrresp("001:0000:0002", "ffc2", 1, 0x00000201, "s400"),
        _qrresp("001:0000:0002", "ffc2", 1, 0x00000001, "s400"),
    )
    comparison = compare_init_logs_strict_phase0(parse_log(reference), parse_log(current), "ref", "cur")
    assert comparison.passed is False
    assert comparison.failure is not None
    assert comparison.failure.code == "pre_state_mismatch"
    json_report = render_strict_phase0_json_report(comparison)
    assert json_report["failure"]["code"] == "pre_state_mismatch"
