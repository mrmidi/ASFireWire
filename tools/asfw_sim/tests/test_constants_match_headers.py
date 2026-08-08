"""The anti-drift gate: the sim must analyse the geometry the driver has.

If a constant below changes in the headers, this test fails loudly and every
conclusion in FINDINGS.md must be re-derived rather than silently re-interpreted.
Both 2026-07 triage reports drew wrong conclusions from stale constants; that is
the failure this file exists to prevent.
"""

from __future__ import annotations

import pytest

from asfw_sim.geometry import Geometry
from asfw_sim.headers import load_driver_headers

EXPECTED_TIMING = {
    "kHalIoPeriodFrames": 512,
    "kFrameRingFrames": 1536,
    "kTxDataHorizonPackets": 400,
    "kTxExposureLeadFrames": 2400,
    "kTxExposureLeadPackets": 438,
    "kTxCoverageLeadPackets": 144,
    "kTxFrameExposureWindowPackets": 534,
    "kTxPreparationLeadPackets": 678,
    "kTxSharedSlotPackets": 912,
    "kTimelineSlots": 1024,
    "kTxHardwareRingPackets": 48,
    "kFramesPerDataPacket": 8,
    "kMinAvgCadencePackets": 80,
    "kMinAvgCadenceFrames": 441,
}

EXPECTED_REPLAY = {"kCapacity": 512, "kReadDelay": 256}


@pytest.fixture(scope="module")
def headers():
    return load_driver_headers()


@pytest.mark.parametrize("name,value", sorted(EXPECTED_TIMING.items()))
def test_timing_constant(headers, name, value):
    assert headers.timing[name] == value, (
        f"{name} changed in AudioTimingGeometry.hpp "
        f"({headers.timing[name]} != {value}); re-derive FINDINGS.md"
    )


@pytest.mark.parametrize("name,value", sorted(EXPECTED_REPLAY.items()))
def test_replay_constant(headers, name, value):
    assert headers.replay[name] == value, (
        f"{name} changed in RxSequenceReplay.hpp "
        f"({headers.replay[name]} != {value}); re-derive FINDINGS.md"
    )


def test_active_profile_is_dice_working(headers):
    assert headers.profile_name == "dice-working-1536"


def test_derived_lead_is_the_sum_of_its_two_budgets(headers):
    """678 = 144 refill coverage + 534 frame exposure."""
    assert (
        headers.timing["kTxCoverageLeadPackets"]
        + headers.timing["kTxFrameExposureWindowPackets"]
        == headers.timing["kTxPreparationLeadPackets"]
    )


def test_replay_capacity_is_a_power_of_two(headers):
    capacity = headers.replay["kCapacity"]
    assert capacity & (capacity - 1) == 0


def test_geometry_reports_the_negative_headroom(headers):
    """Records the state of the tree, not a claim that it is the bug (see F1)."""
    g = Geometry.from_headers(48_000, headers)
    assert g.replay_headroom_packets == 256 - 678
