"""Deterministic Sports acceptance checks; requires the current NOS simulator.

No watch, serial port, or flash tool is used. Every invocation retains scripts,
logs, CSV estimates and screenshots under a fresh output directory. Estimates
and synthetic sensors validate software behavior, not device timing or accuracy.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests"
CASES = (
    "background", "indoor", "treadmill", "gps_gaps", "gps_adversarial",
    "stationary", "hr_gates", "save_retry", "clock_changes", "lap_capacity", "missing_sensors", "gps_geometry",
    "display_off", "boot_no_card", "ui", "ui_lists", "ui_profiles", "ui_no_card", "ui_save_retry",
    "ui_recording_chip", "ui_units", "ui_setup", "ui_priority", "ui_catalog", "profile_names", "profile_gps",
    "saved_restart", "interrupted_restart", "corrupt_storage", "history_capacity", "blur_fallback",
)
RECORD = re.compile(r"^(?:\[f\d+\]\s+|\d+\s+)?WORKOUT\s+(\S+)\s+(\{.*\})\s*$")


def require(condition: bool, message: str):
    if not condition:
        raise AssertionError(message)


def finite_values(value, path="record"):
    if isinstance(value, dict):
        for key, item in value.items():
            finite_values(item, f"{path}.{key}")
    elif isinstance(value, list):
        for index, item in enumerate(value):
            finite_values(item, f"{path}[{index}]")
    elif isinstance(value, float):
        require(math.isfinite(value), f"nonfinite {path}: {value}")


def unchanged(before, after, fields):
    for key in fields:
        require(before[key] == after[key],
                f"{key} changed: {before[key]} -> {after[key]}")


def check_case(name, records):
    for record in records.values():
        finite_values(record)
        require(record["elapsed"] <= record["total"],
                f"active duration exceeds total duration: {record}")
        require(0 <= record["motion_ms"] <= record["elapsed"], "motion coverage exceeds active time")
        require(0 <= record["movement_ms"] <= record["motion_ms"], "movement exceeds valid coverage")
        require(record["kcal"] >= 0, "negative category energy estimate")
        if not record["mass"] or not record["met"]:
            require(record["kcal"] == 0, "calories invented without explicit mass/category")
        require(0 <= record["nlaps"] <= 20, "unbounded lap count")
        require(len(record["laps"]) == record["nlaps"], "lap count disagrees with array")
    if name == "background":
        unchanged(records["pause_begin"], records["pause_end"],
                  ("elapsed", "distance", "steps", "hr_ms", "gps_ms", "motion_ms", "movement_ms", "bursts"))
        require(records["pause_end"]["total"] - records["pause_begin"]["total"] == 10000,
                "paused wall duration did not advance")
        unchanged(records["finished"], records["finished_later"],
                  ("elapsed", "total", "distance", "steps", "hr_ms", "gps_ms", "nlaps", "motion_ms", "movement_ms", "bursts"))
        require(records["background"]["elapsed"] - records["foreground"]["elapsed"] == 10000,
                "leaving the screen changed the recorder cadence")
        require(records["foreground"]["motion_ms"]>0 and
                9980<=records["background"]["motion_ms"]-records["foreground"]["motion_ms"]<=10020,
                "fresh motion coverage stopped when the screen closed or required raw CSV recording")
    elif name == "gps_gaps":
        unchanged(records["before_gap"], records["stale"], ("distance",))
        unchanged(records["stale"], records["reacquired"], ("distance",))
        require(0 <= records["after_gap"]["distance"] - records["reacquired"]["distance"] <= 5,
                "GPS reacquisition bridged the missing segment")
    elif name == "gps_adversarial":
        require(records["rejected_spike"]["distance"] == 0, "teleport added distance")
        require(records["after_spike"]["distance"] == 0, "rejected point became a baseline")
    elif name == "missing_sensors":
        returned, faulted, cleared = (records[label] for label in
                                     ("modules_returned", "modules_faulted", "faults_cleared"))
        unchanged(returned, faulted, ("distance", "hr_ms"))
        require(0 <= cleared["distance"] - faulted["distance"] <= 7,
                "sensor reconnection joined the missing route")
    elif name == "hr_gates":
        good, invalid, returned = (records[label] for label in
                                  ("hr_good", "hr_invalid", "hr_returned"))
        unchanged(good, invalid, ("hr_ms", "avg_bpm", "peak_bpm"))
        require(good["avg_bpm"] == 140 and good["peak_bpm"] == 140,
                "first valid HR was not recorded")
        require(140 <= returned["avg_bpm"] <= 160 and returned["peak_bpm"] == 160,
                "invalid HR contaminated the session summary")
        require(3500 <= returned["hr_ms"] - invalid["hr_ms"] <= 5500,
                "fresh HR intervals were not bounded by valid coverage")
    elif name == "save_retry":
        failed, saved = records["failed_save"], records["retried_save"]
        unchanged(failed, saved, ("id", "elapsed", "distance", "steps"))
        require(saved["history"] == 1 and saved["storage_dirty"] == 0,
                "successful retry failed to clear dirty state or duplicated history")
    elif name == "lap_capacity":
        record = records["capped_laps"]
        elapsed = [lap["elapsed"] for lap in record["laps"]]
        require(len(elapsed) == 20 and elapsed == sorted(set(elapsed)),
                "splits are not twenty distinct cumulative timestamps")
        require(elapsed[-1] <= record["elapsed"], "split extends beyond session end")
    elif name == "ui_setup":
        racket,cricket=records["racket_configured"],records["cricket_configured"]
        require((racket["wrist"],racket["playing_hand"],racket["bowling"],racket["autoresume"])==(2,2,0,0),
                "racket wrist/playing hand setup was conflated")
        require((cricket["wrist"],cricket["playing_hand"],cricket["bowling"],cricket["autoresume"])==(2,1,2,1),
                "cricket batting/bowling/wrist setup was conflated")
        require(racket["mass"]==cricket["mass"]==71 and racket["kcal"]>0 and cricket["kcal"]>0,
                "explicit committed mass was not frozen into category energy estimates")
    elif name == "ui_catalog":
        require({int(record["sport"]) for record in records.values()}==set(range(40)),
                "real catalog navigation did not start all forty stable profiles")
    elif name == "ui_priority":
        require({int(record["sport"]) for record in records.values()}=={6,7,8,1,3},
                "priority tiles selected incorrect profiles")
    elif name == "ui_units":
        unchanged(records["metric_live"], records["imperial_live"],
                  ("distance", "speed", "avg_speed", "sport", "id"))
        unchanged(records["imperial_summary"], records["metric_summary"],
                  ("elapsed", "total", "distance", "speed", "avg_speed", "sport", "id", "laps"))


class Suite:
    def __init__(self, sim: Path, output: Path, screenshots: bool):
        self.sim = sim.resolve()
        self.output = output.resolve()
        self.screenshots = screenshots
        self.results = []

    def run(self, name, script, *, store=None, damage=False, no_card=False):
        destination = self.output / name
        destination.mkdir(parents=True, exist_ok=True)
        source = destination / "script.txt"
        source.write_text(script, encoding="utf-8")
        command = [str(self.sim), "--damagecheck" if damage else "--script",
                   str(source), "--out", str(destination), "--motion", "--quiet"]
        if not damage:
            command += ["--hashes", str(destination / "hashes.txt")]
        if not self.screenshots or damage:
            command.append("--no-media")
        if store is not None:
            command += ["--workout-store", str(store)]
        if no_card:
            command.append("--no-sd")
        completed = subprocess.run(command, cwd=ROOT, capture_output=True,
                                   text=True, encoding="utf-8", errors="replace", timeout=600 if name.startswith("ui_catalog") else 180)
        (destination / "stdout.txt").write_text(completed.stdout, encoding="utf-8")
        (destination / "stderr.txt").write_text(completed.stderr, encoding="utf-8")
        require(completed.returncode == 0,
                f"{name} exited {completed.returncode}; see {destination / 'stdout.txt'}\n"
                f"{completed.stdout[-2000:]}\n{completed.stderr[-1000:]}")
        if damage:
            require("PASS damagecheck" in completed.stdout,
                    "damage check returned success without its pass marker")
            return {}
        records = {}
        log = destination / "log.txt"
        logged = log.read_text(encoding="utf-8") if log.is_file() else completed.stdout
        for line in logged.splitlines():
            match = RECORD.fullmatch(line)
            if match:
                require(match[1] not in records, f"duplicate snapshot label {match[1]}")
                records[match[1]] = json.loads(match[2])
        require(records, f"no machine-readable workout snapshots from {name}")
        frames = destination / "frames.csv"
        require(frames.is_file(), f"missing frames.csv for {name}")
        with frames.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        require(rows, f"empty frames.csv for {name}")
        late = sum(int(row["late"]) for row in rows)
        peak = max(float(row["est_frame_ms"]) for row in rows)
        floor_misses = sum(float(row["est_frame_ms"]) > 1000 / 30 for row in rows)
        audit = destination / "motion.txt"
        motion_text = audit.read_text(encoding="utf-8") if audit.is_file() else ""
        motion_summary = re.search(r"(\d+) moves \((\d+) judged\), (\d+) FAIL", motion_text)
        motion_failures = int(motion_summary[3]) if motion_summary else None
        self.results.append({"name": name, "frames": len(rows), "estimated_late": late,
                             "estimated_max_ms": peak, "floor_30fps_misses": floor_misses,
                             "motion_failures": motion_failures,
                             "records": records})
        # Performance acceptance is enforced on the Sports UI workload. Other
        # cases intentionally navigate unrelated OS screens during long waits.
        if name.startswith("ui"):
            require(floor_misses == 0,
                    f"Sports UI exceeded the 30 FPS model budget: {floor_misses} misses, {peak} ms")
            require(motion_failures == 0,
                    f"Sports UI motion audit has {motion_failures} failures; see {audit}")
        return records

    def fixture(self, name):
        records = self.run(name, (FIXTURES / f"workout_{name}.txt").read_text(),
                           no_card=name in ("boot_no_card", "ui_no_card"))
        check_case(name, records)

    def profile_gps(self):
        script="wait 100\nset step_rate 0\n"
        for sport,speed in ((6,8),(15,15),(16,15)):
            script+=(f"set gps_motion {speed}\nworkout start {sport}\nwait 10000\n"
                     f"expect workout sport {sport}\nexpect workout gps_status 2\n"
                     f"expect workout speed_valid 1\nexpect workout speed {speed-.1} {speed+.1}\n"
                     f"workout info gps_profile_{sport}\n"
                     "workout finish\nwait 200\nworkout discard\nwait 200\n")
        records=self.run("profile_gps",script)
        for sport,speed in ((6,8),(15,15),(16,15)):
            record=records[f"gps_profile_{sport}"]
            # Initial GNSS acquisition has no route credit. Compare distance
            # with validated coverage, rather than assuming an immediate fix.
            expected=record["gps_ms"]*.001*speed
            require(record["gps_ms"]>=5000 and abs(record["distance"]-expected)<max(.5,expected*.002),
                    f"GPS gate rejected/miscredited reasonable speed for profile {sport}")

    def saved_restart(self):
        store = self.output / "saved-restart-store"
        first = self.run("saved_restart_before", """wait 100
present 0
set step_rate 0
workout start 5
wait 5000
workout lap
wait 2000
workout finish
wait 300
expect workout save 2
workout info saved
""", store=store)["saved"]
        second = self.run("saved_restart_after", """wait 200
expect workout history 1
workout history 0 reloaded
""", store=store)["reloaded"]
        unchanged(first, second, ("id", "sport", "elapsed", "total", "distance", "steps",
                                  "nlaps", "laps", "started", "ended"))

    def interrupted_restart(self):
        store = self.output / "interrupted-restart-store"
        first = self.run("interrupted_restart_before", """wait 100
present 0
set step_rate 120
workout start 4
wait 35000
workout service
workout info interrupted
""", store=store)["interrupted"]
        second = self.run("interrupted_restart_after", """wait 200
present 0
set step_rate 120
expect workout state 1
expect workout recovered 1
workout info recovered
wait 5000
workout info recovery_wait
wait 3000
workout finish
wait 300
expect workout save 2
expect workout history 1
workout info recovered_finished
""", store=store)
        restored = second["recovered"]
        require(0 < restored["elapsed"] <= first["elapsed"],
                "reboot invented active time or lost the entire checkpoint")
        require(first["elapsed"] - restored["elapsed"] <= 10000,
                "checkpoint lost more than ten seconds of active time")
        require(4900 <= second["recovery_wait"]["elapsed"]-restored["elapsed"] <= 5100,
                "automatic recovery failed to continue from current boot time")
        require(7900 <= second["recovered_finished"]["elapsed"] - restored["elapsed"] <= 8100,
                "resume credited reboot downtime")

        paused_store=self.output/"interrupted-paused-store"
        paused_first=self.run("interrupted_paused_before", """wait 100
present 0
set step_rate 0
open workout
wait 700
tap 154 244
wait 700
tap 233 307
wait 700
slide 233 380 233 146 700 100
wait 500
tap 233 233
wait 150
expect setup autoresume 0
pop
wait 700
tap 233 372
wait 35000
expect workout autoresume 0
workout service
workout info paused_interrupted
""",store=paused_store)["paused_interrupted"]
        paused=self.run("interrupted_paused_after", """wait 200
present 0
set step_rate 0
expect workout state 2
expect workout recovered 1
expect workout autoresume 0
workout info paused_recovered
wait 5000
workout info paused_wait
workout resume
wait 3000
workout finish
wait 300
expect workout save 2
workout info paused_finished
""",store=paused_store)
        recovered=paused["paused_recovered"]
        require(0<=paused_first["elapsed"]-recovered["elapsed"]<=10000,"paused recovery lost checkpoint")
        unchanged(recovered,paused["paused_wait"],("elapsed","steps","distance","hr_ms","motion_ms","movement_ms"))
        require(2900<=paused["paused_finished"]["elapsed"]-recovered["elapsed"]<=3100,
                "manual resume credited reboot or paused time")

    def corrupt_storage(self):
        store = self.output / "corrupt-store"
        seeded = self.run("corrupt_seed", """wait 100
present 0
set step_rate 0
workout start 5
wait 2000
workout finish
wait 300
workout discard
wait 300
workout start 4
wait 3000
workout finish
wait 300
workout history 0 newest_saved
workout discard
wait 300
workout info seeded
""", store=store)["newest_saved"]
        banks = sorted(store.glob("workouts.*.bin"))
        require(len(banks) == 2, "alternating workout checkpoint did not retain two banks")
        original = {bank: bank.read_bytes() for bank in banks}
        require(all(len(blob) > 104 for blob in original.values()), "truncated checkpoint fixture")
        latest = max(banks, key=lambda bank: struct.unpack_from("<I", original[bank], 12)[0])
        backup = self.output / "corrupt-original-banks"
        backup.mkdir()
        for bank, blob in original.items():
            (backup / bank.name).write_bytes(blob)
        damaged = bytearray(original[latest])
        damaged[100] ^= 0xA5
        latest.write_bytes(damaged)
        fallback = self.run("corrupt_one_bank", """wait 200
expect workout storage_ready 1
expect workout history 2
workout history 0 fallback
""", store=store)["fallback"]
        unchanged(seeded, fallback, ("id", "sport", "elapsed", "total", "distance", "nlaps", "laps"))
        for bank in banks:
            damaged = bytearray(original[bank])
            damaged[100] ^= 0xA5
            bank.write_bytes(damaged)
        before = {bank: bank.read_bytes() for bank in banks}
        blocked = self.run("corrupt_both_banks", """wait 200
expect workout storage_ready 0
expect workout storage_available 0
workout start 5 0
wait 5500
expect workout state 0
expect workout history 0
workout info corrupted
""", store=store)["corrupted"]
        require(blocked["storage_errors"] > 0, "corrupt history did not report a storage error")
        require(before == {bank: bank.read_bytes() for bank in banks},
                "both corrupted banks were silently overwritten")

    def history_capacity(self):
        store = self.output / "history-capacity-store"
        script = "wait 100\npresent 0\nset step_rate 0\n"
        for _ in range(13):
            script += ("workout start 5\nwait 1000\nworkout finish\nwait 100\n"
                       "workout discard\nwait 100\n")
        script += """expect workout history 12
workout history 0 before_newest
workout history 11 before_oldest
workout start 5
wait 1000
set sd 0
workout finish
wait 100
expect workout save 3
workout discard
expect workout history 12
set sd 1
workout retry
wait 300
workout history 0 after_newest
workout history 11 after_oldest
"""
        records = self.run("history_capacity_before", script, store=store)
        for offset in ("newest", "oldest"):
            unchanged(records[f"before_{offset}"], records[f"after_{offset}"],
                      ("id", "sport", "elapsed", "distance", "nlaps", "laps"))
        reloaded = self.run("history_capacity_after", """wait 200
expect workout history 12
workout history 0 reloaded_newest
workout history 11 reloaded_oldest
""", store=store)
        for offset in ("newest", "oldest"):
            unchanged(records[f"before_{offset}"], reloaded[f"reloaded_{offset}"],
                      ("id", "sport", "elapsed", "distance", "nlaps", "laps"))

    def blur_fallback(self):
        # Disabling only the optional directional cache deliberately exercises
        # the compositor's temporal fallback over moving live list content.
        script = """wait 100
present 0
set step_rate 0
workout start 5
wait 1000
workout finish
wait 200
workout discard
open workout
wait 700
tap 233 420
wait 700
tap 233 155
wait 700
expect top SESSION DETAILS
set mblur 0
set temporal_blur {enabled}
wait 500
mark cold_scroll
film! fallback_motion 28 7 2
slide 233 380 233 200 700 100
wait 700
workout info fallback
"""
        for enabled in (0, 1):
            self.run(f"blur_fallback_{enabled}", script.format(enabled=enabled))
        directories = [self.output / f"blur_fallback_{enabled}" for enabled in (0, 1)]
        tables = []
        hashes = []
        for directory in directories:
            with (directory / "frames.csv").open(newline="", encoding="utf-8") as handle:
                tables.append({int(row["frame"]): row for row in csv.DictReader(handle)})
            hashes.append(dict(line.split() for line in (directory / "hashes.txt").read_text().splitlines()))
        with (directories[1] / "motion.csv").open(newline="", encoding="utf-8") as handle:
            positions = [row for row in csv.DictReader(handle)
                         if row["section"] == "cold_scroll" and row["name"] == "list.SESSION DETAILS"]
        moving = []
        previous = None
        for row in positions:
            if previous is not None and row["px"] != previous:
                moving.append(int(row["frame"]))
            previous = row["px"]
        require(len(moving) >= 20, f"insufficient moving fallback observations: {len(moving)}")
        for frame in moving:
            require(int(tables[1][frame]["history_blended"]) > 0,
                    f"moving frame {frame} did not receive temporal fallback")
            require(int(tables[0][frame]["history_blended"]) == 0,
                    f"disabled fallback still blended pixels at frame {frame}")
            require(hashes[1][str(frame)] != hashes[0][str(frame)],
                    f"fallback produced no visible pixel difference at frame {frame}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sim", type=Path, default=ROOT / "build/nos_sports.exe")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--case", choices=CASES, action="append", dest="cases")
    parser.add_argument("--skip-damage", action="store_true")
    parser.add_argument("--no-media", action="store_true")
    args = parser.parse_args()
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = args.out or ROOT / "build/workout-validation" / stamp
    output.mkdir(parents=True, exist_ok=False)
    require(args.sim.is_file(), f"simulator not found: {args.sim}")
    suite = Suite(args.sim, output, screenshots=not args.no_media)
    manifest = {"simulator": str(args.sim.resolve()),
                "simulator_sha256": hashlib.sha256(args.sim.read_bytes()).hexdigest(),
                "started_utc": stamp, "cases": args.cases or CASES,
                "scope": "Simulator only; no watch or firmware flash."}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    failures = []
    for name in args.cases or CASES:
        try:
            if name in ("saved_restart", "interrupted_restart", "corrupt_storage",
                        "history_capacity", "blur_fallback", "profile_gps"):
                getattr(suite, name)()
            else:
                suite.fixture(name)
            if name.startswith("ui") and not args.skip_damage:
                suite.run(f"{name}_damage", (FIXTURES / f"workout_{name}.txt").read_text(),
                          damage=True, no_card=name == "ui_no_card")
            print(f"PASS {name}", flush=True)
        except (AssertionError, OSError, subprocess.TimeoutExpired, json.JSONDecodeError) as error:
            failures.append({"name": name, "error": str(error)})
            print(f"FAIL {name}: {error}", flush=True)
    report = {"manifest": manifest, "results": suite.results, "failures": failures}
    (output / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
    print(f"{len((args.cases or CASES)) - len(failures)}/{len(args.cases or CASES)} cases passed; evidence {output}")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
