"""Analyze copied watch CSV logs and checked Sports journals without device I/O.

Example: python tools/analyze_watch_power.py build/sports-reliability/watch-copy
 --out build/sports-reliability/power-analysis --recent-from 230

Rail voltage is not charge/current. Unknown clocks, boot gaps and unsupported
power intervals remain unknown. CPU values are logged ceilings, not a frequency
measurement during dynamic scaling. All displayed times use IST (UTC+05:30).
"""
import argparse
import collections
import contextlib
import csv
import datetime as dt
import hashlib
import io
import json
import math
import pathlib
import statistics
import struct
import zlib

import sports_export

IST = dt.timezone(dt.timedelta(hours=5, minutes=30), "IST")
STATES = {0: "active", 1: "sensor_idle", 2: "display_off", 3: "deep_sleep", 4: "aod"}
VALID_EPOCH = 1704067200


def local_time(epoch):
    return dt.datetime.fromtimestamp(epoch, IST).isoformat(timespec="milliseconds") if epoch is not None else None


def epoch_valid(epoch):
    return VALID_EPOCH <= epoch < 4102444800


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def preserved_preferences(directory):
    valid = []
    for path in sorted(directory.glob("settings.*.bin")) if directory.exists() else ():
        raw = path.read_bytes()
        if len(raw) != 276:
            continue
        magic, version, generation, size = struct.unpack_from("<4I", raw)
        if magic != 0x4e4f5350 or version != 1 or size != 42 or sports_export.checksum(raw[:-4]) != struct.unpack_from("<I", raw, 272)[0]:
            continue
        payload = raw[16:16 + size]
        offsets = {"brightness": 5, "auto_bright": 6, "aod": 7, "raise_wake": 8, "wrist_left": 9,
                   "sleep_s": 14, "wifi": 16, "ble": 17, "aod_bright": 27, "revision": 29, "sensor_idle": 30}
        valid.append({"file": str(path), "sha256": sha256(path), "generation": generation,
                      "payload_bytes": size, "values": {name: payload[offset] for name, offset in offsets.items()}})
    if not valid:
        return None
    newest = max(valid, key=lambda item: item["generation"])
    newest["scope"] = "Preserved current checkpoint before flash; not proof these preferences applied throughout historical logs."
    newest["source_sensor_idle_panel_level"] = int(6 + max(1, min(100, newest["values"]["aod_bright"])) * 2.49)
    return newest


def read_csv_logs(directory):
    files, points = [], []
    for path in sorted(directory.glob("*.csv")):
        rows, corrections, previous = [], [], None
        with path.open(newline="") as source:
            for line, raw in enumerate(csv.DictReader(source), 2):
                epoch, uptime = int(raw["utc_epoch"]), int(raw["uptime_ms"])
                row = {"file": path.name, "line": line, "uptime_ms": uptime,
                       "raw_epoch": epoch, "epoch": epoch if epoch_valid(epoch) else None,
                       "time_source": "recorded" if epoch_valid(epoch) else "unknown",
                       "rail_mv": int(raw["system_mv"]), "imu_valid": int(raw["imu_valid"])}
                row["clock_segment"] = previous["clock_segment"] if previous else 0
                if previous:
                    elapsed = (uptime - previous["uptime_ms"]) / 1000
                    if elapsed < 0:
                        row["clock_segment"] += 1
                        corrections.append({"line": line, "kind": "uptime_restart", "delta_ms": uptime - previous["uptime_ms"]})
                    elif row["epoch"] is not None and previous["epoch"] is not None:
                        jump = row["epoch"] - previous["epoch"] - elapsed
                        if abs(jump) > 3:
                            row["clock_segment"] += 1
                            corrections.append({"line": line, "kind": "clock_correction", "correction_s": round(jump, 3), "local_time": local_time(epoch)})
                    elif row["epoch"] is not None and previous["epoch"] is None:
                        corrections.append({"line": line, "kind": "clock_established", "local_time": local_time(epoch)})
                rows.append(row)
                previous = row
        # Back-project only leading unset UTCs from the first valid clock in the
        # same monotonic uptime run. Keep explicit provenance; never label 1970.
        for row in rows:
            if row["epoch"] is None:
                anchors = [other for other in rows if other["epoch"] is not None
                           and other["time_source"] == "recorded"
                           and other["clock_segment"] == row["clock_segment"]]
                if anchors:
                    anchor = min(anchors, key=lambda other: abs(other["uptime_ms"] - row["uptime_ms"]))
                    row["epoch"] = anchor["epoch"] + (row["uptime_ms"] - anchor["uptime_ms"]) / 1000
                    row["time_source"] = "inferred_same_run_uptime"
            row["local_time"] = local_time(row["epoch"])
        valid = [row for row in rows if row["epoch"] is not None]
        summary = {"file": path.name, "sha256": sha256(path), "samples": len(rows),
                   "unknown_clock_samples": sum(row["time_source"] == "unknown" for row in rows),
                   "inferred_clock_samples": sum(row["time_source"] == "inferred_same_run_uptime" for row in rows),
                   "clock_events": corrections}
        if rows:
            summary.update(first_rail_mv=rows[0]["rail_mv"], last_rail_mv=rows[-1]["rail_mv"],
                           minimum_rail_mv=min(row["rail_mv"] for row in rows),
                           maximum_rail_mv=max(row["rail_mv"] for row in rows),
                           observed_uptime_s=(rows[-1]["uptime_ms"] - rows[0]["uptime_ms"]) / 1000,
                           first_uptime_s=rows[0]["uptime_ms"] / 1000,
                           last_uptime_s=rows[-1]["uptime_ms"] / 1000,
                           first_local=valid[0]["local_time"] if valid else None,
                           last_local=valid[-1]["local_time"] if valid else None)
        files.append(summary)
        points.extend(rows)
    return files, points


def read_journal(path):
    errors = io.StringIO()
    with contextlib.redirect_stderr(errors):
        rows = list(sports_export.records(path))
    unique, duplicate, conflicts = {}, 0, 0
    for row in rows:
        key = row[1], row[2]
        if key in unique:
            duplicate += 1
            conflicts += row != unique[key]
        else:
            unique[key] = row
    boots = collections.defaultdict(list)
    for row in unique.values():
        boots[row[1]].append({"boot": row[1], "sequence": row[2], "uptime_ms": row[3],
                              "epoch": row[4] if epoch_valid(row[4]) else None,
                              "day": row[5], "minute": row[6], "type": row[7], "v": list(row[8:16])})
    for events in boots.values():
        events.sort(key=lambda event: event["sequence"])
    metadata = {"file": str(path), "bytes": path.stat().st_size, "sha256": sha256(path),
                "valid_records": len(rows), "unique_records": len(unique), "duplicates": duplicate,
                "conflicting_ids": conflicts, "boot_ids": len(boots),
                "truncated_bytes": path.stat().st_size % sports_export.RECORD.size,
                "integrity_messages": errors.getvalue().splitlines()}
    return boots, metadata


def recover_journal(source, destination, expected_bytes=None, expected_crc=None, rejected_lines=None):
    """Never modify the source; admit only complete magic+FNV-checked records."""
    raw = source.read_bytes()
    magic = struct.pack("<I", 0x53505431)
    records, spans, cursor, scan = 0, [], 0, 0
    output = destination / "recovered-records.spt"
    if source.resolve() == output.resolve():
        raise ValueError("Recovery output must differ from the original source")
    with output.open("wb") as sink:
        while True:
            found = raw.find(magic, scan)
            if found < 0 or found + sports_export.RECORD.size > len(raw):
                break
            candidate = raw[found:found + sports_export.RECORD.size]
            if sports_export.checksum(candidate[:-4]) != struct.unpack_from("<I", candidate, 60)[0]:
                scan = found + 1
                continue
            if found > cursor:
                spans.append({"source_offset": cursor, "discarded_bytes": found - cursor,
                              "next_recovered_record": records})
            sink.write(candidate);records += 1
            scan = cursor = found + sports_export.RECORD.size
    if cursor < len(raw):
        spans.append({"source_offset": cursor, "discarded_bytes": len(raw) - cursor, "next_recovered_record": None})
    metadata = {"source": str(source), "source_sha256": sha256(source), "source_bytes": len(raw),
                "source_crc32": f"{zlib.crc32(raw) & 0xffffffff:08x}", "whole_file_verified": False,
                "expected_bytes": expected_bytes, "expected_crc32": expected_crc, "rejected_base64_lines": rejected_lines,
                "recovered_file": str(output), "recovered_sha256": sha256(output), "recovered_records": records,
                "recovered_bytes": records * sports_export.RECORD.size, "byte_resynchronization_holes": spans,
                "discarded_received_bytes": sum(span["discarded_bytes"] for span in spans),
                "missing_records_vs_declared_size": expected_bytes // 64 - records if expected_bytes else None,
                "limits": ["Only individual64-byte FNV-valid records are recovered; this does not verify the original whole file.",
                           "Whole missing transport chunks can concatenate clean records without a byte-resync hole.",
                           "All sequence gaps are unknown for duration; normal event/raw sequence interleaving also creates gaps, which cannot be distinguished from missing transfer data here."]}
    (destination / "recovery.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return output, metadata


def align_journal_times(boots):
    """Anchor subsecond uptime to stable UTC offsets, retaining raw UTC for joins.

    Journal UTC is integer seconds. Summing only intervals where raw UTC changes
    would discard nearly all subsecond evidence, so duration always uses uptime.
    """
    for events in boots.values():
        segment, last = 0, None
        offsets = collections.defaultdict(list)
        for event in events:
            if event["epoch"] is not None:
                if last and abs(event["epoch"] - last["epoch"] -
                                (event["uptime_ms"] - last["uptime_ms"]) / 1000) > 3:
                    segment += 1
                offsets[segment].append(event["epoch"] - event["uptime_ms"] / 1000)
                last = event
            event["clock_segment"] = segment
        anchors = {key: statistics.median(values) for key, values in offsets.items()}
        for event in events:
            anchor = anchors.get(event["clock_segment"])
            event["aligned_epoch"] = anchor + event["uptime_ms"] / 1000 if anchor is not None else None


def journal_intervals(boots, maximum_gap, strict_sequence=False):
    align_journal_times(boots)
    intervals, summaries, transitions = [], [], []
    for boot, events in boots.items():
        if boot == 0:
            # Intake records deliberately use boot=0 and globally persistent IDs.
            # Their uptimes may come from different boots and cannot bound power.
            summaries.append({"boot": 0, "events": len(events), "kind": "persistent_intake_ledger",
                              "observed_s": 0, "omitted_gap_s": 0, "power_state_s": {},
                              "logged_cpu_ceiling_s": {}, "battery_samples": []})
            continue
        state, cpu, brightness, walking, external, valid = None, None, None, None, None, None
        durations = collections.Counter()
        cpu_times, external_times, valid_times = collections.Counter(), collections.Counter(), collections.Counter()
        counts = collections.Counter(sports_export.NAMES[event["type"]] if event["type"] < len(sports_export.NAMES) else str(event["type"]) for event in events)
        omitted_gap_s, observed_s, walking_active_s = 0, 0, 0
        sequence_gap_count, sequence_gap_span = 0, 0
        last_walk, last_raise, last_knock, previous, last_power_state = None, None, None, None, None
        battery = []
        correction_events = []
        for event in events:
            if previous:
                seconds = (event["uptime_ms"] - previous["uptime_ms"]) / 1000
                clock_ok = event["clock_segment"] == previous["clock_segment"]
                sequence_ok = not strict_sequence or event["sequence"] == previous["sequence"] + 1
                if not sequence_ok:
                    sequence_gap_count += 1
                    sequence_gap_span += max(0, event["sequence"] - previous["sequence"] - 1)
                    state = cpu = brightness = walking = external = valid = None
                if seconds >= 0 and seconds <= maximum_gap and clock_ok and sequence_ok:
                    durations[STATES.get(state, "unknown")] += seconds
                    cpu_times[str(cpu) if cpu is not None else "unknown"] += seconds
                    external_times[str(external) if external is not None else "unknown"] += seconds
                    valid_times[str(valid) if valid is not None else "unknown"] += seconds
                    observed_s += seconds
                    if walking and state == 0:
                        walking_active_s += seconds
                    if previous["aligned_epoch"] is not None and event["aligned_epoch"] is not None:
                        intervals.append({"boot": boot, "start": previous["aligned_epoch"], "end": event["aligned_epoch"],
                                          "seconds": seconds, "state": state, "cpu_ceiling": cpu,
                                          "panel_level": brightness if brightness is not None and 0 <= brightness <= 255 else None,
                                          "walking": walking, "external": external, "battery_valid": valid})
                elif seconds > 0:
                    omitted_gap_s += seconds
                    if not clock_ok:
                        correction_events.append({"sequence": event["sequence"], "uptime_ms": event["uptime_ms"],
                                                  "local_time": local_time(event["epoch"])})
            kind, values = event["type"], event["v"]
            if kind == 2 and all(math.isfinite(value) for value in values[:3]):
                new_state, new_cpu = int(values[0]), int(values[1])
                if last_power_state is not None and new_state != last_power_state:
                    transitions.append({"boot": boot, "uptime_ms": event["uptime_ms"], "epoch": event["aligned_epoch"],
                                        "raw_epoch": event["epoch"], "local_time": local_time(event["aligned_epoch"]), "old_state": STATES.get(last_power_state, str(last_power_state)),
                                        "new_state": STATES.get(new_state, str(new_state)), "walking": walking,
                                        "walk_event_age_s": (event["uptime_ms"] - last_walk) / 1000 if last_walk is not None else None,
                                        "raised_within_2s": last_raise is not None and 0 <= event["uptime_ms"] - last_raise <= 2000,
                                        "knock_within_2s": last_knock is not None and 0 <= event["uptime_ms"] - last_knock <= 2000})
                state = new_state if new_state in STATES else None
                last_power_state = state
                cpu = new_cpu if new_cpu in (80, 240) else None
                brightness = values[2]
            elif kind == 5:
                walking = bool(values[0]) if values[2] > 0 else None
                last_walk = event["uptime_ms"]
            elif kind == 7 and values[0] > 0:
                last_raise = event["uptime_ms"]
            elif kind == 6 and values[0] == 3:  # KNOCK_EV_DOUBLE; other reasons are rejected candidates
                last_knock = event["uptime_ms"]
            elif kind in (3, 22):
                external = int(values[2]) if math.isfinite(values[2]) else None
                valid = bool(values[3] > 0 and values[4] > 0)
                if values[5] in STATES:
                    state = int(values[5])
                battery.append({"boot": boot, "epoch": event["epoch"], "uptime_ms": event["uptime_ms"],
                                "percent": values[0], "rail_mv": values[1], "external": external,
                                "battery_valid": bool(values[3]), "sample_valid": bool(values[4]),
                                "mode": values[5], "age_ms": values[6] if kind == 3 else None})
            previous = event
        epochs = [event["epoch"] for event in events if event["epoch"] is not None]
        summaries.append({"boot": boot, "events": len(events), "counts": dict(counts),
                          "first_sequence": events[0]["sequence"], "last_sequence": events[-1]["sequence"],
                          "first_local": local_time(min(epochs)) if epochs else None,
                          "last_local": local_time(max(epochs)) if epochs else None,
                          "observed_s": observed_s, "omitted_gap_s": omitted_gap_s,
                          "power_state_s": dict(durations), "logged_cpu_ceiling_s": dict(cpu_times),
                          "external_power_s": dict(external_times), "battery_valid_s": dict(valid_times),
                          "walking_while_active_s": walking_active_s, "clock_corrections": correction_events,
                          "unknown_sequence_gaps": sequence_gap_count, "sequence_ids_between_received_records": sequence_gap_span,
                          "battery_samples": battery})
    return intervals, summaries, transitions


def match_csv_boots(points, boots):
    index = collections.defaultdict(list)
    for boot, events in boots.items():
        if boot == 0:
            continue
        for event in events:
            if event["epoch"] is not None:
                index[event["epoch"]].append(event)
    grouped = collections.defaultdict(list)
    for point in points:
        grouped[point["file"]].append(point)
    matches = []
    for name, rows in grouped.items():
        hits = collections.Counter()
        for point in rows:
            if point["epoch"] is None:
                continue
            candidates = set()
            for second in range(math.floor(point["epoch"]) - 1, math.floor(point["epoch"]) + 2):
                for event in index.get(second, ()):
                    if abs(event["epoch"] - point["epoch"]) <= 1 and abs(event["uptime_ms"] - point["uptime_ms"]) <= 1500:
                        candidates.add(event["boot"])
            hits.update(candidates)
        ranked = hits.most_common()
        selected = ranked[0][0] if ranked and (len(ranked) == 1 or ranked[0][1] > ranked[1][1]) else None
        matches.append({"file": name, "boot": selected, "matching_rows": ranked[0][1] if selected is not None else 0,
                        "candidates": dict(hits), "timed_rows": sum(row["epoch"] is not None for row in rows),
                        "confidence": "ambiguous" if ranked and selected is None else "unmatched" if not ranked else
                        "single_point" if ranked[0][1] == 1 else "multiple_points",
                        "tolerances": "UTC +/-1s and uptime +/-1500ms; no uptime-only matches"})
    return matches


def summarize_window(intervals, start, end):
    states, cpus, external, valid = (collections.Counter() for _ in range(4))
    observed, walking_active = 0, 0
    clock_ranges = []
    panels = collections.defaultdict(lambda: {"known_s": 0, "level_times_seconds": 0, "at_least_128_s": 0, "at_least_200_s": 0, "maximum": 0})
    active_fragments, active_fragment, last_active = [], 0, None
    for interval in intervals:
        overlap = max(0, min(end, interval["end"]) - max(start, interval["start"]))
        if not overlap or interval["end"] <= interval["start"]:
            continue
        # Uptime is authoritative for duration; UTC provides the clipping window.
        seconds = interval["seconds"] * overlap / (interval["end"] - interval["start"])
        states[STATES.get(interval["state"], "unknown")] += seconds
        cpus[str(interval["cpu_ceiling"]) if interval["cpu_ceiling"] is not None else "unknown"] += seconds
        external[str(interval["external"])] += seconds
        valid[str(interval["battery_valid"])] += seconds
        observed += seconds
        clock_ranges.append((max(start, interval["start"]), min(end, interval["end"])))
        if interval["walking"] and interval["state"] == 0:
            walking_active += seconds
        level = interval.get("panel_level")
        if level is not None:
            panel = panels[STATES.get(interval["state"], "unknown")]
            panel["known_s"] += seconds
            panel["level_times_seconds"] += level * seconds
            panel["maximum"] = max(panel["maximum"], level)
            if level >= 128:
                panel["at_least_128_s"] += seconds
            if level >= 200:
                panel["at_least_200_s"] += seconds
        left, right = max(start, interval["start"]), min(end, interval["end"])
        if interval["state"] == 0:
            if last_active is not None and (last_active[0] != interval["boot"] or abs(last_active[1] - left) > .001):
                active_fragments.append(active_fragment);active_fragment = 0
            active_fragment += seconds
            last_active = interval["boot"], right
        elif active_fragment:
            active_fragments.append(active_fragment);active_fragment = 0;last_active = None
    if active_fragment:
        active_fragments.append(active_fragment)
    for panel in panels.values():
        panel["weighted_mean_level"] = panel.pop("level_times_seconds") / panel["known_s"] if panel["known_s"] else None
    union, right = 0, None
    for left, finish in sorted(clock_ranges):
        union += max(0, finish - max(left, right if right is not None else left))
        right = max(finish, right if right is not None else finish)
    clock_sum = sum(finish - left for left, finish in clock_ranges)
    return {"observed_s": round(observed, 3), "distinct_clock_coverage_s": round(union, 3),
            "overlapping_clock_evidence_s": round(max(0, clock_sum - union), 3),
            "power_state_s": dict(states), "logged_cpu_ceiling_s": dict(cpus),
            "panel_brightness_by_state": dict(panels),
            "active_run_fragments": {"count": len(active_fragments), "median_s": statistics.median(active_fragments) if active_fragments else None,
                                     "maximum_s": max(active_fragments, default=None), "over_60s": sum(value > 60 for value in active_fragments),
                                     "durations_s": active_fragments, "limit": "Fragments split at evidence gaps/boot boundaries; interaction and keep-awake causes are not logged."},
            "external_power_s": dict(external), "battery_valid_s": dict(valid),
            "walking_while_active_s": round(walking_active, 3)}


def workout_windows(directory, points, intervals):
    results = []
    for name in ("0002.csv", "0005.csv"):
        path = directory / name
        if not path.exists():
            continue
        with path.open(newline="") as source:
            stamps = [int(row["utc_ms"]) / 1000 for row in csv.DictReader(source)]
        stamps = [epoch for epoch in stamps if epoch_valid(epoch)]
        if not stamps:
            continue
        start, end = stamps[0], stamps[-1]
        rails = [point for point in points if point["epoch"] is not None and start <= point["epoch"] <= end]
        results.append({"file": name, "sport_context": "User-confirmed badminton, watch on left wrist, playing right-handed",
                        "first_local": local_time(start), "last_local": local_time(end), "duration_s": end - start,
                        "csv_rail_samples": rails, "journal_coverage": summarize_window(intervals, start, end)})
    return results


def point_observations(boots, start, end):
    powers, batteries = [], []
    for boot, events in boots.items():
        if not boot:
            continue
        for event in events:
            epoch = event.get("aligned_epoch")
            if epoch is not None and start <= epoch <= end:
                if event["type"] == 2:
                    powers.append(event)
                elif event["type"] == 3:
                    batteries.append(event)
    valid = [event for event in batteries if event["v"][3] > 0 and event["v"][4] > 0 and event["v"][2] == 0]
    low = [event for event in valid if event["v"][0] <= 15]
    panels = collections.defaultdict(collections.Counter)
    for event in powers:
        panels[STATES.get(int(event["v"][0]), "unknown")][str(int(event["v"][2]))] += 1
    return {
        "power_records": len(powers), "power_state_record_counts": dict(collections.Counter(STATES.get(int(event["v"][0]), "unknown") for event in powers)),
        "cpu_ceiling_record_counts": dict(collections.Counter(str(int(event["v"][1])) for event in powers)),
        "panel_level_record_counts_by_state": {state: dict(counts) for state, counts in panels.items()},
        "sensor_idle_at_80_panel_level_counts": dict(collections.Counter(str(int(event["v"][2])) for event in powers if event["v"][0] == 1 and event["v"][1] == 80)),
        "battery_records": len(batteries), "external_flag_record_counts": dict(collections.Counter(str(int(event["v"][2])) for event in batteries)),
        "valid_inferred_battery_records": len(valid),
        "valid_percent_minimum": min((event["v"][0] for event in valid), default=None),
        "valid_percent_maximum": max((event["v"][0] for event in valid), default=None),
        "valid_low_battery_record_count": len(low),
        "valid_low_battery_mode_counts": dict(collections.Counter(STATES.get(int(event["v"][5]), "unknown") for event in low)),
        "valid_low_battery_observations": [{"boot": event["boot"], "sequence": event["sequence"], "local_time": local_time(event["aligned_epoch"]),
                                           "percent": event["v"][0], "rail_mv": event["v"][1], "mode": STATES.get(int(event["v"][5]), "unknown")} for event in low],
        "limit": "Counts describe independently received observations, not time occupancy. Missing events can hide intervening state changes.",
    }


def render(points, intervals, workouts, destination, maximum_gap, boots=None):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.dates as dates
    import matplotlib.pyplot as plt
    from matplotlib.collections import LineCollection
    from matplotlib.lines import Line2D
    figure, axes = plt.subplots(4, 1, figsize=(14, 10.5), sharex=True,
                                gridspec_kw={"height_ratios": [3, 1.2, 1, .9]})
    run_axis = axes[-1]
    groups = collections.defaultdict(list)
    for point in points:
        if point["epoch"] is not None:
            groups[point["file"]].append(point)
    for name, rows in groups.items():
        for previous, current in zip(rows, rows[1:]):
            elapsed = (current["uptime_ms"] - previous["uptime_ms"]) / 1000
            if 0 <= elapsed <= maximum_gap and current["clock_segment"] == previous["clock_segment"]:
                axes[0].plot([dt.datetime.fromtimestamp(row["epoch"], IST) for row in (previous, current)],
                             [previous["rail_mv"] / 1000, current["rail_mv"] / 1000], color="#285b83", linewidth=1)
        for inferred, marker, color in ((False, ".", "#285b83"), (True, "x", "#c27700")):
            selected = [row for row in rows if (row["time_source"] == "inferred_same_run_uptime") == inferred]
            axes[0].scatter([dt.datetime.fromtimestamp(row["epoch"], IST) for row in selected],
                            [row["rail_mv"] / 1000 for row in selected], s=16, marker=marker, color=color)
        first, last = rows[0]["epoch"], rows[-1]["epoch"]
        run_axis.plot([dt.datetime.fromtimestamp(value, IST) for value in (first, last)], [0, 0], color="#285b83", linewidth=5)
        run_axis.plot(dt.datetime.fromtimestamp(first, IST), 0, marker="|", color="black", markersize=14)
        if int(name[:4]) % 4 == 0 or name in ("0231.csv", "0246.csv", "0249.csv", "0252.csv"):
            run_axis.annotate(name.removesuffix(".csv"), (dt.datetime.fromtimestamp(first, IST), 0),
                             xytext=(0, 10 + 11 * (int(name[:4]) % 2)), textcoords="offset points", fontsize=8, rotation=60)
    colors = {0: "#ca4b45", 1: "#2c88b9", 2: "#555555", 3: "#333333", 4: "#dbb24d", None: "#cccccc"}
    if intervals and groups:
        start = min(row["epoch"] for rows in groups.values() for row in rows)
        end = max(row["epoch"] for rows in groups.values() for row in rows)
        for category, y in (("state", 1), ("cpu_ceiling", 0)):
            segments, inks = [], []
            previous_key = None
            for interval in intervals:
                left, right = max(start, interval["start"]), min(end, interval["end"])
                if right <= left:
                    continue
                value = interval[category]
                first = (dates.date2num(dt.datetime.fromtimestamp(left, IST)), y)
                last = (dates.date2num(dt.datetime.fromtimestamp(right, IST)), y)
                key = interval["boot"], value
                if segments and key == previous_key and abs(segments[-1][-1][0] - first[0]) < 1e-9:
                    segments[-1][-1] = last
                else:
                    segments.append([first, last])
                    inks.append(colors.get(value, "#cccccc") if category == "state" else "#2c88b9" if value == 80 else "#ca4b45" if value == 240 else "#cccccc")
                previous_key = key
            axes[1].add_collection(LineCollection(segments, colors=inks, linewidths=13))
        panels, inks, previous_key = [], [], None
        for interval in intervals:
            left, right = max(start, interval["start"]), min(end, interval["end"])
            level = interval.get("panel_level")
            if right <= left or level is None:
                continue
            first = (dates.date2num(dt.datetime.fromtimestamp(left, IST)), level)
            last = (dates.date2num(dt.datetime.fromtimestamp(right, IST)), level)
            key = interval["boot"], interval["state"], level
            if panels and key == previous_key and abs(panels[-1][-1][0] - first[0]) < 1e-9:
                panels[-1][-1] = last
            else:
                panels.append([first, last]);inks.append(colors.get(interval["state"], "#cccccc"))
            previous_key = key
        axes[2].add_collection(LineCollection(panels, colors=inks, linewidths=2))
    else:
        axes[1].text(.5, .5, "Sports journal not supplied: power state and CPU ceiling unknown", ha="center", transform=axes[1].transAxes)
        axes[2].text(.5, .5, "Logged panel levels unavailable", ha="center", transform=axes[2].transAxes)
    if boots and groups:
        start = min(row["epoch"] for rows in groups.values() for row in rows)
        end = max(row["epoch"] for rows in groups.values() for row in rows)
        powers = [event for boot, events in boots.items() if boot for event in events
                  if event["type"] == 2 and event.get("aligned_epoch") is not None and start <= event["aligned_epoch"] <= end]
        stamps = [dt.datetime.fromtimestamp(event["aligned_epoch"], IST) for event in powers]
        axes[1].scatter(stamps, [1] * len(powers), c=[colors.get(int(event["v"][0]), "#cccccc") for event in powers], marker="|", s=40, linewidths=1)
        axes[1].scatter(stamps, [0] * len(powers), c=["#2c88b9" if event["v"][1] == 80 else "#ca4b45" for event in powers], marker="|", s=40, linewidths=1)
        axes[2].scatter(stamps, [event["v"][2] for event in powers], c=[colors.get(int(event["v"][0]), "#cccccc") for event in powers], marker=".", s=8)
    for workout in workouts:
        start = dt.datetime.fromisoformat(workout["first_local"])
        end = dt.datetime.fromisoformat(workout["last_local"])
        if groups and min(row["epoch"] for rows in groups.values() for row in rows) <= start.timestamp() <= max(row["epoch"] for rows in groups.values() for row in rows):
            for axis in axes:
                axis.axvspan(start, end, color="#7f5aae", alpha=.13)
            axes[0].annotate("Badminton " + workout["file"], (start, axes[0].get_ylim()[1]), xytext=(5, -20), textcoords="offset points", color="#67449a", fontsize=9)
    axes[0].set(ylabel="System rail (V)", title="Watch power evidence — 20 October 2026, IST (UTC+05:30)")
    axes[0].legend(handles=[Line2D([], [], color="#285b83", marker=".", label="Recorded rail; lines only within the same file/run"),
                            Line2D([], [], color="#c27700", marker="x", linestyle="", label="Unset UTC inferred from same-run uptime")], loc="lower left", fontsize=8)
    axes[1].set(yticks=[0, 1], yticklabels=["CPU ceiling", "Power state"], ylim=(-.6, 1.6))
    axes[1].legend(handles=[Line2D([], [], color=color, linewidth=6, label=name) for name, color in
                           (("Active / 240 MHz ceiling", "#ca4b45"), ("Sensor idle / 80 MHz ceiling", "#2c88b9"),
                            ("Display off", "#555555"), ("AOD", "#dbb24d"))], loc="upper left", bbox_to_anchor=(0, 1.3), ncol=4, fontsize=8)
    axes[2].set(yticks=[0, 64, 128, 192, 255], ylim=(-5, 260), ylabel="Panel level\n0–255")
    run_axis.set(yticks=[], ylim=(-.5, 1.1), ylabel="CSV runs", xlabel="Local time (IST); gaps remain unobserved")
    run_axis.xaxis.set_major_formatter(dates.DateFormatter("%H:%M", tz=IST))
    for axis in axes:
        axis.grid(axis="x", alpha=.2)
    figure.text(.01, .01, "Rail is not a fuel gauge/current measurement. Power/panel markers are checked observations; lines require consecutive sequence IDs. Blanks remain unknown; CPU values are ceilings.", fontsize=8)
    figure.tight_layout(rect=(0, .035, 1, 1))
    figure.savefig(destination / "power-timeline.png", dpi=170)
    figure.savefig(destination / "power-timeline.svg")
    plt.close(figure)
    if workouts:
        figure, axes = plt.subplots(1, len(workouts), figsize=(6 * len(workouts), 4.2), squeeze=False)
        for axis, workout in zip(axes[0], workouts):
            start = dt.datetime.fromisoformat(workout["first_local"]).timestamp()
            rows = workout["csv_rail_samples"]
            axis.plot([(row["epoch"] - start) / 60 for row in rows], [row["rail_mv"] / 1000 for row in rows], ".-", color="#285b83")
            axis.set(title=f"Badminton {workout['file']}\n{workout['first_local'][11:19]}–{workout['last_local'][11:19]} IST",
                     xlabel="Minutes from recording start", ylabel="System rail (V)")
            axis.grid(alpha=.2)
            axis.text(.02, .03, "CSV " + ", ".join(sorted({row["file"] for row in rows})), transform=axis.transAxes, fontsize=9)
        figure.suptitle("Two user-confirmed sessions: left-wrist watch, right-handed play", fontsize=12)
        figure.tight_layout(rect=(0, .04, 1, .94))
        figure.text(.02, .01, "Minute rail samples provide context only; these are not current, capacity or workout energy measurements.", fontsize=9)
        figure.savefig(destination / "badminton-windows.png", dpi=170)
        figure.savefig(destination / "badminton-windows.svg")
        plt.close(figure)


def rail_segment(points, first_file, last_file, maximum_gap):
    rows = [point for point in points if first_file <= int(point["file"][:4]) <= last_file and point["epoch"] is not None]
    if not rows:
        return {"files": [first_file, last_file], "samples": 0}
    minimum = min(rows, key=lambda row: row["rail_mv"])
    observed = 0
    gaps = []
    for previous, current in zip(rows, rows[1:]):
        elapsed = current["epoch"] - previous["epoch"]
        if current["file"] == previous["file"] and current["clock_segment"] == previous["clock_segment"] and 0 <= elapsed <= maximum_gap:
            observed += (current["uptime_ms"] - previous["uptime_ms"]) / 1000
        else:
            gaps.append({"previous_file": previous["file"], "next_file": current["file"],
                         "previous_local": previous["local_time"], "next_local": current["local_time"], "sample_gap_s": elapsed})
    return {"first_file": rows[0]["file"], "last_file": rows[-1]["file"],
            "file_runs": len({row["file"] for row in rows}), "samples": len(rows),
            "first_local": rows[0]["local_time"], "last_local": rows[-1]["local_time"],
            "first_time_source": rows[0]["time_source"], "first_rail_mv": rows[0]["rail_mv"], "last_rail_mv": rows[-1]["rail_mv"],
            "minimum_rail_mv": minimum["rail_mv"], "minimum_local": minimum["local_time"],
            "rail_change_mv": rows[-1]["rail_mv"] - rows[0]["rail_mv"],
            "wall_elapsed_s": rows[-1]["epoch"] - rows[0]["epoch"], "observed_same_run_s": observed,
            "gaps_not_attributed_to_power_state_or_reset_cause": gaps,
            "journal_coverage": None, "interpretation": "Descriptive rail trend; CSV does not establish supply or state of charge."}


def analyze(source, destination, recent_from=None, maximum_gap=90, journal=None, spans=(), recovery=None):
    destination.mkdir(parents=True, exist_ok=True)
    files, points = read_csv_logs(source / "watch/logs")
    recent_files = [item for item in files if recent_from is None or int(item["file"][:4]) >= recent_from]
    names = {item["file"] for item in recent_files}
    recent = [point for point in points if point["file"] in names]
    boots, metadata = {}, None
    if journal and journal.exists():
        boots, metadata = read_journal(journal)
    intervals, sessions, transitions = journal_intervals(boots, maximum_gap, strict_sequence=recovery is not None)
    matches = match_csv_boots(points, boots) if boots else []
    timed = [point for point in recent if point["epoch"] is not None]
    window = (min(point["epoch"] for point in timed), max(point["epoch"] for point in timed)) if timed else None
    workouts = workout_windows(source / "imu", points, intervals)
    selected_spans = [rail_segment(points, first, last, maximum_gap) for first, last in spans]
    for span in selected_spans:
        if span.get("first_local"):
            span["journal_coverage"] = summarize_window(intervals, dt.datetime.fromisoformat(span["first_local"]).timestamp(),
                                                        dt.datetime.fromisoformat(span["last_local"]).timestamp())
            span["journal_point_observations"] = point_observations(boots, dt.datetime.fromisoformat(span["first_local"]).timestamp(),
                                                                    dt.datetime.fromisoformat(span["last_local"]).timestamp())
    for workout in workouts:
        workout["journal_point_observations"] = point_observations(boots, dt.datetime.fromisoformat(workout["first_local"]).timestamp(),
                                                                   dt.datetime.fromisoformat(workout["last_local"]).timestamp())
    recent_transitions = [item for item in transitions if item["epoch"] is not None and window and window[0] <= item["epoch"] <= window[1]]
    wake_transitions = [item for item in recent_transitions if item["new_state"] == "active"]
    report = {
        "timezone": "IST UTC+05:30", "sources": {"csv_directory": str(source / "watch/logs"), "csv_files": len(files), "journal": metadata},
        "method": {"maximum_gap_s": maximum_gap, "csv_join": "UTC +/-1s AND uptime +/-1500ms; unique best boot; no uptime-only mapping",
                   "durations": "Only adjacent checked records from the same boot, within the gap bound and without clock correction. No interpolation across boots or omitted gaps.",
                   "strict_sequence_gaps": recovery is not None,
                   "recovered_duration_rule": "For recovered input every nonconsecutive sequence ID invalidates held state/CPU/panel/battery/walking evidence until independently recorded again. Raw/event interleaving prevents reliable occupancy/timeout-duration estimates from this incomplete event-only transfer.",
                   "cpu": "SP_POWER CPU MHz is the requested ceiling logged by firmware; DFS may run below it while locks are released.",
                   "journal_time": "Subsecond uptime is anchored by the median UTC-minus-uptime offset within each stable clock segment. Raw integer UTC is retained for matching; clock corrections split intervals.",
                   "csv_time": "Unset UTC retained as unknown unless anchored to valid UTC in the same monotonic run; inferred rows are marked."},
        "selected_rail_segments": selected_spans,
        "recovery": recovery,
        "current_preserved_preferences": preserved_preferences(source.parent / "prefs-before"),
        "current_source_gauge_rules": {
            "source": "firmware/main/hal_esp32.c; this describes the workspace source, not proof of an older installed binary",
            "rail_not_fuel_gauge": True, "calibration_mv": "Stored calibration is unknown in these CSV files; allowed range +/-400mV.",
            "percent_curve_input": "Smoothed rail plus stored calibration plus25mV awake or10mV asleep when battery-valid; external supply percentage is invalid.",
            "battery_valid_gate": "Calibrated ADC, at least5/8 valid reads, measured and smoothed rail3300..4250mV, spread<=120mV, ten stable samples, no observed external supply.",
            "external_evidence": "USB host SOF or optional charge-status pin; calibrated raw rail>=4400mV for3 stable samples also marks external.",
            "stale_gate": "Cached samples older than3s become sample-invalid/battery-invalid with external unknown.",
        },
        "recent": {"files": recent_files, "first_local": local_time(window[0]) if window else None, "last_local": local_time(window[1]) if window else None,
                   "samples": len(recent), "file_runs": len(recent_files),
                   "journal_point_observations": point_observations(boots, *window) if window else None,
                   "journal_coverage": summarize_window(intervals, *window) if window else None,
                   "wake_relation_summary": {
                       "idle_to_active_transitions": len(wake_transitions),
                       "walking_at_transition": sum(bool(item["walking"]) for item in wake_transitions),
                       "walking_without_nearby_recorded_raise_or_knock": sum(bool(item["walking"]) and not item["raised_within_2s"] and not item["knock_within_2s"] for item in wake_transitions),
                       "recorded_raise_within_2s": sum(item["raised_within_2s"] for item in wake_transitions),
                       "recorded_knock_within_2s": sum(item["knock_within_2s"] for item in wake_transitions),
                       "limitation": "Association only: legacy event journals omit command/touch reasons, so absent raise/knock is not proof of wake cause. Missing sequence IDs invalidate walking state; zero walking matches is not evidence that walking was absent.",
                   },
                   "power_transitions": recent_transitions},
        "csv_files": files, "csv_journal_matches": matches, "journal_boots": sessions, "workouts": workouts,
        "limits": ["CSV system rail does not identify supply, battery state of charge, charge current, capacity or remaining runtime.",
                   "Legacy logs contain no reset reason: short uptime runs cannot distinguish panic, watchdog, manual reset, flashing or power cycling.",
                   "CSV file changes can also follow SD remount/eject or size rotation; individual file/uptime runs are reported without inventing reset causes.",
                   "Journal external=0 means battery inferred from no observed external evidence, not a current-sense proof of unplugging; an unobserved wall charger remains possible.",
                   "No mAh/current inferred from rail voltage. Gaps remain unknown, and source-code behavior alone does not establish the installed binary's exact behavior."],
    }
    (destination / "report.json").write_text(json.dumps(report, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    with (destination / "rail-samples.csv").open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=["file", "line", "uptime_ms", "raw_epoch", "epoch", "local_time", "time_source", "clock_segment", "rail_mv", "imu_valid"])
        writer.writeheader();writer.writerows(recent)
    render(recent, intervals, workouts, destination, maximum_gap, boots)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--recent-from", type=int)
    parser.add_argument("--maximum-gap", type=float, default=90)
    parser.add_argument("--journal", type=pathlib.Path)
    parser.add_argument("--span", action="append", default=[], help="Explicit CSV file-number interval FIRST:LAST for a rail-trend summary")
    parser.add_argument("--recover-unverified", type=pathlib.Path)
    parser.add_argument("--expected-bytes", type=int)
    parser.add_argument("--expected-crc32")
    parser.add_argument("--rejected-lines", type=int)
    args = parser.parse_args()
    journal = args.journal or args.source / "watch/sports/20261020-events.spt"
    recovery = None
    if args.recover_unverified:
        args.out.mkdir(parents=True, exist_ok=True)
        journal, recovery = recover_journal(args.recover_unverified, args.out, args.expected_bytes, args.expected_crc32, args.rejected_lines)
    spans = [tuple(map(int, item.split(":"))) for item in args.span]
    report = analyze(args.source, args.out, args.recent_from, args.maximum_gap, journal, spans, recovery)
    print(json.dumps({"recent": {key: value for key, value in report["recent"].items() if key not in ("files", "power_transitions")},
                      "journal_integrity": report["sources"]["journal"], "outputs": str(args.out)}, indent=2))


if __name__ == "__main__":
    main()
