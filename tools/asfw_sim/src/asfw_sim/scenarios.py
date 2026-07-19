"""YAML scenarios: a hypothesis as a file you can run and falsify.

A scenario is deliberately more than a config. It names a claim, states the
geometry that claim is about, and declares what must be true if the claim holds
(``expect:``). That makes it runnable in CI and makes a wrong hypothesis fail
loudly instead of quietly producing a number someone reads optimistically.

```yaml
name: readdelay-1024
description: F3 candidate -- widen the producer-stall recovery budget.
base: driver          # inherit the live headers; `none` demands a full geometry
rate: 48000

geometry:             # overrides applied on top of `base`
  replay_read_delay: 1024
  replay_capacity: 2048

scenario:
  duration_s: 20
  stall_ms: 100
  stall_at_s: 5

expect:
  collapsed: false
  written_fraction_min: 0.95
```

`sweep:` replaces `expect:` for exploration -- any geometry or scenario field may
take a list, and the cartesian product is run.

Overrides are checked against the header's own ``static_assert`` set via
``derive``: a scenario whose geometry would not compile is reported, not
silently simulated. That check is advisory (a hypothesis may deliberately
explore an invalid geometry) unless ``require_valid: true``.
"""

from __future__ import annotations

import itertools
from dataclasses import dataclass, field, fields
from pathlib import Path
from typing import Any

import yaml

from .derive import derive
from .geometry import SUPPORTED_RATES, Geometry
from .headers import DriverHeaders, load_driver_headers
from .sim import SimConfig, SimResult, run

__all__ = [
    "Scenario",
    "ScenarioError",
    "ScenarioOutcome",
    "load_scenario",
    "load_scenario_dir",
    "run_scenario",
]

CYCLES_PER_SECOND = 8_000

_GEOMETRY_FIELDS = {f.name for f in fields(Geometry)}
_SCENARIO_KEYS = {
    "duration_s",
    "stall_ms",
    "stall_at_s",
    "wake_latency_cycles",
    "unbounded_replay_history",
    "rx_drop_every_cycles",
    "trace_every_cycles",
}
_TOP_LEVEL_KEYS = {
    "name",
    "description",
    "base",
    "rate",
    "geometry",
    "scenario",
    "expect",
    "sweep",
    "require_valid",
}


class ScenarioError(ValueError):
    """A scenario file is malformed, or asserts something impossible."""


@dataclass
class Scenario:
    name: str
    path: Path | None = None
    description: str = ""
    base: str = "driver"
    rate: int = 48_000
    geometry_overrides: dict[str, Any] = field(default_factory=dict)
    scenario: dict[str, Any] = field(default_factory=dict)
    expect: dict[str, Any] = field(default_factory=dict)
    sweep: dict[str, list[Any]] = field(default_factory=dict)
    require_valid: bool = False

    # --- building -----------------------------------------------------------
    def geometry(
        self, headers: DriverHeaders | None = None, **extra: Any
    ) -> Geometry:
        if self.base == "driver":
            base = Geometry.from_headers(self.rate, headers)
        elif self.base == "none":
            base = _geometry_from_scratch(self.rate, self.geometry_overrides)
        else:
            raise ScenarioError(
                f"{self.name}: base must be 'driver' or 'none', got {self.base!r}"
            )
        merged = {**self.geometry_overrides, **extra}
        merged.pop("rate", None)
        unknown = set(merged) - _GEOMETRY_FIELDS
        if unknown:
            raise ScenarioError(
                f"{self.name}: unknown geometry field(s) {sorted(unknown)}; "
                f"valid: {sorted(_GEOMETRY_FIELDS)}"
            )
        return base.evolve(**merged) if merged else base

    def sim_config(self, geometry: Geometry, **extra: Any) -> SimConfig:
        merged = {**self.scenario, **extra}
        unknown = set(merged) - _SCENARIO_KEYS
        if unknown:
            raise ScenarioError(
                f"{self.name}: unknown scenario key(s) {sorted(unknown)}; "
                f"valid: {sorted(_SCENARIO_KEYS)}"
            )
        duration_s = merged.get("duration_s", 20)
        stall_ms = merged.get("stall_ms", 0)
        stall_at_s = merged.get("stall_at_s", 5)
        return SimConfig(
            geometry=geometry,
            duration_cycles=int(duration_s * CYCLES_PER_SECOND),
            stall_at_cycle=int(stall_at_s * CYCLES_PER_SECOND),
            stall_cycles=int(stall_ms * 8),
            wake_latency_cycles=int(merged.get("wake_latency_cycles", 0)),
            unbounded_replay_history=bool(
                merged.get("unbounded_replay_history", False)
            ),
            rx_drop_every_cycles=int(merged.get("rx_drop_every_cycles", 0)),
            trace_every_cycles=int(merged.get("trace_every_cycles", 0)),
        )

    @property
    def sweep_points(self) -> list[dict[str, Any]]:
        """Cartesian product of the ``sweep:`` block ([{}] when absent)."""
        if not self.sweep:
            return [{}]
        keys = sorted(self.sweep)
        return [
            dict(zip(keys, combo))
            for combo in itertools.product(*(self.sweep[k] for k in keys))
        ]


@dataclass
class ScenarioOutcome:
    scenario: Scenario
    point: dict[str, Any]
    geometry: Geometry
    result: SimResult
    failures: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.failures

    @property
    def label(self) -> str:
        if not self.point:
            return self.scenario.name
        detail = " ".join(f"{k}={v}" for k, v in sorted(self.point.items()))
        return f"{self.scenario.name} [{detail}]"


# --- loading ------------------------------------------------------------------


def _geometry_from_scratch(rate: int, overrides: dict[str, Any]) -> Geometry:
    required = _GEOMETRY_FIELDS - {
        "sample_rate",
        "profile_name",
        "rx_transfer_delay_ticks",
        "tx_transfer_delay_ticks",
    }
    missing = required - set(overrides)
    if missing:
        raise ScenarioError(
            f"base: none requires every geometry field; missing {sorted(missing)}"
        )
    return Geometry(
        sample_rate=rate,
        profile_name="scenario",
        **{k: v for k, v in overrides.items() if k in _GEOMETRY_FIELDS
           and k not in {"sample_rate", "profile_name"}},
    )


def load_scenario(path: Path | str) -> Scenario:
    path = Path(path)
    try:
        raw = yaml.safe_load(path.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        raise ScenarioError(f"{path}: invalid YAML: {exc}") from exc
    if not isinstance(raw, dict):
        raise ScenarioError(f"{path}: top level must be a mapping")

    unknown = set(raw) - _TOP_LEVEL_KEYS
    if unknown:
        raise ScenarioError(
            f"{path}: unknown key(s) {sorted(unknown)}; valid: {sorted(_TOP_LEVEL_KEYS)}"
        )

    rate = int(raw.get("rate", 48_000))
    if rate not in SUPPORTED_RATES:
        raise ScenarioError(
            f"{path}: rate {rate} is not implemented in the driver "
            f"(supported: {list(SUPPORTED_RATES)})"
        )

    sweep = raw.get("sweep") or {}
    if sweep and not all(isinstance(v, list) for v in sweep.values()):
        raise ScenarioError(f"{path}: every sweep value must be a list")
    if sweep and raw.get("expect"):
        raise ScenarioError(
            f"{path}: 'sweep' explores and 'expect' asserts; use one or the other"
        )

    return Scenario(
        name=str(raw.get("name") or path.stem),
        path=path,
        description=str(raw.get("description", "")).strip(),
        base=str(raw.get("base", "driver")),
        rate=rate,
        geometry_overrides=dict(raw.get("geometry") or {}),
        scenario=dict(raw.get("scenario") or {}),
        expect=dict(raw.get("expect") or {}),
        sweep={k: list(v) for k, v in sweep.items()},
        require_valid=bool(raw.get("require_valid", False)),
    )


def load_scenario_dir(path: Path | str) -> list[Scenario]:
    path = Path(path)
    files = sorted([*path.glob("*.yaml"), *path.glob("*.yml")])
    if not files:
        raise ScenarioError(f"no .yaml scenarios in {path}")
    return [load_scenario(f) for f in files]


# --- running ------------------------------------------------------------------

_EXPECT_CHECKS = {
    "collapsed": lambda r, v: (r.collapsed == v, f"collapsed={r.collapsed}"),
    "written_fraction_min": lambda r, v: (
        r.written_fraction >= v,
        f"written_fraction={r.written_fraction:.4f}",
    ),
    "written_fraction_max": lambda r, v: (
        r.written_fraction <= v,
        f"written_fraction={r.written_fraction:.4f}",
    ),
    "max_replay_failures": lambda r, v: (
        sum(r.replay_failures.values()) <= v,
        f"replay_failures={sum(r.replay_failures.values())}",
    ),
    "max_align_count": lambda r, v: (r.align_count <= v, f"align={r.align_count}"),
    "max_reclamped": lambda r, v: (r.reclamped <= v, f"reclamped={r.reclamped}"),
    "min_data_packet_fraction": lambda r, v: (
        r.data_packet_fraction >= v,
        f"data_packet_fraction={r.data_packet_fraction:.4f}",
    ),
}


def _check_geometry_validity(
    scenario: Scenario, geometry: Geometry, headers: DriverHeaders
) -> list[str]:
    """Would this geometry satisfy the header's own static_asserts?"""
    derived = derive(
        headers,
        io_budget_frames=geometry.hal_io_period_frames,
        frame_ring_frames=geometry.frame_ring_frames,
        shared_slot_packets=geometry.tx_shared_slot_packets,
        timeline_slots=geometry.timeline_slots,
        data_horizon_packets=geometry.tx_data_horizon_packets,
    )
    return [f"{c.name} ({c.detail})" for c in derived.failures]


def run_scenario(
    scenario: Scenario, headers: DriverHeaders | None = None
) -> list[ScenarioOutcome]:
    """Run a scenario (every sweep point) and evaluate its ``expect`` block."""
    headers = headers or load_driver_headers()
    outcomes: list[ScenarioOutcome] = []

    for point in scenario.sweep_points:
        geometry_extra = {k: v for k, v in point.items() if k in _GEOMETRY_FIELDS}
        scenario_extra = {k: v for k, v in point.items() if k in _SCENARIO_KEYS}
        stray = set(point) - _GEOMETRY_FIELDS - _SCENARIO_KEYS
        if stray:
            raise ScenarioError(
                f"{scenario.name}: sweep key(s) {sorted(stray)} are neither "
                "geometry nor scenario fields"
            )

        geometry = scenario.geometry(headers, **geometry_extra)
        result = run(scenario.sim_config(geometry, **scenario_extra))
        outcome = ScenarioOutcome(scenario, point, geometry, result)

        invalid = _check_geometry_validity(scenario, geometry, headers)
        if invalid:
            message = "geometry would not compile: " + "; ".join(invalid)
            if scenario.require_valid:
                outcome.failures.append(message)
            else:
                outcome.warnings.append(message)

        for key, expected in scenario.expect.items():
            check = _EXPECT_CHECKS.get(key)
            if check is None:
                raise ScenarioError(
                    f"{scenario.name}: unknown expect key {key!r}; "
                    f"valid: {sorted(_EXPECT_CHECKS)}"
                )
            passed, actual = check(result, expected)
            if not passed:
                outcome.failures.append(f"{key}: expected {expected}, got {actual}")

        outcomes.append(outcome)

    return outcomes
