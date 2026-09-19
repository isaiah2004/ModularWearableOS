import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("watchctl", Path(__file__).resolve().parents[1] / "tools/watchctl.py")
watchctl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(watchctl)


class FakePort:
    def __init__(self, lines):
        self.lines = iter(lines)
        self.written = b""

    def write(self, data):
        self.written += data

    def flush(self):
        pass

    def readline(self):
        return next(self.lines, b"")


class WatchControlTests(unittest.TestCase):
    def test_collects_response_amid_boot_logging(self):
        port = FakePort([b"I (123) main: running\r\n", b"BOARD sd=0\r\n", b"DONE\r\n"])
        self.assertEqual(watchctl.execute(port, "board"), ["I (123) main: running", "BOARD sd=0"])
        self.assertEqual(port.written, b"board\n")

    def test_timeout_does_not_claim_success(self):
        with self.assertRaises(TimeoutError):
            watchctl.execute(FakePort([]), "board", timeout=0.001)

    def test_rejects_overflow_and_embedded_commands(self):
        for command in ["", "x" * 192, "home\nopen 3", "home\rsleep"]:
            port = FakePort([])
            with self.assertRaises(ValueError):
                watchctl.execute(port, command)
            self.assertEqual(port.written, b"")


if __name__ == "__main__":
    unittest.main()
