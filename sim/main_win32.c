/* main_win32.c — PC simulator host. Implements hal.h with Win32 and blits
 * the RGB565 framebuffer into a window at 2x scale with a circular mask,
 * so what you see matches the round panel. Mouse = touch. ESC quits. */
#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../core/hal.h"
#include "../core/wos.h"
#include "../core/flat60.h"
#include "../core/notif.h"
#include "../core/settings.h"
#include "../core/font.h"
#include "../core/apps/apps.h"
#include "../core/pip.h"
#include "../core/wake_service.h"

#define DEV_SCALE 1               /* 466px native is already a comfy dev window */
#define PANEL_DIAMETER_IN 1.43f   /* physical size of the round AMOLED */

static uint16_t s_fb[GFX_W * GFX_H];
/* Headless regression can exercise the device's real 16-row stripe geometry. */
static uint16_t s_test_stripes[2][GFX_W*16];
static void test_stripe_flush(uint16_t *pixels,int stride,int x0,int y0,int width,int rows)
{
    assert(x0>=0 && y0>=0 && x0+width<=GFX_W && y0+rows<=GFX_H && width*rows<=GFX_W*16);
    assert(pixels>=s_test_stripes[0] && pixels+(rows-1)*stride+width<=s_test_stripes[1]+GFX_W*16);
    for(int r=0;r<rows;r++)
        memcpy(&s_fb[(y0+r)*GFX_W+x0],&pixels[r*stride],(size_t)width*2);
}
static uint16_t s_disp[GFX_W * GFX_H];
static uint8_t  s_mask[GFX_W * GFX_H]; /* circular panel coverage */
static HWND s_hwnd;
static LARGE_INTEGER s_qpf, s_t0;
static int s_batt = 78;
static int s_client = GFX_W * DEV_SCALE; /* current square client size */
static int s_real_px;                    /* client size for 1:1 physical scale */
static int s_real_mode;
static int s_tear_sim = 0;   /* device-tear simulation (models the NON-atomic flush;
                              * the list is on the atomic tear-free path now), toggle 'T' */
static int s_fps30 = 0;      /* cap the sim to 30fps to preview the watch cadence ('F') */
static int s_overlay = 1;    /* debug dirty-rect overlay + readout, toggle with 'I' */
static float s_ppi;                      /* of the monitor under the window */
static float s_ppi_override;             /* --ppi N, wins when > 0 */

/* ---- HAL ---- */
static uint32_t s_frozen_ms;
static int s_pip_preview;
static int s_preview_clock_minutes=14*60+23;
static uint32_t s_preview_ms=1000;
static int      s_freeze;

static uint32_t millis_raw(void)
{
    if(s_pip_preview) return s_preview_ms;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint32_t)((t.QuadPart - s_t0.QuadPart) * 1000 / s_qpf.QuadPart);
}

static hal_datetime s_frozen_dt;

uint32_t hal_millis(void)
{
    return s_freeze ? s_frozen_ms : millis_raw();
}

uint32_t hal_micros(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint32_t)((t.QuadPart - s_t0.QuadPart) * 1000000 / s_qpf.QuadPart);
}

static void get_datetime_raw(hal_datetime *out)
{
    if(s_pip_preview) {
        *out=(hal_datetime){2026,9,12,6,s_preview_clock_minutes/60,s_preview_clock_minutes%60,
            (int)(s_preview_ms/1000%60),(int)(s_preview_ms%1000)};
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    out->year = st.wYear; out->month = st.wMonth; out->day = st.wDay;
    out->weekday = st.wDayOfWeek;
    out->hour = st.wHour; out->minute = st.wMinute; out->second = st.wSecond;
    out->millis = st.wMilliseconds;
}

void hal_frame_begin(void)
{
    s_frozen_ms = millis_raw();
    get_datetime_raw(&s_frozen_dt);
    s_freeze = 1;
}
void hal_frame_end(void) { s_freeze = 0; }

void hal_get_datetime(hal_datetime *out)
{
    if (s_freeze) { *out = s_frozen_dt; return; }
    get_datetime_raw(out);
}

int  hal_battery_percent(void) { return s_batt; }
int  hal_battery_mv(void) {                                /* sim: a rising charge ramp */
    int r = 3600 + (int)(hal_millis() / 100);
    return r > 4200 ? 4200 : r;
}
static int s_backlight, s_cfg_saves;
void hal_set_backlight(uint8_t level) { s_backlight=level; }
void hal_set_datetime(const hal_datetime *dt) { (void)dt; }   /* sim uses the PC clock */

void hal_store_save(const char *key, const void *data, int size)
{ (void)hal_store_save_checked(key,data,size); }
int hal_store_save_checked(const char *key, const void *data, int size)
{
    char path[128];
    snprintf(path, sizeof path, "store_%s.dat", key);
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok=fwrite(data,1,(size_t)size,f)==(size_t)size;
    if(fclose(f)!=0)ok=0;
    if(ok && !strcmp(key,"cfg"))s_cfg_saves++;
    return ok;
}
uint64_t hal_epoch_seconds(void)
{
    if(s_pip_preview)return 1789200000ull+s_preview_ms/1000;
    FILETIME ft;GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER v;v.LowPart=ft.dwLowDateTime;v.HighPart=ft.dwHighDateTime;
    return (v.QuadPart-116444736000000000ull)/10000000ull;
}
void hal_power_get(hal_power *out)
{ *out=(hal_power){3300+s_batt*9,s_batt,1,0,1,0}; }
#include "../tools/weather_preview.h"
void hal_activity_get(hal_activity *out)
{ *out=(hal_activity){(uint32_t)hal_step_count(),s_pip_preview?0:hal_millis()/1000,0,1}; }
int hal_notification_post(const char *app,const char *title)
{
    if(!app||!title||!*app||!*title||strlen(app)>19||strlen(title)>39)return 0;
    notif_push(app,title,GFX_ACCENT);return 1;
}
int hal_notification_poll(char *app,int an,char *title,int tn)
{ (void)app;(void)an;(void)title;(void)tn;return 0; }
int hal_store_load(const char *key, void *data, int size)
{
    char path[128];
    snprintf(path, sizeof path, "store_%s.dat", key);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = (int)fread(data, 1, (size_t)size, f);
    fclose(f);
    return n == size;
}
void hal_log(const char *msg) { printf("[watch] %s\n", msg); }

int hal_wifi_scan(hal_wifi_ap *out, int max)
{
    static const hal_wifi_ap fake[] = {
        { "HomeNet-5G",     -48, 36 }, { "TP-Link_A2F0",  -61,  6 },
        { "makerspace",     -67, 11 }, { "SKY9821",       -72,  1 },
        { "AndroidAP",      -79,  6 }, { "eduroam",       -83, 44 },
        { "Xfinity",        -88, 11 },
    };
    int n = (int)(sizeof fake / sizeof fake[0]);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = fake[i];
    return n;
}

void hal_wifi_connect(const char *ssid, const char *pass)   /* sim: no radio */
{
    (void)pass;
    printf("[sim] wifi connect -> %s\n", ssid ? ssid : "");
}
void hal_ble_status(char *buf, int n) { snprintf(buf, n, "sim: no radio"); }

int hal_media_get(char *title, int tn, char *artist, int an, int *playing)
{
    (void)title; (void)tn; (void)artist; (void)an; (void)playing;
    return 0;   /* no BLE bridge in the sim -> app_music shows its local mockup */
}
void hal_media_cmd(int cmd) { printf("[sim] media cmd %d\n", cmd); }
int hal_media_time(int *pos_sec, int *dur_sec) { (void)pos_sec; (void)dur_sec; return 0; }
void hal_media_seek(int pos_sec) { printf("[sim] media seek %d\n", pos_sec); }
void hal_media_vol(int pct) { printf("[sim] media vol %d%%\n", pct); }
/* fake a mix of states so the control-panel grid design shows every colour */
hal_conn hal_status(int feature)
{
    switch (feature) {
    case HAL_FEAT_BLE:  return HAL_CONN_UP;
    case HAL_FEAT_WIFI: return HAL_CONN_TRYING;
    case HAL_FEAT_NET:  return HAL_CONN_DOWN;
    case HAL_FEAT_SYNC: return HAL_CONN_UP;
    case HAL_FEAT_AI:   return HAL_CONN_OFF;
    case HAL_FEAT_GPS:  return HAL_CONN_UP;
    }
    return HAL_CONN_OFF;
}
int hal_location(char *city, int n, float *lat, float *lon)
{
    if (city && n > 0) { const char *c = "San Francisco"; int i = 0; for (; c[i] && i < n - 1; i++) city[i] = c[i]; city[i] = 0; }
    if (lat) *lat = 37.7749f;
    if (lon) *lon = -122.4194f;
    return 1;
}
void hal_set_aod(int on) { printf("[sim] AOD %s\n", on ? "on" : "off"); }
void hal_time_resync(void) { printf("[sim] time resync\n"); }
void hal_beep(int f, int ms) { printf("[sim] beep %dHz %dms\n", f, ms); }
void hal_play(int snd) { printf("[sim] play sound %d\n", snd); }
void hal_find_phone(void) { printf("[sim] find my phone -> ring companion\n"); }

/* Fake air analyzer: the classic real-world profile — the non-overlapping channels 1/6/11 carry
 * most traffic, with a little leakage either side and some drift so the heat map animates. */
static int      s_air_run, s_air_c = 1;
static uint8_t  s_air_h[HAL_AIR_NCH][HAL_AIR_HIST];
static uint32_t s_air_t;
int  hal_wifi_air_running(void) { return s_air_run; }
int  hal_wifi_air_channel(void) { return s_air_c; }
int  hal_wifi_air_start(void)   { s_air_run = 1; return 1; }
void hal_wifi_air_stop(void)    { s_air_run = 0; }
static uint8_t air_fake(int ch, double t)
{
    double base = 0;
    const int peaks[3] = { 1, 6, 11 };
    const double amp[3] = { 210, 165, 120 };
    for (int p = 0; p < 3; p++) {
        double d = ch - peaks[p];
        base += amp[p] * exp(-(d * d) / 2.2);
    }
    base *= 0.75 + 0.25 * sin(t * 0.8 + ch);
    base += 6;
    if (base < 0) base = 0;
    if (base > 255) base = 255;
    return (uint8_t)base;
}
void hal_wifi_air_get(hal_air_chan *out)
{
    double t = GetTickCount() / 1000.0;
    s_air_c = 1 + ((GetTickCount() / 120) % HAL_AIR_NCH);
    for (int i = 0; i < HAL_AIR_NCH; i++) {
        uint8_t u = air_fake(i + 1, t);
        out[i].util = u;
        out[i].pkts = (uint16_t)(u * 1.7);
        out[i].rssi_max = u ? (int8_t)(-92 + u / 6) : 0;
        out[i].mgmt = u / 5; out[i].data = u; out[i].ctrl = u / 8;
    }
    if (GetTickCount() - s_air_t > 300) {            /* scroll the history ring */
        s_air_t = GetTickCount();
        for (int i = 0; i < HAL_AIR_NCH; i++) {
            memmove(&s_air_h[i][0], &s_air_h[i][1], HAL_AIR_HIST - 1);
            s_air_h[i][HAL_AIR_HIST - 1] = air_fake(i + 1, t);
        }
    }
}
void hal_wifi_air_hist(uint8_t *out) { memcpy(out, s_air_h, sizeof s_air_h); }
int  hal_locate_active(void) { return 0; }
int hal_log_count(void) { return 7; }
const char *hal_log_get(int i)
{
    static const char *L[7] = {
        "boot ok", "WiFi IP 192.168.1.42", "NTP synced 14:30:02", "BLE connect (st=0)",
        "BLE time sync ok", "BLE loc: Bengaluru 1298,7759", "resync req: ble=up wifi=up",
    };
    return (i >= 0 && i < 7) ? L[i] : "";
}
void hal_log_clear(void) { printf("[sim] log clear\n"); }
void hal_batt_log_tick(void) {}
int hal_media_colors(uint16_t *ring, uint16_t *ab, uint16_t *sb, uint16_t *at, uint16_t *st)
{ (void)ring; (void)ab; (void)sb; (void)at; (void)st; return 0; }
const uint16_t *hal_media_cover(int *px, unsigned *gen) { (void)px; (void)gen; return 0; }
const uint16_t *hal_media_cover_acquire(int *px, unsigned *gen) { return hal_media_cover(px,gen); }
void hal_media_cover_release(const uint16_t *pixels) { (void)pixels; }

int hal_imu_sample(hal_imu *o,uint32_t *sample_ms)
{ *sample_ms=hal_millis();return hal_imu_read(o); }
int hal_imu_read(hal_imu *o)
{
    if(s_pip_preview){*o=(hal_imu){0,0,-1,0,0,0};return 1;}
    float t = hal_millis() / 1000.0f;   /* gentle simulated sway */
    o->ax = 0.35f * sinf(t * 1.3f);
    o->ay = 0.35f * sinf(t * 0.9f + 1.0f);
    o->az = -1.0f; /* same face-up sign observed on the B board */
    o->gx = o->gy = o->gz = 0;
    return 1;
}

int hal_audio_bars(uint8_t *lv, int n)
{
    float t = hal_millis() / 1000.0f;   /* lively fake spectrum */
    for (int i = 0; i < n; i++) {
        float v = 0.5f + 0.5f * sinf(t * 6.0f + i * 0.7f) * sinf(t * 2.3f + i);
        if (v < 0) v = 0;
        if (v > 1) v = 1;
        lv[i] = (uint8_t)(v * 255);
    }
    return n;
}

int hal_mic_level(void)
{
    /* demo envelope: quiet floor, a "music" hump every ~20s, and a sharp
     * "clap" spike every ~13s so Pip's startle/dance reactions are visible */
    float t = hal_millis() / 1000.0f;
    float music = fmodf(t, 20.0f) < 8.0f ? 70 + 30 * sinf(t * 5.0f) : 0;
    float clap = fmodf(t, 13.0f) < 0.25f ? 220 : 0;
    float v = 18 + music + clap;
    return v > 255 ? 255 : (int)v;
}

int hal_mic_pitch(void)
{
    /* demo pitch: sweeps 150-900Hz while the fake music plays, else silent */
    float t = hal_millis() / 1000.0f;
    if (fmodf(t, 20.0f) >= 8.0f) return 0;
    return 150 + (int)(375 * (1 + sinf(t * 0.9f)));
}

int hal_ble_scan(hal_ble_dev *out, int max)
{
    static const hal_ble_dev fake[] = {
        { "Galaxy Buds",   "5C:3A:45:1B:2E:0F", -52 },
        { "Mi Band 7",     "D8:1F:12:9A:44:70", -61 },
        { "JBL Flip 6",    "10:94:BB:5C:0A:22", -68 },
        { "AirPods Pro",   "A4:83:E7:2D:66:91", -74 },
        { "(unknown)",     "F1:22:8C:03:5D:AE", -83 },
        { "TV-Living",     "00:1A:7D:DA:71:13", -88 },
    };
    int n = (int)(sizeof fake / sizeof fake[0]);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = fake[i];
    return n;
}

int hal_step_count(void)
{
    /* simulated: ~1 step/sec ramp so the activity rings visibly fill */
    return 3800 + (int)(hal_millis() / 1000);
}

void hal_haptic(int ms) { (void)ms; }

void *hal_big_alloc(uint32_t bytes) { return malloc(bytes); }

void hal_storage_get(hal_storage *out)
{
    memset(out, 0, sizeof *out);
    snprintf(out->message, sizeof out->message, "Simulator: no SD card");
}
int hal_audio_available(void) { return 0; }

/* ---- window ---- */
/* DEBUG: outline this frame's dirty rects + list their sizes under the name, so
 * the dirty region can be tuned by eye. Red = a band taller than the ~150px
 * tear/60fps limit. Drawn onto s_disp (not s_fb) so it never affects damage. */
static void draw_debug_rects(void)
{
    extern int wos_dbg_rect[16][4], wos_dbg_nr;
    gfx_fb fb;
    fb.px = s_disp; fb.ox = 0; fb.oy = 0; fb.y_base = 0; fb.h = GFX_H;
    fb.cx0 = 0; fb.cx1 = GFX_W; fb.rec = 0; fb.stride = fb.x_base = 0;
    /* Just the box OUTLINES here (panel space, aligned with the icons). The size
     * readout is drawn as crisp native GDI text in present() so it can be tiny —
     * see draw_debug_text(). Red box = >150px tall (tear/60fps risk). */
    for (int i = 0; i < wos_dbg_nr && i < 16; i++) {
        int x0 = wos_dbg_rect[i][0], y0 = wos_dbg_rect[i][1];
        int w = wos_dbg_rect[i][2] - x0, h = wos_dbg_rect[i][3] - y0;
        if (w <= 0 || h <= 0) continue;
        uint16_t col = h >= GFX_H / 2 ? GFX_RGB(255, 60, 60) : GFX_RGB(0, 235, 120);
        gfx_fill_rect(&fb, x0, y0, w, 2, col);
        gfx_fill_rect(&fb, x0, y0 + h - 2, w, 2, col);
        gfx_fill_rect(&fb, x0, y0, 2, h, col);
        gfx_fill_rect(&fb, x0 + w - 2, y0, 2, h, col);
    }
}

/* Per-frame dirty-pixel graph tuning. BUDGET is the tear-free ceiling: a normal
 * scroll frame is the narrow icon column (~68k px); anything markedly above hints
 * at a wide (full-width) frame that risks the atomic blast falling behind the
 * scan. GRAPH_MAX sets the vertical scale (a full-screen redraw is ~217k). */
#define DBG_HIST       120
#define DBG_BUDGET_PX  75000
#define DBG_GRAPH_MAX  150000

/* Crisp small readout of the dirty rects, drawn with native GDI at WINDOW
 * resolution (not the 466px panel grid) so it can be genuinely tiny. */
static void draw_debug_text(HDC dc)
{
    extern int wos_dbg_rect[16][4], wos_dbg_nr;
    static HFONT s_dbgfont;
    if (!s_dbgfont)
        s_dbgfont = CreateFontA(13, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                DEFAULT_QUALITY, FIXED_PITCH, "Consolas");
    HFONT old = (HFONT)SelectObject(dc, s_dbgfont);
    SetBkMode(dc, TRANSPARENT);
    char b[80];
    int total = 0, maxh = 0, ty = 6, shown = 0;
    for (int i = 0; i < wos_dbg_nr && i < 16; i++) {
        int w = wos_dbg_rect[i][2] - wos_dbg_rect[i][0];
        int h = wos_dbg_rect[i][3] - wos_dbg_rect[i][1];
        if (w <= 0 || h <= 0) continue;
        total += w * h; if (h > maxh) maxh = h;
        snprintf(b, sizeof b, "%2d  %3dx%-3d = %5dpx", shown, w, h, w * h);
        SetTextColor(dc, h >= GFX_H / 2 ? RGB(255, 80, 80) : RGB(90, 235, 140));
        TextOutA(dc, 6, ty, b, (int)strlen(b));
        ty += 14; shown++;
    }
    snprintf(b, sizeof b, "boxes=%d  maxh=%d  TOTAL=%dpx", shown, maxh, total);
    SetTextColor(dc, total > DBG_BUDGET_PX ? RGB(255, 120, 60) : RGB(140, 255, 170));
    TextOutA(dc, 6, ty + 3, b, (int)strlen(b));
    snprintf(b, sizeof b, "TEAR-SIM: %s (T)   %dfps cap (F)",
             s_tear_sim ? "ON" : "off", s_fps30 ? 30 : 60);
    SetTextColor(dc, s_tear_sim ? RGB(255, 160, 90) : RGB(120, 120, 130));
    TextOutA(dc, 6, ty + 17, b, (int)strlen(b));

    /* Rolling per-frame dirty-pixel graph (top-right). Each bar = one frame's
     * TOTAL dirty px; the red line marks the budget so spikes over it stand out. */
    static int hist[DBG_HIST];
    static int hpos;
    hist[hpos] = total;
    hpos = (hpos + 1) % DBG_HIST;

    int gw = DBG_HIST, gh = 70;
    int gx = s_client - gw - 8, gy = 8;
    if (gx < 6) gx = 6;
    RECT bg = { gx - 2, gy - 2, gx + gw + 2, gy + gh + 20 };
    HBRUSH bb = CreateSolidBrush(RGB(12, 12, 16));
    FillRect(dc, &bg, bb); DeleteObject(bb);
    /* budget line */
    int by = gy + gh - (int)((float)DBG_BUDGET_PX / DBG_GRAPH_MAX * gh);
    if (by < gy) by = gy;
    HPEN rp = CreatePen(PS_SOLID, 1, RGB(220, 70, 70));
    HPEN op = (HPEN)SelectObject(dc, rp);
    MoveToEx(dc, gx, by, NULL); LineTo(dc, gx + gw, by);
    SelectObject(dc, op); DeleteObject(rp);
    /* bars, oldest -> newest left to right (vertical lines, fast) */
    HPEN blue = CreatePen(PS_SOLID, 1, RGB(90, 200, 255));
    HPEN red  = CreatePen(PS_SOLID, 1, RGB(255, 90, 70));
    HPEN cur = (HPEN)SelectObject(dc, blue);
    int curIsRed = 0;
    for (int k = 0; k < DBG_HIST; k++) {
        int v = hist[(hpos + k) % DBG_HIST];
        int bh = (int)((float)v / DBG_GRAPH_MAX * gh);
        if (bh > gh) bh = gh;
        if (bh < 0) bh = 0;
        int wantRed = v > DBG_BUDGET_PX;
        if (wantRed != curIsRed) { SelectObject(dc, wantRed ? red : blue); curIsRed = wantRed; }
        MoveToEx(dc, gx + k, gy + gh - 1, NULL);
        LineTo(dc, gx + k, gy + gh - 1 - bh);
    }
    SelectObject(dc, cur); DeleteObject(blue); DeleteObject(red);
    snprintf(b, sizeof b, "px/frame  budget=%dk  max=%dk", DBG_BUDGET_PX / 1000,
             DBG_GRAPH_MAX / 1000);
    SetTextColor(dc, RGB(150, 150, 160));
    TextOutA(dc, gx, gy + gh + 3, b, (int)strlen(b));
    SelectObject(dc, old);
}

/* Tearing simulation (toggle with 'T'). The real panel scans top->bottom at
 * ~60Hz while our flush writes the dirty rects. On a DIRTY row, if the scan
 * reaches it before the flush does, the panel shows the OLD frame there -> a
 * visible tear. On black GAP rows the flush spends no time, so the scan pulls
 * ahead harmlessly (nothing there to tear). This composites old/new exactly like
 * the device would, so we can see whether the tear lands on icons or hides in the
 * gaps — and tune the layout to push it into the gaps. */
#define TEAR_FLUSH_PXPERMS 4000.0f   /* device flush pixel throughput (tunable) —
                                      * low because the AA circle raster is costly */
#define TEAR_RECT_OVER_MS  1.0f      /* per-box overhead: DMA setup + render dispatch */
#define TEAR_STARTUP_MS    1.5f      /* render before the first flush can start */

static void simulate_tear(void)
{
    extern int wos_dbg_rect[16][4], wos_dbg_nr;
    static uint16_t prev[GFX_W * GFX_H];
    static uint16_t clean[GFX_W * GFX_H];
    static int have_prev = 0;
    if (!s_tear_sim || !have_prev) {
        memcpy(prev, s_disp, sizeof prev);
        have_prev = 1;
        return;
    }
    memcpy(clean, s_disp, sizeof clean);        /* this frame's intended pixels */
    const float scan_rpms = (float)GFX_H / 16.7f;
    float flush_t = TEAR_STARTUP_MS;
    int prev_torn = 0;
    for (int y = 0; y < GFX_H; y++) {
        int dw = 0;
        for (int i = 0; i < wos_dbg_nr && i < 16; i++) {
            if (y == wos_dbg_rect[i][1])        /* entering a box: pay its overhead */
                flush_t += TEAR_RECT_OVER_MS;
            if (y >= wos_dbg_rect[i][1] && y < wos_dbg_rect[i][3])
                dw += wos_dbg_rect[i][2] - wos_dbg_rect[i][0];
        }
        int torn = 0;
        if (dw > 0) {                           /* gap rows flush nothing */
            flush_t += (float)dw / TEAR_FLUSH_PXPERMS;
            torn = flush_t > (float)y / scan_rpms;  /* scan beat the flush */
        }
        if (torn)
            memcpy(&s_disp[y * GFX_W], &prev[y * GFX_W], GFX_W * 2);  /* show old */
        if (torn && !prev_torn)                 /* mark the tear crossing (magenta) */
            for (int x = 0; x < GFX_W; x++) s_disp[y * GFX_W + x] = GFX_RGB(255, 0, 200);
        prev_torn = torn;
    }
    memcpy(prev, clean, sizeof prev);           /* next frame's "old" = intended */
}

/* --txcheck: one frame on a virtual 16 ms clock through the device stripe path (the
 * framebuffer stands in for panel RAM), then a hash of the visible glass. Run once
 * with damage tracking and once forcing full repaints: any differing frame means a
 * damage rect missed pixels the screen actually changed. */
static int s_txc_full, s_txc_fr, s_txc_dump = -1;
static FILE *s_txc_out;
static void save_bmp(const char *path);
static void compose_display(void);
static void txc_frame(void)
{
    s_preview_ms += 16;
    if (s_txc_full) wos_invalidate();
    wos_run_frame();
    uint32_t h = 2166136261u;
    for (int i = 0; i < GFX_W * GFX_H; i++) {
        float px = i % GFX_W + 0.5f - GFX_W * 0.5f, py = i / GFX_W + 0.5f - GFX_H * 0.5f;
        if (px * px + py * py <= GFX_W * GFX_W * 0.25f) h = (h ^ s_fb[i]) * 16777619u;
    }
    fprintf(s_txc_out, "%d %08x\n", s_txc_fr, (unsigned)h);
    if (s_txc_fr == s_txc_dump) { compose_display(); save_bmp(s_txc_full ? "txc_full.bmp" : "txc_damage.bmp"); }
    s_txc_fr++;
}
static void txc_frames(int n) { while (n-- > 0) txc_frame(); }
static void txc_drag(int x0, int y0, int x1, int y1, int steps)
{
    for (int s = 0; s <= steps; s++) { wos_pointer(x0 + (x1 - x0) * s / steps, y0 + (y1 - y0) * s / steps, 1); txc_frame(); }
    wos_pointer(x1, y1, 0);
}

static void compose_display(void)
{
    /* apply round-panel mask: outside the glass fades to near-black */
    for (int i = 0; i < GFX_W * GFX_H; i++)
        s_disp[i] = s_mask[i] == 255 ? s_fb[i]
                  : gfx_blend565(GFX_RGB(18, 18, 20), s_fb[i], s_mask[i]);
    simulate_tear();     /* composite the device-accurate tear (if enabled) */
    if (s_overlay) draw_debug_rects();
}

/* --shot mode: dump the composed display as 24bpp BMP for headless checks */
static void save_bmp(const char *path)
{
    int stride = (GFX_W * 3 + 3) & ~3; /* BMP rows pad to 4-byte multiples */
    static uint8_t row[GFX_W * 3 + 4];
    BITMAPFILEHEADER fh = { 0 };
    BITMAPINFOHEADER ih = { 0 };
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof fh + sizeof ih;
    fh.bfSize = fh.bfOffBits + stride * GFX_H;
    ih.biSize = sizeof ih;
    ih.biWidth = GFX_W;
    ih.biHeight = -GFX_H;
    ih.biPlanes = 1;
    ih.biBitCount = 24;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    for (int y = 0; y < GFX_H; y++) {
        for (int x = 0; x < GFX_W; x++) {
            uint16_t p = s_disp[y * GFX_W + x];
            row[x * 3 + 2] = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
            row[x * 3 + 1] = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
            row[x * 3 + 0] = (uint8_t)((p & 0x1F) * 255 / 31);
        }
        fwrite(row, stride, 1, f);
    }
    fclose(f);
    printf("wrote %s\n", path);
}

static void present(HDC dc)
{
    compose_display();

    struct { BITMAPINFOHEADER h; DWORD masks[3]; } bmi = { 0 };
    bmi.h.biSize = sizeof(BITMAPINFOHEADER);
    bmi.h.biWidth = GFX_W;
    bmi.h.biHeight = -GFX_H; /* top-down */
    bmi.h.biPlanes = 1;
    bmi.h.biBitCount = 16;
    bmi.h.biCompression = BI_BITFIELDS;
    bmi.masks[0] = 0xF800; bmi.masks[1] = 0x07E0; bmi.masks[2] = 0x001F;
    if (s_client == GFX_W) {
        /* dev view is 1:1 — a straight DIB copy, no expensive HALFTONE box
         * filter (that was the real ~30fps bottleneck, not the renderer). */
        SetDIBitsToDevice(dc, 0, 0, GFX_W, GFX_H, 0, 0, 0, GFX_H,
                          s_disp, (BITMAPINFO *)&bmi, DIB_RGB_COLORS);
    } else {
        /* Upscaled dev view -> fast nearest-neighbour: HALFTONE's area filter is
         * the ~30fps bottleneck here and isn't needed when enlarging. Only the
         * real-size (downscale) path keeps HALFTONE, where the target is small so
         * it's cheap and the quality is worth it. */
        SetStretchBltMode(dc, s_client > GFX_W ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, 0, 0, s_client, s_client, 0, 0, GFX_W, GFX_H,
                      s_disp, (BITMAPINFO *)&bmi, DIB_RGB_COLORS, SRCCOPY);
    }
    if (s_overlay) draw_debug_text(dc);   /* crisp tiny rect readout, top-left */
}

/* Physical PPI of the monitor the window currently occupies, from its EDID
 * physical dimensions. Re-run whenever the window may have changed monitors. */
static void update_ppi(void)
{
    if (s_ppi_override > 0) {
        s_ppi = s_ppi_override;
    } else {
        float ppi = 0;
        HMONITOR mon = MonitorFromWindow(s_hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXA mi;
        memset(&mi, 0, sizeof mi);
        mi.cbSize = sizeof mi;
        if (mon && GetMonitorInfoA(mon, (MONITORINFO *)&mi)) {
            HDC dc = CreateDCA(NULL, mi.szDevice, NULL, NULL);
            if (dc) {
                int mm = GetDeviceCaps(dc, HORZSIZE);
                int px = GetDeviceCaps(dc, HORZRES);
                if (mm > 0)
                    ppi = px / (mm / 25.4f);
                DeleteDC(dc);
            }
        }
        s_ppi = ppi > 0 ? ppi : 96.0f;
    }
    s_real_px = (int)(PANEL_DIAMETER_IN * s_ppi + 0.5f);
}

static void apply_client_size(void)
{
    s_client = s_real_mode ? s_real_px : GFX_W * DEV_SCALE;
    RECT r = { 0, 0, s_client, s_client };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    SetWindowPos(s_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    char title[160];
    if (s_real_mode)
        snprintf(title, sizeof title,
                 "watch sim  REAL SIZE %.2f\" @ %.0f ppi (%dpx)  R=dev view",
                 (double)PANEL_DIAMETER_IN, (double)s_ppi, s_client);
    else
        snprintf(title, sizeof title,
                 "watch sim  dev view %dpx  (R=real size, B=battery, ESC=quit)",
                 s_client);
    SetWindowText(s_hwnd, title);
    InvalidateRect(s_hwnd, NULL, FALSE);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN:
        SetCapture(h);
        wos_pointer(GET_X_LPARAM(lp) * GFX_W / s_client,
                    GET_Y_LPARAM(lp) * GFX_H / s_client, 1);
        return 0;
    case WM_MOUSEMOVE:
        if (wp & MK_LBUTTON)
            wos_pointer(GET_X_LPARAM(lp) * GFX_W / s_client,
                        GET_Y_LPARAM(lp) * GFX_H / s_client, 1);
        return 0;
    case WM_LBUTTONUP:
        ReleaseCapture();
        wos_pointer(GET_X_LPARAM(lp) * GFX_W / s_client,
                    GET_Y_LPARAM(lp) * GFX_H / s_client, 0);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(h);
        if (wp == 'B') { s_batt = (s_batt + 90) % 101; } /* cycle battery for testing */
        if (wp == 'T') { s_tear_sim = !s_tear_sim; }      /* toggle tear simulation */
        if (wp == 'F') { s_fps30 = !s_fps30; }            /* toggle 30fps device cadence */
        if (wp == 'I') { s_overlay = !s_overlay; }        /* toggle debug overlay */
        if (wp == 'L') { extern int applist_pills; applist_pills = !applist_pills; wos_invalidate(); } /* list style */
        if (wp == 'R') {
            s_real_mode = !s_real_mode;
            update_ppi();
            apply_client_size();
        }
        return 0;
    case WM_EXITSIZEMOVE:   /* window may have landed on another monitor */
    case 0x02E0 /* WM_DPICHANGED */: {
        float old = s_ppi;
        update_ppi();
        if (s_ppi != old && s_real_mode)
            apply_client_size();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        present(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

/* Exercise the OS gesture router, separately from the dial's angle math. */
static int s_route_down, s_route_move, s_route_up, s_route_tap;
static int route_test_event(const wos_event *ev)
{
    if (ev->type == WOS_EV_TOUCH_DOWN) s_route_down++;
    if (ev->type == WOS_EV_TOUCH_MOVE) s_route_move++;
    if (ev->type == WOS_EV_TOUCH_UP) s_route_up++;
    if (ev->type == WOS_EV_TAP) s_route_tap++;
    return 1;
}
static void route_test_draw(gfx_fb *fb) { gfx_clear(fb, GFX_BLACK); }
static const wos_app s_route_test = {
    "gesture test", NULL, route_test_event, route_test_draw, GFX_WHITE, ICON_NONE
};

int main(int argc, char **argv)
{
    QueryPerformanceFrequency(&s_qpf);
    QueryPerformanceCounter(&s_t0);

    /* Per-monitor DPI awareness so 1 window px == 1 physical px regardless
     * of Windows display scaling; fall back for older systems. */
    typedef BOOL(WINAPI *dpi_ctx_fn)(HANDLE);
    dpi_ctx_fn set_ctx = (dpi_ctx_fn)(void *)GetProcAddress(
        GetModuleHandle("user32"), "SetProcessDpiAwarenessContext");
    if (!set_ctx || !set_ctx((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */))
        SetProcessDPIAware();

    /* --ppi N overrides a lying EDID; otherwise the PPI of whichever
     * monitor the window sits on is tracked dynamically (update_ppi). */
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--ppi") == 0)
            s_ppi_override = (float)atof(argv[i + 1]);

    /* precompute circular mask with AA edge, radius = panel radius */
    float rad = GFX_W / 2.0f;
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++) {
            float dx = x + 0.5f - rad, dy = y + 0.5f - rad;
            float d = rad - (float)sqrt(dx * dx + dy * dy) + 0.5f;
            s_mask[y * GFX_W + x] = d <= 0 ? 0 : d >= 1 ? 255 : (uint8_t)(d * 255);
        }

    static const wos_app *const apps[] = {
        &app_watchface, &app_stopwatch, &app_timer, &app_alarm,
        &app_worldclock, &app_calendar, &app_weather, &app_activity,
        &app_breathe, &app_music, &app_recorder, &app_voice, &app_findphone,
        &app_ble, &app_netmon, &app_wifi, &app_sensors, &app_level,
        &app_calc, &app_2048, &app_flashlight, &app_battery, &app_pomodoro, &app_settings, &app_storage, &app_pet_care,
    };
    settings_load();
    if(argc>1 && (!strcmp(argv[1],"--pip-gallery") || !strcmp(argv[1],"--pip-checks") ||
                 !strcmp(argv[1],"--pip-render-checks") || !strcmp(argv[1],"--pip-care-gallery") ||
                 !strcmp(argv[1],"--wake-checks") || !strcmp(argv[1],"--brightness-checks"))) {
        s_pip_preview=1;s_overlay=0;g_saved_face=1;
    }
    face_pip_service(0);
    wos_init(s_fb, apps, sizeof apps / sizeof apps[0]);
    wos_register_overlay(WOS_EDGE_LEFT,   &app_launcher,      WOS_OV_PANNABLE);
    wos_register_overlay(WOS_EDGE_RIGHT,  &app_applist,       0);
    { extern int applist_pills;                   /* --list-pills: every row a moving pill */
      for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--list-pills")) applist_pills = 1; }
    wos_register_overlay(WOS_EDGE_TOP,    &app_notifications, 0);
    wos_register_overlay(WOS_EDGE_BOTTOM, &app_control,       0);
    ui_style_apply();      /* Flat60 swaps in its own overlays */
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--flat60")) { wos_ui_style = 1; ui_style_apply(); }

    if(argc>1 && !strcmp(argv[1],"--brightness-checks")) {
        g_brightness=122;
        wos_open_app(23);
        for(int i=0;i<30;i++){s_preview_ms+=50;wos_run_frame();}
        assert(wos_current_app()==23);
        int saves=s_cfg_saves;
        wos_pointer(160,163,1);
        assert(g_brightness<70 && s_backlight==g_brightness);
        s_preview_ms+=50;wos_pointer(233,163,1);
        assert(g_brightness>=127 && g_brightness<=128 && s_cfg_saves==saves);
        s_preview_ms+=50;wos_pointer(340,163,1);
        assert(g_brightness>=239 && s_backlight==g_brightness && s_cfg_saves==saves);
        s_preview_ms+=20;wos_pointer(340,163,0);
        assert(s_cfg_saves==saves+1);
        /* A raw tap must save once despite the derived TAP after TOUCH_UP. */
        saves=s_cfg_saves;s_preview_ms+=50;wos_pointer(115,163,1);
        s_preview_ms+=50;wos_pointer(115,163,0);
        assert(g_brightness==5 && s_backlight==5 && s_cfg_saves==saves+1);
        /* Capture follows the finger past the track, while unrelated drags
         * crossing the same row must never grab the brightness control. */
        s_preview_ms+=50;wos_pointer(160,163,1);
        s_preview_ms+=50;wos_pointer(400,180,1);
        s_preview_ms+=20;wos_pointer(400,180,0);
        assert(g_brightness==255 && s_backlight==255);
        s_preview_ms+=50;wos_pointer(65,163,1);
        s_preview_ms+=50;wos_pointer(220,163,1);
        s_preview_ms+=20;wos_pointer(220,163,0);
        assert(g_brightness==255);
        /* Opening Control Center keeps the existing setting; its slider
         * then changes the exact same saved value and HAL command. */
        s_preview_ms+=50;wos_pointer(233,460,1);
        s_preview_ms+=50;wos_pointer(233,150,1);
        s_preview_ms+=20;wos_pointer(233,150,0);
        for(int i=0;i<30;i++){s_preview_ms+=50;wos_run_frame();}
        assert(g_brightness==255);
        wos_pointer(130,112,1);s_preview_ms+=50;wos_pointer(250,112,1);
        s_preview_ms+=20;wos_pointer(250,112,0);
        assert(g_brightness>120 && g_brightness<140 && s_backlight==g_brightness);
        puts("PASS brightness: Settings live drag/tap, HAL writes, one save per gesture, clamps, hit bounds, Control Center");
        return 0;
    }

    /* --app N: boot straight into app N and stay interactive (design preview). */
    if(s_pip_preview) {
        if(argc>3 && !strcmp(argv[3],"--stripes"))
            wos_set_stripes(s_test_stripes[0],s_test_stripes[1],16,NULL,test_stripe_flush);
        wos_open_app(0);
        for(int i=0;i<30;i++){s_preview_ms+=50;wos_run_frame();}
        if(!strcmp(argv[1],"--wake-checks")) {
            wake_info wi;
            wake_service_set("wrist",0);wake_service_set("pickup",1);wake_service_set("turn",1);
            wos_open_app(23);
            for(int i=0;i<20;i++){s_preview_ms+=50;wos_run_frame();}
            wos_pointer(GFX_S(120),GFX_S(150),1);s_preview_ms+=50;wos_run_frame();
            wos_pointer(GFX_S(120),GFX_S(150),0);s_preview_ms+=50;wos_run_frame();
            compose_display();save_bmp("wake-left.bmp");
            wos_pointer(GFX_S(154),GFX_S(76),1);s_preview_ms+=50;wos_run_frame();
            wos_pointer(GFX_S(154),GFX_S(76),0);s_preview_ms+=50;wos_run_frame();
            wake_service_get(&wi);assert(wi.wrist==1);
            wos_pointer(GFX_S(178),GFX_S(112),1);s_preview_ms+=50;wos_run_frame();
            wos_pointer(GFX_S(178),GFX_S(112),0);s_preview_ms+=50;wos_run_frame();
            wake_service_get(&wi);assert(!wi.pickup && wi.turn);
            compose_display();save_bmp("wake-right.bmp");
            wos_pointer(GFX_S(120),GFX_S(181),1);s_preview_ms+=50;wos_run_frame();
            wos_pointer(GFX_S(120),GFX_S(181),0);s_preview_ms+=50;wos_run_frame();
            wake_service_get(&wi);assert(wi.learning);
            compose_display();save_bmp("wake-learning.bmp");
            for(int i=0;i<25;i++){s_preview_ms+=50;wake_service_tick(s_preview_ms,0);wos_run_frame();}
            wake_service_get(&wi);assert(wi.learned && wi.learn_result==1);
            compose_display();save_bmp("wake-learned.bmp");
            extern void app_diagnostics_imu_test(void);
            wos_open_app(16);app_diagnostics_imu_test();
            for(int i=0;i<20;i++){s_preview_ms+=50;wos_run_frame();}
            compose_display();save_bmp("imu.bmp");
            puts("PASS wake settings: page navigation, wrist selection, independent toggles, learning and rendered IMU labels");
            return 0;
        }
        if(!strcmp(argv[1],"--pip-checks")) {
            char status[224];
            face_pip_command("live");
            wos_pointer(180,240,1);wos_run_frame();
            for(int i=0;i<6;i++) {
                s_preview_ms+=50;wos_pointer((i&1)?180:270,240,1);wos_run_frame();
            }
            wos_pointer(180,240,0);s_preview_ms+=20;wos_run_frame();
            assert(g_saved_face==1);
            assert(strstr(face_pip_debug(status,sizeof status),"state=petted"));
            /* Below-eye swipe still switches faces. */
            wos_pointer(320,380,1);wos_run_frame();
            s_preview_ms+=100;wos_pointer(220,380,1);wos_run_frame();
            s_preview_ms+=100;wos_pointer(120,380,1);wos_run_frame();
            wos_pointer(120,380,0);
            for(int i=0;i<20;i++){s_preview_ms+=50;wos_run_frame();}
            assert(g_saved_face==2);
            g_saved_face=1;wos_open_app(0);
            for(int i=0;i<20;i++){s_preview_ms+=50;wos_run_frame();}
            /* Begin and cancel an edge gesture. Pet must release its pointer. */
            wos_pointer(6,233,1);wos_run_frame();
            s_preview_ms+=100;wos_pointer(170,233,1);wos_run_frame();
            s_preview_ms+=100;wos_pointer(6,233,1);wos_run_frame();
            wos_pointer(6,233,0);
            for(int i=0;i<25;i++){s_preview_ms+=50;wos_run_frame();}
            assert(strstr(face_pip_debug(status,sizeof status),"down=0"));
            assert(g_saved_face==1);
            /* Care footer opens a real app; it can return to Pip. */
            wos_pointer(233,409,1);s_preview_ms+=100;wos_run_frame();
            wos_pointer(233,409,0);
            for(int i=0;i<30;i++){s_preview_ms+=50;wos_run_frame();}
            assert(!strcmp(wos_app_name(wos_current_app()),"Pet care"));
            wos_pointer(233,422,1);s_preview_ms+=100;wos_run_frame();
            wos_pointer(233,422,0);
            for(int i=0;i<30;i++){s_preview_ms+=50;wos_run_frame();}
            assert(wos_current_app()==0 && g_saved_face==1);
            puts("PASS Pip/WOS: looping pet stroke, gallery swipe, edge cancellation");
            puts("PASS Pip/WOS: care footer and greeting navigation");
            return 0;
        }
        if(!strcmp(argv[1],"--pip-care-gallery")) {
            face_pip_command("care");
            for(int page=0;page<3;page++) {
                for(int i=0;i<30;i++){s_preview_ms+=50;face_pip_service(0);wos_run_frame();}
                compose_display();char path[40];snprintf(path,sizeof path,"care_%d.bmp",page);save_bmp(path);
                wos_pointer(310,275,1);s_preview_ms+=100;wos_run_frame();
                wos_pointer(130,275,1);s_preview_ms+=100;wos_run_frame();
                wos_pointer(130,275,0);
            }
            return 0;
        }
        int render_checks=!strcmp(argv[1],"--pip-render-checks"),mismatches=0;
        FILE *hashes=render_checks && argc>2 ? fopen(argv[2],"w") : NULL;
        if(render_checks && argc>2 && !hashes)return 1;
        for(int action=0;action<PIP_ACTION_COUNT;action++) {
            char cmd[48];snprintf(cmd,sizeof cmd,"demo %s",pip_action_name((pip_action)action));
            face_pip_command(cmd);
            for(int frame=0;frame<(render_checks?120:80);frame++) {
                s_preview_ms+=50;wos_run_frame();compose_display();
                if(render_checks) {
                    if(hashes) {
                        uint64_t hash=UINT64_C(14695981039346656037);
                        for(int i=0;i<GFX_W*GFX_H;i++) {
                            hash^=s_fb[i];hash*=UINT64_C(1099511628211);
                        }
                        fprintf(hashes,"%d,%d,%016llx\n",action,frame,(unsigned long long)hash);
                    }
                    static uint16_t clean[GFX_W*GFX_H];
                    gfx_fb clean_fb={.px=clean,.h=GFX_H,.cx1=GFX_W};
                    hal_datetime dt;hal_get_datetime(&dt);face_pip_draw(&clean_fb,&dt);
                    if(memcmp(s_fb,clean,sizeof clean)) {
                        printf("FAIL dirty/full render action=%s frame=%d\n",pip_action_name((pip_action)action),frame);
                        mismatches++;
                    }
                    continue;
                }
                char name[80];snprintf(name,sizeof name,"pip_frames/frame_%03d.bmp",action*40+frame/2);
                if(frame%2==0)save_bmp(name);
                if(frame==11){snprintf(name,sizeof name,"pip_%s.bmp",pip_action_name((pip_action)action));save_bmp(name);}
            }
        }
        if(render_checks)printf("%s Pip: %d frames compared against clean full rendering\n",mismatches?"FAIL":"PASS",PIP_ACTION_COUNT*120);
        if(hashes)fclose(hashes);
        if(render_checks) {
            const int clock_cases[]={14*60+24,14*60+59,15*60,23*60+59,0};
            for(unsigned i=0;i<sizeof clock_cases/sizeof clock_cases[0];i++) {
                s_preview_clock_minutes=clock_cases[i];s_preview_ms+=17;wos_run_frame();
                static uint16_t clean[GFX_W*GFX_H];
                gfx_fb clean_fb={.px=clean,.h=GFX_H,.cx1=GFX_W};
                hal_datetime dt;hal_get_datetime(&dt);face_pip_draw(&clean_fb,&dt);
                assert(!memcmp(s_fb,clean,sizeof clean));
                extern int wos_dbg_nr,wos_dbg_rect[16][4];
                assert(wos_dbg_nr>0);
                for(int j=0;j<wos_dbg_nr && j<16;j++)
                    assert(wos_dbg_rect[j][2]-wos_dbg_rect[j][0]<GFX_W ||
                           wos_dbg_rect[j][3]-wos_dbg_rect[j][1]<GFX_H);
            }
            puts("PASS Pip: minute/hour/midnight clock changes match full pixels without full-screen damage");
        }
        return mismatches?1:0;
    }
    for (int i = 1; i < argc - 1; i++)
        if (strcmp(argv[i], "--app") == 0) wos_open_app(atoi(argv[i + 1]));

    notif_push("Messages", "Mom: call me back", GFX_GREEN);
    notif_push("Calendar", "Standup in 10 min", GFX_RED);

    if (argc > 1 && strcmp(argv[1], "--overlay-gesture-checks") == 0) {
        wos_register_overlay(WOS_EDGE_LEFT, &s_route_test, WOS_OV_PANNABLE);
        wos_pointer(8, 233, 1);
        wos_pointer(300, 233, 1);
        wos_pointer(300, 233, 0);
        for (int i = 0; i < 24; i++) { Sleep(5); wos_run_frame(); }
        wos_pointer(233, 100, 1);
        wos_pointer(233, 100, 0);
        assert(s_route_tap == 1 && s_route_down == 0 && s_route_up == 0);
        /* Out and back ends at the same point, but already crossed drag slop. */
        wos_pointer(233, 100, 1);
        wos_pointer(300, 100, 1);
        wos_pointer(233, 100, 1);
        wos_pointer(233, 100, 0);
        assert(s_route_tap == 1 && s_route_down == 1 && s_route_up == 1);
        wos_pointer(233, 100, 1);
        wos_pointer(366, 233, 1);
        wos_pointer(233, 366, 1);
        wos_pointer(100, 233, 1);
        wos_pointer(233, 100, 1);
        wos_pointer(233, 100, 0);
        assert(s_route_tap == 1 && s_route_down == 2 && s_route_up == 2);
        assert(s_route_move == 6);
        /* A subsequent genuine tap is still delivered. */
        wos_pointer(233, 100, 1);
        wos_pointer(233, 100, 0);
        assert(s_route_tap == 2);
        puts("PASS overlay routing: taps, returning drags and full circles");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--handtest") == 0) {
        /* Sweep the second hand via the dirty-rect path for a while, then force a
         * clean full redraw at the same instant. Any residual the partial redraw
         * leaves behind shows up as extra pixels when the two are diffed. */
        wos_open_app(0);
        for (int i = 0; i < 160; i++) { Sleep(10); wos_run_frame(); }
        compose_display(); save_bmp("hand_dirty.bmp");
        wos_open_app(1); wos_run_frame();          /* leave the face... */
        wos_open_app(0); wos_run_frame();          /* ...return -> forces full redraw */
        compose_display(); save_bmp("hand_full.bmp");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--diag") == 0) {
        int idx = 16;
        for (int i = 0; i < wos_app_count(); i++)
            if (strcmp(wos_app_name(i), "Diagnostics") == 0) idx = i;
        wos_open_app(idx);
        for (int i = 0; i < 22; i++) { Sleep(16); wos_run_frame(); }
        const char *names[5] = { "diag_system.bmp", "diag_tests.bmp", "diag_log.bmp", "diag_imu.bmp", "diag_touch.bmp" };
        for (int p = 0; p < 5; p++) {
            for (int i = 0; i < 6; i++) { Sleep(16); wos_run_frame(); }
            compose_display(); save_bmp(names[p]);
            wos_pointer(340, 233, 1); Sleep(12); wos_run_frame();   /* mid-screen swipe left -> next page */
            wos_pointer(230, 233, 1); Sleep(12); wos_run_frame();
            wos_pointer(120, 233, 1); wos_run_frame();
            wos_pointer(120, 233, 0);
            for (int i = 0; i < 4; i++) { Sleep(16); wos_run_frame(); }
        }
        return 0;
    }

    if (argc > 2 && strcmp(argv[1], "--face") == 0) {
        extern int g_saved_face;
        g_saved_face = atoi(argv[2]);
        wos_open_app(0);
        for (int i = 0; i < 40; i++) { Sleep(12); wos_run_frame(); }
        compose_display(); save_bmp("shot_faceX.bmp");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--trans") == 0) {
        int C = GFX_W / 2;
        wos_open_app(0);
        for (int i = 0; i < 20; i++) { Sleep(12); wos_run_frame(); }
        /* DRAG in from the RIGHT edge -> app-list; the fade front should track the finger. */
        wos_pointer(GFX_W - 6, C, 1); Sleep(16); wos_run_frame();
        for (int s = 1; s <= 10; s++) {                 /* gradual inward drag to ~55% */
            int x = (GFX_W - 6) - s * 26;
            wos_pointer(x, C, 1); Sleep(16); wos_run_frame();
        }
        compose_display(); save_bmp("trans_drag55.bmp");   /* finger held at ~55% (mid-fade) */
        wos_pointer(120, C, 0);                          /* release past 1/3 -> commit */
        for (int i = 0; i < 12; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("trans_applist.bmp"); /* settled open */
        /* now a CANCEL: start dragging back out but release early (<1/3) -> should snap back open */
        wos_pointer(120, C, 1); Sleep(16); wos_run_frame();
        for (int s = 1; s <= 4; s++) { wos_pointer(120 + s * 12, C, 1); Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("trans_cancel_mid.bmp");
        wos_pointer(168, C, 0);                          /* small move -> cancel, back to app-list */
        for (int i = 0; i < 12; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("trans_cancel_settled.bmp");
        /* watch-face swap: mid-screen quick left swipe -> scan-fade to next face + name callout */
        wos_open_app(0);
        for (int i = 0; i < 12; i++) { Sleep(16); wos_run_frame(); }
        wos_pointer(340, C, 1); Sleep(12); wos_run_frame();
        wos_pointer(230, C, 1); Sleep(12); wos_run_frame();
        wos_pointer(120, C, 1); wos_run_frame();
        wos_pointer(120, C, 0);
        for (int i = 0; i < 3; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("trans_face_mid.bmp");     /* mid scan-fade between faces */
        for (int i = 0; i < 10; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("trans_face_settled.bmp"); /* new face + name callout */
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--shot") == 0) {
        int C = GFX_W / 2;
        wos_run_frame();
        compose_display();
        save_bmp("shot_watchface.bmp");
        /* open each edge overlay, screenshot, close between */
        struct { int sx, sy, ex, ey; const char *name; } edges[] = {
            {  8, C, 300, C, "shot_launcher.bmp" },  /* left  -> honeycomb  */
            { GFX_W - 8, C, GFX_W - 300, C, "shot_applist.bmp" }, /* right */
            { C, GFX_H - 8, C, GFX_H - 300, "shot_control.bmp" }, /* bottom */
            { C, 8, C, 300, "shot_notifications.bmp" },           /* top   */
        };
        for (int e = 0; e < 4; e++) {
            wos_pointer(edges[e].sx, edges[e].sy, 1);
            Sleep(20); wos_run_frame();
            int steps = 6;
            for (int s = 1; s <= steps; s++) {
                int x = edges[e].sx + (edges[e].ex - edges[e].sx) * s / steps;
                int y = edges[e].sy + (edges[e].ey - edges[e].sy) * s / steps;
                wos_pointer(x, y, 1);
                Sleep(16); wos_run_frame();
            }
            wos_pointer(edges[e].ex, edges[e].ey, 0);
            for (int i = 0; i < 45; i++) { Sleep(16); wos_run_frame(); }
            compose_display();
            save_bmp(edges[e].name);
            wos_close_overlay();
            for (int i = 0; i < 45; i++) { Sleep(16); wos_run_frame(); }
        }
        /* open every app by index and screenshot it */
        for (int a = 1; a < wos_app_count(); a++) {
            char nm[32];
            snprintf(nm, sizeof nm, "shot_app%d.bmp", a);
            wos_open_app(a);
            for (int i = 0; i < 30; i++) { Sleep(16); wos_run_frame(); }
            compose_display(); save_bmp(nm);
        }

        /* back to the watchface, then swipe through all faces */
        wos_open_app(0);
        for (int i = 0; i < 10; i++) { Sleep(16); wos_run_frame(); }
        for (int f = 0; f < 13; f++) {          /* keep in sync with FACES[] */
            char nm[32];
            snprintf(nm, sizeof nm, "shot_face%d.bmp", f);
            wos_run_frame(); compose_display(); save_bmp(nm);
            /* quick mid-screen left swipe -> next face */
            wos_pointer(340, 233, 1); Sleep(12); wos_run_frame();
            wos_pointer(240, 233, 1); Sleep(12); wos_run_frame();
            wos_pointer(130, 233, 1); wos_run_frame();
            wos_pointer(130, 233, 0);
            for (int i = 0; i < 4; i++) { Sleep(12); wos_run_frame(); }
        }
        return 0;
    }

    /* --music: open the Music app and screenshot one frame (design iteration). */
    if (argc > 1 && strcmp(argv[1], "--air") == 0) {
        extern int wos_debug_hud; wos_debug_hud = 0;
        s_overlay = 0;
        int idx = -1;
        for (int i = 0; i < wos_app_count(); i++)
            if (strcmp(wos_app_name(i), "Wi-Fi") == 0) idx = i;
        wos_open_app(idx);
        for (int i = 0; i < 8; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("air_aps.bmp");
        const char *names[2] = { "air_chan.bmp", "air_heat.bmp" };
        for (int p = 0; p < 2; p++) {                 /* swipe left -> next page */
            wos_pointer(360, 233, 1); Sleep(16); wos_run_frame();
            wos_pointer(280, 233, 1); Sleep(16); wos_run_frame();
            wos_pointer(180, 233, 1); Sleep(16); wos_run_frame();
            wos_pointer(110, 233, 1); Sleep(16); wos_run_frame();
            wos_pointer(110, 233, 0); wos_run_frame();
            for (int i = 0; i < 40; i++) { Sleep(16); wos_run_frame(); }
            compose_display(); save_bmp(names[p]);
        }
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--alarm") == 0) {
        extern int wos_debug_hud; wos_debug_hud = 0;
        s_overlay = 0;
        wos_open_app(3);                                 /* alarm */
        for (int i = 0; i < 6; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("alarm_list.bmp");
        /* swipe up to scroll the list (multi-step drag so it classifies as a swipe) */
        wos_pointer(233, 360, 1); Sleep(16); wos_run_frame();
        wos_pointer(233, 300, 1); Sleep(16); wos_run_frame();
        wos_pointer(233, 220, 1); Sleep(16); wos_run_frame();
        wos_pointer(233, 150, 1); Sleep(16); wos_run_frame();
        wos_pointer(233, 150, 0); wos_run_frame();
        for (int i = 0; i < 12; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("alarm_scroll.bmp");
        /* tap a row's time area -> enter the HH:MM picker */
        wos_pointer(90, 233, 1); wos_run_frame(); wos_pointer(90, 233, 0);
        for (int i = 0; i < 12; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("alarm_edit.bmp");
        /* swipe up on the hours drum -> roll the value (mid-animation) */
        wos_pointer(160, 300, 1); wos_run_frame();
        wos_pointer(160, 170, 1); Sleep(10); wos_run_frame();
        wos_pointer(160, 170, 0); wos_run_frame();
        compose_display(); save_bmp("alarm_roll.bmp");
        for (int i = 0; i < 10; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("alarm_edit2.bmp");
        return 0;
    }

    if (argc > 1 && strcmp(argv[1], "--music") == 0) {
        extern int wos_debug_hud; wos_debug_hud = 0;   /* hide FPS pill */
        extern int wos_dbg_rect[16][4], wos_dbg_nr;
        s_overlay = 0;                       /* clean shot: no dirty-rect debug chrome */
        wos_open_app(9);
        wos_run_frame();                     /* one fresh full-redraw frame */
        compose_display(); save_bmp("shot_fresh.bmp");
        for (int i = 0; i < 30; i++) {
            Sleep(16); wos_run_frame();
            int total = 0, maxh = 0;
            for (int k = 0; k < wos_dbg_nr && k < 16; k++) {
                int w = wos_dbg_rect[k][2] - wos_dbg_rect[k][0];
                int h = wos_dbg_rect[k][3] - wos_dbg_rect[k][1];
                total += w * h; if (h > maxh) maxh = h;
            }
            printf("f%02d nr=%d totalpx=%d maxh=%d (full=%d)\n",
                   i, wos_dbg_nr, total, maxh, GFX_W * GFX_H);
        }
        compose_display(); save_bmp("shot_music.bmp");
        /* tap the centre -> controls should appear (replay under play) */
        wos_pointer(233, 233, 1); wos_run_frame(); wos_pointer(233, 233, 0);
        for (int i = 0; i < 4; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("shot_music_tap.bmp");
        /* swipe left from centre -> scan-line transition -> volume screen */
        wos_pointer(233, 233, 1); wos_run_frame();
        wos_pointer(150, 233, 1); Sleep(10); wos_run_frame();
        wos_pointer(60, 233, 1);  wos_run_frame();
        wos_pointer(60, 233, 0);
        for (int i = 0; i < 8; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("shot_music_trans.bmp");     /* mid-transition crossfade */
        for (int i = 0; i < 22; i++) { Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("shot_music_vol.bmp");       /* landed on volume */
        return 0;
    }

    /* --bench <faceIndex> [frames]: render a face as fast as possible and
     * report ms/frame + fps, so renderer optimization is measurable. */
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        int face = argc > 2 ? atoi(argv[2]) : 0;
        int frames = argc > 3 ? atoi(argv[3]) : 400;
        for (int i = 0; i < face; i++) {   /* swipe to the requested face */
            wos_pointer(340, 233, 1); wos_run_frame();
            wos_pointer(240, 233, 1); wos_run_frame();
            wos_pointer(130, 233, 1); wos_run_frame();
            wos_pointer(130, 233, 0);
            for (int k = 0; k < 20; k++) wos_run_frame();
        }
        LARGE_INTEGER a, b;
        QueryPerformanceCounter(&a);
        for (int i = 0; i < frames; i++) wos_run_frame();
        QueryPerformanceCounter(&b);
        double ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / s_qpf.QuadPart / frames;
        printf("bench face %d: %.3f ms/frame  =  %.1f fps\n", face, ms, 1000.0 / ms);
        return 0;
    }

    /* --listrects: open the app list, fling it, and print each frame's dirty
     * rects (count, WxH, total px) so the explicit-rect scroll can be verified
     * headless. Saves a couple of mid-fling BMPs with the debug overlay. */
    if (argc > 1 && strcmp(argv[1], "--listrects") == 0) {
        extern int wos_dbg_rect[16][4], wos_dbg_nr;
        int C = GFX_W / 2;
        /* swipe in from the right edge to open the list */
        wos_pointer(GFX_W - 8, C, 1); Sleep(20); wos_run_frame();
        for (int s = 1; s <= 6; s++) {
            int x = (GFX_W - 8) + ((GFX_W - 300) - (GFX_W - 8)) * s / 6;
            wos_pointer(x, C, 1); Sleep(16); wos_run_frame();
        }
        wos_pointer(GFX_W - 300, C, 0);
        for (int i = 0; i < 40; i++) { Sleep(16); wos_run_frame(); }   /* settle open */
        /* fast upward fling */
        wos_pointer(240, 380, 1); wos_run_frame();
        wos_pointer(240, 300, 1); Sleep(8);
        /* The list declares tight explicit damage while scrolling. A display
         * invalidation must still win and repaint a complete trusted baseline. */
        wos_invalidate();
        wos_run_frame();
        assert(wos_dbg_nr == 1 &&
               wos_dbg_rect[0][0] == 0 && wos_dbg_rect[0][1] == 0 &&
               wos_dbg_rect[0][2] == GFX_W && wos_dbg_rect[0][3] == GFX_H);
        wos_pointer(240, 200, 1); Sleep(8); wos_run_frame();
        wos_pointer(240, 120, 1); Sleep(8); wos_run_frame();
        wos_pointer(240, 120, 0);
        for (int f = 0; f < 22; f++) {
            wos_run_frame();
            int total = 0, maxh = 0;
            printf("f%02d nr=%d ", f, wos_dbg_nr);
            for (int i = 0; i < wos_dbg_nr && i < 16; i++) {
                int w = wos_dbg_rect[i][2] - wos_dbg_rect[i][0];
                int h = wos_dbg_rect[i][3] - wos_dbg_rect[i][1];
                total += w * h; if (h > maxh) maxh = h;
                printf("[%dx%d]", w, h);
            }
            printf(" total=%d maxh=%d\n", total, maxh);
            if (f == 2 || f == 6 || f == 12) {
                char nm[32]; snprintf(nm, sizeof nm, "listrects_%d.bmp", f);
                compose_display(); save_bmp(nm);
            }
            Sleep(16);
        }
        return 0;
    }

    /* --rectcheck: device "redraw boxes" debug. Scroll the app list with outlines on
     * (saves rectcheck_on.bmp), switch them off, and verify the framebuffer then
     * matches a clean full repaint - i.e. every old outline was erased. */
    if (argc > 1 && strcmp(argv[1], "--rectcheck") == 0) {
        extern int wos_debug_rects;
        static uint16_t clean[GFX_W * GFX_H];
        int C = GFX_W / 2;
        s_overlay = 0;
        wos_debug_rects = 1;
        for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--no-outline")) wos_debug_rects = 0;
        wos_pointer(GFX_W - 8, C, 1); Sleep(20); wos_run_frame();
        for (int s = 1; s <= 6; s++) {
            wos_pointer((GFX_W - 8) + ((GFX_W - 300) - (GFX_W - 8)) * s / 6, C, 1);
            Sleep(16); wos_run_frame();
        }
        wos_pointer(GFX_W - 300, C, 0);
        for (int i = 0; i < 40; i++) { Sleep(16); wos_run_frame(); }
        for (int d = 0; d <= 200; d += 12) { wos_pointer(C, 380 - d, 1); Sleep(16); wos_run_frame(); }
        compose_display(); save_bmp("rectcheck_on.bmp");
        wos_pointer(C, 180, 0);
        for (int i = 0; i < 5; i++) { Sleep(16); wos_run_frame(); }
        wos_debug_rects = 0;
        for (int i = 0; i < 90; i++) { Sleep(16); wos_run_frame(); }
        memcpy(clean, s_fb, sizeof clean);
        wos_invalidate(); wos_run_frame();
        int bad = 0, bx0 = GFX_W, by0 = GFX_H, bx1 = -1, by1 = -1;
        for (int i = 0; i < GFX_W * GFX_H; i++) {
            int x = i % GFX_W, y = i / GFX_W;
            float px = x + 0.5f - GFX_W * 0.5f, py = y + 0.5f - GFX_H * 0.5f;
            if (px * px + py * py > GFX_W * GFX_W * 0.25f) continue;   /* off the glass */
            if (clean[i] != s_fb[i]) {
                bad++;
                if (x < bx0) bx0 = x;
                if (x > bx1) bx1 = x;
                if (y < by0) by0 = y;
                if (y > by1) by1 = y;
            }
        }
        printf("rectcheck: %d stale pixels after turning outlines off", bad);
        if (bad) printf(" in [%d,%d]-[%d,%d]", bx0, by0, bx1, by1);
        printf("\n");
        if (bad) { memcpy(s_fb, clean, sizeof clean); compose_display(); save_bmp("rectcheck_stale.bmp"); }
        return bad != 0;
    }

    /* --facerects N [SECONDS]: run face N on the virtual 16 ms clock and summarise what
     * damage repaints per frame (rects, pixels, how often it is the whole screen); saves
     * facerects_N.bmp with the dirty-rect overlay at the frame with the most damage. */
    if (argc > 2 && strcmp(argv[1], "--facerects") == 0) {
        extern uint32_t wos_dbg_dirty_pixels;
        extern int wos_dbg_rect[16][4], wos_dbg_nr;
        int face = atoi(argv[2]), n = (argc > 3 ? atoi(argv[3]) : 3) * 60;
        s_pip_preview = 1; s_preview_ms = 1000; g_saved_face = face;
        wos_open_app(0); wos_invalidate();
        for (int i = 0; i < 20; i++) { s_preview_ms += 16; wos_run_frame(); }
        double sum = 0; uint32_t mx = 0; int full = 0, drew = 0, mxf = -1;
        static uint16_t best[GFX_W * GFX_H]; int best_r[16][4], best_n = 0;
        for (int i = 0; i < n; i++) {
            s_preview_ms += 16;
            wos_run_frame();
            uint32_t px = wos_dbg_dirty_pixels;
            sum += px; drew += px > 0; full += px >= (uint32_t)(GFX_W * GFX_H * 9 / 10);
            if (px > mx) {
                mx = px; mxf = i; best_n = wos_dbg_nr;
                memcpy(best, s_fb, sizeof best);
                memcpy(best_r, wos_dbg_rect, sizeof best_r);
            }
        }
        printf("face %d: %d frames, drew %d, avg %.0f px/frame, max %u px (frame %d), full-screen %d\n",
               face, n, drew, sum / n, mx, mxf, full);
        for (int r = 0; r < best_n && r < 16; r++)
            printf("  max-frame rect %d: [%d,%d]-[%d,%d] %dx%d\n", r, best_r[r][0], best_r[r][1], best_r[r][2],
                   best_r[r][3], best_r[r][2] - best_r[r][0], best_r[r][3] - best_r[r][1]);
        memcpy(s_fb, best, sizeof best);
        memcpy(wos_dbg_rect, best_r, sizeof best_r); wos_dbg_nr = best_n;
        s_overlay = 1; compose_display();
        char nm[32]; snprintf(nm, sizeof nm, "facerects_%d.bmp", face); save_bmp(nm);
        return 0;
    }

    /* --facecheck damage|full OUT [FACE]: every watch face (or just FACE) for 450
     * frames from 0:54 across the minute rollover, through the stripe path, hashing
     * each frame (txc_frame). The damage run must match the full-repaint run. */
    if (argc > 3 && strcmp(argv[1], "--facecheck") == 0) {
        s_txc_full = !strcmp(argv[2], "full");
        s_txc_out = fopen(argv[3], "w");
        int only = argc > 4 ? atoi(argv[4]) : -1;
        s_overlay = 0; s_pip_preview = 1;
        wos_set_stripes(s_test_stripes[0], s_test_stripes[1], 16, NULL, test_stripe_flush);
        for (int f = 0; f < 14; f++) {
            if (only >= 0 && f != only) continue;
            g_saved_face = f; s_preview_ms = 54000;
            wos_open_app(0); wos_invalidate();
            fprintf(s_txc_out, "face %d\n", f);
            txc_frames(450);
        }
        fclose(s_txc_out);
        printf("facecheck %s: %d frames\n", argv[2], s_txc_fr);
        return 0;
    }

    /* --stackshot: open the Flat60 app list, save it settled and mid-scroll. */
    if (argc > 1 && strcmp(argv[1], "--stackshot") == 0) {
        int C = GFX_W / 2;
        s_overlay = 0; s_pip_preview = 1; s_preview_ms = 1000; s_txc_out = fopen("NUL", "w");
        wos_ui_style = 1; ui_style_apply();
        wos_open_app(0); wos_invalidate(); txc_frames(10);
        txc_drag(GFX_W - 4, C, GFX_W - 300, C, 6); txc_frames(40);
        compose_display(); save_bmp("stack_settled.bmp");
        for (int s = 0; s <= 5; s++) { wos_pointer(C, 380 - s * 25, 1); txc_frame(); }
        compose_display(); save_bmp("stack_moving.bmp");
        wos_pointer(C, 255, 0); txc_frames(60);
        compose_display(); save_bmp("stack_settled2.bmp");
        return 0;
    }

    /* --txcheck damage|full OUT [DUMP_FRAME]: transitions, overlays and a face swap
     * through the stripe path on a virtual clock (see txc_frame). Compare the two
     * runs' OUT files; DUMP_FRAME saves that frame as a BMP. */
    if (argc > 3 && strcmp(argv[1], "--txcheck") == 0) {
        int C = GFX_W / 2;
        s_txc_full = !strcmp(argv[2], "full");
        s_txc_out = fopen(argv[3], "w");
        if (argc > 4) s_txc_dump = atoi(argv[4]);
        s_overlay = 0; s_pip_preview = 1; s_preview_ms = 1000;
        g_saved_face = 0;
        wos_set_stripes(s_test_stripes[0], s_test_stripes[1], 16, NULL, test_stripe_flush);
        wos_open_app(0); wos_invalidate(); txc_frames(30);
        wos_open_app(5); txc_frames(20);                 /* app open: horizontal sweep */
        wos_open_app(0); txc_frames(20);                 /* and back */
        txc_drag(C, 4, C, 300, 8); txc_frames(25);       /* top overlay, dragged open */
        wos_close_overlay(); txc_frames(25);             /* auto close */
        txc_drag(C, GFX_H - 4, C, GFX_H - 100, 6); txc_frames(25);   /* bottom: cancelled drag */
        txc_drag(GFX_W - 4, C, GFX_W - 300, C, 6); txc_frames(40);   /* app list overlay */
        txc_drag(C, 380, C, 200, 8); txc_frames(40);     /* scroll it */
        wos_pointer(C, C, 1); txc_frame(); wos_pointer(C, C, 0); txc_frames(30);   /* tap: open app */
        wos_open_app(0); txc_frames(25);
        txc_drag(360, C, 100, C, 6); txc_frames(30);     /* swipe the face (face crossfade) */
        txc_drag(4, C, 300, C, 6); txc_frames(40);       /* left launcher */
        wos_open_app(0); txc_frames(25);
        fclose(s_txc_out);
        printf("txcheck %s: %d frames\n", argv[2], s_txc_fr);
        return 0;
    }

    /* --scancheck N: drive the device stripe path in scan order N (0 rows, 1/2
     * column bands) through the app list (open, scroll, mid-drag hold, settle) and
     * the dial, checking each held frame against a clean in-place full render. */
    if (argc > 2 && strcmp(argv[1], "--scancheck") == 0) {
        static uint16_t snap[GFX_W * GFX_H];
        int C = GFX_W / 2, fails = 0, checks = 0;
        wos_scan_order = atoi(argv[2]);
        for (int i = 3; i < argc; i++) if (!strcmp(argv[i], "--list-pills")) { extern int applist_pills; applist_pills = 1; }
        s_overlay = 0;
        #define SC_STRIPES() wos_set_stripes(s_test_stripes[0], s_test_stripes[1], 16, NULL, test_stripe_flush)
        #define SC_CHECK(name) do {                                                   \
            memcpy(snap, s_fb, sizeof snap);                                          \
            wos_set_stripes(NULL, NULL, 0, NULL, NULL); wos_invalidate(); wos_run_frame(); \
            int bad = 0;                                                              \
            for (int i = 0; i < GFX_W * GFX_H; i++) {                                 \
                float px = i % GFX_W + 0.5f - GFX_W * 0.5f, py = i / GFX_W + 0.5f - GFX_H * 0.5f; \
                if (px * px + py * py <= GFX_W * GFX_W * 0.25f) bad += snap[i] != s_fb[i]; \
            }                                                                         \
            printf("scancheck %d %-14s %s (%d px differ)\n", wos_scan_order, name, bad ? "FAIL" : "ok", bad); \
            if (bad) { fails++; compose_display(); save_bmp("scancheck_clean.bmp");   \
                       memcpy(s_fb, snap, sizeof snap); compose_display(); save_bmp("scancheck_bad.bmp"); } \
            memcpy(s_fb, snap, sizeof snap); checks++; SC_STRIPES();                   \
        } while (0)
        SC_STRIPES();
        wos_invalidate(); wos_run_frame();
        SC_CHECK("face");
        wos_pointer(GFX_W - 8, C, 1); Sleep(20); wos_run_frame();
        for (int s = 1; s <= 6; s++) {
            wos_pointer((GFX_W - 8) + ((GFX_W - 300) - (GFX_W - 8)) * s / 6, C, 1);
            Sleep(16); wos_run_frame();
        }
        wos_pointer(GFX_W - 300, C, 0);
        for (int i = 0; i < 40; i++) { Sleep(16); wos_run_frame(); }
        SC_CHECK("list-open");
        for (int d = 0; d <= 120; d += 12) { wos_pointer(C, 380 - d, 1); Sleep(16); wos_run_frame(); }
        for (int i = 0; i < 4; i++) { Sleep(16); wos_run_frame(); }
        SC_CHECK("list-held");
        wos_pointer(C, 260, 0);
        for (int i = 0; i < 90; i++) { Sleep(16); wos_run_frame(); }
        SC_CHECK("list-settled");
        printf("scancheck %d: %d/%d checks passed\n", wos_scan_order, checks - fails, checks);
        return fails != 0;
    }

    /* --listdrag: open the app list, then drag slowly one row toward the next item and
     * one row back toward the previous, saving listdrag_<next|prev>_<k>.bmp at evenly
     * spaced finger offsets so the in-between motion can be inspected frame by frame. */
    if (argc > 1 && strcmp(argv[1], "--listdrag") == 0) {
        int C = GFX_W / 2;
        wos_pointer(GFX_W - 8, C, 1); Sleep(20); wos_run_frame();
        for (int s = 1; s <= 6; s++) {
            wos_pointer((GFX_W - 8) + ((GFX_W - 300) - (GFX_W - 8)) * s / 6, C, 1);
            Sleep(16); wos_run_frame();
        }
        wos_pointer(GFX_W - 300, C, 0);
        for (int i = 0; i < 60; i++) { Sleep(16); wos_run_frame(); }
        /* park on row 2 so both neighbours exist */
        for (int d = 0; d <= 280; d += 10) { wos_pointer(C, 400 - d, 1); Sleep(16); wos_run_frame(); }
        for (int i = 0; i < 10; i++) { Sleep(16); wos_run_frame(); }
        wos_pointer(C, 120, 0);
        for (int i = 0; i < 90; i++) { Sleep(16); wos_run_frame(); }
        static const char *dir[2] = { "next", "prev" };
        for (int pass = 0; pass < 2; pass++) {
            extern int applist_pills;
            int sgn = pass == 0 ? -1 : 1, span = applist_pills ? 104 : 140, shots = 6;
            for (int d = 0; d <= span; d += 4) {
                wos_pointer(C, C + sgn * d, 1); Sleep(16); wos_run_frame();
                if (d % (span / (shots - 1) / 4 * 4) == 0 || d == span) {
                    char nm[48]; snprintf(nm, sizeof nm, "listdrag_%s_%03d.bmp", dir[pass], d);
                    compose_display(); save_bmp(nm);
                }
            }
            for (int i = 0; i < 10; i++) { Sleep(16); wos_run_frame(); }
            wos_pointer(C, C + sgn * span, 0);
            for (int i = 0; i < 90; i++) { Sleep(16); wos_run_frame(); }
        }
        return 0;
    }

    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(0);
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.lpszClassName = "watchsim";
    RegisterClass(&wc);

    RECT r = { 0, 0, s_client, s_client };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    s_hwnd = CreateWindow("watchsim", "watch sim",
                          WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                          CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    ShowWindow(s_hwnd, SW_SHOW);
    update_ppi();
    apply_client_size();
    printf("monitor: %.1f ppi -> real watch size = %d px (press R to toggle)\n",
           s_ppi, s_real_px);

    hal_log("simulator running");

    /* main loop, paced precisely to 60 fps */
    timeBeginPeriod(1);   /* 1ms timer resolution so Sleep() is accurate */
    LARGE_INTEGER prev;
    QueryPerformanceCounter(&prev);
    for (;;) {
        double budget_ms = s_fps30 ? 1000.0 / 30.0 : 1000.0 / 60.0;   /* 'F' toggles */
        MSG msg;
        while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        alarm_service_tick();
        face_pip_service(0);
        wake_service_tick(hal_millis(),0);
        if (wos_run_frame()) {
            HDC dc = GetDC(s_hwnd);
            present(dc);
            ReleaseDC(s_hwnd, dc);
        }
        /* sleep only the remainder of the frame budget */
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double used = (double)(now.QuadPart - prev.QuadPart) * 1000.0 / s_qpf.QuadPart;
        if (used < budget_ms)
            Sleep((DWORD)(budget_ms - used));
        QueryPerformanceCounter(&prev);
    }
}
