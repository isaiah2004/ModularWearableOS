import unittest
import zlib
from tests.test_target_benchmark_plan import Firmware,line
from tools import target_benchmark as B

class SharpBenchmarkEvidenceTests(unittest.TestCase):
    def test_guard_crc_nonce_and_complete_fields(self):
        from tools.benchmark_sharp import parse_usb_frame,USB_FIELDS
        values={k:'0' for k in USB_FIELDS};values.update(v='2',nonce='123',valid='1',guard='1')
        body=' '.join(k+'='+values[k] for k in sorted(values));frame=f'USBDIAG {len(body)}:{body} crc={zlib.crc32(body.encode()):08x}'
        self.assertEqual(parse_usb_frame(frame,123)['guard'],'1')
        with self.assertRaises(B.ProtocolError):parse_usb_frame(frame,124)
        with self.assertRaises(B.ProtocolError):parse_usb_frame(frame[:-1]+('0' if frame[-1]!='0' else '1'),123)
    def test_job_list_is_complete_and_stable(self):
        from tools.benchmark_sharp import jobs
        self.assertEqual(len(jobs()),32)
        self.assertEqual(jobs()[18][1],'apps')
        self.assertEqual(jobs()[18][2]['name'],'fling')
    def test_new_diagnostics_are_explicitly_scoped(self):
        from pathlib import Path
        session=B.Session('offline',Path('build/sharp-20261022/tests'))
        with self.assertRaises(ValueError):session.command('rotation_bench',mutation=True)
        self.assertFalse(session.diagnostic_commands)
        session=B.Session('offline',Path('build/sharp-20261022/tests'),diagnostic_commands=('rotation_bench','rotation 2'))
        self.assertEqual(session.diagnostic_commands,frozenset(('rotation_bench','rotation 2')))
        with self.assertRaises(ValueError):B.Session('offline',Path('build'),diagnostic_commands=('erase',))
    def capture(self):
        fw=Firmware();fw.page=1;fw.maximum=100*256
        table=B.records(fw.capture())
        blur=table['NOSBLURCAP'][0]
        for name in ('history_frames','history_pixels','history_blended','h_filtered_pixels','v_filtered_pixels','h_filter_frames','v_filter_frames','blend_frames'):
            blur[name]='0'
        cache=table['NOSCACHECAP'][0]
        cache['native_blurred_frames']='0';cache['native_blurred_processed_area']='0'
        return [line(prefix,record) for prefix,rows in table.items() for record in rows]
    def test_sharp_capture_requires_no_filters_or_history(self):
        lines=self.capture()
        self.assertEqual(B.capture_statistics(lines,7,sharp=True)['intervals'],59)
        with self.assertRaises(B.AdmissionError):B.capture_statistics(lines,7)
        lines=[row.replace('history_pixels=0','history_pixels=1') for row in lines]
        with self.assertRaises(B.AdmissionError):B.capture_statistics(lines,7,sharp=True)
    def test_sharp_snapshot_requires_zero_storage_and_interpolation(self):
        fw=Firmware();fw.state.update(mblur=0,history=0,worker=0)
        state=fw.state_lines()+['NOSRENDER v=1 sharp=1 sprite_interpolation=0 history_storage_bytes=0 cost_probe=0']
        snap=B.snapshot(state,[line('NOSRUNTIME',fw.runtime)],fw.nav(),fw.prefs(),fw.log())
        B.check_snapshot(snap,initial=True)
        snap['sections']['NOSRENDER'][0]['history_storage_bytes']='490232'
        with self.assertRaises(B.AdmissionError):B.check_snapshot(snap,initial=True)

if __name__=='__main__':unittest.main()
