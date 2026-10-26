"""Bind dirty+untracked sources, assets, config, symbols and flash layout to a build.

prepare is called BEFORE idf.py build, finalize only AFTER a successful build.
The source archive is the actual workspace, never git HEAD. Package/flash reject
old builds without this identity and builds whose inputs/artifacts have changed.
"""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import uuid
import zipfile
import zlib
if __package__:
    from .host_support import ROOT, checked_run, contained, sha256, write_json, local_environment
    from .flash_layout import checked_layout
else:
    from host_support import ROOT, checked_run, contained, sha256, write_json, local_environment
    from flash_layout import checked_layout

SOURCE_DIRS = ('core', 'nos', 'firmware', 'sim', 'tools', 'tests', 'assets', 'third_party', '.github')
SKY_ASSETS = {name + '.565': 1024 * 512 * 2 for name in
              ('sun','mercury','venus','earth','earth_night','moon','mars','jupiter','saturn','uranus','neptune')}
SKY_ASSETS['saturn_ring.rgba'] = 512 * 4
# The catalogs tools/skypack_c.py writes (nos/faces/orrery_sd.c reads them): magic and record bytes; size comes from the header.
SKY_PACK = {'belt.bin': (b'ORBL', 32), 'moons.bin': (b'ORMN', 88)}
SKY_ASSETS.update({name: None for name in SKY_PACK})
# Tracked canonical copies that tools/sdsync.py must push from build/sd: staged there on every prepare.
TRACKED_SD = {'menu/native_menu_sharp.pack': 'assets/sd/menu/native_menu_sharp.pack',
              'sky/belt.bin': 'assets/sd/sky/belt.bin', 'sky/moons.bin': 'assets/sd/sky/moons.bin'}
REQUIRED_FEATURES = {
    'Solar/Orrery': ('nos/faces/faces.c', '&face_orrery', 'face_orrery', 'nos/faces/face_orrery.c'),
    'Developer tools': ('nos/apps/registry.c', '&scr_developer', 'scr_developer', 'nos/apps/set_developer.c'),
    'Sports': ('nos/apps/registry.c', '&scr_workout', 'scr_workout', 'nos/apps/fit_workout.c'),
    'Energy': ('nos/apps/registry.c', '&scr_energy', 'scr_energy', 'nos/apps/fit_energy.c'),
    'Sensor logs': ('nos/apps/registry.c', '&scr_sports_logs', 'scr_sports_logs', 'nos/apps/fit_energy.c'),
    'Background jobs': ('nos/apps/registry.c', '&scr_background', 'scr_background', 'nos/apps/sys_background.c'),
    'Voice chat': ('nos/apps/registry.c', '&scr_chat', 'scr_chat', 'nos/apps/con_chat.c'),
    'Integrated home pager': ('nos/apps/sys_home.c', 'sys_launcher_page.h', 'scr_home', 'nos/apps/sys_home.c'),
}

def source_paths(root):
    root = Path(root).resolve()
    paths = []
    for name in SOURCE_DIRS:
        for p in (root / name).rglob('*'):
            relative = p.relative_to(root)
            if not p.is_file() or p.is_symlink(): continue
            if any(part in ('__pycache__', '.git', 'managed_components', 'node_modules') or part.startswith('build')
                   for part in relative.parts[1:-1]): continue
            if p.suffix in ('.pyc', '.o', '.exe', '.obj') or p.name.endswith('.old'): continue
            paths.append(p)
    paths.extend(p for p in (root / 'AGENTS.md', root / 'README.md', root / '.gitignore') if p.is_file())
    # These generated runtime assets deliberately live under ignored build/sd.
    # Include only Solar artwork, never logs/recordings or a copied user's card.
    paths.extend(p for p in (root / 'build/sd/sky').glob('*') if p.is_file() and p.name in SKY_ASSETS)
    paths.extend(p for p in (root / 'build/sd/menu/native_menu_sharp.pack',) if p.is_file())
    return sorted(set(paths))

def source_manifest(root):
    root = Path(root).resolve()
    records = [{'path': p.relative_to(root).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha256(p)} for p in source_paths(root)]
    digest = hashlib.sha256(json.dumps(records, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return {'tree_sha256': digest, 'files': records}

def source_features(root):
    result = {}
    for name, (path, marker, symbol, source) in REQUIRED_FEATURES.items():
        if marker not in (Path(root) / path).read_text(encoding='utf-8'):
            raise ValueError(f'Required current-runtime feature missing from registry: {name}')
        if not (Path(root) / source).is_file(): raise ValueError(f'Feature implementation missing: {source}')
        result[name] = {'registry': path, 'symbol': symbol, 'source': source}
    return result

def sky_pack_size(path, name, root):
    """Validate a catalog's header, payload CRC32 and that it belongs to the pack compiled into the flash tables."""
    data = Path(path).read_bytes()
    magic, record = SKY_PACK[name]
    if len(data) < 48: raise ValueError('Generated Solar SD asset is truncated: ' + name)
    tag, version, head, count, recsz, payload, crc, jd = struct.unpack_from('<4sHHIIIId', data)
    if (tag, version, head, recsz) != (magic, 1, 48, record) or len(data) != 48 + payload or not count:
        raise ValueError('Generated Solar SD asset has an invalid header: ' + name)
    if zlib.crc32(data[48:]) & 0xffffffff != crc: raise ValueError('Generated Solar SD asset fails its CRC32: ' + name)
    source = Path(root) / 'nos/faces/orrery_data.c'
    found = re.search(r'od_pack_jd\s*=\s*([0-9.]+);', source.read_text(encoding='utf-8')) if source.is_file() else None
    if found and float(found[1]) != jd: raise ValueError('Generated Solar SD asset is from another sky pack than orrery_data.c: ' + name)
    return len(data)

def stage_sd(root):
    """Copy the tracked menu pack and sky catalogs into build/sd (what sdsync mirrors) when absent or different."""
    staged = []
    for destination, source in TRACKED_SD.items():
        source, target = Path(root) / source, Path(root) / 'build/sd' / destination
        if not source.is_file(): continue
        data = source.read_bytes()
        if target.is_file() and target.read_bytes() == data: continue
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        staged.append(destination)
    return staged

def menu_pack_check(root, pack):
    """The pack must be the one this source tree builds: header and payload CRC32, and its build hash vs native_menu_assets_id.h."""
    data = Path(pack).read_bytes()
    if len(data) < 152 or data[:4] != b'NMAP': raise ValueError('Native menu SD pack has an invalid header')
    hdr_bytes, file_bytes, payload_crc = struct.unpack_from('<III', data, 8)
    if hdr_bytes != 152 or file_bytes != len(data): raise ValueError('Native menu SD pack has an invalid size')
    if zlib.crc32(data[:148]) & 0xffffffff != struct.unpack_from('<I', data, 148)[0] or             zlib.crc32(data[hdr_bytes:]) & 0xffffffff != payload_crc:
        raise ValueError('Native menu SD pack fails its CRC32')
    ident = Path(root) / 'nos/native_menu_assets_id.h'
    found = re.search(r'NATIVE_MENU_BUILD_SHA256\s+"([0-9a-f]{64})"', ident.read_text(encoding='utf-8')) if ident.is_file() else None
    if found and data[84:116].hex() != found[1]:
        raise ValueError('Native menu SD pack is stale for this source tree (build hash differs from nos/native_menu_assets_id.h); regenerate with tools/generate_native_menu_assets.py --export')

def sd_assets(root):
    records, missing = [], []
    for name, size in sorted(SKY_ASSETS.items()):
        path = Path(root) / 'build/sd/sky' / name
        if not path.is_file(): missing.append('sky/' + name);continue
        if size is None: size = sky_pack_size(path, name, root)
        if path.stat().st_size != size: raise ValueError('Generated Solar SD asset has an invalid size: ' + name)
        records.append({'source': 'build/sd/sky/' + name, 'destination': 'sky/' + name,
                        'bytes': size, 'sha256': sha256(path)})
    pack = Path(root) / 'build/sd/menu/native_menu_sharp.pack'  # staged from the tracked copy; this is what sdsync sends
    if not pack.is_file(): missing.append('menu/native_menu_sharp.pack')
    else:
        menu_pack_check(root, pack)
        records.append({'source': 'build/sd/menu/native_menu_sharp.pack', 'destination': 'menu/native_menu_sharp.pack',
                        'bytes': pack.stat().st_size, 'sha256': sha256(pack)})
    for destination, source in TRACKED_SD.items():
        tracked, staged = Path(root) / source, Path(root) / 'build/sd' / destination
        if tracked.is_file() and staged.is_file() and tracked.read_bytes() != staged.read_bytes():
            raise ValueError('Staged SD asset differs from its tracked copy: ' + destination)
    return {'complete': not missing, 'files': records, 'missing': missing}

def image_elf_identity(image, elf):
    # ESP-IDF esp_image_header (24B), first segment header (8B),
    # esp_app_desc_t.magic_word + its app_elf_sha256 at descriptor offset 144.
    with Path(image).open('rb') as stream: header = stream.read(288)
    if len(header) < 288 or header[0] != 0xE9 or struct.unpack_from('<I', header, 32)[0] != 0xABCD5432:
        raise ValueError('Application image has no valid ESP-IDF descriptor')
    if header[176:208].hex() != sha256(elf):
        raise ValueError('Application image and symbolication ELF hashes do not match')
    return {'idf_version': header[144:176].split(b'\0', 1)[0].decode('ascii'),
            'firmware_elf_sha256': header[176:208].hex()}

def prepare(root, directory):
    root, directory = Path(root).resolve(), Path(directory).resolve()
    contained(root, directory)
    staged = stage_sd(root)  # before the manifest: build/sd/{menu,sky} files are hashed into it
    if staged: print('build_identity: staged ' + ', '.join(staged) + ' into build/sd from assets/sd', file=sys.stderr)
    features, manifest, assets = source_features(root), source_manifest(root), sd_assets(root)
    if not assets['complete']:
        print('build_identity: WARNING SD assets incomplete, missing ' + ', '.join(assets['missing']) +
              '; the watch will fall back (no Orrery photos/belt/moons, dynamic menu bake) until tools/sdsync.py has pushed them', file=sys.stderr)
    local_environment(root, minimum_free=sum(record['bytes'] for record in manifest['files']) + 1024 * 1024)
    run = directory / 'provenance' / uuid.uuid4().hex
    run.mkdir(parents=True)
    archive = run / 'source.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as output:
        for record in manifest['files']: output.write(root / record['path'], record['path'])
    if source_manifest(root) != manifest: raise ValueError('Source changed while archiving; retry after edits settle')
    write_json(run / 'source-manifest.json', manifest)
    generated = directory / 'generated/nos_build_identity.h'
    generated.parent.mkdir(parents=True, exist_ok=True)
    generated.write_text('#pragma once\n#define NOS_SOURCE_TREE_SHA256 "' + manifest['tree_sha256'] +
                         '"\n#define NOS_SOURCE_BUILD_ID "' + manifest['tree_sha256'][:16] + '"\n', encoding='utf-8')
    record = {'schema': 1, 'prepared_utc': datetime.now(timezone.utc).isoformat(), 'workspace': str(root),
              'build_directory': str(directory), 'source_tree_sha256': manifest['tree_sha256'],
              'source_manifest': (run / 'source-manifest.json').relative_to(directory).as_posix(),
              'source_archive': archive.relative_to(directory).as_posix(), 'features': features,
              'sd_assets': assets,
              'source_archive_sha256': sha256(archive), 'source_manifest_sha256': sha256(run / 'source-manifest.json')}
    write_json(directory / 'source-inputs.json', record)
    return record

def finalize(root, directory):
    root, directory = Path(root).resolve(), Path(directory).resolve()
    inputs = json.loads((directory / 'source-inputs.json').read_text())
    if Path(inputs['workspace']).resolve() != root or Path(inputs['build_directory']).resolve() != directory:
        raise ValueError('Source snapshot belongs to another workspace/build directory')
    manifest = source_manifest(root)
    if inputs['source_tree_sha256'] != manifest['tree_sha256']:
        raise ValueError('Sources/config/assets changed during firmware build; rebuild before packaging or flashing')
    for key in ('source_archive', 'source_manifest'):
        if sha256(contained(directory, directory / inputs[key])) != inputs[key + '_sha256']:
            raise ValueError('Prepared source snapshot changed: ' + key)
    description = json.loads((directory / 'project_description.json').read_text())
    if Path(description['project_path']).resolve() != root / 'firmware' or description['target'] != 'esp32s3':
        raise ValueError('Firmware artifacts belong to another workspace or target')
    _, segments, budget = checked_layout(directory)
    elf = directory / description['app_elf']
    if manifest['tree_sha256'].encode('ascii') not in elf.read_bytes():
        raise ValueError('ELF omits the prepared source identity; include generated/nos_build_identity.h and emit NOS_SOURCE_TREE_SHA256 in NOSBUILD')
    app_image = next(segment for segment in segments if segment['offset'] == budget['app_offset'])
    image_description = image_elf_identity(directory / app_image['file'], elf)
    compiler = Path(description['c_compiler'])
    nm = compiler.with_name(compiler.name.replace('gcc', 'nm'))
    symbols_text = checked_run([nm, '--defined-only', elf], env=local_environment(root)).stdout
    symbols = {line.split()[-1] for line in symbols_text.splitlines() if line.split()}
    compiled = json.loads((directory / 'compile_commands.json').read_text())
    compiled_paths = {Path(c['file']).resolve() for c in compiled}
    features = source_features(root)
    for name, feature in features.items():
        if feature['symbol'] not in symbols or (root / feature['source']).resolve() not in compiled_paths:
            raise ValueError(f'Required feature is absent from compiled/linked firmware: {name}')
        feature['linked'] = True
    toolchain = {'compiler': str(compiler), 'sha256': sha256(compiler),
                 'version': checked_run([compiler, '--version'], env=local_environment(root)).stdout,
                 'idf_path': description['idf_path'], 'idf_git_revision': description.get('git_revision'),
                 **image_description}
    paths = {s['file'] for s in segments}
    paths.update({description['app_elf'], 'watchfw.map', 'project_description.json', 'compile_commands.json',
                  'flasher_args.json', 'flash_args', inputs['source_manifest'], inputs['source_archive'], 'source-inputs.json',
                  'generated/nos_build_identity.h'})
    paths.update({'config/sdkconfig.h', 'config/sdkconfig.json'})
    configuration = Path(description.get('config_file', root / 'firmware/sdkconfig'))
    if not configuration.is_absolute(): configuration = root / 'firmware' / configuration
    contained(root, configuration)
    for source, dest in ((configuration, 'provenance/sdkconfig'),
                         (root / 'firmware/dependencies.lock', 'provenance/dependencies.lock')):
        if not source.is_file(): raise ValueError(f'Required build configuration missing: {source}')
        target = directory / dest
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(source.read_bytes()); paths.add(dest)
    files = [{'file': name, 'bytes': contained(directory, directory / name).stat().st_size,
              'sha256': sha256(directory / name)} for name in sorted(paths)]
    identity = {'schema': 1, 'built_utc': datetime.now(timezone.utc).isoformat(), 'workspace': str(root),
                'source_tree_sha256': manifest['tree_sha256'], 'firmware_elf_sha256': sha256(elf),
                'features': features, 'budget': budget, 'toolchain': toolchain,
                'source_archive': inputs['source_archive'], 'source_manifest': inputs['source_manifest'],
                'sd_assets': inputs['sd_assets'], 'segments': segments, 'files': files}
    identity['bundle_id'] = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    write_json(directory / 'build-identity.json', identity)
    return identity

def verify(directory, *, root=None):
    directory = Path(directory).resolve()
    identity = json.loads((directory / 'build-identity.json').read_text(encoding='utf-8-sig'))
    unsigned = dict(identity); expected = unsigned.pop('bundle_id')
    if hashlib.sha256(json.dumps(unsigned, sort_keys=True).encode()).hexdigest() != expected:
        raise ValueError('Build identity is corrupt')
    if root is not None:
        root = Path(root).resolve()
        if Path(identity['workspace']).resolve() != root: raise ValueError('Build belongs to another workspace')
        if source_manifest(root)['tree_sha256'] != identity['source_tree_sha256']:
            raise ValueError('Firmware build does not match current dirty/untracked source/config/assets')
    for record in identity['files']:
        file = contained(directory, directory / record['file'])
        if file.stat().st_size != record['bytes'] or sha256(file) != record['sha256']:
            raise ValueError('Build artifact changed: ' + record['file'])
    _, segments, budget = checked_layout(directory)
    if segments != identity['segments'] or budget != identity['budget']:
        raise ValueError('Flash layout no longer matches the verified build')
    if not all(feature.get('linked') for feature in identity['features'].values()) or set(identity['features']) != set(REQUIRED_FEATURES):
        raise ValueError('Required compiled/linked feature registry is incomplete')
    return identity

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('prepare', 'finalize', 'verify'))
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--build', type=Path, default=ROOT / 'firmware/build')
    args = parser.parse_args(argv)
    try:
        result = verify(args.build, root=args.root) if args.action == 'verify' else globals()[args.action](args.root, args.build)
        print(json.dumps({k: result[k] for k in ('bundle_id', 'source_tree_sha256', 'budget') if k in result}))
        return 0
    except (OSError, KeyError, ValueError, RuntimeError) as exc:
        print(f'build_identity: {exc}', file=sys.stderr); return 1

if __name__ == '__main__': raise SystemExit(main())
