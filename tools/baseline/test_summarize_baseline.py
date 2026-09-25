#!/usr/bin/env python3
"""Unit tests for summarize_baseline.py (run by ctest via tests/tools)."""

from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import summarize_baseline as sb  # noqa: E402


def rtl_doc(raw: float | None, ts: float | None, accepted: int = 20, trials: int = 20,
            residual: float | None = -40.0, rate: float = 48000.0, buffer: int = 128,
            rejected: dict[str, int] | None = None) -> dict:
    def stat(v):
        return {"n": accepted, "median_frames": v} if v is not None and accepted else {"n": 0}
    return {
        "schema": sb.RTL_SCHEMA,
        "provenance": {"sample_rate_hz": rate, "buffer_frames_actual": buffer, "timed_out": False},
        "summary": {
            "trials_run": trials,
            "accepted": accepted,
            "rejected": rejected or {},
            "rtl_raw": stat(raw),
            "rtl_ts": stat(ts),
            "scheduling_distance": stat(raw - ts if raw is not None and ts is not None else None),
            "residual_frames": residual,
        },
    }


class SummarizeBaselineTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "sessions").mkdir()

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def write(self, index: int, doc: dict) -> None:
        (self.dir / "sessions" / f"session-{index:02d}.json").write_text(json.dumps(doc))

    def test_single_class_distribution(self) -> None:
        for i, raw in enumerate([423.0, 424.0, 422.5, 423.5]):
            self.write(i, rtl_doc(raw, raw - 228.0))
        s = sb.summarize(self.dir, 16.0, 288.0)
        self.assertEqual(s["sessions_measured"], 4)
        self.assertEqual(len(s["rtl_raw_classes"]), 1)
        self.assertAlmostEqual(s["session_median_rtl_raw_frames"]["median"], 423.25)
        self.assertAlmostEqual(s["session_median_scheduling_frames"]["median"], 228.0)
        self.assertTrue(s["consistent_configuration"])

    def test_lap_classes_are_separated_and_expressed_in_laps(self) -> None:
        # Two sessions one lap (288 frames) later than the others.
        for i, raw in enumerate([423.0, 711.0, 424.0, 712.0, 423.5]):
            self.write(i, rtl_doc(raw, raw - 228.0))
        s = sb.summarize(self.dir, 16.0, 288.0)
        classes = s["rtl_raw_classes"]
        self.assertEqual(len(classes), 2)
        self.assertEqual(len(classes[0]["sessions"]), 3)
        self.assertAlmostEqual(classes[1]["offset_laps"], 1.0, places=2)
        self.assertTrue(classes[1]["near_integer_laps"])
        md = sb.render_markdown(s)
        self.assertIn("More than one class", md)

    def test_sessions_without_accepted_trials_are_reported_not_averaged(self) -> None:
        self.write(0, rtl_doc(423.0, 195.0))
        self.write(1, rtl_doc(None, None, accepted=0, residual=None,
                              rejected={"lost_frames": 12, "no_signal": 8}))
        s = sb.summarize(self.dir, 16.0, 288.0)
        self.assertEqual(s["sessions_measured"], 1)
        self.assertEqual(s["sessions_without_accepted_trials"], ["session-01"])
        self.assertEqual(s["rejected_by_verdict"], {"lost_frames": 12, "no_signal": 8})
        self.assertEqual(s["session_median_rtl_raw_frames"]["n"], 1)

    def test_mixed_configuration_is_flagged(self) -> None:
        self.write(0, rtl_doc(423.0, 195.0, buffer=128))
        self.write(1, rtl_doc(300.0, 195.0, buffer=64))
        s = sb.summarize(self.dir, 16.0, 288.0)
        self.assertFalse(s["consistent_configuration"])
        self.assertIn("WARNING", sb.render_markdown(s))

    def test_withheld_residual_stays_absent(self) -> None:
        self.write(0, rtl_doc(423.0, 195.0, residual=None))
        s = sb.summarize(self.dir, 16.0, 288.0)
        self.assertIsNone(s["session_residual_frames"])

    def test_rejects_foreign_documents(self) -> None:
        (self.dir / "sessions" / "session-00.json").write_text(json.dumps({"schema": "other"}))
        with self.assertRaises(ValueError):
            sb.summarize(self.dir, 16.0, 288.0)

    def test_main_writes_outputs_and_exit_codes(self) -> None:
        self.write(0, rtl_doc(423.0, 195.0))
        (self.dir / "hal_clock.json").write_text(json.dumps({
            "schema": sb.HAL_SCHEMA,
            "devices": [{"clock": {"ppm_vs_nominal": 37.1, "worst_residual_frames": 1.5,
                                   "backward_jumps": 0, "forward_reanchors": 0}}]}))
        with redirect_stdout(io.StringIO()) as out:
            self.assertEqual(sb.main([str(self.dir)]), 0)
        self.assertIn("37.10", out.getvalue())
        self.assertTrue((self.dir / "summary.json").is_file())
        self.assertTrue((self.dir / "summary.md").is_file())

        empty = self.dir / "empty"
        (empty / "sessions").mkdir(parents=True)
        with redirect_stdout(io.StringIO()):
            self.assertEqual(sb.main([str(empty)]), 1)
        self.assertEqual(sb.main([str(self.dir / "missing")]), 2)


if __name__ == "__main__":
    unittest.main()
