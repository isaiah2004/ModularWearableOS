import importlib.util
import pathlib
import sys
import tempfile
import unittest
import struct

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
SPEC = importlib.util.spec_from_file_location("watch_power", ROOT / "tools/analyze_watch_power.py")
POWER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(POWER)
BASE = 1792500000


def event(boot, sequence, uptime, epoch, kind=4, values=None):
    return {"boot": boot, "sequence": sequence, "uptime_ms": uptime, "epoch": epoch,
            "type": kind, "v": (values or [0, 0, 0]) + [0] * (8 - len(values or [0, 0, 0]))}


class PowerAnalysisTests(unittest.TestCase):
    def test_unset_utc_and_clock_correction_are_explicit(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "0001.csv"
            path.write_text("utc_epoch,uptime_ms,system_mv,imu_valid\n"
                            f"60,60000,4000,1\n{BASE + 120},120000,3990,1\n"
                            f"{BASE + 1180},180000,3980,1\n")
            files, rows = POWER.read_csv_logs(path.parent)
            self.assertEqual(rows[0]["epoch"], BASE + 60)
            self.assertEqual(rows[0]["time_source"], "inferred_same_run_uptime")
            self.assertEqual(rows[2]["clock_segment"], 1)
            self.assertEqual([item["kind"] for item in files[0]["clock_events"]],
                             ["clock_established", "clock_correction"])

    def test_subsecond_records_count_duration_without_cross_boot_or_gap_fill(self):
        boots = {1: [event(1, 1, 0, BASE, 2, [0, 240, 100]), event(1, 2, 100, BASE),
                     event(1, 3, 1000, BASE + 1), event(1, 4, 2000, BASE + 2, 2, [1, 80, 10]),
                     event(1, 5, 3000, BASE + 3), event(1, 6, 200000, BASE + 200)],
                 2: [event(2, 1, 0, BASE + 500, 2, [0, 240, 100]), event(2, 2, 1000, BASE + 501)]}
        intervals, summaries, _ = POWER.journal_intervals(boots, 90)
        self.assertAlmostEqual(sum(item["observed_s"] for item in summaries), 4)
        self.assertAlmostEqual(summaries[0]["omitted_gap_s"], 197)
        summary = POWER.summarize_window(intervals, BASE - 1, BASE + 600)
        self.assertAlmostEqual(summary["observed_s"], 4)
        self.assertAlmostEqual(summary["power_state_s"]["active"], 3)
        self.assertAlmostEqual(summary["power_state_s"]["sensor_idle"], 1)

    def test_clock_correction_does_not_invent_elapsed_power_time(self):
        boots = {1: [event(1, 1, 0, BASE, 2, [0, 240, 100]),
                     event(1, 2, 1000, BASE + 100, 2, [1, 80, 10]), event(1, 3, 2000, BASE + 101)]}
        _, summaries, _ = POWER.journal_intervals(boots, 90)
        self.assertEqual(summaries[0]["observed_s"], 1)
        self.assertEqual(summaries[0]["power_state_s"], {"sensor_idle": 1})
        self.assertEqual(len(summaries[0]["clock_corrections"]), 1)

    def test_persistent_intake_ids_are_not_hardware_boots(self):
        boots = {0: [event(0, 1, 60000, BASE, 12), event(0, 2, 120000, BASE + 60, 13)]}
        intervals, summaries, _ = POWER.journal_intervals(boots, 90)
        self.assertFalse(intervals)
        self.assertEqual(summaries[0]["observed_s"], 0)
        self.assertEqual(summaries[0]["kind"], "persistent_intake_ledger")
        point = {"file": "0001.csv", "epoch": BASE, "uptime_ms": 60000}
        self.assertEqual(POWER.match_csv_boots([point], boots)[0]["confidence"], "unmatched")

    def test_recovery_sequence_gap_invalidates_held_power_state(self):
        boots = {1: [event(1, 1, 0, BASE, 2, [1, 80, 165]), event(1, 3, 1000, BASE + 1),
                     event(1, 4, 2000, BASE + 2), event(1, 5, 3000, BASE + 3, 2, [0, 240, 95]),
                     event(1, 6, 4000, BASE + 4)]}
        _, summaries, _ = POWER.journal_intervals(boots, 90, strict_sequence=True)
        self.assertEqual(summaries[0]["power_state_s"], {"unknown": 2, "active": 1})
        self.assertEqual(summaries[0]["unknown_sequence_gaps"], 1)

    def test_matching_requires_both_utc_and_uptime(self):
        points = [{"file": "0001.csv", "epoch": BASE, "uptime_ms": 60000}]
        boots = {1: [event(1, 1, 60020, BASE)], 2: [event(2, 1, 90000, BASE)]}
        matches = POWER.match_csv_boots(points, boots)
        self.assertEqual(matches[0]["boot"], 1)
        self.assertEqual(matches[0]["confidence"], "single_point")
        boots[2] = [event(2, 1, 60000, BASE)]
        self.assertEqual(POWER.match_csv_boots(points, boots)[0]["confidence"], "ambiguous")

    def test_checked_journal_deduplicates_and_reports_corruption_and_tail(self):
        prefix = struct.pack("<6I2H8f", 0x53505431, 1, 1, 1000, BASE, 20261020,
                             1000, 2, 0, 240, 100, 0, 0, 0, 0, 0)
        record = prefix + struct.pack("<I", POWER.sports_export.checksum(prefix))
        damaged = bytearray(record);damaged[20] ^= 1
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "events.spt"
            path.write_bytes(record + record + damaged + b"bad")
            boots, info = POWER.read_journal(path)
        self.assertEqual(len(boots[1]), 1)
        self.assertEqual(info["duplicates"], 1)
        self.assertEqual(info["truncated_bytes"], 3)
        self.assertEqual(len(info["integrity_messages"]), 2)

    def test_salvage_resynchronizes_without_modifying_unverified_source(self):
        prefix = struct.pack("<6I2H8f", 0x53505431, 1, 1, 1000, BASE, 20261020,
                             1000, 2, 0, 240, 100, 0, 0, 0, 0, 0)
        record = prefix + struct.pack("<I", POWER.sports_export.checksum(prefix))
        raw = b"broken" + record + b"junk" + record + b"tail"
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            path = folder / "input.unverified";path.write_bytes(raw)
            recovered, info = POWER.recover_journal(path, folder, 192, "unknown", 1)
            self.assertEqual(path.read_bytes(), raw)
            self.assertEqual(recovered.read_bytes(), record + record)
            self.assertEqual(info["recovered_records"], 2)
            self.assertEqual(info["discarded_received_bytes"], 14)
            self.assertFalse(info["whole_file_verified"])


if __name__ == "__main__":
    unittest.main()
