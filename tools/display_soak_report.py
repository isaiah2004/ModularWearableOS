"""Emit a machine-readable summary of Breathe display soak JSONL.

The input is produced by ``display_perf.py``.  Exit status is zero when a
report was emitted, including incomplete or failed reports; malformed input
is an invocation error and returns nonzero.
"""
import argparse
import json
from pathlib import Path
import re
import sys


INFO_FIELDS = (
    "internal_free", "internal_largest", "dma_largest", "psram_free",
    "main_stack_free_bytes", "flush_stack_free_bytes",
)
COUNTERS = (
    "submitted_tx", "completed_tx", "cancelled_tx", "pending_tx",
    "submitted_bytes", "completed_bytes", "cancelled_bytes", "pending_bytes",
    "unmatched_completions", "submit_overlaps",
)
FAULT_PATTERNS = (
    ("panic", re.compile(r"\b(?:guru meditation|panic(?:ked)?\b)", re.I)),
    ("abort", re.compile(r"\babort(?:\(\))?(?:ed|ing)?\b", re.I)),
    ("watchdog", re.compile(
        r"\b(?:task watchdog|task_wdt|watchdog got triggered|wdt reset|"
        r"task watchdog got triggered)\b", re.I)),
    ("brownout", re.compile(r"\bbrownout(?: detector)?\b", re.I)),
    ("reboot", re.compile(r"\b(?:rst:0x|esp-rom:|rebooting\b|software reset\b)", re.I)),
)


def _decoded_record(obj):
    if isinstance(obj.get("record"), str) and isinstance(obj.get("perf"), dict):
        return obj["record"], obj["perf"]
    line = obj.get("line")
    if not isinstance(line, str):
        return None, None
    match = re.fullmatch(r"\s*(PERF_[A-Z0-9_]+)(?:\s+|\s*,\s*)(.+?)\s*", line)
    if not match:
        return None, None
    try:
        payload = json.loads(match.group(2))
    except json.JSONDecodeError:
        return None, None
    return (match.group(1), payload) if isinstance(payload, dict) else (None, None)


def load(path):
    records = []
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_number}: invalid JSON: {exc.msg}") from exc
            if not isinstance(obj, dict):
                raise ValueError(f"{path}:{line_number}: JSONL record must be an object")
            records.append((line_number, obj))
    return records


def _range(infos, field):
    values = [item[field] for item in infos if isinstance(item.get(field), (int, float))]
    return {"min": min(values), "max": max(values)} if values else None


def summarize(records, expected_traces=30, expected_seconds=1800.0):
    traces = []
    infos = []
    faults = []
    for line_number, obj in records:
        kind, payload = _decoded_record(obj)
        if kind == "PERF_APP" and payload.get("trace") == "breathe":
            traces.append((line_number, payload))
        elif kind == "PERF_INFO":
            infos.append(payload)
        raw = obj.get("line")
        if obj.get("direction") == "rx" and isinstance(raw, str):
            for category, pattern in FAULT_PATTERNS:
                if pattern.search(raw):
                    faults.append({"line": line_number, "category": category, "text": raw})
                    break

    missing = []
    elapsed_values = []
    fps_values = []
    totals = {name: 0 for name in COUNTERS}
    missing_counters = {name: 0 for name in COUNTERS}
    accounting = []
    present_frames = 0
    for line_number, trace in traces:
        elapsed_us = trace.get("elapsed_us")
        frames = trace.get("present_frames")
        if isinstance(elapsed_us, (int, float)) and elapsed_us > 0:
            elapsed_values.append(elapsed_us)
            if isinstance(frames, (int, float)):
                present_frames += frames
                fps_values.append(frames * 1_000_000 / elapsed_us)
            else:
                missing.append(f"line {line_number}: missing present_frames")
        else:
            missing.append(f"line {line_number}: missing positive elapsed_us")
        for name in COUNTERS:
            value = trace.get(name)
            if isinstance(value, int) and value >= 0:
                totals[name] += value
            else:
                missing_counters[name] += 1
        for unit in ("tx", "bytes"):
            names = [f"submitted_{unit}", f"completed_{unit}",
                     f"cancelled_{unit}", f"pending_{unit}"]
            if all(isinstance(trace.get(name), int) for name in names):
                submitted, completed, cancelled, pending = (trace[name] for name in names)
                if submitted != completed + cancelled + pending:
                    accounting.append({"line": line_number, "unit": unit,
                                       "submitted": submitted, "completed": completed,
                                       "cancelled": cancelled, "pending": pending})

    for name, count in missing_counters.items():
        if count:
            missing.append(f"{count} trace(s) missing {name}")
    for field in INFO_FIELDS:
        if not any(isinstance(item.get(field), (int, float)) for item in infos):
            missing.append(f"no PERF_INFO value for {field}")

    elapsed_seconds = sum(elapsed_values) / 1_000_000
    coverage = {
        "expected_trace_count": expected_traces,
        "actual_trace_count": len(traces),
        "expected_elapsed_seconds": expected_seconds,
        "actual_elapsed_seconds": elapsed_seconds,
        "trace_count_satisfied": len(traces) >= expected_traces,
        "elapsed_satisfied": elapsed_seconds >= expected_seconds,
    }
    anomalies = {
        "accounting_mismatches": accounting,
        "cancelled_tx": totals["cancelled_tx"],
        "cancelled_bytes": totals["cancelled_bytes"],
        "pending_tx": totals["pending_tx"],
        "pending_bytes": totals["pending_bytes"],
        "unmatched_completions": totals["unmatched_completions"],
        "submit_overlaps": totals["submit_overlaps"],
    }
    has_anomaly = bool(accounting or totals["cancelled_tx"] or totals["cancelled_bytes"] or
                       totals["pending_tx"] or totals["pending_bytes"] or
                       totals["unmatched_completions"] or totals["submit_overlaps"])
    if faults or has_anomaly:
        status = "fail"
    elif missing or not coverage["trace_count_satisfied"] or not coverage["elapsed_satisfied"]:
        status = "incomplete"
    else:
        status = "pass"
    return {
        "status": status,
        "coverage": coverage,
        "present_fps": {
            "min": min(fps_values) if fps_values else None,
            "max": max(fps_values) if fps_values else None,
            "weighted": (present_frames * 1_000_000 / sum(elapsed_values))
                        if elapsed_values and len(fps_values) == len(elapsed_values) else None,
        },
        "transactions": {"submitted": totals["submitted_tx"],
                         "completed": totals["completed_tx"]},
        "bytes": {"submitted": totals["submitted_bytes"],
                  "completed": totals["completed_bytes"]},
        "anomalies": anomalies,
        "perf_info": {field: _range(infos, field) for field in INFO_FIELDS},
        "runtime_faults": faults,
        "missing_evidence": missing,
        "notes": [
            "No pooled percentiles are reported because raw histograms are unavailable.",
            "Pass checks trace coverage, counter accounting, and captured runtime fault lines; "
            "it does not verify the physical screen, current draw, or prove absence of a leak.",
            "Coverage assumes each Breathe PERF_APP is one live trace; exported or duplicate "
            "records cannot be identified because trace IDs are unavailable.",
        ],
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("-o", "--output", type=Path)
    parser.add_argument("--expected-traces", type=int, default=30)
    parser.add_argument("--expected-seconds", type=float, default=1800.0)
    args = parser.parse_args(argv)
    if args.expected_traces <= 0 or args.expected_seconds <= 0:
        parser.error("expected traces and seconds must be positive")
    try:
        report = summarize(load(args.input), args.expected_traces, args.expected_seconds)
        rendered = json.dumps(report, indent=2) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered, encoding="utf-8")
        else:
            sys.stdout.write(rendered)
    except (OSError, ValueError) as exc:
        print(f"display_soak_report: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
