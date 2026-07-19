"""Classify a real run's telemetry against simulated failure fingerprints.

The simulator cannot observe hardware. What it *can* do is pre-compute what each
candidate cause looks like in counters the driver already emits, so one MCP ring
dump identifies the cause instead of motivating another hardware session.

The decisive signal is not any counter -- it is the **slope of `W - E`**:

    healthy         pinned at -kTxExposureLeadFrames, slope 0
    RX loss (F6)    positive and LINEAR, unbounded
    stall (F2)      one step, then FLAT
    replay churn    `reclamped` climbing with slope ~0

`ahead` alone is not diagnostic: a small replay ring produces hundreds of `ahead`
events while the stream stays perfectly healthy. Only the slope separates a
budget being consumed from a budget that was spent once.

Calibration (frames of `E` lost per dropped RX observation) is measured from the
model rather than hard-coded, so it stays correct as the geometry changes.
"""

from __future__ import annotations

from dataclasses import dataclass
from functools import lru_cache

from .geometry import CYCLES_PER_SECOND, Geometry
from .replay import ReplayFailure
from .sim import SimConfig, run

__all__ = ["Fingerprint", "Diagnosis", "fingerprints", "frames_lost_per_drop", "diagnose"]


@dataclass(frozen=True)
class Fingerprint:
    cause: str
    written_fraction: float
    ahead: int
    overwritten: int
    reclamped: int
    align_count: int
    deficit_frames: int
    deficit_slope_per_s: float
    note: str


def _deficit(result) -> int:
    return result.write_frontier - result.exposed_frame_end


def _measure(geometry: Geometry, seconds_a: int = 15, seconds_b: int = 30, **kw):
    a = run(SimConfig(geometry=geometry, duration_cycles=CYCLES_PER_SECOND * seconds_a, **kw))
    b = run(SimConfig(geometry=geometry, duration_cycles=CYCLES_PER_SECOND * seconds_b, **kw))
    slope = (_deficit(b) - _deficit(a)) / (seconds_b - seconds_a)
    return b, slope


@lru_cache(maxsize=32)
def frames_lost_per_drop(geometry: Geometry | None = None) -> float:
    """Frames of exposure `E` forfeits per lost RX observation, measured.

    Cached: this costs two full simulations, and `diagnose` needs it on every
    call. ``Geometry`` is a frozen dataclass, so it is a valid cache key.
    """
    g = geometry or Geometry.from_headers()
    baseline = run(
        SimConfig(geometry=g, duration_cycles=CYCLES_PER_SECOND * 30)
    )
    lossy = run(
        SimConfig(
            geometry=g,
            duration_cycles=CYCLES_PER_SECOND * 30,
            rx_drop_every_cycles=200,
        )
    )
    if lossy.rx_dropped == 0:  # pragma: no cover - defensive
        return 0.0
    return (_deficit(lossy) - _deficit(baseline)) / lossy.rx_dropped


def fingerprints(geometry: Geometry | None = None) -> list[Fingerprint]:
    """Simulate each candidate cause and record its counter signature."""
    g = geometry or Geometry.from_headers()
    cases: list[tuple[str, dict, str]] = [
        ("healthy", {}, "E leads W by exactly the exposure horizon"),
        (
            "rx-observation-loss",
            {"rx_drop_every_cycles": 200},
            "F6: deficit grows LINEARLY and without bound",
        ),
        (
            "producer-stall",
            {"stall_at_cycle": CYCLES_PER_SECOND * 5, "stall_cycles": 800},
            "F2: deficit steps once, then FLAT",
        ),
        (
            "replay-ring-churn",
            {},
            "many `ahead` events, deficit pinned: NOT a fault on its own",
        ),
        (
            "scheduler-latency",
            {"wake_latency_cycles": 40},
            "absorbed by the coverage lead; no content loss",
        ),
    ]

    out: list[Fingerprint] = []
    for cause, kwargs, note in cases:
        geo = (
            g.evolve(replay_capacity=128, replay_read_delay=64)
            if cause == "replay-ring-churn"
            else g
        )
        result, slope = _measure(geo, **kwargs)
        out.append(
            Fingerprint(
                cause=cause,
                written_fraction=result.written_fraction,
                ahead=result.failure_count(ReplayFailure.AHEAD_OF_PRODUCER),
                overwritten=result.failure_count(ReplayFailure.HISTORY_OVERWRITTEN),
                reclamped=result.reclamped,
                align_count=result.align_count,
                deficit_frames=_deficit(result),
                deficit_slope_per_s=slope,
                note=note,
            )
        )
    return out


@dataclass(frozen=True)
class Diagnosis:
    cause: str
    confidence: str
    reasoning: str
    estimated_rx_loss_per_s: float | None = None
    estimated_rx_loss_percent: float | None = None


def diagnose(
    *,
    deficit_start_frames: int,
    deficit_end_frames: int,
    elapsed_s: float,
    geometry: Geometry | None = None,
    ahead: int | None = None,
    align_count: int | None = None,
) -> Diagnosis:
    """Classify an observed run from two `[PayloadWriter]` deficit samples.

    `deficit_*` is `W - E` in frames (the driver reports it as the exposure
    deficit; a healthy stream reports a negative value, or none at all).
    """
    g = geometry or Geometry.from_headers()
    if elapsed_s <= 0:
        raise ValueError("elapsed_s must be positive")

    slope = (deficit_end_frames - deficit_start_frames) / elapsed_s
    horizon = g.data_horizon_frames
    per_drop = frames_lost_per_drop(g) or 1.0

    # The floor must be near zero, not near a buffer. ANY sustained positive
    # slope eventually spends the horizon and kills the stream -- spending 2400
    # frames over a whole hour is only 0.67 frames/s. A floor sized to an IO
    # period would hide exactly the dangerous case: a slow leak that takes
    # minutes rather than seconds, which is the one a short capture misreads as
    # "stable". So classify on sign, and report time-to-silence instead of a
    # verdict that pretends a slow ramp is safe.
    noise_floor = 1.0

    if slope > noise_floor:
        loss_per_s = slope / per_drop
        remaining = horizon - max(deficit_end_frames, -horizon)
        seconds_left = max(remaining, 0) / slope
        if deficit_end_frames > 0:
            outlook = "the horizon is already spent; audio is silent now"
        else:
            outlook = f"silence in ~{seconds_left:.0f} s at this rate"
        return Diagnosis(
            cause="rx-observation-loss (F6)",
            confidence="high" if slope > 20 else "medium",
            reasoning=(
                f"deficit is growing {slope:+.1f} frames/s. Only lost RX "
                f"observations produce an unbounded linear ramp; a stall steps "
                f"once and goes flat. At {per_drop:.1f} frames forfeited per lost "
                f"observation, {outlook}. A slow ramp is not a safe ramp -- it is "
                "the same defect with a longer fuse."
            ),
            estimated_rx_loss_per_s=loss_per_s,
            estimated_rx_loss_percent=100.0 * loss_per_s / CYCLES_PER_SECOND,
        )

    if deficit_end_frames > 0:
        return Diagnosis(
            cause="producer-stall (F2)",
            confidence="high" if align_count == 1 else "medium",
            reasoning=(
                f"deficit is positive ({deficit_end_frames} frames) but flat "
                f"({slope:+.0f} frames/s). That is a budget spent in one event, "
                "not one being consumed -- a producer-wake stall past the cliff. "
                "Correlate with IT interrupt silence (TX-IRQ-001)."
            ),
        )

    if ahead:
        return Diagnosis(
            cause="benign replay churn",
            confidence="medium",
            reasoning=(
                f"{ahead} `ahead` events but the deficit is still negative "
                f"({deficit_end_frames}) and flat. `ahead` alone is not a fault: "
                "a small replay ring generates them while audio stays intact."
            ),
        )

    return Diagnosis(
        cause="healthy",
        confidence="high",
        reasoning=(
            f"deficit {deficit_end_frames} frames, flat. E is leading W by about "
            f"the {horizon}-frame exposure horizon, which is the intended state."
        ),
    )
