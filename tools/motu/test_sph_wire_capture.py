"""Checks for the passive-observer capture summary (sph_wire_capture.py)."""
import csv
import tempfile
import unittest
from pathlib import Path

from sph_wire_capture import (load_data_packets, offset_summary, same_cycle_offsets,
                              step_histogram, summarize_direction, wrap_difference)

COLUMNS = ['index', 'gap_before', 'wire_sec', 'wire_cycle', 'packet', 'sph_cycle', 'sph_offset']


def write_capture(path, rows):
    with open(path, 'w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=COLUMNS)
        writer.writeheader()
        for index, (cycle, tick) in enumerate(rows):
            data = tick is not None
            writer.writerow({'index': index, 'gap_before': 'no', 'wire_sec': 0, 'wire_cycle': cycle,
                             'packet': 'DATA' if data else 'NO-DATA',
                             'sph_cycle': (tick // 3072) % 8000 if data else 'none',
                             'sph_offset': tick % 3072 if data else 'none'})


class CaptureTests(unittest.TestCase):
    def test_wrap_difference_folds_across_the_second(self):
        self.assertEqual(wrap_difference(100, 24_576_000 - 100), 200)
        self.assertEqual(wrap_difference(24_576_000 - 100, 100), -200)

    def test_same_cycle_offset_in_frames_and_residue(self):
        with tempfile.TemporaryDirectory() as directory:
            host, device = Path(directory) / 'h.csv', Path(directory) / 'd.csv'
            # Device stamps 4096 apart on DATA cycles; host stamps 13 frames ahead.
            device_rows, host_rows, tick = [], [], 1000 * 3072 + 4000
            for cycle in range(1000, 1040):
                if cycle % 4 == 0:
                    device_rows.append((cycle, None))
                    host_rows.append((cycle, None))
                    continue
                device_rows.append((cycle, tick))
                host_rows.append((cycle, tick + 13 * 512))
                tick += 4096
            write_capture(host, host_rows)
            write_capture(device, device_rows)
            pairs = same_cycle_offsets(load_data_packets(host), load_data_packets(device))
            summary = offset_summary(pairs)
            self.assertEqual(summary['mode_ticks'], 6656)
            self.assertEqual((summary['mode_frames'], summary['mode_residue_ticks']), (13, 0))
            self.assertEqual(summary['within_1_tick_of_mode'], 1.0)
            steps = step_histogram(load_data_packets(device))
            self.assertEqual(set(steps), {4096})
            direction = summarize_direction(load_data_packets(device))
            self.assertAlmostEqual(direction['period_ppm_vs_nominal'], 0.0)


if __name__ == '__main__':
    unittest.main()
