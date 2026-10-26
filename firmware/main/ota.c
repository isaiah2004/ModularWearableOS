#include "psram.h"
/* ota.c — see ota.h. One writer at a time (s_busy); the image streams straight
 * into flash in small chunks, so no RAM-sized buffer is needed. esp_ota_end()
 * validates the image header and SHA-256 before the boot slot is switched. */
#include "ota.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_memory_utils.h"
#include "board.h"
#include "prefs.h"
#include "voice_chat.h"
#include "nos.h"
#include "sports.h"
#include "workout.h"
#include "svc_track.h"
#include "imu_backend.h"
#include "hal_ext.h"
#include "esp_ota_ops.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OTA_SD_PATH  "/sdcard/update.bin"
#define OTA_SD_DONE  "/sdcard/update.done"
#define OTA_CHUNK    4096

static atomic_int s_busy;
/* One non-UI claimant offers the current mutation scope up to 200 ms to finish.
 * Intent stops the UI from reacquiring owner 2; UI admission never waits. */
static atomic_int s_writer_waiting;
static portMUX_TYPE s_session_lock=portMUX_INITIALIZER_UNLOCKED;
static int64_t s_session_deadline;
static char s_session_token[33];
static uint32_t s_session_generation;
/* Published before a worker is launched; only that exclusive owner releases it. */
static uint32_t s_writer_generation;
static int64_t s_writer_deadline;
static int s_runtime_blocked=1;
static int64_t s_runtime_checked;
/* Native host owns shutdown admission; read it only in the UI publication. */
extern int nos_host_shutdown_pending(void);
/* These service APIs are UI-thread-owned. Publish admission policy from that
 * thread, never dereference a mutable workout/preferences object on HTTP tasks. */
void ota_poll(void)
{
    hal_imu_record recording;hal_imu_record_get(&recording);
    hal_imu_file file;hal_imu_file_get(&file);
    hal_imu_stream stream;hal_imu_stream_get(&stream);
    workout_storage_status storage;workout_storage_get(&storage);
    const workout_snapshot *workout=workout_get();
    int blocked=nos_host_shutdown_pending() || prefs_pending() || prefs_loading() || voice_chat_active() ||
        (recording.state!=HAL_IMU_IDLE && recording.state!=HAL_IMU_ERROR) || file.busy ||
        imu_worker_pending() || stream.hz || stream.queued ||
        !workout || workout->state==WORKOUT_RUNNING || workout->state==WORKOUT_PAUSED ||
        (workout->state==WORKOUT_FINISHED && workout->save_state!=WORKOUT_SAVE_SAVED) ||
        !storage.ready || storage.uncertain || storage.busy || storage.dirty ||
        svc_track_active() || svc_track_pending() || svc_track_saving() ||
        hal_rec_active() || hal_audio_playing();
    portENTER_CRITICAL(&s_session_lock);
    s_runtime_blocked=blocked;s_runtime_checked=esp_timer_get_time();
    portEXIT_CRITICAL(&s_session_lock);
}
int ota_busy(void) {return atomic_load(&s_busy)==1;}
/* A read of ota_busy alone cannot close check-then-start races. The UI holds
 * owner 2 around an activity/preference mutation, publishes its new policy,
 * then releases it. A flash writer can only claim owner 0. */
int ota_activity_begin(void)
{
    if(atomic_load(&s_writer_waiting)) return 0;
    int expected=0;
    if(!atomic_compare_exchange_strong(&s_busy,&expected,2)) return 0;
    /* Close the gap between the intent read and ownership CAS. No mutation has
     * happened yet, so releasing here preserves the last published policy. */
    if(atomic_load(&s_writer_waiting)) {
        expected=2;(void)atomic_compare_exchange_strong(&s_busy,&expected,0);return 0;
    }
    return 1;
}
void ota_activity_end(void)
{
    if(atomic_load(&s_busy)!=2) return;
    ota_poll();int expected=2;
    (void)atomic_compare_exchange_strong(&s_busy,&expected,0);
}
int ota_external_activity_begin(void)
{
    if(atomic_load(&s_busy)==1) return 0;
    int expected=0;
    if(!atomic_compare_exchange_strong(&s_writer_waiting,&expected,1)) return 0;
    int claimed=0;
    int64_t deadline=esp_timer_get_time()+200000;
    for(;;) {
        expected=0;
        if(atomic_compare_exchange_strong(&s_busy,&expected,2)) {claimed=1;break;}
        if(expected==1 || esp_timer_get_time()>=deadline) break;
        uint32_t ticks=pdMS_TO_TICKS(5);vTaskDelay(ticks?ticks:1);
    }
    atomic_store(&s_writer_waiting,0);
    return claimed;
}
void ota_external_activity_end(void)
{
    /* Transport tasks cannot inspect UI-owned policy. Invalidate it before
     * release; a subsequent UI scope must publish fresh admission. */
    portENTER_CRITICAL(&s_session_lock);
    if(atomic_load(&s_busy)==2) {
        s_runtime_checked=0;s_runtime_blocked=1;
        int expected=2;(void)atomic_compare_exchange_strong(&s_busy,&expected,0);
    }
    portEXIT_CRITICAL(&s_session_lock);
}
void ota_session_begin(void)
{
    char token[33];
    for(int i=0;i<4;i++) snprintf(token+i*8,9,"%08lx",(unsigned long)esp_random());
    portENTER_CRITICAL(&s_session_lock);
    memcpy(s_session_token,token,sizeof token);
    if(!++s_session_generation) ++s_session_generation;
    s_session_deadline=esp_timer_get_time()+600000000LL;
    portEXIT_CRITICAL(&s_session_lock);
}
void ota_session_end(void)
{
    portENTER_CRITICAL(&s_session_lock);s_session_deadline=0;
    memset(s_session_token,0,sizeof s_session_token);portEXIT_CRITICAL(&s_session_lock);
}
static uint32_t session_live(char *token)
{
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_session_lock);
    uint32_t live=s_session_deadline && now<s_session_deadline?s_session_generation:0;
    if(token) {if(live) memcpy(token,s_session_token,33);else token[0]=0;}
    portEXIT_CRITICAL(&s_session_lock);return live;
}
static uint32_t web_authorized(httpd_req_t *req)
{
    char wanted[33],received[33];unsigned difference=0;
    uint32_t generation=session_live(wanted);
    if(!generation || httpd_req_get_hdr_value_len(req,"X-Watch-Update")!=32 ||
       httpd_req_get_hdr_value_str(req,"X-Watch-Update",received,sizeof received)!=ESP_OK) {
        httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Start Phone setup on the watch, then open /update");return 0;
    }
    for(int i=0;i<32;i++) difference|=(unsigned char)wanted[i]^(unsigned char)received[i];
    memset(wanted,0,sizeof wanted);memset(received,0,sizeof received);
    if(difference) {httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Update session expired or invalid");return 0;}
    return generation;
}
static int writer_claim(uint32_t generation)
{
    if(!generation || atomic_load(&s_busy)==1) return 0;
    int expected=0;
    if(!atomic_compare_exchange_strong(&s_writer_waiting,&expected,1)) return 0;
    int claimed=0;
    int64_t deadline=esp_timer_get_time()+200000;
    for(;;) {
        portENTER_CRITICAL(&s_session_lock);
        int64_t now=esp_timer_get_time();int owner=atomic_load(&s_busy);
        int authorized=generation==s_session_generation && now<s_session_deadline;
        if(authorized && owner==0) {
            expected=0;
            if(atomic_compare_exchange_strong(&s_busy,&expected,1)) {
                /* Ownership is established before checking the policy. UI
                 * cannot mutate it between this snapshot and writer launch. */
                claimed=!s_runtime_blocked && s_runtime_checked &&
                    now>=s_runtime_checked && now-s_runtime_checked<=2000000;
                if(claimed) {s_writer_generation=generation;s_writer_deadline=now+180000000LL;}
                else {expected=1;(void)atomic_compare_exchange_strong(&s_busy,&expected,0);}
            }
        }
        int again=authorized && owner==2 && now<deadline;
        portEXIT_CRITICAL(&s_session_lock);
        if(claimed || !again) break;
        uint32_t ticks=pdMS_TO_TICKS(5);vTaskDelay(ticks?ticks:1);
    }
    atomic_store(&s_writer_waiting,0);
    return claimed;
}
static int writer_live(void)
{
    portENTER_CRITICAL(&s_session_lock);
    int64_t now=esp_timer_get_time();
    int live=atomic_load(&s_busy)==1 && !s_runtime_blocked && s_writer_generation==s_session_generation &&
        now<s_session_deadline && now<s_writer_deadline;
    portEXIT_CRITICAL(&s_session_lock);return live;
}
static void writer_release(void)
{ int expected=1;(void)atomic_compare_exchange_strong(&s_busy,&expected,0); }

static void reboot_soon(void)
{
    printf("OTA: rebooting into the new image\n");
    vTaskDelay(pdMS_TO_TICKS(800));          /* let the HTTP reply / log drain */
    esp_restart();
}

void ota_mark_valid(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_err_t e=esp_ota_mark_app_valid_cancel_rollback();
        if(e==ESP_OK) printf("OTA: new image confirmed (%s)\n", run->label);
        else printf("OTA: image confirmation failed (%s)\n",esp_err_to_name(e));
    }
}

void ota_print_status(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    printf("OTA: running=%s next=%s", run ? run->label : "?", next ? next->label : "none");
    if (run && esp_ota_get_partition_description(run, &d) == ESP_OK)
        printf(" version=%s built=%s %s", d.version, d.date, d.time);
    printf("\n");
}

/* Streaming writer shared by the upload and SD paths. */
typedef struct { esp_ota_handle_t h; const esp_partition_t *part; size_t got; int open; } ota_sink;

static esp_err_t sink_begin(ota_sink *s)
{
    s->part = esp_ota_get_next_update_partition(NULL);
    s->got = 0;
    if (!s->part) return ESP_ERR_NOT_FOUND;   /* single-slot table: no OTA */
    printf("OTA: writing %s @0x%lx\n", s->part->label, (unsigned long)s->part->address);
    if(!writer_live()) return ESP_ERR_TIMEOUT;
    esp_err_t e=esp_ota_begin(s->part, OTA_WITH_SEQUENTIAL_WRITES, &s->h);
    s->open=e==ESP_OK;return e;
}
static void sink_abort(ota_sink *s)
{ if(s->open) {esp_ota_abort(s->h);s->open=0;} }

static esp_err_t sink_finish(ota_sink *s)
{
    if(!writer_live() || !s->got) {sink_abort(s);return ESP_ERR_TIMEOUT;}
    esp_err_t e = esp_ota_end(s->h);          /* checks magic + SHA-256 */
    s->open=0; /* end consumes its handle, including validation failures. */
    if(e==ESP_OK && !writer_live()) e=ESP_ERR_TIMEOUT;
    if (e == ESP_OK) e = esp_ota_set_boot_partition(s->part);
    printf("OTA: %u bytes, %s\n", (unsigned)s->got, e == ESP_OK ? "verified" : esp_err_to_name(e));
    return e;
}

/* ---- HTTP upload: POST /ota with the raw .bin as the body ---- */
static esp_err_t web_ota_write(httpd_req_t *req)
{
    char *buf = nos_mem_alloc(OTA_CHUNK, "ota");
    ota_sink s = {0};
    esp_err_t e = buf ? sink_begin(&s) : ESP_ERR_NO_MEM;
    size_t left = req->content_len;
    while (e == ESP_OK && left > 0) {
        if(!writer_live()) {e=ESP_ERR_TIMEOUT;break;}
        size_t count=left<OTA_CHUNK?left:OTA_CHUNK;
        int n = httpd_req_recv(req, buf, count);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || (size_t)n>count) { e = ESP_FAIL; break; }
        if(!writer_live()) {e=ESP_ERR_TIMEOUT;break;}
        e = esp_ota_write(s.h, buf, n);
        s.got += n; left -= n;
        if ((s.got & 0x7FFFF) < (size_t)n) printf("OTA: %u/%u KB\n", (unsigned)(s.got >> 10), (unsigned)(req->content_len >> 10));
    }
    nos_mem_free(buf);
    if (e == ESP_OK) e = sink_finish(&s);
    else sink_abort(&s);
    if (e != ESP_OK) httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, esp_err_to_name(e));
    else httpd_resp_sendstr(req, "ok, rebooting");
    return e;
}

/* The portal has a PSRAM stack. Flash operations must execute on internal RAM,
 * because writing flash disables the cache that serves external stacks. */
static void web_ota_task(void *arg)
{
    httpd_req_t *req=arg;
    esp_err_t e=web_ota_write(req);
    httpd_req_async_handler_complete(req);
    if(e==ESP_OK) reboot_soon(); /* Keep the writer claimed through reply and reboot. */
    writer_release();
    vTaskDelete(NULL);
}
static esp_err_t web_ota(httpd_req_t *req)
{
    uint32_t generation=web_authorized(req);
    if(!generation) return ESP_OK;
    const esp_partition_t *part=esp_ota_get_next_update_partition(NULL);
    if(req->content_len<=0 || !part || (size_t)req->content_len>part->size)
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid firmware image size");
    if(!writer_claim(generation)) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Save settings and stop recording/workout before updating; another update may be active");
    httpd_req_t *async=NULL;
    if(httpd_req_async_handler_begin(req,&async)!=ESP_OK || !async) {
        writer_release();
        return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"no request memory");
    }
    if(xTaskCreate(web_ota_task,"ota-upload",4096,async,5,NULL)!=pdPASS) {
        writer_release();
        httpd_resp_send_err(async,HTTPD_500_INTERNAL_SERVER_ERROR,"no internal stack memory");
        httpd_req_async_handler_complete(async);
    }
    return ESP_OK;
}

/* ---- URL pull (also used by POST /ota_url with the URL as the body) ---- */
static int url_write(const char *url)
{
    if(!writer_live()) {writer_release();return ESP_ERR_TIMEOUT;}
    printf("OTA: downloading firmware\n"); /* URLs may contain private credentials. */
    esp_http_client_config_t http = {
        .url = url, .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000, .keep_alive_enable = true, .buffer_size = 4096,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t handle=NULL;
    esp_err_t e=esp_https_ota_begin(&cfg,&handle);
    if(e==ESP_OK && !handle) e=ESP_FAIL;
    while(handle && (e==ESP_OK || e==ESP_ERR_HTTPS_OTA_IN_PROGRESS)) {
        if(!writer_live()) {e=ESP_ERR_TIMEOUT;break;}
        e=esp_https_ota_perform(handle);
        if(e==ESP_OK) break;
    }
    if(e==ESP_OK && !esp_https_ota_is_complete_data_received(handle)) e=ESP_FAIL;
    if(e==ESP_OK && !writer_live()) e=ESP_ERR_TIMEOUT;
    if(e==ESP_OK) e=esp_https_ota_finish(handle);
    else if(handle) esp_https_ota_abort(handle);
    printf("OTA: %s\n", e == ESP_OK ? "verified" : esp_err_to_name(e));
    if (e == ESP_OK) reboot_soon();
    writer_release();
    return e;
}
/* HTTP body and API calls accept one printable absolute HTTP(S) URL. Embedded
 * NUL, whitespace, controls and trailing body bytes must never be ignored. */
static int url_valid(const char *url,size_t n)
{
    size_t scheme=n>8 && !memcmp(url,"https://",8)?8:
                  n>7 && !memcmp(url,"http://",7)?7:0;
    if(!scheme || url[scheme]=='/' || url[scheme]=='?' || url[scheme]=='#') return 0;
    for(size_t i=0;i<n;i++) if((unsigned char)url[i]<=32 || (unsigned char)url[i]>=127) return 0;
    return 1;
}
int ota_from_url(const char *url)
{
    unsigned char stack_marker;
    if(!esp_ptr_internal(&stack_marker)) return ESP_ERR_INVALID_STATE;
    if(!url) return ESP_ERR_INVALID_ARG;
    size_t n=strnlen(url,513);
    if(n>512 || !url_valid(url,n)) return ESP_ERR_INVALID_ARG;
    if(!writer_claim(session_live(NULL))) return ESP_ERR_INVALID_STATE;
    return url_write(url);
}

static void url_task(void *arg)
{
    url_write((const char *)arg);
    nos_mem_free(arg);
    vTaskDelete(NULL);
}

static esp_err_t web_ota_url(httpd_req_t *req)
{
    uint32_t generation=web_authorized(req);
    if(!generation) return ESP_OK;
    size_t n = req->content_len;
    if (n == 0 || n > 512) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body = firmware URL");
    if(!writer_claim(generation)) return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Update unavailable: save settings and stop active sessions");
    char *url = nos_mem_alloc(n + 1, "ota");
    if (!url) {writer_release();return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");}
    size_t got = 0;
    while (got < n) {
        if(!writer_live()) {nos_mem_free(url);writer_release();return httpd_resp_send_err(req,HTTPD_408_REQ_TIMEOUT,"Update session expired");}
        int r = httpd_req_recv(req, url + got, n - got);
        if(r==HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || (size_t)r>n-got) { nos_mem_free(url);writer_release();return httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "timeout"); }
        got += r;
    }
    url[n] = 0;
    if(!writer_live()) {nos_mem_free(url);writer_release();return httpd_resp_send_err(req,HTTPD_408_REQ_TIMEOUT,"Update session expired");}
    if(!url_valid(url,n)) {nos_mem_free(url);writer_release();return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Expected one HTTP(S) URL without spaces or controls");}
    /* download on its own task so the request returns now */
    if (xTaskCreate(url_task, "ota_url", 8192, url, 5, NULL) != pdPASS) {
        writer_release();
        nos_mem_free(url);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no task");
    }
    return httpd_resp_sendstr(req, "downloading; the watch reboots when done");
}

/* ---- SD card: /sdcard/update.bin -> renamed to update.done once applied ---- */
/* Close each read under its bounded SD lock. Keeping a descriptor between locks
 * would require unbounded lock retries to close safely after a lock timeout.
 * The mount lease spans the whole operation; image length is checked each read
 * and esp_ota_end still validates the complete streamed image. */
static esp_err_t sd_chunk(char *buf,size_t offset,size_t *total,size_t *got)
{
    if(!writer_live() || !hal_sports_sd_lock()) return ESP_ERR_TIMEOUT;
    FILE *f=fopen(OTA_SD_PATH,"rb");
    esp_err_t e=f?ESP_OK:ESP_ERR_NOT_FOUND;
    long length=-1;
    if(f) {
        if(fseek(f,0,SEEK_END)!=0 || (length=ftell(f))<=0 ||
           (*total && (size_t)length!=*total) || fseek(f,(long)offset,SEEK_SET)!=0) e=ESP_FAIL;
        if(e==ESP_OK) {
            *total=(size_t)length;
            size_t count=*total-offset;if(count>OTA_CHUNK) count=OTA_CHUNK;
            *got=fread(buf,1,count,f);
            if(ferror(f) || *got!=count) e=ESP_FAIL;
        }
        if(fclose(f)!=0) e=ESP_FAIL;
    }
    hal_sports_sd_unlock();return e;
}
int ota_from_sd(void)
{
    unsigned char stack_marker;
    if(!esp_ptr_internal(&stack_marker)) return ESP_ERR_INVALID_STATE;
    if (!writer_claim(session_live(NULL))) return ESP_ERR_INVALID_STATE;
    if(!board_sd_asset_acquire()) {writer_release();return ESP_ERR_NOT_FOUND;}
    char *buf = nos_mem_alloc(OTA_CHUNK, "ota");
    ota_sink s = {0};
    size_t total=0,n=0;
    esp_err_t e=buf?sd_chunk(buf,0,&total,&n):ESP_ERR_NO_MEM;
    const esp_partition_t *part=esp_ota_get_next_update_partition(NULL);
    if(e==ESP_OK && (!part || total>part->size)) e=ESP_ERR_INVALID_SIZE;
    if(e==ESP_OK) e=sink_begin(&s);
    while(e==ESP_OK) {
        if(!writer_live()) {e=ESP_ERR_TIMEOUT;break;}
        e=esp_ota_write(s.h,buf,n);
        if(e!=ESP_OK) break;
        s.got+=n;
        if(s.got==total) break;
        e=sd_chunk(buf,s.got,&total,&n);
    }
    nos_mem_free(buf);
    if (e == ESP_OK) e = sink_finish(&s);
    else sink_abort(&s);
    if (e != ESP_OK) {board_sd_asset_release();writer_release();return e;}
    if(hal_sports_sd_lock()) {
        remove(OTA_SD_DONE);
        if(rename(OTA_SD_PATH,OTA_SD_DONE)!=0) printf("OTA: applied image retained; rename failed\n");
        hal_sports_sd_unlock();
    } else printf("OTA: applied image retained; rename lock unavailable\n");
    board_sd_asset_release();
    reboot_soon();
    writer_release();
    return ESP_OK;
}

/* ---- GET /update: phone-friendly upload page ---- */
static esp_err_t web_update_page(httpd_req_t *req)
{
    char token[33];
    if(!session_live(token)) return httpd_resp_send_err(req,HTTPD_403_FORBIDDEN,"Start Phone setup on the watch");
    static const char *html =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Watch update</title><style>body{font-family:system-ui,sans-serif;background:#0d0d10;"
        "color:#eee;max-width:440px;margin:2em auto;padding:0 1.2em}h2{color:#5cd}"
        "input,button{font-size:1em;padding:.65em;width:100%;box-sizing:border-box;margin:.35em 0;"
        "border-radius:10px;border:1px solid #333;background:#1a1a20;color:#eee}"
        "button{background:#5cd;color:#000;font-weight:700;border:0}progress{width:100%}</style>"
        "<h2>&#8986; Firmware update</h2>"
        "<p>Upload a firmware .bin</p><input type=file id=f accept='.bin'>"
        "<button onclick=up()>Upload &amp; install</button><progress id=p max=1 value=0></progress>"
        "<p>or install from a URL</p><input id=u placeholder='https://.../watchfw.bin'>"
        "<button onclick=url()>Download &amp; install</button><p id=s></p>"
        "<script>const s=t=>document.getElementById('s').textContent=t;"
        "function up(){const f=document.getElementById('f').files[0];if(!f)return s('pick a file');"
        "const x=new XMLHttpRequest();x.open('POST','/ota');"
        "x.setRequestHeader('X-Watch-Update',watchToken);"
        "x.upload.onprogress=e=>document.getElementById('p').value=e.loaded/e.total;"
        "x.onload=()=>s(x.status==200?'Installed - the watch is rebooting':'Failed: '+x.responseText);"
        "x.onerror=()=>s('Connection lost');s('Uploading...');x.send(f)}"
        "function url(){fetch('/ota_url',{method:'POST',headers:{'X-Watch-Update':watchToken},body:document.getElementById('u').value})"
        ".then(r=>r.text()).then(s)}</script>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req,"Cache-Control","no-store");
    httpd_resp_set_hdr(req,"Referrer-Policy","no-referrer");
    char bootstrap[96];snprintf(bootstrap,sizeof bootstrap,"<script>const watchToken='%s';</script>",token);
    memset(token,0,sizeof token);
    esp_err_t e=httpd_resp_send_chunk(req,bootstrap,HTTPD_RESP_USE_STRLEN);
    if(e==ESP_OK) e=httpd_resp_send_chunk(req,html,HTTPD_RESP_USE_STRLEN);
    if(e==ESP_OK) e=httpd_resp_send_chunk(req,NULL,0);
    return e;
}

void ota_register_web(httpd_handle_t h)
{
    httpd_uri_t pg = { .uri = "/update",  .method = HTTP_GET,  .handler = web_update_page };
    httpd_uri_t up = { .uri = "/ota",     .method = HTTP_POST, .handler = web_ota };
    httpd_uri_t ur = { .uri = "/ota_url", .method = HTTP_POST, .handler = web_ota_url };
    httpd_register_uri_handler(h, &pg);
    httpd_register_uri_handler(h, &up);
    httpd_register_uri_handler(h, &ur);
}
