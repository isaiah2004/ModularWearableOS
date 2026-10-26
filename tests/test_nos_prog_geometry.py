"""Independent pre-cache scalar renderer versus retained ring geometry."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class ProgGeometryTests(unittest.TestCase):
    def run_oracle(self, flags, source="tests/nos_prog_geometry_test.c"):
        with tempfile.TemporaryDirectory(prefix="nos-prog-geometry-") as tmp:
            exe = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            command = ["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Icore", "-Inos", *flags, source,
                       "nos/prog.c", "nos/dot.c", "nos/dot_glyphs.c", "core/gfx.c",
                       "-lm", "-o", str(exe)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS 14448 exact pixels+record cases", result.stdout)
            if source.endswith("nos_prog_poly_test.c"):
                self.assertIn("PASS 30000 polygon property cases", result.stdout)

    def test_pixels_records_and_allocation_lifecycle(self):
        self.run_oracle([])

    def test_tracked_budget_reserve_and_failure_fallback(self):
        self.run_oracle(["-DNOS_PSRAM_TRACKING"])

    def test_polygon_solid_intervals_match_scalar_pixels_and_counters(self):
        self.run_oracle(["-DNOS_PSRAM_TRACKING", "-DGFX_COST_MODEL"],
                        "tests/nos_prog_poly_test.c")

    def test_fused_multiply_add_preserves_translated_pixels(self):
        if platform.machine().lower() not in ("amd64", "x86_64", "i386", "i686"):
            self.skipTest("host FMA probe is specific to x86")
        with tempfile.TemporaryDirectory(prefix="nos-prog-fma-") as tmp:
            probe = Path(tmp) / "probe.c"
            exe = Path(tmp) / ("probe.exe" if os.name == "nt" else "probe")
            probe.write_text('int main(void) { return !__builtin_cpu_supports("fma"); }')
            result = subprocess.run(["gcc", str(probe), "-o", str(exe)],
                                    capture_output=True, text=True)
            if result.returncode or subprocess.run([str(exe)]).returncode:
                self.skipTest("compiler and CPU must support FMA")
        self.run_oracle(["-DNOS_PSRAM_TRACKING", "-mfma", "-ffp-contract=fast"])
        self.run_oracle(["-DNOS_PSRAM_TRACKING", "-DGFX_COST_MODEL",
                         "-mfma", "-ffp-contract=fast"], "tests/nos_prog_poly_test.c")


if __name__ == "__main__":
    unittest.main()
