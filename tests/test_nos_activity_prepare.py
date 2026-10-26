"""Activity frame metadata against original drawing and actual lifecycle."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class ActivityPrepareTests(unittest.TestCase):
    def run_c(self, source, extra, flags=()):
        with tempfile.TemporaryDirectory(prefix="nos-activity-prepare-") as tmp:
            exe = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            result = subprocess.run(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra",
                                     "-Werror", "-Icore", "-Inos", *flags, str(source),
                                     *map(str, extra), "-lm", "-o", str(exe)],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS", result.stdout)

    def test_actual_activity_preparation_and_lifecycle(self):
        self.run_c("tests/nos_activity_prepare_test.c", ["nos/motion.c", "core/gfx.c"],
                   ["-Inos/apps", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections"])

    def test_activity_preserves_fma_order(self):
        if platform.machine().lower() not in ("amd64", "x86_64", "i386", "i686"):
            self.skipTest("host FMA probe is specific to x86")
        with tempfile.TemporaryDirectory(prefix="nos-activity-fma-") as tmp:
            source = Path(tmp) / "probe.c"
            exe = Path(tmp) / ("probe.exe" if os.name == "nt" else "probe")
            source.write_text('int main(void) { return !__builtin_cpu_supports("fma"); }')
            result = subprocess.run(["gcc", str(source), "-o", str(exe)], capture_output=True)
            if result.returncode or subprocess.run([str(exe)]).returncode:
                self.skipTest("compiler and CPU must support FMA")
        self.run_c("tests/nos_activity_prepare_test.c", ["nos/motion.c", "core/gfx.c"],
                   ["-Inos/apps", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                    "-mfma", "-ffp-contract=fast"])


if __name__ == "__main__":
    unittest.main()
