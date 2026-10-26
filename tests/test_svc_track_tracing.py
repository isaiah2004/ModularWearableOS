"""Actual track owner with delayed worker completion and a checked GPX file."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "GCC required")
class TrackTracingTests(unittest.TestCase):
    def test_background_completion_and_sparse_quality(self):
        base = ROOT / "build/alpha05/checks/platform/track-tracing"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="actual-", dir=base) as value:
            directory = Path(value)
            headers = directory / "freertos"
            headers.mkdir()
            (headers / "FreeRTOS.h").write_text("#pragma once\n#define pdPASS 1\n")
            (headers / "task.h").write_text(
                "#pragma once\nint xTaskCreate(void (*)(void *),const char*,unsigned,void*,unsigned,void*);\n"
                "void vTaskDelete(void*);\n")
            exe = directory / "track.exe"
            command = ["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-Wno-format-truncation", "-I" + str(directory), "-Icore", "-Inos", "-Itests",
                       "tests/svc_track_tracing_test.c", "tests/app_observability_test_log.c", "-lm", "-o", str(exe)]
            environment = dict(os.environ, TEMP=str(directory), TMP=str(directory), TMPDIR=str(directory))
            result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True, timeout=60)
            (base / "compile.log").write_text(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=directory, env=environment, capture_output=True, text=True, timeout=20)
            (base / "result.log").write_text(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
