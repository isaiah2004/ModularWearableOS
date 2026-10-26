"""Power-policy and asynchronous battery-history regression harnesses."""
import pathlib
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class PowerPolicyTests(unittest.TestCase):
    def compile_run(self, name):
        compiler = shutil.which("gcc")
        if not compiler:
            self.skipTest("host GCC is unavailable")
        with tempfile.TemporaryDirectory(prefix="watch-power-") as directory:
            executable = pathlib.Path(directory) / (name + ".exe")
            subprocess.run([
                compiler, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I" + str(ROOT / "core"), "-I" + str(ROOT / "nos"),
                str(ROOT / "tests" / (name + ".c")), "-lm", "-o", str(executable),
            ], check=True, cwd=ROOT, capture_output=True, text=True)
            subprocess.run([str(executable)], check=True, cwd=directory,
                           capture_output=True, text=True)

    def test_power_policy(self):
        self.compile_run("power_policy_test")

    @unittest.skipUnless(__import__("os").name == "nt", "sports host uses Windows direct.h")
    def test_battery_history_worker(self):
        self.compile_run("sports_test")


if __name__ == "__main__":
    unittest.main()
