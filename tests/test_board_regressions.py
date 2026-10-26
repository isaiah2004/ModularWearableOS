"""Current board functions against acquisition, I2C and delayed-I/O faults.

Private objects are separately compiled with the real IDF. These harnesses run
the actual state-machine functions, with deterministic drivers and worker delays.
They do not exercise a device or make motion/clock accuracy claims.
"""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/alpha05/checks/platform-board/regressions"
BOARD = (ROOT / "firmware/main/board.c").read_text()
HEADER = (ROOT / "firmware/main/board.h").read_text()


def function(name, occurrence=0):
    matches = list(re.finditer(r"^(?:static )?(?:void|int|uint32_t|esp_err_t|hal_imu|board_log_snapshot|board_touch_diag) " +
                              name + r"\([^;\n]*\)\s*\{", BOARD, re.M))
    assert len(matches) > occurrence, name
    match = matches[occurrence]
    end, depth = match.end(), 1
    while depth:
        depth += (BOARD[end] == "{") - (BOARD[end] == "}")
        end += 1
    return BOARD[match.start():end] + "\n"


ENUMS = "\n".join(re.findall(r"enum \{ BOARD_.*?\};", HEADER, re.S))
TYPES = "\n".join(re.findall(r"typedef struct \{[^}]+\} board_(?:device|touch)_diag;", HEADER))
COMMON = r'''
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdatomic.h>
#include <time.h>
#include "hal.h"
#include "pedometer.h"
#include "sports.h"
#include "tuning.h"
#include "board_protocol.h"
#define BOARD_HAS_IMU 1
#define BOARD_HAS_RTC 1
#define BOARD_HAS_SD 1
#define CONFIG_WATCH_BOARD_143C 0
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 11
#define ESP_ERR_INVALID_RESPONSE 12
#define ESP_ERR_NOT_SUPPORTED 13
#define ESP_ERR_INVALID_STATE 14
#define ESP_FAIL 15
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(n) (n)
#define portENTER_CRITICAL(p) ((void)0)
#define portEXIT_CRITICAL(p) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef int esp_err_t;
typedef int SemaphoreHandle_t;
typedef int i2c_master_dev_handle_t;
typedef int i2c_master_bus_handle_t;
typedef struct { unsigned device_address,scl_speed_hz; } i2c_device_config_t;
static _Atomic int64_t clock_us;
static time_t clock_epoch=1792540800;
static int64_t esp_timer_get_time(void) { return atomic_load(&clock_us); }
static time_t mock_time(time_t *out) { if(out)*out=clock_epoch;return clock_epoch; }
#define time mock_time
static int prefs_are_loading;
int prefs_loading(void){return prefs_are_loading;}
static atomic_int sem_tokens[16];
static int xSemaphoreTake(int which,unsigned wait) {
 assert(wait==0);int token=atomic_load(&sem_tokens[which]);
 return token && atomic_compare_exchange_strong(&sem_tokens[which],&token,0);
}
static void xSemaphoreGive(int which) { atomic_store(&sem_tokens[which],1); }
''' + ENUMS + "\n" + TYPES + "\n"

DEVICE_STATE = r'''
#define TOUCH_ADDR 0x38
#define IMU_ADDR 0x6b
#define RTC_ADDR 0x51
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_touch,s_imu,s_rtc;
static atomic_int s_touch_ok,s_imu_ok,s_rtc_ok,s_rtc_valid;
enum { IMU_MODE_LEGACY = 0, IMU_MODE_FIFO = 1, IMU_MODE_POLL = 2 };
static int s_imu_mode;
static atomic_int *const s_device_ok[BOARD_DEVICE_COUNT]={&s_touch_ok,&s_imu_ok,&s_rtc_ok};
static SemaphoreHandle_t s_device_lock[BOARD_DEVICE_COUNT]={1,2,3};
static board_device_diag s_devices[BOARD_DEVICE_COUNT];
static int64_t s_device_retry_at[BOARD_DEVICE_COUNT];
static unsigned s_device_backoff[BOARD_DEVICE_COUNT];
static atomic_int s_retry_devices;
static _Atomic uint32_t s_touch_reads,s_touch_errors,s_touch_injected,s_touch_pending;
static _Atomic int s_touch_last_error;
static int adds[128],probes[128],probe_error[128],read_error[128],write_error[128];
static int write_seen[128],write_fail_nth[128];
static int id_bad,rtc_fail_stage,rtc_invalidate_error,rtc_invalidations,rtc_restart_calls,calendar_calls;
static uint8_t touch_packet[5]={1,0x80,100,0,120},calendar[7];
static void (*on_calendar)(void);
static int i2c_master_bus_add_device(int bus,const i2c_device_config_t *cfg,int *handle) {
 (void)bus;assert(!*handle);adds[cfg->device_address]++;*handle=(int)cfg->device_address;return 0;
}
static int i2c_master_probe(int bus,unsigned addr,unsigned timeout) {
 (void)bus;assert(timeout==30);probes[addr]++;return probe_error[addr];
}
static int i2c_master_transmit_receive(int dev,const uint8_t *reg,unsigned rn,void *out,size_t n,unsigned timeout) {
 assert(rn==1 && (timeout==0||timeout==30));
 if(read_error[dev])return read_error[dev];
 if(dev==TOUCH_ADDR){assert(*reg==2&&n==5);memcpy(out,touch_packet,5);return 0;}
 if(dev==IMU_ADDR){memset(out,0,n);if(!*reg)*(uint8_t*)out=id_bad?0:5;return 0;}
 assert(dev==RTC_ADDR&&!*reg&&n==1);if(rtc_fail_stage==1)return -101;
 *(uint8_t*)out=0x85;return 0;
}
static int i2c_master_transmit(int dev,const uint8_t *data,size_t n,unsigned timeout) {
 assert(timeout==30);write_seen[dev]++;
 if(write_fail_nth[dev]&&write_seen[dev]==write_fail_nth[dev])return -88;
 if(write_error[dev])return write_error[dev];
 if(dev!=RTC_ADDR)return 0;
 if(n==8){assert(data[0]==4);calendar_calls++;memcpy(calendar,data+1,7);if(on_calendar)on_calendar();return rtc_fail_stage==3?-103:0;}
 assert(n==2);
 if(data[0]==4){assert(data[1]==0x80);rtc_invalidations++;return rtc_invalidate_error?-106:0;}
 assert(!data[0]);
 if(data[1]&0x20)return rtc_fail_stage==2?-102:0;
 rtc_restart_calls++;return rtc_fail_stage==4?-104:0;
}
static void init_devices(void) { for(int i=1;i<=3;i++)atomic_store(&sem_tokens[i],1); }
'''


def device_functions():
    return "\n".join(function(name) for name in (
        "increment_sat", "device_result", "device_begin", "device_end", "rd", "wr", "device",
        "configure_device", "retry_devices", "touch_rd", "board_imu_raw", "board_device_diagnostics"))


RTC_STATE = r'''
static time_t s_rtc_pending;
static uint32_t s_rtc_generation;
static int s_rtc_dirty,s_system_trusted;
static unsigned s_rtc_failed_operation;
static esp_err_t s_rtc_last_error;
static uint32_t s_rtc_failures;
static int64_t s_rtc_retry_at;
'''


@unittest.skipUnless(shutil.which("gcc"), "gcc required")
class BoardRegressions(unittest.TestCase):
    def run_c(self, name, code, extras=(), flags=()):
        OUT.mkdir(parents=True, exist_ok=True)
        os.environ["TEMP"] = os.environ["TMP"] = str(OUT)
        path, exe = OUT / (name + ".c"), OUT / (name + ".exe")
        path.write_text(code)
        command = [shutil.which("gcc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-Wno-unused-variable", "-I" + str(ROOT / "core"),
                   "-I" + str(ROOT / "nos"), "-I" + str(ROOT / "firmware/main"), str(path),
                   *[str(ROOT / item) for item in extras], *flags, "-lm", "-o", str(exe)]
        result = subprocess.run(command, cwd=OUT, text=True, capture_output=True, timeout=60)
        (OUT / (name + ".compile.log")).write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        result = subprocess.run([str(exe)], cwd=OUT, text=True, capture_output=True, timeout=30)
        (OUT / (name + ".log")).write_text(result.stdout + result.stderr)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("pass", result.stdout.lower())

    def test_touch_invalid_and_boot_reprobe_stable_handles(self):
        code = COMMON + DEVICE_STATE + device_functions() + function("board_touch_read", 1) + r'''
int main(void) {
 init_devices();probe_error[TOUCH_ADDR]=-91;probe_error[IMU_ADDR]=-92;probe_error[RTC_ADDR]=-93;
 retry_devices(1);assert(!s_touch_ok&&!s_imu_ok&&!s_rtc_ok);
 assert(adds[TOUCH_ADDR]==1&&adds[IMU_ADDR]==1&&adds[RTC_ADDR]==1);
 assert(s_device_retry_at[0]==1000000&&s_devices[0].first_error==-91);
 int x=333,y=444;assert(board_touch_read(&x,&y)==BOARD_TOUCH_INVALID&&x==333&&y==444);
 atomic_store(&clock_us,999999);retry_devices(0);assert(probes[TOUCH_ADDR]==1);
 for(int i=0;i<128;i++)probe_error[i]=0;
 atomic_store(&clock_us,1000000);retry_devices(0);
 assert(s_touch_ok&&s_imu_ok&&s_rtc_ok&&adds[TOUCH_ADDR]==1&&adds[IMU_ADDR]==1&&adds[RTC_ADDR]==1);
 assert(board_touch_read(&x,&y)==1&&x==100&&y==120);
 int live_probes=probes[IMU_ADDR];atomic_store(&s_retry_devices,1);retry_devices(0);assert(probes[IMU_ADDR]==live_probes);
 read_error[TOUCH_ADDR]=-77;
 for(int i=0;i<3;i++)assert(board_touch_read(&x,&y)==-1&&x==100&&y==120);
 assert(!s_touch_ok&&s_devices[0].last_error==-77&&s_devices[0].failures==4);
 assert(s_touch_errors==3&&s_touch_last_error==-77);
 read_error[TOUCH_ADDR]=0;atomic_store(&s_retry_devices,1);retry_devices(0);assert(s_touch_ok);
 touch_packet[0]=0;assert(board_touch_read(&x,&y)==0&&x==100&&y==120);
 touch_packet[0]=1;touch_packet[1]=0x81;touch_packet[2]=0xd2;
 assert(board_touch_read(&x,&y)==-1&&x==100&&y==120);
 atomic_store(&sem_tokens[1],0);assert(board_touch_read(&x,&y)==-1);atomic_store(&sem_tokens[1],1);
 atomic_store(&s_imu_ok,0);id_bad=1;atomic_store(&s_retry_devices,1);retry_devices(0);
 assert(!s_imu_ok&&adds[IMU_ADDR]==1);
 const unsigned delays[]={2,4,8,30,30};
 for(unsigned i=0;i<5;i++){int64_t due=s_device_retry_at[1];atomic_store(&clock_us,due);retry_devices(0);assert(s_device_retry_at[1]==due+(int64_t)delays[i]*1000000);}
 id_bad=0;atomic_store(&s_retry_devices,1);retry_devices(0);assert(s_imu_ok&&adds[IMU_ADDR]==1);
 atomic_store(&sem_tokens[2],0);hal_imu m;assert(!board_imu_raw(&m));
 atomic_store(&sem_tokens[2],1);assert(board_imu_raw(&m));
 board_device_diag info[3];board_device_diagnostics(info);assert(info[1].available&&info[1].failures>=6);
 for(int stage=1;stage<=6;stage++) {
  atomic_store(&s_imu_ok,0);write_seen[IMU_ADDR]=0;write_fail_nth[IMU_ADDR]=stage;
  atomic_store(&s_retry_devices,1);retry_devices(0);assert(!s_imu_ok&&adds[IMU_ADDR]==1);
  write_fail_nth[IMU_ADDR]=0;atomic_store(&s_retry_devices,1);retry_devices(0);assert(s_imu_ok&&adds[IMU_ADDR]==1);
 }
 atomic_store(&s_touch_ok,0);write_error[TOUCH_ADDR]=-66;
 atomic_store(&s_retry_devices,1);retry_devices(0);assert(!s_touch_ok);
 /* The base board has no touch IRQ. Repeated idle/monitor NAKs must never
  * stretch the touch retry interval to the sensor policy's 8--30 seconds. */
 for(unsigned i=0;i<7;i++) {
  int64_t due=s_device_retry_at[0];atomic_store(&clock_us,due);retry_devices(0);
  assert(!s_touch_ok && s_device_retry_at[0]==due+1000000);
 }
 write_error[TOUCH_ADDR]=0;atomic_store(&s_retry_devices,1);retry_devices(0);assert(s_touch_ok&&adds[TOUCH_ADDR]==1);
 s_devices[1].failures=s_devices[1].consecutive=UINT32_MAX;device_result(1,-2);
 assert(s_devices[1].failures==UINT32_MAX&&s_devices[1].consecutive==UINT32_MAX);
 puts("PASS touch invalid/up/down, stable handles, live-device exclusion, bounded retry and saturation");
}
'''
        self.run_c("devices", code, ("firmware/main/board_protocol.c",))

    def test_rtc_each_ack_failure_fresh_retry_and_newer_generation(self):
        code = COMMON + DEVICE_STATE + device_functions() + RTC_STATE + "\n".join(function(name) for name in (
            "board_clock_changed", "rtc_failure", "rtc_save", "rtc_maintain")) + r'''
static void newer_request(void){on_calendar=NULL;clock_epoch+=100;board_clock_changed(clock_epoch);}
int main(void) {
 init_devices();s_rtc=RTC_ADDR;
 for(int stage=1;stage<=4;stage++) {
  atomic_store(&s_rtc_ok,1);s_devices[2].consecutive=0;s_rtc_valid=1;
  rtc_fail_stage=stage;rtc_restart_calls=rtc_invalidations=0;
  board_clock_changed(clock_epoch);uint32_t generation=s_rtc_generation;
  assert(!rtc_maintain(0)&&s_rtc_dirty&&s_rtc_generation==generation&&!s_rtc_valid);
  assert(s_rtc_failed_operation==(unsigned)stage&&s_rtc_last_error==-100-stage);
  if(stage==1)assert(!rtc_restart_calls);else assert(rtc_restart_calls==1);
  if(stage==3)assert(rtc_invalidations==1);
  int before=calendar_calls;clock_epoch+=9;atomic_fetch_add(&clock_us,4999999);
  assert(!rtc_maintain(0)&&calendar_calls==before);
  rtc_fail_stage=0;atomic_fetch_add(&clock_us,1);
  assert(rtc_maintain(0)&&!s_rtc_dirty&&s_rtc_valid&&s_rtc_last_error==0);
  int64_t epoch;assert(board_rtc_decode(calendar,&epoch)&&epoch==clock_epoch);
 }
 board_clock_changed(clock_epoch);uint32_t old=s_rtc_generation;on_calendar=newer_request;
 assert(!rtc_maintain(0)&&s_rtc_dirty&&s_rtc_generation==old+1);
 clock_epoch+=2;assert(rtc_maintain(0)&&!s_rtc_dirty);
 int64_t epoch;assert(board_rtc_decode(calendar,&epoch)&&epoch==clock_epoch);
 board_clock_changed(clock_epoch);atomic_store(&sem_tokens[3],0);
 assert(!rtc_maintain(1)&&s_rtc_dirty);atomic_store(&sem_tokens[3],1);assert(rtc_maintain(1));
 board_clock_changed(clock_epoch);rtc_fail_stage=3;rtc_invalidate_error=1;rtc_restart_calls=0;
 assert(!rtc_maintain(1)&&s_rtc_dirty&&!rtc_restart_calls);
 rtc_fail_stage=rtc_invalidate_error=0;assert(rtc_maintain(1)&&!s_rtc_dirty&&rtc_restart_calls==1);
 board_clock_changed(0);assert(!rtc_maintain(1)&&s_rtc_dirty&&!s_system_trusted);
 assert(s_rtc_failures==5);
 puts("PASS RTC control/stop/calendar/restart faults, current UTC retry, concurrent generation and trust");
}
'''
        self.run_c("rtc", code, ("firmware/main/board_protocol.c",))

    def test_worker_delay_timeout_cancel_and_true_ack_unmount(self):
        code = COMMON + r'''
#include <pthread.h>
#include <unistd.h>
#undef portENTER_CRITICAL
#undef portEXIT_CRITICAL
static pthread_mutex_t state_mutex=PTHREAD_MUTEX_INITIALIZER;
#define portENTER_CRITICAL(p) pthread_mutex_lock(&state_mutex)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(&state_mutex)
static atomic_int quit,blocked_mount,release_mount,samples,save_fail,unmounts,leases;
static atomic_int s_stopping,s_sd_retry,s_background_persistence=1,s_imu_ok;
static void *s_sensor_task=(void*)1,*s_fs_task=(void*)2;
static SemaphoreHandle_t s_board_stopped=5,s_fs_stopped=6;
static uint32_t s_sleep_generation,s_sensor_ack,s_fs_ack;
static int s_fs_sleep_ok,s_sleep_state,s_sleep_sensor_received,s_sleep_fs_received;
static int64_t s_sleep_deadline;
static esp_err_t s_sd_teardown_error;
static pedometer_saved s_pedometer_snapshot,s_pedometer_saved;
static pedometer s_pedometer;
static hal_activity s_activity;
static uint32_t s_sensor_max_gap_us,s_fs_max_io_us;
static int64_t s_sensor_last_us,s_fs_io_started;
static int s_walk_log,s_rtc_dirty;
static int s_calendar_timezone_ready=1,s_calendar_sample_ready=1;
static uint32_t calendar_day(int64_t now){(void)now;return 20261021;}
static struct tm *localtime_r(const time_t*t,struct tm*out){*out=*localtime(t);return out;}
typedef struct {time_t epoch;uint64_t uptime_ms;int millivolts,valid;hal_imu imu;}board_log_snapshot;
static void vTaskDelay(unsigned ticks) {
 if(atomic_load(&quit))pthread_exit(NULL);
 usleep(ticks*1000);atomic_fetch_add(&clock_us,(int64_t)ticks*1000);
}
static void flash_info_fill(void){}
static void fs_wait(void){vTaskDelay(100);}
static int board_batch_wanted(void){return 0;} /* display ON path: the batch path has its own tests */
static void board_batch_cycle(int64_t *power_at){(void)power_at;}
static void imu_leave_fifo(void){}
static void retry_devices(int initial){(void)initial;}
static void board_retry_devices(void){}
static int board_sd_mount(void) {
 atomic_store(&blocked_mount,1);atomic_store(&leases,1);
 while(!atomic_load(&release_mount))usleep(1000);
 atomic_store(&leases,0);return 0;
}
static int board_imu_raw(hal_imu *m){memset(m,0,sizeof *m);m->az=1;return 1;}
static hal_imu imu_take(const hal_imu *m){atomic_fetch_add(&samples,1);return *m;}
static void knock_poll(void){}
void pedometer_update(pedometer*p,uint32_t day,uint32_t ms,const hal_imu*m,int valid){(void)day;(void)ms;(void)m;if(valid)p->today.steps++;}
void pedometer_get(const pedometer*p,hal_activity*a){memset(a,0,sizeof *a);a->steps=p->today.steps;}
void tuning_get(sports_tuning*t){memset(t,0,sizeof *t);}
void sports_emit(int t,const float*v,int n){(void)t;(void)v;(void)n;}
void sports_flush(void){}
int sports_quiesced(void){return 1;}
void sports_get_status(sports_status*s){memset(s,0,sizeof *s);}
int hal_store_save_checked(const char*key,const void*p,int n){assert(!strcmp(key,"pedometer")&&n==sizeof(pedometer_saved));assert(p!=&s_pedometer_snapshot);return !atomic_load(&save_fail);}
static int rtc_maintain(int force){(void)force;return 1;}
static board_log_snapshot log_snapshot(void){board_log_snapshot s={0};return s;}
static int log_sample(const board_log_snapshot*s){(void)s;return 1;}
static void hal_esp32_sample_power(void){}
void hal_asset_cancel(void){}
static int device_begin(unsigned index){(void)index;return 0;}
static void device_result(unsigned index,int err){(void)index;(void)err;}
static void device_end(unsigned index){(void)index;}
static int wr(int dev,uint8_t reg,uint8_t value){(void)dev;(void)reg;(void)value;return 0;}
static int s_imu;
static int board_sd_unmount(void) {
 assert(s_sensor_ack==s_sleep_generation&&s_fs_ack==s_sleep_generation&&atomic_load(&s_stopping));
 if(atomic_load(&leases))return ESP_ERR_INVALID_STATE;
 atomic_fetch_add(&unmounts,1);return 0;
}
''' + "\n".join(function(name) for name in (
            "persistence_started", "persistence_finished", "save_pedometer", "board_walk_step", "board_sensor_publish",
            "board_fs_task", "board_task",
            "sleep_failed", "board_cancel_sleep", "board_prepare_sleep")) + r'''
static void *sensor(void *arg){board_task(arg);return NULL;}
static void *writer(void *arg){board_fs_task(arg);return NULL;}
int main(void) {
 pthread_t sensing,fs;s_sd_retry=1;atomic_store(&clock_us,1000);
 assert(!pthread_create(&sensing,NULL,sensor,NULL));assert(!pthread_create(&fs,NULL,writer,NULL));
 for(int i=0;i<1000&&!atomic_load(&blocked_mount);i++)usleep(1000);
 assert(blocked_mount&&leases);int before=samples;usleep(240000);assert(samples>=before+5);
 assert(board_prepare_sleep()==BOARD_SLEEP_PENDING&&!unmounts);
 assert(board_prepare_sleep()==BOARD_SLEEP_PENDING&&!unmounts);
 assert(board_cancel_sleep()==1&&!s_stopping&&leases&&!unmounts);
 assert(board_prepare_sleep()==BOARD_SLEEP_PENDING);
 atomic_fetch_add(&clock_us,6000000);assert(board_prepare_sleep()==BOARD_SLEEP_FAILED&&!s_stopping&&leases&&!unmounts);
 atomic_store(&release_mount,1);usleep(180000);assert(!leases&&samples>before+5);
 /* Failed checked final save produces a real failed ACK, never an unmount. */
 atomic_store(&save_fail,1);assert(board_prepare_sleep()==BOARD_SLEEP_PENDING);
 int result=-1;for(int i=0;i<1000&&result==-1;i++){usleep(1000);result=board_prepare_sleep();}
 assert(result==BOARD_SLEEP_FAILED&&!unmounts&&!s_stopping);
 atomic_store(&save_fail,0);assert(board_prepare_sleep()==BOARD_SLEEP_PENDING);
 result=-1;for(int i=0;i<1000&&result==-1;i++){usleep(1000);result=board_prepare_sleep();}
 assert(result==BOARD_SLEEP_READY&&unmounts==1&&s_stopping);
 assert(board_prepare_sleep()==BOARD_SLEEP_READY&&unmounts==1&&board_cancel_sleep()==0);
 atomic_store(&quit,1);assert(!pthread_join(sensing,NULL));assert(!pthread_join(fs,NULL));
 printf("PASS delayed mount sampling=%d, timeout/cancel ownership, checked save failure and both real ACKs\n",samples);
}
'''
        self.run_c("workers", code, flags=("-pthread",))
        sensor = function("board_task")
        for slow in ("hal_store_", "log_sample", "board_sd_mount", "flash_info_fill", "rtc_save", "sports_flush"):
            self.assertNotIn(slow, sensor)
        self.assertIn('nos_mem_alloc(4096,"board.sensor_stack")', function("start_board_workers"))
        self.assertIn("MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT", function("start_board_workers"))

    def test_checked_snapshot_retry_and_sdk_partial_unmount(self):
        code = COMMON + r'''
static pedometer_saved s_pedometer_snapshot,s_pedometer_saved;
static int s_calendar_timezone_ready=1,s_calendar_sample_ready=1;
static int save_fail,change_during_save,saves;
static pedometer_saved written;
int hal_store_save_checked(const char*key,const void*p,int bytes) {
 assert(!strcmp(key,"pedometer")&&bytes==sizeof written);saves++;written=*(const pedometer_saved*)p;
 if(change_during_save)s_pedometer_snapshot.steps++;
 return !save_fail;
}
static int s_sd_lock=4,s_sd_teardown_error;
static void *s_card=(void*)99;
static unsigned s_asset_users;
static char s_log_path[96]="preserved.csv";
static int unmount_error,unmount_calls,status_calls;
void hal_imu_record_get(hal_imu_record*r){memset(r,0,sizeof *r);r->state=HAL_IMU_IDLE;}
void hal_imu_file_get(hal_imu_file*f){memset(f,0,sizeof *f);}
static int esp_vfs_fat_sdcard_unmount(const char*root,void *card){(void)root;assert(card==(void*)99);unmount_calls++;return unmount_error;}
static void storage_status(const char*message){(void)message;status_calls++;}
#define SD_ROOT "/sdcard"
''' + function("save_pedometer") + function("board_sd_unmount") + r'''
int main(void) {
 s_pedometer_snapshot.steps=12;s_calendar_timezone_ready=0;
 assert(!save_pedometer()&&!saves&&s_pedometer_saved.steps==0);
 s_calendar_timezone_ready=1;s_calendar_sample_ready=0;assert(!save_pedometer()&&!saves);
 s_calendar_sample_ready=1;save_fail=1;assert(!save_pedometer()&&s_pedometer_saved.steps==0&&written.steps==12);
 save_fail=0;change_during_save=1;assert(save_pedometer()&&s_pedometer_saved.steps==12&&s_pedometer_snapshot.steps==13);
 change_during_save=0;assert(save_pedometer()&&s_pedometer_saved.steps==13&&written.steps==13);
 int count=saves;assert(save_pedometer()&&saves==count);
 atomic_store(&sem_tokens[4],1);s_asset_users=1;assert(board_sd_unmount()==ESP_ERR_INVALID_STATE&&s_card==(void*)99&&!unmount_calls);
 s_asset_users=0;atomic_store(&sem_tokens[4],0);assert(board_sd_unmount()==ESP_ERR_INVALID_STATE&&s_card==(void*)99&&!unmount_calls);
 atomic_store(&sem_tokens[4],1);unmount_error=ESP_FAIL;
 assert(board_sd_unmount()==ESP_FAIL&&!s_card&&s_sd_teardown_error==ESP_FAIL&&unmount_calls==1&&!s_log_path[0]);
 assert(board_sd_unmount()==ESP_FAIL&&unmount_calls==1&&status_calls==1);
 puts("PASS immutable checked checkpoint retry, SD lease exclusion and SDK partial-teardown latch");
}
'''
        self.run_c("checkpoint_unmount", code)

    def test_calendar_gate_retained_utc_delayed_timezone_and_count_carry(self):
        tracker = re.search(r"typedef struct \{[^}]+\} board_calendar_tracker;", BOARD)[0]
        code = COMMON + r'''
static pedometer s_pedometer;
static pedometer_saved s_pedometer_snapshot,s_calendar_pending_base;
static int s_calendar_timezone_ready,s_calendar_sample_ready;
static uint32_t s_calendar_revision,s_calendar_closed_generation=1;
static hal_activity s_activity;
static int64_t s_sample_us;
static int offset_seconds,conversions,close_during_conversion;
void board_calendar_ready(int ready);
static struct tm *localtime_r(const time_t*epoch,struct tm*out) {
 conversions++;time_t local=*epoch+offset_seconds;*out=*gmtime(&local);
 if(close_during_conversion){close_during_conversion=0;board_calendar_ready(0);}
 return out;
}
''' + tracker + "\nstatic board_calendar_tracker s_calendar;\n" + "\n".join(function(name) for name in (
            "board_calendar_ready", "calendar_hold", "calendar_day", "hal_activity_get")) + r'''
static void seed(uint32_t day,uint32_t steps) {
 pedometer_saved saved={.version=1,.day=day,.steps=steps,.active_ms=20000};
 pedometer_init(&s_pedometer,&saved);s_pedometer_snapshot=s_calendar_pending_base=saved;
 memset(&s_calendar,0,sizeof s_calendar);s_calendar_revision=0;s_calendar_closed_generation=1;
 s_calendar_timezone_ready=s_calendar_sample_ready=0;offset_seconds=0;conversions=0;
 clock_epoch=1792526400; /* 2026-10-20 20:00 UTC; India is already October21. */
}
static void collect(void) {
 for(uint32_t i=0;i<30000;i+=20) {
  hal_imu m={.az=1+.25f*sinf(i*.001f*2*6.2831853f),.gx=30};
  uint32_t day=calendar_day((int64_t)i*1000+1);
  assert(!day);pedometer_update(&s_pedometer,day,i,&m,1);s_pedometer_snapshot=s_pedometer.today;
 }
}
int main(void) {
 seed(20261021,400);collect();assert(!conversions&&s_pedometer.today.day==20261021);
 uint32_t accumulated=s_pedometer.today.steps;uint64_t active=s_pedometer.today.active_ms;
 assert(accumulated>=450);offset_seconds=19800;board_calendar_ready(1);
 assert(calendar_day(31000000)==20261021&&s_pedometer.today.steps==accumulated&&s_pedometer.today.active_ms==active);
 /* A genuinely new resolved day carries the deferred boot samples instead of clearing them. */
 seed(20261020,500);collect();uint32_t pending=s_pedometer.today.steps-500;
 uint64_t pending_active=s_pedometer.today.active_ms-20000;assert(pending>=50&&pending_active>0);
 offset_seconds=19800;board_calendar_ready(1);assert(calendar_day(31000000)==20261021);
 assert(s_pedometer.today.day==20261021&&s_pedometer.today.steps==pending&&s_pedometer.today.active_ms==pending_active);
 /* A pending preference restore may expose a stale TZ for many samples. */
 s_pedometer_snapshot=s_pedometer.today;board_calendar_ready(0);clock_epoch+=86400;offset_seconds=0;
 int before=conversions;uint32_t baseline=s_pedometer.today.steps;
 collect();assert(conversions==before&&s_pedometer.today.steps>baseline);
 uint32_t later=s_pedometer.today.steps-baseline;uint64_t later_active=s_pedometer.today.active_ms-s_calendar_pending_base.active_ms;
 offset_seconds=19800;board_calendar_ready(1);assert(calendar_day(62000000)==20261022);
 assert(s_pedometer.today.steps==later&&s_pedometer.today.active_ms==later_active);
 /* A raw restore holds the UI elsewhere: the board must close its gate from
  * the atomic pending flag even before the host can call calendar_preferences. */
 s_pedometer_snapshot=s_pedometer.today;prefs_are_loading=1;clock_epoch+=86400;
 assert(!calendar_day(62500000)&&!s_calendar_timezone_ready);
 prefs_are_loading=0;board_calendar_ready(1);assert(calendar_day(62600000)==20261023);
 /* Even a close/open shorter than a sensor period retains in-flight samples. */
 s_pedometer_snapshot=s_pedometer.today;board_calendar_ready(0);
 s_pedometer.today.steps+=3;s_pedometer.today.active_ms+=60;clock_epoch+=86400;
 board_calendar_ready(1);assert(calendar_day(63000000)==20261024&&s_pedometer.today.steps==3&&s_pedometer.today.active_ms==60);
 /* Do not consume a conversion that raced a gate change. */
 s_pedometer_snapshot=s_pedometer.today;clock_epoch+=86400;close_during_conversion=1;
 assert(!calendar_day(65000000)&&s_pedometer.today.steps==3);
 board_calendar_ready(1);assert(calendar_day(66000000)==20261025&&s_pedometer.today.steps==0);
 /* Suppress wrong-day service publication even before the next sensor tick. */
 memset(&s_activity,0,sizeof s_activity);s_activity.valid=1;s_activity.steps=23;s_sample_us=1;
 s_calendar_sample_ready=1;atomic_store(&clock_us,1000);hal_activity out;
 hal_activity_get(&out);assert(out.valid&&out.steps==23);
 board_calendar_ready(0);hal_activity_get(&out);assert(!out.valid&&out.steps==23);
 clock_epoch=0;board_calendar_ready(1);assert(!calendar_day(67000000));
 puts("PASS retained UTC/default TZ, delayed authoritative timezone, same/new-day count preservation, short/racing gates and invalid publication");
}
'''
        self.run_c("calendar", code, ("core/pedometer.c", "core/walk.c"))
        self.assertIn("board_calendar_ready(0)", function("board_apply_timezone"))

    def test_existing_protocol_variants_and_pedometer(self):
        for variant in (0, 1):
            self.run_c("protocol_" + str(variant), (ROOT / "tests/board_protocol_test.c").read_text(),
                       ("firmware/main/board_protocol.c",), ("-DCONFIG_WATCH_BOARD_143C=" + str(variant),))
        self.run_c("pedometer", (ROOT / "tests/pedometer_test.c").read_text(), ("core/pedometer.c", "core/walk.c"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
