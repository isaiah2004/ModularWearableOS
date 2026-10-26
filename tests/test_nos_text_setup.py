"""Exact shared font lookup and dot stripe-setup regression checks."""
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class TextSetupTests(unittest.TestCase):
    def run_c(self, source, extra, flags=()):
        with tempfile.TemporaryDirectory(prefix="nos-text-setup-") as tmp:
            exe = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            result = subprocess.run(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra",
                                     "-Werror", "-Icore", "-Inos", *flags, str(source),
                                     *map(str, extra), "-lm", "-o", str(exe)],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS", result.stdout)

    def test_actual_font_lookup_pointer_selection(self):
        with tempfile.TemporaryDirectory(prefix="nos-font-lookups-") as tmp:
            functions = []
            for filename, old, new in [("core/font.c", "find_glyph", "test_core_lookup"),
                                       ("nos/ui.c", "glyph", "test_ui_lookup")]:
                source = (ROOT / filename).read_text()
                start = source.index("static const font_glyph *" + old + "(")
                end = source.index("\n}", start) + 2
                functions.append(source[start:end].replace("static ", "", 1)
                                 .replace(old + "(", new + "(", 1))
            fonts = sorted((ROOT / "core/fonts").glob("*.c")) + sorted((ROOT / "nos/fonts").glob("*.c"))
            names = [re.search(r"const font_t (\w+)", font.read_text())[1] for font in fonts]
            generated = Path(tmp) / "lookups.c"
            generated.write_text('#include "font.h"\n' + "\n".join(functions) + "\n" +
                                 "\n".join("extern const font_t " + name + ";" for name in names) +
                                 "\nconst font_t *test_fonts[]={" + ",".join("&" + name for name in names) +
                                 "};\nunsigned test_font_count=sizeof test_fonts/sizeof *test_fonts;\n")
            self.run_c("tests/nos_font_lookup_test.c", [generated, *fonts], ["-pedantic-errors"])

    def test_dot_row_bounds_preserve_pixels_records_and_allocation(self):
        self.run_c("tests/nos_dot_row_bounds_test.c", ["nos/dot_glyphs.c", "core/gfx.c"])

    def test_dot_row_bounds_preserve_fma_pixels(self):
        if platform.machine().lower() not in ("amd64", "x86_64", "i386", "i686"):
            self.skipTest("host FMA probe is specific to x86")
        with tempfile.TemporaryDirectory(prefix="nos-text-fma-") as tmp:
            source = Path(tmp) / "probe.c"
            exe = Path(tmp) / ("probe.exe" if os.name == "nt" else "probe")
            source.write_text('int main(void) { return !__builtin_cpu_supports("fma"); }')
            result = subprocess.run(["gcc", str(source), "-o", str(exe)], capture_output=True)
            if result.returncode or subprocess.run([str(exe)]).returncode:
                self.skipTest("compiler and CPU must support FMA")
        self.run_c("tests/nos_dot_row_bounds_test.c", ["nos/dot_glyphs.c", "core/gfx.c"],
                   ["-mfma", "-ffp-contract=fast"])


if __name__ == "__main__":
    unittest.main()
