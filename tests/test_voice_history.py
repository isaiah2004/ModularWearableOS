from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import re
ROOT=Path(__file__).resolve().parents[1]
@unittest.skipUnless(shutil.which('gcc'),'GCC required')
class VoiceHistoryTests(unittest.TestCase):
    def production_functions(self,path,names):
        source=(ROOT/path).read_text()
        result=[]
        for name in names:
            found=re.search(r'^(?:static\s+)?(?:void|int)\s+'+name+r'\([^;]*?\)\s*\{',source,re.M)
            self.assertIsNotNone(found,name)
            end,depth=found.end(),1
            while depth:
                depth+=(source[end]=='{')-(source[end]=='}');end+=1
            result.append(source[found.start():end])
        return '\n\n'.join(result)

    def test_production_state_and_codec_ownership(self):
        groups=[('voice_chat_state_test.c','firmware/main/voice_chat.c',
                 ['active','status','voice_chat_cancel','voice_chat_clear','voice_chat_volume','voice_chat_audio',
                  'voice_chat_history_list','voice_chat_history_select','voice_chat_history_delete',
                  'voice_chat_history_pending','voice_chat_maintenance']),
                ('voice_audio_levels_test.c','firmware/main/voice_audio.c',['voice_audio_levels','levels_apply'])]
        with tempfile.TemporaryDirectory(prefix='voice-state-',dir=ROOT/'build') as directory:
            for name,path,names in groups:
                with self.subTest(name=name):
                    source=Path(directory)/name;exe=source.with_suffix('.exe')
                    source.write_text((ROOT/'tests'/name).read_text().replace('/* ACTUAL FUNCTIONS */',self.production_functions(path,names)))
                    run=subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Icore','-Inos',str(source),
                        'nos/voice_history.c','nos/voice_policy.c','-lm','-o',str(exe)],cwd=ROOT,capture_output=True,text=True)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)
                    run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
                    self.assertEqual(run.returncode,0,run.stdout+run.stderr)
                    self.assertIn('PASS',run.stdout)
    def test_actual_history_capacity_utf8_and_corruption(self):
        with tempfile.TemporaryDirectory(prefix='voice-history-',dir=ROOT/'build') as directory:
            exe=Path(directory)/'history.exe'
            run=subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Inos',
                'tests/voice_history_test.c','nos/voice_history.c','nos/voice_policy.c','-lm','-o',str(exe)],
                cwd=ROOT,capture_output=True,text=True)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            run=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10)
            self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertIn('PASS',run.stdout)
if __name__=='__main__':unittest.main()
