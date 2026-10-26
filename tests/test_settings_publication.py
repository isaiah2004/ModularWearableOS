"""Exercise actual Settings publication ownership with the full current renderer.

Only loop counters are inserted into a private copy. The production algorithm,
assets, allocator, fonts and blur observer are linked from current sources.
"""
from pathlib import Path
import hashlib
import json
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, link, run, workspace


def counted_source(source):
    """Observe function entries and row visits without replacing decisions."""
    start = "static void rows_rom_sync(void)\n{"
    walk = "    row_cancel();\n    for(int e=0;e<NE;e++){\n        unsigned sig="
    assert source.count(start) == 1
    assert source.count(walk) == 1
    source = source.replace(start, start + "\n    test_sync_calls++;", 1)
    source = source.replace(walk, "    test_sync_walks++;\n    row_cancel();\n"
                            "    for(int e=0;e<NE;e++){\n"
                            "        test_entry_visits++;\n        unsigned sig=", 1)
    return "static unsigned test_sync_calls,test_sync_walks,test_entry_visits;\n" + source


@unittest.skipUnless(AVAILABLE, "Windows and gcc required")
class SettingsPublicationTests(unittest.TestCase):
    def test_actual_publication_dedup_and_invalidation(self):
        out = workspace("settings-publication")
        original = ROOT / "nos/apps/set_settings.c"
        before = hashlib.sha256(original.read_bytes()).hexdigest()
        source = out / "counted_settings.c"
        source.write_text(counted_source(original.read_text()))
        exe = out / "check.exe"
        link("tests/settings_publication_test.c", exe,
             exclude={"nos/apps/set_settings.c"},
             defines=(f'SETTINGS_SOURCE="{source.as_posix()}"',
                      "SETTINGS_TEST_PUBLICATION_SERIAL=1"),
             extra_flags=("-Wl,--wrap=native_menu_asset", "-Wl,--wrap=native_menu_sprite"))
        result = run(exe, out, "skipboot\nroot home\nwait 2\n", timeout=180)
        after = hashlib.sha256(original.read_bytes()).hexdigest()
        (out / "publication.json").write_text(json.dumps({
            "source_sha256": before, "source_unchanged": before == after,
            "renderer": "full current source closure with strong real blur observer",
            "stdout": result.stdout, "returncode": result.returncode,
            "timing": "No target timing claim; source loop counters only"
        }, indent=2) + "\n")
        self.assertEqual(before, after, "Settings changed during regression")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS Settings publication", result.stdout)
        self.assertIn("warm_pairs=40 visits_per_pair=36", result.stdout)


if __name__ == "__main__":
    unittest.main()
