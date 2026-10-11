"""Regress hardware measurements using compact contributor capture fixtures."""
import json
import random
import unittest
from collections import Counter
from pathlib import Path
from sph_wire_capture import load_data_packets, offset_summary, same_cycle_offsets, step_histogram, summarize_direction
from sph_controllers import Device, DeviceParams

FIXTURES = Path(__file__).resolve().parents[2] / 'documentation/fixtures/MOTU/828mk3-48k'

def load(name):
    return load_data_packets(FIXTURES / name)

class CaptureGoldenTests(unittest.TestCase):
    def test_every_fixture_has_manifest_data_count(self):
        manifest = json.loads((FIXTURES / 'manifest.json').read_text())
        for name, meta in manifest.items():
            with self.subTest(name=name):
                self.assertEqual(len(load(name)), meta['data_packets'])

    def test_official_steady_phase_holds_thirteen_frames(self):
        host = load('vendor-sampled-90s_host-to-device_ch33.csv')
        device = load('vendor-sampled-90s_device-to-host_ch34.csv')
        stats = offset_summary(same_cycle_offsets(host, device))
        self.assertEqual(stats['pairs'], 2044)
        self.assertEqual(stats['mode_ticks'], 6656)
        self.assertEqual(stats['median_ticks'], 6656)
        self.assertEqual(stats['within_1_tick_of_mode'], 0.9741)
        steps = step_histogram(device)
        self.assertEqual(set(steps), {4095, 4096, 4097})
        # 30 independent observer bursts; no fictitious step through a gap.
        self.assertEqual(sum(steps.values()), len(device) - 30)

    def test_official_startup_seed_lead_and_session_residue(self):
        host = load('vendor-start-0.5s_host-to-device_ch33.csv')
        device = load('vendor-start-0.5s_device-to-host_ch34.csv')
        stats = summarize_direction(host)
        self.assertEqual(stats['first_lead_ticks'], 9216)
        self.assertEqual(stats['lead_min_ticks'], 9216)
        self.assertEqual(stats['lead_max_ticks'], 12287)
        self.assertEqual(offset_summary(same_cycle_offsets(host,device))['mode_ticks'], 6677)

    def test_contributor_asfw_transmit_seed_and_observed_band(self):
        start = summarize_direction(load('asfw-start-0.5s_host-to-device_ch0.csv'))
        self.assertEqual(start['first_lead_ticks'], 9216)
        sampled = summarize_direction(load('asfw-sampled-90s_host-to-device_ch0.csv'))
        self.assertEqual(sampled['data_packets'], 2784)
        self.assertGreaterEqual(sampled['lead_min_ticks'], 8500)
        self.assertLess(sampled['lead_max_ticks'], 12288)

    def test_model_device_calibration_stays_close_to_recorded_steps(self):
        measured = step_histogram(load('vendor-sampled-90s_device-to-host_ch34.csv'))
        device = Device(DeviceParams(),random.Random(1),0,4000)
        stamps = [packet[1] for c in range(8000) if (packet := device.packet(c))]
        simulated = Counter(b-a for a,b in zip(stamps,stamps[1:]))
        for step in (4095,4096,4097):
            self.assertAlmostEqual(simulated[step]/sum(simulated.values()),
                                   measured[step]/sum(measured.values()),delta=0.02)

if __name__ == '__main__':
    unittest.main()
