import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "display_perf_report", Path(__file__).resolve().parents[1] / "tools/display_perf_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


class DisplayPerfReportTests(unittest.TestCase):
    def test_pairs_config_and_preserves_missing_metrics(self):
        rows = report.summarize([
            ("capture.jsonl", 1, "PERF_CONFIG",
             {"source": "sram", "frames_requested": 2, "completed_frames": 2, "errors": 0}),
            ("capture.jsonl", 2, "PERF_RUN",
             {"elapsed_us": 500000, "frames": 2, "present_frames": 2,
              "dirty_pixels": 200,
              "submitted_tx": 2, "completed_tx": 2, "cancelled_tx": 0, "pending_tx": 0,
              "submitted_bytes": 400, "completed_bytes": 400,
              "cancelled_bytes": 0, "pending_bytes": 0,
              "frame_work": {"p50_us": 1000, "p95_us": 2000, "p99_us": 2000},
              "copy": {"n": 2, "total_us": 80}}),
        ])
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["name"], "sram")
        self.assertEqual(rows[0]["evaluation_fps"], 4.0)
        self.assertEqual(rows[0]["present_fps"], 4.0)
        self.assertEqual(rows[0]["dirty_pixels_per_frame"], 100.0)
        self.assertEqual(rows[0]["stage_us_per_frame"]["copy"], 40.0)
        self.assertIsNone(rows[0]["stage_us_per_frame"]["swap"])
        self.assertEqual(rows[0]["anomalies"], [])

    def test_loads_capture_and_flags_accounting(self):
        records = [
            {"direction": "rx", "line": 'PERF_CONFIG,{"frames_requested":3,"completed_frames":2}'},
            {"direction": "rx", "record": "PERF_RUN", "perf": {
                "trace": "generator", "frames": 2, "elapsed_us": 1000000,
                "submitted_tx": 3, "completed_tx": 1, "cancelled_tx": 0, "pending_tx": 0,
                "unmatched_completions": 1}},
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.jsonl"
            path.write_text("".join(json.dumps(item) + "\n" for item in records), encoding="utf-8")
            rows = report.summarize(report.load_records([path]))
        self.assertIn("transaction accounting mismatch", rows[0]["anomalies"])
        self.assertIn("1 unmatched completion(s)", rows[0]["anomalies"])
        self.assertIn("completed 2/3 requested frames", rows[0]["anomalies"])
        self.assertIsNone(rows[0]["frame_p50_us"])

    def test_does_not_pair_configuration_across_files(self):
        rows = report.summarize([
            ("first.jsonl", 1, "PERF_CONFIG", {"source": "sram"}),
            ("second.jsonl", 1, "PERF_RUN", {"frames": 1, "elapsed_us": 1}),
        ])
        self.assertIsNone(rows[0]["config"])
        self.assertIn("missing preceding PERF_CONFIG", rows[0]["anomalies"])

    def test_distinguishes_evaluation_and_present_rates(self):
        rows = report.summarize([
            ("apps.jsonl", 1, "PERF_APP",
             {"trace": "level", "elapsed_us": 60000000,
              "frames": 3444, "present_frames": 2765}),
        ])
        self.assertEqual(rows[0]["evaluation_fps"], 57.4)
        self.assertAlmostEqual(rows[0]["present_fps"], 46.0833333333)
        rendered = report.markdown(rows)
        self.assertIn("present FPS", rendered)
        self.assertIn("evaluation FPS", rendered)


if __name__ == "__main__":
    unittest.main()
