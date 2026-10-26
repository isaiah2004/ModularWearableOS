"""Actual command parser framing and numeric rejection metadata, without devices."""
from pathlib import Path
import json
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("gcc"), "GCC required")
class CommandInputMetadataTests(unittest.TestCase):
    def test_actual_parser_framing_and_numeric_metadata(self):
        base = ROOT / "build/alpha05/checks/command-input-metadata"
        base.mkdir(parents=True, exist_ok=True)
        out = Path(tempfile.mkdtemp(prefix="parser-", dir=base))
        results = []
        for optimization in ("-O0", "-O2"):
            binary = out / (optimization[1:] + ".exe")
            command = ["gcc", "-std=c11", optimization, "-Wall", "-Wextra", "-Werror", "-Inos",
                       "tests/command_input_test.c", "nos/command_input.c", "-o", str(binary)]
            built = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=30)
            (out / (optimization[1:] + "-compile.log")).write_text(built.stdout + built.stderr)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], cwd=out, capture_output=True, text=True, timeout=15)
            (out / (optimization[1:] + ".log")).write_text(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("Numeric rejection offsets/NULs:", result.stdout)
            results.append({"optimization": optimization, "compile_exit": built.returncode,
                            "run_exit": result.returncode, "output": result.stdout})
        (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")


if __name__ == "__main__":
    unittest.main()
