"""Correlation, rejection, redaction and mutation replay safety without devices."""
import collections
import errno
import unittest
from unittest.mock import patch
from tools import host_transport as host

class Link:
    def __init__(self, lines=(), on_send=None):
        self.lines, self.sent, self.raw = collections.deque(lines), [], []
        self.on_send = on_send
    def send(self, command):
        self.sent.append(command)
        if self.on_send: self.on_send(self, command)
    def write(self, data): self.raw.append(data)
    def readline(self, timeout): return self.lines.popleft() if self.lines else ''

class TransportTests(unittest.TestCase):
    def test_negotiate_legacy_requires_explicit_completed_rejection(self):
        transport = host.HostTransport(Link(['ERR unknown: hostcaps', 'DONE']))
        self.assertEqual(transport.negotiate(), {})
        self.assertIsNone(transport.active)
        self.assertFalse(transport.correlated)
        with self.assertRaises(host.ProtocolError):
            host.HostTransport(Link(['DONE'])).negotiate()
        with self.assertRaises(host.ProtocolError):
            host.HostTransport(Link(['ERR unknown: hostcaps'])).negotiate(timeout=.001)

    def test_ignores_stale_done_and_requires_matching_begin(self):
        link = Link(['NOSHOST v=1 req=1 max_command=191 typed_errors=1', 'DONE',
                     'DONE old ok=1', 'DONE', 'BOARD stale', 'BEGIN deadbeef',
                     'BEGIN 12345678', 'BOARD sd=1', 'DONE old ok=1', 'DONE 12345678 ok=1'])
        transport = host.HostTransport(link)
        transport.negotiate()
        with patch.object(host.secrets, 'token_hex', return_value='12345678'):
            self.assertEqual(transport.execute('board'), ['BOARD sd=1'])
        self.assertEqual(link.sent, ['hostcaps', 'req 12345678 board'])
        self.assertEqual(transport.last_result['id'], '12345678')

    def test_done_before_begin_and_failed_completion_are_not_success(self):
        for lines, error in ((['DONE 12345678 ok=1'], host.ProtocolError),
                             (['BEGIN 12345678', 'DONE 12345678 ok=0 code=EBUSY'], host.DeviceError)):
            transport = host.HostTransport(Link(lines)); transport.correlated = True
            with patch.object(host.secrets, 'token_hex', return_value='12345678'), self.assertRaises(error):
                transport.execute('sdrm watch/x', timeout=.01)

    def test_control_bytes_and_envelope_budget_rejected_before_write(self):
        for command in ('', ' home', 'home\nreset', 'home\rreset', 'sdls\0', '\x1b', 'x' * 192):
            link = Link()
            with self.assertRaises(ValueError): host.HostTransport(link).begin(command)
            self.assertEqual(link.sent, [])
        transport = host.HostTransport(Link()); transport.correlated = True
        with self.assertRaises(ValueError): transport.begin('x' * 179)
        self.assertEqual(transport.link.sent, [])

    def test_typed_busy_is_not_missing_or_retryable(self):
        transport = host.HostTransport(Link(['SDCRC ERR open 16', 'DONE']))
        with self.assertRaises(host.DeviceError) as failure:
            transport.execute('sdcrc watch/settings.0.bin', retries=2)
        self.assertEqual(failure.exception.errno, errno.EBUSY)
        self.assertEqual(failure.exception.phase, 'open')
        self.assertEqual(len(transport.link.sent), 1)

    def test_only_read_only_rejection_is_retried_after_resync(self):
        def reply(link, command):
            if len(link.sent) == 1: link.lines.extend(['ERR command input reason=1 bytes=5 bad=0', 'DONE'])
            elif command == 'hostcaps': link.lines.extend(['ERR unknown: hostcaps', 'DONE'])
            else: link.lines.extend(['BOARD sd=1', 'DONE'])
        link = Link(on_send=reply)
        transport = host.HostTransport(link)
        self.assertEqual(transport.execute('board', retries=1), ['BOARD sd=1'])
        self.assertEqual(link.raw, [b'\x00\n'])
        self.assertEqual(link.sent, ['board', 'hostcaps', 'board'])
        for command in ('prefs flush', 'sdrm x', 'sdput x 0', 'sdput! x 0', 'reset', 'home', 'workout start', 'time 1'):
            with self.subTest(command=command), self.assertRaises(ValueError):
                host.HostTransport(Link()).execute(command, retries=1)

    def test_mutation_timeout_is_ambiguous_and_never_replayed(self):
        transport = host.HostTransport(Link())
        with self.assertRaises(host.CommandTimeout) as failure:
            transport.execute('prefs flush', timeout=.001)
        self.assertTrue(failure.exception.ambiguous)
        self.assertEqual(transport.link.sent, ['prefs flush'])
        with self.assertRaises(host.ProtocolError): transport.begin('prefs flush')

    def test_diagnostics_redact_credentials_on_both_directions(self):
        records = []
        transport = host.HostTransport(Link(['SETUP active=1 ssid=NOS password=secret', 'DONE']), trace=records.append)
        transport.execute('wifi {"ssid":"NOS","password":"secret"}')
        self.assertNotIn('secret', str(records))
        self.assertIn('redacted', str(records))
        self.assertNotIn('secret', host.redact('ERR unknown: wifi {"password":"secret"}'))

    def test_resync_ignores_old_done_until_fresh_probe_payload(self):
        link = Link(['DONE', 'ERR command input reason=1 bytes=1 bad=0', 'DONE',
                     'ERR unknown: hostcaps', 'DONE', 'BOARD fresh', 'DONE'])
        transport = host.HostTransport(link)
        transport.resynchronize()
        self.assertEqual(transport.execute('board'), ['BOARD fresh'])

if __name__ == '__main__': unittest.main()
