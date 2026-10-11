/* hal_esp32.c — hal.h implementation for the ESP32-S3-Touch-AMOLED-1.43.
 * UNTESTED until the board arrives: compiles, but expect to tweak. */
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
#include "esp_http_server.h"
#include "esp_sntp.h"
#include <stdlib.h>   /* setenv */
#include <limits.h>
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
#include "svc.h"      /* hal_phone_* contract (nos/svc.h) */

/* Advertised + GAP name. Gadgetbridge auto-detects a Bangle.js by a name matching
 * "Bangle\.js.*" (BangleJSCoordinator) and scan-filters on the NUS service UUID. */
#define BLE_NAME "Bangle.js NOS"

void display_set_brightness(uint8_t level); /* main.c, DCS 0x51 */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static volatile int g_synced;

static uint32_t s_frozen_ms;
static int      s_freeze;

static uint32_t millis_raw(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
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

void hal_set_datetime(const hal_datetime *dt)
{
    if (!board_datetime_valid(dt)) { hal_log("Invalid date/time; not changed"); return; }
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
    struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    board_clock_changed(tv.tv_sec);
    g_synced = 1;
}

/* ---- persistent blob store (NVS) ---- */
int hal_store_save_checked(const char *key, const void *data, int size)
{
    if (!key || !data || size <= 0) return 0;
    nvs_handle_t h;
    if (nvs_open("wos", NVS_READWRITE, &h) != ESP_OK) return 0;
    esp_err_t err=nvs_set_blob(h, key, data, (size_t)size);
    if (err==ESP_OK) err=nvs_commit(h);
    nvs_close(h);
    return err==ESP_OK;
}
void hal_store_save(const char *key, const void *data, int size)
{ (void)hal_store_save_checked(key,data,size); }
int hal_store_load(const char *key, void *data, int size)
{
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
static hal_power s_power={.battery_percent=50,.external_power=-1,.age_ms=UINT32_MAX};
static uint32_t s_power_ms;
static int s_power_seen;
static int s_battery_stable, s_high_stable, s_smooth_mv;

void hal_esp32_sample_power(void)
{
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
            p.external_power=1; s_battery_stable=0;
        } else if(s_cali && measured>=3300 && measured<=4250 &&
                  s_smooth_mv>=3300 && s_smooth_mv<=4250 &&
                  samples[count-1]-samples[0]<=120) {
            if(s_battery_stable<10) s_battery_stable++;
            if(s_battery_stable>=10) {
                p.external_power=0; p.battery_valid=1;
                int percent=(s_smooth_mv-3300)/9;
                p.battery_percent=percent<0 ? 0 : percent>100 ? 100 : percent;
            }
        } else s_battery_stable=0;
    } else { s_battery_stable=0; s_high_stable=0; }
    p.age_ms=0;
    portENTER_CRITICAL(&s_power_lock);
    s_power=p; s_power_ms=millis_raw(); s_power_seen=1;
    portEXIT_CRITICAL(&s_power_lock);
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
int hal_battery_percent(void) { hal_power p; hal_power_get(&p); return p.battery_percent; }

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

/* ---- passive BLE scan (NimBLE observer; white-hat, observe-only) --------
 * A continuous PASSIVE discovery runs in the background on the NimBLE host
 * task from the moment the controller syncs. Each advertisement report is
 * folded into a small de-duplicated table (keyed by address) so hal_ble_scan
 * can copy the latest snapshot out instantly — the BLE app never blocks on
 * the radio. Strictly observe-only: passive scan, never advertises, connects,
 * pairs or spoofs. */
#define BLE_TABLE_MAX 24
static hal_ble_dev     s_ble_tab[BLE_TABLE_MAX];
static int             s_ble_count;
static SemaphoreHandle_t s_ble_mtx;
static uint8_t         s_ble_addr_type;

/* fold one advertisement report into the table (called on the host task) */
static void ble_record(const struct ble_gap_disc_desc *d)
{
    if (!s_ble_mtx) return;

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
        if (s_ble_count >= BLE_TABLE_MAX) { xSemaphoreGive(s_ble_mtx); return; }
        idx = s_ble_count++;
        s_ble_tab[idx].name[0] = 0;
        snprintf(s_ble_tab[idx].addr, sizeof s_ble_tab[idx].addr, "%s", addr);
    }
    s_ble_tab[idx].rssi = d->rssi;
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
        ble_start_scan();   /* duration is FOREVER, but re-arm defensively */
        return 0;
    default:
        return 0;
    }
}

static void ble_start_scan(void)
{
    struct ble_gap_disc_params p = { 0 };
    p.passive = 1;             /* PASSIVE: listen only, no scan requests    */
    p.filter_duplicates = 0;   /* keep folding in reports to refresh RSSI   */
    /* LOW DUTY CYCLE (units of 0.625ms): scan ~10ms out of every 160ms so the
     * radio is free the other ~94% of the time to TRANSMIT our own adverts and
     * share the 2.4GHz band with Wi-Fi. A continuous scan (itvl==window) starves
     * advertising and makes the watch invisible/unconnectable. */
    p.itvl   = 256;            /* 160 ms between scan windows */
    p.window = 16;             /* 10 ms listening per window  */
    ble_gap_disc(s_ble_addr_type, BLE_HS_FOREVER, &p, ble_gap_event, NULL);
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
 * stored in one PSRAM buffer, blitted by app_music via hal_media_cover(). ---- */
#define COVER_PX 250
static const ble_uuid128_t MEDIA_COVER_CHR = BLE_UUID128_INIT(
    0x23, 0xd1, 0xbc, 0xea, 0x5f, 0x78, 0x23, 0x15, 0xde, 0xef, 0x12, 0x12, 0x05, 0x00, 0xf0, 0xe3);
static uint16_t   *g_cover;
static volatile int g_cover_ready;
static volatile unsigned g_cover_gen;
const uint16_t *hal_media_cover(int *px, unsigned *gen)
{
    if (px) *px = COVER_PX;
    if (gen) *gen = g_cover_gen;
    return g_cover_ready ? g_cover : NULL;
}
/* Chunk write: [offset:4 LE][RGB565 data]. offset 0xFFFFFFFF => commit (image complete). */
static int media_cover_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    uint8_t buf[540]; uint16_t got = 0;
    ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &got);
    if (got < 4) return 0;
    uint32_t off; memcpy(&off, buf, 4);
    size_t need = (size_t)COVER_PX * COVER_PX * 2;
    if (off == 0xFFFFFFFFu) {
        g_cover_ready = 1; g_cover_gen++;
        printf("COVER(ble): commit gen=%u\n", g_cover_gen);
        return 0;
    }
    if (!g_cover) g_cover = hal_big_alloc(need);
    if (!g_cover) return 0;
    int dn = (int)got - 4;
    if ((size_t)off + (size_t)dn <= need) memcpy((uint8_t *)g_cover + off, buf + 4, (size_t)dn);
    return 0;
}

/* connectivity + IP-derived location status, surfaced to the control panel via hal_status/location */
static volatile int s_wifi_want;       /* (also defined near the Wi-Fi code; tentative def) */
static volatile int g_wifi_up;         /* got a DHCP IP */
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
    case HAL_FEAT_BLE:  return (g_med_conn != BLE_HS_CONN_HANDLE_NONE) ? HAL_CONN_UP : HAL_CONN_DOWN;
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
    if (!s_nus_asm) s_nus_asm = heap_caps_malloc(NUS_LINE_MAX, MALLOC_CAP_SPIRAM);
    if (!s_nus_ring) s_nus_ring = heap_caps_malloc(NUS_RING, MALLOC_CAP_SPIRAM);
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
    return g_nus_sub && g_med_conn != BLE_HS_CONN_HANDLE_NONE;
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
void hal_phone_write_line(const char *line)
{
    if (!line || !hal_phone_connected() || !g_nus_tx_h) return;
    uint16_t conn = g_med_conn;
    int len = (int)strlen(line), total = len + 1;       /* + '\n' */
    int chunk = g_nus_mtu > 23 ? g_nus_mtu - 3 : 20;
    if (chunk > 180) chunk = 180;
    uint8_t buf[180];
    for (int off = 0; off < total; off += chunk) {
        int k = total - off < chunk ? total - off : chunk;
        for (int i = 0; i < k; i++) buf[i] = off + i < len ? (uint8_t)line[off + i] : '\n';
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, (uint16_t)k);
        if (!om || ble_gatts_notify_custom(conn, g_nus_tx_h, om) != 0) {
            s_nus_dropped++;
            break;                                       /* out of mbufs: drop the rest */
        }
    }
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

static const struct ble_gatt_svc_def WATCH_GATT[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &WATCH_SVC.u, .characteristics = WATCH_CHRS },
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &NUS_SVC.u, .characteristics = NUS_CHRS },
    { 0 },
};

/* last-pairing outcome, queryable any time via the `bleinfo` serial command */
static volatile int g_ble_conns, g_ble_encst = -2, g_ble_bonded, g_ble_disc = -1;
void hal_ble_status(char *buf, int n)
{
    snprintf(buf, n, "conns=%d enc_status=%d bonded=%d last_disc=0x%x",
             g_ble_conns, g_ble_encst, g_ble_bonded, g_ble_disc & 0xff);
}

static int peri_gap_event(struct ble_gap_event *ev, void *arg);
static void adv_start(void)
{
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)BLE_NAME; f.name_len = sizeof BLE_NAME - 1; f.name_is_complete = 1;
    ble_gap_adv_set_fields(&f);
    /* scan response: the NUS UUID, which Gadgetbridge's Bangle.js scan filter looks for */
    struct ble_hs_adv_fields r = { 0 };
    r.uuids128 = &NUS_SVC; r.num_uuids128 = 1; r.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&r);
    struct ble_gap_adv_params ap = { 0 };
    ap.conn_mode = BLE_GAP_CONN_MODE_UND;
    ap.disc_mode = BLE_GAP_DISC_MODE_GEN;
    ble_gap_adv_start(s_ble_addr_type, NULL, BLE_HS_FOREVER, &ap, peri_gap_event, NULL);
}
static int peri_gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc d;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        wlog("BLE connect (st=%d)", ev->connect.status);
        if (ev->connect.status != 0) { adv_start(); break; }
        g_ble_conns++;
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
        wlog("BLE disconnect (0x%x)", ev->disconnect.reason);
        g_ble_disc = ev->disconnect.reason;
        g_med_conn = BLE_HS_CONN_HANDLE_NONE;
        g_med_have = 0;                          /* now-playing is stale once the bridge drops */
        g_nus_sub = 0; g_nus_mtu = 23;           /* Gadgetbridge link gone */
        s_nus_asm_n = 0; s_nus_asm_drop = 0;
        adv_start();
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
            wlog("NUS %s (Gadgetbridge)", g_nus_sub ? "subscribed" : "unsubscribed");
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        break;
    default:
        printf("BLE: gap event type=%d\n", ev->type);
        break;
    }
    return 0;
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
    ble_start_scan();
    adv_start();
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();                 /* runs until nimble_port_stop() */
    nimble_port_freertos_deinit();
}

static void ble_init(void)
{
    s_ble_mtx = xSemaphoreCreateMutex();
    esp_err_t e = nimble_port_init();   /* brings up the BT controller + host */
    if (e != ESP_OK) {
        ESP_LOGE("watch", "nimble_port_init failed: %d", (int)e);
        return;
    }
    nus_alloc();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(WATCH_GATT);
    ble_gatts_add_svcs(WATCH_GATT);
    printf("BLE: GATT registered (media_cmd handle=%d)\n", g_med_cmd_h);
    ble_svc_gap_device_name_set(BLE_NAME);
    extern void ble_store_config_init(void);
    ble_store_config_init();
    /* NO bonding: the media chars are unencrypted, so the host never persists this device
     * and can't auto-reconnect in the background to fight the bridge. Just a clean, fresh
     * connectable peripheral each time. */
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_sc = 0;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
}

int hal_ble_scan(hal_ble_dev *out, int max)
{
    if (!s_ble_mtx || max <= 0) return 0;
    xSemaphoreTake(s_ble_mtx, portMAX_DELAY);
    int n = s_ble_count < max ? s_ble_count : max;
    for (int i = 0; i < n; i++) out[i] = s_ble_tab[i];
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
static hal_wifi_ap  s_aps[WSCAN_MAX];
static int          s_ap_n;
static volatile int s_scan_busy;
static uint32_t     s_scan_done_ms, s_scan_start_ms;

static void wifi_scan_collect(void)
{
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    static wifi_ap_record_t recs[WSCAN_MAX];
    uint16_t got = n < WSCAN_MAX ? n : WSCAN_MAX;
    if (got) esp_wifi_scan_get_ap_records(&got, recs);
    else     { esp_wifi_clear_ap_list(); got = 0; }
    int m = 0;
    for (int i = 0; i < got; i++) {
        const char *ss = (const char *)recs[i].ssid;
        snprintf(s_aps[m].ssid, sizeof s_aps[m].ssid, "%s", ss[0] ? ss : "(hidden)");
        s_aps[m].rssi = recs[i].rssi;
        s_aps[m].chan = recs[i].primary;
        m++;
    }
    s_ap_n = m;
    s_scan_busy = 0;
    s_scan_done_ms = hal_millis();
    wlog("wifi scan: %d AP%s", m, m == 1 ? "" : "s");
}

int hal_wifi_scan(hal_wifi_ap *out, int max)
{
    uint32_t now = hal_millis();
    if (!s_scan_busy && (s_scan_done_ms == 0 || now - s_scan_done_ms > 4000)) {
        wifi_scan_config_t cfg = {
            .show_hidden = true,                /* hidden SSIDs are still signals worth showing */
            .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
            /* dwell longer per channel than the stock ~120ms -> hears weak / distant APs */
            .scan_time.active = { .min = 100, .max = 250 },
        };
        esp_err_t rc = esp_wifi_scan_start(&cfg, false);      /* async — never blocks the UI */
        if (rc == ESP_OK) { s_scan_busy = 1; s_scan_start_ms = now; }
        else              wlog("wifi scan start: %s", esp_err_to_name(rc));
    }
    if (s_scan_busy && now - s_scan_start_ms > 9000) {        /* watchdog: don't wedge forever */
        esp_wifi_scan_stop();
        s_scan_busy = 0;
        wlog("wifi scan timed out");
    }
    int m = s_ap_n < max ? s_ap_n : max;
    for (int i = 0; i < m; i++) out[i] = s_aps[i];
    return m;
}

/* Wi-Fi station connect (own-device: joins the user's own network). Once associated it
 * prints its DHCP IP over stdout (USB-serial) as "WIFI_IP: <ip>" so the host can find the
 * board's config web page. Auto-reconnects while a connect is desired. */
static volatile int s_wifi_want;

/* ---- config web server (served on the watch's own IP once on the network) ---- */
static httpd_handle_t s_httpd;

static esp_err_t web_root(httpd_req_t *req)
{
    static const char *html =
        "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>Watch</title><style>body{font-family:system-ui,sans-serif;background:#0d0d10;"
        "color:#eee;max-width:440px;margin:2em auto;padding:0 1.2em}h2{color:#5cd}"
        "input,button{font-size:1em;padding:.65em;width:100%;box-sizing:border-box;margin:.35em 0;"
        "border-radius:10px;border:1px solid #333;background:#1a1a20;color:#eee}"
        "button{background:#5cd;color:#000;font-weight:700;border:0}</style>"
        "<h2>&#8986; Watch setup</h2><p>The watch is on your network. Set a Wi-Fi network for it "
        "to join:</p><form method=POST action=/wifi>"
        "<input name=ssid placeholder='Network name (SSID)' autofocus>"
        "<input name=pass type=password placeholder='Password'>"
        "<button>Save &amp; connect</button></form>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}
static esp_err_t web_wifi(httpd_req_t *req)
{
    char buf[192];
    int n = httpd_req_recv(req, buf, sizeof buf - 1);
    if (n <= 0) return ESP_FAIL;
    buf[n] = 0;
    char ssid[40] = { 0 }, pass[68] = { 0 };
    httpd_query_key_value(buf, "ssid", ssid, sizeof ssid);
    httpd_query_key_value(buf, "pass", pass, sizeof pass);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<meta name=viewport content='width=device-width'>"
                            "<body style='font-family:sans-serif;background:#0d0d10;color:#eee'>"
                            "Saved &mdash; the watch is switching networks.");
    if (ssid[0]) hal_wifi_connect(ssid, pass);   /* after replying (this closes our socket) */
    return ESP_OK;
}
/* Wi-Fi alternative to the BLE MEDIA_COVER char: POST raw RGB565 (COVER_PX^2) to /cover. */
static esp_err_t web_cover(httpd_req_t *req)
{
    size_t need = (size_t)COVER_PX * COVER_PX * 2;
    if (!g_cover) g_cover = hal_big_alloc(need);
    if (!g_cover) { printf("COVER: PSRAM alloc failed\n");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no psram"); return ESP_FAIL; }
    int total = req->content_len, got = 0;
    uint8_t *dst = (uint8_t *)g_cover;
    while (got < total && (size_t)got < need) {
        int r = httpd_req_recv(req, (char *)dst + got, (size_t)(total - got));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) break;
        got += r;
    }
    g_cover_ready = ((size_t)got == need);
    if (g_cover_ready) g_cover_gen++;
    printf("COVER: got %d/%d bytes -> ready=%d gen=%u\n", got, total, g_cover_ready, g_cover_gen);
    httpd_resp_sendstr(req, g_cover_ready ? "ok" : "short");
    return ESP_OK;
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
    cfg.recv_wait_timeout = 12;              /* cover POST is ~160KB; allow slow chunks */
    cfg.max_uri_handlers = 12;               /* 6 here + 3 OTA (ota.c) */
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) { s_httpd = NULL; return; }
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

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        wifi_scan_collect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        g_wifi_up = 0;
        wifi_event_sta_disconnected_t *ev=data;
        if (s_wifi_want) { printf("WIFI: disconnected reason=%u, retrying\n",ev->reason); esp_wifi_connect(); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        g_wifi_up = 1;
        wlog("WiFi IP " IPSTR, IP2STR(&ev->ip_info.ip));
        web_start();
        ntp_start();       /* auto time sync over the internet */
        hal_weather_refresh();
    }
}

/* Persisted STA credentials (NVS) so the watch rejoins its network on boot. */
typedef struct { char ssid[33]; char pass[65]; } wifi_cred_t;

void hal_wifi_connect(const char *ssid, const char *pass)
{
    if(!ssid || !ssid[0] || strlen(ssid)>32 || !pass || strlen(pass)<8 || strlen(pass)>63) {
        printf("ERR Wi-Fi requires SSID 1..32 bytes, WPA password 8..63 bytes\n"); return;
    }
    wifi_config_t wc = { 0 };
    memcpy(wc.sta.ssid,ssid,strlen(ssid));
    memcpy(wc.sta.password,pass,strlen(pass));
    wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;   /* accept WPA2/WPA3-Personal */
    s_wifi_want = 0;
    esp_wifi_disconnect();
    esp_err_t rc=esp_wifi_set_config(WIFI_IF_STA, &wc);
    if(rc!=ESP_OK) {printf("ERR Wi-Fi config %s\n",esp_err_to_name(rc));return;}
    s_wifi_want = 1;
    printf("WIFI: connecting to \"%s\"...\n", ssid ? ssid : "");
    esp_wifi_connect();

    /* remember this network for auto-reconnect on next boot */
    if (ssid && ssid[0]) {
        wifi_cred_t c = { 0 };
        snprintf(c.ssid, sizeof c.ssid, "%s", ssid);
        snprintf(c.pass, sizeof c.pass, "%s", pass ? pass : "");
        hal_store_save("wifi_cred", &c, sizeof c);
    }
}

/* ---- Wi-Fi air analyzer: promiscuous monitor mode + channel hopping --------------------
 * OBSERVE-ONLY. We register an RX callback and count frames; we never transmit. Keep the
 * callback tiny — it runs in the Wi-Fi task on every single frame in the air. */
#define AIR_DWELL_MS 120

static hal_air_chan s_air_view[HAL_AIR_NCH];
static uint8_t      s_air_hist[HAL_AIR_NCH][HAL_AIR_HIST];
static struct { uint32_t pkts, bytes; int8_t rssi_max; uint16_t mgmt, data, ctrl; }
                    s_air_acc[HAL_AIR_NCH];
static volatile int s_air_on;
static volatile int s_air_ch = 1;
static uint32_t     s_air_last_read;
static int          s_air_want_sta;
static esp_timer_handle_t s_air_timer;

static void air_clear_acc(int i)
{
    memset(&s_air_acc[i], 0, sizeof s_air_acc[i]);
    s_air_acc[i].rssi_max = -128;
}

static void air_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    int i = s_air_ch - 1;
    if (i < 0 || i >= HAL_AIR_NCH || !p) return;
    s_air_acc[i].pkts++;
    s_air_acc[i].bytes += (uint32_t)p->rx_ctrl.sig_len;
    if (p->rx_ctrl.rssi > s_air_acc[i].rssi_max) s_air_acc[i].rssi_max = p->rx_ctrl.rssi;
    if      (type == WIFI_PKT_MGMT) s_air_acc[i].mgmt++;
    else if (type == WIFI_PKT_DATA) s_air_acc[i].data++;
    else if (type == WIFI_PKT_CTRL) s_air_acc[i].ctrl++;
}

static void air_shutdown(void)
{
    s_air_on = 0;
    if (s_air_timer) esp_timer_stop(s_air_timer);
    esp_wifi_set_promiscuous(false);
    wlog("air: monitor off");
    if (s_air_want_sta) { s_wifi_want = 1; esp_wifi_connect(); }   /* give the STA link back */
}

/* Fires every dwell: fold the channel we just listened to into the smoothed view + heat-map
 * history, then hop to the next one. */
static void air_hop(void *arg)
{
    (void)arg;
    if (!s_air_on) return;
    if (hal_millis() - s_air_last_read > 3000) { air_shutdown(); return; }  /* auto-release */

    int i = s_air_ch - 1;
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

    s_air_ch = (s_air_ch % HAL_AIR_NCH) + 1;
    esp_wifi_set_channel((uint8_t)s_air_ch, WIFI_SECOND_CHAN_NONE);
}

int hal_wifi_air_running(void) { return s_air_on; }
int hal_wifi_air_channel(void) { return s_air_ch; }

int hal_wifi_air_start(void)
{
    s_air_last_read = hal_millis();
    if (s_air_on) return 1;
    memset(s_air_view, 0, sizeof s_air_view);
    memset(s_air_hist, 0, sizeof s_air_hist);
    for (int i = 0; i < HAL_AIR_NCH; i++) air_clear_acc(i);

    /* Hopping channels would break an association anyway, so park the station link. */
    s_air_want_sta = s_wifi_want;
    s_wifi_want = 0;
    esp_wifi_disconnect();

    wifi_promiscuous_filter_t f = { .filter_mask = WIFI_PROMIS_FILTER_MASK_ALL };
    esp_wifi_set_promiscuous_filter(&f);
    esp_wifi_set_promiscuous_rx_cb(air_rx);
    esp_err_t rc = esp_wifi_set_promiscuous(true);
    if (rc != ESP_OK) {
        wlog("air: promisc %s", esp_err_to_name(rc));
        s_wifi_want = s_air_want_sta;
        if (s_wifi_want) esp_wifi_connect();
        return 0;
    }
    s_air_ch = 1;
    esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
    if (!s_air_timer) {
        const esp_timer_create_args_t a = { .callback = air_hop, .name = "airhop" };
        esp_timer_create(&a, &s_air_timer);
    }
    esp_timer_start_periodic(s_air_timer, AIR_DWELL_MS * 1000);
    s_air_on = 1;
    wlog("air: monitor on, ch1-%d", HAL_AIR_NCH);
    return 1;
}

void hal_wifi_air_stop(void) { if (s_air_on) air_shutdown(); }

void hal_wifi_air_get(hal_air_chan *out)
{
    s_air_last_read = hal_millis();          /* also the keep-alive for the auto-release */
    memcpy(out, s_air_view, sizeof s_air_view);
}

void hal_wifi_air_hist(uint8_t *out)
{
    s_air_last_read = hal_millis();
    memcpy(out, s_air_hist, sizeof s_air_hist);
}

/* Wi-Fi in AP+STA: STA scans/joins networks, AP "Watch-Setup" hosts the config page for
 * first-run setup (join it -> browse http://192.168.4.1). Connect happens via hal_wifi_connect. */
static void wifi_init(void)
{
    if (esp_netif_init() != ESP_OK) return;
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return;   /* tolerate re-init */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&cfg) != ESP_OK) return;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    /* The stock country policy caps scanning at channels 1-11; India/EU and much of the world
     * also use 12-13, so APs parked up there were simply invisible to the scanner. */
    wifi_country_t ctry = { .cc = "IN", .schan = 1, .nchan = 13,
                            .policy = WIFI_COUNTRY_POLICY_MANUAL };
    esp_wifi_set_country(&ctry);

    wifi_cred_t c = { 0 };
    int have = hal_store_load("wifi_cred", &c, sizeof c) && c.ssid[0];

    if (have) {
        /* Known network -> STA-only. A constantly-beaconing SoftAP hogs the shared
         * 2.4GHz radio and hurts BLE; once configured we don't need it. Reconfigure
         * later via the watch's web page on the LAN. */
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_start();
        printf("WIFI: auto-reconnect to saved \"%s\"\n", c.ssid);
        hal_wifi_connect(c.ssid, c.pass);   /* wifi_evt retries on any drop */
    } else {
        /* First run: host the "Watch-Setup" hotspot so the user can pick a network. */
        esp_netif_create_default_wifi_ap();
        esp_wifi_set_mode(WIFI_MODE_APSTA);
        wifi_config_t ap = { 0 };
        const char *apssid = "Watch-Setup";
        memcpy(ap.ap.ssid, apssid, strlen(apssid));
        ap.ap.ssid_len = strlen(apssid);
        ap.ap.channel = 1;
        ap.ap.max_connection = 4;
        ap.ap.authmode = WIFI_AUTH_OPEN;
        esp_wifi_set_config(WIFI_IF_AP, &ap);
        esp_wifi_start();
        web_start();
        printf("WIFI: setup hotspot \"Watch-Setup\" -> http://192.168.4.1\n");
    }
}

void hal_esp32_init_periph(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nv = nvs_flash_init();
    }
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
    extern void weather_service_start(void);
    weather_service_start();
    wifi_init();
    ble_init();
}
