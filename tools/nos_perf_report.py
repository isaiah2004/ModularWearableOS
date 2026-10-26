"""Audit recorded NOS animation timing; missing evidence never becomes a pass.

python tools/nos_perf_report.py capture.jsonl --manifest run.json --output report.json
Manifest example: {"source_revision":"...", "tree_sha256":"...",
 "firmware_elf_sha256":"...", "cpu_mhz":240, "capture_scope":"animation",
 "instrumentation":"nos-device-v1", "blur_coverage":"unmeasured"}.
Use animation_windows [{"start_us":...,"end_us":...}] for mixed NOSF captures.
Host serial receipt timestamps are retained but never treated as DMA timestamps.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import re


def records(text):
    """Read raw serial, {line,t} JSONL, and watchctl {response,time} JSONL."""
    for number, raw in enumerate(text.splitlines(), 1):
        stamp, lines = None, [raw]
        if raw.lstrip().startswith("{"):
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise ValueError(f"Malformed JSONL on line {number}") from exc
            if not isinstance(obj, dict):
                raise ValueError(f"Expected object on line {number}")
            stamp = obj.get("t", obj.get("time"))
            lines = obj.get("response", [obj.get("line", "")])
            if not isinstance(lines, list) or any(not isinstance(x, str) for x in lines):
                raise ValueError(f"Expected text lines on line {number}")
        for line in lines:
            match = re.search(r"\b(NOSF|NOSPERF|NOSBUILD|NOSCACHE|NOSDIAG|NOSWORKER|NOSPM)\s+", line)
            if not match:
                continue
            fields = dict(re.findall(r"\b(\w+)=([^\s]+)", line[match.end():]))
            yield {"kind": match[1], "fields": fields, "capture_time": stamp,
                   "line_number": number}


def number(fields, key):
    if key not in fields:
        return None
    try:
        value = float(fields[key])
    except (TypeError, ValueError) as exc:
        raise ValueError(f"Invalid numeric field {key}={fields[key]!r}") from exc
    if not math.isfinite(value):
        raise ValueError(f"Nonfinite numeric field {key}")
    return value


def analyze(text, manifest):
    rows = list(records(text))
    builds = [r["fields"] for r in rows if r["kind"] == "NOSBUILD"]
    caches = [r["fields"] for r in rows if r["kind"] == "NOSCACHE"]
    clocks = [r["fields"] for r in rows if r["kind"] == "NOSPM"]
    # Diagnostics are partial updates: an omitted control retains its previous
    # value. Optimization-only A/B flags never imply disabled production blur.
    diagnostics = [r["fields"] for r in rows if r["kind"] == "NOSDIAG"
                   and ("layers" in r["fields"] or "history" in r["fields"])]
    windows = manifest.get("animation_windows", [])
    for window in windows:
        if window["end_us"] <= window["start_us"]:
            raise ValueError("Animation windows must have positive duration")
    animation_only = manifest.get("capture_scope") == "animation"
    selected = []
    runtime_controls = []
    worker_mode = None
    production_state = {}
    for row in rows:
        if row["kind"] in ("NOSDIAG", "NOSWORKER", "NOSPM"):
            runtime_controls.append(row)
            for key in ("layers", "history"):
                if key in row["fields"]:
                    production_state[key] = number(row["fields"], key)
            if "exposure_worker" in row["fields"]:
                worker_mode = number(row["fields"], "exposure_worker")
        if row["kind"] in ("NOSBUILD", "NOSCACHE", "NOSDIAG", "NOSWORKER", "NOSPM"):
            continue
        row["production_state"] = production_state.copy()
        row["exposure_worker"] = number(row["fields"], "exposure_worker")
        if row["exposure_worker"] is None:
            row["exposure_worker"] = worker_mode
        if animation_only:
            selected.append(row)
        elif row["kind"] == "NOSF" and windows:
            begin = number(row["fields"], "begin_us")
            if begin is not None:
                for index, window in enumerate(windows):
                    if window["start_us"] <= begin < window["end_us"]:
                        row["window"] = index
                        selected.append(row)
                        break
    frames = [r for r in selected if r["kind"] == "NOSF"]
    summaries = [r for r in selected if r["kind"] == "NOSPERF"]
    notes = []
    checks = {"identity": "incomplete", "animation_scope": "pass" if selected else "incomplete",
              "target40": "incomplete", "floor30": "incomplete",
              "cpu200_budget": "incomplete", "blur_coverage": "incomplete",
              "allocations": "incomplete", "production_features": "incomplete"}
    if diagnostics:
        reported = {key: [number(d, key) for d in diagnostics if key in d]
                    for key in ("layers", "history")}
        if any(value != 1 for values in reported.values() for value in values):
            checks["production_features"] = "fail"
            notes.append("A reported runtime cache/history control was disabled; "
                         "this capture cannot qualify production behavior.")
        elif (all(reported.values()) and selected
              and all(all(row["production_state"].get(key) == 1 for key in ("layers", "history"))
                      for row in selected)):
            checks["production_features"] = "pass"
    fingerprint = manifest.get("firmware_elf_sha256", "")
    identity = (bool(manifest.get("source_revision"))
                and bool(re.fullmatch(r"[0-9a-fA-F]{64}", manifest.get("tree_sha256", "")))
                and bool(re.fullmatch(r"[0-9a-fA-F]{64}", fingerprint)))
    if identity:
        checks["identity"] = "pass"
    if builds and fingerprint and any(b.get("elf_sha256", "").lower() != fingerprint.lower() for b in builds):
        checks["identity"] = "fail"
        notes.append("Captured firmware ELF fingerprint differs from the manifest.")
    trusted = manifest.get("instrumentation") == "nos-device-v1"
    if builds and any(b.get("metrics") != "nos-device-v1" for b in builds):
        trusted = False
    cpu = manifest.get("cpu_mhz")
    if cpu is not None and (not isinstance(cpu, (int, float)) or cpu <= 0):
        raise ValueError("cpu_mhz must be positive")
    if builds and cpu and any(number(b, "cpu_mhz") != cpu for b in builds):
        checks["identity"] = "fail"
        notes.append("Captured CPU configuration differs from the manifest.")

    target_violations, floor_violations = 0, 0
    measured_intervals, e2e, estimates = [], [], []
    async_estimate_skipped = False
    allocation_failures = []
    cache_bytes = []
    blur_bytes = []
    for fields in caches + [r["fields"] for r in frames]:
        for key, destination in (("allocfail", allocation_failures), ("cache_bytes", cache_bytes),
                                 ("blur_bytes", blur_bytes)):
            value = number(fields, key)
            if value is not None:
                if value < 0:
                    raise ValueError(f"Negative {key}")
                destination.append(value)
    if allocation_failures:
        checks["allocations"] = "fail" if max(allocation_failures) else "pass"
    target_evidence = floor_evidence = False
    previous = None
    timestamped = 0
    painted_intervals = 0
    for row in frames:
        f = row["fields"]
        duration, lateness = number(f, "e2e"), number(f, "deadline_us")
        begin, completed = number(f, "begin_us"), number(f, "completed_us")
        if (begin is None) != (completed is None):
            raise ValueError("Incomplete device frame timestamps")
        if begin is not None:
            measured = completed - begin
            if measured < 0:
                raise ValueError("Completion precedes frame start")
            if duration is not None and duration != measured:
                raise ValueError("End-to-end duration disagrees with device timestamps")
            duration = measured
        if duration is not None:
            if duration < 0:
                raise ValueError("Negative end-to-end duration")
            e2e.append(duration / 1000)
            target_evidence = True
            target_violations += duration > 25000
        if lateness is not None:
            target_evidence = True
            target_violations += lateness > 0
        if begin is not None and completed is not None:
            if completed < begin:
                raise ValueError("Completion precedes frame start")
            timestamped += 1
            frame_id = number(f, "frame_id")
            painted = number(f, "px") != 0  # NOSF historically emits only painted frames.
            if previous is not None and completed <= previous[0]:
                raise ValueError("Nonmonotonic or duplicate completion timestamps")
            known_ids = previous is not None and frame_id is not None and previous[2] is not None
            contiguous = (painted and previous is not None and row.get("window") == previous[1]
                          and ((known_ids and frame_id == previous[2] + 1)
                               or (not known_ids and manifest.get("trace_contiguous") is True)))
            if contiguous:
                interval = completed - previous[0]
                if interval <= 0:
                    raise ValueError("Nonmonotonic or duplicate completion timestamps")
                painted_intervals += 1
                measured_intervals.append(interval / 1000)
                floor_evidence = True
                floor_violations += interval > 1000000 / 30
                target_violations += interval > 25000
            previous = (completed, row.get("window"), frame_id) if painted else None
        else:
            previous = None
        # Only explicitly timed CPU stages are scaled. Raster includes waits;
        # rotation overlaps CPU/bus and must not be added a second time.
        rec, ras, wait = (number(f, k) for k in ("rec", "ras", "wait"))
        if duration is not None and cpu and None not in (rec, ras, wait):
            if min(rec, ras, wait) < 0 or wait > ras:
                raise ValueError("Invalid stage timing")
            tick = number(f, "tick_us")
            if tick is not None and tick < 0:
                raise ValueError("Negative tick timing")
            known_cpu = rec + ras - wait + (tick or 0)
            factor = max(cpu / 200.0, 1.0)
            if row["exposure_worker"] == 1:
                # Worker CPU overlaps UI raster, queue waits and DMA. A summed
                # history_us is not an additive critical-path CPU stage.
                async_estimate_skipped = True
            else:
                estimates.append((duration + (factor - 1) * known_cpu) / 1000)
    for row in summaries:
        f = row["fields"]
        maximum = number(f, "e2e_max")
        if maximum is not None:
            e2e.append(maximum)
            target_evidence = True
            target_violations += maximum > 25
        deadline, slots = number(f, "deadline"), number(f, "target_slots")
        if deadline is not None and slots is not None:
            if min(deadline, slots) < 0:
                raise ValueError("Negative miss count")
            target_evidence = True
            target_violations += deadline + slots
        floor, interval = number(f, "floor"), number(f, "present_max")
        if floor is not None and interval is not None:
            if min(floor, interval) < 0:
                raise ValueError("Negative floor metric")
            count = number(f, "present_intervals")
            if count is None:
                count = number(f, "intervals")
            if count is not None and (count < 0 or count != int(count)):
                raise ValueError("Invalid painted interval count")
            if count is not None and count > 0 and interval > 0:
                painted_intervals += int(count)
                floor_evidence = True
            floor_violations += floor + (interval > 1000 / 30)
            target_violations += interval > 25
            if interval:
                measured_intervals.append(interval)
        rate = number(f, "rate")
        if rate is not None and rate > 0 and rate < 39.5:
            target_violations += 1
    motion_evidence = painted_intervals > 0
    checks["animation_scope"] = "pass" if selected and motion_evidence else "incomplete"
    if target_evidence or target_violations:
        checks["target40"] = "fail" if target_violations else ("pass" if trusted and motion_evidence else "incomplete")
    if floor_evidence or floor_violations:
        checks["floor30"] = "fail" if floor_violations else ("pass" if trusted else "incomplete")
    clock_verified = bool(clocks)
    for clock in clocks:
        observed, maximum = number(clock, "cpu_mhz"), number(clock, "max_mhz")
        if any(value is not None and value <= 0 for value in (observed, maximum)):
            raise ValueError("Invalid observed/locked CPU clock")
        if (observed is None or maximum is None or observed > maximum
                or observed > 200 or maximum > 200):
            clock_verified = False
    if (cpu and cpu <= 200 and e2e and clock_verified
            and checks["target40"] == "pass" and checks["floor30"] == "pass"):
        checks["cpu200_budget"] = "pass"
    elif estimates and max(estimates) > 25:
        checks["cpu200_budget"] = "fail_estimate"
    if cpu and cpu <= 200 and not clock_verified:
        notes.append("A boot/config CPU value cannot establish the active 200 MHz budget: "
                     "require NOSPM observed cpu_mhz and locked max_mhz both at or below 200; "
                     "missing, partial or contradictory clock evidence remains incomplete.")
    coverage = manifest.get("blur_coverage", "unmeasured")
    if (coverage == "verified" and manifest.get("blur_coverage_evidence")
            and checks["production_features"] == "pass"):
        checks["blur_coverage"] = "pass"
    else:
        notes.append("Moving-object blur coverage or enabled production controls are unverified; timing alone cannot establish them.")
    if estimates:
        notes.append("200 MHz values scale only tick (when recorded) + record + (raster - stripe wait), "
                     "not bus/rotation. Cache, radio contention, host housekeeping and overlap "
                     "are not modeled: estimate is not proof.")
    if async_estimate_skipped:
        notes.append("200 MHz extrapolation omitted for async exposure frames: worker/UI/DMA "
                     "overlap needs direct hardware measurement. history_us sums preparation "
                     "and worker elapsed work-stage time; it must not be added to raster or e2e.")
    if not trusted:
        notes.append("Legacy or unknown instrumentation cannot certify completion/deadline semantics.")
    if not motion_evidence:
        notes.append("No proven consecutive painted completions: legacy summaries without a nonzero "
                     "present_intervals count cannot certify animation cadence, even when painted > 0.")
    if not selected:
        notes.append("No animation-scoped samples; supply capture_scope=animation or device-time windows.")
    if timestamped < len(frames):
        notes.append("Some frame traces lack device completion timestamps; serial receipt time is not substituted.")
    failed = any(v == "fail" for v in checks.values())
    estimated_failure = any(v == "fail_estimate" for v in checks.values())
    complete = all(v == "pass" for v in checks.values())
    stamps = [r["capture_time"] for r in rows if r["capture_time"] is not None]
    return {"status": "observed_failure" if failed else "estimated_budget_failure" if estimated_failure
            else "pass_observed" if complete else "incomplete",
            "checks": checks, "manifest": manifest,
            "capture_sha256": hashlib.sha256(text.encode()).hexdigest(),
            "capture_first_timestamp": stamps[0] if stamps else None,
            "capture_last_timestamp": stamps[-1] if stamps else None,
            "runtime_controls": runtime_controls,
            "samples": {"frames": len(frames), "summaries": len(summaries),
                        "device_timestamped_frames": timestamped, "painted_intervals": painted_intervals},
            "max_e2e_ms": max(e2e, default=None),
            "max_completion_interval_ms": max(measured_intervals, default=None),
            "cpu200_estimate_max_ms": max(estimates, default=None),
            "allocation_failures_max": max(allocation_failures, default=None),
            "layer_history_cache_bytes_max": max(cache_bytes, default=None),
            "ui_blur_cache_bytes_max": max(blur_bytes, default=None),
            "notes": notes + ["Results cover recorded windows only; they do not guarantee future no-drop hardware behavior."]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        report = analyze(args.capture.read_text(encoding="utf-8"),
                         json.loads(args.manifest.read_text(encoding="utf-8")))
    except (ValueError, KeyError, TypeError, OSError) as exc:
        parser.error(str(exc))
    rendered = json.dumps(report, indent=2, allow_nan=False) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0 if report["status"] == "pass_observed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
