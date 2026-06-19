"""Log comparison: sequence diff of two FireBug init logs."""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum

from .log_parser import LogEvent
from .dice_address_map import annotate
from .payload_decoder import decode_payload


@dataclass
class RegisterOp:
    address: str          # region e.g. "ffff.e000.0074"
    register: str         # from annotate()
    direction: str        # "R", "W", "L"
    value: int | None     # quadlet value (None for block reads)
    decoded: str          # human-readable
    size: int | None      # block size for Bread
    timestamp: str
    raw_kind: str         # Qwrite, Bread, LockRq, etc.
    payload: bytes | None # block/lock payload when available


class DiffStatus(Enum):
    MATCH = "match"
    MISMATCH = "mismatch"
    REF_ONLY = "ref_only"
    DEBUG_ONLY = "debug_only"


@dataclass
class DiffLine:
    status: DiffStatus
    ref_op: RegisterOp | None
    debug_op: RegisterOp | None


# ── Init extraction ──────────────────────────────────────────────────────────

_ENABLE_REGION = "ffff.e000.0078"
_CONFIG_ROM_PREFIX = "ffff.f000.04"
_SKIP_KINDS = {"BusReset", "SelfID", "CycleStart", "PHYResume", "WrResp"}


def _region(ev: LogEvent) -> str | None:
    if not ev.address:
        return None
    parts = ev.address.split(".")
    return ".".join(parts[1:]) if len(parts) >= 2 else ev.address


def _is_config_rom_region(region: str) -> bool:
    return region.startswith(_CONFIG_ROM_PREFIX)


def extract_init_sequence(events: list[LogEvent]) -> list[LogEvent]:
    """Extract init sequence: from last BusReset before final ENABLE write, to that ENABLE."""
    # Find last Qwrite to GLOBAL_ENABLE
    enable_idx = None
    for i in range(len(events) - 1, -1, -1):
        if events[i].kind == "Qwrite" and _region(events[i]) == _ENABLE_REGION:
            enable_idx = i
            break

    if enable_idx is not None:
        # Walk back to preceding BusReset
        bus_reset_idx = None
        for i in range(enable_idx - 1, -1, -1):
            if events[i].kind == "BusReset":
                bus_reset_idx = i
                break
        start = bus_reset_idx if bus_reset_idx is not None else 0
        return events[start : enable_idx + 1]

    # Fallback: everything after last BusReset
    last_reset = None
    for i in range(len(events) - 1, -1, -1):
        if events[i].kind == "BusReset":
            last_reset = i
            break
    if last_reset is not None:
        return events[last_reset:]
    return events


# ── Normalization ────────────────────────────────────────────────────────────

def _compact_decoded_line(line: str) -> str:
    replacements = (
        ("DICE_GLOBAL_OFFSET: ", "GLOBAL_OFF="),
        ("DICE_GLOBAL_SIZE: ", "GLOBAL_SIZE="),
        ("DICE_TX_OFFSET: ", "TX_OFF="),
        ("DICE_TX_SIZE: ", "TX_SIZE="),
        ("DICE_RX_OFFSET: ", "RX_OFF="),
        ("DICE_RX_SIZE: ", "RX_SIZE="),
        ("DICE_EXT_SYNC_OFFSET: ", "EXT_OFF="),
        ("DICE_EXT_SYNC_SIZE: ", "EXT_SIZE="),
        ("OWNER: ", "OWNER="),
        ("NOTIFICATION: ", "NOTIFY="),
        ("NICK_NAME: ", "NAME="),
        ("CLOCK_SELECT: ", "CLOCK="),
        ("ENABLE: ", "ENABLE="),
        ("STATUS: ", "STATUS="),
        ("SAMPLE_RATE: ", "RATE="),
        ("TX_NUMBER: ", "TX_COUNT="),
        ("TX_SIZE: ", "TX_STRIDE="),
        ("RX_NUMBER: ", "RX_COUNT="),
        ("RX_SIZE: ", "RX_STRIDE="),
        ("TX[0] ISO channel: ", "TX[0].ISO="),
        ("TX[0] audio channels: ", "TX[0].PCM="),
        ("TX[0] MIDI ports: ", "TX[0].MIDI="),
        ("TX[0] speed: ", "TX[0].SPD="),
        ("RX[0] ISO channel: ", "RX[0].ISO="),
        ("RX[0] seq start: ", "RX[0].SEQ="),
        ("RX[0] audio channels: ", "RX[0].PCM="),
        ("RX[0] MIDI ports: ", "RX[0].MIDI="),
        ("CAS old_val: ", "CAS.old="),
        ("CAS new_val: ", "CAS.new="),
        ("IRM_BANDWIDTH_AVAILABLE returned: ", "BW="),
        ("IRM_CHANNELS_AVAILABLE_HI returned: ", "HI="),
        ("IRM_CHANNELS_AVAILABLE_LO returned: ", "LO="),
    )
    compact = line.strip()
    for old, new in replacements:
        compact = compact.replace(old, new)
    compact = compact.replace(" quadlets ", "q ")
    compact = compact.replace(" bytes", "B")
    compact = compact.replace(" Hz", "Hz")
    return compact


def _truncate(text: str, limit: int = 160) -> str:
    if len(text) <= limit:
        return text
    return text[: limit - 1] + "…"


def _format_owner_value(owner: int) -> str:
    if owner == 0xFFFF000000000000:
        return "No owner"
    node = (owner >> 48) & 0xFFFF
    addr = owner & 0x0000FFFFFFFFFFFF
    return f"node 0x{node:04x} notify@0x{addr:012x}"


def _summarize_global_owner_payload(payload: bytes, size: int | None, raw_kind: str) -> str:
    byte_count = size if size is not None else len(payload)
    prefix = f"{byte_count}B"
    if size is not None and len(payload) < size:
        prefix += f" partial({len(payload)}B)"

    if raw_kind == "LockRq" and len(payload) >= 16:
        old_val = int.from_bytes(payload[0:8], "big")
        new_val = int.from_bytes(payload[8:16], "big")
        return (
            f"{prefix} CAS.old=0x{old_val:016x} | CAS.new=0x{new_val:016x}"
        )

    parts: list[str] = []
    if len(payload) >= 8:
        owner = int.from_bytes(payload[0:8], "big")
        parts.append(f"OWNER={_format_owner_value(owner)}")
    if len(payload) >= 12:
        notify = int.from_bytes(payload[8:12], "big")
        parts.append(f"NOTIFY=0x{notify:08x}")
    if len(payload) >= 16 and size is not None and len(payload) < size:
        name_head = payload[12:16].decode("ascii", errors="replace").rstrip("\x00")
        if name_head:
            parts.append(f"NAME_HEAD={name_head!r}")

    if not parts:
        head = payload[:16].hex()
        suffix = "…" if len(payload) > 16 else ""
        parts.append(f"head={head}{suffix}")

    return _truncate(f"{prefix} {' | '.join(parts)}")


def _pick_summary_lines(region: str | None, decoded_lines: list[str]) -> list[str]:
    if not decoded_lines:
        return []

    preferred_prefixes: dict[str, tuple[str, ...]] = {
        "ffff.e000.0000": (
            "DICE_GLOBAL_OFFSET:",
            "DICE_GLOBAL_SIZE:",
            "DICE_TX_OFFSET:",
            "DICE_TX_SIZE:",
            "DICE_RX_OFFSET:",
            "DICE_RX_SIZE:",
        ),
        "ffff.e000.0028": (
            "OWNER:",
            "NOTIFICATION:",
            "CLOCK_SELECT:",
            "ENABLE:",
            "STATUS:",
            "SAMPLE_RATE:",
            "CAS old_val:",
            "CAS new_val:",
        ),
        "ffff.e000.01a4": (
            "TX_NUMBER:",
            "TX_SIZE:",
            "TX[0] ISO channel:",
            "TX[0] audio channels:",
            "TX[0] MIDI ports:",
            "TX[0] speed:",
        ),
        "ffff.e000.03dc": (
            "RX_NUMBER:",
            "RX_SIZE:",
            "RX[0] ISO channel:",
            "RX[0] seq start:",
            "RX[0] audio channels:",
            "RX[0] MIDI ports:",
        ),
        "ffff.f000.0220": (
            "IRM_BANDWIDTH_AVAILABLE returned:",
            "IRM_BANDWIDTH_AVAILABLE:",
            "  → ",
        ),
        "ffff.f000.0224": (
            "IRM_CHANNELS_AVAILABLE_HI returned:",
            "IRM_CHANNELS_AVAILABLE_HI:",
            "  → ",
        ),
        "ffff.f000.0228": (
            "IRM_CHANNELS_AVAILABLE_LO returned:",
            "IRM_CHANNELS_AVAILABLE_LO:",
            "  → ",
        ),
    }

    prefixes = preferred_prefixes.get(region)
    if prefixes is None:
        return decoded_lines[:4]

    selected: list[str] = []
    for prefix in prefixes:
        for line in decoded_lines:
            if line.startswith(prefix):
                selected.append(line)
    return selected or decoded_lines[:4]


def _summarize_payload(address: str | None, payload: bytes | None, size: int | None, raw_kind: str) -> str:
    byte_count = size if size is not None else len(payload or b"")
    if not payload:
        if raw_kind == "Bread":
            return f"{byte_count}B read request"
        if raw_kind == "BRresp":
            return f"{byte_count}B read response"
        if raw_kind == "LockRq":
            return f"{byte_count}B lock request"
        if raw_kind == "LockResp":
            return f"{byte_count}B lock response"
        return f"{byte_count}B payload"

    if address:
        parts = address.split(".")
        region = ".".join(parts[1:]) if len(parts) >= 2 else address
    else:
        region = None

    if region == "ffff.e000.0028":
        if raw_kind == "LockRq" and len(payload) >= 16:
            return _summarize_global_owner_payload(payload, size, raw_kind)
        if raw_kind in {"BRresp", "LockResp"} and size == 8 and len(payload) >= 8:
            return _summarize_global_owner_payload(payload, size, raw_kind)
        if raw_kind == "BRresp" and size is not None and len(payload) < size:
            return _summarize_global_owner_payload(payload, size, raw_kind)

    decoded_lines = [line for line in decode_payload(address, payload, size) if line.strip()]
    selected = _pick_summary_lines(region, decoded_lines)
    if not selected:
        head = payload[:16].hex()
        suffix = "…" if len(payload) > 16 else ""
        return f"{byte_count}B head={head}{suffix}"

    summary = " | ".join(_compact_decoded_line(line) for line in selected)
    return _truncate(f"{byte_count}B {summary}")


def describe_payload_difference(ref_op: RegisterOp, debug_op: RegisterOp) -> str | None:
    if ref_op.payload is None or debug_op.payload is None:
        return None
    if ref_op.payload == debug_op.payload:
        return None

    ref_payload = ref_op.payload
    dbg_payload = debug_op.payload
    min_len = min(len(ref_payload), len(dbg_payload))

    for idx in range(min_len):
        if ref_payload[idx] != dbg_payload[idx]:
            word_off = idx & ~0x3
            ref_word = ref_payload[word_off : word_off + 4].hex()
            dbg_word = dbg_payload[word_off : word_off + 4].hex()
            return (
                f"payload diff @+0x{idx:03x} "
                f"(word 0x{word_off:03x}: ref={ref_word} dbg={dbg_word})"
            )

    return f"payload length diff: ref={len(ref_payload)} dbg={len(dbg_payload)}"


def normalize(events: list[LogEvent], *, ignore_config_rom: bool = False) -> list[RegisterOp]:
    """Convert LogEvents to RegisterOps, skipping bus events and responses."""
    ops: list[RegisterOp] = []
    for ev in events:
        if ev.kind in _SKIP_KINDS:
            continue

        region = _region(ev)
        if region is None:
            continue
        if ignore_config_rom and _is_config_rom_region(region):
            continue

        reg_name, decoded = annotate(ev.address, ev.value)

        if ev.kind == "Qwrite":
            ops.append(RegisterOp(
                address=region, register=reg_name, direction="W",
                value=ev.value, decoded=decoded, size=None,
                timestamp=ev.timestamp, raw_kind=ev.kind,
                payload=None,
            ))
        elif ev.kind in ("Qread", "QRresp"):
            ops.append(RegisterOp(
                address=region, register=reg_name, direction="R",
                value=ev.value, decoded=decoded, size=None,
                timestamp=ev.timestamp, raw_kind=ev.kind,
                payload=None,
            ))
        elif ev.kind in ("Bread", "BRresp"):
            ops.append(RegisterOp(
                address=region, register=reg_name, direction="R",
                value=None,
                decoded=_summarize_payload(ev.address, ev.payload, ev.size, ev.kind),
                size=ev.size,
                timestamp=ev.timestamp, raw_kind=ev.kind,
                payload=ev.payload,
            ))
        elif ev.kind in ("LockRq", "LockResp"):
            ops.append(RegisterOp(
                address=region, register=reg_name, direction="L",
                value=ev.value,
                decoded=_summarize_payload(ev.address, ev.payload, ev.size, ev.kind),
                size=ev.size,
                timestamp=ev.timestamp, raw_kind=ev.kind,
                payload=ev.payload,
            ))

    return ops


# ── LCS-based sequence diff ─────────────────────────────────────────────────

def _op_key(op: RegisterOp) -> tuple[str, str]:
    return (op.address, op.raw_kind)


def _values_match(a: RegisterOp, b: RegisterOp) -> bool:
    if a.payload is not None or b.payload is not None:
        return a.payload == b.payload
    if a.size is not None or b.size is not None:
        return a.size == b.size
    return a.value == b.value


def diff_sequences(ref_ops: list[RegisterOp], debug_ops: list[RegisterOp]) -> list[DiffLine]:
    """LCS-based diff of two RegisterOp sequences."""
    m, n = len(ref_ops), len(debug_ops)

    # Build LCS table on keys
    ref_keys = [_op_key(op) for op in ref_ops]
    dbg_keys = [_op_key(op) for op in debug_ops]

    dp = [[0] * (n + 1) for _ in range(m + 1)]
    for i in range(m - 1, -1, -1):
        for j in range(n - 1, -1, -1):
            if ref_keys[i] == dbg_keys[j]:
                dp[i][j] = dp[i + 1][j + 1] + 1
            else:
                dp[i][j] = max(dp[i + 1][j], dp[i][j + 1])

    # Walk the DP table to produce diff lines
    result: list[DiffLine] = []
    i, j = 0, 0
    while i < m or j < n:
        if i < m and j < n and ref_keys[i] == dbg_keys[j]:
            # Matched pair
            status = DiffStatus.MATCH if _values_match(ref_ops[i], debug_ops[j]) else DiffStatus.MISMATCH
            result.append(DiffLine(status=status, ref_op=ref_ops[i], debug_op=debug_ops[j]))
            i += 1
            j += 1
        elif j >= n or (i < m and dp[i + 1][j] >= dp[i][j + 1]):
            result.append(DiffLine(status=DiffStatus.REF_ONLY, ref_op=ref_ops[i], debug_op=None))
            i += 1
        else:
            result.append(DiffLine(status=DiffStatus.DEBUG_ONLY, ref_op=None, debug_op=debug_ops[j]))
            j += 1

    return result


# ── Top-level comparison ─────────────────────────────────────────────────────

def compare_logs(
    ref_events: list[LogEvent],
    debug_events: list[LogEvent],
    *,
    ignore_config_rom: bool = False,
) -> tuple[list[DiffLine], dict[str, int]]:
    """Full comparison pipeline: extract → normalize → diff.

    Returns (diff_lines, summary_counts).
    """
    ref_init = extract_init_sequence(ref_events)
    dbg_init = extract_init_sequence(debug_events)

    ref_ops = normalize(ref_init, ignore_config_rom=ignore_config_rom)
    dbg_ops = normalize(dbg_init, ignore_config_rom=ignore_config_rom)

    diff = diff_sequences(ref_ops, dbg_ops)

    summary = {
        "match": sum(1 for d in diff if d.status == DiffStatus.MATCH),
        "mismatch": sum(1 for d in diff if d.status == DiffStatus.MISMATCH),
        "ref_only": sum(1 for d in diff if d.status == DiffStatus.REF_ONLY),
        "debug_only": sum(1 for d in diff if d.status == DiffStatus.DEBUG_ONLY),
        "ref_ops": len(ref_ops),
        "debug_ops": len(dbg_ops),
    }

    return diff, summary
