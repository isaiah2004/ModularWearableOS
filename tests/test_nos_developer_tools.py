"""Developer overlay controls and the cached, read-only trace status page."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class DeveloperToolsTests(unittest.TestCase):
    def test_controls_and_trace_status(self):
        compiler = shutil.which("gcc")
        if not compiler:
            self.skipTest("host GCC unavailable")
        with tempfile.TemporaryDirectory(prefix="watch-developer-") as directory:
            executable = pathlib.Path(directory) / "developer.exe"
            compiled = subprocess.run([
                compiler, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-I" + str(ROOT / "core"), "-I" + str(ROOT / "nos"),
                str(ROOT / "tests/developer_tools_test.c"), "-lm", "-o", str(executable),
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
