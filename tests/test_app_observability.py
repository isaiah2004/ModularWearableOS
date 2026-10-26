"""Private fresh-source app transactions; no shared objects, IDF or device.

Reuse the original APP fixtures, adding assertions about the emitted transactions
at their actual queue/finalization boundaries. Shared headers must be current.
"""
from pathlib import Path
import importlib.util
import os
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/alpha05/checks/app_observability"
spec = importlib.util.spec_from_file_location("app_content_fixtures", ROOT / "tests/test_content_regressions.py")
content = importlib.util.module_from_spec(spec)
spec.loader.exec_module(content)


class AppObservability(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        content.OUT = OUT
        content.INC = OUT / "headers"
        os.environ["CONTENT_REQUIRE_SHARED"] = "1"
        content.prepare()
        os.environ["TMPDIR"] = str(OUT)
        content.COMMON = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
''' + '#include "' + (ROOT / "tests/app_observability_test_log.h").as_posix() + '"\n'

    def setUp(self):
        self.edits = []
        self.tail = ""

    def run_c(self, name, body, extra=()):
        for before, after in self.edits:
            self.assertEqual(body.count(before), 1, before)
            body = body.replace(before, after, 1)
        if self.tail:
            at = body.rfind(' puts("PASS:')
            self.assertGreater(at, 0)
            body = body[:at] + self.tail + body[at:]
        content.ContentRegressions.run_c(self, name, body,
            (*extra, "tests/app_observability_test_log.c"))

    def test_memo_request_survives_finalizing_collision_and_failure(self):
        self.edits = [
            (' rec_start();assert(attempts==3);', r'''
 uint32_t start=app_test_request(DIAG_DOMAIN_MEMO,DIAG_ACT_START);
 assert(start&&app_test_target(start)==1&&app_test_results(start,DIAG_RESULT_PENDING)==1);
 assert(!app_test_results(start,DIAG_RESULT_SUCCESS));
 rec_start();assert(attempts==3);
 uint32_t rejected=app_test_request(DIAG_DOMAIN_MEMO,DIAG_ACT_START);
 assert(rejected!=start&&app_test_results(rejected,DIAG_RESULT_REJECTED)==1);
'''),
            (' svc_memo_tick(.0333f);assert(s_rec_open&&s_nmemo==0&&feedback_success==0);', r'''
 uint32_t stop=app_test_request(DIAG_DOMAIN_MEMO,DIAG_ACT_STOP);
 assert(stop&&app_test_target(stop)==1&&app_test_results(stop,DIAG_RESULT_PENDING)==1);
 unsigned waiting=app_test_event_count;
 for(int tick=0;tick<10000;tick++)svc_memo_tick(.0333f);
 assert(app_test_event_count==waiting&&!app_test_results(start,DIAG_RESULT_SUCCESS));
 assert(!app_test_results(stop,DIAG_RESULT_SUCCESS));
 svc_memo_tick(.0333f);assert(s_rec_open&&s_nmemo==0&&feedback_success==0);
'''),
            (' svc_memo_tick(.0333f);assert(s_nmemo==1&&feedback_success==1);', r'''
 assert(app_test_results(start,DIAG_RESULT_SUCCESS)==1&&app_test_results(stop,DIAG_RESULT_SUCCESS)==1);
 unsigned completed=app_test_event_count;
 svc_memo_tick(.0333f);assert(s_nmemo==1&&feedback_success==1);
 assert(app_test_event_count==completed);
'''),
        ]
        self.tail = r'''
 uint32_t failed_start=app_test_request(DIAG_DOMAIN_MEMO,DIAG_ACT_START);
 uint32_t failed_stop=app_test_request(DIAG_DOMAIN_MEMO,DIAG_ACT_STOP);
 assert(app_test_results(failed_start,DIAG_RESULT_FAILED)==1&&!app_test_results(failed_start,DIAG_RESULT_SUCCESS));
 assert(app_test_results(failed_stop,DIAG_RESULT_FAILED)==1&&!app_test_results(failed_stop,DIAG_RESULT_SUCCESS));
 puts("PASS OBS: memo queue/finalizing emits no success; terminal saved/error completes retained session once; stale HAL success rejected");
'''
        content.ContentRegressions.test_memo_collision_and_async_completion(self)

    def test_sms_backend_result_survives_reader_reset(self):
        self.edits = [
            (' reply_send(0);assert(R.result==2&&!success&&!strcmp(recipient,"+111"));', r'''
 reply_send(0);assert(R.result==2&&!success&&!strcmp(recipient,"+111"));
 uint32_t first=app_test_request(DIAG_DOMAIN_SMS,DIAG_ACT_SEND);
 assert(app_test_target(first)==42&&app_test_results(first,DIAG_RESULT_PENDING)==1);
 assert(!app_test_results(first,DIAG_RESULT_SUCCESS));
'''),
            (' state=4;read_tick(.0333f);assert(R.result==-1&&error==1&&!pops);', r'''
 state=4;read_tick(.0333f);assert(R.result==-1&&error==1&&!pops);
 assert(app_test_results(first,DIAG_RESULT_FAILED)==1&&!app_test_results(first,DIAG_RESULT_SUCCESS));
'''),
        ]
        self.tail = r'''
 request=71;state=1;R.result=0;reply_send(0);
 uint32_t background=app_test_request(DIAG_DOMAIN_SMS,DIAG_ACT_SEND);
 assert(app_test_target(background)==42&&app_test_results(background,DIAG_RESULT_PENDING)==1);
 read_enter();assert(!R.request&&s_send_backend==71); /* a fresh reader can't own the old request */
 reply_send(1);
 uint32_t busy=app_test_request(DIAG_DOMAIN_SMS,DIAG_ACT_SEND);
 assert(app_test_results(busy,DIAG_RESULT_REJECTED)==1&&s_send_trace==background);
 state=2;unsigned waiting=app_test_event_count;
 for(int tick=0;tick<10000;tick++)svc_cell_tick(.0333f);
 assert(app_test_event_count==waiting&&!app_test_results(background,DIAG_RESULT_SUCCESS));
 state=3;svc_cell_tick(0); /* CMGS completion, without ticking a visible reader */
 assert(!s_send_backend&&app_test_results(background,DIAG_RESULT_SUCCESS)==1);
 unsigned complete=app_test_event_count;svc_cell_tick(0);assert(app_test_event_count==complete);
 state=1;R.result=0;reply_send(0);uint32_t lost=app_test_request(DIAG_DOMAIN_SMS,DIAG_ACT_SEND);
 state=0;svc_cell_tick(0);
 assert(app_test_results(lost,DIAG_RESULT_STALE)==1&&!app_test_results(lost,DIAG_RESULT_SUCCESS));
 puts("PASS OBS: stable SMS target survives reorder/re-entry; background CMGS success/failure/unknown is final once; pending sends reject overwrite");
'''
        content.ContentRegressions.test_sms_identity_pending_result_and_leases(self)

    def test_weather_edges_are_sparse_and_contain_state_only(self):
        self.tail = r'''
 feed.valid=1;feed.fetching=0;feed.stale=0;feed.updated_epoch=(uint32_t)epoch;
 feed.source=2;feed.generation=17;nos_frame++;wx_view_tick();
 uint32_t fresh=app_test_request(DIAG_DOMAIN_WEATHER,DIAG_ACT_RECEIVE);
 assert(app_test_target(fresh)==17&&app_test_results(fresh,DIAG_RESULT_SUCCESS)==1);
 unsigned quiet=app_test_event_count;
 for(int tick=0;tick<10000;tick++){nos_frame++;nos_t+=.0333;wx_view_tick();}
 assert(app_test_event_count==quiet); /* >5 minutes of widget/app ticks, no repeated snapshots */
 feed.updated_epoch=(uint32_t)epoch-7200;nos_frame++;wx_view_tick();
 assert(app_test_event_count==quiet+3);
 uint32_t stale=app_test_request(DIAG_DOMAIN_WEATHER,DIAG_ACT_RECEIVE);
 assert(app_test_target(stale)==17&&app_test_results(stale,DIAG_RESULT_STALE)==1);
 assert(app_test_events[quiet+1].code==DIAG_ACTION_CONTEXT&&app_test_events[quiet+1].b==2&&app_test_events[quiet+1].c==7200);
 strcpy(feed.loc,"PRIVATE CITY");strcpy(feed.status,"PRIVATE STATUS");nos_frame++;wx_view_tick();
 assert(app_test_event_count==quiet+3); /* text changes never become routine events or payload hashes */
 puts("PASS OBS: weather generation/source/stale/age edges emit 3 numeric records; 10000 idle ticks and text-only changes emit none");
'''
        content.ContentRegressions.test_weather_acquisition_epochs_and_invalidated_feed(self)

    def test_settings_slider_cancel_and_gesture_coalescing(self):
        self.run_c("settings_gesture_observability", r'''
#include "apps/set_settings.c"
nos_time_t nos_t=604800.000123;nos_prefs_t nos_prefs;
static int writes,backlights;
void prefs_save(void){writes++;}
void nos_feedback(int f){}
nos_time_t nos_enter_t(void){return nos_t;}
uint32_t hal_ext_present(void){return 0;}
int hal_light_get(hal_light*out){memset(out,0,sizeof *out);return 0;}
void hal_set_backlight(uint8_t v){backlights++;}
void nos_brightness_override(int v){}
void hal_audio_volume(int v){}
void hal_haptic_set_strength(int v){}
void hal_haptic(int p){}
void hal_sfx(int s){}
int main(void){
 SL.id=I_BRIGHT;nos_prefs.brightness=20;slide_enter();
 SL.touch=SL.dragging=1;
 for(int i=21;i<=75;i++){SL.v=(float)i;slide_tick(.0333f);nos_t+=.0333;}
 assert(nos_prefs.brightness==75&&writes==55&&backlights==55&&!app_test_event_count);
 nos_input end={.type=IN_DRAG_END};slide_input(&end);slide_tick(.0333f);
 uint32_t committed=app_test_request(DIAG_DOMAIN_CONTROL,DIAG_ACT_SET);
 assert(app_test_target(committed)==0x5000+I_BRIGHT&&app_test_results(committed,DIAG_RESULT_SUCCESS)==1);
 unsigned quiet=app_test_event_count;
 for(int i=0;i<1000;i++)slide_tick(.0333f);
 assert(app_test_event_count==quiet); /* all 55 live values form one gesture result */
 SL.touch=SL.dragging=1;SL.v=80;slide_tick(.0333f);assert(nos_prefs.brightness==80);
 mtw_start(&SL.tw,80,100,M_FRAMES(9),&EASE_STEADY);SL.animating=1;SL.v=90;
 nos_input cancel={.type=IN_CANCEL};slide_input(&cancel);
 uint32_t cancelled=app_test_request(DIAG_DOMAIN_CONTROL,DIAG_ACT_SET);
 assert(cancelled!=committed&&app_test_results(cancelled,DIAG_RESULT_CANCELLED)==1);
 assert(!SL.animating&&!SL.dragging&&!SL.touch&&SL.v==80);
 quiet=app_test_event_count;int saved=writes;
 for(int i=0;i<1000;i++){nos_t+=.0333;slide_tick(.0333f);}
 assert(nos_prefs.brightness==80&&writes==saved&&app_test_event_count==quiet);
 puts("PASS: 55 actual slider updates yield one stable-target result; cancellation freezes the applied value at seven-day uptime without deferred commits");return 0;
}
''', ("nos/motion.c",))

    def test_navigation_records_settled_page_without_drag_spam(self):
        self.edits = [
            ('void svc_track_start(void){assert(!pending);active=1;}',
             'static int reject_start;\nvoid svc_track_start(void){assert(!pending);if(!reject_start)active=1;}')
        ]
        self.tail = r'''
 pg_init(&N.pg,NPAGE,0);N.trace_page=0;
 unsigned before=app_test_event_count;
 N.pg.dragging=1;
 for(int i=0;i<200;i++){msp_set(&N.pg.sp,(float)(i%3)*PAGE_W);nav_tick(.0333f);}
 assert(app_test_event_count==before);
 N.pg.dragging=0;msp_set(&N.pg.sp,2*PAGE_W);nav_tick(.0333f);
 uint32_t page=app_test_request(DIAG_DOMAIN_NAVIGATION,DIAG_ACT_TRANSITION);
 assert(app_test_target(page)==2&&app_test_results(page,DIAG_RESULT_SUCCESS)==1);
 assert(app_test_event_count==before+3);
 for(int i=0;i<1000;i++)nav_tick(.0333f);
 assert(app_test_event_count==before+3);
 active=0;pending=0;reject_start=1;N.present=1;N.saved_state=2;
 pg_init(&N.pg,NPAGE,0);N.trace_page=0;N.ak=N.ak_prev=A_START;N.ak_t=N.t0=nos_t-1;
 assert(on_rec_button(UI_CX,UI_BTN_Y));
 assert(nav_input(&tap)&&!active&&N.saved_state==2); /* rejected service start cannot confirm recording */
 puts("PASS OBS: Navigate settles to a stable page target once; 200 drag samples and 1000 idle ticks emit no repeat transactions");
'''
        content.ContentRegressions.test_navigation_lost_fix_stop_and_stale(self)

    def test_manual_wrist_provenance_is_in_the_transaction(self):
        fixture = (ROOT / "tests/settings_preferences_regression_test.c").as_posix()
        self.run_c("manual_wrist_provenance", '#define main app_baseline_main\n#include "' + fixture + '"\n#undef main\n' + r'''
int main(void){
 char *args[]={"app-observability",NULL};assert(app_baseline_main(1,args)==0);
 uint32_t request=app_test_request(DIAG_DOMAIN_CONTROL,DIAG_ACT_SET);
 assert(app_test_target(request)==0x5000+I_WRIST&&app_test_results(request,DIAG_RESULT_SUCCESS)==1);
 int found=0;
 for(unsigned i=0;i<app_test_event_count;i++) {
  const app_test_event *event=&app_test_events[i];
  if(event->code==DIAG_ACTION_CONTEXT&&event->a==(int32_t)request&&event->b==PREFS_WRIST_SOURCE_MANUAL&&
     event->c==(int32_t)prefs_wrist_revision())found++;
 }
 assert(found==1);
 puts("PASS: actual manual wrist edits trace stable control ID, explicit MANUAL provenance and matching authoritative revision");return 0;
}
''', ("nos/prefs.c", "nos/motion.c"))

    def test_modules_calibration_requires_new_progress_and_power_requires_readback(self):
        fixture = (ROOT / "tests/settings_modules_imu_regression_test.c").read_text()
        substitutions = [
            ('#include "../nos/apps/set_modules.c"', '#include "apps/set_modules.c"'),
            ('#include "settings_semantic_draw_stubs.h"', '#include "' + (ROOT / "tests/settings_semantic_draw_stubs.h").as_posix() + '"'),
            ('int main(void)', 'int app_unused_baseline_main(void)'),
            ('uint32_t hal_ext_present(void) { return 0; }', 'static uint32_t module_mask;\nuint32_t hal_ext_present(void) { return module_mask; }'),
            ('int hal_mag_get(hal_mag *out) { memset(out,0,sizeof *out); return 0; }',
             'static hal_mag mag_sample;\nint hal_mag_get(hal_mag *out) { *out=mag_sample; return 1; }'),
            ('int hal_lte_get(hal_lte *out) { memset(out,0,sizeof *out); return 0; }',
             'static hal_lte lte_sample;static int lte_present;\nint hal_lte_get(hal_lte *out) { *out=lte_sample; return lte_present; }'),
            ('void hal_mag_calibrate(int on) { (void)on; assert(0); }', 'static int calibration_stops;\nvoid hal_mag_calibrate(int on) { assert(!on);calibration_stops++; }'),
            ('void nos_feedback(int kind) { (void)kind; assert(0); }', 'void nos_feedback(int kind) { (void)kind; }'),
            ('void nos_toast(const char *s) { (void)s; assert(0); }', 'void nos_toast(const char *s) { (void)s; }'),
        ]
        for before, after in substitutions:
            self.assertEqual(fixture.count(before), 1, before)
            fixture = fixture.replace(before, after, 1)
        self.run_c("modules_completion_readback", fixture + r'''
float hal_mic_db(void){return 0;}
int main(void){
 module_mask=EXT_MAG;D.m=M_MAG;D.next=nos_t+1000;
 mag_sample=(hal_mag){.valid=1,.calibrated=1,.cal_progress=1};
 D.cal_trace=module_action(DIAG_ACT_START,1);D.calibrating=1;
 uint32_t calibration=D.cal_trace;diag_action_result(calibration,DIAG_RESULT_PENDING,0);
 detail_tick(.0333f);assert(D.calibrating&&!calibration_stops&&!app_test_results(calibration,DIAG_RESULT_SUCCESS));
 mag_sample.cal_progress=0;detail_tick(.0333f);assert(D.cal_seen_low&&D.calibrating);
 mag_sample.cal_progress=1;detail_tick(.0333f);
 assert(!D.calibrating&&calibration_stops==1&&app_test_results(calibration,DIAG_RESULT_SUCCESS)==1);
 unsigned quiet=app_test_event_count;detail_tick(.0333f);assert(app_test_event_count==quiet);
 D.m=M_LTE;module_mask=EXT_LTE;lte_present=0;
 D.lte_req=-1;D.lte_req_t=nos_t;D.lte_trace=module_action(DIAG_ACT_SET,1);
 uint32_t off=D.lte_trace;diag_action_result(off,DIAG_RESULT_PENDING,0);
 nos_t+=1;detail_tick(.0333f);
 assert(D.lte_req==-1&&!app_test_results(off,DIAG_RESULT_SUCCESS)); /* absent != confirmed off */
 nos_t+=21;detail_tick(.0333f);
 assert(!D.lte_req&&app_test_results(off,DIAG_RESULT_FAILED)==1);
 lte_present=1;lte_sample.powered=1;D.lte_req=1;D.lte_req_t=nos_t;D.lte_trace=module_action(DIAG_ACT_SET,0);
 uint32_t on=D.lte_trace;diag_action_result(on,DIAG_RESULT_PENDING,1);detail_tick(.0333f);
 assert(!D.lte_req&&app_test_results(on,DIAG_RESULT_SUCCESS)==1);
 puts("PASS: Modules rejects stale calibrated progress, completes after a fresh cycle once, and requires actual LTE readback or emits timeout failure");return 0;
}
''', ("nos/motion.c",))

    def test_private_values_are_never_hashed_for_logging(self):
        checked = 0
        for path in (ROOT / "nos/apps").glob("*.c"):
            if not path.name.startswith(("con_", "out_", "time_", "fit_", "set_")) or path.name == "set_developer.c":
                continue
            text = path.read_bytes().decode("latin1")
            for call in re.finditer(r"diag_log_name_id\(([^\n)]*)\)", text):
                self.assertRegex(call[1], r'^"[A-Za-z0-9_. /-]+"$', str(path))
                checked += 1
        self.assertGreater(checked, 15)
        (OUT / "privacy.log").write_text(f"PASS: {checked} app diagnostic hashes use fixed literals; no message/contact/number/credential/location/payload hash source\n")

    def test_workout_commands_report_acceptance_without_confirming_sd_save(self):
        self.run_c("workout_command_outcomes", r'''
#include "apps/fit_workout.c"
static workout_snapshot session;
static int permit,retries;
const workout_snapshot *workout_get(void){return &session;}
int workout_pause(void){if(!permit||session.state!=WORKOUT_RUNNING)return 0;session.state=WORKOUT_PAUSED;return 1;}
int workout_resume(void){if(!permit||session.state!=WORKOUT_PAUSED)return 0;session.state=WORKOUT_RUNNING;return 1;}
int workout_lap_add(void){if(!permit||session.state!=WORKOUT_RUNNING)return 0;session.nlaps++;return 1;}
int workout_finish(void){if(!permit||session.state!=WORKOUT_PAUSED)return 0;session.state=WORKOUT_FINISHED;session.save_state=WORKOUT_SAVE_PENDING;return 1;}
void workout_retry_save(void){retries++;}
int main(void){
 session.id=901;session.state=WORKOUT_RUNNING;
 assert(!session_action(DIAG_ACT_PAUSE));
 uint32_t rejected=app_test_request(DIAG_DOMAIN_SENSOR,DIAG_ACT_PAUSE);
 assert(app_test_target(rejected)==901&&app_test_results(rejected,DIAG_RESULT_REJECTED)==1);
 permit=1;assert(session_action(DIAG_ACT_PAUSE)&&session.state==WORKOUT_PAUSED);
 assert(!session_action(DIAG_ACT_MARK));
 uint32_t paused_lap=app_test_request(DIAG_DOMAIN_SENSOR,DIAG_ACT_MARK);
 assert(app_test_results(paused_lap,DIAG_RESULT_REJECTED)==1&&!session.nlaps);
 assert(session_action(DIAG_ACT_RESUME)&&session_action(DIAG_ACT_MARK)&&session.nlaps==1);
 assert(session_action(DIAG_ACT_PAUSE)&&session_action(DIAG_ACT_STOP));
 uint32_t stop=app_test_request(DIAG_DOMAIN_SENSOR,DIAG_ACT_STOP);
 assert(app_test_target(stop)==901&&app_test_results(stop,DIAG_RESULT_SUCCESS)==1);
 assert(session.state==WORKOUT_FINISHED&&session.save_state==WORKOUT_SAVE_PENDING);
 session_retry();uint32_t save=app_test_request(DIAG_DOMAIN_SENSOR,DIAG_ACT_SAVE);
 assert(app_test_target(save)==901&&app_test_results(save,DIAG_RESULT_PENDING)==1);
 assert(!app_test_results(save,DIAG_RESULT_SUCCESS)&&retries==1);
 puts("PASS: actual Sports callback helper logs rejected/accepted pause/resume/lap/stop by session ID; stop and save retry never confirm checked SD persistence");return 0;
}
''')


if __name__ == "__main__":
    unittest.main(verbosity=2)
