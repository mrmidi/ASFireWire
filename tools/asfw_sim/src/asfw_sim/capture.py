"""Capture format: MCP driver-ring telemetry, normalised for the simulator.

A capture is the durable, re-analysable form of one hardware run. Ad-hoc parsing
in a shell one-liner is how a 2.7-second window got read as "flat" when the same
stream was ramping (FINDINGS F8), so the rule here is: **import once, analyse
many times, and keep the raw MCP payload alongside the parsed form** so a parser
fix can be replayed against old runs.

Schema ``asfw.sim.capture.v1``:

```json
{
  "schema": "asfw.sim.capture.v1",
  "device": "Duet", "sample_rate": 48000,
  "captured_at": "2026-07-19T17:10:00Z",
  "notes": "cold start to silence in ~57 s",
  "geometry": { "kTxPreparationLeadPackets": 678, ... },
  "records": [
    {"seq": 99924, "t_ns": 8..., "tag": "TxPrepFrame",
     "fields": {"target": 2554016, "after": 2553176, "write": 2551616}}
  ]
}
```

`fields` keeps the driver's own key names verbatim -- no renaming, so a record is
always traceable to the `ASFW_LOG` format string that produced it. Paired values
like `under=12/925624` and `prepared=a/b/c` expand to `under_0`, `under_1`, ...
"""

from __future__ import annotations

import json
import re
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

from .geometry import Geometry
from .headers import load_driver_headers

__all__ = ["Record", "Capture", "SCHEMA", "parse_mcp_response", "load_capture"]

SCHEMA = "asfw.sim.capture.v1"

#: `[Tag]` -- matched non-greedily so EVERY bracket in a message is found.
#: A greedy `\[(\w+)\]\s*(.*)` with DOTALL yields only the outer category
#: bracket, which silently tagged every record "DirectAudio".
_TAG = re.compile(r"\[(\w+)\]")
#: `key=v`, `key=v/v`, `key=v/v/v`, signed; also `fail=name`
_KV = re.compile(r"(\w+)=(-?\d+(?:/-?\d+)*|[A-Za-z][\w-]*)")

#: Tags the simulator knows how to interpret. Anything else is kept verbatim
#: but not promoted into a series.
KNOWN_TAGS = frozenset(
    {
        "TxPrepFrame",
        "TxPrepRange",
        "TxPrep",
        "TxReplay",
        "PayloadWriter",
        "RxReplayReset",
        "TxProducerFatal",
    }
)


@dataclass
class Record:
    seq: int
    t_ns: int
    tag: str
    fields: dict[str, Any] = field(default_factory=dict)
    #: PayloadWriter emits three different shapes under one tag.
    subtag: str | None = None

    def get(self, key: str, default: Any = None) -> Any:
        return self.fields.get(key, default)


@dataclass
class Capture:
    device: str = "unknown"
    sample_rate: int = 48_000
    captured_at: str = ""
    notes: str = ""
    geometry: dict[str, int] = field(default_factory=dict)
    records: list[Record] = field(default_factory=list)

    # --- series ------------------------------------------------------------
    def by_tag(self, tag: str, subtag: str | None = None) -> list[Record]:
        return [
            r
            for r in self.records
            if r.tag == tag and (subtag is None or r.subtag == subtag)
        ]

    def cursor_series(self) -> list[tuple[float, int, int]]:
        """``(t_seconds, W, E)`` from every record that carries both cursors.

        Time comes from `W` divided by the sample rate, not from the host
        timestamp: `W` is the CoreAudio frame counter and is exact, while ring
        timestamps are emission times subject to rate limiting.
        """
        out: list[tuple[float, int, int]] = []
        for r in self.by_tag("TxPrepFrame"):
            w, e = r.get("write"), r.get("after")
            if w is not None and e is not None:
                out.append((w / self.sample_rate, w, e))
        for r in self.by_tag("PayloadWriter", "deficit"):
            w, e = r.get("write"), r.get("exposed")
            if w is not None and e is not None:
                out.append((w / self.sample_rate, w, e))
        return sorted(set(out))

    def deficit_slope_per_s(self) -> float | None:
        """Least-squares slope of ``W - E``. None if fewer than two points."""
        pts = self.cursor_series()
        if len(pts) < 2:
            return None
        n = len(pts)
        xs = [t for t, _, _ in pts]
        ys = [w - e for _, w, e in pts]
        mx, my = sum(xs) / n, sum(ys) / n
        denom = sum((x - mx) ** 2 for x in xs)
        if denom == 0:
            return None
        return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / denom

    def cursor_rates(self) -> tuple[float, float] | None:
        """``(W frames/s, E frames/s)`` across the capture."""
        pts = self.cursor_series()
        if len(pts) < 2:
            return None
        (t0, w0, e0), (t1, w1, e1) = pts[0], pts[-1]
        dt = t1 - t0
        if dt <= 0:
            return None
        return (w1 - w0) / dt, (e1 - e0) / dt

    def replay_failure_counts(self) -> dict[str, int]:
        counts: dict[str, int] = {}
        for r in self.by_tag("TxReplay"):
            name = r.get("fail") or ("reclamped" if r.subtag == "reclamped" else None)
            if name:
                counts[name] = counts.get(name, 0) + 1
        return counts

    def ahead_count_in_window(self, t_start_s: float, t_end_s: float) -> int:
        """Count [TxReplay] fail=ahead records between two time bounds.

        Time is derived from the record's position in the capture (t_ns).
        This is a LOWER BOUND: driver ring records are rate-limited, so the
        actual number of ahead events may be higher.
        """
        t_start_ns = int(t_start_s * 1_000_000_000)
        t_end_ns = int(t_end_s * 1_000_000_000)
        count = 0
        for r in self.by_tag("TxReplay"):
            if r.get("fail") != "ahead":
                continue
            if t_start_ns <= r.t_ns <= t_end_ns:
                count += 1
        return count

    def silent(self) -> bool:
        """Did any `[PayloadWriter] delta` record show zero frames written?"""
        deltas = self.by_tag("PayloadWriter", "delta")
        return bool(deltas) and all(r.get("w") == 0 for r in deltas)

    # --- persistence -------------------------------------------------------
    def to_json(self) -> str:
        return json.dumps(
            {
                "schema": SCHEMA,
                "device": self.device,
                "sample_rate": self.sample_rate,
                "captured_at": self.captured_at
                or datetime.now(timezone.utc).isoformat(timespec="seconds"),
                "notes": self.notes,
                "geometry": self.geometry,
                "records": [asdict(r) for r in self.records],
            },
            indent=2,
            sort_keys=False,
        )

    def save(self, path: Path | str) -> Path:
        path = Path(path)
        path.write_text(self.to_json(), encoding="utf-8")
        return path

    def geometry_object(self) -> Geometry:
        """The `Geometry` this capture was taken under.

        Falls back to the live headers when the capture predates geometry
        recording -- with a warning-worthy caveat: conclusions then assume the
        tree has not moved since.
        """
        return Geometry.from_headers(self.sample_rate)

    def summary(self) -> str:
        pts = self.cursor_series()
        rates = self.cursor_rates()
        slope = self.deficit_slope_per_s()
        lines = [
            f"device={self.device} rate={self.sample_rate} records={len(self.records)}",
            f"  captured_at: {self.captured_at}",
        ]
        if self.notes:
            lines.append(f"  notes: {self.notes}")
        tags: dict[str, int] = {}
        for r in self.records:
            key = f"{r.tag}/{r.subtag}" if r.subtag else r.tag
            tags[key] = tags.get(key, 0) + 1
        lines.append("  tags: " + ", ".join(f"{k}={v}" for k, v in sorted(tags.items())))
        if pts:
            t0, w0, e0 = pts[0]
            t1, w1, e1 = pts[-1]
            lines += [
                f"  span: {t1 - t0:.1f} s ({len(pts)} cursor points)",
                f"  lead E-W: {e0 - w0:+d} -> {e1 - w1:+d} frames",
            ]
        if slope is not None:
            lines.append(f"  deficit slope: {slope:+.1f} frames/s (least squares)")
        if rates:
            wr, er = rates
            ppm = 1e6 * (wr - er) / wr if wr else 0.0
            lines += [
                f"  W rate: {wr:.1f} frames/s",
                f"  E rate: {er:.1f} frames/s   ({ppm:+.0f} ppm slow)",
            ]
        fails = self.replay_failure_counts()
        lines.append(f"  replay failures: {fails or 'none'}")
        lines.append(f"  silent: {self.silent()}")
        return "\n".join(lines)


# --- parsing ------------------------------------------------------------------


def _coerce(value: str) -> Any:
    try:
        return int(value)
    except ValueError:
        return value


def _expand(key: str, raw: str, into: dict[str, Any]) -> None:
    if "/" in raw:
        for i, part in enumerate(raw.split("/")):
            into[f"{key}_{i}"] = _coerce(part)
        into[key] = raw
    else:
        into[key] = _coerce(raw)


def parse_message(message: str) -> tuple[str | None, str | None, dict[str, Any]]:
    """Split one ring message into ``(tag, subtag, fields)``.

    Driver messages carry a category prefix and then a tag, e.g.
    ``[DirectAudio] [PayloadWriter] deficit sample=... write=...``.
    """
    tag = subtag = None
    rest = message
    matches = list(_TAG.finditer(message))
    if not matches:
        return None, None, {}
    # Prefer a known tag; the driver prefixes every message with its category
    # bracket, so the interesting tag is usually the second one.
    chosen = next((m for m in matches if m.group(1) in KNOWN_TAGS), matches[-1])
    tag = chosen.group(1)
    rest = message[chosen.end():].strip()

    if tag == "PayloadWriter":
        head = rest.split(None, 1)[0] if rest.split() else ""
        if head in {"delta", "last", "deficit", "anomaly"}:
            subtag = head
    elif tag == "TxReplay" and rest.startswith("reclamped"):
        subtag = "reclamped"

    fields: dict[str, Any] = {}
    for key, raw in _KV.findall(rest):
        _expand(key, raw, fields)
    return tag, subtag, fields


def parse_mcp_response(payload: dict | str) -> list[Record]:
    """Records from one `asfw_log_query` response (raw dict or JSON text)."""
    if isinstance(payload, str):
        payload = json.loads(payload)
    data = payload.get("structuredContent", payload).get("data", {})
    out: list[Record] = []
    for raw in data.get("records", []):
        tag, subtag, fields = parse_message(raw.get("message", ""))
        if tag is None:
            continue
        out.append(
            Record(
                seq=int(raw.get("sequence", 0)),
                t_ns=int(raw.get("timestampNs", 0)),
                tag=tag,
                subtag=subtag,
                fields=fields,
            )
        )
    return out


def capture_from_responses(
    payloads: Iterable[dict | str],
    *,
    device: str = "unknown",
    sample_rate: int = 48_000,
    notes: str = "",
    record_geometry: bool = True,
) -> Capture:
    """Build one capture by merging several `asfw_log_query` responses.

    Responses are commonly fetched per tag (`contains: "[TxPrepFrame]"` etc.),
    so records are de-duplicated on sequence and re-sorted.
    """
    seen: dict[int, Record] = {}
    for payload in payloads:
        for record in parse_mcp_response(payload):
            seen.setdefault(record.seq, record)

    geometry: dict[str, int] = {}
    if record_geometry:
        h = load_driver_headers()
        geometry = {
            "kTxPreparationLeadPackets": h.tx_preparation_lead_packets,
            "kTxCoverageLeadPackets": h.tx_coverage_lead_packets,
            "kTxSharedSlotPackets": h.tx_shared_slot_packets,
            "kTxDataHorizonPackets": h.tx_data_horizon_packets,
            "kTxExposureLeadFrames": h.tx_exposure_lead_frames,
            "kHalIoPeriodFrames": h.hal_io_period_frames,
            "kFrameRingFrames": h.frame_ring_frames,
            "kTimelineSlots": h.timeline_slots,
            "replay_kCapacity": h.replay_capacity,
            "replay_kReadDelay": h.replay_read_delay,
        }

    return Capture(
        device=device,
        sample_rate=sample_rate,
        captured_at=datetime.now(timezone.utc).isoformat(timespec="seconds"),
        notes=notes,
        geometry=geometry,
        records=sorted(seen.values(), key=lambda r: r.seq),
    )


def load_capture(path: Path | str) -> Capture:
    raw = json.loads(Path(path).read_text(encoding="utf-8"))
    if raw.get("schema") != SCHEMA:
        raise ValueError(f"{path}: expected schema {SCHEMA}, got {raw.get('schema')!r}")
    return Capture(
        device=raw.get("device", "unknown"),
        sample_rate=int(raw.get("sample_rate", 48_000)),
        captured_at=raw.get("captured_at", ""),
        notes=raw.get("notes", ""),
        geometry=raw.get("geometry", {}),
        records=[Record(**r) for r in raw.get("records", [])],
    )
