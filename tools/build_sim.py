"""Strict simulator builds with current-source fingerprints and isolated caches."""
from __future__ import annotations
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys
if __package__:
    from .host_support import ROOT, checked_run, contained, local_environment, run_directory, sha256, write_json
else:
    from host_support import ROOT, checked_run, contained, local_environment, run_directory, sha256, write_json

CORE = 'asset_job gfx font sprite anim notif battery pedometer walk motion_wake wake_service knock ephem'.split()

def fingerprint(paths, root):
    return {p.relative_to(root).as_posix(): sha256(p) for p in sorted(set(paths))}

def digest(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True).encode()).hexdigest()

def compile_object(compiler, flags, source, temporary, target, environment, log):
    checked_run([compiler, *flags, '-c', source, '-o', temporary], env=environment, log=log)
    if not temporary.is_file() or not temporary.stat().st_size:
        raise RuntimeError(f'Compiler produced no nonempty object: {source}')
    os.replace(temporary, target)

def verify_cost(root, compiler, environment, directory):
    snapshot = directory / 'snapshot'
    shutil.copytree(root / 'core', snapshot / 'core')
    (snapshot / 'tools').mkdir()
    shutil.copy2(root / 'tools/gfx_cost_exact.c', snapshot / 'tools/gfx_cost_exact.c')
    paths = [p for p in snapshot.rglob('*') if p.is_file()]
    before = fingerprint(paths, snapshot)
    sources = [snapshot / 'tools/gfx_cost_exact.c',
               *[snapshot / 'core' / (s + '.c') for s in ('gfx', 'font', 'sprite')],
               *sorted((snapshot / 'core/fonts').glob('*.c'))]
    expected_size = 400 * 466 * 466 * 2  # 400 full RGB565 frames in gfx_cost_exact.c
    records = []
    for name, definitions in (('cost', ['-DGFX_COST_MODEL']), ('plain', [])):
        executable, output = directory / (name + '.exe'), directory / (name + '.bin')
        checked_run([compiler, '-O2', '-std=c99', '-w', '-I' + str(snapshot / 'core'),
                     *definitions, *sources, '-lm', '-o', executable], env=environment,
                    log=directory / (name + '-compile.log'))
        checked_run([executable, output], cwd=directory, env=environment, log=directory / (name + '-run.log'))
        if not output.is_file() or output.stat().st_size != expected_size:
            raise RuntimeError(f'{name}: expected a fresh {expected_size}-byte raster')
        records.append({'profile': name, 'sha256': sha256(output), 'bytes': output.stat().st_size})
    if records[0]['sha256'] != records[1]['sha256']:
        raise RuntimeError('Cost hooks changed pixels in the same current-source snapshot')
    checked_run([directory / 'cost.exe', '--accuracy'], cwd=directory, env=environment, log=directory / 'accuracy.log')
    if before != fingerprint(paths, snapshot):
        raise RuntimeError('Verification snapshot changed during the comparison')
    report = {'status': 'passed', 'reference': 'same current source, hooks disabled',
              'source_sha256': digest(before), 'sources': before, 'rasters': records}
    write_json(directory / 'verify.json', report)
    return report

def build(root, compiler, output='nos_sim.exe', *, clean=False, debug=False,
          no_cost=False, verify=False, legacy=False, jobs=None):
    root = Path(root).resolve()
    if Path(output).name != output or '/' in output or '\\' in output or not output.endswith('.exe'):
        raise ValueError('Output must be an .exe basename inside build/')
    target = contained(root, root / 'build' / output)
    target.parent.mkdir(parents=True, exist_ok=True)
    # Space for both 400-frame rasters plus objects, on the workspace volume.
    environment = local_environment(root, minimum_free=(2 * 400 * 466 * 466 * 2 if verify else 0) + 64 * 1024 * 1024)
    directory = run_directory('sim', root)
    cache = root / 'build/sim-objects' / output
    cache.mkdir(parents=True, exist_ok=True)
    lock = cache / 'build.lock'
    try:
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
    except FileExistsError as exc:
        raise RuntimeError(f'Another build owns {lock}; inspect its PID before removing a stale lock') from exc
    os.write(descriptor, str(os.getpid()).encode()); os.close(descriptor)
    report = {'status': 'failed', 'output': str(target), 'logs': str(directory), 'strict': True}
    try:
        if not legacy:
            checked_run([sys.executable, root / 'tools/generate_native_menu_assets.py', '--verify'],
                        cwd=root, env=environment, log=directory / 'assets.log')
        if legacy:
            sources = [*sorted((root / 'core').glob('*.c')), *sorted((root / 'core/apps').glob('*.c')),
                       *sorted((root / 'core/fonts').glob('*.c')), root / 'sim/main_win32.c']
        else:
            sources = [*sorted((root / 'nos').rglob('*.c')), *[root / 'core' / (s + '.c') for s in CORE],
                       *sorted((root / 'core/fonts').glob('*.c')), *sorted((root / 'sim').glob('nos_*.c'))]
        headers = [p for name in ('core', 'nos', 'sim') for p in (root / name).rglob('*') if p.suffix in ('.h', '.inc')]
        inputs = fingerprint(sources + headers, root)
        optimization = ['-O0', '-g'] if debug else ['-O2']
        flags = [*optimization, '-std=c99', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-format-truncation',
                 *['-I' + str(root / p) for p in ('core', 'nos', 'nos/fonts', 'sim')]]
        if not legacy:
            flags += ['-DNOS_PSRAM_TRACKING']
            if not no_cost: flags += ['-DGFX_COST_MODEL']
            if not (root / 'nos/fonts/nos_fonts.h').is_file(): flags += ['-DNOS_FONT_SHIM']
        compiler = Path(compiler).resolve()
        version = checked_run([compiler, '--version'], env=environment).stdout
        shared = digest({'headers': fingerprint(headers, root), 'flags': flags, 'compiler': sha256(compiler), 'version': version})
        old_path = cache / 'objects.json'
        old = json.loads(old_path.read_text()) if old_path.is_file() else {}
        objects, pending = [], []
        for source in sources:
            relative = source.relative_to(root).as_posix()
            key = digest([relative, inputs[relative], shared])
            obj = cache / (hashlib.sha256(relative.encode()).hexdigest()[:20] + '.o')
            objects.append((relative, obj, key))
            record = old.get(relative, {})
            if clean or record.get('key') != key or not obj.is_file() or record.get('sha256') != sha256(obj):
                pending.append((source, directory / (obj.name + '.tmp'), obj, directory / (obj.stem + '.log')))
        with ThreadPoolExecutor(max_workers=jobs or min(os.cpu_count() or 1, 8)) as executor:
            futures = [executor.submit(compile_object, compiler, flags, s, t, d, environment, log) for s, t, d, log in pending]
            failures = []
            for future in futures:
                try: future.result()
                except Exception as exc: failures.append(str(exc))
        if failures: raise RuntimeError('Compilation failed; executable was not linked:\n' + '\n'.join(failures))
        if fingerprint(sources + headers, root) != inputs: raise RuntimeError('Sources changed during compilation; retry after edits settle')
        new_executable = directory / output
        checked_run([compiler, *optimization, *[obj for _, obj, _ in objects], '-o', new_executable,
                     '-lgdi32', '-luser32', '-lwinmm', '-lm'], env=environment, log=directory / 'link.log')
        if not new_executable.is_file() or not new_executable.stat().st_size: raise RuntimeError('Linker produced no executable')
        if verify: report['verification'] = verify_cost(root, compiler, environment, directory)
        if fingerprint(sources + headers, root) != inputs: raise RuntimeError('Sources changed during link/verification; output was not published')
        try: os.replace(new_executable, target)
        except PermissionError:
            os.replace(target, directory / (output + '.previous'))
            os.replace(new_executable, target)
        records = {relative: {'key': key, 'sha256': sha256(obj)} for relative, obj, key in objects}
        write_json(old_path, records)
        report.update(status='passed', source_sha256=digest(inputs), sources=inputs, flags=flags,
                      compiler={'path': str(compiler), 'sha256': sha256(compiler), 'version': version},
                      objects=records, executable_sha256=sha256(target), compiled=len(pending),
                      runtime='legacy core/apps' if legacy else 'NOS')
        write_json(target.with_suffix(target.suffix + '.build.json'), report)
        return target, directory
    except Exception as exc:
        report['error'] = str(exc)
        raise
    finally:
        write_json(directory / 'build.json', report)
        lock.unlink()  # only this invocation's lock; never recursive cleanup

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', default='nos_sim.exe')
    parser.add_argument('--compiler', default=shutil.which('gcc'))
    for name in ('clean', 'debug', 'no-cost', 'verify', 'legacy'): parser.add_argument('--' + name, action='store_true')
    args = parser.parse_args(argv)
    if not args.compiler: parser.error('Put host GCC/WinLibs on PATH, or supply --compiler')
    try:
        target, logs = build(ROOT, args.compiler, args.out, clean=args.clean, debug=args.debug,
                             no_cost=args.no_cost, verify=args.verify, legacy=args.legacy)
        print(f'Built {target}; strict manifest {target}.build.json; logs {logs}')
        return 0
    except (OSError, ValueError, RuntimeError) as exc:
        print(f'build_sim: {exc}', file=sys.stderr)
        return 1

if __name__ == '__main__': raise SystemExit(main())
