"""Private native regression checks for DEV-001..006/015; never use a device/build.

Original positive repros are preserved under the full-OS audit. These assertions
compile actual app callbacks and fail if the observed bugs return.
"""
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/alpha05/checks/settings"


@unittest.skipUnless(shutil.which("gcc"), "GCC required for app callback regression checks")
class SettingsAuditRegressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / "tmp").mkdir(exist_ok=True)
        cls.env = dict(os.environ, TEMP=str(OUT / "tmp"), TMP=str(OUT / "tmp"),
                       TMPDIR=str(OUT / "tmp"), PYTHONDONTWRITEBYTECODE="1")

    def compile_run(self, name, sources, expected):
        exe = OUT / (name + ".exe")
        command = ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra",
                   "-Wno-unused-parameter", "-Wno-format-truncation",
                   "-Werror=implicit-function-declaration", "-flto", "-fwhole-program",
                   "-ffunction-sections", "-fdata-sections", "-Icore", "-Inos", "-Inos/fonts",
                   *sources, "tests/app_observability_test_log.c", "-Wl,--gc-sections", "-lm", "-o", str(exe)]
        result = subprocess.run(command, cwd=ROOT, env=self.env, capture_output=True, text=True, timeout=60)
        log = "$ " + subprocess.list2cmdline(command) + "\n" + result.stdout + result.stderr
        record = {"command": command, "compile_exit": result.returncode}
        if result.returncode == 0:
            run = subprocess.run([str(exe)], cwd=OUT, env=self.env, capture_output=True, text=True, timeout=30)
            log += "\n" + run.stdout + run.stderr
            record.update(run_exit=run.returncode, output=run.stdout.strip())
        (OUT / (name + ".log")).write_text(log, encoding="utf-8")
        (OUT / (name + ".json")).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        self.assertEqual(run.returncode, 0, log)
        for message in expected:
            self.assertIn(message, run.stdout)

    def test_timezone_and_checked_time(self):
        self.compile_run("time", ["tests/settings_time_regression_test.c", "nos/motion.c"],
                         ["PASS DEV-001", "PASS DEV-015"])

    def test_wifi_cover_reveal_and_scrub(self):
        self.compile_run("wifi", ["tests/settings_wifi_lifecycle_test.c"], ["PASS DEV-004"])

    def test_light_plausibility(self):
        self.compile_run("light", ["tests/settings_light_regression_test.c"], ["PASS DEV-006"])

    def test_manual_wrist_and_unavailable_gps(self):
        self.compile_run("preferences", ["tests/settings_preferences_regression_test.c", "nos/prefs.c", "nos/motion.c"],
                         ["PASS DEV-002", "PASS DEV-003"])

    def test_modules_failed_imu_and_recovery(self):
        self.compile_run("modules", ["tests/settings_modules_imu_regression_test.c", "nos/motion.c"],
                         ["PASS DEV-005"])


if __name__ == "__main__":
    unittest.main()
