"""Exercise the read-only stub protocol without requiring connected hardware."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
import types
import unittest
from unittest.mock import patch

MODULES = {name: types.ModuleType(name) for name in ("esptool", "esptool.targets", "esptool.targets.esp32s3")}
MODULES["esptool.targets.esp32s3"].ESP32S3ROM = object
SPEC = importlib.util.spec_from_file_location("backup_flash_test_module", Path(__file__).parents[1] / "tools/backup_flash.py")
BACKUP = importlib.util.module_from_spec(SPEC)
with patch.dict(sys.modules, MODULES):
    SPEC.loader.exec_module(BACKUP)


class FakeStub:
    ESP_READ_FLASH = 0xD2

    def __init__(self, packets):
        self.packets = iter(packets)
        self.acks = []
        self.command = None

    def check_command(self, label, opcode, data):
        self.command = (label, opcode, struct.unpack("<IIII", data))

    def read(self):
        return next(self.packets)

    def write(self, data):
        self.acks.append(struct.unpack("<I", data)[0])


class BackupFlashTest(unittest.TestCase):
    def test_exact_packets_and_device_digest(self):
        data = bytes(range(256)) * 2
        stub = FakeStub([data[:256], data[256:], hashlib.md5(data).digest()])
        self.assertEqual(BACKUP.read_flash_bounded(stub, 0x20000, 512, 1, 256), data)
        self.assertEqual(stub.command[2], (0x20000, 512, 256, 1))
        self.assertEqual(stub.acks, [256, 512])

    def test_short_packet_rejected(self):
        stub = FakeStub([b"x" * 255])
        with self.assertRaisesRegex(RuntimeError, "Short flash packet"):
            BACKUP.read_flash_bounded(stub, 0, 256, 1, 256)
        self.assertEqual(stub.acks, [])

    def test_wrong_device_digest_rejected(self):
        stub = FakeStub([b"x" * 256, b"0" * 16])
        with self.assertRaisesRegex(RuntimeError, "MD5 mismatch"):
            BACKUP.read_flash_bounded(stub, 0, 256, 1, 256)


if __name__ == "__main__":
    unittest.main()
