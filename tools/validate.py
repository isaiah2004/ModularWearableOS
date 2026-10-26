"""One host validation entry point: portable host, full offline NOS, or hardware.

No profile builds firmware or flashes. Offline consumes an explicitly supplied
strict, current-source simulator and runs all test_*.py plus native/workload
groups. Hardware runs only with --run-hardware, a port and expected ELF identity.
"""
from __future__ import annotations
import argparse
import importlib.util
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
if __package__:
    from .host_support import ROOT, contained, local_environment, run_directory, sha256, write_json
    from .build_identity import source_features, source_manifest
else:
    from host_support import ROOT, contained, local_environment, run_directory, sha256, write_json
    from build_identity import source_features, source_manifest

def python_dependency_preflight(profile, *, run_hardware=False, root=ROOT):
    # Looking up top-level modules does not import plotting or open a device.
    required = (('matplotlib', 'matplotlib'), ('PIL', 'Pillow')) if profile in ('host', 'offline') else (
        (('serial', 'pyserial'), ('esptool', 'esptool')) if run_hardware else ())
    dependencies = []
    for module, package in required:
        try:
            available = importlib.util.find_spec(module) is not None
        except (ImportError, ValueError):
            available = False
        dependencies.append({'module': module, 'package': package, 'available': available})
    missing = [item['package'] for item in dependencies if not item['available']]
    if missing:
        context = f"Missing Python dependencies for {profile} in {sys.executable}: {', '.join(missing)}. "
        if profile == 'hardware':
            raise ValueError(context + 'Activate the matching ESP-IDF Python environment with pyserial and esptool; do not substitute a host esptool installation.')
        raise ValueError(context + 'Activate the intended host Python environment, then run: python -m pip install -r "' + str(Path(root) / 'tools/requirements-host.txt') + '"')
    return {'python_executable': sys.executable, 'modules': dependencies}


def simulator_identity(root, simulator):
    root = Path(root).resolve()
    simulator = Path(simulator).resolve()
    report = json.loads(simulator.with_suffix(simulator.suffix + '.build.json').read_text())
    if report.get('status') != 'passed' or report.get('strict') is not True or report.get('runtime') != 'NOS':
        raise ValueError('Offline validation requires a strict current NOS simulator manifest')
    if report['executable_sha256'] != sha256(simulator): raise ValueError('Simulator binary hash differs from its manifest')
    for path, expected in report['sources'].items():
        if sha256(contained(root, Path(root) / path)) != expected:
            raise ValueError('Simulator has stale source/header inputs: ' + path)
    # Additions cannot silently evade a manifest created before them, including
    # a newly included header or simulator C unit (not just nos/*.c).
    current = [*list((root / 'nos').rglob('*.c')), *list((root / 'sim').glob('nos_*.c')),
               *list((root / 'core/fonts').glob('*.c'))]
    if __package__:
        from .build_sim import CORE
    else:
        from build_sim import CORE
    current += [root / 'core' / (name + '.c') for name in CORE if (root / 'core' / (name + '.c')).is_file()]
    current += [p for name in ('core','nos','sim') for p in (root / name).rglob('*') if p.suffix in ('.h','.inc')]
    if set(report['sources']) != {p.relative_to(root).as_posix() for p in current}:
        raise ValueError('Simulator manifest omits newly added sources/headers or has removed inputs')
    return {'source_sha256': report['source_sha256'], 'executable_sha256': report['executable_sha256']}

def run_step(name, command, directory, environment, *, cwd=ROOT):
    start = time.monotonic()
    log = directory / (name + '.log')
    try:
        result = subprocess.run([str(x) for x in command], cwd=cwd, env=environment,
                                capture_output=True, text=True)
        log.write_text(result.stdout + result.stderr, encoding='utf-8')
        return {'name': name, 'status': 'passed' if result.returncode == 0 else 'failed',
                'command': [str(x) for x in command], 'exit_code': result.returncode,
                'seconds': round(time.monotonic() - start, 3), 'log': str(log)}
    except OSError as exc:
        log.write_text(str(exc), encoding='utf-8')
        return {'name': name, 'status': 'failed', 'command': [str(x) for x in command], 'error': str(exc), 'log': str(log)}

def validate(args, root=ROOT):
    root = Path(root).resolve()
    matrix = json.loads((root / 'tools/validation_matrix.json').read_text())
    environment = local_environment(root)
    directory = run_directory('validation-' + args.profile, root, parent=args.out or root / 'build/validation')
    environment.update(TEMP=str(directory), TMP=str(directory), TMPDIR=str(directory))
    environment['PYTHONPATH'] = str(root) + os.pathsep + environment.get('PYTHONPATH', '')
    report = {'schema': 1, 'profile': args.profile, 'started_utc': datetime.now(timezone.utc).isoformat(),
              'workspace': str(root), 'output': str(directory), 'python': sys.version, 'python_executable': sys.executable, 'steps': [], 'skips': []}
    def step(name, command, cwd=root):
        record = run_step(name, command, directory, environment, cwd=cwd)
        report['steps'].append(record)
        print(record['status'].upper() + ' ' + name, flush=True)
        write_json(directory / 'report.json', report)
        return record['status'] == 'passed'
    try:
        report['dependencies'] = python_dependency_preflight(args.profile, run_hardware=args.run_hardware, root=root)
        report['source_tree_sha256'] = source_manifest(root)['tree_sha256']
        if args.profile == 'hardware':
            if not args.port or not args.expected_elf_sha256:
                raise ValueError('Hardware profile requires --port and --expected-elf-sha256, even for a plan')
            step('connectivity-hardware', [sys.executable, root / 'tools/check_connectivity_device.py',
                 '--port', args.port, '--expected-elf-sha256', args.expected_elf_sha256,
                 '--out', directory / 'connectivity', '--run' if args.run_hardware else '--dry-run'])
            if not args.run_hardware: report['skips'].append({'group': 'hardware execution', 'reason': '--run-hardware was not specified; plan only'})
        else:
            report['features'] = source_features(root)
            if args.profile == 'offline':
                if args.sim is None: raise ValueError('Offline profile requires --sim built with sim/nos_build.ps1 -Strict')
                simulator = args.sim.resolve()
                report['simulator'] = simulator_identity(root, simulator)
                cjson = Path(args.cjson_dir or Path(environment.get('IDF_PATH', str(root.parent / 'esp-idf'))) / 'components/json/cJSON').resolve()
                dependency = [cjson / 'cJSON.c', cjson / 'cJSON.h']
                if not all(path.is_file() for path in dependency):
                    raise ValueError('Offline weather parser tests require SDK cJSON sources; supply --cjson-dir')
                report['cjson'] = {str(path): sha256(path) for path in dependency}
            compiler = shutil.which('gcc')
            if not compiler: raise ValueError('Host GCC is required; missing toolchains must not silently skip C regressions')
            step('compiler-version', [compiler, '--version'])
            modules = sorted((root / 'tests').glob('test_*.py')) if args.profile == 'offline' else [root / path for path in matrix['host_tests']]
            report['selected_tests'] = [p.relative_to(root).as_posix() for p in modules]
            for module in modules:
                name = module.stem
                unit_report = directory / (name + '.json')
                step(name, [sys.executable, root / 'tools/validation_unit.py', module, '--report', unit_report])
                if unit_report.is_file():
                    counts = json.loads(unit_report.read_text())
                    report['steps'][-1]['unit_results'] = counts
                    report['skips'].extend({'group': name, **skip} for skip in counts['skips'])
            if args.profile == 'offline':
                step('native-menu-assets', [sys.executable, root / 'tools/generate_native_menu_assets.py', '--verify'])
                step('orrery-sky-pack', [sys.executable, root / 'tools/test_orrery_sd.py'])
                for case in matrix['native_tests']:
                    executable = directory / (case['name'] + '.exe')
                    sources = [root / path.replace('{cjson}', str(cjson)) for path in case['sources']]
                    flags = ['-std=c99', '-O2', '-Wall', '-Wextra', '-Werror',
                             '-I' + str(root / 'core'), '-I' + str(root / 'nos'), '-I' + str(root / 'firmware/main')]
                    flags += case.get('flags', [])
                    flags = [f.replace('{root}', str(root)).replace('{cjson}', str(cjson)) for f in flags]
                    if step(case['name'] + '-compile', [compiler, *flags, *sources, '-lm', '-o', executable]):
                        if case.get('directory'):
                            (directory / (case['name'] + '-data')).mkdir()
                        for index, parameters in enumerate(case.get('runs', [[]])):
                            parameters = [p.replace('{root}', str(root)).replace('{cjson}', str(cjson)) for p in parameters]
                            if case.get('directory'): parameters = [str(directory / (case['name'] + '-data')), *parameters]
                            step(case['name'] + '-' + str(index), [executable, *parameters], cwd=directory)
                step('sim-script-integrity', [sys.executable, root / 'tools/test_sim_script_io.py', '--sim', simulator])
                step('sports-simulator', [sys.executable, root / 'tools/test_workout_sim.py', '--sim', simulator,
                                         '--out', directory / 'sports', '--no-media'])
            else:
                report['skips'].append({'group': 'full NOS/native/pixel/Sports simulator',
                    'reason': 'host profile; run --profile offline --sim <strict current NOS executable>'})
            report['skips'].append({'group': 'hardware-only QA', 'reason': 'requires --profile hardware with explicit target identity'})
        report['status'] = 'failed' if any(s['status'] == 'failed' for s in report['steps']) else 'passed'
        report['final_source_tree_sha256'] = source_manifest(root)['tree_sha256']
        if report['final_source_tree_sha256'] != report['source_tree_sha256']:
            report.update(status='failed', error='Source/config/assets changed during validation; rerun after edits settle')
        if 'cjson' in report and any(sha256(Path(path)) != expected for path, expected in report['cjson'].items()):
            report.update(status='failed', error='cJSON dependency changed during validation')
    except (OSError, ValueError, KeyError) as exc:
        report.update(status='failed', error=str(exc))
        print(str(exc), file=sys.stderr)
    finally:
        report['completed_utc'] = datetime.now(timezone.utc).isoformat()
        write_json(directory / 'report.json', report)
        print(str(directory / 'report.json'), flush=True)
    return 0 if report['status'] == 'passed' else 1

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--profile', choices=('host', 'offline', 'hardware'), default='host')
    parser.add_argument('--out', type=Path, help='Parent directory for a fresh workspace-local run directory')
    parser.add_argument('--sim', type=Path)
    parser.add_argument('--cjson-dir', type=Path, help='cJSON.c/h directory (default: ESP-IDF components/json/cJSON)')
    parser.add_argument('--port')
    parser.add_argument('--expected-elf-sha256')
    parser.add_argument('--run-hardware', action='store_true')
    args = parser.parse_args(argv)
    if args.run_hardware and args.profile != 'hardware': parser.error('--run-hardware requires --profile hardware')
    return validate(args)

if __name__ == '__main__': raise SystemExit(main())
