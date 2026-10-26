import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class PipelineProbeTests(unittest.TestCase):
    def test_fenced_threaded_capture(self):
        with tempfile.TemporaryDirectory(prefix="nos-pipeline-") as tmp:
            binary = Path(tmp) / ("probe.exe" if os.name == "nt" else "probe")
            result = subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                     "-pthread", "tests/nos_pipeline_probe_test.c", "-o", str(binary)],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS pipeline", result.stdout)


if __name__ == "__main__":
    unittest.main()
