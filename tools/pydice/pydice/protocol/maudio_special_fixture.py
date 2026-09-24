"""Export the successful M-Audio 1814 FireBug startup as a replayable fixture."""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re

from .log_parser import parse_log

_PACKET = re.compile(
    r"^(\d+:\d+:\d+)\s+Isoch channel (\d+), tag (\d+), sy (\d+), "
    r"size (\d+) \[actual (\d+)\] (\S+)"
)
_HEX = re.compile(r"^\s+([0-9a-f]{4})\s+((?:[0-9a-f]{8}\s*)+)")
_EVENT_KINDS = {
    "BusReset": "Reset", "CycleStart": "CycleStart", "Qread": "Qread",
    "QRresp": "QRresp", "Qwrite": "Qwrite", "WrResp": "WrResp",
    "Bread": "Bread", "BRresp": "BRresp", "Bwrite": "Bwrite",
    "LockRq": "LockRq", "LockResp": "LockResp",
}


@dataclass(frozen=True)
class FixtureEvent:
    timestamp: str
    elapsed_cycles: int
    kind: str
    src: int
    dst: int
    address: int
    size: int
    value: int
    ack: int
    speed: str
    payload: bytes


def _node_id(node: str) -> int:
    try:
        return int(node, 16)
    except ValueError:
        return 0


def _address_value(address: str | None) -> int:
    if not address:
        return 0
    return int("".join(address.split(".")), 16)


def _all_events(text: str) -> list[FixtureEvent]:
    lines = text.splitlines()
    parsed = parse_log(text)
    positioned: list[tuple[int, FixtureEvent]] = []
    search_from = 0
    for event in parsed:
        if event.kind not in _EVENT_KINDS:
            continue
        try:
            source_line = lines.index(event.raw_line, search_from)
        except ValueError:
            source_line = search_from
        search_from = source_line + 1
        positioned.append((source_line, FixtureEvent(
            timestamp=event.timestamp, elapsed_cycles=0,
            kind=_EVENT_KINDS[event.kind], src=_node_id(event.src), dst=_node_id(event.dst),
            address=_address_value(event.address), size=event.size or 0,
            value=event.value or 0, ack=event.ack or 0, speed=event.speed or "",
            payload=event.payload or b"",
        )))

    # log_parser intentionally focuses on async transactions. Include every
    # displayed isoch sample packet too, parsing its real FireBug hex dump.
    index = 0
    while index < len(lines):
        match = _PACKET.match(lines[index])
        if not match:
            index += 1
            continue
        timestamp, channel, tag, sy, size, actual, speed = match.groups()
        payload = bytearray()
        cursor = index + 1
        while cursor < len(lines):
            dump = _HEX.match(lines[cursor])
            if not dump:
                break
            for quadlet in re.findall(r"[0-9a-f]{8}", dump.group(2)):
                payload.extend(bytes.fromhex(quadlet))
            cursor += 1
        # Retain the captured sample with channel/tag/sy/actual metadata packed
        # into fields the consumer can inspect; size is the on-wire size.
        positioned.append((index, FixtureEvent(
            timestamp=timestamp, elapsed_cycles=0, kind="IsochPacket",
            src=0, dst=int(channel), address=0, size=int(size),
            value=(int(tag) << 24) | (int(sy) << 16) | int(actual),
            ack=0, speed=speed, payload=bytes(payload[:int(actual)]),
        )))
        index = cursor

    # FireBug timestamps wrap the seconds field when its capture counter rolls
    # over. Sort by the timestamp's first position in the source text so the
    # fixture keeps actual capture order across those rollovers.
    positioned.sort(key=lambda item: item[0])
    events = [event for _, event in positioned]
    if not events:
        return events
    elapsed = 0
    previous = None
    wrapped_seconds = 0
    updated: list[FixtureEvent] = []
    for event in events:
        seconds, cycles, _ = (int(part) for part in event.timestamp.split(":"))
        if previous is not None and seconds < previous:
            wrapped_seconds += 256
        absolute = (wrapped_seconds + seconds) * 8000 + cycles
        if previous is None:
            first_absolute = absolute
        elapsed = absolute - first_absolute
        previous = seconds
        updated.append(FixtureEvent(**{**event.__dict__, "elapsed_cycles": elapsed}))
    return updated


def _happy_run(events: list[FixtureEvent]) -> list[FixtureEvent]:
    """Select the last run proving 1814 ROM identity, accepted start, and duplex traffic."""
    resets = [i for i, event in enumerate(events) if event.kind == "Reset"]
    candidates: list[int] = []
    for position in resets:
        window = events[position + 1:]
        next_reset = next((i for i, event in enumerate(window) if event.kind == "Reset"), len(window))
        session = window[:next_reset]
        # Config ROM identity words spell "M-AUDIO" and "FW 1814". This
        # distinguishes the operational 1814 from the bootloader persona and
        # from generic DICE register access.
        rom_words = {
            e.address & 0xFFFFFFFF: e.value
            for e in session
            if e.kind == "QRresp" and e.address >> 32 == 0xFFC2FFFF
        }
        has_maudio_1814_rom_identity = (
            rom_words.get(0xF0000454) == 0x4D2D4155  # "M-AU"
            and rom_words.get(0xF0000458) == 0x44494F00  # "DIO\0"
            and rom_words.get(0xF0000468) == 0x46572031  # "FW 1"
            and rom_words.get(0xF000046C) == 0x38313400  # "814\0"
        )
        has_irm_allocation = any(e.kind == "LockRq" and 0xF0000220 <= (e.address & 0xFFFFFFFF) <= 0xF0000228
                                 for e in session)
        has_cmp_change = any(e.kind == "LockRq" and 0xF0000900 <= (e.address & 0xFFFFFFFF) <= 0xF0000984
                             for e in session)
        tx_samples = [i for i, e in enumerate(session)
                      if e.kind == "IsochPacket" and e.dst == 0 and e.payload]
        rx_samples = [i for i, e in enumerate(session)
                      if e.kind == "IsochPacket" and e.dst == 1 and e.payload]
        has_tx_rx_samples = bool(tx_samples and rx_samples)
        # FireBug reports later keepalive/restart RX packets too. The startup
        # acceptance needs to follow the first observed TX and RX samples.
        last_startup_sample = max(tx_samples[0], rx_samples[0]) if tx_samples and rx_samples else -1
        has_final_accepted_start = any(
            i > last_startup_sample
            and e.kind == "Bwrite"
            and (e.address & 0xFFFFFFFF) == 0xF0000D00
            and len(e.payload) >= 4
            and e.payload[0] == 0x09  # ACCEPTED
            and e.payload[2] == 0x19  # INPUT PLUG SIGNAL FORMAT
            for i, e in enumerate(session)
        )
        if (has_maudio_1814_rom_identity and has_irm_allocation and has_cmp_change
                and has_tx_rx_samples and has_final_accepted_start):
            candidates.append(position)
    if not candidates:
        raise ValueError("capture contains no reset-to-duplex M-Audio happy path")
    selected = events[candidates[-1]:]
    origin = selected[0].elapsed_cycles
    return [FixtureEvent(**{**event.__dict__, "elapsed_cycles": event.elapsed_cycles - origin})
            for event in selected]


def render_maudio_special_fixture(text: str, source_label: str) -> str:
    events = _happy_run(_all_events(text))
    arrays: list[str] = []
    payload_exprs: list[str] = []
    for i, event in enumerate(events):
        if event.payload:
            name = f"kPayload{i:03d}"
            data = ", ".join(f"0x{byte:02x}" for byte in event.payload)
            arrays.append(f"inline constexpr uint8_t {name}[] = {{{data}}};")
            payload_exprs.append(name)
        else:
            payload_exprs.append("nullptr")
    lines = [
        "// Generated by `pydice export-maudio-special-cpp` from FireBug capture.",
        "// Event order, payloads, and elapsed cycle values are parsed from the capture.",
        "#include <cstddef>", "#include <cstdint>", "", "namespace MAudioSpecialHappyPathFixture {",
        "enum class EventKind : uint8_t { Reset, CycleStart, Qread, QRresp, Qwrite, WrResp, Bread, BRresp, Bwrite, LockRq, LockResp, IsochPacket };",
        "struct Event { uint32_t elapsedCycles; EventKind kind; uint16_t src; uint16_t dst; uint64_t address; uint32_t size; uint32_t value; uint8_t ack; const char* speed; const uint8_t* payload; size_t payloadSize; };",
        f'inline constexpr char kSourceLog[] = "{source_label}";', "",
        *arrays, "",
        "inline constexpr Event kEvents[] = {",
    ]
    for event, payload in zip(events, payload_exprs):
        lines.append(
            f"    {{{event.elapsed_cycles}U, EventKind::{event.kind}, 0x{event.src:04x}U, "
            f"0x{event.dst:04x}U, 0x{event.address:016x}ULL, {event.size}U, "
            f"0x{event.value:08x}U, {event.ack}U, \"{event.speed}\", {payload}, {len(event.payload)}U}},"
        )
    lines.extend(["};", "inline constexpr size_t kEventCount = sizeof(kEvents) / sizeof(kEvents[0]);",
                  "} // namespace MAudioSpecialHappyPathFixture", ""])
    return "\n".join(lines)


def load_and_export_maudio_special_fixture(log_path: str | Path, out_path: str | Path) -> Path:
    source = Path(log_path)
    rendered = render_maudio_special_fixture(
        source.read_text(encoding="utf-8", errors="replace"), source.name
    )
    target = Path(out_path)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(rendered, encoding="utf-8")
    return target
