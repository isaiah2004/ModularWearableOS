"""Audit input integrity and timestamp-gap behavior against the real C adapter."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("imu_audit", ROOT / "tools/imu/audit.py")
audit = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit)


@unittest.skipUnless(shutil.which("gcc"), "GCC required for production-engine replay")
class AuditTest(unittest.TestCase):
    def test_immutable_input_gap_nonfinite_and_empty(self):
        with tempfile.TemporaryDirectory(prefix="imu-audit-") as directory:
            out = Path(directory)
            binary = out / "replay.exe"
            subprocess.run(["gcc", "-std=c99", "-Wall", "-Wextra", "-Werror", "-Inos",
                            "tools/imu/motion_replay.c", "nos/workout_motion.c", "-lm", "-o", str(binary)],
                           cwd=ROOT, check=True, capture_output=True)
            source = out / "fixture.csv"
            source.write_text("ms,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n"
                              "0,0,0,1,0,0,0\n40,0,0,1,0,0,0\n"
                              "1000,0,0,1,0,0,0\n1040,0,0,1,0,0,0\n"
                              "1080,nan,0,1,0,0,0\ninvalid,row\n")
            before = source.read_bytes()
            result = audit.analyze(source, binary, out, "synthetic fixture")
            self.assertEqual(result["engine_gap_count"], 1)
            self.assertEqual(result["motion"]["valid_ms"], 80)
            self.assertEqual(result["motion"]["movement_ms_est"], 0)
            self.assertEqual(result["axes"]["ax_g"]["nonfinite"], 1)
            self.assertEqual(len(result["malformed_rows"]), 1)
            self.assertEqual(result["identical_adjacent_6axis_rows"], 3)
            self.assertEqual(source.read_bytes(), before)
            empty = out / "empty.csv"
            empty.write_bytes(b"")
            self.assertEqual(audit.analyze(empty, binary, out, "fixture")["status"], "empty_preserved")
            self.assertEqual(empty.read_bytes(), b"")


if __name__ == "__main__":
    unittest.main()
