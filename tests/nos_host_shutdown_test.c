#include "nos.h"
#include "hal_ext.h"
#include "workout.h"
#include "svc_track.h"
#include "prefs.h"
#include "diag_log.h"
#include "sports.h"
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
enum {BOARD_SLEEP_PENDING=-1,BOARD_SLEEP_FAILED=0,BOARD_SLEEP_READY=1};
enum {PWR_ON,PWR_AOD,PWR_OFF,PWR_SENSOR,GPIO_NUM_0=0};
#define pdMS_TO_TICKS(ms) (ms)
static int s_shutdown_mode,s_shutdown_phase;
static uint32_t s_shutdown_retry,s_shutdown_report;
static hal_imu_record recorder;static hal_imu_file file;
static hal_imu_stream stream;static int imu_pending;
static workout_snapshot workout;static workout_storage_status storage;
static int ota,track,track_pending,track_saving,asset_busy,prefs_ok,ext_result,board_result;
static int cancel_ok=1,quiesced,pref_dirty,s_pwr=PWR_ON;
static int audio_status,loading;
static int voice_busy,voice_cancelled;
int voice_chat_active(void){return voice_busy;}
void voice_chat_cancel(void){voice_cancelled++;}
void voice_chat_maintenance(void){}
int voice_chat_history_pending(void){return 0;}
static char error_reply[160];
static int cmd_printf(const char *format,...){va_list args;va_start(args,format);int n=vsnprintf(error_reply,sizeof error_reply,format,args);va_end(args);return n;}
static unsigned cancelled_ext,cancelled_board;
static unsigned ext_begin,ext_poll,board_poll,parked,fences,restarts,sleeps,toasts,asset_cancel;
static jmp_buf completed;
int ota_busy(void){return ota;}
void hal_imu_record_get(hal_imu_record *out){*out=recorder;}
void hal_imu_file_get(hal_imu_file *out){*out=file;}
void hal_imu_stream_get(hal_imu_stream *out){*out=stream;}
int imu_worker_pending(void){return imu_pending;}
int hal_rec_status(void){return audio_status;}
int hal_rec_active(void){return audio_status==1;}
const workout_snapshot *workout_get(void){return &workout;}
void workout_storage_get(workout_storage_status *out){*out=storage;}
int svc_track_active(void){return track;}
int svc_track_pending(void){return track_pending;}
int svc_track_saving(void){return track_saving;}
void nos_toast(const char *text){toasts++;}
void nos_keep_awake(void){}
void nos_invalidate(void){}
static void go_state(int mode){assert(mode==PWR_ON || mode==PWR_OFF);}
void prefs_save(void){}
int prefs_flush(void){if(prefs_ok)pref_dirty=0;return prefs_ok;}
int prefs_pending(void){return pref_dirty;}
int prefs_loading(void){return loading;}
void hal_asset_cancel(void){asset_cancel++;}
int hal_asset_busy(void){return asset_busy;}
void orrery_card_asset_poll(void){}
void hal_ext_shutdown_begin(void){ext_begin++;}
int hal_ext_shutdown_poll(void){ext_poll++;return ext_result;}
void hal_ext_shutdown_cancel(void){cancelled_ext++;}
void sports_quiesce(int pause){quiesced=pause;}
static int board_cancel_sleep(void){cancelled_board++;return cancel_ok;}
static int board_prepare_sleep(void){board_poll++;return board_result;}
static void set_level(int level){assert(!level);parked++;}
static void panel_sleep(int asleep){assert(asleep==1);parked++;}
static void frame_drain(void){fences++;}
static void vTaskDelay(unsigned ms){assert(ms==150);}
static void esp_restart(void){restarts++;longjmp(completed,1);}
static void esp_sleep_enable_ext0_wakeup(int pin,int level){assert(pin==GPIO_NUM_0 && !level);}
static void nos_power_set_sleep_hooks(void (*before)(void),void (*after)(long long)){(void)before;(void)after;}
#define ESP_SLEEP_WAKEUP_GPIO 7
static void esp_sleep_disable_wakeup_source(int source){assert(source==ESP_SLEEP_WAKEUP_GPIO);}
static void esp_deep_sleep_start(void){sleeps++;longjmp(completed,1);}
void diag_log_event(uint16_t code,int32_t a,int32_t b,int32_t c){assert(code==DIAG_SLEEP_BLOCK || code==DIAG_POWER_POLICY);}
/* ACTUAL FUNCTIONS */
static void reset(void)
{
    memset(&recorder,0,sizeof recorder);memset(&file,0,sizeof file);memset(&stream,0,sizeof stream);imu_pending=0;
    memset(&workout,0,sizeof workout);memset(&storage,0,sizeof storage);storage.ready=1;
    s_shutdown_mode=s_shutdown_phase=0;s_shutdown_retry=s_shutdown_report=0;
    ota=track=track_pending=track_saving=asset_busy=0;prefs_ok=1;ext_result=0;board_result=BOARD_SLEEP_PENDING;
    cancel_ok=1;cancelled_ext=cancelled_board=quiesced=pref_dirty=0;
    audio_status=loading=voice_busy=voice_cancelled=0;error_reply[0]=0;
    ext_begin=ext_poll=board_poll=parked=fences=restarts=sleeps=toasts=asset_cancel=0;
}
int main(void)
{
    reset();ota=1;nos_host_power(2);assert(!nos_host_shutdown_pending() && !ext_begin);
    reset();recorder.state=HAL_IMU_RECORDING;nos_host_power(2);assert(!nos_host_shutdown_pending());
    reset();file.busy=1;nos_host_power(1);assert(!nos_host_shutdown_pending());
    reset();recorder.state=HAL_IMU_ERROR;imu_pending=1;nos_host_power(2);assert(!nos_host_shutdown_pending());
    reset();stream.hz=50;nos_host_power(2);assert(!nos_host_shutdown_pending());
    reset();stream.queued=1;nos_host_power(2);assert(!nos_host_shutdown_pending());
    reset();audio_status=2;nos_host_power(2);assert(!nos_host_shutdown_pending());
    reset();voice_busy=1;nos_host_power(2);assert(!nos_host_shutdown_pending() && voice_cancelled==1);
    reset();workout.state=WORKOUT_PAUSED;nos_host_power(2);assert(!nos_host_shutdown_pending());
    reset();workout.state=WORKOUT_FINISHED;workout.save_state=WORKOUT_SAVE_PENDING;nos_host_power(2);assert(!nos_host_shutdown_pending());
    reset();storage.uncertain=1;nos_host_power(2);assert(!nos_host_shutdown_pending());
    reset();storage.ready=0;nos_host_power(2);assert(!nos_host_shutdown_pending());
    reset();track_pending=1;nos_host_power(2);assert(!nos_host_shutdown_pending());

    reset();nos_host_power(2);assert(nos_host_shutdown_pending() && asset_cancel==1 && quiesced);
    asset_busy=1;shutdown_tick(1);assert(!ext_begin && !parked);
    asset_busy=0;prefs_ok=0;shutdown_tick(2);assert(!ext_begin && !parked);
    prefs_ok=1;shutdown_tick(1003);assert(ext_begin==1 && ext_poll==1 && !board_poll && !parked);
    ext_result=-1;shutdown_tick(2000);assert(ext_begin==1 && !board_poll && !sleeps && !restarts && !parked);
    ext_result=0;shutdown_tick(3000);assert(!board_poll && !parked);
    ext_result=1;shutdown_tick(4000);assert(board_poll==1 && !parked && nos_host_shutdown_pending());
    prefs_ok=0;pref_dirty=1;shutdown_tick(4500);assert(cancelled_board==1 && board_poll==1 && s_shutdown_phase==1 && !parked);
    prefs_ok=1;shutdown_tick(5501);assert(board_poll==2 && !parked);
    board_result=BOARD_SLEEP_FAILED;shutdown_tick(6000);
    assert(!nos_host_shutdown_pending() && !quiesced && cancelled_ext==1 && !parked && !sleeps);

    reset();nos_host_power(2);ext_result=1;board_result=BOARD_SLEEP_READY;
    if(!setjmp(completed)){shutdown_tick(1);assert(0);}
    assert(sleeps==1 && !restarts && parked==2 && fences==1);

    reset();nos_host_power(1);ext_result=1;board_result=BOARD_SLEEP_READY;
    if(!setjmp(completed)){shutdown_tick(1);assert(0);}
    assert(restarts==1 && !sleeps && parked==2 && fences==1);
    reset();nos_host_power(2);shutdown_tick(1);nos_host_power(-1);
    assert(!nos_host_shutdown_pending() && !quiesced && cancelled_ext==1 && cancelled_board==1 && !parked);
    reset();nos_host_power(2);cancel_ok=0;nos_host_power(-1);
    assert(nos_host_shutdown_pending() && quiesced && s_shutdown_phase==3 && cancelled_ext==1);
    ext_result=1;board_result=BOARD_SLEEP_READY;shutdown_tick(1);assert(!parked && !sleeps && !restarts);
    ota=1;assert(!power_restart_request() && strstr(error_reply,"ota_busy"));ota=0;
    imu_pending=1;assert(!power_restart_request() && strstr(error_reply,"imu_owner"));imu_pending=0;
    loading=1;assert(!power_restart_request() && strstr(error_reply,"preferences_unknown"));loading=0;
    prefs_ok=0;assert(!power_restart_request() && strstr(error_reply,"preferences_not_durable"));prefs_ok=1;
    if(!setjmp(completed)){(void)power_restart_request();assert(0);}
    assert(restarts==1 && !sleeps);
    puts("PASS actual shutdown: admission guards, asset adoption, checked prefs, ext pending/error, board pending/error, UI remains unparked until both ACK clean");
    return 0;
}
