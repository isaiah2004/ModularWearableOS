"""Strict build and same-source verification regressions with isolated fake tools."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
import shutil
from unittest.mock import patch
from tools import build_sim

class BuildTests(unittest.TestCase):
    def workspace(self, directory):
        root = Path(directory)
        for name in ('core/fonts', 'core/apps', 'nos', 'sim', 'tools'): (root / name).mkdir(parents=True, exist_ok=True)
        (root / 'core/a.c').write_text('int a;')
        (root / 'sim/main_win32.c').write_text('int main(void){return 0;}')
        compiler = root / 'compiler.exe'; compiler.write_bytes(b'fake compiler identity')
        return root, compiler

    def runner(self, root, *, compile_fail=False, link_fail=False):
        calls = []
        def run(command, **kwargs):
            calls.append(command)
            if '--version' in command: return subprocess.CompletedProcess(command, 0, 'gcc test', '')
            if '-c' in command and compile_fail: raise RuntimeError('injected compile failure')
            if '-c' not in command and link_fail: raise RuntimeError('injected link failure')
            target = Path(command[command.index('-o') + 1]); target.write_bytes(b'object' if '-c' in command else b'executable')
            return subprocess.CompletedProcess(command, 0, '', '')
        return run, calls

    def test_failed_compile_does_not_link_cached_objects_or_replace_executable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, compiler = self.workspace(temporary)
            good, _ = self.runner(root)
            with patch.object(build_sim, 'checked_run', side_effect=good):
                target, _ = build_sim.build(root, compiler, 'test.exe', legacy=True, jobs=1)
            previous = target.read_bytes()
            (root / 'core/a.c').write_text('invalid changed source')
            bad, calls = self.runner(root, compile_fail=True)
            with patch.object(build_sim, 'checked_run', side_effect=bad), self.assertRaises(RuntimeError):
                build_sim.build(root, compiler, 'test.exe', legacy=True, jobs=1)
            self.assertEqual(target.read_bytes(), previous)
            self.assertFalse(any('-o' in c and '-c' not in c for c in calls))

    def test_failed_link_and_source_race_do_not_publish_success(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, compiler = self.workspace(temporary)
            bad, _ = self.runner(root, link_fail=True)
            with patch.object(build_sim, 'checked_run', side_effect=bad), self.assertRaises(RuntimeError):
                build_sim.build(root, compiler, 'test.exe', legacy=True, jobs=1)
            self.assertFalse((root / 'build/test.exe').exists())
            good, _ = self.runner(root)
            def race(command, **kwargs):
                result = good(command, **kwargs)
                if '-c' in command: (root / 'core/a.c').write_text('changed while compiling')
                return result
            with patch.object(build_sim, 'checked_run', side_effect=race), self.assertRaisesRegex(RuntimeError, 'Sources changed'):
                build_sim.build(root, compiler, 'test.exe', legacy=True, clean=True, jobs=1)
            self.assertFalse((root / 'build/test.exe').exists())

    def test_named_outputs_flags_and_preserved_mtime_have_separate_fingerprints(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, compiler = self.workspace(temporary)
            run, calls = self.runner(root)
            with patch.object(build_sim, 'checked_run', side_effect=run):
                build_sim.build(root, compiler, 'first.exe', legacy=True, jobs=1)
                stamp = (root / 'core/a.c').stat().st_mtime_ns
                (root / 'core/a.c').write_text('int b;')
                os.utime(root / 'core/a.c', ns=(stamp, stamp))
                start = len(calls)
                build_sim.build(root, compiler, 'first.exe', legacy=True, jobs=1)
                self.assertTrue(any('-c' in c for c in calls[start:]))
                build_sim.build(root, compiler, 'second.exe', legacy=True, debug=True, jobs=1)
            first = json.loads((root / 'build/first.exe.build.json').read_text())
            second = json.loads((root / 'build/second.exe.build.json').read_text())
            self.assertNotEqual(first['flags'], second['flags'])
            self.assertEqual(first['sources'], second['sources'])
            self.assertEqual(len(list((root / 'build/sim-objects').iterdir())), 2)

    def test_verification_gates_compile_run_size_and_accuracy_failures(self):
        for failure in ('compile', 'run', 'size', 'accuracy'):
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as temporary:
                root, compiler = self.workspace(temporary)
                for name in ('gfx', 'font', 'sprite'): (root / 'core' / (name + '.c')).write_text('// current recovered source')
                (root / 'tools/gfx_cost_exact.c').write_text('// current verifier')
                directory = root / 'verify'; directory.mkdir()
                def run(command, **kwargs):
                    if '-o' in command:
                        if failure == 'compile': raise RuntimeError('compile')
                        Path(command[command.index('-o') + 1]).write_bytes(b'exe')
                    elif '--accuracy' in command:
                        if failure == 'accuracy': raise RuntimeError('accuracy')
                    else:
                        if failure == 'run': raise RuntimeError('run')
                        output = Path(command[1])
                        with output.open('wb') as stream: stream.truncate(1 if failure == 'size' else 400 * 466 * 466 * 2)
                    return subprocess.CompletedProcess(command, 0, '', '')
                with patch.object(build_sim, 'checked_run', side_effect=run), self.assertRaises(RuntimeError):
                    build_sim.verify_cost(root, compiler, {}, directory)
                self.assertFalse((directory / 'verify.json').exists())
                snapshot = (directory / 'snapshot/core/gfx.c').read_text()
                self.assertIn('current recovered source', snapshot)

    @unittest.skipUnless(os.name == 'nt' and shutil.which('pwsh'), 'PowerShell wrapper regression requires Windows')
    def test_powershell_entrypoints_propagate_failed_native_exit(self):
        with tempfile.TemporaryDirectory() as temporary:
            failure = Path(temporary) / 'failed-python.cmd'
            failure.write_text('@exit /b 7\n')
            for script, extra in (('sim/nos_build.ps1', ['-Run']), ('sim/build.ps1', []),
                                  ('sim/run.ps1', []), ('tools/package.ps1', []),
                                  ('tools/flash-package.ps1', ['-Port', 'UNUSED'])):
                with self.subTest(script=script):
                    result = subprocess.run([shutil.which('pwsh'), '-NoProfile', '-File',
                                             str(build_sim.ROOT / script), '-Python', str(failure), *extra],
                                            capture_output=True, text=True)
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == '__main__': unittest.main()
