"""Missing/truncated workloads must never turn into passing one-frame pixel checks."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", type=Path, default=ROOT / "build/nos_sim.exe")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="nos-script-") as directory:
        temporary = Path(directory)
        missing = temporary / "missing.txt"
        include = temporary / "include.txt"
        recursive = temporary / "recursive.txt"
        valid = temporary / "valid.txt"
        # Script include paths have no quoting syntax; use root-relative fixture
        # names in a temporary working directory whose input paths have no spaces.
        include.write_text("include missing.txt\nwait 25\n")
        recursive.write_text("include recursive.txt\n")
        valid.write_text("wait 25\nexpect top HOME\n")
        cases = [("valid", "--script", valid, True),
                 ("missing", "--script", missing, False),
                 ("include", "--script", include, False),
                 ("recursive", "--script", recursive, False),
                 ("damage", "--damagecheck", missing, False)]
        for name, mode, script, success in cases:
            result = subprocess.run(
                [str(args.sim.resolve()), mode, str(script), "--out", str(temporary / name),
                 "--no-media"], cwd=temporary, capture_output=True, text=True)
            assert (result.returncode == 0) == success, (name, result.stdout, result.stderr)
            if not success:
                assert "PASS damagecheck" not in result.stdout
            print(f"PASS {name}: exit {result.returncode}")


if __name__ == "__main__":
    main()
