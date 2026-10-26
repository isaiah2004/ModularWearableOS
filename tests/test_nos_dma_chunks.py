import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/alpha05/checks/dma-chunks"


def function(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


@unittest.skipUnless(shutil.which("gcc"), "gcc is required")
class DMAChunkTests(unittest.TestCase):
    def test_actual_flush_task_ownership_and_coordinates(self):
        source = (ROOT / "firmware/main/nos_main.c").read_text(encoding="utf-8")
        OUT.mkdir(parents=True, exist_ok=True)
        self.assertTrue(OUT.resolve().is_relative_to(ROOT.resolve()))
        env = dict(os.environ, TEMP=str(OUT), TMP=str(OUT), TMPDIR=str(OUT))
        with tempfile.TemporaryDirectory(prefix="nos-dma-", dir=OUT) as tmp:
            path = Path(tmp)
            functions = ("static void display_take(", "static esp_err_t panel_execute(",
                         "static esp_err_t panel_execute_retry(", "static void flush_task(void *arg)")
            (path / "actual_flush_task.h").write_text("\n\n".join(function(source, marker) for marker in functions), encoding="utf-8")
            binary = path / ("dma.exe" if os.name == "nt" else "dma")
            result = subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                                     "-Wno-unused-parameter", "-I" + str(path),
                                     "tests/nos_dma_chunks_test.c", "-o", str(binary)],
                                    cwd=ROOT, env=env, capture_output=True, text=True)
            (OUT / "compile.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], cwd=OUT, env=env, capture_output=True, text=True, timeout=30)
            (OUT / "run.log").write_text(result.stdout + result.stderr, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("PASS actual flush_task", result.stdout)


if __name__ == "__main__":
    unittest.main()
