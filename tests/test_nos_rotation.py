import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class RotationTests(unittest.TestCase):
    def test_actual_rotation_against_coordinates_and_memory_guards(self):
        with tempfile.TemporaryDirectory(prefix="nos-rotation-") as tmp:
            executable = Path(tmp) / ("rotate.exe" if os.name == "nt" else "rotate")
            compiled = subprocess.run(
                ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-Icore", "-Ifirmware/main", "tests/nos_rotation_test.c",
                 "firmware/main/board_protocol.c", "-lm", "-o", str(executable)],
                cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("20705 coordinate/canary/source-integrity cases x3 production modes",
                          result.stdout)


if __name__ == "__main__":
    unittest.main()
