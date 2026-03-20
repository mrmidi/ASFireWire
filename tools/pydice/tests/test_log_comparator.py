"""Tests for pydice.protocol.log_comparator."""
import pytest

from pydice.protocol.log_parser import LogEvent
from pydice.protocol.log_comparator import (
    RegisterOp,
    DiffStatus,
    compare_logs,
    describe_payload_difference,
    extract_init_sequence,
    normalize,
    diff_sequences,
)


# ── Helpers ──────────────────────────────────────────────────────────────────

def _ev(kind: str, address: str | None = None, value: int | None = None,
        size: int | None = None, payload: bytes | None = None,
        ts: str = "000:0000:0000") -> LogEvent:
    return LogEvent(
        timestamp=ts, kind=kind, address=address, value=value, size=size, payload=payload,
    )


def _op(address: str, direction: str, value: int | None = None,
        size: int | None = None, raw_kind: str | None = None,
        payload: bytes | None = None) -> RegisterOp:
    return RegisterOp(
        address=address, register="", direction=direction,
        value=value, decoded="", size=size,
        timestamp="000:0000:0000",
        raw_kind=raw_kind or {"R": "QRresp", "W": "Qwrite", "L": "LockRq"}[direction],
        payload=payload,
    )


# ── extract_init_sequence ────────────────────────────────────────────────────

class TestExtractInit:
    def test_extract_init_basic(self):
        """BusReset..ENABLE range extracted correctly."""
        events = [
            _ev("BusReset", ts="001:0000:0000"),
            _ev("SelfID", ts="001:0001:0000"),
            _ev("Qwrite", "ffc2.ffff.e000.0074", 0x0000020c, ts="001:0002:0000"),
            _ev("Qwrite", "ffc2.ffff.e000.0078", 0x00000001, ts="001:0003:0000"),
            _ev("CycleStart", ts="001:0004:0000"),
        ]
        result = extract_init_sequence(events)
        assert len(result) == 4  # BusReset, SelfID, clock write, enable write
        assert result[0].kind == "BusReset"
        assert result[-1].kind == "Qwrite"
        assert result[-1].value == 0x00000001

    def test_extract_init_fallback(self):
        """No ENABLE → everything after last BusReset."""
        events = [
            _ev("BusReset", ts="001:0000:0000"),
            _ev("SelfID", ts="001:0001:0000"),
            _ev("Qwrite", "ffc2.ffff.e000.0074", 0x0000020c, ts="001:0002:0000"),
        ]
        result = extract_init_sequence(events)
        assert len(result) == 3
        assert result[0].kind == "BusReset"

    def test_extract_init_no_busreset_no_enable(self):
        """No BusReset and no ENABLE → return all events."""
        events = [
            _ev("Qwrite", "ffc2.ffff.e000.0074", 0x0000020c),
            _ev("Qread", "ffc2.ffff.e000.0084"),
        ]
        result = extract_init_sequence(events)
        assert len(result) == 2


# ── normalize ────────────────────────────────────────────────────────────────

class TestNormalize:
    def test_normalize_qwrite(self):
        events = [_ev("Qwrite", "ffc2.ffff.e000.0074", 0x0000020c)]
        ops = normalize(events)
        assert len(ops) == 1
        assert ops[0].direction == "W"
        assert ops[0].value == 0x0000020c
        assert ops[0].address == "ffff.e000.0074"

    def test_normalize_bread_single_op(self):
        """Bread keeps request semantics clear when no payload is present."""
        events = [_ev("Bread", "ffc2.ffff.e000.0028", size=512)]
        ops = normalize(events)
        assert len(ops) == 1
        assert ops[0].direction == "R"
        assert ops[0].size == 512
        assert ops[0].value is None
        assert ops[0].decoded == "512B read request"

    def test_normalize_skips_bus_events(self):
        """BusReset, SelfID, CycleStart, PHYResume are excluded."""
        events = [
            _ev("BusReset"),
            _ev("SelfID"),
            _ev("CycleStart"),
            _ev("PHYResume"),
            _ev("WrResp"),
            _ev("Qwrite", "ffc2.ffff.e000.0078", 0x00000001),
        ]
        ops = normalize(events)
        assert len(ops) == 1
        assert ops[0].direction == "W"

    def test_normalize_qread(self):
        events = [_ev("QRresp", "ffc2.ffff.e000.0084", value=48000)]
        ops = normalize(events)
        assert len(ops) == 1
        assert ops[0].direction == "R"
        assert ops[0].value == 48000

    def test_normalize_lock(self):
        events = [_ev(
            "LockRq",
            "ffc2.ffff.f000.0220",
            size=8,
            payload=bytes.fromhex("00001333000010f3"),
        )]
        ops = normalize(events)
        assert len(ops) == 1
        assert ops[0].direction == "L"
        assert "old=4915, new=4339" in ops[0].decoded

    def test_normalize_brresp_summarizes_payload(self):
        events = [_ev(
            "BRresp",
            "ffc2.ffff.e000.0028",
            size=104,
            payload=bytes.fromhex(
                "ffff0000000000000000002050726f32"
                "344453502d3030343731330000000000"
                "00000000000000000000000000000000"
                "00000000000000000000000000000000"
                "0000020c000000010000bb8001000c00"
                "112c001e000000000000000000000000"
                "53747265616d2d3100000000"
            ),
        )]
        ops = normalize(events)
        assert len(ops) == 1
        assert "OWNER=No owner" in ops[0].decoded
        assert "NOTIFY=0x00000020" in ops[0].decoded

    def test_normalize_brresp_partial_global_owner_stays_global(self):
        events = [_ev(
            "BRresp",
            "ffc2.ffff.e000.0028",
            size=380,
            payload=bytes.fromhex("ffff0000000000000000002050726f32"),
        )]
        ops = normalize(events)
        assert len(ops) == 1
        assert "partial(16B)" in ops[0].decoded
        assert "OWNER=No owner" in ops[0].decoded
        assert "NOTIFY=0x00000020" in ops[0].decoded
        assert "CAS.old" not in ops[0].decoded


# ── diff_sequences ───────────────────────────────────────────────────────────

class TestDiffSequences:
    def test_diff_match(self):
        """Same ops → MATCH lines."""
        ref = [_op("ffff.e000.0074", "W", value=0x020c)]
        dbg = [_op("ffff.e000.0074", "W", value=0x020c)]
        result = diff_sequences(ref, dbg)
        assert len(result) == 1
        assert result[0].status == DiffStatus.MATCH

    def test_diff_mismatch(self):
        """Same address+direction, different value → MISMATCH."""
        ref = [_op("ffff.e000.01ac", "W", value=0)]
        dbg = [_op("ffff.e000.01ac", "W", value=0xFFFFFFFF)]
        result = diff_sequences(ref, dbg)
        assert len(result) == 1
        assert result[0].status == DiffStatus.MISMATCH

    def test_diff_ref_only(self):
        """Op only in ref → REF_ONLY."""
        ref = [_op("ffff.e000.01b0", "R", value=16)]
        dbg: list[RegisterOp] = []
        result = diff_sequences(ref, dbg)
        assert len(result) == 1
        assert result[0].status == DiffStatus.REF_ONLY
        assert result[0].ref_op is not None
        assert result[0].debug_op is None

    def test_diff_debug_only(self):
        """Op only in debug → DEBUG_ONLY."""
        ref: list[RegisterOp] = []
        dbg = [_op("ffff.e020.0000", "R", size=72)]
        result = diff_sequences(ref, dbg)
        assert len(result) == 1
        assert result[0].status == DiffStatus.DEBUG_ONLY
        assert result[0].ref_op is None
        assert result[0].debug_op is not None

    def test_diff_preserves_order(self):
        """Output follows merged timeline order."""
        ref = [
            _op("ffff.e000.0000", "R", value=10),
            _op("ffff.e000.0074", "W", value=0x020c),
            _op("ffff.e000.01b0", "R", value=16),  # ref-only
        ]
        dbg = [
            _op("ffff.e000.0000", "R", value=10),
            _op("ffff.e020.0000", "R", size=72),    # debug-only
            _op("ffff.e000.0074", "W", value=0x020c),
        ]
        result = diff_sequences(ref, dbg)
        assert len(result) == 4
        assert result[0].status == DiffStatus.MATCH       # e000.0000
        assert result[1].status == DiffStatus.DEBUG_ONLY   # e020.0000
        assert result[2].status == DiffStatus.MATCH        # e000.0074
        assert result[3].status == DiffStatus.REF_ONLY     # e000.01b0

    def test_diff_block_size_match(self):
        """Block responses with same payload → MATCH."""
        payload = bytes.fromhex("ffff00000000000000000020")
        ref = [_op("ffff.e000.0028", "R", size=12, raw_kind="BRresp", payload=payload)]
        dbg = [_op("ffff.e000.0028", "R", size=12, raw_kind="BRresp", payload=payload)]
        result = diff_sequences(ref, dbg)
        assert result[0].status == DiffStatus.MATCH

    def test_diff_block_size_mismatch(self):
        """Block responses with different payload bytes → MISMATCH."""
        ref = [_op(
            "ffff.e000.0028", "R", size=12, raw_kind="BRresp",
            payload=bytes.fromhex("ffff00000000000000000020"),
        )]
        dbg = [_op(
            "ffff.e000.0028", "R", size=12, raw_kind="BRresp",
            payload=bytes.fromhex("ffff00000000000000000010"),
        )]
        result = diff_sequences(ref, dbg)
        assert result[0].status == DiffStatus.MISMATCH

    def test_diff_separates_request_from_response(self):
        """Bread and BRresp use different diff keys."""
        ref = [
            _op("ffff.e000.0028", "R", size=380, raw_kind="Bread"),
            _op("ffff.e000.0028", "R", size=380, raw_kind="BRresp", payload=b"\x00" * 16),
        ]
        dbg = [
            _op("ffff.e000.0028", "R", size=380, raw_kind="Bread"),
        ]
        result = diff_sequences(ref, dbg)
        assert [line.status for line in result] == [DiffStatus.MATCH, DiffStatus.REF_ONLY]

    def test_compare_logs_can_ignore_config_rom(self):
        ref_events = [
            _ev("BusReset"),
            _ev("Qread", "ffc2.ffff.f000.0400"),
            _ev("QRresp", "ffc2.ffff.f000.0400", value=0x0404E3D3),
            _ev("Qwrite", "ffc2.ffff.e000.0078", value=1),
        ]
        dbg_events = [
            _ev("BusReset"),
            _ev("Qread", "ffc2.ffff.f000.0400"),
            _ev("QRresp", "ffc2.ffff.f000.0400", value=0x0404A54B),
            _ev("Qwrite", "ffc2.ffff.e000.0078", value=1),
        ]

        diff, summary = compare_logs(ref_events, dbg_events, ignore_config_rom=True)

        assert summary["mismatch"] == 0
        assert summary["match"] == 1
        assert len(diff) == 1
        assert diff[0].ref_op is not None
        assert diff[0].ref_op.address == "ffff.e000.0078"

    def test_describe_payload_difference_reports_first_word(self):
        ref = _op(
            "ffff.e000.0028", "R", size=16, raw_kind="BRresp",
            payload=bytes.fromhex("ffff00000000000000000020aaaaaaaa"),
        )
        dbg = _op(
            "ffff.e000.0028", "R", size=16, raw_kind="BRresp",
            payload=bytes.fromhex("ffff00000000000000000010aaaaaaaa"),
        )
        detail = describe_payload_difference(ref, dbg)
        assert detail is not None
        assert "@+0x00b" in detail
        assert "ref=00000020" in detail
        assert "dbg=00000010" in detail
