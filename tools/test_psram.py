"""Memory stress/fault recovery. Run after sim/nos_build.ps1 -Strict; no device IO."""
from pathlib import Path
import subprocess
import re

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/psram_tests"
OUT.mkdir(parents=True, exist_ok=True)

def run(name, lines, coverage=None):
    script = OUT / (name + ".txt")
    script.write_text("\n".join(lines) + "\n")
    result = subprocess.run([str(ROOT / "build/nos_sim.exe"), "--script", str(script),
                             "--out", str(OUT / name), "--no-media"], cwd=ROOT,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (OUT / (name + ".log")).write_bytes(result.stdout)
    if result.returncode:
        raise RuntimeError(f"{name} failed: see {OUT / (name + '.log')}")
    for owner, minimum in (coverage or {}).items():
        match = re.search(rf"BLUR kind={owner} moving=\d+ fast=(\d+) blurred=(\d+)", result.stdout.decode())
        if not match or int(match[1]) == 0 or 100 * int(match[2]) < minimum * int(match[1]):
            raise AssertionError(f"{name}: blur coverage below {minimum}% for owner {owner}")
    print(name, "PASS", flush=True)

lines = ["memprewarm", "include tools/nos_blur.txt", "home_now"]
for face in range(9):
    lines += [f"face {face}", "wait 3000"]
for app in ("activity heart breathe timer stopwatch alarm worldclock compass baro nav weather "
            "music cell find memo torch modules diag find_watch").split():
    lines += [f"open! {app}", "wait 1500", "home_now"]
lines += ["include tools/orrery/zoom.txt", "mem", "expect memfailures 0"]
for _ in range(5):
    lines += ["card 3", "open! orrery_card", "wait 7000", "mem",
              "expect memowner orrery_card.c 3166112", "expect memfailures 0",
              "open! torch", "wait 1000", "pop", "wait 1000",
              "expect top ORRERY CARD", "expect memowner orrery_card.c 3166112",
              "home", "wait 7000", "expect memowner orrery_card.c 0", "expect memfailures 0"]
lines += ["mem", "expect mempeak 6815744"]
run("heavy_reopen", lines, {1: 100})

for index in range(24):
    run(f"card_fail_{index}", ["memprewarm", "wait 10000", "card 3", f"memfail {index}",
        "open! orrery_card", "wait 100", "expect memowner orrery_card.c 0",
        "expect memfailures 1", "home_now", "wait 1000", "open! orrery_card", "wait 7000",
        "expect memowner orrery_card.c 3166112", "expect memfailures 1", "home_now",
        "wait 1000", "expect memowner orrery_card.c 0", "mem"])

lines = ["memprewarm", "face 8"]
for i in range(20):
    lines += [f"notify Messages|Card {i}|A longer message with two lines of body text."]
lines += ["wait 10000", "open notifs", "wait 2000"]
for _ in range(8):
    lines += ["flick 233 400 233 80 180", "wait 1800"]
lines += ["home_now", "card 3", "open! orrery_card", "wait 7000",
          "expect memowner orrery_card.c 3166112", "expect memfailures 0", "mem"]
run("older_notifications", lines, {2: 95})

run("fragmented_card", ["memprewarm", "wait 10000", "memchunk 524288", "card 3",
    "open! orrery_card", "wait 7000", "expect memowner orrery_card.c 3166112",
    "expect memfailures 0", "memchunk 0", "home_now", "wait 1000",
    "expect memowner orrery_card.c 0"])
for index in range(16):
    run(f"night_fail_{index}", ["memprewarm", "wait 10000", "card 3", "open! orrery_card",
        f"memfail {index}", "wait 7000", "expect memowner orrery_card.c 3166112",
        "expect memfailures 1", "home_now", "wait 1000", "expect memowner orrery_card.c 0"])
