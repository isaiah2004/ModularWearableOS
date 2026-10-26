"""Actual Control Center draws are quiet; disabled input has one causal trace."""
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import render_private_build as render


@unittest.skipUnless(render.AVAILABLE, "Windows GCC simulator closure required")
class ControlDrawLoggingTests(unittest.TestCase):
    def test_actual_draws_stripes_blur_sources_and_disabled_taps(self):
        out = render.workspace("control-draw-logging")
        binary = out / "control.exe"
        wraps = tuple("-Wl,--wrap=" + name for name in (
            "diag_action_request", "diag_action_context", "diag_action_result", "diag_log_event"))
        render.link("tests/control_draw_logging_test.c", binary,
                    exclude=("nos/apps/sys_control.c",), extra_flags=wraps)
        result = render.run(binary, out, script="skipboot\nroot home\nwait 2\n", timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("pixel_cases=74 draw_requests=0 disabled_taps=4", result.stdout)


if __name__ == "__main__":
    unittest.main()
