"""Independent pre-cull card raster/recorder oracle, fresh current-source objects."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, link, run, stage_sky, workspace


@unittest.skipUnless(AVAILABLE, "Windows and gcc required")
class CardStripeTest(unittest.TestCase):
    def test_original_pages_equal_culled_raster_and_recorder(self):
        output = workspace("card-stripes")
        binary = output / "oracle.exe"
        link("tests/card_stripe_test.c", binary, exclude={"nos/faces/orrery_card.c"})
        stage_sky(output)
        result = run(binary, output)
        (output / "oracle.log").write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr)[-2000:])
        self.assertIn("original/candidate", result.stdout)


if __name__ == "__main__":
    unittest.main()
