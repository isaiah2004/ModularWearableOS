"""Settings cache regression and reproducible sim PNGs; no device IO.

Build with sim/nos_build.ps1 -Strict, then run this script. Pass --before EXE
to also capture a saved baseline and make the 2-column, 4-row contact sheet.
"""
import argparse
import csv
import json
import re
import shutil
import statistics
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/settings_blur"
SIM = ROOT / "build/nos_sim.exe"
TIMING_FAILURES = []


def native_checks(cc):
    flags = ["-O2", "-std=c99", "-DNOS_PSRAM_TRACKING", "-DGFX_COST_MODEL",
             "-Icore", "-Inos", "-Inos/fonts", "-Isim"]
    main_obj = OUT / "sim_test_main.o"
    subprocess.run([cc, *flags, "-Dmain=settings_sim_main", "-c", "sim/nos_sim.c",
                    "-o", str(main_obj)], cwd=ROOT, check=True, timeout=60)
    objects = [p for p in (ROOT / "build/nos_obj").glob("*.o")
               if p.name not in ("sim_nos_sim.o", "nos_apps_set_settings.o", "nos_settings_shutter.o", "nos_native_menu_assets.o")]
    for test in ("settings_cache_test", "settings_incremental_test", "settings_shutter_test"):
        exe = OUT / (test + ".exe")
        subprocess.run([cc, *flags, "tests/" + test + ".c", "nos/settings_shutter.c", "nos/native_menu_assets.c", str(main_obj),
                        *map(str, objects), "-o", str(exe), "-lgdi32", "-luser32",
                        "-lwinmm", "-lm"], cwd=ROOT, check=True, timeout=60)
        run(test, "open! settings\nwait 500", exe)



def run(name, script, exe=None):
    exe = exe or SIM
    path = OUT / f"{name}.txt"
    path.write_text(script + "\n")
    proc = subprocess.run([str(exe), "--script", str(path), "--out", str(OUT / name)],
                          cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    text = proc.stdout.decode(errors="replace")
    (OUT / f"{name}.log").write_text(text)
    if proc.returncode:
        failures = re.findall(r"EXPECT FAIL ([^\r\n]+)", text)
        # Keep timing failures fatal, but finish pixel/ownership checks and visual
        # captures so a model deadline failure does not hide correctness evidence.
        if failures and all(f.startswith("nolate:") for f in failures):
            TIMING_FAILURES.extend({"run": name, "failure": f} for f in failures)
        else:
            raise AssertionError(f"{name}: see {OUT / (name + '.log')}")
    return text


CAPTURE = """set mblur 2
motion on
wait 5000
open settings
wait 2500
shot rest_top
mark fling
film! fling 70 10 2
flick 233 420 233 40 120
wait 2400
home_now
open! settings
wait 500
mark middle
""" + ("drag 233 400 233 50 500\nwait 300\n" * 4) + """wait 200
shot rest_middle
mark bottom
flick 233 420 233 40 120
wait 3000
flick 233 420 233 40 120
wait 3000
shot rest_bottom
mem
"""

# Full-residency assertions explicitly wait for bounded background work. Cold
# interaction is tested separately below and must never drain work synchronously.
INVALIDATION = """set mblur 2
time 10:10:00
open! settings
wait 30000
mark stable
mem
wait 200
mem
tap 370 381
wait 2000
mem
time 10:11:00
wait 2000
mem
shot changed
tap 233 291
wait 2000
mem
shot light
mark fling
flick 233 420 233 40 120
wait 2300
expect nolate
mem
home_now
wait 1000
mem
"""


def captures(name, exe):
    run(name, CAPTURE, exe)
    folder = OUT / name
    with (folder / "motion.csv").open() as f:
        motion = [r for r in csv.DictReader(f) if r["section"] == "fling"
                  and r["name"] == "list.SETTINGS"]
    first = int(motion[0]["frame"])
    with (folder / "frames.csv").open() as f:
        costs = {int(r["frame"]): r for r in csv.DictReader(f)}
    chosen = {}
    for length, lo, hi in ((4, 3, 8), (16, 8, 32), (48, 32, 1000)):
        frames = [r for r in motion if r["class"] == "momentum"
                  and lo <= abs(float(r["step"])) < hi]
        r = frames[len(frames) // 2]
        frame = int(r["frame"])
        shutil.copyfile(folder / f"fling_{frame - first:03d}.png", folder / f"blur_{length}.png")
        chosen[length] = {"frame": frame, "step_px_per_normalized_frame": float(r["step"]),
                          "exposure_estimate_from_rounded_audit_px": min(48.0, abs(float(r["step"])) * 0.5)}
    fast = [costs[int(r["frame"])] for r in motion if abs(float(r["step"])) >= 12]
    metrics = {key: round(statistics.median(float(r[key]) for r in fast), 3)
               for key in ("est_raster_ms", "ms_blit", "est_frame_ms")}
    (folder / "selected.txt").write_text(f"length: (frame, step) = {chosen}\nfast-frame medians = {metrics}\n")
    return metrics


def sheet():
    from PIL import Image, ImageDraw, ImageFont
    font = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 22)
    small = ImageFont.truetype("C:/Windows/Fonts/arial.ttf", 16)
    canvas = Image.new("RGB", (964, 2056), "#202124")
    draw = ImageDraw.Draw(canvas)
    for col, name in enumerate(("before", "after")):
        for row, (file, label) in enumerate((("rest_top", "Rest: top"),
                                            ("rest_middle", "Rest: middle"),
                                            ("rest_bottom", "Rest: bottom"),
                                            ("blur_16", "Fling: medium exposure"))):
            x, y = 8 + col * 482, row * 514
            draw.text((x + 8, y + 6), f"{name.upper()}  |  {label}", font=font, fill="white")
            canvas.paste(Image.open(OUT / name / f"{file}.png"), (x, y + 38))
    canvas.save(OUT / "sheet.png")
    levels = Image.new("RGB", (964, 1542), "#202124")
    draw = ImageDraw.Draw(levels)
    for col, name in enumerate(("before", "after")):
        for row, length in enumerate((4, 16, 48)):
            x, y = 8 + col * 482, row * 514
            draw.text((x + 8, y + 8), f"{name.upper()}  |  {length} px", font=small, fill="white")
            levels.paste(Image.open(OUT / name / f"blur_{length}.png"), (x, y + 38))
    levels.save(OUT / "blur_levels.png")


def check_cold_fallback(output, require_temporal=True):
    """Map each audited fast list frame to its compositor work, not aggregate blends."""
    frames = {r["frame"]: r for r in csv.DictReader((output / "frames.csv").open())}
    fast = [r for r in csv.DictReader((output / "motion.csv").open())
            if r["name"] == "list.SETTINGS" and abs(float(r["step"])) >= 12
            and frames[r["frame"]]["screen"] == "SETTINGS"]
    assert fast, "No audited fast Settings frames"
    for moving in fast:
        r = frames[moving["frame"]]
        # Compositor partitions the painted region into directional-copy and
        # temporal spans. The history counter includes both; all pixels must pass.
        assert int(r["dirty_px"]) > 0 and int(r["history_pixels"]) == int(r["dirty_px"]), r
        if require_temporal:
            assert int(r["history_blended"]) > 0, f"Fast live frame has no temporal exposure: {r}"
    return len(fast)


def main():
    global SIM, OUT
    if sys.platform == "win32":
        import ctypes
        ctypes.windll.kernel32.SetErrorMode(0x0001 | 0x0002)
        ctypes.CDLL("ucrtbase.dll")._set_abort_behavior(0, 0x1 | 0x2)
    parser = argparse.ArgumentParser()
    parser.add_argument("--before", type=Path)
    parser.add_argument("--sim", type=Path, default=SIM)
    parser.add_argument("--out", type=Path, default=OUT)
    parser.add_argument("--cc", default=shutil.which("gcc") or "C:/mingw64/bin/gcc.exe")
    args = parser.parse_args()
    SIM = args.sim.resolve(); OUT = args.out.resolve()
    TIMING_FAILURES.clear()
    OUT.mkdir(parents=True, exist_ok=True)
    native_checks(args.cc)
    text = run("invalidation", INVALIDATION)
    bakes = [int(n) for n in re.findall(r"SETTINGS_CACHE .* bakes=(\d+)", text)]
    # ROM owns every static row and both theme rasters from entry. Toggle,
    # clock, theme and scrolling update only mutable overlays or descriptor views.
    assert bakes == [0] * 7, bakes
    control_rasters = [int(n) for n in re.findall(r"SETTINGS_CONTROLS rasters=(\d+)", text)]
    assert len(control_rasters) == len(bakes), control_rasters
    assert control_rasters[0] == control_rasters[1], control_rasters  # idle reuse
    assert control_rasters[2] > control_rasters[1], control_rasters  # animated toggle
    assert control_rasters[3] == control_rasters[2], control_rasters  # offscreen clock needs no capture
    assert text.count("SETTINGS_CACHE entries=36/36") == 7, text
    assert "NOSNATIVE" not in text or "rom=1" in text
    for fail in range(5):
        text = run(f"failure_{fail}", f"set native_rom 0\nmemfail {fail}\nopen! settings\nwait 30000\nmem\nexpect memfailures 1")
        assert "SETTINGS_CACHE entries=36/36" in text
    text = run("failure_rom", "memfail 0\nopen! settings\nwait 30000\nmem\nexpect memfailures 1")
    assert "SETTINGS_CACHE entries=36/36" in text and "bakes=0" in text
    text = run("immediate_raw", "set native_rom 0\nmotion on\nset mblur 2\nopen! settings\nmark cold\nflick 233 420 233 40 120\nwait 2300\nexpect nolate\nmem")
    cold_fast_frames = check_cold_fallback(OUT / "immediate_raw")
    text = run("immediate_rom", "motion on\nset mblur 2\nopen! settings\nmark rom_entry\nflick 233 420 233 40 120\nwait 2300\nexpect nolate\nmem")
    rom_fast_frames = check_cold_fallback(OUT / "immediate_rom", require_temporal=False)
    assert "SETTINGS_CACHE entries=36/36" in text and "bakes=0" in text
    rom_match = re.search(r"BLUR kind=1 moving=\d+ fast=(\d+) blurred=(\d+)", text)
    assert rom_match and int(rom_match[1]) > 0 and rom_match[1] == rom_match[2], rom_match
    # Warm directional coverage retains its original100% requirement independently.
    text = run("warm_fling", "set mblur 2\nopen! settings\nwait 30000\nflick 233 420 233 40 120\nwait 2300\nexpect nolate\nmem")
    match = re.search(r"BLUR kind=1 moving=\d+ fast=(\d+) blurred=(\d+)", text)
    assert match and int(match[1]) > 0 and match[1] == match[2], match
    proc = subprocess.run([str(SIM), "--damagecheck", str(OUT / "invalidation.txt"),
                           "--out", str(OUT / "damage")], cwd=ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, timeout=60)
    (OUT / "damage.log").write_bytes(proc.stdout)
    assert proc.returncode == 0, "damagecheck failed"
    metrics = {"after": captures("after", SIM)}
    if args.before:
        metrics["before"] = captures("before", args.before.resolve())
        sheet()
    report = {"static_bakes": bakes, "control_rasters": control_rasters,
              "raw_cold_fast_frames_with_actual_temporal_blends": cold_fast_frames,
              "rom_immediate_fast_frames_all_processed": rom_fast_frames,
              "rom_immediate_native_coverage": [int(rom_match[1]), int(rom_match[2])],
              "warm_fast_frames": int(match[1]), "warm_blurred_frames": int(match[2]),
              "model_timing_failures": TIMING_FAILURES, "host_cost_model_metrics": metrics,
              "captures": str(OUT / "after"), "damagecheck": "PASS"}
    (OUT / "integration_report.json").write_text(json.dumps(report, indent=2) + "\n")
    print("PASS correctness: scalar blur/crop, armed fling, release, static/live invalidation, allocation recovery, immediate fallback, damagecheck")
    print(f"Cold fallback: {cold_fast_frames} fast frames each fully processed with actual temporal blends")
    print(metrics)
    assert not TIMING_FAILURES, f"Modeled deadline assertions failed: {TIMING_FAILURES}; see integration_report.json"


if __name__ == "__main__":
    main()
