"""Round-6 pixel equivalence and consistently priced round-5 baseline. No hardware."""
import csv
import os
from pathlib import Path
import shutil
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build/orrery/round6_equivalence"
OUT.mkdir(parents=True, exist_ok=True)
os.chdir(ROOT)
gcc = shutil.which("gcc") or r"C:\mingw64\bin\gcc.exe"
baseline = OUT / "baseline"
baseline.mkdir(exist_ok=True)
for name in ("face_orrery.c", "face_orrery_draw.h", "face_orrery_runs.h",
             "face_orrery_controls.h", "face_orrery_profile.h"):
    (baseline / name).write_bytes(subprocess.check_output(
        ["git", "show", f"05e8a2b:nos/faces/{name}"]))
(baseline / "nos.c").write_bytes(subprocess.check_output(["git", "show", "05e8a2b:nos/nos.c"]))
flags = ["-O2", "-std=c99", "-DGFX_COST_MODEL", "-Icore", "-Inos", "-Inos/fonts", "-Isim"]
for name in ("face_orrery", "nos"):
    subprocess.run([gcc, *flags, "-c", str(baseline / f"{name}.c"),
                    "-o", str(baseline / f"{name}.o")], check=True)
objects = [str(p) for p in (ROOT / "build/nos_obj").glob("*.o")
           if p.name not in ("nos_faces_face_orrery.o", "nos_nos.o")]
old = baseline / "sim.exe"
subprocess.run([gcc, *objects, str(baseline / "face_orrery.o"), str(baseline / "nos.o"),
                "-o", str(old), "-lgdi32", "-luser32", "-lwinmm", "-lm"], check=True)

sweep = OUT / "iris_sweep.txt"
lines = []
for face in range(9):
    lines += ["home_now", f"face {face}", "wait 2500"]
    for blur in (0, 1):
        for x, y in ((233, 233), (380, 100), (0, 233)):
            lines += [f"set iris_blur {blur}", f"mark face{face}_blur{blur}",
                      f"open settings {x} {y}", "wait 600", "pop", "wait 600",
                      f"open notifs {x} {y}", "wait 100", "pop", "wait 600"]
sweep.write_text("\n".join(lines) + "\n")
sources = [*sorted((ROOT / "tools/orrery").glob("*.txt")), sweep]
total = 0
for source in sources:
    script = OUT / source.name
    script.write_text("\n".join(line for line in source.read_text().splitlines()
                               if not line.lstrip().startswith("expect ")) + "\n")
    hashes = []
    for mode, exe in (("before", old), ("damage", ROOT / "build/nos_sim_round6.exe"),
                      ("full", ROOT / "build/nos_sim_round6.exe")):
        dest = OUT / f"{source.stem}_{mode}"
        digest = OUT / f"{source.stem}_{mode}.hash"
        args = [str(exe), "--script", str(script), "--out", str(dest),
                "--hashes", str(digest), "--no-media", "--quiet"]
        if mode == "full":
            args.append("--force-full")
        subprocess.run(args, check=True, capture_output=True)
        hashes.append(digest.read_bytes())
    assert hashes[0] == hashes[1], f"before/after pixels differ: {source.name}"
    assert hashes[1] == hashes[2], f"damage/full pixels differ: {source.name}"
    frames = len(hashes[0].splitlines())
    total += frames
    print(f"PASS before/after and damage/full: {source.name}, {frames} frames", flush=True)
print(f"PASS {total} frames in each comparison")

for mode in ("before", "damage"):
    rows = list(csv.DictReader((OUT / f"perf_round6_{mode}/frames.csv").open()))
    for section in dict.fromkeys(r["section"] for r in rows):
        sent = [r for r in rows if r["section"] == section and int(r["dirty_px"]) > 0]
        if sent:
            values = [float(r["est_frame_ms"]) for r in sent]
            print(f"{mode} {section}: p50={statistics.median(values):.3f} "
                  f"max={max(values):.3f}, late={sum(int(r['late']) for r in sent)}/{len(sent)}")
