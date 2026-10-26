"""Portable async asset mailbox ownership, cancellation and I/O timing tests."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which("gcc"),"gcc required")
class AsyncAssetTests(unittest.TestCase):
    def test_mailbox_ownership_and_timing(self):
        with tempfile.TemporaryDirectory(prefix="nos-asset-") as tmp:
            exe=Path(tmp)/("check.exe" if os.name=="nt" else "check")
            subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror","-Icore","-Inos",
                            "tests/asset_job_test.c","core/asset_job.c","-o",str(exe)],cwd=ROOT,check=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS immutable",result.stdout)
    def test_real_thread_cancel_while_destination_is_owned(self):
        with tempfile.TemporaryDirectory(prefix="nos-asset-thread-") as tmp:
            exe=Path(tmp)/("check.exe" if os.name=="nt" else "check")
            flags=[] if os.name=="nt" else ["-pthread"]
            subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror","-Icore",
                            "tests/asset_job_thread_test.c","core/asset_job.c",*flags,"-o",str(exe)],cwd=ROOT,check=True)
            result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            self.assertIn("PASS200",result.stdout)
if __name__=="__main__":unittest.main()
