"""Reset acknowledgement and the single checked preferences checkpoint boundary."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class SettingsCheckpointTests(unittest.TestCase):
    def test_apps_cannot_bypass_preferences_acknowledgement(self):
        for path in (ROOT / "nos/apps").glob("*.c"):
            self.assertIsNone(re.search(r'hal_store_save(?:_checked)?\s*\(\s*"nosprefs"', path.read_text()), str(path))

    @unittest.skipUnless(shutil.which("gcc"), "GCC required")
    def test_actual_reset_unknown_failed_and_checked_completion(self):
        source = (ROOT / "nos/apps/set_settings.c").read_text()
        start = source.index("static void reset_all(void)")
        end = source.index("\nstatic void activate", start)
        body = source[start:end]
        prefix = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "prefs.h"
#include "app_observability_test_log.h"
enum { I_RESET=29, FB_SUCCESS=1, FB_ERROR=2 };
nos_prefs_t nos_prefs;
static int loading, checked, defaults, saves, flushes, syncs, updates, feedback;
static char toast[80];
static uint32_t settings_action(int id,uint16_t action,int old)
{ (void)old;return diag_action_request(DIAG_DOMAIN_CONTROL,action,0x5000+id); }
int prefs_loading(void){return loading;}
void prefs_defaults(void){defaults++;nos_prefs.brightness=70;nos_prefs.volume=60;nos_prefs.haptic_strength=70;}
void prefs_save(void){saves++;}
int prefs_flush(void){flushes++;return checked;}
static void settings_sync_toggles(void){syncs++;}
static void hal_set_backlight(uint8_t level){(void)level;}
static void hal_audio_volume(int volume){(void)volume;}
static void hal_haptic_set_strength(int strength){(void)strength;}
static void nos_feedback(int value){feedback=value;}
static void nos_toast(const char *text){snprintf(toast,sizeof toast,"%s",text);}
static void update_values(void){updates++;}
'''
        main = r'''
int main(void){
 nos_prefs.brightness=62;loading=1;reset_all();
 uint32_t request=app_test_request(DIAG_DOMAIN_CONTROL,DIAG_ACT_SET);
 assert(nos_prefs.brightness==62&&!defaults&&!saves&&!flushes&&!syncs&&!updates);
 assert(feedback==FB_ERROR&&!strcmp(toast,"SETTINGS NOT READY"));
 assert(app_test_results(request,DIAG_RESULT_REJECTED)==1);
 loading=0;reset_all();request=app_test_request(DIAG_DOMAIN_CONTROL,DIAG_ACT_SET);
 assert(defaults==1&&saves==1&&flushes==1&&syncs==1&&updates==1);
 assert(nos_prefs.brightness==70&&feedback==FB_ERROR&&!strcmp(toast,"RESET SAVE PENDING"));
 assert(app_test_results(request,DIAG_RESULT_PENDING)==1&&!app_test_results(request,DIAG_RESULT_SUCCESS));
 checked=1;reset_all();request=app_test_request(DIAG_DOMAIN_CONTROL,DIAG_ACT_SET);
 assert(defaults==2&&saves==2&&flushes==2&&feedback==FB_SUCCESS&&!strcmp(toast,"RESET"));
 assert(app_test_results(request,DIAG_RESULT_SUCCESS)==1);
 puts("PASS: unknown preferences retained, failed checkpoint stays pending, reset success requires checked acknowledgement");
 return 0;
}
'''
        base = ROOT / "build/alpha05/checks/platform/settings-checkpoint"
        base.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="actual-", dir=base) as value:
            directory = Path(value)
            fixture, exe = directory / "reset.c", directory / "reset.exe"
            fixture.write_text(prefix + body + main)
            command = ["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Inos", "-Itests",
                       str(fixture), "tests/app_observability_test_log.c", "-o", str(exe)]
            environment = dict(os.environ, TEMP=str(directory), TMP=str(directory), TMPDIR=str(directory))
            result = subprocess.run(command, cwd=ROOT, env=environment, capture_output=True, text=True, timeout=60)
            (base / "compile.log").write_text(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(exe)], cwd=directory, env=environment, capture_output=True, text=True, timeout=20)
            (base / "result.log").write_text(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
