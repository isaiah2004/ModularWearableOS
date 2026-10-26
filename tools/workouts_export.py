"""Export verified Sports v1-v4 checkpoint banks to JSON or summary/lap CSV.

Reads only: the source files are never repaired or changed. A missing/corrupt
newest bank falls back to the last valid bank with a warning. Output files must
not already exist. See nos/workout.h for the frozen little-endian v1 layout.
"""
from __future__ import annotations

import argparse
import csv
from datetime import datetime, timezone
import io
import json
import math
from pathlib import Path
import struct
import sys

MAGIC = 0x574F5331
VERSION = 1
SNAPSHOT_BYTES = 236
RECORD_BYTES = 3104
CHECKSUM_OFFSET = 3100
HISTORY_OFFSET = 268
HISTORY_MAX = 12
LAPS_MAX = 20
MAX_MS = 30 * 24 * 60 * 60 * 1000
SPORTS = ("Walk", "Run", "Cycle", "Hike", "Treadmill", "Workout", "Football", "Cricket",
          "Badminton", "Trail run", "Trekking", "Race walk", "Nordic walk", "Indoor walk",
          "Stairs", "Mountain bike", "Gravel bike", "Indoor cycle", "Tennis", "Table tennis",
          "Squash", "Pickleball", "Padel", "Basketball", "Volleyball", "Handball",
          "Field hockey", "Rugby", "Strength", "HIIT", "Circuit", "Aerobics", "Dance",
          "Jump rope", "Boxing fitness", "Yoga", "Pilates", "Stretching", "Indoor rowing", "Kayak")
OUTDOOR = {0, 1, 2, 3, 6, 7, 9, 10, 11, 12, 15, 16, 26, 27, 39}
STEPS = {0, 1, 3, 4, 9, 10, 11, 12, 13, 14}
V2_SNAPSHOT_BYTES, V2_RECORD_BYTES, V2_CHECKSUM_OFFSET = 264, 3472, 3464
V3_SNAPSHOT_BYTES, V3_RECORD_BYTES, V3_CHECKSUM_OFFSET = 328, 4304, 4296
V4_SNAPSHOT_BYTES, V4_RECORD_BYTES, V4_CHECKSUM_OFFSET = 584, 7632, 7624
SERIES_MAX = 120
BASIS_FLAGS = ((1, "heart_rate"), (2, "movement_met"), (4, "category_met"), (8, "assumed_mass"), (16, "assumed_body"))
QUALITY_REASONS = ("none", "hr_missing", "hr_contact", "hr_quality", "hr_range", "hr_stale",
                   "hr_before_segment", "hr_order", "hr_spike", "gps_missing", "gps_fix",
                   "gps_stale", "gps_quality", "gps_speed", "gps_before_segment", "gps_order",
                   "gps_jump", "motion_invalid", "motion_saturated", "motion_gap")
STATES = ("idle", "running", "paused", "finished")
SAVES = ("none", "pending", "saved", "failed")
PREFIX = struct.Struct("<8I6f3H11B3x")
LAP = struct.Struct("<If")
INTEGER_FIELDS = ("id", "started_epoch", "ended_epoch", "elapsed_ms", "total_ms",
                  "steps", "hr_active_ms", "gps_active_ms")
FLOAT_FIELDS = ("distance_m", "speed_mps", "avg_speed_mps", "pace_s_km",
                "avg_pace_s_km", "cadence_spm")
SHORT_FIELDS = ("bpm", "avg_bpm", "peak_bpm")
BYTE_FIELDS = ("sport", "state", "save_state", "gps_status", "distance_valid",
               "speed_valid", "pace_valid", "hr_valid", "cadence_valid",
               "recovered", "nlaps")
SUMMARY_COLUMNS = ("id", "sport", "state", "saved", "started_utc", "ended_utc",
                   "active_seconds", "observed_seconds", "distance_m",
                   "gps_valid_seconds", "gps_average_kph", "gps_average_pace_s_km",
                   "steps", "hr_valid_seconds", "average_bpm", "peak_bpm", "lap_count", "worn_wrist", "dominant_hand", "playing_hand",
                   "bowling_hand", "placement_source", "placement_revision", "movement_valid_seconds",
                   "movement_seconds_est", "movement_bursts_est", "energy_kcal_est", "energy_net_kcal_est", "energy_basis", "pedometer_steps", "series_bin_seconds", "recovery_count", "category_met", "mass_kg")
LAP_COLUMNS = ("workout_id", "sport", "lap", "active_seconds", "distance_m",
               "split_seconds", "split_distance_m")


class InvalidBank(ValueError):
    pass


def fnv1a(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def require(condition: bool, message: str):
    if not condition:
        raise InvalidBank(message)


def snapshot(data: bytes, offset: int, *, historical: bool = False, version: int = 1) -> dict:
    values = PREFIX.unpack_from(data, offset)
    fields = INTEGER_FIELDS + FLOAT_FIELDS + SHORT_FIELDS + BYTE_FIELDS
    item = dict(zip(fields, values))
    require(item["sport"] < len(SPORTS) and item["state"] < len(STATES)
            and item["save_state"] < len(SAVES) and item["gps_status"] <= 5,
            "unknown workout/profile/status")
    require(all(item[key] in (0, 1) for key in BYTE_FIELDS[4:10]),
            "invalid workout validity flag")
    require(item["nlaps"] <= LAPS_MAX, "too many laps")
    require(0 <= item["elapsed_ms"] <= item["total_ms"] <= MAX_MS,
            "invalid workout durations")
    require(item["hr_active_ms"] <= item["elapsed_ms"]
            and item["gps_active_ms"] <= item["elapsed_ms"],
            "sensor coverage exceeds active time")
    limits = (10_000_000, 35, 35, 36_000, 36_000, 500)
    require(all(math.isfinite(item[key]) and 0 <= item[key] <= limit
                for key, limit in zip(FLOAT_FIELDS, limits)),
            "nonfinite or out-of-range workout metric")
    require(all(item[key] <= 230 for key in SHORT_FIELDS), "invalid heart rate")
    if historical:
        require(item["state"] == 3 and item["id"] != 0
                and item["save_state"] == 2, "invalid saved history entry")
    elif item["state"] != 0:
        require(item["id"] != 0, "session has no identifier")
    laps = []
    previous_ms, previous_m = 0, 0.0
    for index in range(item["nlaps"]):
        elapsed, metres = LAP.unpack_from(data, offset + PREFIX.size + index * LAP.size)
        require(previous_ms <= elapsed <= item["elapsed_ms"]
                and math.isfinite(metres) and previous_m <= metres <= item["distance_m"],
                "invalid cumulative lap")
        laps.append({"elapsed_ms": elapsed, "distance_m": metres})
        previous_ms, previous_m = elapsed, metres
    item["laps"] = laps
    setup_fields = ("worn_wrist", "dominant_hand", "playing_hand", "bowling_hand",
                    "auto_resume", "mass_kg", "placement_source")
    extension_fields = ("motion_valid_ms", "movement_ms", "movement_bursts", "energy_kcal_est", "recovery_count")
    item["met_tenths"] = data[offset + 243] if version >= 2 else 0
    if version >= 2:
        setup = dict(zip(setup_fields, struct.unpack_from("<7B", data, offset + 236)))
        item.update(zip(extension_fields, struct.unpack_from("<3IfI", data, offset + 244)))
        require(setup["worn_wrist"] <= 2 and setup["dominant_hand"] <= 2
                and setup["playing_hand"] <= 3 and setup["bowling_hand"] <= 2
                and setup["auto_resume"] <= 1 and setup["placement_source"] <= 2,
                "invalid placement setup")
        require(item["movement_ms"] <= item["motion_valid_ms"] <= item["elapsed_ms"]
                and math.isfinite(item["energy_kcal_est"]) and item["energy_kcal_est"] >= 0,
                "invalid motion/energy estimate")
    else:
        require(item["sport"] < 6, "unknown v1 profile")
        setup = dict.fromkeys(setup_fields, 0)
        item.update(dict.fromkeys(extension_fields, 0))
    item["setup"] = setup
    item["placement_revision"] = 0
    item["quality"] = None  # older formats did not retain rejection provenance
    if version >= 3:
        item["placement_revision"] = struct.unpack_from("<I", data, offset + 264)[0]
        counters = struct.unpack_from("<20H", data, offset + 268)
        stamps = struct.unpack_from("<4I", data, offset + 308)
        reason = data[offset + 324]
        require(reason < len(QUALITY_REASONS), "invalid quality rejection reason")
        item["quality"] = {"rejections": dict(zip(QUALITY_REASONS[1:], counters[1:])),
                           "last_reason": QUALITY_REASONS[reason],
                           **dict(zip(("hr_sample_ms", "gps_sample_ms", "motion_sample_ms", "last_reject_ms"), stamps))}
    item.update(body={"height_cm": 0, "age_years": 0, "sex": 0}, ped_steps=0, energy_net_kcal=0.0,
                energy_basis=0, series_bin_s=0, series=[])
    if version >= 4:
        height, age, sex = struct.unpack_from("<3B", data, offset + 328)
        ped_steps, net = struct.unpack_from("<If", data, offset + 332)
        basis, count, bin_s = struct.unpack_from("<BBH", data, offset + 340)
        moves = data[offset + 344:offset + 344 + SERIES_MAX]
        bpms = data[offset + 464:offset + 464 + SERIES_MAX]
        require(sex <= 2 and basis < 32 and count <= SERIES_MAX and (count == 0 or bin_s > 0)
                and math.isfinite(net) and 0 <= net <= item["energy_kcal_est"] + 1e-3
                and all(m <= 100 or m == 255 for m in moves[:count]) and all(b <= 230 for b in bpms[:count]),
                "invalid energy/series extension")
        item.update(body={"height_cm": height, "age_years": age, "sex": sex}, ped_steps=ped_steps,
                    energy_net_kcal=net, energy_basis=basis, series_bin_s=bin_s,
                    series=[{"movement_pct": None if m == 255 else m, "bpm": b or None}
                            for m, b in zip(moves[:count], bpms[:count])])
    return item


def read_bank(path: Path) -> dict:
    with path.open("rb") as handle:
        data = handle.read(V4_RECORD_BYTES + 1)
    require(len(data) >= 32, "truncated workout bank")
    magic, version, size, sequence, count, next_id, integral = struct.unpack_from("<6IQ", data)
    require(magic == MAGIC, "wrong workout-bank magic")
    require(version in (1, 2, 3, 4), f"unsupported workout-bank version {version}")
    record_bytes, snapshot_bytes, checksum_offset = {
        1: (RECORD_BYTES, SNAPSHOT_BYTES, CHECKSUM_OFFSET),
        2: (V2_RECORD_BYTES, V2_SNAPSHOT_BYTES, V2_CHECKSUM_OFFSET),
        3: (V3_RECORD_BYTES, V3_SNAPSHOT_BYTES, V3_CHECKSUM_OFFSET),
        4: (V4_RECORD_BYTES, V4_SNAPSHOT_BYTES, V4_CHECKSUM_OFFSET)}[version]
    require(len(data) == record_bytes, f"expected {record_bytes} bytes, got {len(data)}")
    require(size == record_bytes and count <= HISTORY_MAX and next_id != 0,
            "invalid workout-bank header")
    require(struct.unpack_from("<I", data, checksum_offset)[0]
            == fnv1a(data[:checksum_offset]), "workout-bank checksum mismatch")
    current = snapshot(data, 32, version=version)
    require(integral <= 230 * current["hr_active_ms"], "invalid HR integral")
    history = [snapshot(data, 32 + (index + 1) * snapshot_bytes, historical=True, version=version)
               for index in range(count)]
    require(len({entry["id"] for entry in history}) == len(history), "duplicate session identifiers")
    return {"source": str(path), "sequence": sequence, "current": current, "history": history}


def load(source: Path) -> tuple[dict, list[str]]:
    paths = [source / f"workouts.{slot}.bin" for slot in range(2)] if source.is_dir() else [source]
    banks, warnings = [], []
    for path in paths:
        try:
            banks.append(read_bank(path))
        except (OSError, InvalidBank, struct.error) as error:
            warnings.append(f"{path.name}: {error}")
    if not banks:
        raise InvalidBank("no valid workout bank; " + "; ".join(warnings))
    newest = banks[0]
    for bank in banks[1:]:
        delta = (bank["sequence"] - newest["sequence"]) & 0xFFFFFFFF
        if 0 < delta < 0x80000000:
            newest = bank
    return newest, warnings


def utc(epoch: int) -> str | None:
    return datetime.fromtimestamp(epoch, timezone.utc).isoformat().replace("+00:00", "Z") if epoch else None


def summary(item: dict) -> dict:
    outdoor = item["sport"] in OUTDOOR
    return {
        "id": item["id"], "sport": SPORTS[item["sport"]], "state": STATES[item["state"]],
        "saved": item["save_state"] == 2,
        "started_utc": utc(item["started_epoch"]), "ended_utc": utc(item["ended_epoch"]),
        "active_seconds": item["elapsed_ms"] / 1000, "observed_seconds": item["total_ms"] / 1000,
        "distance_m": item["distance_m"] if item["distance_valid"] else None,
        "gps_valid_seconds": item["gps_active_ms"] / 1000,
        "gps_average_kph": item["avg_speed_mps"] * 3.6 if item["gps_active_ms"] else None,
        "gps_average_pace_s_km": item["avg_pace_s_km"] or None,
        "steps": item["steps"] if item["sport"] in STEPS else None,
        "hr_valid_seconds": item["hr_active_ms"] / 1000,
        "average_bpm": item["avg_bpm"] if item["hr_active_ms"] else None,
        "peak_bpm": item["peak_bpm"] or None, "lap_count": item["nlaps"],
        "category_met": item["met_tenths"] / 10 or None,
        "mass_kg": item["setup"]["mass_kg"] or None,
        "setup": item["setup"],
        "placement_revision": item["placement_revision"], "quality": item["quality"],
        **{key: item["setup"][key] for key in ("worn_wrist", "dominant_hand", "playing_hand", "bowling_hand", "placement_source")},
        "movement_valid_seconds": item["motion_valid_ms"] / 1000,
        "movement_seconds_est": item["movement_ms"] / 1000,
        "movement_bursts_est": item["movement_bursts"],
        "energy_kcal_est": item["energy_kcal_est"] or None,
        "energy_net_kcal_est": item["energy_net_kcal"] or None,
        "energy_basis": [name for bit, name in BASIS_FLAGS if item["energy_basis"] & bit],
        "energy_note": "gross session energy; net excludes resting metabolism; assumed_* = default body values were used",
        "body": item["body"], "pedometer_steps": item["ped_steps"],
        "series_bin_seconds": item["series_bin_s"] or None, "series": item["series"],
        "recovery_count": item["recovery_count"],
        "recovered": bool(item["recovered"]), "distance_source": "accepted_gnss" if outdoor else None,
        "average_speed_denominator": "valid_gnss_time" if outdoor else None,
    }


def lap_rows(item: dict) -> list[dict]:
    rows, previous_ms, previous_m = [], 0, 0.0
    for index, lap in enumerate(item["laps"], 1):
        rows.append({
            "workout_id": item["id"], "sport": SPORTS[item["sport"]], "lap": index,
            "active_seconds": lap["elapsed_ms"] / 1000,
            "distance_m": lap["distance_m"] if item["distance_valid"] else None,
            "split_seconds": (lap["elapsed_ms"] - previous_ms) / 1000,
            "split_distance_m": lap["distance_m"] - previous_m if item["distance_valid"] else None,
        })
        previous_ms, previous_m = lap["elapsed_ms"], lap["distance_m"]
    return rows


def csv_text(rows: list[dict], fields: tuple[str, ...]) -> str:
    output = io.StringIO(newline="")
    writer = csv.DictWriter(output, fieldnames=fields, extrasaction="ignore")
    writer.writeheader()
    writer.writerows({key: ";".join(map(str, value)) if isinstance(value, list) else value
                      for key, value in row.items()} for row in rows)
    return output.getvalue()


def export(bank: dict, *, include_checkpoint: bool = False) -> dict:
    items = list(bank["history"])
    current = bank["current"]
    if include_checkpoint and current["state"] in (1, 2):
        items.insert(0, current)
    return {
        "format": "NOS Sports export v1", "source_bank": bank["source"],
        "sequence": bank["sequence"],
        "notes": [
            "GNSS averages use valid GNSS time; missing segments are excluded.",
            "Observed time excludes time powered off after a recovered checkpoint.",
            "Sensor values are engineering estimates; simulator data is synthetic.",
            "The recorder stores summaries and cumulative laps; no route geometry is stored here.",
            "v4 adds a per-bin movement percentage (null = unknown) and mean heart rate series; bins merge and double when a session is long.",
            "Quality counters are engineering gate rejections; sample timestamps are boot-relative and not physiological validation.",
        ],
        "workouts": [{**summary(item), "laps": lap_rows(item)} for item in items],
    }


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="workouts directory or one checkpoint bank")
    parser.add_argument("--out", required=True, type=Path, help="new .json or .csv file")
    parser.add_argument("--laps-out", type=Path, help="optional new lap .csv file")
    parser.add_argument("--include-checkpoint", action="store_true",
                        help="also export an interrupted/running checkpoint as unfinished")
    args = parser.parse_args(argv)
    try:
        if args.out.suffix.lower() not in (".json", ".csv"):
            raise ValueError("--out must end in .json or .csv")
        targets = [args.out] + ([args.laps_out] if args.laps_out else [])
        if len({path.resolve() for path in targets}) != len(targets):
            raise ValueError("summary and lap output paths must differ")
        for path in targets:
            if path.exists():
                raise FileExistsError(f"refusing to overwrite {path}")
        bank, warnings = load(args.source)
        for warning in warnings:
            print(f"warning: {warning}", file=sys.stderr)
        result = export(bank, include_checkpoint=args.include_checkpoint)
        rows = result["workouts"]
        content = (json.dumps(result, indent=2, allow_nan=False) + "\n"
                   if args.out.suffix.lower() == ".json" else csv_text(rows, SUMMARY_COLUMNS))
        # Exclusive creation protects against a destination appearing mid-export.
        with args.out.open("x", encoding="utf-8", newline="") as handle:
            handle.write(content)
        if args.laps_out:
            with args.laps_out.open("x", encoding="utf-8", newline="") as handle:
                handle.write(csv_text([lap for item in rows for lap in item["laps"]], LAP_COLUMNS))
        print(f"Exported {len(rows)} workout(s) from {Path(bank['source']).name} to {args.out}")
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
