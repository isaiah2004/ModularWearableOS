"""Actual firmware records stay whole when SDK logs run between stdio calls."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/alpha05/checks/usb-tx-serialization'


def function(source, marker):
    start = source.index(marker)
    brace = source.index('{', start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def fixture(source):
    start = source.index('        const esp_app_desc_t *app = esp_app_get_description();')
    end = source.index('        nos_power_report();', start)
    stack_start = source.index('        printf("NOSCMDSTACK ')
    stack_end = source.index('        printf("NOSTOUCH ', stack_start)
    text = (ROOT / 'tests/host_atomic_records_test.c').read_text()
    for marker, value in (
        ('/* PRODUCTION CMD PRINTF */', function(source, 'static int cmd_printf(')),
        ('/* PRODUCTION NOSBUILD */', source[start:end]),
        ('/* PRODUCTION CRASH */', function(source, 'static int host_crash_command(')),
        ('/* PRODUCTION COMMAND STACK */', source[stack_start:stack_end]),
    ):
        text = text.replace(marker, value)
    return text


@unittest.skipUnless(shutil.which('gcc'), 'Host GCC required')
class HostAtomicRecordsTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.temporary = tempfile.TemporaryDirectory(prefix='actual-', dir=OUT)
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.path = Path(cls.temporary.name)
        cls.environment = os.environ | {key: str(cls.path) for key in ('TEMP', 'TMP', 'TMPDIR')}
        cls.source = ROOT / 'firmware/main/nos_main.c'
        cls.before = hashlib.sha256(cls.source.read_bytes()).hexdigest()
        generated = cls.path / 'records.c'
        generated.write_text(fixture(cls.source.read_text()))
        cls.executable = cls.path / ('records.exe' if os.name == 'nt' else 'records')
        compiled = subprocess.run(['gcc', '-O2', '-std=c11', '-Wall', '-Wextra', '-Werror',
                                   str(generated), '-o', str(cls.executable)],
                                  cwd=cls.path, env=cls.environment, capture_output=True,
                                  text=True, timeout=30)
        (OUT / 'actual-compile.log').write_text(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise AssertionError(compiled.stdout + compiled.stderr)

    def run_case(self, name):
        result = subprocess.run([str(self.executable), name], cwd=self.path,
                                env=self.environment, capture_output=True, text=True, timeout=10)
        (OUT / ('actual-' + name + '.log')).write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('1 complete stdio call', result.stdout)
        self.assertEqual(hashlib.sha256(self.source.read_bytes()).hexdigest(), self.before)

    def test_full_build_identity_is_one_complete_record(self):
        self.run_case('build')

    def test_bounded_crash_data_is_one_complete_record(self):
        self.run_case('crash')

    def test_command_stack_samples_reserved_task_in_byte_units(self):
        self.run_case('command-stack')


if __name__ == '__main__':
    unittest.main()
