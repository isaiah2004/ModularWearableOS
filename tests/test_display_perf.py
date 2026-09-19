import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest


spec = importlib.util.spec_from_file_location(
    "display_perf", Path(__file__).resolve().parents[1] / "tools/display_perf.py")
display_perf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(display_perf)


class DisplayPerfParserTests(unittest.TestCase):
    def test_parses_each_perf_json_record(self):
        for kind in sorted(display_perf.PERF_KINDS):
            self.assertEqual(display_perf.parse_perf_line(f'{kind} {{"fps":59,"ok":true}}'),
                             (kind, {"fps": 59, "ok": True}))
            self.assertEqual(display_perf.parse_perf_line(f'{kind},{{"fps":59,"ok":true}}'),
                             (kind, {"fps": 59, "ok": True}))

    def test_decodes_configuration_and_probe_records(self):
        self.assertEqual(display_perf.parse_perf_line('PERF_CONFIG,{"width":466}'),
                         ("PERF_CONFIG", {"width": 466}))
        self.assertEqual(display_perf.parse_perf_line('PANEL_PROBE,{"stable":false}'),
                         ("PANEL_PROBE", {"stable": False}))
    def test_ignores_unrelated_raw_lines(self):
        self.assertIsNone(display_perf.parse_perf_line("I (123) display: ready"))
        self.assertIsNone(display_perf.parse_perf_line("DONE"))

    def test_rejects_bad_or_non_object_perf_json(self):
        with self.assertRaisesRegex(ValueError, "Malformed PERF_RUN"):
            display_perf.parse_perf_line("PERF_RUN {bad}")
        with self.assertRaisesRegex(ValueError, "JSON object"):
            display_perf.parse_perf_line("PERF_APP [1,2]")

    def test_expected_record_is_command_specific(self):
        self.assertEqual(display_perf.expected_record("perf display info"), "PERF_INFO")
        self.assertEqual(display_perf.expected_record("PERF DISPLAY fill 500 generator"), "PERF_RUN")
        self.assertEqual(display_perf.expected_record("perf display rect 20 30 500 psram"), "PERF_RUN")
        self.assertEqual(display_perf.expected_record("perf display app breathe 60"), "PERF_APP")
        self.assertIsNone(display_perf.expected_record("perf display probe"))
        self.assertIsNone(display_perf.expected_record("perf display export"))
        self.assertIsNone(display_perf.expected_record("face 0"))
        self.assertIsNone(display_perf.expected_record("perf unknown"))

    def test_reads_script_without_comments_or_blank_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "workload.txt"
            path.write_text("# display workload\n\n perf display info \nperf display fill 500 generator\n", encoding="utf-8")
            self.assertEqual(display_perf.read_script(path),
                             ["perf display info", "perf display fill 500 generator"])

    def test_identity_requires_matching_port_and_serial(self):
        class Port:
            def __init__(self, device, serial_number):
                self.device, self.serial_number = device, serial_number
        display_perf.verify_identity("com3", display_perf.WATCH_SERIAL,
                                     [Port("COM3", display_perf.WATCH_SERIAL.upper())])
        with self.assertRaisesRegex(RuntimeError, "does not match"):
            display_perf.verify_identity("COM3", display_perf.WATCH_SERIAL, [Port("COM3", "other")])
        with self.assertRaisesRegex(RuntimeError, "was not found"):
            display_perf.verify_identity("COM4", display_perf.WATCH_SERIAL, [Port("COM3", display_perf.WATCH_SERIAL)])

    def test_capture_keeps_command_and_every_received_line(self):
        class Port:
            def __init__(self):
                self.lines = iter([b"I (1) unrelated log\r\n",
                                   b'PERF_INFO,{"frames":3}\r\n', b"DONE\r\n"])
                self.written = b""
            def write(self, data): self.written += data
            def flush(self): pass
            def readline(self): return next(self.lines, b"")

        port, output = Port(), io.StringIO()
        display_perf.Capture(port, output).command("perf display info", 1)
        rows = [json.loads(line) for line in output.getvalue().splitlines()]
        self.assertEqual([(row["direction"], row["line"]) for row in rows],
                         [("tx", "perf display info"), ("rx", "I (1) unrelated log"),
                          ("rx", 'PERF_INFO,{"frames":3}'), ("rx", "DONE")])
        self.assertEqual(rows[2]["record"], "PERF_INFO")
        self.assertEqual(rows[2]["perf"], {"frames": 3})
        self.assertEqual(port.written, b"perf display info\n")

    def test_command_rejects_device_error(self):
        class Port:
            def __init__(self): self.lines = iter([b"ERR bad workload\n", b"DONE\n"])
            def write(self, data): pass
            def flush(self): pass
            def readline(self): return next(self.lines, b"")
        with self.assertRaisesRegex(RuntimeError, "ERR bad workload"):
            display_perf.Capture(Port(), io.StringIO()).command("something", 1)


if __name__ == "__main__":
    unittest.main()
