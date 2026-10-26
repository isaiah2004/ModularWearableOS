"""Private, native execution of actual HAL/OTA sources against faulted SDK calls."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/alpha05/checks/hal"
SDK_HEADERS = """esp_err.h esp_timer.h esp_log.h esp_wifi.h esp_wifi_default.h esp_event.h esp_netif.h
esp_http_server.h esp_sntp.h esp_heap_caps.h nvs_flash.h nvs.h esp_random.h
esp_ota_ops.h esp_https_ota.h esp_crt_bundle.h esp_system.h esp_memory_utils.h cJSON.h sdkconfig.h
esp_adc/adc_oneshot.h esp_adc/adc_cali.h esp_adc/adc_cali_scheme.h
freertos/FreeRTOS.h freertos/semphr.h freertos/task.h freertos/queue.h
driver/usb_serial_jtag.h driver/gpio.h driver/i2c_master.h nimble/nimble_port.h
nimble/nimble_port_freertos.h host/ble_hs.h host/ble_gap.h host/ble_hs_adv.h
host/ble_gatt.h host/ble_uuid.h services/gap/ble_svc_gap.h services/gatt/ble_svc_gatt.h
host/util/util.h""".split()


@unittest.skipUnless(shutil.which("gcc"), "GCC required")
class HalWave2(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        (OUT / "tmp").mkdir(parents=True, exist_ok=True)
        for name in SDK_HEADERS:
            path = OUT / "mock" / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('#include "hal_wave2_mock.h"\n', encoding="utf-8")
        cls.env = dict(os.environ, TMP=str(OUT / "tmp"), TEMP=str(OUT / "tmp"),
                       TMPDIR=str(OUT / "tmp"), PYTHONDONTWRITEBYTECODE="1")

    def compile_run(self, name):
        sources = [f"tests/hal_wave2_{name}_test.c"]
        if name == "storage":
            sources.append("nos/prefs.c")
        command = ["gcc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-Wno-format-truncation", "-Wno-unused-function",
                   "-Wno-unused-variable", "-ffunction-sections", "-fdata-sections", "-flto",
                   "-pthread", "-Itests", "-Ibuild/alpha05/checks/hal/mock", "-Icore", "-Inos",
                   *sources, "-Wl,--gc-sections", "-lm", "-o", str(OUT / f"{name}.exe")]
        result = subprocess.run(command, cwd=ROOT, env=self.env, text=True, capture_output=True, timeout=60)
        log = "$ " + subprocess.list2cmdline(command) + "\n" + result.stdout + result.stderr
        record = {"command": command, "compile_exit": result.returncode}
        if result.returncode == 0:
            cases = [None]
            if name == "storage":
                cases += ["offline-create", "offline-unknown", "offline-return",
                          "offline-tombstone", "offline-after"]
            if name == "radio_startup":
                cases = ["off", "guard", "gates", "later", "start-again", "weather", "ap-only", "ap", "ble-cleanup"]
                cases += ["wifi-" + stage for stage in ("netif", "loop", "sta-new", "attach", "sta-handler",
                          "queue", "task", "driver", "wifi-event", "ip-event", "storage", "country", "mode", "start")]
                cases += ["ble-" + stage for stage in ("host-task", "nimble", "gatt-count", "gatt-add", "name", "callout")]
            executions = []
            for case in cases:
                invocation = [str(OUT / f"{name}.exe")] + ([case] if case else [])
                run = subprocess.run(invocation, cwd=OUT, env=self.env,
                                     text=True, capture_output=True, timeout=30)
                log += "$ " + subprocess.list2cmdline(invocation) + "\n" + run.stdout + run.stderr
                executions.append({"case": case or "default", "exit": run.returncode,
                                   "output": run.stdout, "stderr": run.stderr})
                if run.returncode:
                    break
            record.update(run_exit=run.returncode, executions=executions,
                          output="".join(item["output"] for item in executions))
        (OUT / f"{name}.log").write_text(log, encoding="utf-8")
        (OUT / f"{name}.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        self.assertEqual(result.returncode, 0, log)
        self.assertEqual(run.returncode, 0, log)
        self.assertIn("PASS", run.stdout)

    def test_hal_actual_source(self):
        self.compile_run("radio_cover")

    def test_radio_startup_faults(self):
        self.compile_run("radio_startup")

    def test_ota_actual_source(self):
        self.compile_run("ota")

    def test_storage_actual_source(self):
        self.compile_run("storage")


if __name__ == "__main__":
    unittest.main()
