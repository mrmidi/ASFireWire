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
    # V3 (decision D2): per-rate ring == ZTS period, 12288 frames at 48 kHz,
    # inside a fixed 24576-frame allocation. kFrameRingFrames and
    # kHalZeroTimestampPeriodFrames are the sim's 48 kHz values, derived from
    # HalBufferProfileForRate (headers.SIM_SAMPLE_RATE_HZ).
    "kHalIoPeriodFrames": 1024,
    "kFrameRingFrames": 12288,
    "kHalZeroTimestampPeriodFrames": 12288,
    "kAllocatedFrameRingFrames": 24576,
    "kMaxClientIoFrames": 4096,
    "kTxDataHorizonPackets": 400,
    # Floored at the 4096-frame ADK client maximum + jitter (Defect B).
    "kTxExposureFloorFrames": 4160,
    "kTxExposureLeadFrames": 4160,
    "kTxExposureLeadPackets": 760,
    "kTxCoverageLeadPackets": 1008,
    "kTxFrameExposureWindowPackets": 1504,
    "kTxPreparationLeadPackets": 1008,
    "kTxSharedSlotPackets": 1512,
    "kTimelineSlots": 1512,
    "kTxHardwareRingPackets": 504,
    "kFramesPerDataPacket": 8,
    "kMinAvgCadencePackets": 80,
    "kMinAvgCadenceFrames": 441,
}

EXPECTED_REPLAY = {"kCapacity": 2048, "kReadDelay": 256}


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


def test_active_profile_is_v3(headers):
    assert headers.profile_name == "audio-engine-v3-1x"


def test_derived_lead_is_the_coverage_alone(headers):
    """Since T5 the lead is refill coverage only (1008 = ring 504 + slack 504);
    the frame-exposure window no longer sizes anything."""
    assert (
        headers.timing["kTxCoverageLeadPackets"]
        == headers.timing["kTxPreparationLeadPackets"]
    )


def test_replay_capacity_is_a_power_of_two(headers):
    capacity = headers.replay["kCapacity"]
    assert capacity & (capacity - 1) == 0


def test_geometry_reports_the_negative_headroom(headers):
    """Records the state of the tree, not a claim that it is the bug (see F1)."""
    g = Geometry.from_headers(48_000, headers)
    assert g.replay_headroom_packets == 256 - 1008


def test_sim_horizon_mirrors_the_header_floor(headers):
    """TxDataHorizonFrames = max(400 cycles, max client IO + jitter)."""
    g = Geometry.from_headers(48_000, headers)
    assert g.data_horizon_frames == 4160
    assert Geometry.from_headers(96_000, headers).data_horizon_frames == 4800
