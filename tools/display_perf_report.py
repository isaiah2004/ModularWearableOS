"""Summarize display benchmark capture JSONL without filling absent metrics.

Examples:
  python tools/display_perf_report.py build/display-sweep.jsonl
  python tools/display_perf_report.py build/display-*.jsonl --format json -o build/report.json
"""
import argparse
import json
from pathlib import Path
import sys


STAGES = ("copy", "swap", "rotate", "queue_wait", "dma_wait")


def _ratio(value, divisor):
    return value / divisor if isinstance(value, (int, float)) and divisor else None


def _capture_record(obj):
    if not isinstance(obj, dict):
        return None, None
    if isinstance(obj.get("record"), str) and isinstance(obj.get("perf"), dict):
        return obj["record"], obj["perf"]
    line = obj.get("line")
    if not isinstance(line, str):
        return None, None
    for separator in (",", " "):
        if separator in line:
            kind, payload = line.split(separator, 1)
            if kind.startswith("PERF_"):
                try:
                    decoded = json.loads(payload)
                except json.JSONDecodeError:
                    return None, None
                return (kind, decoded) if isinstance(decoded, dict) else (None, None)
    return None, None


def load_records(paths):
    records = []
    for path in paths:
        with path.open(encoding="utf-8") as stream:
            for line_number, line in enumerate(stream, 1):
                if not line.strip():
                    continue
                try:
                    obj = json.loads(line)
                except json.JSONDecodeError as exc:
                    raise ValueError(f"{path}:{line_number}: invalid JSON: {exc.msg}") from exc
                kind, payload = _capture_record(obj)
                if kind:
                    records.append((str(path), line_number, kind, payload))
    return records


def _anomalies(perf, config):
    issues = []
    submitted_tx = perf.get("submitted_tx")
    completed_tx = perf.get("completed_tx")
    cancelled_tx = perf.get("cancelled_tx")
    pending_tx = perf.get("pending_tx")
    if all(isinstance(x, int) for x in (submitted_tx, completed_tx, cancelled_tx, pending_tx)):
        if submitted_tx != completed_tx + cancelled_tx + pending_tx:
            issues.append("transaction accounting mismatch")
    submitted_bytes = perf.get("submitted_bytes")
    completed_bytes = perf.get("completed_bytes")
    cancelled_bytes = perf.get("cancelled_bytes")
    pending_bytes = perf.get("pending_bytes")
    if all(isinstance(x, int) for x in
           (submitted_bytes, completed_bytes, cancelled_bytes, pending_bytes)):
        if submitted_bytes != completed_bytes + cancelled_bytes + pending_bytes:
            issues.append("byte accounting mismatch")
    if perf.get("unmatched_completions", 0):
        issues.append(f"{perf['unmatched_completions']} unmatched completion(s)")
    if perf.get("submit_overlaps", 0):
        issues.append(f"{perf['submit_overlaps']} overlapping submit(s)")
    if config is None and perf.get("trace", "") == "":
        issues.append("missing preceding PERF_CONFIG")
    if config and isinstance(config.get("frames_requested"), int):
        completed = config.get("completed_frames")
        if isinstance(completed, int) and completed != config["frames_requested"]:
            issues.append(f"completed {completed}/{config['frames_requested']} requested frames")
        if config.get("errors"):
            issues.append(f"benchmark error: {config.get('error_name', 'unknown')}")
    return issues


def summarize(records):
    rows = []
    pending_config = None
    pending_source = None
    for source, line, kind, payload in records:
        if kind == "PERF_CONFIG":
            pending_config = payload
            pending_source = source
            continue
        if kind not in ("PERF_RUN", "PERF_APP"):
            continue
        config = pending_config if kind == "PERF_RUN" and source == pending_source else None
        if kind == "PERF_RUN":
            pending_config = None
            pending_source = None
        elapsed_us = payload.get("elapsed_us")
        frames = payload.get("frames")
        row = {
            "kind": kind,
            "source_file": source,
            "source_line": line,
            "name": payload.get("trace") or (config or {}).get("source"),
            "elapsed_us": elapsed_us,
            "frames": frames,
            "fps": _ratio(frames * 1000000 if isinstance(frames, (int, float)) else None,
                          elapsed_us),
            "throughput_Bps": payload.get("throughput_Bps"),
            "frame_p50_us": (payload.get("frame_work") or {}).get("p50_us"),
            "frame_p95_us": (payload.get("frame_work") or {}).get("p95_us"),
            "frame_p99_us": (payload.get("frame_work") or {}).get("p99_us"),
            "dirty_pixels_per_frame": _ratio(payload.get("dirty_pixels"), frames),
            "deadline_misses": payload.get("deadline_misses"),
            "config": config,
        }
        row["stage_us_per_frame"] = {
            stage: _ratio((payload.get(stage) or {}).get("total_us"), frames)
            if (payload.get(stage) or {}).get("n", 0) else None
            for stage in STAGES
        }
        row["anomalies"] = _anomalies(payload, config)
        rows.append(row)
    return rows


def _display(value, digits=2):
    if value is None:
        return "—"
    if isinstance(value, float):
        return f"{value:.{digits}f}"
    return str(value)


def markdown(rows):
    columns = ["kind", "name", "fps", "frame P50 µs", "P95 µs", "P99 µs",
               "dirty px/frame", "throughput B/s", "stage µs/frame", "anomalies"]
    output = ["| " + " | ".join(columns) + " |",
              "| " + " | ".join(["---"] * len(columns)) + " |"]
    for row in rows:
        stages = ", ".join(f"{name}={_display(value)}"
                           for name, value in row["stage_us_per_frame"].items()
                           if value is not None) or "—"
        anomalies = "; ".join(row["anomalies"]) or "—"
        values = [row["kind"], _display(row["name"]), _display(row["fps"]),
                  _display(row["frame_p50_us"]), _display(row["frame_p95_us"]),
                  _display(row["frame_p99_us"]), _display(row["dirty_pixels_per_frame"]),
                  _display(row["throughput_Bps"]), stages, anomalies]
        output.append("| " + " | ".join(str(x).replace("|", "\\|") for x in values) + " |")
    return "\n".join(output) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("inputs", type=Path, nargs="+")
    parser.add_argument("--format", choices=("markdown", "json"), default="markdown")
    parser.add_argument("-o", "--output", type=Path)
    args = parser.parse_args(argv)
    try:
        rows = summarize(load_records(args.inputs))
    except (OSError, ValueError) as exc:
        print(f"display_perf_report: {exc}", file=sys.stderr)
        return 1
    rendered = json.dumps(rows, indent=2) + "\n" if args.format == "json" else markdown(rows)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    else:
        sys.stdout.write(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
