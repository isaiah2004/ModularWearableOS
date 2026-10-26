"""Independent Wave3 boundary checks; host C only, private output/card directories."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/alpha05/checks/critical-review"

@unittest.skipUnless(os.name == "nt" and shutil.which("gcc"), "Windows GCC required")
class CriticalStorageTests(unittest.TestCase):
    def run_module(self, module, scenarios):
        OUT.mkdir(parents=True, exist_ok=True)
        temporary_root = Path("A:/.worktrees/smartwatch2-critical-review")
        temporary_root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix=module + "-", dir=temporary_root) as directory:
            directory = Path(directory)
            self.assertTrue(directory.resolve().is_relative_to(temporary_root.resolve()))
            binary = directory / (module + ".exe")
            command = ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-Wno-format-truncation", "-Icore", "-Inos",
                       str(ROOT / ("tests/critical_" + module + "_test.c")), "-lm", "-o", str(binary)]
            if module == "transfer":
                transfer = (ROOT / "firmware/main/sd_xfer.c").read_text()
                hardware = ["freertos/FreeRTOS.h", "driver/usb_serial_jtag.h", "esp_rom_crc.h",
                            "esp_timer.h", "mbedtls/base64.h", "board.h"]
                for header in hardware:
                    transfer, count = re.subn(r'^#include "' + re.escape(header) + r'"\s*$', "", transfer,
                                              flags=re.MULTILINE)
                    self.assertEqual(count, 1, header)
                fixture = (ROOT / "tests/storage_xfer_fault_test.c").read_text().replace("/* @SD_XFER_SOURCE@ */", transfer)
                harness = (ROOT / "tests/critical_transfer_test.c").read_text().replace("/* @TRANSFER_FIXTURE@ */", fixture)
                generated = directory / "transfer.c"
                generated.write_text(harness)
                command[command.index(str(ROOT / "tests/critical_transfer_test.c"))] = str(generated)
                command.extend(["-Ifirmware/main", str(ROOT / "nos/prefs.c"), str(ROOT / "nos/prefs_disk.c")])
            if module == "prefs":
                prefs = (ROOT / "nos/prefs.c").read_text()
                prefs = prefs.replace('__attribute__((weak)) int hal_store_load_pending(const char *key) { (void)key; return 0; }',
                                      'int critical_load_pending(const char *key);\n#define hal_store_load_pending critical_load_pending')
                harness = (ROOT / "tests/critical_prefs_test.c").read_text()
                harness = harness.replace('#include "../nos/prefs_disk.c"', (ROOT / "nos/prefs_disk.c").read_text())
                harness = harness.replace('#include "../nos/prefs.c"', prefs)
                generated = directory / "prefs.c"
                generated.write_text(harness)
                command[command.index(str(ROOT / "tests/critical_prefs_test.c"))] = str(generated)
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
            (OUT / (module + "-compile.log")).write_text(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            for scenario in scenarios:
                with self.subTest(scenario=scenario):
                    card = directory / scenario
                    card.mkdir()
                    result = subprocess.run([str(binary), scenario], cwd=card,
                                            capture_output=True, text=True, timeout=30)
                    (OUT / (module + "-" + scenario + ".log")).write_text(result.stdout + result.stderr)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_journal_durability_reserve_and_shutdown(self):
        self.run_module("sports", ["sync", "rotation", "reserve", "quiesce"])

    def test_imu_restore_identity_pending_ownership_and_tail_read(self):
        self.run_module("imu", ["bank", "pending", "previous", "previous-read", "previous-close", "restore", "stop-failure"])

    def test_preferences_schema_explicit_provenance_and_runtime_restore(self):
        self.run_module("prefs", ["unsupported", "wrist", "restore", "restore-edit", "repair-unknown",
                                  "overlay-retry", "overlay-unknown", "overlay-corrupt", "overlay-wrist"])

    def test_real_transfer_preference_owner_and_path_guards(self):
        self.run_module("transfer", ["aliases", "clean", "dirty", "late-edit", "ota",
                                     *[f"fixture-{n}" for n in [*range(1,12),14]]])

if __name__ == "__main__":
    unittest.main()
