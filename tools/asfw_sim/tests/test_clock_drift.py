"""F9 clock-domain tests: bus drift, ZTS correlation modes, cadence accuracy."""

import pytest

from asfw_sim.cadence import BlockingCadence
from asfw_sim.geometry import CYCLES_PER_SECOND, Geometry
from asfw_sim.sim import SimConfig, run


@pytest.fixture(scope="module")
def geometry() -> Geometry:
    return Geometry.from_headers()


class TestCadenceDrift:
    def test_zero_drift_exact_fraction(self, geometry: Geometry):
        cadence = BlockingCadence(48000, 8, drift_ppm=0.0)
        cycles = CYCLES_PER_SECOND * 10
        total_frames = sum(cadence.next_packet_frames() for _ in range(cycles))
        expected = 48000 * 10
        assert total_frames == expected

    def test_negative_drift_reduces_output(self, geometry: Geometry):
        cadence = BlockingCadence(48000, 8, drift_ppm=-4478)
        cycles = CYCLES_PER_SECOND * 60
        total_frames = sum(cadence.next_packet_frames() for _ in range(cycles))
        expected = 48000 * (1 - 4478 / 1e6) * 60
        assert abs(total_frames - expected) < 8 * 60

    def test_positive_drift_increases_output(self, geometry: Geometry):
        cadence = BlockingCadence(48000, 8, drift_ppm=100)
        cycles = CYCLES_PER_SECOND * 60
        total_frames = sum(cadence.next_packet_frames() for _ in range(cycles))
        expected = 48000 * (1 + 100 / 1e6) * 60
        assert abs(total_frames - expected) < 8 * 60


class TestZeroDriftHealthy:
    @pytest.mark.parametrize("mode", ["correlated", "uncorrelated", "perfect"])
    def test_all_modes_healthy_at_zero_drift(self, geometry: Geometry, mode: str):
        result = run(
            SimConfig(
                geometry=geometry,
                duration_cycles=CYCLES_PER_SECOND * 20,
                zts_mode=mode,
                bus_drift_ppm=0.0,
            )
        )
        assert result.written_fraction > 0.95
        assert not result.collapsed


class TestUncorrelatedDrift:
    def test_reproduces_f9_collapse(self, geometry: Geometry):
        result = run(
            SimConfig(
                geometry=geometry,
                duration_cycles=CYCLES_PER_SECOND * 30,
                zts_mode="uncorrelated",
                bus_drift_ppm=-4478,
                self_heal=False,
            )
        )
        deficit = result.write_frontier - result.exposed_frame_end
        assert deficit > 0
        assert result.collapsed

    def test_slope_matches_drift(self, geometry: Geometry):
        seconds = 30
        result = run(
            SimConfig(
                geometry=geometry,
                duration_cycles=CYCLES_PER_SECOND * seconds,
                zts_mode="uncorrelated",
                bus_drift_ppm=-4478,
                self_heal=False,
            )
        )
        deficit = result.write_frontier - result.exposed_frame_end
        horizon = geometry.data_horizon_frames
        net_drift = deficit + horizon
        slope = net_drift / seconds
        expected_slope = 48000 * 4478 / 1e6
        assert abs(slope - expected_slope) / expected_slope < 0.30


class TestCorrelatedDrift:
    def test_bounded_at_4478(self, geometry: Geometry):
        result = run(
            SimConfig(
                geometry=geometry,
                duration_cycles=CYCLES_PER_SECOND * 60,
                zts_mode="correlated",
                bus_drift_ppm=-4478,
                self_heal=False,
            )
        )
        assert not result.collapsed

    def test_realistic_drift_absorbed(self, geometry: Geometry):
        for ppm in (-50, -100, 100):
            result = run(
                SimConfig(
                    geometry=geometry,
                    duration_cycles=CYCLES_PER_SECOND * 60,
                    zts_mode="correlated",
                    bus_drift_ppm=ppm,
                )
            )
            assert not result.collapsed, f"collapsed at {ppm} ppm"
            assert result.written_fraction > 0.90


class TestPerfectDrift:
    def test_zero_slope(self, geometry: Geometry):
        result = run(
            SimConfig(
                geometry=geometry,
                duration_cycles=CYCLES_PER_SECOND * 30,
                zts_mode="perfect",
                bus_drift_ppm=-4478,
                self_heal=False,
            )
        )
        deficit = result.write_frontier - result.exposed_frame_end
        horizon = geometry.data_horizon_frames
        assert abs(deficit + horizon) < horizon * 0.5
        assert not result.collapsed
        assert result.written_fraction > 0.95


class TestIIRTransient:
    def test_convergence_within_budget(self, geometry: Geometry):
        result = run(
            SimConfig(
                geometry=geometry,
                duration_cycles=CYCLES_PER_SECOND * 10,
                zts_mode="correlated",
                bus_drift_ppm=-4478,
                self_heal=False,
                trace_every_cycles=CYCLES_PER_SECOND,
            )
        )
        horizon = geometry.data_horizon_frames
        max_w_minus_e = 0
        for _, w, e, _, _, _ in result.trace:
            max_w_minus_e = max(max_w_minus_e, w - e)
        assert max_w_minus_e < horizon, (
            f"IIR transient peak {max_w_minus_e} exceeded budget {horizon}"
        )


class TestIIRSensitivity:
    @pytest.mark.parametrize("alpha", [0.05, 0.10, 0.25, 0.50, 0.90])
    def test_correlated_bounded_across_alphas(self, geometry: Geometry, alpha: float):
        result = run(
            SimConfig(
                geometry=geometry,
                duration_cycles=CYCLES_PER_SECOND * 60,
                zts_mode="correlated",
                bus_drift_ppm=-4478,
                iir_alpha=alpha,
                self_heal=False,
            )
        )
        assert not result.collapsed, f"collapsed at alpha={alpha}"
