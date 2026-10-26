"""Independent binary/CLI checks for the frozen Sports v1 export format."""
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("workouts_export", ROOT / "tools/workouts_export.py")
exporter = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(exporter)


def snapshot(*, identifier=1, state=3, saved=2):
    value = bytearray(236)
    struct.pack_into("<8I", value, 0, identifier, 1792497600, 1792497725,
                     120000, 125000, 180, 100000, 110000)
    struct.pack_into("<6f", value, 32, 240, 2, 240 / 110, 500, 110000 / 240, 90)
    struct.pack_into("<3H", value, 56, 0, 140, 150)
    struct.pack_into("<11B", value, 62, 1, state, saved, 2, 1, 0, 0, 0, 0, 0, 2)
    struct.pack_into("<IfIf", value, 76, 60000, 120, 120000, 240)
    return value


def bank(sequence=1, *, history=None, current=None):
    value = bytearray(3104)
    history = [snapshot()] if history is None else history
    current = bytearray(236) if current is None else current
    struct.pack_into("<6IQ", value, 0, 0x574F5331, 1, 3104, sequence, len(history), 3, 0)
    value[32:268] = current
    for index, item in enumerate(history):
        value[268 + 236 * index:268 + 236 * (index + 1)] = item
    struct.pack_into("<I", value, 3100, exporter.fnv1a(value[:3100]))
    return value


def reseal(value):
    struct.pack_into("<I", value, 3100, exporter.fnv1a(value[:3100]))
    return value


class ExportTests(unittest.TestCase):
    def test_v3_provenance_and_rejection_extension(self):
        value = bytearray(exporter.V3_RECORD_BYTES)
        struct.pack_into("<6IQ",value,0,exporter.MAGIC,3,len(value),9,1,3,0)
        at = 32 + exporter.V3_SNAPSHOT_BYTES
        item = snapshot();item[62]=8
        value[at:at+236]=item
        struct.pack_into("<8B",value,at+236,1,0,2,0,1,70,2,55)
        struct.pack_into("<3IfII",value,at+244,100000,60000,15,6.7,2,19)
        struct.pack_into("<H",value,at+268+3*2,17)
        struct.pack_into("<4IB",value,at+308,5000,6000,7000,8000,3)
        struct.pack_into("<I",value,exporter.V3_CHECKSUM_OFFSET,exporter.fnv1a(value[:exporter.V3_CHECKSUM_OFFSET]))
        path=self.directory / "v3.bin";path.write_bytes(value)
        record=exporter.export(exporter.read_bank(path))["workouts"][0]
        self.assertEqual((record["worn_wrist"],record["dominant_hand"],record["playing_hand"],record["placement_revision"]),(1,0,2,19))
        self.assertEqual(record["quality"]["rejections"]["hr_quality"],17)
        self.assertEqual(record["quality"]["last_reason"],"hr_quality")
        self.assertEqual(record["quality"]["hr_sample_ms"],5000)
        value[at+324]=20
        struct.pack_into("<I",value,exporter.V3_CHECKSUM_OFFSET,exporter.fnv1a(value[:exporter.V3_CHECKSUM_OFFSET]))
        path.write_bytes(value)
        with self.assertRaises(exporter.InvalidBank):exporter.read_bank(path)

    def test_v4_energy_basis_body_and_series(self):
        value = bytearray(exporter.V4_RECORD_BYTES)
        struct.pack_into("<6IQ",value,0,exporter.MAGIC,4,len(value),9,1,3,0)
        at = 32 + exporter.V4_SNAPSHOT_BYTES
        item = snapshot();item[62]=8
        value[at:at+236]=item
        struct.pack_into("<8B",value,at+236,1,0,2,0,1,70,2,55)
        struct.pack_into("<3IfII",value,at+244,100000,60000,15,6.7,2,19)
        struct.pack_into("<3B",value,at+328,175,31,1)
        struct.pack_into("<If",value,at+332,321,3.5)
        struct.pack_into("<BBH",value,at+340,1|8,3,60)
        value[at+344:at+347]=bytes((70,255,100));value[at+464:at+467]=bytes((120,0,150))
        seal = lambda: struct.pack_into("<I",value,exporter.V4_CHECKSUM_OFFSET,exporter.fnv1a(value[:exporter.V4_CHECKSUM_OFFSET]))
        seal();path=self.directory / "v4.bin";path.write_bytes(value)
        record=exporter.export(exporter.read_bank(path))["workouts"][0]
        self.assertEqual(record["body"],{"height_cm":175,"age_years":31,"sex":1})
        self.assertEqual((record["pedometer_steps"],record["energy_net_kcal_est"],record["series_bin_seconds"]),(321,3.5,60))
        self.assertEqual(record["energy_basis"],["heart_rate","assumed_mass"])
        self.assertEqual(record["series"],[{"movement_pct":70,"bpm":120},{"movement_pct":None,"bpm":None},{"movement_pct":100,"bpm":150}])
        self.assertEqual(exporter.csv_text([record],exporter.SUMMARY_COLUMNS).splitlines()[1].count("heart_rate;assumed_mass"),1)
        # Net energy above gross, an unknown basis flag or an out-of-range bin must be rejected.
        for offset,data in ((336,struct.pack("<f",9.0)),(340,bytes((32,))),(344,bytes((101,)))):
            bad=bytearray(value);bad[at+offset:at+offset+len(data)]=data
            struct.pack_into("<I",bad,exporter.V4_CHECKSUM_OFFSET,exporter.fnv1a(bad[:exporter.V4_CHECKSUM_OFFSET]))
            path.write_bytes(bad)
            with self.assertRaises(exporter.InvalidBank):exporter.read_bank(path)

    def test_v2_frozen_placement_and_motion_extension(self):
        value = bytearray(3472)
        struct.pack_into("<6IQ", value, 0, exporter.MAGIC, 2, 3472, 1, 1, 2, 0)
        item = snapshot()
        item[62] = 8  # badminton
        value[296:532] = item
        struct.pack_into("<8B", value, 296 + 236, 1, 2, 2, 1, 1, 70, 1, 55)
        struct.pack_into("<3IfI", value, 296 + 244, 100000, 60000, 15, 0, 2)
        struct.pack_into("<I", value, 3464, exporter.fnv1a(value[:3464]))
        path = self.directory / "v2.bin"
        path.write_bytes(value)
        result = exporter.export(exporter.read_bank(path))["workouts"][0]
        self.assertEqual(result["sport"], "Badminton")
        self.assertEqual(result["category_met"], 5.5)
        self.assertEqual(result["mass_kg"], 70)
        self.assertEqual(result["worn_wrist"], 1)
        self.assertEqual(result["playing_hand"], 2)
        self.assertEqual(result["movement_seconds_est"], 60)
        self.assertEqual(result["movement_bursts_est"], 15)
        self.assertEqual(result["recovery_count"], 2)
        # A forged checksum must not allow impossible movement coverage.
        struct.pack_into("<I", value, 296 + 248, 100001)
        struct.pack_into("<I", value, 3464, exporter.fnv1a(value[:3464]))
        path.write_bytes(value)
        with self.assertRaises(exporter.InvalidBank):
            exporter.read_bank(path)

    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="workout-export-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def write(self, slot, value):
        path = self.directory / f"workouts.{slot}.bin"
        path.write_bytes(value)
        return path

    def test_real_summary_and_cumulative_split_conversion(self):
        self.write(0, bank())
        loaded, _ = exporter.load(self.directory)
        result = exporter.export(loaded)
        record = result["workouts"][0]
        self.assertEqual((record["sport"], record["active_seconds"], record["observed_seconds"]),
                         ("Run", 120, 125))
        self.assertEqual(record["distance_m"], 240)
        self.assertEqual(record["gps_valid_seconds"], 110)
        self.assertAlmostEqual(record["gps_average_kph"], 240 / 110 * 3.6, places=5)
        self.assertEqual(record["average_bpm"], 140)
        self.assertEqual([lap["split_seconds"] for lap in record["laps"]], [60, 60])
        self.assertEqual([lap["split_distance_m"] for lap in record["laps"]], [120, 120])

    def test_sequence_rollover_selects_newest_bank(self):
        self.write(0, bank(0xFFFFFFFF))
        self.write(1, bank(0))
        loaded, warnings = exporter.load(self.directory)
        self.assertEqual(loaded["sequence"], 0)
        self.assertEqual(warnings, [])

    def test_invalid_newer_bank_falls_back_without_mutating_source(self):
        first = self.write(0, bank(2))
        corrupt = bank(3)
        corrupt[300] ^= 0x40
        second = self.write(1, corrupt)
        before = [first.read_bytes(), second.read_bytes()]
        loaded, warnings = exporter.load(self.directory)
        self.assertEqual(loaded["sequence"], 2)
        self.assertTrue(any("checksum" in warning for warning in warnings))
        self.assertEqual(before, [first.read_bytes(), second.read_bytes()])

    def test_forged_checksum_does_not_bypass_semantic_validation(self):
        for offset, fmt, bad in ((268 + 32, "<f", float("nan")),
                                 (268 + 66, "<B", 2),
                                 (268 + 72, "<B", 21),
                                 (268 + 76, "<I", 130000)):
            with self.subTest(offset=offset):
                value = bank()
                struct.pack_into(fmt, value, offset, bad)
                path = self.write(0, reseal(value))
                with self.assertRaises(exporter.InvalidBank):
                    exporter.read_bank(path)

    def test_duplicates_truncation_extra_bytes_and_unknown_version_refused(self):
        invalid = [bank(history=[snapshot(), snapshot()]), bank()[:-1], bank() + b"x"]
        version = bank()
        struct.pack_into("<I", version, 4, 2)
        invalid.append(reseal(version))
        for index, value in enumerate(invalid):
            with self.subTest(case=index):
                with self.assertRaises(exporter.InvalidBank):
                    exporter.read_bank(self.write(0, value))

    def test_unfinished_checkpoint_is_explicit_and_opt_in(self):
        self.write(0, bank(history=[], current=snapshot(state=1, saved=0)))
        loaded, _ = exporter.load(self.directory)
        self.assertEqual(exporter.export(loaded)["workouts"], [])
        result = exporter.export(loaded, include_checkpoint=True)["workouts"][0]
        self.assertEqual(result["state"], "running")
        self.assertFalse(result["saved"])

    def test_missing_metrics_export_as_null(self):
        item = snapshot()
        struct.pack_into("<I", item, 24, 0)
        struct.pack_into("<I", item, 28, 0)
        item[62] = 5  # general indoor
        item[66:71] = b"\0" * 5
        self.write(0, bank(history=[item]))
        loaded, _ = exporter.load(self.directory)
        record = exporter.export(loaded)["workouts"][0]
        for field in ("distance_m", "gps_average_kph", "steps", "average_bpm"):
            self.assertIsNone(record[field])

    def test_cli_json_csv_laps_and_overwrite_refusal(self):
        source = self.write(0, bank())
        before = source.read_bytes()
        script = ROOT / "tools/workouts_export.py"
        for suffix in ("json", "csv"):
            output = self.directory / f"export.{suffix}"
            laps = self.directory / f"laps-{suffix}.csv"
            result = subprocess.run([sys.executable, str(script), str(self.directory),
                                     "--out", str(output), "--laps-out", str(laps)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            if suffix == "json":
                self.assertEqual(json.loads(output.read_text())["workouts"][0]["sport"], "Run")
            else:
                self.assertIn("gps_average_kph", output.read_text())
            self.assertEqual(len(laps.read_text().splitlines()), 3)
            result = subprocess.run([sys.executable, str(script), str(self.directory),
                                     "--out", str(output)], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("refusing to overwrite", result.stderr)
        self.assertEqual(source.read_bytes(), before)

    @unittest.skipUnless(shutil.which("gcc"), "host C compiler required")
    def test_cross_language_reader_of_actual_c_checkpoints(self):
        executable = self.directory / "workout_test.exe"
        compilation = subprocess.run(
            ["gcc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Icore", "-Inos", str(ROOT / "tests/workout_test.c"), "-lm", "-o", str(executable)],
            cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(compilation.returncode, 0, compilation.stderr)
        store = self.directory / "real-store"
        for mode in ("--write-recovery", "--read-recovery"):
            process = subprocess.run([str(executable), mode, str(store)],
                                     cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            loaded, _ = exporter.load(store)
            if mode == "--write-recovery":
                self.assertEqual(loaded["current"]["state"], 1)
                self.assertEqual(loaded["current"]["elapsed_ms"], 2000)
            else:
                record = exporter.export(loaded)["workouts"][0]
                self.assertEqual((record["active_seconds"], record["steps"], record["lap_count"]),
                                 (3, 6, 2))


if __name__ == "__main__":
    unittest.main()
