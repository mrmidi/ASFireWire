"""Every shipped YAML hypothesis must hold, and the loader must reject junk."""

from __future__ import annotations

from pathlib import Path

import pytest

from asfw_sim.headers import load_driver_headers
from asfw_sim.scenarios import (
    ScenarioError,
    load_scenario,
    load_scenario_dir,
    run_scenario,
)

SCENARIO_DIR = Path(__file__).resolve().parents[1] / "scenarios"


@pytest.fixture(scope="module")
def headers():
    return load_driver_headers()


def _scenarios():
    return load_scenario_dir(SCENARIO_DIR)


def test_scenario_directory_is_not_empty():
    assert _scenarios()


@pytest.mark.parametrize(
    "scenario", _scenarios(), ids=lambda s: s.name
)
def test_shipped_scenario_holds(scenario, headers):
    if not scenario.expect and not scenario.require_valid:
        pytest.skip(f"{scenario.name} is exploratory (sweep, no expect block)")
    for outcome in run_scenario(scenario, headers):
        assert outcome.ok, f"{outcome.label}: " + "; ".join(outcome.failures)


@pytest.mark.parametrize("scenario", _scenarios(), ids=lambda s: s.name)
def test_shipped_scenario_documents_itself(scenario):
    """A hypothesis without a stated claim is a config file, not evidence."""
    assert len(scenario.description) > 40, (
        f"{scenario.name}: describe the claim this scenario tests"
    )


# --- loader validation --------------------------------------------------------


def _write(tmp_path: Path, body: str) -> Path:
    path = tmp_path / "scenario.yaml"
    path.write_text(body, encoding="utf-8")
    return path


def test_unknown_top_level_key_is_rejected(tmp_path):
    with pytest.raises(ScenarioError, match="unknown key"):
        load_scenario(_write(tmp_path, "name: x\ngeomtry: {}\n"))


def test_unknown_geometry_field_is_rejected(tmp_path, headers):
    scenario = load_scenario(
        _write(tmp_path, "name: x\ngeometry:\n  replay_red_delay: 1024\n")
    )
    with pytest.raises(ScenarioError, match="unknown geometry field"):
        scenario.geometry(headers)


def test_unsupported_rate_is_rejected(tmp_path):
    with pytest.raises(ScenarioError, match="not implemented"):
        load_scenario(_write(tmp_path, "name: x\nrate: 96000\n"))


def test_sweep_and_expect_are_mutually_exclusive(tmp_path):
    with pytest.raises(ScenarioError, match="one or the other"):
        load_scenario(
            _write(
                tmp_path,
                "name: x\nsweep:\n  replay_capacity: [512]\nexpect:\n  collapsed: false\n",
            )
        )


def test_sweep_values_must_be_lists(tmp_path):
    with pytest.raises(ScenarioError, match="must be a list"):
        load_scenario(_write(tmp_path, "name: x\nsweep:\n  replay_capacity: 512\n"))


def test_unknown_expect_key_is_rejected(tmp_path, headers):
    scenario = load_scenario(
        _write(tmp_path, "name: x\nexpect:\n  sounds_nice: true\n")
    )
    with pytest.raises(ScenarioError, match="unknown expect key"):
        run_scenario(scenario, headers)


def test_sweep_expands_to_the_cartesian_product(tmp_path):
    scenario = load_scenario(
        _write(
            tmp_path,
            "name: x\nsweep:\n"
            "  replay_capacity: [512, 2048]\n"
            "  stall_ms: [0, 50, 100]\n",
        )
    )
    assert len(scenario.sweep_points) == 6


def test_require_valid_fails_an_uncompilable_geometry(tmp_path, headers):
    """shared(912) < preparationLead for a 4096-frame IO budget."""
    scenario = load_scenario(
        _write(
            tmp_path,
            "name: x\nrequire_valid: true\n"
            "geometry:\n  hal_io_period_frames: 4096\n"
            "scenario:\n  duration_s: 2\n",
        )
    )
    outcome = run_scenario(scenario, headers)[0]
    assert not outcome.ok
    assert any("would not compile" in f for f in outcome.failures)


def test_invalid_geometry_is_only_a_warning_without_require_valid(tmp_path, headers):
    scenario = load_scenario(
        _write(
            tmp_path,
            "name: x\ngeometry:\n  hal_io_period_frames: 4096\n"
            "scenario:\n  duration_s: 2\n",
        )
    )
    outcome = run_scenario(scenario, headers)[0]
    assert outcome.ok
    assert any("would not compile" in w for w in outcome.warnings)


def test_rx_loss_does_not_skip_cycle_events(headers):
    """[P1] RX-loss injection must only skip replay publishing, not whole cycle events.
    Verify that an RX-loss run has the same final completion_cursor and write_frontier
    as a same-duration baseline run.
    """
    from asfw_sim.sim import SimConfig, run, WARMUP_CYCLES
    from asfw_sim.geometry import Geometry
    g = Geometry.from_headers(48000, headers)

    duration = WARMUP_CYCLES + 8000
    baseline = run(SimConfig(geometry=g, duration_cycles=duration, rx_drop_every_cycles=0))
    rx_loss = run(SimConfig(geometry=g, duration_cycles=duration, rx_drop_every_cycles=200))

    assert rx_loss.rx_dropped > 0, "drops never fired — duration too short vs WARMUP_CYCLES"
    assert baseline.completion_cursor == rx_loss.completion_cursor
    assert baseline.write_frontier == rx_loss.write_frontier
