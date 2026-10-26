"""Fresh current-engine regressions; all ABI participants compiled privately."""
from pathlib import Path
import importlib.util
import shutil
import subprocess
import unittest
import os

# The validation runner imports this module by file path and does not put
# tests/ on sys.path. Resolve the sibling helper independently of its caller.
SPEC = importlib.util.spec_from_file_location("engine_private_build", Path(__file__).with_name("engine_private_build.py"))
ENGINE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ENGINE)
CHECKS, build_objects, link_test, run_test = ENGINE.CHECKS, ENGINE.build_objects, ENGINE.link_test, ENGINE.run_test

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc") and os.name == "nt", "Windows host GCC required")
class EngineClockTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        CHECKS.mkdir(parents=True,exist_ok=True)
        cls.out=CHECKS / "integration"
        cls.out.mkdir(parents=True,exist_ok=True)
        cls.objects=build_objects(cls.out / "objects")

    def test_real_engine_elapsed_clock_and_pacing(self):
        exe=self.out / "clock.exe"
        link_test("tests/nos_engine_clock_test.c",exe,self.objects)
        result=run_test(exe,self.out / "clock")
        (CHECKS / "engine-clock.log").write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn("PASS actual engine elapsed clock",result.stdout)

    def test_notification_gesture_stable_identity(self):
        exe=self.out / "notifs.exe"
        link_test("tests/nos_notifs_identity_test.c",exe,self.objects,exclude={"nos/apps/sys_notifs.c"})
        result=run_test(exe,self.out / "notifs")
        (CHECKS / "notification-identity.log").write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn("PASS notification stable IDs",result.stdout)

    def test_navigation_logs_actual_completion(self):
        exe=self.out / "semantic.exe"
        link_test("tests/nos_semantic_actions_test.c",exe,self.objects)
        result=run_test(exe,self.out / "semantic")
        (CHECKS / "semantic-actions.log").write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn("PASS semantic navigation",result.stdout)


if __name__ == "__main__":
    unittest.main()
