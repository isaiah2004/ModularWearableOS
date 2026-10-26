import importlib.util
import json
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("nos_perf_report", Path(__file__).parents[1] / "tools/nos_perf_report.py")
REPORT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(REPORT)


class NosPerfReportTest(unittest.TestCase):
    def setUp(self):
        self.manifest = {"source_revision": "abc123", "tree_sha256": "a" * 64,
                         "firmware_elf_sha256": "b" * 64, "cpu_mhz": 160,
                         "capture_scope": "animation", "instrumentation": "nos-device-v1",
                         "blur_coverage": "verified", "blur_coverage_evidence": "coverage.json"}
        self.frame1 = ("NOSF frame_id=10 begin_us=100000 completed_us=120000 e2e=20000 "
                       "deadline_us=-5000 rec=1000 ras=15000 wait=5000 allocfail=0")
        self.frame2 = self.frame1.replace("id=10", "id=11").replace("100000", "125000").replace("120000", "145000")

    def report(self, text, **changes):
        cpu = changes.get("cpu_mhz", self.manifest["cpu_mhz"])
        return REPORT.analyze(f"NOSDIAG layers=1 history=1\nNOSPM cpu_mhz={cpu} max_mhz={cpu}\n" + text, self.manifest | changes)

    def test_complete_recorded_window(self):
        report = self.report(self.frame1 + "\n" + self.frame2)
        self.assertEqual(report["status"], "pass_observed")
        self.assertEqual(report["max_completion_interval_ms"], 25)

    def test_uniform_shortcut_ab_does_not_disable_blur(self):
        text = "NOSDIAG history_uniform=0\n" + self.frame1 + "\n" + self.frame2
        self.assertEqual(self.report(text)["status"], "pass_observed")
        self.assertEqual(REPORT.analyze(text, self.manifest)["checks"]["production_features"], "incomplete")
        partial = self.report(text + "\nNOSDIAG layers=1")
        self.assertEqual(partial["checks"]["production_features"], "pass")

    def test_partial_production_reports_merge_without_inventing_values(self):
        report = REPORT.analyze("NOSDIAG layers=1\n" + self.frame1, self.manifest)
        self.assertEqual(report["checks"]["production_features"], "incomplete")
        report = REPORT.analyze("NOSDIAG layers=1\nNOSDIAG history=1\n" + self.frame1,
                                self.manifest)
        self.assertEqual(report["checks"]["production_features"], "pass")
        self.assertEqual(self.report("NOSDIAG history=0")["checks"]["production_features"],
                         "fail")

    def test_async_controls_retained_without_double_counting_history(self):
        text = ("NOSWORKER exposure_worker=1 history_packed=1 job_bytes=2064\n"
                "NOSDIAG history_packed=0\n" + self.frame1 + " history_us=90000\n" +
                self.frame2 + " history_us=90000")
        report = self.report(text, cpu_mhz=240)
        self.assertEqual(report["checks"]["production_features"], "pass")
        self.assertEqual(report["checks"]["target40"], "pass")
        self.assertIsNone(report["cpu200_estimate_max_ms"])
        self.assertEqual(next(row for row in report["runtime_controls"] if row["kind"] == "NOSWORKER")["fields"]["exposure_worker"], "1")
        self.assertTrue(any("must not be added" in note for note in report["notes"]))
        # An explicit per-frame off flag supersedes previously reported mode.
        report = self.report(text.replace("history_us=90000", "exposure_worker=0"), cpu_mhz=240)
        self.assertAlmostEqual(report["cpu200_estimate_max_ms"], 22.2)

    def test_target_failure_can_pass_floor(self):
        second = self.frame2.replace("145000", "146000").replace("e2e=20000", "e2e=21000")
        report = self.report(self.frame1 + "\n" + second)
        self.assertEqual(report["checks"]["target40"], "fail")
        self.assertEqual(report["checks"]["floor30"], "pass")

    def test_floor_failure(self):
        second = self.frame2.replace("145000", "160000").replace("e2e=20000", "e2e=35000")
        self.assertEqual(self.report(self.frame1 + "\n" + second)["checks"]["floor30"], "fail")

    def test_floor_boundary_uses_thirty_hz_not_rounded_up_period(self):
        text = self.frame1 + "\n" + self.frame2.replace("145000", "153333").replace("e2e=20000", "e2e=28333")
        self.assertEqual(self.report(text)["checks"]["floor30"], "pass")
        text = self.frame1 + "\n" + self.frame2.replace("145000", "153334").replace("e2e=20000", "e2e=28334")
        self.assertEqual(self.report(text)["checks"]["floor30"], "fail")

    def test_sparse_trace_does_not_fabricate_interval(self):
        second = self.frame2.replace("id=11", "id=99").replace("145000", "945000").replace("e2e=20000", "e2e=820000")
        report = self.report(self.frame1 + "\n" + second)
        self.assertEqual(report["checks"]["floor30"], "incomplete")
        self.assertIsNone(report["max_completion_interval_ms"])

    def test_legacy_metrics_and_receipt_time_never_certify(self):
        text = "\n".join(json.dumps({"t": i / 40, "line": "NOSPERF fps=40 avg_ms=20 max_ms=22 late=0"}) for i in range(3))
        report = self.report(text)
        self.assertEqual(report["status"], "incomplete")
        self.assertEqual(report["samples"]["device_timestamped_frames"], 0)
        self.assertEqual(report["capture_last_timestamp"], 0.05)

    def test_summary_provenance_and_coverage(self):
        text = "NOSCACHE allocfail=0 cache_bytes=1300000 blur_bytes=100000\nNOSPERF rate=40 e2e_max=23 deadline=0 target_slots=0 floor=0 present_max=25 present_intervals=39"
        self.assertEqual(self.report(text)["status"], "pass_observed")
        self.assertEqual(self.report(text, instrumentation="unknown")["status"], "incomplete")
        self.assertEqual(self.report(text, blur_coverage="unmeasured")["status"], "incomplete")

    def test_cpu_scales_only_known_cpu_and_never_certifies_estimate(self):
        report = self.report(self.frame1 + "\n" + self.frame2, cpu_mhz=240)
        self.assertAlmostEqual(report["cpu200_estimate_max_ms"], 22.2)
        self.assertEqual(report["checks"]["cpu200_budget"], "incomplete")
        frame = self.frame1.replace("e2e=20000", "e2e=24000").replace("completed_us=120000", "completed_us=124000")
        report = self.report(frame, cpu_mhz=240)
        self.assertAlmostEqual(report["cpu200_estimate_max_ms"], 26.2)
        self.assertEqual(report["status"], "estimated_budget_failure")

    def test_tick_in_cpu_estimate_and_allocation_failure(self):
        report = self.report(self.frame1 + " tick_us=2000", cpu_mhz=240)
        self.assertAlmostEqual(report["cpu200_estimate_max_ms"], 22.6)
        report = self.report(self.frame1.replace("allocfail=0", "allocfail=1"))
        self.assertEqual(report["checks"]["allocations"], "fail")
        self.assertEqual(report["allocation_failures_max"], 1)

    def test_disabled_history_never_qualifies_production(self):
        text = self.frame1 + "\n" + self.frame2 + "\nNOSDIAG layers=1 history=0"
        report = self.report(text)
        self.assertEqual(report["checks"]["production_features"], "fail")
        self.assertEqual(report["status"], "observed_failure")
        report = REPORT.analyze(self.frame1 + "\n" + self.frame2, self.manifest)
        self.assertEqual(report["checks"]["production_features"], "incomplete")

    def test_manifest_hash_mismatch(self):
        text = "NOSBUILD metrics=nos-device-v1 cpu_mhz=160 elf_sha256=" + "c" * 64
        self.assertEqual(self.report(text)["checks"]["identity"], "fail")
        self.assertEqual(self.report(self.frame1, tree_sha256="unknown")["checks"]["identity"], "incomplete")

    def test_mixed_capture_windows(self):
        report = self.report(self.frame1 + "\n" + self.frame2, capture_scope="mixed",
                             animation_windows=[{"start_us": 100000, "end_us": 125000}])
        self.assertEqual(report["samples"]["frames"], 1)
        self.assertEqual(report["checks"]["floor30"], "incomplete")

    def test_watchctl_jsonl(self):
        text = json.dumps({"time": "2026-10-16T00:00:00Z", "response": [self.frame1, self.frame2]})
        self.assertEqual(self.report(text)["status"], "pass_observed")

    def test_idle_or_legacy_summary_cannot_pass_cadence(self):
        for suffix in ("painted=0", "painted=40", "painted=40 present_intervals=0"):
            report = self.report("NOSCACHE allocfail=0\nNOSPERF rate=40 e2e_max=0 deadline=0 floor=0 present_max=0 target_slots=0 " + suffix)
            self.assertEqual(report["status"], "incomplete")
            for key in ("animation_scope", "target40", "floor30", "cpu200_budget"):
                self.assertEqual(report["checks"][key], "incomplete")
        legacy = self.report("NOSPERF rate=40 painted=40 e2e_max=20 deadline=0 floor=0 present_max=25 target_slots=0")
        self.assertEqual(legacy["checks"]["floor30"], "incomplete")

    def test_summary_completion_interval_uses_same_target_as_frames(self):
        report = self.report("NOSPERF rate=40 e2e_max=20 deadline=0 target_slots=0 floor=0 present_max=30 present_intervals=39")
        self.assertEqual(report["checks"]["target40"], "fail")
        self.assertEqual(report["checks"]["floor30"], "pass")
        report = self.report("NOSPERF present_max=30 floor=0")
        self.assertEqual(report["checks"]["target40"], "fail")

    def test_summary_misses_fail_without_interval_count(self):
        report = self.report("NOSPERF rate=40 painted=0 e2e_max=40 deadline=1 floor=1 present_max=40 target_slots=1")
        self.assertEqual(report["checks"]["target40"], "fail")
        self.assertEqual(report["checks"]["floor30"], "fail")
        self.assertEqual(report["status"], "observed_failure")

    def test_explicit_zero_paint_or_frame_gap_is_not_motion_evidence(self):
        for second in (self.frame2 + " px=0", self.frame2.replace("id=11", "id=99")):
            report = self.report(self.frame1 + "\n" + second, trace_contiguous=True)
            self.assertEqual(report["checks"]["floor30"], "incomplete")
            self.assertEqual(report["checks"]["target40"], "incomplete")

    def test_cpu_config_alone_cannot_establish_active_clock_budget(self):
        base = "NOSDIAG layers=1 history=1\nNOSBUILD metrics=nos-device-v1 cpu_mhz=160 elf_sha256=" + "b" * 64 + "\n"
        frames = self.frame1 + "\n" + self.frame2
        for clock in ("", "NOSPM cpu_mhz=160\n", "NOSPM max_mhz=160\n",
                      "NOSPM cpu_mhz=240 max_mhz=240\n", "NOSPM cpu_mhz=80 max_mhz=240\n",
                      "NOSPM cpu_mhz=240 max_mhz=160\n"):
            report = REPORT.analyze(base + clock + frames, self.manifest)
            self.assertEqual(report["checks"]["cpu200_budget"], "incomplete")
            self.assertNotEqual(report["status"], "pass_observed")
        report = REPORT.analyze(base + "NOSPM cpu_mhz=160 max_mhz=160\n" + frames, self.manifest)
        self.assertEqual(report["checks"]["cpu200_budget"], "pass")
        self.assertEqual(report["status"], "pass_observed")
        contradictory = REPORT.analyze(base + "NOSPM cpu_mhz=240 max_mhz=240\nNOSPM cpu_mhz=160 max_mhz=160\n" + frames, self.manifest)
        self.assertEqual(contradictory["checks"]["cpu200_budget"], "incomplete")

    def test_unknown_partial_or_late_controls_do_not_certify_blur(self):
        frames = self.frame1 + "\n" + self.frame2
        for controls in ("", "NOSDIAG layers=1\n", "NOSDIAG history=1\n"):
            report = REPORT.analyze(controls + frames, self.manifest)
            self.assertEqual(report["checks"]["production_features"], "incomplete")
            self.assertEqual(report["checks"]["blur_coverage"], "incomplete")
        late = REPORT.analyze(frames + "\nNOSDIAG layers=1 history=1", self.manifest)
        self.assertEqual(late["checks"]["production_features"], "incomplete")
        self.assertEqual(late["checks"]["blur_coverage"], "incomplete")

    def test_device_timestamps_derive_duration_and_reject_transport_contradictions(self):
        good = self.frame1 + "\n" + self.frame2
        derived = self.report(good.replace("e2e=20000 ", ""))
        self.assertEqual(derived["max_e2e_ms"], 20)
        self.assertEqual(derived["status"], "pass_observed")
        for bad in (good.replace("e2e=20000", "e2e=1000"),
                    good.replace("completed_us=120000 ", ""),
                    good.replace("begin_us=100000 ", ""),
                    good.replace("completed_us=120000", "completed_us=200000")):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                self.report(bad)

    def test_reject_bad_data(self):
        for text in ("{bad", "NOSF e2e=nan", "NOSF e2e=-3",
                     "NOSF e2e=12 rec=1 ras=10 wait=20", "NOSPERF floor=-1 present_max=20"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.report(text)
        with self.assertRaises(ValueError):
            self.report(self.frame1 + "\n" + self.frame1, trace_contiguous=True)


if __name__ == "__main__":
    unittest.main()
