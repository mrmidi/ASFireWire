"""Phase-0 parity markdown exporter for FireBug startup traces."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

from .log_comparator import normalize
from .log_parser import parse_log
from .parity_phase0 import collect_phase0_events, region_from_event
from .semantic_analysis import (
    PHASE_BUS_RESET,
    PHASE_CLOCK,
    PHASE_CONFIGROM,
    PHASE_ENABLE,
    PHASE_GLOBAL,
    PHASE_IRM,
    PHASE_LAYOUT,
    PHASE_ORDER,
    PHASE_OWNER,
    PHASE_RX,
    PHASE_STREAM,
    PHASE_TCAT,
    PHASE_TX,
    PHASE_WAIT,
    SessionAnalysis,
    analyze_session,
    _phase_kind_for_transaction,
)

STYLE_PHASES = "phases"
STYLE_TIMELINE = "timeline"
STYLE_BOTH = "both"

PHASE_TITLES = {
    PHASE_BUS_RESET: "Bus Reset",
    PHASE_CONFIGROM: "Config ROM Probe",
    PHASE_LAYOUT: "DICE Layout Discovery",
    PHASE_GLOBAL: "Global State Read",
    PHASE_OWNER: "Owner Claim",
    PHASE_CLOCK: "Clock Select",
    PHASE_WAIT: "Completion Wait",
    PHASE_STREAM: "Stream Discovery",
    PHASE_IRM: "IRM Reservation",
    PHASE_RX: "RX Programming",
    PHASE_TX: "TX Programming",
    PHASE_TCAT: "TCAT Extended Discovery",
    PHASE_ENABLE: "Enable",
}

PHASE_TAGS = {
    PHASE_BUS_RESET: "bus_reset",
    PHASE_CONFIGROM: "configrom",
    PHASE_LAYOUT: "layout",
    PHASE_GLOBAL: "global",
    PHASE_OWNER: "owner",
    PHASE_CLOCK: "clock",
    PHASE_WAIT: "wait",
    PHASE_STREAM: "stream",
    PHASE_IRM: "irm",
    PHASE_RX: "rx",
    PHASE_TX: "tx",
    PHASE_TCAT: "tcat",
    PHASE_ENABLE: "enable",
}


@dataclass(frozen=True)
class ParityItem:
    seq: int
    phase: str
    timestamp: str
    raw_kind: str
    address: str | None
    register: str
    size_or_value: str
    detail: str


def load_and_export_parity_markdown(
    log_path: str | Path,
    out_dir: str | Path,
    *,
    ignore_config_rom: bool = False,
    style: str = STYLE_BOTH,
) -> dict[str, Path]:
    session = load_and_analyze_session(log_path)
    return export_parity_markdown(
        session,
        log_path,
        out_dir,
        ignore_config_rom=ignore_config_rom,
        style=style,
    )


def load_and_analyze_session(log_path: str | Path) -> SessionAnalysis:
    text = Path(log_path).read_text(encoding="utf-8", errors="replace")
    return analyze_session(parse_log(text), Path(log_path).name)


def export_parity_markdown(
    session: SessionAnalysis,
    source_path: str | Path,
    out_dir: str | Path,
    *,
    ignore_config_rom: bool = False,
    style: str = STYLE_BOTH,
) -> dict[str, Path]:
    outputs = render_parity_markdown(
        session,
        source_label=Path(source_path).name,
        ignore_config_rom=ignore_config_rom,
        style=style,
    )
    out_root = Path(out_dir)
    out_root.mkdir(parents=True, exist_ok=True)

    written: dict[str, Path] = {}
    for key, text in outputs.items():
        filename = f"reference-phase0-{key}.md"
        target = out_root / filename
        target.write_text(text, encoding="utf-8")
        written[key] = target
    return written


def render_parity_markdown(
    session: SessionAnalysis,
    *,
    source_label: str,
    ignore_config_rom: bool = False,
    style: str = STYLE_BOTH,
) -> dict[str, str]:
    if style not in {STYLE_PHASES, STYLE_TIMELINE, STYLE_BOTH}:
        raise ValueError(f"Unsupported style: {style}")

    items = _collect_items(session, ignore_config_rom=ignore_config_rom)
    outputs: dict[str, str] = {}
    if style in {STYLE_PHASES, STYLE_BOTH}:
        outputs[STYLE_PHASES] = _render_phase_markdown(
            session,
            source_label=source_label,
            ignore_config_rom=ignore_config_rom,
            items=items,
        )
    if style in {STYLE_TIMELINE, STYLE_BOTH}:
        outputs[STYLE_TIMELINE] = _render_timeline_markdown(
            session,
            source_label=source_label,
            ignore_config_rom=ignore_config_rom,
            items=items,
        )
    return outputs


def _collect_items(session: SessionAnalysis, *, ignore_config_rom: bool) -> list[ParityItem]:
    phase_by_event_index = _build_phase_index(session)
    items: list[ParityItem] = []
    seq = 1
    for phase0_event in collect_phase0_events(session, ignore_config_rom=ignore_config_rom):
        event = phase0_event.event
        if event.kind == "BusReset":
            items.append(
                ParityItem(
                    seq=seq,
                    phase=PHASE_BUS_RESET,
                    timestamp=event.timestamp,
                    raw_kind=event.kind,
                    address=None,
                    register="Bus Reset",
                    size_or_value="-",
                    detail="session begins at the last bus reset before final `GLOBAL_ENABLE = 1`",
                )
            )
            seq += 1
            continue

        normalized = normalize([event], ignore_config_rom=ignore_config_rom)
        if not normalized:
            continue

        op = normalized[0]
        items.append(
            ParityItem(
                seq=seq,
                phase=phase_by_event_index.get(
                    phase0_event.event_index, _fallback_phase_for_event(event)
                ),
                timestamp=event.timestamp,
                raw_kind=event.kind,
                address=op.address,
                register=op.register,
                size_or_value=_size_or_value(event),
                detail=_detail_for_event(op),
            )
        )
        seq += 1
    return [
        ParityItem(
            seq=index + 1,
            phase=item.phase,
            timestamp=item.timestamp,
            raw_kind=item.raw_kind,
            address=item.address,
            register=item.register,
            size_or_value=item.size_or_value,
            detail=item.detail,
        )
        for index, item in enumerate(items)
    ]


def _build_phase_index(session: SessionAnalysis) -> dict[int, str]:
    phase_by_event_index: dict[int, str] = {}
    clock_seen = False
    for tx in session.transactions:
        phase = _phase_kind_for_transaction(tx, clock_seen)
        if tx.region == "ffff.e000.0074" and tx.direction == "write":
            clock_seen = True
        if phase is None:
            continue
        if tx.request_event_index is not None:
            phase_by_event_index[tx.request_event_index] = phase
        if tx.response_event_index is not None:
            phase_by_event_index[tx.response_event_index] = phase
    return phase_by_event_index


def _fallback_phase_for_event(event: LogEvent) -> str:
    region = region_from_event(event)
    if region == "ffff.e000.0078":
        return PHASE_ENABLE
    if region == "ffff.e000.0074":
        return PHASE_CLOCK
    if region is not None and region.startswith("ffff.e020."):
        return PHASE_TCAT
    if region is not None and region.startswith("ffff.f000.022"):
        return PHASE_IRM
    return PHASE_STREAM


def _size_or_value(event: LogEvent) -> str:
    if event.size is not None:
        return f"{event.size}B"
    if event.value is not None:
        return f"0x{event.value:08x}"
    return "-"


def _detail_for_event(op) -> str:
    if op.raw_kind == "Qread":
        return "read request"
    if op.raw_kind == "Bread":
        return op.decoded or "block read request"
    if op.raw_kind == "LockRq":
        return op.decoded or "lock request"
    if op.raw_kind == "LockResp":
        return op.decoded or "lock response"
    if op.raw_kind == "QRresp":
        return op.decoded or "read response"
    if op.raw_kind == "BRresp":
        return op.decoded or "block read response"
    if op.raw_kind == "Qwrite":
        return op.decoded or "write request"
    return op.decoded or op.raw_kind


def _render_phase_markdown(
    session: SessionAnalysis,
    *,
    source_label: str,
    ignore_config_rom: bool,
    items: list[ParityItem],
) -> str:
    lines = _render_header(
        title="Phase 0 Reference Parity Checklist",
        session=session,
        source_label=source_label,
        ignore_config_rom=ignore_config_rom,
        item_count=len(items),
    )

    phase_summary = {
        phase.kind: phase.summary
        for phase in session.phases
    }
    grouped: dict[str, list[ParityItem]] = {phase: [] for phase in PHASE_ORDER}
    for item in items:
        grouped.setdefault(item.phase, []).append(item)

    for phase in PHASE_ORDER:
        phase_items = grouped.get(phase, [])
        if not phase_items:
            continue
        lines.append(f"## {PHASE_TITLES.get(phase, phase)}")
        if phase in phase_summary:
            lines.append(f"Summary: {phase_summary[phase]}")
        lines.append("")
        for item in phase_items:
            lines.append(_render_item(item))
        lines.append("")

    return "\n".join(lines).rstrip() + "\n"


def _render_timeline_markdown(
    session: SessionAnalysis,
    *,
    source_label: str,
    ignore_config_rom: bool,
    items: list[ParityItem],
) -> str:
    lines = _render_header(
        title="Phase 0 Reference Parity Timeline",
        session=session,
        source_label=source_label,
        ignore_config_rom=ignore_config_rom,
        item_count=len(items),
    )
    lines.append("## Ordered Timeline")
    lines.append("")
    for item in items:
        lines.append(_render_item(item, include_phase_tag=True))
    return "\n".join(lines).rstrip() + "\n"


def _render_header(
    *,
    title: str,
    session: SessionAnalysis,
    source_label: str,
    ignore_config_rom: bool,
    item_count: int,
) -> list[str]:
    start_ts = session.window.bus_reset_timestamp or (session.window.events[0].timestamp if session.window.events else "?")
    end_ts = session.window.enable_timestamp or (session.window.events[-1].timestamp if session.window.events else "?")
    filters = [
        "Config ROM skipped" if ignore_config_rom else "Config ROM included",
        "initial IRM compare-verify skipped",
        "Self-ID skipped",
        "CycleStart skipped",
        "PHY Resume skipped",
        "WrResp skipped",
    ]
    return [
        f"# {title}",
        "",
        f"- Source log: `{source_label}`",
        f"- Filters: {', '.join(filters)}",
        (
            "- Session window: last `BusReset` before final `GLOBAL_ENABLE = 1` "
            f"({start_ts} → {end_ts})"
        ),
        f"- Generation target: unknown (FireBug does not encode generation directly)",
        f"- Checklist items: {item_count}",
        "",
    ]


def _render_item(item: ParityItem, *, include_phase_tag: bool = False) -> str:
    tag = ""
    if include_phase_tag:
        tag = f"[{PHASE_TAGS.get(item.phase, item.phase)}] "
    address = f"`{item.address}`" if item.address else ""
    register = f"`{item.register}`" if item.register else ""
    size_or_value = f"`{item.size_or_value}`" if item.size_or_value else ""
    parts = [f"- [ ] {item.seq:03d} {tag}`{item.raw_kind}`"]
    if address:
        parts.append(address)
    if register:
        parts.append(register)
    if size_or_value:
        parts.append(size_or_value)
    body = " ".join(parts)
    return f"{body} — {item.detail}"
