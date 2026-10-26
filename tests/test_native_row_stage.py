"""Exact optional native staging, with all compiler/runtime writes in the workspace."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHECKS = ROOT / "build/alpha05/checks/native-row-stage-tests"


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class NativeRowStageTests(unittest.TestCase):
    def compile_run(self, source, extra_sources=(), flags=()):
        CHECKS.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="actual-", dir=CHECKS) as tmp:
            out = Path(tmp)
            env = os.environ | {key: str(out) for key in ("TEMP", "TMP", "TMPDIR")}
            exe = out / "check.exe"
            if isinstance(source, str):
                path = out / "fixture.c"
                path.write_text(source)
            else:
                path = source
            command = ["gcc", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-ffunction-sections", "-fdata-sections",
                       "-Icore", "-Inos", *flags, str(path), *extra_sources,
                       "-Wl,--gc-sections", "-lm", "-o", str(exe)]
            compiled = subprocess.run(command, cwd=ROOT, env=env, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            ran = subprocess.run([str(exe)], cwd=out, env=env, capture_output=True,
                                 text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            return ran.stdout

    def test_actual_kernel_modes_recording_coverage_and_copy_bounds(self):
        output = self.compile_run(ROOT / "tests/native_row_stage_test.c",
                                  ("core/gfx.c", "core/sprite.c"))
        self.assertIn("PASS rows=30000", output)
        self.assertIn("PASS blits=5600 modes=0,1,2,3", output)
        self.assertIn("coverage_observation_equal", output)

    def test_production_default_storage_size_and_alignment(self):
        # Compile the actual production definition prefix, without the unrelated
        # renderer's dependencies (PE linkers retain its exported functions).
        prefix = (ROOT / "nos/ui_blur.c").read_text().split(
            "/* ------------------------------------------------ retained color motion cache", 1)[0]
        self.assertIn("int nos_native_row_stage_mode=0;", prefix)
        self.assertIn("DRAM_ATTR", prefix)
        output = self.compile_run(prefix + '''
#include "ui_blur_observe.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
int main(void){
 assert(nos_native_row_stage_mode==0);
 assert(sizeof nos_native_row_stage_scratch==512);
 assert(((uintptr_t)nos_native_row_stage_scratch&3u)==0);
 for(unsigned i=0;i<sizeof nos_native_row_stage_scratch;i++)
  assert(nos_native_row_stage_scratch[i]==0);
 puts("PASS actual production default0 scratch512 aligned4");
 return 0;
}
''')
        self.assertIn("PASS actual production default0", output)


if __name__ == "__main__":
    unittest.main()
