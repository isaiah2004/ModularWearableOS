"""Root-run, identity-bound Apps/Settings presentation benchmark.

Import and the default invocation are offline. Only --run with an explicit
--port opens serial. No unlock, preference write, flash, erase or OTA command is
implemented. Production protocol/build-verification modules are reused as-is.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import re
import sys
import time

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build/alpha05/checks/target-benchmark"
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from tools.host_support import write_json
from tools.host_transport import (CommandRejected, CommandTimeout, DeviceError,
                                 HostTransport, ProtocolError, SerialLines,
                                 jsonl_trace)

READONLY_RECORDS = {"hostcaps": "NOSHOST", "info": "NOSBUILD", "state": "NOSSTATE",
                    "runtime_state": "NOSRUNTIME", "nav_state": "NOSNAV",
                    "scroll_state": "NOSSCROLL", "prefs": "NOSPREFS", "diag log": "NOSLOG"}
READONLY = frozenset(READONLY_RECORDS)
GLOBAL_KEYS = ("awake", "perf", "trace", "mblur", "layers", "history", "uniform",
               "packed", "worker", "qz", "profile", "full", "light", "shade",
               "sun", "pipeline", "card_budget_us", "detail", "proxy",
               "pipeline_probe", "settings_shutter", "rotation", "damage_bands",
               "cache_packed", "native_stage")
EXTRA_FLAGS = {"NOSPACING": "mode", "NOSNATIVE": "rom", "NOSHISTORY": "native_copy",
               "NOSHISTORYV": "enabled", "NOSSCHED": "mode", "NOSCARRY": "enabled"}
REQUIRED_ZERO = ("ota_busy", "shutdown_pending", "prefs_loading", "prefs_dirty",
                 "imu_pending", "imu_hz", "imu_queued", "imu_file_busy",
                 "workout_dirty", "workout_busy", "workout_uncertain",
                 "workout_corrupt", "workout_foreign", "track_active",
                 "track_pending", "track_saving", "audio_active", "touch_down",
                 "touch_pending", "button_down", "injected", "asset_busy", "radio_pending")
WORKLOADS = (
    {"name": "slow", "distance_px": 150, "duration_ms": 2000, "speed_px_s": 75,
     "tail_s": 1.2},
    {"name": "typical", "distance_px": 200, "duration_ms": 600,
     "speed_px_s": 200000 / 600, "tail_s": 1.2},
    {"name": "fling", "distance_px": 300, "duration_ms": 120,
     "speed_px_s": 2500, "tail_s": 2.2},
)


class AdmissionError(RuntimeError):
    pass


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def sha256_file(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def records(lines):
    """Keep repeated records (e.g. the two NOSPIPEFRAME roles) separately."""
    result = {}
    for line in lines:
        parts = line.split()
        if not parts:
            continue
        # Legacy human-readable counters repeat keys for horizontal/vertical
        # sections. Only the machine NOS records have this unique-key schema.
        if not re.fullmatch(r"NOS[A-Z0-9_]+", parts[0]):
            continue
        fields = {}
        for part in parts[1:]:
            if "=" not in part:
                continue
            key, value = part.split("=", 1)
            if key in fields:
                raise AdmissionError(f"Duplicate field {key}: {line}")
            fields[key] = value
        result.setdefault(parts[0], []).append(fields)
    return result


def one(table, name):
    matches = table.get(name, [])
    if len(matches) != 1:
        raise AdmissionError(f"Expected exactly one {name} record, received {len(matches)}")
    return matches[0]


def integer(record, key):
    value = record.get(key, "")
    if not re.fullmatch(r"-?\d+", value):
        raise AdmissionError(f"Missing/invalid integer {key}={value!r}")
    return int(value)


def require_equal(record, key, expected):
    actual = integer(record, key)
    if actual != expected:
        raise AdmissionError(f"{key}={actual}, required {expected}")


def check_identity(info_lines, identity):
    build = one(records(info_lines), "NOSBUILD")
    expected_elf = identity.get("firmware_elf_sha256", "")
    expected_source = identity.get("source_tree_sha256", "")
    for key, expected in (("elf_sha256", expected_elf), ("source_sha256", expected_source)):
        if not re.fullmatch(r"[0-9a-f]{64}", expected) or expected == "0" * 64:
            raise AdmissionError(f"Invalid local expected {key}")
        actual = build.get(key, "")
        if not re.fullmatch(r"[0-9a-f]{64}", actual) or actual != expected:
            raise AdmissionError(f"Exact build mismatch: {key} expected={expected} actual={actual}")
    if build.get("source_build_id") != expected_source[:16]:
        raise AdmissionError("Short source build ID does not match the full verified source hash")
    if build.get("metrics") != "nos-device-v1":
        raise AdmissionError("Unsupported target metrics schema")
    require_equal(build, "cpu_mhz", 240)
    return build


def check_runtime(runtime, *, capture_live=False):
    require_equal(runtime, "v", 1)
    for key in REQUIRED_ZERO:
        require_equal(runtime, key, 0)
    # Conservative benchmark admission: a terminal recorder error needs operator
    # review too, even though the backend may already have closed its file.
    require_equal(runtime, "imu_state", 0)
    require_equal(runtime, "workout_ready", 1)
    state = integer(runtime, "workout_state")
    save = integer(runtime, "workout_save")
    if state not in (0, 3) or save not in (0, 2) or (state == 3 and save != 2):
        raise AdmissionError(f"Workout is active/unknown/unsaved: state={state} save={save}")
    if integer(runtime, "audio_status") not in (0, 3):
        raise AdmissionError("WAV recorder is active, finalizing, failed or unknown")
    if not capture_live and integer(runtime, "capture_state") not in (0, 3, 4, 5):
        raise AdmissionError("A capture is already live or has an unknown state")


def snapshot(state_lines, runtime_lines, nav_lines, prefs_lines, log_lines):
    table = records(state_lines)
    log_table = records(log_lines)
    return {"state": one(table, "NOSSTATE"), "sections": table,
            "runtime": one(records(runtime_lines), "NOSRUNTIME"),
            "nav": one(records(nav_lines), "NOSNAV"),
            "prefs": one(records(prefs_lines), "NOSPREFS"),
            "log": one(log_table, "NOSLOG"),
            "faults": log_table.get("NOSFAULT", []),
            "evidence": log_table.get("NOSEVIDENCE", [])}


def preserved_flags(snap):
    state, table = snap["state"], snap["sections"]
    result = {key: integer(state, key) for key in GLOBAL_KEYS}
    for section, field in EXTRA_FLAGS.items():
        result[section + "." + field] = integer(one(table, section), field)
    return result


def prefs_fingerprint(prefs):
    for key in ("ok", "stored", "match"):
        require_equal(prefs, key, 1)
    for key in ("hash", "stored_hash"):
        if not re.fullmatch(r"[0-9a-fA-F]{8}", prefs.get(key, "")):
            raise AdmissionError("Missing complete preference checksum")
    if prefs["hash"].lower() != prefs["stored_hash"].lower():
        raise AdmissionError("RAM/SD preference checksums differ")
    return (prefs["hash"].lower(), prefs["stored_hash"].lower())


def check_snapshot(snap, *, initial=False):
    state, nav, table = snap["state"], snap["nav"], snap["sections"]
    render = one(table,"NOSRENDER") if "NOSRENDER" in table else None
    sharp = render is not None and integer(render,"sharp")==1
    if sharp:
        require_equal(render,"sprite_interpolation",0)
        require_equal(render,"history_storage_bytes",0)
    for key, value in (("pwr", 0), ("locked", 0), ("tx", 0), ("perf", 0),
                       ("trace", 0), ("mblur", 0 if sharp else 2), ("history", 0 if sharp else 1), ("full", 0)):
        require_equal(state, key, value)
    check_runtime(snap["runtime"])
    prefs_fingerprint(snap["prefs"])
    require_equal(one(table, "NOSPACING"), "mode", 1)
    require_equal(one(table, "NOSNATIVE"), "rom", 1)
    require_equal(one(table, "NOSNATIVE"), "compatible", 1)
    display = one(table, "NOSDISPLAY")
    require_equal(display, "fault", 0)
    if display.get("errors") != "0,0,0":
        raise AdmissionError("Panel command acknowledgments have errors")
    for name in ("brightness", "invert", "sleep"):
        if integer(display, name + "_requested") != integer(display, name + "_applied"):
            raise AdmissionError(f"Panel {name} has an unacknowledged request")
    if integer(display, "sleep_applied") or integer(display, "brightness_applied") <= 0:
        raise AdmissionError("Panel is sleeping/dark")
    boot = snap["log"].get("boot", "")
    if not re.fullmatch(r"[0-9a-fA-F]{8}", boot) or int(boot, 16) == 0:
        raise AdmissionError("Missing boot identity")
    require_equal(snap["log"], "ready", 1)
    require_equal(snap["log"], "available", 1)
    preserved_flags(snap)  # Missing flags are unknown, never treated as defaults.
    if initial:
        require_equal(state, "pipeline_probe", 0)
        if state.get("top") != "HOME":
            raise AdmissionError("Start unlocked on HOME, settled on face page0 or embedded Apps page1")
        page = integer(nav, "page")
        if page not in (0, 1) or integer(nav, "offset_q8") != page * 466 * 256:
            raise AdmissionError("HOME is not settled on page0/page1")
        require_equal(nav, "dragging", 0)


def capture_statistics(lines, app, minimum_intervals=10, *, sharp=False):
    table = records(lines)
    cap, blur, cache = (one(table, name) for name in ("NOSCAPTURE", "NOSBLURCAP", "NOSCACHECAP"))
    values = {key: integer(cap, key) for key in
              ("state", "frames", "painted", "intervals", "e2e_max_us", "present_max_us",
               "deadline", "floor", "p50_ms", "p95_ms", "p99_ms", "start_us", "completed_us")}
    if values["state"] != 3 or values["intervals"] < minimum_intervals:
        raise AdmissionError("Incomplete/insufficient painted capture")
    if not (0 < values["intervals"] < values["painted"] <= values["frames"]):
        raise AdmissionError("Invalid consecutive-painted interval counts")
    if values["frames"] >= 1 << 32:
        raise AdmissionError("Capture frame count exceeds the target counter range")
    if not (0 <= values["floor"] <= values["intervals"] and
            0 < values["p50_ms"] <= values["p95_ms"] <= values["p99_ms"] <=
            math.ceil(values["present_max_us"] / 1000)):
        raise AdmissionError("Invalid presentation statistics")
    if (values["present_max_us"] > 33333) != (values["floor"] > 0):
        raise AdmissionError("Hard-floor count and exact maximum disagree")
    if values["completed_us"] <= values["start_us"]:
        raise AdmissionError("Capture clock did not advance")
    for key in ("first_app", "last_app"):
        require_equal(blur, key, app)
    require_equal(blur, "state", 3)
    require_equal(blur, "axis", 2)
    require_equal(blur, "foreign_frames", 0)
    require_equal(blur, "allocation_failures", 0)
    if integer(blur, "max_q8") - integer(blur, "min_q8") < 60 * 256:
        raise AdmissionError("Capture did not cover at least 60 px of actual list motion")
    if integer(blur, "pose_changed") <= 0 or integer(blur, "drag_frames") <= 0:
        raise AdmissionError("Capture lacks actual drag/pose evidence")
    for key in ("stale_frames", "unknown_requests"):
        require_equal(cache, key, 0)
    require_equal(cache, "v", 1)
    require_equal(cache, "state", 3)
    for key in ("frames", "painted"):
        require_equal(cache, key, values[key])
    require_equal(cache, "processed_not_unique", 1)
    require_equal(cache, "final_visible", 0)
    counts = {key: integer(cache, key) for key in
              ("requested_frames", "sharp_frames", "blurred_frames", "native_frames", "native_blurred_frames")}
    areas = {key: integer(cache, key) for key in
             ("requested_processed_area", "sharp_processed_area", "blurred_processed_area",
              "native_processed_area", "native_blurred_processed_area")}
    if any(not 0 <= count <= values["frames"] for count in counts.values()):
        raise AdmissionError("Cache/native frame count is outside the captured population")
    if any(not 0 <= area < 1 << 64 for area in areas.values()):
        raise AdmissionError("Invalid cache/native processed area")
    for count_key, area_key in (("requested_frames", "requested_processed_area"),
                                ("sharp_frames", "sharp_processed_area"),
                                ("blurred_frames", "blurred_processed_area"),
                                ("native_frames", "native_processed_area"),
                                ("native_blurred_frames", "native_blurred_processed_area")):
        count, area = counts[count_key], areas[area_key]
        if (count == 0) != (area == 0) or area < count:
            raise AdmissionError("Inconsistent processed-area/frame evidence: " + count_key)
    if (max(counts["sharp_frames"], counts["blurred_frames"]) > counts["requested_frames"] or
            areas["sharp_processed_area"] + areas["blurred_processed_area"] > areas["requested_processed_area"] or
            counts["native_blurred_frames"] > counts["native_frames"] or
            areas["native_blurred_processed_area"] > areas["native_processed_area"]):
        raise AdmissionError("Processed source subsets exceed their observed populations")
    for key in ("pose_changed", "drag_frames", "v_filter_frames", "blend_frames"):
        if not 0 <= integer(blur, key) <= values["frames"]:
            raise AdmissionError("Invalid moving-frame evidence: " + key)
    if sharp:
        for key in ("history_frames","history_pixels","history_blended","h_filtered_pixels","v_filtered_pixels","h_filter_frames","v_filter_frames"):
            require_equal(blur,key,0)
        for key in ("blurred_frames","native_blurred_frames","blurred_processed_area","native_blurred_processed_area"):
            require_equal(cache,key,0)
    elif app in (7, 8):
        if not counts["native_blurred_frames"] or not areas["native_blurred_processed_area"]:
            raise AdmissionError("No prepared native blurred-raster evidence in the moving capture")
    elif not any(integer(record, key) > 0 for record, key in
                 ((blur, "v_filter_frames"), (blur, "blend_frames"), (cache, "blurred_frames"))):
        raise AdmissionError("No observed blur work in the moving capture")
    return {"capture": values, "p50_ms": values["p50_ms"], "p95_ms": values["p95_ms"],
            "p99_ms": values["p99_ms"], "max_ms": values["present_max_us"] / 1000,
            "over_33_333333ms": values["floor"], "intervals": values["intervals"],
            "quantile_resolution_us": 1000, "quantiles": "ceil-ms nearest-rank histogram; overflow bin uses ceil(max)",
            "interval_population": "consecutive painted UI iterations after final DMA fence; idle breaks the chain",
            "optical_fence": False, "blur": blur, "cache": cache,
            "native_observation": "prepared raster draw calls with caller blur intent; overlapping processed area, not unique/final-visible/optical coverage or proof of nonidentity pixels; direct sharp fast path omitted",
            "pipeline": table.get("NOSPIPE", []), "pipeline_frames": table.get("NOSPIPEFRAME", []),
            "cost": table.get("NOSCOST", []), "cost_buckets": table.get("NOSCOSTBUCKET", []),
            "cost_frames": table.get("NOSCOSTFRAME", [])}


def painted_interval_statistics(frames):
    """Offline trace fixture: unknown frame gaps and idle break the chain.

    Actual target acceptance uses the quiet C capture, not per-frame USB trace.
    This helper makes the population/ceil-bin rules independently testable.
    """
    intervals, previous, previous_id = [], None, None
    for frame in frames:
        frame_id, completed, painted = frame["frame_id"], frame["completed_us"], frame["px"] > 0
        consecutive = previous_id is not None and frame_id == (previous_id + 1) % (1 << 32)
        if painted and previous is not None and consecutive:
            if completed <= previous:
                raise AdmissionError("Non-increasing completion clock")
            intervals.append(completed - previous)
        previous = completed if painted else None
        previous_id = frame_id
    if not intervals:
        raise AdmissionError("No consecutive painted intervals")
    maximum = max(intervals)
    histogram = [0] * 64
    for interval in intervals:
        histogram[min(63, math.ceil(interval / 1000))] += 1
    def quantile(pct):
        rank, count = math.ceil(len(intervals) * pct / 100), 0
        for index, amount in enumerate(histogram):
            count += amount
            if count >= rank:
                return index if index < 63 else math.ceil(maximum / 1000)
        raise AssertionError("histogram")
    return {"intervals": len(intervals), "p50_ms": quantile(50), "p95_ms": quantile(95),
            "p99_ms": quantile(99), "present_max_us": maximum,
            "floor": sum(interval > 33333 for interval in intervals), "intervals_us": intervals}


def plan(repeat=3, history_ab=False, pipeline_profile=False):
    variants = ([{"label": "diagnostic-history0-A", "history_virtual": 0},
                 {"label": "diagnostic-history1-B", "history_virtual": 1},
                 {"label": "diagnostic-history0-A-repeat", "history_virtual": 0}]
                if history_ab else [{"label": "acceptance-current-flags", "history_virtual": None}])
    passes = [{"label": "quiet", "pipeline_probe": None}]
    if pipeline_profile:
        passes.append({"label": "diagnostic-pipeline-probe", "pipeline_probe": 1})
    return {"schema": 1, "target_run_executed": False, "repeat": repeat,
            "workloads": WORKLOADS, "directions": ["up", "down"],
            "apps": [{"name": "apps", "pose_app": 7, "route": ["home", "nav_state", "swipe 380 230 90 230 300", "nav_state"],
                      "required": "NOSSTATE top=HOME, NOSNAV page1 offset_q8=119296, NOSSCROLL app7 axis2"},
                     {"name": "settings", "pose_app": 8, "route": ["home", "open Settings", "state", "scroll_state"],
                      "required": "real SETTINGS screen, NOSSCROLL app8 axis2"}],
            "variants": variants, "passes": passes,
            "window_count": len(variants) * len(passes) * repeat * 2 * 2 * len(WORKLOADS),
            "preflight_commands": ["hostcaps", "info", "state", "runtime_state", "nav_state", "prefs", "diag log"],
            "window_commands": ["state", "runtime_state", "nav_state", "scroll_state", "diag log", "bench begin",
                                "swipe 233 Y0 233 Y1 DURATION_MS", "bench end", "state", "runtime_state", "nav_state", "scroll_state", "diag log"],
            "statistics": {"population": "consecutive painted completed-DMA presentation intervals; idle breaks chain",
                           "quantiles": "p50/p95/p99 nearest-rank ceil1ms bins; overflow>=63ms uses ceil(exact max)",
                           "max": "exact integer microseconds", "hard_floor": "count interval_us>33333 (30Hz threshold 1000000/30us)",
                           "pooling": "sum counts/max only; retain each window's quantiles, never average or pretend pooled percentiles",
                           "native_blur_evidence": "Apps7/Settings8 require positive native blurred frames and processed area; prepared-raster/caller-intent counters include overlap and omit direct sharp fast path, and cannot establish nonidentity pixels or unique/final-visible/optical/every-moving-frame coverage",
                           "not_measured": "optical panel latch / photon timing"},
            "safety": ["exact64 ELF + source + short source ID, finalized local source/artifact verification",
                       "explicit port and --run; no discovery/import-time serial",
                       "reject locked/unknown/active owner state before any mutation; require clean matching SD preferences",
                       "no unlock, erase, flash, OTA, preference write, recorder stop, or sensor lease-renewing query",
                       "only awake and optional history_virtual/pipeline_probe flags may change; snapshot and restore observed values",
                       "mutations execute once; uncertain outcome aborts workload; resync/reconnect before cleanup",
                       "cleanup checks same ELF/source/boot, reads actual values, then compensates once and verifies",
                       "physical touch/phone/other host interference invalidates evidence; use exclusive quiet bench custody"],
            "cleanup_commands": ["<NUL> newline + fresh correlated hostcaps barrier if any rejected/incomplete command",
                                 "info", "diag log", "state", "runtime_state", "bench end only if owned capture still live",
                                 "history_virtual ORIGINAL only if observed different", "pipeline_probe ORIGINAL only if observed different",
                                 "home + verified horizontal swipe to original HOME page only if safe",
                                 "awake toggle only if observed different", "state", "runtime_state", "nav_state", "prefs", "diag log"]}


class PinnedTransport(HostTransport):
    """Strict benchmark-only capability barrier; no legacy/guessed admission."""

    def resynchronize(self, timeout=10):
        import secrets

        previous = self.active
        retired = {getattr(self, "_barrier_id", None), (previous or {}).get("id"),
                   (self.last_result or {}).get("id")}
        for _ in range(8):
            nonce = secrets.token_hex(4)
            if re.fullmatch(r"[0-9a-f]{8}", nonce) and nonce not in retired:
                break
        else:
            raise ProtocolError("Could not allocate a fresh capability nonce")
        self._barrier_id = nonce
        probe = f"req {nonce} hostcaps"
        state = {"id": nonce, "command": "hostcaps", "begun": False,
                 "lines": [], "ok": None}
        end = time.monotonic() + timeout
        self._record("tx", "<NUL> <newline> (resynchronize)")
        self.active = state
        self._record("tx", probe)
        try:
            # One packet avoids an intermediate host-side dispatch by default.
            # The optional diagnostic adapter paces its bytes. Neither mode is
            # a USB atomicity guarantee: only the nonce ACK proves completion.
            self.link.write(b"\x00\n" + probe.encode("ascii") + b"\n")
            capabilities = None
            while time.monotonic() < end:
                reply = self.link.readline(max(0, end - time.monotonic()))
                if not reply:
                    continue
                self._record("rx", reply)
                if reply == "BEGIN " + nonce:
                    if state["begun"]:
                        raise ProtocolError("Duplicate capability BEGIN")
                    state["begun"] = True
                    continue
                if reply.startswith("DONE " + nonce + " "):
                    if not state["begun"] or capabilities is None:
                        raise ProtocolError("Capability completion preceded its BEGIN/payload")
                    if reply != f"DONE {nonce} ok=1":
                        raise ProtocolError("Capability barrier was not acknowledged: " + reply)
                    state["lines"].append("NOSHOST " + " ".join(f"{k}={v}" for k, v in capabilities.items()))
                    state["ok"] = True
                    self.capabilities, self.correlated = capabilities, True
                    self.active, self.last_result = None, state
                    return capabilities
                if not state["begun"]:
                    continue  # NUL rejection and stale/legacy records prove nothing.
                if reply.startswith("BEGIN "):
                    raise ProtocolError("Interleaved capability request envelope")
                if reply.startswith("NOSHOST "):
                    if capabilities is not None:
                        raise ProtocolError("Duplicate capability payload")
                    fields = {}
                    for token in reply.split()[1:]:
                        if token.count("=") != 1:
                            raise ProtocolError("Malformed capability field")
                        key, value = token.split("=", 1)
                        if not key or not value or key in fields:
                            raise ProtocolError("Duplicate/empty capability field")
                        fields[key] = value
                    maximum = fields.get("max_command", "")
                    if (fields.get("v") != "1" or fields.get("req") != "1" or
                            not re.fullmatch(r"[1-9]\d{0,2}", maximum) or
                            not len(probe) <= int(maximum) <= 191):
                        raise ProtocolError("Unsupported capability payload: " + reply)
                    capabilities = fields
                elif reply.startswith(("ERR ", "? ")):
                    raise ProtocolError("Capability request was rejected: " + reply)
            raise ProtocolError("Correlated capability barrier timed out; reconnect before continuing")
        except BaseException:
            # Keep an ambiguous mutation blocked, or retain this failed barrier.
            # A failed/partial exchange never upgrades capabilities or clears it.
            self.active = previous if previous is not None else state
            raise


class PacedSerialLines(SerialLines):
    """Opt-in diagnostic delivery; identical bytes, flushed at 2 ms intervals."""

    def write(self, data):
        for byte in data:
            super().write(bytes([byte]))
            time.sleep(0.002)


class Session:
    def __init__(self, port, output, *, link_factory=None, sleep=time.sleep, usb_paced_writes=False,
                 diagnostic_commands=()):
        self.port, self.output, self.sleep = port, Path(output), sleep
        permitted={"cost_calibrate","rotation_bench","cost_probe 0","cost_probe 1",
                   "rotation 0","rotation 1","rotation 2"}
        self.diagnostic_commands=frozenset(diagnostic_commands)
        if not self.diagnostic_commands<=permitted:
            raise ValueError("Unsupported additional diagnostic command")
        link_type = PacedSerialLines if usb_paced_writes else SerialLines
        self.link_factory = link_factory or (lambda: link_type.open(port, timeout=0.15, write_timeout=5))
        self.link = self.transport = None
        self.commands, self.recoveries = [], []
        self.tainted = False

    def persist(self):
        write_json(self.output / "commands.json", self.commands)
        write_json(self.output / "recovery.json", self.recoveries)

    def new_transport(self, known_correlated=False):
        self.link = self.link_factory()
        self.transport = PinnedTransport(self.link, require_done=True,
                                         trace=jsonl_trace(self.output / "wire.jsonl"))
        # Prior support is not acknowledgement of a newly opened connection.

    def strict_caps(self):
        if not self.transport.correlated or self.transport.capabilities.get("req") != "1":
            raise AdmissionError("Correlated hostcaps v1 is required; legacy fallback is disabled")

    def connect(self):
        self.new_transport()
        # Initial attempt plus at most one explicit same-port reopen. The
        # completed barrier itself negotiates; do not send another hostcaps.
        self.recover("initial strict correlated capability handshake")
        self.strict_caps()
        self.tainted = False

    def recover(self, reason):
        entry = {"utc": utc_now(), "reason": reason, "status": "pending"}
        self.recoveries.append(entry)
        self.tainted = True
        self.persist()
        known = bool(self.transport and self.transport.correlated)
        try:
            try:
                self.transport.resynchronize(timeout=8)
                entry["method"] = "NUL invalidation + fresh hostcaps barrier"
            except (OSError, TimeoutError) as exc:
                entry["barrier_error"] = str(exc)
                if self.link:
                    self.link.close()
                self.new_transport(known_correlated=known)
                self.transport.resynchronize(timeout=8)
                entry["method"] = "close/reopen explicit same port + fresh hostcaps barrier"
            self.strict_caps()
            self.tainted = False
            entry["status"] = "complete"
        except BaseException as exc:
            entry.update(status="failed", error=f"{type(exc).__name__}: {exc}")
            self.tainted = True
            raise
        finally:
            self.persist()

    def command(self, command, *, mutation=False, purpose=""):
        if not mutation and command not in READONLY:
            raise ValueError("Read-only command has not been reviewed: " + command)
        if mutation and not (command in {"awake", "home", "open Settings", "bench begin", "bench end"}
                             or command in self.diagnostic_commands
                             or re.fullmatch(r"(?:history_virtual|pipeline_probe) [01]", command)
                             or re.fullmatch(r"swipe \d+ \d+ \d+ \d+ \d+", command)):
            raise ValueError("Mutation is outside benchmark scope: " + command)
        if self.tainted:
            raise ProtocolError("Recovery must complete before another command")
        entry = {"utc": utc_now(), "command": command, "mutation": mutation,
                 "purpose": purpose, "status": "pending"}
        self.commands.append(entry)
        self.persist()
        try:
            # NEVER ask the shared transport to replay a mutation (or a query).
            lines = self.transport.execute(command, timeout=10, retries=0)
            entry.update(lines=lines, request_id=(self.transport.last_result or {}).get("id"))
            if not mutation:
                try:
                    record = one(records(lines), READONLY_RECORDS[command])
                    if command == "info":
                        for key in ("source_sha256", "elf_sha256"):
                            if not re.fullmatch(r"[0-9a-f]{64}", record.get(key, "")):
                                raise AdmissionError("Malformed NOSBUILD " + key)
                        if (record.get("source_build_id") != record["source_sha256"][:16] or
                                record.get("metrics") != "nos-device-v1"):
                            raise AdmissionError("Malformed NOSBUILD source_build_id/metrics")
                except AdmissionError as exc:
                    raise ProtocolError(f"Incomplete read-only reply for {command}: {exc}") from exc
            entry["status"] = "complete"
            return lines
        except BaseException as exc:
            self.tainted = True
            entry.update(status="unknown" if mutation else "failed",
                         error=f"{type(exc).__name__}: {exc}",
                         request_id=(self.transport.active or {}).get("id") or entry.get("request_id"))
            raise
        finally:
            self.persist()

    def read(self, command):
        try:
            return self.command(command)
        except (CommandRejected, CommandTimeout, ProtocolError) as exc:
            self.recover("reviewed read-only query failed: " + str(exc))
            return self.command(command, purpose="one read-only retry after completed barrier")

    def close(self):
        if self.link:
            self.link.close()


class Benchmark:
    def __init__(self, session, identity, configuration, *, minimum_intervals=10):
        self.session, self.identity, self.configuration = session, identity, configuration
        self.minimum_intervals = minimum_intervals
        self.original = None
        self.possible_capture = False
        self.possible_navigation = False
        self.possible_flags = set()
        self.report = {"schema": 1, "target_run_executed": True,
                       "qualification": "live target run; acceptance requires complete valid evidence and verified cleanup",
                       "started_utc": utc_now(), "plan": configuration,
                       "expected_identity": identity, "windows": [], "cleanup": {}, "status": "running"}

    def persist(self):
        write_json(self.session.output / "results.json", self.report)

    def take_snapshot(self):
        return snapshot(*(self.session.read(command) for command in
                          ("state", "runtime_state", "nav_state", "prefs", "diag log")))

    def check_same_device(self):
        actual = check_identity(self.session.read("info"), self.identity)
        log = one(records(self.session.read("diag log")), "NOSLOG")
        if self.original and log.get("boot") != self.original["log"]["boot"]:
            raise AdmissionError("Boot changed: no compensation from the previous boot is authorized")
        return actual

    def preflight(self):
        actual = self.check_same_device()
        before = self.take_snapshot()
        check_snapshot(before, initial=True)
        # A second quiet status read detects a transition/gesture across the
        # separate diagnostics, before the first mutation is sent.
        state2 = one(records(self.session.read("state")), "NOSSTATE")
        runtime2 = one(records(self.session.read("runtime_state")), "NOSRUNTIME")
        check_runtime(runtime2)
        for key in (*GLOBAL_KEYS, "top", "locked", "tx", "pwr"):
            if state2.get(key) != before["state"].get(key):
                raise AdmissionError("Preflight changed across reads: " + key)
        self.original = before
        self.report.update(device_identity=actual, before=before,
                           original_flags=preserved_flags(before))
        self.persist()

    def mutate(self, command, purpose):
        return self.session.command(command, mutation=True, purpose=purpose)

    def quiet_status(self):
        state_table = records(self.session.read("state"))
        state = one(state_table, "NOSSTATE")
        runtime = one(records(self.session.read("runtime_state")), "NOSRUNTIME")
        sharp=self.configuration.get("sharp",False)
        if sharp:
            render=one(state_table,"NOSRENDER")
            for key,value in (("sharp",1),("sprite_interpolation",0),("history_storage_bytes",0)):
                require_equal(render,key,value)
            require_equal(state,"history",0)
        for key, value in (("pwr", 0), ("locked", 0), ("tx", 0), ("mblur", 0 if sharp else 2), ("perf", 0), ("trace", 0)):
            require_equal(state, key, value)
        check_runtime(runtime)
        return state, runtime

    def wait_route(self, app, timeout=8):
        end = time.monotonic() + timeout
        while True:
            state = one(records(self.session.read("state")), "NOSSTATE")
            runtime = one(records(self.session.read("runtime_state")), "NOSRUNTIME")
            if integer(state, "locked") or integer(state, "pwr") != 0:
                raise AdmissionError("Watch locked/powered down; no automatic wake/unlock")
            if not integer(state, "tx") and not integer(runtime, "injected"):
                nav = one(records(self.session.read("nav_state")), "NOSNAV")
                if app == "face" and state.get("top") == "HOME" and integer(nav, "page") == 0 and integer(nav, "offset_q8") == 0 and not integer(nav, "dragging"):
                    return
                if app == "apps" and state.get("top") == "HOME" and integer(nav, "page") == 1 and integer(nav, "offset_q8") == 466 * 256 and not integer(nav, "dragging"):
                    return
                if app == "settings" and state.get("top", "").upper() == "SETTINGS":
                    return
            if time.monotonic() >= end:
                raise AdmissionError("Did not reach real settled route: " + app)
            self.session.sleep(0.15)

    def home_face(self):
        self.quiet_status()
        self.possible_navigation = True
        self.mutate("home", "return stack to HOME; acceptance is verified separately")
        self.session.sleep(0.6)
        nav = one(records(self.session.read("nav_state")), "NOSNAV")
        page = integer(nav, "page")
        if page == 1:
            self.mutate("swipe 90 230 380 230 300", "embedded Apps page1 -> face page0")
        elif page != 0:
            raise AdmissionError("Unexpected HOME page; refusing an unverified navigation gesture")
        self.wait_route("face")

    def route(self, app):
        self.home_face()
        if app == "apps":
            self.mutate("swipe 380 230 90 230 300", "HOME horizontal swipe to embedded Apps page1")
        elif app == "settings":
            self.mutate("open Settings", "open actual registered Settings app")
        else:
            raise ValueError(app)
        self.wait_route(app)
        pose = one(records(self.session.read("scroll_state")), "NOSSCROLL")
        require_equal(pose, "app", 7 if app == "apps" else 8)
        require_equal(pose, "axis", 2)

    def settled_pose(self, app, timeout=10):
        end, previous = time.monotonic() + timeout, None
        while True:
            state, _ = self.quiet_status()
            if (app == "apps" and state.get("top") != "HOME") or (app == "settings" and state.get("top", "").upper() != "SETTINGS"):
                raise AdmissionError("App route changed during positioning")
            pose = one(records(self.session.read("scroll_state")), "NOSSCROLL")
            require_equal(pose, "app", 7 if app == "apps" else 8)
            current = integer(pose, "offset_q8")
            if not integer(pose, "dragging") and previous is not None and abs(current - previous) <= 256:
                return pose
            previous = current
            if time.monotonic() >= end:
                raise AdmissionError("List did not settle before capture")
            self.session.sleep(0.25)

    def position(self, app, direction, travel):
        for _ in range(12):
            pose = self.settled_pose(app)
            offset, limit = integer(pose, "offset_q8") / 256, integer(pose, "limit_q8") / 256
            if limit < 2 * travel + 60:
                raise AdmissionError("List is too short for this measured workload")
            target = limit * (0.4 if direction == "up" else 0.6)
            if abs(offset - target) <= 60:
                return pose
            # App lists may snap/fling past the pointer travel on release.
            # Half-error corrections avoid a two-row oscillation around the
            # interior target; every following action uses a fresh settled pose.
            distance = max(15, min(300, round(abs(target - offset) * 0.5)))
            duration = max(400, round(distance / 150 * 1000))
            y0, y1 = 233 + distance // 2, 233 - (distance - distance // 2)
            if target < offset:
                y0, y1 = y1, y0
            self.mutate(f"swipe 233 {y0} 233 {y1} {duration}", "bounded feedback positioning outside capture")
            self.session.sleep(duration / 1000 + 0.75)
        raise AdmissionError("Could not position list; gestures are not blindly repeated after an uncertain result")

    def verify_boot(self):
        log = one(records(self.session.read("diag log")), "NOSLOG")
        if log.get("boot") != self.original["log"]["boot"]:
            raise AdmissionError("Target rebooted during benchmark")
        return log

    def window(self, app, workload, direction, variant, profile, repetition):
        before_pose = self.position(app, direction, workload["distance_px"])
        self.quiet_status()
        self.wait_route(app)
        self.verify_boot()
        distance = workload["distance_px"]
        y0, y1 = 233 + distance // 2, 233 - (distance - distance // 2)
        if direction == "down":
            y0, y1 = y1, y0
        gesture = f"swipe 233 {y0} 233 {y1} {workload['duration_ms']}"
        entry = {"app": app, "workload": workload, "direction": direction,
                 "variant": variant, "profile": profile, "repetition": repetition,
                 "before_pose": before_pose, "gesture": gesture, "status": "started"}
        self.report["windows"].append(entry)
        self.persist()
        try:
            self.possible_capture = True  # Mark before sending; a lost ACK is ambiguous.
            self.mutate("bench begin", "arm quiet on-device completed-DMA presentation capture")
            self.mutate(gesture, "one measured gesture; NEVER replayed after rejection/timeout")
            self.session.sleep(workload["duration_ms"] / 1000 + workload["tail_s"])
            lines = self.mutate("bench end", "stop before report output can affect capture")
            self.possible_capture = False
            entry["raw_report"] = lines
            entry["statistics"] = capture_statistics(lines, 7 if app == "apps" else 8,
                                                       self.minimum_intervals,sharp=self.configuration.get("sharp",False))
            self.quiet_status()
            self.wait_route(app)
            entry["after_pose"] = one(records(self.session.read("scroll_state")), "NOSSCROLL")
            self.session.sleep(0.25)
            settled = one(records(self.session.read("scroll_state")), "NOSSCROLL")
            if abs(integer(settled, "offset_q8") - integer(entry["after_pose"], "offset_q8")) > 256:
                raise AdmissionError("Motion continues beyond the captured tail; this window is incomplete")
            entry["log_after"] = self.verify_boot()
            entry["status"] = "valid"
        except BaseException as exc:
            entry.update(status="invalid", error=f"{type(exc).__name__}: {exc}")
            raise
        finally:
            self.persist()

    def set_flag(self, name, desired):
        table = records(self.session.read("state"))
        current = (integer(one(table, "NOSHISTORYV"), "enabled") if name == "history_virtual"
                   else integer(one(table, "NOSSTATE"), "pipeline_probe"))
        if current != desired:
            self.possible_flags.add(name)
            self.mutate(f"{name} {desired}", "explicit diagnostic variant; original observed flag retained")
            after = records(self.session.read("state"))
            actual = (integer(one(after, "NOSHISTORYV"), "enabled") if name == "history_virtual"
                      else integer(one(after, "NOSSTATE"), "pipeline_probe"))
            if actual != desired:
                raise AdmissionError("Flag dispatch did not produce the requested state: " + name)

    def execute(self):
        self.preflight()
        if not integer(self.original["state"], "awake"):
            self.possible_flags.add("awake")
            self.mutate("awake", "temporary sleep inhibition; original value retained")
            require_equal(one(records(self.session.read("state")), "NOSSTATE"), "awake", 1)
        for variant in self.configuration["variants"]:
            if variant["history_virtual"] is not None:
                self.set_flag("history_virtual", variant["history_virtual"])
            for profile in self.configuration["passes"]:
                if profile["pipeline_probe"] is not None:
                    self.set_flag("pipeline_probe", profile["pipeline_probe"])
                else:
                    self.set_flag("pipeline_probe", integer(self.original["state"], "pipeline_probe"))
                for repetition in range(self.configuration["repeat"]):
                    for app in ("apps", "settings"):
                        self.route(app)
                        for workload in WORKLOADS:
                            for direction in ("up", "down"):
                                self.window(app, workload, direction, variant["label"], profile["label"], repetition)
        self.report["status"] = "measured"

    def cleanup(self):
        cleanup = self.report["cleanup"]
        cleanup.update(started_utc=utc_now(), status="pending", actions=[])
        self.persist()
        try:
            if self.session.tainted or (self.session.transport and self.session.transport.active is not None):
                self.session.recover("mandatory barrier before cleanup after rejected/incomplete command")
                cleanup["actions"].append("resynchronized/reconnected before any cleanup command")
            if self.original is None:
                cleanup.update(status="no_mutation", detail="Preflight did not authorize any benchmark mutation")
                return
            self.check_same_device()
            runtime = one(records(self.session.read("runtime_state")), "NOSRUNTIME")
            # A rejected/lost gesture ACK must not cause another gesture. Let
            # this run's already-owned bounded injection finish, observing only.
            injection_deadline = time.monotonic() + 8
            while self.possible_navigation and integer(runtime, "injected"):
                if time.monotonic() >= injection_deadline:
                    raise AdmissionError("Owned injection did not finish; no automatic cancel/replay")
                self.session.sleep(0.15)
                runtime = one(records(self.session.read("runtime_state")), "NOSRUNTIME")
            if self.possible_capture and integer(runtime, "capture_state") in (1, 2):
                try:
                    self.mutate("bench end", "stop this run's observed live capture during cleanup")
                except (OSError, TimeoutError) as exc:
                    self.session.recover("ambiguous cleanup capture stop: " + str(exc))
                runtime = one(records(self.session.read("runtime_state")), "NOSRUNTIME")
                if integer(runtime, "capture_state") in (1, 2):
                    raise AdmissionError("Capture stop remains unconfirmed; no blind replay")
            check_runtime(runtime)
            state = one(records(self.session.read("state")), "NOSSTATE")
            if integer(state, "locked") or integer(state, "pwr") != 0:
                raise AdmissionError("Cleanup requires unlocked active panel; no automatic wake/unlock")
            compensation_errors = []
            for flag, desired in (("history_virtual", integer(one(self.original["sections"], "NOSHISTORYV"), "enabled")),
                                  ("pipeline_probe", integer(self.original["state"], "pipeline_probe"))):
                if flag not in self.possible_flags:
                    continue
                try:
                    try:
                        self.set_flag(flag, desired)  # Observe before a single compensation.
                    except (OSError, TimeoutError) as exc:
                        self.session.recover("ambiguous flag compensation; verify, do not replay: " + str(exc))
                    current = records(self.session.read("state"))
                    actual = (integer(one(current, "NOSHISTORYV"), "enabled") if flag == "history_virtual"
                              else integer(one(current, "NOSSTATE"), "pipeline_probe"))
                    if actual != desired:
                        raise AdmissionError("Flag compensation is not confirmed; no blind replay: " + flag)
                    cleanup["actions"].append("verified original " + flag)
                except (OSError, TimeoutError, AdmissionError) as exc:
                    compensation_errors.append(str(exc))
            if self.possible_navigation:
                try:
                    observed = one(records(self.session.read("state")), "NOSSTATE")
                    nav = one(records(self.session.read("nav_state")), "NOSNAV")
                    wanted = integer(self.original["nav"], "page")
                    already = (observed.get("top") == "HOME" and integer(nav, "page") == wanted
                               and integer(nav, "offset_q8") == wanted * 466 * 256 and not integer(nav, "dragging"))
                    if not already:
                        self.home_face()
                        if wanted == 1:
                            self.mutate("swipe 380 230 90 230 300", "restore original embedded HOME page1")
                            self.wait_route("apps")
                    cleanup["actions"].append("verified original HOME page (list scroll positions are workload state)")
                except (OSError, TimeoutError, AdmissionError) as exc:
                    compensation_errors.append("navigation: " + str(exc))
                    if self.session.tainted:
                        self.session.recover("cleanup navigation uncertain; verify flags next without replay")
            if "awake" in self.possible_flags:
                actual = integer(one(records(self.session.read("state")), "NOSSTATE"), "awake")
                desired = integer(self.original["state"], "awake")
                if actual != desired:
                    try:
                        self.mutate("awake", "one compensation after observing current awake value")
                    except (OSError, TimeoutError) as exc:
                        self.session.recover("ambiguous awake compensation; verify, do not replay: " + str(exc))
                require_equal(one(records(self.session.read("state")), "NOSSTATE"), "awake", desired)
                cleanup["actions"].append("verified original awake")
            after = self.take_snapshot()
            check_snapshot(after)
            if preserved_flags(after) != preserved_flags(self.original):
                raise AdmissionError("A global flag changed; no automatic rewrite of unowned flags")
            if prefs_fingerprint(after["prefs"]) != prefs_fingerprint(self.original["prefs"]):
                raise AdmissionError("Preferences changed; no automatic preference restore/write")
            if after["log"]["boot"] != self.original["log"]["boot"]:
                raise AdmissionError("Boot changed during cleanup")
            def fault_key(f):
                return tuple(f.get(key) for key in ("boot", "code", "a", "b", "c"))
            original_faults = {fault_key(f): integer(f, "count") for f in self.original["faults"]}
            new_faults = [f for f in after["faults"] if integer(f, "count") > original_faults.get(fault_key(f), 0)]
            self.report["after"] = after
            cleanup.update(status="verified", flags_preserved=True, preferences_preserved=True, new_faults=new_faults)
            if new_faults:
                raise AdmissionError("New retained faults appeared during benchmark; evidence is invalid")
            if compensation_errors:
                raise AdmissionError("; ".join(compensation_errors))
        except BaseException as exc:
            cleanup.update(status="incomplete", error=f"{type(exc).__name__}: {exc}")
            self.report["status"] = "failed"
        finally:
            cleanup["completed_utc"] = utc_now()
            self.persist()

    def run(self):
        self.persist()
        try:
            self.execute()
        except BaseException as exc:
            self.report.update(status="failed", error=f"{type(exc).__name__}: {exc}")
        finally:
            self.cleanup()
            valid = [w for w in self.report["windows"] if w["status"] == "valid"]
            self.report["summary"] = {"valid_windows": len(valid), "expected_windows": self.configuration["window_count"],
                                      "intervals": sum(w["statistics"]["intervals"] for w in valid),
                                      "over_33_333333ms": sum(w["statistics"]["over_33_333333ms"] for w in valid),
                                      "max_ms": max((w["statistics"]["max_ms"] for w in valid), default=None),
                                      "percentiles": "Per-window values retained; no pooled percentile claim"}
            groups = {}
            for window in valid:
                key = "/".join((window["variant"], window["profile"], window["app"], window["workload"]["name"], window["direction"]))
                groups.setdefault(key, []).append(window["statistics"])
            self.report["groups"] = {key: {"windows": len(values),
                    "intervals": sum(s["intervals"] for s in values),
                    "over_33_333333ms": sum(s["over_33_333333ms"] for s in values),
                    "max_ms": max(s["max_ms"] for s in values),
                    "p50_by_window_ms": [s["p50_ms"] for s in values],
                    "p95_by_window_ms": [s["p95_ms"] for s in values],
                    "p99_by_window_ms": [s["p99_ms"] for s in values]} for key, values in groups.items()}
            complete = (self.report["status"] == "measured" and len(valid) == self.configuration["window_count"]
                        and self.report["cleanup"]["status"] == "verified")
            self.report["measured_all_windows_meet_30hz_floor"] = bool(complete and self.report["summary"]["over_33_333333ms"] == 0)
            self.report["completed_utc"] = utc_now()
            self.persist()
        return self.report


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="explicitly open the requested target port")
    parser.add_argument("--port", help="required with --run; no auto-discovery/default port")
    parser.add_argument("--build", type=Path, default=ROOT / "firmware/build")
    parser.add_argument("--out", type=Path)
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--history-ab", action="store_true", help="separate labeled 0/1/0 diagnostic A/B; never silently enable")
    parser.add_argument("--pipeline-profile", action="store_true", help="add a separately labeled instrumented owner-profile pass")
    parser.add_argument("--usb-paced-writes", action="store_true", help="diagnostic mitigation: flush each command byte, then wait 2 ms; does not fix USB reply gaps")
    args = parser.parse_args(argv)
    if not 1 <= args.repeat <= 20:
        parser.error("--repeat must be 1..20")
    configuration = plan(args.repeat, args.history_ab, args.pipeline_profile)
    if not args.run:
        print(json.dumps(configuration, indent=2))
        return 0
    if not args.port:
        parser.error("--run requires an explicit --port")
    output = (args.out or BASE / ("run-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ"))).resolve()
    if not output.is_relative_to(BASE) or output == BASE:
        parser.error("--out must be a new child directory inside " + str(BASE))
    # Verify current dirty/untracked source, actual full ELF/image, required
    # linked features and NVS-excluding layout BEFORE importing serial/opening.
    from tools.build_identity import verify
    identity = verify(args.build, root=ROOT)
    output.mkdir(parents=True, exist_ok=False)
    write_json(output / "plan.json", configuration)
    write_json(output / "local-identity.json", identity)
    write_json(output / "invocation.json", {"argv": sys.argv if argv is None else argv,
                "harness_sha256": sha256_file(__file__), "transport_sha256": sha256_file(ROOT / "tools/host_transport.py"),
                "build_identity_sha256": sha256_file(args.build / "build-identity.json"), "port": args.port,
                "usb_paced_writes": args.usb_paced_writes,
                "note": "No flash/erase/unlock/OTA action; operator must provide exclusive quiet bench custody"})
    session = Session(args.port, output, usb_paced_writes=args.usb_paced_writes)
    started = False
    try:
        session.connect()
        started = True
        report = Benchmark(session, identity, configuration).run()
        print(json.dumps({"output": str(output), "status": report["status"],
                          "cleanup": report["cleanup"]["status"], "summary": report["summary"],
                          "measured_all_windows_meet_30hz_floor": report["measured_all_windows_meet_30hz_floor"]}, indent=2))
        return 0 if report["status"] == "measured" and report["cleanup"]["status"] == "verified" else 1
    except BaseException as exc:
        write_json(output / "connection-error.json", {"target_run_executed": started,
                   "mutation_outcome": "consult journal; never infer rollback from this error" if started else "none",
                   "error": f"{type(exc).__name__}: {exc}"})
        print(f"Benchmark refused/failed: {exc}", file=sys.stderr)
        return 1
    finally:
        session.close()


if __name__ == "__main__":
    raise SystemExit(main())
