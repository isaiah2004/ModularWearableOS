"""Create a new immutable firmware bundle from a verified current-workspace build."""
from __future__ import annotations
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import sys
import uuid
import zipfile
if __package__:
    from .host_support import ROOT, contained, sha256, write_json, local_environment
    from .build_identity import verify
else:
    from host_support import ROOT, contained, sha256, write_json, local_environment
    from build_identity import verify

SUPPORT = ('flash_watch.py', 'host_transport.py', 'host_support.py', 'flash_layout.py', 'build_identity.py')

def package(root, build, destination=None):
    root, build = Path(root).resolve(), Path(build).resolve()
    identity = verify(build, root=root)
    local_environment(root, minimum_free=sum(record['bytes'] for record in identity['files']) + 1024 * 1024)
    destination = Path(destination or root / 'dist' / ('NOS-v0.5-Alpha-' + identity['bundle_id'][:16])).resolve()
    contained(root, destination)
    if destination.exists(): raise ValueError('Bundle destination already exists; preserve it and select a new directory')
    staging = destination.with_name(destination.name + '.' + uuid.uuid4().hex + '.partial')
    staging.mkdir(parents=True)
    for record in identity['files']:
        source, target = build / record['file'], contained(staging, staging / record['file'])
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    shutil.copy2(build / 'build-identity.json', staging / 'build-identity.json')
    (staging / 'tooling').mkdir()
    for name in SUPPORT: shutil.copy2(root / 'tools' / name, staging / 'tooling' / name)
    with zipfile.ZipFile(build / identity['source_archive']) as archive:
        for asset in identity['sd_assets']['files']:
            target = contained(staging, staging / 'sd' / asset['destination'])
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(asset['source']) as source, target.open('wb') as output:
                shutil.copyfileobj(source, output)
            if sha256(target) != asset['sha256']: raise ValueError('Archived Solar SD asset hash mismatch')
    shutil.copy2(root / 'tools/flash-package.ps1', staging / 'flash.ps1')
    evidence_path = root / 'docs/device-validation.json'
    evidence = json.loads(evidence_path.read_text(encoding='utf-8-sig')) if evidence_path.is_file() else None
    tested = {r['file']: r['sha256'] for r in evidence.get('images', [])} if evidence else {}
    matches = bool(tested) and all(tested.get(Path(s['file']).name) == s['sha256'] for s in identity['segments'])
    write_json(staging / 'hardware-validation.json', {'applies_to_packaged_images': matches, 'evidence': evidence})
    (staging / 'README.md').write_text('''# NOS v0.5 Alpha — Waveshare ESP32-S3-Touch-AMOLED-1.43-B

Use the ESP-IDF Python environment (pyserial and esptool installed), close other
serial monitors, and select the intended device's port explicitly:

```powershell
.\\flash.ps1 -Port COM3 -Python <ESP-IDF-python.exe> -DryRun
.\\flash.ps1 -Port COM3 -Python <ESP-IDF-python.exe>
```

The same checked updater as the workspace validates every bundled artifact,
partition/app limits and flash ranges, then requires a completed `prefs flush`
with `ok=1 stored=1 match=1` before any esptool reset. A failed checkpoint stops
the update. NVS is never erased. Settings are authoritative in the two checked
SD files `/watch/settings.0.bin` and `/watch/settings.1.bin`; NVS is a fallback.
The actual offsets come from flasher_args.json, including OTA initialization.
Never substitute fixed app offsets from older packages.

Never flash a merged image at 0x0 for an update: its gap bytes overwrite NVS.
No merged image is supplied. The source archive, source manifest, ELF/map,
sdkconfig, dependency lock, compiler identity, linked feature registry and exact
partition headroom are indexed by build-identity.json and manifest.json.
Use the bundled ELF with the matching ESP-IDF addr2line to symbolicate PCs:
`xtensa-esp32s3-elf-addr2line -pfiaC -e watchfw.elf 0xADDRESS`.

Hardware evidence applies only when all packaged segment hashes match the record.
Host checks do not establish display/touch, radio, power or sensor accuracy.
The 1.43C uses separate wiring/runtime and is not supported by this package.
Generated Solar SD textures present at build time are copied to sd/sky, and the
level-0 native menu art pack to sd/menu; both are bound
by build-identity.json sd_assets. Its missing list explicitly records unavailable
artwork; missing SD textures use the firmware's degraded rendering fallback.
Provision bundled textures separately with tools/sdsync.py <bundle>/sd --port COM3.
''', encoding='utf-8')
    # Validate the copied artifacts too, then bind updater/docs/evidence hashes.
    verify(staging)
    verify(build, root=root)  # source/artifact edits during packaging invalidate publication
    records = [{'file': p.relative_to(staging).as_posix(), 'bytes': p.stat().st_size, 'sha256': sha256(p)}
               for p in sorted(staging.rglob('*')) if p.is_file()]
    write_json(staging / 'manifest.json', {'schema': 1, 'bundle_id': identity['bundle_id'],
                'packaged_utc': datetime.now(timezone.utc).isoformat(), 'files': records,
                'hardware_validated': False, 'hardware_evidence_matches': matches,
                'budget': identity['budget'], 'source_tree_sha256': identity['source_tree_sha256']})
    os.replace(staging, destination)
    return destination

def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=ROOT / 'firmware/build')
    parser.add_argument('--out', type=Path)
    args = parser.parse_args(argv)
    try:
        print(package(ROOT, args.build, args.out)); return 0
    except (OSError, ValueError, KeyError, RuntimeError) as exc:
        print(f'package: {exc}', file=sys.stderr); return 1

if __name__ == '__main__': raise SystemExit(main())
