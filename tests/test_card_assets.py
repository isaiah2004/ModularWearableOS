"""Real card delayed-backend ownership lifecycle using private simulator objects."""
from pathlib import Path
import sys
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from render_private_build import AVAILABLE, CHECKS, link, run, workspace

@unittest.skipUnless(AVAILABLE, "Windows and gcc required")
class CardAssetTests(unittest.TestCase):
    def test_exit_reentry_night_ring_and_shutdown(self):
        output=workspace("card-assets")
        exe=output/"oracle.exe"
        link("tests/card_asset_test.c",exe,exclude={"nos/faces/orrery_card.c"})
        with tempfile.TemporaryDirectory(prefix="nos-card-assets-",dir=CHECKS) as tmp:
            sky=Path(tmp)/"sky";sky.mkdir()
            for name,value,size in [("earth.565",0x11,1048576),("earth_night.565",0x22,1048576),
                                    ("mars.565",0x33,1048576),("saturn.565",0x44,1048576),
                                    ("saturn_ring.rgba",0x77,2048)]:
                (sky/name).write_bytes(bytes([value])*size)
            result=run(exe,output,env={"NOS_SD_ROOT":tmp})
        (output/"oracle.log").write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,(result.stdout+result.stderr)[-2000:])
        self.assertIn("PASS async card",result.stdout)
if __name__=="__main__":unittest.main()
