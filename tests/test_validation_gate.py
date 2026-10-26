"""Aggregate reports failures/skips and refuses a stale or degraded simulator."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import patch
from tools import validate
from tools.host_support import sha256
from tools import host_support

class ValidationGateTests(unittest.TestCase):
    def test_nonzero_exit_is_a_machine_readable_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            result=subprocess.CompletedProcess(['fake'],7,'before failure','compiler failed')
            with patch.object(validate.subprocess,'run',return_value=result):
                report=validate.run_step('gate',['fake'],Path(temporary),{})
            self.assertEqual(report['status'],'failed');self.assertEqual(report['exit_code'],7)
            self.assertIn('compiler failed',Path(report['log']).read_text())

    def test_stale_degraded_binary_or_missing_new_source_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'nos').mkdir();(root/'nos/app.c').write_text('current source')
            executable=root/'sim.exe';executable.write_bytes(b'current executable')
            manifest={'status':'passed','strict':True,'runtime':'NOS','source_sha256':'tree',
                'sources':{'nos/app.c':sha256(root/'nos/app.c')},'executable_sha256':sha256(executable)}
            path=root/'sim.exe.build.json';path.write_text(json.dumps(manifest))
            self.assertEqual(validate.simulator_identity(root,executable)['source_sha256'],'tree')
            (root/'nos/new.c').write_text('new recovered source')
            with self.assertRaisesRegex(ValueError,'newly added'):validate.simulator_identity(root,executable)
            (root/'nos/new.c').unlink();(root/'nos/app.c').write_text('changed source')
            with self.assertRaisesRegex(ValueError,'stale source'):validate.simulator_identity(root,executable)
            manifest['strict']=False;path.write_text(json.dumps(manifest))
            with self.assertRaisesRegex(ValueError,'strict current'):validate.simulator_identity(root,executable)

    def test_local_temp_policy_preserves_existing_directories_and_rejects_escape(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary); private = root / '.worktrees/_audit_tmp';private.mkdir(parents=True)
            evidence = private / 'preexisting.log';evidence.write_text('preserve')
            environment = host_support.local_environment(root)
            self.assertEqual(Path(environment['TEMP']),private)
            self.assertEqual(environment['TEMP'],environment['TMP'])
            self.assertEqual(evidence.read_text(),'preserve')
            with self.assertRaises(ValueError):host_support.contained(root,root / '../escape')
            with patch.object(host_support.shutil,'disk_usage',return_value=type('Disk',(),{'free':0})()), self.assertRaises(OSError):
                host_support.local_environment(root,minimum_free=1)

    def test_missing_plot_dependencies_fail_before_source_hash_compiler_and_tests(self):
        for profile in ('host','offline'):
            for missing in ({'matplotlib'},{'PIL'},{'matplotlib','PIL'}):
                with self.subTest(profile=profile,missing=missing),tempfile.TemporaryDirectory() as temporary:
                    root=Path(temporary);(root/'tools').mkdir()
                    (root/'tools/validation_matrix.json').write_text('{"host_tests": []}')
                    args=SimpleNamespace(profile=profile,out=None,run_hardware=False,port=None,
                        expected_elf_sha256=None,sim=None,cjson_dir=None)
                    def available(module):return None if module in missing else object()
                    with patch.object(validate.importlib.util,'find_spec',side_effect=available), \
                         patch.object(validate,'source_manifest',side_effect=AssertionError('must preflight before hashing')), \
                         patch.object(validate.shutil,'which',side_effect=AssertionError('must not reach compiler')), \
                         patch.object(validate,'run_step',side_effect=AssertionError('must not run tests')), \
                         contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                        self.assertEqual(validate.validate(args,root),1)
                    report=json.loads(next((root/'build/validation').glob('*/report.json')).read_text())
                    self.assertEqual(report['status'],'failed');self.assertEqual(report['steps'],[])
                    self.assertEqual(report['python_executable'],sys.executable)
                    self.assertIn('python -m pip install -r',report['error'])
                    self.assertIn(str(root/'tools/requirements-host.txt'),report['error'])
                    for module in missing:self.assertIn('Pillow' if module=='PIL' else module,report['error'])

    def test_host_and_offline_require_only_actual_plot_dependencies(self):
        for profile in ('host','offline'):
            with self.subTest(profile=profile),patch.object(validate.importlib.util,'find_spec',return_value=object()) as lookup:
                result=validate.python_dependency_preflight(profile)
            self.assertEqual([item['module'] for item in result['modules']],['matplotlib','PIL'])
            self.assertTrue(all(item['available'] for item in result['modules']))
            self.assertEqual([call.args[0] for call in lookup.call_args_list],['matplotlib','PIL'])

    def test_hardware_plan_uses_no_optional_runtime_modules(self):
        with patch.object(validate.importlib.util,'find_spec',side_effect=AssertionError('dry-run must need no serial/SDK')):
            result=validate.python_dependency_preflight('hardware',run_hardware=False)
        self.assertEqual(result['modules'],[])

    def test_hardware_execution_requires_sdk_serial_and_esptool_only(self):
        with patch.object(validate.importlib.util,'find_spec',return_value=object()) as lookup:
            result=validate.python_dependency_preflight('hardware',run_hardware=True)
        self.assertEqual([item['module'] for item in result['modules']],['serial','esptool'])
        self.assertEqual([call.args[0] for call in lookup.call_args_list],['serial','esptool'])
        for missing in ('serial','esptool'):
            with self.subTest(missing=missing),patch.object(validate.importlib.util,'find_spec',side_effect=lambda name:None if name==missing else object()):
                with self.assertRaisesRegex(ValueError,'matching ESP-IDF Python environment'):
                    validate.python_dependency_preflight('hardware',run_hardware=True)

    def test_missing_hardware_runtime_fails_before_any_scenario(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);(root/'tools').mkdir();(root/'tools/validation_matrix.json').write_text('{}')
            args=SimpleNamespace(profile='hardware',out=None,run_hardware=True,port='OFFLINE-NO-DEVICE',
                expected_elf_sha256='1'*64,sim=None,cjson_dir=None)
            with patch.object(validate.importlib.util,'find_spec',return_value=None), \
                 patch.object(validate,'source_manifest',side_effect=AssertionError('must stop first')), \
                 patch.object(validate,'run_step',side_effect=AssertionError('must never start scenario')), \
                 contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(validate.validate(args,root),1)
            report=json.loads(next((root/'build/validation').glob('*/report.json')).read_text())
            self.assertEqual(report['steps'],[]);self.assertIn('pyserial, esptool',report['error'])

    def test_broken_module_spec_is_an_actionable_dependency_failure(self):
        for error in (ImportError('broken search'),ValueError('module spec absent')):
            with self.subTest(error=error),patch.object(validate.importlib.util,'find_spec',side_effect=error):
                with self.assertRaisesRegex(ValueError,'python -m pip install -r'):
                    validate.python_dependency_preflight('host')

    def test_local_tests_package_wins_over_installed_tests_package(self):
        with tempfile.TemporaryDirectory() as temporary:
            root=Path(temporary);local=root/'workspace';foreign=root/'site-packages'
            (local/'tests').mkdir(parents=True);(foreign/'tests').mkdir(parents=True)
            # Copy the actual repository package marker, rather than inventing a test-only fix.
            marker=Path(validate.__file__).resolve().parents[1]/'tests/__init__.py'
            (local/'tests/__init__.py').write_bytes(marker.read_bytes())
            (local/'tests/helper.py').write_text('local_helper = True\n')
            (foreign/'tests/__init__.py').write_text('raise RuntimeError("wrong installed tests package")\n')
            (foreign/'tests/helper.py').write_text('raise RuntimeError("wrong installed helper")\n')
            code=('import pathlib,sys;sys.path[:0]=sys.argv[1:3];import tests;from tests.helper import local_helper;'
                  'assert local_helper;assert pathlib.Path(tests.__file__).resolve()==pathlib.Path(sys.argv[1],"tests/__init__.py").resolve()')
            env=os.environ | {key:str(root) for key in ('TEMP','TMP','TMPDIR')}
            result=subprocess.run([sys.executable,'-I','-c',code,str(local),str(foreign)],
                                  cwd=root,env=env,capture_output=True,text=True,timeout=10)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__':unittest.main()
