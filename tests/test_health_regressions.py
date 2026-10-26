"""Assertion-backed checks of the audited health callbacks and service paths.

Harnesses compile the current C functions, including the UI acquisition block,
with deterministic HAL faults. They make no sensor-accuracy claims.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = Path(os.environ.get("HEALTH_CHECK_OUT", ROOT / "build/alpha05/checks/health/regressions"))


def source(path):
    return (ROOT / path).read_text(encoding="utf-8")


def function(path, name):
    text = source(path)
    match = re.search(r"^(?:static )?(?:void|int|uint32_t) " + name + r"\([^;\n]*\)\s*\{", text, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[match.start():end]


COMMON = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "hal.h"
#include "hal_ext.h"
#include "nos.h"
#include "prefs.h"
#include "svc.h"
#include "diag_log.h"
'''


@unittest.skipUnless(shutil.which("gcc"), "gcc required")
class HealthRegressions(unittest.TestCase):
    def run_c(self, name, body, extra=()):
        OUT.mkdir(parents=True, exist_ok=True)
        path = OUT / (name + ".c")
        exe = OUT / (name + ".exe")
        path.write_text(COMMON + body, encoding="utf-8")
        command = [shutil.which("gcc"), "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-I" + str(ROOT / "core"), "-I" + str(ROOT / "nos"), str(path),
                   *[str(ROOT / item) for item in extra], "-lm", "-o", str(exe)]
        result = subprocess.run(command, cwd=OUT, text=True, capture_output=True, timeout=60)
        (OUT / (name + ".compile.log")).write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(exe)], cwd=OUT, text=True, capture_output=True, timeout=60)
        (OUT / (name + ".log")).write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS", result.stdout)

    def test_energy_parent_reveal_report_mark_and_checked_reset(self):
        app = "nos/apps/fit_energy.c"
        declaration = re.search(r"typedef struct \{.*?\} energy_state;", source(app), re.S)[0]
        body = r'''
#include "sports.h"
#include "tuning.h"
''' + declaration + r'''
static energy_state *states[5],*active;
#define E (*active)
static int trial_feature,trial_expected=10,trial_report;
static uint32_t trial_started,trial_duration;
static int revealed,marks,save_ok=1,pushes;
nos_prefs_t nos_prefs;
void *nos_mem_alloc(uint32_t n,const char*owner){(void)owner;return calloc(1,n);}
const nos_screen scr_sports_logs={0},scr_quick_report={0};
int nos_revealed(void){return revealed;}
uint32_t hal_millis(void){return 10000;}
uint32_t sports_day(int d){return (uint32_t)(20261021+d);}
uint32_t sports_query_request(uint32_t d,int a,int z){(void)d;(void)a;(void)z;return 1;}
int sports_report(int f,int o,int s){(void)f;(void)o;(void)s;marks++;return 1;}
int sports_intake(int w,int a){(void)w;(void)a;return 1;}
int sports_undo(void){return 1;}
void sports_emit(int t,const float*v,int n){(void)t;(void)v;(void)n;}
int hal_store_load(const char*k,void*v,int n){(void)k;(void)v;(void)n;return 0;}
int hal_store_save_checked(const char*k,const void*v,int n){(void)k;(void)v;(void)n;return save_ok;}
void prefs_save(void){}
void nos_feedback(int f){(void)f;}
void nos_push(const nos_screen*s,int t){(void)s;(void)t;pushes++;}
void wake_service_learn(void){}
'''
        body += "\n".join(function(app, n) for n in ("refresh", "init", "energy_enter", "battery_enter", "logs_enter", "report_enter", "tune_enter", "bar_at", "input"))
        body += r'''
int main(void){
 energy_enter();E.day=-4;E.start=300;E.end=600;E.editor=1;E.amount=777;
 revealed=1;energy_enter();assert(E.day==-4&&E.start==300&&E.end==600&&E.editor&&E.amount==777);
 revealed=0;battery_enter();E.day=-2;logs_enter();E.log_offset=9;
 revealed=1;battery_enter();assert(E.day==-2);logs_enter();assert(E.log_offset==9);
 energy_enter();assert(E.day==-4&&E.editor&&E.amount==777);
 revealed=0;report_enter();assert(marks==1);E.feature=8;E.outcome=2;E.seconds=300;
 revealed=1;report_enter();assert(marks==1&&E.feature==8&&E.outcome==2&&E.seconds==300);
 revealed=0;tune_enter();assert(tuning_adjust(0,1));sports_tuning before,after;tuning_get(&before);
 unsigned rev=tuning_revision();save_ok=0;
 nos_input tap={.type=IN_TAP,.x=150,.y=337};assert(input(&tap));
 tuning_get(&after);assert(!memcmp(&before,&after,sizeof before)&&tuning_revision()==rev);
 assert(strstr(E.message,"FAILED"));save_ok=1;assert(input(&tap));tuning_get(&after);
 assert(after.walk_g==.10f&&tuning_revision()==rev+1&&strstr(E.message,"RESTORED"));
 puts("PASS energy retained parent/child/reveal context, one explicit report mark, reset failure/retry and active revision");return 0;
}
'''
        self.run_c("energy_reveal", body, ("nos/tuning.c",))

    def test_hr_history_keeps_acquisition_epoch_and_gaps(self):
        svc = "nos/svc.c"
        body = r'''
static svc_heart_t s_heart;
static int s_hr_rec_day_min,s_hr_rec_day_max;
static uint32_t s_hr_day,s_hr_last_sample_ms;
static uint32_t wall=1792497600u,clock_ms=100000;
static hal_hr_sample history[2]={{100000,1792497600u,123},{0,0,0}};
static int count=1;
uint32_t hal_millis(void){return clock_ms;}
uint64_t hal_epoch_seconds(void){return wall;}
void hal_get_datetime(hal_datetime*d){*d=(hal_datetime){.year=2026,.month=10,.day=21,.hour=12};}
static uint32_t day_key(const hal_datetime*d){return (uint32_t)(d->year*10000+d->month*100+d->day);}
int hal_hr_history_samples(hal_hr_sample*out,int max){assert(max>=count);memcpy(out,history,count*sizeof *out);return count;}
'''
        body += "\n".join(function(svc, n) for n in ("minutes_today", "heart_tick", "svc_heart_sample_epoch", "svc_heart_record_at"))
        body += r'''
int main(void){
 heart_tick();assert(s_heart.bpm_last_epoch==wall&&s_heart.hist[143]==123);
 uint32_t acquired=s_heart.bpm_last_epoch;wall+=600;clock_ms+=600000;heart_tick();
 assert(s_heart.bpm_last_epoch==acquired&&s_heart.hist[142]==123&&!s_heart.hist[143]);
 for(int i=0;i<100;i++){heart_tick();}assert(s_heart.bpm_last_epoch==acquired);
 history[1]=(hal_hr_sample){clock_ms,wall,135};count=2;heart_tick();
 assert(s_heart.bpm_last_epoch==wall&&s_heart.bpm_last==135&&s_heart.hist[143]==135);
 wall+=600;heart_tick();assert(s_heart.bpm_last_epoch==history[1].epoch&&!s_heart.hist[143]);
 assert(svc_heart_sample_epoch(clock_ms-2000)==wall-2);
 assert(!svc_heart_sample_epoch(clock_ms-4000));
 svc_heart_record_at(140,97,wall-3);assert(s_heart.bpm_last_epoch==wall-3);
 svc_heart_record_at(141,0,wall-4);assert(s_heart.bpm_last==140&&s_heart.bpm_last_epoch==wall-3);
 wall+=86400;heart_tick();assert(!s_heart.nhist&&s_heart.bpm_last_epoch!=wall);
 puts("PASS timestamped HR history does not renew cached acquisitions, bins actual gaps and preserves measurement time");return 0;
}
'''
        self.run_c("hr_history", body)

    def test_breathe_accepts_new_in_session_acquisitions(self):
        body = r'''
static struct{int hr_on,hr_avg;uint64_t hr_integral;
 uint32_t hr_start_ms,hr_last_ms,hr_valid_ms,hr_epoch;int hr_previous,hr_have;}B;
static uint32_t clock_ms=100000;
static unsigned owners;
static hal_hr reading={.on_wrist=1,.bpm=100,.quality=.9f,.sample_ms=100000};
uint32_t hal_millis(void){return clock_ms;}
unsigned hal_ext_present(void){return EXT_HR;}
void hal_hr_request(unsigned owner,int on){if(on)owners|=owner;else owners&=~owner;}
int hal_hr_get(hal_hr*out){*out=reading;return 1;}
uint32_t svc_heart_sample_epoch(uint32_t ms){return 1792497600u-(clock_ms-ms)/1000;}
'''
        body += "\n".join(function("nos/apps/fit_breathe.c", n) for n in ("hr_start", "hr_stop", "hr_sample", "hr_average"))
        body += r'''
int main(void){
 owners=HAL_HR_OWNER_WORKOUT;hr_start();clock_ms++;B.hr_start_ms=clock_ms;
 for(int i=0;i<100;i++){hr_sample();}assert(!B.hr_have&&!B.hr_valid_ms);
 reading.sample_ms=clock_ms;hr_sample();for(int i=0;i<100;i++)hr_sample();assert(!B.hr_valid_ms);
 clock_ms+=1000;reading.sample_ms=clock_ms;reading.bpm=120;hr_sample();assert(B.hr_valid_ms==1000&&B.hr_integral==110000);
 for(int i=0;i<100;i++){hr_sample();}assert(B.hr_valid_ms==1000&&B.hr_integral==110000);
 clock_ms+=2000;reading.sample_ms=clock_ms;reading.bpm=140;hr_sample();
 assert(B.hr_valid_ms==3000&&B.hr_integral==370000);
 clock_ms+=5000;hr_sample();assert(!B.hr_have&&B.hr_valid_ms==3000);
 reading.sample_ms=clock_ms;hr_sample();assert(B.hr_valid_ms==3000);
 assert(!hr_average(60000));B.hr_valid_ms=30000;B.hr_integral=3700000;
 assert(hr_average(60000)==123);
 hr_stop();assert(owners==HAL_HR_OWNER_WORKOUT);
 puts("PASS breathing rejects stale/pre-session/duplicate samples, integrates acquisition time and preserves workout demand");return 0;
}
'''
        self.run_c("breathe_acquisition", body)

    def test_heart_live_cached_read_cannot_lock_or_retain_invalid_values(self):
        app = source("nos/apps/fit_heart.c")
        block = app[app.index("    int fresh = 0;", app.index("static void heart_tick(float dt)")):]
        block = block[:block.index("    if (H.present && (H.mode == M_MEASURING")]
        body = r'''
#define HN 128
#define WIN 80
#define LOCK_Q .6f
#define M_FRAMES(n) ((n)/30.0f)
enum{M_ABSENT,M_NOCONTACT,M_MEASURING,M_LOCKED};
static struct{
 hal_hr r;int present,mode,have_sample,last_lockable,locked;
 uint32_t last_sample_ms,entry_ms,accepted_epoch;
 uint8_t v[HN];int n,have_last,npk,last_peak;
 float acc,ph,ph_ok,lo,hi,beat_t,off_t,good_t,contact_t,rearm_t,lock_t;
 int bpm_shown,bpm_prev,spo2;float bpm_morph,bpm_change_t;
}H;
nos_prefs_t nos_prefs;
static uint32_t clock_ms=100000,recorded_at;
static hal_hr reading={.on_wrist=1,.bpm=123,.quality=.9f,.spo2=97,.sample_ms=100000};
uint32_t hal_millis(void){return clock_ms;}
unsigned hal_ext_present(void){return EXT_HR;}
void hal_hr_request(unsigned owner,int on){(void)owner;(void)on;}
int hal_hr_get(hal_hr*out){*out=reading;return 1;}
static void set_mode(int m){H.mode=m;}
static void ingest(const uint8_t*w,float dt){(void)w;(void)dt;}
static void playhead_tick(float dt){(void)dt;}
static float m_clamp(float v,float lo,float hi){return v<lo?lo:v>hi?hi:v;}
void hal_haptic(int p){(void)p;}
uint32_t svc_heart_sample_epoch(uint32_t at){return 1792497600u-(clock_ms-at)/1000;}
void svc_heart_record_at(int b,int o,uint32_t at){(void)b;(void)o;recorded_at=at;}
static void acquire(float dt){
''' + block + r'''
}
int main(void){
 H.present=1;H.entry_ms=clock_ms;H.rearm_t=-1;
 for(int i=0;i<90;i++){acquire(1/30.f);clock_ms+=33;}
 assert(!H.locked&&H.contact_t==0&&H.good_t==0);
 reading.sample_ms=clock_ms;acquire(.1f);
 for(int i=0;i<22;i++){clock_ms+=100;reading.sample_ms=clock_ms;acquire(.1f);}
 assert(H.locked&&H.bpm_shown==123&&H.spo2==97&&recorded_at);
 clock_ms+=100;reading.sample_ms=clock_ms;reading.spo2=0;acquire(.1f);assert(H.locked&&!H.spo2);
 clock_ms+=100;reading.sample_ms=clock_ms;reading.bpm=0;acquire(.1f);assert(!H.locked&&!H.bpm_shown&&!H.spo2);
 reading.bpm=125;reading.sample_ms=clock_ms+1;clock_ms++;acquire(.1f);
 assert(!H.locked&&H.good_t==0);
 for(int i=0;i<23;i++){clock_ms+=100;reading.sample_ms=clock_ms;acquire(.1f);}
 assert(H.locked);clock_ms+=4000;acquire(.1f);assert(!H.locked&&!H.bpm_shown);
 puts("PASS live Heart counts acquisition intervals, rejects cached locks, clears missing SpO2/BPM and stale live state");return 0;
}
'''
        self.run_c("heart_live", body)

    def test_activity_save_failure_is_dirty_and_retries_without_more_steps(self):
        text = source("nos/svc.c")
        start = text.rfind("typedef struct {", 0, text.index("uint8_t  version, valid_days"))
        declaration = text[start:text.index("const svc_activity_t *svc_activity", start)]
        body = r'''
#include "energy.h"
static svc_activity_t s_act;static svc_sys_t s_sys;nos_prefs_t nos_prefs;
''' + declaration + r'''
static int s_as_dirty,s_as_retry,s_min_key=-1;
static uint32_t s_as_save_ms,s_as_attempt_ms,s_min_steps;
static uint32_t clock_ms=100000;static int save_ok,calls,events;
static hal_datetime date={.year=2026,.month=10,.day=21,.hour=12};
uint32_t hal_millis(void){return clock_ms;}
uint64_t hal_epoch_seconds(void){return 1792497600u;}
int workout_day_energy(uint32_t since,uint32_t*ped,float*net){(void)since;*ped=0;*net=0;return 0;}
void hal_get_datetime(hal_datetime*d){*d=date;}
void hal_activity_get(hal_activity*a){*a=(hal_activity){.valid=1};}
int hal_store_load(const char*k,void*p,int n){(void)k;(void)p;(void)n;return 0;}
int hal_store_save_checked(const char*k,const void*p,int n){assert(!strcmp(k,"nosact"));(void)p;(void)n;calls++;return save_ok;}
void diag_log_event(uint16_t c,int32_t a,int32_t b,int32_t d){(void)c;(void)a;(void)b;(void)d;events++;}
uint32_t diag_log_name_id(const char*n){(void)n;return 1;}
static uint32_t day_key(const hal_datetime*d){return (uint32_t)(d->year*10000+d->month*100+d->day);}
'''
        # days_from_civil returns int64_t, unlike the other helper signatures.
        dayfunc = re.search(r"static int64_t days_from_civil.*?\n\}", text, re.S)[0]
        keyfunc = re.search(r"static int64_t key_days[^\n]+", text)[0]
        body += function("nos/svc.c", "act_energy") + function("nos/svc.c", "act_publish") + dayfunc + "\n" + keyfunc
        body += "\n".join(function("nos/svc.c", n) for n in ("act_save", "act_rollover", "act_init"))
        # There are sim and hardware act_tick definitions: explicitly extract hardware.
        hardware = text[text.index("static void act_tick(uint32_t now)", text.index("static void act_rollover")):]
        body += re.search(r"static void act_tick\(uint32_t now\).*?\n\}", hardware, re.S)[0]
        body += r'''
int main(void){
 act_init();act_tick(clock_ms);assert(s_as_dirty&&s_act.week_valid==64);
 clock_ms+=600001;act_tick(clock_ms);assert(calls==1&&s_as_dirty&&s_act.checkpoint_pending&&events==1);
 clock_ms+=4999;act_tick(clock_ms);assert(calls==1);
 clock_ms++;act_tick(clock_ms);assert(calls==2&&s_as_dirty);
 save_ok=1;clock_ms+=5000;act_tick(clock_ms);assert(calls==3&&!s_as_dirty&&!s_act.checkpoint_pending&&s_act.checkpoint_epoch);
 save_ok=0;date.day++;act_tick(clock_ms);assert(calls==4&&s_as_dirty&&s_as.week[5]==0&&(s_act.week_valid&32));
 save_ok=1;clock_ms+=5000;act_tick(clock_ms);assert(calls==5&&!s_as_dirty);
 puts("PASS activity known zero validity and checked-save dirty retry, including a failed day rollover without new steps");return 0;
}
'''
        self.run_c("activity_retry", body)

    def test_worker_resources_retry_with_backoff(self):
        body = r'''
#define ESP_PLATFORM 1
#define RETRY_MS 5000u
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define tskNO_AFFINITY 0
#define pdPASS 1
#include "workout.h"
typedef void *TaskHandle_t;static TaskHandle_t writer_handle;
typedef struct{int unused;}writer_memory;
static writer_memory *io;static workout_storage_status storage;
static uint32_t worker_attempt_ms,clock_ms=100000;
static int worker_attempted,alloc_fail=1,task_fail=1,allocs,tasks,last_op;
static uint32_t now_ms(void){return clock_ms;}
static void writer_task(void*p){(void)p;}
static void io_failure(unsigned op,int slot,int err){(void)slot;assert(err==ENOMEM);last_op=(int)op;}
static void *heap_caps_calloc(unsigned n,unsigned size,int caps){(void)caps;allocs++;return alloc_fail?NULL:calloc(n,size);}
static void heap_caps_free(void*p){free(p);}
static int xTaskCreatePinnedToCoreWithCaps(void(*fn)(void*),const char*n,int stack,void*arg,int priority,void*handle,int core,int caps){
 (void)fn;(void)n;(void)stack;(void)arg;(void)priority;(void)handle;(void)core;(void)caps;tasks++;return task_fail?0:pdPASS;}
''' + function("nos/workout.c", "writer_ensure") + r'''
int main(void){
 writer_ensure();assert(!io&&storage.worker_failed&&last_op==WORKOUT_IO_ALLOC&&allocs==1);
 clock_ms+=4999;writer_ensure();assert(allocs==1);
 alloc_fail=0;clock_ms++;writer_ensure();assert(!io&&last_op==WORKOUT_IO_TASK&&tasks==1&&allocs==2);
 clock_ms+=5000;task_fail=0;writer_ensure();assert(io&&!storage.worker_failed&&tasks==2);
 for(int i=0;i<100;i++){writer_ensure();}assert(allocs==3&&tasks==2);heap_caps_free(io);
 puts("PASS writer allocation/task failures remain distinct and recover after bounded retry");return 0;
}
'''
        self.run_c("worker_retry", body)

    def test_history_empty_only_after_authoritative_load(self):
        body = r'''
#include "ui.h"
#include "workout.h"
#include "dot.h"
#define LIST_TOP 116
static struct{int count;ui_list list;}H;
static workout_storage_status st;static char heading[80];static int retries;
void workout_storage_get(workout_storage_status*out){*out=st;}
void workout_retry_save(void){retries++;}
const workout_snapshot *workout_get(void){static workout_snapshot s;return &s;}
uint32_t diag_action_request(uint16_t d,uint16_t a,int32_t t){(void)d;(void)a;(void)t;return 1;}
void diag_action_context(uint32_t r,int32_t s,int32_t v){(void)r;(void)s;(void)v;}
void diag_action_result(uint32_t r,int32_t s,int32_t d){(void)r;(void)s;(void)d;}
uint32_t diag_log_name_id(const char*s){(void)s;return 1;}
void nos_feedback(int f){(void)f;}
int ui_list_input(ui_list*l,const nos_input*in){(void)l;(void)in;return 0;}
void ui_list_draw(gfx_fb*f,ui_list*l){(void)f;(void)l;}
void ui_title(gfx_fb*f,const char*s,float scroll){(void)f;(void)s;(void)scroll;}
void ui_absent(gfx_fb*f,int icon,const char*head,const char*body,const char*foot,float p){(void)f;(void)icon;(void)body;(void)foot;(void)p;snprintf(heading,sizeof heading,"%s",head);}
static void list_header(gfx_fb*f,const char*s,const char*h){(void)f;(void)s;(void)h;}
''' + function("nos/apps/fit_workout.c", "session_retry") + function("nos/apps/fit_workout.c", "history_draw") + function("nos/apps/fit_workout.c", "history_input") + r'''
int main(void){
 history_draw(NULL);assert(strstr(heading,"Loading"));
 st.errors=1;history_draw(NULL);assert(strstr(heading,"unavailable"));
 st.uncertain=1;history_draw(NULL);assert(strstr(heading,"read failed"));
 st.uncertain=0;st.corrupt=1;history_draw(NULL);assert(strstr(heading,"damaged"));
 st.foreign=1;history_draw(NULL);assert(strstr(heading,"Original card"));
 memset(&st,0,sizeof st);st.worker_failed=1;history_draw(NULL);assert(strstr(heading,"worker unavailable"));
 memset(&st,0,sizeof st);st.ready=st.available=1;history_draw(NULL);assert(!strcmp(heading,"No sessions yet"));
 nos_input tap={.type=IN_TAP,.y=250};assert(history_input(&tap)&&retries==1);
 puts("PASS history distinguishes loading, unavailable, ambiguous reads, corruption, foreign card, worker failure and proven empty");return 0;
}
'''
        self.run_c("history_state", body)

    def test_imu_visible_span_comes_from_timestamps(self):
        app = source("nos/apps/out_imu.c")
        declaration = re.search(r"typedef struct \{.*?\} imu_ui;", app, re.S)[0]
        body = "\n#define HIST 64\n" + declaration + r'''
static imu_ui state,*s=&state;static uint32_t clock_ms=100000;
static uint32_t now_ms_step=50;
uint32_t hal_millis(void){return clock_ms;}
int hal_imu_read(hal_imu*v){*v=(hal_imu){.az=1};return 1;}
static void refresh(void){}
''' + function("nos/apps/out_imu.c", "clamp") + function("nos/apps/out_imu.c", "tick") + r'''
int main(void){
 for(int i=0;i<64;i++){clock_ms+=now_ms_step;tick(.05f);}
 assert(s->filled==64&&s->span_ms==3150&&strstr(s->axis,"ACCEL +/-2G / 3."));
 memset(s,0,sizeof *s);now_ms_step=67;
 for(int i=0;i<64;i++){clock_ms+=now_ms_step;tick(.067f);}
 assert(s->span_ms==4221&&strstr(s->axis,"4.2S"));
 uint32_t old_span=s->span_ms;clock_ms+=1000;tick(1);
 assert(s->span_ms==old_span+933&&s->stamps[(s->pos+63)%64]==clock_ms);
 puts("PASS live IMU measured spans follow 50ms, 67ms and delayed UI samples");return 0;
}
'''
        self.run_c("imu_timestamps", body, ("core/gfx.c",))


if __name__ == "__main__":
    unittest.main()
