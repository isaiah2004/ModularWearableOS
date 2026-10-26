"""Compile-only ESP32-S3 size accounting; never links firmware or uses a device."""
from pathlib import Path
import json
import subprocess
import review

root, out = review.ROOT, review.OUT
compilers = list((Path.home()/'.espressif/tools/xtensa-esp-elf').rglob('xtensa-esp32s3-elf-gcc.exe'))
if not compilers:
    raise SystemExit('ESP32-S3 compiler not found under ~/.espressif/tools')
gcc = compilers[-1].with_name('xtensa-esp-elf-gcc.exe')
config = gcc.parent.parent/'lib/xtensa_esp32s3.so'
nm = gcc.with_name('xtensa-esp-elf-nm.exe')
result = {}
for kind in ['before', 'after']:
    source = root if kind == 'after' else out/'baseline-source'
    includes = f'#include "{(source / "nos/faces/face_orrery.c").as_posix()}"\n'
    fields = dict(list='sizeof(orr_list)', state='sizeof *s_orr')
    if kind == 'after':
        # ui_arc first: face capture aliases must not affect its implementation.
        includes = f'#include "{(root / "nos/ui_arc.c").as_posix()}"\n' + includes
        fields.update(glyphs='sizeof(arc_mask)*ARC_SLOTS', bands='sizeof *orr_band')
    probe = out/f'memory_{kind}.c'
    probe.write_text(includes + '\n'.join(f'char probe_{key}[{value}];' for key, value in fields.items())+'\n')
    obj = probe.with_suffix('.o')
    review.command([gcc, f'-mdynconfig={config}', '-Os', '-std=c99', '-Wall', '-Wextra', '-Wno-unused-parameter',
                    *(f'-I{source/p}' for p in ['core','nos','nos/fonts']),
                    '-c', probe, '-o', obj], out/f'memory_{kind}_build.log')
    symbols = subprocess.check_output([str(nm), '-S', '--radix=d', str(obj)], text=True)
    result[kind] = {line.split()[3][6:]: int(line.split()[1]) for line in symbols.splitlines()
                    if len(line.split()) == 4 and line.split()[3].startswith('probe_')}
result['added_bytes'] = (sum(result['after'].values()) - sum(result['before'].values()))
(out/'memory.json').write_text(json.dumps(result, indent=2)+'\n')
print(json.dumps(result, indent=2))
