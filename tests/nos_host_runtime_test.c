#include "hal.h"
#include "workout.h"
#include "nos_frame_perf.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* No sensor-start, lease-renewal, filesystem, preference-write or audio-start
 * stub exists here: adding one to the actual diagnostic fails this compile. */
static hal_imu_record imu;
static hal_imu_stream stream;
static hal_imu_file file;
static workout_snapshot workout;
static workout_storage_status storage;
static nos_frame_capture s_capture;
static int ota,shutdown,loading,dirty,pending,track,track_pending,track_saving;
static int audio,audio_active,s_touch_now,s_touch_any,s_btn_down,s_inj_active,asset,radio_pending;
static int no_workout,calls,critical;
#define portENTER_CRITICAL(mux) do {(void)(mux);assert(!critical);critical=1;} while(0)
#define portEXIT_CRITICAL(mux) do {(void)(mux);assert(critical);critical=0;} while(0)
static int s_btn_mux;
void hal_imu_record_get(hal_imu_record *out){*out=imu;calls++;}
void hal_imu_stream_get(hal_imu_stream *out){*out=stream;calls++;}
void hal_imu_file_get(hal_imu_file *out){*out=file;calls++;}
const workout_snapshot *workout_get(void){calls++;return no_workout?NULL:&workout;}
void workout_storage_get(workout_storage_status *out){*out=storage;calls++;}
static int ota_busy(void){calls++;return ota;}
static int nos_host_shutdown_pending(void){calls++;return shutdown;}
static int prefs_loading(void){calls++;return loading;}
static int prefs_pending(void){calls++;return dirty;}
static int imu_worker_pending(void){calls++;return pending;}
static int svc_track_active(void){calls++;return track;}
static int svc_track_pending(void){calls++;return track_pending;}
static int svc_track_saving(void){calls++;return track_saving;}
static int hal_rec_status(void){calls++;return audio;}
static int hal_rec_active(void){calls++;return audio_active;}
int hal_asset_busy(void){calls++;return asset;}
int hal_esp32_radios_pending(void){calls++;return radio_pending;}
static char reply[4096];
static int capture_printf(const char *format,...)
{va_list args;va_start(args,format);int n=vsnprintf(reply,sizeof reply,format,args);va_end(args);assert(n>0 && n<(int)sizeof reply);return n;}
#define printf capture_printf
/* ACTUAL FUNCTIONS */
#undef printf
static int value(const char *key)
{char field[64];snprintf(field,sizeof field," %s=",key);const char *p=strstr(reply,field);assert(p);return atoi(p+strlen(field));}
int main(void)
{
    assert(host_command_readonly("runtime_state"));
    assert(!host_command_readonly("runtime_state clear"));
    assert(!host_command_readonly("runtime_state stop"));
    imu.state=HAL_IMU_ERROR;stream.hz=800;stream.queued=17;file.busy=1;
    workout.state=WORKOUT_PAUSED;workout.save_state=WORKOUT_SAVE_FAILED;
    storage.ready=0;storage.dirty=1;storage.busy=1;storage.uncertain=1;storage.corrupt=1;storage.foreign=1;
    ota=shutdown=loading=dirty=pending=track=track_pending=track_saving=1;
    audio=4;audio_active=1;s_touch_now=1;s_touch_any=1;s_btn_down=1;s_inj_active=1;asset=1;radio_pending=1;s_capture.state=2;
    runtime_state_report();
    assert(!strncmp(reply,"NOSRUNTIME v=1 ",15) && !critical && calls==17);
    const char *ones[]={"ota_busy","shutdown_pending","prefs_loading","prefs_dirty","imu_pending","imu_file_busy",
        "workout_dirty","workout_busy","workout_uncertain","workout_corrupt","workout_foreign","track_active",
        "track_pending","track_saving","audio_active","touch_down","touch_pending","button_down","injected","asset_busy","radio_pending"};
    for(unsigned i=0;i<sizeof ones/sizeof *ones;i++)assert(value(ones[i])==1);
    assert(value("imu_state")==HAL_IMU_ERROR && value("imu_hz")==800 && value("imu_queued")==17);
    assert(value("workout_ready")==0 && value("workout_state")==WORKOUT_PAUSED && value("workout_save")==WORKOUT_SAVE_FAILED);
    assert(value("audio_status")==4 && value("capture_state")==2);
    no_workout=1;calls=0;runtime_state_report();
    assert(value("workout_state")==-1 && value("workout_save")==-1 && calls==17);
    /* Unknown state is evidence, never normalized to a safe/inactive result. */
    imu.state=-1;audio=-1;pending=1;runtime_state_report();
    assert(value("imu_state")==-1 && value("audio_status")==-1 && value("imu_pending")==1);
    puts("PASS actual runtime diagnostic: cached owner states, unknowns preserved, readonly admission, no lease/I/O dependencies");
    return 0;
}
