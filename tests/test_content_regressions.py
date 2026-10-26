"""Behavioral regressions for the APP audit, compiled against the current C.

All compiler, fixture and temporary paths stay under A:/.../checks/content.
Missing root-owned interface declarations can be supplied by an explicit private
contract header; this mode tests the consumer only, not the adapter implementation.
Run with CONTENT_REQUIRE_SHARED=1 after root integrates the requested interfaces.
"""
from pathlib import Path
import json
import os
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/alpha05/checks/content"
INC = OUT / "contract"
COMMON = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "diag_log.h"
/* APP-020 diagnostics belong to the root wave; domain assertions below exercise state. */
uint32_t diag_action_request(uint16_t d,uint16_t a,int32_t t){return 1;}
void diag_log_event(uint16_t c,int32_t a,int32_t b,int32_t d){}
void diag_action_context(uint32_t r,int32_t s,int32_t v){}
void diag_action_result(uint32_t r,int32_t s,int32_t d){}
uint32_t diag_log_name_id(const char*s){uint32_t h=2166136261u;for(;*s;s++){h^=(unsigned char)*s;h*=16777619u;}return h;}
'''


def function(path, name):
    s = (ROOT / path).read_text(encoding="utf-8")
    m = re.search(r"^(?:static\s+)?[^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*\)\s*\{", s, re.M)
    assert m, name
    end, depth = m.end(), 1
    while depth:
        depth += (s[end] == "{") - (s[end] == "}")
        end += 1
    return s[m.start():end] + "\n"


def prepare():
    OUT.mkdir(parents=True, exist_ok=True)
    INC.mkdir(exist_ok=True)
    os.environ["TEMP"] = os.environ["TMP"] = str(OUT)
    additions = []
    for filename in ("svc.h", "hal_ext.h"):
        s = (ROOT / "nos" / filename).read_text(encoding="utf-8")
        if filename == "svc.h":
            if "hours_epoch" not in s:
                s = s.replace("} svc_weather_t;", """    int stale, fetching, source;
    uint32_t generation, age_seconds;
    int utc_offset_seconds;
    int64_t hours_epoch[12], days_epoch[5];
    char status[48];
} svc_weather_t;""")
                additions.append("weather metadata")
            for decl in ("uint32_t svc_alarm_generation(void);", "uint32_t svc_alarm_id(int index);",
                         "int svc_alarm_find(uint32_t id);", "uint32_t svc_call_generation(void);"):
                if decl.split("(")[0].split()[-1] + "(" not in s:
                    s = s.replace("#endif", decl + "\n#endif")
                    additions.append(decl)
        else:
            if not re.search(r"\buint32_t\s+id\s*;[^}]*}\s*hal_sms", s, re.S):
                s = s.replace("int unread; } hal_sms;", "int unread; uint32_t id; } hal_sms;")
                additions.append("hal_sms.id")
            if "humidity_valid" not in s:
                s = s.replace("float temp_c, humidity, pressure_hpa;", "float temp_c, humidity, pressure_hpa; int humidity_valid;")
                additions.append("hal_env.humidity_valid")
            for decl in ("uint32_t hal_lte_sms_send_request(const char *to,const char *text);",
                         "int hal_lte_sms_send_status(uint32_t id);", "void hal_lte_sms_mark_read_id(uint32_t id);",
                         "int hal_rec_start_exclusive(const char *path);", "int hal_rec_status(void);"):
                if decl.split("(")[0].split()[-1] + "(" not in s:
                    s = s.replace("#endif", decl + "\n#endif")
                    additions.append(decl)
        (INC / filename).write_text(s, encoding="utf-8")
    (OUT / "interface-mode.json").write_text(json.dumps({"private_contract_additions": additions,
        "consumer_only": bool(additions)}, indent=2) + "\n")
    if os.environ.get("CONTENT_REQUIRE_SHARED") and additions:
        raise AssertionError("Root interfaces pending: " + ", ".join(additions))
    (INC / "freertos").mkdir(exist_ok=True)
    (INC / "freertos/FreeRTOS.h").write_text('#pragma once\n#define pdPASS 1\n')
    (INC / "freertos/task.h").write_text('''#pragma once
typedef void (*TaskFunction_t)(void *);
int xTaskCreate(TaskFunction_t,const char*,unsigned,void*,unsigned,void*);
void vTaskDelete(void*);
void vTaskDelay(unsigned);
''')


@unittest.skipUnless(shutil.which("gcc"), "gcc required")
class ContentRegressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        prepare()

    def run_c(self, name, body, extra=()):
        src, exe = OUT / (name + ".c"), OUT / (name + ".exe")
        src.write_text(COMMON + body, encoding="utf-8")
        (OUT / (name + ".log")).write_text("NOT RUN: compilation has not completed\n")
        command = [shutil.which("gcc"), "-std=c99", "-O2", "-Wall", "-Wextra",
                   "-Wno-unused-parameter", "-Wno-format-truncation", "-Werror=implicit-function-declaration",
                   "-flto", "-fwhole-program", "-ffunction-sections", "-fdata-sections",
                   "-I" + str(INC), "-I" + str(ROOT / "core"), "-I" + str(ROOT / "nos"),
                   "-I" + str(ROOT / "nos/fonts"), str(src),
                   *[str(ROOT / x) for x in extra], "-Wl,--gc-sections", "-lm", "-o", str(exe)]
        (OUT / (name + ".command.json")).write_text(json.dumps(command, indent=2))
        r = subprocess.run(command, cwd=OUT, text=True, capture_output=True, timeout=60)
        (OUT / (name + ".compile.log")).write_text(r.stdout + r.stderr)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        r = subprocess.run([str(exe)], cwd=OUT, text=True, capture_output=True, timeout=30)
        (OUT / (name + ".log")).write_text(r.stdout + r.stderr)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("PASS", r.stdout)

    def test_track_reanchor_and_save_retry(self):
        self.run_c("track_reanchor_retry", r'''
#include <direct.h>
#include <sys/stat.h>
static int fault, lock_ok=1;
static int flush_fault(FILE *f){int r=fflush(f);return fault==1?EOF:r;}
static int close_fault(FILE *f){int r=fclose(f);return fault==2?EOF:r;}
static int rename_fault(const char*a,const char*b){return fault==3?-1:rename(a,b);}
#define fflush flush_fault
#define fclose close_fault
#define rename rename_fault
#define TRACK_ROOT "track_fixture"
#include "svc_track.c"
#undef fflush
#undef fclose
#undef rename
uint32_t nos_frame;
static uint32_t clock_ms;
static hal_gps fix;
void sports_emit(int t,const float*v,int n){(void)t;(void)v;(void)n;}
void *hal_big_alloc(uint32_t n){return calloc(1,n);}
uint32_t hal_millis(void){return clock_ms;}
uint64_t hal_epoch_seconds(void){return 1792569600u+clock_ms/1000;}
void hal_gps_enable(int on){(void)on;}
int hal_gps_get(hal_gps*out){*out=fix;return 1;}
int hal_sports_sd_lock(void){return lock_ok;}
void hal_sports_sd_unlock(void){}
static void tick(uint32_t ms,double lon,int valid){nos_frame++;clock_ms=ms;fix.lon=lon;fix.valid=valid;svc_track_tick(.0333f);}
int main(void){
 fix.valid=1;svc_track_start();tick(1000,.0001,1);
 assert(T.n==2);double before=T.dist;
 tick(100000,.0001,0);tick(101000,.01,1);tick(102000,.0101,1);
 assert(T.n==3&&T.pts[2].segment_start&&T.dist==before);
 tick(103000,.0102,1);assert(T.n==4&&T.dist>before&&T.dist<before+12);
 tick(104000,100,1);assert(T.n==4);tick(105000,.0103,1);tick(106000,.0104,1);
 assert(T.n>=5&&T.active);
 int n=T.n;double distance=T.dist;lock_ok=0;
 assert(svc_track_stop()==0&&svc_track_pending()&&!svc_track_active());
 svc_track_start();assert(T.n==n&&T.dist==distance&&!T.active);
 lock_ok=1;
 for(fault=1;fault<=3;fault++)assert(svc_track_stop()==0&&T.pending&&T.n==n);
 fault=0;int saved=svc_track_stop();assert(saved>0&&!T.pending&&T.n==n);
 char path[100];snprintf(path,sizeof path,TRACK_DIR "/%04d.gpx",saved);
 FILE*f=fopen(path,"rb");assert(f);char b[10000];size_t z=fread(b,1,sizeof b-1,f);b[z]=0;fclose(f);
 assert(strstr(b,"</trkseg><trkseg>"));
 svc_track_start();tick(107000,.0105,1);assert(T.active&&T.n>=2);
 int next=svc_track_stop();assert(next>saved); /* saved path was never reused */
 svc_track_start();T.n=MAXPTS;for(int i=0;i<MAXPTS;i++)T.pts[i]=(svc_track_pt){.lon=.0105};
 T.pts[MAXPTS-1].segment_start=1;T.last_point_ms=107000;T.gap=T.candidate=0;
 tick(108000,.0106,1);assert(T.n==MAXPTS/2+1&&T.pts[T.n-1].segment_start);
 puts("PASS: GPS gap/outlier recovery segments, excluded gap distance, pending retention and lock/flush/close/publish fault retries");
 return 0;
}
''')

    def test_track_device_worker_retains_immutable_points(self):
        self.run_c("track_device_worker", r'''
#include <direct.h>
#include <io.h>
#define mkdir(p,mode) _mkdir(p)
#define fsync _commit
#define ESP_PLATFORM 1
#define TRACK_ROOT "track_worker_fixture"
#include "svc_track.c"
uint32_t nos_frame;static uint32_t clock_ms;static hal_gps fix;
static TaskFunction_t job;static void *argument;static int accept,leases,locks,update_owned,shutdown_pending;
int ota_busy(void){return update_owned;}
int nos_host_shutdown_pending(void){return shutdown_pending;}
int xTaskCreate(TaskFunction_t f,const char*n,unsigned stack,void*a,unsigned pri,void*h){if(!accept)return 0;job=f;argument=a;return pdPASS;}
void vTaskDelete(void*p){}
int board_sd_asset_acquire(void){leases++;return 1;}
void board_sd_asset_release(void){leases--;}
int hal_sports_sd_lock(void){assert(leases==1);locks++;return 1;}
void hal_sports_sd_unlock(void){locks--;}
void sports_emit(int t,const float*v,int n){}
void *hal_big_alloc(uint32_t n){return calloc(1,n);}
uint32_t hal_millis(void){return clock_ms;}
uint64_t hal_epoch_seconds(void){return 1792569600u;}
void hal_gps_enable(int on){}
int hal_gps_get(hal_gps*out){*out=fix;return 1;}
int main(void){
 update_owned=1;svc_track_start();assert(!T.active&&!T.pts);
 update_owned=0;shutdown_pending=1;svc_track_start();assert(!T.active&&!T.pts);
 shutdown_pending=0;
 fix.valid=1;svc_track_start();nos_frame++;clock_ms=1000;fix.lon=.0001;svc_track_tick(.0333f);assert(T.n==2);
 assert(svc_track_stop()==0&&svc_track_pending()&&!svc_track_saving());
 accept=1;assert(svc_track_stop()==-2&&svc_track_saving()&&svc_track_pending()&&!svc_track_active());
 int n=T.n;unsigned ver=T.ver;svc_track_start();assert(T.n==n&&T.ver==ver&&!T.active);
 assert(!leases&&!locks&&job);job(argument);
 assert(!svc_track_saving()&&!svc_track_pending()&&svc_track_save_result()>0&&!leases&&!locks);
 puts("PASS: device save queues off the UI, preserves immutable pending points across queue failure, owns/releases mount lease and lock before acknowledging success");return 0;}
''')

    def test_navigation_lost_fix_stop_and_stale(self):
        self.run_c("navigation_lost_fix", r'''
#include "apps/out_nav.c"
nos_time_t nos_t;float nos_dt=.0333f;uint32_t nos_frame;nos_prefs_t nos_prefs;
static hal_gps sample;static int active=1,pending,saving,present=1,stops,fastsave;
static svc_track_pt points[2]={{.lat=0,.lon=0},{.lat=0,.lon=.0001}};
void svc_track_tick(float dt){}
void hal_gps_enable(int on){}
int hal_gps_get(hal_gps*out){*out=sample;return present;}
uint32_t hal_ext_present(void){return present?EXT_GPS:0;}
int hal_mag_get(hal_mag*out){memset(out,0,sizeof *out);return 0;}
void *hal_big_alloc(uint32_t n){return calloc(1,n);}
void nos_feedback(int k){}
void nos_toast(const char*s){(void)s;}
void nos_keep_awake(void){}
int svc_track_active(void){return active;}
int svc_track_pending(void){return pending;}
int svc_track_saving(void){return saving;}
int svc_track_save_result(void){return pending?0:1;}
unsigned svc_track_version(void){return 0;}
int svc_track_points(const svc_track_pt**o){if(o)*o=points;return 2;}
double svc_track_distance(void){return 11.12;}
uint32_t svc_track_elapsed_ms(void){return 35000;}
int svc_track_stop(void){stops++;active=0;pending=!fastsave;return fastsave?-2:0;}
void svc_track_start(void){assert(!pending);active=1;}
void ui_trail_push(ui_trail*t,float x,float y){}
void ui_trail_reset(ui_trail*t,float x,float y){memset(t,0,sizeof *t);}
float gfx_fast_sqrt(float x){return sqrtf(x);}
int main(void){
 N.pl.init=N.pl.shown=N.present=N.fixed=N.have_last=N.show_now=1;
 N.last=(hal_gps){.valid=1,.lon=.0001};N.last_fix_t=0;N.fix_t=-10;N.ak_t=-10;N.val_t=-100;N.saved_t=-100;
 pg_init(&N.pg,NPAGE,0);msp_init(&N.arrow,0,.28f,1);nos_prefs.units_metric=1;
 for(int i=0;i<40;i++){nos_t=10+i*.0333f;nav_tick(.0333f);}
 assert(N.dim==1&&N.ak==A_STOP);
 for(int i=0;i<40;i++){nos_t=31+i*.0333f;nav_tick(.0333f);}
 assert(N.dim==1&&N.stale&&N.have_last&&!N.fixed&&N.ak==A_STOP);
 assert(on_rec_button(UI_CX,UI_BTN_Y));
 N.holding=1;N.hold_t0=nos_t-2;N.pressed=P_REC;
 nos_input cancel={.type=IN_CANCEL};assert(nav_input(&cancel)&&!N.holding&&!N.pressed);
 nav_tick(.0333f);assert(!stops);
 present=0;for(int i=0;i<40;i++){nos_t+=.0333f;nav_tick(.0333f);}
 assert(!N.present&&N.ak==A_STOP&&on_rec_button(UI_CX,UI_BTN_Y));
 stop_track();nos_t+=1;nav_tick(.0333f);nos_t+=1;
 assert(N.ak==A_RETRY&&on_rec_button(UI_CX,UI_BTN_Y));
 nos_input tap={.type=IN_TAP,.x=UI_CX,.y=UI_BTN_Y};assert(nav_input(&tap)&&stops==2);
 active=1;pending=0;present=1;sample=(hal_gps){.valid=1,.lat=0,.lon=.0001};
 for(int i=0;i<40;i++){nos_t+=.0333f;nav_tick(.0333f);}
 assert(N.fixed&&!N.stale&&N.dim==0);
 fastsave=1;stop_track();assert(N.save_wait);nav_tick(.0333f);assert(!N.save_wait&&N.saved_state==1&&N.ak==A_SUMMARY);
 puts("PASS: Stop survives prolonged loss/removal; retained guidance stays stale; Retry cannot start over pending points; live recovery clears stale");return 0;
}
''', ("nos/motion.c",))

    def test_memo_newest24_scan_and_year(self):
        fixture = OUT / "memo_fixture"
        fixture.mkdir(exist_ok=True)
        for i in range(1, 41):
            (fixture / f"M_20261021_0000{i:02d}_0000.wav").write_bytes(bytes(44 + 32000))
        (fixture / "M_20251021_235959_0000.wav").write_bytes(bytes(44))
        (fixture / "M_20271021_000000_0000.wav.part").write_bytes(bytes(44))
        self.run_c("memo_newest24", r'''
#define ESP_PLATFORM 1
#define MEMO_DIR "memo_fixture"
#include "apps/con_memo.c"
nos_prefs_t nos_prefs;
static TaskFunction_t queued;static void *argument;static int leases,locks;
int xTaskCreate(TaskFunction_t f,const char*n,unsigned stack,void*a,unsigned pri,void*h){queued=f;argument=a;return pdPASS;}
void vTaskDelete(void*p){}void vTaskDelay(unsigned t){}
int board_sd_asset_acquire(void){leases++;return 1;}
void board_sd_asset_release(void){leases--;}
void board_sd_asset_lock(void){assert(!locks);locks++;}
void board_sd_asset_unlock(void){assert(locks==1);locks--;}
void hal_audio_play_stop(void){}
void ui_list_set_n(ui_list*l,int n){}
const nos_screen*nos_top(void){return NULL;}
void nos_toast(const char*s){}
void *hal_big_alloc(uint32_t n){return calloc(1,n);}
void hal_get_datetime(hal_datetime*out){*out=(hal_datetime){.year=2026,.month=10,.day=21};}
int main(void){memo_load();assert(s_nmemo==0&&queued);queued(argument);assert(s_nmemo==0&&!leases&&!locks);memo_poll();assert(s_nmemo==24&&s_seq==41);
 assert(strstr(s_memos[0].path,"000040_0000.wav"));
 assert(strstr(s_memos[23].path,"000017_0000.wav"));
 assert(s_memos[0].year==2026&&s_memos[0].dur_ms==1000);
 for(int i=1;i<s_nmemo;i++)assert(s_memos[i-1].stamp>=s_memos[i].stamp);
 puts("PASS: all 41 finalized WAVs scanned; newest 24 selected; year retained; partial publication excluded");return 0;}
''')

    def test_memo_collision_and_async_completion(self):
        self.run_c("memo_async_collision", r'''
#include "apps/con_memo.c"
nos_time_t nos_t=10;nos_prefs_t nos_prefs;
static int status=0,attempts,feedback_success,feedback_error,stops,result=HAL_REC_ERROR,collision_budget;
static char last_path[100],toast[64];
int hal_rec_start_exclusive(const char*p){attempts++;strcpy(last_path,p);if(attempts<=2)return -2;if(collision_budget){collision_budget--;return -2;}status=1;return 1;}
int hal_rec_status(void){return status;}
int hal_rec_result(void){return result;}
void hal_rec_stop(void){stops++;if(status==1)status=2;}
uint32_t hal_rec_ms(void){return 4321;}
uint64_t hal_epoch_seconds(void){return 1792540800u;}
void hal_get_datetime(hal_datetime*out){*out=(hal_datetime){.year=2026,.month=10,.day=21,.hour=5,.minute=30};}
void ui_list_set_n(ui_list*l,int n){}
void nos_feedback(int k){if(k==FB_SUCCESS)feedback_success++;if(k==FB_ERROR)feedback_error++;}
void nos_toast(const char*s){snprintf(toast,sizeof toast,"%s",s);}
const nos_screen *nos_top(void){return NULL;}
void hal_audio_play_stop(void){}
void sports_emit(int t,const float*v,int n){}
int main(void){
 nos_prefs.tz_min=330;s_memos=calloc(MEMO_MAX,sizeof *s_memos);rec_start();
 assert(attempts==3&&strstr(last_path,"M_20261021_000000_0002.wav")&&s_rec_open);
 rec_start();assert(attempts==3);
 assert(svc_memo_stop()==1&&stops==1&&status==2&&s_rec_open&&feedback_success==0&&s_nmemo==0);
 svc_memo_tick(.0333f);assert(s_rec_open&&s_nmemo==0&&feedback_success==0);
 status=3;svc_memo_tick(.0333f);assert(!s_rec_open&&s_nmemo==1&&feedback_success==1);
 svc_memo_tick(.0333f);assert(s_nmemo==1&&feedback_success==1);
 assert(strstr(s_memos[0].path,"0002.wav")&&s_memos[0].dur_ms==4321&&s_memos[0].year==2026);
 rec_start();status=4;result=HAL_REC_EXISTS;int tried=attempts;svc_memo_tick(.0333f);
 assert(s_rec_open&&attempts==tried+1&&strstr(last_path,"0001.wav")&&feedback_error==0);
 status=4;result=HAL_REC_ERROR;svc_memo_tick(.0333f);
 assert(!s_rec_open&&s_nmemo==1&&feedback_success==1&&feedback_error==1&&!strcmp(toast,"SAVE FAILED"));
 status=3;collision_budget=20;rec_start();assert(s_rec_open&&s_rec_retry&&status==3);svc_memo_stop();svc_memo_tick(.0333f);
 assert(!s_rec_open&&s_nmemo==1&&feedback_success==1&&feedback_error==2); /* never accept the previous terminal success */
 for(int i=0;i<30;i++){char p[50];snprintf(p,sizeof p,"memo-%d",i);memo_add(p,1,10,21,0,0);}
 assert(s_nmemo==24&&!strcmp(s_memos[0].path,"memo-29")&&!strcmp(s_memos[1].path,"memo-28")&&!strcmp(s_memos[23].path,"memo-6"));
 puts("PASS: exclusive collision retries preserve UTC/year, finalizing never confirms saved, one completion publishes, error stays failed, capacity drops only oldest");return 0;
}
''')

    def test_compass_cover_and_invalid_level(self):
        self.run_c("compass_cover_level", r'''
#include "apps/out_compass.c"
nos_time_t nos_t=10;float nos_dt=.0333f;uint32_t nos_frame;
static int revealed,covered,calibrating=1,read_ok;static hal_imu sample;
int nos_revealed(void){return revealed;}
int nos_leaving_covered(void){return covered;}
void hal_mag_calibrate(int on){calibrating=on;}
int hal_mag_get(hal_mag*out){memset(out,0,sizeof *out);return 1;}
int hal_imu_read(hal_imu*out){*out=sample;return read_ok;}
uint32_t hal_ext_present(void){return EXT_MAG;}
void nos_invalidate(void){}
nos_time_t nos_enter_t(void){return nos_t;}
void nos_feedback(int k){}
float gfx_fast_sqrt(float x){return sqrtf(x);}
int main(void){C.mode=M_CAL;covered=1;compass_leave();revealed=1;compass_enter();assert(calibrating&&C.mode==M_CAL);
 covered=0;compass_leave();assert(!calibrating);
 C.level_ok=1;tick_level(.0333f);assert(!C.level_valid&&!C.level_ok&&C.level_init);
 read_ok=1;sample=(hal_imu){.az=1};tick_level(.0333f);assert(C.level_valid&&C.level_ok);
 sample.ax=NAN;tick_level(.0333f);assert(!C.level_valid&&!C.level_ok);
 sample=(hal_imu){0};tick_level(.0333f);assert(!C.level_valid&&!C.level_ok);
 sample=(hal_imu){.ax=.4f,.az=1};tick_level(.0333f);assert(C.level_valid&&!C.level_ok&&C.tilt_shown>10);
 puts("PASS: cover preserves calibration, real leave stops it; missing/NaN/zero IMU cannot qualify FLAT; valid recovery reflects tilt");return 0;}
''', ("nos/motion.c",))

    def test_sms_identity_pending_result_and_leases(self):
        self.run_c("sms_identity_async", r'''
#include "apps/con_cell.c"
nos_time_t nos_t=10;uint32_t nos_frame;nos_prefs_t nos_prefs;
const font_t *UI_F_BODY;
static hal_sms backend[2];static uint32_t marked,request=71;static int state=1,success,error,pops,renewals;
static char recipient[30];
void *hal_big_alloc(uint32_t n){return calloc(1,n);}
int hal_lte_sms_list(hal_sms*out,int n){memcpy(out,backend,2*sizeof *out);return 2;}
void hal_lte_sms_mark_read_id(uint32_t id){marked=id;for(int i=0;i<2;i++)if(backend[i].id==id)backend[i].unread=0;}
uint32_t hal_lte_sms_send_request(const char*to,const char*text){strcpy(recipient,to);return request;}
int hal_lte_sms_send_status(uint32_t id){assert(id==71);return state;}
void hal_lte_enable(int on){assert(on);renewals++;}
uint32_t hal_ext_present(void){return EXT_LTE;}
int nos_revealed(void){return 0;}
int nos_transitioning(void){return 0;}
nos_time_t nos_enter_t(void){return nos_t;}
void nos_pop(void){pops++;}
void nos_push(const nos_screen*s,int tx){}
void nos_feedback(int f){if(f==FB_SUCCESS)success++;if(f==FB_ERROR)error++;}
int ui_text_width(const font_t*f,const char*s,int tracking){return (int)strlen(s)*10;}
void ui_list_init(ui_list*l,int n,int h,int top,ui_row_fn row,void(*tap)(int)){memset(l,0,sizeof *l);l->n=n;l->row_h=h;l->top=top;}
void ui_list_set_n(ui_list*l,int n){l->n=n;}
void ui_list_tick(ui_list*l,float dt){}
int ui_list_input(ui_list*l,const nos_input*in){return 1;}
int main(void){
 nos_prefs.lte=1;backend[0]=(hal_sms){.id=42,.unread=1};strcpy(backend[0].from,"+111");strcpy(backend[0].text,"First");
 backend[1]=(hal_sms){.id=99,.unread=1};strcpy(backend[1].from,"+222");
 sms_refresh();inbox_tap(0);hal_sms tmp=backend[0];backend[0]=backend[1];backend[1]=tmp;
 read_enter();assert(marked==42&&backend[0].unread==1&&!backend[1].unread&&!strcmp(s_selected.text,"First"));
 R.pend=0;nos_input cancel={.type=IN_CANCEL};read_input(&cancel);assert(R.pend==-1);
 sms_refresh();assert(s_sms[0].id==99&&s_sms[0].unread&&s_sms[1].id==42&&!s_sms[1].unread);
 reply_send(0);assert(R.result==2&&!success&&!strcmp(recipient,"+111"));
 nos_t+=20;read_tick(.0333f);assert(R.result==2&&!success&&!pops);
 state=4;read_tick(.0333f);assert(R.result==-1&&error==1&&!pops);
 nos_t+=2;read_tick(.0333f);assert(!R.result);
 reply_send(1);state=3;read_tick(.0333f);assert(R.result==1&&success==1&&!pops);
 nos_t+=2;read_tick(.0333f);assert(pops==1);
 inbox_enter();for(int i=0;i<30;i++){nos_t+=1;inbox_tick(.0333f);}
 assert(renewals>30);request=0;reply_send(0);assert(R.result==-1&&success==1);
 puts("PASS: selected message snapshot/ID survives reordering; reply recipient stable; queue stays pending until success/failure; foreground LTE demand renewed beyond 8s");return 0;}
''', ("nos/motion.c",))

    def test_alarm_editor_rejects_replacement_before_commit(self):
        body = r'''
#include "nos.h"
#include "ui.h"
#include "svc.h"
static struct {int gone,valid,shown;nos_time_t del_at;} E;
static struct {int pressed,empty_pressed,leaving,press_bound;uint32_t press_generation,press_id;nos_time_t unpress_at;} A;
static int s_edit_idx=0,s_edit_new,a_svc=1,s_edit_conflict;
static uint32_t s_edit_generation=10,s_edit_id=100,generation=10,id=100;
static svc_alarm s_draft,alarm;static int saves,adds,pops,toasts,transition;
const nos_screen scr_alarm_edit={0};
uint32_t svc_alarm_generation(void){return generation;}
uint32_t svc_alarm_id(int i){return i==0?id:0;}
static int al_count(void){return 1;}
static svc_alarm*al_get(int i){return &alarm;}
static void al_save(void){saves++;}
static int al_add(int h,int m){adds++;generation++;id++;return 0;}
static void ed_delete(void){assert(0);}
int nos_leaving_covered(void){return 0;}
int nos_transitioning(void){return transition;}
const nos_screen*nos_top(void){return &scr_alarm_edit;}
void nos_pop(void){pops++;}
void nos_toast(const char*s){toasts++;assert(!strcmp(s,"ALARMS UPDATED")||!strcmp(s,"List full"));}
'''
        for n in ("alarm_press_valid", "ed_valid", "ed_reconcile", "ed_commit", "edit_leave"):
            body += function("nos/apps/time_alarm.c", n)
        body += r'''
int main(void){
 alarm=(svc_alarm){.h=9,.m=45,.on=1};assert(ed_valid());
 A.press_bound=1;A.press_generation=generation;A.press_id=id;assert(alarm_press_valid(0));
 generation++;id=200;assert(!ed_valid());assert(!ed_reconcile()&&pops==1&&toasts==1);
 assert(!alarm_press_valid(0));
 edit_leave();assert(alarm.h==9&&alarm.m==45&&!saves&&!adds);
 s_edit_idx=-1;s_edit_new=1;s_edit_conflict=0;s_edit_generation=generation;s_edit_id=0;
 s_draft=(svc_alarm){.h=7,.m=15,.days=0x3e,.on=1};ed_commit();
 assert(adds==1&&saves==1&&s_edit_idx==0&&s_edit_id==id&&s_edit_generation==generation&&ed_valid());
 generation++;s_edit_new=1;edit_leave();assert(adds==1&&saves==1);
 puts("PASS: remote same-index replacement invalidates before save; new drafts bind updated ID/generation after commit");return 0;}
'''
        self.run_c("alarm_editor_identity", body)

    def test_ended_alarm_and_timer_callbacks_close(self):
        # Extract just the current alert callbacks so unrelated drawing is never stubbed into passing.
        for kind in ("alarm", "timer"):
            if kind == "alarm":
                body = r'''
#include "nos.h"
#include "svc.h"
#include "ui.h"
static struct {nos_time_t t0,snz_fire,snz_unpress;float build;int leaving,snz_pressed,grab,committed;mspring knob;} R;
static int active=-1,pops,transition;static const nos_screen screen={0};
#define scr_alarm_ring screen
#define BUILD_F 12
int svc_alarm_ringing(void){return active;}
uint32_t svc_alarm_id(int i){return i>=0?(uint32_t)i+1:0;}
uint32_t svc_alarm_generation(void){return 1;}
void svc_alarm_stop(void){active=-1;}
static void ring_leave(void){}
static void snooze(void){}
static float qz(float x,float n){return floorf(x*n+.5f)/n;}
static float sl_x0(void){return 0;}static float sl_x1(void){return 1;}
static int bk(float x){return (int)x;}static float scale_k(int x,int y){return 0;}
'''
            else:
                body = r'''
#include "nos.h"
#include "svc.h"
#include "ui.h"
typedef struct {int state;uint32_t total,left;}tm_t;
static struct {nos_time_t t0,fire_at,unpress_at;float build;int fire,leaving,pressed,haptic_on;}RG;
static int active,pops,transition;static const nos_screen screen={0};
#define scr_timer_ring screen
#define BUILD_F 12
#define RGP_NONE 0
#define RG_BTN_W 140
static void tm_poll(void){}static void tm_get(tm_t*t){t->state=active;}
static void ring_leave(void){}static void ring_act(int h){}
static int bk(float x){return (int)x;}static float scale_k(int x,int y){return 0;}
static float build_scale(float x){return x;}
void hal_haptic_stop(void){}
'''
            body += r'''
nos_time_t nos_t=100;
const nos_screen*nos_top(void){return &screen;}
int nos_transitioning(void){return transition;}
void nos_keep_awake(void){}
void nos_pop(void){pops++;}
'''
            body += function("nos/apps/time_" + kind + ".c", "ring_tick")
            body += r'''
int main(void){transition=1;ring_tick(.0333f);assert(!pops);transition=0;ring_tick(.0333f);assert(pops==1);
 puts("PASS: ended covered alert closes on reveal after transition");return 0;}
'''
            self.run_c("ended_" + kind + "_callback", body, ("nos/motion.c",))

    def test_call_generation_and_ended_reveal(self):
        self.run_c("call_generation_reveal", r'''
#include "apps/con_call.c"
nos_time_t nos_t=10;nos_prefs_t nos_prefs;const font_t *UI_F_TITLE;
static svc_call_t call;static uint32_t generation=1;static int pops,transition;
const svc_call_t *svc_call(void){return &call;}
uint32_t svc_call_generation(void){return generation;}
uint32_t hal_millis(void){return (uint32_t)(nos_t*1000);}
nos_time_t nos_enter_t(void){return nos_t;}
int ui_text_width(const font_t*f,const char*s,int tracking){return (int)strlen(s)*10;}
void nos_keep_awake(void){}
int nos_transitioning(void){return transition;}
void nos_pop(void){pops++;}
float gfx_fast_sqrt(float x){return sqrtf(x);}
int main(void){call.state=1;strcpy(call.name,"Caller A");call_enter();assert(C.st==CS_INCOMING);
 call.state=2;nos_t+=1;call_tick(.0333f);assert(C.st==CS_ACTIVE);
 call.state=0;nos_t+=1;call_tick(.0333f);assert(C.st==CS_ENDED);
 call.state=1;generation++;call.name[0]=0;strcpy(call.number,"+222");call_tick(.0333f);
 assert(C.st==CS_INCOMING&&C.generation==2&&!C.name[0]&&!C.popped);
 call.state=0;call_enter();transition=1;call_tick(.0333f);assert(!pops);transition=0;call_tick(.0333f);assert(pops==1);
 puts("PASS: new call session replaces active/ended UI with fresh identity; ended covered reveal exits after transition");return 0;}
''', ("nos/motion.c",))

    def test_find_watch_ended_on_reveal(self):
        self.run_c("find_watch_ended", r'''
#include "apps/con_find.c"
nos_time_t nos_t=10;static int active,override=-1,pops,transition;
int svc_find_watch_active(void){return active;}
void hal_haptic_stop(void){}void hal_sfx_stop(void){}
void nos_brightness_override(int v){override=v;}
void nos_keep_awake(void){}
int nos_transitioning(void){return transition;}
const nos_screen*nos_top(void){return &scr_find_watch;}
void nos_pop(void){pops++;}
int ui_label_width(const char*s){return (int)strlen(s)*8;}
void *hal_big_alloc(uint32_t n){return NULL;}
float gfx_fast_sin(float x){return sinf(x);}float gfx_fast_cos(float x){return cosf(x);}
float gfx_fast_sqrt(float x){return sqrtf(x);}
int ui_ripple_blur=1;
uint16_t ui_dim(uint16_t c,float k){return c;}
int main(void){fw_enter();assert(override==-1);transition=1;fw_tick(.0333f);assert(!pops);transition=0;fw_tick(.0333f);assert(pops==1&&override==-1);
 active=1;fw_enter();assert(override==255);active=0;fw_tick(.0333f);assert(pops==2&&override==-1);
 puts("PASS: expired Find Watch closes even when already ended on reveal, avoids raising brightness, restores active alert override");return 0;}
''', ("nos/motion.c",))

    def test_torch_reveal_and_seven_day_animation_origin(self):
        self.run_c("torch_reveal_origin", r'''
#include "apps/con_torch.c"
nos_time_t nos_t=604800.000123;nos_prefs_t nos_prefs;static int revealed,override;
int nos_revealed(void){return revealed;}
void nos_brightness_override(int n){override=n;}
void nos_keep_awake(void){}void nos_feedback(int n){}
static uint32_t clock_ms;uint32_t hal_millis(void){return clock_ms;}
void hal_power_get(hal_power*p){memset(p,0,sizeof*p);p->sample_valid=p->battery_valid=1;p->battery_percent=80;}
int nos_transitioning(void){return 0;}void nos_pop(void){}
float gfx_fast_sqrt(float x){return sqrtf(x);}
int main(void){torch_enter();assert(T.t_enter==nos_t);torch_tick(1.0f/NOS_FPS);
 switch_mode();T.level[1]=.2f;torch_tick(1.0f/NOS_FPS);int red=override;assert(T.mode==1&&red<50);
 T.dragging=1;nos_input cancel={.type=IN_CANCEL};assert(torch_input(&cancel)&&!T.dragging&&T.mode==1&&T.level[1]==.2f);
 torch_leave();assert(override==-1&&!nos_prefs.torch);revealed=1;nos_t+=4;torch_enter();assert(T.mode==1&&T.level[1]==.2f&&override==red&&nos_prefs.torch);
 torch_tick(1.0f/NOS_FPS);assert(override==red);
 revealed=0;nos_t+=1;torch_enter();assert(!T.mode&&T.level[0]==1&&T.t_enter==nos_t);
 T.t0=nos_t;assert(T.t0==nos_t);for(int i=1;i<=4;i++){nos_t=T.t0+(double)i/NOS_FPS;torch_tick(1.0f/NOS_FPS);}assert(T.p==1);
 puts("PASS: Torch preserves red/dim settings through cover, restores fresh defaults on new visit, seven-day origins preserve frame-scale precision");return 0;}
''', ("nos/motion.c",))

    def test_humidity_capability_preserves_true_zero(self):
        self.run_c("humidity_capability", r'''
#include "apps/out_baro.c"
int main(void){B.valid=1;B.env.humidity=0;B.env.humidity_valid=0;assert(!humidity_available());
 B.env.humidity_valid=1;assert(humidity_available());B.env.humidity=NAN;assert(!humidity_available());
 B.env.humidity=30;B.valid=0;assert(!humidity_available());
 puts("PASS: BMP/missing humidity unavailable; genuine measured zero remains valid; NaN and invalid climate state rejected");return 0;}
''')

    def test_music_hardware_queue_and_lease(self):
        self.run_c("music_gesture_lease", r'''
#include "apps/con_music.c"
nos_time_t nos_t;nos_prefs_t nos_prefs;static int renewals,drains,enabled,gesture,foreground=1,transition,command=-1;
void hal_light_gesture_enable(int on){enabled=on;if(on)renewals++;}
int hal_light_gesture(void){drains++;int g=gesture;gesture=0;return g;}
const nos_screen*nos_top(void){return foreground?&scr_music:NULL;}
int nos_transitioning(void){return transition;}
void svc_music_cmd(int c){command=c;}
void nos_feedback(int f){}
void nos_air(int g){nos_input in={.type=IN_AIR,.x=g};music_input(&in);}
int main(void){P.have=1;M.vol=50;nos_t=604800.000123;
 for(int i=0;i<600;i++){nos_t+=1.0/NOS_FPS;if(i==500)gesture=GEST_UP;music_gestures();}
 assert(renewals==600&&drains>=600&&M.vol==60&&command==MUSIC_VOLUP);
 gesture=GEST_LEFT;music_gestures();assert(command==MUSIC_PREV);
 foreground=0;gesture=GEST_RIGHT;int old=drains;music_gestures();assert(drains==old);
 music_leave();assert(!enabled);foreground=1;transition=1;music_gestures();assert(drains==old);
 transition=0;P.have=0;music_gestures();assert(!enabled&&drains==old);
 puts("PASS: production APDS queue delivers Music commands beyond eight seconds, stays bounded to active consumer, releases on cover/leave");return 0;}
''', ("nos/motion.c",))

    def test_time_service_boundary_positives(self):
        self.run_c("time_service_positives", r'''
#include "svc.c"
static uint32_t clock_ms;uint32_t hal_millis(void){return clock_ms;}
int main(void){clock_ms=0xfffffff0u;svc_sw_start();clock_ms=0x50u;svc_sw_stop();assert(svc_stopwatch()->elapsed_ms==96);
 svc_sw_reset();svc_sw_start();for(int i=0;i<SVC_LAPS+10;i++){clock_ms++;svc_sw_lap();}assert(svc_stopwatch()->nlaps==SVC_LAPS);
 svc_sw_stop();svc_sw_reset();assert(!svc_stopwatch()->running&&!svc_stopwatch()->elapsed_ms&&!svc_stopwatch()->nlaps);
 clock_ms=0xfffffff0u;svc_timer_start(1000);clock_ms=0x54u;svc_timer_pause();assert(svc_timer()->state==2&&svc_timer()->left_ms==900);
 clock_ms=0x1000u;svc_timer_resume();assert(s_timer_end==clock_ms+900);svc_timer_add(60000);
 assert(svc_timer()->total_ms==61000&&svc_timer()->left_ms==60900);svc_timer_cancel();assert(!svc_timer()->state&&!svc_timer()->left_ms);
 puts("PASS: audited positives retained for stopwatch uptime wrap, lap capacity/reset, timer wrap/pause/resume/add/cancel");return 0;}
''')

    def test_alarm_service_add_persists_count_and_identity(self):
        self.run_c("alarm_service_persistence", r'''
#include "svc.c"
static int saved_count,writes;
void hal_store_save(const char*key,const void*p,int n){assert(!strcmp(key,"nosalarm"));saved_count=((const unsigned char*)p)[1];writes++;}
int hal_store_save_checked(const char*key,const void*p,int n){hal_store_save(key,p,n);return 1;}
int main(void){uint32_t gen=svc_alarm_generation();int i=svc_alarm_add(7,15);assert(i==0&&writes==1&&saved_count==1);
 uint32_t id=svc_alarm_id(i);assert(id&&svc_alarm_generation()!=gen&&svc_alarm_find(id)==0);
 gen=svc_alarm_generation();svc_alarm_get(0)->m=30;svc_alarm_save();assert(svc_alarm_id(0)==id&&svc_alarm_generation()==gen);
 assert(svc_alarm_add(25,15)==-1&&svc_alarm_count()==1);
 puts("PASS: service add persists incremented count; alarm identity survives ordinary edit/save and invalid times rejected");return 0;}
''')

    def test_owned_app_syntax_and_long_uptime_origins(self):
        examples = {
            "con_call": "C.t_enter", "con_cell": "R.t_result", "con_find": "W.t_enter",
            "con_memo": "R.m0", "con_music": "G.fs_shown", "con_torch": "T.t_enter",
            "time_alarm": "E.arm_t0", "time_timer": "T.build_t0", "time_worldclock": "WL.empty_t0",
            "time_stopwatch": "S.hand_t0", "out_nav": "N.you_t0", "out_compass": "C.ro_changed",
            "out_weather": "s_conn_since", "out_baro": "K.gps_t",
        }
        for app, field in examples.items():
            self.run_c("origin_" + app, '#include "apps/' + app + '.c"\nint main(void){\n' +
                       field + '=604800.000123;assert(' + field + '==604800.000123);\n' +
                       'puts("PASS: owned source compiles and absolute origin retains subframe precision at seven days");return 0;}\n')

    def test_covered_service_alert_ownership(self):
        self.run_c("covered_service_ownership", r'''
#include "svc.c"
const nos_screen scr_alarm_ring={.name="ALARM"},scr_timer_ring={.name="TIMER"},scr_call={.name="CALL"},scr_find_watch={.name="FIND"};
static const nos_screen root={.name="ROOT"};static const nos_screen*stack[8];static int depth;static uint32_t clock_ms;
const nos_screen*nos_top(void){return stack[depth-1];}
int nos_contains(const nos_screen*s){for(int i=0;i<depth;i++)if(stack[i]==s)return 1;return 0;}
int nos_transitioning(void){return 0;}
void nos_push(const nos_screen*s,int tx){stack[depth++]=s;}
void nos_pop(void){assert(depth>1);depth--;}
uint32_t hal_millis(void){return clock_ms;}
int main(void){stack[0]=&root;depth=1;ring_scr alarm={&scr_alarm_ring,0,0,0};
 ring_screen(&alarm,1);nos_push(&scr_find_watch,TX_RISE);ring_screen(&alarm,0);assert(alarm.pushed&&depth==3);
 nos_pop();clock_ms=10000;ring_screen(&alarm,0);assert(!alarm.pushed&&depth==1&&nos_top()==&root);
 puts("PASS: covered ended alert remains owned until actual stack removal; no orphan survives after cover dismissal");return 0;}
''')

    def test_weather_service_preserves_metadata_and_invalidation(self):
        self.run_c("weather_service_metadata", r'''
#include "svc.c"
static hal_weather cache;
void hal_weather_get(hal_weather*out){*out=cache;}
uint64_t hal_epoch_seconds(void){return 1792569600u;}
void hal_get_datetime(hal_datetime*out){*out=(hal_datetime){.year=2026,.month=10,.day=21,.hour=10};}
int main(void){cache.valid=1;cache.stale=1;cache.age_seconds=172800;cache.generation=17;cache.utc_offset_seconds=20700;cache.observed_epoch=1792396800;
 strcpy(cache.city,"Old city");strcpy(cache.status,"Fetch failed");cache.temp=31;cache.hours[0].epoch=1792569600;cache.days[0].epoch=1792540800;
 wx_from_hal();assert(s_wx.valid&&s_wx.stale&&s_wx.age_seconds==172800&&s_wx.generation==17&&s_wx.source==2);
 assert(s_wx.utc_offset_seconds==20700&&s_wx.hours_epoch[0]==cache.hours[0].epoch&&s_wx.days_epoch[0]==cache.days[0].epoch&&!strcmp(s_wx.status,"Fetch failed"));
 cache.valid=0;cache.generation++;cache.fetching=1;strcpy(cache.city,"New city");wx_from_hal();assert(!s_wx.valid&&!strcmp(s_wx.loc,"New city")&&s_wx.fetching&&s_wx.generation==18&&!s_wx.nhours);
 puts("PASS: actual service preserves HAL age/source/location epochs and clears old feed on explicit cache invalidation");return 0;}
''', ("nos/gb.c",))

    def test_worldclock_full_calendar_offsets(self):
        self.run_c("worldclock_dates", r'''
#include "apps/time_worldclock.c"
nos_prefs_t nos_prefs;static hal_datetime sample;static uint64_t epoch;
void hal_get_datetime(hal_datetime*out){*out=sample;}
uint64_t hal_epoch_seconds(void){return epoch;}
int main(void){
 epoch=(uint64_t)(days_civil(2026,10,21)*86400+16*3600);
 sample=(hal_datetime){.year=2026,.month=10,.day=22,.weekday=4,.hour=5};nos_prefs.tz_min=780;
 ctime_t london;city_time(4,&london);assert(local_offset_min(&sample)==780);
 assert(london.h==17&&london.wd==3&&london.dayrel==-1&&london.rel_min==-720);
 epoch++;assert(local_offset_min(&sample)==780);epoch--; /* reads may straddle a second */
 int offsets[]={-720,720,780,840,345};
 for(unsigned i=0;i<sizeof offsets/sizeof *offsets;i++){
  int mins=16*60+offsets[i],day=21;while(mins<0){mins+=1440;day--;}while(mins>=1440){mins-=1440;day++;}
  sample.day=day;sample.hour=mins/60;sample.minute=mins%60;
  assert(local_offset_min(&sample)==offsets[i]);
 }
 epoch=0;nos_prefs.tz_min=-720;assert(local_offset_min(&sample)==-720);
 puts("PASS: UTC-12/+12/+13/+14 and quarter-hour offsets retain full civil-day identity; London date-line relation correct");return 0;}
''')

    def test_weather_acquisition_epochs_and_invalidated_feed(self):
        self.run_c("weather_epochs_freshness", r'''
#include "apps/out_weather.c"
nos_time_t nos_t;uint32_t nos_frame;nos_prefs_t nos_prefs;static uint64_t epoch=1792623600u;
static svc_weather_t feed;static svc_sys_t system_state;
uint64_t hal_epoch_seconds(void){return epoch;}
const svc_weather_t *svc_weather(void){return &feed;}
const svc_sys_t *svc_sys(void){return &system_state;}
float gfx_fast_sin(float x){return sinf(x);}
const char *ui_weekday3(int d){static const char*x[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};return x[d%7];}
int main(void){
 feed.valid=1;feed.source=2;feed.updated_epoch=(uint32_t)epoch-7200;feed.utc_offset_seconds=20700;
 feed.nhours=3;feed.hours_epoch[0]=(int64_t)epoch-3600;feed.hours_epoch[1]=(int64_t)epoch+3600;feed.hours_epoch[2]=(int64_t)epoch+7200;
 feed.hours_c[0]=10;feed.hours_c[1]=20;feed.hours_c[2]=30;feed.ndays=2;
 int64_t today=wx_local_day((int64_t)epoch,feed.utc_offset_seconds)*86400-feed.utc_offset_seconds;
 feed.days_epoch[0]=today-86400;feed.days_epoch[1]=today;
 svc_weather_t a;wx_prepare(&a,&feed);assert(a.nhours==2&&a.hours_c[0]==20&&a.ndays==1&&a.days_epoch[0]==today&&a.stale&&a.age_seconds==7200);
 char label[40],same[40];wx_hour_label(&a,0,label,sizeof label);assert(!strcmp(label,"05:45"));
 assert(!strcmp(wx_day_label(&a,0),"TODAY"));wx_age_label(&a,same,sizeof same);assert(strstr(same,"STALE")&&strstr(same,"2H"));
 epoch+=1800;wx_prepare(&a,&feed);wx_hour_label(&a,0,same,sizeof same);assert(!strcmp(label,same));
 wx_view_tick();assert(s_cur.ws==WS_OK&&s_cur.d.stale);
 nos_frame++;feed.valid=0;strcpy(feed.loc,"New city");strcpy(feed.status,"Fetch failed");wx_view_tick();assert(s_cur.ws!=WS_OK&&!s_cur.d.valid&&!strcmp(s_cur.d.loc,"New city"));
 nos_frame++;feed.fetching=1;wx_view_tick();assert(s_cur.ws==WS_UPDATING);
 epoch+=3*86400;wx_prepare(&a,&feed);assert(!a.nhours&&!a.ndays);
 puts("PASS: forecast labels remain tied to location epochs, expired entries removed, stale age shown, invalidation clears current UI, fetching is explicit");return 0;}
''')


if __name__ == "__main__":
    unittest.main(verbosity=2)
