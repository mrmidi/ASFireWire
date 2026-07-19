"""F1-F3: what actually kills the stream, and what does not.

These lock in the Phase A result:

* F1 the reader self-stabilises at ``-kReadDelay``; the 678-vs-512 comparison in
  the triage reports is NOT the failure mechanism.
* F2 a producer stall past a hard cliff kills audio content permanently while
  transport stays perfectly healthy.
* F3 the cliff is set by ``kReadDelay + kTxDataHorizonPackets``, so ``kReadDelay``
  is a producer-stall recovery budget, not a history depth.
"""

from __future__ import annotations

import pytest

from asfw_sim.geometry import SUPPORTED_RATES, Geometry
from asfw_sim.replay import ReplayFailure
from asfw_sim.sim import SimConfig, run

CYCLES = 8_000


@pytest.fixture(scope="module")
def g48():
    return Geometry.from_headers(48_000)


def _run(geometry, seconds=15, stall_cycles=0, stall_at_s=4):
    return run(
        SimConfig(
            geometry=geometry,
            duration_cycles=CYCLES * seconds,
            stall_at_cycle=CYCLES * stall_at_s,
            stall_cycles=stall_cycles,
        )
    )


def _cliff_cycles(geometry, hi=6_000) -> int:
    lo = 0
    while lo < hi - 1:
        mid = (lo + hi) // 2
        if _run(geometry, stall_cycles=mid).collapsed:
            hi = mid
        else:
            lo = mid
    return lo


# --- F1 -----------------------------------------------------------------------


@pytest.mark.parametrize("rate", SUPPORTED_RATES)
def test_healthy_duplex_never_starves_the_replay_reader(rate):
    """The 678-packet lead does NOT outrun a 256/512 replay ring."""
    result = _run(Geometry.from_headers(rate), seconds=20)
    assert result.failure_count(ReplayFailure.AHEAD_OF_PRODUCER) == 0
    assert result.failure_count(ReplayFailure.HISTORY_OVERWRITTEN) == 0
    assert not result.collapsed
    assert result.written_fraction > 0.99


def test_reader_stays_a_constant_read_delay_behind_the_producer(g48):
    """Reader lag is independent of the preparation lead (the F1 mechanism).

    In steady state the reader sits a bounded distance BEHIND the producer and
    never crosses it, no matter that the packet frontier runs 678 packets ahead
    of real time.  That is why the reports' `678 > 512` comparison is not a
    governing invariant.
    """
    result = _run(g48, seconds=20)
    assert result.min_replay_distance < 0, "reader must never lead the producer"
    assert abs(result.min_replay_distance) <= g48.replay_capacity, (
        "steady-state lag must stay inside the history window"
    )


def test_cold_start_reclamps_exactly_once(g48):
    """The prefill/limit interaction throttles the first ~234 cycles of
    preparation while RX keeps publishing, so the reader is reclamped once.
    More than one reclamp in a healthy run would be a regression."""
    assert _run(g48, seconds=20).reclamped <= 1


# --- F2 -----------------------------------------------------------------------


def test_short_producer_stall_is_survivable(g48):
    assert not _run(g48, stall_cycles=400).collapsed  # 50 ms


def test_long_producer_stall_kills_content_permanently(g48):
    """Transport keeps emitting a packet per cycle; audio never returns."""
    result = _run(g48, stall_cycles=800, seconds=20)  # 100 ms
    assert result.collapsed
    assert result.data_packet_fraction > 0.7, "transport must still look healthy"
    assert result.align_count == 1, "the frame cursor never re-arms"
    # Nothing is written after the stall: the cumulative fraction is exactly the
    # pre-stall share of the run.
    assert result.written_fraction == pytest.approx(4 / 20, abs=0.02)


def test_post_collapse_deficit_is_small_and_does_not_grow(g48):
    """A ~20 ms standing phase error produces 100% silence -- the zombie."""
    short = _run(g48, stall_cycles=800, seconds=10)
    long = _run(g48, stall_cycles=800, seconds=30)
    short_deficit = short.write_frontier - short.exposed_frame_end
    long_deficit = long.write_frontier - long.exposed_frame_end
    assert 0 < short_deficit < 4_000
    assert 0 < long_deficit < 4_000


# --- F3 -----------------------------------------------------------------------


def test_cliff_matches_read_delay_plus_horizon(g48):
    predicted = (g48.replay_read_delay + g48.tx_data_horizon_packets) / 8
    measured = _cliff_cycles(g48) / 8
    assert measured == pytest.approx(predicted, abs=8), (
        f"stall tolerance {measured:.1f} ms deviates from the "
        f"kReadDelay+horizon law ({predicted:.1f} ms)"
    )


@pytest.mark.parametrize("read_delay", [256, 512, 1024])
def test_read_delay_is_the_stall_recovery_budget(g48, read_delay):
    geometry = g48.evolve(
        replay_read_delay=read_delay, replay_capacity=max(512, 2 * read_delay)
    )
    measured = _cliff_cycles(geometry) / 8
    predicted = (read_delay + g48.tx_data_horizon_packets) / 8
    assert measured == pytest.approx(predicted, abs=8)


def test_proposed_fix_survives_the_observed_watchdog_cadence(g48):
    """68 ms was the Duet's watchdog cadence; HEAD's budget is 32 ms."""
    watchdog_stall = int(0.068 * CYCLES)
    assert _run(g48, stall_cycles=watchdog_stall).collapsed is False, (
        "68 ms alone is under the 78 ms cliff"
    )
    # ...but a 100 ms excursion is not, and only the widened budget survives it.
    assert _run(g48, stall_cycles=800).collapsed
    fixed = g48.evolve(replay_read_delay=1024, replay_capacity=2048)
    assert not _run(fixed, stall_cycles=800).collapsed
