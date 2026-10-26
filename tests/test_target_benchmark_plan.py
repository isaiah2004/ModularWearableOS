"""Offline target-benchmark safety, correlated recovery and statistics fixtures.

No serial module, target port, SDK build or firmware mutation is used.
"""
from collections import deque
import ctypes
import errno
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from tools import host_support

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/alpha05/checks/target-benchmark"
SPEC = importlib.util.spec_from_file_location("target_benchmark_plan", ROOT / "tools/target_benchmark.py")
B = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(B)
IDENTITY = {"firmware_elf_sha256": "1" * 64, "source_tree_sha256": "2" * 64, "bundle_id": "offline-fixture"}


def line(prefix, fields):
    return prefix + " " + " ".join(f"{k}={v}" for k, v in fields.items())


class Firmware:
    """A serial-line fixture; protocol parsing is the actual HostTransport."""
    def __init__(self):
        self.state = {key: 0 for key in B.GLOBAL_KEYS}
        self.state.update(pwr=0, top="HOME", locked=0, tx=0, mblur=2, history=1,
                          layers=1, card_budget_us=1500, detail=4)
        self.runtime = {key: 0 for key in B.REQUIRED_ZERO}
        self.runtime.update(v=1, imu_state=0, workout_ready=1, workout_state=0,
                            workout_save=0, audio_status=0, capture_state=0)
        self.page, self.virtual, self.prefhash = 0, 0, "1234abcd"
        self.boot, self.minimum, self.maximum = "0a0b0c0d", 0, 0
        self.offsets = {7: 0, 8: 0}
        self.commands, self.writes, self.events = [], [], []
        self.reject_gesture, self.rejected = False, False
        self.corrupt_prefs, self.legacy = False, False
        self.awake_timeouts = 0

    def info(self):
        return [line("NOSNATIVE", {"source_sha256": "unrelated-asset-hash"}),
                line("NOSBUILD", {"source_sha256": IDENTITY["source_tree_sha256"],
                   "source_build_id": IDENTITY["source_tree_sha256"][:16],
                   "metrics": "nos-device-v1", "cpu_mhz": 240,
                   "elf_sha256": IDENTITY["firmware_elf_sha256"]})]

    def state_lines(self):
        return [line("NOSDISPLAY", {"v": 1, "brightness_requested": 100, "brightness_applied": 100,
                    "invert_requested": 0, "invert_applied": 0, "sleep_requested": 0,
                    "sleep_applied": 0, "errors": "0,0,0", "fault": 0}),
                line("NOSSTATE", self.state), "NOSPACING mode=1", "NOSNATIVE rom=1 compatible=1",
                "NOSHISTORY native_copy=1", f"NOSHISTORYV enabled={self.virtual}",
                "NOSSCHED mode=0", "NOSCARRY enabled=0"]

    def nav(self):
        return [f"NOSNAV page={self.page} offset_q8={self.page*466*256} dragging=0 scroll_q8={self.offsets[7]} settings_scroll_q8={self.offsets[8]}"]

    def prefs(self):
        return [f"NOSPREFS ok=1 stored=1 match=1 hash={self.prefhash} stored_hash={self.prefhash}"]

    def log(self):
        return [f"NOSLOG v=1 boot={self.boot} ready=1 available=1 errors=0"]

    def app(self):
        return 8 if self.state["top"] == "SETTINGS" else 7 if self.page == 1 else 0

    def capture(self):
        self.runtime["capture_state"] = 3
        return ["NOSCAPTURE state=3 frames=100 painted=60 intervals=59 e2e_max_us=33000 present_max_us=33334 deadline=1 floor=1 p50_ms=33 p95_ms=33 p99_ms=34 start_us=1000000 completed_us=4000000",
                line("NOSBLURCAP", {"v": 1, "state": 3, "first_app": self.app(), "last_app": self.app(),
                    "axis": 2, "foreign_frames": 0, "allocation_failures": 0,
                    "pose_changed": 30, "drag_frames": 20, "v_filter_frames": 30,
                    "blend_frames": 30, "min_q8": self.minimum, "max_q8": self.maximum}),
                line("NOSCACHECAP", {"v": 1, "state": 3, "frames": 100, "painted": 60,
                    "requested_frames": 0, "sharp_frames": 0, "blurred_frames": 0,
                    "stale_frames": 0, "requested_processed_area": 0,
                    "sharp_processed_area": 0, "blurred_processed_area": 0,
                    "unknown_requests": 0, "native_frames": 45, "native_blurred_frames": 30,
                    "native_processed_area": 450000, "native_blurred_processed_area": 300000,
                    "processed_not_unique": 1, "final_visible": 0})]

    def respond(self, command):
        self.commands.append(command)
        self.events.append(("command", command))
        if command == "hostcaps":
            return ["ERR unknown: hostcaps"] if self.legacy else ["NOSHOST v=1 req=1 max_command=191 typed_errors=1"]
        if command == "info": return self.info()
        if command == "state": return self.state_lines()
        if command == "runtime_state": return [line("NOSRUNTIME", self.runtime)]
        if command == "nav_state": return self.nav()
        if command == "prefs": return self.prefs()
        if command == "diag log": return self.log()
        if command == "scroll_state":
            app = self.app()
            return [f"NOSSCROLL v=1 app={app} axis=2 page=0 pages=1 offset_q8={self.offsets.get(app,0)} limit_q8=512000 dragging=0"]
        if command == "awake":
            self.state["awake"] ^= 1
            return [f"AWAKE {self.state['awake']}"]
        if command == "home":
            self.state["top"] = "HOME"
            return []
        if command == "open Settings":
            self.state["top"] = "SETTINGS"
            return []
        if command.startswith("history_virtual "):
            self.virtual = int(command[-1])
            return [f"NOSHISTORYV enabled={self.virtual}"]
        if command.startswith("pipeline_probe "):
            self.state["pipeline_probe"] = int(command[-1])
            return [f"NOSDIAG pipeline_probe={self.state['pipeline_probe']}"]
        if command == "bench begin":
            self.runtime["capture_state"] = 2
            self.minimum = self.maximum = self.offsets[self.app()]
            return ["NOSCAPTURE armed=1 bytes=320"]
        if command == "bench end": return self.capture()
        if command.startswith("swipe "):
            _, x0, y0, x1, y1, duration = command.split()
            if y0 == y1:
                self.page = 1 if int(x1) < int(x0) else 0
            else:
                app = self.app()
                if not app: raise AssertionError("vertical swipe on face")
                self.offsets[app] = max(0, min(512000, self.offsets[app] + (int(y0)-int(y1))*256))
                if self.runtime["capture_state"] == 2:
                    self.minimum = min(self.minimum, self.offsets[app])
                    self.maximum = max(self.maximum, self.offsets[app])
                    if self.corrupt_prefs: self.prefhash = "deadbeef"
            return []
        raise AssertionError("Unexpected command " + command)


class FakeLink:
    def __init__(self, firmware):
        self.firmware, self.pending, self.closed = firmware, deque(), False

    def write(self, data):
        assert re.fullmatch(rb"\x00\nreq [0-9a-f]{8} hostcaps\n", data), data
        self.firmware.writes.append(data)
        self.firmware.events.append(("barrier", "NUL"))
        # The real transport must ignore both old styles of completion until
        # the fresh hostcaps payload + correlated ID arrive.
        self.pending.extend(["ERR command input reason=1 bytes=1 bad=0", "DONE", "DONE deadbeef ok=1"])
        self.send(data[2:-1].decode("ascii"))

    def send(self, wire):
        request = wire.split(" ", 2)
        correlated = len(request) == 3 and request[0] == "req"
        command = request[2] if correlated else wire
        if self.firmware.reject_gesture and not self.firmware.rejected and command.startswith("swipe ") and self.firmware.runtime["capture_state"] == 2:
            self.firmware.rejected = True
            self.firmware.commands.append(command)
            self.firmware.events.append(("rejected", command))
            self.pending.append("ERR command input reason=1 bytes=25 bad=0")
            return
        result = self.firmware.respond(command)
        if correlated: self.pending.append("BEGIN " + request[1])
        self.pending.extend(result)
        self.pending.append(f"DONE {request[1]} ok=1" if correlated else "DONE")

    def readline(self, timeout):
        return self.pending.popleft() if self.pending else ""

    def close(self): self.closed = True


class BenchmarkPlanTests(unittest.TestCase):
    def directory(self):
        if not OUT.resolve().is_relative_to(ROOT.resolve()):
            raise AssertionError("Private fixture path escaped the workspace")
        OUT.mkdir(parents=True, exist_ok=True)
        return tempfile.TemporaryDirectory(prefix="offline-", dir=OUT)

    def connect(self, directory, firmware):
        session = B.Session("OFFLINE-NO-DEVICE", directory, link_factory=lambda: FakeLink(firmware), sleep=lambda _: None)
        session.connect()
        return session

    def test_default_is_offline_and_plan_covers_real_routes_and_all_speeds(self):
        with patch.object(B.SerialLines, "open", side_effect=AssertionError("must not open a port")), patch("sys.stdout", new_callable=io.StringIO) as out:
            self.assertEqual(B.main([]), 0)
        self.assertIn('"target_run_executed": false', out.getvalue())
        plan = B.plan()
        self.assertEqual(plan["window_count"], 36)
        self.assertEqual(plan["workloads"][0]["speed_px_s"], 75)
        self.assertEqual({x["name"] for x in plan["workloads"]}, {"slow", "typical", "fling"})
        self.assertEqual(plan["directions"], ["up", "down"])
        self.assertNotIn("open launcher", str(plan).lower())
        self.assertIn("swipe 380 230 90 230 300", plan["apps"][0]["route"])
        ab = B.plan(history_ab=True, pipeline_profile=True)
        self.assertEqual([x["history_virtual"] for x in ab["variants"]], [0, 1, 0])
        self.assertTrue(all(x["label"].startswith("diagnostic-") for x in ab["variants"]))

    def test_local_build_mismatch_refuses_before_opening_serial(self):
        with patch.object(B.SerialLines,"open",side_effect=AssertionError("must not open")) as opened, \
             patch("tools.build_identity.verify",side_effect=ValueError("dirty source changed")):
            with self.assertRaisesRegex(ValueError,"dirty source changed"):
                B.main(["--run","--port","OFFLINE-NO-DEVICE"])
        opened.assert_not_called()

    def test_identity_requires_entire_elf_source_and_current_metrics(self):
        firmware = Firmware()
        self.assertEqual(B.check_identity(firmware.info(), IDENTITY)["elf_sha256"], "1"*64)
        for field, value in (("elf_sha256", "1"*16), ("elf_sha256", "1"*63+"0"),
                             ("source_sha256", "2"*63+"0"), ("source_build_id", "f"*16),
                             ("cpu_mhz", "80")):
            bad = B.records(firmware.info())["NOSBUILD"][0]
            bad[field] = value
            with self.subTest(field=field,value=value), self.assertRaises(B.AdmissionError):
                B.check_identity([line("NOSBUILD",bad)], IDENTITY)
        with self.assertRaises(B.AdmissionError):
            B.check_identity(firmware.info()*2, IDENTITY)

    def test_all_unknown_active_owner_states_and_lock_reject_before_mutation(self):
        mutations = [("state", "locked", 1), ("state", "mblur", 0), ("state", "pwr", 1),
                     ("runtime", "imu_state", -1), ("runtime", "imu_state", 2),
                     ("runtime", "imu_pending", 1), ("runtime", "workout_state", 1),
                     ("runtime", "workout_state", 2), ("runtime", "workout_ready", 0),
                     ("runtime", "workout_uncertain", 1), ("runtime", "workout_corrupt", 1),
                     ("runtime", "workout_foreign", 1), ("runtime", "audio_status", -1),
                     ("runtime", "audio_status", 1), ("runtime", "audio_status", 2),
                     ("runtime", "capture_state", 2)]
        mutations += [("runtime", key, 1) for key in B.REQUIRED_ZERO]
        mutations += [("runtime", "imu_pending", None)]
        with self.directory() as directory:
            for section, field, value in mutations:
                firmware = Firmware()
                record = getattr(firmware, section)
                if value is None: record.pop(field)
                else: record[field] = value
                session = self.connect(directory, firmware)
                bench = B.Benchmark(session, IDENTITY, B.plan(repeat=1))
                with self.subTest(field=field, value=value), self.assertRaises(B.AdmissionError):
                    bench.preflight()
                self.assertFalse(any(c not in B.READONLY for c in firmware.commands))

    def test_quiet_capture_population_and_quality_rejects_false_passes(self):
        firmware = Firmware();firmware.page=1;firmware.minimum=100*256;firmware.maximum=300*256
        good = firmware.capture()
        result = B.capture_statistics(good, 7)
        self.assertEqual(result["max_ms"],33.334)
        self.assertEqual(result["over_33_333333ms"],1)
        for old,new in (("state=3 frames", "state=5 frames"), ("intervals=59", "intervals=0"),
                        ("first_app=7", "first_app=0"), ("foreign_frames=0", "foreign_frames=1"),
                        ("allocation_failures=0", "allocation_failures=1"),
                        ("stale_frames=0", "stale_frames=1"), ("floor=1", "floor=0")):
            bad = [s.replace(old,new) for s in good]
            with self.subTest(old=old), self.assertRaises(B.AdmissionError):
                B.capture_statistics(bad,7)
        bad = [s.replace("v_filter_frames=30","v_filter_frames=0").replace("blend_frames=30","blend_frames=0").replace("blurred_frames=30","blurred_frames=0") for s in good]
        with self.assertRaises(B.AdmissionError):B.capture_statistics(bad,7)

    def test_stats_exclude_idle_and_missing_frames_and_count_33334us(self):
        frames = [{"frame_id":0,"px":100,"completed_us":1000000},
                  {"frame_id":1,"px":100,"completed_us":1033333},
                  {"frame_id":2,"px":0,"completed_us":1050000},
                  {"frame_id":3,"px":100,"completed_us":1100000},
                  {"frame_id":4,"px":100,"completed_us":1133334},
                  {"frame_id":7,"px":100,"completed_us":1300000},
                  {"frame_id":8,"px":100,"completed_us":1400000}]
        stats = B.painted_interval_statistics(frames)
        self.assertEqual(stats["intervals_us"],[33333,33334,100000])
        self.assertEqual((stats["p50_ms"],stats["p95_ms"],stats["p99_ms"],stats["floor"]),(34,100,100,2))

    def test_native_blur_evidence_requires_current_consistent_counts_and_areas(self):
        for app in (7, 8):
            firmware=Firmware();firmware.page=1
            if app==8:firmware.state["top"]="SETTINGS"
            firmware.minimum=100*256;firmware.maximum=300*256
            good=firmware.capture();cache=B.one(B.records(good),"NOSCACHECAP")
            def changed(fields, remove=()):
                updated=cache | {key:str(value) for key,value in fields.items()}
                for key in remove:updated.pop(key)
                return [line("NOSCACHECAP",updated) if value.startswith("NOSCACHECAP ") else value for value in good]
            for key in ("native_frames","native_blurred_frames","native_processed_area","native_blurred_processed_area"):
                with self.subTest(app=app,missing=key),self.assertRaises(B.AdmissionError):
                    B.capture_statistics(changed({},(key,)),app)
            invalid=[{"native_blurred_frames":0,"native_blurred_processed_area":0},
                     {"native_frames":-1},{"native_frames":101},{"native_blurred_frames":46},
                     {"native_processed_area":-1},{"native_processed_area":1<<64},
                     {"native_blurred_processed_area":450001},
                     {"native_blurred_frames":0},{"native_blurred_processed_area":0},
                     {"native_blurred_processed_area":29},{"native_processed_area":44},
                     {"frames":99},{"painted":59},{"v":0},
                     {"processed_not_unique":0},{"final_visible":1},
                     {"requested_frames":1},{"requested_processed_area":1},
                     {"requested_frames":1,"requested_processed_area":1,"blurred_frames":2,"blurred_processed_area":2},
                     {"requested_frames":1,"requested_processed_area":1,"sharp_frames":1,"sharp_processed_area":2}]
            for mutation in invalid:
                with self.subTest(app=app,mutation=mutation),self.assertRaises(B.AdmissionError):
                    B.capture_statistics(changed(mutation),app)
            old=[value for value in good if not value.startswith("NOSCACHECAP ")]
            old.append("NOSCACHECAP state=3 stale_frames=0 unknown_requests=0 blurred_frames=30")
            with self.assertRaises(B.AdmissionError):B.capture_statistics(old,app)
            # Neither generic temporal blend nor a filter counter substitutes for native evidence.
            no_native=changed({"native_frames":0,"native_blurred_frames":0,
                               "native_processed_area":0,"native_blurred_processed_area":0})
            no_native=[value.replace("blend_frames=30","blend_frames=1") for value in no_native]
            with self.assertRaises(B.AdmissionError):B.capture_statistics(no_native,app)
            # Native work is independently observed: generic cache/filter/blend work can be zero.
            native_only=[value.replace("v_filter_frames=30","v_filter_frames=0").replace("blend_frames=30","blend_frames=0") for value in good]
            observed=B.capture_statistics(native_only,app)
            self.assertIn("not unique",observed["native_observation"])
            # Processed area includes overlap, so screen-area bounds would be incorrect.
            overlapping=changed({"native_processed_area":10**9,"native_blurred_processed_area":10**9})
            B.capture_statistics(overlapping,app)

    @unittest.skipUnless(shutil.which("gcc"),"GCC required for actual C interval capture")
    def test_offline_stats_match_actual_c_capture_idle_and_overflow(self):
        source = r'''#include "nos_frame_perf.h"
#include <stdio.h>
int main(void){nos_frame_perf p={0};long long times[]={1000000,1033333,1050000,1100000,1133334,1233334};int painted[]={1,1,0,1,1,1};
for(int i=0;i<6;i++)nos_frame_perf_add(&p,times[i]-10000,times[i],times[i]+1,painted[i]);
printf("%u %u %u %u %u %lld\n",p.intervals,p.floor_misses,nos_frame_perf_percentile(&p,50),nos_frame_perf_percentile(&p,95),nos_frame_perf_percentile(&p,99),(long long)p.present_max);return 0;}'''
        with self.directory() as directory:
            path=Path(directory);c=path/"actual-statistics.c";binary=path/"actual-statistics.exe";c.write_text(source)
            compile=subprocess.run(["gcc","-std=c11","-Wall","-Wextra","-Werror","-Ifirmware/main",str(c),"-o",str(binary)],cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(compile.returncode,0,compile.stderr)
            result=subprocess.run([str(binary)],capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(result.stdout.strip(),"3 2 34 100 100 100000")

    def test_rejected_nul_gesture_resyncs_before_cleanup_and_never_replays(self):
        with self.directory() as directory:
            firmware=Firmware();firmware.reject_gesture=True
            session=self.connect(directory,firmware)
            result=B.Benchmark(session,IDENTITY,B.plan(repeat=1)).run()
            self.assertEqual(result["status"],"failed")
            self.assertEqual(result["cleanup"]["status"],"verified")
            rejected=next(i for i,event in enumerate(firmware.events) if event[0]=="rejected")
            barrier=next(i for i,event in enumerate(firmware.events) if i>rejected and event[0]=="barrier")
            end=next(i for i,event in enumerate(firmware.events) if i>rejected and event==("command","bench end"))
            self.assertLess(barrier,end)
            self.assertEqual(len([event for event in firmware.events if event[0]=="rejected"]),1)
            self.assertEqual(firmware.state["awake"],0)
            self.assertEqual(firmware.runtime["capture_state"],3)
            self.assertTrue(session.recoveries)
            self.assertFalse(result["measured_all_windows_meet_30hz_floor"])

    def test_unknown_awake_outcome_is_observed_then_compensated_once(self):
        with self.directory() as directory:
            firmware=Firmware();session=self.connect(directory,firmware)
            original=session.transport.execute;failed=False
            def execute(command,*args,**kwargs):
                nonlocal failed
                if command=="awake" and not failed:
                    failed=True;firmware.respond(command)
                    session.transport.active={"id":"aabbccdd","command":command}
                    raise B.CommandTimeout(command,"aabbccdd")
                return original(command,*args,**kwargs)
            with patch.object(session.transport,"execute",side_effect=execute):
                result=B.Benchmark(session,IDENTITY,B.plan(repeat=1)).run()
            self.assertEqual(result["status"],"failed")
            self.assertEqual(result["cleanup"]["status"],"verified")
            self.assertEqual(firmware.commands.count("awake"),2)
            self.assertEqual(firmware.state["awake"],0)
            first=firmware.commands.index("awake");last=len(firmware.commands)-1-firmware.commands[::-1].index("awake")
            self.assertIn("state",firmware.commands[first+1:last])

    def test_reconnect_after_barrier_failure_precedes_compensation(self):
        with self.directory() as directory:
            firmware=Firmware();session=self.connect(directory,firmware)
            bench=B.Benchmark(session,IDENTITY,B.plan(repeat=1));bench.preflight()
            bench.possible_flags.add("awake");firmware.state["awake"]=1;session.tainted=True
            old_link=session.link
            with patch.object(session.transport,"resynchronize",side_effect=B.ProtocolError("fixture broken link")):
                bench.cleanup()
            self.assertTrue(old_link.closed)
            self.assertEqual(bench.report["cleanup"]["status"],"verified")
            self.assertEqual(firmware.state["awake"],0)
            self.assertIn("close/reopen",session.recoveries[-1]["method"])

    def test_changed_boot_blocks_compensation_and_no_preferences_are_rewritten(self):
        with self.directory() as directory:
            firmware=Firmware();session=self.connect(directory,firmware)
            bench=B.Benchmark(session,IDENTITY,B.plan(repeat=1));bench.preflight()
            firmware.state["awake"]=1;bench.possible_flags.add("awake");firmware.boot="12345678"
            bench.cleanup()
            self.assertEqual(bench.report["cleanup"]["status"],"incomplete")
            self.assertNotIn("awake",firmware.commands)
            self.assertFalse(any(c.startswith(("prefs ","sdput","sdrm","ota ","bri ")) for c in firmware.commands))

    def test_cleanup_navigation_rejection_resyncs_and_still_restores_awake(self):
        with self.directory() as directory:
            firmware=Firmware();session=self.connect(directory,firmware)
            bench=B.Benchmark(session,IDENTITY,B.plan(repeat=1));bench.preflight()
            bench.possible_navigation=True;bench.possible_flags.add("awake")
            firmware.page=1;firmware.state["awake"]=1
            original=session.transport.execute
            def execute(command,*args,**kwargs):
                if command=="swipe 90 230 380 230 300":
                    session.transport.active={"id":"aabbccdd","command":command}
                    raise B.CommandRejected("ERR command input reason=1 bytes=25 bad=0",command,"aabbccdd")
                return original(command,*args,**kwargs)
            with patch.object(session.transport,"execute",side_effect=execute):bench.cleanup()
            self.assertEqual(bench.report["cleanup"]["status"],"incomplete")
            self.assertTrue(bench.report["cleanup"]["flags_preserved"])
            self.assertEqual(firmware.state["awake"],0)
            self.assertIn("cleanup navigation uncertain",session.recoveries[-1]["reason"])

    def test_preferences_changed_by_other_owner_are_detected_and_never_rewritten(self):
        with self.directory() as directory:
            firmware=Firmware();session=self.connect(directory,firmware)
            bench=B.Benchmark(session,IDENTITY,B.plan(repeat=1));bench.preflight()
            bench.possible_flags.add("awake");firmware.state["awake"]=1;firmware.prefhash="deadbeef"
            bench.cleanup()
            self.assertEqual(bench.report["cleanup"]["status"],"incomplete")
            self.assertEqual(firmware.state["awake"],0)
            self.assertEqual(firmware.prefhash,"deadbeef")
            self.assertFalse(any(c.startswith(("prefs ","sdput","sdrm")) for c in firmware.commands))

    def test_complete_offline_workload_preserves_flags_and_routes(self):
        with self.directory() as directory:
            firmware=Firmware();firmware.page=1;firmware.virtual=1
            session=self.connect(directory,firmware)
            # Journaling is separately exercised by fault cases. Avoid rewriting
            # thousands of fixture records while simulating all72 windows.
            with patch.object(session,"persist"),patch.object(B.Benchmark,"persist"):
                result=B.Benchmark(session,IDENTITY,B.plan(repeat=1,history_ab=True,pipeline_profile=True)).run()
            self.assertEqual(result["status"],"measured",result.get("error"))
            self.assertEqual(result["cleanup"]["status"],"verified",result["cleanup"])
            self.assertEqual(len(result["windows"]),72)
            self.assertEqual(firmware.virtual,1)
            self.assertEqual(firmware.page,1)
            self.assertEqual(firmware.state["awake"],0)
            self.assertEqual(firmware.state["pipeline_probe"],0)
            self.assertEqual(result["summary"]["over_33_333333ms"],72)
            self.assertFalse(result["measured_all_windows_meet_30hz_floor"])
            self.assertNotIn("open launcher",[c.lower() for c in firmware.commands])


CAPS = "NOSHOST v=1 req=1 max_command=191 typed_errors=1"


class Clock:
    def __init__(self): self.now = 0
    def __call__(self): return self.now


class Link:
    def __init__(self, replies=None, clock=None):
        self.replies = replies or (lambda nonce: ["BEGIN " + nonce, CAPS, "DONE " + nonce + " ok=1"])
        self.clock, self.pending, self.raw, self.sent = clock, deque(), [], []
        self.closed = False

    def write(self, data):
        assert re.fullmatch(rb"\x00\nreq [0-9a-f]{8} hostcaps\n", data), data
        self.raw.append(data)
        self.pending.extend(self.replies(data[6:14].decode("ascii")))

    def send(self, command): self.sent.append(command)
    def readline(self, timeout):
        if self.pending: return self.pending.popleft()
        if self.clock: self.clock.now += max(timeout, .001)
        return ""
    def close(self): self.closed = True


class StrictHandshakeTests(unittest.TestCase):
    def test_initial_connection_one_combined_write_and_no_redundant_negotiate(self):
        with BenchmarkPlanTests().directory() as directory:
            fw = Firmware()
            session = B.Session("OFFLINE", directory, link_factory=lambda: FakeLink(fw))
            with patch.object(B.HostTransport, "negotiate", side_effect=AssertionError("redundant negotiation")):
                session.connect()
            self.assertEqual(fw.commands, ["hostcaps"])
            self.assertEqual(len(fw.writes), 1)
            self.assertTrue(session.transport.correlated)
            self.assertEqual(session.transport.capabilities["max_command"], "191")
            trace = [json.loads(s) for s in (Path(directory) / "wire.jsonl").read_text().splitlines()]
            nonce = fw.writes[0][6:14].decode()
            self.assertEqual(next(x for x in trace if x["line"] == CAPS)["request_id"], nonce)

    def test_transient_initial_barrier_reopens_once_and_adopts_only_actual_caps(self):
        clock = Clock(); broken = Link(lambda n: [], clock)
        fresh = Link(lambda n: ["BEGIN " + n, CAPS.replace("191", "128"), "DONE " + n + " ok=1"], clock)
        links = iter([broken, fresh])
        with BenchmarkPlanTests().directory() as directory, patch.object(B.time, "monotonic", clock):
            session = B.Session("OFFLINE", directory, link_factory=lambda: next(links))
            session.connect()
            self.assertTrue(broken.closed)
            self.assertFalse(session.tainted)
            self.assertEqual(session.transport.capabilities["max_command"], "128")
            self.assertEqual(len(broken.raw), 1)
            self.assertEqual(len(fresh.raw), 1)
            self.assertNotEqual(broken.raw, fresh.raw)
            self.assertEqual(broken.sent + fresh.sent, [])
            self.assertIn("close/reopen", session.recoveries[-1]["method"])

    def test_two_failed_barriers_abort_without_any_query_or_mutation(self):
        clock = Clock(); links = [Link(lambda n: [], clock), Link(lambda n: [], clock)]
        it = iter(links)
        with BenchmarkPlanTests().directory() as directory, patch.object(B.time, "monotonic", clock):
            session = B.Session("OFFLINE", directory, link_factory=lambda: next(it))
            with self.assertRaises(B.ProtocolError): session.connect()
            self.assertTrue(session.tainted)
            self.assertEqual([len(l.raw) for l in links], [1, 1])
            self.assertEqual(links[0].sent + links[1].sent, [])
            with self.assertRaises(B.ProtocolError): session.command("info")

    def test_stale_and_nul_records_cannot_negotiate_before_matching_begin(self):
        clock = Clock()
        link = Link(lambda n: ["ERR command input reason=1 bytes=1 bad=0", "DONE",
                              "NOSHOST v=0 req=0 max_command=999", "BEGIN deadbeef", "DONE deadbeef ok=1",
                              "BEGIN " + n, CAPS, "DONE deadbeef ok=1", "DONE", "DONE " + n + " ok=1"], clock)
        with patch.object(B.time, "monotonic", clock):
            transport = B.PinnedTransport(link)
            transport.resynchronize(.1)
        self.assertTrue(transport.correlated)
        self.assertEqual(transport.capabilities["max_command"], "191")

    def test_duplicate_missing_interleaved_and_failed_barriers_fail_closed(self):
        cases = {
            "duplicate_payload": lambda n: ["BEGIN " + n, CAPS, CAPS, "DONE " + n + " ok=1"],
            "duplicate_begin": lambda n: ["BEGIN " + n, "BEGIN " + n, CAPS, "DONE " + n + " ok=1"],
            "missing_payload": lambda n: ["BEGIN " + n, "DONE " + n + " ok=1"],
            "missing_begin": lambda n: [CAPS, "DONE " + n + " ok=1"],
            "missing_terminal": lambda n: ["BEGIN " + n, CAPS, "DONE deadbeef ok=1"],
            "terminal_before_begin": lambda n: ["DONE " + n + " ok=1", "BEGIN " + n, CAPS],
            "interleaved_begin": lambda n: ["BEGIN " + n, "BEGIN deadbeef", CAPS, "DONE " + n + " ok=1"],
            "failure": lambda n: ["BEGIN " + n, CAPS, "DONE " + n + " ok=0 code=EBUSY"],
            "unexpected_rejection": lambda n: ["BEGIN " + n, "ERR unknown: hostcaps", "DONE " + n + " ok=1"],
        }
        for name, replies in cases.items():
            clock = Clock(); transport = B.PinnedTransport(Link(replies, clock))
            with self.subTest(name=name), patch.object(B.time, "monotonic", clock):
                with self.assertRaises(B.ProtocolError): transport.resynchronize(.1)
                self.assertFalse(transport.correlated)
                self.assertEqual(transport.capabilities, {})
                with self.assertRaises(B.ProtocolError): transport.begin("info")

    def test_unsupported_duplicate_and_malformed_caps_are_not_adopted(self):
        invalid = [CAPS.replace("v=1", "v=2"), CAPS.replace("req=1", "req=0"),
                   CAPS.replace("req=1 ", ""), CAPS + " req=1", CAPS + " malformed",
                   CAPS + " blank=", CAPS.replace("NOSHOST ", "NOSHOSTNOSHOST ")]
        invalid += [CAPS.replace("191", value) for value in ("0", "20", "192", "999", "0191", "1.5", "unknown", "-1")]
        for payload in invalid:
            clock = Clock(); link = Link(lambda n: ["BEGIN " + n, payload, "DONE " + n + " ok=1"], clock)
            transport = B.PinnedTransport(link)
            with self.subTest(payload=payload), patch.object(B.time, "monotonic", clock):
                with self.assertRaises(B.ProtocolError): transport.resynchronize(.1)
                self.assertEqual(transport.capabilities, {})
                self.assertFalse(transport.correlated)

    def test_timeout_keeps_prior_ambiguous_mutation_blocked_and_does_not_upgrade(self):
        clock = Clock(); transport = B.PinnedTransport(Link(lambda n: ["BEGIN " + n, CAPS.replace("191", "128")], clock))
        prior = {"id": "deadbeef", "command": "prefs flush"}
        transport.active = prior; transport.correlated = True
        transport.capabilities = {"v": "1", "req": "1", "max_command": "191"}
        with patch.object(B.time, "monotonic", clock):
            with self.assertRaises(B.ProtocolError): transport.resynchronize(.1)
        self.assertIs(transport.active, prior)
        self.assertEqual(transport.capabilities["max_command"], "191")
        self.assertEqual(transport.link.sent, [])

    def test_timed_out_mutation_is_not_replayed_when_barrier_completes(self):
        clock = Clock(); link = Link(clock=clock); transport = B.PinnedTransport(link)
        transport.correlated = True; transport.capabilities = {"v": "1", "req": "1", "max_command": "191"}
        with patch.object(B.time, "monotonic", clock):
            with self.assertRaises(B.CommandTimeout) as error: transport.execute("prefs flush", timeout=.1)
            self.assertTrue(error.exception.ambiguous)
            transport.resynchronize(.1)
        self.assertEqual(len(link.sent), 1)
        self.assertTrue(link.sent[0].endswith(" prefs flush"))
        self.assertEqual(transport.last_result["command"], "hostcaps")

    def test_real_seriallines_single_write_flush_and_fragmented_reply(self):
        class Port:
            def __init__(self): self.tx = []; self.rx = deque(); self.flushes = 0
            @property
            def in_waiting(self): return 1
            def write(self, data):
                self.tx.append(data); nonce = data[6:14].decode()
                response = "ERR command input reason=1 bytes=1 bad=0\nDONE\nBEGIN " + nonce + "\n" + CAPS + "\nDONE " + nonce + " ok=1\n"
                self.rx.extend(response[i:i+3].encode() for i in range(0, len(response), 3))
                return len(data)
            def read(self, count): return self.rx.popleft() if self.rx else b""
            def flush(self): self.flushes += 1
        port = Port(); transport = B.PinnedTransport(B.SerialLines(port))
        transport.resynchronize(.2)
        self.assertEqual(len(port.tx), 1)
        self.assertEqual(port.flushes, 1)
        self.assertTrue(transport.correlated)

    def test_actual_seriallines_short_write_fails_without_retry(self):
        class ShortPort:
            def __init__(self): self.tx = []; self.flushes = 0
            def write(self, data): self.tx.append(data); return len(data) - 1
            def flush(self): self.flushes += 1
        port = ShortPort(); transport = B.PinnedTransport(B.SerialLines(port))
        with self.assertRaises(B.ProtocolError): transport.resynchronize(.1)
        self.assertEqual(len(port.tx), 1)
        self.assertFalse(transport.correlated)
        self.assertIsNotNone(transport.active)

    def test_actual_format_human_info_with_repeated_legacy_keys(self):
        # Actual human MBLUR format intentionally repeats ran/skipped keys.
        # Machine identity is complete and inline; no saved build log is required.
        lines = Firmware().info() + ["MBLUR h ran=0 skipped=0  v ran=0 skipped=0  ns/px h=100 v=150"]
        self.assertTrue(any(s.startswith("MBLUR h ran=") and s.count("ran=") == 2 for s in lines))
        result = B.check_identity(lines, IDENTITY)
        self.assertEqual(result["source_sha256"], IDENTITY["source_tree_sha256"])
        with self.assertRaises(B.AdmissionError):
            B.check_identity(lines + [next(x for x in lines if x.startswith("NOSBUILD "))], IDENTITY)


def c_function(source, signature):
    start=source.index(signature);brace=source.index("{",start);depth=1;end=brace+1
    while depth:
        depth+=(source[end]=="{")-(source[end]=="}");end+=1
    return source[start:end]


def full_error_position(self, app, direction, travel):
    """Historical controller retained as an explicit failing reference."""
    for _ in range(12):
        pose=self.settled_pose(app)
        offset,limit=B.integer(pose,"offset_q8")/256,B.integer(pose,"limit_q8")/256
        if limit<2*travel+60:raise B.AdmissionError("List too short")
        target=limit*(.4 if direction=="up" else .6)
        if abs(offset-target)<=60:return pose
        distance=max(30,min(300,round(abs(target-offset))))
        duration=max(400,round(distance/150*1000))
        y0,y1=233+distance//2,233-(distance-distance//2)
        if target<offset:y0,y1=y1,y0
        self.mutate(f"swipe 233 {y0} 233 {y1} {duration}","historical full-error reference")
        self.session.sleep(duration/1000+.75)
    raise B.AdmissionError("Historical full-error controller did not converge")


class Feedback:
    """Settled actual-C projection; terminal velocity is an explicit fixture."""
    def __init__(self,native,offset=0,limit=2300,factor=1,stuck=False,reject=False):
        self.native,self.offset,self.limit,self.factor=native,offset,limit,factor
        self.stuck,self.reject=stuck,reject
        self.poses,self.gestures,self.sleeps=[],[],[]
        self.session=SimpleNamespace(sleep=self.sleeps.append)

    def settled_pose(self,app):
        self.poses.append(self.offset)
        return {"app":"7","axis":"2","offset_q8":str(round(self.offset*256)),
                "limit_q8":str(round(self.limit*256)),"dragging":"0"}

    def mutate(self,command,purpose):
        match=re.fullmatch(r"swipe 233 (\d+) 233 (\d+) (\d+)",command)
        assert match,command
        y0,y1,duration=map(int,match.groups());drag=y0-y1
        assert 15<=abs(drag)<=300 and 400<=duration<=2000
        assert 0<=y0<466 and 0<=y1<466
        self.gestures.append(command)
        if self.reject:raise B.CommandTimeout(command,"feedface")
        if not self.stuck:self.offset=self.native.landing(self.offset,drag,duration,self.factor,self.limit)


@unittest.skipUnless(shutil.which("gcc"),"GCC required for actual production snap/projection")
class SnapFeedbackTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True,exist_ok=True)
        cls.out=Path(tempfile.mkdtemp(prefix="actual-snap-",dir=OUT))
        launcher=(ROOT/"nos/apps/sys_launcher.c").read_text()
        motion=(ROOT/"nos/motion.c").read_text();header=(ROOT/"nos/motion.h").read_text()
        row=re.search(r"^#define ROW_P\s+[^\n]+",launcher,re.M).group(0)
        decay=re.search(r"^#define DECAY_RATE\s+[^\n]+",header,re.M).group(0)
        source='#include <math.h>\n#include <stdint.h>\n#include "command_input.h"\n'+row+"\n"+decay+"\n"
        source+="static float limit;static float max_x(void){return limit;}\n"
        source+="static float m_clamp(float x,float low,float high){return x<low?low:x>high?high:x;}\n"
        source+=c_function(motion,"static float s_decay_tau(void)")+"\n"
        source+=c_function(motion,"float decay_project(float x, float v)")+"\n"
        source+=c_function(launcher,"static float snap_row(float x)")+"\n"
        source+='''
#ifdef _WIN32
#define EXPORT __declspec(dllexport)
#else
#define EXPORT
#endif
EXPORT double landing(double x,double drag,double duration,double factor,double ceiling){
 limit=(float)ceiling;float velocity=(float)(drag/(duration/1000)*factor);
 return snap_row(decay_project((float)(x+drag),velocity));
}
EXPORT int classify(const unsigned char *p,int size,unsigned *bytes,unsigned *reason,unsigned *bad){
 command_input input;command_input_reset(&input);int result=0;
 for(int i=0;i<size;i++)result=command_input_push(&input,p[i]);
 *bytes=input.bytes;*reason=input.reason;*bad=input.first_bad;return result;
}
'''
        path=cls.out/"actual_native.c";path.write_text(source)
        binary=cls.out/("actual_native.dll" if os.name=="nt" else "actual_native.so")
        env=os.environ | {key:str(cls.out) for key in ("TEMP","TMP","TMPDIR")}
        flags=[] if os.name=="nt" else ["-fPIC"]
        result=subprocess.run(["gcc","-std=c11","-O2","-Wall","-Wextra","-Werror","-shared",*flags,
                              "-Inos",str(path),"nos/command_input.c","-lm","-o",str(binary)],
                              cwd=ROOT,env=env,capture_output=True,text=True,timeout=45)
        (cls.out/"compile.log").write_text(result.stdout+result.stderr)
        if result.returncode:raise AssertionError(result.stdout+result.stderr)
        cls.native=ctypes.CDLL(str(binary))
        cls.native.landing.argtypes=[ctypes.c_double]*5;cls.native.landing.restype=ctypes.c_double
        cls.native.classify.argtypes=[ctypes.c_char_p,ctypes.c_int]+[ctypes.POINTER(ctypes.c_uint)]*3
        cls.native.classify.restype=ctypes.c_int
        inputs=("nos/apps/sys_launcher.c","nos/motion.c","nos/motion.h","nos/command_input.c","nos/command_input.h")
        (cls.out/"inputs.json").write_text(json.dumps({path:hashlib.sha256((ROOT/path).read_bytes()).hexdigest() for path in inputs},indent=2)+"\n")

    def test_actual_old_full_error_rule_reproduces_snap_oscillation(self):
        feedback=Feedback(self.native)
        with self.assertRaises(B.AdmissionError):full_error_position(feedback,"apps","up",150)
        self.assertEqual(feedback.poses[:4],[0,368,736,1012])
        self.assertEqual(feedback.poses[4:],[828,1012]*4)
        self.assertEqual(len(feedback.gestures),12)
        self.assertEqual(feedback.gestures[0],"swipe 233 383 233 83 2000")

    def test_actual_half_error_converges_with_identical_snap_and_inertia(self):
        feedback=Feedback(self.native)
        result=B.Benchmark.position(feedback,"apps","up",150)
        self.assertEqual(feedback.poses,[0,368,736,920])
        self.assertEqual(result["offset_q8"],str(920*256))
        self.assertEqual(len(feedback.gestures),3)

    def test_directions_origins_limits_and_terminal_velocity_variation(self):
        cases=0
        for limit in (828,1840,2300,2760):
            for start in (0,limit,92*int(limit/184+.5)):
                for factor in (.5,.75,1,1.25):
                    for direction in ("up","down"):
                        with self.subTest(limit=limit,start=start,factor=factor,direction=direction):
                            feedback=Feedback(self.native,start,limit,factor)
                            result=B.Benchmark.position(feedback,"apps",direction,150)
                            target=limit*(.4 if direction=="up" else .6)
                            self.assertLessEqual(abs(int(result["offset_q8"])/256-target),60)
                            self.assertLessEqual(len(feedback.gestures),12)
                            self.assertEqual(len(feedback.poses),len(feedback.gestures)+1)
                            cases+=1
        self.assertEqual(cases,96)

    def test_stuck_and_short_lists_remain_bounded_and_fail_closed(self):
        stuck=Feedback(self.native,stuck=True)
        with self.assertRaises(B.AdmissionError):B.Benchmark.position(stuck,"apps","up",150)
        self.assertEqual(len(stuck.gestures),12)
        short=Feedback(self.native,limit=300)
        with self.assertRaises(B.AdmissionError):B.Benchmark.position(short,"apps","up",150)
        self.assertEqual(short.gestures,[])

    def test_uncertain_positioning_mutation_is_not_replayed(self):
        feedback=Feedback(self.native,reject=True)
        with self.assertRaises(B.CommandTimeout) as error:B.Benchmark.position(feedback,"apps","up",150)
        self.assertTrue(error.exception.ambiguous)
        self.assertEqual(len(feedback.gestures),1)
        self.assertEqual(len(feedback.poses),1)

    def test_actual_parser_nul_replacement_insertion_and_barrier(self):
        def classify(data):
            count,reason,bad=ctypes.c_uint(),ctypes.c_uint(),ctypes.c_uint()
            result=self.native.classify(data,len(data),ctypes.byref(count),ctypes.byref(reason),ctypes.byref(bad))
            return result,count.value,reason.value,bad.value
        wire=b"req e28e87ed state"
        self.assertEqual(classify(wire+b"\n"),(1,len(wire),0,0))
        for position in range(len(wire)):
            altered=wire[:position]+b"\0"+wire[position+1:]
            self.assertEqual(classify(altered+b"\n"),(2,len(wire),1,0))
        for position in range(len(wire)+1):
            altered=wire[:position]+b"\0"+wire[position:]
            self.assertEqual(classify(altered+b"\n"),(2,len(wire)+1,1,0))
        self.assertEqual(classify(b"\0\n"),(2,1,1,0))
        self.assertEqual(classify(wire+b"\n"),(1,len(wire),0,0))


class JournalClock:
    def __init__(self):self.now=0.;self.waits=[]
    def monotonic(self):return self.now
    def sleep(self,duration):self.waits.append(duration);self.now+=duration


def journal_error():
    error=PermissionError(errno.EACCES,'Windows sharing fixture');error.winerror=5;return error


class JournalPersistenceTests(unittest.TestCase):
    def test_benchmark_reuses_shared_durable_atomic_writer(self):
        self.assertIs(B.write_json,host_support.write_json)

    def test_transient_pre_send_journal_lock_retries_rename_without_resending(self):
        with BenchmarkPlanTests().directory() as directory:
            firmware=Firmware();session=BenchmarkPlanTests().connect(directory,firmware)
            clock=JournalClock();real_replace=host_support.os.replace;failures=0
            def replace(source,target):
                nonlocal failures
                if Path(target).name=='commands.json' and failures<3:
                    failures+=1;raise journal_error()
                real_replace(source,target)
            with patch.object(host_support.os,'replace',side_effect=replace), \
                 patch.object(host_support.time,'monotonic',side_effect=clock.monotonic), \
                 patch.object(host_support.time,'sleep',side_effect=clock.sleep), \
                 patch.object(session.transport,'execute',wraps=session.transport.execute) as execute:
                session.command('awake',mutation=True,purpose='offline journal test')
            self.assertEqual(clock.waits,[.01,.02,.04]);execute.assert_called_once()
            self.assertEqual(execute.call_args.kwargs['retries'],0)
            self.assertEqual(firmware.commands.count('awake'),1);self.assertEqual(firmware.state['awake'],1)
            self.assertEqual(json.loads((Path(directory)/'commands.json').read_text())[-1]['status'],'complete')

    def test_exhausted_pre_send_journal_writes_send_zero_device_commands(self):
        with BenchmarkPlanTests().directory() as directory:
            firmware=Firmware();session=BenchmarkPlanTests().connect(directory,firmware)
            path=Path(directory)/'commands.json';original=path.read_bytes();clock=JournalClock()
            with patch.object(host_support.os,'replace',side_effect=journal_error()), \
                 patch.object(host_support.time,'monotonic',side_effect=clock.monotonic), \
                 patch.object(host_support.time,'sleep',side_effect=clock.sleep), \
                 patch.object(session.transport,'execute',wraps=session.transport.execute) as execute:
                with self.assertRaises(PermissionError):session.command('awake',mutation=True)
            execute.assert_not_called();self.assertNotIn('awake',firmware.commands)
            self.assertEqual(firmware.state['awake'],0);self.assertEqual(path.read_bytes(),original)
            pending=list(Path(directory).glob('commands.json.*.tmp'));self.assertEqual(len(pending),1)
            self.assertEqual(json.loads(pending[0].read_text())[-1]['status'],'pending')
            self.assertAlmostEqual(clock.now,1.)

    def test_failed_final_journal_after_ack_never_replays_command_or_query(self):
        for command,mutation in (('awake',True),('state',False)):
            for transient in (True,False):
                with self.subTest(command=command,transient=transient),BenchmarkPlanTests().directory() as directory:
                    firmware=Firmware();session=BenchmarkPlanTests().connect(directory,firmware)
                    clock=JournalClock();real_replace=host_support.os.replace
                    def replace(source,target):
                        data=json.loads(Path(source).read_text())
                        if Path(target).name=='commands.json' and data and data[-1]['status']=='complete':
                            raise journal_error() if transient else OSError(errno.EIO,'permanent journaling fixture')
                        real_replace(source,target)
                    with patch.object(host_support.os,'replace',side_effect=replace), \
                         patch.object(host_support.time,'monotonic',side_effect=clock.monotonic), \
                         patch.object(host_support.time,'sleep',side_effect=clock.sleep), \
                         patch.object(session.transport,'execute',wraps=session.transport.execute) as execute:
                        with self.assertRaises(OSError):
                            if mutation:session.command(command,mutation=True)
                            else:session.read(command)
                    execute.assert_called_once();self.assertEqual(execute.call_args.kwargs['retries'],0)
                    self.assertEqual(firmware.commands.count(command),1)
                    if mutation:self.assertEqual(firmware.state['awake'],1)
                    # On-disk pre-send record remains intact, with a complete durable
                    # replacement retained separately; no partial destination or replay.
                    path=Path(directory)/'commands.json'
                    self.assertEqual(json.loads(path.read_text())[-1]['status'],'pending')
                    temps=list(Path(directory).glob('commands.json.*.tmp'));self.assertEqual(len(temps),1)
                    self.assertEqual(json.loads(temps[0].read_text())[-1]['status'],'complete')
                    self.assertEqual(clock.waits!=[],transient)


REQUIRED_READ_RECORDS = {"hostcaps":"NOSHOST","info":"NOSBUILD","state":"NOSSTATE",
    "runtime_state":"NOSRUNTIME","nav_state":"NOSNAV","scroll_state":"NOSSCROLL",
    "prefs":"NOSPREFS","diag log":"NOSLOG"}


class PayloadFirmware(Firmware):
    def __init__(self,command,modes=()):
        super().__init__();self.target=command;self.modes=deque(modes)
        self.armed=self.barrier=False;self.requests=[];self.extra=[]

    def respond(self,command):
        body=super().respond(command)
        if self.armed and not self.barrier and command==self.target:
            marker=REQUIRED_READ_RECORDS[command]
            if self.modes:
                mode=self.modes.popleft();record=next(value for value in body if value.split()[0]==marker)
                if mode=='missing':body=[value for value in body if value.split()[0]!=marker]
                elif mode=='duplicate':body.append(record)
                elif mode=='bad_field':body=[value+' duplicate=1 duplicate=2' if value==record else value for value in body]
                else:raise AssertionError(mode)
            body+=self.extra
        return body


class PayloadLink(FakeLink):
    def write(self,data):
        self.firmware.barrier=True
        try:super().write(data)
        finally:self.firmware.barrier=False

    def send(self,wire):
        if self.firmware.armed and not self.firmware.barrier:
            _,nonce,command=wire.split(' ',2)
            self.firmware.requests.append({'id':nonce,'command':command})
        super().send(wire)


class MandatoryReadPayloadTests(unittest.TestCase):
    def connected(self,directory,command,modes=()):
        firmware=PayloadFirmware(command,modes)
        session=B.Session('OFFLINE-NO-DEVICE',directory,link_factory=lambda:PayloadLink(firmware),sleep=lambda _:None)
        session.connect();firmware.armed=True
        return firmware,session

    def test_missing_or_duplicate_required_records_retry_once_after_fresh_barrier(self):
        for command,record in REQUIRED_READ_RECORDS.items():
            for mode in ('missing','duplicate'):
                with self.subTest(command=command,mode=mode),BenchmarkPlanTests().directory() as directory:
                    firmware,session=self.connected(directory,command,(mode,))
                    first_event=len(firmware.events);before_writes=len(firmware.writes)
                    result=session.read(command)
                    self.assertEqual(len(B.records(result)[record]),1)
                    self.assertFalse(session.tainted);self.assertEqual(len(firmware.requests),2)
                    self.assertEqual([item['command'] for item in firmware.requests],[command,command])
                    self.assertEqual(len(firmware.writes),before_writes+1)
                    events=firmware.events[first_event:]
                    self.assertEqual(events,[('command',command),('barrier','NUL'),('command','hostcaps'),('command',command)])
                    barrier_id=firmware.writes[-1][6:14].decode()
                    self.assertNotIn(barrier_id,[item['id'] for item in firmware.requests])
                    self.assertEqual([item['status'] for item in session.commands],['failed','complete'])
                    self.assertEqual(session.commands[0]['request_id'],firmware.requests[0]['id'])
                    self.assertEqual(session.recoveries[-1]['status'],'complete')

    def test_second_bad_body_fails_closed_tainted_and_blocks_mutation(self):
        for command in REQUIRED_READ_RECORDS:
            with self.subTest(command=command),BenchmarkPlanTests().directory() as directory:
                firmware,session=self.connected(directory,command,('missing','duplicate'))
                before_writes=len(firmware.writes)
                with self.assertRaisesRegex(B.ProtocolError,'Incomplete read-only reply'):session.read(command)
                self.assertTrue(session.tainted);self.assertEqual(len(firmware.requests),2)
                self.assertEqual(len(firmware.writes),before_writes+1)
                self.assertEqual([item['status'] for item in session.commands],['failed','failed'])
                self.assertEqual(session.commands[-1]['request_id'],firmware.requests[-1]['id'])
                with self.assertRaises(B.ProtocolError):session.command('awake',mutation=True)
                self.assertNotIn('awake',firmware.commands);self.assertEqual(firmware.state['awake'],0)
                self.assertEqual(len(firmware.requests),2)

    def test_repeated_unrelated_records_and_human_repeated_keys_are_allowed(self):
        with BenchmarkPlanTests().directory() as directory:
            firmware,session=self.connected(directory,'state')
            firmware.extra=['NOSPIPEFRAME role=previous frame=1','NOSPIPEFRAME role=current frame=2',
                            'MBLUR h ran=0 skipped=0 v ran=0 skipped=0']
            before_writes=len(firmware.writes);result=session.read('state')
            self.assertEqual(len(B.records(result)['NOSPIPEFRAME']),2)
            self.assertEqual(len(firmware.requests),1);self.assertEqual(len(firmware.writes),before_writes)
            self.assertFalse(session.tainted)

    def test_malformed_required_record_is_protocol_failure_before_return(self):
        with BenchmarkPlanTests().directory() as directory:
            firmware,session=self.connected(directory,'state',('bad_field',))
            with self.assertRaisesRegex(B.ProtocolError,'Duplicate field duplicate'):
                session.command('state')
            self.assertTrue(session.tainted);self.assertEqual(len(firmware.requests),1)
            self.assertEqual(session.commands[0]['request_id'],firmware.requests[0]['id'])
            self.assertIn('duplicate=1 duplicate=2',session.commands[0]['lines'][1])

    def test_empty_mutation_reply_is_not_retried_or_given_a_read_schema(self):
        with BenchmarkPlanTests().directory() as directory:
            firmware,session=self.connected(directory,'state')
            before_writes=len(firmware.writes)
            self.assertEqual(session.command('home',mutation=True),[])
            self.assertEqual(firmware.commands.count('home'),1);self.assertEqual(len(firmware.requests),1)
            self.assertEqual(len(firmware.writes),before_writes)
            self.assertFalse(session.tainted)

    def test_unreviewed_query_has_no_record_guess_or_device_send(self):
        with BenchmarkPlanTests().directory() as directory:
            firmware,session=self.connected(directory,'state');before_writes=len(firmware.writes)
            with self.assertRaises(ValueError):session.read('battery')
            self.assertEqual(firmware.requests,[]);self.assertEqual(len(firmware.writes),before_writes)

    def test_recovery_nonce_cannot_reuse_completed_bad_read_id(self):
        with BenchmarkPlanTests().directory() as directory, \
             patch('tools.host_transport.secrets.token_hex',side_effect=['11111111','22222222','22222222','33333333','44444444']):
            firmware,session=self.connected(directory,'state',('missing',))
            result=session.read('state')
            self.assertEqual(len(B.records(result)['NOSSTATE']),1)
            self.assertEqual([item['id'] for item in firmware.requests],['22222222','44444444'])
            self.assertEqual(firmware.writes[-1],b'\x00\nreq 33333333 hostcaps\n')
            self.assertFalse(session.tainted)


class IdentityPayloadFirmware(PayloadFirmware):
    def __init__(self, replacements=()):
        super().__init__("info"); self.replacements = deque(replacements)

    def respond(self, command):
        body = super().respond(command)
        if command == "info" and self.armed and not self.barrier and self.replacements:
            replacement = self.replacements.popleft()
            if isinstance(replacement, list):
                return replacement
            record = B.one(B.records(body), "NOSBUILD")
            for key, value in replacement.items():
                if value is None: record.pop(key, None)
                else: record[key] = value
            body = [line("NOSBUILD", record) if value.startswith("NOSBUILD ") else value for value in body]
        return body


class IdentityBodyTests(unittest.TestCase):
    def connected(self, directory, replacements=(), link_type=PayloadLink):
        firmware = IdentityPayloadFirmware(replacements)
        session = B.Session("OFFLINE-NO-DEVICE", directory, link_factory=lambda: link_type(firmware), sleep=lambda _: None)
        session.connect(); firmware.armed = True
        return firmware, session

    def test_malformed_identity_fields_retry_once_before_returning_a_complete_read(self):
        invalid = [{key: value} for key in ("source_sha256", "elf_sha256")
                   for value in (None, "", "1"*63, "g"*64)]
        invalid += [{"source_build_id":value} for value in (None, "", "2"*15, "2"*17, "3"*16)]
        invalid += [{"metrics":value} for value in (None, "", "other-device-v1")]
        for fields in invalid:
            with self.subTest(fields=fields), BenchmarkPlanTests().directory() as directory:
                firmware, session = self.connected(directory, (fields,))
                before = len(firmware.events)
                result = session.read("info")
                self.assertEqual(B.one(B.records(result), "NOSBUILD")["elf_sha256"], "1"*64)
                self.assertEqual([item["status"] for item in session.commands], ["failed", "complete"])
                self.assertEqual([item["command"] for item in firmware.requests], ["info", "info"])
                self.assertEqual(firmware.events[before:], [("command","info"), ("barrier","NUL"),
                                                          ("command","hostcaps"), ("command","info")])
                self.assertIn("Malformed NOSBUILD", session.commands[0]["error"])
                self.assertFalse(session.tainted)

    def test_actual_format_sdk_log_interleaving_is_rejected_without_stitching_fragments(self):
        # Inline actual wire shape; no ignored build log or expected flash hash.
        malformed = [f"NOSBUILD source_sha256={'2'*64} source_build_id={'2'*16} metrics=nos-device-v1 cpu_mhz=240 elf_sha256=19455f4ff3a9615e85I (532563) wifi:state: init -> auth (0xb0)",
                     "67e9c9d58b6b24905529e8cd0d08cf6fff0df75e8f117c idf=897cf407 stripe_rows=16"]
        with BenchmarkPlanTests().directory() as directory:
            firmware, session = self.connected(directory, (malformed,))
            result = session.read("info")
            self.assertEqual(firmware.commands.count("info"), 2)
            self.assertEqual(session.commands[0]["lines"], malformed)
            self.assertIn("Malformed NOSBUILD elf_sha256", session.commands[0]["error"])
            self.assertEqual(B.one(B.records(result), "NOSBUILD")["elf_sha256"], "1"*64)
            self.assertNotIn(malformed[1], result)

    def test_second_malformed_identity_fails_tainted_and_never_replays_mutations(self):
        with BenchmarkPlanTests().directory() as directory:
            firmware, session = self.connected(directory, ({"elf_sha256":"1"*63}, {"source_build_id":"3"*16}))
            with self.assertRaisesRegex(B.ProtocolError, "Malformed NOSBUILD"):
                session.read("info")
            self.assertTrue(session.tainted)
            self.assertEqual([item["status"] for item in session.commands], ["failed", "failed"])
            self.assertEqual(firmware.commands.count("info"), 2)
            self.assertEqual(len(firmware.writes), 2)  # Initial + one fresh recovery barrier.
            with self.assertRaises(B.ProtocolError): session.command("awake", mutation=True)
            self.assertNotIn("awake", firmware.commands)

    def test_well_formed_different_identity_is_left_for_expected_build_admission(self):
        with BenchmarkPlanTests().directory() as directory:
            firmware, session = self.connected(directory, ({"source_sha256":"3"*64, "source_build_id":"3"*16,
                                                             "elf_sha256":"4"*64},))
            result = session.read("info")
            self.assertEqual(firmware.commands.count("info"), 1)
            self.assertEqual(len(firmware.writes), 1)
            self.assertEqual(session.commands[0]["status"], "complete")
            with self.assertRaises(B.AdmissionError): B.check_identity(result, IDENTITY)


class PacedWriteTests(unittest.TestCase):
    def test_actual_paced_barrier_preserves_all_bytes_and_write_flush_sleep_order(self):
        events = []
        class Port:
            def __init__(self): self.pending = bytearray(); self.rx = deque()
            @property
            def in_waiting(self): return 1
            def write(self, data):
                events.append(("write", data)); self.pending.extend(data)
                if self.pending.endswith(b"\n"):
                    packet = bytes(self.pending); self.pending.clear()
                    if packet.startswith(b"req "):
                        nonce = packet.split()[1].decode()
                        reply = f"BEGIN {nonce}\n{CAPS}\nDONE {nonce} ok=1\n".encode()
                        self.rx.extend(reply[i:i+3] for i in range(0, len(reply), 3))
                return len(data)
            def flush(self): events.append(("flush",))
            def read(self, count): return self.rx.popleft() if self.rx else b""
        port = Port(); transport = B.PinnedTransport(B.PacedSerialLines(port))
        packet = b"\x00\nreq 1234abcd hostcaps\n"
        with patch("secrets.token_hex", return_value="1234abcd"), \
             patch.object(B.time, "sleep", side_effect=lambda delay: events.append(("sleep", delay))):
            transport.resynchronize(.5)
        expected = []
        for byte in packet:
            expected.extend([("write", bytes([byte])), ("flush",), ("sleep", .002)])
        self.assertEqual(events, expected)
        self.assertTrue(transport.correlated); self.assertIsNone(transport.active)
        self.assertEqual(transport.last_result["id"], "1234abcd")

    def test_partial_paced_mutation_stops_at_failure_without_suffix_or_replay(self):
        for failure in ("short_write", "flush_error"):
            with self.subTest(failure=failure), BenchmarkPlanTests().directory() as directory:
                events = []
                class Port:
                    def __init__(self): self.writes = []
                    def write(self, data):
                        self.writes.append(data); events.append(("write", data))
                        return 0 if failure == "short_write" and len(self.writes) == 6 else len(data)
                    def flush(self):
                        events.append(("flush",))
                        if failure == "flush_error" and len(self.writes) == 6:
                            raise OSError("underlying flush failed")
                port = Port(); link = B.PacedSerialLines(port)
                session = B.Session("OFFLINE-NO-DEVICE", directory, link_factory=lambda: link,
                                    usb_paced_writes=True)
                session.new_transport(); session.transport.correlated = True
                session.transport.capabilities = {"v":"1", "req":"1", "max_command":"191"}
                with patch("secrets.token_hex", return_value="1234abcd"), \
                     patch.object(B.time, "sleep", side_effect=lambda delay: events.append(("sleep", delay))):
                    with self.assertRaises(B.ProtocolError if failure == "short_write" else OSError):
                        session.command("awake", mutation=True)
                    self.assertTrue(session.tainted)
                    with self.assertRaises(B.ProtocolError): session.command("awake", mutation=True)
                packet = b"req 1234abcd awake\n"
                self.assertEqual(port.writes, [bytes([byte]) for byte in packet[:6]])
                self.assertEqual(sum(event[0] == "sleep" for event in events), 5)
                self.assertEqual(sum(event[0] == "flush" for event in events), 5 if failure == "short_write" else 6)
                self.assertEqual(len(session.commands), 1)
                self.assertEqual(session.commands[0]["status"], "unknown")
                self.assertEqual(session.transport.active["command"], "awake")

    def test_session_default_and_explicit_factories_are_preserved(self):
        with BenchmarkPlanTests().directory() as directory:
            normal, paced, supplied = object(), object(), object()
            with patch.object(B.PacedSerialLines, "open", return_value=paced) as pace_open, \
                 patch.object(B.SerialLines, "open", return_value=normal) as normal_open:
                self.assertIs(B.Session("OFFLINE", directory).link_factory(), normal)
                self.assertIs(B.Session("OFFLINE", directory, usb_paced_writes=True).link_factory(), paced)
                factory = lambda: supplied
                session = B.Session("OFFLINE", directory, link_factory=factory, usb_paced_writes=True)
                self.assertIs(session.link_factory, factory); self.assertIs(session.link_factory(), supplied)
                normal_open.assert_called_once_with("OFFLINE", timeout=.15, write_timeout=5)
                pace_open.assert_called_once_with("OFFLINE", timeout=.15, write_timeout=5)

    def test_main_records_and_passes_selected_pacing_without_opening_a_port(self):
        with BenchmarkPlanTests().directory() as directory:
            build = Path(directory) / "fixture-build"; build.mkdir()
            (build / "build-identity.json").write_text("{}\n")
            for paced in (False, True):
                output = Path(directory) / f"fixture-run-{paced}"
                args = ["--run", "--port", "OFFLINE-NO-DEVICE", "--build", str(build), "--out", str(output)]
                if paced: args.append("--usb-paced-writes")
                with patch("tools.build_identity.verify", return_value=IDENTITY), \
                     patch.object(B, "Session", autospec=True) as session_type, \
                     patch.object(B, "Benchmark", autospec=True) as benchmark_type, \
                     patch("sys.stdout", new_callable=io.StringIO):
                    benchmark_type.return_value.run.return_value = {"status":"measured", "cleanup":{"status":"verified"},
                        "summary":{}, "measured_all_windows_meet_30hz_floor":False}
                    self.assertEqual(B.main(args), 0)
                session_type.assert_called_once_with("OFFLINE-NO-DEVICE", output.resolve(), usb_paced_writes=paced)
                session_type.return_value.connect.assert_called_once()
                session_type.return_value.close.assert_called_once()
                invocation = json.loads((output / "invocation.json").read_text())
                self.assertIs(invocation["usb_paced_writes"], paced)
                self.assertEqual(invocation["argv"], args)

    def test_opt_in_plan_remains_offline(self):
        with patch.object(B.PacedSerialLines, "open", side_effect=AssertionError("must not open")), \
             patch.object(B.SerialLines, "open", side_effect=AssertionError("must not open")), \
             patch("sys.stdout", new_callable=io.StringIO):
            self.assertEqual(B.main(["--usb-paced-writes"]), 0)


if __name__=="__main__":unittest.main()
