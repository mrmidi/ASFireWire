"""The classifier must separate the causes that share an end state.

RX loss (F6) and a producer stall (F2) both finish with W > E and near-total
silence. They are told apart only by the SLOPE of the deficit, so these tests
pin that: the fingerprints must stay separable, and a slow leak must never be
reported as safe.
"""

from __future__ import annotations

import pytest

from asfw_sim.diagnose import diagnose, fingerprints, frames_lost_per_drop
from asfw_sim.geometry import Geometry


@pytest.fixture(scope="module")
def g48():
    return Geometry.from_headers(48_000)


@pytest.fixture(scope="module")
def prints(g48):
    return {f.cause: f for f in fingerprints(g48)}


def test_healthy_deficit_is_pinned_at_the_exposure_horizon(prints, g48):
    healthy = prints["healthy"]
    assert healthy.deficit_frames == -g48.data_horizon_frames
    assert healthy.deficit_slope_per_s == pytest.approx(0, abs=1)


def test_rx_loss_ramps_and_stall_does_not(prints):
    """The one discriminator. If this fails the classifier is unsound."""
    rx = prints["rx-observation-loss"]
    stall = prints["producer-stall"]
    assert rx.deficit_slope_per_s > 50, "RX loss must show an unbounded ramp"
    assert abs(stall.deficit_slope_per_s) < 20, "a stall must go flat"
    assert rx.deficit_frames > 0 and stall.deficit_frames > 0, (
        "both causes share the same end state; only the slope separates them"
    )


def test_ahead_alone_is_not_diagnostic(prints):
    """A small replay ring emits hundreds of `ahead` with audio intact."""
    churn = prints["replay-ring-churn"]
    assert churn.ahead > 100
    assert churn.written_fraction > 0.95
    assert churn.deficit_frames < 0


def test_scheduler_latency_is_absorbed(prints):
    latency = prints["scheduler-latency"]
    assert latency.written_fraction > 0.95
    assert latency.deficit_frames < 0


def test_calibration_is_about_one_data_packet(g48):
    """E forfeits roughly the frames the missed DATA packet would have carried."""
    per_drop = frames_lost_per_drop(g48)
    assert 4.0 < per_drop < g48.frames_per_data_packet + 1


# --- classification -----------------------------------------------------------


def test_classifies_a_ramp_as_rx_loss(g48):
    d = diagnose(
        deficit_start_frames=-2400,
        deficit_end_frames=380,
        elapsed_s=60,
        geometry=g48,
    )
    assert d.cause.startswith("rx-observation-loss")
    assert d.estimated_rx_loss_per_s is not None


def test_classifies_a_flat_positive_deficit_as_a_stall(g48):
    d = diagnose(
        deficit_start_frames=1030,
        deficit_end_frames=1032,
        elapsed_s=20,
        geometry=g48,
        align_count=1,
    )
    assert d.cause.startswith("producer-stall")


def test_slow_leak_is_never_reported_as_healthy(g48):
    """The dangerous case: a ramp too slow to notice in a short capture.

    An earlier revision used a noise floor of one IO period per second, which
    classified this as a stall -- i.e. as a spent budget rather than one still
    draining. Regression guard.
    """
    d = diagnose(
        deficit_start_frames=-2400,
        deficit_end_frames=-2100,
        elapsed_s=120,
        geometry=g48,
    )
    assert d.cause.startswith("rx-observation-loss")
    assert d.estimated_rx_loss_per_s == pytest.approx(0.4, abs=0.3)


def test_classifies_a_flat_negative_deficit_as_healthy(g48):
    d = diagnose(
        deficit_start_frames=-2400,
        deficit_end_frames=-2400,
        elapsed_s=30,
        geometry=g48,
    )
    assert d.cause == "healthy"


def test_rejects_a_nonpositive_window(g48):
    with pytest.raises(ValueError):
        diagnose(
            deficit_start_frames=0,
            deficit_end_frames=0,
            elapsed_s=0,
            geometry=g48,
        )
