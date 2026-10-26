import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class FrameWaitTests(unittest.TestCase):
    def test_injected_timer_delivery_and_failure_paths(self):
        compiler = shutil.which("gcc")
        if not compiler:
            self.skipTest("host GCC unavailable")
        with tempfile.TemporaryDirectory(prefix="watch-frame-wait-") as directory:
            executable = pathlib.Path(directory) / "frame_wait.exe"
            compiled = subprocess.run([
                compiler, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "firmware/main"),
                str(ROOT / "tests/nos_frame_wait_test.c"), str(ROOT / "firmware/main/nos_frame_wait.c"),
                "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
