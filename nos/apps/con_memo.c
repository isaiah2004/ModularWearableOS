/* con_memo.c — Recorder: voice memos and a noise meter (INMP441 mic, MAX98357A speaker).
 *
 * A three-page horizontal pager (page dots at the bottom):
 *   RECORDER  live waveform as mirrored solid bars that step left one column per
 *             100 ms (dim while monitoring, white while recording), the elapsed time
 *             in dot-matrix with the input level beside it (eight stacking triangles,
 *             one per 5 dB, separate from the duration), and the record button: a red
 *             dot (the screen's one red) that morphs into a rounded square while
 *             recording.
 *   MEMOS     saved recordings (on the device: /sdcard/watch/memos/ WAV files; in the
 *             simulator an in-memory list) with playback and a segmented progress bar.
 *   NOISE     live sound level as a big dot-matrix dB number, a 5 dB-per-dot meter
 *             whose dots past 85 dB are red (the overflow), and a state label.
 * Module-absent states for the mic (record, noise) and the speaker (playback).
 *
 * The memo service (svc_memo_*): a recording belongs to the HAL recorder, not to this
 * screen, so leaving Recorder keeps it going (like the track service) and the system
 * REC chip reads it from here. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "sports.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "prog.h"
#include "diag_log.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#ifdef ESP_PLATFORM
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern int board_sd_asset_acquire(void);
extern void board_sd_asset_release(void);
extern void board_sd_asset_lock(void);
extern void board_sd_asset_unlock(void);
#endif

#ifndef MEMO_DIR
#define MEMO_DIR  "/sdcard/watch/memos"
#endif
#define NPAGES    3
#define PAGE_W    466.0f

static float q(float x, float n) { return floorf(m_clamp(x, 0, 1) * n + 0.5f) / n; }

/* ------------------------------------------- content <-> absent swap */
/* Losing (or getting back) the phone or a module: the page on the glass leaves over 2
 * perceptual ink steps at 30 fps (56 %, 19 %: m_ink), then the other page builds from its
 * start: ui_absent's
 * icon from the centre and its text ladder, or the content's own entry build. Same
 * timings as the outdoor apps. */
#define PS_OUT_F 2
typedef struct { int shown, want; nos_time_t t_out, t_in; } pswap;
static void ps_init(pswap *s, int v) { s->shown = s->want = v; s->t_out = s->t_in = -10; }
static int ps_frames(const pswap *s) { return (int)lroundf((nos_t - s->t_out) * NOS_FPS); }
/* per tick with the live state; 1 on the frame the other page takes over (its build
 * clock t_in starts a frame back, so that first frame already shows the build) */
static int ps_tick(pswap *s, int v)
{
    if (v != s->want) { s->want = v; s->t_out = nos_t; }
    if (s->want != s->shown && ps_frames(s) >= PS_OUT_F) { s->shown = s->want; s->t_in = nos_t - 1.0f / NOS_FPS; return 1; }
    return 0;
}
/* ink of the page on the glass: 1 at rest, then m_ink(2/3), m_ink(1/3) while it leaves */
static float ps_ink(const pswap *s)
{
    if (s->want == s->shown) return 1.0f;
    return m_ink(1.0f - (float)(ps_frames(s) + 1) / (PS_OUT_F + 1));
}
/* dims what is drawn next while the page leaves (restore fb->dim with the return value).
 * The dim is in no primitive's hash, so the page is recorded once with the step. */
static int ps_dim(gfx_fb *fb, const pswap *s)
{
    int d0 = fb->dim;
    float k = ps_ink(s);
    if (k < 1) {
        if (fb->rec) gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H, gfx_hash_f(0x5A11u, k));
        nos_ink_dim(fb, k);
    }
    return d0;
}

static void body_line(gfx_fb *fb, int y, const char *s, uint16_t c)
{
    int x0, x1;
    ui_row_span((float)(y - 8), 40, &x0, &x1);
    ui_text_fit(fb, UI_F_BODY, UI_CX, y, s, c, UI_CENTER, x1 - x0);
}

static int s_keep_title;          /* a mic page fading out: its title stays at full ink */
static void page_title(gfx_fb *fb, const char *s) { if (!s_keep_title) ui_title(fb, s, 0); }

static void absent(gfx_fb *fb, const char *title, const char *what, float build)
{
    page_title(fb, title);
    ui_absent(fb, DOT_ICON_MIC, "Not connected", what, "MIC MODULE", build);
}

#define HOLD_S    (2.5f / NOS_FPS)   /* a quick tap shows its pressed fill >= 3 frames */

/* =========================================================== memo store ===== */
#define MEMO_MAX 24
typedef struct { char path[96]; char name[20]; uint32_t dur_ms; int64_t stamp; uint16_t year; uint8_t mo, d, h, mi; } memo;
static memo *s_memos;          /* PSRAM: internal DRAM is full */
static int   s_nmemo;
static int   s_seq;            /* next "Memo N" */

#ifndef ESP_PLATFORM
static void memo_add(const char *path, uint32_t dur, int mo, int d, int h, int mi)
{
    if (!s_memos) return;
    if (s_nmemo >= MEMO_MAX) s_nmemo = MEMO_MAX - 1;   /* drop only the oldest */
    /* newest first */
    memmove(&s_memos[1], &s_memos[0], sizeof(memo) * (size_t)s_nmemo);
    memo *m = &s_memos[0];
    memset(m, 0, sizeof *m);
    snprintf(m->path, sizeof m->path, "%s", path);
    snprintf(m->name, sizeof m->name, "Memo %d", ++s_seq);
    m->mo = (uint8_t)mo; m->d = (uint8_t)d; m->h = (uint8_t)h; m->mi = (uint8_t)mi;
    m->dur_ms = dur;
    hal_datetime now; hal_get_datetime(&now); m->year = (uint16_t)now.year;
    s_nmemo++;
}
#endif

static int64_t memo_days(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yo = (unsigned)(y - era * 400), mp = m > 2 ? m - 3 : m + 9;
    unsigned doy = (153 * mp + 2) / 5 + d - 1;
    return (int64_t)era * 146097 + yo * 365 + yo / 4 - yo / 100 + doy - 719468;
}

static void memo_date_at(memo *m, int64_t epoch, int tz_min)
{
    int64_t local = epoch + (int64_t)tz_min * 60;
    int64_t days = local >= 0 ? local / 86400 : (local - 86399) / 86400;
    int seconds = (int)(local - days * 86400);
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yo = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int y = (int)(yo + era * 400);
    unsigned doy = doe - (365 * yo + yo / 4 - yo / 100), mp = (5 * doy + 2) / 153;
    unsigned d = doy - (153 * mp + 2) / 5 + 1, mo = mp < 10 ? mp + 3 : mp - 9;
    y += mo <= 2;
    m->year = (uint16_t)y; m->mo = (uint8_t)mo; m->d = (uint8_t)d;
    m->h = (uint8_t)(seconds / 3600); m->mi = (uint8_t)(seconds / 60 % 60);
}

#ifndef ESP_PLATFORM
static void memo_date(memo *m, int64_t epoch) { memo_date_at(m, epoch, nos_prefs.tz_min); }
#endif

#ifdef ESP_PLATFORM
static struct memo_scan_result {
    memo items[MEMO_MAX];
    int count, total, ok, tz_min;
    volatile int state;                /* 1 scanning, 2 ready for UI publication */
} *s_scan;
static int s_scan_again;

static int memo_scan(memo *items, int *count, int *total, int tz_min)
{
    (*count) = 0; (*total) = 0;
    board_sd_asset_lock();
    DIR *dir = opendir(MEMO_DIR);
    board_sd_asset_unlock();
    if (!dir) return 0;
    struct dirent *e;
    /* Visit every finalized WAV; keep only the newest 24 in bounded memory. */
    int ok = 1, visited = 0;
    for (;;) {
        board_sd_asset_lock();
        errno = 0; e = readdir(dir);
        if (!e && errno) ok = 0;
        board_sd_asset_unlock();
        if (!e) break;
        if (++visited % 8 == 0) vTaskDelay(1);
        size_t l = strlen(e->d_name);
        if (l < 5 || l > 48 || strcmp(e->d_name + l - 4, ".wav")) continue;
        memo item; memset(&item, 0, sizeof item);
        int length = snprintf(item.path, sizeof item.path, MEMO_DIR "/%s", e->d_name);
        if (length < 0 || (size_t)length >= sizeof item.path) continue;
        struct stat st;
        board_sd_asset_lock();
        int found = stat(item.path, &st);
        board_sd_asset_unlock();
        if (found || !S_ISREG(st.st_mode) || st.st_size < 44) continue;
        item.dur_ms = (uint32_t)((st.st_size - 44) / 32);
        int y, mo, d, h, mi, sec, suffix;
        if (sscanf(e->d_name, "M_%4d%2d%2d_%2d%2d%2d_%4d.wav", &y, &mo, &d, &h, &mi, &sec, &suffix) == 7 &&
            y >= 1970 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h < 24 && mi < 60 && sec < 60) {
            item.stamp = memo_days(y, (unsigned)mo, (unsigned)d) * 86400 + h * 3600 + mi * 60 + sec;
            memo_date_at(&item, item.stamp, tz_min);
        } else {
            item.stamp = st.st_mtime;
            if (item.stamp >= 1577836800) memo_date_at(&item, item.stamp, tz_min);
            if (sscanf(e->d_name, "M_%2d%2d_%2d%2d", &mo, &d, &h, &mi) == 4) {
                item.mo = (uint8_t)mo; item.d = (uint8_t)d; item.h = (uint8_t)h; item.mi = (uint8_t)mi;
            }
        }
        (*total)++;
        int at = 0;
        while (at < (*count) && (items[at].stamp > item.stamp ||
               (items[at].stamp == item.stamp && strcmp(items[at].path, item.path) > 0))) at++;
        if (at >= MEMO_MAX) continue;
        if ((*count) < MEMO_MAX) (*count)++;
        memmove(&items[at + 1], &items[at], (size_t)((*count) - at - 1) * sizeof(memo));
        items[at] = item;
    }
    board_sd_asset_lock();
    if (closedir(dir)) ok = 0;
    board_sd_asset_unlock();
    for (int i = 0; i < (*count); i++) snprintf(items[i].name, sizeof items[i].name, "Memo %d", (*total) - i);
    return ok;
}

static void memo_scan_task(void *arg)
{
    struct memo_scan_result *scan = arg;
    scan->ok = 0;
    if (board_sd_asset_acquire()) {
        scan->ok = memo_scan(scan->items, &scan->count, &scan->total, scan->tz_min);
        board_sd_asset_release();
    }
    __atomic_store_n(&scan->state, 2, __ATOMIC_RELEASE);
    vTaskDelete(NULL);
}
#endif

static void memo_load(void)
{
    if (!s_memos) s_memos = (memo *)hal_big_alloc(sizeof(memo) * MEMO_MAX);
    if (!s_memos) return;
#ifdef ESP_PLATFORM
    if (!s_scan) {
        s_scan = hal_big_alloc(sizeof *s_scan);
        if (s_scan) memset(s_scan, 0, sizeof *s_scan);
    }
    if (!s_scan) return;
    if (__atomic_load_n(&s_scan->state, __ATOMIC_ACQUIRE)) { s_scan_again = 1; return; }
    s_scan->tz_min = nos_prefs.tz_min;
    __atomic_store_n(&s_scan->state, 1, __ATOMIC_RELEASE);
    if (xTaskCreate(memo_scan_task, "memo_scan", 4096, s_scan, 1, NULL) != pdPASS)
        __atomic_store_n(&s_scan->state, 0, __ATOMIC_RELEASE);
#else
    if (s_nmemo || s_seq) return;          /* the simulator keeps its list in memory */
    memo_add("/sdcard/watch/memos/M_1010_1832.wav", 42000, 10, 10, 18, 32);
    memo_add("/sdcard/watch/memos/M_1011_0841.wav", 8000, 10, 11, 8, 41);
    memo_add("/sdcard/watch/memos/M_1011_0954.wav", 127000, 10, 11, 9, 54);
#endif
}

/* =============================================================== state ===== */
#define WAVE_COLS  23
#define WAVE_HALF  4               /* dots above and below the centre line */
#define WAVE_PITCH 12.0f
#define WAVE_STEP_F 3              /* frames per column (10 Hz) ... */
#define WAVE_SPEED (WAVE_PITCH / WAVE_STEP_F)   /* ... so the wave scrolls 4 whole px a frame */
#define WAVE_CY    150.0f
#define WAVE_BAR   6.0f                /* a wave bar: 6 px wide, 6 px + 10 px a level tall */
#define WAVE_LV    10.0f
#define LVL_N      8                   /* input level: eight triangles ... */
#define LVL_W      12.0f               /* ... 12 px base ... */
#define LVL_H      8.0f                /* ... 8 px high ... */
#define LVL_GAP    3.0f                /* ... 3 px apart, stacked up beside the time */
#define LVL_DB0    36.0f               /* the first lights at 41 dB, one more every 5 dB */
#define REC_CY     356.0f
#define REC_R      42.0f
#define TIME_Y     212.0f

static struct {
    nos_time_t t_enter; float build, txt;
    /* pager */
    mspring px;
    int   page, dragging;
    float grab_x;
    /* mic */
    int   mic, spk;
    uint8_t wave[WAVE_COLS];     /* 0..WAVE_HALF per column */
    int   wave_frame;
    float db, db_shown;          /* smoothed level, the number on screen */
    float db_raw;                /* the level the meter is heading to */
    char  dbs[8], dbs_prev[8];
    nos_time_t t_db; float db_morph;
    int   band, band_prev;       /* QUIET .. HEARING RISK of the shown number; -1 none */
    nos_time_t t_band;
    int   meter_frame;
    float meter;                 /* dB on the meter (10 Hz) */
    int   meter_dots;            /* dots lit on the glass: one a frame toward the meter */
    int   lvl;                   /* input-level triangles lit: one a frame toward the level */
    int   touching;              /* a finger is down (it holds a caught pager) */
    /* recording */
    int   rec;
    int   pressed;
    nos_time_t t_down;                /* record button press start */
    uint32_t rec_ms;
    nos_time_t t_rec;                 /* start/stop moment (morph) */
    char  ts[12], ts_prev[12];
    nos_time_t t_ts; float ts_morph;
    nos_time_t t_saved;
    uint32_t saved_ms;
    int   rec_failed;
    /* playback */
    ui_list list;
    int   list_inited;
    int   playing;               /* memo index, -1 none */
    nos_time_t t_play;
    uint32_t play_ms;
    int   real_play;             /* the HAL is actually playing it */
    int   rec_lock;              /* a sideways drag while recording: rubber band only */
    int   row_held;              /* memo row whose fill holds after a quick tap */
    nos_time_t row_t;
    /* the mic pages (Recorder, Noise): their own build clock, restarted when the mic
     * module goes or comes back (pswap: 1 = content, 0 = absent page) */
    pswap msw;
    nos_time_t m0; float mb, mt;
} R = { .playing = -1, .row_held = -1 };

static int on_page(int p) { return fabsf(R.px.x - p * PAGE_W) < PAGE_W; }

/* ----------------------------------------------------------- memos list */
static void play_stop(void)
{
    if (R.playing < 0) return;
    if (R.real_play) hal_audio_play_stop();
    R.playing = -1;
    R.real_play = 0;
}

static void memo_poll(void)
{
#ifdef ESP_PLATFORM
    if (!s_scan || __atomic_load_n(&s_scan->state, __ATOMIC_ACQUIRE) != 2) return;
    if (s_scan->ok) {
        play_stop();                    /* indices are from the old immutable snapshot */
        memcpy(s_memos, s_scan->items, (size_t)s_scan->count * sizeof(memo));
        s_nmemo = s_scan->count; s_seq = s_scan->total;
        if (R.list_inited) ui_list_set_n(&R.list, s_nmemo);
    } else if (nos_top() == &scr_memo) nos_toast("LIBRARY ERROR");
    __atomic_store_n(&s_scan->state, 0, __ATOMIC_RELEASE);
    if (s_scan_again) { s_scan_again = 0; memo_load(); }
#endif
}

#define MCARD_X0 53                 /* fixed card: 360 px wide, r 40 */
#define MCARD_W  360

/* "6 s" under a minute, else "2:07" (never confusable with a clock time). Units keep
 * their case (ui_label_unit): the caller draws it with ui_text + label tracking. */
static void fmt_len(char *b, int n, uint32_t ms)
{
    uint32_t s = ms / 1000;                    /* floored, as the hero clock counts */
    if (s < 60) snprintf(b, (size_t)n, "%u s", (unsigned)s);
    else if (s < 3600) snprintf(b, (size_t)n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    else snprintf(b, (size_t)n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
}
#define LABEL_TRACK 2               /* ui_label's tracking: these lines match the caps labels */

static float row_ink(int k) { return m_ink(m_clamp((float)(k + 1) / 4.0f, 0, 1)); }

static void memo_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    if (!s_memos || i >= s_nmemo) return;
    const memo *m = &s_memos[i];
    /* rows fade in one frame apart (whole frames), 4 perceptual ink steps each */
    float k = fade * row_ink(m_stagger_k(m_frame_k(R.t_enter), i, 1));
    if (k <= 0) return;
    pressed = pressed || (R.row_held == i && nos_t - R.row_t < HOLD_S);
    ui_rrect(fb, (float)MCARD_X0, (float)(y + 4), (float)MCARD_W, (float)(h - 8), 40,
             ui_dim(pressed ? C_LINE2 : C_SURF2, k));
    int playing = R.playing == i;
    float bx = (float)MCARD_X0 + 44, by = (float)y + h * 0.5f;
    /* no speaker: the play buttons go quiet */
    uint16_t disc = playing ? C_WHITE : R.spk ? C_SURF3 : C_SURF;
    uint16_t ink = playing ? C_BG : R.spk ? C_PRI : C_DIS;
    ui_circle(fb, bx, by, 24, ui_dim(disc, k));
    int icon = playing ? DOT_ICON_PAUSE : DOT_ICON_PLAY;
    dot_icon(fb, icon, bx - (playing ? 0 : 2.2f), by, 2.4f, 1.9f, playing ? ink : ui_dim(ink, k));
    int tx = MCARD_X0 + 84, tr = UI_VALUE_X;
    ui_text_fit(fb, UI_F_BODY, tx, y + 38, m->name, ui_dim(C_PRI, k), UI_LEFT, tr - tx);
    char len[16], b[40];
    if (playing && m->dur_ms) {
        /* progress: 8 solid segments (13 x 6 px), the leading one growing along its
         * length, the elapsed time beside them */
        float p = m_clamp((float)R.play_ms / (float)m->dur_ms, 0, 1);
        prog_bar(fb, PROG_SEGMENTS, (float)tx - 1.7f, (float)(y + 60), 136.0f, 6.0f, 8, p, C_WHITE, C_LINE2);
        fmt_len(len, sizeof len, R.play_ms);
        ui_text(fb, UI_F_LABEL, tr, y + 69, len, ui_dim(C_SEC, k), UI_RIGHT, LABEL_TRACK);
    } else {
        /* length, then the time if it was today, else the day */
        char w[12];
        hal_datetime now;
        hal_get_datetime(&now);
        if (now.year == m->year && now.month == m->mo && now.day == m->d) ui_fmt_time(w, sizeof w, m->h, m->mi);
        else snprintf(w, sizeof w, "%d %s", m->d, ui_month3(m->mo));
        fmt_len(len, sizeof len, m->dur_ms);
        snprintf(b, sizeof b, "%s \xB7 %s", len, w);
        ui_text(fb, UI_F_LABEL, tx, y + 69, b, ui_dim(C_DIS, k), UI_LEFT, LABEL_TRACK);
    }
}

static void memo_tap(int i)
{
    if (!s_memos || i < 0 || i >= s_nmemo) return;
    R.row_held = i; R.row_t = R.list.press_t;        /* the fill stays >= 3 frames */
    if (R.playing == i) { play_stop(); return; }
    play_stop();
    if (!R.spk) { nos_feedback(FB_ERROR); nos_toast("NO SPEAKER"); return; }
    int ok = hal_audio_play_file(s_memos[i].path);
#ifdef ESP_PLATFORM
    if (!ok) { nos_feedback(FB_ERROR); nos_toast("CAN'T PLAY"); return; }
#endif
    R.real_play = ok;
    R.playing = i;
    R.t_play = nos_t;
    R.play_ms = 0;
}

/* ------------------------------------------------------- memo service */
static int      s_rec_open;     /* a recording this app started has not been saved yet */
static uint32_t s_saved_ms;     /* length of the last saved memo */
static int      s_rec_stopping;
static char     s_rec_path[96];
static hal_datetime s_rec_date;
static int64_t  s_rec_epoch;
static struct tm s_rec_stamp;
static unsigned s_rec_suffix;
static int s_rec_retry;
/* Session IDs survive screen leave and HAL finalization. Never log paths/audio. */
static uint32_t s_rec_generation, s_rec_request, s_rec_stop_request;

static int rec_queue(void)
{
    for (int attempt = 0; attempt < 16 && s_rec_suffix < 10000; attempt++) {
        snprintf(s_rec_path, sizeof s_rec_path, MEMO_DIR "/M_%04d%02d%02d_%02d%02d%02d_%04u.wav",
                 s_rec_stamp.tm_year + 1900, s_rec_stamp.tm_mon + 1, s_rec_stamp.tm_mday,
                 s_rec_stamp.tm_hour, s_rec_stamp.tm_min, s_rec_stamp.tm_sec, s_rec_suffix);
        int result = hal_rec_start_exclusive(s_rec_path);
        if (result != -2) return result;
        s_rec_suffix++;
    }
    return s_rec_suffix < 10000 ? -2 : 0;
}

/* The HAL recorder has stopped (by us or by itself): add the memo, return its number. */
static int rec_finish(void)
{
    int status = hal_rec_status();
    if (s_rec_retry && s_rec_stopping) status = 4; /* no new file was ever accepted */
    if (s_rec_open && (s_rec_retry || (status == 4 && hal_rec_result() == HAL_REC_EXISTS)) && !s_rec_stopping) {
        if (!s_rec_retry) s_rec_suffix++;
        int result = rec_queue();
        s_rec_retry = result == -2;
        if (result == 1 || s_rec_retry) return 0;
        status = 4;
    }
    if (!s_rec_open || (status != 3 && status != 4)) return 0;
    int detail = status == 3 ? HAL_REC_DONE : HAL_REC_ERROR;
    int outcome = status == 3 ? DIAG_RESULT_SUCCESS : DIAG_RESULT_FAILED;
    diag_action_result(s_rec_request, outcome, detail);
    if (s_rec_stop_request) diag_action_result(s_rec_stop_request, outcome, detail);
    s_rec_request = s_rec_stop_request = 0;
    float event[]={2,0};sports_emit(SP_ROUTINE,event,2);
    uint32_t ms = hal_rec_ms();
    s_rec_open = 0;
    s_rec_stopping = 0;
    s_rec_retry = 0;
    R.rec = 0; R.t_rec = nos_t; R.t_saved = nos_t;
    R.rec_failed = status == 4;
    R.saved_ms = ms;
    if (status == 4) {
        nos_feedback(FB_ERROR); nos_toast("SAVE FAILED"); return 0;
    }
    s_saved_ms = ms;
#ifdef ESP_PLATFORM
    memo_load();                     /* the file is on the card now */
#else
    {
        memo_add(s_rec_path, ms, s_rec_date.month, s_rec_date.day, s_rec_date.hour, s_rec_date.minute);
        if (s_nmemo) { s_memos[0].stamp = s_rec_epoch; memo_date(&s_memos[0], s_rec_epoch); }
    }
#endif
    if (R.list_inited) ui_list_set_n(&R.list, s_nmemo);
    return s_seq ? s_seq : 1;
}

/* A save the user can't see in Recorder: say it on whatever is showing. Toast copy
 * stays within 10 caps so the pill keeps the top of the glass: "SAVED 0:04". */
static void saved_toast(int n)
{
    (void)n;
    char b[24];
    uint32_t s = s_saved_ms / 1000;
    if (s < 600) snprintf(b, sizeof b, "SAVED %u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    else snprintf(b, sizeof b, "MEMO SAVED");
    nos_toast(b);
}

int svc_memo_active(void) { return s_rec_open; }
uint32_t svc_memo_elapsed_ms(void) { return svc_memo_active() ? hal_rec_ms() : 0; }
int svc_memo_number(void) { return s_seq + 1; }          /* "Memo n" it will be saved as */

/* Stop and save a background recording (the REC chip); returns the memo number, 0 if
 * nothing was recording. */
int svc_memo_stop(void)
{
    if (!s_rec_open || s_rec_stopping) return 0;
    s_rec_stop_request = diag_action_request(DIAG_DOMAIN_MEMO, DIAG_ACT_STOP, (int32_t)s_rec_generation);
    diag_action_context(s_rec_stop_request, (int32_t)diag_log_name_id("RECORDER"), hal_rec_status());
    hal_rec_stop();
    s_rec_stopping = 1;
    diag_action_result(s_rec_stop_request, DIAG_RESULT_PENDING, hal_rec_status());
    nos_toast("SAVING");
    return 1;                         /* stop accepted; completion is asynchronous */
}

/* Per frame (svc_tick): a background recording that ends by itself (card full) is
 * saved and announced. Recorder handles its own while it is on top. */
void svc_memo_tick(float dt)
{
    (void)dt;
    memo_poll();
    if (s_rec_open) {
        int n = rec_finish();
        if (n) { nos_feedback(FB_SUCCESS); if (nos_top() != &scr_memo) saved_toast(n); }
    }
}

/* --------------------------------------------------------------- enter */
static void memo_enter(void)
{
    /* revealed by back (a call or an alarm went away): no rebuild, same page */
    int back = nos_revealed();
    R.t_enter = back ? nos_t - 10.0f : nos_enter_t();   /* build from 40% of the opening iris */
    R.build = R.txt = back ? 1.0f : 0.0f;
    if (!back) { msp_init_px(&R.px, 0, SPRING_PAGE); R.page = 0; R.meter_dots = 0; }
    R.dragging = 0;
    R.touching = 0;
    R.pressed = 0;
    R.t_down = -10;
    R.row_held = -1;
    svc_memo_tick(0);
    R.rec = svc_memo_active();             /* ... or still running: pick it up */
    R.rec_ms = R.rec ? hal_rec_ms() : 0;
    R.t_rec = -10;
    R.t_saved = -10;
    R.rec_failed = 0;
    float event[]={2,1};sports_emit(SP_ROUTINE,event,2);
    if (!back) {
        R.ts[0] = R.ts_prev[0] = 0;
        R.dbs[0] = R.dbs_prev[0] = 0;
        R.band = R.band_prev = -1;
        R.t_band = -10;
        R.db = R.db_shown = R.db_raw = 0;
        R.meter = 0;
        memset(R.wave, 0, sizeof R.wave);
    }
    R.mic = (hal_ext_present() & EXT_MIC) != 0;
    ps_init(&R.msw, R.mic);
    R.m0 = R.t_enter;
    memo_load();
    if (!R.list_inited) { ui_list_init(&R.list, s_nmemo, UI_ROW_H, 104, memo_row, memo_tap); R.list_inited = 1; }
    else ui_list_set_n(&R.list, s_nmemo);
    R.list.margin = 30;
    R.playing = -1;
}

static void rec_stop(void)
{
    if (R.rec) svc_memo_stop();
}

static void rec_start(void)
{
    uint32_t request = diag_action_request(DIAG_DOMAIN_MEMO, DIAG_ACT_START, (int32_t)(s_rec_generation + 1u));
    diag_action_context(request, (int32_t)diag_log_name_id("RECORDER"), hal_rec_status());
    if (s_rec_open) {
        diag_action_result(request, DIAG_RESULT_REJECTED, hal_rec_status());
        nos_toast("SAVING"); return;
    }
    hal_datetime dt;
    hal_get_datetime(&dt);
    s_rec_date = dt;
    s_rec_epoch = (int64_t)hal_epoch_seconds();
    if (!s_rec_epoch) s_rec_epoch = memo_days(dt.year, (unsigned)dt.month, (unsigned)dt.day) * 86400 +
        dt.hour * 3600 + dt.minute * 60 + dt.second - (int64_t)nos_prefs.tz_min * 60;
    time_t utc = (time_t)s_rec_epoch;
    struct tm *v = gmtime(&utc);
    if (!v) { diag_action_result(request, DIAG_RESULT_REJECTED, -3); R.rec_failed = 1; R.t_saved = nos_t; nos_feedback(FB_ERROR); return; }
    s_rec_stamp = *v; s_rec_suffix = 0;
    play_stop();
    int result = rec_queue();
    s_rec_retry = result == -2;
    if (result != 1 && !s_rec_retry) {
        diag_action_result(request, DIAG_RESULT_REJECTED, result);
        R.rec_failed = 1;
        R.t_saved = nos_t;
        nos_feedback(FB_ERROR);
        return;
    }
    R.rec_failed = 0;
    R.rec = 1;
    s_rec_open = 1;
    s_rec_generation++;
    s_rec_request = request;
    s_rec_stop_request = 0;
    diag_action_result(request, DIAG_RESULT_PENDING, result);
    s_rec_stopping = 0;
    R.rec_ms = 0;
    R.t_rec = nos_t;
    nos_feedback(FB_TOGGLE_ON);           /* the monitored wave carries on, now in white */
}

static void memo_leave(void)
{
    /* A recording keeps going in the background and R.rec stays as it is: the page
     * still on the glass during the transition reads RECORDING. Covered (a call, Find
     * watch, an alarm) it says nothing: the REC chip already shows it, and the toast
     * would sit on the caller's name. Leaving for good says it once. */
    if (!nos_leaving_covered() && R.rec && svc_memo_active()) nos_toast("RECORDING");
    play_stop();
}

/* ---------------------------------------------------------------- tick */
static int s_wave_steps;                 /* columns pushed (the audit's scroll position) */
static void push_wave(uint8_t lvl)
{
    s_wave_steps++;
    memmove(R.wave, R.wave + 1, WAVE_COLS - 1);
    R.wave[WAVE_COLS - 1] = lvl;
}

static int band_of(float db) { return db < 40 ? 0 : db < 70 ? 1 : db < 85 ? 2 : 3; }

/* The record button's red: a dot (52 px, r 26) <-> a rounded square (34 px, r 8) on a
 * 4-frame table (45 78 95 100 %), the tap's own frame first. Returns 0 dot .. 1 square. */
static float rec_sq(void)
{
    float m = mf_at(&MF_MICRO4, m_frame_k(R.t_rec) + 1);
    return R.rec ? m : 1.0f - m;
}

/* Motion audit (sim only, docs/NOS_SIM.md): what draw() puts on the glass this frame. */
static void memo_probes(void)
{
#ifdef GFX_COST_MODEL
    nos_motion_probe("memo.pager", R.dragging || R.rec_lock ? MC_FINGER : MC_MOMENTUM, (float)lroundf(R.px.x));
    nos_motion_probe("memo.swap.ink", MC_FADE, 100.0f * ps_ink(&R.msw));
    if (!R.msw.shown || nos_t < R.m0) return;
    if (on_page(0)) {
        nos_motion_probe("memo.wave", MC_TRACKED, (float)s_wave_steps * WAVE_PITCH + (float)R.wave_frame * WAVE_SPEED);
        nos_motion_probe("memo.rec.half", MC_MICRO, m_lerp(52.0f, 34.0f, rec_sq()) * 0.5f);
    }
    if (on_page(0)) nos_motion_probe("memo.level", MC_DECORATIVE, (float)R.lvl * (LVL_H + LVL_GAP));
    if (on_page(2)) nos_motion_probe("memo.meter", MC_DECORATIVE, (float)R.meter_dots * 12.0f);
    if (on_page(1) && s_nmemo && nos_t + 0.5f / NOS_FPS >= R.t_enter)
        nos_motion_probe("memo.row.ink0", MC_FADE, 100.0f * row_ink(m_frame_k(R.t_enter)));
#endif
}

static void memo_tick(float dt)
{
    int frames = (int)lroundf(dt * NOS_FPS);
    if (frames < 1) frames = 1;
    uint32_t pres = hal_ext_present();
    R.mic = (pres & EXT_MIC) != 0;
    R.spk = (pres & EXT_SPEAKER) != 0;
    R.build = q((nos_t - R.t_enter) / UI_ENTRY_S, 32);
    R.txt = nos_t >= R.t_enter ? 1.0f : 0.0f;
    ps_tick(&R.msw, R.mic);                      /* mic gone or back: the pages swap */
    R.m0 = R.msw.t_in > R.t_enter ? R.msw.t_in : R.t_enter;
    R.mb = q((nos_t - R.m0) / UI_ENTRY_S, 32);
    R.mt = nos_t >= R.m0 ? 1.0f : 0.0f;

    /* pager (a pixel spring: it lands with a 1 px snap). A pager caught mid-settle by a
     * touch that did not become a drag goes on to its page once the finger lifts. */
    if (!R.dragging && !R.rec_lock && !R.touching && !R.px.moving) {
        float t = floorf(R.px.x / PAGE_W + 0.5f);
        t = m_clamp(t, 0, NPAGES - 1) * PAGE_W;
        if (R.px.x != t) msp_to(&R.px, t);
    }
    if (!R.dragging && msp_step(&R.px, dt)) {}
    int p = (int)floorf(R.px.x / PAGE_W + 0.5f);
    if (p < 0) p = 0;
    if (p >= NPAGES) p = NPAGES - 1;
    if (p != R.page) { R.page = p; nos_feedback(FB_TICK); }

    /* mic: only while a mic page is on the glass or a recording runs (it costs power) */
    if (R.mic && (R.rec || on_page(0) || on_page(2))) {
        float db = hal_mic_db();
        if (db >= 0) { R.db_raw = db; R.db += (db - R.db) * m_clamp(dt * 8.0f, 0, 1); }
        R.wave_frame += frames;
        while (R.wave_frame >= WAVE_STEP_F) {    /* 10 Hz: one column per step */
            R.wave_frame -= WAVE_STEP_F;
            uint8_t w[32];
            int lvl = 0;
            if (hal_mic_wave(w) > 0) {
                int pk = 0;
                for (int i = 28; i < 32; i++) if (w[i] > pk) pk = w[i];
                lvl = (pk * (WAVE_HALF + 1)) / 256;
            }
            push_wave((uint8_t)(lvl > WAVE_HALF ? WAVE_HALF : lvl));
        }
        R.meter_frame += frames;
        if (R.meter_frame >= 3) { R.meter_frame = 0; R.meter = R.db; }
        /* the number: 1 Hz with a digit morph, but at once when the meter settles in
         * another band, so the hero, the label and the red dots always agree. A band the
         * meter only passes through (49 -> 92 crosses LOUD) is never named: the number
         * waits until the meter is in the band it is heading to. */
        int bm = band_of(R.meter), bs = band_of(R.db_shown);
        int settled = bm == band_of(R.db_raw);
        int cross = R.dbs[0] && bm != bs && settled;
        int due = nos_t - R.t_db >= 1.0f && (settled || bm == bs || nos_t - R.t_db >= 2.0f);
        if (due || !R.dbs[0] || cross) {
            char b[8];
            int v = (int)floorf(R.meter);           /* floored like the meter's dots */
            if (v < 0) v = 0;
            if (v > 140) v = 140;
            snprintf(b, sizeof b, "%d", v);
            if (strcmp(b, R.dbs)) {
                if (R.dbs[0] && strlen(b) == strlen(R.dbs)) memcpy(R.dbs_prev, R.dbs, sizeof R.dbs);
                else R.dbs_prev[0] = 0;
                memcpy(R.dbs, b, sizeof b);
                R.t_db = nos_t;
            } else R.t_db = nos_t - 0.75f;         /* unchanged: look again in 0.25 s */
            R.db_shown = (float)v;
            int bd = band_of(R.db_shown);
            if (bd != R.band) { R.band_prev = R.band; R.band = bd; R.t_band = nos_t; }
        }
        R.db_morph = R.dbs_prev[0] ? m_clamp((float)(m_frame_k(R.t_db) + 1) / 8.0f, 0, 1) : 1.0f;
    }
    /* the meter's dots follow the level one dot a frame (12 px: a discrete fill that never
     * jumps, like a VU's ballistics); a mic page that builds fills it from empty */
    {
        int tgt = R.meter > 0 ? (int)floorf(m_clamp(R.meter, 0, 119) / 5.0f) + 1 : 0;
        if (nos_t < R.m0) R.meter_dots = 0;
        else if (R.meter_dots < tgt) R.meter_dots++;
        else if (R.meter_dots > tgt) R.meter_dots--;
        /* the Recorder's input level: the live (smoothed) dB, 5 dB a triangle, one
         * triangle a frame toward it (rises in at most 8 frames from the page's build) */
        int lt = R.mic ? (int)floorf(m_clamp((R.db - LVL_DB0) / 5.0f, 0, LVL_N)) : 0;
        if (nos_t < R.m0) R.lvl = 0;
        else if (R.lvl < lt) R.lvl++;
        else if (R.lvl > lt) R.lvl--;
    }

    /* recording clock */
    svc_memo_tick(dt);
    if (s_rec_open) { R.rec_ms = hal_rec_ms(); nos_keep_awake(); }
    char t[12];
    if (R.rec || nos_t - R.t_saved < 1.6f) ui_fmt_dur(t, sizeof t, R.rec ? R.rec_ms : R.saved_ms, 0);
    else snprintf(t, sizeof t, UI_NONE ":" UI_NONE);   /* at rest: no 00:00 */
    if (strcmp(t, R.ts)) {
        if (R.ts[0] && strlen(t) == strlen(R.ts)) { memcpy(R.ts_prev, R.ts, sizeof R.ts); R.t_ts = nos_t; }
        else R.ts_prev[0] = 0;
        memcpy(R.ts, t, sizeof t);
    }
    R.ts_morph = R.ts_prev[0] ? m_clamp((float)(m_frame_k(R.t_ts) + 1) / 6.0f, 0, 1) : 1.0f;

    /* playback */
    if (R.playing >= 0) {
        uint32_t dur = s_memos[R.playing].dur_ms;
        if (R.real_play) {
            R.play_ms = hal_audio_play_pos_ms();
            if (!hal_audio_playing()) { R.playing = -1; R.real_play = 0; }
        } else {
            R.play_ms = (uint32_t)((nos_t - R.t_play) * 1000.0f);
            if (R.play_ms >= dur) R.playing = -1;
        }
        nos_keep_awake();
    }
    ui_list_tick(&R.list, dt);
    memo_probes();
}

/* ---------------------------------------------------------------- draw */
/* Mirrored solid bars about a centre line: one damage command, bars filled in raster.
 * The wave scrolls left at a whole 4 px a frame (R6: a tracked scroll of fine detail at a
 * whole-pixel speed, a third of the pitch), a new column every 3 frames: the newest fades
 * in at the right edge and the oldest out at the left (perceptual steps), so the scroll
 * runs on without a column popping in or out. */
static void wave_draw(gfx_fb *fb, float x, float cy, uint16_t on, uint16_t line, float build)
{
    const float d = WAVE_BAR, e = d * 0.5f + 1.0f;
    float w = WAVE_COLS * WAVE_PITCH, top = cy - WAVE_HALF * WAVE_LV - e, bot = cy + WAVE_HALF * WAVE_LV + e;
    int nb = (int)(build * WAVE_COLS + 0.5f);   /* columns built so far, from the centre out */
    int ph = R.wave_frame;                      /* 0..2: frames into this column */
    float off = (float)ph * WAVE_SPEED;
    x += WAVE_SPEED;                            /* centred on the scroll's middle phase */
    if (fb->rec) {
        unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_u(gfx_hash_u(0x3AFEu, on), line), (unsigned)nb), (unsigned)ph);
        for (int c = 0; c < WAVE_COLS; c++) h = gfx_hash_u(h, R.wave[c]);
        gfx_rec_cmd(fb, (int)floorf(x - off - e), (int)floorf(top), (int)ceilf(x + w + e), (int)ceilf(bot), h);
        return;
    }
    int lo = fb->y_base - fb->oy, hi = lo + fb->h;      /* logical rows in this stripe */
    if (bot < lo || top > hi) return;
    int mid = WAVE_COLS / 2;
    float k_old = m_ink(1.0f - (float)ph / WAVE_STEP_F), k_new = m_ink((float)(ph + 1) / WAVE_STEP_F);
    for (int c = 0; c < WAVE_COLS; c++) {
        if (abs(c - mid) * 2 >= nb) continue;
        float cx = x + (c + 0.5f) * WAVE_PITCH - off;
        float k = c == 0 ? k_old : c == WAVE_COLS - 1 ? k_new : 1.0f;
        uint16_t con = k < 1 ? ui_dim(on, k) : on, cli = k < 1 ? ui_dim(line, k) : line;
        int lv = R.wave[c];
        /* half height: a capsule; silence is a 3 px dash on the centre line */
        float hh = lv ? d * 0.5f + (float)lv * WAVE_LV : 1.5f;
        if (cy + hh + 1 < lo || cy - hh - 1 > hi) continue;
        ui_rrect(fb, cx - d * 0.5f, cy - hh, d, 2 * hh, hh < d * 0.5f ? hh : d * 0.5f, lv ? con : cli);
    }
}

/* The input level: LVL_N triangles stacked upwards from `bot` (x = the stack's centre),
 * the lowest first; each one polygon, so a level change repaints the triangles it
 * crosses. The off ones are the track. */
static void level_draw(gfx_fb *fb, float x, float bot, int lit, uint16_t on, uint16_t off)
{
    for (int i = 0; i < LVL_N; i++) {
        uint16_t c = i < lit ? on : off;
        if (!c) continue;
        float yb = bot - (float)i * (LVL_H + LVL_GAP);
        float xy[6] = { x - LVL_W * 0.5f, yb, x + LVL_W * 0.5f, yb, x, yb - LVL_H };
        prog_poly(fb, xy, 3, c);
    }
}

/* Start and stop run on one 4-frame clock with the button (the tap's own frame first):
 * colours take 4 steps, words leave over 2 perceptual steps and the new ones arrive over
 * the next 2. sw_x: 0..1 through a swap that started at t0, in whole frames. */
#define SWAP_F 4
#define SWAP_S M_FRAMES(SWAP_F)
static float sw_x(nos_time_t t0) { return m_clamp((float)(m_frame_k(t0) + 1) / SWAP_F, 0, 1); }
static float rec_x(void) { return sw_x(R.t_rec); }                                /* 1 = settled */
static float rec_look(void) { float x = rec_x(); return R.rec ? x : 1.0f - x; }  /* 1 = recording */
static float swap_out(float x) { return x < 0.5f ? m_ink(1.0f - 2.0f * x) : 0.0f; }
static float swap_in(float x) { return x >= 0.5f ? m_ink(2.0f * x - 1.0f) : 0.0f; }

enum { H_RECORD, H_STOP, H_SAVED, H_NOSD };
static void hint_draw(gfx_fb *fb, int h, float k)
{
    if (k <= 0) return;
    switch (h) {
    case H_NOSD:  ui_state(fb, UI_CX, 298, "SAVE FAILED", ui_dim(C_PRI, k)); break;
    case H_SAVED: ui_state(fb, UI_CX, 298, "SAVED", ui_dim(C_PRI, k)); break;
    case H_STOP:  ui_label(fb, UI_CX, 298, "TAP TO STOP", ui_dim(C_SEC, k * R.mt), UI_CENTER); break;
    default:      ui_label(fb, UI_CX, 298, "TAP TO RECORD", ui_dim(C_SEC, k * R.mt), UI_CENTER); break;
    }
}
static void hint_swap(gfx_fb *fb, int a, int b, float x)
{
    if (a == b || x >= 1) { hint_draw(fb, b, 1); return; }
    hint_draw(fb, a, swap_out(x));
    hint_draw(fb, b, swap_in(x));
}

static void draw_recorder(gfx_fb *fb)
{
    if (!R.msw.shown) { absent(fb, "RECORDER", "Add the mic strap to record voice memos", R.mb); return; }
    if (nos_t < R.m0) { page_title(fb, "RECORDER"); return; }
    float x = rec_x(), rl = rec_look();
    const char *t_new = R.rec ? "RECORDING" : "RECORDER", *t_old = R.rec ? "RECORDER" : "RECORDING";
    if (s_keep_title) {}
    else if (x >= 1) ui_title(fb, t_new, 0);
    else {
        if (swap_out(x) > 0) ui_label(fb, UI_CX, UI_TITLE_Y, t_old, ui_dim(C_SEC, swap_out(x)), UI_CENTER);
        if (swap_in(x) > 0) ui_label(fb, UI_CX, UI_TITLE_Y, t_new, ui_dim(C_SEC, swap_in(x)), UI_CENTER);
    }
    wave_draw(fb, UI_CX - WAVE_COLS * WAVE_PITCH * 0.5f, WAVE_CY, ui_mix(C_DIS, C_WHITE, rl), C_LINE2, R.mb);

    /* the idle state line: SAVED (or NO SD CARD) for a moment, then the hint */
    float since = nos_t - R.t_saved;
    float hold = R.rec_failed ? 2.0f : 1.6f;
    int first = R.rec_failed ? H_NOSD : H_SAVED, ra, rb;
    float rx;
    if (R.rec_failed && since < SWAP_S) { ra = H_RECORD; rb = first; rx = sw_x(R.t_saved); }
    else if (since < hold) { ra = rb = first; rx = 1; }
    else { ra = first; rb = H_RECORD; rx = sw_x(R.t_saved + hold); }

    /* elapsed time: white while recording, C_PRI on the saved length, ghosted at rest */
    float sk = R.rec_failed ? 0.0f : 1.0f - sw_x(R.t_saved + 1.6f);
    dot_style st = { DOT_FONT_TALL, 6.0f, 4.8f, ui_mix(ui_mix(C_DIS, C_PRI, sk), C_WHITE, rl), 0, 1, 1 };
    /* the time and the level beside it are centred as a group (a 5-glyph time keeps one
     * place; an hour-long one moves the pair once) */
    float tw = (float)dot_text_width(&st, "00:00"), tw1 = (float)dot_text_width(&st, R.ts);
    if (tw1 > tw) tw = tw1;
    const float lgap = 18.0f;
    float tcx = UI_CX - (lgap + LVL_W) * 0.5f, lx = tcx + tw * 0.5f + lgap + LVL_W * 0.5f;
    if (R.ts_prev[0] && R.ts_morph < 1) dot_text_morph(fb, &st, tcx, TIME_Y, R.ts_prev, R.ts, 1, R.ts_morph);
    else dot_text_anim(fb, &st, tcx, TIME_Y, R.ts, 1, q((nos_t - R.m0 - 0.05f) / 0.3f, 32), DOT_REVEAL_RANDOM, 7);
    /* input level: light grey while monitoring, white while recording; its track
     * arrives over 4 perceptual steps with the page */
    float lk = m_ink(m_clamp((float)(m_frame_k(R.m0) + 1) / 4.0f, 0, 1));
    float lmid = TIME_Y + 4.0f * 6.0f;                 /* the time's middle row */
    float lspan = LVL_N * LVL_H + (LVL_N - 1) * LVL_GAP;
    level_draw(fb, lx, floorf(lmid + lspan * 0.5f + 0.5f), R.lvl, ui_mix(C_SEC, C_WHITE, rl),
               lk < 1 ? ui_dim(C_LINE2, lk) : C_LINE2);

    /* state line */
    if (s_rec_open && (s_rec_stopping || hal_rec_status() == 2)) {
        ui_state(fb, UI_CX, 298, "SAVING", C_SEC);
    } else if (x < 1) {
        int rest = rx < 0.5f ? ra : rb;
        hint_swap(fb, R.rec ? rest : H_STOP, R.rec ? H_STOP : rest, x);
    } else if (R.rec) hint_draw(fb, H_STOP, 1);
    else hint_swap(fb, ra, rb, rx);

    /* record button: white ring, red dot <-> rounded square (a 4-frame micro table) */
    int pr = R.pressed || nos_t - R.t_down < HOLD_S;
    ui_circle(fb, UI_CX, REC_CY, REC_R, pr ? C_LINE2 : C_SURF2);
    ui_circle_line(fb, UI_CX, REC_CY, REC_R, 3, C_LINE2);
    float k = rec_sq();                                /* 1 = square */
    float side = m_lerp(52.0f, 34.0f, k), rad = m_lerp(26.0f, 8.0f, k);
    uint16_t red = pr ? C_RED_D : C_RED;
    ui_rrect(fb, UI_CX - side / 2, REC_CY - side / 2, side, side, rad, red);
}

static void draw_memos(gfx_fb *fb)
{
    if (!s_nmemo) {
        ui_title(fb, "MEMOS", 0);
        ui_absent(fb, DOT_ICON_MIC, "No memos", "Your recordings land here", NULL, R.build);
        return;
    }
    ui_list_draw(fb, &R.list);
    ui_title(fb, "MEMOS", ui_list_offset(&R.list));
    if (!R.spk) {   /* list footer, under the last card */
        int fy = R.list.top + s_nmemo * R.list.row_h + 34 - (int)floorf(ui_list_offset(&R.list) + 0.5f);
        if (fy < GFX_H - 20) ui_state(fb, UI_CX, fy, "NO SPEAKER", C_DIS);
    }
}

static void band_words(gfx_fb *fb, int band, float k)
{
    static const char *const lab[4] = { "QUIET", "MODERATE", "LOUD", "HEARING RISK" };
    static const char *const line[4] = { "Like a library", "Normal conversation", "Busy street traffic", "Limit your exposure" };
    if (k <= 0 || band < 0 || band > 3) return;
    uint16_t c1 = ui_dim(C_PRI, R.mt * k);
    if (band == 3) ui_state(fb, UI_CX, 318, lab[band], c1);      /* red stays on the meter */
    else ui_label(fb, UI_CX, 318, lab[band], c1, UI_CENTER);
    body_line(fb, 356, line[band], ui_dim(C_SEC, R.mt * k));
}

static void draw_noise(gfx_fb *fb)
{
    if (!R.msw.shown) { absent(fb, "NOISE", "Add the mic strap to measure noise levels", R.mb); return; }
    page_title(fb, "NOISE");
    dot_style st = { DOT_FONT_TALL, 11.0f, 8.8f, C_WHITE, 0, 1, 1 };
    const char *s = R.dbs[0] ? R.dbs : "--";
    if (R.dbs_prev[0] && R.db_morph < 1) dot_text_morph(fb, &st, UI_CX, 104, R.dbs_prev, s, 1, R.db_morph);
    else dot_text(fb, &st, UI_CX - dot_text_width(&st, s) * 0.5f, 104, s);
    ui_text(fb, UI_F_LABEL, UI_CX, 236, "dB", ui_dim(C_SEC, R.mt), UI_CENTER, 2);

    /* meter: 24 dots x 5 dB; white to 85, red beyond (over-limit: the screen's one red) */
    const int n = 24, nsafe = 17;
    const float pitch = 12.0f, d = 8.0f;
    float x0 = UI_CX - pitch * (n - 1) * 0.5f;
    /* dot i lights at 5 i dB, so the first red dot and HEARING RISK both start at 85 */
    float lit = (float)R.meter_dots;
    dot_bar(fb, x0, 272, nsafe, pitch, d, lit < nsafe ? lit : nsafe, C_PRI, C_LINE2);
    /* the red dots belong to HEARING RISK: they light with the hero's band and take the
     * label's 2-step ramp, so the number, the words and the red never disagree */
    float x = sw_x(R.t_band);
    int cur = R.band >= 0 ? R.band : band_of(R.db_shown);
    int prev3 = R.band_prev == 3 && x < 1;
    float rk = cur == 3 ? (R.band_prev != 3 && x < 1 ? swap_in(x) : 1.0f) : prev3 ? swap_out(x) : 0.0f;
    float red = cur == 3 || prev3 ? (lit > nsafe ? lit - nsafe : 0) : 0;
    dot_bar(fb, x0 + nsafe * pitch, 272, n - nsafe, pitch, d, red, ui_mix(C_LINE2, C_RED, rk), C_LINE2);

    /* the band words swap out-then-in on the number's clock */
    if (R.band_prev >= 0 && x < 1) {
        band_words(fb, R.band_prev, swap_out(x));
        band_words(fb, cur, swap_in(x));
    } else band_words(fb, cur, 1);
}

static void memo_draw(gfx_fb *fb)
{
    int xi = (int)lroundf(R.px.x);
    for (int pg = 0; pg < NPAGES; pg++) {
        int ox = pg * (int)PAGE_W - xi;
        if (ox <= -(int)PAGE_W || ox >= GFX_W) continue;
        gfx_fb sub;
        int x0 = ox < 0 ? 0 : ox, x1 = ox + (int)PAGE_W > GFX_W ? GFX_W : ox + (int)PAGE_W;
        if (x1 <= x0 || !nos_subview(fb, &sub, x0, 0, x1, GFX_H, ox, 0)) continue;
        if (pg == 1) { draw_memos(&sub); continue; }
        /* a mic page leaving (module out or back): dimmed under a steady title */
        s_keep_title = ps_ink(&R.msw) < 1;
        if (s_keep_title) ui_title(&sub, pg == 0 ? "RECORDER" : "NOISE", 0);
        int d0 = ps_dim(&sub, &R.msw);
        if (pg == 0) draw_recorder(&sub);
        else draw_noise(&sub);
        sub.dim = d0;
        s_keep_title = 0;
    }
    ui_page_dots_a(fb, UI_CX, UI_DOTS_Y, NPAGES, R.px.x / PAGE_W, 1.0f - rec_look());   /* gone while recording */
}

/* --------------------------------------------------------------- input */
static int in_rec_button(int x, int y)
{
    float dx = x - UI_CX, dy = y - REC_CY;
    return dx * dx + dy * dy <= (REC_R + 10) * (REC_R + 10);
}

static int memo_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { R.pressed = R.touching = R.dragging = R.rec_lock = 0; R.row_held = -1; R.t_down = -10; R.px.v = 0; msp_to(&R.px, R.page * PAGE_W); ui_list_input(&R.list, in); return 1; }

    int settled = !R.dragging && fabsf(R.px.x - R.page * PAGE_W) < 2.0f;
    switch (in->type) {
    case IN_DOWN:
        R.touching = 1;
        if (R.px.moving) { R.px.v = 0; R.px.target = R.px.x; }     /* catch the pager */
        R.pressed = settled && R.page == 0 && R.mic && R.msw.shown && in_rec_button(in->x, in->y);
        if (R.pressed) R.t_down = nos_t;
        if (settled && R.page == 1 && s_nmemo) ui_list_input(&R.list, in);
        return 1;
    case IN_DRAG_START:
        R.pressed = 0;
        R.t_down = -10;
        R.row_held = -1;
        if (in->axis == AXIS_X) {
            if (R.page == 1) ui_list_input(&R.list, in);            /* cancels its press */
            if (R.rec) { R.rec_lock = 1; R.grab_x = R.px.x; nos_feedback(FB_LIMIT); }   /* stay, but show it */
            else {
                R.dragging = 1;
                R.grab_x = R.px.x;
            }
            /* fallthrough into the first move */
        } else {
            if (R.page == 1 && settled && s_nmemo) return ui_list_input(&R.list, in);
            return 0;
        }
        /* fallthrough */
    case IN_DRAG:
        if (R.rec_lock) {
            float d = -in->dx;
            msp_set(&R.px, R.grab_x + (d > 0 ? 1.0f : -1.0f) * m_clamp(rubber_band(fabsf(d), PAGE_W), 0, 20));
            return 1;
        }
        if (R.dragging) {
            float x = R.grab_x - in->dx, max = (NPAGES - 1) * PAGE_W;
            if (x < 0) x = -rubber_band(-x, PAGE_W);
            if (x > max) x = max + rubber_band(x - max, PAGE_W);
            msp_set(&R.px, x);
            return 1;
        }
        if (R.page == 1) return ui_list_input(&R.list, in);
        return 0;
    case IN_DRAG_END:
        R.touching = 0;
        if (R.rec_lock) { R.rec_lock = 0; msp_kick(&R.px, R.page * PAGE_W, 0); return 1; }
        if (R.dragging) {
            R.dragging = 0;
            float v = -in->vx;
            int cur = (int)floorf(R.grab_x / PAGE_W + 0.5f);
            int tgt = cur;
            if (v > 350.0f) tgt = cur + 1;
            else if (v < -350.0f) tgt = cur - 1;
            else tgt = (int)floorf(R.px.x / PAGE_W + 0.5f);
            if (tgt < 0) tgt = 0;
            if (tgt >= NPAGES) tgt = NPAGES - 1;
            float t = tgt * PAGE_W;
            /* never overshoot: a critically damped spring absorbs at most w * distance */
            float dist = fabsf(t - R.px.x), vmax = (6.2831853f / 0.38f) * dist;
            if ((t - R.px.x) * v < 0) v = 0;
            if (v > vmax) v = vmax;
            if (v < -vmax) v = -vmax;
            msp_kick(&R.px, t, v);
            return 1;
        }
        if (R.page == 1) return ui_list_input(&R.list, in);
        return 0;
    case IN_TAP:
        R.pressed = 0;
        if (!settled) return 1;
        if (R.page == 0 && R.mic && in_rec_button(in->x, in->y)) {
            if (R.rec) rec_stop(); else rec_start();
            return 1;
        }
        if (R.page == 1 && s_nmemo) return ui_list_input(&R.list, in);
        return 1;
    case IN_UP:
        R.pressed = 0;
        R.touching = 0;
        if (R.rec_lock) { R.rec_lock = 0; msp_kick(&R.px, R.page * PAGE_W, 0); }
        if (R.page == 1 && s_nmemo) ui_list_input(&R.list, in);
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_memo = { "RECORDER", memo_enter, memo_leave, memo_tick, memo_draw, memo_input,
                              SCR_HDRAG };
