"""Actual firmware display, command-wrapper and shutdown functions with fault I/O.

No target, shared build or SDK mutation. Extract complete functions verbatim so
fault assertions exercise the production control flow rather than a second model.
"""
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT / "build/alpha05/checks/platform-host"
SOURCE=(ROOT / "firmware/main/nos_main.c").read_text()


def function(name):
    match=re.search(r"^(?:static\s+)?(?:void|int|esp_err_t|uint32_t|nos_scroll_snapshot)\s+(?:__attribute__\(\(noreturn\)\)\s+)?"+name+r"\s*\([^;]*?\)\s*\{",SOURCE,re.M)
    if not match:raise AssertionError(name)
    # Functions in this set contain balanced braces in comments/strings.
    depth=1;pos=match.end()
    while depth:
        depth+=(SOURCE[pos]=="{")-(SOURCE[pos]=="}");pos+=1
    return SOURCE[match.start():pos]


@unittest.skipUnless(shutil.which("gcc"),"host GCC required")
class HostLifecycleTests(unittest.TestCase):
    def run_harness(self,name,functions):
        OUT.mkdir(parents=True,exist_ok=True)
        text=(ROOT / f"tests/nos_host_{name}_test.c").read_text()
        text=text.replace("/* ACTUAL FUNCTIONS */","\n\n".join(function(n) for n in functions))
        generated=OUT / (name+".c");generated.write_text(text)
        binary=OUT / (name+".exe")
        compile=subprocess.run(["gcc","-O2","-std=c11","-Wall","-Wextra","-Werror",
             "-Wno-unused-parameter","-Icore","-Inos","-Ifirmware/main",str(generated),"-o",str(binary)],
             cwd=ROOT,capture_output=True,text=True)
        self.assertEqual(compile.returncode,0,compile.stdout+compile.stderr)
        result=subprocess.run([str(binary)],cwd=OUT,capture_output=True,text=True,timeout=30)
        (OUT / (name+".log")).write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn("PASS",result.stdout)

    def test_display_timeout_and_real_ack_ownership(self):
        self.run_harness("display",("display_fault","display_take","display_queue","panel_execute",
            "panel_execute_retry","panel_command","flush_task","stripe_wait","stripe_flush","frame_drain"))

    def test_correlated_actual_host_wrappers(self):
        self.run_harness("protocol",("cmd_printf","host_transfer_line","host_done","host_transfer_execute","host_ui_execute",
                                     "host_command_readonly","host_ui_dispatch"))

    def test_shutdown_actual_barriers(self):
        self.run_harness("shutdown",("nos_host_shutdown_pending","shutdown_cancel","shutdown_guard","nos_host_power","shutdown_tick","power_restart_request"))

    def test_contact_loss_and_stale_panel_ack(self):
        self.run_harness("contact",())

    def test_failed_waking_touch_rearms_the_next_gesture(self):
        self.run_harness("touch_recovery",("touch_drain",))

    def test_touch_diagnostic_distinguishes_task_and_controller_stalls(self):
        self.run_harness("touch_state",("touch_state_report","host_command_readonly"))

    def test_authoritative_wrist_and_radio_boot_intent(self):
        self.run_harness("preferences",("wake_wrist_sync","wake_learn_request","radio_preferences","calendar_preferences"))

    def test_crash_operator_commands_are_bounded(self):
        self.run_harness("crash",("host_crash_command",))

    def test_runtime_snapshot_is_readonly_and_preserves_unknowns(self):
        self.run_harness("runtime",("runtime_state_report","host_command_readonly"))

    def test_embedded_apps_pose_requires_settled_home_page(self):
        self.run_harness("scroll",("scroll_snapshot",))


if __name__=="__main__":unittest.main()
