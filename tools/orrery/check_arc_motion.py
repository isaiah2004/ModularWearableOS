"""Build HEAD's arc baseline read-only, fit its missing PSRAM scan cost, and
compare against the current simulator. No firmware/device access. Run after
sim/nos_build.ps1 -Strict -Out nos_sim_arc_after.exe (and no-cost equivalent).
All source snapshots, executables, logs and PNGs live under build/arc_motion.
"""
from pathlib import Path
from concurrent.futures import ThreadPoolExecutor
import csv
import ctypes
import hashlib
import io
import json
import os
import shutil
import statistics
import subprocess
import tarfile

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
OUT = ROOT / "build/arc_motion"
OUT.mkdir(parents=True, exist_ok=True)
if os.name == "nt":
    ctypes.windll.kernel32.SetErrorMode(0x8003)  # inherited, no OS crash dialogs
CC = shutil.which("gcc") or "C:/mingw64/bin/gcc.exe"
FLAGS = ["-O2", "-std=c99", "-DGFX_COST_MODEL", "-DNOS_PSRAM_TRACKING"]
LIBS = ["-lgdi32", "-luser32", "-lwinmm", "-lm"]


def command(args, log):
    p = subprocess.run(list(map(str, args)), capture_output=True, timeout=180)
    log.write_bytes(p.stdout + p.stderr)
    if p.returncode:
        raise RuntimeError(f"exit {p.returncode}: {log}")
    return p.stdout


def baseline():
    dest = OUT / "baseline"
    data = subprocess.check_output(
        ["git", "archive", "98330d1", "core", "nos", "sim", "firmware/main"], stderr=subprocess.PIPE)
    with tarfile.open(fileobj=io.BytesIO(data)) as archive:
        for member in archive.getmembers():
            if not member.isfile() or Path(member.name).suffix not in (".c", ".h", ".inc"):
                continue
            p = (dest / member.name).resolve()
            assert p.is_relative_to(dest.resolve())
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(archive.extractfile(member).read())
    core = "gfx font sprite anim notif battery pedometer walk motion_wake wake_service knock ephem".split()
    sources = [*sorted((dest / "nos").rglob("*.c")),
               *(dest / "core" / f"{n}.c" for n in core),
               *sorted((dest / "core/fonts").glob("*.c")),
               *sorted((dest / "sim").glob("nos_*.c"))]
    includes = [f"-I{dest / p}" for p in ["core", "nos", "nos/fonts", "sim"]]

    def compile_one(src):
        obj = src.with_suffix(".o")
        command([CC, *FLAGS, *includes, "-c", src, "-o", obj], src.with_suffix(".log"))
        return obj

    with ThreadPoolExecutor(max_workers=8) as pool:
        objects = list(pool.map(compile_one, sources))
    command([CC, *objects, "-o", OUT / "before.exe", *LIBS], OUT / "baseline-link.log")
    src = dest / "nos/ui_arc.c"
    original = src.read_bytes()
    # Aggregate fitted cost per visited 1.1KB-strided PSRAM record, including
    # scan/compare/LRU bookkeeping. NOT an isolated PSRAM latency measurement.
    # Actual visit counts (early exits included), never 384 assumed per hit.
    src.write_bytes(original.replace(b"arc_mask *m=cache+i;",
                    b"arc_mask *m=cache+i; GFX_COST_PX(GFX_COST_ORR_PRE_CPU,1400);"))
    compile_one(src)
    command([CC, *objects, "-o", OUT / "priced.exe", *LIBS], OUT / "priced-link.log")
    src.write_bytes(original)
    renames = ["-Dui_text_arc_begin=legacy_begin", "-Dui_text_arc_prepare=legacy_prepare",
               "-Dui_text_arc=legacy_draw", "-Dui_text_arc_drop=legacy_drop",
               "-Dui_text_arc_stats=legacy_stats"]
    command([CC, *FLAGS, *includes, *renames, "-c", src, "-o", OUT / "legacy.o"], OUT / "legacy-build.log")
    print("Built untouched 98330d1 and cost-only calibrated baseline", flush=True)


def unit():
    includes = ["-Icore", "-Inos", "-Inos/fonts", "-Isim"]
    command([CC, *FLAGS, *includes, "-Dmain=sim_program_main", "-c", "sim/nos_sim.c",
             "-o", OUT / "sim_test.o"], OUT / "sim-test-build.log")
    objects = [p for p in (ROOT / "build/nos_obj").glob("*.o")
               if p.name not in ("sim_nos_sim.o", "nos_ui_arc.o")]
    command([CC, *FLAGS, *includes, "tests/ui_arc_motion_test.c", *objects,
             OUT / "sim_test.o", OUT / "legacy.o", "-o", OUT / "test.exe", *LIBS], OUT / "test-build.log")
    boot = OUT / "boot.txt"
    boot.write_text("face 8\nwait 100\n")
    result = command([OUT / "test.exe", "--script", boot, "--out", OUT / "unit",
                      "--quiet", "--no-media"], OUT / "unit.log")
    print(result.decode(errors="replace")[-1200:], flush=True)


def compare():
    executables = {"before": OUT / "before.exe", "priced": OUT / "priced.exe",
                   "after": ROOT / "build/nos_sim_arc_after.exe"}
    no_cost = ROOT / "build/nos_sim_arc_after_nocost.exe"
    if no_cost.exists():
        executables["nocost"] = no_cost
    summaries = {}
    for name, exe in executables.items():
        command([exe, "--script", "tools/orrery/arc_motion.txt", "--out", OUT / name,
                 "--hashes", OUT / f"{name}.hash", "--quiet"], OUT / f"{name}.log")
        rows = list(csv.DictReader((OUT / name / "frames.csv").open()))
        sections = {}
        for section in dict.fromkeys(r["section"] for r in rows):
            group = [r for r in rows if r["section"] == section]
            values = sorted(float(r["est_frame_ms"]) for r in group if int(r["dirty_px"]))
            sections[section] = dict(frames=len(group), sent=len(values),
                mean_ms=statistics.mean(values) if values else 0,
                p95_ms=values[int(.95*(len(values)-1))] if values else 0,
                max_ms=max(values, default=0), late=sum(int(r["late"]) for r in group))
        summaries[name] = sections
    hashes = {}
    for suffix in ["now", "travelled", "returned", "tilt"]:
        filename = f"style5_{suffix}.png"
        hashes[suffix] = {n: hashlib.sha256((OUT / n / filename).read_bytes()).hexdigest()
                          for n in executables}
        assert len(set(hashes[suffix].values())) == 1, f"rest capture differs: {suffix}"
    assert (OUT / "before.hash").read_bytes() == (OUT / "priced.hash").read_bytes()
    if "nocost" in executables:
        assert (OUT / "after.hash").read_bytes() == (OUT / "nocost.hash").read_bytes()
    # Forced-full comparison deliberately renders during idle intervals. Keep
    # idle/nolate assertions in the ordinary run, not in that forced mode.
    damage = OUT / "damage.txt"
    damage.write_text("\n".join(line for line in (ROOT / "tools/orrery/arc_motion.txt").read_text().splitlines()
                                if not line.startswith("expect ")) + "\n")
    command([executables["after"], "--damagecheck", damage, "--out", OUT / "damage"], OUT / "damage.log")
    (OUT / "results.json").write_text(json.dumps(dict(timing=summaries, rest_png_sha256=hashes), indent=2))
    for name in ["before", "priced", "after"]:
        print(name, {s: summaries[name][s] for s in ["open", "drag", "coast", "return_now", "open_rest"]})
    print("PASS all settled PNG SHA-256 hashes match 98330d1; cost hooks preserve pixels")


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-baseline", action="store_true")
    parser.add_argument("--skip-unit", action="store_true")
    args = parser.parse_args()
    if not args.skip_baseline:
        baseline()
    if not args.skip_unit:
        unit()
    compare()
