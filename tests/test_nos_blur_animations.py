"""Actual retained callbacks preserve authored animation pixels and band signatures."""
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class BlurAnimationTests(unittest.TestCase):
    def compile_run(self, source, extra=(), flags=()):
        with tempfile.TemporaryDirectory(prefix="nos-blur-animation-") as tmp:
            exe = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            result = subprocess.run([
                "gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-Wno-format-truncation", "-Wno-unused-parameter", "-Icore", "-Inos", "-Inos/apps",
                "-flto", "-fwhole-program", "-ffunction-sections", "-fdata-sections",
                *flags, str(source), *map(str, extra), "-Wl,--gc-sections", "-lm", "-o", str(exe),
            ], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS", result.stdout)

    def animation(self, case, cadence=40, flags=()):
        self.compile_run("tests/nos_blur_animation_test.c", [
            "core/gfx.c", "core/sprite.c", "nos/blur_cache.c", "nos/dot.c", "nos/dot_glyphs.c",
            "nos/prog.c", "nos/ui.c", "nos/motion.c", *sorted((ROOT / "nos/fonts").glob("*.c")),
        ], [f"-DAPP_CASE={case}", f"-DNOS_FPS={cadence}", *flags])

    def test_weather_current_icon_pixels_and_selective_signatures(self):
        self.animation(1)

    def test_about_glint_and_morph_pixels_and_selective_signatures(self):
        self.animation(2)

    def test_modules_live_morph_and_sampling_thresholds_at_30_40_60(self):
        for cadence in (30, 40, 60):
            with self.subTest(cadence=cadence):
                self.animation(3, cadence)

    def test_authored_pixels_preserve_fma_order(self):
        if platform.machine().lower() not in ("amd64", "x86_64", "i386", "i686"):
            self.skipTest("host FMA probe is specific to x86")
        with tempfile.TemporaryDirectory(prefix="nos-animation-fma-") as tmp:
            source = Path(tmp) / "probe.c"
            exe = Path(tmp) / ("probe.exe" if os.name == "nt" else "probe")
            source.write_text('int main(void) { return !__builtin_cpu_supports("fma"); }')
            result = subprocess.run(["gcc", str(source), "-o", str(exe)], capture_output=True)
            if result.returncode or subprocess.run([str(exe)]).returncode:
                self.skipTest("compiler and CPU must support FMA")
        for case in (1, 2, 3):
            with self.subTest(app=case):
                self.animation(case, flags=["-mfma", "-ffp-contract=fast"])

    def test_actual_modules_tick_uses_separate_sampling_gate(self):
        source = (ROOT / "nos/apps/set_modules.c").read_text()
        start = source.index("static void modules_tick(float dt)")
        end = source.index("\n}\n", start) + 3
        with tempfile.TemporaryDirectory(prefix="nos-sampling-guard-") as tmp:
            generated = Path(tmp) / "sampling.c"
            generated.write_text((ROOT / "tests/nos_modules_sampling_test.c").read_text()
                                 .replace("/* ACTUAL_MODULES_TICK */", source[start:end]))
            self.compile_run(generated)


if __name__ == "__main__":
    unittest.main()
