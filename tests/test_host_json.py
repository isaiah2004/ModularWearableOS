"""Atomic host journal replacement: durable data, bounded Windows retries."""
import errno
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
from tools import host_support


def windows_error(code=5):
    error=PermissionError(errno.EACCES,'sharing/access fixture')
    error.winerror=code
    return error


class Clock:
    def __init__(self):self.now=0.;self.waits=[]
    def monotonic(self):return self.now
    def sleep(self,duration):self.waits.append(duration);self.now+=duration


class HostJsonTests(unittest.TestCase):
    def test_transient_windows_replace_retries_same_durable_temp_then_succeeds(self):
        for code in (5,32,33):
            with self.subTest(winerror=code),tempfile.TemporaryDirectory() as directory:
                destination=Path(directory)/'commands.json';original=b'{"old": true}\n';destination.write_bytes(original)
                value={'complete':'new journal','commands':[1,2,3]};clock=Clock();paths=[];events=[]
                real_replace=host_support.os.replace;real_fsync=host_support.os.fsync
                def sync(fd):events.append('fsync');real_fsync(fd)
                def replace(source,target):
                    paths.append(Path(source))
                    self.assertEqual(events,['fsync'])
                    self.assertEqual(destination.read_bytes(),original)
                    self.assertEqual(json.loads(Path(source).read_text()),value)
                    if len(paths)<=3:raise windows_error(code)
                    real_replace(source,target)
                with patch.object(host_support.os,'replace',side_effect=replace), \
                     patch.object(host_support.os,'fsync',side_effect=sync), \
                     patch.object(host_support.time,'monotonic',side_effect=clock.monotonic), \
                     patch.object(host_support.time,'sleep',side_effect=clock.sleep):
                    host_support.write_json(destination,value)
                self.assertEqual(json.loads(destination.read_text()),value)
                self.assertEqual(clock.waits,[.01,.02,.04])
                self.assertEqual(len(set(paths)),1);self.assertFalse(paths[0].exists())

    def test_exhausted_deadline_preserves_destination_and_durable_new_temp(self):
        with tempfile.TemporaryDirectory() as directory:
            destination=Path(directory)/'commands.json';original=b'{"old": true}\n';destination.write_bytes(original)
            clock=Clock();error=windows_error();paths=[];value={'new':'fully written'}
            def replace(source,target):
                paths.append(Path(source));self.assertEqual(destination.read_bytes(),original)
                self.assertEqual(json.loads(Path(source).read_text()),value);raise error
            with patch.object(host_support.os,'replace',side_effect=replace), \
                 patch.object(host_support.time,'monotonic',side_effect=clock.monotonic), \
                 patch.object(host_support.time,'sleep',side_effect=clock.sleep):
                with self.assertRaises(PermissionError) as raised:host_support.write_json(destination,value)
            self.assertIs(raised.exception,error);self.assertEqual(destination.read_bytes(),original)
            self.assertLessEqual(len(paths),32);self.assertGreater(len(paths),3)
            self.assertAlmostEqual(clock.now,1.)
            self.assertTrue(all(0<wait<=.05 for wait in clock.waits))
            self.assertEqual(len(set(paths)),1);self.assertEqual(json.loads(paths[0].read_text()),value)

    def test_attempt_cap_still_bounds_retry_if_clock_does_not_advance(self):
        with tempfile.TemporaryDirectory() as directory:
            destination=Path(directory)/'commands.json';destination.write_text('original')
            attempts=0
            def locked(source,target):
                nonlocal attempts
                attempts+=1
                if attempts>32:raise AssertionError('retry attempt budget exceeded')
                raise windows_error()
            with patch.object(host_support.os,'replace',side_effect=locked) as replace, \
                 patch.object(host_support.time,'monotonic',return_value=0), \
                 patch.object(host_support.time,'sleep') as sleep:
                with self.assertRaises(PermissionError):host_support.write_json(destination,{'new':1})
            self.assertEqual(replace.call_count,32);self.assertEqual(sleep.call_count,31)
            self.assertEqual(destination.read_text(),'original')

    def test_non_windows_errors_and_other_windows_codes_fail_without_sleep(self):
        errors=[PermissionError(errno.EACCES,'plain access error'),OSError(errno.EIO,'I/O failure'),
                OSError(errno.ENOSPC,'disk full'),windows_error(112),windows_error(2)]
        for error in errors:
            with self.subTest(error=error),tempfile.TemporaryDirectory() as directory:
                destination=Path(directory)/'commands.json';original=b'{"old": true}\n';destination.write_bytes(original)
                with patch.object(host_support.os,'replace',side_effect=error) as replace, \
                     patch.object(host_support.time,'sleep') as sleep:
                    with self.assertRaises(OSError) as raised:host_support.write_json(destination,{'new':1})
                self.assertIs(raised.exception,error);replace.assert_called_once();sleep.assert_not_called()
                self.assertEqual(destination.read_bytes(),original)
                temporary=list(Path(directory).glob('commands.json.*.tmp'));self.assertEqual(len(temporary),1)
                self.assertEqual(json.loads(temporary[0].read_text()),{'new':1})

    def test_fsync_failure_does_not_retry_or_replace_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            destination=Path(directory)/'commands.json';destination.write_text('original')
            with patch.object(host_support.os,'fsync',side_effect=OSError(errno.EIO,'durability failed')), \
                 patch.object(host_support.os,'replace') as replace,patch.object(host_support.time,'sleep') as sleep:
                with self.assertRaises(OSError):host_support.write_json(destination,{'new':1})
            replace.assert_not_called();sleep.assert_not_called();self.assertEqual(destination.read_text(),'original')

    def test_independent_failed_writes_have_distinct_complete_temps(self):
        with tempfile.TemporaryDirectory() as directory:
            destination=Path(directory)/'commands.json';destination.write_text('original')
            for value in (1,2):
                with patch.object(host_support.os,'replace',side_effect=OSError(errno.EIO,'permanent')):
                    with self.assertRaises(OSError):host_support.write_json(destination,{'new':value})
            temporary=list(Path(directory).glob('commands.json.*.tmp'));self.assertEqual(len(temporary),2)
            self.assertEqual(sorted(json.loads(path.read_text())['new'] for path in temporary),[1,2])
            self.assertEqual(destination.read_text(),'original')


if __name__=='__main__':unittest.main()
