"""Deterministic checks for the three-controller SPH model (sph_controllers.py)."""
import random
import unittest
from collections import Counter

from sph_controllers import (Device, DeviceParams, LEAD_TICKS, SphSynthesizer, TICKS_PER_CYCLE,
                             VENDOR_OFFSET_TICKS, score, simulate)


class DeviceModelTests(unittest.TestCase):
    def test_step_mix_resembles_the_828mk3_capture(self):
        # Capture: 4096 79%, 4097 12%, 4095 6% between consecutive DATA packets.
        device = Device(DeviceParams(), random.Random(1), 0, 4000)
        stamps = [p[1] for p in (device.packet(c) for c in range(8000)) if p]
        steps = Counter(b - a for a, b in zip(stamps, stamps[1:]))
        self.assertEqual(set(steps), {4095, 4096, 4097})
        self.assertGreater(steps[4096] / sum(steps.values()), 0.6)
        self.assertGreater(steps[4097], steps[4095])

    def test_capture_lead_stays_within_the_observed_band(self):
        device = Device(DeviceParams(), random.Random(1), 0, 4000)
        leads = [p[1] - c * TICKS_PER_CYCLE for c in range(8000) if (p := device.packet(c))]
        self.assertGreaterEqual(min(leads), 2604 - 700)
        self.assertLessEqual(max(leads), 6322 + 1)


class ControllerTests(unittest.TestCase):
    def test_restored_holds_the_vendor_offset_and_lead(self):
        run = simulate('restored', seconds=1.5, seed=3)
        result = score(run, settle_s=0.5)
        self.assertEqual(result['resyncs'], 0)
        self.assertLess(result['lock_s'], 0.1)
        self.assertLessEqual(result['settled_max_abs_error_ticks'], 3)
        self.assertEqual(result['first_tx_lead_ticks'], LEAD_TICKS)
        self.assertGreaterEqual(result['tx_lead_min_ticks'], LEAD_TICKS)
        self.assertLess(result['tx_lead_max_ticks'], 4 * TICKS_PER_CYCLE + 1000)

    def test_pr191_holds_the_offset_after_its_absolute_seed(self):
        result = score(simulate('pr191', seconds=1.5, seed=3), settle_s=0.8)
        self.assertEqual(result['resyncs'], 0)
        self.assertLessEqual(result['settled_max_abs_error_ticks'], 3)
        self.assertEqual(result['first_tx_lead_ticks'], LEAD_TICKS)

    def test_replay_seeded_synthesizer_inherits_the_device_capture_lead(self):
        # Replay rebases each device stamp onto a later transmit cycle, so the
        # transmit lead equals the capture lead (1-2 cycles), whatever the
        # cadence delay; the official driver holds 3-4 cycles.
        for delay in (3, 4, 5):
            with self.subTest(delay=delay):
                result = score(simulate('synth', seconds=1.0, seed=1, cadence_delay=delay), settle_s=0.5)
                self.assertLess(result['tx_lead_max_ticks'], 2 * TICKS_PER_CYCLE + 300)
                self.assertGreater(abs(result['settled_mean_error_ticks']), 1000)

    def test_restored_follows_a_device_phase_step(self):
        params = DeviceParams(phase_step_ticks=500, phase_step_at_s=1.0)
        result = score(simulate('restored', seconds=2.0, seed=1, device=params), settle_s=1.5, after_s=1.0)
        self.assertGreater(result['lock_s'], 0)   # the step is visible ...
        self.assertLess(result['lock_s'], 0.05)   # ... and removed within 50 ms
        self.assertLessEqual(result['settled_max_abs_error_ticks'], 3)

    def test_external_seed_takes_the_first_observation_as_rate_reference(self):
        # A seed at transmit time + lead is not an observation: measuring the
        # first rate across it would fail the +/-2000 ppm check and re-anchor.
        synth = SphSynthesizer()
        synth.seed(1_000_000, external=True)
        tick = 1_001_440
        for _ in range(4):
            synth.observe(tick)
            for _ in range(512):
                synth.next_stamp()
            tick += 512 * 512
        self.assertEqual(synth.resyncs, 0)

    def test_target_residue_is_a_parameter(self):
        # The 0.5 s capture held 13 frames + 21 ticks, the 90 s capture + 0.
        target = VENDOR_OFFSET_TICKS + 21
        result = score(simulate('restored', seconds=1.5, seed=2, target_offset=target), target, settle_s=0.5)
        self.assertLessEqual(result['settled_max_abs_error_ticks'], 3)


if __name__ == '__main__':
    unittest.main()
