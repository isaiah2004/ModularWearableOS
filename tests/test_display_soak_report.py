import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "display_soak_report", Path(__file__).resolve().parents[1] / "tools/display_soak_report.py")
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def capture(kind, payload, line=""):
    return {"direction": "rx", "line": line, "record": kind, "perf": payload}


INFO = {"internal_free": 60000, "internal_largest": 31000, "dma_largest": 31000, "psram_free": 470000,
        "main_stack_free_bytes": 3000, "flush_stack_free_bytes": 1700}
TRACE = {"trace": "breathe", "elapsed_us": 60_000_000, "present_frames": 2400,
         "submitted_tx": 10, "completed_tx": 10, "cancelled_tx": 0, "pending_tx": 0,
         "submitted_bytes": 1000, "completed_bytes": 1000,
         "cancelled_bytes": 0, "pending_bytes": 0,
         "unmatched_completions": 0, "submit_overlaps": 0}


class DisplaySoakReportTests(unittest.TestCase):
    def records(self, objects):
        return list(enumerate(objects, 1))

    def test_incomplete_fixture_is_not_a_pass(self):
        result = report.summarize(self.records([capture("PERF_INFO", INFO),
                                                capture("PERF_APP", TRACE)]))
        self.assertEqual(result["status"], "incomplete")
        self.assertFalse(result["coverage"]["trace_count_satisfied"])
        self.assertFalse(result["coverage"]["elapsed_satisfied"])
        self.assertEqual(result["present_fps"]["weighted"], 40.0)

    def test_accounting_error_fixture_fails(self):
        bad = dict(TRACE, submitted_tx=11)
        result = report.summarize(self.records(
            [capture("PERF_INFO", INFO)] + [capture("PERF_APP", bad)] * 30))
        self.assertEqual(result["status"], "fail")
        self.assertEqual(len(result["anomalies"]["accounting_mismatches"]), 30)

    def test_accounted_cancellation_fixture_still_fails(self):
        cancelled = dict(TRACE, completed_tx=9, cancelled_tx=1,
                         completed_bytes=900, cancelled_bytes=100)
        result = report.summarize(self.records(
            [capture("PERF_INFO", INFO)] + [capture("PERF_APP", cancelled)] * 30))
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["anomalies"]["accounting_mismatches"], [])
        self.assertEqual(result["anomalies"]["cancelled_tx"], 30)
        self.assertEqual(result["anomalies"]["cancelled_bytes"], 3000)

    def test_reset_evidence_fixture_fails(self):
        objects = [capture("PERF_INFO", INFO)] + [capture("PERF_APP", TRACE)] * 30
        objects.append({"direction": "rx", "line": "rst:0x8 (TG1WDT_SYS_RST),boot:0x8"})
        result = report.summarize(self.records(objects))
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["runtime_faults"][0]["category"], "reboot")

    def test_task_wdt_evidence_fixture_fails(self):
        objects = [capture("PERF_INFO", INFO)] + [capture("PERF_APP", TRACE)] * 30
        objects.append({"direction": "rx",
                        "line": "E task_wdt: Task watchdog got triggered. Tasks did not reset"})
        result = report.summarize(self.records(objects))
        self.assertEqual(result["status"], "fail")
        self.assertEqual(result["runtime_faults"][0]["category"], "watchdog")

    def test_passing_fixture_reports_ranges_and_weighted_rate(self):
        slow = dict(TRACE, elapsed_us=60_000_000, present_frames=1800)
        fast = dict(TRACE, elapsed_us=60_000_000, present_frames=3000)
        low_info = dict(INFO, internal_free=55000, flush_stack_free_bytes=1500)
        objects = [capture("PERF_INFO", INFO), capture("PERF_INFO", low_info)]
        objects += [capture("PERF_APP", slow)] * 15
        objects += [capture("PERF_APP", fast)] * 15
        result = report.summarize(self.records(objects))
        self.assertEqual(result["status"], "pass")
        self.assertEqual(result["present_fps"], {"min": 30.0, "max": 50.0, "weighted": 40.0})
        self.assertEqual(result["transactions"], {"submitted": 300, "completed": 300})
        self.assertEqual(result["perf_info"]["internal_free"], {"min": 55000, "max": 60000})
        self.assertNotIn("percentiles", result)

    def test_loads_raw_jsonl_perf_lines(self):
        obj = {"direction": "rx", "line": "PERF_APP," + json.dumps(TRACE)}
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "capture.jsonl"
            path.write_text(json.dumps(obj) + "\n", encoding="utf-8")
            loaded = report.load(path)
        self.assertEqual(report.summarize(loaded)["coverage"]["actual_trace_count"], 1)


if __name__ == "__main__":
    unittest.main()
