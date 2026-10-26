"""Decode copied /watch/diagnostics files; never connects to or resets a watch.

Usage: python tools/diag_export.py path/to/diagnostics --json trace.json
       python tools/diag_export.py diag-*.bin --csv trace.csv --strict

Records: LE <IHHIIIIHHiiiI>, 44 bytes. File headers: LE <8I>, 32 bytes.
Both use IEEE CRC32 over preceding bytes. Replayed (boot, sequence) IDs are
deduplicated. Durations cover observed events only, with no battery-life claim.
"""
import argparse
import csv
import datetime
import json
import pathlib
import re
import struct
import sys
import zlib

RECORD = struct.Struct("<IHHIIIIHHiiiI")
HEADER = struct.Struct("<8I")
RECORD_MAGIC = 0x31474C44
FILE_MAGIC = 0x46474C44
CODES = {
    1: "BOOT", 2: "RESET_CONTEXT", 3: "STORAGE_ERROR", 4: "QUEUE_DROPS", 5: "FAULT_SUMMARY",
    0x100: "POWER_SAMPLE", 0x101: "POWER_STATE", 0x102: "POWER_POLICY",
    0x103: "WAKE", 0x104: "SLEEP_BLOCK", 0x105: "HEAP", 0x106: "FRAME_PACE",
    0x107: "DMA_HEAP", 0x108: "ALLOC_FAILURE", 0x109: "ALLOC_CONTEXT", 0x10A: "ALLOC_HEAP",
    0x10B: "CRASH_ARTIFACT", 0x10C: "CRASH_PC", 0x10E: "DISPLAY_FAULT",
    0x200: "ACTION", 0x201: "NAVIGATION", 0x202: "COMMAND", 0x203: "COMMAND_INPUT", 0x204: "COMMAND_INPUT_DETAIL",
    0x210: "ACTION_REQUEST", 0x211: "ACTION_RESULT", 0x212: "ACTION_CONTEXT",
    0x300: "WORKOUT", 0x310: "IMU_START", 0x311: "IMU_RESUME",
    0x312: "IMU_STOP", 0x313: "IMU_ERROR", 0x320: "WORKOUT_RECOVERY",
    0x321: "SENSOR_QUALITY",
}
RESETS = {
    0: "unknown", 1: "power_on", 2: "external_pin", 3: "software_restart",
    4: "panic", 5: "interrupt_watchdog", 6: "task_watchdog", 7: "watchdog",
    8: "deep_sleep", 9: "brownout", 10: "sdio", 11: "usb", 12: "jtag",
    13: "efuse", 14: "power_glitch", 15: "cpu_lockup",
}
POWER_STATES = {0: "active", 1: "aod", 2: "display_off", 3: "sensor_idle"}
DOMAINS = dict(enumerate(("navigation", "control", "notification", "face", "alarm", "timer",
                         "phone", "sms", "memo", "gnss", "weather", "sensor", "music", "find", "developer"), 1))
ACTIONS = dict(enumerate(("open", "dismiss", "set", "start", "stop", "fire", "timeout", "pause",
                         "resume", "save", "send", "receive", "cancel", "transition", "reject", "mark"), 1))
RESULTS = {0: "pending", 1: "success", 2: "cancelled", -1: "failed", -2: "rejected", -3: "stale"}
DISPLAY_PHASES = {1: "dma_credit", 2: "stripe_credit", 3: "queue", 4: "frame_fence", 5: "panel_command", 6: "draw"}
DISPLAY_CHANNELS = {0: "brightness", 1: "inversion", 2: "sleep"}
GNSS_REASONS = {1: "unavailable", 2: "invalid", 4: "stale", 8: "invalid_range_or_nonfinite",
                16: "quality", 32: "outlier", 64: "unstable_reacquisition"}
ANALYSIS_LIMITS = {"transactions": 65536, "allocations": 65536, "details_per_transaction": 16,
                   "issues": 256, "duplicate_variants_per_record": 16, "allocation_sequence_window": 16}
SCHEMA = {
    "record_bytes": RECORD.size, "file_header_bytes": HEADER.size,
    "checksum": "IEEE CRC32 over preceding bytes; little endian",
    "retained_flag": 1, "deduplication": ["boot", "sequence"],
    "arguments": {
        "BOOT": ["reset_reason", "previous_valid_breadcrumbs", "crash_boots"],
        "RESET_CONTEXT": ["previous_boot", "previous_max_sequence", "crash_reset"],
        "STORAGE_ERROR": ["errno", "operation_phase", "slot"],
        "QUEUE_DROPS": ["total", "since_last_report", "queued"],
        "FAULT_SUMMARY": ["retained_fault_slot", "occurrence_count_may_span_boots", "original_code"],
        "POWER_SAMPLE": ["battery_percent", "rail_mv", "flags_and_sample_age"],
        "POWER_STATE": ["old_state", "new_state", "battery_saver"],
        "POWER_POLICY": ["battery_saver", "battery_percent", "external_power"],
        "WAKE": ["touch1_button2_raise4_knock8", "previous_state", "battery_saver"],
        "SLEEP_BLOCK": ["manual_awake1_keepawake_screen2_touch4", "awake_ms", "timeout_s"],
        "HEAP": ["internal_free_bytes", "internal_minimum_bytes", "internal_largest_block"],
        "FRAME_PACE": ["requested_delay_us", "ESP_arm_error_or_negative_wrapper_error", "phase_deadline1_arm2_timeout3_early4"],
        "DMA_HEAP": ["internal_DMA_free_bytes", "internal_DMA_minimum_bytes", "internal_DMA_largest_block"],
        "ALLOC_FAILURE": ["requested_size_bytes", "capabilities", "known_caller_PC_or_zero"],
        "ALLOC_CONTEXT": ["capture_sequence", "task_TCB_token", "core_bit0_flags_shift8"],
        "ALLOC_HEAP": ["capture_sequence", "new_failures_since_report", "hook_acquisition_uptime_ms"],
        "CRASH_ARTIFACT": ["crashing_ELF_hash_token", "artifact_bytes", "task_TCB_token"],
        "CRASH_PC": ["exception_PC", "stack_pointer", "exception_cause"],
        "DISPLAY_FAULT": ["phase_or_0x100_plus_command_channel", "error_or_requested_command", "packed_credits_or_applied_command"],
        "ACTION": ["screen_name_fnv1a", "input_type", "axis"],
        "NAVIGATION": ["screen_name_fnv1a", "depth", "revealing"],
        "COMMAND": ["command_verb_fnv1a", "power_state", "reserved"],
        "COMMAND_INPUT": ["reason_1_control_2_overlong", "line_bytes_excluding_LF_including_CR", "first_bad_control_byte_or_zero_for_overflow"],
        "COMMAND_INPUT_DETAIL": ["first_bad_offset_1based_INT32_saturated", "first_NUL_offset_1based_INT32_saturated", "NUL_count_INT32_saturated"],
        "ACTION_REQUEST": ["request_id_boot_scoped", "domain_shift16_action_low16", "numeric_target_or_known_name_hash"],
        "ACTION_RESULT": ["request_id_boot_scoped", "signed_result", "numeric_detail"],
        "ACTION_CONTEXT": ["request_id_boot_scoped", "numeric_source_or_known_name_hash", "old_state_or_generation"],
        "WORKOUT": ["workout_id", "state", "sport"],
        "IMU_START": ["file_id", "previous_file_id", "sample_hz"],
        "IMU_RESUME": ["file_id", "previous_file_id", "sample_hz"],
        "IMU_ERROR": ["errno", "file_id", "rows"],
        "WORKOUT_RECOVERY": ["workout_id", "recovery_count", "state"],
        "SENSOR_QUALITY": ["domain_shift16_reason_mask_low16", "rejected_count", "last_age_ms_INT32_MAX_unknown"],
    },
    "power_sample_flags": {
        "bit_0": "sample_valid", "bit_1": "battery_valid",
        "bits_2_3": "external: 0 unknown, 1 battery, 2 external",
        "bits_8_31": "sample age_ms, saturated at 16777215",
    },
    "power_state_values": POWER_STATES, "reset_reasons": RESETS,
    "action_domains": DOMAINS, "action_values": ACTIONS, "action_results": RESULTS,
    "transaction_acknowledged": "Request and nonconflicting terminal decision were observed; includes failure/cancellation and does not itself mean success.",
    "transaction_states": {"unresolved": "request without result/terminal evidence", "pending": "explicit pending result, no terminal evidence",
                           "orphan": "context/result without request evidence", "unknown_result": "unrecognized positive result, no terminal evidence",
                           "conflict": "inconsistent identity/order/result evidence; no authoritative outcome selected"},
    "allocator_flags": {1: "caller_known", 2: "ISR", 4: "metrics_cached"},
    "display_phases": DISPLAY_PHASES, "display_channels": DISPLAY_CHANNELS,
    "power_policy_variants": {
        "0_or_1": ["battery_saver", "battery_percent", "external_power"],
        "0x100": ["shutdown_intent", "mode_restart1_deep_sleep2", "actual_current_power_state"],
        "0x101": ["shutdown_parked_commit", "mode_restart1_deep_sleep2", "actual_parked_power_state"],
        "0x102": ["developer_recovery_restart", "shutdown_mode", "shutdown_phase"],
    },
    "gnss_rejection_reasons": GNSS_REASONS,
    "analysis_limits": ANALYSIS_LIMITS,
    "correlation": {
        "actions": "(record boot, unsigned request ID), ordered by sequence after replay deduplication. Missing terminal evidence is unresolved; conflicts never acknowledge success.",
        "allocation": "Context/heap metadata share (record boot, unsigned capture sequence). An untagged ALLOC_FAILURE can only be an ordered candidate within the bounded sequence window, never an identity proof.",
        "DMA_heap": "A later maintenance/periodic snapshot, not necessarily headroom at the failed allocation.",
        "retained_faults": "Summary counts can span retained boots. No original boot, first/last payload or elapsed duration is encoded; do not sum repeated summaries.",
        "crash": "Artifact and PC arguments are decoded independently; these records carry no shared crash identity.",
        "known_labels": "Only hashes matching local source screen/command labels are annotated; raw signed values and unsigned IDs remain available.",
        "gnss": "Sparse rejection aggregate, no coordinates. A successful GNSS START acknowledges collection armed, not a position fix or saved file.",
        "shutdown_policy": "Intent/parked/recovery policy variants are not POWER_STATE transitions and never contribute battery-saver or inferred deep-sleep durations.",
    },
}


def name_id(name):
    value = 2166136261
    for byte in name.encode("utf-8"):
        value = ((value ^ byte) * 16777619) & 0xFFFFFFFF
    return value


def source_names():
    """Resolve hashes only when they match known names; payloads stay absent."""
    names = {}
    for source in (pathlib.Path(__file__).resolve().parents[1] / "nos/apps").glob("*.c"):
        for name in re.findall(r'const\s+nos_screen\s+\w+\s*=\s*\{\s*"([^"\n]+)"',
                               source.read_text(encoding="utf-8", errors="replace")):
            names[name_id(name)] = name
    for verb in ("open", "home", "state", "info", "mem", "diag", "prefs", "imu",
                 "sports", "wake", "walk", "knock", "off", "awake", "btn", "tap",
                 "swipe", "ext", "gb", "network", "bat", "batcal", "trace", "help"):
        names[name_id(verb)] = verb
    names[name_id("diag.marker")] = "diag.marker"
    return names


def issue(path, offset, kind, **details):
    return {"file": str(path), "offset": offset, "kind": kind, **details}


def unsigned(value):
    return value & 0xFFFFFFFF


def event_ref(event):
    return {"boot": event["boot"], "sequence": event["sequence"]}


def decode_arguments(event):
    """Add typed numeric views without changing any original record fields."""
    code, a, b, c = (event[key] for key in ("code", "a", "b", "c"))
    if code == 5:
        event["fault_summary"] = {"slot": a, "occurrences": unsigned(b), "original_code": unsigned(c),
                                  "original_event": CODES.get(unsigned(c), "UNKNOWN_" + str(unsigned(c)))}
    elif code == 0x102:
        if a in (0, 1):
            event["power_policy"] = {"kind": "battery_saver", "enabled": bool(a), "percent": b, "external_power": c}
        elif a in (0x100, 0x101):
            event["power_policy"] = {"kind": "shutdown_intent" if a == 0x100 else "shutdown_parked_commit",
                                      "mode": b, "mode_name": {1: "restart", 2: "deep_sleep"}.get(b, "unknown"),
                                      "actual_power_state": c, "actual_power_name": POWER_STATES.get(c, "unknown")}
        elif a == 0x102:
            event["power_policy"] = {"kind": "developer_recovery_restart", "shutdown_mode": b, "shutdown_phase": c}
        else:
            event["power_policy"] = {"kind": "unknown", "variant": a, "b": b, "c": c}
    elif code == 0x107:
        event["dma_heap"] = dict(zip(("free_bytes", "minimum_bytes", "largest_block"), map(unsigned, (a, b, c))))
    elif code == 0x108:
        event["allocation_failure"] = {"size_bytes": unsigned(a), "capabilities": unsigned(b),
                                       "caller_pc": unsigned(c), "caller_pc_hex": f"{unsigned(c):08x}"}
    elif code == 0x109:
        flags = unsigned(c) >> 8
        event["allocation_context"] = {"capture_sequence": unsigned(a), "task": unsigned(b),
                                       "task_hex": f"{unsigned(b):08x}", "packed": unsigned(c), "core": c & 1,
                                       "flags": flags, "caller_known": bool(flags & 1), "isr": bool(flags & 2),
                                       "metrics_cached": bool(flags & 4)}
    elif code == 0x10A:
        event["allocation_heap"] = {"capture_sequence": unsigned(a), "new_failure_count": unsigned(b),
                                    "acquisition_uptime_ms": unsigned(c)}
    elif code == 0x10B:
        event["crash_artifact"] = {"elf_hash_token": unsigned(a), "elf_hash_hex": f"{unsigned(a):08x}",
                                   "bytes": unsigned(b), "task": unsigned(c), "task_hex": f"{unsigned(c):08x}"}
    elif code == 0x10C:
        event["crash_pc"] = {"pc": unsigned(a), "pc_hex": f"{unsigned(a):08x}", "sp": unsigned(b),
                             "sp_hex": f"{unsigned(b):08x}", "cause": unsigned(c)}
    elif code == 0x10E:
        if 0x100 <= a <= 0x102:
            event["display"] = {"kind": "command_ack", "channel": a - 0x100,
                                 "channel_name": DISPLAY_CHANNELS[a - 0x100], "requested": b, "applied": c}
        else:
            event["display"] = {"kind": "fault", "phase": a, "phase_name": DISPLAY_PHASES.get(a, "unknown"),
                                 "error": b, "packed_credits": unsigned(c), "dma_credits": c & 0xFF,
                                 "stripe_credits": (c >> 8) & 0xFF}
    elif code == 0x210:
        domain, action = unsigned(b) >> 16, b & 0xFFFF
        event["action_request"] = {"request_id": unsigned(a), "domain": domain,
                                    "domain_name": DOMAINS.get(domain, "unknown"), "action": action,
                                    "action_name": ACTIONS.get(action, "unknown"), "target": c, "target_id": unsigned(c)}
    elif code == 0x211:
        event["action_result"] = {"request_id": unsigned(a), "result": b, "detail": c,
                                   "result_name": RESULTS.get(b, "failed" if b < 0 else "unknown"),
                                   "terminal": b < 0 or b in (1, 2)}
    elif code == 0x212:
        event["action_context"] = {"request_id": unsigned(a), "source": b, "source_id": unsigned(b), "value": c}
    elif code == 0x321 and unsigned(a) >> 16 == 10:
        mask = a & 0xFFFF
        event["gnss_quality"] = {"domain": 10, "reason_mask": mask,
                                  "reasons": [name for bit, name in GNSS_REASONS.items() if mask & bit],
                                  "unknown_reason_bits": mask & ~0x7F, "rejected_count": b,
                                  "age_ms_raw": c, "age_ms": None if c == 0x7FFFFFFF else c,
                                  "age_unknown": c == 0x7FFFFFFF}


class AnalysisIssues:
    """Bound annotations, not evidence: original events stay in the trace."""
    def __init__(self):
        self.items, self.counts = [], {}
        self.omitted = 0

    def add(self, kind, **details):
        self.counts[kind] = self.counts.get(kind, 0) + 1
        if len(self.items) < ANALYSIS_LIMITS["issues"]:
            self.items.append({"kind": kind, **details})
        else:
            self.omitted += 1


def semantic_transactions(events, issues):
    transactions, omitted_events = {}, 0
    detail_limit = ANALYSIS_LIMITS["details_per_transaction"]
    for event in sorted(events, key=lambda item: (item["boot"], item["sequence"])):
        code = event["code"]
        if code not in (0x210, 0x211, 0x212):
            continue
        key = event["boot"], unsigned(event["a"])
        transaction = transactions.get(key)
        if transaction is None:
            if len(transactions) >= ANALYSIS_LIMITS["transactions"]:
                omitted_events += 1
                continue
            transaction = {"boot": key[0], "request_id": key[1], "first_sequence": event["sequence"],
                           "request": None, "request_count": 0, "contexts": [], "context_count": 0,
                           "results": [], "result_count": 0, "terminal_result": None,
                           "omitted_contexts": 0, "omitted_results": 0, "conflicts": [], "pending_observed": False}
            transactions[key] = transaction
        if not key[1] and "zero_request_id" not in transaction["conflicts"]:
            transaction["conflicts"].append("zero_request_id")
            issues.add("zero_request_id", **event_ref(event))
        if code == 0x210:
            transaction["request_count"] += 1
            if transaction["request"] is None:
                transaction["request"] = {**event_ref(event), **event["action_request"]}
            elif "reused_request_id" not in transaction["conflicts"]:
                transaction["conflicts"].append("reused_request_id")
                issues.add("reused_request_id", **event_ref(event), request_id=key[1])
        elif code == 0x212:
            transaction["context_count"] += 1
            if len(transaction["contexts"]) < detail_limit:
                transaction["contexts"].append({**event_ref(event), **event["action_context"]})
            else:
                transaction["omitted_contexts"] += 1
        else:
            result = {**event_ref(event), **event["action_result"]}
            transaction["result_count"] += 1
            if len(transaction["results"]) < detail_limit:
                transaction["results"].append(result)
            else:
                transaction["omitted_results"] += 1
            terminal = transaction["terminal_result"]
            if result["terminal"]:
                if terminal is None:
                    transaction["terminal_result"] = result
                elif (terminal["result"], terminal["detail"]) != (result["result"], result["detail"]):
                    if "conflicting_terminal_results" not in transaction["conflicts"]:
                        transaction["conflicts"].append("conflicting_terminal_results")
                        issues.add("conflicting_terminal_results", **event_ref(event), request_id=key[1])
            else:
                if result["result"] == 0:
                    transaction["pending_observed"] = True
                else:
                    issues.add("unknown_action_result", **event_ref(event), request_id=key[1], result=result["result"])
                if terminal is not None and "nonterminal_after_terminal" not in transaction["conflicts"]:
                    transaction["conflicts"].append("nonterminal_after_terminal")
                    issues.add("nonterminal_after_terminal", **event_ref(event), request_id=key[1])

    # A conflicting replay must not silently become an acknowledged success by
    # choosing the first copy. Conflicting payload variants are bounded below.
    uncertain_boots = set()
    known_boots = {key[0] for key in transactions}
    for event in events:
        if not event.get("duplicate_id_conflict"):
            continue
        variants = [event, *event.get("conflicting_copies", [])]
        keys = {(event["boot"], unsigned(item["a"])) for item in variants if item["code"] in (0x210, 0x211, 0x212)}
        for key in keys:
            if key in transactions and "record_id_conflict" not in transactions[key]["conflicts"]:
                transactions[key]["conflicts"].append("record_id_conflict")
                issues.add("semantic_record_id_conflict", boot=key[0], request_id=key[1], sequence=event["sequence"])
        if event.get("omitted_conflicting_copies"):
            if event["boot"] in known_boots:
                uncertain_boots.add(event["boot"])
            issues.add("replay_variants_omitted", **event_ref(event), count=event["omitted_conflicting_copies"])

    states = {}
    for transaction in transactions.values():
        if transaction["boot"] in uncertain_boots:
            transaction["conflicts"].append("unbounded_replay_conflict")
        request, terminal = transaction["request"], transaction["terminal_result"]
        transaction["orphan"] = request is None
        transaction["terminal_observed"] = terminal is not None
        if request is None:
            issues.add("missing_action_request", boot=transaction["boot"], request_id=transaction["request_id"])
        elif terminal is None:
            issues.add("missing_action_terminal", boot=transaction["boot"], request_id=transaction["request_id"])
        if request and terminal and terminal["sequence"] < request["sequence"]:
            transaction["conflicts"].append("terminal_before_request")
            issues.add("terminal_before_request", boot=transaction["boot"], request_id=transaction["request_id"])
        if transaction["conflicts"]:
            state = "conflict"
        elif request is None:
            state = "orphan"
        elif terminal:
            state = terminal["result_name"]
        elif transaction["pending_observed"]:
            state = "pending"
        elif transaction["result_count"]:
            state = "unknown_result"
        else:
            state = "unresolved"
        transaction["state"] = state
        transaction["acknowledged"] = bool(request and terminal and not transaction["conflicts"])
        states[state] = states.get(state, 0) + 1
    if omitted_events:
        issues.add("transaction_analysis_limit", omitted_events=omitted_events)
    return list(transactions.values()), {"states": states, "omitted_events": omitted_events}


def allocation_correlations(events, issues):
    allocations, tagged, pending = [], {}, None
    omitted_events = 0

    def new(event):
        if len(allocations) >= ANALYSIS_LIMITS["allocations"]:
            return None
        item = {"boot": event["boot"], "first_sequence": event["sequence"], "capture_sequence": None,
                "failure": None, "context": None, "heap": None, "context_count": 0, "heap_count": 0,
                "failure_correlation": "missing", "conflicts": []}
        allocations.append(item)
        return item

    for event in sorted(events, key=lambda item: (item["boot"], item["sequence"])):
        code = event["code"]
        if code not in (0x108, 0x109, 0x10A):
            continue
        if code == 0x108:
            pending = new(event)
            if pending is not None:
                pending["failure"] = {**event_ref(event), **event["allocation_failure"]}
                pending["failure_correlation"] = "unmatched"
                if event.get("duplicate_id_conflict"):
                    pending["conflicts"].append("record_id_conflict")
            else:
                omitted_events += 1
            continue
        key = event["boot"], unsigned(event["a"])
        item = tagged.get(key)
        if code == 0x109 and pending is not None:
            gap = event["sequence"] - pending["failure"]["sequence"]
            if (pending["boot"] == key[0] and 0 < gap <= ANALYSIS_LIMITS["allocation_sequence_window"]
                    and (item is None or item["failure"] is None)):
                if item is None:
                    item = pending
                else:
                    item["failure"] = pending["failure"]
                    item["first_sequence"] = min(item["first_sequence"], pending["first_sequence"])
                    item["conflicts"].extend(pending["conflicts"])
                    pending["_merged"] = True
                item["failure_correlation"] = "ordered_candidate"
                item["failure_sequence_gap"] = gap
            pending = None
        if item is None:
            item = new(event)
            if item is None:
                omitted_events += 1
                continue
        item["capture_sequence"] = key[1]
        tagged[key] = item
        field = "context" if code == 0x109 else "heap"
        item[field + "_count"] += 1
        if item[field] is None:
            item[field] = {**event_ref(event), **event["allocation_" + field]}
        elif "reused_capture_sequence" not in item["conflicts"]:
            item["conflicts"].append("reused_capture_sequence")
            issues.add("reused_allocation_capture", **event_ref(event), capture_sequence=key[1])
        if event.get("duplicate_id_conflict") and "record_id_conflict" not in item["conflicts"]:
            item["conflicts"].append("record_id_conflict")
        if not key[1] and "zero_capture_sequence" not in item["conflicts"]:
            item["conflicts"].append("zero_capture_sequence")

    uncertain_boots = set()
    known_boots = {item["boot"] for item in allocations}
    for event in events:
        if not event.get("duplicate_id_conflict"):
            continue
        for variant in [event, *event.get("conflicting_copies", [])]:
            if variant["code"] in (0x109, 0x10A):
                item = tagged.get((event["boot"], unsigned(variant["a"])))
                if item is not None and "record_id_conflict" not in item["conflicts"]:
                    item["conflicts"].append("record_id_conflict")
        if event.get("omitted_conflicting_copies") and event["boot"] in known_boots:
            uncertain_boots.add(event["boot"])
    allocations = [item for item in allocations if not item.get("_merged")]
    for item in allocations:
        if item["boot"] in uncertain_boots:
            item["conflicts"].append("unbounded_replay_conflict")
        item["missing"] = [field for field in ("failure", "context", "heap") if item[field] is None]
        item["status"] = "conflict" if item["conflicts"] else "partial" if item["missing"] else "correlated_candidate"
        if item["missing"]:
            issues.add("incomplete_allocation", boot=item["boot"], sequence=item["first_sequence"], missing=item["missing"])
    if omitted_events:
        issues.add("allocation_analysis_limit", omitted_events=omitted_events)
    return allocations, {"groups": len(allocations), "omitted_events": omitted_events}


def decode_file(path):
    path = pathlib.Path(path)
    data = path.read_bytes()
    errors, events = [], []
    info = {"file": str(path), "size": len(data), "generation": 0, "valid_header": False}
    if len(data) < HEADER.size:
        errors.append(issue(path, 0, "truncated_header", bytes=len(data)))
        return info, events, errors
    header = HEADER.unpack_from(data)
    if (header[:3] == (FILE_MAGIC, 1, HEADER.size)
            and zlib.crc32(data[:HEADER.size - 4]) == header[-1]):
        info.update(generation=header[3], boot=header[4], epoch=header[5], valid_header=True)
    else:
        errors.append(issue(path, 0, "invalid_header"))
    offset = HEADER.size
    while offset + RECORD.size <= len(data):
        values = RECORD.unpack_from(data, offset)
        magic, version, size, boot, sequence, uptime, epoch, code, flags, a, b, c, check = values
        if (magic, version, size) != (RECORD_MAGIC, 1, RECORD.size):
            found = data.find(struct.pack("<I", RECORD_MAGIC), offset + 1)
            errors.append(issue(path, offset, "invalid_record_header", skipped=(found - offset if found >= 0 else len(data) - offset)))
            if found < 0:
                offset = len(data)
                break
            offset = found
            continue
        if zlib.crc32(data[offset:offset + RECORD.size - 4]) != check:
            errors.append(issue(path, offset, "record_crc_mismatch", boot=boot, sequence=sequence))
            offset += RECORD.size
            continue
        event = {
            "boot": boot, "sequence": sequence, "uptime_ms": uptime, "epoch": epoch,
            "code": code, "event": CODES.get(code, "UNKNOWN_" + str(code)),
            "flags": flags, "retained": bool(flags & 1), "a": a, "b": b, "c": c,
            "file": str(path), "offset": offset, "copies": 1,
        }
        if epoch:
            event["utc"] = datetime.datetime.fromtimestamp(epoch, datetime.timezone.utc).isoformat()
        if code == 0x100:
            bits = c & 0xFFFFFFFF
            event["power"] = {
                "percent": a, "rail_mv": b, "sample_valid": bool(bits & 1),
                "battery_valid": bool(bits & 2), "external_power": ((bits >> 2) & 3) - 1,
                "sample_age_ms": bits >> 8,
            }
        decode_arguments(event)
        events.append(event)
        offset += RECORD.size
    if offset < len(data):
        errors.append(issue(path, offset, "truncated_tail", bytes=len(data) - offset))
    info["valid_records"] = len(events)
    return info, events, errors


def summarize(events):
    groups = {}
    for event in events:
        groups.setdefault(event["boot"], []).append(event)
    summaries = []
    for boot, items in groups.items():
        items.sort(key=lambda event: event["sequence"])
        times, rollover, previous = [], 0, 0
        for item in items:
            raw = item["uptime_ms"]
            if raw < previous and previous - raw > 0x80000000:
                rollover += 0x100000000
            # Concurrent producers can sample a millisecond before acquiring
            # their sequence ID. Clamp that jitter, never invent negative time.
            times.append(max(times[-1] if times else 0, raw + rollover))
            previous = raw
        start, end = times[0], times[-1]
        transition = next((item for item in items if item["code"] == 0x101), None)
        state = transition["a"] if transition else None
        cursor, durations = start, {name: 0 for name in (*POWER_STATES.values(), "unknown")}
        saver, saver_cursor, saver_ms = False, start, 0
        for item, timestamp in zip(items, times):
            if item["code"] == 0x101:
                durations[POWER_STATES.get(state, "unknown")] += timestamp - cursor
                cursor, state = timestamp, item["b"]
            if item["code"] == 0x102 and item["a"] in (0, 1):
                if saver:
                    saver_ms += timestamp - saver_cursor
                saver, saver_cursor = bool(item["a"]), timestamp
        durations[POWER_STATES.get(state, "unknown")] += end - cursor
        if saver:
            saver_ms += end - saver_cursor
        boot_event = next((item for item in items if item["code"] == 1), None)
        reset = boot_event["a"] if boot_event else None
        batteries = [(item, timestamp) for item, timestamp in zip(items, times)
                     if item["code"] == 0x100 and item["power"]["sample_valid"]
                     and item["power"]["battery_valid"]
                     and item["power"]["external_power"] == 0
                     and 0 <= item["a"] <= 100]
        battery = {"valid_battery_samples": len(batteries)}
        if batteries:
            first, last = batteries[0], batteries[-1]
            battery.update(first_percent=first[0]["a"], last_percent=last[0]["a"],
                           percent_change=last[0]["a"] - first[0]["a"],
                           sample_span_ms=last[1] - first[1])
        summaries.append({
            "boot": boot, "events": len(items), "first_sequence": items[0]["sequence"],
            "last_sequence": items[-1]["sequence"], "reset_reason": reset,
            "reset_name": RESETS.get(reset, "not_retained"),
            "crash_reset": reset in (4, 5, 6, 7, 15),
            "retained_events": sum(item["retained"] for item in items),
            "observed_ms": end - start, "power_ms": durations,
            "battery_saver_ms": saver_ms, "battery": battery,
            "reported_queue_drops": max((item["a"] for item in items if item["code"] == 4), default=0),
        })
    return summaries


def load(paths):
    files = []
    for value in paths:
        path = pathlib.Path(value)
        files.extend(path.rglob("diag-*.bin") if path.is_dir() else [path])
    files = sorted(set(files))
    if not files:
        raise ValueError("No diag-*.bin files found")
    decoded = [decode_file(path) for path in files]
    decoded.sort(key=lambda item: (item[0]["generation"], item[0]["file"]))
    errors = [error for _, _, errors_in_file in decoded for error in errors_in_file]
    unique, duplicates = {}, 0
    for _, events, _ in decoded:
        for event in events:
            key = event["boot"], event["sequence"]
            old = unique.get(key)
            if old:
                duplicates += 1
                for field in ("uptime_ms", "epoch", "code", "a", "b", "c"):
                    if old[field] != event[field]:
                        errors.append(issue(event["file"], event["offset"], "duplicate_id_conflict", boot=key[0], sequence=key[1]))
                        old["duplicate_id_conflict"] = True
                        variants = old.setdefault("conflicting_copies", [])
                        if len(variants) < ANALYSIS_LIMITS["duplicate_variants_per_record"]:
                            variants.append({field: event[field] for field in
                                             ("uptime_ms", "epoch", "code", "a", "b", "c", "flags", "file", "offset")})
                        else:
                            old["omitted_conflicting_copies"] = old.get("omitted_conflicting_copies", 0) + 1
                        break
                old["copies"] += 1
                old["retained"] |= event["retained"]
            else:
                unique[key] = event
    names = source_names()
    events = list(unique.values())
    for event in events:
        if event["code"] in (0x200, 0x201, 0x202):
            hashed = event["a"] & 0xFFFFFFFF
            event["name_id"] = f"{hashed:08x}"
            if hashed in names:
                event["name"] = names[hashed]
        if event["code"] == 0x210 and event["action_request"]["target_id"] in names:
            event["action_request"]["target_known_name"] = names[event["action_request"]["target_id"]]
        elif event["code"] == 0x212 and event["action_context"]["source_id"] in names:
            event["action_context"]["source_known_name"] = names[event["action_context"]["source_id"]]
    analysis = AnalysisIssues()
    transactions, transaction_summary = semantic_transactions(events, analysis)
    allocations, allocation_summary = allocation_correlations(events, analysis)
    return {
        "schema": SCHEMA, "files": [info for info, _, _ in decoded],
        "errors": errors, "duplicates_removed": duplicates, "events": events,
        "sessions": summarize(events),
        "transactions": transactions, "allocations": allocations,
        "diagnostic_summary": {"transactions": transaction_summary, "allocations": allocation_summary,
                               "issue_counts": analysis.counts, "omitted_issues": analysis.omitted},
        "semantic_issues": analysis.items,
        "limits": "Durations cover observed evidence only. Percent is a rail-derived estimate; no remaining-runtime prediction.",
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="+", help="Copied diagnostics directories or .bin files")
    parser.add_argument("--json", type=pathlib.Path, help="Write full decoded trace and summaries")
    parser.add_argument("--csv", type=pathlib.Path, help="Write deduplicated event rows")
    parser.add_argument("--strict", action="store_true", help="Exit 2 if corruption or truncated evidence is detected")
    args = parser.parse_args(argv)
    try:
        report = load(args.paths)
        if args.json:
            args.json.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if args.csv:
            columns = ["boot", "sequence", "uptime_ms", "epoch", "event", "a", "b", "c", "name", "retained", "copies", "file", "offset"]
            with args.csv.open("w", newline="", encoding="utf-8") as output:
                writer = csv.DictWriter(output, fieldnames=columns, extrasaction="ignore")
                writer.writeheader()
                writer.writerows(report["events"])
        print(json.dumps({key: report[key] for key in ("sessions", "errors", "duplicates_removed", "diagnostic_summary", "limits")}, indent=2))
        return 2 if args.strict and report["errors"] else 0
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    sys.exit(main())
