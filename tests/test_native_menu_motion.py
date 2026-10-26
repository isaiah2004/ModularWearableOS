"""Private host checks for native fractional tiles and Settings control ownership."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, CHECKS, environment, link, run, workspace

@unittest.skipUnless(shutil.which("gcc"), "gcc required")
class NativeMenuMotionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        CHECKS.mkdir(parents=True, exist_ok=True)

    def test_normalized_continuous_smear_reference(self):
        with tempfile.TemporaryDirectory(prefix="native-smear-", dir=CHECKS) as tmp:
            exe = Path(tmp) / "check.exe"
            env = environment(Path(tmp))
            subprocess.run(["gcc", "-O2", "-std=c99", "-Icore", "-Inos", "tests/native_smear_test.c", "-lm", "-o", str(exe)], cwd=ROOT, env=env, check=True)
            result = subprocess.run([str(exe)], cwd=Path(tmp), env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS normalized continuous smear", result.stdout)

    def test_independent_fractional_and_prepared_pixels(self):
        with tempfile.TemporaryDirectory(prefix="native-tile-", dir=CHECKS) as tmp:
            exe = Path(tmp) / "check.exe"
            env = environment(Path(tmp))
            subprocess.run(["gcc", "-O2", "-std=c99", "-ffunction-sections", "-fdata-sections", "-Icore", "-Inos", "-Inos/fonts", "tests/native_tile_test.c", "core/gfx.c", "core/sprite.c", "-Wl,--gc-sections", "-lm", "-o", str(exe)], cwd=ROOT, env=env, check=True)
            result = subprocess.run([str(exe)], cwd=Path(tmp), env=env, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS prepared tiles", result.stdout)
            self.assertIn("PASS native tiles", result.stdout)

    def test_bounded_carry_matches_legacy_and_independent_box(self):
        with tempfile.TemporaryDirectory(prefix="native-carry-",dir=CHECKS) as tmp:
            exe=Path(tmp)/"check.exe"
            env = environment(Path(tmp))
            result=subprocess.run(["gcc","-O2","-std=c11","-ffunction-sections","-fdata-sections",
                                   "-Icore","-Inos","-Inos/fonts","tests/native_tile_carry_test.c",
                                   "core/gfx.c","core/sprite.c","-Wl,--gc-sections","-lm","-o",str(exe)],
                                  cwd=ROOT,env=env,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            result=subprocess.run([str(exe)],cwd=Path(tmp),env=env,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS carry",result.stdout)

    @unittest.skipUnless(AVAILABLE, "Windows and gcc required")
    def test_actual_settings_carry_owner_and_lifecycle(self):
        out = workspace("settings-carry")
        exe = out / "check.exe"
        link("tests/settings_carry_test.c", exe, exclude={"nos/apps/set_settings.c"})
        result = run(exe, out, "open! settings\nwait 25\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS Settings carry", result.stdout)

    @unittest.skipUnless(AVAILABLE, "Windows and gcc required")
    def test_actual_settings_atlas_sources_and_stripes(self):
        out = workspace("settings-controls")
        exe = out / "check.exe"
        link("tests/settings_controls_test.c", exe, exclude={"nos/apps/set_settings.c"},
             defines=("settings_sim_main=sim_program_main",))
        result = run(exe, out, "open! settings\nwait 25\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS Settings controls", result.stdout)

if __name__ == "__main__":
    unittest.main()
