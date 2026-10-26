#include "hal_wave2_common.inc"
#include "workout.h"
static FILE *sd_open(const char*,const char*);
static int sd_seek(FILE*,long,int),sd_close(FILE*),sd_error(FILE*);
static long sd_tell(FILE*);
static size_t sd_read(void*,size_t,size_t,FILE*);
static int sd_remove(const char*),sd_rename(const char*,const char*);
#define fopen sd_open
#define fseek sd_seek
#define ftell sd_tell
#define fread sd_read
#define ferror sd_error
#define fclose sd_close
#define remove sd_remove
#define rename sd_rename
#include "../firmware/main/ota.c"
#undef fopen
#undef fseek
#undef ftell
#undef fread
#undef ferror
#undef fclose
#undef remove
#undef rename

static workout_snapshot mock_workout;
static int mock_prefs,mock_loading,mock_imu_state,mock_file_busy,mock_track,mock_shutdown;
static workout_storage_status mock_storage;
static int mock_imu_pending,mock_stream_hz,mock_stream_queued,mock_track_pending,mock_track_saving,mock_rec,mock_audio;
static unsigned mock_policy_reads;
int nos_host_shutdown_pending(void) {return mock_shutdown;}
int prefs_pending(void) {mock_policy_reads++;return mock_prefs;}
int prefs_loading(void) {return mock_loading;}
void hal_imu_record_get(hal_imu_record *o) {memset(o,0,sizeof *o);o->state=mock_imu_state;}
void hal_imu_file_get(hal_imu_file *o) {memset(o,0,sizeof *o);o->busy=mock_file_busy;}
const workout_snapshot *workout_get(void) {return &mock_workout;}
int svc_track_active(void) {return mock_track;}
void hal_imu_stream_get(hal_imu_stream *o) {memset(o,0,sizeof *o);o->hz=mock_stream_hz;o->queued=mock_stream_queued;}
int imu_worker_pending(void) {return mock_imu_pending;}
void workout_storage_get(workout_storage_status *o) {*o=mock_storage;}
int svc_track_pending(void) {return mock_track_pending;}
int svc_track_saving(void) {return mock_track_saving;}
int hal_rec_active(void) {return mock_rec;}
int hal_audio_playing(void) {return mock_audio;}
static esp_partition_t mock_part={"ota_1",1024*1024,0x200000};
static int mock_no_part,mock_begin_error,mock_end_error,mock_write_error,mock_boot_error;
static int mock_begin_count,mock_end_count,mock_abort_count,mock_boot_count,mock_writes;
static int mock_open_sink;
static void (*mock_end_hook)(void);
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *p) {return mock_no_part?NULL:&mock_part;}
esp_err_t esp_ota_begin(const esp_partition_t *p,size_t n,esp_ota_handle_t *h)
{mock_begin_count++;
        if(mock_begin_error) {return ESP_FAIL;}assert(!mock_open_sink);mock_open_sink=1;*h=17;return ESP_OK;}
esp_err_t esp_ota_write(esp_ota_handle_t h,const void *p,size_t n)
{assert(h==17 && mock_open_sink && n>0);mock_writes++;return mock_write_error?ESP_FAIL:ESP_OK;}
esp_err_t esp_ota_end(esp_ota_handle_t h)
{assert(h==17 && mock_open_sink);mock_open_sink=0;mock_end_count++;
        if(mock_end_hook) {mock_end_hook();}return mock_end_error?ESP_FAIL:ESP_OK;}
esp_err_t esp_ota_abort(esp_ota_handle_t h)
{assert(h==17 && mock_open_sink);mock_open_sink=0;mock_abort_count++;return ESP_OK;}
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p)
{assert(!mock_open_sink);mock_boot_count++;return mock_boot_error?ESP_FAIL:ESP_OK;}
static int https_begin_error,https_null,https_perform_error,https_complete=1,https_finish_error;
static int https_open,https_begins,https_performs,https_aborts,https_finishes;
static int https_progress;
static void (*https_perform_hook)(void),(*https_complete_hook)(void);
esp_err_t esp_crt_bundle_attach(void *p) {return ESP_OK;}
esp_err_t esp_https_ota_begin(const esp_https_ota_config_t *c,esp_https_ota_handle_t *h)
{https_begins++;assert(!https_open);*h=NULL;
        if(https_begin_error) {return ESP_FAIL;}
        if(!https_null) {https_open=1;*h=&https_open;}return ESP_OK;}
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t h)
{assert(h && https_open);https_performs++;
        if(https_perform_hook) {https_perform_hook();}
        if(https_perform_error) {return ESP_FAIL;}return https_progress?ESP_ERR_HTTPS_OTA_IN_PROGRESS:ESP_OK;}
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t h)
{assert(h && https_open);
        if(https_complete_hook) {https_complete_hook();}return https_complete;}
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t h)
{assert(h && https_open);https_open=0;https_finishes++;return https_finish_error?ESP_FAIL:ESP_OK;}
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t h)
{assert(h && https_open);https_open=0;https_aborts++;return ESP_OK;}
static int sd_lease,sd_locked,sd_acquire_fail,sd_lock_fail,sd_open_fail,sd_read_fail,sd_close_fail,sd_seek_fail,sd_opened;
static size_t sd_length=8192,sd_offset;
static int sd_opens,sd_closes,sd_reads,sd_renames,sd_rename_fail;
int board_sd_asset_acquire(void) {if(sd_acquire_fail) {return 0;}assert(!sd_lease);sd_lease=1;return 1;}
void board_sd_asset_release(void) {assert(sd_lease && !sd_opened && !sd_locked);sd_lease=0;}
int hal_sports_sd_lock(void) {assert(sd_lease && !sd_locked);
        if(sd_lock_fail) {return 0;}sd_locked=1;return 1;}
void hal_sports_sd_unlock(void) {assert(sd_locked && !sd_opened);sd_locked=0;}
static FILE *sd_open(const char *path,const char *mode)
{assert(sd_locked && !sd_opened);sd_opens++;
        if(sd_open_fail) {return NULL;}sd_opened=1;sd_offset=0;return (FILE*)&sd_opened;}
static int sd_seek(FILE *f,long offset,int whence)
{assert(sd_locked && sd_opened);
        if(sd_seek_fail) {return -1;}sd_offset=whence==SEEK_END?sd_length:(size_t)offset;return 0;}
static long sd_tell(FILE *f) {assert(sd_locked && sd_opened);return (long)sd_offset;}
static size_t sd_read(void *p,size_t size,size_t count,FILE *f)
{assert(sd_locked && sd_opened && size==1 && sd_offset+count<=sd_length);sd_reads++;
        if(sd_read_fail) {return 0;}memset(p,0x35,count);sd_offset+=count;return count;}
static int sd_error(FILE *f) {assert(sd_locked && sd_opened);return sd_read_fail;}
static int sd_close(FILE *f) {assert(sd_locked && sd_opened);sd_opened=0;sd_closes++;return sd_close_fail?-1:0;}
static int sd_remove(const char *p) {assert(sd_locked);return 0;}
static int sd_rename(const char *a,const char *b) {assert(sd_locked);sd_renames++;return sd_rename_fail?-1:0;}

static void reset(void)
{
    assert(!atomic_load(&s_busy) && !atomic_load(&s_writer_waiting) && !mock_open_sink && !https_open && !sd_lease && !sd_locked && !sd_opened && !mock_task_fn);
    mock_canaries();
    mock_alloc_fail=mock_task_fail=mock_async_fail=mock_recv_stall=mock_recv_oversize=0;
    mock_recv_stop=-1;mock_recv_hook=mock_reboot_hook=NULL;
    mock_external_stack=0;
    mock_delay_hook=NULL;
    mock_prefs=mock_loading=mock_imu_state=mock_file_busy=mock_track=mock_shutdown=0;memset(&mock_workout,0,sizeof mock_workout);
    mock_imu_pending=mock_stream_hz=mock_stream_queued=mock_track_pending=mock_track_saving=mock_rec=mock_audio=mock_voice_busy=0;
    memset(&mock_storage,0,sizeof mock_storage);mock_storage.ready=1;
    mock_no_part=mock_begin_error=mock_end_error=mock_write_error=mock_boot_error=0;
    mock_begin_count=mock_end_count=mock_abort_count=mock_boot_count=mock_writes=mock_reboots=mock_complete_count=0;mock_end_hook=NULL;
    https_begin_error=https_null=https_perform_error=https_finish_error=https_progress=0;https_complete=1;
    https_begins=https_performs=https_aborts=https_finishes=0;https_perform_hook=https_complete_hook=NULL;
    sd_acquire_fail=sd_lock_fail=sd_open_fail=sd_read_fail=sd_close_fail=sd_seek_fail=sd_rename_fail=0;
    sd_opens=sd_closes=sd_reads=sd_renames=0;sd_length=8192;
    ota_session_end();atomic_store(&mock_us,1000000);ota_session_begin();ota_poll();
}
static httpd_req_t request(size_t n)
{httpd_req_t r={.content_len=n,.method=HTTP_POST,.token=s_session_token};return r;}
static void revoked(void) {ota_session_end();}
static void replaced(void) {ota_session_end();ota_session_begin();}
static void expire(void) {atomic_fetch_add(&mock_us,600000001);}
static void timeout_step(void) {atomic_fetch_add(&mock_us,20000000);}
static void reboot_exclusive(void) {assert(atomic_load(&s_busy));assert(!writer_claim(session_live(NULL)));assert(!ota_activity_begin());}
static void *claim_thread(void *unused)
{return (void*)(uintptr_t)writer_claim(session_live(NULL));}

static void test_admission(void)
{
    reset();ota_session_end();httpd_req_t r=request(32);web_ota(&r);assert(r.response==403 && !r.recv_calls && !mock_task_fn);
    int alloc_before=mock_alloc_count;r=request(32);web_ota_url(&r);assert(r.response==403 && !r.recv_calls && mock_alloc_count==alloc_before);
    web_update_page(&r);assert(r.response==403);
    assert(ota_from_url("https://example.test/a.bin")==ESP_ERR_INVALID_STATE);assert(ota_from_sd()==ESP_ERR_INVALID_STATE);
    reset();r=request(32);r.token="00000000000000000000000000000000";web_ota(&r);assert(r.response==403 && !r.recv_calls);
    reset();r=request(32);r.token=NULL;web_ota_url(&r);assert(r.response==403 && !r.recv_calls && !mock_task_fn);
    reset();r=request(0);web_ota(&r);assert(r.response==400 && !atomic_load(&s_busy));
    reset();uint32_t old=session_live(NULL);ota_session_begin();assert(!writer_claim(old));
    for(int i=0;i<23;i++) {
        reset();
        if(i==0) mock_prefs=1;
        if(i==1) mock_loading=1;
        if(i==2) mock_imu_state=HAL_IMU_RECORDING;
        if(i==3) mock_file_busy=1;
        if(i==4) mock_workout.state=WORKOUT_RUNNING;
        if(i==5) mock_workout.state=WORKOUT_PAUSED;
        if(i==6) mock_track=1;
        if(i==7) mock_imu_pending=1;
        if(i==8) mock_stream_hz=25;
        if(i==9) mock_stream_queued=1;
        if(i==10) mock_storage.ready=0;
        if(i==11) mock_storage.busy=1;
        if(i==12) mock_storage.dirty=1;
        if(i==13) mock_storage.uncertain=1;
        if(i==14) {mock_workout.state=WORKOUT_FINISHED;mock_workout.save_state=WORKOUT_SAVE_PENDING;}
        if(i==15) mock_track_pending=1;
        if(i==16) mock_track_saving=1;
        if(i==17) mock_rec=1;
        if(i==18) mock_audio=1;
        if(i==19) {mock_workout.state=WORKOUT_FINISHED;mock_workout.save_state=WORKOUT_SAVE_FAILED;}
        if(i==20) mock_imu_state=HAL_IMU_STOPPING;
        if(i==21) mock_shutdown=1;
        if(i==22) mock_voice_busy=1;
        ota_poll();assert(!writer_claim(session_live(NULL)) && !atomic_load(&s_busy));
    }
    reset();pthread_t a,b;assert(!pthread_create(&a,NULL,claim_thread,NULL));assert(!pthread_create(&b,NULL,claim_thread,NULL));void *x,*y;
    pthread_join(a,&x);pthread_join(b,&y);assert((uintptr_t)x+(uintptr_t)y==1);writer_release();
    reset();atomic_fetch_add(&mock_us,2000001);assert(!writer_claim(session_live(NULL)));ota_poll();assert(writer_claim(session_live(NULL)));
    mock_prefs=1;ota_poll();assert(!writer_live());writer_release();
    reset();assert(ota_activity_begin());assert(!ota_busy() && !writer_claim(session_live(NULL)));
    mock_workout.state=WORKOUT_RUNNING;ota_activity_end();assert(!writer_claim(session_live(NULL)));
    assert(ota_activity_begin());mock_workout.state=WORKOUT_IDLE;ota_activity_end();assert(writer_claim(session_live(NULL)));
    assert(ota_busy() && !ota_activity_begin());writer_release();
    puts("PASS PLAT-002: physical-session/token admission, generation binding, 22 active/pending/finalizing/unknown/shutdown policy gates and concurrent CAS writer ownership");
}
static int handoff_mode,handoff_delays;
static void handoff(uint32_t ticks)
{
    assert(ticks && atomic_load(&s_busy)==2 && atomic_load(&s_writer_waiting));
    assert(!ota_activity_begin()); /* next UI scope never waits/reacquires */
    if(++handoff_delays<6) return; /* 30 ms actual current-frame handoff */
    mock_delay_hook=NULL;
    if(handoff_mode==1) mock_track_pending=1;
    if(handoff_mode==2) {ota_session_end();ota_session_begin();}
    if(handoff_mode==3) mock_prefs=0;
    ota_activity_end();
}
static void test_claim_handoff(void)
{
    reset();mock_external_stack=1;
    assert(ota_from_url("https://example.test/a.bin")==ESP_ERR_INVALID_STATE);
    assert(ota_from_sd()==ESP_ERR_INVALID_STATE && !atomic_load(&s_busy) && !mock_begin_count);
    for(handoff_mode=0;handoff_mode<4;handoff_mode++) {
        reset();
        if(handoff_mode==3) {mock_prefs=1;ota_poll();}
        assert(ota_activity_begin());handoff_delays=0;mock_delay_hook=handoff;
        int64_t start=esp_timer_get_time();uint32_t generation=session_live(NULL);
        int claimed=writer_claim(generation);
        assert(handoff_delays==6 && esp_timer_get_time()-start==30000);
        assert(claimed==(handoff_mode==0 || handoff_mode==3));
        assert(!atomic_load(&s_writer_waiting));
        if(claimed) {assert(!ota_activity_begin());writer_release();}
        assert(!atomic_load(&s_busy));
    }
    reset();assert(ota_activity_begin());int64_t start=esp_timer_get_time();
    assert(!writer_claim(session_live(NULL)) && esp_timer_get_time()-start==200000);
    assert(!atomic_load(&s_writer_waiting) && atomic_load(&s_busy)==2);
    ota_activity_end();assert(ota_activity_begin());
    unsigned reads=mock_policy_reads;ota_external_activity_end();
    assert(mock_policy_reads==reads && !atomic_load(&s_busy) && !s_runtime_checked);
    assert(!writer_claim(session_live(NULL)));assert(ota_activity_begin());ota_activity_end();
    assert(writer_claim(session_live(NULL)));start=esp_timer_get_time();
    assert(!writer_claim(session_live(NULL)) && esp_timer_get_time()==start);writer_release();
    puts("PASS PLAT-002: bounded 30ms UI handoff, fair nonblocking intent, fresh policy and generation rechecks, exact 200ms timeout, immediate writer collision, transport end reads no UI state");
}
static pthread_mutex_t concurrent_lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t concurrent_cv=PTHREAD_COND_INITIALIZER;
static int concurrent_waiting,concurrent_released;
static void concurrent_delay(uint32_t ticks)
{
    assert(ticks && !pthread_mutex_lock(&concurrent_lock));
    concurrent_waiting=1;assert(!pthread_cond_broadcast(&concurrent_cv));
    while(!concurrent_released) assert(!pthread_cond_wait(&concurrent_cv,&concurrent_lock));
    assert(!pthread_mutex_unlock(&concurrent_lock));
}
static void test_concurrent_policy_release(void)
{
    for(int blocked=0;blocked<2;blocked++) {
        reset();assert(ota_activity_begin());
        concurrent_waiting=concurrent_released=0;mock_delay_hook=concurrent_delay;
        pthread_t writer;assert(!pthread_create(&writer,NULL,claim_thread,NULL));
        assert(!pthread_mutex_lock(&concurrent_lock));
        while(!concurrent_waiting) assert(!pthread_cond_wait(&concurrent_cv,&concurrent_lock));
        assert(atomic_load(&s_busy)==2 && atomic_load(&s_writer_waiting));
        assert(!ota_activity_begin()); /* UI retry returns immediately */
        mock_track_pending=blocked;ota_activity_end();
        concurrent_released=1;assert(!pthread_cond_broadcast(&concurrent_cv));
        assert(!pthread_mutex_unlock(&concurrent_lock));
        void *claimed;assert(!pthread_join(writer,&claimed));mock_delay_hook=NULL;
        assert((uintptr_t)claimed==(uintptr_t)!blocked);
        if(claimed) {assert(atomic_load(&s_busy)==1 && !ota_activity_begin());writer_release();}
        assert(!atomic_load(&s_busy) && !atomic_load(&s_writer_waiting));
    }
    puts("PASS PLAT-002: actual concurrent UI scope and OTA claimant serialize through fresh release policy; pending track blocks claim and idle hands off exclusively");
}
static int external_delays;
static void external_handoff(uint32_t ticks)
{
    assert(ticks && atomic_load(&s_writer_waiting) && atomic_load(&s_busy)==2);
    assert(!ota_activity_begin());
    if(++external_delays==6) {mock_delay_hook=NULL;ota_external_activity_end();}
}
static void test_external_claim(void)
{
    reset();ota_session_end();unsigned reads=mock_policy_reads;
    assert(ota_external_activity_begin() && atomic_load(&s_busy)==2 && !ota_activity_begin());
    assert(mock_policy_reads==reads);ota_external_activity_end();assert(!s_runtime_checked);
    assert(ota_activity_begin());external_delays=0;mock_delay_hook=external_handoff;
    int64_t start=esp_timer_get_time();assert(ota_external_activity_begin());
    assert(esp_timer_get_time()-start==30000 && external_delays==6 && mock_policy_reads==reads);
    assert(atomic_load(&s_busy)==2 && !atomic_load(&s_writer_waiting));ota_external_activity_end();
    assert(ota_activity_begin());start=esp_timer_get_time();assert(!ota_external_activity_begin());
    assert(esp_timer_get_time()-start==200000 && !atomic_load(&s_writer_waiting));ota_external_activity_end();
    reset();assert(writer_claim(session_live(NULL)));reads=mock_policy_reads;start=esp_timer_get_time();
    assert(!ota_external_activity_begin() && esp_timer_get_time()==start && mock_policy_reads==reads);writer_release();
    puts("PASS PLAT-002: external USB/BLE claims need no setup session or UI reads, fair 30ms handoff, bounded 200ms timeout, immediate OTA rejection and invalidating release");
}
static void test_upload(void)
{
    for(int fault=0;fault<11;fault++) {
        reset();httpd_req_t r=request(8192);
        if(fault==0) r.content_len=mock_part.size+1;
        if(fault==1) mock_async_fail=1;
        if(fault==2) mock_task_fail=1;
        if(fault==3) mock_alloc_fail=1;
        if(fault==4) mock_begin_error=1;
        if(fault==5) mock_recv_stop=1500;
        if(fault==6) mock_recv_stall=1;
        if(fault==7) mock_recv_oversize=1;
        if(fault==8) mock_write_error=1;
        if(fault==9) mock_end_error=1;
        if(fault==10) mock_end_hook=revoked;
        web_ota(&r);
        if(mock_task_fn) {mock_run_task();}
        assert(!atomic_load(&s_busy) && !mock_open_sink && !mock_reboots && !mock_boot_count);
        assert(r.response>=400 && r.max_recv<=OTA_CHUNK);
        if(fault==4) assert(!mock_abort_count); /* failed begin never owns a handle */
        if(fault==9 || fault==10) assert(!mock_abort_count && mock_end_count==1);
    }
    reset();httpd_req_t r=request(8192);mock_recv_hook=replaced;web_ota(&r);mock_run_task();assert(!mock_boot_count && mock_abort_count==1);
    reset();r=request(8192);mock_reboot_hook=reboot_exclusive;web_ota(&r);assert(atomic_load(&s_busy));mock_run_task();
    assert(mock_boot_count==1 && mock_reboots==1 && mock_complete_count==1 && !atomic_load(&s_busy));
    puts("PASS PLAT-002: upload size/allocation/task/async/start/read/deadline/write/end faults, revocation before boot, exact-once cleanup and reboot exclusivity");
}
static void test_url(void)
{
    for(int fault=0;fault<8;fault++) {
        reset();
        if(fault==0) https_begin_error=1;
        if(fault==1) https_null=1;
        if(fault==2) https_perform_error=1;
        if(fault==3) https_complete=0;
        if(fault==4) https_perform_hook=revoked;
        if(fault==5) https_complete_hook=expire;
        if(fault==6) {https_progress=1;https_perform_hook=timeout_step;}if(fault==7) https_finish_error=1;
        assert(ota_from_url("https://example.test/a.bin")!=ESP_OK);
        assert(!atomic_load(&s_busy) && !https_open && !mock_reboots);
        if(fault<2) assert(!https_performs && !https_aborts && !https_finishes);
        else assert(https_aborts+https_finishes==1);
    }
    const char *bad[]={"", "ftp://a/b", "https://", "https:///a", "https://a/b ","https://a/b\nextra"};
    for(unsigned i=0;i<sizeof bad/sizeof bad[0];i++) {reset();assert(ota_from_url(bad[i])==ESP_ERR_INVALID_ARG && !https_begins);}
    reset();static const uint8_t hidden[]="https://example.test/a\0https://example.test/b";httpd_req_t r=request(sizeof hidden-1);r.body=hidden;
    web_ota_url(&r);assert(r.response==400 && !mock_task_fn && !atomic_load(&s_busy));
    reset();r=request(32);mock_recv_stall=1;web_ota_url(&r);assert(r.response==408 && !mock_task_fn && !atomic_load(&s_busy));
    reset();r=request(24);r.body=(const uint8_t*)"https://example.test/a.bin";mock_recv_hook=revoked;web_ota_url(&r);
    assert(r.response==408 && !mock_task_fn && !https_begins && !atomic_load(&s_busy));
    reset();r=request(24);r.body=(const uint8_t*)"https://example.test/a.bin";mock_task_fail=1;web_ota_url(&r);assert(r.response==500 && !atomic_load(&s_busy));
    reset();mock_reboot_hook=reboot_exclusive;assert(ota_from_url("https://example.test/a.bin")==ESP_OK);assert(https_finishes==1 && !https_aborts && mock_reboots==1);
    puts("PASS PLAT-002: URL begin/null/perform/incomplete/finish/expiry/deadline faults; raw NUL/control/trailing-byte rejection; no private URL logging");
}
static void test_sd(void)
{
    for(int fault=0;fault<10;fault++) {
        reset();
        if(fault==0) sd_acquire_fail=1;
        if(fault==1) sd_lock_fail=1;
        if(fault==2) sd_open_fail=1;
        if(fault==3) sd_read_fail=1;
        if(fault==4) sd_close_fail=1;
        if(fault==5) sd_seek_fail=1;
        if(fault==6) sd_length=0;
        if(fault==7) sd_length=mock_part.size+1;
        if(fault==8) mock_begin_error=1;
        if(fault==9) mock_write_error=1;
        assert(ota_from_sd()!=ESP_OK);assert(!sd_lease && !sd_locked && !sd_opened && !atomic_load(&s_busy) && !mock_boot_count && !mock_reboots);
        if(fault<8) assert(!mock_begin_count);
    }
    reset();mock_reboot_hook=reboot_exclusive;sd_rename_fail=1;assert(ota_from_sd()==ESP_OK);
    assert(sd_opens==2 && sd_closes==2 && sd_reads==2 && sd_renames==1 && mock_reboots==1 && !sd_lease);
    puts("PASS PLAT-002: SD lease/lock/open/seek/read/close/size/start/write failures; bounded per-read ownership, retained image on rename failure");
}
int main(void)
{test_admission();test_claim_handoff();test_concurrent_policy_release();test_external_claim();test_upload();test_url();test_sd();mock_canaries();puts("PASS actual ota.c fault suite");return 0;}
