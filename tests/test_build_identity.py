"""Bundles bind current recovered sources, symbols, partitions and exact images."""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib
from unittest.mock import patch
import zipfile
from tools import build_identity as identity
from tools import package_build, flash_watch
from tools.host_support import sha256
from tests.test_flash_watch import layout

def fake_pack(build_hash,payload=b'payload'):
    head = bytearray(152); head[:4] = b'NMAP'
    struct.pack_into('<III', head, 8, 152, 152 + len(payload), zlib.crc32(payload) & 0xffffffff)
    head[84:116] = build_hash
    struct.pack_into('<I', head, 148, zlib.crc32(bytes(head[:148])) & 0xffffffff)
    return bytes(head) + payload

def workspace(directory):
    root = Path(directory)
    for name in identity.SOURCE_DIRS: (root / name).mkdir(parents=True, exist_ok=True)
    for _, (registry, marker, symbol, source) in identity.REQUIRED_FEATURES.items():
        for path in (root / registry, root / source):
            path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists(): path.write_text('')
        with (root / registry).open('a') as stream: stream.write('\n' + marker)
    (root / 'firmware/sdkconfig').write_text('CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y\n')
    (root / 'firmware/dependencies.lock').write_text('version: 2.0.0\n')
    (root / 'nos/recovered-untracked.c').write_text('int recovered=7;')
    for name in package_build.SUPPORT: (root / 'tools' / name).write_bytes((identity.ROOT / 'tools' / name).read_bytes())
    (root / 'tools/flash-package.ps1').write_bytes((identity.ROOT / 'tools/flash-package.ps1').read_bytes())
    build = root / 'firmware/build'; build.mkdir()
    return root, build

def artifacts(root, build):
    layout(build, app_size=320)
    inputs=json.loads((build/'source-inputs.json').read_text())
    elf = build / 'watchfw.elf'; elf.write_bytes(b'exact ELF symbols\0'+inputs['source_tree_sha256'].encode())
    (build / 'watchfw.map').write_text('symbols and linked section map')
    image = bytearray(320); image[0]=0xE9; struct.pack_into('<I', image, 32, 0xABCD5432)
    image[176:208]=bytes.fromhex(sha256(elf)); (build / 'app.bin').write_bytes(image)
    compiler = root / 'compiler-gcc.exe'; compiler.write_bytes(b'compiler identity')
    (build / 'project_description.json').write_text(json.dumps({'project_path': str(root / 'firmware'),
        'target': 'esp32s3', 'app_elf': 'watchfw.elf', 'c_compiler': str(compiler), 'idf_path': str(root / 'idf')}))
    (build / 'compile_commands.json').write_text(json.dumps([{'file': str(root / feature[3])} for feature in identity.REQUIRED_FEATURES.values()]))
    (build / 'flasher_args.json').write_text(json.dumps({'write_flash_args': ['--flash_mode','dio','--flash_freq','80m','--flash_size','16MB'],
        'flash_files': {'0x0':'boot.bin','0x8000':'partition.bin','0x20000':'app.bin'}}))
    (build / 'config').mkdir(exist_ok=True)
    (build / 'config/sdkconfig.h').write_text('// built config')
    (build / 'config/sdkconfig.json').write_text('{}')

def tool(command, **kwargs):
    text = '\n'.join('0000 D ' + item[2] for item in identity.REQUIRED_FEATURES.values()) if '--defined-only' in command else 'gcc test version'
    return subprocess.CompletedProcess(command, 0, text, '')

class BuildIdentityTests(unittest.TestCase):
    def test_current_untracked_source_archive_and_preserved_mtime_are_authoritative(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, build = workspace(temporary)
            before = identity.prepare(root, build)
            with zipfile.ZipFile(build / before['source_archive']) as archive:
                self.assertEqual(archive.read('nos/recovered-untracked.c'), b'int recovered=7;')
            path=root / 'nos/recovered-untracked.c'; stamp=path.stat().st_mtime_ns
            path.write_text('int recovered=8;'); os.utime(path, ns=(stamp,stamp))
            artifacts(root, build)
            with patch.object(identity,'checked_run',side_effect=tool), self.assertRaisesRegex(ValueError,'changed during'):
                identity.finalize(root, build)

    def test_linked_registry_elf_match_and_actual_partition_budget_are_required(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, build = workspace(temporary); identity.prepare(root, build); artifacts(root,build)
            with patch.object(identity,'checked_run',return_value=subprocess.CompletedProcess([],0,'','')):
                with self.assertRaisesRegex(ValueError,'absent from compiled/linked'): identity.finalize(root,build)
            with patch.object(identity,'checked_run',side_effect=tool): result=identity.finalize(root,build)
            self.assertEqual(result['budget']['app_headroom_bytes'],0x780000-320)
            self.assertTrue(all(feature['linked'] for feature in result['features'].values()))
            self.assertEqual(identity.verify(build,root=root)['bundle_id'],result['bundle_id'])
            (build / 'watchfw.elf').write_bytes(b'wrong ELF')
            with self.assertRaisesRegex(ValueError,'artifact changed'): identity.verify(build,root=root)

    def test_other_workspace_and_tampered_source_snapshot_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, build=workspace(temporary); prepared=identity.prepare(root,build); artifacts(root,build)
            (build / prepared['source_archive']).write_bytes(b'tampered archive')
            with self.assertRaisesRegex(ValueError,'snapshot changed'): identity.finalize(root,build)
            identity.prepare(root,build)
            desc=json.loads((build/'project_description.json').read_text());desc['project_path']=str(root/'another/firmware')
            (build/'project_description.json').write_text(json.dumps(desc))
            with self.assertRaisesRegex(ValueError,'another workspace'): identity.finalize(root,build)

    def test_stale_link_without_prepared_source_hash_is_rejected(self):
        with tempfile.TemporaryDirectory() as temporary:
            root,build=workspace(temporary);identity.prepare(root,build);artifacts(root,build)
            (build/'watchfw.elf').write_bytes(b'old linked code without prepared source ID')
            with self.assertRaisesRegex(ValueError,'omits the prepared source identity'):identity.finalize(root,build)

    def test_package_contains_source_config_elf_checked_updater_and_is_immutable(self):
        with tempfile.TemporaryDirectory() as temporary:
            root, build=workspace(temporary);identity.prepare(root,build);artifacts(root,build)
            with patch.object(identity,'checked_run',side_effect=tool):identity.finalize(root,build)
            output=package_build.package(root,build)
            bundled=flash_watch.verify_package(output)
            self.assertTrue((output/'watchfw.elf').is_file());self.assertTrue((output/'watchfw.map').is_file())
            self.assertTrue((output/bundled['source_archive']).is_file())
            self.assertTrue((output/'provenance/sdkconfig').is_file())
            self.assertTrue((output/'tooling/host_transport.py').is_file())
            hostile=root/'external-workspace';(hostile/'tools').mkdir(parents=True)
            (hostile/'tools/__init__.py').write_text('raise RuntimeError("external workspace helpers must not execute")')
            environment=os.environ.copy();environment['PYTHONPATH']=str(hostile)
            dry_run=subprocess.run([sys.executable,str(output/'tooling/flash_watch.py'),'--package',str(output),
                                    '--port','UNUSED','--dry-run'],cwd=hostile,env=environment,capture_output=True,text=True)
            self.assertEqual(dry_run.returncode,0,dry_run.stdout+dry_run.stderr)
            with self.assertRaisesRegex(ValueError,'already exists'):package_build.package(root,build,output)
            (output/'tooling/flash_watch.py').write_text('changed')
            with self.assertRaisesRegex(ValueError,'Package artifact changed'):flash_watch.verify_package(output)

    def test_generated_sd_artwork_is_hashed_archived_and_packaged_separately(self):
        with tempfile.TemporaryDirectory() as temporary:
            root,build=workspace(temporary);sky=root/'build/sd/sky';sky.mkdir(parents=True)
            earth=sky/'earth.565';earth.write_bytes(b'\x12\x34'*(1024*512))
            menu=root/'assets/sd/menu/native_menu_sharp.pack';menu.parent.mkdir(parents=True);menu.write_bytes(fake_pack(b'\x11'*32))
            staged=root/'build/sd/menu/native_menu_sharp.pack'
            identity.prepare(root,build);artifacts(root,build)
            with patch.object(identity,'checked_run',side_effect=tool):result=identity.finalize(root,build)
            self.assertFalse(result['sd_assets']['complete'])
            self.assertIn('sky/sun.565',result['sd_assets']['missing']);self.assertNotIn('menu/native_menu_sharp.pack',result['sd_assets']['missing'])
            output=package_build.package(root,build)
            self.assertEqual(sha256(output/'sd/sky/earth.565'),sha256(earth))
            self.assertEqual(sha256(output/'sd/menu/native_menu_sharp.pack'),sha256(menu))
            self.assertEqual(staged.read_bytes(),menu.read_bytes()) # prepare staged the tracked pack where sdsync reads it
            ident=root/'nos/native_menu_assets_id.h';ident.parent.mkdir(exist_ok=True);ident.write_text('#define NATIVE_MENU_BUILD_SHA256 "'+'22'*32+'"\n')
            with self.assertRaisesRegex(ValueError,'stale for this source tree'):identity.prepare(root,build)
            ident.write_text('#define NATIVE_MENU_BUILD_SHA256 "'+'11'*32+'"\n');identity.prepare(root,build)
            menu.write_bytes(fake_pack(b'\x11'*32,b'other')) # a re-exported tracked pack replaces a stale staged copy
            identity.prepare(root,build);self.assertEqual(staged.read_bytes(),menu.read_bytes())
            earth.write_bytes(b'incomplete')
            with self.assertRaisesRegex(ValueError,'invalid size'):identity.prepare(root,build)

if __name__=='__main__':unittest.main()
