"""Validate ESP-IDF segment files and partition limits before any serial/reset I/O."""
from __future__ import annotations
import json
from pathlib import Path
import re
import shlex
import struct
if __package__:
    from .host_support import contained, sha256
else:
    from host_support import contained, sha256

FLASH_BYTES = 16 * 1024 * 1024
NVS_RANGE = (0x9000, 0xF000)  # preserve the migration/card-absent fallback too
OPTIONS = {'--flash_mode': {'dio'}, '--flash_freq': {'80m'}, '--flash_size': {'8MB', '16MB'}}

def partitions(path, flash_bytes=FLASH_BYTES):
    data, result = Path(path).read_bytes(), []
    for offset in range(0, len(data) - 31, 32):
        magic, kind, subtype, start, size, label, flags = struct.unpack_from('<HBBII16sI', data, offset)
        if magic in (0xFFFF, 0xEBEB): break  # erased tail or ESP-IDF MD5 record
        if magic != 0x50AA: raise ValueError('Invalid partition table magic')
        if not size or start + size > flash_bytes: raise ValueError('Partition exceeds configured flash size')
        result.append({'type': kind, 'subtype': subtype, 'offset': start, 'bytes': size,
                       'name': label.rstrip(b'\0').decode('ascii'), 'flags': flags})
    if not result: raise ValueError('Empty partition table')
    ordered = sorted(result, key=lambda p: p['offset'])
    for left, right in zip(ordered, ordered[1:]):
        if left['offset'] + left['bytes'] > right['offset']: raise ValueError('Overlapping partitions')
    return result

def flash_args(directory):
    directory = Path(directory).resolve()
    path = directory / 'flash_args'
    if (directory / 'flasher_args.json').is_file():
        config = json.loads((directory / 'flasher_args.json').read_text(encoding='utf-8-sig'))
        if config.get('extra_esptool_args', {}).get('chip', 'esp32s3') != 'esp32s3':
            raise ValueError('Bundle targets a different chip')
        args = [*config['write_flash_args']]
        for offset, file in config['flash_files'].items(): args.extend((offset, file))
    else:
        args = shlex.split(path.read_text(encoding='utf-8-sig'))
    settings, segments = {}, []
    index = 0
    while index < len(args):
        token = args[index]
        if token in OPTIONS:
            if index + 1 >= len(args) or args[index + 1] not in OPTIONS[token] or token in settings:
                raise ValueError('Unsupported/duplicate flash setting: ' + token)
            settings[token] = args[index + 1]; index += 2
            continue
        if not re.fullmatch(r'0x[0-9a-fA-F]+', token) or index + 1 >= len(args):
            raise ValueError('Unsupported flash argument: ' + token)
        relative = args[index + 1]
        file = contained(directory, directory / relative)
        size, start = file.stat().st_size, int(token, 16)
        if size <= 0 or start + size > FLASH_BYTES: raise ValueError('Invalid flash segment size/range')
        if start < NVS_RANGE[1] and start + size > NVS_RANGE[0]:
            raise ValueError('Flash image overlaps saved NVS settings')
        segments.append({'offset': start, 'file': file.relative_to(directory).as_posix(),
                         'bytes': size, 'sha256': sha256(file)})
        index += 2
    if not segments: raise ValueError('Flash plan has no segments')
    flash_bytes = int(settings.get('--flash_size', '16MB')[:-2]) * 1024 * 1024
    if any(s['offset'] + s['bytes'] > flash_bytes for s in segments):
        raise ValueError('Flash segment exceeds configured flash size')
    ordered = sorted(segments, key=lambda s: s['offset'])
    for left, right in zip(ordered, ordered[1:]):
        if left['offset'] + left['bytes'] > right['offset']: raise ValueError('Overlapping flash segments')
    return args, segments

def checked_layout(directory, *, require_partitions=True):
    args, segments = flash_args(directory)
    table = next((s for s in segments if s['offset'] == 0x8000), None)
    if not table:
        if require_partitions: raise ValueError('Flash plan omits the partition table')
        return args, segments, {}
    flash_bytes = int(args[args.index('--flash_size') + 1][:-2]) * 1024 * 1024 if '--flash_size' in args else FLASH_BYTES
    entries = partitions(Path(directory) / table['file'], flash_bytes)
    nvs = [p for p in entries if p['type'] == 1 and p['subtype'] == 2]
    if not nvs or any((p['offset'], p['offset'] + p['bytes']) != NVS_RANGE for p in nvs):
        raise ValueError('NVS partition layout changed; an explicit migration is required')
    for segment in segments:
        for partition in nvs:
            if segment['offset'] < partition['offset'] + partition['bytes'] and segment['offset'] + segment['bytes'] > partition['offset']:
                raise ValueError('Flash segment overlaps an NVS partition')
    apps = [p for p in entries if p['type'] == 0]
    flashed = [(p, s) for p in apps for s in segments if p['offset'] == s['offset']]
    if len(flashed) != 1: raise ValueError('Expected exactly one app image at a declared app partition offset')
    app, image = flashed[0]
    if image['bytes'] > app['bytes']: raise ValueError('Application image exceeds its partition')
    # The true flash-slot limit is the gate; headroom is reported in bytes.
    budget = {'flash_bytes': flash_bytes, 'app_partition': app['name'], 'app_offset': app['offset'],
              'app_limit_bytes': app['bytes'], 'app_bytes': image['bytes'],
              'app_headroom_bytes': app['bytes'] - image['bytes'], 'nvs_preserved': True}
    return args, segments, budget
