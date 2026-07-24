"""Re-derive AudioTimingGeometry from a chosen HAL IO budget, and check it.

``kHalIoPeriodFrames`` is not an isolated knob.  Raising it (to widen the
CoreAudio buffer-size range) propagates through the whole TX budget chain and
must still satisfy every ``static_assert`` in ``AudioTimingGeometry.hpp``:

    ioBudget -> kTxFrameExposureWindowPackets -> kTxPreparationLeadPackets
             -> kTxSharedSlotPackets -> kTimelineSlots

This module reproduces those formulas and constraints so a proposed buffer size
can be costed before anyone edits a header.  Formula sources are cited per
function against the header they mirror.
"""

from __future__ import annotations

from dataclasses import dataclass

from .headers import DriverHeaders

__all__ = ["DerivedGeometry", "ConstraintResult", "derive", "solve_for_io_budget"]


def _round_up(value: int, multiple: int) -> int:
    return ((value + multiple - 1) // multiple) * multiple


@dataclass(frozen=True)
class ConstraintResult:
    name: str
    ok: bool
    detail: str


@dataclass(frozen=True)
class DerivedGeometry:
    """A candidate geometry and whether the header's asserts would hold."""

    io_budget_frames: int
    frame_ring_frames: int
    zts_period_frames: int

    exposure_lead_frames: int
    exposure_lead_packets: int
    coverage_lead_packets: int
    frame_exposure_window_packets: int
    preparation_lead_packets: int
    shared_slot_packets: int
    timeline_slots: int
    max_covered_delta_consumed: int

    constraints: tuple[ConstraintResult, ...]

    @property
    def ok(self) -> bool:
        return all(c.ok for c in self.constraints)

    @property
    def failures(self) -> tuple[ConstraintResult, ...]:
        return tuple(c for c in self.constraints if not c.ok)


def derive(
    headers: DriverHeaders,
    *,
    io_budget_frames: int,
    frame_ring_frames: int | None = None,
    zts_period_frames: int | None = None,
    shared_slot_packets: int | None = None,
    timeline_slots: int | None = None,
    data_horizon_packets: int | None = None,
) -> DerivedGeometry:
    """Recompute the TX chain for a candidate IO budget and check the asserts."""
    t = headers.timing
    sample_rate = t["kSampleRateHz"]
    cad_pkts = t["kMinAvgCadencePackets"]
    cad_frames = t["kMinAvgCadenceFrames"]
    group = t["kTxPacketsPerGroup"]
    cadence_block = t["kCadenceBlockPackets"]
    hw_ring = t["kTxHardwareRingPackets"]
    jitter = t["kSchedulingJitterFrames"]
    horizon_packets = (
        data_horizon_packets
        if data_horizon_packets is not None
        else t["kTxDataHorizonPackets"]
    )

    # AudioTimingGeometry.hpp:101-120
    exposure_lead_frames = (horizon_packets * sample_rate) // 8000
    exposure_lead_raw = (exposure_lead_frames * cad_pkts + cad_frames - 1) // cad_frames
    exposure_lead_packets = _round_up(exposure_lead_raw, group)

    # :137-156
    coverage_lead = hw_ring + 2 * hw_ring
    window_raw = (
        (io_budget_frames + exposure_lead_frames) * cad_pkts + cad_frames - 1
    ) // cad_frames
    frame_exposure_window = _round_up(window_raw, group)
    preparation_lead = coverage_lead + frame_exposure_window

    ring = frame_ring_frames if frame_ring_frames is not None else t["kFrameRingFrames"]
    zts = zts_period_frames if zts_period_frames is not None else ring
    shared = (
        shared_slot_packets
        if shared_slot_packets is not None
        else t["kTxSharedSlotPackets"]
    )
    timeline = timeline_slots if timeline_slots is not None else t["kTimelineSlots"]
    max_covered = preparation_lead - hw_ring

    checks: list[ConstraintResult] = [
        ConstraintResult(
            "shared >= preparationLead",
            shared >= preparation_lead,
            f"{shared} >= {preparation_lead}",
        ),
        ConstraintResult(
            "preparationLead <= shared - hwRing",
            preparation_lead <= shared - hw_ring,
            f"{preparation_lead} <= {shared - hw_ring}",
        ),
        ConstraintResult(
            "shared >= 2 * exposureLeadPackets",
            shared >= 2 * exposure_lead_packets,
            f"{shared} >= {2 * exposure_lead_packets}",
        ),
        ConstraintResult(
            "shared <= timelineSlots",
            shared <= timeline,
            f"{shared} <= {timeline}",
        ),
        ConstraintResult(
            "shared % group == 0", shared % group == 0, f"{shared} % {group}"
        ),
        ConstraintResult(
            "shared % cadenceBlock == 0",
            shared % cadence_block == 0,
            f"{shared} % {cadence_block}",
        ),
        ConstraintResult(
            "exposureLeadFrames >= ioBudget + jitter",
            exposure_lead_frames >= io_budget_frames + jitter,
            f"{exposure_lead_frames} >= {io_budget_frames + jitter}",
        ),
        ConstraintResult(
            "maxCoveredDelta >= 16 groups",
            max_covered >= 16 * group,
            f"{max_covered} >= {16 * group}",
        ),
        ConstraintResult(
            "ring % ioBudget == 0",
            ring % io_budget_frames == 0,
            f"{ring} % {io_budget_frames}",
        ),
        ConstraintResult("ring % zts == 0", ring % zts == 0, f"{ring} % {zts}"),
        ConstraintResult("ring % 32 == 0", ring % 32 == 0, f"{ring} % 32"),
        ConstraintResult(
            "ring >= ioBudget", ring >= io_budget_frames, f"{ring} >= {io_budget_frames}"
        ),
        ConstraintResult("zts % 32 == 0", zts % 32 == 0, f"{zts} % 32"),
    ]

    return DerivedGeometry(
        io_budget_frames=io_budget_frames,
        frame_ring_frames=ring,
        zts_period_frames=zts,
        exposure_lead_frames=exposure_lead_frames,
        exposure_lead_packets=exposure_lead_packets,
        coverage_lead_packets=coverage_lead,
        frame_exposure_window_packets=frame_exposure_window,
        preparation_lead_packets=preparation_lead,
        shared_slot_packets=shared,
        timeline_slots=timeline,
        max_covered_delta_consumed=max_covered,
        constraints=tuple(checks),
    )


def solve_for_io_budget(
    headers: DriverHeaders,
    io_budget_frames: int,
    *,
    ring_multiple: int = 4,
) -> DerivedGeometry:
    """Smallest consistent geometry that supports ``io_budget_frames``.

    Grows, in order: the exposure horizon (so it still covers one IO window plus
    scheduling jitter), then the shared packet ring, then the timeline slots, then
    the frame ring.  Everything else follows the header's own formulas.
    """
    t = headers.timing
    jitter = t["kSchedulingJitterFrames"]
    group = t["kTxPacketsPerGroup"]
    cadence_block = t["kCadenceBlockPackets"]

    # The horizon must cover one IO window plus jitter, in packet time.
    needed_horizon_frames = io_budget_frames + jitter
    horizon_packets = _round_up(
        (needed_horizon_frames * 8000 + t["kSampleRateHz"] - 1) // t["kSampleRateHz"],
        group,
    )
    horizon_packets = max(horizon_packets, t["kTxDataHorizonPackets"])

    probe = derive(
        headers,
        io_budget_frames=io_budget_frames,
        data_horizon_packets=horizon_packets,
        shared_slot_packets=1 << 30,
        timeline_slots=1 << 30,
        frame_ring_frames=max(t["kFrameRingFrames"], io_budget_frames * ring_multiple),
    )

    lcm_step = group * cadence_block // _gcd(group, cadence_block)
    shared = _round_up(
        max(
            probe.preparation_lead_packets + t["kTxHardwareRingPackets"],
            2 * probe.exposure_lead_packets,
        ),
        lcm_step,
    )
    timeline = 1 << max(shared - 1, 1).bit_length()

    ring = max(t["kFrameRingFrames"], io_budget_frames * ring_multiple)
    ring = _round_up(ring, io_budget_frames)
    zts = io_budget_frames if io_budget_frames % 32 == 0 else 32

    return derive(
        headers,
        io_budget_frames=io_budget_frames,
        data_horizon_packets=horizon_packets,
        frame_ring_frames=ring,
        zts_period_frames=zts,
        shared_slot_packets=shared,
        timeline_slots=timeline,
    )


def _gcd(a: int, b: int) -> int:
    while b:
        a, b = b, a % b
    return a
