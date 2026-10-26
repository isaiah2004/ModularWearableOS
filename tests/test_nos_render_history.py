import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class RenderHistoryTests(unittest.TestCase):
    def test_virtual_black_handoffs_and_odd_stride(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-virtual-") as tmp:
            for name in ("nos_history_virtual_test", "nos_history_virtual_multirow_test"):
                binary=Path(tmp)/(name+(".exe" if os.name=="nt" else ""))
                result=subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror",
                                       "-Icore","-Inos","tests/"+name+".c","-o",str(binary)],
                                      cwd=ROOT,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                result=subprocess.run([str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)

    def test_virtual_black_randomized_logical_history_matches_physical(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-virtual-diff-") as tmp:
            outputs=[]
            for mode in (0,1):
                binary=Path(tmp)/("check"+str(mode)+(".exe" if os.name=="nt" else ""))
                args=["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror","-Icore","-Inos"]
                if mode: args.append("-DCANDIDATE")
                result=subprocess.run([*args,"tests/nos_history_virtual_differential_test.c","-o",str(binary)],
                                      cwd=ROOT,capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                result=subprocess.run([str(binary)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                outputs.append(result.stdout)
            self.assertEqual(outputs[0],outputs[1])

    def test_virtual_black_worker_snapshot_matches_row_exposure(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-virtual-exposure-") as tmp:
            binary=Path(tmp)/("check.exe" if os.name=="nt" else "check")
            result=subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror",
                                   "-Icore","-Inos","tests/nos_history_virtual_exposure_test.c",
                                   "nos/render_exposure.c","-o",str(binary)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS6000",result.stdout)

    def test_native_copy_runtime_modes_preserve_sharp_and_metadata(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-native-copy-") as tmp:
            binary=Path(tmp)/("check.exe" if os.name=="nt" else "check")
            result=subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror",
                                   "-Icore","-Inos","tests/nos_history_native_copy_test.c",
                                   "-o",str(binary)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS native copy",result.stdout)

    def test_q5_pair_matches_independent_channels(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-q5-pair-") as tmp:
            binary=Path(tmp)/("check.exe" if os.name=="nt" else "check")
            result=subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror",
                                   "-Icore","-Inos","tests/nos_history_q5_pair_test.c","-o",str(binary)],
                                  cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS Q5 pair",result.stdout)

    def test_uniform_scan_matches_independent_scalar(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-uniform-scan-") as tmp:
            binary=Path(tmp)/("check.exe" if os.name=="nt" else "check")
            result=subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror",
                                   "-Icore","-Inos","tests/nos_history_uniform_scan_test.c","-o",str(binary)],
                                  cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS uniform scan",result.stdout)

    def test_cadence_weight_contract_pixels_paths_and_allocation(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-cadence-") as tmp:
            binary=Path(tmp)/("check.exe" if os.name=="nt" else "check")
            result=subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror",
                                   "-Icore","-Inos","tests/nos_history_cadence_test.c","-o",str(binary)],
                                  cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS cadence history",result.stdout)

    def test_temporal_exposure_equivalence_and_contract(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-") as tmp:
            binary = Path(tmp) / ("history.exe" if os.name == "nt" else "history")
            result = subprocess.run(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Icore", "-Inos", "tests/nos_render_history_test.c", "nos/render_history.c", "-o", str(binary)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("match", result.stdout)
    def test_disjoint_fast_path_matches_generic(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-disjoint-") as tmp:
            binary=Path(tmp)/("check.exe" if os.name=="nt" else "check")
            result=subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror","-Icore","-Inos","tests/nos_history_disjoint_test.c","nos/render_history.c","-o",str(binary)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn("exact",result.stdout)
    def test_packed_kernel_matches_independent_channels_and_legacy(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-packed-") as tmp:
            binary = Path(tmp) / ("check.exe" if os.name == "nt" else "check")
            result = subprocess.run(["gcc", "-O2", "-std=c11", "-Wall", "-Wextra",
                                     "-Werror", "-Icore", "-Inos",
                                     "tests/nos_history_packed_test.c", "-o", str(binary)],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS packed", result.stdout)

    def test_uniform_blocks_and_runtime_toggle(self):
        with tempfile.TemporaryDirectory(prefix="nos-history-uniform-") as tmp:
            binary=Path(tmp)/("check.exe" if os.name=="nt" else "check")
            result=subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror","-Icore","-Inos","tests/nos_history_uniform_test.c","-o",str(binary)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertIn("PASS solid",result.stdout)
if __name__ == "__main__": unittest.main()
