"""Executable diagnostics durability/recovery and exporter regressions."""
import importlib.util
import json
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("diag_export", ROOT / "tools/diag_export.py")
EXPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(EXPORT)


class DiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("gcc")
        if not compiler:
            raise unittest.SkipTest("host GCC unavailable")
        cls.build = tempfile.TemporaryDirectory(prefix="watch-diag-build-")
        cls.executable = pathlib.Path(cls.build.name) / "diag_log_test.exe"
        subprocess.run([
            compiler, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I" + str(ROOT / "core"), "-I" + str(ROOT / "nos"),
            str(ROOT / "tests/diag_log_test.c"), "-o", str(cls.executable),
        ], check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.build.cleanup()

    def run_case(self, case, directory):
        result = subprocess.run([str(self.executable), case, str(directory)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_queue_overflow_and_no_producer_filesystem(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_case("queue", pathlib.Path(directory) / "trace")

    def test_semantic_request_target_and_result(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_case("semantic", pathlib.Path(directory) / "trace")

    def test_emergency_first_fault_survives_sd_pressure_and_torn_suffix(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_case("emergency", pathlib.Path(directory) / "trace")

    def test_concurrent_producers_and_retained_ring(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_case("concurrent", pathlib.Path(directory) / "trace")

    def test_simulator_service_is_throttled(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_case("throttle", pathlib.Path(directory) / "trace")

    def test_partial_write_and_fsync_failure_preserve_and_deduplicate(self):
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "trace"
            self.run_case("fault", target)
            report = EXPORT.load([target])
            self.assertEqual(report["duplicates_removed"], 2)
            self.assertEqual([error["kind"] for error in report["errors"]], ["truncated_tail"])
            actions = [event["a"] for event in report["events"] if event["code"] == 0x200]
            self.assertEqual(sorted(actions), [0, 1, 2, 3, 4, 99])

    def test_bounded_rotation(self):
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "trace"
            self.run_case("rotation", target)
            report = EXPORT.load([target])
            self.assertEqual(len(report["files"]), 8)
            self.assertFalse(report["errors"])
            actions = [event["a"] for event in report["events"] if event["code"] == 0x200]
            self.assertEqual(max(actions), 299)
            self.assertNotIn(0, actions)

    def test_restart_breadcrumb_integrity_and_reset_classification(self):
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "trace"
            self.run_case("restart", target)
            report = EXPORT.load([target])
            self.assertFalse(report["errors"])
            self.assertGreaterEqual(report["duplicates_removed"], 8)
            self.assertEqual([session["reset_name"] for session in report["sessions"]],
                             ["power_on", "panic", "software_restart", "power_on"])
            self.assertEqual(sum(session["crash_reset"] for session in report["sessions"]), 1)

    def test_unavailable_storage_retains_queue(self):
        with tempfile.TemporaryDirectory() as directory:
            self.run_case("unavailable", pathlib.Path(directory) / "absent/trace")

    def test_power_durations_valid_battery_and_cli_exports(self):
        with tempfile.TemporaryDirectory() as directory:
            target = pathlib.Path(directory) / "trace"
            self.run_case("timeline", target)
            report = EXPORT.load([target])
            session = report["sessions"][0]
            self.assertEqual(session["observed_ms"], 10000)
            self.assertEqual(session["power_ms"], {
                "active": 3000, "aod": 0, "display_off": 5000,
                "sensor_idle": 2000, "unknown": 0,
            })
            self.assertEqual(session["battery_saver_ms"], 7000)
            self.assertEqual(session["battery"]["valid_battery_samples"], 2)
            self.assertEqual(session["battery"]["percent_change"], -2)
            output = pathlib.Path(directory) / "trace.json"
            csv = pathlib.Path(directory) / "trace.csv"
            command = [shutil.which("python") or "python", str(ROOT / "tools/diag_export.py"),
                       str(target), "--json", str(output), "--csv", str(csv), "--strict"]
            subprocess.run(command, check=True, capture_output=True, text=True)
            self.assertEqual(json.loads(output.read_text())["schema"]["record_bytes"], 44)
            self.assertIn("POWER_STATE", csv.read_text())
            # A damaged complete record is detected independently of the tail.
            first = sorted(target.glob("diag-*.bin"))[0]
            data = bytearray(first.read_bytes());data[32 + 20] ^= 1;first.write_bytes(data)
            damaged = EXPORT.load([target])
            self.assertIn("record_crc_mismatch", [error["kind"] for error in damaged["errors"]])
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 2)


if __name__ == "__main__":
    unittest.main()
