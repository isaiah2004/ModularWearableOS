"""Fault regressions against production preferences, recorder, journal and transfers.

Run: python -m unittest discover -s tests -p test_storage_recovery.py -v
All temporary cards and binaries remain below build/alpha05/checks/storage.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CHECKS = ROOT / "build/alpha05/checks/storage"


@unittest.skipUnless(os.name == "nt" and shutil.which("gcc"), "Windows host GCC required")
class StorageRecoveryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        CHECKS.mkdir(parents=True, exist_ok=True)
        cls.directory = tempfile.TemporaryDirectory(prefix="regressions-", dir=CHECKS)
        cls.out = Path(cls.directory.name)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def compile(self, name, sources):
        binary = self.out / (name + ".exe")
        command = ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wno-format-truncation", "-I" + str(ROOT / "core"),
                   "-I" + str(ROOT / "nos"), "-I" + str(ROOT / "firmware/main"),
                   *map(str, sources), "-lm", "-o", str(binary)]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
        (CHECKS / (name + "-compile.log")).write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return binary

    def run_case(self, binary, name, *args, directory=None):
        directory = directory or self.out / name
        directory.mkdir(exist_ok=True)
        result = subprocess.run([str(binary), *map(str, args)], cwd=directory,
                                capture_output=True, text=True, timeout=30)
        (CHECKS / (name + ".log")).write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_preferences_read_write_and_schema_faults(self):
        binary = self.compile("prefs-disk-faults", [ROOT / "tests/prefs_disk_fault_test.c"])
        self.run_case(binary, "prefs-disk-faults")

    def test_imu_unknown_intent_worker_retry(self):
        binary = self.compile("imu-intent-faults", [ROOT / "tests/imu_intent_fault_test.c"])
        for scenario in range(1, 7):
            with self.subTest(scenario=scenario):
                self.run_case(binary, f"imu-intent-{scenario}", scenario)

    def test_journal_open_torn_tail_and_failed_close_replay(self):
        binary = self.compile("sports-journal-faults", [ROOT / "tests/sports_test.c"])
        self.run_case(binary, "sports-journal-faults")

    def test_transfer_publication_recovery_and_typed_reads(self):
        source = (ROOT / "firmware/main/sd_xfer.c").read_text()
        hardware = ["freertos/FreeRTOS.h", "driver/usb_serial_jtag.h", "esp_rom_crc.h",
                    "esp_timer.h", "mbedtls/base64.h", "board.h"]
        for header in hardware:
            source, count = re.subn(r'^#include "' + re.escape(header) + r'"\s*$', "", source,
                                    flags=re.MULTILINE)
            self.assertEqual(count, 1, header)
        fixture = (ROOT / "tests/storage_xfer_fault_test.c").read_text()
        generated = self.out / "storage_xfer_fault_test.c"
        generated.write_text(fixture.replace("/* @SD_XFER_SOURCE@ */", source))
        binary = self.compile("sd-xfer-faults", [generated, ROOT / "nos/prefs_disk.c"])
        for scenario in range(1, 12):
            with self.subTest(scenario=scenario):
                self.run_case(binary, f"sd-xfer-{scenario}", scenario)
        restart = self.out / "sd-xfer-restart"
        self.run_case(binary, "sd-xfer-restart-prepare", 12, directory=restart)
        self.run_case(binary, "sd-xfer-restart-recover", 13, directory=restart)
        self.run_case(binary, "sd-xfer-successful-paced-read", 14)


if __name__ == "__main__":
    unittest.main()
