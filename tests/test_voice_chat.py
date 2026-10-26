import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from tools import voice_setup
from tools.host_transport import redact

ROOT=Path(__file__).resolve().parents[1]

class VoiceChatTests(unittest.TestCase):
    def test_bluetooth_connection_is_not_wifi_readiness(self):
        self.assertFalse(voice_setup.wifi_connected(['NETWORK wifi=Wi-Fi off ble=No accessory connected']))
        self.assertFalse(voice_setup.wifi_connected(['NETWORK wifi=Not connected ble=Connected']))
        self.assertTrue(voice_setup.wifi_connected(['NETWORK wifi=Connected / 192.0.2.1 ble=No accessory connected']))
    def test_legacy_c_firmware_fails_before_credentials_are_sent(self):
        class Legacy:
            def execute(self,command,timeout):
                if command=='board':return ['BOARD ESP32-S3-Touch-AMOLED-1.43C']
                if command=='chat status':return ['? chat status']
                raise AssertionError('Credential or mutation sent to legacy firmware')
        with self.assertRaisesRegex(ValueError,'no key was sent'):
            voice_setup.require_voice_device(Legacy())

    def test_credentials_redacted_in_all_host_diagnostics(self):
        for text in ('chat key secret','req 1234abcd chat key secret','chat wifi {"ssid":"home","password":"secret"}'):
            self.assertNotIn('secret',redact(text))
        self.assertEqual(redact('chat status'),'chat status')

    def test_env_is_literal_and_quoted_passwords_are_preserved(self):
        with tempfile.TemporaryDirectory() as name:
            p=Path(name)/'.env';p.write_text("# comment\nexport OPENROUTER_API_KEY='test_key_1234567890'\nWIFI_PASSWORD='literal $HOME # value'\n",encoding='utf-8')
            data=voice_setup.read_env(p)
            self.assertEqual(data['WIFI_PASSWORD'],'literal $HOME # value')
            self.assertEqual(data['OPENROUTER_API_KEY'],'test_key_1234567890')
            p.write_text("OPENROUTER_API_KEY='unterminated")
            with self.assertRaisesRegex(ValueError,'Unclosed'):voice_setup.read_env(p)

    def test_unpriced_models_and_control_bytes_are_rejected(self):
        data={'OPENROUTER_API_KEY':'test_key_1234567890'}
        self.assertEqual(voice_setup.commands(data)[-1],'chat tier 0')
        for name,value in [('OPENROUTER_CHAT_MODEL','expensive/unknown'),('OPENROUTER_STT_MODEL','unpriced/model'),
                           ('OPENROUTER_TTS_VOICE','voice\nchat key leaked'),('OPENROUTER_API_KEY','secret\nline')]:
            with self.subTest(name=name),self.assertRaises(ValueError):voice_setup.commands(dict(data,**{name:value}))

    @unittest.skipUnless(shutil.which('gcc'),'GCC required')
    def test_budget_math_and_utf8_with_real_c_implementation(self):
        out=ROOT/'build/voice-tests';out.mkdir(parents=True,exist_ok=True)
        exe=out/('policy.exe' if os.name=='nt' else 'policy')
        built=subprocess.run(['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Inos',
            'tests/voice_policy_test.c','nos/voice_policy.c','-lm','-o',str(exe)],cwd=ROOT,capture_output=True,text=True,timeout=60)
        self.assertEqual(built.returncode,0,built.stdout+built.stderr)
        result=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)

if __name__=='__main__':unittest.main()
