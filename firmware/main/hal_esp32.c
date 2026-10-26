#include "sports.h"
#include "psram.h"
/* Active NOS HAL for ESP32-S3-Touch-AMOLED-1.43-B. Strap drivers are optional. */
#include <time.h>
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi_default.h"
#include "esp_http_server.h"
#include "esp_sntp.h"
#include <stdlib.h>   /* setenv */
#include <limits.h>
#include <stdatomic.h>
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"
/* NimBLE passive-observer stack (host + controller) */
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_hs_adv.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host/util/util.h"
#include <stdio.h>
#include "../../core/hal.h"
#include "pins.h"
#include "board.h"
#include "board_protocol.h"
#include "ota.h"
#include "weather_service.h"
#include "sleep_grid.h"
#include "svc.h"      /* hal_phone_* contract (nos/svc.h) */
#include "prefs_disk.h"
#include "prefs.h"
#include "cJSON.h"
#include <sys/stat.h>

/* Advertised + GAP name. Gadgetbridge auto-detects a Bangle.js by a name matching
 * "Bangle\.js.*" (BangleJSCoordinator) and scan-filters on the NUS service UUID. */
#define BLE_NAME "Bangle.js NOS"

void display_set_brightness(uint8_t level); /* active NOS display host, DCS 0x51 */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static volatile int g_synced;

static uint32_t s_frozen_ms;
static int      s_freeze;

static uint32_t millis_raw(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* Optional radio admission never spends the mandatory UI/USB worker reserve.
 * Successful stages remain owned across bounded retries, never re-created. */
typedef struct {
    atomic_uint attempts,failures,deferred,retry_ms;
    atomic_int stage,error;
} radio_startup;
static radio_startup s_ble_startup,s_wifi_startup,s_weather_startup;
static atomic_int s_radios_started,s_radio_busy;
static atomic_flag s_radio_starting=ATOMIC_FLAG_INIT;
static void radio_poll(void);
static int radio_fail(radio_startup *s,int stage,int error)
{
    s->stage=stage;s->error=error;s->failures++;
    s->retry_ms=millis_raw()+5000;
    ESP_LOGW("radio","init deferred stage=%d error=%d attempts=%u failures=%u",stage,error,
             (unsigned)atomic_load(&s->attempts),(unsigned)atomic_load(&s->failures));
    return 0;
}
static int radio_admit(radio_startup *s,unsigned internal,unsigned largest)
{
    uint32_t now=millis_raw(),retry=atomic_load(&s->retry_ms);
    if(retry && (int32_t)(now-retry)<0)return 0;
    if(heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<internal ||
       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<largest ||
       heap_caps_get_free_size(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)<65536 ||
       heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)<16384) {
        s->deferred++;s->stage=1;s->error=ESP_ERR_NO_MEM;s->retry_ms=now+5000;
        ESP_LOGW("radio","init waiting for heap free=%u largest=%u skips=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
                 (unsigned)atomic_load(&s->deferred));
        return 0;
    }
    s->attempts++;return 1;
}

static hal_datetime s_frozen_dt;

uint32_t hal_millis(void)
{
    return s_freeze ? s_frozen_ms : millis_raw();
}

static void get_datetime_raw(hal_datetime *out)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    out->year = tm.tm_year + 1900;
    out->month = tm.tm_mon + 1;
    out->day = tm.tm_mday;
    out->weekday = tm.tm_wday;
    out->hour = tm.tm_hour;
    out->minute = tm.tm_min;
    out->second = tm.tm_sec;
    out->millis = (int)(tv.tv_usec / 1000);
}

void hal_frame_begin(void)
{
    s_frozen_ms = millis_raw();
    get_datetime_raw(&s_frozen_dt);
    s_freeze = 1;
}
void hal_frame_end(void) { s_freeze = 0; }

uint32_t hal_micros(void) { return (uint32_t)esp_timer_get_time(); }

void hal_get_datetime(hal_datetime *out)
{
    if (s_freeze) { *out = s_frozen_dt; return; }
    get_datetime_raw(out);
}

uint64_t hal_epoch_seconds(void)
{
    time_t now=time(NULL);
    return now >= (time_t)1704067200 && now < (time_t)4102444800LL ? (uint64_t)now : 0;
}

int hal_set_datetime_checked(const hal_datetime *dt)
{
    if (!dt || !board_datetime_valid(dt)) { hal_log("Invalid date/time; not changed"); return 0; }
    board_apply_timezone();
    struct tm tm = { 0 };
    tm.tm_year = dt->year - 1900;
    tm.tm_mon  = dt->month - 1;
    tm.tm_mday = dt->day;
    tm.tm_hour = dt->hour;
    tm.tm_min  = dt->minute;
    tm.tm_sec  = dt->second;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    if(t==(time_t)-1) return 0;
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    if(settimeofday(&tv, NULL)) {hal_log("Clock write failed");return 0;}
    board_clock_changed(tv.tv_sec);
    g_synced = 1;
    return 1;
}
void hal_set_datetime(const hal_datetime *dt) { (void)hal_set_datetime_checked(dt); }

/* ---- persistent blob store: SD-authoritative preferences, NVS fallback ---- */
static int nvs_save(const char *key, const void *data, int size)
{
    if (!key || !data || size <= 0) return 0;
    nvs_handle_t h;
    if (nvs_open("wos", NVS_READWRITE, &h) != ESP_OK) return 0;
    esp_err_t err=nvs_set_blob(h, key, data, (size_t)size);
    if (err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    return err==ESP_OK;
}
/* Separate, opaque fallback for explicit field edits. Portable prefs owns its
 * schema/checksum/mask; this adapter only commits and verifies exact bytes.
 * In particular, success here never resolves an unknown SD bank. */
#define PREFS_OVERLAY_CAPACITY 256
static atomic_flag s_overlay_owner=ATOMIC_FLAG_INIT;
static int nvs_load_exact(const char *key,void *data,int size)
{
    nvs_handle_t h;
    esp_err_t e=nvs_open("wos",NVS_READONLY,&h);
    if(e!=ESP_OK) return e==ESP_ERR_NVS_NOT_FOUND?0:-1;
    size_t bytes=(size_t)size;
    e=nvs_get_blob(h,key,data,&bytes);
    nvs_close(h);
    if(e==ESP_ERR_NVS_NOT_FOUND) return 0;
    return e==ESP_OK && bytes==(size_t)size?1:-1;
}
int hal_prefs_overlay_save_checked(const void *data,int size)
{
    if(!data || size<=0 || size>PREFS_OVERLAY_CAPACITY ||
       atomic_flag_test_and_set(&s_overlay_owner)) return 0;
    unsigned char verified[PREFS_OVERLAY_CAPACITY];
    int ok=nvs_save("prefs_overlay",data,size) &&
        nvs_load_exact("prefs_overlay",verified,size)==1 && !memcmp(data,verified,(size_t)size);
    memset(verified,0,sizeof verified);
    atomic_flag_clear(&s_overlay_owner);
    return ok;
}
int hal_prefs_overlay_load(void *data,int size)
{
    if(!data || size<=0 || size>PREFS_OVERLAY_CAPACITY ||
       atomic_flag_test_and_set(&s_overlay_owner)) return -1;
    unsigned char loaded[PREFS_OVERLAY_CAPACITY];
    int status=nvs_load_exact("prefs_overlay",loaded,size);
    if(status==1) memcpy(data,loaded,(size_t)size);
    memset(loaded,0,sizeof loaded);
    atomic_flag_clear(&s_overlay_owner);
    return status;
}
static atomic_int s_preference_read_pending;
int hal_store_load_pending(const char *key)
{ return key && !strcmp(key,"nosprefs") && s_preference_read_pending; }
int hal_store_save_checked(const char *key, const void *data, int size)
{
    if (!key || !data || size <= 0) return 0;
#if !BOARD_HAS_SD
    /* C has no SD hardware. Its checked NVS fallback is the durable store;
     * a missing slot is distinct from a failed save on an SD-capable board. */
    return nvs_save(key,data,size);
#endif
    if (strcmp(key, "nosprefs") && strcmp(key, "wifi_cred")) return nvs_save(key, data, size);
    if(!strcmp(key,"nosprefs") && s_preference_read_pending) return 0;
    int ok = 0,leased=board_sd_asset_acquire();
    board_sd_asset_lock();
    /* Raw restore owns this gate under the same SD mutex. Keep the check,
     * checkpoint and NVS mirror in one serialized section: a mirror written
     * after unlock could otherwise resurrect pre-restore RAM credentials. */
    const char *dir=!strcmp(key,"wifi_cred")?"/sdcard/watch/wifi":"/sdcard/watch";
    int allowed=prefs_disk_write_allowed(dir);
    if(allowed && leased) {
        if (!strcmp(key,"wifi_cred")) mkdir(dir,0777);
        ok = prefs_disk_save(dir, data, (size_t)size);
    }
    /* Mirror for card-absent boots. An unavailable SD keeps the RAM copy dirty. */
    if(allowed) (void)nvs_save(key, data, size);
    board_sd_asset_unlock();
    if(leased) board_sd_asset_release();
    if (ok) ESP_LOGI("prefs", "SD settings checkpoint verified");
    return ok;
}
void hal_store_save(const char *key, const void *data, int size)
{ (void)hal_store_save_checked(key,data,size); }
int hal_store_load(const char *key, void *data, int size)
{
    if (!key || !data || size <= 0) return 0;
    int preferences=!strcmp(key,"nosprefs");
    if (BOARD_HAS_SD && (preferences || !strcmp(key,"wifi_cred"))) {
        if(board_sd_asset_acquire()) {
            board_sd_asset_lock();
            prefs_disk_read_status status=prefs_disk_load_status(!strcmp(key,"wifi_cred")?"/sdcard/watch/wifi":"/sdcard/watch",data,(size_t)size);
            board_sd_asset_unlock();
            board_sd_asset_release();
            if(preferences) s_preference_read_pending=status==PREFS_DISK_IO || status==PREFS_DISK_CORRUPT;
            if(status==PREFS_DISK_OK) {ESP_LOGI("prefs","Loaded checked SD settings size=%d",size);return 1;}
            if(status!=PREFS_DISK_MISSING) {
                ESP_LOGW("prefs","SD load unresolved status=%d size=%d; retaining card state",status,size);
                return 0; /* Never silently replace unreadable settings with NVS. */
            }
        } else if(preferences) s_preference_read_pending=1;
    }
    nvs_handle_t h;
    if (nvs_open("wos", NVS_READONLY, &h) != ESP_OK) return 0;
    size_t sz = (size_t)size;
    esp_err_t e = nvs_get_blob(h, key, data, &sz);
    nvs_close(h);
    return (e == ESP_OK && sz == (size_t)size) ? 1 : 0;
}

/* GPIO4 is the system rail, not a fuel gauge. Only board_task calls this
 * sampler; all readers receive a cached snapshot under the short lock. */
static portMUX_TYPE s_power_lock=portMUX_INITIALIZER_UNLOCKED;
static hal_power s_power={.battery_percent=-1,.external_power=-1,.age_ms=UINT32_MAX}; /* -1: no estimate yet (was a false 50) */
static uint32_t s_power_ms;
static int s_power_seen;
static int s_battery_stable, s_high_stable, s_smooth_mv;

/* Battery percent from the rail voltage. The old straight line (3.3 V = 0 %, 4.2 V = 100 %)
 * read a full cell as ~75-80 %: a LiPo spends most of its charge between 3.7 and 4.1 V, the
 * charger (ETA6098) only restarts after the cell falls 160 mV below 4.2 V, and the rail sags a
 * little under load. So: a resting-voltage (OCV) curve for a LiPo, a small load allowance
 * (screen on ~90 mA, off ~40 mA through the cell's ~0.25 ohm + the power-path PMOS), and a stored
 * calibration offset (serial `batcal MV`: the cell's true voltage from a meter minus this
 * reading) for the divider/ADC error, which can be +-150 mV at the rail. */
static int s_bat_cal_mv, s_bat_cal_loaded, s_bat_awake = 1;
/* Battery-only drop between the cell and the B divider (power path + cell resistance). Fitted
 * 2026-10-25 from one B cycle: USB plateau 4150 (CV 4.2 V) and overnight 3995 vs a 4.06 V meter
 * give ADC cal +65; first battery minute read 3910 screen-on, so ~85 awake / ~75 idle. The
 * 1.43C samples VBAT/2 directly and keeps the old small allowance. */
#ifdef PIN_CHG_STAT
#define BAT_DROP_AWAKE_MV 25
#define BAT_DROP_IDLE_MV  10
#else
#define BAT_DROP_AWAKE_MV 85
#define BAT_DROP_IDLE_MV  75
#endif
/* Displayed percent on battery moves one step only after the estimate has differed for 30 s,
 * so a radio burst (Wi-Fi start sagged the rail ~50 mV = ~20 % on the flat curve) cannot jump it. */
#define BAT_STEP_MS 30000u
static int s_pct_shown = -1;
static uint32_t s_pct_step_ms;
static int lipo_percent(int mv)
{
    static const short MV[] = { 3300, 3610, 3690, 3710, 3730, 3750, 3770, 3790, 3800, 3820, 3840,
                                3850, 3870, 3910, 3950, 3980, 4020, 4080, 4110, 4150, 4200 };
    if (mv <= MV[0]) return 0;
    for (int i = 1; i < (int)(sizeof MV / sizeof MV[0]); i++)
        if (mv <= MV[i]) return (i - 1) * 5 + (mv - MV[i - 1]) * 5 / (MV[i] - MV[i - 1]);
    return 100;
}
void hal_battery_set_awake(int awake) { s_bat_awake = awake; }
int hal_battery_cal_mv(void) { return s_bat_cal_mv; }
void hal_battery_cal_set(int mv)
{
    s_bat_cal_mv = mv < -400 ? -400 : mv > 400 ? 400 : mv;
    hal_store_save("bat_cal", &s_bat_cal_mv, sizeof s_bat_cal_mv);
}
void hal_esp32_sample_power(void)
{
    if (!s_bat_cal_loaded) { s_bat_cal_loaded = 1; if (!hal_store_load("bat_cal", &s_bat_cal_mv, sizeof s_bat_cal_mv)) s_bat_cal_mv = 0; }
    hal_power p=s_power; /* This task is the snapshot's only writer. */
    uint32_t now_ms=millis_raw();
    if(s_power_seen && now_ms-s_power_ms>3000) { s_battery_stable=0; s_high_stable=0; }
    int samples[8], count=0;
    int usb=usb_serial_jtag_is_connected(); /* SOF from a host, not charge current. */
#ifdef PIN_CHG_STAT
    if(!gpio_get_level(PIN_CHG_STAT)) usb=1; /* 1.43C: GPIO4 is VBAT/2; STAT low = charging */
#endif
    for (int i=0;s_adc && i<8;i++) {
        int raw=0,mv=0;
        if (adc_oneshot_read(s_adc,BAT_ADC_CHANNEL,&raw)!=ESP_OK) continue;
        if (s_cali) {
            if(adc_cali_raw_to_voltage(s_cali,raw,&mv)!=ESP_OK) continue;
        } else mv=raw*3300/4095;
        mv*=BAT_DIVIDER;
        if(mv>=2500 && mv<=5600) samples[count++]=mv;
    }
    p.sample_valid=count>=5;
    p.battery_valid=0; p.external_power=usb ? 1 : -1;
    if(p.sample_valid) {
        for(int i=1;i<count;i++) {
            int value=samples[i],j=i;
            while(j>0 && samples[j-1]>value) { samples[j]=samples[j-1]; j--; }
            samples[j]=value;
        }
        int measured=samples[count/2];
        s_smooth_mv=s_smooth_mv ? (s_smooth_mv*4+measured)/5 : measured;
        p.rail_mv=s_smooth_mv;
        if(s_cali && measured>=4400) {
            if(s_high_stable<3) s_high_stable++;
        } else s_high_stable=0;
        if(usb || s_high_stable>=3) {
            p.external_power=1; s_battery_stable=0; s_pct_shown=-1;
            /* On USB the charger holds the cell between 4.04 and 4.2 V, so the rail reads a little
             * high; still a better estimate than the boot-time placeholder (50) the UI showed. */
            if(s_smooth_mv>=3300 && s_smooth_mv<=4300) p.battery_percent=lipo_percent(s_smooth_mv+s_bat_cal_mv);
        } else if(s_cali && measured>=3300 && measured<=4250 &&
                  s_smooth_mv>=3300 && s_smooth_mv<=4250 &&
                  samples[count-1]-samples[0]<=120) {
            if(s_battery_stable<10) s_battery_stable++;
            if(s_battery_stable>=10) {
                p.external_power=0; p.battery_valid=1;
                int target=lipo_percent(s_smooth_mv+s_bat_cal_mv+(s_bat_awake ? BAT_DROP_AWAKE_MV : BAT_DROP_IDLE_MV));
                if(s_pct_shown<0 || target==s_pct_shown) { s_pct_shown=target; s_pct_step_ms=now_ms; }
                else if(now_ms-s_pct_step_ms>=BAT_STEP_MS) { s_pct_shown+=target<s_pct_shown?-1:1; s_pct_step_ms=now_ms; }
                p.battery_percent=s_pct_shown;
            }
        } else s_battery_stable=0;
        /* Until the 10-sample qualification (or on USB) the UI would otherwise have no number: give an
         * unvalidated rail estimate (battery_valid stays 0) instead of a placeholder. */
        if(p.battery_percent<0 && s_cali && s_smooth_mv>=3300 && s_smooth_mv<=4250)
            p.battery_percent=lipo_percent(s_smooth_mv+s_bat_cal_mv+(s_bat_awake ? BAT_DROP_AWAKE_MV : BAT_DROP_IDLE_MV));
    } else { s_battery_stable=0; s_high_stable=0; }
    p.age_ms=0;
    portENTER_CRITICAL(&s_power_lock);
    s_power=p; s_power_ms=millis_raw(); s_power_seen=1;
    portEXIT_CRITICAL(&s_power_lock);
    float reading[]={(float)p.battery_percent,(float)p.rail_mv,(float)p.external_power,(float)p.battery_valid,(float)p.sample_valid,(float)sports_mode()};
    sports_emit(SP_RAIL,reading,6);
    static int external=-2,valid=-1;
    if(external!=p.external_power || valid!=p.battery_valid) {
        sports_emit(SP_BATTERY,reading,6);external=p.external_power;valid=p.battery_valid;
    }

}

void hal_power_get(hal_power *out)
{
    if(!out) return;
    portENTER_CRITICAL(&s_power_lock);
    *out=s_power;
    out->age_ms=s_power_seen ? millis_raw()-s_power_ms : UINT32_MAX;
    portEXIT_CRITICAL(&s_power_lock);
    if(out->age_ms>3000) {
        out->sample_valid=0; out->battery_valid=0; out->external_power=-1;
    }
}
int hal_battery_mv(void) { hal_power p; hal_power_get(&p); return p.sample_valid ? p.rail_mv : 0; }
/* Legacy faces require 0..100. Retain the last estimate during USB/unknown
 * intervals; Pip and diagnostics must use hal_power_get() for validity. */
int hal_battery_percent(void) { hal_power p; hal_power_get(&p); return p.battery_percent<0?0:p.battery_percent; } /* 0 = unknown, as faces expect */

/* Producers never mutate core notifications. Main thread drains this queue.
 * Capacity matches the core store; a full queue rejects new input explicitly. */
typedef struct { char app[20], title[40]; } pending_notification;
static QueueHandle_t s_notifications;

static int notification_text(const char *text,size_t cap,int is_app)
{
    if(!text || !text[0]) return 0;
    size_t i=0;
    for(;i<cap && text[i];i++) {
        unsigned char c=(unsigned char)text[i];
        if(c<32 || c==127 || (is_app && c=='|')) return 0;
    }
    return i<cap;
}
int hal_notification_post(const char *app,const char *title)
{
    if(!s_notifications || !notification_text(app,20,1) || !notification_text(title,40,0)) return 0;
    pending_notification p={0};
    memcpy(p.app,app,strlen(app)); memcpy(p.title,title,strlen(title));
    return xQueueSend(s_notifications,&p,0)==pdTRUE;
}
int hal_notification_poll(char *app,int an,char *title,int tn)
{
    if(!s_notifications || !app || !title || an<=0 || tn<=0) return 0;
    pending_notification p;
    if(xQueueReceive(s_notifications,&p,0)!=pdTRUE) return 0;
    snprintf(app,(size_t)an,"%s",p.app); snprintf(title,(size_t)tn,"%s",p.title);
    return 1;
}
static int notification_decode(const uint8_t *data,size_t n,pending_notification *out)
{
    if(n<3 || n>59 || memchr(data,0,n)) return 0;
    const uint8_t *separator=memchr(data,'|',n);
    if(!separator) return 0;
    size_t app_len=(size_t)(separator-data), title_len=n-app_len-1;
    if(!app_len || app_len>=sizeof out->app || !title_len || title_len>=sizeof out->title) return 0;
    memset(out,0,sizeof *out);
    memcpy(out->app,data,app_len); memcpy(out->title,separator+1,title_len);
    return notification_text(out->app,sizeof out->app,1) &&
           notification_text(out->title,sizeof out->title,0);
}

void hal_set_backlight(uint8_t level)
{
    display_set_brightness(level);
}

/* ---- in-watch log ring (diagnostics log screen) ---- */
#include <stdarg.h>
#define WLOG_N 32
static char s_wlog[WLOG_N][80];
static volatile int s_wlog_head, s_wlog_cnt;
static void wlog_push(const char *s)
{
    int i = s_wlog_head, j = 0;
    for (; s[j] && j < (int)sizeof s_wlog[i] - 1; j++) s_wlog[i][j] = s[j];
    s_wlog[i][j] = 0;
    s_wlog_head = (s_wlog_head + 1) % WLOG_N;
    if (s_wlog_cnt < WLOG_N) s_wlog_cnt++;
}
/* printf AND capture to the ring — use for events worth seeing on-watch. */
static void wlog(const char *fmt, ...)
{
    char b[80];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("%s\n", b);
    wlog_push(b);
}
int hal_log_count(void) { return s_wlog_cnt; }
void hal_log_clear(void) { s_wlog_cnt = 0; s_wlog_head = 0; }
const char *hal_log_get(int i)
{
    if (i < 0 || i >= s_wlog_cnt) return "";
    int idx = (s_wlog_head - s_wlog_cnt + i + WLOG_N * 4) % WLOG_N;
    return s_wlog[idx];
}
void hal_log(const char *msg)
{
    ESP_LOGI("watch", "%s", msg);
    wlog_push(msg);
}

int hal_imu_read(hal_imu *o) { return board_imu_read(o); }

/* No microphone on the 1.43 board — the visualizer shows "NO MIC". (An
 * external I2S mic, or the 1.43C/1.75/2.06 boards, would fill this in.) */
int hal_audio_bars(uint8_t *lv, int n)
{
    (void)lv; (void)n;
    return 0;
}

/* ---- on-demand BLE discovery -----------------------------------------
 * The device list renews a short active-scan lease. Reports are deduplicated,
 * aged out and copied under a mutex; the renderer never waits for a sweep. */
#define BLE_TABLE_MAX 24
static struct {
    hal_ble_dev table[BLE_TABLE_MAX];
    ble_addr_t addresses[BLE_TABLE_MAX];
    uint32_t seen[BLE_TABLE_MAX];
} *s_ble_records;
#define s_ble_tab (s_ble_records->table)
#define s_ble_addresses (s_ble_records->addresses)
#define s_ble_seen (s_ble_records->seen)
static int             s_ble_count;
static SemaphoreHandle_t s_ble_mtx;
static uint8_t         s_ble_addr_type;
static atomic_int s_ble_ready;
static atomic_uint s_ble_requested_ms;
static atomic_int s_ble_enabled, s_ble_initialized, s_ble_rf_off=1;
static TaskHandle_t s_ble_host_task;
static int s_ble_stack_owned,s_ble_cleanup_pending;
static atomic_int s_central_pending, s_central_result;
static atomic_uint s_central_handle=BLE_HS_CONN_HANDLE_NONE;
static struct ble_npl_event s_ble_control_event;
static struct ble_npl_callout s_ble_control_callout;
static atomic_int s_ble_control_pending, s_ble_control_clock_ready;
static portMUX_TYPE s_ble_control_lock=portMUX_INITIALIZER_UNLOCKED;
static ble_addr_t s_central_peer;
static int s_central_queued, s_central_cancel;
/* Host-task-owned handles cover every connection, not an assumed 0..7 range. */
static uint16_t s_ble_handles[CONFIG_BT_NIMBLE_MAX_CONNECTIONS];
static atomic_uint s_ble_handles_n;
static atomic_int s_ble_control_error;
static void ble_control(struct ble_npl_event *event);
static void ble_control_tick(struct ble_npl_event *event);
static void ble_control_schedule(void)
{
    /* SDK eventq_put may wait indefinitely for queue space. UI callers only
     * publish intent; the host's small 100-ms callout services it. */
    atomic_store(&s_ble_control_pending,1);
}
void hal_ble_set_enabled(int enabled)
{
    int changed=atomic_exchange(&s_ble_enabled,!!enabled)!=!!enabled;
    if(changed && enabled)s_ble_startup.retry_ms=0;
    if(!enabled) s_ble_requested_ms=0;
    ble_control_schedule(); /* Never stop the host/controller on the UI task. */
}
static void ble_handle_add(uint16_t handle)
{
    for(unsigned i=0;i<s_ble_handles_n;i++) if(s_ble_handles[i]==handle) return;
    if(s_ble_handles_n<CONFIG_BT_NIMBLE_MAX_CONNECTIONS) s_ble_handles[s_ble_handles_n++]=handle;
}
static void ble_handle_remove(uint16_t handle)
{
    for(unsigned i=0;i<s_ble_handles_n;i++) if(s_ble_handles[i]==handle) {
        s_ble_handles[i]=s_ble_handles[--s_ble_handles_n];break;
    }
}

/* fold one advertisement report into the table (called on the host task) */
static void ble_record(const struct ble_gap_disc_desc *d)
{
    if (!atomic_load(&s_ble_enabled) || !s_ble_mtx || !s_ble_records) return;

    /* format the advertiser address big-endian "AA:BB:CC:DD:EE:FF" */
    char addr[18];
    const uint8_t *v = d->addr.val;   /* val[] is little-endian */
    snprintf(addr, sizeof addr, "%02X:%02X:%02X:%02X:%02X:%02X",
             v[5], v[4], v[3], v[2], v[1], v[0]);

    /* pull the local name out of the adv payload if present (not NUL-term) */
    char name[24];
    name[0] = 0;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0 &&
        f.name != NULL && f.name_len > 0) {
        int n = f.name_len < (int)sizeof name - 1 ? f.name_len
                                                  : (int)sizeof name - 1;
        memcpy(name, f.name, (size_t)n);
        name[n] = 0;
    }

    xSemaphoreTake(s_ble_mtx, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_ble_count; i++)
        if (strcmp(s_ble_tab[i].addr, addr) == 0) { idx = i; break; }
    if (idx < 0) {
        if (s_ble_count >= BLE_TABLE_MAX) {
            idx=0;
            for(int i=1;i<s_ble_count;i++) if((int32_t)(s_ble_seen[i]-s_ble_seen[idx])<0) idx=i;
        } else idx = s_ble_count++;
        s_ble_tab[idx].name[0] = 0;
        snprintf(s_ble_tab[idx].addr, sizeof s_ble_tab[idx].addr, "%s", addr);
    }
    s_ble_tab[idx].rssi = d->rssi;
    s_ble_addresses[idx]=d->addr;
    s_ble_seen[idx]=millis_raw();
    /* keep a previously-learned name if this particular report has none
     * (names often arrive only in the scan-response packet) */
    if (name[0])
        snprintf(s_ble_tab[idx].name, sizeof s_ble_tab[idx].name, "%s", name);
    xSemaphoreGive(s_ble_mtx);
}

static void ble_start_scan(void);

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        ble_record(&event->disc);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if(!s_central_pending) ble_start_scan();
        if(!atomic_load(&s_ble_enabled) || !s_ble_control_clock_ready) ble_control_schedule();
        return 0;
    default:
        return 0;
    }
}

static void ble_start_scan(void)
{
    if(!atomic_load(&s_ble_enabled) || !s_ble_control_clock_ready || !s_ble_ready || !s_ble_requested_ms || millis_raw()-s_ble_requested_ms>5000 ||
       s_central_pending || ble_gap_disc_active()) return;
    struct ble_gap_disc_params p = { 0 };
    p.passive = 0;             /* Request scan responses for friendly names. */
    p.filter_duplicates = 0;   /* keep folding in reports to refresh RSSI   */
    /* LOW DUTY CYCLE (units of 0.625ms): scan ~10ms out of every 160ms so the
     * radio is free the other ~94% of the time to TRANSMIT our own adverts and
     * share the 2.4GHz band with Wi-Fi. A continuous scan (itvl==window) starves
     * advertising and makes the watch invisible/unconnectable. */
    p.itvl   = 256;            /* 160 ms between scan windows */
    p.window = 16;             /* 10 ms listening per window  */
    int rc=ble_gap_disc(s_ble_addr_type,8000,&p,ble_gap_event,NULL);
    if(!rc) atomic_store(&s_ble_rf_off,0);
    if(rc) wlog("BLE scan failed: %d",rc);
}

static int central_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    if(ev->type==BLE_GAP_EVENT_CONNECT) {
        s_central_result=ev->connect.status;
        if(!ev->connect.status) {
            ble_handle_add(ev->connect.conn_handle);
            s_central_handle=ev->connect.conn_handle;
            if(!atomic_load(&s_ble_enabled) || !s_ble_control_clock_ready)
                ble_gap_terminate(ev->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
        }
        s_central_pending=0;
        ble_start_scan();
    } else if(ev->type==BLE_GAP_EVENT_DISCONNECT) {
        ble_handle_remove(ev->disconnect.conn.conn_handle);
        s_central_handle=BLE_HS_CONN_HANDLE_NONE;
        s_central_result=ev->disconnect.reason;
        ble_start_scan();
    }
    if(!atomic_load(&s_ble_enabled) || !s_ble_control_clock_ready) ble_control_schedule();
    return 0;
}
int hal_ble_connect_device(const char *addr)
{
    if(!addr || !atomic_load(&s_ble_enabled) || !s_ble_control_clock_ready || !s_ble_ready || !s_ble_mtx || !s_ble_records || s_central_pending || s_central_handle!=BLE_HS_CONN_HANDLE_NONE) return 0;
    ble_addr_t peer; int found=0;
    xSemaphoreTake(s_ble_mtx,portMAX_DELAY);
    for(int i=0;i<s_ble_count;i++) if(!strcmp(s_ble_tab[i].addr,addr) && millis_raw()-s_ble_seen[i]<30000) {
        peer=s_ble_addresses[i];found=1;break;
    }
    xSemaphoreGive(s_ble_mtx);
    if(!found) { s_central_result=BLE_HS_ENOENT;return 0; }
    portENTER_CRITICAL(&s_ble_control_lock);
    s_central_peer=peer;s_central_queued=1;s_central_cancel=0;s_central_pending=1;s_central_result=0;
    portEXIT_CRITICAL(&s_ble_control_lock);
    ble_control_schedule();return 1;
}
void hal_ble_disconnect_device(void)
{
    portENTER_CRITICAL(&s_ble_control_lock);s_central_cancel=1;s_central_queued=0;portEXIT_CRITICAL(&s_ble_control_lock);
    ble_control_schedule();
}
void hal_ble_device_status(char *text,int n)
{
    if(!atomic_load(&s_ble_enabled)) snprintf(text,n,"%s%s",
        atomic_load(&s_ble_rf_off)?"Bluetooth off":"Stopping Bluetooth",s_ble_initialized?" / stack retained":"");
    else if(!s_ble_initialized) snprintf(text,n,"Bluetooth deferred / stage %d error %d",
        atomic_load(&s_ble_startup.stage),atomic_load(&s_ble_startup.error));
    else if(!s_ble_ready || !s_ble_control_clock_ready) snprintf(text,n,"Bluetooth starting / error %d",atomic_load(&s_ble_control_error));
    else if(s_central_pending) snprintf(text,n,"Connecting...");
    else if(s_central_handle!=BLE_HS_CONN_HANDLE_NONE) snprintf(text,n,"BLE link connected");
    else if(s_central_result) snprintf(text,n,"Not connected (code %d)",s_central_result);
    else snprintf(text,n,"No accessory connected");
}

/* ---- BLE peripheral: connectable + pairable, with a GATT time-sync service (own device).
 * A central (your laptop/phone) can pair/bond and WRITE a uint32 epoch to set the clock. ---- */
static const ble_uuid128_t WATCH_SVC = BLE_UUID128_INIT(
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15, 0xde, 0xef, 0x12, 0x12, 0x01, 0x00, 0xf0, 0xe3);
static const ble_uuid128_t TIME_CHR = BLE_UUID128_INIT(
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15, 0xde, 0xef, 0x12, 0x12, 0x02, 0x00, 0xf0, 0xe3);
/* media relay: a phone/PC bridge WRITEs now-playing to MEDIA_INFO; the watch NOTIFYs
 * a transport command (0=play/pause,1=next,2=prev,3=vol+,4=vol-) back on MEDIA_CMD. */
static const ble_uuid128_t MEDIA_INFO_CHR = BLE_UUID128_INIT(
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15, 0xde, 0xef, 0x12, 0x12, 0x03, 0x00, 0xf0, 0xe3);
static const ble_uuid128_t MEDIA_CMD_CHR = BLE_UUID128_INIT(
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15, 0xde, 0xef, 0x12, 0x12, 0x04, 0x00, 0xf0, 0xe3);

/* now-playing pushed from the bridge; read by app_music via hal_media_get() */
static char     g_med_title[64];
static char     g_med_artist[64];
static volatile int g_med_playing;
static volatile int g_med_have;
static volatile int g_med_pos;       /* track position (s) at the moment it was written */
static volatile int g_med_dur;       /* track duration (s) */
static volatile uint32_t g_med_pos_ms;   /* hal_millis() when g_med_pos was set (for interpolation) */
static uint16_t g_col_ring, g_col_artbg, g_col_songbg, g_col_arttxt, g_col_songtxt; /* art palette */
static volatile int g_col_have;
static uint16_t g_med_conn = BLE_HS_CONN_HANDLE_NONE;  /* central to notify */
static uint16_t g_med_cmd_h;                            /* MEDIA_CMD value handle */
static volatile int s_batt_dump_req;                   /* set on subscribe -> dump battery log */

/* ---- album cover: received in chunks over BLE (MEDIA_COVER) OR over Wi-Fi (POST /cover),
 * two persistent PSRAM pages, pinned by app_music during pixel reads. ---- */
#define COVER_PX 250
static const ble_uuid128_t MEDIA_COVER_CHR = BLE_UUID128_INIT(
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15, 0xde, 0xef, 0x12, 0x12, 0x05, 0x00, 0xf0, 0xe3);
static uint16_t *s_cover_buffers[2];
static unsigned s_cover_readers[2], g_cover_gen;
static int s_cover_active = -1, s_cover_stage = -1, s_cover_owner;
static size_t s_cover_received;
static int64_t s_cover_last_us;
static uint16_t s_cover_conn=BLE_HS_CONN_HANDLE_NONE;
static portMUX_TYPE s_cover_lock = portMUX_INITIALIZER_UNLOCKED;
static StaticSemaphore_t s_cover_writer_storage;
static SemaphoreHandle_t s_cover_writer;
#define COVER_BYTES ((size_t)COVER_PX * COVER_PX * 2)
/* Readers pin the published image. Writers never mutate it, and incomplete or
 * abandoned uploads cannot change either the pixels or their generation. */
const uint16_t *hal_media_cover_acquire(int *px, unsigned *gen)
{
    portENTER_CRITICAL(&s_cover_lock);
    int at=s_cover_active;
    if (at>=0) s_cover_readers[at]++;
    const uint16_t *p=at>=0?s_cover_buffers[at]:NULL;
    if (px) *px=COVER_PX;
    if (gen) *gen=g_cover_gen;
    portEXIT_CRITICAL(&s_cover_lock);
    return p;
}
void hal_media_cover_release(const uint16_t *pixels)
{
    portENTER_CRITICAL(&s_cover_lock);
    for(int i=0;i<2;i++) if(pixels && pixels==s_cover_buffers[i] && s_cover_readers[i]) s_cover_readers[i]--;
    portEXIT_CRITICAL(&s_cover_lock);
}
const uint16_t *hal_media_cover(int *px, unsigned *gen)
{
    /* Legacy alias must obey the same pin/release ownership as acquire. */
    return hal_media_cover_acquire(px,gen);
}
static int cover_begin(int owner)
{
    int64_t now=esp_timer_get_time();
    portENTER_CRITICAL(&s_cover_lock);
    /* A disconnected BLE sender must not hold the HTTP path indefinitely. */
    if(s_cover_owner==1 && now-s_cover_last_us>15000000) s_cover_owner=0;
    int at=s_cover_active==0?1:0;
    if(s_cover_owner || s_cover_readers[at]) {portEXIT_CRITICAL(&s_cover_lock);return 0;}
    s_cover_owner=owner;s_cover_stage=at;s_cover_received=0;s_cover_last_us=now;
    uint16_t *buffer=s_cover_buffers[at];
    portEXIT_CRITICAL(&s_cover_lock);
    if(!buffer) buffer=nos_mem_alloc(COVER_BYTES,"cover_stage");
    /* Release/acquire readers also inspect this table: publish a newly allocated
     * page under the same lock, without holding the lock during allocation. */
    portENTER_CRITICAL(&s_cover_lock);
    if(buffer) s_cover_buffers[at]=buffer;
    else {s_cover_owner=0;s_cover_stage=-1;}
    portEXIT_CRITICAL(&s_cover_lock);
    return buffer!=NULL;
}
static void cover_end(int owner,int complete)
{
    portENTER_CRITICAL(&s_cover_lock);
    if(s_cover_owner==owner) {
        if(complete && s_cover_received==COVER_BYTES) {s_cover_active=s_cover_stage;g_cover_gen++;}
        s_cover_owner=0;s_cover_stage=-1;
    }
    portEXIT_CRITICAL(&s_cover_lock);
}
/* Chunk write: [offset:4 LE][RGB565 data]. offset 0xFFFFFFFF => commit (image complete). */
static int media_cover_write(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if(s_cover_owner==1 && esp_timer_get_time()-s_cover_last_us>=15000000) cover_end(1,0);
    if(s_cover_owner==1 && s_cover_conn!=conn) return BLE_ATT_ERR_INSUFFICIENT_RES;
    uint8_t buf[540]; uint16_t got = 0;
    if(ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &got)!=0 || got<4) {
        cover_end(1,0);return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    uint32_t off; memcpy(&off, buf, 4);
    if (off == 0xFFFFFFFFu) {
        int complete=got==4 && s_cover_owner==1 && s_cover_received==COVER_BYTES;
        cover_end(1,complete);
        return complete?0:BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    size_t dn=(size_t)got-4;
    if(!dn) {cover_end(1,0);return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;}
    if(off==0) {cover_end(1,0);if(!cover_begin(1)) return BLE_ATT_ERR_INSUFFICIENT_RES;s_cover_conn=conn;}
    if(s_cover_owner!=1 || off!=s_cover_received || dn>COVER_BYTES-s_cover_received) {
        cover_end(1,0);return BLE_ATT_ERR_INVALID_OFFSET;
    }
    memcpy((uint8_t *)s_cover_buffers[s_cover_stage]+off,buf+4,dn);
    s_cover_received+=dn;s_cover_last_us=esp_timer_get_time();
    return 0;
}
static int media_cover_access(uint16_t conn,uint16_t attr,struct ble_gatt_access_ctxt *ctxt,void *arg)
{
    if(!s_cover_writer || xSemaphoreTake(s_cover_writer,0)!=pdTRUE) return BLE_ATT_ERR_INSUFFICIENT_RES;
    int rc=media_cover_write(conn,attr,ctxt,arg);
    xSemaphoreGive(s_cover_writer);return rc;
}

/* connectivity + IP-derived location status, surfaced to the control panel via hal_status/location */
static atomic_int s_wifi_want;
static atomic_int g_wifi_up;           /* got a DHCP IP */
static volatile int g_loc_have;
static char  g_loc_city[24];
static float g_loc_lat, g_loc_lon;

extern int g_tz_offset_min;            /* settings.c: minutes east of UTC */

/* Set the display timezone from an offset in minutes EAST of UTC (e.g. Bengaluru = +330). The
 * clock itself stays UTC (NTP + the bridge both deliver UTC now); localtime_r applies this. */
static void apply_tz(int off_min)
{
    if (off_min < -720 || off_min > 840) return;
    g_tz_offset_min = off_min;
    int mag = off_min < 0 ? -off_min : off_min;
    char tz[24];
    snprintf(tz, sizeof tz, "UTC%c%d:%02d", off_min >= 0 ? '-' : '+', mag / 60, mag % 60);
    setenv("TZ", tz, 1);
    tzset();
}
/* NOS services (Gadgetbridge E.setTimeZone): display timezone, minutes east of UTC. */
void hal_set_tz_min(int tz_min);
void hal_set_tz_min(int tz_min) { apply_tz(tz_min); }

/* TIME char write: [epoch:4 LE UTC][tz_min:2 LE east], optionally + [lat:4 f][lon:4 f]"city".
 * The companion pushes UTC time + the IP-derived timezone and location. */
/* Deadline for an inbound "ring the puck" ping; the main loop polls hal_locate_active(). */
static volatile uint32_t s_locate_until;

int hal_locate_active(void)
{
    uint32_t now = hal_millis();
    return (s_locate_until && now < s_locate_until) ? (int)(s_locate_until - now) : 0;
}

static int time_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[64]; uint16_t got = 0;
        if(ble_hs_mbuf_to_flat(ctxt->om,buf,sizeof buf,&got)!=0)
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        /* Reserved long-frame extension: F2 followed by app|title. Legacy
         * 4/6-byte clock writes remain unchanged. Longer F2 frames must be
         * notifications; do not interpret malformed text as a clock value. */
        if(got>=7 && buf[0]==0xF2) {
            pending_notification p;
            if(!notification_decode(buf+1,(size_t)got-1,&p)) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
            return hal_notification_post(p.app,p.title) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        /* "ring the puck": a 2-byte [0xF1][seconds] write. Distinct from any time payload (those
         * are >= 4 bytes), so it rides this char without adding a new GATT attribute. Only arms a
         * deadline — the main loop makes the noise, so the NimBLE host task never blocks here. */
        if (got == 2 && buf[0] == 0xF1) {
            int secs = buf[1] ? buf[1] : 20;
            if (secs > 120) secs = 120;
            s_locate_until = hal_millis() + (uint32_t)secs * 1000u;
            wlog("find-my-puck: ring %ds", secs);
            return 0;
        }
        if (got >= 4) {                                   /* [epoch:4 UTC] */
            uint32_t epoch; memcpy(&epoch, buf, 4);
            if (epoch < 946684800u || epoch >= 4102444800u) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
            struct timeval tv = { .tv_sec = (time_t)epoch };
            settimeofday(&tv, NULL);
            board_clock_changed(tv.tv_sec);
            g_synced = 1;
            wlog("BLE time sync ok (%lu)", (unsigned long)epoch);
        }
        if (got >= 6) {                                   /* + [tz_min:2] minutes east of UTC */
            int16_t tzm; memcpy(&tzm, buf + 4, 2);
            apply_tz(tzm);
        }
        if (got >= 14) {                                  /* + [lat:4][lon:4]"city" */
            memcpy(&g_loc_lat, buf + 6, 4);
            memcpy(&g_loc_lon, buf + 10, 4);
            int n = got - 14; if (n > (int)sizeof g_loc_city - 1) n = sizeof g_loc_city - 1;
            memcpy(g_loc_city, buf + 14, n); g_loc_city[n] = 0;
            g_loc_have = 1;
            wlog("BLE loc: %s %d,%d", g_loc_city, (int)(g_loc_lat * 100), (int)(g_loc_lon * 100));
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
hal_conn hal_status(int feature)
{
    switch (feature) {
    case HAL_FEAT_BLE:  return !atomic_load(&s_ble_enabled) || !s_ble_ready || !s_ble_control_clock_ready ? HAL_CONN_OFF :
        (g_med_conn != BLE_HS_CONN_HANDLE_NONE || s_central_handle!=BLE_HS_CONN_HANDLE_NONE) ? HAL_CONN_UP :
        s_central_pending ? HAL_CONN_TRYING : HAL_CONN_DOWN;
    case HAL_FEAT_WIFI: return g_wifi_up ? HAL_CONN_UP : (s_wifi_want ? HAL_CONN_TRYING : HAL_CONN_OFF);
    case HAL_FEAT_NET:  return (g_wifi_up || g_loc_have) ? HAL_CONN_UP : HAL_CONN_DOWN;
    case HAL_FEAT_SYNC: return (g_synced || board_clock_valid()) ? HAL_CONN_UP : HAL_CONN_DOWN;
    case HAL_FEAT_AI:   return HAL_CONN_OFF;       /* planned: OpenRouter AI, not wired yet */
    case HAL_FEAT_GPS:  return g_loc_have ? HAL_CONN_UP : HAL_CONN_DOWN;
    }
    return HAL_CONN_OFF;
}
int hal_location(char *city, int n, float *lat, float *lon)
{
    if(hal_weather_location(city,n,lat,lon)) return 1;
    if (!g_loc_have) return 0;
    if (city && n > 0) { int i = 0; for (; g_loc_city[i] && i < n - 1; i++) city[i] = g_loc_city[i]; city[i] = 0; }
    if (lat) *lat = g_loc_lat;
    if (lon) *lon = g_loc_lon;
    return 1;
}
/* [playing:1][pos:4 LE s][dur:4 LE s]"title\0artist" -> shared now-playing state. Fed by
 * BOTH the BLE characteristic and Wi-Fi POST /media (`via` names the transport in the log). */
/* [playing:1][pos:4][dur:4][5 x RGB565 palette:10] then "title\0artist" */
static void media_apply(const uint8_t *buf, int got, const char *via)
{
    if (got < 19) return;
    g_med_playing = buf[0] ? 1 : 0;
    uint32_t pos, dur;
    memcpy(&pos, buf + 1, 4);
    memcpy(&dur, buf + 5, 4);
    g_med_pos = (int)pos;
    g_med_dur = (int)dur;
    g_med_pos_ms = hal_millis();
    uint16_t c[5]; memcpy(c, buf + 9, 10);
    g_col_ring = c[0]; g_col_artbg = c[1]; g_col_songbg = c[2]; g_col_arttxt = c[3]; g_col_songtxt = c[4];
    g_col_have = 1;
    int tl = 0; while (19 + tl < got && buf[19 + tl]) tl++;
    snprintf(g_med_title, sizeof g_med_title, "%.*s", tl, (const char *)buf + 19);
    const char *artist = (19 + tl + 1 <= got) ? (const char *)buf + 19 + tl + 1 : "";
    snprintf(g_med_artist, sizeof g_med_artist, "%s", artist);
    g_med_have = 1;
    printf("MEDIA(%s): \"%s\" / \"%s\" [%s] %d/%ds\n", via, g_med_title, g_med_artist,
           g_med_playing ? "play" : "pause", g_med_pos, g_med_dur);
}
int hal_media_colors(uint16_t *ring, uint16_t *artbg, uint16_t *songbg, uint16_t *arttxt, uint16_t *songtxt)
{
    if (!g_col_have) return 0;
    if (ring) *ring = g_col_ring;
    if (artbg) *artbg = g_col_artbg;
    if (songbg) *songbg = g_col_songbg;
    if (arttxt) *arttxt = g_col_arttxt;
    if (songtxt) *songtxt = g_col_songtxt;
    return 1;
}
static int media_info_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[160]; uint16_t got = 0;
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf - 1, &got);
        buf[got < sizeof buf ? got : (uint16_t)(sizeof buf - 1)] = 0;
        media_apply(buf, got, "ble");
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
static int media_cmd_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;   /* value delivered via notify; read returns 0 */
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t z = 0; os_mbuf_append(ctxt->om, &z, 1); return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
/* UNENCRYPTED / no-bond: music metadata + art is low-sensitivity, and requiring a bond made
 * Windows persist + auto-reconnect the device, which fought the bridge. Without a bond the
 * host connects freshly each time and there's nothing to interfere with. */
static const struct ble_gatt_chr_def WATCH_CHRS[] = {
    { .uuid = &TIME_CHR.u, .access_cb = time_access,
      .flags = BLE_GATT_CHR_F_WRITE },
    { .uuid = &MEDIA_INFO_CHR.u, .access_cb = media_info_access,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = &MEDIA_COVER_CHR.u, .access_cb = media_cover_access,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = &MEDIA_CMD_CHR.u, .access_cb = media_cmd_access,
      .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
      .val_handle = &g_med_cmd_h },
    { 0 },
};

int hal_media_get(char *title, int tn, char *artist, int an, int *playing)
{
    if (!g_med_have) return 0;
    if (title && tn) snprintf(title, tn, "%s", g_med_title);
    if (artist && an) snprintf(artist, an, "%s", g_med_artist);
    if (playing) *playing = g_med_playing;
    return 1;
}
/* Track time in MILLISECONDS. Position is INTERPOLATED locally between BLE updates so the
 * ring advances every frame (smooth 60fps), not once a second. Returns 1 if known. */
int hal_media_time(int *pos_ms, int *dur_ms)
{
    if (!g_med_have || g_med_dur <= 0) return 0;
    int pms = g_med_pos * 1000;
    if (g_med_playing) pms += (int)(hal_millis() - g_med_pos_ms);
    int dms = g_med_dur * 1000;
    if (pms > dms) pms = dms;
    if (pms < 0) pms = 0;
    if (pos_ms) *pos_ms = pms;
    if (dur_ms) *dur_ms = dms;
    return 1;
}
static volatile int g_cmd_seq;         /* increments on each transport tap */
static volatile int g_cmd_val = -1;    /* last command; polled over Wi-Fi via GET /cmd */
static volatile int g_cmd_pos = -1;    /* seek target (s) for cmd 4 */
void hal_media_cmd(int cmd)
{
    g_cmd_val = cmd; g_cmd_pos = -1; g_cmd_seq++;
    if (g_med_conn != BLE_HS_CONN_HANDLE_NONE && g_med_cmd_h) {   /* BLE notify */
        uint8_t b = (uint8_t)cmd;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&b, 1);
        if (om) ble_gatts_notify_custom(g_med_conn, g_med_cmd_h, om);
    }
    printf("MEDIA: cmd %d -> host (seq %d)\n", cmd, g_cmd_seq);
}
void hal_media_seek(int pos_sec)
{
    g_cmd_val = 4; g_cmd_pos = pos_sec; g_cmd_seq++;
    g_med_pos = pos_sec; g_med_pos_ms = hal_millis();     /* reflect locally right away */
    if (g_med_conn != BLE_HS_CONN_HANDLE_NONE && g_med_cmd_h) {
        uint8_t b[5]; b[0] = 4; memcpy(b + 1, &pos_sec, 4);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(b, 5);
        if (om) ble_gatts_notify_custom(g_med_conn, g_med_cmd_h, om);
    }
    printf("MEDIA: seek %d -> host (seq %d)\n", pos_sec, g_cmd_seq);
}
void hal_media_vol(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_cmd_val = 5; g_cmd_pos = pct; g_cmd_seq++;
    if (g_med_conn != BLE_HS_CONN_HANDLE_NONE && g_med_cmd_h) {
        uint8_t b[5]; b[0] = 5; int v = pct; memcpy(b + 1, &v, 4);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(b, 5);
        if (om) ble_gatts_notify_custom(g_med_conn, g_med_cmd_h, om);
    }
    printf("MEDIA: vol %d%% -> host (seq %d)\n", pct, g_cmd_seq);
}
/* Force-drop every active BLE connection so the watch re-advertises cleanly — clears the stale
 * Windows/zombie links that otherwise block the bridge from connecting. */
void ble_drop_all(void)
{
    int n = 0;
    for (uint16_t h = 0; h < 8; h++)
        if (ble_gap_terminate(h, 0x13) == 0) n++;   /* 0x13 = remote-user-terminated */
    wlog("BLE dropped %d conn(s)", n);
}
void hal_time_resync(void)
{
    hal_weather_refresh();
    g_cmd_val = 6; g_cmd_pos = -1; g_cmd_seq++;
    if (g_med_conn != BLE_HS_CONN_HANDLE_NONE && g_med_cmd_h) {   /* ask the companion to re-push */
        uint8_t b = 6;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&b, 1);
        if (om) ble_gatts_notify_custom(g_med_conn, g_med_cmd_h, om);
    }
    if (g_wifi_up && esp_sntp_enabled()) esp_sntp_restart();      /* and re-kick NTP if online */
    wlog("resync req: ble=%s wifi=%s", g_med_conn != BLE_HS_CONN_HANDLE_NONE ? "up" : "DOWN",
         g_wifi_up ? "up" : "down");
}
/* ---- Nordic UART Service: the Gadgetbridge "Bangle.js" link --------------------------------------
 * 6e400001-b5a3-f393-e0a9-e50e24dcca9e; RX ...0002 (phone writes), TX ...0003 (watch notifies).
 * The NimBLE host task only splits received bytes into lines and queues them in a PSRAM ring;
 * the UI task pops them with hal_phone_read_line() (zero-timeout lock: never blocks a frame) and
 * nos/svc.c parses them. Appended AFTER the watch service in the GATT table so the companion's
 * cached handles stay where they were. */
static const ble_uuid128_t NUS_SVC = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_RX_CHR = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t NUS_TX_CHR = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);
#define NUS_LINE_MAX 2048          /* = GB_LINE_MAX (nos/gb.h); longer lines are dropped */
#define NUS_RING     8192          /* queued complete lines, [len:2][bytes] records */
static uint16_t g_nus_tx_h;
static volatile int g_nus_sub;     /* the central enabled TX notifications */
static volatile uint16_t g_nus_mtu = 23;
static char    *s_nus_asm;         /* line being assembled (host task only) */
static int      s_nus_asm_n, s_nus_asm_drop;
static uint8_t *s_nus_ring;        /* guarded by s_nus_mtx */
static int      s_nus_head, s_nus_used;
static SemaphoreHandle_t s_nus_mtx;
static volatile uint32_t s_nus_lines, s_nus_dropped;

static void nus_alloc(void)
{
    if (!s_nus_mtx) s_nus_mtx = xSemaphoreCreateMutex();
    if (!s_nus_asm) s_nus_asm = nos_mem_alloc(NUS_LINE_MAX, "ble_nus");
    if (!s_nus_ring) s_nus_ring = nos_mem_alloc(NUS_RING, "ble_nus");
    if (!s_nus_asm || !s_nus_ring || !s_nus_mtx) ESP_LOGE("watch", "NUS: no PSRAM for line buffers");
}
static void nus_push_line(const char *s, int n)
{
    if (!s_nus_ring || !s_nus_mtx || n <= 0) return;
    xSemaphoreTake(s_nus_mtx, portMAX_DELAY);          /* the UI side holds it for a memcpy only */
    if (s_nus_used + n + 2 <= NUS_RING) {
        int w = (s_nus_head + s_nus_used) % NUS_RING;
        uint8_t hdr[2] = { (uint8_t)(n & 0xFF), (uint8_t)(n >> 8) };
        for (int i = 0; i < 2; i++) { s_nus_ring[w] = hdr[i]; w = (w + 1) % NUS_RING; }
        int first = NUS_RING - w < n ? NUS_RING - w : n;
        memcpy(s_nus_ring + w, s, (size_t)first);
        if (first < n) memcpy(s_nus_ring, s + first, (size_t)(n - first));
        s_nus_used += n + 2;
        s_nus_lines++;
    } else s_nus_dropped++;
    xSemaphoreGive(s_nus_mtx);
}
static void nus_rx_bytes(const uint8_t *b, int n)
{
    if (!s_nus_asm) return;
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c == '\n') {
            if (!s_nus_asm_drop && s_nus_asm_n) nus_push_line(s_nus_asm, s_nus_asm_n);
            else if (s_nus_asm_drop) s_nus_dropped++;
            s_nus_asm_n = 0; s_nus_asm_drop = 0;
        } else if (c == '\r') {
            continue;
        } else if (s_nus_asm_n < NUS_LINE_MAX - 1) {
            s_nus_asm[s_nus_asm_n++] = (char)c;
        } else s_nus_asm_drop = 1;
    }
}
static int nus_rx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[128];
    int len = OS_MBUF_PKTLEN(ctxt->om);
    for (int off = 0; off < len; off += (int)sizeof buf) {
        int k = len - off < (int)sizeof buf ? len - off : (int)sizeof buf;
        if (os_mbuf_copydata(ctxt->om, off, k, buf) != 0) break;
        nus_rx_bytes(buf, k);
    }
    return 0;
}
static int nus_tx_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)ctxt; (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;              /* notify only */
}
static const struct ble_gatt_chr_def NUS_CHRS[] = {
    { .uuid = &NUS_RX_CHR.u, .access_cb = nus_rx_access,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { .uuid = &NUS_TX_CHR.u, .access_cb = nus_tx_access,
      .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_nus_tx_h },
    { 0 },
};

int hal_phone_connected(void)
{
    return atomic_load(&s_ble_enabled) && s_ble_control_clock_ready && g_nus_sub && g_med_conn != BLE_HS_CONN_HANDLE_NONE;
}
int hal_phone_read_line(char *buf, int n)
{
    if (!buf || n <= 0 || !s_nus_ring || !s_nus_mtx) return 0;
    if (xSemaphoreTake(s_nus_mtx, 0) != pdTRUE) return 0;   /* busy: try next frame */
    int k = 0;
    if (s_nus_used >= 2) {
        int len = s_nus_ring[s_nus_head] | (s_nus_ring[(s_nus_head + 1) % NUS_RING] << 8);
        int r = (s_nus_head + 2) % NUS_RING;
        k = len < n - 1 ? len : n - 1;
        int first = NUS_RING - r < k ? NUS_RING - r : k;
        memcpy(buf, s_nus_ring + r, (size_t)first);
        if (first < k) memcpy(buf + first, s_nus_ring, (size_t)(k - first));
        s_nus_head = (s_nus_head + 2 + len) % NUS_RING;
        s_nus_used -= len + 2;
    }
    xSemaphoreGive(s_nus_mtx);
    buf[k] = 0;
    return k;
}
/* Called from the UI task only (svc.c), so lines never interleave. Framing for Gadgetbridge:
 * every line ends in HAL_PHONE_EOL ("\r\n", see svc.h: the phone drops the byte before '\n').
 * Gadgetbridge glues bytes into receivedLine until a '\n', so a line cut short (mbufs ran out
 * mid-line) or a fragment left from a previous link would be glued onto the NEXT line and both
 * fail to parse. So: wait briefly for mbufs instead of dropping a chunk, and after a cut (and on
 * every new subscription) lead with an EOL - the phone then sees the fragment as its own line,
 * and an empty one when there is no fragment, which it ignores. */
static volatile int s_nus_tx_resync;
void hal_phone_write_line(const char *line)
{
    if (!line || !hal_phone_connected() || !g_nus_tx_h) return;
    static const char eol[] = HAL_PHONE_EOL;
    const int nl = (int)sizeof eol - 1;
    uint16_t conn = g_med_conn;
    int pre = s_nus_tx_resync ? nl : 0;
    int len = (int)strlen(line), total = pre + len + nl;
    int chunk = g_nus_mtu > 23 ? g_nus_mtu - 3 : 20;
    if (chunk > 180) chunk = 180;
    uint8_t buf[180];
    for (int off = 0; off < total; off += chunk) {
        int k = total - off < chunk ? total - off : chunk;
        for (int i = 0; i < k; i++) {
            int j = off + i - pre;                       /* index into line */
            buf[i] = (uint8_t)(j < 0 ? eol[j + pre] : j < len ? line[j] : eol[j - len]);
        }
        for (int tries = 0;; tries++) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)k);
            int rc = om ? ble_gatts_notify_custom(conn, g_nus_tx_h, om) : BLE_HS_ENOMEM;  /* consumes om */
            if (rc == 0) break;
            if (rc != BLE_HS_ENOMEM || tries >= 20 || !hal_phone_connected()) {
                s_nus_dropped++;
                if (off > 0) s_nus_tx_resync = 1;        /* a fragment is on the phone */
                return;
            }
            vTaskDelay(1);                               /* host is draining the mbuf pool */
        }
    }
    s_nus_tx_resync = 0;
}

/* ---- battery history logger (dumped over BLE for offline analysis) ---------------------------
 * A ~24h ring of battery mV (one sample/min). On the companion's subscribe we push it, delta-
 * compressed, on the SAME MEDIA_CMD notify char (prefix 0xB1/0xB2/0xB3 — distinct from the 0-6
 * transport commands) so no new GATT characteristic is needed (avoids the Windows GATT-cache trap). */
#define BATT_N          1440
#define BATT_INTERVAL_S 60
static uint16_t s_batt_mv[BATT_N];
static int      s_batt_head, s_batt_cnt;
static uint32_t s_batt_last;

static void cmd_notify(const void *buf, int len)
{
    if (g_med_conn == BLE_HS_CONN_HANDLE_NONE || !g_med_cmd_h) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
    if (om) ble_gatts_notify_custom(g_med_conn, g_med_cmd_h, om);
}
/* Find my phone: code 7 on the MEDIA_CMD notify char -> the companion rings ITSELF at full volume. */
void hal_find_phone(void)
{
    uint8_t c = 7;
    cmd_notify(&c, 1);
    wlog("find-my-phone sent (ble=%s)",
         g_med_conn != BLE_HS_CONN_HANDLE_NONE ? "up" : "DOWN");
}

static void batt_log_dump(void)
{
    if (g_med_conn == BLE_HS_CONN_HANDLE_NONE || !g_med_cmd_h || s_batt_cnt == 0) return;
    int cnt = s_batt_cnt, oldest = (s_batt_head - cnt + BATT_N * 2) % BATT_N;
    uint16_t base = s_batt_mv[oldest];
    uint8_t h[11]; h[0] = 0xB1;
    uint32_t ep = (uint32_t)time(NULL);      memcpy(h + 1, &ep, 4);
    uint16_t iv = BATT_INTERVAL_S;           memcpy(h + 5, &iv, 2);
    uint16_t c  = (uint16_t)cnt;             memcpy(h + 7, &c, 2);
    memcpy(h + 9, &base, 2);
    cmd_notify(h, 11);
    uint8_t chunk[201]; chunk[0] = 0xB2; int ci = 1; int prev = base;
    for (int i = 1; i < cnt; i++) {
        int v = s_batt_mv[(oldest + i) % BATT_N];
        int d = v - prev; if (d > 127) d = 127; if (d < -128) d = -128;
        chunk[ci++] = (uint8_t)(int8_t)d; prev += d;
        if (ci >= (int)sizeof chunk) { cmd_notify(chunk, ci); ci = 1; }
    }
    if (ci > 1) cmd_notify(chunk, ci);
    uint8_t e = 0xB3; cmd_notify(&e, 1);
    wlog("battlog dumped %d samples", cnt);
}
static uint32_t s_batt_dump_ms;
void hal_batt_log_tick(void)
{
    uint32_t now = hal_millis();
    if (s_batt_last == 0 || now - s_batt_last >= (uint32_t)BATT_INTERVAL_S * 1000u) {
        s_batt_last = now;
        s_batt_mv[s_batt_head] = (uint16_t)hal_battery_mv();
        s_batt_head = (s_batt_head + 1) % BATT_N;
        if (s_batt_cnt < BATT_N) s_batt_cnt++;
    }
    /* push the log on request (subscribe/manual) or every 2 min while a companion is connected */
    /* (not while Gadgetbridge holds the link: it would get unknown-handle notifies) */
    int connected = (g_med_conn != BLE_HS_CONN_HANDLE_NONE && g_med_cmd_h && !g_nus_sub);
    if (s_batt_dump_req || (connected && (s_batt_dump_ms == 0 || now - s_batt_dump_ms >= 120000u))) {
        s_batt_dump_req = 0; s_batt_dump_ms = now;
        batt_log_dump();
    }
}
void hal_batt_dump_now(void) { s_batt_dump_req = 1; }

extern const ble_uuid128_t BLE_XFER_SVC;                /* ble_xfer.c: SD file transfer */
extern const struct ble_gatt_chr_def BLE_XFER_CHRS[];
void ble_xfer_disconnected(void);
static const struct ble_gatt_svc_def WATCH_GATT[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &WATCH_SVC.u, .characteristics = WATCH_CHRS },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &NUS_SVC.u, .characteristics = NUS_CHRS },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &BLE_XFER_SVC.u, .characteristics = BLE_XFER_CHRS },
    { 0 },
};

/* last-pairing outcome, queryable any time via the `bleinfo` serial command */
static volatile int g_ble_conns, g_ble_encst = -2, g_ble_bonded, g_ble_disc = -1;
void hal_ble_status(char *buf, int n)
{
    snprintf(buf, n, "%s%s; stack=%d conns=%u error=%d init_stage=%d attempts=%u failures=%u deferred=%u enc=%d bonded=%d disc=0x%x",
             !s_ble_ready || !s_ble_control_clock_ready?"control unavailable":atomic_load(&s_ble_enabled)?"enabled":
             atomic_load(&s_ble_rf_off)?"radio off":"stopping radio",
             s_ble_initialized?" / stack retained":"",atomic_load(&s_ble_initialized),atomic_load(&s_ble_handles_n), s_ble_control_error,
             atomic_load(&s_ble_startup.stage),atomic_load(&s_ble_startup.attempts),atomic_load(&s_ble_startup.failures),
             atomic_load(&s_ble_startup.deferred),g_ble_encst, g_ble_bonded, g_ble_disc & 0xff);
}

static int peri_gap_event(struct ble_gap_event *ev, void *arg);
int hal_display_mode(void); /* nos_main.c: 2 = ON, 1 = AOD/sensor, 0 = off */
#define BLE_ADV_SLOW_MIN 800 /* 0.625 ms units: 500 ms */
#define BLE_ADV_SLOW_MAX 1000
static int s_adv_fast=-1;    /* advertising interval class of the running advertisement */
static void adv_start(void)
{
    if(!s_ble_ready || !s_ble_control_clock_ready || !atomic_load(&s_ble_enabled) || ble_gap_adv_active()) return;
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)BLE_NAME; f.name_len = sizeof BLE_NAME - 1; f.name_is_complete = 1;
    int rc=ble_gap_adv_set_fields(&f);
    if(rc) {s_ble_control_error=rc;return;}
    /* scan response: the NUS UUID, which Gadgetbridge's Bangle.js scan filter looks for */
    struct ble_hs_adv_fields r = { 0 };
    r.uuids128 = &NUS_SVC; r.num_uuids128 = 1; r.uuids128_is_complete = 1;
    rc=ble_gap_adv_rsp_set_fields(&r);
    if(rc) {s_ble_control_error=rc;return;}
    struct ble_gap_adv_params ap = { 0 };
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* NimBLE's default (30..60 ms) is ~20 radio wakes/s while no phone is connected. Keep it with the
     * display ON (pairing/scan screens); otherwise 500..625 ms. ble_control_tick restarts on change. */
    s_adv_fast=hal_display_mode()==2;
    if(!s_adv_fast) {ap.itvl_min=BLE_ADV_SLOW_MIN;ap.itvl_max=BLE_ADV_SLOW_MAX;}
    rc=ble_gap_adv_start(s_ble_addr_type, NULL, BLE_HS_FOREVER, &ap, peri_gap_event, NULL);
    if(!rc) atomic_store(&s_ble_rf_off,0);else s_ble_control_error=rc;
}
/* All GAP starts/stops are serialized on NimBLE's existing host event queue.
 * Logical Off retains stack allocations, but owns no advertising, discovery,
 * initiating or connection RF activity. Late callbacks can only terminate links. */
static void ble_control(struct ble_npl_event *event)
{
    (void)event;
    if(!s_ble_ready) return;
    portENTER_CRITICAL(&s_ble_control_lock);
    int cancel=s_central_cancel,queued=s_central_queued;
    ble_addr_t peer=s_central_peer;s_central_cancel=0;s_central_queued=0;
    portEXIT_CRITICAL(&s_ble_control_lock);
    if(!atomic_load(&s_ble_enabled) || !s_ble_control_clock_ready) {
        int rc=0;
        if(ble_gap_disc_active() && (rc=ble_gap_disc_cancel())!=0) s_ble_control_error=rc;
        if(ble_gap_adv_active() && (rc=ble_gap_adv_stop())!=0) s_ble_control_error=rc;
        if(ble_gap_conn_active() && (rc=ble_gap_conn_cancel())!=0) s_ble_control_error=rc;
        s_central_pending=0;
        for(unsigned i=0;i<s_ble_handles_n;i++) {
            rc=ble_gap_terminate(s_ble_handles[i],BLE_ERR_REM_USER_CONN_TERM);
            if(rc && rc!=BLE_HS_ENOTCONN && rc!=BLE_HS_EALREADY) s_ble_control_error=rc;
        }
        atomic_store(&s_ble_rf_off,!ble_gap_disc_active() && !ble_gap_adv_active() &&
                     !ble_gap_conn_active() && !s_ble_handles_n);
        return;
    }
    if(cancel) {
        ble_gap_conn_cancel();s_central_pending=0;
        if(s_central_handle!=BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(s_central_handle,BLE_ERR_REM_USER_CONN_TERM);
    } else if(queued) {
        ble_gap_disc_cancel();
        int rc=ble_gap_connect(s_ble_addr_type,&peer,10000,NULL,central_event,NULL);
        if(rc) {s_central_pending=0;s_central_result=rc;}
        else atomic_store(&s_ble_rf_off,0);
    }
    adv_start();ble_start_scan();
}
static void ble_control_tick(struct ble_npl_event *event)
{
    /* Display ON polls control intent every 100 ms. Otherwise 1 s on the shared light-sleep
     * grid; the latency applies to the first UI toggle after a wake (docs/LIGHT_SLEEP.md). */
    uint32_t period=hal_display_mode()==2?100u:sleep_grid_delay_ms(sleep_grid_now_ms(),1000u);
    int rc=ble_npl_callout_reset(&s_ble_control_callout,ble_npl_time_ms_to_ticks32(period));
    s_ble_control_clock_ready=rc==0;
    if(rc) s_ble_control_error=rc;
    atomic_store(&s_ble_control_pending,0);
    if(s_adv_fast>=0 && s_adv_fast!=(hal_display_mode()==2) && ble_gap_adv_active() && !s_ble_handles_n)
        ble_gap_adv_stop(); /* ble_control below restarts it at the interval for the new display state */
    ble_control(event); /* A failed control timer quiesces RF and is observable. */
}
static int peri_gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    if((!atomic_load(&s_ble_enabled) || !s_ble_control_clock_ready) && ev->type!=BLE_GAP_EVENT_CONNECT &&
       ev->type!=BLE_GAP_EVENT_DISCONNECT && ev->type!=BLE_GAP_EVENT_ADV_COMPLETE) return 0;
    struct ble_gap_conn_desc d;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        wlog("BLE connect (st=%d)", ev->connect.status);
        if (ev->connect.status != 0) { adv_start(); break; }
        ble_handle_add(ev->connect.conn_handle);
        g_ble_conns++;
        if(!atomic_load(&s_ble_enabled) || !s_ble_control_clock_ready) {
            ble_gap_terminate(ev->connect.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
            ble_control_schedule();break;
        }
        g_med_conn = ev->connect.conn_handle;   /* target for media-cmd notifies */
        if (ble_gap_conn_find(ev->connect.conn_handle, &d) == 0)
            printf("BLE:   bonded=%d enc=%d auth=%d  itvl=%d(%dms) latency=%d\n",
                   d.sec_state.bonded, d.sec_state.encrypted, d.sec_state.authenticated,
                   d.conn_itvl, (d.conn_itvl * 5) / 4, d.conn_latency);
        /* Ask for a GENTLE connection: a longer interval + slave latency means the watch grabs
         * far fewer 2.4GHz slots, leaving airtime for A2DP audio on the user's other BT headset.
         * ~45-70ms is still snappy for transport control (art is a background trickle anyway). */
        {
            struct ble_gap_upd_params up = {
                .itvl_min = 64,   /* 80 ms  (units of 1.25ms) */
                .itvl_max = 96,   /* 120 ms */
                .latency  = 3,    /* may skip up to 3 events when idle */
                .supervision_timeout = 800,   /* 8 s */
                .min_ce_len = 0, .max_ce_len = 0,
            };
            int rc = ble_gap_update_params(ev->connect.conn_handle, &up);
            printf("BLE:   requested gentle conn params (rc=%d)\n", rc);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ble_handle_remove(ev->disconnect.conn.conn_handle);
        wlog("BLE disconnect (0x%x)", ev->disconnect.reason);
        if(g_ble_conns>0) g_ble_conns--;
        g_ble_disc = ev->disconnect.reason;
        g_med_conn = BLE_HS_CONN_HANDLE_NONE;
        g_med_have = 0;                          /* now-playing is stale once the bridge drops */
        g_nus_sub = 0; g_nus_mtu = 23;           /* Gadgetbridge link gone */
        ble_xfer_disconnected();
        s_nus_asm_n = 0; s_nus_asm_drop = 0;
        adv_start();
        if(!atomic_load(&s_ble_enabled) || !s_ble_control_clock_ready) ble_control_schedule();
        break;
    case BLE_GAP_EVENT_ENC_CHANGE:
        printf("BLE: ENC_CHANGE status=%d\n", ev->enc_change.status);
        g_ble_encst = ev->enc_change.status;
        if (ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0) {
            g_ble_bonded = d.sec_state.bonded;
            printf("BLE:   -> bonded=%d enc=%d\n", d.sec_state.bonded, d.sec_state.encrypted);
        }
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:            /* stale bond -> drop it and re-pair */
        printf("BLE: REPEAT_PAIRING -> unpair + retry\n");
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0)
            ble_gap_unpair(&d.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
        printf("BLE: PASSKEY_ACTION action=%d\n", ev->passkey.params.action);
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        printf("BLE: CONN_UPDATE status=%d\n", ev->conn_update.status);
        if (ble_gap_conn_find(ev->conn_update.conn_handle, &d) == 0)
            printf("BLE:   now itvl=%d(%dms) latency=%d\n",
                   d.conn_itvl, (d.conn_itvl * 5) / 4, d.conn_latency);
        break;
    case BLE_GAP_EVENT_MTU:
        printf("BLE: MTU=%d\n", ev->mtu.value);
        g_nus_mtu = ev->mtu.value;
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        printf("BLE: SUBSCRIBE handle=%d\n", ev->subscribe.attr_handle);
        if (ev->subscribe.attr_handle == g_med_cmd_h && ev->subscribe.cur_notify)
            s_batt_dump_req = 1;     /* companion is listening -> push the battery history */
        if (ev->subscribe.attr_handle == g_nus_tx_h) {
            g_nus_sub = ev->subscribe.cur_notify;
            s_nus_tx_resync = 1;     /* flush any stale fragment in the phone's line buffer */
            wlog("NUS %s (Gadgetbridge)", g_nus_sub ? "subscribed" : "unsubscribed");
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        if(!atomic_load(&s_ble_enabled) || !s_ble_control_clock_ready) ble_control_schedule();
        break;
    default:
        printf("BLE: gap event type=%d\n", ev->type);
        break;
    }
    return 0;
}
static void ble_on_reset(int reason)
{
    s_ble_ready=0;s_ble_handles_n=0;s_central_pending=0;
    s_central_handle=BLE_HS_CONN_HANDLE_NONE;g_med_conn=BLE_HS_CONN_HANDLE_NONE;
    g_ble_conns=0;g_nus_sub=0;g_med_have=0;s_ble_control_error=reason;
    atomic_store(&s_ble_rf_off,1);
}

/* host synced with controller: pick an address, then observe AND advertise (dual role) */
static void ble_on_sync(void)
{
    /* Use a FIXED static-random address (MSB top 2 bits = 1) as this build's BLE identity.
     * A stable fresh identity sidesteps stale GATT caches + auto-connect on hosts that
     * paired an earlier firmware whose service table was different. */
    static const uint8_t rnd_addr[6] = { 0x37, 0x59, 0x71, 0x93, 0xB5, 0xD7 };
    if (ble_hs_id_set_rnd(rnd_addr) == 0) {
        s_ble_addr_type = BLE_OWN_ADDR_RANDOM;
    } else {
        ble_hs_util_ensure_addr(0);
        if (ble_hs_id_infer_auto(0, &s_ble_addr_type) != 0)
            s_ble_addr_type = BLE_OWN_ADDR_PUBLIC;
    }
    uint8_t a[6] = { 0 };
    ble_hs_id_copy_addr(s_ble_addr_type, a, NULL);
    printf("BLE: addr %02X:%02X:%02X:%02X:%02X:%02X type=%d name=\"%s\"\n",
           a[5], a[4], a[3], a[2], a[1], a[0], s_ble_addr_type, BLE_NAME);
    /* Discovery is leased by the visible device list, not left running at boot. */
    s_ble_ready=1;
    ble_control_tick(NULL);
}

static void ble_host_task(void *param)
{
    (void)param;
    while(!atomic_load(&s_ble_initialized))ulTaskNotifyTake(pdTRUE,portMAX_DELAY);
    nimble_port_run();                 /* runs until nimble_port_stop() */
    s_ble_ready=0;s_ble_control_clock_ready=0;
    s_ble_control_error=BLE_HS_ENOTCONN;
    s_ble_host_task=NULL;vTaskDelete(NULL);
}
static int ble_cleanup(void)
{
    esp_err_t clean=nimble_port_deinit();
    if(clean!=ESP_OK)return radio_fail(&s_ble_startup,8,clean);
    s_ble_cleanup_pending=s_ble_stack_owned=0;
    return 1;
}

static int ble_init(void)
{
    if(s_ble_initialized)return 1;
    if(s_ble_cleanup_pending)return 0; /* Cleanup has its own memory-independent retry. */
    if(!s_ble_enabled)return 0;
    if(!s_ble_records)s_ble_records=heap_caps_calloc(1,sizeof *s_ble_records,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s_ble_mtx)s_ble_mtx=xSemaphoreCreateMutex();
    nus_alloc();
    if(!s_ble_records || !s_ble_mtx || !s_nus_mtx || !s_nus_asm || !s_nus_ring)
        return radio_fail(&s_ble_startup,2,ESP_ERR_NO_MEM);
    /* The SDK freertos wrapper ignores task allocation failure. Reserve our
     * checked, parked host first; only notify it after the stack is complete. */
    if(!s_ble_host_task && xTaskCreatePinnedToCore(ble_host_task,"nimble_host",
        CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE,NULL,configMAX_PRIORITIES-4,&s_ble_host_task,
        CONFIG_BT_NIMBLE_PINNED_TO_CORE)!=pdPASS)
        return radio_fail(&s_ble_startup,3,ESP_ERR_NO_MEM);
    esp_err_t e = nimble_port_init();   /* brings up the BT controller + host */
    if (e != ESP_OK) {
        return radio_fail(&s_ble_startup,4,e);
    }
    s_ble_stack_owned=1;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc=ble_gatts_count_cfg(WATCH_GATT);
    if(!rc)rc=ble_gatts_add_svcs(WATCH_GATT);
    if(!rc)rc=ble_svc_gap_device_name_set(BLE_NAME);
    if(rc) {
        s_ble_cleanup_pending=1;
        return radio_fail(&s_ble_startup,5,rc);
    }
    printf("BLE: GATT registered (media_cmd handle=%d)\n", g_med_cmd_h);
    ble_npl_event_init(&s_ble_control_event,ble_control_tick,NULL);
    rc=ble_npl_callout_init(&s_ble_control_callout,nimble_port_get_dflt_eventq(),ble_control_tick,NULL);
    if(rc) {s_ble_cleanup_pending=1;return radio_fail(&s_ble_startup,6,rc);}
    extern void ble_store_config_init(void);
    ble_store_config_init();
    /* NO bonding: the media chars are unencrypted, so the host never persists this device
     * and can't auto-reconnect in the background to fight the bridge. Just a clean, fresh
     * connectable peripheral each time. */
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    s_ble_startup.stage=0;s_ble_startup.error=0;s_ble_startup.retry_ms=0;
    atomic_store(&s_ble_initialized,1);xTaskNotifyGive(s_ble_host_task);
    return 1;
}

int hal_ble_scan(hal_ble_dev *out, int max)
{
    if (!atomic_load(&s_ble_enabled) || !out || !s_ble_mtx || !s_ble_records || max <= 0) return 0;
    s_ble_requested_ms=millis_raw();
    ble_control_schedule();
    xSemaphoreTake(s_ble_mtx, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < s_ble_count && n<max; i++)
        if(millis_raw()-s_ble_seen[i]<30000) out[n++] = s_ble_tab[i];
    xSemaphoreGive(s_ble_mtx);
    return n;
}

/* Estimated walking-like cadence; not a validated fitness measurement. */
int hal_step_count(void)
{
    hal_activity a; hal_activity_get(&a);
    return a.steps>(uint32_t)INT_MAX ? INT_MAX : (int)a.steps;
}

/* hal_haptic(int pattern) lives in firmware/main/ext/ext_haptic.c (nos/hal_ext.h). */

/* Large buffers go to external PSRAM (the S3R8 has 8MB) so the layer caches
 * don't exhaust the ~320KB of internal SRAM. */
const char *hal_sd_root(void) { return "/sdcard"; }

#undef hal_big_alloc
void *hal_big_alloc(uint32_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
}

/* Observe-only AP scan (no frame injection). ASYNC + cached: the old version called
 * esp_wifi_scan_start(..., true), which (a) blocked the render loop for seconds and (b) returned
 * 0 SILENTLY whenever a connect/reconnect was in flight or a scan was already running — which is
 * the usual reason the list looked empty ("not picking up any signals"). Now the sweep runs in the
 * background, errors get logged, and hal_wifi_scan() serves the last completed sweep instantly.
 * NOTE: the ESP32-S3 radio is 2.4GHz ONLY — 5GHz APs are invisible to it, no fix possible. */
#define WSCAN_MAX 32
static struct {
    hal_wifi_ap aps[WSCAN_MAX],next[WSCAN_MAX];
    wifi_ap_record_t records[WSCAN_MAX];
} *s_scan_cache;
#define s_aps (s_scan_cache->aps)
static int          s_ap_n;
static atomic_int s_scan_busy;
static uint32_t     s_scan_done_ms, s_scan_start_ms, s_scan_try_ms;
static atomic_uint s_scan_pause_until;
static portMUX_TYPE s_scan_lock=portMUX_INITIALIZER_UNLOCKED;
static atomic_int s_air_reserved, s_wifi_worker_ready;
static atomic_int s_wifi_enabled,s_wifi_initialized,s_wifi_on_demand,s_wifi_start_pending;
static int s_wifi_driver_owned,s_wifi_default_handlers;
static int s_wifi_event_registered,s_wifi_ip_registered;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_event,s_wifi_ip;
static TaskHandle_t s_wifi_task;
static int wifi_init(void);
static esp_netif_t *wifi_netif_new(int ap)
{
    esp_netif_config_t sta=ESP_NETIF_DEFAULT_WIFI_STA(),access=ESP_NETIF_DEFAULT_WIFI_AP();
    esp_netif_t *netif=esp_netif_new(ap?&access:&sta);
    if(!netif)return NULL;
    esp_err_t rc=ap?esp_netif_attach_wifi_ap(netif):esp_netif_attach_wifi_station(netif);
    if(rc!=ESP_OK) {esp_netif_destroy_default_wifi(netif);return NULL;}
    return netif;
}
static void air_supervise(void);

static void wifi_scan_collect(void)
{
    if(!s_scan_cache) { esp_wifi_clear_ap_list();s_scan_busy=0;return; }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    wifi_ap_record_t *recs=s_scan_cache->records;
    uint16_t got = n < WSCAN_MAX ? n : WSCAN_MAX;
    if (got) { if(esp_wifi_scan_get_ap_records(&got, recs)!=ESP_OK) got=0; }
    else     { esp_wifi_clear_ap_list(); got = 0; }
    hal_wifi_ap *next=s_scan_cache->next;
    int m = 0;
    for (int i = 0; i < got; i++) {
        /* The wire name is at most 32 bytes. Never trust an SDK terminator or
         * truncate a connect target to its drawn label. */
        char ss[33];memcpy(ss,recs[i].ssid,32);ss[32]=0;
        int duplicate=0;
        for(int j=0;j<m;j++) if(!strcmp(next[j].ssid,ss)) duplicate=1;
        if(duplicate) continue;
        snprintf(next[m].ssid, sizeof next[m].ssid, "%s", ss[0] ? ss : "(hidden)");
        next[m].rssi = recs[i].rssi;
        next[m].chan = recs[i].primary;
        m++;
    }
    portENTER_CRITICAL(&s_scan_lock);
    memcpy(s_aps,next,(size_t)m*sizeof *next);s_ap_n = m;
    s_scan_busy = 0;
    s_scan_done_ms = millis_raw();
    portEXIT_CRITICAL(&s_scan_lock);
    wlog("wifi scan: %d AP%s", m, m == 1 ? "" : "s");
}

int hal_wifi_scan(hal_wifi_ap *out, int max)
{
    if(!out || max<=0 || !s_scan_cache) return 0;
    uint32_t now = millis_raw();
    portENTER_CRITICAL(&s_scan_lock);
    int start=s_wifi_enabled && s_wifi_initialized && !atomic_load(&s_air_reserved) && !s_scan_busy && (!s_scan_try_ms || now-s_scan_try_ms>1200) &&
              (s_scan_done_ms==0 || now-s_scan_done_ms>8000);
    if(start) { s_scan_busy=1;s_scan_start_ms=now;s_scan_try_ms=now; }
    portEXIT_CRITICAL(&s_scan_lock);
    if (start) {
        wifi_scan_config_t cfg = {
            .show_hidden = true,                /* hidden SSIDs are still signals worth showing */
            .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
            /* Default dwell is required by ESP-IDF Wi-Fi/BLE coexistence. */
        };
        esp_err_t rc = esp_wifi_scan_start(&cfg, false);      /* async — never blocks the UI */
        if(rc!=ESP_OK) {
            s_scan_busy=0;
            /* Release an unsuccessful station join so the next sweep can run.
             * Retry timer defers reconnection until the sweep finishes. */
            if(rc==ESP_ERR_WIFI_STATE && !g_wifi_up) {
                s_scan_pause_until=now+7000;
                esp_wifi_disconnect();
            }
            wlog("wifi scan start: %s", esp_err_to_name(rc));
        }
    }
    if (s_scan_busy && now - s_scan_start_ms > 9000) {        /* watchdog: don't wedge forever */
        esp_wifi_scan_stop();
        s_scan_busy = 0;
        wlog("wifi scan timed out");
    }
    portENTER_CRITICAL(&s_scan_lock);
    int m = s_ap_n < max ? s_ap_n : max;
    for (int i = 0; i < m; i++) out[i] = s_aps[i];
    portEXIT_CRITICAL(&s_scan_lock);
    return m;
}

/* Wi-Fi station connect (own-device: joins the user's own network). Once associated it
 * prints its DHCP IP over stdout (USB-serial) as "WIFI_IP: <ip>" so the host can find the
 * board's config web page. Auto-reconnects while a connect is desired. */

/* ---- config web server (served on the watch's own IP once on the network) ---- */
static httpd_handle_t s_httpd;

#include "watch_setup.inc"

/* Wi-Fi alternative to the BLE MEDIA_COVER char: POST raw RGB565 (COVER_PX^2) to /cover. */
static esp_err_t web_cover_write(httpd_req_t *req)
{
    if(req->content_len!=COVER_BYTES)
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Expected exactly 125000 RGB565 bytes");
    if(!cover_begin(2))
        return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"cover busy or no memory");
    int64_t deadline=esp_timer_get_time()+15000000;
    uint8_t *dst=(uint8_t *)s_cover_buffers[s_cover_stage];
    while(s_cover_received<COVER_BYTES && esp_timer_get_time()<deadline) {
        size_t left=COVER_BYTES-s_cover_received;
        size_t count=left>4096?4096:left;
        int r=httpd_req_recv(req,(char *)dst+s_cover_received,count);
        if(r==HTTPD_SOCK_ERR_TIMEOUT) continue;
        if(r<=0 || (size_t)r>count || esp_timer_get_time()>=deadline) break;
        s_cover_received+=(size_t)r;
    }
    int complete=s_cover_received==COVER_BYTES;
    cover_end(2,complete);
    return complete?httpd_resp_sendstr(req,"ok"):
        httpd_resp_send_err(req,HTTPD_408_REQ_TIMEOUT,"Incomplete cover; prior image preserved");
}
static esp_err_t web_cover(httpd_req_t *req)
{
    if(req->content_len!=COVER_BYTES)
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Expected exactly 125000 RGB565 bytes");
    if(!s_cover_writer || xSemaphoreTake(s_cover_writer,0)!=pdTRUE)
        return httpd_resp_send_err(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Cover upload already in progress");
    esp_err_t rc=web_cover_write(req);xSemaphoreGive(s_cover_writer);return rc;
}
/* Wi-Fi now-playing: same payload as the BLE MEDIA_INFO char, so a PC bridge can run
 * WiFi-only (no BLE pairing/interference). */
static esp_err_t web_media(httpd_req_t *req)
{
    uint8_t buf[160];
    int total = req->content_len, got = 0;
    if (total > (int)sizeof buf - 1) total = (int)sizeof buf - 1;
    while (got < total) {
        int r = httpd_req_recv(req, (char *)buf + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        got += r;
    }
    buf[got] = 0;
    media_apply(buf, got, "wifi");
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}
/* Transport back-channel: the bridge polls this; "<seq> <cmd>" changes when the watch is tapped. */
static esp_err_t web_cmd(httpd_req_t *req)
{
    char b[32];
    snprintf(b, sizeof b, "%d %d %d", g_cmd_seq, g_cmd_val, g_cmd_pos);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, b);
}
static esp_err_t web_notify(httpd_req_t *req)
{
    uint8_t body[59];
    size_t wanted=req->content_len, got=0;
    if(wanted<3 || wanted>sizeof body) {
        httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Expected app|title, max 19|39 bytes");
        return ESP_FAIL;
    }
    while(got<wanted) {
        int n=httpd_req_recv(req,(char *)body+got,wanted-got);
        if(n==HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_err(req,HTTPD_408_REQ_TIMEOUT,"Notification body timed out");
            return ESP_FAIL;
        }
        if(n<=0) return ESP_FAIL;
        got+=(size_t)n;
    }
    pending_notification p;
    if(!notification_decode(body,got,&p))
        return httpd_resp_send_err(req,HTTPD_400_BAD_REQUEST,"Invalid app|title");
    if(!hal_notification_post(p.app,p.title)) {
        httpd_resp_set_status(req,"503 Service Unavailable");
        return httpd_resp_sendstr(req,"Notification queue full");
    }
    httpd_resp_set_type(req,"text/plain");
    return httpd_resp_sendstr(req,"queued");
}
static void web_start(void)
{
    if (s_httpd) return;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.recv_wait_timeout = 12;              /* cover POST is exactly 125000 bytes */
    cfg.max_uri_handlers = 16;
    cfg.stack_size = 6144;
    /* Portal handlers only queue changes. Flash-writing OTA uploads run on a
     * separate internal-stack task (ota.c), leaving internal RAM for SD DMA. */
    cfg.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    esp_err_t server_rc=httpd_start(&s_httpd,&cfg);
    if(server_rc!=ESP_OK) {s_httpd=NULL;ESP_LOGE("setup","HTTP start: %s; internal free=%u",esp_err_to_name(server_rc),(unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));return;}
    httpd_uri_t r  = { .uri = "/",      .method = HTTP_GET,  .handler = web_root };
    httpd_uri_t w  = { .uri = "/wifi",  .method = HTTP_POST, .handler = web_wifi };
    httpd_uri_t c  = { .uri = "/cover", .method = HTTP_POST, .handler = web_cover };
    httpd_uri_t m  = { .uri = "/media", .method = HTTP_POST, .handler = web_media };
    httpd_uri_t cm = { .uri = "/cmd",   .method = HTTP_GET,  .handler = web_cmd };
    httpd_uri_t nt = { .uri = "/notify",.method = HTTP_POST, .handler = web_notify };
    httpd_register_uri_handler(s_httpd, &r);
    httpd_register_uri_handler(s_httpd, &w);
    httpd_register_uri_handler(s_httpd, &c);
    httpd_register_uri_handler(s_httpd, &m);
    httpd_register_uri_handler(s_httpd, &cm);
    httpd_register_uri_handler(s_httpd, &nt);
    httpd_uri_t ns={.uri="/api/networks",.method=HTTP_GET,.handler=web_networks};
    httpd_uri_t st={.uri="/api/status",.method=HTTP_GET,.handler=web_status};
    httpd_uri_t se={.uri="/api/settings",.method=HTTP_POST,.handler=web_settings};
    httpd_register_uri_handler(s_httpd,&ns);
    httpd_register_uri_handler(s_httpd,&st);
    httpd_register_uri_handler(s_httpd,&se);
    ota_register_web(s_httpd);
    printf("WEB: server up (/wifi /cover /media /cmd /notify /update)\n");
}

/* SNTP: once online, sync the clock from the internet (auto date/time over Wi-Fi). */
static void ntp_synced(struct timeval *tv)
{
    board_clock_changed(tv->tv_sec);
    g_synced = 1;
    time_t now = time(NULL); struct tm lt; localtime_r(&now, &lt);
    wlog("NTP synced %02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
}
static void ntp_start(void)
{
    if (esp_sntp_enabled()) return;
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(ntp_synced);
    esp_sntp_init();
    printf("NTP: started\n");
}

/* Reconnect with backoff: an immediate esp_wifi_connect() on every drop kept the radio scanning
 * flat out away from the saved network (+60-90 mA, power review). 2 s, doubling to 5 min. */
static esp_timer_handle_t s_wifi_retry;
static atomic_uint s_wifi_backoff_ms;
static void wifi_retry_cb(void *arg)
{
    (void)arg;
    if(!s_wifi_want || !s_wifi_enabled || atomic_load(&s_air_reserved)) return;
    if(s_scan_busy || (int32_t)(millis_raw()-s_scan_pause_until)<0) {
        esp_timer_start_once(s_wifi_retry,1000000);return;
    }
    esp_wifi_connect();
}
static void wifi_retry_later(void)
{
    if(!s_wifi_enabled) return;
    if (!s_wifi_retry) {
        const esp_timer_create_args_t a = { .callback = wifi_retry_cb, .name = "wifi_retry" };
        if (esp_timer_create(&a, &s_wifi_retry) != ESP_OK) { esp_wifi_connect(); return; }
    }
    s_wifi_backoff_ms = s_wifi_backoff_ms ? s_wifi_backoff_ms * 2 : 2000;
    if (s_wifi_backoff_ms > 300000) s_wifi_backoff_ms = 300000;
    esp_timer_stop(s_wifi_retry);
    esp_timer_start_once(s_wifi_retry, (uint64_t)s_wifi_backoff_ms * 1000);
}
/* Explicit, password-protected setup session; leave STA credentials untouched. */
static esp_timer_handle_t s_ap_timer;
static esp_netif_t *s_ap_netif;
static void ap_stop_cb(void *arg)
{
    (void)arg;
    hal_setup_set(0);
}
int hal_setup_set(int enabled)
{
    if(!s_wifi_initialized) {
        if(enabled) {s_wifi_on_demand=1;s_wifi_startup.retry_ms=0;wifi_message("Wi-Fi starting; retry Phone setup");}
        return 0;
    }
    if(!enabled) {
        ota_session_end();
        if(s_ap_timer) esp_timer_stop(s_ap_timer);
        int rc=esp_wifi_set_mode(WIFI_MODE_STA);
        portENTER_CRITICAL(&s_setup_lock);s_setup_active=0;portEXIT_CRITICAL(&s_setup_lock);
        return rc==ESP_OK;
    }
    if(atomic_load(&s_air_reserved)) return 0;
    if(!s_ap_netif) {
        if(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)<4096) {
            radio_fail(&s_wifi_startup,10,ESP_ERR_NO_MEM);return 0;
        }
        s_ap_netif=wifi_netif_new(1);
        if(s_ap_netif && esp_wifi_set_default_wifi_ap_handlers()!=ESP_OK) {
            esp_netif_destroy_default_wifi(s_ap_netif);s_ap_netif=NULL;
        }
    }
    if(!s_ap_netif) return 0;
    wifi_config_t ap={0};
    uint8_t mac[6]={0};esp_wifi_get_mac(WIFI_IF_STA,mac);
    char ssid[33],pass[24];
    snprintf(ssid,sizeof ssid,"Watch-Setup-%02X%02X",mac[4],mac[5]);
    snprintf(pass,sizeof pass,"%08lx%04lx",(unsigned long)esp_random(),(unsigned long)(esp_random()&0xffff));
    memcpy(ap.ap.ssid,ssid,strlen(ssid));ap.ap.ssid_len=strlen(ssid);
    memcpy(ap.ap.password,pass,strlen(pass));
    ap.ap.channel=1;ap.ap.max_connection=2;ap.ap.authmode=WIFI_AUTH_WPA2_PSK;
    if(esp_wifi_set_mode(WIFI_MODE_APSTA)!=ESP_OK || esp_wifi_set_config(WIFI_IF_AP,&ap)!=ESP_OK ||
       esp_wifi_start()!=ESP_OK) { esp_wifi_set_mode(WIFI_MODE_STA);return 0; }
    if(!s_ap_timer) {
        const esp_timer_create_args_t a={.callback=ap_stop_cb,.name="wifi_ap"};
        if(esp_timer_create(&a,&s_ap_timer)!=ESP_OK) {esp_wifi_set_mode(WIFI_MODE_STA);return 0;}
    }
    esp_timer_stop(s_ap_timer);
    if(esp_timer_start_once(s_ap_timer,600ULL*1000000ULL)!=ESP_OK) {esp_wifi_set_mode(WIFI_MODE_STA);return 0;}
    portENTER_CRITICAL(&s_setup_lock);
    snprintf(s_setup_ssid,sizeof s_setup_ssid,"%s",ssid);snprintf(s_setup_pass,sizeof s_setup_pass,"%s",pass);
    s_setup_active=1;
    portEXIT_CRITICAL(&s_setup_lock);
    web_start();
    if(!s_httpd) {hal_setup_set(0);return 0;}
    ota_session_begin();
    return 1;
}
void hal_wifi_set_enabled(int enabled)
{
    int changed=atomic_exchange(&s_wifi_enabled,!!enabled)!=!!enabled;
    if(changed && enabled)s_wifi_startup.retry_ms=0;
    if(!enabled) {s_wifi_on_demand=0;s_wifi_start_pending=0;}
    if(!s_wifi_initialized) {wifi_message(enabled?"Wi-Fi initialization deferred":"Wi-Fi off");return;}
    if(!enabled) {
        hal_wifi_air_stop();
        if(s_wifi_retry) esp_timer_stop(s_wifi_retry);
        hal_setup_set(0);esp_wifi_stop();s_scan_busy=0;g_wifi_up=0;
        wifi_message("Wi-Fi off");
    } else {
        esp_err_t rc=esp_wifi_start();
        if(rc!=ESP_OK) {s_wifi_start_pending=1;radio_fail(&s_wifi_startup,9,rc);wifi_message("Wi-Fi start failed; retrying");return;}
        s_wifi_start_pending=0;
        if(s_wifi_want) { wifi_message("Joining network...");esp_wifi_connect(); }
        else wifi_message("Select a network");
    }
}

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        wifi_scan_collect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_up = 0;
        wifi_event_sta_disconnected_t *ev=data;
        if(s_wifi_want) {
            wifi_message(ev->reason==WIFI_REASON_AUTH_FAIL || ev->reason==WIFI_REASON_HANDSHAKE_TIMEOUT ||
                         ev->reason==WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT?"Password rejected; try again":
                         ev->reason==WIFI_REASON_NO_AP_FOUND?"Network not found; retrying":"Disconnected; retrying");
        }
        if (s_wifi_want) {
            wifi_retry_later();
            printf("WIFI: disconnected reason=%u, retry in %u s\n", ev->reason, (unsigned)(s_wifi_backoff_ms / 1000));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        g_wifi_up = 1;
        s_wifi_backoff_ms = 0;                   /* connected: the next drop retries quickly again */
        char status[96];snprintf(status,sizeof status,"Connected / " IPSTR,IP2STR(&ev->ip_info.ip));
        wifi_message(status);
        if(s_wifi_retry) esp_timer_stop(s_wifi_retry);
        wlog("WiFi IP " IPSTR, IP2STR(&ev->ip_info.ip));
        web_start();
        ntp_start();       /* auto time sync over the internet */
        hal_weather_refresh();
    }
}

/* Successful credentials checkpoint to SD off the render task, retaining a
 * pending copy on failed writes. NVS remains migration/card-absent fallback. */
typedef struct { char ssid[33]; char pass[65]; } wifi_cred_t;
static QueueHandle_t s_wifi_requests;
static esp_err_t wifi_configure(const wifi_cred_t *credentials)
{
    wifi_config_t wc={0};
    memcpy(wc.sta.ssid,credentials->ssid,strlen(credentials->ssid));
    memcpy(wc.sta.password,credentials->pass,strlen(credentials->pass));
    wc.sta.threshold.authmode=credentials->pass[0]?WIFI_AUTH_WPA2_PSK:WIFI_AUTH_OPEN;
    esp_err_t rc=esp_wifi_set_config(WIFI_IF_STA,&wc);
    memset(&wc,0,sizeof wc);return rc;
}
static int wifi_credentials_checkpoint(const wifi_cred_t *credentials)
{
    /* Association completes on this worker even while the UI is read-only.
     * Its later SD/NVS save must share the OTA owner, not rely on a UI snapshot
     * or a check of ota_busy before touching either persistent store. */
    if(!ota_external_activity_begin()) return 0;
    int ok=hal_store_save_checked("wifi_cred",credentials,sizeof *credentials);
    ota_external_activity_end();return ok;
}
static void wifi_worker(void *unused)
{
    (void)unused;
    while(!atomic_load(&s_wifi_initialized))vTaskDelay(pdMS_TO_TICKS(100));
    wifi_cred_t candidate={0},durable={0},next;
    int awaiting_success=0;
    uint32_t save_at=0;
    for(;;) {
        air_supervise();
        /* Timer-failure recovery runs off the UI; normal BLE control is hosted
         * by NimBLE's callout and remains available without this Wi-Fi worker. */
        if(atomic_load(&s_ble_initialized) && !atomic_load(&s_ble_control_clock_ready) &&
           atomic_exchange(&s_ble_control_pending,0))
            ble_npl_eventq_put(nimble_port_get_dflt_eventq(),&s_ble_control_event);
        /* 100 ms only while the Wi-Fi analyzer hops channels; otherwise a 1 s wait on the shared wake
         * grid (a queued request still wakes it at once). */
        uint32_t worker_wait=atomic_load(&s_air_reserved)?100u:sleep_grid_delay_ms(sleep_grid_now_ms(),1000u);
        if(xQueueReceive(s_wifi_requests,&next,pdMS_TO_TICKS(worker_wait))==pdTRUE) {
            hal_wifi_air_stop();
            if(atomic_load(&s_air_reserved)) {
                /* Keep a newer queued request if one arrived during cleanup. */
                xQueueSendToFront(s_wifi_requests,&next,0);memset(&next,0,sizeof next);continue;
            }
            s_wifi_want=0;
            if(s_wifi_retry) esp_timer_stop(s_wifi_retry);
            esp_wifi_scan_stop();s_scan_busy=0;
            s_scan_pause_until=0;
            esp_wifi_disconnect();g_wifi_up=0;
            esp_err_t rc=wifi_configure(&next);
            if(rc==ESP_OK) {
                candidate=next;awaiting_success=1;s_wifi_want=1;s_wifi_backoff_ms=0;
                if(s_wifi_enabled) {
                    esp_wifi_start();
                    wifi_message("Joining network...");
                    rc=esp_wifi_connect();
                } else wifi_message("Wi-Fi off");
            }
            if(rc!=ESP_OK) wifi_message("Could not connect; try again");
            memset(&next,0,sizeof next);
        }
        if(awaiting_success && g_wifi_up) {
            wifi_ap_record_t ap;
            if(esp_wifi_sta_get_ap_info(&ap)==ESP_OK && !strncmp((const char *)ap.ssid,candidate.ssid,32)) {
                durable=candidate;s_credentials_dirty=1;save_at=0;awaiting_success=0;
            }
        }
        if(s_credentials_dirty && durable.ssid[0] && (!save_at || millis_raw()-save_at>=5000)) {
            save_at=millis_raw();
            if(wifi_credentials_checkpoint(&durable)) s_credentials_dirty=0;
        }
    }
}

void hal_wifi_connect(const char *ssid, const char *pass)
{
    if(!setup_credentials_valid(ssid,pass)) {wifi_message("Invalid network name or password");return;}
    if(!s_wifi_requests) {wifi_message("Wi-Fi unavailable");return;}
    wifi_cred_t c={0};
    snprintf(c.ssid,sizeof c.ssid,"%s",ssid);snprintf(c.pass,sizeof c.pass,"%s",pass);
    xQueueOverwrite(s_wifi_requests,&c);memset(&c,0,sizeof c);
    wifi_message("Connection queued...");
}

/* ---- Wi-Fi air analyzer: promiscuous monitor mode + channel hopping --------------------
 * OBSERVE-ONLY. We register an RX callback and count frames; we never transmit. Keep the
 * callback tiny — it runs in the Wi-Fi task on every single frame in the air. */
#define AIR_DWELL_MS 120

static hal_air_chan s_air_view[HAL_AIR_NCH];
static uint8_t      s_air_hist[HAL_AIR_NCH][HAL_AIR_HIST];
static struct { uint32_t pkts, bytes; int8_t rssi_max; uint16_t mgmt, data, ctrl; }
                    s_air_acc[HAL_AIR_NCH];
static atomic_int s_air_on, s_air_ch=1, s_air_stop_requested, s_air_hop_pending;
static atomic_uint s_air_last_read;
static int          s_air_want_sta;
static esp_timer_handle_t s_air_timer;
static uint8_t s_air_restore_channel;
static wifi_second_chan_t s_air_restore_second;
static StaticSemaphore_t s_air_mutex_storage;
static SemaphoreHandle_t s_air_mutex;
static portMUX_TYPE s_air_data_lock=portMUX_INITIALIZER_UNLOCKED;

static void air_clear_acc(int i)
{
    memset(&s_air_acc[i], 0, sizeof s_air_acc[i]);
    s_air_acc[i].rssi_max = -128;
}

static void air_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    int i = s_air_ch - 1;
    if (!atomic_load(&s_air_on) || i < 0 || i >= HAL_AIR_NCH || !p) return;
    portENTER_CRITICAL(&s_air_data_lock);
    s_air_acc[i].pkts++;
    s_air_acc[i].bytes += (uint32_t)p->rx_ctrl.sig_len;
    if (p->rx_ctrl.rssi > s_air_acc[i].rssi_max) s_air_acc[i].rssi_max = p->rx_ctrl.rssi;
    if      (type == WIFI_PKT_MGMT) s_air_acc[i].mgmt++;
    else if (type == WIFI_PKT_DATA) s_air_acc[i].data++;
    else if (type == WIFI_PKT_CTRL) s_air_acc[i].ctrl++;
    portEXIT_CRITICAL(&s_air_data_lock);
}

/* Caller owns s_air_mutex. Failed hardware cleanup remains reserved and is
 * retried by the independent Wi-Fi worker, even if the hop timer never fires. */
static int air_shutdown(void)
{
    atomic_store(&s_air_on,0);atomic_store(&s_air_hop_pending,0);
    if (s_air_timer) esp_timer_stop(s_air_timer);
    esp_err_t rc=esp_wifi_set_promiscuous(false);
    esp_err_t cb=esp_wifi_set_promiscuous_rx_cb(NULL);
    if(s_wifi_enabled && (rc!=ESP_OK || cb!=ESP_OK ||
       esp_wifi_set_channel(s_air_restore_channel,s_air_restore_second)!=ESP_OK)) {
        atomic_store(&s_air_stop_requested,1);return 0;
    }
    s_wifi_want=s_air_want_sta;
    atomic_store(&s_air_stop_requested,0);atomic_store(&s_air_reserved,0);
    wlog("air: monitor off");
    if(s_wifi_want && s_wifi_enabled) {
        rc=esp_wifi_connect();
        if(rc!=ESP_OK) wifi_retry_later();
    }
    return 1;
}

/* Timer callback only wakes work. Driver changes and cleanup share one mutex;
 * delayed timer callbacks never operate the Wi-Fi driver. */
static void air_hop(void *arg)
{
    (void)arg;
    if(atomic_load(&s_air_on)) atomic_store(&s_air_hop_pending,1);
}
static void air_supervise(void)
{
    if(!s_air_mutex || !atomic_load(&s_air_reserved) || xSemaphoreTake(s_air_mutex,0)!=pdTRUE) return;
    if(atomic_load(&s_air_stop_requested) || !s_wifi_enabled ||
       millis_raw()-atomic_load(&s_air_last_read)>=3000) {
        air_shutdown();xSemaphoreGive(s_air_mutex);return;
    }
    if(!atomic_exchange(&s_air_hop_pending,0)) {xSemaphoreGive(s_air_mutex);return;}
    int i = s_air_ch - 1;
    portENTER_CRITICAL(&s_air_data_lock);
    uint32_t u = s_air_acc[i].bytes / 24;          /* same dwell everywhere -> comparable */
    if (u > 255) u = 255;
    s_air_view[i].pkts = (uint16_t)((s_air_view[i].pkts * 3 + s_air_acc[i].pkts) / 4);
    s_air_view[i].util = (uint8_t)((s_air_view[i].util * 3 + u) / 4);
    s_air_view[i].rssi_max = s_air_acc[i].rssi_max > -128 ? s_air_acc[i].rssi_max : 0;
    s_air_view[i].mgmt = s_air_acc[i].mgmt;
    s_air_view[i].data = s_air_acc[i].data;
    s_air_view[i].ctrl = s_air_acc[i].ctrl;

    memmove(&s_air_hist[i][0], &s_air_hist[i][1], HAL_AIR_HIST - 1);
    s_air_hist[i][HAL_AIR_HIST - 1] = s_air_view[i].util;
    air_clear_acc(i);
    portEXIT_CRITICAL(&s_air_data_lock);
    int next=(s_air_ch%HAL_AIR_NCH)+1;
    if(esp_wifi_set_channel((uint8_t)next,WIFI_SECOND_CHAN_NONE)!=ESP_OK) air_shutdown();
    else s_air_ch=next;
    xSemaphoreGive(s_air_mutex);
}

int hal_wifi_air_running(void) { return s_air_on; }
/* board.c minute log: 0 radio off, 1 radio on (joining, retrying, setup or monitor), 2 has an IP. */
int hal_wifi_log_state(void)
{
    if (atomic_load(&g_wifi_up)) return 2;
    return atomic_load(&s_wifi_enabled) || atomic_load(&s_wifi_on_demand) || s_air_on;
}
int hal_external_power(void) { hal_power p; hal_power_get(&p); return p.external_power; }
int hal_wifi_air_channel(void) { return s_air_ch; }

int hal_wifi_air_start(void)
{
    portENTER_CRITICAL(&s_setup_lock);int setup_active=s_setup_active;portEXIT_CRITICAL(&s_setup_lock);
    if(!s_wifi_enabled || !atomic_load(&s_wifi_worker_ready) || setup_active ||
       !s_air_mutex || xSemaphoreTake(s_air_mutex,0)!=pdTRUE) return 0;
    s_air_last_read=millis_raw();
    if(s_air_on) {xSemaphoreGive(s_air_mutex);return 1;}
    if(atomic_load(&s_air_reserved)) {xSemaphoreGive(s_air_mutex);return 0;}
    esp_err_t rc=ESP_OK;
    /* Timer allocation must succeed before the station is parked. */
    if(!s_air_timer) {
        const esp_timer_create_args_t a={.callback=air_hop,.name="airhop"};
        rc=esp_timer_create(&a,&s_air_timer);
    }
    if(rc!=ESP_OK || !s_air_timer ||
       esp_wifi_get_channel(&s_air_restore_channel,&s_air_restore_second)!=ESP_OK) {
        xSemaphoreGive(s_air_mutex);return 0;
    }
    portENTER_CRITICAL(&s_air_data_lock);
    memset(s_air_view, 0, sizeof s_air_view);
    memset(s_air_hist, 0, sizeof s_air_hist);
    for (int i = 0; i < HAL_AIR_NCH; i++) air_clear_acc(i);
    portEXIT_CRITICAL(&s_air_data_lock);

    /* Hopping channels would break an association anyway, so park the station link. */
    s_air_want_sta = s_wifi_want;
    atomic_store(&s_air_reserved,1);atomic_store(&s_air_stop_requested,0);
    s_wifi_want = 0;
    if(s_wifi_retry) esp_timer_stop(s_wifi_retry);
    esp_wifi_scan_stop();s_scan_busy=0;
    esp_wifi_disconnect();

    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    rc=esp_wifi_set_promiscuous_filter(&f);
    if(rc==ESP_OK) rc=esp_wifi_set_promiscuous_rx_cb(air_rx);
    if(rc==ESP_OK) rc=esp_wifi_set_promiscuous(true);
    if(rc==ESP_OK) rc=esp_wifi_set_channel(1,WIFI_SECOND_CHAN_NONE);
    if(rc==ESP_OK) rc=esp_timer_start_periodic(s_air_timer,AIR_DWELL_MS*1000);
    if (rc != ESP_OK) {
        wlog("air: start %s", esp_err_to_name(rc));
        air_shutdown();xSemaphoreGive(s_air_mutex);
        return 0;
    }
    s_air_ch = 1;
    s_air_on = 1;
    wlog("air: monitor on, ch1-%d", HAL_AIR_NCH);
    xSemaphoreGive(s_air_mutex);
    return 1;
}

void hal_wifi_air_stop(void)
{
    if(!atomic_load(&s_air_reserved)) return;
    atomic_store(&s_air_stop_requested,1);
    if(s_air_mutex && xSemaphoreTake(s_air_mutex,0)==pdTRUE) {
        air_shutdown();xSemaphoreGive(s_air_mutex);
    }
}

void hal_wifi_air_get(hal_air_chan *out)
{
    if(!out) return;
    s_air_last_read = millis_raw();          /* keep-alive uses monotonic raw time */
    portENTER_CRITICAL(&s_air_data_lock);
    memcpy(out, s_air_view, sizeof s_air_view);
    portEXIT_CRITICAL(&s_air_data_lock);
}

void hal_wifi_air_hist(uint8_t *out)
{
    if(!out) return;
    s_air_last_read = millis_raw();
    portENTER_CRITICAL(&s_air_data_lock);
    memcpy(out, s_air_hist, sizeof s_air_hist);
    portEXIT_CRITICAL(&s_air_data_lock);
}

/* Station by default. The explicit Phone setup session temporarily enables a
 * password-protected AP+STA portal; credentials persist through the SD worker. */
static int wifi_init(void)
{
    if(s_wifi_initialized)return 1;
    esp_err_t e=esp_netif_init();
    if(e!=ESP_OK)return radio_fail(&s_wifi_startup,2,e);
    e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE)return radio_fail(&s_wifi_startup,3,e);
    if(!s_sta_netif)s_sta_netif=wifi_netif_new(0);
    if(!s_sta_netif)return radio_fail(&s_wifi_startup,4,ESP_ERR_NO_MEM);
    if(!s_wifi_default_handlers) {
        e=esp_wifi_set_default_wifi_sta_handlers();
        if(e!=ESP_OK)return radio_fail(&s_wifi_startup,4,e);
        s_wifi_default_handlers=1;
    }

    if(!s_scan_cache)s_scan_cache=heap_caps_calloc(1,sizeof *s_scan_cache,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s_scan_cache)return radio_fail(&s_wifi_startup,5,ESP_ERR_NO_MEM);
    if(!s_wifi_requests)s_wifi_requests=xQueueCreate(1,sizeof(wifi_cred_t));
    if(!s_wifi_requests)return radio_fail(&s_wifi_startup,5,ESP_ERR_NO_MEM);
    /* Reserve the persistence owner's INTERNAL stack before the driver spends
     * optional heap. It parks until all checked initialization is complete. */
    if(!s_wifi_task && xTaskCreate(wifi_worker,"wifi-settings",6144,NULL,2,&s_wifi_task)!=pdPASS)
        return radio_fail(&s_wifi_startup,5,ESP_ERR_NO_MEM);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if(!s_wifi_driver_owned) {
        e=esp_wifi_init(&cfg);
        if(e!=ESP_OK)return radio_fail(&s_wifi_startup,6,e);
        s_wifi_driver_owned=1;
    }
    if(!s_wifi_event_registered) {
        e=esp_event_handler_instance_register(WIFI_EVENT,ESP_EVENT_ANY_ID,wifi_evt,NULL,&s_wifi_event);
        if(e!=ESP_OK)return radio_fail(&s_wifi_startup,7,e);
        s_wifi_event_registered=1;
    }
    if(!s_wifi_ip_registered) {
        e=esp_event_handler_instance_register(IP_EVENT,IP_EVENT_STA_GOT_IP,wifi_evt,NULL,&s_wifi_ip);
        if(e!=ESP_OK)return radio_fail(&s_wifi_startup,7,e);
        s_wifi_ip_registered=1;
    }
    e=esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if(e!=ESP_OK)return radio_fail(&s_wifi_startup,8,e);

    /* The stock country policy caps scanning at channels 1-11; India/EU and much of the world
     * also use 12-13, so APs parked up there were simply invisible to the scanner. */
    wifi_country_t ctry = { .cc = "IN", .schan = 1, .nchan = 13,
                            .policy = WIFI_COUNTRY_POLICY_MANUAL };
    e=esp_wifi_set_country(&ctry);
    if(e!=ESP_OK)return radio_fail(&s_wifi_startup,8,e);

    wifi_cred_t c = { 0 };
    int have = hal_store_load("wifi_cred", &c, sizeof c) && c.ssid[0];

    e=esp_wifi_set_mode(WIFI_MODE_STA);
    if(e==ESP_OK && s_wifi_enabled)e=esp_wifi_start();
    if(e!=ESP_OK) {memset(&c,0,sizeof c);return radio_fail(&s_wifi_startup,9,e);}
    atomic_store(&s_wifi_initialized,1);
    atomic_store(&s_wifi_worker_ready,1);
    if(have && memchr(c.ssid,0,sizeof c.ssid) && memchr(c.pass,0,sizeof c.pass))
        hal_wifi_connect(c.ssid,c.pass);
    else wifi_message("Select a network or start Phone setup");
    memset(&c,0,sizeof c);
    s_wifi_on_demand=0;s_wifi_startup.stage=0;s_wifi_startup.error=0;s_wifi_startup.retry_ms=0;
    return 1;
}

void hal_esp32_init_periph(void)
{
    s_cover_writer=xSemaphoreCreateMutexStatic(&s_cover_writer_storage);
    s_air_mutex=xSemaphoreCreateMutexStatic(&s_air_mutex_storage);
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Never silently erase user settings to recover an NVS init error. */
        ESP_LOGE("hal", "NVS needs recovery (0x%x); preserving saved configuration", (unsigned)nv);
    }
    if (nv != ESP_ERR_NVS_NO_FREE_PAGES && nv != ESP_ERR_NVS_NEW_VERSION_FOUND)
        ESP_ERROR_CHECK(nv);

    s_notifications=xQueueCreate(8,sizeof(pending_notification));
    if(!s_notifications) ESP_LOGW("hal","Notification queue unavailable");

    /* Radios: STA-mode Wi-Fi for AP scans + NimBLE observer for BLE recon.
     * Both share the 2.4GHz radio; software coexistence (sdkconfig) arbitrates
     * a foreground Wi-Fi scan against the background BLE discovery. */


    adc_oneshot_unit_init_cfg_t acfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&acfg, &s_adc));
    adc_oneshot_chan_cfg_t chcfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chcfg));
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK)
        s_cali = NULL;
#ifdef PIN_CHG_STAT
    gpio_config_t chg = { .pin_bit_mask = 1ULL << PIN_CHG_STAT, .mode = GPIO_MODE_INPUT,
                          .pull_up_en = GPIO_PULLUP_ENABLE };
    ESP_ERROR_CHECK(gpio_config(&chg));
#endif
#ifdef PIN_LED
    gpio_config_t led = { .pin_bit_mask = 1ULL << PIN_LED, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&led));
    gpio_set_level(PIN_LED, 1); /* active low: off */
#endif

}

/* Start radio callbacks only after the RTC and saved timezone are restored. */
void hal_esp32_start_radios(void)
{
    s_radios_started=1;
    radio_poll();
}
static void radio_poll(void)
{
    if(!s_radios_started || prefs_loading() || ota_busy() ||
       atomic_flag_test_and_set(&s_radio_starting))return;
    s_radio_busy=1;
    if(!s_ble_enabled && !s_ble_initialized && !s_ble_cleanup_pending && s_ble_host_task) {
        vTaskDelete(s_ble_host_task);s_ble_host_task=NULL; /* Still parked; no SDK owner ran. */
    }
    if(s_ble_cleanup_pending) {
        if((int32_t)(millis_raw()-atomic_load(&s_ble_startup.retry_ms))>=0)(void)ble_cleanup();
    } else if(s_wifi_initialized && s_wifi_enabled && s_wifi_start_pending &&
       (int32_t)(millis_raw()-atomic_load(&s_wifi_startup.retry_ms))>=0) {
        esp_err_t rc=esp_wifi_start();
        if(rc==ESP_OK) {
            s_wifi_start_pending=0;s_wifi_startup.error=0;s_wifi_startup.stage=0;
            if(s_wifi_want)esp_wifi_connect();
            wifi_message(s_wifi_want?"Joining network...":"Select a network");
        } else radio_fail(&s_wifi_startup,9,rc);
    } else if(!s_wifi_initialized && (s_wifi_enabled || s_wifi_on_demand) &&
       radio_admit(&s_wifi_startup,s_wifi_task?16384:24576,8192)) {
        if(!wifi_init())wifi_message("Wi-Fi deferred; retrying initialization");
    } else if(!s_ble_initialized && s_ble_enabled &&
              radio_admit(&s_ble_startup,s_ble_host_task?16384:20480,8192)) {
        (void)ble_init();
    }
    /* The checked, idempotent owner retains its configuration on allocation
     * failure. Retry its INTERNAL8KiB worker with the same bounded admission. */
    if(s_wifi_initialized && s_wifi_enabled && !weather_service_ready() &&
       radio_admit(&s_weather_startup,12288,8192)) {
        if(weather_service_start()) {
            s_weather_startup.stage=0;s_weather_startup.error=0;s_weather_startup.retry_ms=0;
        } else radio_fail(&s_weather_startup,2,ESP_ERR_NO_MEM);
    }
    s_radio_busy=0;
    atomic_flag_clear(&s_radio_starting);
}
int hal_esp32_radios_pending(void)
{
    return !s_radios_started || s_radio_busy || s_ble_cleanup_pending ||
        ((s_wifi_enabled || s_wifi_on_demand) && (!s_wifi_initialized || !s_wifi_worker_ready ||
          s_wifi_start_pending)) ||
        (s_wifi_enabled && !weather_service_ready()) ||
        (s_ble_enabled && (!s_ble_initialized || !s_ble_host_task || !s_ble_ready || !s_ble_control_clock_ready));
}
void hal_esp32_radio_report(void)
{
    printf("NOSRADIO v=1 pending=%d wifi_requested=%d wifi_ready=%d wifi_worker=%d wifi_start_pending=%d wifi_stage=%d wifi_error=%d wifi_attempts=%u wifi_failures=%u wifi_deferred=%u weather_ready=%d weather_stage=%d weather_error=%d weather_attempts=%u weather_failures=%u weather_deferred=%u ble_requested=%d ble_initialized=%d ble_ready=%d ble_stage=%d ble_error=%d ble_attempts=%u ble_failures=%u ble_deferred=%u\n",
        hal_esp32_radios_pending(),atomic_load(&s_wifi_enabled),atomic_load(&s_wifi_initialized),atomic_load(&s_wifi_worker_ready),
        atomic_load(&s_wifi_start_pending),atomic_load(&s_wifi_startup.stage),atomic_load(&s_wifi_startup.error),
        atomic_load(&s_wifi_startup.attempts),atomic_load(&s_wifi_startup.failures),atomic_load(&s_wifi_startup.deferred),
        weather_service_ready(),atomic_load(&s_weather_startup.stage),atomic_load(&s_weather_startup.error),
        atomic_load(&s_weather_startup.attempts),atomic_load(&s_weather_startup.failures),atomic_load(&s_weather_startup.deferred),
        atomic_load(&s_ble_enabled),atomic_load(&s_ble_initialized),atomic_load(&s_ble_ready),
        atomic_load(&s_ble_startup.stage),atomic_load(&s_ble_startup.error),atomic_load(&s_ble_startup.attempts),
        atomic_load(&s_ble_startup.failures),atomic_load(&s_ble_startup.deferred));
}

/* One private asset stream. UI transfers unpublished destination pages only. */
#include "asset_job.h"
static asset_job s_asset_job;
static TaskHandle_t s_asset_task;
static uint32_t s_asset_retry_ms, s_asset_create_failures, s_asset_admission_skips;
static void *asset_open(const char *path) {
    if (!board_sd_asset_acquire())
        return NULL;
    board_sd_asset_lock();
    FILE *file = fopen(path, "rb");
    board_sd_asset_unlock();
    if (!file)
        board_sd_asset_release();
    return file;
}
static size_t asset_read(void *file, void *destination, size_t count) {
    board_sd_asset_lock();
    size_t got = fread(destination, 1, count, file);
    board_sd_asset_unlock();
    return got;
}
static void asset_close(void *file) {
    board_sd_asset_lock();
    fclose(file);
    board_sd_asset_unlock();
    board_sd_asset_release();
}
static const asset_io s_asset_io = {asset_open, asset_read, asset_close, hal_micros};
static void asset_task(void *unused) {
    (void)unused;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (asset_worker_step(&s_asset_job, &s_asset_io)) {
            if (atomic_load_explicit(&s_asset_job.state, memory_order_acquire) != ASSET_QUEUED)
                break;
            vTaskDelay(1); /* one bounded-size SD call before scheduler opportunity */
        }
    }
}
/* External stack follows existing IMU writer policy. CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM
 * (legacy alias CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY) must remain enabled.
 * PSRAM is inaccessible with flash cache disabled: qualify this new stdio/error
 * call chain on hardware; allocation support alone is not a safety proof. */
static int asset_wake(void) {
    if (s_asset_task)
        return 1;
    uint32_t now = hal_millis();
    if ((int32_t)(now - s_asset_retry_ms) < 0)
        return 0;
    s_asset_retry_ms = now + 1000;
    const unsigned bytes = 6144;
    if (nos_mem_available() < bytes + 8u ||
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < sizeof(StaticTask_t) + 8192u) {
        s_asset_admission_skips++;
        return 0;
    }
    StackType_t *stack = nos_mem_alloc(bytes, "asset.worker_stack");
    if (!stack) {
        s_asset_create_failures++;
        return 0;
    }
    StaticTask_t *tcb = heap_caps_malloc(sizeof *tcb, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!tcb) {
        s_asset_create_failures++;
        nos_mem_free(stack);
        return 0;
    }
    asset_init(&s_asset_job);
    s_asset_task = xTaskCreateStaticPinnedToCore(asset_task, "asset-read", bytes, NULL, 1, stack,
                                                 tcb, tskNO_AFFINITY);
    if (!s_asset_task) {
        s_asset_create_failures++;
        free(tcb);
        nos_mem_free(stack);
        return 0;
    }
    return 1;
}
int hal_asset_submit(const asset_request *request) {
    if (!asset_wake())
        return -1;
    int accepted = asset_submit(&s_asset_job, request);
    if (accepted)
        xTaskNotifyGive(s_asset_task);
    return accepted;
}
void hal_asset_cancel(void) {
    if (s_asset_task) {
        asset_cancel(&s_asset_job);
        xTaskNotifyGive(s_asset_task);
    }
}
int hal_asset_take(asset_request *request, asset_result *result) {
    return s_asset_task ? asset_take(&s_asset_job, request, result) : 0;
}
int hal_asset_busy(void) {
    return s_asset_task &&
           atomic_load_explicit(&s_asset_job.state, memory_order_acquire) != ASSET_IDLE;
}
void hal_asset_poll(void) {}
void hal_asset_wait(void) {
    if (!s_asset_task)
        return;
    int state;
    while ((state = atomic_load_explicit(&s_asset_job.state, memory_order_acquire)) != ASSET_IDLE &&
           state != ASSET_DONE)
        vTaskDelay(1);
}

void hal_asset_get_diagnostics(asset_diagnostics *out) {
    memset(out, 0, sizeof *out);
    out->worker = s_asset_task != NULL;
    out->create_failures = s_asset_create_failures;
    out->admission_skips = s_asset_admission_skips;
    if (s_asset_task) {
        out->state = (uint32_t)atomic_load_explicit(&s_asset_job.state, memory_order_acquire);
        if (out->state != ASSET_IDLE)
            out->bytes_owned = s_asset_job.request.bytes;
        out->stack_min_free = (uint32_t)uxTaskGetStackHighWaterMark(s_asset_task);
    }
}
