"""Independent exact cooperative body preparation with fresh current-source objects."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, link, run, stage_sky, workspace

@unittest.skipUnless(AVAILABLE, "Windows and gcc required")
class CardPrepareIncrementalTests(unittest.TestCase):
    def test_original_state_and_cancellation(self):
        out = workspace("card-prepare")
        includes = [ROOT / "nos/faces/orrery_card.c",
                    ROOT / "nos/faces/orrery_card_prepare.inc",
                    ROOT / "tests/card_prepare_reference.h",
                    ROOT / "tests/card_prepare_incremental_test.inc"]
        harness = "#include <assert.h>\n" + "".join(
            '#include "' + path.as_posix() + '"\n' for path in includes)
        harness += ("extern int sim_program_main(int,char**);\n"
                    "int main(int argc,char**argv){assert(!sim_program_main(argc,argv));"
                    "card_prepare_incremental_tests();return 0;}\n")
        source = out / "probe.c"
        source.write_text(harness)
        binary = out / "check.exe"
        link(source, binary, exclude={"nos/faces/orrery_card.c"})
        stage_sky(out)
        result = run(binary, out, "face 8\nwait 3000\n")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS incremental prepare 99 exact bodies", result.stdout)

if __name__ == "__main__":
    unittest.main()
