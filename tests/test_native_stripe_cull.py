"""Actual Apps/Settings stripe work budgets and full-frame output equivalence."""
from pathlib import Path
import hashlib
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, link, run, workspace

FIXTURE = ROOT / "tests/native_stripe_cull_test.c"
SOURCES = {7: "nos/apps/sys_launcher.c", 8: "nos/apps/set_settings.c"}


def probe(app, source):
    out = workspace("native-stripe-" + str(app))
    original = source.read_text()
    marker = ("        const uint16_t *lut = launcher_palette(g.lev,pressed);" if app == 7 else
              "                    const entry_image *a=&r->image[low],*b=&r->image[high];")
    assert original.count(marker) == 1
    counted = "static unsigned long long stripe_test_metadata;\n" + original.replace(
        marker, "stripe_test_metadata++;\n" + marker, 1)
    private = out / "counted_app.c"
    private.write_text(counted)
    exe = out / "check.exe"
    link(FIXTURE, exe, exclude={SOURCES[app]},
         defines=(f'APP_SOURCE="{private.as_posix()}"', f"APP_ID={app}"))
    result = run(exe, out, "skipboot\nroot home\nwait 2\n", timeout=120)
    (out / "evidence.json").write_text(json.dumps({
        "source": SOURCES[app], "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(FIXTURE.read_bytes()).hexdigest(),
        "exit_code": result.returncode, "stdout": result.stdout,
        "timing": "Work counts only; no target FPS claim"
    }, indent=2) + "\n")
    return result, out


@unittest.skipUnless(AVAILABLE, "Windows and GCC required for current NOS source closure")
class NativeStripeCullTests(unittest.TestCase):
    def test_apps_actual_metadata_budget_and_output(self):
        result, out = probe(7, ROOT / SOURCES[7])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr + "\n" + str(out))
        self.assertIn("PASS native stripe metadata app=7 cases=15", result.stdout)

    def test_settings_actual_metadata_budget_and_output(self):
        result, out = probe(8, ROOT / SOURCES[8])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr + "\n" + str(out))
        self.assertIn("PASS native stripe metadata app=8 cases=15", result.stdout)


if __name__ == "__main__":
    unittest.main()
