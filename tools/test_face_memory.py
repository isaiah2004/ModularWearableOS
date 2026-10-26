"""Check interactive-face cache priority and automatic list-cache recovery; no device IO."""
import argparse
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = """memprewarm
face 8
notify Messages|Priority test|Retain this notification across face interaction.
wait 10000
log FACE_MEMORY locked_warm
mem
touch 300 300 1
wait 1700
touch 300 300 0
wait 800
log FACE_MEMORY interactive
mem
expect memowner kblur.c 0
expect memowner set_settings.c 0
drag 176 445 120 410 500
wait 400
tap 330 430
wait 400
drag 330 430 233 452 500
wait 5000
log FACE_MEMORY interactive_rest
mem
expect memowner kblur.c 0
expect memowner set_settings.c 0
expect memfailures 0
press
wait 10000
expect top HOME
log FACE_MEMORY locked_recovered
mem
expect memfailures 0
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", type=Path, default=ROOT / "build/nos_sim.exe")
    parser.add_argument("--out", type=Path, default=ROOT / "build/face_memory")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    script = args.out / "interactive_priority.txt"
    script.write_text(SCRIPT)
    result = subprocess.run([str(args.sim.resolve()), "--script", str(script.resolve()),
                             "--out", str((args.out/"frames").resolve()), "--no-media"],
                            cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log = args.out / "interactive_priority.log"
    log.write_bytes(result.stdout)
    if result.returncode:
        raise AssertionError(f"Simulator lifecycle assertions failed: {log}")
    text = result.stdout.decode(errors="replace")
    for phase in ("locked_warm", "locked_recovered"):
        match = re.search(r"FACE_MEMORY "+phase+r"\r?\n(.*?)(?=\[f|$)", text, re.S)
        assert match, f"Missing {phase}: {log}"
        for owner in ("kblur.c", "set_settings.c"):
            amount = re.search(r"MEM "+re.escape(owner)+r"\s+bytes=(\d+)", match[1])
            assert amount and int(amount[1]) > 0, f"{phase}: {owner} did not prewarm: {log}"
    print(f"PASS: interactive zoom has 0 allocation failures, inactive caches stay released, locking automatically prewarms lists; see {log}")


if __name__ == "__main__":
    main()

