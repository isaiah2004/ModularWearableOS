"""The profiling transport must reject commands outside its declared scope."""
import unittest
from tools.profile_lists import extra_command,jobs,ListSession
from tools import target_benchmark as B
class ListProfileTests(unittest.TestCase):
    def test_extra_scope_and_bounded_reads(self):
        for cap in (0,24,30,40,48,60):self.assertTrue(extra_command(f'fps_cap {cap}',True))
        for value in ('fps_cap 25','fps_cap -1','prefs erase','wifi secret','power restart','list_profile 2'):
            self.assertFalse(extra_command(value,True))
        for offset,count in ((0,8),(511,1),(504,8)):
            self.assertTrue(extra_command(f'list_samples {offset} {count}',False))
        for command in ('list_samples 512 1','list_samples 511 2','list_samples 0 9',
                        'list_samples -1 1','fps_cap 24','list_profile 1'):
            self.assertFalse(extra_command(command,False))
    def test_incomplete_frame_reply_taints_without_replaying(self):
        class Transport:
            last_result={}
            def execute(self,*args,**kwargs):
                self.calls+=1;return ['NOSLISTFRAME index=1']
            calls=0
        session=object.__new__(ListSession);session.tainted=False;session.commands=[]
        session.persist=lambda:None;session.transport=Transport()
        with self.assertRaises(B.ProtocolError):session.command('list_samples 0 1')
        self.assertTrue(session.tainted);self.assertEqual(session.transport.calls,1)
    def test_window_coverage(self):
        quiet=[(cap,app) for cap,app,_,_,profile in jobs() if profile=='quiet']
        self.assertEqual(len(quiet),12)
        self.assertEqual(set(quiet),{(cap,app) for cap in (0,24,30,40,48,60) for app in ('apps','settings')})
        self.assertEqual(len(jobs()),24)
if __name__=='__main__':unittest.main()
