"""Actual external drivers with controlled I2C/UART/SD faults; no device I/O.

SDK adapters are thin Windows host stubs. The production state machines, signed
time helpers, ring/file owner and shutdown coordinator are included unchanged.
"""
from pathlib import Path
import subprocess
import unittest
import shutil
import tempfile
import os

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/alpha05/checks/peripherals/contracts'
FLAGS=['-O2','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
       '-Wno-unused-variable','-Wno-format-truncation','-ffunction-sections','-fdata-sections',
       '-Itests/stubs/ext','-Itests','-Icore','-Inos','-Isim']
CASES={
'environment':('ext_env.c',r'''
static int data_mode;
static int rd(uint8_t reg,void *out,size_t n){
 if(reg!=0xf7||n!=8)return 0;
 uint8_t *v=out;memset(v,0,8);uint32_t p=data_mode?0x80000:415148,t=data_mode?0x80000:519888;
 v[0]=p>>12;v[1]=p>>4;v[2]=p<<4;v[3]=t>>12;v[4]=t>>4;v[5]=t<<4;
 InterlockedIncrement64(&test_clock);return 1;
}
int main(void){
 assert(ext_elapsed(10000,10001)==0&&!ext_idle(10000,10001));
 assert(ext_elapsed(20,0xfffffff0u)==36&&ext_idle(9000,0xfffffff0u));
 E=calloc(1,sizeof *E);E->dev=(void*)1;E->chip=0x58;E->sea=1013.25;
 E->T1=27504;E->T2=26435;E->T3=-1000;E->P1=36477;E->P2=-10685;E->P3=3024;
 E->P4=2855;E->P5=140;E->P6=-7;E->P7=15500;E->P8=-14600;E->P9=6000;
 E->cur=(hal_env){.valid=1,.humidity_valid=1,.sample_ms=1000,.pressure_hpa=1000};
 test_clock=3000;test_write_ok=0;env_service(3000);hal_env sample;assert(hal_env_get(&sample));
 assert(sample.sample_ms==1000);test_clock=7001;env_service(7001);assert(!hal_env_get(&sample)&&!sample.humidity_valid);
 test_write_ok=1;test_read_cb=rd;E->triggered=1;E->t_trig=6900;test_clock=7100;data_mode=1;
 env_service(7100);assert(E->last_error_op==3&&!hal_env_get(&sample));
 data_mode=0;E->triggered=1;E->t_trig=7000;test_clock=7200;env_service(7200);
 assert(hal_env_get(&sample)&&sample.sample_ms==7201&&!sample.humidity_valid);
 assert(fabsf(sample.temp_c-25.08f)<.02f&&sample.pressure_hpa>900&&sample.pressure_hpa<1100);
 uint32_t acquisition=sample.sample_ms;test_clock=7400;assert(hal_env_get(&sample)&&sample.sample_ms==acquisition);
 test_clock=acquisition+NOS_ENV_MAX_AGE_MS+1;assert(!hal_env_get(&sample));
 test_write_ok=0;assert(env_shutdown_poll(ext_ms())==-1);test_write_ok=1;assert(env_shutdown_poll(ext_ms())==1);
 free(E);E=NULL;assert(env_shutdown_poll(ext_ms())==1);
 puts("PASS actual environment: trigger/readiness failures expire; fixed successful acquisition time; BMP humidity absent; acknowledged sleep");return 0;
}
'''),
'light':('ext_light.c',r'''
static int channels=3,als_ok=1,prox_ok=1,status_ok=1;
static int rd(uint8_t reg,void *out,size_t n){
 if(reg==R_STATUS){*(uint8_t*)out=channels;return status_ok;}
 if(reg==R_CDATA&&als_ok){uint16_t data[4]={10000,5000,7000,2000};memcpy(out,data,n);InterlockedIncrement64(&test_clock);return 1;}
 if(reg==R_PDATA&&prox_ok){*(uint8_t*)out=42;InterlockedIncrement64(&test_clock);return 1;}return 0;
}
int main(void){
 Lt=calloc(1,sizeof *Lt);Lt->dev=(void*)1;Lt->again=1;test_read_cb=rd;test_clock=1000;
 light_service(1000);hal_light sample;assert(hal_light_get(&sample)&&sample.prox_valid);
 uint32_t als_ms=sample.sample_ms,prox_ms=sample.prox_ms;assert(als_ms==1001&&prox_ms==1002);
 als_ok=0;
 for(int i=1;i<=6;i++){test_clock=1000+400*i;light_service(ext_ms());}
 assert(!hal_light_get(&sample)&&sample.prox_valid&&sample.sample_ms==als_ms&&sample.prox_ms>prox_ms&&Lt->als_errors>=6);
 assert(Lt->status_errors==0);channels=0;test_clock=6000;light_service(6000);hal_light_get(&sample);assert(!sample.valid&&!sample.prox_valid&&sample.prox==0);
 channels=3;als_ok=prox_ok=1;test_clock=6400;light_service(6400);assert(hal_light_get(&sample)&&sample.prox_valid);
 als_ms=sample.sample_ms;status_ok=0;test_clock=9000;light_service(9000);assert(!hal_light_get(&sample)&&!sample.prox_valid&&sample.sample_ms==als_ms);
 test_write_ok=0;assert(light_shutdown_poll(ext_ms())==-1);test_write_ok=1;assert(light_shutdown_poll(ext_ms())==1);
 status_ok=1;test_clock+=400;test_write_ok=0;light_service(ext_ms());assert(Lt->suspended);
 test_write_ok=1;test_clock+=400;light_service(ext_ms());assert(!Lt->suspended&&hal_light_get(&sample));
 free(Lt);Lt=NULL;assert(light_shutdown_poll(ext_ms())==1);
 puts("PASS actual APDS: independent ALS/prox times and errors; status success cannot refresh a failed channel; stale/not-ready expiry and sleep ACK");return 0;
}
'''),
'heart':('ext_hr.c',r'''
static int mode;
static int rd(uint8_t reg,void *out,size_t n){
 memset(out,0,n);if(reg==4){if(mode==1)return 0;if(mode==2)((uint8_t*)out)[0]=1;return 1;}
 return 0;
}
static void reset(int failure){
 if(H)free(H);
 H=calloc(1,sizeof *H);H->dev=(void*)1;H->running=H->bg=1;H->bg_t0=1000;H->next_bg=1000000;
 H->next=1000;mode=failure;test_clock=1000;test_writes=0;test_write_ok=1;test_read_cb=rd;
}
int main(void){
 reset(0);test_clock=41000;hr_service(41000);assert(!H->bg&&!H->running&&H->hn==1&&H->hist[0].bpm==0);
 for(int failure=1;failure<=2;failure++){
  reset(failure);for(int i=0;i<11;i++){test_clock=1000+i*120;hr_service(ext_ms());}
  assert(!H->bg&&!H->running&&H->restart_at);unsigned writes=test_writes;test_clock+=120;hr_service(ext_ms());assert(test_writes==writes);
  assert(failure==1?H->fifo_pointer_errors==11:H->fifo_payload_errors==11);
 }
 reset(0);H->bg=0;H->enabled=1;H->owners=HAL_HR_OWNER_WORKOUT;H->last_get=10001;H->next=20000;test_clock=10000;
 hr_service(10000);assert(H->enabled&&H->owners==HAL_HR_OWNER_WORKOUT);
 H->cur=(hal_hr){.on_wrist=1,.bpm=72,.spo2=98,.quality=.9,.sample_ms=8000};hal_hr sample;
 assert(hal_hr_get(&sample)&&sample.bpm==72);test_clock=10001;hal_hr_get(&sample);assert(!sample.bpm&&!sample.spo2&&!sample.quality&&sample.sample_ms==8000);
 hal_hr_request(HAL_HR_OWNER_HEART,1);hal_hr_request(HAL_HR_OWNER_HEART,0);assert(H->owners==HAL_HR_OWNER_WORKOUT);
 test_shutdown=1;hal_hr_request(HAL_HR_OWNER_HEART,1);assert(H->owners==HAL_HR_OWNER_WORKOUT);
 test_write_ok=0;assert(hr_shutdown_poll(ext_ms())==-1&&H->shutdown_pending);
 test_shutdown=0;hal_hr_request(HAL_HR_OWNER_HEART,1);hr_service(ext_ms());assert(H->shutdown_pending);
 test_write_ok=1;hr_service(ext_ms());assert(!H->shutdown_pending&&H->running&&H->owners==HAL_HR_OWNER_HEART);
 test_shutdown=1;assert(hr_shutdown_poll(ext_ms())==1&&!H->running);
 free(H);H=NULL;assert(hr_shutdown_poll(ext_ms())==1);
 puts("PASS actual MAX30102: deadline before empty/read failures, capped restart, lease renewal, stale quality, preserved consumer owners/history and sleep ACK");return 0;
}
'''),
'gps':('ext_gps.c',r'''
static int emitted;
static int uart(void *out,unsigned cap){
 if(emitted++)return 0;
 char text[96]="$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,";
 unsigned sum=0;for(char *p=text+1;*p;p++)sum^=(unsigned char)*p;
 int n=(int)strlen(text);snprintf(text+n,sizeof text-n,"*%02X\r\n",sum);n=(int)strlen(text);
 assert((unsigned)n<=cap);memcpy(out,text,n);test_clock=10001;hal_gps_get(NULL);return n;
}
int main(void){
 G=calloc(1,sizeof *G);G->state=G_RUN;G->installed=G->enabled=1;G->last_get=10000;ext_present_bits|=EXT_GPS;
 test_clock=10000;test_uart_cb=uart;gps_service(10000);assert(G->fix.valid&&G->last_fix==10001&&G->enabled);
 hal_gps sample;test_clock=10000;assert(hal_gps_get(&sample)&&sample.valid&&sample.age_ms==0);
 test_clock=13002;gps_service(13002);hal_gps_get(&sample);assert(!sample.valid);
 assert(G->rx_bytes>60&&G->pump_gap_max==3002);
 test_shutdown=1;hal_gps_enable(1);G->enabled=0;hal_gps_enable(1);assert(!G->enabled);
 test_uart_tx_ok=0;assert(gps_shutdown_poll(ext_ms())==-1&&G->installed);
 test_uart_tx_ok=1;test_uart_delete_ok=0;assert(gps_shutdown_poll(ext_ms())==-1&&G->installed);
 test_uart_delete_ok=1;assert(gps_shutdown_poll(ext_ms())==1&&!G->installed&&!G->fix.valid);
 G->installed=1;G->state=G_RUN;test_uart_tx_ok=0;assert(gps_shutdown_poll(ext_ms())==-1&&G->shutdown_pending);
 test_shutdown=0;test_uart_tx_ok=1;hal_gps_enable(1);gps_service(ext_ms());assert(!G->shutdown_pending&&G->state==G_RUN&&G->enabled&&G->installed);
 free(G);G=NULL;assert(gps_shutdown_poll(ext_ms())==1);
 puts("PASS actual GPS NMEA: parsed fix and concurrent lease newer than caller remain fresh, stale expiry, TX-drain/delete shutdown barrier");return 0;
}
'''),
'lte':('ext_lte.c',r'''
static void message(int slot,const char *when,const char *text){
 char header[160];snprintf(header,sizeof header,"+CMGL: %d,\"REC UNREAD\",\"+12345\",\"\",\"%s\"",slot,when);
 line(header);char body[160];snprintf(body,sizeof body,"%s",text);line(body);
}
int main(void){
 L=calloc(1,sizeof *L);L->state=L_READY;L->installed=1;L->st.powered=L->st.sim=1;L->next_poll=L->next_inbox=L->next_cpin=1000000;
 L->enabled=1;L->last_get=1001;test_clock=1000;ext_present_bits|=EXT_LTE;assert(want_on(1000)&&L->enabled);
 assert(!lte_debug_request("at ")&&!lte_debug_request("sms +12345")&&!lte_debug_request("invalid"));
 assert(lte_debug_request("at AT")&&!lte_debug_request("at AT+CSQ"));L->raw_pending=0;
 L->cmd=C_CMGL;message(7,"26/10/21,12:00:00","first");message(9,"26/10/20,12:00:00","second");inbox_publish();
 uint32_t first=L->sms[0].id,second=L->sms[1].id;assert(first&&second&&first!=second);hal_lte_sms_mark_read_id(first);
 L->ntmp=0;L->cmd=C_CMGL;message(9,"26/10/20,12:00:00","second");message(7,"26/10/21,12:00:00","first");inbox_publish();
 assert(L->sms[0].id==first&&!L->sms[0].unread&&L->sms[1].id==second);
 L->ntmp=0;L->cmd=C_CMGL;message(7,"26/10/21,13:00:00","reused slot");inbox_publish();assert(L->sms[0].id!=first&&L->sms[0].unread);
 hal_lte_sms_mark_read_id(first);assert(L->sms[0].unread);
 L->cmd=C_NONE;L->in_text=0;uint32_t id=hal_lte_sms_send_request("+12345","hello");assert(id&&hal_lte_sms_send_status(id)==1);
 lte_service(ext_ms());assert(hal_lte_sms_send_status(id)==2&&L->cmd==C_CMGS_HDR);
 L->result=R_PROMPT;lte_service(ext_ms());assert(L->cmd==C_CMGS_BODY);char accepted[]="+CMGS: 3";line(accepted);
 assert(hal_lte_sms_send_status(id)==3);L->result=R_OK;lte_service(ext_ms());assert(!L->send_pending);
 uint32_t fail=hal_lte_sms_send_request("+12345","fail");assert(fail);L->cmd=C_CMGS_HDR;L->result=R_ERR;lte_service(ext_ms());assert(hal_lte_sms_send_status(fail)==4);
 test_shutdown=1;assert(!hal_lte_sms_send_request("+12345","blocked"));assert(lte_shutdown_poll(ext_ms())==0&&L->st.powered);
 test_clock+=8000;assert(lte_shutdown_poll(ext_ms())==0);test_clock+=1000;assert(lte_shutdown_poll(ext_ms())==0&&L->st.powered);
 test_clock+=5001;assert(lte_shutdown_poll(ext_ms())==-1&&L->st.powered&&L->installed);
 test_shutdown=0;hal_lte_enable(1);lte_service(ext_ms());assert(L->state==L_KEYOFF&&L->st.powered&&L->installed&&!L->shutdown_phase);
 char off[]="POWERED DOWN";line(off);lte_service(ext_ms());assert(L->state==L_OFF&&!L->installed&&!L->st.powered);
 lte_service(ext_ms());assert(L->state==L_CHECK&&L->installed);test_shutdown=1;assert(lte_shutdown_poll(ext_ms())==1&&!L->installed);
 free(L);L=NULL;assert(lte_shutdown_poll(ext_ms())==1);
 puts("PASS actual LTE: stable SMS IDs/read overlay/slot reuse, request acceptance vs CMGS result, failed send, shutdown waits for real modem acknowledgment");return 0;
}
'''),
'park':('ext_haptic.c',r'''
int main(void){
 haptic_init();assert(s_ok);test_shutdown=1;s_callback_active=1;assert(haptic_shutdown_poll(ext_ms())==0);
 s_callback_active=0;test_gpio_ok=0;assert(haptic_shutdown_poll(ext_ms())==-1);
 test_gpio_ok=1;assert(haptic_shutdown_poll(ext_ms())==1);s_req=-1;hal_haptic(HAPTIC_ALARM);assert(s_req==-1);
 puts("PASS actual haptic: callback ownership and acknowledged PWM-zero barrier; no restart during shutdown");return 0;
}
'''),
'mag':('ext_mag.c',r'''
int haptic_active(void){return 0;}int audio_amp_on(void){return 0;}
int main(void){
 M=calloc(1,sizeof *M);M->dev=(void*)1;M->chip=1;M->running=1;M->cur.valid=1;
 assert(!mag_debug_request("caljunk")&&!mag_debug_request("map +x+x+z")&&!mag_debug_request("map +x+y+z junk"));
 test_store_fail=1;assert(!mag_debug_request("map +x-y+z +y"));test_store_fail=0;
 assert(mag_debug_request("map +x-y+z +y")&&M->cal.map[1]==-2&&M->cal.fwd==2);
 test_shutdown=1;hal_mag_calibrate(1);assert(!M->calibrating);
 test_write_ok=0;assert(mag_shutdown_poll(ext_ms())==-1&&M->running&&M->shutdown_pending);
 test_shutdown=0;hal_mag_calibrate(1);mag_service(ext_ms());assert(M->shutdown_pending);
 test_write_ok=1;mag_service(ext_ms());assert(!M->shutdown_pending&&M->running&&M->calibrating);
 test_shutdown=1;assert(mag_shutdown_poll(ext_ms())==1&&!M->running&&!M->cur.valid);
 free(M);M=NULL;assert(mag_shutdown_poll(ext_ms())==1);
 puts("PASS actual compass: acknowledged standby and absent no-op, calibration cannot restart shutdown");return 0;
}
'''),
}

SYS_BODY=r'''
static int park_state[8],park_calls[8],sensor_calls,audio_calls,cancel_in_hook;static uint32_t service_time[6];
#define INIT(name) void name##_init(void){}
#define SENSOR(name,index) uint32_t name##_service(uint32_t now){service_time[index]=now;sensor_calls++;InterlockedAdd64(&test_clock,2);return 20;}
#define PARK(name,index) int name##_shutdown_poll(uint32_t now){(void)now;park_calls[index]++;if(cancel_in_hook==index+1)hal_ext_shutdown_cancel();return park_state[index];}
INIT(haptic) INIT(audio) INIT(gps) INIT(lte) INIT(env) INIT(light) INIT(mag) INIT(hr)
SENSOR(gps,0) SENSOR(lte,1) SENSOR(env,2) SENSOR(light,3) SENSOR(mag,4) SENSOR(hr,5)
uint32_t audio_service(uint32_t now){(void)now;audio_calls++;return 20;}
PARK(haptic,0) PARK(audio,1) PARK(gps,2) PARK(lte,3) PARK(env,4) PARK(light,5) PARK(mag,6) PARK(hr,7)
#define DEBUG(name) void name##_debug(void){}
DEBUG(haptic) DEBUG(audio) DEBUG(gps) DEBUG(lte) DEBUG(env) DEBUG(light) DEBUG(mag) DEBUG(hr)
void lte_debug_cmd(const char *s){(void)s;} void mag_debug_cmd(const char *s){(void)s;}
static int backend_accept;
int lte_debug_request(const char *s){(void)s;return backend_accept;}
int mag_debug_request(const char *s){(void)s;return backend_accept;}
UBaseType_t audio_stack_free(void){return 0;}
int hal_gps_get(hal_gps *p){(void)p;return 0;} int hal_lte_get(hal_lte *p){(void)p;return 0;}
int hal_hr_get(hal_hr *p){(void)p;return 0;} int hal_mag_get(hal_mag *p){(void)p;return 0;}
void hal_haptic(int n){(void)n;} void hal_sfx(int n){(void)n;} void hal_audio_volume(int n){(void)n;}
int hal_audio_play_file(const char *p){(void)p;return 0;} int hal_rec_start(const char *p){(void)p;return 0;}
int hal_rec_start_exclusive(const char *p){(void)p;return 0;}
void hal_rec_stop(void){} void hal_gps_enable(int n){(void)n;} void hal_lte_enable(int n){(void)n;}
void hal_hr_enable(int n){(void)n;} void hal_light_gesture_enable(int n){(void)n;}
void hal_env_set_sea_level(float n){(void)n;}
int main(void){
#if !EXT_BOARD_OK
 (void)ext_task;
 hal_ext_shutdown_begin();assert(hal_ext_shutdown_poll()==1);puts("PASS absent board: external shutdown is no-op success");
#else
 assert(!ext_idle(10000,10001)&&!ext_idle(10,0xfffffff0u)&&!ext_idle(0xfffffff0u,10));
 assert(ext_idle(10000,0xfffffff0u));
 test_clock=1000;test_delay_escape=1;
 if(!setjmp(test_delay_jmp))ext_task(NULL);
 test_delay_escape=0;assert(sensor_calls==6&&!audio_calls);
 for(int i=0;i<6;i++)assert(service_time[i]==1000u+2u*i);
 ext_present_bits=0x1ff;
 assert(!hal_ext_debug_command("haptic 99")&&!hal_ext_debug_command("sfx frog")&&!hal_ext_debug_command("vol 5junk"));
 assert(!hal_ext_debug_command("sea nan")&&!hal_ext_debug_command("play missing")&&!hal_ext_debug_command("rec missing"));
 assert(!hal_ext_debug_command("lte at AT")&&!hal_ext_debug_command("mag map bad"));backend_accept=1;
 assert(hal_ext_debug_command("lte at AT")&&hal_ext_debug_command("mag cal")&&hal_ext_debug_command("vol 50"));
 s_ext_task=(void*)1;ext_present_bits=0x1ff;test_clock=0x80000010u;hal_ext_shutdown_begin();
 uint32_t began=s_shutdown_started;test_clock+=50;hal_ext_shutdown_begin();assert(s_shutdown_started==began);
 for(int i=0;i<8;i++)park_state[i]=1;
 park_state[1]=park_state[3]=0;park_state[5]=-1;
 shutdown_service(ext_ms());assert(hal_ext_shutdown_poll()==-1);
 int haptic_calls=park_calls[0];park_state[5]=1;test_clock+=101;shutdown_service(ext_ms());assert(hal_ext_shutdown_poll()==0&&park_calls[0]==haptic_calls);
 test_clock=(uint64_t)began+31001;shutdown_service(ext_ms());assert(hal_ext_shutdown_poll()==-1);
 park_state[1]=park_state[3]=1;test_clock+=101;shutdown_service(ext_ms());assert(hal_ext_shutdown_poll()==1);
 hal_ext_shutdown_cancel();assert(ext_shutdown_requested()&&!hal_ext_shutdown_poll());shutdown_cancel_service();assert(!ext_shutdown_requested());
 hal_ext_shutdown_begin();cancel_in_hook=1;int audio_before=park_calls[1];shutdown_service(ext_ms());
 assert(s_shutdown_cancel&&!s_shutdown_done&&park_calls[1]==audio_before);shutdown_cancel_service();assert(!ext_shutdown_requested());
 cancel_in_hook=0;hal_ext_shutdown_begin();shutdown_service(ext_ms());assert(hal_ext_shutdown_poll()==1&&park_calls[1]>audio_before);
 puts("PASS actual ext coordinator: sensor loop excludes file I/O, refreshes each service time; idempotent begin, pending/error/timeout stay awake, late ACK becomes clean");
#endif
 return 0;
}
'''

AUDIO_PREFIX=r'''
#include <io.h>
#include <sys/stat.h>
#include <fcntl.h>
static SRWLOCK sd_mutex=SRWLOCK_INIT;
static __thread int io_locked;
static volatile LONG leases,io_calls,slow_read,fail_sync,fail_write,fail_close;
static DWORD ui_thread;static HANDLE entered,release_read,request,done;
static int worker_exit;
static void assert_file_owner(void){assert(io_locked&&GetCurrentThreadId()!=ui_thread);InterlockedIncrement(&io_calls);}
int board_sd_asset_acquire(void){InterlockedIncrement(&leases);return 1;}
void board_sd_asset_release(void){assert(InterlockedDecrement(&leases)>=0);}
void board_sd_asset_lock(void){AcquireSRWLockExclusive(&sd_mutex);io_locked++;}
void board_sd_asset_unlock(void){assert(io_locked==1);io_locked--;ReleaseSRWLockExclusive(&sd_mutex);}
static FILE *test_fopen(const char *path,const char *mode){assert_file_owner();return fopen(path,mode);}
static size_t test_fread(void *p,size_t n,size_t count,FILE *f){
 assert_file_owner();if(InterlockedExchange(&slow_read,0)){SetEvent(entered);assert(WaitForSingleObject(release_read,3000)==WAIT_OBJECT_0);InterlockedAdd64(&test_clock,2000);}
 return fread(p,n,count,f);
}
static size_t test_fwrite(const void *p,size_t n,size_t count,FILE *f){
 assert_file_owner();if(n==2&&InterlockedExchange(&fail_write,0)){errno=EIO;return fwrite(p,n,count/2,f);}return fwrite(p,n,count,f);
}
static int test_fseek(FILE *f,long at,int whence){assert_file_owner();return fseek(f,at,whence);}
static long test_ftell(FILE *f){assert_file_owner();return ftell(f);}
static int test_fflush(FILE *f){assert_file_owner();return fflush(f);}
static int test_fsync(int fd){assert_file_owner();if(InterlockedExchange(&fail_sync,0)){errno=EIO;return -1;}return _commit(fd);}
static int test_fclose(FILE *f){assert_file_owner();int result=fclose(f);if(InterlockedExchange(&fail_close,0)){errno=EIO;return EOF;}return result;}
static int test_open(const char *path,int flags,int mode){assert_file_owner();return _open(path,flags|_O_BINARY,mode);}
static int test_stat(const char *path,struct stat *out){assert_file_owner();return stat(path,out);}
static int test_rename(const char *old,const char *new_path){assert_file_owner();return rename(old,new_path);}
#define fopen test_fopen
#define fread test_fread
#define fwrite test_fwrite
#define fseek test_fseek
#define ftell test_ftell
#define fflush test_fflush
#define fsync test_fsync
#define fclose test_fclose
#define open test_open
#define stat(path,out) test_stat(path,out)
#define rename test_rename
'''
AUDIO_BODY=r'''
#undef fopen
#undef fread
#undef fwrite
#undef fseek
#undef ftell
#undef fflush
#undef fsync
#undef fclose
#undef open
#undef stat
#undef rename
static DWORD WINAPI file_thread(void *arg){(void)arg;for(;;){WaitForSingleObject(request,INFINITE);if(worker_exit)break;audio_service(ext_ms());SetEvent(done);}return 0;}
static void step(void){SetEvent(request);assert(WaitForSingleObject(done,3000)==WAIT_OBJECT_0);}
static void path_of(char path[256],const char *root,const char *name){snprintf(path,256,"%s/%s",root,name);}
static int exists(const char *path){struct stat st;return !stat(path,&st);}
static void create(const char *path,size_t bytes){FILE *f=fopen(path,"wb");assert(f);while(bytes--)assert(fputc(0x35,f)!=EOF);assert(!fclose(f));}
static void capture(void){int16_t data[AU_BLOCK*2];for(int i=0;i<AU_BLOCK*2;i++)data[i]=100+i;mic_block(data,AU_BLOCK,ext_ms());}
int main(int argc,char **argv){
 assert(argc==2);ui_thread=GetCurrentThreadId();test_clock=0x80000010u;audio_init();A->mic_ok=1;A->mic_start_ms=(uint32_t)test_clock-1000;
 assert(test_ntasks==2&&!strcmp(test_tasks[1].name,"extf")&&test_tasks[1].fn==audio_file_task);
 entered=CreateEvent(NULL,FALSE,FALSE,NULL);release_read=CreateEvent(NULL,FALSE,FALSE,NULL);
 request=CreateEvent(NULL,FALSE,FALSE,NULL);done=CreateEvent(NULL,FALSE,FALSE,NULL);
 HANDLE worker=CreateThread(NULL,0,file_thread,NULL,0,NULL);assert(worker);
 char pcm[256],rec[256],part[264];path_of(pcm,argv[1],"input.pcm");path_of(rec,argv[1],"record.wav");create(pcm,40000);
 test_ota=1;assert(!hal_audio_play_file(pcm)&&!hal_rec_start_exclusive(rec));test_ota=0;
 test_host_shutdown=1;assert(!hal_audio_play_file(pcm)&&!hal_rec_start_exclusive(rec));test_host_shutdown=0;
 assert(hal_audio_play_file(pcm)&&!io_calls);slow_read=1;SetEvent(request);assert(WaitForSingleObject(entered,3000)==WAIT_OBJECT_0);
 ULONGLONG began=GetTickCount64();assert(hal_audio_playing());uint8_t v;ext_rd((void*)1,0,&v,1);
 assert(hal_rec_start_exclusive(rec)==1&&hal_rec_start_exclusive(rec)==0&&GetTickCount64()-began<50);
 SetEvent(release_read);assert(WaitForSingleObject(done,3000)==WAIT_OBJECT_0);hal_audio_play_stop();step();
 snprintf(part,sizeof part,"%s.part",rec);assert(hal_rec_status()==1&&exists(part)&&!exists(rec)&&leases==1);
 for(int i=0;i<5;i++)capture();
 hal_rec_stop();assert(hal_rec_status()==2&&hal_rec_active()&&!exists(rec));fail_sync=1;step();
 assert(hal_rec_status()==2&&!exists(rec)&&leases==1);test_shutdown=1;assert(audio_shutdown_poll(ext_ms())==0);
 test_shutdown=0;assert(!hal_rec_start_exclusive(rec));test_clock+=1001;step();
 assert(hal_rec_status()==3&&hal_rec_result()==HAL_REC_DONE&&exists(rec)&&!exists(part)&&!leases);
 FILE *f=fopen(rec,"rb");assert(f);uint8_t header[44];assert(fread(header,1,44,f)==44);uint32_t bytes;memcpy(&bytes,header+40,4);assert(bytes==5*AU_BLOCK*2);assert(!fclose(f));
 char denied[256];path_of(denied,argv[1],"admission-denied.wav");assert(hal_rec_start_exclusive(denied)==1);
 test_ota=1;step();test_ota=0;assert(hal_rec_status()==4&&hal_rec_result()==HAL_REC_ERROR&&!exists(denied)&&!leases);
 LONG before=io_calls;assert(hal_rec_start_exclusive(rec)==1&&io_calls==before);step();assert(hal_rec_status()==4&&hal_rec_result()==HAL_REC_EXISTS&&!leases);
 char collision[256];path_of(collision,argv[1],"part-collision.wav");snprintf(part,sizeof part,"%s.part",collision);create(part,7);
 assert(hal_rec_start_exclusive(collision)==1);step();assert(hal_rec_result()==HAL_REC_EXISTS&&exists(part)&&!exists(collision));
 path_of(collision,argv[1],"publish-collision.wav");assert(hal_rec_start_exclusive(collision)==1);step();capture();create(collision,7);hal_rec_stop();step();
 assert(hal_rec_result()==HAL_REC_EXISTS&&exists(collision));struct stat st;assert(!stat(collision,&st)&&st.st_size==7);
 path_of(collision,argv[1],"short-write.wav");assert(hal_rec_start_exclusive(collision)==1);step();capture();hal_rec_stop();fail_write=1;step();step();
 assert(hal_rec_status()==4&&hal_rec_result()==HAL_REC_ERROR&&!exists(collision)&&A->rec_written==AU_BLOCK/2&&A->rec_drops==AU_BLOCK/2);
 assert(port_up());test_shutdown=1;test_i2s_disable_fail=1;assert(!audio_park());assert(audio_shutdown_poll(ext_ms())==0);
 assert(audio_park());step();assert(audio_shutdown_poll(ext_ms())==1&&!s_tx&&!s_rx&&!A->amp);
 assert(!hal_rec_start_exclusive(rec)&&!hal_audio_play_file(pcm)&&hal_mic_db()<0);
 test_shutdown=0;assert(port_up()&&!s_audio_parked);test_shutdown=1;assert(!audio_shutdown_poll(ext_ms()));assert(audio_park());
 test_shutdown=0;path_of(collision,argv[1],"close-error.wav");assert(hal_rec_start_exclusive(collision)==1);step();hal_rec_stop();fail_close=1;step();
 assert(A->close_error&&leases==1&&hal_rec_status()==4);test_shutdown=1;assert(audio_shutdown_poll(ext_ms())==-1);
 worker_exit=1;SetEvent(request);assert(WaitForSingleObject(worker,3000)==WAIT_OBJECT_0);
 puts("PASS actual audio: UI queues without SD; blocked file read leaves sensor/queries runnable; bounded ring drain, sync retry, final/temp/publication collisions, checked short-write accounting, close-error lease barrier, I2S ACK and shutdown rejection");return 0;
}
'''

@unittest.skipUnless(shutil.which('gcc'),'gcc required')
class ExternalDriverContracts(unittest.TestCase):
    def run_case(self,name,extra=()):
        OUT.mkdir(parents=True,exist_ok=True)
        src,body=CASES[name]
        before='#define EXT_TEST_SYS 1\n' if name.startswith('system') else ''
        source=OUT/(name+'.c');source.write_text(before+'#include "'+(ROOT/'tests/ext_driver_support.h').as_posix()+'"\n'+
            (AUDIO_PREFIX if name=='audio' else '')+'#include "'+(ROOT/'firmware/main/ext'/src).as_posix()+'"\n'+body)
        exe=source.with_suffix('.exe')
        command=['gcc',*FLAGS,*extra,str(source),'-Wl,--gc-sections','-lm','-o',str(exe)]
        env=os.environ|{k:str(OUT) for k in ('TEMP','TMP','TMPDIR')}
        result=subprocess.run(command,cwd=ROOT,env=env,capture_output=True,text=True,timeout=60)
        (OUT/(name+'-compile.log')).write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        sd=Path(tempfile.mkdtemp(prefix=name+'-sd-',dir=OUT))
        result=subprocess.run([str(exe),str(sd)],cwd=ROOT,env=env,capture_output=True,text=True,timeout=30)
        (OUT/(name+'.log')).write_text(result.stdout+result.stderr)
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        self.assertIn('PASS',result.stdout);print(result.stdout)
    def test_environment(self):self.run_case('environment')
    def test_light(self):self.run_case('light')
    def test_heart(self):self.run_case('heart')
    def test_gps(self):self.run_case('gps')
    def test_lte(self):self.run_case('lte')
    def test_haptic(self):self.run_case('park')
    def test_mag(self):self.run_case('mag')
    def test_system(self):self.run_case('system')
    def test_system_absent(self):self.run_case('system-absent',['-DCONFIG_WATCH_BOARD_143C=1'])
    def test_audio(self):self.run_case('audio',['-DESP_PLATFORM=1'])
CASES['system']=('ext_sys.c',SYS_BODY)
CASES['system-absent']=('ext_sys.c',SYS_BODY)
CASES['audio']=('ext_audio.c',AUDIO_BODY)
if __name__=='__main__':unittest.main()
