"""Compile and run reproducible public-API workout state-machine stress tests."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seeds", type=int, default=100)
    parser.add_argument("--actions", type=int, default=1000)
    parser.add_argument("--first-seed", type=int, default=1)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--cc", default="gcc")
    args = parser.parse_args()
    if args.seeds < 1 or args.actions < 1:
        parser.error("--seeds and --actions must be positive")
    if args.first_seed < 0 or args.first_seed + args.seeds - 1 > 0xFFFFFFFF:
        parser.error("seed range must fit uint32")
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = args.out or ROOT / "build/workout-fuzz" / stamp
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    binary = output / "workout_fuzz_test.exe"
    command = [args.cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Icore", "-Inos",
               "tests/workout_fuzz_test.c", "nos/workout.c", "nos/workout_motion.c", "-lm", "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True)
    results = []
    for seed in range(args.first_seed, args.first_seed + args.seeds):
        store = output / f"seed-{seed}"
        completed = subprocess.run([str(binary), str(seed), str(store), str(args.actions)],
                                   cwd=ROOT, capture_output=True, text=True, timeout=60)
        (output / f"seed-{seed}.txt").write_text(completed.stdout + completed.stderr)
        results.append({"seed": seed, "returncode": completed.returncode,
                        "stdout": completed.stdout, "stderr": completed.stderr})
        if completed.returncode:
            print(completed.stderr, flush=True)
            break
        if len(results) % 10 == 0:
            print(f"PASS {len(results)} seeds / {len(results) * args.actions} randomized actions", flush=True)
    report = {"seeds": args.seeds, "actions_per_seed": args.actions,
              "completed": len(results), "source_sha256": hashlib.sha256((ROOT / "nos/workout.c").read_bytes()).hexdigest(),
              "public_test_sha256": hashlib.sha256((ROOT / "tests/workout_fuzz_test.c").read_bytes()).hexdigest(),
              "results": results}
    (output / "results.json").write_text(json.dumps(report, indent=2))
    success = len(results) == args.seeds and all(result["returncode"] == 0 for result in results)
    print(f"{'PASS' if success else 'FAIL'} evidence {output}")
    return not success


if __name__ == "__main__":
    sys.exit(main())
