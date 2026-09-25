#!/usr/bin/env python3
"""Summarise a baseline evidence directory into a cross-session distribution.

A baseline is N measurement *sessions* (each a fresh stream start, i.e. a new
rtl_loopback process) of M trials each. Recorded RTL on the midi branch was
stable within a session but landed in different 288-frame "lap" classes across
stream restarts (8.8 ms .. 84 ms), so one session's median is not a baseline.
This script reports the distribution of per-session medians and groups them
into classes.

Input layout (written by capture_baseline.sh):
    <dir>/provenance.json
    <dir>/hal_snapshot.json            (optional, asfw.hal_geometry.v1)
    <dir>/hal_clock.json               (optional, asfw.hal_geometry.v1)
    <dir>/sessions/session-NN.json     (asfw.rtl_loopback.v1)

Output: <dir>/summary.json and <dir>/summary.md.

Standard library only.

    python3 tools/baseline/summarize_baseline.py documentation/baselines/<run>
"""

from __future__ import annotations

import argparse
import json
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

RTL_SCHEMA = "asfw.rtl_loopback.v1"
HAL_SCHEMA = "asfw.hal_geometry.v1"


@dataclass
class Session:
    name: str
    accepted: int
    trials_run: int
    rejected: dict[str, int]
    rtl_raw: float | None
    rtl_ts: float | None
    scheduling: float | None
    residual: float | None
    sample_rate: float | None
    buffer_frames: int | None
    timed_out: bool


@dataclass
class LapClass:
    centre: float
    sessions: list[str] = field(default_factory=list)
    medians: list[float] = field(default_factory=list)


def _median_of(stat: dict[str, Any] | None) -> float | None:
    if not stat or stat.get("n", 0) <= 0:
        return None
    value = stat.get("median_frames")
    return float(value) if value is not None else None


def load_session(path: Path) -> Session:
    doc = json.loads(path.read_text())
    if doc.get("schema") != RTL_SCHEMA:
        raise ValueError(f"{path}: not an {RTL_SCHEMA} document")
    summary = doc["summary"]
    prov = doc["provenance"]
    residual = summary.get("residual_frames")
    return Session(
        name=path.stem,
        accepted=int(summary["accepted"]),
        trials_run=int(summary["trials_run"]),
        rejected={k: int(v) for k, v in summary.get("rejected", {}).items()},
        rtl_raw=_median_of(summary.get("rtl_raw")),
        rtl_ts=_median_of(summary.get("rtl_ts")),
        scheduling=_median_of(summary.get("scheduling_distance")),
        residual=float(residual) if residual is not None else None,
        sample_rate=prov.get("sample_rate_hz"),
        buffer_frames=prov.get("buffer_frames_actual"),
        timed_out=bool(prov.get("timed_out", False)),
    )


def classify(values: list[tuple[str, float]], tolerance: float) -> list[LapClass]:
    """Group session medians: consecutive sorted values closer than `tolerance`
    frames belong to one class. Classes are returned in ascending order."""
    classes: list[LapClass] = []
    for name, value in sorted(values, key=lambda item: item[1]):
        if classes and value - classes[-1].medians[-1] <= tolerance:
            cls = classes[-1]
        else:
            cls = LapClass(centre=value)
            classes.append(cls)
        cls.sessions.append(name)
        cls.medians.append(value)
        cls.centre = statistics.median(cls.medians)
    return classes


def distribution(values: list[float]) -> dict[str, Any] | None:
    if not values:
        return None
    return {
        "n": len(values),
        "median": statistics.median(values),
        "min": min(values),
        "max": max(values),
        "spread": max(values) - min(values),
        "sd": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def lap_offsets(classes: list[LapClass], lap_frames: float) -> list[dict[str, Any]]:
    """Offsets of each class from the lowest, in frames and in laps."""
    if not classes:
        return []
    base = classes[0].centre
    out = []
    for cls in classes:
        offset = cls.centre - base
        laps = offset / lap_frames if lap_frames > 0 else None
        out.append({
            "centre_frames": cls.centre,
            "offset_frames": offset,
            "offset_laps": laps,
            "near_integer_laps": laps is not None and abs(laps - round(laps)) < 0.05,
            "sessions": cls.sessions,
        })
    return out


def summarize(directory: Path, tolerance: float, lap_frames: float) -> dict[str, Any]:
    session_dir = directory / "sessions"
    paths = sorted(session_dir.glob("session-*.json")) if session_dir.is_dir() else []
    sessions = [load_session(p) for p in paths]

    rejected_total: dict[str, int] = {}
    for s in sessions:
        for key, count in s.rejected.items():
            rejected_total[key] = rejected_total.get(key, 0) + count

    measured = [s for s in sessions if s.accepted > 0 and s.rtl_raw is not None]
    rates = {s.sample_rate for s in sessions if s.sample_rate}
    buffers = {s.buffer_frames for s in sessions if s.buffer_frames}
    classes = classify([(s.name, s.rtl_raw) for s in measured], tolerance)

    result: dict[str, Any] = {
        "schema": "asfw.baseline_summary.v1",
        "directory": str(directory),
        "sessions_total": len(sessions),
        "sessions_measured": len(measured),
        "sessions_without_accepted_trials": [s.name for s in sessions if s.accepted == 0],
        "sessions_timed_out": [s.name for s in sessions if s.timed_out],
        "trials_run": sum(s.trials_run for s in sessions),
        "trials_accepted": sum(s.accepted for s in sessions),
        "rejected_by_verdict": dict(sorted(rejected_total.items())),
        # Mixed configurations are not one baseline: say so instead of averaging.
        "sample_rates_hz": sorted(rates),
        "buffer_frames": sorted(buffers),
        "consistent_configuration": len(rates) <= 1 and len(buffers) <= 1,
        "session_median_rtl_raw_frames": distribution([s.rtl_raw for s in measured]),
        "session_median_rtl_ts_frames": distribution(
            [s.rtl_ts for s in measured if s.rtl_ts is not None]),
        "session_median_scheduling_frames": distribution(
            [s.scheduling for s in measured if s.scheduling is not None]),
        "session_residual_frames": distribution(
            [s.residual for s in measured if s.residual is not None]),
        "class_tolerance_frames": tolerance,
        "lap_frames": lap_frames,
        "rtl_raw_classes": lap_offsets(classes, lap_frames),
        "sessions": [s.__dict__ for s in sessions],
    }

    for key, name in (("hal_snapshot", "hal_snapshot.json"), ("hal_clock", "hal_clock.json")):
        path = directory / name
        if path.is_file():
            doc = json.loads(path.read_text())
            if doc.get("schema") == HAL_SCHEMA and doc.get("devices"):
                result[key] = doc["devices"][0]
    return result


def _fmt(value: Any, digits: int = 2) -> str:
    if value is None:
        return "—"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def render_markdown(s: dict[str, Any]) -> str:
    rate = s["sample_rates_hz"][0] if len(s["sample_rates_hz"]) == 1 else None

    def ms(frames: float | None) -> str:
        return _fmt(frames * 1000.0 / rate, 3) if frames is not None and rate else "—"

    lines = [
        "# Baseline summary",
        "",
        f"- sessions: **{s['sessions_measured']} measured** of {s['sessions_total']}",
        f"- trials: **{s['trials_accepted']} accepted** of {s['trials_run']}",
        f"- rejected: {', '.join(f'{k} {v}' for k, v in s['rejected_by_verdict'].items()) or 'none'}",
        f"- sample rate(s): {s['sample_rates_hz']}; buffer frames: {s['buffer_frames']}",
    ]
    if not s["consistent_configuration"]:
        lines.append("- **WARNING: sessions used different rates or buffer sizes -- "
                     "this is not one baseline.**")
    if s["sessions_without_accepted_trials"]:
        lines.append(f"- sessions with no accepted trial: {s['sessions_without_accepted_trials']}")
    lines += ["", "## Distribution of per-session medians", "",
              "| Quantity | n | median fr | median ms | min | max | spread | sd |",
              "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for label, key in (("RTL_raw", "session_median_rtl_raw_frames"),
                       ("RTL_ts", "session_median_rtl_ts_frames"),
                       ("scheduling distance", "session_median_scheduling_frames"),
                       ("residual", "session_residual_frames")):
        d = s[key]
        if d is None:
            lines.append(f"| {label} | 0 | — | — | — | — | — | — |")
        else:
            lines.append(f"| {label} | {d['n']} | {_fmt(d['median'])} | {ms(d['median'])} | "
                         f"{_fmt(d['min'])} | {_fmt(d['max'])} | {_fmt(d['spread'])} | {_fmt(d['sd'])} |")
    lines += ["", f"## RTL_raw classes (tolerance {s['class_tolerance_frames']:g} fr, "
                  f"lap {s['lap_frames']:g} fr)", "",
              "| centre fr | offset fr | offset laps | sessions |", "|---:|---:|---:|---|"]
    for c in s["rtl_raw_classes"]:
        laps = _fmt(c["offset_laps"]) + (" ✓" if c["near_integer_laps"] and c["offset_frames"] else "")
        lines.append(f"| {_fmt(c['centre_frames'])} | {_fmt(c['offset_frames'])} | {laps} | "
                     f"{', '.join(c['sessions'])} |")
    if len(s["rtl_raw_classes"]) > 1:
        lines += ["", "More than one class: the round trip depends on how a stream "
                      "started. Report the distribution, never one session's number."]
    clock = s.get("hal_clock", {}).get("clock") if s.get("hal_clock") else None
    if clock:
        lines += ["", "## HAL-published clock", "",
                  f"- ppm vs nominal: {_fmt(clock.get('ppm_vs_nominal'))}",
                  f"- worst residual: {_fmt(clock.get('worst_residual_frames'))} fr",
                  f"- backward jumps: {clock.get('backward_jumps')}, "
                  f"forward re-anchors: {clock.get('forward_reanchors')}"]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", type=Path)
    parser.add_argument("--class-tolerance", type=float, default=16.0,
                        help="frames within which session medians are one class (default 16)")
    parser.add_argument("--lap-frames", type=float, default=288.0,
                        help="lap size used to express class offsets (default 288)")
    args = parser.parse_args(argv)
    if not args.directory.is_dir():
        print(f"error: {args.directory} is not a directory", file=sys.stderr)
        return 2
    try:
        summary = summarize(args.directory, args.class_tolerance, args.lap_frames)
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    (args.directory / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    markdown = render_markdown(summary)
    (args.directory / "summary.md").write_text(markdown)
    sys.stdout.write(markdown)
    return 0 if summary["sessions_measured"] > 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
