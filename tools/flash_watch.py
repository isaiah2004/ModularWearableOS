"""Checked NOS updater: validate identity/ranges, checkpoint SD settings, then flash.

Run with ESP-IDF Python. --dry-run performs file checks without serial imports,
opening a port or resetting a device. Boot recovery accepts only a verified
receipt for the known pre-READY command-stack failure, never a blind checkpoint
bypass. It verifies the parked ROM device and unchanged flash/NVS before writing.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import zipfile
if __package__:
    from .host_support import ROOT, contained, sha256
    from .host_transport import HostTransport, SerialLines, jsonl_trace
    from .flash_layout import checked_layout
    from .build_identity import verify, image_elf_identity
else:
    from host_support import ROOT, contained, sha256
    from host_transport import HostTransport, SerialLines, jsonl_trace
    from flash_layout import checked_layout
    from build_identity import verify, image_elf_identity

RECOVERY_KIND = 'nos-command-stack-before-ready-v1'
RECOVERY_ASSERT = 'assert failed: app_main nos_main.c:1660 (ok == pdPASS)'
NVS_OFFSET, NVS_BYTES = 0x9000, 0x6000


def _require(condition, message):
    if not condition:
        raise ValueError('Boot recovery refused: ' + message)


def _json(text):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            _require(key not in result, 'duplicate JSON field ' + key)
            result[key] = value
        return result
    return json.loads(text, object_pairs_hook=unique)


def _keys(value, expected):
    _require(isinstance(value, dict) and set(value) == set(expected), 'unknown or missing receipt fields')


def _relative(root, name):
    _require(isinstance(name, str) and name and not Path(name).is_absolute() and
             '..' not in Path(name).parts, 'evidence path must stay inside the workspace')
    return contained(root, root / name)


def _evidence(root, record):
    _keys(record, ('path', 'bytes', 'sha256'))
    _require(type(record['bytes']) is int and record['bytes'] > 0 and
             isinstance(record['sha256'], str) and re.fullmatch('[0-9a-f]{64}', record['sha256']),
             'invalid evidence size/hash')
    path = _relative(root, record['path'])
    _require(path.stat().st_size == record['bytes'] and sha256(path) == record['sha256'],
             'evidence changed: ' + record['path'])
    return path


def _mac(text, expected):
    found = {value.lower() for value in re.findall(r'\bMAC: ([0-9a-fA-F:]{17})\b', text)}
    _require(found == {expected}, 'device MAC missing or different')


def _trace(path):
    rows = [_json(line) for line in path.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    _require(bool(rows), 'empty wire evidence')
    previous = -math.inf
    for row in rows:
        _keys(row, ('monotonic', 'direction', 'line', 'request_id'))
        now = row['monotonic']
        _require(type(now) in (int, float) and math.isfinite(now) and now >= previous and
                 row['direction'] in ('tx', 'rx') and isinstance(row['line'], str), 'invalid wire ordering')
        previous = now
    return rows


def _checkpoint_wire(path, ack):
    rows = _trace(path)
    sent = [i for i, row in enumerate(rows) if row['direction'] == 'tx']
    _require(len(sent) == 2 and sent[0] == 0 and rows[0]['line'] == 'hostcaps',
             'checkpoint must contain only completed negotiation and final prefs flush')
    at = sent[1]
    negotiation = [row['line'] for row in rows[1:at]]
    _require(negotiation and negotiation[-1] == 'DONE' and
             all(row['request_id'] is None for row in rows[:at]) and
             not any(line.startswith(('NOSPREFS ', 'READY')) for line in negotiation),
             'incomplete checkpoint negotiation')
    legacy = 'ERR unknown: hostcaps' in negotiation or '? hostcaps' in negotiation
    capabilities = [line for line in negotiation if line.startswith('NOSHOST ')]
    if legacy:
        _require(not capabilities and rows[at]['line'] == 'prefs flush', 'ambiguous legacy checkpoint')
        request = None
        payload = rows[at + 1:]
        terminal = 'DONE'
    else:
        _require(len(capabilities) == 1 and {'v=1', 'req=1'} <= set(capabilities[0].split()),
                 'unsupported checkpoint negotiation')
        match = re.fullmatch(r'req ([0-9a-f]{8}) prefs flush', rows[at]['line'])
        _require(match is not None, 'missing correlated checkpoint request')
        request = match[1]
        payload = rows[at + 1:]
        _require(payload and payload[0]['line'] == 'BEGIN ' + request, 'missing checkpoint BEGIN')
        terminal = 'DONE ' + request + ' ok=1'
    _require(payload and payload[-1]['line'] == terminal and
             rows[at]['request_id'] == request and
             all(row['direction'] == 'rx' and row['request_id'] == request for row in payload),
             'missing checkpoint completion or later command')
    lines = [row['line'] for row in payload]
    _require([line for line in lines if line.startswith('NOSPREFS ')] == [ack] and
             sum(line.startswith('DONE') for line in lines) == 1 and
             sum(line.startswith('BEGIN') for line in lines) == (1 if request else 0) and
             not any(line.startswith(('ERR ', 'READY', 'NOSIDENTITY')) for line in lines),
             'checkpoint acknowledgment missing/conflicted')


def _flash_log(path, ack, identity, mac):
    lines = [line.strip() for line in path.read_text(encoding='utf-8-sig').splitlines() if line.strip()]
    _require(len(lines) > 2 and lines[1] == ack, 'checkpoint is not immediately before failed flash')
    header = _json(lines[0])
    _require(header == {'bundle_id': identity['bundle_id'], 'budget': identity['budget'], 'dry_run': False},
             'failed flash identity differs from receipt')
    text = '\n'.join(lines[2:])
    _mac(text, mac)
    _require(lines[-1] == 'Hard resetting via RTS pin...' and
             not re.search(r'(?im)\b(error|failed|timeout|traceback)\b|^(?:READY|NOSPREFS|NOSIDENTITY|DONE)', text),
             'failed flash did not complete cleanly or contains later app activity')
    wrote = [(int(size), int(offset, 16)) for size, offset in
             re.findall(r'Wrote (\d+) bytes .*? at (0x[0-9a-fA-F]+) in ', text)]
    expected = [(item['bytes'], item['offset']) for item in identity['segments']]
    _require(sorted(wrote) == sorted(expected) and text.count('Hash of data verified.') == len(expected),
             'failed flash segment verification incomplete')
    erased = [(int(a, 16), int(b, 16) + 1) for a, b in
              re.findall(r'Flash will be erased from (0x[0-9a-fA-F]+) to (0x[0-9a-fA-F]+)', text)]
    ranges = [(item['offset'] & ~4095, (item['offset'] + item['bytes'] + 4095) & ~4095)
              for item in identity['segments']]
    _require(sorted(erased) == sorted(ranges) and
             all(end <= NVS_OFFSET or begin >= NVS_OFFSET + NVS_BYTES for begin, end in erased),
             'failed flash erase ranges unproven or overlap NVS')


def _boot_evidence(raw, wire, elf_hash):
    text = raw.read_text(encoding='utf-8-sig')
    _require(not re.search(r'(?m)^.*(?:READY[, ]|NOS up\.|NOSIDENTITY|NOSPREFS|Guru Meditation|abort\(\))', text),
             'boot reached app readiness or has an unrelated panic')
    panics = list(re.finditer(r'^assert failed:.*$', text, re.M))
    _require(len(panics) >= 2 and all(match[0].strip() == RECOVERY_ASSERT for match in panics),
             'need repeated matching pre-READY command allocation panics')
    complete = 0
    previous = 0
    for panic in panics:
        end = text.find('ESP-ROM:', panic.end())
        block = text[panic.end():end if end >= 0 else len(text)]
        _require(re.findall(r'^ELF file SHA256: ([0-9a-f]{64})\s*$', block, re.M) == [elf_hash] and
                 re.search(r'^Backtrace: (?:0x[0-9a-f]+:0x[0-9a-f]+\s*){2,}', block, re.M) and
                 'Rebooting...' in block, 'panic ELF/backtrace/reboot evidence incomplete')
        prefix = text[previous:panic.start()]
        complete += 'ESP-ROM:' in prefix and 'main_task: Calling app_main()' in prefix
        previous = panic.end()
    _require(complete >= 2, 'need two complete distinct boot attempts')
    for row in _trace(wire):
        if row['direction'] == 'tx':
            _require(row['line'] == 'hostcaps' and row['request_id'] is None,
                     'post-checkpoint command attempt is not read-only negotiation')
        else:
            _require(not re.search(r'^(?:BEGIN|DONE|NOSHOST|NOSPREFS|READY|NOSIDENTITY)\b', row['line']),
                     'post-checkpoint app accepted a command or reached readiness')
    return len(panics)


def _failed_source(directory, identity):
    manifest = _json((directory / identity['source_manifest']).read_text(encoding='utf-8-sig'))
    digest = hashlib.sha256(json.dumps(manifest['files'], sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    _require(digest == manifest['tree_sha256'] == identity['source_tree_sha256'], 'failed source manifest differs')
    with zipfile.ZipFile(directory / identity['source_archive']) as archive:
        for record in manifest['files']:
            data = archive.read(record['path'])
            _require(len(data) == record['bytes'] and hashlib.sha256(data).hexdigest() == record['sha256'],
                     'failed source archive entry differs')
        source = archive.read('firmware/main/nos_main.c').decode('utf-8').splitlines()
    _require(len(source) > 1660 and re.search(r'\bok\s*=\s*xTaskCreate\(cmd_task,\s*"cmd",\s*6144,', source[1658]) and
             re.fullmatch(r'\s*assert\(ok\s*==\s*pdPASS\);\s*', source[1659]),
             'failed source is not the known command allocation assertion')
    before = '\n'.join(source[:1660])
    body = before[before.rfind('void app_main(void)'):]
    _require(before.count('xTaskCreate(cmd_task,') == 1 and 'void app_main(void)' in body and
             'prefs_set_persistence_allowed(0);' in body and 'board_set_background_persistence(0);' in body and
             not re.search(r'(?:nos_init|nos_apps_boot)\s*\(|(?:prefs_set_persistence_allowed|board_set_background_persistence)\(1\)', body),
             'failed app could have entered UI or enabled persistence before panic')


def _sd_backup(root, value):
    _keys(value, ('directory', 'manifest'))
    directory = _relative(root, value['directory'])
    manifest = _evidence(root, value['manifest'])
    _require(manifest.parent == directory, 'SD backup manifest is outside backup directory')
    records = _json(manifest.read_text(encoding='utf-8-sig'))
    expected = {'watch/settings.0.bin', 'watch/settings.1.bin', 'imu/intent.0.bin', 'imu/intent.1.bin'}
    _require(isinstance(records, list) and len(records) == 4 and {r['path'] for r in records} == expected,
             'both SD banks and both IMU intent records are required')
    intents = []
    for record in records:
        path = directory / record['path'].replace('/', '-')
        _require(path.stat().st_size == record['bytes'] and sha256(path) == record['sha256'], 'SD backup changed')
        if record['path'].startswith('imu/'):
            data = path.read_bytes()
            _require(len(data) == 24, 'invalid IMU intent backup')
            magic, seq, active, hz, file, checksum = struct.unpack('<6I', data)
            h = 2166136261
            for byte in data[:20]:
                h = ((h ^ byte) * 16777619) & 0xffffffff
            _require(magic == 0x494d5532 and active in (0, 1) and hz in (25, 50, 100) and file <= 10000 and
                     checksum == h, 'IMU intent checksum/metadata invalid')
            intents.append((seq, active))
    distance = (intents[1][0] - intents[0][0]) & 0xffffffff
    _require(distance not in (0, 0x80000000), 'ambiguous IMU intent generation')
    _require(intents[1 if distance < 0x80000000 else 0][1] == 0, 'last durable IMU intent was active')


def recovery_receipt(path, current, *, root=ROOT):
    """Pure offline proof gate; never opens/resets a device or accepts a timeout."""
    root = Path(root).resolve()
    path = contained(root, Path(path).resolve())
    receipt = _json(path.read_text(encoding='utf-8-sig'))
    _keys(receipt, ('schema', 'kind', 'workspace', 'device_mac', 'failed_build', 'checkpoint',
                    'boot_capture', 'nvs', 'sd_backup'))
    _require(type(receipt['schema']) is int and receipt['schema'] == 1 and receipt['kind'] == RECOVERY_KIND and
             Path(receipt['workspace']).resolve() == root and
             re.fullmatch(r'(?:[0-9a-f]{2}:){5}[0-9a-f]{2}', receipt['device_mac']), 'wrong recovery kind/workspace/MAC')
    failed = receipt['failed_build']
    _keys(failed, ('directory', 'bundle_id', 'source_tree_sha256', 'firmware_elf_sha256', 'identity_sha256'))
    directory = _relative(root, failed['directory'])
    _require(sha256(directory / 'build-identity.json') == failed['identity_sha256'], 'failed build identity changed')
    identity = verify(directory)
    for key in ('bundle_id', 'source_tree_sha256', 'firmware_elf_sha256'):
        _require(identity[key] == failed[key], 'failed identity mismatch: ' + key)
    _require(current['bundle_id'] != identity['bundle_id'] and
             current['firmware_elf_sha256'] != identity['firmware_elf_sha256'], 'cannot reflash the failed candidate')
    app = next(segment for segment in identity['segments'] if segment['offset'] == identity['budget']['app_offset'])
    elf = next(record for record in identity['files'] if record['sha256'] == identity['firmware_elf_sha256'])
    _require(image_elf_identity(directory / app['file'], directory / elf['file'])['firmware_elf_sha256'] == failed['firmware_elf_sha256'],
             'failed application does not match ELF')
    _failed_source(directory, identity)
    checkpoint = receipt['checkpoint']
    _keys(checkpoint, ('flash_log', 'wire_log', 'ack'))
    ack = checkpoint['ack']
    tokens = ack.split() if isinstance(ack, str) else []
    fields = [token.split('=', 1) for token in tokens[1:] if '=' in token]
    values = dict(fields)
    _require(tokens and tokens[0] == 'NOSPREFS' and len(fields) == len(values) == len(tokens) - 1 and
             all(values.get(key) == '1' for key in ('ok', 'stored', 'match')) and
             re.fullmatch('[0-9a-f]{8}', values.get('hash', '')) and values.get('stored_hash') == values['hash'],
             'invalid durable preference acknowledgment')
    _checkpoint_wire(_evidence(root, checkpoint['wire_log']), ack)
    _flash_log(_evidence(root, checkpoint['flash_log']), ack, identity, receipt['device_mac'])
    capture = receipt['boot_capture']
    _keys(capture, ('raw', 'wire'))
    panics = _boot_evidence(_evidence(root, capture['raw']), _evidence(root, capture['wire']), failed['firmware_elf_sha256'])
    nvs = receipt['nvs']
    _keys(nvs, ('offset', 'bytes', 'snapshot', 'read_log'))
    _require(type(nvs['offset']) is int and type(nvs['bytes']) is int and
             (nvs['offset'], nvs['bytes']) == (NVS_OFFSET, NVS_BYTES), 'NVS snapshot range changed')
    snapshot = _evidence(root, nvs['snapshot'])
    _require(snapshot.stat().st_size == NVS_BYTES, 'NVS snapshot size differs')
    read_log = _evidence(root, nvs['read_log']).read_text(encoding='utf-8-sig')
    _mac(read_log, receipt['device_mac'])
    _require(re.search(r'Read 24576 bytes at 0x00009000\b', read_log) and
             read_log.strip().endswith('Staying in bootloader.') and
             not re.search(r'(?i)hard resetting|\berror\b|\bfailed\b|\b(?:writing|wrote|erase|erasing|write_flash)\b', read_log),
             'snapshot was not a completed read-only ROM operation')
    _sd_backup(root, receipt['sd_backup'])
    return {'receipt': path, 'receipt_sha256': sha256(path), 'mac': receipt['device_mac'],
            'failed_identity': identity, 'app': app, 'nvs_sha256': sha256(snapshot), 'panics': panics}


def _recovery_sdk():
    """Keep SDK-only imports out of ordinary host preflight and dry runs."""
    try:
        from esptool.targets.esp32s3 import ESP32S3ROM
        from esptool.util import FatalError
        from esptool.reset import USBJTAGSerialReset
        from serial.tools.list_ports import comports
        if __package__:
            from .backup_flash import read_flash_bounded
        else:
            from backup_flash import read_flash_bounded
    except ImportError as exc:
        raise RuntimeError('Boot recovery requires the ESP-IDF Python esptool environment') from exc
    return ESP32S3ROM, read_flash_bounded, FatalError, USBJTAGSerialReset, comports


def recovery_rom(port, proof, *, root=ROOT):
    """MD5-checked packet reads in one parked SDK session, never an app reset."""
    import contextlib
    import time
    import traceback

    root = Path(root).resolve()
    directory = contained(root, root / 'build/alpha05/checks/boot-recovery')
    directory.mkdir(parents=True, exist_ok=True)
    out = Path(tempfile.mkdtemp(prefix='rom-verify-', dir=directory)).resolve()
    contained(directory, out)
    ROM, read_flash_bounded, SDKFailure, USBDownloadReset, comports = _recovery_sdk()
    block_size = 32  # Qualified full native-USB read; retain device MD5 per chunk.
    esp = None
    verified = False

    def close():
        nonlocal esp, verified
        owned, esp = esp, None
        verified = False
        if owned is not None:
            owned._port.close()  # Never toggle DTR/RTS or request an app reset.

    def mac():
        nonlocal verified
        found = ':'.join(f'{byte:02x}' for byte in esp.read_mac())
        print('MAC: ' + found)
        _require(found == proof['mac'], 'device MAC missing or different')
        verified = True

    def return_to_rom():
        try:
            esp.soft_reset(stay_in_bootloader=True)
            print('Returned previous stub to ROM only.')
        except (OSError, RuntimeError, StopIteration, SDKFailure):
            traceback.print_exc()
            devices = [device for device in comports() if device.device.casefold() == str(port).casefold()]
            _require(verified and len(devices) == 1 and
                     (devices[0].vid, devices[0].pid) == (0x303A, 0x1001),
                     'forced download requires the verified MAC and native USB-JTAG device')
            print('Forcing native USB-JTAG download ROM with IO0 asserted; never an app reset.')
            USBDownloadReset(esp._port)()

    def connect():
        nonlocal esp
        for fresh in (False, True):
            esp = ROM(port, baud=115200)
            esp._port.timeout = 3
            try:
                esp._port.set_buffer_size(rx_size=1024 * 1024, tx_size=65536)
            except (AttributeError, OSError, ValueError):
                print(f'RX buffer hint unavailable; bounded{block_size}-byte packets remain enforced.')
            esp.connect(mode='no_reset', attempts=3)
            mac()
            if not esp.sync_stub_detected:
                break
            _require(not fresh, 'could not return the previous stub to ROM')
            esp = esp.run_stub()
            return_to_rom()
            close()
        esp = esp.run_stub()
        esp.change_baud(460800)
        esp.flash_spi_attach(0)
        esp.flash_set_parameters(16 * 1024 * 1024)
        esp._port.timeout = 3
        esp._port.write_timeout = 10

    class TimedRead:
        def __init__(self):
            self.deadline = time.monotonic() + 240
            self.ESP_READ_FLASH = esp.ESP_READ_FLASH

        def check(self):
            if time.monotonic() >= self.deadline:
                raise TimeoutError('Bounded recovery read exceeded240s')

        def check_command(self, *args):
            self.check()
            return esp.check_command(*args)

        def read(self):
            self.check()
            return esp.read()

        def write(self, data):
            self.check()
            return esp.write(data)

    def stage(offset, size, name):
        for attempt in range(1, 4):
            label = f'{name}-attempt-{attempt}'
            with (out / (label + '.log')).open('w', encoding='utf-8') as log, \
                    contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
                try:
                    if esp is None:
                        connect()
                    mac()
                    if size is None:
                        return None, attempt, label
                    timed = TimedRead()
                    data = read_flash_bounded(timed, offset, size, 1, block_size)
                    timed.check()
                    if len(data) != size:
                        raise OSError(f'ROM read size differs: expected {size}, received {len(data)}')
                    print(f'Read {size} bytes at {offset:#010x}; block{block_size}/window1, device MD5 verified.')
                    print('Read complete; flasher stub remains parked, no app reset requested.')
                    return data, attempt, label
                except (OSError, RuntimeError, StopIteration, SDKFailure) as exc:
                    traceback.print_exc()
                    print('[read refused] ' + str(exc))
                    if esp is not None and verified:
                        try:
                            return_to_rom()
                        except (OSError, RuntimeError, StopIteration, SDKFailure):
                            traceback.print_exc()
                    close()
                    if attempt == 3:
                        raise RuntimeError('Recovery read exhausted3 attempts: ' + name) from exc
            time.sleep(0.05)

    try:
        stage(None, None, 'mac')
        app = out / 'failed-app.bin'
        chunks = []
        with app.open('wb') as image:
            for position in range(0, proof['app']['bytes'], 64 * 1024):
                size = min(64 * 1024, proof['app']['bytes'] - position)
                offset = proof['app']['offset'] + position
                data, attempt, label = stage(offset, size, f'app-{len(chunks):04d}')
                chunk = out / (label + '.bin')
                chunk.write_bytes(data)
                image.write(data)
                chunks.append({'offset': offset, 'bytes': size, 'attempt': attempt,
                               'file': chunk.name, 'sha256': hashlib.sha256(data).hexdigest(),
                               'block_size': block_size, 'max_inflight': 1, 'device_md5_verified': True})
                (out / 'app-chunks.json').write_text(json.dumps(chunks, indent=2) + '\n', encoding='utf-8')
        _require(app.stat().st_size == proof['app']['bytes'] and sha256(app) == proof['app']['sha256'],
                 'flashed application differs from failed candidate')
        for attempt in (1, 2):
            data, _, _ = stage(NVS_OFFSET, NVS_BYTES, f'nvs-{attempt}')
            nvs = out / f'nvs-{attempt}.bin'
            nvs.write_bytes(data)
            _require(len(data) == NVS_BYTES and sha256(nvs) == proof['nvs_sha256'],
                     'NVS differs from held-ROM snapshot')
        stage(None, None, 'mac-final')
        return out
    finally:
        close()


def checkpoint(port, *, transport=None, trace=None):
    link = None
    if transport is None:
        link = SerialLines.open(port)
        transport = HostTransport(link, trace=trace)
    try:
        if link is not None:
            transport.negotiate()
        lines = transport.execute('prefs flush', timeout=10)
        acknowledgments = [line for line in lines if line.startswith('NOSPREFS ')]
        if len(acknowledgments) != 1 or not all(field in acknowledgments[0].split()
                for field in ('ok=1', 'stored=1', 'match=1')):
            raise RuntimeError('Checked preference save did not succeed; flash stopped: ' + repr(acknowledgments))
        print(acknowledgments[0], flush=True)
        return acknowledgments[0]
    finally:
        if link is not None: link.close()


def verify_ranges(build):
    return checked_layout(build, require_partitions=False)[0]


def verify_package(directory):
    manifest = json.loads((directory / 'manifest.json').read_text(encoding='utf-8-sig'))
    for record in manifest['files']:
        file = contained(directory, directory / record['file'])
        if file.stat().st_size != record['bytes'] or sha256(file) != record['sha256']:
            raise ValueError('Package artifact changed: ' + record['file'])
    identity = verify(directory)
    if manifest['bundle_id'] != identity['bundle_id']:
        raise ValueError('Package/build identity mismatch')
    return identity


def legacy_c_backup(path, identity, *, root=ROOT):
    """Verify a complete legacy C-board backup before its one-time migration.

    That WOS runtime predates NOSPREFS. A verified backup plus a live ROM
    MAC/NVS comparison supplies the preservation proof; timeouts never do.
    """
    path = contained(root, Path(path).resolve())
    record = _json(path.read_text(encoding='utf-8-sig'))
    image = path.with_suffix('.bin')
    _require(identity['budget']['flash_bytes'] == 0x800000 and
             identity['budget']['app_offset'] == 0x10000, 'legacy migration requires the C-board build')
    _require(record.get('bytes') == 0x800000 and image.stat().st_size == 0x800000 and
             sha256(image) == record.get('sha256'), 'legacy full-flash backup missing or changed')
    _require(record.get('transport') == 'esptool stub MD5-verified reads' and
             re.fullmatch(r'(?:[0-9a-f]{2}:){5}[0-9a-f]{2}', record.get('mac', '')),
             'legacy backup lacks verified transport/device identity')
    data = image.read_bytes()
    position = 0
    for chunk in record['chunks']:
        size = chunk['bytes']
        _require(type(size) is int and size > 0 and chunk['offset'] == position and
                 hashlib.sha256(data[position:position + size]).hexdigest() == chunk['sha256'],
                 'legacy backup chunk missing or changed')
        position += size
    _require(position == len(data), 'legacy backup does not cover the complete flash')
    return record['mac'], data[:0x10000]


def verify_legacy_c_device(port, proof):
    from esptool.targets.esp32s3 import ESP32S3ROM
    if __package__:
        from .backup_flash import read_flash_bounded
    else:
        from backup_flash import read_flash_bounded
    esp = ESP32S3ROM(port, baud=115200)
    try:
        esp.connect(mode='no_reset')
        _require(':'.join(f'{b:02x}' for b in esp.read_mac()) == proof[0], 'legacy backup belongs to another device')
        esp = esp.run_stub()
        esp.flash_spi_attach(0)
        esp.flash_set_parameters(0x800000)
        esp._port.timeout = 5
        actual = read_flash_bounded(esp, 0, 0x10000, 1, 256)
        _require(actual == proof[1], 'legacy boot/partition/NVS changed after backup')
        print(json.dumps({'legacy_backup_verified': True, 'device_mac': proof[0],
                          'nvs_sha256': hashlib.sha256(actual[NVS_OFFSET:NVS_OFFSET + NVS_BYTES]).hexdigest()}), flush=True)
    finally:
        esp._port.close()  # hold ROM/stub until the checked write, without app writes


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--package', type=Path, help='Flash a complete immutable package')
    parser.add_argument('--build', type=Path, default=ROOT / 'firmware/build', help='Verified workspace build directory')
    parser.add_argument('--legacy-c-backup', type=Path, help='Complete MD5-verified C-board legacy flash backup manifest')
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--wire-log', type=Path, help='Append redacted checkpoint transport diagnostics')
    parser.add_argument('--boot-recovery', type=Path,
                        help='Verified receipt for the known pre-READY command-stack boot failure (workspace build only)')
    args = parser.parse_args(argv)
    build = args.package.resolve() if args.package else contained(ROOT, args.build.resolve())
    try:
        _require(not (args.boot_recovery and args.package), 'recovery requires the current workspace build')
        _require(not (args.legacy_c_backup and (args.boot_recovery or args.package)), 'legacy migration requires a workspace build')
        validate = (lambda: verify_package(build)) if args.package else (lambda: verify(build, root=ROOT))
        identity = validate()
        flash_arguments, _, budget = checked_layout(build)
        print(json.dumps({'bundle_id': identity['bundle_id'], 'budget': budget, 'dry_run': args.dry_run}))
        proof = recovery_receipt(args.boot_recovery, identity, root=ROOT) if args.boot_recovery else None
        legacy = legacy_c_backup(args.legacy_c_backup, identity) if args.legacy_c_backup else None
        if args.dry_run: return 0
        if proof is not None:
            result = recovery_rom(args.port, proof, root=ROOT)
            checked = recovery_receipt(args.boot_recovery, identity, root=ROOT)
            _require(checked['receipt_sha256'] == proof['receipt_sha256'], 'receipt changed during ROM verification')
            (result / 'verified.json').write_text(json.dumps({
                'receipt_sha256': proof['receipt_sha256'], 'device_mac': proof['mac'],
                'failed_bundle_id': proof['failed_identity']['bundle_id'],
                'fixed_bundle_id': identity['bundle_id'], 'nvs_sha256': proof['nvs_sha256'],
                'app_readback_matches': True, 'nvs_readbacks_match': 2}, indent=2) + '\n', encoding='utf-8')
            print(json.dumps({'boot_recovery_verified': True, 'evidence': str(result),
                              'receipt_sha256': proof['receipt_sha256']}), flush=True)
        elif legacy is not None:
            verify_legacy_c_device(args.port, legacy)
        else:
            checkpoint(args.port, trace=jsonl_trace(args.wire_log))  # never replay a missing completion
        if validate() != identity:
            raise ValueError('Build/source artifacts changed during the preservation gate; flash stopped')
        if checked_layout(build)[0] != flash_arguments:
            raise ValueError('Flash plan changed during the preservation gate; flash stopped')
        subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32s3', '--port', args.port,
                        '-b', '460800', '--before', 'no_reset' if proof or legacy else 'default_reset', '--after', 'hard_reset',
                        'write_flash', *flash_arguments], cwd=build, check=True)
        return 0
    except (OSError, ValueError, KeyError, TypeError, IndexError, AttributeError, StopIteration,
            RuntimeError, zipfile.BadZipFile, subprocess.SubprocessError) as exc:
        print(f'flash_watch: {exc}', file=sys.stderr); return 1

if __name__ == '__main__': raise SystemExit(main())
