"""Actual Card streaming uses negotiated IDs and typed SD errors, without devices."""
import base64
import collections
import contextlib
import errno
import io
import unittest
from unittest.mock import patch
import zlib
from tools import sdsync
from tools.host_transport import DeviceError

class Device:
    block, completion = 2048, True
    def __init__(self, *, restore=True, fail=None):
        self.lines, self.sent, self.writes = collections.deque(), [], []
        self.restore, self.fail, self.upload = restore, fail, None
    def send(self, command):
        self.sent.append(command)
        if command == 'hostcaps':
            self.lines.extend(['NOSHOST v=1 req=1 max_command=191 typed_errors=1', 'DONE']);return
        _, request_id, inner = command.split(' ',2)
        self.lines.append('BEGIN '+request_id)
        if inner=='sdfeatures':
            self.lines.extend(['SDXFER v=2 paced_get=1 typed_errors=1 safe_put=1 restore_put='+str(int(self.restore)),
                               'DONE '+request_id+' ok=1'])
        elif inner.startswith('sdget2 '):
            if inner.endswith(' missing'):
                self.lines.extend(['SDGET2 MISSING','DONE '+request_id+' ok=1']);return
            data=b'abc'; self.lines.extend(['SDGET2 3 352441c2', 'SDG2 0 352441c2 YWJj', 'SDGET2 END', 'DONE '+request_id+' ok=1'])
        elif inner.startswith(('sdput ', 'sdput! ')):
            if self.fail:
                self.lines.extend(['SDPUT ERR protected '+str(self.fail), 'DONE '+request_id+' ok=0 code=EBUSY'])
            else:
                self.upload=request_id;self.lines.append('SDPUT READY')
        elif inner.startswith('sdcrc '):
            if inner.endswith(' missing'):
                self.lines.extend(['SDCRC MISSING','DONE '+request_id+' ok=1']);return
            self.lines.extend(['SDCRC ERR open 5', 'DONE '+request_id+' ok=0 code=EIO'])
    def write(self, data):
        self.writes.append(data)
        if self.upload:
            self.lines.extend(['SDK '+str(len(data)), 'SDPUT OK %d %08x'%(len(data),zlib.crc32(data)),
                               'DONE '+self.upload+' ok=1']); self.upload=None
    def readline(self, timeout):return self.lines.popleft() if self.lines else ''

class ContractTests(unittest.TestCase):
    def test_paced_get_completes_outer_correlated_request(self):
        device=Device();card=sdsync.Card(device,negotiate=True)
        with contextlib.redirect_stdout(io.StringIO()):self.assertEqual(card.get('watch/x'),b'abc')
        self.assertIsNone(card.transport.active)
        self.assertEqual(card.features['typed_errors'],'1')
        self.assertIn(b'SGA 3\n',device.writes)
        self.assertTrue(all(command.startswith('req ') for command in device.sent[1:]))

    def test_old_usb_firmware_has_done_for_hostcaps_but_not_sd_commands(self):
        class Legacy(Device):
            def send(self, command):
                self.sent.append(command)
                if command == 'hostcaps': self.lines.extend(['ERR unknown: hostcaps', 'DONE'])
                elif command == 'sdfeatures': self.lines.append('SDXFER v=2 paced_get=1')
                elif command.startswith('sdget2 '):
                    self.lines.extend(['SDGET2 3 352441c2', 'SDG2 0 352441c2 YWJj', 'SDGET2 END'])
        device=Legacy();card=sdsync.Card(device,negotiate=True)
        with contextlib.redirect_stdout(io.StringIO()):self.assertEqual(card.get('watch/x'),b'abc')
        self.assertEqual(device.sent,['hostcaps','sdfeatures','sdget2 watch/x'])

    def test_restore_is_explicit_and_capability_gated(self):
        device=Device();card=sdsync.Card(device,negotiate=True)
        with contextlib.redirect_stdout(io.StringIO()):card.put('watch/settings.0.bin',b'abc',restore_protected=True)
        self.assertTrue(any(' sdput! watch/settings.0.bin 3' in command for command in device.sent))
        self.assertIsNone(card.transport.active)
        device=Device(restore=False);card=sdsync.Card(device,negotiate=True)
        with self.assertRaisesRegex(RuntimeError,'does not advertise'):card.put('watch/settings.0.bin',b'abc',restore_protected=True)
        self.assertFalse(any('sdput' in command for command in device.sent))

    def test_busy_upload_and_io_crc_are_typed_and_never_replayed(self):
        device=Device(fail=errno.EBUSY);card=sdsync.Card(device,negotiate=True)
        with self.assertRaises(DeviceError) as failure:card.put('watch/settings.0.bin',b'abc')
        self.assertEqual(failure.exception.errno,errno.EBUSY)
        self.assertEqual(sum(' sdput ' in command for command in device.sent),1)
        self.assertEqual(device.writes,[])
        device=Device();card=sdsync.Card(device,negotiate=True)
        with self.assertRaises(DeviceError) as failure:card.crc('watch/x')
        self.assertEqual(failure.exception.errno,errno.EIO)

    def test_missing_completes_as_typed_absence_without_poisoning_next_request(self):
        device=Device();card=sdsync.Card(device,negotiate=True)
        self.assertIsNone(card.crc('missing'))
        self.assertIsNone(card.transport.active)
        with self.assertRaises(FileNotFoundError):card.get('missing')
        self.assertIsNone(card.transport.active)

    def test_paths_and_pulled_entries_cannot_inject_commands_or_escape(self):
        for path in ('../escape','watch/../escape','watch/x\nreset','watch/x\0','A:/x','watch\\x'):
            with self.subTest(path=path),self.assertRaises(ValueError):sdsync.safe_path(path)
        class UnsafeCard:
            def ls(self,path):return [(3,'../escape')]
        with self.assertRaises(ValueError):sdsync.pull(UnsafeCard(),'watch','unused')

if __name__=='__main__':unittest.main()
