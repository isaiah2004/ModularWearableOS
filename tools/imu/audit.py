"""Read-only CSV signal audit and exact C workout movement-engine replay.

Run: python tools/imu/audit.py --out build/sports-reliability/audit-v1 INPUT...
Existing output directories are refused; original recordings are never modified.
"""
from __future__ import annotations
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import io
import json
import math
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
AXES = ("ax_g", "ay_g", "az_g", "gx_dps", "gy_dps", "gz_dps")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quantiles(values):
    data = sorted(v for v in values if math.isfinite(v))
    if not data:
        return None
    def q(p):
        x = (len(data) - 1) * p
        lo = int(x)
        return data[lo] + (data[min(lo + 1, len(data) - 1)] - data[lo]) * (x - lo)
    return dict(zip(("min", "p01", "p50", "p95", "p99", "max"),
                    (q(p) for p in (0, .01, .5, .95, .99, 1))))


def analyze(path, binary, out, label, metadata=None):
    report = {"source": str(path.resolve()), "sha256": sha(path), "bytes": path.stat().st_size,
              "user_context": label, "placement_metadata": metadata or {}, "sport_accuracy_validated": False}
    if not path.stat().st_size:
        return {**report, "status": "empty_preserved", "samples": 0}
    with path.open(newline="") as handle:
        reader = csv.DictReader(handle)
        if not {"ms", *AXES} <= set(reader.fieldnames or []):
            return {**report, "status": "unsupported_header", "header": reader.fieldnames}
        rows = list(reader)
    times, values, malformed = [], [], []
    for index, row in enumerate(rows, 2):
        try:
            t = int(row["ms"])
            if not 0 <= t <= 0xFFFFFFFF:
                raise ValueError("timestamp outside uint32")
            v = tuple(float(row[key]) for key in AXES)
        except (ValueError, TypeError, KeyError) as error:
            malformed.append({"line": index, "error": str(error)})
            continue
        times.append(t)
        values.append(v)
    report.update(samples=len(values), malformed_rows=malformed,
                  skipped_rows_policy="skip malformed rows; actual timestamp gaps remain visible")
    if not values:
        return {**report, "status": "no_parseable_samples"}
    dt = [b-a for a, b in zip(times, times[1:])]
    gap_indices = [i+1 for i, d in enumerate(dt) if d > 250 or d < 0]
    accel = [math.sqrt(sum(x*x for x in v[:3])) for v in values]
    gyro = [math.sqrt(sum(x*x for x in v[3:])) for v in values]
    per_axis = {key: {"nonfinite": sum(not math.isfinite(v[i]) for v in values),
                      "range": quantiles(v[i] for v in values)} for i, key in enumerate(AXES)}
    repeated = sum(a == b for a, b in zip(values, values[1:]))
    longest = run = 1
    for a, b in zip(values, values[1:]):
        run = run + 1 if a == b else 1
        longest = max(longest, run)
    normalized = "".join(",".join([str(t), *(format(v, ".9g") for v in sample)]) + "\n"
                         for t, sample in zip(times, values))
    result = subprocess.run([str(binary)], input=normalized, capture_output=True, text=True, check=True)
    replay_path = out / f"{path.stem}-replay.csv"
    replay_path.write_text(result.stdout)
    replay = list(csv.DictReader(io.StringIO(result.stdout)))
    final = {key: int(value) for key, value in replay[-1].items()}
    duration = max(0, times[-1]-times[0])
    # Current source config is +/-8g, +/-512dps. Historical firmware/filters are
    # unknown: these are conditional near-rail counts, not proven clipped frames.
    rails = {key: sum(math.isfinite(v[i]) and abs(v[i]) >= (.995 * (8 if i < 3 else 512))
                      for v in values) for i, key in enumerate(AXES)}
    report.update(status="audited", first_ms=times[0], last_ms=times[-1], duration_ms=duration,
                  timestamp_nonmonotonic=sum(d < 0 for d in dt), duplicate_timestamps=sum(d == 0 for d in dt),
                  dt_ms=quantiles(dt), effective_hz=(len(values)-1)*1000/duration if duration else None,
                  engine_gap_count=len(gap_indices), gap_intervals_ms=[[times[i-1], times[i]] for i in gap_indices],
                  axes=per_axis, accel_magnitude_g=quantiles(accel), gyro_magnitude_dps=quantiles(gyro),
                  identical_adjacent_6axis_rows=repeated, longest_identical_run_samples=longest,
                  duplicate_value_interpretation="Exact rounded CSV values only; repetition alone cannot establish stale acquisition.",
                  near_current_config_rails=rails,
                  clipping_interpretation="Conditional >=99.5% of current-source +/-8g or +/-512dps rails, per axis. Historical config and smoothing unknown; neither presence nor absence establishes true clipping.",
                  motion={"valid_ms": final["valid_ms"], "movement_ms_est": final["movement_ms"],
                          "bursts_est": final["bursts"],
                          "coverage_fraction": final["valid_ms"]/duration if duration else None},
                  replay_csv=replay_path.name)
    sidecar = out / f"{path.stem}-context.json"
    sidecar.write_text(json.dumps({"source": report["source"], "source_sha256": report["sha256"],
                                  "user_context": label, "placement_metadata": metadata or {}}, indent=2) + "\n")
    report["context_sidecar"] = sidecar.name
    plot(path.stem, times, accel, gyro, replay, gap_indices, out, metadata)
    report["plots"] = [f"{path.stem}-motion.svg", f"{path.stem}-motion.png"]
    return report


def plot(name, times, accel, gyro, replay, gaps, out, metadata=None):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    t = [(x-times[0])/1000 for x in times]
    fig, axes = plt.subplots(2, 1, figsize=(12, 6), sharex=True, constrained_layout=True)
    axes[0].plot(t, accel, lw=.55, color="#145a82")
    axes[1].plot(t, gyro, lw=.55, color="#7a3f86")
    axes[0].set_ylabel("Acceleration magnitude (g)")
    axes[1].set_ylabel("Gyro magnitude (deg/s)")
    axes[1].set_xlabel("Time from first recorded sample (s)")
    for axis in axes:
        axis.grid(alpha=.2)
        # Episode state includes the 600ms release hysteresis; shaded extent
        # is deliberately not represented as cumulative movement-active time.
        axis.fill_between(t, 0, 1, where=[bool(int(r["episode_active"])) for r in replay],
                          transform=axis.get_xaxis_transform(), color="#1b9968", alpha=.12,
                          label="Estimated episode state (includes release hysteresis)")
        for i in gaps:
            axis.axvspan(t[i-1], t[i], color="#c33333", alpha=.25)
        axis.legend(loc="upper right", fontsize=8)
    axes[0].axhline(1, lw=.5, color="gray")
    axes[1].axhline(55, lw=.6, ls=":", color="gray", label="Gyro onset threshold")
    context = ""
    if metadata:
        context = (f"\n{metadata.get('sport', 'unknown sport')} | watch {metadata.get('worn_wrist', 'unknown')} wrist | "
                   f"playing hand {metadata.get('playing_hand', 'unknown')} | user-confirmed; dominance unknown")
    fig.suptitle(f"{name}: exact C movement-engine replay: estimates, not shots or accuracy{context}")
    for extension in ("svg", "png"):
        fig.savefig(out / f"{name}-motion.{extension}", dpi=160)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path, nargs="+")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--context", default="Unconfirmed recording identity; wrist/playing hand not inferred")
    parser.add_argument("--metadata", type=Path, help="JSON placement provenance applying to every selected input")
    args = parser.parse_args()
    metadata = json.loads(args.metadata.read_text()) if args.metadata else None
    if len({path.stem for path in args.inputs}) != len(args.inputs):
        parser.error("input stems must be unique within one output directory")
    args.out.mkdir(parents=True, exist_ok=False)
    binary = (args.out / "motion_replay.exe").resolve()
    command = [args.cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-Inos",
               "tools/imu/motion_replay.c", "nos/workout_motion.c", "-lm", "-o", str(binary)]
    subprocess.run(command, cwd=ROOT, check=True, capture_output=True)
    sources = ("tools/imu/audit.py", "tools/imu/motion_replay.c", "nos/workout_motion.c",
               "nos/workout_motion.h", "firmware/main/board.c", "firmware/main/board_protocol.c")
    report = {"created_utc": datetime.now(timezone.utc).isoformat(), "compiler_command": command,
              "compiler_version": subprocess.run([args.cc, "--version"], capture_output=True, text=True, check=True).stdout,
              "source_sha256": {p: sha(ROOT / p) for p in sources},
              "binary_sha256": sha(binary),
              "units": {"time": "ms from CSV; uint32 engine clock", "acceleration": "g", "gyro": "deg/s"},
              "configuration": {"gap_limit_ms": 250, "onset_duration_ms": 300, "release_ms": 600,
                                "accel_on_abs_magnitude_minus_1_g": .16, "accel_off_g": .10,
                                "gyro_on_dps": 55, "gyro_off_dps": 35,
                                "invalid_accel_magnitude_gt_g": 32, "invalid_gyro_magnitude_gt_dps": 4000,
                                "threshold_policy": "fixed production source; no fitting to these recordings"},
              "limitations": ["No synchronized labels; no shot, rally or sporting accuracy estimate.",
                              "Record timestamps establish CSV cadence, not independent sensor freshness.",
                              "Host float32 engine replay; not device runtime timing verification.",
                              "Episode shading includes release hysteresis; cumulative movement counts only qualifying movement plus confirmed onset."],
              "recordings": [analyze(p, binary, args.out, args.context, metadata) for p in args.inputs]}
    (args.out / "report.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
    for recording in report["recordings"]:
        print(Path(recording["source"]).name, recording["status"], recording.get("motion", {}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
