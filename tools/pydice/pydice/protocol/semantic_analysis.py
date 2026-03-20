"""Semantic init analysis for DICE / Focusrite FireWire logs."""
from __future__ import annotations

import hashlib
import json
import struct
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from .dice_address_map import annotate
from .log_parser import LogEvent
from .payload_decoder import decode_payload
from .tcat.global_section import _deserialize_labels
from .codec import unpack_label

PHASE_BUS_RESET = "bus_reset"
PHASE_CONFIGROM = "configrom_probe"
PHASE_LAYOUT = "dice_layout_discovery"
PHASE_GLOBAL = "global_state_read"
PHASE_OWNER = "owner_claim"
PHASE_CLOCK = "clock_select"
PHASE_WAIT = "completion_wait"
PHASE_STREAM = "stream_discovery"
PHASE_IRM = "irm_reservation"
PHASE_RX = "rx_programming"
PHASE_TX = "tx_programming"
PHASE_TCAT = "tcat_extended_discovery"
PHASE_ENABLE = "enable"

PHASE_ORDER = [
    PHASE_BUS_RESET,
    PHASE_CONFIGROM,
    PHASE_LAYOUT,
    PHASE_GLOBAL,
    PHASE_OWNER,
    PHASE_CLOCK,
    PHASE_WAIT,
    PHASE_STREAM,
    PHASE_IRM,
    PHASE_RX,
    PHASE_TX,
    PHASE_TCAT,
    PHASE_ENABLE,
]

SEVERITY_ORDER = {"high": 0, "medium": 1, "info": 2}

_REQUEST_TO_RESPONSE = {
    "Qread": "QRresp",
    "Bread": "BRresp",
    "LockRq": "LockResp",
    "Qwrite": "WrResp",
    "Bwrite": "WrResp",
}

_WAIT_REGIONS = {
    "ffff.e000.0030",
    "0001.0000.0000",
    "00ff.0000.d1cc",
}

_GLOBAL_FIELD_REGIONS = {
    "ffff.e000.0028",
    "ffff.e000.0030",
    "ffff.e000.0034",
    "ffff.e000.0074",
    "ffff.e000.0078",
    "ffff.e000.007c",
    "ffff.e000.0080",
    "ffff.e000.0084",
    "ffff.e000.0088",
    "ffff.e000.008c",
    "ffff.e000.0090",
}

_TX_REGIONS = {
    "ffff.e000.01a4",
    "ffff.e000.01a8",
    "ffff.e000.01ac",
    "ffff.e000.01b0",
    "ffff.e000.01b4",
    "ffff.e000.01b8",
    "ffff.e000.01bc",
}

_RX_REGIONS = {
    "ffff.e000.03dc",
    "ffff.e000.03e0",
    "ffff.e000.03e4",
    "ffff.e000.03e8",
    "ffff.e000.03ec",
    "ffff.e000.03f0",
    "ffff.e000.03f4",
}

_IRM_REGIONS = {
    "ffff.f000.0220",
    "ffff.f000.0224",
    "ffff.f000.0228",
}


@dataclass
class SessionWindow:
    start_index: int
    end_index: int
    events: list[LogEvent]
    bus_reset_timestamp: str | None
    enable_timestamp: str | None
    used_enable_one: bool


@dataclass
class SemanticTransaction:
    index: int
    request_kind: str | None
    response_kind: str | None
    direction: str
    address: str | None
    region: str | None
    register: str
    status: str
    timestamp_start: str
    timestamp_end: str
    size: int | None
    request_value: int | None
    response_value: int | None
    request_payload: bytes | None
    response_payload: bytes | None
    evidence: list[str]
    request_event_index: int | None = None
    response_event_index: int | None = None
    poll_count: int = 1

    @property
    def scalar_value(self) -> int | None:
        if self.direction == "read":
            return self.response_value
        return self.request_value

    @property
    def payload(self) -> bytes | None:
        if self.direction == "read":
            return self.response_payload
        if self.response_payload:
            return self.response_payload
        return self.request_payload

    @property
    def kind_label(self) -> str:
        if self.request_kind:
            return self.request_kind
        if self.response_kind:
            return self.response_kind
        return "Unknown"

    @property
    def decoded_value(self) -> str:
        if not self.region:
            return ""
        value = self.scalar_value
        if value is None:
            return ""
        return _annotate_region(self.region, value)[1]

    @property
    def decoded_lines(self) -> list[str]:
        if not self.region:
            return []
        payload = self.payload
        if payload:
            return decode_payload(_address_for_region(self.region), payload, self.size)
        value = self.scalar_value
        if value is None:
            return []
        decoded = _annotate_region(self.region, value)[1]
        return [decoded] if decoded else []


@dataclass
class PhaseRecord:
    index: int
    kind: str
    transaction_indexes: list[int]
    summary: str
    details: dict[str, Any]


@dataclass
class SessionAnalysis:
    label: str
    window: SessionWindow
    transactions: list[SemanticTransaction]
    phases: list[PhaseRecord]
    state: dict[str, Any]
    unknown_regions: list[dict[str, Any]]


@dataclass
class PhaseComparison:
    index: int
    kind: str
    classification: str
    reference_phase_index: int | None
    current_phase_index: int | None
    reference_summary: str | None
    current_summary: str | None


@dataclass
class Finding:
    severity: str
    title: str
    why: str
    reference: str
    current: str
    phase: str
    phase_indexes: list[int]


@dataclass
class SemanticComparison:
    metadata: dict[str, Any]
    reference: SessionAnalysis
    current: SessionAnalysis
    phases: list[PhaseComparison]
    findings: list[Finding]
    state_diffs: dict[str, Any]
    unknown_regions: list[dict[str, Any]]


@dataclass
class StrictPhase0Step:
    index: int
    region: str
    direction: str
    request_kind: str | None
    response_kind: str | None
    size: int | None
    request_value: int | None
    response_value: int | None
    request_payload_hex: str | None
    response_payload_hex: str | None
    summary: str


@dataclass
class StrictPhase0Failure:
    code: str
    message: str
    reference_step: StrictPhase0Step | None = None
    current_step: StrictPhase0Step | None = None


@dataclass
class StrictPhase0Analysis:
    label: str
    session: SessionAnalysis
    pre_state: dict[str, Any]
    core_steps: list[StrictPhase0Step]
    allowed_noise: list[str]
    unexpected_noise: list[StrictPhase0Step]
    unexpected_state_changes: list[StrictPhase0Step]


@dataclass
class StrictPhase0Comparison:
    metadata: dict[str, Any]
    reference: StrictPhase0Analysis
    current: StrictPhase0Analysis
    must_match: list[str]
    may_differ: list[str]
    warnings: list[str]
    failure: StrictPhase0Failure | None
    passed: bool


STRICT_MAY_DIFFER = [
    "Reference-aligned pre-state reads (GLOBAL_STATUS, GLOBAL_SAMPLE_RATE, short owner/global readback).",
    "Stable notification polling that does not cross a state-changing step boundary.",
]


def compare_init_logs(
    reference_events: list[LogEvent],
    current_events: list[LogEvent],
    reference_name: str = "reference",
    current_name: str = "current",
) -> SemanticComparison:
    """Compare two init traces at semantic level."""
    reference = analyze_session(reference_events, reference_name)
    current = analyze_session(current_events, current_name)
    phases = _compare_phases(reference, current)
    state_diffs = _build_state_diffs(reference.state, current.state)
    unknown_regions = _compare_unknown_regions(reference.unknown_regions, current.unknown_regions)
    findings = _build_findings(reference, current, phases, state_diffs, unknown_regions)
    metadata = {
        "analysis": "semantic_init",
        "session_selection": "last_bus_reset_before_final_enable_1",
        "reference_label": reference_name,
        "current_label": current_name,
    }
    return SemanticComparison(
        metadata=metadata,
        reference=reference,
        current=current,
        phases=phases,
        findings=findings,
        state_diffs=state_diffs,
        unknown_regions=unknown_regions,
    )


def compare_init_logs_strict_phase0(
    reference_events: list[LogEvent],
    current_events: list[LogEvent],
    reference_name: str = "reference",
    current_name: str = "current",
) -> StrictPhase0Comparison:
    """Compare two init traces against the strict phase-0 startup contract."""
    reference = _analyze_strict_phase0(reference_events, reference_name)
    current = _analyze_strict_phase0(current_events, current_name)
    failure, warnings = _compare_strict_phase0(reference, current)
    metadata = {
        "analysis": "strict_phase0",
        "session_selection": "last_bus_reset_before_final_enable_1",
        "reference_label": reference_name,
        "current_label": current_name,
    }
    must_match = [step.summary for step in reference.core_steps]
    return StrictPhase0Comparison(
        metadata=metadata,
        reference=reference,
        current=current,
        must_match=must_match,
        may_differ=STRICT_MAY_DIFFER,
        warnings=warnings,
        failure=failure,
        passed=failure is None,
    )


def analyze_session(events: list[LogEvent], label: str) -> SessionAnalysis:
    """Analyze a single log's last init session."""
    window = extract_last_init_window(events)
    transactions = _merge_transactions(window.events)
    state = _build_state(transactions)
    phases = _build_phases(transactions, state)
    unknown_regions = _collect_unknown_regions(transactions)
    return SessionAnalysis(
        label=label,
        window=window,
        transactions=transactions,
        phases=phases,
        state=state,
        unknown_regions=unknown_regions,
    )


def _analyze_strict_phase0(events: list[LogEvent], label: str) -> StrictPhase0Analysis:
    from .parity_phase0 import collect_phase0_events

    session = analyze_session(events, label)
    phase0_events = collect_phase0_events(session, ignore_config_rom=True)
    transactions = _merge_transactions([item.event for item in phase0_events])
    pre_state = _extract_strict_pre_state(transactions)
    core_steps: list[StrictPhase0Step] = []
    allowed_noise: list[str] = []
    unexpected_noise: list[StrictPhase0Step] = []
    unexpected_state_changes: list[StrictPhase0Step] = []

    for tx in transactions:
        if _is_strict_core_transaction(tx):
            core_steps.append(_strict_step_from_transaction(tx))
            continue
        if _is_allowed_strict_noise_transaction(tx):
            allowed_noise.append(_strict_noise_summary(tx))
            continue
        if tx.direction in {"write", "lock"}:
            unexpected_state_changes.append(_strict_step_from_transaction(tx))
            continue
        unexpected_noise.append(_strict_step_from_transaction(tx))

    return StrictPhase0Analysis(
        label=label,
        session=session,
        pre_state=pre_state,
        core_steps=core_steps,
        allowed_noise=allowed_noise,
        unexpected_noise=unexpected_noise,
        unexpected_state_changes=unexpected_state_changes,
    )


def extract_last_init_window(events: list[LogEvent]) -> SessionWindow:
    """Return the last init session ending at the final GLOBAL_ENABLE=1 write."""
    enable_idx = None
    used_enable_one = True
    for idx in range(len(events) - 1, -1, -1):
        ev = events[idx]
        if ev.kind == "Qwrite" and _region(ev.address) == "ffff.e000.0078" and ev.value == 1:
            enable_idx = idx
            break
    if enable_idx is None:
        used_enable_one = False
        for idx in range(len(events) - 1, -1, -1):
            ev = events[idx]
            if ev.kind == "Qwrite" and _region(ev.address) == "ffff.e000.0078":
                enable_idx = idx
                break
    if enable_idx is None:
        enable_idx = len(events) - 1
        used_enable_one = False

    start_idx = 0
    bus_reset_timestamp = None
    for idx in range(enable_idx, -1, -1):
        if events[idx].kind == "BusReset":
            start_idx = idx
            bus_reset_timestamp = events[idx].timestamp
            break

    session_events = events[start_idx : enable_idx + 1]
    enable_timestamp = events[enable_idx].timestamp if 0 <= enable_idx < len(events) else None
    return SessionWindow(
        start_index=start_idx,
        end_index=enable_idx,
        events=session_events,
        bus_reset_timestamp=bus_reset_timestamp,
        enable_timestamp=enable_timestamp,
        used_enable_one=used_enable_one,
    )


def _merge_transactions(events: list[LogEvent]) -> list[SemanticTransaction]:
    transactions: list[SemanticTransaction] = []
    pending: dict[tuple[str, str, int], list[SemanticTransaction]] = {}

    def add_pending(tx: SemanticTransaction, response_kind: str | None, src: str, tlabel: int | None) -> None:
        if response_kind is None or tlabel is None:
            return
        key = (response_kind, src, tlabel)
        pending.setdefault(key, []).append(tx)

    for event_index, ev in enumerate(events):
        if ev.kind in {"BusReset", "SelfID", "CycleStart", "PHYResume"}:
            continue

        if ev.kind in _REQUEST_TO_RESPONSE:
            region = _region(ev.address)
            register = _annotate_region(region, ev.value)[0] if region else ""
            tx = SemanticTransaction(
                index=len(transactions),
                request_kind=ev.kind,
                response_kind=None,
                direction=_direction_for_kind(ev.kind),
                address=ev.address,
                region=region,
                register=register,
                status="request_only",
                timestamp_start=ev.timestamp,
                timestamp_end=ev.timestamp,
                size=ev.size,
                request_value=ev.value,
                response_value=None,
                request_payload=ev.payload,
                response_payload=None,
                evidence=[ev.raw_line],
                request_event_index=event_index,
            )
            transactions.append(tx)
            add_pending(tx, _REQUEST_TO_RESPONSE[ev.kind], ev.src, ev.tLabel)
            continue

        if ev.kind in {"QRresp", "BRresp", "LockResp", "WrResp"}:
            key = (ev.kind, ev.dst, ev.tLabel if ev.tLabel is not None else -1)
            matched = pending.get(key, [])
            if matched:
                tx = matched.pop(0)
                if not matched:
                    pending.pop(key, None)
                tx.response_kind = ev.kind
                tx.response_event_index = event_index
                tx.timestamp_end = ev.timestamp
                tx.status = "complete"
                if ev.address and not tx.address:
                    tx.address = ev.address
                    tx.region = _region(ev.address)
                if tx.region and not tx.register:
                    tx.register = _annotate_region(tx.region, tx.request_value or ev.value)[0]
                tx.response_value = ev.value
                tx.response_payload = ev.payload
                tx.size = ev.size or tx.size
                tx.evidence.append(ev.raw_line)
                continue

            region = _region(ev.address)
            register = _annotate_region(region, ev.value)[0] if region else ""
            transactions.append(
                SemanticTransaction(
                    index=len(transactions),
                    request_kind=None,
                    response_kind=ev.kind,
                    direction=_direction_for_kind(ev.kind),
                    address=ev.address,
                    region=region,
                    register=register,
                    status="response_only",
                    timestamp_start=ev.timestamp,
                    timestamp_end=ev.timestamp,
                    size=ev.size,
                    request_value=None,
                    response_value=ev.value,
                    request_payload=None,
                    response_payload=ev.payload,
                    evidence=[ev.raw_line],
                    response_event_index=event_index,
                )
            )

    return transactions


def _build_state(transactions: list[SemanticTransaction]) -> dict[str, Any]:
    state: dict[str, Any] = {
        "layout": {},
        "global": {
            "fields": {},
            "coverage": {
                "requested_sizes": [],
                "max_read_size": 0,
                "expected_bytes": None,
            },
            "owner_claims": [],
        },
        "tx": {
            "number": None,
            "size_quadlets": None,
            "streams": {},
            "coverage": {
                "block_sizes": [],
                "field_keys": set(),
            },
        },
        "rx": {
            "number": None,
            "size_quadlets": None,
            "streams": {},
            "coverage": {
                "block_sizes": [],
                "field_keys": set(),
            },
        },
        "irm": {
            "reads": {},
            "allocations": [],
        },
        "configrom": {
            "values": {},
            "read_count": 0,
        },
        "completion": {
            "polls": [],
            "async_writes": [],
        },
        "tcat_extended": {
            "regions": {},
        },
    }

    for tx in transactions:
        region = tx.region
        if not region:
            continue

        if region.startswith("ffff.f000.04") and tx.direction == "read" and tx.response_value is not None:
            state["configrom"]["values"][region] = tx.response_value
            state["configrom"]["read_count"] += tx.poll_count
            continue

        if region == "ffff.e000.0000" and tx.direction == "read" and tx.payload:
            layout = _extract_layout_fields(tx.payload)
            state["layout"].update(layout)
            if "DICE_GLOBAL_SIZE" in layout:
                state["global"]["coverage"]["expected_bytes"] = layout["DICE_GLOBAL_SIZE"] * 4
            continue

        if region == "ffff.e000.0028":
            if tx.direction == "read":
                size = tx.size or 0
                if size:
                    state["global"]["coverage"]["requested_sizes"].append(size)
                    state["global"]["coverage"]["max_read_size"] = max(
                        state["global"]["coverage"]["max_read_size"], size
                    )
                if tx.payload:
                    state["global"]["fields"].update(_extract_global_fields(tx.payload))
            elif tx.direction == "lock" and tx.request_payload and len(tx.request_payload) >= 16:
                state["global"]["owner_claims"].append(_extract_cas_values(tx.request_payload))
            continue

        if region in _GLOBAL_FIELD_REGIONS:
            _apply_global_scalar(state["global"]["fields"], region, tx.scalar_value, tx.payload)
            if region == "ffff.e000.0030" and tx.direction == "read":
                state["completion"]["polls"].append(
                    {
                        "region": region,
                        "value": tx.scalar_value,
                        "decoded": tx.decoded_value,
                        "count": tx.poll_count,
                        "timestamp_start": tx.timestamp_start,
                        "timestamp_end": tx.timestamp_end,
                    }
                )
            continue

        if region in _TX_REGIONS:
            _apply_tx_state(state["tx"], tx)
            continue

        if region in _RX_REGIONS:
            _apply_rx_state(state["rx"], tx)
            continue

        if region in _IRM_REGIONS:
            _apply_irm_state(state["irm"], tx)
            continue

        if region in {"0001.0000.0000", "00ff.0000.d1cc"}:
            state["completion"]["async_writes"].append(
                {
                    "region": region,
                    "register": tx.register,
                    "value": tx.request_value,
                    "decoded": tx.decoded_value or _hex_or_empty(tx.request_value),
                    "timestamp": tx.timestamp_start,
                }
            )
            continue

        if region.startswith("ffff.e020."):
            state["tcat_extended"]["regions"].setdefault(region, {"count": 0, "sizes": []})
            state["tcat_extended"]["regions"][region]["count"] += 1
            if tx.size is not None:
                state["tcat_extended"]["regions"][region]["sizes"].append(tx.size)
            if region == "ffff.e020.0d24" and tx.response_value is not None:
                state["tcat_extended"]["playlist_count"] = tx.response_value

    return _normalize_state_for_output(state)


def _build_phases(transactions: list[SemanticTransaction], state: dict[str, Any]) -> list[PhaseRecord]:
    phases: list[PhaseRecord] = [
        PhaseRecord(
            index=0,
            kind=PHASE_BUS_RESET,
            transaction_indexes=[],
            summary="session begins at the last bus reset before final enable",
            details={"kind": PHASE_BUS_RESET},
        )
    ]
    current_kind: str | None = None
    current_indexes: list[int] = []
    clock_seen = False

    def flush() -> None:
        nonlocal current_kind, current_indexes
        if current_kind is None:
            return
        txs = [transactions[idx] for idx in current_indexes]
        summary, details = _summarize_phase(current_kind, txs, state)
        phases.append(
            PhaseRecord(
                index=len(phases),
                kind=current_kind,
                transaction_indexes=current_indexes[:],
                summary=summary,
                details=details,
            )
        )
        current_kind = None
        current_indexes = []

    for tx in transactions:
        kind = _phase_kind_for_transaction(tx, clock_seen)
        if tx.region == "ffff.e000.0074" and tx.direction == "write":
            clock_seen = True
        if kind is None:
            continue
        if current_kind == kind:
            current_indexes.append(tx.index)
            continue
        flush()
        current_kind = kind
        current_indexes = [tx.index]

    flush()
    return phases


def _collect_unknown_regions(transactions: list[SemanticTransaction]) -> list[dict[str, Any]]:
    groups: dict[tuple[str, str], dict[str, Any]] = {}
    for tx in transactions:
        region = tx.region
        if not region:
            continue
        if tx.register != region:
            continue
        payload = tx.payload
        if payload:
            fingerprint = hashlib.sha1(payload).hexdigest()[:12]
            preview = payload[:16].hex()
        else:
            fingerprint = _hex_or_empty(tx.scalar_value) or "none"
            preview = _hex_or_empty(tx.scalar_value)
        key = (region, fingerprint)
        entry = groups.setdefault(
            key,
            {
                "address": region,
                "fingerprint": fingerprint,
                "count": 0,
                "transfer_sizes": [],
                "preview": preview,
                "directions": set(),
            },
        )
        entry["count"] += tx.poll_count
        entry["directions"].add(tx.direction)
        if tx.size is not None:
            entry["transfer_sizes"].append(tx.size)

    result = []
    for entry in groups.values():
        result.append(
            {
                "address": entry["address"],
                "fingerprint": entry["fingerprint"],
                "count": entry["count"],
                "transfer_sizes": sorted(set(entry["transfer_sizes"])),
                "preview": entry["preview"],
                "directions": sorted(entry["directions"]),
            }
        )
    result.sort(key=lambda item: (item["address"], item["fingerprint"]))
    return result


def _compare_phases(reference: SessionAnalysis, current: SessionAnalysis) -> list[PhaseComparison]:
    ref_phases = reference.phases
    cur_phases = current.phases
    ref_keys = [phase.kind for phase in ref_phases]
    cur_keys = [phase.kind for phase in cur_phases]
    m, n = len(ref_keys), len(cur_keys)
    dp = [[0] * (n + 1) for _ in range(m + 1)]
    for i in range(m - 1, -1, -1):
        for j in range(n - 1, -1, -1):
            if ref_keys[i] == cur_keys[j]:
                dp[i][j] = dp[i + 1][j + 1] + 1
            else:
                dp[i][j] = max(dp[i + 1][j], dp[i][j + 1])

    comparisons: list[PhaseComparison] = []
    i = 0
    j = 0
    while i < m or j < n:
        if i < m and j < n and ref_keys[i] == cur_keys[j]:
            classification = _classify_phase_pair(ref_phases[i], cur_phases[j], reference.state, current.state)
            comparisons.append(
                PhaseComparison(
                    index=len(comparisons),
                    kind=ref_phases[i].kind,
                    classification=classification,
                    reference_phase_index=ref_phases[i].index,
                    current_phase_index=cur_phases[j].index,
                    reference_summary=ref_phases[i].summary,
                    current_summary=cur_phases[j].summary,
                )
            )
            i += 1
            j += 1
            continue
        if j >= n or (i < m and dp[i + 1][j] >= dp[i][j + 1]):
            comparisons.append(
                PhaseComparison(
                    index=len(comparisons),
                    kind=ref_phases[i].kind,
                    classification="missing",
                    reference_phase_index=ref_phases[i].index,
                    current_phase_index=None,
                    reference_summary=ref_phases[i].summary,
                    current_summary=None,
                )
            )
            i += 1
        else:
            comparisons.append(
                PhaseComparison(
                    index=len(comparisons),
                    kind=cur_phases[j].kind,
                    classification="extra",
                    reference_phase_index=None,
                    current_phase_index=cur_phases[j].index,
                    reference_summary=None,
                    current_summary=cur_phases[j].summary,
                )
            )
            j += 1
    return comparisons


def _build_state_diffs(reference: dict[str, Any], current: dict[str, Any]) -> dict[str, Any]:
    return {
        "global": _domain_diff(reference["global"], current["global"], _format_global_state),
        "tx": _domain_diff(reference["tx"], current["tx"], _format_stream_state),
        "rx": _domain_diff(reference["rx"], current["rx"], _format_stream_state),
        "irm": _domain_diff(reference["irm"], current["irm"], _format_irm_state),
        "tcat_extended": _domain_diff(reference["tcat_extended"], current["tcat_extended"], _format_tcat_state),
        "unknown_regions": {
            "reference_count": len(reference.get("unknown_regions", [])),
            "current_count": len(current.get("unknown_regions", [])),
        },
    }


def _compare_unknown_regions(
    reference_unknown: list[dict[str, Any]],
    current_unknown: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    reference_map = {(entry["address"], entry["fingerprint"]): entry for entry in reference_unknown}
    current_map = {(entry["address"], entry["fingerprint"]): entry for entry in current_unknown}
    keys = sorted(set(reference_map) | set(current_map))
    result = []
    for key in keys:
        result.append(
            {
                "address": key[0],
                "fingerprint": key[1],
                "reference": reference_map.get(key),
                "current": current_map.get(key),
                "classification": _pair_classification(reference_map.get(key), current_map.get(key)),
            }
        )
    return result


def _build_findings(
    reference: SessionAnalysis,
    current: SessionAnalysis,
    phases: list[PhaseComparison],
    state_diffs: dict[str, Any],
    unknown_regions: list[dict[str, Any]],
) -> list[Finding]:
    findings: list[Finding] = []

    ref_programs = _phase_indices_for(reference.phases, {PHASE_TX, PHASE_RX})
    ref_irm_before_program = any(
        any(reference.phases[idx].kind == PHASE_IRM for idx in range(phase_idx))
        for phase_idx in ref_programs
    )
    current_has_irm = any(phase.kind == PHASE_IRM for phase in current.phases)
    if ref_irm_before_program and not current_has_irm:
        findings.append(
            Finding(
                severity="high",
                title="Missing IRM reservation before stream programming",
                why="Reference reserves bandwidth/channels before programming isoch streams; skipping IRM allocation can break compliance or collide on a busy bus.",
                reference="Reference performs IRM reads/locks before RX/TX programming.",
                current="Current session programs RX/TX channels without any IRM reservation phase.",
                phase=PHASE_IRM,
                phase_indexes=_phase_comparison_indexes(phases, PHASE_IRM),
            )
        )

    tx_ref = _final_stream_program(reference.state["tx"])
    tx_cur = _final_stream_program(current.state["tx"])
    rx_ref = _final_stream_program(reference.state["rx"])
    rx_cur = _final_stream_program(current.state["rx"])
    channel_diffs = []
    if tx_ref.get("iso_channel") != tx_cur.get("iso_channel"):
        channel_diffs.append(
            f"TX iso channel ref={_render_iso(tx_ref.get('iso_channel'))} current={_render_iso(tx_cur.get('iso_channel'))}"
        )
    if rx_ref.get("iso_channel") != rx_cur.get("iso_channel"):
        channel_diffs.append(
            f"RX iso channel ref={_render_iso(rx_ref.get('iso_channel'))} current={_render_iso(rx_cur.get('iso_channel'))}"
        )
    if channel_diffs:
        findings.append(
            Finding(
                severity="high",
                title="Programmed stream channels differ at enable time",
                why="Mismatched programmed isoch channels point to a real behavior change even if the rest of init looks similar.",
                reference="Reference final stream programming sets the expected TX/RX channels before enable.",
                current="; ".join(channel_diffs),
                phase=PHASE_ENABLE,
                phase_indexes=_phase_comparison_indexes(phases, PHASE_ENABLE),
            )
        )

    current_has_enable = any(phase.kind == PHASE_ENABLE for phase in current.phases)
    current_has_clock = any(phase.kind == PHASE_CLOCK for phase in current.phases)
    current_has_tx = any(phase.kind == PHASE_TX for phase in current.phases)
    current_has_rx = any(phase.kind == PHASE_RX for phase in current.phases)
    if current_has_enable and not (current_has_clock and current_has_tx and current_has_rx):
        findings.append(
            Finding(
                severity="high",
                title="Enable occurs before required prerequisites",
                why="Enabling the device before the clock and stream programming steps complete can leave the device in an undefined state.",
                reference="Reference reaches clock select and both TX/RX programming phases before final enable.",
                current="Current session reaches enable without all prerequisite phases present.",
                phase=PHASE_ENABLE,
                phase_indexes=_phase_comparison_indexes(phases, PHASE_ENABLE),
            )
        )

    ref_strategy = _completion_strategy(reference.state)
    cur_strategy = _completion_strategy(current.state)
    if ref_strategy != cur_strategy:
        findings.append(
            Finding(
                severity="medium",
                title="Completion strategy differs from reference",
                why="Clock-select completion can be surfaced through async notify writes, notification latches, or polling. A different mechanism often explains why later sequencing diverges.",
                reference=f"Reference completion path: {ref_strategy}.",
                current=f"Current completion path: {cur_strategy}.",
                phase=PHASE_WAIT,
                phase_indexes=_phase_comparison_indexes(phases, PHASE_WAIT),
            )
        )

    partial_notes = []
    ref_global_max = reference.state["global"]["coverage"].get("max_read_size", 0)
    cur_global_max = current.state["global"]["coverage"].get("max_read_size", 0)
    if ref_global_max and cur_global_max and cur_global_max < ref_global_max:
        partial_notes.append(f"global block read {cur_global_max}B vs reference {ref_global_max}B")
    if partial_notes:
        findings.append(
            Finding(
                severity="medium",
                title="Current trace leaves reference-visible state undiscovered",
                why="Smaller block reads can hide fields that the reference init path uses to validate or verify device state.",
                reference="Reference reads a larger global state block.",
                current=", ".join(partial_notes),
                phase=PHASE_GLOBAL,
                phase_indexes=_phase_comparison_indexes(phases, PHASE_GLOBAL),
            )
        )

    ref_cfg = reference.state["configrom"]["values"]
    cur_cfg = current.state["configrom"]["values"]
    overlap = set(ref_cfg) & set(cur_cfg)
    extras = sorted(set(cur_cfg) - set(ref_cfg))
    if extras and all(ref_cfg[key] == cur_cfg[key] for key in overlap):
        findings.append(
            Finding(
                severity="info",
                title="Current trace performs deeper Config ROM probing",
                why="Extra Config ROM reads are usually harmless if the overlapping probe values match the reference.",
                reference=f"Reference probes {len(ref_cfg)} Config ROM offsets.",
                current=f"Current probes {len(cur_cfg)} offsets, including extras: {', '.join(_short_region(region) for region in extras[:6])}.",
                phase=PHASE_CONFIGROM,
                phase_indexes=_phase_comparison_indexes(phases, PHASE_CONFIGROM),
            )
        )

    ref_tcat = set(reference.state["tcat_extended"]["regions"])
    cur_tcat = set(current.state["tcat_extended"]["regions"])
    extra_tcat = sorted(cur_tcat - ref_tcat)
    if extra_tcat:
        findings.append(
            Finding(
                severity="info",
                title="Current trace performs extra TCAT discovery",
                why="Additional TCAT extension discovery changes the trace shape but is not automatically a functional bug.",
                reference="Reference does not read these TCAT extension regions in the analyzed session.",
                current=f"Current touches: {', '.join(_short_region(region) for region in extra_tcat[:6])}.",
                phase=PHASE_TCAT,
                phase_indexes=_phase_comparison_indexes(phases, PHASE_TCAT),
            )
        )

    repeated_polls: list[dict[str, Any]] = []
    for phase in current.phases:
        if phase.kind != PHASE_WAIT:
            continue
        repeated_polls.extend(
            step
            for step in phase.details.get("steps", [])
            if step.get("kind") == "poll" and step.get("poll_count", 1) > 1
        )
    if repeated_polls:
        poll = repeated_polls[0]
        findings.append(
            Finding(
                severity="info",
                title="Current repeats identical notification polls",
                why="Repeated stable notification polls are usually a wait loop rather than meaningful state change.",
                reference="Reference does not show the same repeated stable polling pattern in the analyzed session.",
                current=(
                    f"Current polls GLOBAL_NOTIFICATION {poll['poll_count']}x and stays at "
                    f"{poll['decoded'] or poll['value_hex']}."
                ),
                phase=PHASE_WAIT,
                phase_indexes=_phase_comparison_indexes(phases, PHASE_WAIT),
            )
        )

    current_only_unknown = [entry for entry in unknown_regions if entry["classification"] == "extra"]
    if current_only_unknown:
        sample = current_only_unknown[0]
        findings.append(
            Finding(
                severity="info",
                title="Current touches unknown regions",
                why="Grouping unknown payload fingerprints helps prioritize new decoders without losing the trace evidence.",
                reference="Reference does not access this unknown region in the analyzed session.",
                current=f"{sample['address']} fingerprint={sample['fingerprint']}.",
                phase=PHASE_TCAT,
                phase_indexes=_phase_comparison_indexes(phases, PHASE_TCAT),
            )
        )

    findings.sort(key=lambda finding: (SEVERITY_ORDER[finding.severity], finding.title))
    return findings


def _compare_strict_phase0(
    reference: StrictPhase0Analysis,
    current: StrictPhase0Analysis,
) -> tuple[StrictPhase0Failure | None, list[str]]:
    warnings: list[str] = []

    if reference.pre_state != current.pre_state:
        return (
            StrictPhase0Failure(
                code="pre_state_mismatch",
                message=(
                    "Initial device pre-state differs before ownership claim. "
                    "Phase-0 parity must start from the same reset-derived state."
                ),
            ),
            warnings,
        )

    if current.unexpected_state_changes:
        return (
            StrictPhase0Failure(
                code="unexpected_state_change",
                message="Current trace contains an extra state-changing write/lock outside the allowed phase-0 contract.",
                current_step=current.unexpected_state_changes[0],
            ),
            warnings,
        )

    if current.unexpected_noise:
        return (
            StrictPhase0Failure(
                code="unexpected_read_noise",
                message="Current trace contains extra phase-0 reads outside the strict whitelist.",
                current_step=current.unexpected_noise[0],
            ),
            warnings,
        )

    rx_delay = _detect_rx_programming_delay(current.core_steps)
    if rx_delay is not None:
        warnings.append(rx_delay.message)

    for index, reference_step in enumerate(reference.core_steps):
        if index >= len(current.core_steps):
            return (
                StrictPhase0Failure(
                    code="missing_step",
                    message="Current trace ends before the full phase-0 control-plane contract is satisfied.",
                    reference_step=reference_step,
                ),
                warnings,
            )

        current_step = current.core_steps[index]
        if _strict_step_signature(reference_step) != _strict_step_signature(current_step):
            return (
                rx_delay
                if rx_delay is not None
                else StrictPhase0Failure(
                    code="state_changing_mismatch",
                    message="State-changing phase-0 sequence differs from the golden reference.",
                    reference_step=reference_step,
                    current_step=current_step,
                ),
                warnings,
            )

    if len(current.core_steps) > len(reference.core_steps):
        return (
            StrictPhase0Failure(
                code="extra_step",
                message="Current trace contains extra phase-0 contract steps after the reference sequence completes.",
                current_step=current.core_steps[len(reference.core_steps)],
            ),
            warnings,
        )

    return (None, warnings)


def _detect_rx_programming_delay(steps: list[StrictPhase0Step]) -> StrictPhase0Failure | None:
    rx_index = next((index for index, step in enumerate(steps) if step.region in {"ffff.e000.03e0", "ffff.e000.03e4", "ffff.e000.03e8"}), None)
    if rx_index is None:
        return None

    irm_locks_before_rx = sum(
        1
        for step in steps[:rx_index]
        if step.direction == "lock" and step.region in _IRM_REGIONS
    )
    tx_programming_before_rx = any(
        step.region in {"ffff.e000.01a8", "ffff.e000.01ac", "ffff.e000.01b8", "ffff.e000.0078"}
        for step in steps[:rx_index]
    )
    if irm_locks_before_rx > 2 or tx_programming_before_rx:
        return StrictPhase0Failure(
            code="rx_programming_delayed",
            message=(
                "RX programming was delayed past the first IRM allocation block. "
                "Reference programs RX immediately after reserving playback resources."
            ),
            current_step=steps[rx_index],
        )
    return None


def _extract_strict_pre_state(transactions: list[SemanticTransaction]) -> dict[str, Any]:
    fields: dict[str, Any] = {}
    for tx in transactions:
        if tx.region == "ffff.e000.0028" and tx.direction == "lock":
            break
        if tx.direction != "read" or tx.region is None:
            continue
        if tx.region == "ffff.e000.0028" and tx.payload:
            fields.update(_extract_global_fields(tx.payload))
        elif tx.region in _GLOBAL_FIELD_REGIONS:
            _apply_global_scalar(fields, tx.region, tx.scalar_value, tx.payload)

    return {
        "owner": fields.get("owner"),
        "clock_select": fields.get("clock_select"),
        "notification": fields.get("notification"),
        "status": fields.get("status"),
        "sample_rate": fields.get("sample_rate"),
    }


def _is_strict_core_transaction(tx: SemanticTransaction) -> bool:
    region = tx.region
    if region is None:
        return False
    if region in _IRM_REGIONS:
        return True
    if region == "ffff.e000.0028":
        return tx.direction == "lock" or (tx.direction == "read" and (tx.size or 0) <= 16)
    return region in {
        "ffff.e000.0074",
        "ffff.e000.0078",
        "ffff.e000.01a8",
        "ffff.e000.01ac",
        "ffff.e000.01b8",
        "ffff.e000.03e0",
        "ffff.e000.03e4",
        "ffff.e000.03e8",
    }


def _is_allowed_strict_noise_transaction(tx: SemanticTransaction) -> bool:
    if tx.direction == "read":
        return (tx.region or "") in {
            "ffff.e000.0030",
            "ffff.e000.007c",
            "ffff.e000.0084",
        }
    return tx.region in {"0001.0000.0000", "00ff.0000.d1cc"}


def _strict_step_from_transaction(tx: SemanticTransaction) -> StrictPhase0Step:
    return StrictPhase0Step(
        index=tx.index,
        region=tx.region or "",
        direction=tx.direction,
        request_kind=tx.request_kind,
        response_kind=tx.response_kind,
        size=tx.size,
        request_value=tx.request_value,
        response_value=tx.response_value,
        request_payload_hex=tx.request_payload.hex() if tx.request_payload else None,
        response_payload_hex=tx.response_payload.hex() if tx.response_payload else None,
        summary=_strict_step_summary(tx),
    )


def _strict_step_summary(tx: SemanticTransaction) -> str:
    region = tx.region or "<unknown>"
    if tx.direction == "lock":
        payload_desc = tx.request_payload.hex() if tx.request_payload else _hex_or_empty(tx.request_value)
        return f"lock {_short_region(region)} payload={payload_desc}"
    if tx.direction == "write":
        return f"write {_short_region(region)} = {tx.decoded_value or _hex_or_empty(tx.request_value)}"
    if tx.direction == "read":
        if tx.payload:
            return f"read {_short_region(region)} ({tx.size or len(tx.payload)}B)"
        return f"read {_short_region(region)} -> {tx.decoded_value or _hex_or_empty(tx.response_value)}"
    return f"{tx.kind_label} {_short_region(region)}"


def _strict_noise_summary(tx: SemanticTransaction) -> str:
    region = tx.region or "<unknown>"
    return f"{tx.direction} {_short_region(region)}"


def _strict_step_signature(step: StrictPhase0Step) -> tuple[Any, ...]:
    return (
        step.direction,
        step.request_kind,
        step.response_kind,
        step.region,
        step.size,
        step.request_value,
        step.response_value,
        step.request_payload_hex,
        step.response_payload_hex,
    )


def render_text_report(comparison: SemanticComparison, sections: list[str] | None = None) -> str:
    """Render a human-readable semantic comparison report."""
    sections = sections or ["findings", "phases", "state"]
    lines: list[str] = []
    lines.extend(_render_session_summary(comparison))

    if "findings" in sections:
        lines.append("")
        lines.append("Findings")
        lines.append("--------")
        if not comparison.findings:
            lines.append("No findings.")
        for finding in comparison.findings:
            lines.append(f"[{finding.severity.upper()}] {finding.title}")
            lines.append(f"  Why: {finding.why}")
            lines.append(f"  Reference: {finding.reference}")
            lines.append(f"  Current: {finding.current}")
            lines.append(f"  Phase: {finding.phase}")

    if "phases" in sections:
        lines.append("")
        lines.append("Phase Timeline")
        lines.append("--------------")
        for phase in comparison.phases:
            lines.append(f"{phase.classification.upper():<10} {phase.kind}")
            if phase.reference_summary:
                lines.append(f"  Reference: {phase.reference_summary}")
            if phase.current_summary:
                lines.append(f"  Current:   {phase.current_summary}")

    if "state" in sections:
        lines.append("")
        lines.append("State Differences")
        lines.append("-----------------")
        lines.extend(_render_state_diffs(comparison.state_diffs))

    if "appendix" in sections:
        lines.append("")
        lines.append("Appendix")
        lines.append("--------")
        lines.extend(_render_appendix(comparison))

    return "\n".join(lines).rstrip() + "\n"


def render_strict_phase0_text_report(comparison: StrictPhase0Comparison) -> str:
    lines = [
        "Strict Phase-0 Parity",
        "---------------------",
        f"Reference: {comparison.metadata['reference_label']}",
        f"Current:   {comparison.metadata['current_label']}",
        f"Status:    {'PASS' if comparison.passed else 'FAIL'}",
        "",
        "Pre-state",
        "---------",
        f"Reference: {comparison.reference.pre_state}",
        f"Current:   {comparison.current.pre_state}",
        "",
        "MUST MATCH",
        "----------",
    ]
    for item in comparison.must_match:
        lines.append(f"- {item}")

    lines.extend(["", "MAY DIFFER", "----------"])
    for item in comparison.may_differ:
        lines.append(f"- {item}")

    if comparison.warnings:
        lines.extend(["", "Warnings", "--------"])
        for warning in comparison.warnings:
            lines.append(f"- {warning}")

    lines.extend(["", "FAIL PARITY", "-----------"])
    if comparison.failure is None:
        lines.append("No parity failures.")
    else:
        lines.append(f"{comparison.failure.code}: {comparison.failure.message}")
        if comparison.failure.reference_step is not None:
            lines.append(f"Reference step: {comparison.failure.reference_step.summary}")
        if comparison.failure.current_step is not None:
            lines.append(f"Current step:   {comparison.failure.current_step.summary}")

    return "\n".join(lines).rstrip() + "\n"


def render_json_report(comparison: SemanticComparison) -> dict[str, Any]:
    """Return a stable JSON-ready dictionary for semantic comparison."""
    return {
        "metadata": comparison.metadata,
        "reference": _session_to_json(comparison.reference),
        "current": _session_to_json(comparison.current),
        "summary": {
            "finding_counts": {
                severity: sum(1 for finding in comparison.findings if finding.severity == severity)
                for severity in ("high", "medium", "info")
            },
            "phase_counts": {
                "reference": len(comparison.reference.phases),
                "current": len(comparison.current.phases),
            },
        },
        "findings": [
            {
                "severity": finding.severity,
                "title": finding.title,
                "why": finding.why,
                "reference": finding.reference,
                "current": finding.current,
                "phase": finding.phase,
                "phase_indexes": finding.phase_indexes,
            }
            for finding in comparison.findings
        ],
        "phases": [
            {
                "index": phase.index,
                "kind": phase.kind,
                "classification": phase.classification,
                "reference_phase_index": phase.reference_phase_index,
                "current_phase_index": phase.current_phase_index,
                "reference_summary": phase.reference_summary,
                "current_summary": phase.current_summary,
            }
            for phase in comparison.phases
        ],
        "state_diffs": comparison.state_diffs,
        "unknown_regions": comparison.unknown_regions,
    }


def render_strict_phase0_json_report(comparison: StrictPhase0Comparison) -> dict[str, Any]:
    return {
        "metadata": comparison.metadata,
        "passed": comparison.passed,
        "pre_state": {
            "reference": comparison.reference.pre_state,
            "current": comparison.current.pre_state,
        },
        "must_match": comparison.must_match,
        "may_differ": comparison.may_differ,
        "warnings": comparison.warnings,
        "failure": None
        if comparison.failure is None
        else {
            "code": comparison.failure.code,
            "message": comparison.failure.message,
            "reference_step": None
            if comparison.failure.reference_step is None
            else comparison.failure.reference_step.__dict__,
            "current_step": None
            if comparison.failure.current_step is None
            else comparison.failure.current_step.__dict__,
        },
    }


def load_and_compare_init(
    reference_path: str | Path,
    current_path: str | Path,
) -> SemanticComparison:
    """Load two logs from disk and compare them semantically."""
    from .log_parser import parse_log

    reference_text = Path(reference_path).read_text(encoding="utf-8", errors="replace")
    current_text = Path(current_path).read_text(encoding="utf-8", errors="replace")
    return compare_init_logs(
        parse_log(reference_text),
        parse_log(current_text),
        Path(reference_path).name,
        Path(current_path).name,
    )


def load_and_compare_init_strict_phase0(
    reference_path: str | Path,
    current_path: str | Path,
) -> StrictPhase0Comparison:
    from .log_parser import parse_log

    reference_text = Path(reference_path).read_text(encoding="utf-8", errors="replace")
    current_text = Path(current_path).read_text(encoding="utf-8", errors="replace")
    return compare_init_logs_strict_phase0(
        parse_log(reference_text),
        parse_log(current_text),
        Path(reference_path).name,
        Path(current_path).name,
    )


def _phase_kind_for_transaction(tx: SemanticTransaction, clock_seen: bool) -> str | None:
    region = tx.region
    if region is None:
        return None
    if region.startswith("ffff.f000.04"):
        return PHASE_CONFIGROM
    if region == "ffff.e000.0000":
        return PHASE_LAYOUT
    if region == "ffff.e000.0028" and tx.direction == "lock":
        return PHASE_OWNER
    if region == "ffff.e000.0028":
        return PHASE_OWNER if tx.size in {8, 16, 104, 380} and tx.direction == "read" and not clock_seen else PHASE_GLOBAL
    if region == "ffff.e000.0074" and tx.direction == "write":
        return PHASE_CLOCK
    if region == "ffff.e000.0078":
        return PHASE_ENABLE
    if region in _WAIT_REGIONS and clock_seen:
        return PHASE_WAIT
    if region in _GLOBAL_FIELD_REGIONS:
        return PHASE_GLOBAL
    if region in _IRM_REGIONS:
        return PHASE_IRM
    if region in {"ffff.e000.03e4", "ffff.e000.03e8"} and tx.direction == "write":
        return PHASE_RX
    if region in {"ffff.e000.01ac", "ffff.e000.01b8"} and tx.direction == "write":
        return PHASE_TX
    if region in _TX_REGIONS or region in _RX_REGIONS:
        return PHASE_STREAM
    if region.startswith("ffff.e020."):
        return PHASE_TCAT
    return None


def _summarize_phase(kind: str, transactions: list[SemanticTransaction], state: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    if kind == PHASE_CONFIGROM:
        offsets = sorted(tx.region for tx in transactions if tx.region)
        summary = f"probe {len(offsets)} Config ROM reads"
        return summary, {"offsets": offsets}
    if kind == PHASE_LAYOUT:
        layout = state["layout"]
        if layout:
            summary = (
                f"read section layout: global={layout.get('DICE_GLOBAL_SIZE', '?') * 4 if layout.get('DICE_GLOBAL_SIZE') else '?'}B, "
                f"tx_stride={state['tx'].get('size_quadlets') or '?'}q, rx_stride={state['rx'].get('size_quadlets') or '?'}q"
            )
            return summary, {"layout": layout}
        return "read DICE section layout", {}
    if kind == PHASE_OWNER:
        claims = state["global"]["owner_claims"]
        if claims:
            claim = claims[-1]
            summary = f"owner claim CAS old={claim['old']} new={claim['new']}"
            return summary, {"owner_claims": claims}
        sizes = [tx.size for tx in transactions if tx.size is not None]
        return f"owner/state reads ({', '.join(str(size) + 'B' for size in sizes)})", {"sizes": sizes}
    if kind == PHASE_CLOCK:
        tx = transactions[-1]
        return f"write GLOBAL_CLOCK_SELECT = {tx.decoded_value or _hex_or_empty(tx.request_value)}", {
            "value": tx.request_value,
            "decoded": tx.decoded_value,
        }
    if kind == PHASE_WAIT:
        steps = _collapse_wait_steps(transactions)
        summary_parts = []
        for step in steps:
            if step["kind"] == "poll":
                summary_parts.append(
                    f"poll {step['register']} {step['poll_count']}x => {step['decoded'] or step['value_hex']}"
                )
            else:
                summary_parts.append(
                    f"async write {step['register']} = {step['decoded'] or step['value_hex']}"
                )
        summary = "; ".join(summary_parts) if summary_parts else "wait for completion"
        return summary, {"steps": steps}
    if kind == PHASE_STREAM:
        tx_regs = sorted({tx.register for tx in transactions if tx.register})
        tx_stream = _format_stream_short(state["tx"])
        rx_stream = _format_stream_short(state["rx"])
        summary = f"discover streams ({', '.join(tx_regs[:4])}) => TX {tx_stream}; RX {rx_stream}"
        return summary, {"registers": tx_regs}
    if kind == PHASE_IRM:
        allocations = state["irm"]["allocations"]
        summary = f"IRM reservations {len(allocations)} lock ops"
        return summary, {"allocations": allocations}
    if kind == PHASE_RX:
        stream = _final_stream_program(state["rx"])
        summary = f"program RX[0] = {_render_iso(stream.get('iso_channel'))}, seq={stream.get('seq_start', 0)}"
        return summary, {"stream": stream}
    if kind == PHASE_TX:
        stream = _final_stream_program(state["tx"])
        summary = f"program TX[0] = {_render_iso(stream.get('iso_channel'))}, speed={stream.get('speed') or '?'}"
        return summary, {"stream": stream}
    if kind == PHASE_TCAT:
        regions = sorted({tx.region for tx in transactions if tx.region})
        summary = f"read {len(regions)} TCAT regions: {', '.join(_short_region(region) for region in regions[:4])}"
        return summary, {"regions": regions}
    if kind == PHASE_ENABLE:
        tx = transactions[-1]
        enabled = "True" if tx.request_value == 1 else "False"
        return f"write GLOBAL_ENABLE = {enabled}", {"value": tx.request_value}
    if kind == PHASE_GLOBAL:
        fields = state["global"]["fields"]
        summary = (
            f"read global state clock={fields.get('clock_select', '?')}, "
            f"notify={fields.get('notification', '?')}, rate={fields.get('sample_rate', '?')}"
        )
        return summary, {"fields": fields}
    return f"{kind} ({len(transactions)} ops)", {}


def _collapse_wait_steps(transactions: list[SemanticTransaction]) -> list[dict[str, Any]]:
    steps: list[dict[str, Any]] = []
    for tx in transactions:
        if tx.region == "ffff.e000.0030" and tx.direction == "read":
            value = tx.scalar_value
            decoded = tx.decoded_value
            value_hex = _hex_or_empty(value)
            if steps and steps[-1]["kind"] == "poll" and steps[-1]["value"] == value:
                steps[-1]["poll_count"] += tx.poll_count
                steps[-1]["timestamp_end"] = tx.timestamp_end
                continue
            steps.append(
                {
                    "kind": "poll",
                    "register": tx.register,
                    "value": value,
                    "value_hex": value_hex,
                    "decoded": decoded,
                    "poll_count": tx.poll_count,
                    "timestamp_start": tx.timestamp_start,
                    "timestamp_end": tx.timestamp_end,
                }
            )
            continue
        steps.append(
            {
                "kind": "notify_write",
                "register": tx.register,
                "value": tx.request_value,
                "value_hex": _hex_or_empty(tx.request_value),
                "decoded": tx.decoded_value,
                "timestamp_start": tx.timestamp_start,
                "timestamp_end": tx.timestamp_end,
            }
        )
    return steps


def _classify_phase_pair(
    reference_phase: PhaseRecord,
    current_phase: PhaseRecord,
    reference_state: dict[str, Any],
    current_state: dict[str, Any],
) -> str:
    kind = reference_phase.kind
    if reference_phase.summary == current_phase.summary:
        return "match"
    if kind == PHASE_CONFIGROM:
        ref_offsets = set(reference_phase.details.get("offsets", []))
        cur_offsets = set(current_phase.details.get("offsets", []))
        ref_cfg = reference_state["configrom"]["values"]
        cur_cfg = current_state["configrom"]["values"]
        overlap = ref_offsets & cur_offsets
        if overlap and all(ref_cfg.get(key) == cur_cfg.get(key) for key in overlap) and (
            ref_offsets <= cur_offsets or cur_offsets <= ref_offsets
        ):
            return "equivalent"
        return "different"
    if kind == PHASE_STREAM:
        if _stream_signature(_final_stream_program(reference_state["tx"])) == _stream_signature(
            _final_stream_program(current_state["tx"])
        ) and _stream_signature(_final_stream_program(reference_state["rx"])) == _stream_signature(
            _final_stream_program(current_state["rx"])
        ):
            return "equivalent"
        return "different"
    if kind == PHASE_TCAT:
        ref_regions = set(reference_phase.details.get("regions", []))
        cur_regions = set(current_phase.details.get("regions", []))
        if ref_regions <= cur_regions or cur_regions <= ref_regions:
            return "equivalent"
        return "different"
    if kind in {PHASE_TX, PHASE_RX, PHASE_CLOCK, PHASE_ENABLE, PHASE_OWNER, PHASE_LAYOUT}:
        return "different"
    if kind == PHASE_WAIT:
        return "different"
    if kind == PHASE_GLOBAL:
        ref_fields = reference_state["global"]["fields"]
        cur_fields = current_state["global"]["fields"]
        overlap = {key for key in ref_fields if key in cur_fields}
        if overlap and all(ref_fields[key] == cur_fields[key] for key in overlap):
            return "equivalent"
        return "different"
    if kind == PHASE_IRM:
        if reference_state["irm"]["allocations"] == current_state["irm"]["allocations"]:
            return "match"
        return "different"
    return "different"


def _domain_diff(reference: Any, current: Any, formatter: Any) -> dict[str, Any]:
    return {
        "reference": formatter(reference),
        "current": formatter(current),
        "equal": reference == current,
    }


def _render_session_summary(comparison: SemanticComparison) -> list[str]:
    ref_window = comparison.reference.window
    cur_window = comparison.current.window
    return [
        "Session Summary",
        "---------------",
        f"Analyzed the last session ending at GLOBAL_ENABLE=1 for {comparison.metadata['reference_label']} and {comparison.metadata['current_label']}.",
        f"Reference: events {ref_window.start_index}-{ref_window.end_index}, {len(comparison.reference.transactions)} merged transactions, {len(comparison.reference.phases)} phases.",
        f"Current:   events {cur_window.start_index}-{cur_window.end_index}, {len(comparison.current.transactions)} merged transactions, {len(comparison.current.phases)} phases.",
    ]


def _render_state_diffs(state_diffs: dict[str, Any]) -> list[str]:
    lines: list[str] = []
    for domain in ("global", "tx", "rx", "irm", "tcat_extended"):
        diff = state_diffs[domain]
        lines.append(f"{domain}:")
        lines.append(f"  Reference: {json.dumps(diff['reference'], sort_keys=True)}")
        lines.append(f"  Current:   {json.dumps(diff['current'], sort_keys=True)}")
        lines.append(f"  Equal:     {diff['equal']}")
    return lines


def _render_appendix(comparison: SemanticComparison) -> list[str]:
    lines: list[str] = []
    if comparison.unknown_regions:
        lines.append("Unknown regions:")
        for entry in comparison.unknown_regions:
            lines.append(
                f"  {entry['classification'].upper():<8} {entry['address']} fingerprint={entry['fingerprint']}"
            )
    else:
        lines.append("No unknown regions.")
    return lines


def _session_to_json(session: SessionAnalysis) -> dict[str, Any]:
    return {
        "label": session.label,
        "session": {
            "start_index": session.window.start_index,
            "end_index": session.window.end_index,
            "bus_reset_timestamp": session.window.bus_reset_timestamp,
            "enable_timestamp": session.window.enable_timestamp,
            "used_enable_one": session.window.used_enable_one,
        },
        "transaction_count": len(session.transactions),
        "phase_count": len(session.phases),
        "phases": [
            {
                "index": phase.index,
                "kind": phase.kind,
                "summary": phase.summary,
                "transaction_indexes": phase.transaction_indexes,
                "details": _json_ready(phase.details),
            }
            for phase in session.phases
        ],
        "state": _json_ready(session.state),
        "unknown_regions": session.unknown_regions,
    }


def _normalize_state_for_output(state: dict[str, Any]) -> dict[str, Any]:
    state["tx"]["streams"] = {
        str(index): _json_ready(stream)
        for index, stream in sorted(state["tx"]["streams"].items())
    }
    state["rx"]["streams"] = {
        str(index): _json_ready(stream)
        for index, stream in sorted(state["rx"]["streams"].items())
    }
    return _json_ready(state)


def _json_ready(value: Any) -> Any:
    if isinstance(value, dict):
        return {str(key): _json_ready(inner) for key, inner in value.items()}
    if isinstance(value, list):
        return [_json_ready(inner) for inner in value]
    if isinstance(value, set):
        return sorted(_json_ready(inner) for inner in value)
    return value


def _apply_global_scalar(fields: dict[str, Any], region: str, value: int | None, payload: bytes | None) -> None:
    if region == "ffff.e000.0030" and value is not None:
        fields["notification"] = _annotate_region(region, value)[1] or _hex_or_empty(value)
    elif region == "ffff.e000.0034" and payload:
        fields["nickname"] = unpack_label(payload[:64])
    elif region == "ffff.e000.0074" and value is not None:
        fields["clock_select"] = _annotate_region(region, value)[1]
    elif region == "ffff.e000.0078" and value is not None:
        fields["enable"] = bool(value)
    elif region == "ffff.e000.007c" and value is not None:
        fields["status"] = _annotate_region(region, value)[1]
    elif region == "ffff.e000.0080" and value is not None:
        fields["extended_status"] = _annotate_region(region, value)[1]
    elif region == "ffff.e000.0084" and value is not None:
        fields["sample_rate"] = _annotate_region(region, value)[1]
    elif region == "ffff.e000.0088" and value is not None:
        fields["version"] = _annotate_region(region, value)[1]
    elif region == "ffff.e000.008c" and value is not None:
        fields["clock_caps"] = _annotate_region(region, value)[1]
    elif region == "ffff.e000.0090" and payload:
        fields["clock_source_labels"] = _deserialize_labels(payload[:256])


def _apply_tx_state(state: dict[str, Any], tx: SemanticTransaction) -> None:
    region = tx.region
    if region is None:
        return
    if region == "ffff.e000.01a4" and tx.payload:
        state["coverage"]["block_sizes"].append(tx.size or len(tx.payload))
        fields = _extract_tx_fields(tx.payload)
        state["number"] = fields.get("number", state["number"])
        state["size_quadlets"] = fields.get("size_quadlets", state["size_quadlets"])
        for index, stream in fields.get("streams", {}).items():
            state["streams"].setdefault(index, {}).update(stream)
        state["coverage"]["field_keys"].update(fields.get("field_keys", set()))
        return
    if region == "ffff.e000.01bc" and tx.payload:
        labels = _deserialize_labels(tx.payload[:256])
        state["streams"].setdefault(0, {})["channel_names"] = labels
        state["coverage"]["field_keys"].add("streams.0.channel_names")
        return
    if region == "ffff.e000.01a8" and tx.scalar_value is not None:
        state["size_quadlets"] = tx.scalar_value
        state["coverage"]["field_keys"].add("size_quadlets")
    elif region == "ffff.e000.01ac" and tx.scalar_value is not None:
        state["streams"].setdefault(0, {})["iso_channel"] = _normalize_iso_channel(tx.scalar_value)
        state["coverage"]["field_keys"].add("streams.0.iso_channel")
    elif region == "ffff.e000.01b0" and tx.scalar_value is not None:
        state["streams"].setdefault(0, {})["audio_channels"] = tx.scalar_value
        state["coverage"]["field_keys"].add("streams.0.audio_channels")
    elif region == "ffff.e000.01b4" and tx.scalar_value is not None:
        state["streams"].setdefault(0, {})["midi_ports"] = tx.scalar_value
        state["coverage"]["field_keys"].add("streams.0.midi_ports")
    elif region == "ffff.e000.01b8" and tx.scalar_value is not None:
        state["streams"].setdefault(0, {})["speed"] = _annotate_region(region, tx.scalar_value)[1]
        state["coverage"]["field_keys"].add("streams.0.speed")
    elif region == "ffff.e000.01a4" and tx.scalar_value is not None:
        state["number"] = tx.scalar_value
        state["coverage"]["field_keys"].add("number")


def _apply_rx_state(state: dict[str, Any], tx: SemanticTransaction) -> None:
    region = tx.region
    if region is None:
        return
    if region == "ffff.e000.03dc" and tx.payload:
        state["coverage"]["block_sizes"].append(tx.size or len(tx.payload))
        fields = _extract_rx_fields(tx.payload)
        state["number"] = fields.get("number", state["number"])
        state["size_quadlets"] = fields.get("size_quadlets", state["size_quadlets"])
        for index, stream in fields.get("streams", {}).items():
            state["streams"].setdefault(index, {}).update(stream)
        state["coverage"]["field_keys"].update(fields.get("field_keys", set()))
        return
    if region == "ffff.e000.03f4" and tx.payload:
        labels = _deserialize_labels(tx.payload[:256])
        state["streams"].setdefault(0, {})["channel_names"] = labels
        state["coverage"]["field_keys"].add("streams.0.channel_names")
        return
    if region == "ffff.e000.03e0" and tx.scalar_value is not None:
        state["size_quadlets"] = tx.scalar_value
        state["coverage"]["field_keys"].add("size_quadlets")
    elif region == "ffff.e000.03e4" and tx.scalar_value is not None:
        state["streams"].setdefault(0, {})["iso_channel"] = _normalize_iso_channel(tx.scalar_value)
        state["coverage"]["field_keys"].add("streams.0.iso_channel")
    elif region == "ffff.e000.03e8" and tx.scalar_value is not None:
        state["streams"].setdefault(0, {})["seq_start"] = tx.scalar_value
        state["coverage"]["field_keys"].add("streams.0.seq_start")
    elif region == "ffff.e000.03ec" and tx.scalar_value is not None:
        state["streams"].setdefault(0, {})["audio_channels"] = tx.scalar_value
        state["coverage"]["field_keys"].add("streams.0.audio_channels")
    elif region == "ffff.e000.03f0" and tx.scalar_value is not None:
        state["streams"].setdefault(0, {})["midi_ports"] = tx.scalar_value
        state["coverage"]["field_keys"].add("streams.0.midi_ports")
    elif region == "ffff.e000.03dc" and tx.scalar_value is not None:
        state["number"] = tx.scalar_value
        state["coverage"]["field_keys"].add("number")


def _apply_irm_state(state: dict[str, Any], tx: SemanticTransaction) -> None:
    if tx.region is None:
        return
    if tx.direction == "read" and tx.scalar_value is not None:
        state["reads"][tx.region] = tx.scalar_value
        return
    if tx.direction != "lock" or not tx.request_payload:
        return
    payload = tx.request_payload
    if tx.region == "ffff.f000.0220" and len(payload) >= 8:
        old_value, new_value = struct.unpack(">II", payload[:8])
        state["allocations"].append(
            {
                "region": tx.region,
                "old": old_value,
                "new": new_value,
                "delta": old_value - new_value,
            }
        )
    elif tx.region in {"ffff.f000.0224", "ffff.f000.0228"} and len(payload) >= 8:
        old_bitmap, new_bitmap = struct.unpack(">II", payload[:8])
        state["allocations"].append(
            {
                "region": tx.region,
                "old": old_bitmap,
                "new": new_bitmap,
                "delta": old_bitmap ^ new_bitmap,
            }
        )


def _extract_layout_fields(payload: bytes) -> dict[str, int]:
    layout: dict[str, int] = {}
    if len(payload) < 32:
        return layout
    names = [
        "DICE_GLOBAL_OFFSET",
        "DICE_GLOBAL_SIZE",
        "DICE_TX_OFFSET",
        "DICE_TX_SIZE",
        "DICE_RX_OFFSET",
        "DICE_RX_SIZE",
        "DICE_EXT_SYNC_OFFSET",
        "DICE_EXT_SYNC_SIZE",
    ]
    values = struct.unpack(">8I", payload[:32])
    for name, value in zip(names, values):
        layout[name] = value
    return layout


def _extract_global_fields(payload: bytes) -> dict[str, Any]:
    fields: dict[str, Any] = {}
    if len(payload) >= 8:
        owner = struct.unpack(">Q", payload[0:8])[0]
        fields["owner"] = _format_owner(owner)
    if len(payload) >= 12:
        notification = struct.unpack(">I", payload[8:12])[0]
        fields["notification"] = _annotate_region("ffff.e000.0030", notification)[1]
    if len(payload) >= 76:
        fields["nickname"] = unpack_label(payload[12:76])
    if len(payload) >= 80:
        clock_select = struct.unpack(">I", payload[76:80])[0]
        fields["clock_select"] = _annotate_region("ffff.e000.0074", clock_select)[1]
    if len(payload) >= 84:
        fields["enable"] = bool(struct.unpack(">I", payload[80:84])[0])
    if len(payload) >= 88:
        status = struct.unpack(">I", payload[84:88])[0]
        fields["status"] = _annotate_region("ffff.e000.007c", status)[1]
    if len(payload) >= 92:
        extended = struct.unpack(">I", payload[88:92])[0]
        fields["extended_status"] = _annotate_region("ffff.e000.0080", extended)[1]
    if len(payload) >= 96:
        sample_rate = struct.unpack(">I", payload[92:96])[0]
        fields["sample_rate"] = _annotate_region("ffff.e000.0084", sample_rate)[1]
    if len(payload) >= 100:
        version = struct.unpack(">I", payload[96:100])[0]
        fields["version"] = _annotate_region("ffff.e000.0088", version)[1]
    if len(payload) >= 104:
        caps = struct.unpack(">I", payload[100:104])[0]
        fields["clock_caps"] = _annotate_region("ffff.e000.008c", caps)[1]
    if len(payload) >= 360:
        fields["clock_source_labels"] = _deserialize_labels(payload[104:360])
    return fields


def _extract_tx_fields(payload: bytes) -> dict[str, Any]:
    fields: dict[str, Any] = {"streams": {}, "field_keys": set()}
    if len(payload) < 8:
        return fields
    number, size_quadlets = struct.unpack(">II", payload[:8])
    fields["number"] = number
    fields["size_quadlets"] = size_quadlets
    fields["field_keys"].update({"number", "size_quadlets"})
    stride = size_quadlets * 4
    for index in range(number):
        base = 8 + index * stride
        if base + 16 > len(payload):
            break
        iso_channel, audio_channels, midi_ports, speed = struct.unpack(">IIII", payload[base : base + 16])
        stream = {
            "iso_channel": _normalize_iso_channel(iso_channel),
            "audio_channels": audio_channels,
            "midi_ports": midi_ports,
            "speed": _annotate_region("ffff.e000.01b8", speed)[1],
        }
        names_start = base + 16
        if names_start < len(payload):
            labels = _deserialize_labels(payload[names_start : min(names_start + 256, len(payload))])
            if labels:
                stream["channel_names"] = labels
                fields["field_keys"].add(f"streams.{index}.channel_names")
        fields["streams"][index] = stream
        fields["field_keys"].update(
            {
                f"streams.{index}.iso_channel",
                f"streams.{index}.audio_channels",
                f"streams.{index}.midi_ports",
                f"streams.{index}.speed",
            }
        )
    return fields


def _extract_rx_fields(payload: bytes) -> dict[str, Any]:
    fields: dict[str, Any] = {"streams": {}, "field_keys": set()}
    if len(payload) < 8:
        return fields
    number, size_quadlets = struct.unpack(">II", payload[:8])
    fields["number"] = number
    fields["size_quadlets"] = size_quadlets
    fields["field_keys"].update({"number", "size_quadlets"})
    stride = size_quadlets * 4
    for index in range(number):
        base = 8 + index * stride
        if base + 16 > len(payload):
            break
        iso_channel, seq_start, audio_channels, midi_ports = struct.unpack(">IIII", payload[base : base + 16])
        stream = {
            "iso_channel": _normalize_iso_channel(iso_channel),
            "seq_start": seq_start,
            "audio_channels": audio_channels,
            "midi_ports": midi_ports,
        }
        names_start = base + 16
        if names_start < len(payload):
            labels = _deserialize_labels(payload[names_start : min(names_start + 256, len(payload))])
            if labels:
                stream["channel_names"] = labels
                fields["field_keys"].add(f"streams.{index}.channel_names")
        fields["streams"][index] = stream
        fields["field_keys"].update(
            {
                f"streams.{index}.iso_channel",
                f"streams.{index}.seq_start",
                f"streams.{index}.audio_channels",
                f"streams.{index}.midi_ports",
            }
        )
    return fields


def _extract_cas_values(payload: bytes) -> dict[str, str]:
    old_hi, old_lo, new_hi, new_lo = struct.unpack(">IIII", payload[:16])
    return {
        "old": f"0x{((old_hi << 32) | old_lo):016x}",
        "new": f"0x{((new_hi << 32) | new_lo):016x}",
    }


def _format_owner(owner: int) -> str:
    if owner == 0xFFFF000000000000:
        return "No owner"
    node = (owner >> 48) & 0xFFFF
    address = owner & 0x0000FFFFFFFFFFFF
    return f"node 0x{node:04x} notify@0x{address:012x}"


def _completion_strategy(state: dict[str, Any]) -> str:
    has_poll = bool(state["completion"]["polls"])
    has_async = bool(state["completion"]["async_writes"])
    if has_poll and has_async:
        async_targets = ",".join(sorted(item["register"] for item in state["completion"]["async_writes"]))
        return f"poll+async({async_targets})"
    if has_poll:
        return "poll_only"
    if has_async:
        async_targets = ",".join(sorted(item["register"] for item in state["completion"]["async_writes"]))
        return f"async_only({async_targets})"
    return "none"


def _format_global_state(state: dict[str, Any]) -> dict[str, Any]:
    return {
        "fields": state["fields"],
        "coverage": state["coverage"],
        "owner_claims": state["owner_claims"],
    }


def _format_stream_state(state: dict[str, Any]) -> dict[str, Any]:
    return {
        "number": state["number"],
        "size_quadlets": state["size_quadlets"],
        "streams": state["streams"],
        "coverage": state["coverage"],
    }


def _format_irm_state(state: dict[str, Any]) -> dict[str, Any]:
    return state


def _format_tcat_state(state: dict[str, Any]) -> dict[str, Any]:
    return state


def _final_stream_program(state: dict[str, Any]) -> dict[str, Any]:
    streams = state.get("streams", {})
    if "0" in streams:
        return streams["0"]
    if 0 in streams:
        return streams[0]
    return {}


def _stream_signature(stream: dict[str, Any]) -> dict[str, Any]:
    return {
        key: stream.get(key)
        for key in ("iso_channel", "audio_channels", "midi_ports", "speed", "seq_start")
        if key in stream
    }


def _format_stream_short(state: dict[str, Any]) -> str:
    stream = _final_stream_program(state)
    if not stream:
        return "unknown"
    iso = _render_iso(stream.get("iso_channel"))
    speed = stream.get("speed")
    if speed:
        return f"{iso}, {speed}"
    return iso


def _phase_indices_for(phases: list[PhaseRecord], kinds: set[str]) -> list[int]:
    return [index for index, phase in enumerate(phases) if phase.kind in kinds]


def _phase_comparison_indexes(phases: list[PhaseComparison], kind: str) -> list[int]:
    return [phase.index for phase in phases if phase.kind == kind]


def _pair_classification(reference: Any, current: Any) -> str:
    if reference and current:
        return "match"
    if reference:
        return "missing"
    return "extra"


def _direction_for_kind(kind: str) -> str:
    if kind in {"Qread", "QRresp", "Bread", "BRresp"}:
        return "read"
    if kind in {"LockRq", "LockResp"}:
        return "lock"
    return "write"


def _region(address: str | None) -> str | None:
    if not address:
        return None
    parts = address.split(".")
    return ".".join(parts[1:]) if len(parts) >= 2 else address


def _address_for_region(region: str) -> str:
    return f"ffc0.{region}" if region.startswith(("ffff", "00ff", "0001")) else region


def _annotate_region(region: str | None, value: int | None) -> tuple[str, str]:
    if not region:
        return ("", "")
    return annotate(_address_for_region(region), value)


def _hex_or_empty(value: int | None) -> str:
    if value is None:
        return ""
    return f"0x{value:08x}"


def _normalize_iso_channel(value: int) -> int:
    return -1 if value == 0xFFFFFFFF else value


def _render_iso(value: Any) -> str:
    if value == -1:
        return "unused (-1)"
    if value is None:
        return "unknown"
    return f"channel {value}"


def _short_region(region: str) -> str:
    return region.replace("ffff.", "")
