"""Shared phase-0 filtering for reference startup parity exports."""
from __future__ import annotations

from dataclasses import dataclass

from .log_comparator import _is_config_rom_region
from .log_parser import LogEvent
from .semantic_analysis import SessionAnalysis


LOW_SIGNAL_KINDS = {"SelfID", "CycleStart", "PHYResume", "WrResp"}


@dataclass(frozen=True)
class Phase0Event:
    event_index: int
    event: LogEvent


def region_from_event(event: LogEvent) -> str | None:
    if not event.address:
        return None
    parts = event.address.split(".")
    return ".".join(parts[1:]) if len(parts) >= 2 else event.address


def collect_phase0_events(
    session: SessionAnalysis,
    *,
    ignore_config_rom: bool = False,
) -> list[Phase0Event]:
    items: list[Phase0Event] = []
    for event_index, event in enumerate(session.window.events):
        if event.kind == "BusReset":
            items.append(Phase0Event(event_index=event_index, event=event))
            continue

        if event.kind in LOW_SIGNAL_KINDS:
            continue

        region = region_from_event(event)
        if region is None:
            continue
        if ignore_config_rom and _is_config_rom_region(region):
            continue

        items.append(Phase0Event(event_index=event_index, event=event))

    return _trim_phase0_prelude(items)


def _trim_phase0_prelude(items: list[Phase0Event]) -> list[Phase0Event]:
    """Drop Apple-side startup preflight noise that is not Saffire-driver behavior.

    Today this is the leading IRM LO compare-verify sequence on 0x0228:
      Qread -> QRresp -> LockRq(old==new) -> LockResp
    when it appears immediately after the bus-reset marker.
    """
    if len(items) < 5:
        return items

    prelude = items[1:5]
    expected_kinds = ["Qread", "QRresp", "LockRq", "LockResp"]
    if [item.event.kind for item in prelude] != expected_kinds:
        return items

    if any(region_from_event(item.event) != "ffff.f000.0228" for item in prelude):
        return items

    lock_payload = prelude[2].event.payload or b""
    if len(lock_payload) < 8 or lock_payload[:4] != lock_payload[4:8]:
        return items

    return [items[0], *items[5:]]
