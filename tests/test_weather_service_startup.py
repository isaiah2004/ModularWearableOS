"""Actual weather worker startup faults/recovery without tasks or networking."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/alpha05/checks/weather-startup"


@unittest.skipUnless(shutil.which("gcc"), "host GCC is required")
class WeatherServiceStartup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.temp = tempfile.TemporaryDirectory(prefix="actual-worker-", dir=OUT)
        cls.directory = Path(cls.temp.name)
        cls.addClassCleanup(cls.temp.cleanup)
        for name in ("esp_http_client.h", "esp_crt_bundle.h", "esp_timer.h", "esp_log.h",
                     "freertos/FreeRTOS.h", "freertos/task.h"):
            header = cls.directory / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "weather_service_startup_mock.h"\n')
        cls.binary = cls.directory / ("startup.exe" if os.name == "nt" else "startup")
        cls.env = dict(os.environ, TEMP=str(cls.directory), TMP=str(cls.directory), TMPDIR=str(cls.directory))
        command = ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-I"+str(cls.directory),
                   "-Itests", "-Icore", "-Ifirmware/main", "tests/weather_service_startup_test.c",
                   "firmware/main/weather_service.c", "-o", str(cls.binary)]
        result = subprocess.run(command, cwd=ROOT, env=cls.env, capture_output=True, text=True)
        (OUT / "compile.log").write_text(subprocess.list2cmdline(command)+"\n"+result.stdout+result.stderr)
        if result.returncode:
            raise AssertionError(result.stdout+result.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.binary), name], cwd=ROOT, env=self.env,
                                capture_output=True, text=True, timeout=10)
        (OUT / (name+".log")).write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn("PASS actual weather service "+name, result.stdout)

    def test_stack_and_tcb_failure_recovery_preserves_config_and_snapshot(self):
        self.run_case("allocation-recovery")

    def test_reentrant_start_and_location_updates_cannot_replace_state_or_duplicate_worker(self):
        self.run_case("concurrent-selection")

    def test_unavailable_initial_location_read_can_recover_on_later_attempt(self):
        self.run_case("initial-read-recovery")


if __name__ == "__main__":
    unittest.main()
