"""Checked checkpoint completion and real partition/range gates, no serial I/O."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
import zipfile
from unittest.mock import patch
from tools import flash_watch, flash_layout
from tools.build_identity import REQUIRED_FEATURES, source_manifest
from tools.host_transport import HostTransport
from tests.test_host_transport import Link
from tests.test_backup_flash import BACKUP

CHECKS = Path(__file__).resolve().parents[1] / 'build/alpha05/checks/boot-recovery/tests'
CHECKS.mkdir(parents=True, exist_ok=True)


def temporary():
    return tempfile.TemporaryDirectory(prefix='flash-', dir=CHECKS)

def partition(kind, subtype, offset, size, label):
    return struct.pack('<HBBII16sI', 0x50aa, kind, subtype, offset, size, label.encode(), 0)

def layout(directory, *, app_size=32, app_offset=0x20000):
    (directory / 'partition.bin').write_bytes(partition(1, 2, 0x9000, 0x6000, 'nvs') +
        partition(1, 0, 0x10000, 0x2000, 'otadata') + partition(0, 0x10, 0x20000, 0x780000, 'ota_0') + b'\xff' * 32)
    (directory / 'boot.bin').write_bytes(b'boot')
    (directory / 'app.bin').write_bytes(b'x' * app_size)
    (directory / 'flash_args').write_text('--flash_mode dio --flash_size 16MB --flash_freq 80m\n'
        f'0x0 boot.bin\n0x8000 partition.bin\n{app_offset:#x} app.bin\n')

class CheckedFlashTests(unittest.TestCase):
    def test_c_board_8mb_limit_and_nvs_are_preserved(self):
        with temporary() as name:
            directory = Path(name)
            layout(directory)
            args = directory / 'flash_args'
            args.write_text(args.read_text().replace('16MB', '8MB'))
            self.assertEqual(flash_layout.checked_layout(directory)[2]['flash_bytes'], 0x800000)
            (directory / 'partition.bin').write_bytes(partition(1, 2, 0x9000, 0x6000, 'nvs') +
                partition(0, 0, 0x20000, 0x800000, 'factory') + b'\xff' * 32)
            with self.assertRaisesRegex(ValueError, 'configured flash'):
                flash_layout.checked_layout(directory)

    def test_legacy_backup_cannot_bypass_nos_checkpoint_for_b_board(self):
        with temporary() as name:
            path = Path(name) / 'flash.json'; path.write_text('{}')
            with self.assertRaisesRegex(ValueError, 'C-board build'):
                flash_watch.legacy_c_backup(path, {'budget': {'flash_bytes': 0x1000000, 'app_offset': 0x20000}}, root=path.parent)

    def test_checked_checkpoint_requires_ack_and_done(self):
        valid = 'NOSPREFS ok=1 stored=1 match=1'
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(flash_watch.checkpoint('unused', transport=HostTransport(Link([valid, 'DONE']))), valid)
        for line in ('NOSPREFS ok=0 stored=0 match=0', 'NOSPREFS ok=1 stored=1 match=0'):
            with self.assertRaises(RuntimeError):
                flash_watch.checkpoint('unused', transport=HostTransport(Link([line, 'DONE'])))
        self.assertEqual(HostTransport(Link()).correlated, False)

    def test_correlated_checkpoint_ignores_stale_save_ack(self):
        transport = HostTransport(Link(['NOSPREFS ok=1 stored=1 match=1', 'DONE', 'BEGIN 12345678',
                                        'NOSPREFS ok=0 stored=0 match=0', 'DONE 12345678 ok=1']))
        transport.correlated = True
        with patch('tools.host_transport.secrets.token_hex', return_value='12345678'), self.assertRaises(RuntimeError):
            flash_watch.checkpoint('unused', transport=transport)

    def test_partition_headroom_uses_actual_slot_and_app_offset(self):
        with temporary() as name:
            directory = Path(name); layout(directory)
            _, segments, budget = flash_layout.checked_layout(directory)
            self.assertEqual(budget['app_offset'], 0x20000)
            self.assertEqual(budget['app_headroom_bytes'], 0x780000 - 32)
            self.assertTrue(budget['nvs_preserved'])
            layout(directory, app_offset=0x10000)
            with self.assertRaises(ValueError): flash_layout.checked_layout(directory)
            layout(directory, app_size=0x780001)
            with self.assertRaises(ValueError): flash_layout.checked_layout(directory)

    def test_nvs_overlap_unknown_flags_and_path_escape_rejected(self):
        with temporary() as name:
            directory = Path(name); (directory / 'image.bin').write_bytes(b'x' * 256)
            for args in ('0x9000 image.bin', '0x8f80 image.bin', '--erase-all 0x0 image.bin',
                         '0x0 ../image.bin', '0x0 image.bin 0x80 image.bin', ''):
                with self.subTest(args=args):
                    (directory / 'flash_args').write_text(args)
                    with self.assertRaises((ValueError, OSError)): flash_watch.verify_ranges(directory)

    def test_failed_preflight_never_opens_serial_or_calls_esptool(self):
        with patch.object(flash_watch, 'verify', side_effect=ValueError('no identity')), \
             patch.object(flash_watch, 'checkpoint') as save, patch.object(flash_watch.subprocess, 'run') as flash, \
             contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(flash_watch.main(['--port', 'UNUSED']), 1)
            save.assert_not_called(); flash.assert_not_called()


def known_source(fixed=False):
    lines = ['/* binary receipt fixture */'] * 1665
    lines[1599] = 'void app_main(void)'
    lines[1600] = '{'
    lines[1601] = 'prefs_set_persistence_allowed(0);'
    lines[1602] = 'board_set_background_persistence(0);'
    lines[1658] = ('ok = xTaskCreateStaticPinnedToCore(cmd_task,"cmd",6144,NULL,4,stack,tcb,0);'
                   if fixed else 'ok = xTaskCreate(cmd_task,"cmd",6144,NULL,4,NULL);')
    lines[1659] = 'assert(ok == pdPASS);'
    lines[1660] = 'nos_init(NULL);'
    lines[1661] = 'nos_apps_boot();'
    lines[1662] = '}'
    return '\n'.join(lines) + '\n'


def signed_build(directory, root, *, fixed=False, app_bytes=288):
    directory.mkdir(parents=True)
    source = root / 'firmware/main/nos_main.c'
    source.parent.mkdir(parents=True, exist_ok=True)
    source.write_text(known_source(fixed), encoding='utf-8')
    manifest = source_manifest(root)
    elf = b'ELF fixture, not executable:' + manifest['tree_sha256'].encode()
    elf_hash = hashlib.sha256(elf).hexdigest()
    image = bytearray((i * 37 + (i // 65536) * 13) % 256 for i in range(app_bytes))
    image[:288] = bytes(288)  # Keep the real app descriptor strings zero terminated.
    image[0] = 0xe9
    struct.pack_into('<I', image, 32, 0xabcd5432)
    image[144:148] = b'test'
    image[176:208] = bytes.fromhex(elf_hash)
    layout(directory, app_size=len(image))
    (directory / 'app.bin').write_bytes(image)
    (directory / 'app.elf').write_bytes(elf)
    (directory / 'source-manifest.json').write_text(json.dumps(manifest), encoding='utf-8')
    with zipfile.ZipFile(directory / 'source.zip', 'w') as archive:
        archive.write(source, 'firmware/main/nos_main.c')
    _, segments, budget = flash_layout.checked_layout(directory)
    identity = {'schema': 1, 'workspace': str(root), 'source_tree_sha256': manifest['tree_sha256'],
                'source_manifest': 'source-manifest.json', 'source_archive': 'source.zip',
                'firmware_elf_sha256': elf_hash, 'segments': segments, 'budget': budget,
                'features': {name: {'linked': True} for name in REQUIRED_FEATURES},
                'files': [{'file': p.name, 'bytes': p.stat().st_size, 'sha256': flash_watch.sha256(p)}
                          for p in sorted(directory.iterdir()) if p.is_file()]}
    identity['bundle_id'] = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    (directory / 'build-identity.json').write_text(json.dumps(identity), encoding='utf-8')
    return identity


class RecoveryFixture:
    mac = '28:84:85:b0:8b:30'
    ack = ('NOSPREFS ok=1 stored=1 match=1 hash=6f530340 stored_hash=6f530340 '
           'face=8 brightness=62 dnd=0 wifi=1 ble=0 aod=0 sensor_idle=1')

    def __init__(self, root, *, correlated=False, app_bytes=288):
        self.root = root
        self.failed = root / 'failed'
        self.old = signed_build(self.failed, root, app_bytes=app_bytes)
        self.current = root / 'firmware/build'
        self.new = signed_build(self.current, root, fixed=True)
        self.path = root / 'receipt.json'
        self.flash_log = root / 'flash.log'
        log = [json.dumps({'bundle_id': self.old['bundle_id'], 'budget': self.old['budget'], 'dry_run': False}),
               self.ack, 'esptool.py v4.12', 'MAC: ' + self.mac]
        for item in self.old['segments']:
            begin = item['offset'] & ~4095
            end = ((item['offset'] + item['bytes'] + 4095) & ~4095) - 1
            log.append(f'Flash will be erased from {begin:#010x} to {end:#010x}...')
        for item in self.old['segments']:
            log += [f"Wrote {item['bytes']} bytes (42 compressed) at {item['offset']:#010x} in 0.1 seconds...",
                    'Hash of data verified.']
        log += ['Leaving...', 'Hard resetting via RTS pin...']
        self.flash_log.write_text('\n'.join(log) + '\n', encoding='utf-8')
        request = '1234abcd' if correlated else None
        lines = [('tx', 'hostcaps', None), ('rx', 'NOSHOST v=1 req=1' if correlated else 'ERR unknown: hostcaps', None),
                 ('rx', 'DONE', None), ('tx', f'req {request} prefs flush' if request else 'prefs flush', request)]
        if request:
            lines.append(('rx', 'BEGIN ' + request, request))
        lines += [('rx', self.ack, request), ('rx', 'DONE ' + request + ' ok=1' if request else 'DONE', request)]
        self.flush_wire = root / 'flush.jsonl'
        self.wire(self.flush_wire, lines)
        self.boot = root / 'boot.log'
        attempt = ('ESP-ROM:esp32s3-20210327\nI (1582) main_task: Calling app_main()\n' +
                   flash_watch.RECOVERY_ASSERT + '\nBacktrace: 0x40380001:0x3fcc0000 0x42010001:0x3fcc1000\n' +
                   'ELF file SHA256: ' + self.old['firmware_elf_sha256'] + '\nRebooting...\n')
        self.boot.write_text(attempt * 2, encoding='utf-8')
        self.boot_wire = root / 'boot.jsonl'
        self.wire(self.boot_wire, [('tx', 'hostcaps', None), ('rx', 'Rebooting...', None)])
        self.nvs = root / 'nvs.bin'
        self.nvs.write_bytes(bytes(range(256)) * 96)
        self.nvs_log = root / 'nvs.log'
        self.nvs_log.write_text('MAC: ' + self.mac + '\nRead 24576 bytes at 0x00009000 in 2.1 seconds...\nStaying in bootloader.\n', encoding='utf-8')
        self.backup = root / 'backup'
        self.backup.mkdir()
        for name, data in [('watch-settings.0.bin', b'bank0'), ('watch-settings.1.bin', b'bank1'),
                           ('imu-intent.0.bin', self.intent(8, 1)), ('imu-intent.1.bin', self.intent(9, 0))]:
            (self.backup / name).write_bytes(data)
        self.backup_manifest = self.backup / 'files.json'
        self.refresh_backup()
        self.receipt = {'schema': 1, 'kind': flash_watch.RECOVERY_KIND, 'workspace': str(root), 'device_mac': self.mac,
            'failed_build': {'directory': 'failed', 'bundle_id': self.old['bundle_id'],
                'source_tree_sha256': self.old['source_tree_sha256'], 'firmware_elf_sha256': self.old['firmware_elf_sha256'],
                'identity_sha256': flash_watch.sha256(self.failed / 'build-identity.json')},
            'checkpoint': {'flash_log': self.record(self.flash_log), 'wire_log': self.record(self.flush_wire), 'ack': self.ack},
            'boot_capture': {'raw': self.record(self.boot), 'wire': self.record(self.boot_wire)},
            'nvs': {'offset': 0x9000, 'bytes': 0x6000, 'snapshot': self.record(self.nvs), 'read_log': self.record(self.nvs_log)},
            'sd_backup': {'directory': 'backup', 'manifest': self.record(self.backup_manifest)}}
        self.save()

    @staticmethod
    def wire(path, lines):
        path.write_text(''.join(json.dumps({'monotonic': i, 'direction': direction, 'line': line, 'request_id': request}) + '\n'
                                for i, (direction, line, request) in enumerate(lines)), encoding='utf-8')

    @staticmethod
    def intent(seq, active):
        data = struct.pack('<5I', 0x494d5532, seq, active, 100, 9)
        checksum = 2166136261
        for byte in data:
            checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
        return data + struct.pack('<I', checksum)

    def refresh_backup(self):
        records = [{'path': name, 'bytes': (self.backup / name.replace('/', '-')).stat().st_size,
                    'sha256': flash_watch.sha256(self.backup / name.replace('/', '-'))}
                   for name in ('watch/settings.0.bin', 'watch/settings.1.bin', 'imu/intent.0.bin', 'imu/intent.1.bin')]
        self.backup_manifest.write_text(json.dumps(records), encoding='utf-8')

    def record(self, path):
        return {'path': path.relative_to(self.root).as_posix(), 'bytes': path.stat().st_size, 'sha256': flash_watch.sha256(path)}

    def save(self):
        self.path.write_text(json.dumps(self.receipt), encoding='utf-8')

    def commands(self, fault=None):
        self.rig = SDKRig(self, fault)
        flash_watch._recovery_sdk.side_effect = None
        flash_watch._recovery_sdk.return_value = self.rig.sdk()
        return self.rig.execute, self.rig.calls


class SDKFault(Exception):
    pass


class SDKRig:
    """Strict SDK adapter; actual backup_flash packet helper supplies protocol."""
    def __init__(self, fixture, fault=None):
        self.f, self.fault = fixture, fault
        self.calls, self.reads, self.events, self.acks = [], [], [], []
        self.attempts, self.connections, self.live = {}, 0, 0
        self.nvs_reads = 0
        self.stub_running = fault in ('stale-stub', 'forced-download', 'wrong-usb')
        self.soft_fault = fault in ('forced-download', 'wrong-usb')
        self.changed_mac = False
        self.dead = False
        self.ports = []

    def sdk(self):
        rig = self
        class Port:
            def __init__(self):
                self.timeout, self.write_timeout = None, None
                self.closed = False
                rig.ports.append(self)
            def set_buffer_size(self, **kwargs):
                assert kwargs == dict(rx_size=1024*1024, tx_size=65536)
                rig.events.append(('buffer', kwargs))
            def close(self):
                assert not self.closed
                self.closed = True
                rig.live -= 1
                rig.events.append(('close',))
        class ROM:
            ESP_READ_FLASH = 0xD2
            def __init__(self, port, baud):
                assert port == 'UNUSED' and baud == 115200 and rig.live == 0
                rig.connections += 1
                rig.live += 1
                self._port = Port()
                self.sync_stub_detected = rig.stub_running
                self.data, self.position = b'', 0
                self.current = None
                rig.events.append(('open', rig.connections))
            def connect(self, **kwargs):
                assert kwargs == dict(mode='no_reset', attempts=3)
                rig.events.append(('connect', kwargs))
                print('connect stdout')
                if rig.fault in ('command-error', 'command-timeout'):
                    print('partial stdout')
                    print('error stderr', file=sys.stderr)
                    raise SDKFault('connection timeout' if rig.fault == 'command-timeout' else 'connection error')
            def read_mac(self):
                rig.events.append(('mac',))
                wrong = rig.fault == 'mac' or rig.changed_mac
                if rig.fault == 'chunk-mac' and len([r for r in rig.reads if r['offset'] >= 0x20000]) >= 1:
                    wrong = True
                return bytes.fromhex(('000000000000' if wrong else rig.f.mac.replace(':', '')))
            def run_stub(self):
                rig.events.append(('stub', self.sync_stub_detected))
                rig.stub_running = True
                return self
            def soft_reset(self, *, stay_in_bootloader):
                assert stay_in_bootloader is True
                rig.events.append(('rom-only', True))
                if rig.dead:
                    raise StopIteration('dead SLIP generator during cleanup')
                if rig.soft_fault:
                    raise SDKFault('pending read cannot accept soft reset')
                rig.stub_running = False
            def hard_reset(self):
                raise AssertionError('Application reset is forbidden in the preservation gate')
            def change_baud(self, baud):
                assert baud == 460800
                rig.events.append(('baud', baud))
            def flash_spi_attach(self, value):
                assert value == 0
                rig.events.append(('attach', value))
            def flash_set_parameters(self, value):
                assert value == 0x1000000
                rig.events.append(('flash-size', value))
            def check_command(self, label, opcode, data):
                assert label == 'read flash' and opcode == self.ESP_READ_FLASH
                offset, size, block, window = struct.unpack('<4I', data)
                assert block == 32 and window == 1
                app = (rig.f.failed / 'app.bin').read_bytes()
                is_app = 0x20000 <= offset < 0x20000 + len(app)
                rig.attempts[offset] = rig.attempts.get(offset, 0) + 1
                self.current = dict(offset=offset, bytes=size, block=block, window=window,
                                    attempt=rig.attempts[offset], session=rig.connections, app=is_app)
                rig.reads.append(self.current)
                if is_app:
                    assert size <= 65536 and offset + size <= 0x20000 + len(app)
                    self.data = app[offset-0x20000:offset-0x20000+size]
                else:
                    assert offset == 0x9000 and size == flash_watch.NVS_BYTES
                    rig.nvs_reads += 1
                    self.data = rig.f.nvs.read_bytes()
                corrupt = (rig.fault == 'app' and is_app or
                           rig.fault == 'chunk-hash' and offset == 0x30000 or
                           rig.fault == 'nvs-first' and not is_app and rig.nvs_reads == 1 or
                           rig.fault == 'nvs-second' and not is_app and rig.nvs_reads == 2)
                if corrupt:self.data = b'?' + self.data[1:]
                if rig.nvs_reads == 2 and not is_app:
                    if rig.fault == 'receipt-changed':rig.f.path.write_text(rig.f.path.read_text() + ' ')
                    if rig.fault == 'build-changed':
                        with (rig.f.current / 'app.bin').open('ab') as stream:stream.write(b'changed')
                self.position = 0
            def read(self):
                offset = self.current['offset']
                first = self.position == 0 and offset == 0x30000
                fault = rig.fault
                if first and (fault in ('chunk-error', 'chunk-timeout', 'chunk-error-mac') or
                              fault in ('chunk-retry', 'dead-slip') and self.current['attempt'] == 1):
                    print('partial chunk stdout')
                    print('chunk error stderr', file=sys.stderr)
                    if fault == 'chunk-error-mac':rig.changed_mac = True
                    if fault == 'dead-slip':rig.dead = True
                    raise SDKFault('packet timeout received223' if fault == 'chunk-timeout' else 'packet content corrupt')
                if first and fault == 'chunk-reset':raise ValueError('unexpected application reset')
                if self.position < len(self.data):
                    part = self.data[self.position:self.position+self.current['block']]
                    if first and fault == 'chunk-short':part = part[:-1]
                    self.position += len(part)
                    return part
                if fault == 'device-md5':return b'0'*16
                return hashlib.md5(self.data).digest()
            def write(self, data):
                assert struct.unpack('<I', data)[0] == self.position
                rig.acks.append(self.position)
        class USBDownload:
            def __init__(self, port):
                assert port in rig.ports and not port.closed
            def __call__(self):
                rig.events.append(('usb-download-ROM',))
                rig.stub_running = False
                rig.dead = False
        def comports():
            return [types.SimpleNamespace(device='UNUSED',vid=0x303A,pid=0 if rig.fault=='wrong-usb' else 0x1001)]
        return ROM, BACKUP.read_flash_bounded, SDKFault, USBDownload, comports

    def execute(self, args, **kwargs):
        self.calls.append(args)
        assert 'write_flash' in args, 'Preservation reads must use the SDK packet helper, not subprocess CLI'
        assert args[args.index('-b')+1] == '460800'
        assert args[args.index('--before')+1] == 'no_reset'
        assert args[args.index('--after')+1] == 'hard_reset'
        assert self.live == 0 and all(port.closed for port in self.ports)
        assert self.nvs_reads == 2
        return subprocess.CompletedProcess(args,0)


class BootRecoveryTests(unittest.TestCase):
    def setUp(self):
        self.sdk = self.enterContext(patch.object(flash_watch, '_recovery_sdk', side_effect=AssertionError('SDK opened unexpectedly')))

    def test_real_identity_archive_binary_and_both_checkpoint_protocols(self):
        for correlated in (False, True):
            with self.subTest(correlated=correlated), temporary() as name:
                f = RecoveryFixture(Path(name), correlated=correlated)
                proof = flash_watch.recovery_receipt(f.path, f.new, root=f.root)
                self.assertEqual(proof['panics'], 2)
                self.assertEqual(proof['app']['sha256'], flash_watch.sha256(f.failed / 'app.bin'))

    def test_dry_run_is_offline_and_package_recovery_is_refused(self):
        with temporary() as name:
            f = RecoveryFixture(Path(name))
            with patch.object(flash_watch, 'ROOT', f.root), patch.object(flash_watch, 'checkpoint') as save, \
                 patch.object(flash_watch.subprocess, 'run') as command, contextlib.redirect_stdout(io.StringIO()), \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(flash_watch.main(['--port', 'UNUSED', '--boot-recovery', str(f.path), '--dry-run']), 0)
                self.assertEqual(flash_watch.main(['--port', 'UNUSED', '--boot-recovery', str(f.path), '--package', str(f.current)]), 1)
                save.assert_not_called(); command.assert_not_called()
                self.sdk.assert_not_called()

    def test_read_only_rom_identity_and_stable_nvs_before_checked_write(self):
        with temporary() as name:
            f = RecoveryFixture(Path(name)); execute, calls = f.commands()
            with patch.object(flash_watch, 'ROOT', f.root), patch.object(flash_watch, 'checkpoint') as save, \
                 patch.object(flash_watch.subprocess, 'run', side_effect=execute), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(flash_watch.main(['--port', 'UNUSED', '--boot-recovery', str(f.path)]), 0)
            save.assert_not_called()
            self.assertEqual(len(calls), 1)
            self.assertEqual([(row['offset'],row['bytes']) for row in f.rig.reads], [(0x20000,288),(0x9000,24576),(0x9000,24576)])
            self.assertEqual(f.rig.connections, 1)
            self.assertTrue(all(port.closed for port in f.rig.ports))
            self.assertFalse(any(event[0] in ('rom-only','usb-download-ROM') for event in f.rig.events))
            self.assertEqual(calls[-1][calls[-1].index('--before') + 1], 'no_reset')
            self.assertEqual(calls[-1][calls[-1].index('--after') + 1], 'hard_reset')
            self.assertEqual(calls[-1][calls[-1].index('-b') + 1], '460800')
            at = calls[-1].index('write_flash')
            self.assertEqual(calls[-1][at + 1:], flash_layout.checked_layout(f.current)[0])

    def test_rom_mismatch_error_and_mid_gate_changes_never_write(self):
        for fault in ('mac', 'app', 'nvs-first', 'nvs-second', 'command-error', 'command-timeout', 'receipt-changed', 'build-changed'):
            with self.subTest(fault=fault), temporary() as name:
                f = RecoveryFixture(Path(name)); execute, calls = f.commands(fault)
                with patch.object(flash_watch, 'ROOT', f.root), patch.object(flash_watch, 'checkpoint') as save, \
                     patch.object(flash_watch.subprocess, 'run', side_effect=execute), contextlib.redirect_stdout(io.StringIO()), \
                     contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(flash_watch.main(['--port', 'UNUSED', '--boot-recovery', str(f.path)]), 1)
                save.assert_not_called()
                self.assertFalse(any('write_flash' in call for call in calls), calls)

    def test_full_application_is_concatenated_from_bounded_reads_including_short_tail(self):
        with temporary() as name:
            f = RecoveryFixture(Path(name), app_bytes=2 * 65536 + 193)
            proof = flash_watch.recovery_receipt(f.path, f.new, root=f.root)
            execute, calls = f.commands()
            with patch.object(flash_watch.subprocess, 'run', side_effect=execute):
                output = flash_watch.recovery_rom('UNUSED', proof, root=f.root)
            self.assertEqual((output / 'failed-app.bin').read_bytes(), (f.failed / 'app.bin').read_bytes())
            self.assertEqual(flash_watch.sha256(output / 'failed-app.bin'), proof['app']['sha256'])
            self.assertEqual([(r['offset'],r['bytes']) for r in f.rig.reads],
                             [(0x20000,65536),(0x30000,65536),(0x40000,193),(0x9000,24576),(0x9000,24576)])
            self.assertTrue(all(r['block']==32 and r['window']==1 for r in f.rig.reads))
            self.assertEqual(f.rig.connections,1)
            chunks = json.loads((output / 'app-chunks.json').read_text())
            self.assertEqual([record['offset'] for record in chunks], [0x20000,0x30000,0x40000])
            self.assertEqual([record['bytes'] for record in chunks], [65536,65536,193])
            for record in chunks:
                self.assertEqual(record['attempt'],1)
                self.assertTrue(record['device_md5_verified'])
                self.assertEqual(record['sha256'],flash_watch.sha256(output/record['file']))
                self.assertIn('device MD5 verified',(output/Path(record['file']).with_suffix('.log')).read_text())
            self.assertFalse(calls)

    def test_read_only_chunk_retries_preserve_each_failure_and_never_repeat_the_write(self):
        with temporary() as name:
            f = RecoveryFixture(Path(name),app_bytes=2*65536+193)
            execute,calls = f.commands('chunk-retry')
            with patch.object(flash_watch,'ROOT',f.root),patch.object(flash_watch,'checkpoint') as save, \
                 patch.object(flash_watch.subprocess,'run',side_effect=execute),contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(flash_watch.main(['--port','UNUSED','--boot-recovery',str(f.path)]),0)
            save.assert_not_called()
            repeated=[r for r in f.rig.reads if r['offset']==0x30000]
            self.assertEqual(len(repeated),2)
            self.assertNotEqual(repeated[0]['session'],repeated[1]['session'])
            output=next((f.root/'build/alpha05/checks/boot-recovery').glob('rom-verify-*'))
            failure=(output/'app-0001-attempt-1.log').read_text()
            self.assertIn('partial chunk stdout',failure)
            self.assertIn('chunk error stderr',failure)
            self.assertIn('[read refused]',failure)
            self.assertFalse((output/'app-0001-attempt-1.bin').exists())
            self.assertEqual(json.loads((output/'app-chunks.json').read_text())[1]['attempt'],2)
            self.assertEqual((output/'failed-app.bin').read_bytes(),(f.failed/'app.bin').read_bytes())
            self.assertEqual(sum('write_flash' in call for call in calls),1)
            self.assertEqual(f.rig.connections,2)
            self.assertEqual(sum(event[0]=='rom-only' for event in f.rig.events),1)

    def test_chunk_failure_timeout_and_short_read_exhaust_three_attempts_before_any_write(self):
        for fault in ('chunk-error','chunk-timeout','chunk-short','device-md5'):
            with self.subTest(fault=fault),temporary() as name:
                f=RecoveryFixture(Path(name),app_bytes=2*65536+193)
                execute,calls=f.commands(fault)
                with patch.object(flash_watch,'ROOT',f.root),patch.object(flash_watch.subprocess,'run',side_effect=execute), \
                     contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(flash_watch.main(['--port','UNUSED','--boot-recovery',str(f.path)]),1)
                expected=[0x20000]*3 if fault=='device-md5' else [0x20000]+[0x30000]*3
                self.assertEqual([r['offset'] for r in f.rig.reads],expected)
                self.assertFalse(calls)
                self.assertEqual(f.rig.live,0)
                output=next((f.root/'build/alpha05/checks/boot-recovery').glob('rom-verify-*'))
                self.assertEqual((output/'failed-app.bin').stat().st_size,0 if fault=='device-md5' else 65536)
                index=0 if fault=='device-md5' else 1
                for attempt in range(1,4):
                    log=(output/f'app-{index:04d}-attempt-{attempt}.log').read_text()
                    self.assertIn('[read refused]',log)
                    if fault=='chunk-short':self.assertIn('expected 32, received 31',log)
                    elif fault=='device-md5':self.assertIn('MD5 mismatch',log)
                    else:
                        self.assertIn('partial chunk stdout',log)
                        self.assertIn('stderr',log)

    def test_correct_length_corruption_wrong_mac_and_reset_still_fail_closed(self):
        for fault in ('chunk-hash','chunk-mac','chunk-error-mac','chunk-reset'):
            with self.subTest(fault=fault),temporary() as name:
                f=RecoveryFixture(Path(name),app_bytes=2*65536+193)
                execute,calls=f.commands(fault)
                with patch.object(flash_watch,'ROOT',f.root),patch.object(flash_watch.subprocess,'run',side_effect=execute), \
                     contextlib.redirect_stdout(io.StringIO()),contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(flash_watch.main(['--port','UNUSED','--boot-recovery',str(f.path)]),1)
                self.assertEqual(f.rig.nvs_reads,0)
                self.assertFalse(calls)
                self.assertEqual(sum(r['offset']==0x30000 for r in f.rig.reads),0 if fault=='chunk-mac' else 1)
                self.assertFalse(any(event[0]=='usb-download-ROM' for event in f.rig.events))
                if fault=='chunk-hash':
                    output=next((f.root/'build/alpha05/checks/boot-recovery').glob('rom-verify-*'))
                    self.assertEqual((output/'failed-app.bin').stat().st_size,2*65536+193)
                    self.assertNotEqual(flash_watch.sha256(output/'failed-app.bin'),flash_watch.sha256(f.failed/'app.bin'))

    def test_sdk_error_and_timeout_keep_stdout_stderr_even_before_first_chunk(self):
        for fault in ('command-error','command-timeout'):
            with self.subTest(fault=fault),temporary() as name:
                f=RecoveryFixture(Path(name));execute,calls=f.commands(fault)
                proof=flash_watch.recovery_receipt(f.path,f.new,root=f.root)
                with patch.object(flash_watch.subprocess,'run',side_effect=execute),self.assertRaises(RuntimeError):
                    flash_watch.recovery_rom('UNUSED',proof,root=f.root)
                logs=list((f.root/'build/alpha05/checks/boot-recovery').glob('rom-verify-*/mac-attempt-*.log'))
                self.assertEqual(len(logs),3)
                for log in logs:
                    self.assertIn('partial stdout',log.read_text())
                    self.assertIn('stderr',log.read_text())
                    self.assertIn('[read refused]',log.read_text())
                self.assertEqual(f.rig.connections,3)
                self.assertEqual(f.rig.live,0)
                self.assertFalse(calls)

    def test_write_failure_is_never_replayed_by_read_only_chunk_retry(self):
        with temporary() as name:
            f = RecoveryFixture(Path(name), app_bytes=65536 + 193)
            execute, calls = f.commands()
            def fail_write(args, **kwargs):
                if 'write_flash' in args:
                    calls.append(args)
                    raise subprocess.CalledProcessError(2, args)
                return execute(args, **kwargs)
            with patch.object(flash_watch, 'ROOT', f.root), patch.object(flash_watch.subprocess, 'run', side_effect=fail_write), \
                 contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(flash_watch.main(['--port', 'UNUSED', '--boot-recovery', str(f.path)]), 1)
            self.assertEqual(sum('write_flash' in call for call in calls), 1)

    def test_dead_slip_generator_during_cleanup_uses_download_rom_and_retries_once(self):
        with temporary() as name:
            f=RecoveryFixture(Path(name),app_bytes=2*65536+193)
            execute,calls=f.commands('dead-slip')
            with patch.object(flash_watch,'ROOT',f.root),patch.object(flash_watch.subprocess,'run',side_effect=execute), \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(flash_watch.main(['--port','UNUSED','--boot-recovery',str(f.path)]),0)
            repeated=[row for row in f.rig.reads if row['offset']==0x30000]
            self.assertEqual(len(repeated),2)
            self.assertEqual([row['attempt'] for row in repeated],[1,2])
            self.assertNotEqual(repeated[0]['session'],repeated[1]['session'])
            self.assertEqual(sum(event[0]=='usb-download-ROM' for event in f.rig.events),1)
            self.assertEqual(sum(event[0]=='rom-only' for event in f.rig.events),1)
            output=next((f.root/'build/alpha05/checks/boot-recovery').glob('rom-verify-*'))
            log=(output/'app-0001-attempt-1.log').read_text()
            self.assertIn('packet content corrupt',log)
            self.assertIn('StopIteration',log)
            self.assertIn('Forcing native USB-JTAG download ROM',log)
            self.assertEqual((output/'failed-app.bin').read_bytes(),(f.failed/'app.bin').read_bytes())
            self.assertEqual(len(calls),1)
            self.assertEqual(f.rig.live,0)

    def test_stale_stub_is_returned_to_fresh_rom_before_any_read(self):
        for fault in ('stale-stub','forced-download','wrong-usb'):
            with self.subTest(fault=fault),temporary() as name:
                f=RecoveryFixture(Path(name));execute,calls=f.commands(fault)
                proof=flash_watch.recovery_receipt(f.path,f.new,root=f.root)
                with patch.object(flash_watch.subprocess,'run',side_effect=execute):
                    if fault=='wrong-usb':
                        with self.assertRaisesRegex(ValueError,'native USB-JTAG'):
                            flash_watch.recovery_rom('UNUSED',proof,root=f.root)
                        self.assertFalse(f.rig.reads)
                        self.assertFalse(any(event[0]=='usb-download-ROM' for event in f.rig.events))
                    else:
                        flash_watch.recovery_rom('UNUSED',proof,root=f.root)
                        self.assertEqual(f.rig.connections,2)
                        self.assertTrue(all(row['session']==2 for row in f.rig.reads))
                        self.assertEqual([event for event in f.rig.events if event[0]=='stub'],[('stub',True),('stub',False)])
                        self.assertEqual(sum(event[0]=='usb-download-ROM' for event in f.rig.events),int(fault=='forced-download'))
                self.assertEqual(f.rig.live,0)
                self.assertFalse(calls)

    def test_per_chunk_deadline_is_bounded_and_refuses_before_packet_io(self):
        with temporary() as name:
            f=RecoveryFixture(Path(name));execute,calls=f.commands()
            proof=flash_watch.recovery_receipt(f.path,f.new,root=f.root)
            with patch.object(flash_watch.subprocess,'run',side_effect=execute), \
                 patch('time.monotonic',side_effect=[0,241]*3),self.assertRaises(RuntimeError):
                flash_watch.recovery_rom('UNUSED',proof,root=f.root)
            self.assertFalse(f.rig.reads)
            self.assertEqual(f.rig.connections,3)
            self.assertEqual(f.rig.live,0)
            self.assertFalse(calls)

    def test_evidence_unknown_mutation_ready_panic_and_integrity_guards(self):
        faults = ('missing-done', 'later-command', 'duplicate-begin', 'accepted-after-checkpoint', 'ready', 'other-panic', 'one-boot', 'wrong-elf',
                  'active-intent', 'sd-backup', 'nvs-log-reset', 'wrong-nvs-range', 'failed-identity', 'failed-app', 'ack', 'unknown-field')
        for fault in faults:
            with self.subTest(fault=fault), temporary() as name:
                f = RecoveryFixture(Path(name), correlated=fault == 'duplicate-begin')
                if fault == 'missing-done':
                    f.flush_wire.write_text('\n'.join(f.flush_wire.read_text().splitlines()[:-1]) + '\n')
                    f.receipt['checkpoint']['wire_log'] = f.record(f.flush_wire)
                elif fault == 'later-command':
                    with f.flush_wire.open('a') as stream:
                        stream.write(json.dumps({'monotonic': 99, 'direction': 'tx', 'line': 'bri 20', 'request_id': None}) + '\n')
                    f.receipt['checkpoint']['wire_log'] = f.record(f.flush_wire)
                elif fault == 'duplicate-begin':
                    rows = [json.loads(line) for line in f.flush_wire.read_text().splitlines()]
                    rows.insert(5, dict(rows[4]))
                    f.wire(f.flush_wire, [(row['direction'], row['line'], row['request_id']) for row in rows])
                    f.receipt['checkpoint']['wire_log'] = f.record(f.flush_wire)
                elif fault == 'accepted-after-checkpoint':
                    f.wire(f.boot_wire, [('tx', 'hostcaps', None), ('rx', 'NOSHOST v=1 req=1', None), ('rx', 'DONE', None)])
                    f.receipt['boot_capture']['wire'] = f.record(f.boot_wire)
                elif fault in ('ready', 'other-panic', 'one-boot', 'wrong-elf'):
                    text = f.boot.read_text()
                    if fault == 'ready':text += 'READY,{"os":"NOS"}\n'
                    elif fault == 'other-panic':text = text.replace('nos_main.c:1660', 'nos_main.c:1650')
                    elif fault == 'one-boot':text = text[:len(text)//2]
                    else:text = text.replace(f.old['firmware_elf_sha256'], '0' * 64)
                    f.boot.write_text(text);f.receipt['boot_capture']['raw'] = f.record(f.boot)
                elif fault == 'active-intent':
                    (f.backup / 'imu-intent.1.bin').write_bytes(f.intent(9, 1));f.refresh_backup()
                    f.receipt['sd_backup']['manifest'] = f.record(f.backup_manifest)
                elif fault == 'sd-backup':(f.backup / 'watch-settings.0.bin').write_bytes(b'changed')
                elif fault == 'nvs-log-reset':
                    f.nvs_log.write_text(f.nvs_log.read_text() + 'Hard resetting via RTS pin...\n')
                    f.receipt['nvs']['read_log'] = f.record(f.nvs_log)
                elif fault == 'wrong-nvs-range':f.receipt['nvs']['offset'] = 0x8000
                elif fault == 'failed-identity':f.receipt['failed_build']['bundle_id'] = '0' * 64
                elif fault == 'failed-app':(f.failed / 'app.bin').write_bytes(b'bad')
                elif fault == 'ack':f.receipt['checkpoint']['ack'] = f.ack.replace('match=1', 'match=0')
                else:f.receipt['skip_checkpoint'] = True
                f.save()
                with self.assertRaises((OSError, ValueError)):
                    flash_watch.recovery_receipt(f.path, f.new, root=f.root)

    def test_known_failed_image_cannot_be_reflashed_and_duplicate_receipt_fields_fail(self):
        with temporary() as name:
            f = RecoveryFixture(Path(name))
            with self.assertRaises(ValueError):flash_watch.recovery_receipt(f.path, f.old, root=f.root)
            f.path.write_text(f.path.read_text().replace('"schema": 1', '"schema": 1, "schema": 1'))
            with self.assertRaises(ValueError):flash_watch.recovery_receipt(f.path, f.new, root=f.root)

    def test_fully_hashed_source_with_different_failure_site_is_refused(self):
        with temporary() as name:
            root = Path(name)
            identity = signed_build(root / 'different-failure', root, fixed=True)
            with self.assertRaises(ValueError):
                flash_watch._failed_source(root / 'different-failure', identity)

if __name__ == '__main__': unittest.main()
