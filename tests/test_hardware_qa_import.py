"""Import and dry-run of hardware QA must not access a device/network/filesystem."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from unittest.mock import MagicMock

ROOT = Path(__file__).resolve().parents[1]

class ImportSafetyTests(unittest.TestCase):
    def test_import_and_dry_run_do_not_import_hardware_dependencies_or_create_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / 'must-not-exist'
            spec = importlib.util.spec_from_file_location('tools.qa_import_probe', ROOT / 'tools/check_connectivity_device.py')
            module = importlib.util.module_from_spec(spec)
            with patch.dict(sys.modules, {'serial': None, 'esptool.reset': None}), \
                 patch.object(subprocess, 'run', side_effect=AssertionError('network/process I/O')), \
                 contextlib.redirect_stdout(io.StringIO()):
                spec.loader.exec_module(module)
                self.assertEqual(module.main(['--port', 'UNUSED', '--expected-elf-sha256', 'a' * 64,
                                              '--dry-run', '--out', str(output)]), 0)
            self.assertFalse(output.exists())

    def test_requires_explicit_port_identity_and_mode(self):
        from tools import check_connectivity_device as qa
        for arguments in ([], ['--port', 'UNUSED'], ['--port', 'UNUSED', '--expected-elf-sha256', 'wrong', '--run'],
                          ['--port', 'UNUSED', '--expected-elf-sha256', 'a' * 64]):
            with self.subTest(arguments=arguments), contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                qa.main(arguments)

    def test_checkpoint_checksum_and_sequence_wrap(self):
        from tools import check_connectivity_device as qa
        data = bytearray(276); struct.pack_into('<4I', data, 0, 0x4e4f5350, 1, 0xffffffff, 44)
        data[27] = 1
        checksum = 2166136261
        for value in data[:272]: checksum = ((checksum ^ value) * 16777619) & 0xffffffff
        struct.pack_into('<I', data, 272, checksum)
        old = qa.decode_checkpoint(data)
        self.assertEqual(old['dnd'], 1)
        data[27] ^= 1
        with self.assertRaises(ValueError): qa.decode_checkpoint(data)
        new = {'header': (0x4e4f5350, 1, 0, 44), 'dnd': 0}
        self.assertIs(qa.newest_checkpoint([old, new]), new)

    def test_wrong_target_never_mutates_network_or_resets_watch(self):
        from tools import check_connectivity_device as qa
        serial_module, reset_module = MagicMock(), MagicMock()
        with tempfile.TemporaryDirectory() as temporary, \
             patch.dict(sys.modules, {'serial': serial_module, 'esptool.reset': reset_module}), \
             patch.object(qa.HostTransport, 'negotiate'), \
             patch.object(qa.HostTransport, 'execute', return_value=['NOSBUILD elf_sha256=' + 'b' * 64]), \
             patch.object(qa.subprocess, 'run') as network, patch.object(qa.time, 'sleep'):
            with self.assertRaisesRegex(RuntimeError, 'identity mismatch'):
                qa.run('UNUSED', 'a' * 64, Path(temporary))
            network.assert_not_called()
            reset_module.HardReset.assert_not_called()
            serial_module.Serial.return_value.close.assert_called_once()

if __name__ == '__main__': unittest.main()
