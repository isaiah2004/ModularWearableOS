"""USB download framing survives arbitrary serial chunk and timeout boundaries."""
import base64
import contextlib
import importlib.util
import io
import pathlib
import unittest
import tempfile
from unittest.mock import patch
import zlib

SPEC = importlib.util.spec_from_file_location(
    "tools.sdsync", pathlib.Path(__file__).resolve().parents[1] / "tools/sdsync.py")
sdsync = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(sdsync)


class SerialChunks:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.sent = bytearray()
        self.timeout = 0

    @property
    def in_waiting(self):
        return len(self.chunks[0]) if self.chunks else 0

    def read(self, _n):
        return self.chunks.pop(0) if self.chunks else b""

    def write(self, data):
        self.sent.extend(data)


def usb(chunks):
    link = sdsync.UsbLink.__new__(sdsync.UsbLink)
    link.s = SerialChunks(chunks)
    link._rx = bytearray()
    return link


class DownloadTests(unittest.TestCase):
    def test_timeout_keeps_partial_line(self):
        link = usb([b"SDG Y"])
        with patch.object(sdsync.time, "monotonic", side_effect=[0, 0, 2]):
            self.assertEqual(link.readline(1), "")
        link.s.chunks += [b"WJj\r\nSDGET END\n"]
        self.assertEqual(link.readline(1), "SDG YWJj")
        self.assertEqual(link.readline(1), "SDGET END")

    def test_full_crc_download_with_split_lines_and_logs(self):
        data = bytes(range(256)) * 25
        wire = b"ERR unknown: sdfeatures\nDONE\n"
        wire += f"SDGET {len(data)} {zlib.crc32(data):08x}\n".encode()
        for offset in range(0, len(data), 1536):
            wire += b"I (123) unrelated log\nSDG " + base64.b64encode(data[offset:offset + 1536]) + b"\r\n"
        wire += b"SDGET END\n"
        link = usb([wire[i:i + 137] for i in range(0, len(wire), 137)])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sdsync.Card(link).get("watch/test.spt"), data)
        self.assertEqual(link.s.sent, b"sdfeatures\nsdget watch/test.spt\n")

    def test_corruption_is_not_silently_accepted(self):
        link = usb([b"SDGET 3 00000000\nSDG YWJj\nSDGET END\n"])
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(IOError):
            sdsync.Card(link)._get_legacy("watch/test.spt")

    def test_non_base64_rejected(self):
        link = usb([b"SDGET 3 00000000\nSDG YW!Jj\nSDGET END\n"])
        with self.assertRaises(ValueError):
            sdsync.Card(link)._get_legacy("watch/test.spt")

    def test_paced_recovers_bad_crc_bad_base64_and_duplicate(self):
        data = bytes(range(256)) * 7
        first, last = data[:1536], data[1536:]
        frame = lambda offset, chunk: (f"SDG2 {offset} {zlib.crc32(chunk):08x} ".encode()
                                       + base64.b64encode(chunk) + b"\n")
        wire = b"SDXFER v=2 paced_get=1\n"
        wire += f"SDGET2 {len(data)} {zlib.crc32(data):08x}\n".encode()
        wire += b"SDG2 0 00000000 YWJj\nSDG2 0 12345678 ###\n"
        wire += frame(1536, last)  # out of order, must retry at zero
        wire += frame(0, first) + frame(0, first)  # lost ACK
        wire += frame(1536, last) + b"SDGET2 END\n"
        link = usb([wire[i:i + 83] for i in range(0, len(wire), 83)])
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(sdsync.Card(link).get("watch/test.spt"), data)
        self.assertEqual(link.s.sent, b"sdfeatures\nsdget2 watch/test.spt\nSGR 0\nSGR 0\nSGR 0\nSGA 1536\nSGA 1536\nSGA 1792\n")

    def test_paced_rejects_truncation_or_wrong_whole_file_crc(self):
        for wire in (b"SDGET2 3 352441c2\nSDGET2 END\n",
                     b"SDGET2 3 00000000\nSDG2 0 352441c2 YWJj\nSDGET2 END\n"):
            with self.subTest(wire=wire), contextlib.redirect_stdout(io.StringIO()), self.assertRaises(IOError):
                sdsync.Card(usb([b"SDXFER v=2 paced_get=1\n" + wire])).get("watch/test.spt")

    def test_paced_empty_and_missing_files(self):
        self.assertEqual(sdsync.Card(usb([b"SDXFER v=2 paced_get=1\nSDGET2 0 00000000\nSDGET2 END\n"])).get("empty"), b"")
        with self.assertRaises(FileNotFoundError):
            sdsync.Card(usb([b"SDXFER v=2 paced_get=1\nSDGET2 MISSING\n"])).get("missing")


class GetRecoveryTests(unittest.TestCase):
    def test_partial_header_resync_then_verified_retry(self):
        wire=b"ERR command input reason=1 bytes=11 bad=0\nDONE\nERR unknown: hostcaps\nDONE\nSDGET2 3 352441c2\nSDG2 0 352441c2 YWJj\nSDGET2 END\n"
        link=usb([wire]);card=sdsync.Card(link);card.paced=True
        real_reply=card.reply;calls=0
        def reply(prefixes,timeout=10):
            nonlocal calls
            calls+=1
            if calls==1:
                link._rx.extend(b"SDGE")  # partial/bad reply remains buffered across timeout
                raise TimeoutError("header lost")
            return real_reply(prefixes,timeout)
        with patch.object(card,'reply',side_effect=reply),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(card.get('watch/x'),b'abc')
        self.assertEqual(link.s.sent,b'sdget2 watch/x\n\x00\nhostcaps\nsdget2 watch/x\nSGA 3\n')
        self.assertEqual(card.last_get['crc32'],'352441c2')

    def test_capability_probe_timeout_resync_retry(self):
        link=usb([b"ERR command input reason=1 bytes=11 bad=0\nDONE\nERR unknown: hostcaps\nDONE\nSDXFER v=2 paced_get=1\nSDGET2 0 00000000\nSDGET2 END\n"])
        card=sdsync.Card(link);real_reply=card.reply;calls=0
        def reply(prefixes,timeout=10):
            nonlocal calls
            calls+=1
            if calls==1:raise TimeoutError('probe lost')
            return real_reply(prefixes,timeout)
        with patch.object(card,'reply',side_effect=reply):self.assertEqual(card.get('x'),b'')
        self.assertEqual(link.s.sent,b'sdfeatures\n\x00\nhostcaps\nsdfeatures\nsdget2 x\n')

    def test_body_timeout_resync_retry(self):
        link=usb([b"ERR command input reason=1 bytes=1 bad=0\nDONE\nERR unknown: hostcaps\nDONE\nSDGET2 3 352441c2\nSDG2 0 352441c2 YWJj\nSDGET2 END\n"])
        card=sdsync.Card(link);card.paced=True;real_reply=card.reply;calls=0
        def reply(prefixes,timeout=10):
            nonlocal calls
            calls+=1
            if calls==1:return 'SDGET2 3 352441c2'
            if calls==2:raise TimeoutError('body lost')
            return real_reply(prefixes,timeout)
        with patch.object(card,'reply',side_effect=reply),contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(card.get('x'),b'abc')
        self.assertEqual(link.s.sent,b'sdget2 x\n\x00\nhostcaps\nsdget2 x\nSGA 3\n')

    def test_only_two_retries_and_no_legacy_or_ble_retry(self):
        card=sdsync.Card(usb([]));card.paced=True
        with patch.object(card,'_get_once',side_effect=TimeoutError('lost')) as get,patch.object(card,'_resync_get_usb') as sync:
            with self.assertRaises(TimeoutError):card.get('x')
            self.assertEqual(get.call_count,3);self.assertEqual(sync.call_count,2)
        card=sdsync.Card(usb([]));card.paced=False
        with patch.object(card,'_get_legacy',side_effect=TimeoutError('legacy body')) as get,patch.object(card,'_resync_get_usb') as sync:
            with self.assertRaises(TimeoutError):card.get('x')
            self.assertEqual(get.call_count,1);sync.assert_not_called()
        card=sdsync.Card(object());card.paced=True
        with patch.object(card,'_get_once',side_effect=TimeoutError('BLE')) as get,patch.object(card,'_resync_get_usb') as sync:
            with self.assertRaises(TimeoutError):card.get('x')
            self.assertEqual(get.call_count,1);sync.assert_not_called()

    def test_missing_is_not_retried(self):
        link=usb([b'SDGET2 MISSING\n']);card=sdsync.Card(link);card.paced=True
        with self.assertRaises(FileNotFoundError):card.get('x')
        self.assertEqual(link.s.sent,b'sdget2 x\n')

    def test_failed_pull_preserves_destination_and_creates_no_empty_file(self):
        class FailingCard:
            def ls(self,rel):return [(20,'x.bin')]
            def crc(self,rel):return None
            def get(self,rel):raise TimeoutError('failed before verification')
        with tempfile.TemporaryDirectory() as temporary:
            target=pathlib.Path(temporary)/'watch/x.bin';target.parent.mkdir();target.write_bytes(b'original')
            with self.assertRaises(TimeoutError):sdsync.pull(FailingCard(),'watch',temporary)
            self.assertEqual(target.read_bytes(),b'original')
            target.unlink()
            with self.assertRaises(TimeoutError):sdsync.pull(FailingCard(),'watch',temporary)
            self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
