/* con_memo.c — Recorder: voice memos and a noise meter (INMP441 mic, MAX98357A speaker).
 *
 * A three-page horizontal pager (page dots at the bottom):
 *   RECORDER  live waveform as mirrored dot columns that step left one column per
 *             100 ms (dim while monitoring, white while recording), the elapsed time
 *             in dot-matrix, and the record button: a red dot (the screen's one red)
 *             that morphs into a rounded square while recording.
 *   MEMOS     saved recordings (on the device: /sdcard/watch/memos/ WAV files; in the
 *             simulator an in-memory list) with playback and a progress dot bar.
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
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include <dirent.h>
#include <sys/stat.h>
#endif

#define MEMO_DIR  "/sdcard/watch/memos"
#define NPAGES    3
#define PAGE_W    466.0f

static float q(float x, float n) { return floorf(m_clamp(x, 0, 1) * n + 0.5f) / n; }

static void body_line(gfx_fb *fb, int y, const char *s, uint16_t c)
{
    int x0, x1;
    ui_row_span((float)(y - 8), 40, &x0, &x1);
    ui_text_fit(fb, UI_F_BODY, UI_CX, y, s, c, UI_CENTER, x1 - x0);
}

static void absent(gfx_fb *fb, const char *title, const char *what, float build)
{
    ui_title(fb, title, 0);
    ui_absent(fb, DOT_ICON_MIC, "Not connected", what, "MIC MODULE", build);
}

#define HOLD_S    (2.5f / NOS_FPS)   /* a quick tap shows its pressed fill >= 3 frames */

/* =========================================================== memo store ===== */
#define MEMO_MAX 24
typedef struct { char path[64]; char name[20]; uint32_t dur_ms; uint8_t mo, d, h, mi; } memo;
static memo *s_memos;          /* PSRAM: internal DRAM is full */
static int   s_nmemo;
static int   s_seq;            /* next "Memo N" */

static void memo_add(const char *path, uint32_t dur, int mo, int d, int h, int mi)
{
    if (!s_memos) return;
    if (s_nmemo >= MEMO_MAX) { memmove(&s_memos[0], &s_memos[1], sizeof(memo) * (MEMO_MAX - 1)); s_nmemo--; }
    /* newest first */
    memmove(&s_memos[1], &s_memos[0], sizeof(memo) * (size_t)s_nmemo);
    memo *m = &s_memos[0];
    memset(m, 0, sizeof *m);
    snprintf(m->path, sizeof m->path, "%s", path);
    snprintf(m->name, sizeof m->name, "Memo %d", ++s_seq);
    m->mo = (uint8_t)mo; m->d = (uint8_t)d; m->h = (uint8_t)h; m->mi = (uint8_t)mi;
    m->dur_ms = dur;
    s_nmemo++;
}

static void memo_load(void)
{
    if (!s_memos) s_memos = (memo *)hal_big_alloc(sizeof(memo) * MEMO_MAX);
    if (!s_memos) return;
#ifdef ESP_PLATFORM
    s_nmemo = 0; s_seq = 0;
    DIR *dir = opendir(MEMO_DIR);
    if (!dir) return;
    struct dirent *e;
    /* names are M_MMDD_HHMMSS.wav, so a sort by name is a sort by time */
    char names[MEMO_MAX][24];
    int n = 0;
    while ((e = readdir(dir)) && n < MEMO_MAX) {
        size_t l = strlen(e->d_name);
        if (l < 5 || l > 23 || strcmp(e->d_name + l - 4, ".wav")) continue;
        snprintf(names[n++], sizeof names[0], "%s", e->d_name);
    }
    closedir(dir);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && strcmp(names[j - 1], names[j]) > 0; j--) {
            char t[24]; memcpy(t, names[j], 24); memcpy(names[j], names[j - 1], 24); memcpy(names[j - 1], t, 24);
        }
    for (int i = 0; i < n; i++) {
        char path[64];
        snprintf(path, sizeof path, MEMO_DIR "/%s", names[i]);
        struct stat st;
        uint32_t dur = 0;
        if (!stat(path, &st) && st.st_size > 44) dur = (uint32_t)((st.st_size - 44) / 32);   /* 16 kHz x 16 bit */
        int mo = 1, d = 1, h = 0, mi = 0;
        sscanf(names[i], "M_%2d%2d_%2d%2d", &mo, &d, &h, &mi);
        memo_add(path, dur, mo, d, h, mi);
    }
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
#define WAVE_PITCH 11.0f
#define WAVE_CY    150.0f
#define REC_CY     356.0f
#define REC_R      42.0f
#define TIME_Y     212.0f

static struct {
    float t_enter, build, txt;
    /* pager */
    mspring px;
    int   page, dragging;
    float grab_x;
    /* mic */
    int   mic, spk;
    uint8_t wave[WAVE_COLS];     /* 0..WAVE_HALF per column */
    int   wave_frame;
    float db, db_shown;          /* smoothed level, the number on screen */
    char  dbs[8], dbs_prev[8];
    float t_db, db_morph;
    int   band, band_prev;       /* QUIET .. HEARING RISK of the shown number; -1 none */
    float t_band;
    int   meter_frame;
    float meter;                 /* dB on the meter (10 Hz) */
    /* recording */
    int   rec;
    int   pressed;
    float t_down;                /* record button press start */
    uint32_t rec_ms;
    float t_rec;                 /* start/stop moment (morph) */
    char  ts[12], ts_prev[12];
    float t_ts, ts_morph;
    float t_saved;
    uint32_t saved_ms;
    int   rec_failed;
    /* playback */
    ui_list list;
    int   list_inited;
    int   playing;               /* memo index, -1 none */
    float t_play;
    uint32_t play_ms;
    int   real_play;             /* the HAL is actually playing it */
    int   rec_lock;              /* a sideways drag while recording: rubber band only */
    int   row_held;              /* memo row whose fill holds after a quick tap */
    float row_t;
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

static void memo_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    if (!s_memos || i >= s_nmemo) return;
    const memo *m = &s_memos[i];
    float st = q((nos_t - R.t_enter - 0.034f * (i < 6 ? i : 6)) / 0.15f, 4);
    float k = fade * st;
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
        /* progress: 12 dots filling one by one, the elapsed time beside them */
        float p = m_clamp((float)R.play_ms / (float)m->dur_ms, 0, 1);
        dot_bar(fb, (float)tx + 3, (float)(y + 63), 12, 11.0f, 6.0f, q(p, 12) * 12.0f, C_WHITE, C_LINE2);
        fmt_len(len, sizeof len, R.play_ms);
        ui_text(fb, UI_F_LABEL, tr, y + 69, len, ui_dim(C_SEC, k), UI_RIGHT, LABEL_TRACK);
    } else {
        /* length, then the time if it was today, else the day */
        char w[12];
        hal_datetime now;
        hal_get_datetime(&now);
        if (now.month == m->mo && now.day == m->d) ui_fmt_time(w, sizeof w, m->h, m->mi);
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

/* The HAL recorder has stopped (by us or by itself): add the memo, return its number. */
static int rec_finish(void)
{
    uint32_t ms = hal_rec_ms();
    s_rec_open = 0;
    s_saved_ms = ms;
#ifdef ESP_PLATFORM
    memo_load();                     /* the file is on the card now */
#else
    {
        hal_datetime dt;
        hal_get_datetime(&dt);
        char path[64];
        snprintf(path, sizeof path, MEMO_DIR "/M_%02d%02d_%02d%02d%02d.wav", dt.month, dt.day, dt.hour, dt.minute, dt.second);
        memo_add(path, ms, dt.month, dt.day, dt.hour, dt.minute);
    }
#endif
    if (R.list_inited) ui_list_set_n(&R.list, s_nmemo);
    return s_seq;
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

int svc_memo_active(void) { return s_rec_open && hal_rec_active(); }
uint32_t svc_memo_elapsed_ms(void) { return svc_memo_active() ? hal_rec_ms() : 0; }
int svc_memo_number(void) { return s_seq + 1; }          /* "Memo n" it will be saved as */

/* Stop and save a background recording (the REC chip); returns the memo number, 0 if
 * nothing was recording. */
int svc_memo_stop(void)
{
    if (!s_rec_open) return 0;
    hal_rec_stop();
    int n = rec_finish();
    nos_feedback(FB_SUCCESS);
    saved_toast(n);
    return n;
}

/* Per frame (svc_tick): a background recording that ends by itself (card full) is
 * saved and announced. Recorder handles its own while it is on top. */
void svc_memo_tick(float dt)
{
    (void)dt;
    if (s_rec_open && !hal_rec_active() && nos_top() != &scr_memo) saved_toast(rec_finish());
}

/* --------------------------------------------------------------- enter */
static void memo_enter(void)
{
    /* revealed by back (a call or an alarm went away): no rebuild, same page */
    int back = nos_revealed();
    R.t_enter = back ? nos_t - 10.0f : nos_enter_t();   /* build from 40% of the opening iris */
    R.build = R.txt = back ? 1.0f : 0.0f;
    if (!back) { msp_init(&R.px, 0, SPRING_PAGE); R.page = 0; }
    R.dragging = 0;
    R.pressed = 0;
    R.t_down = -10;
    R.row_held = -1;
    if (s_rec_open && !hal_rec_active()) rec_finish();   /* ended by itself while away */
    R.rec = svc_memo_active();             /* ... or still running: pick it up */
    R.rec_ms = R.rec ? hal_rec_ms() : 0;
    R.t_rec = -10;
    R.t_saved = -10;
    R.rec_failed = 0;
    R.ts[0] = R.ts_prev[0] = 0;
    R.dbs[0] = R.dbs_prev[0] = 0;
    R.band = R.band_prev = -1;
    R.t_band = -10;
    R.db = R.db_shown = 0;
    R.meter = 0;
    memset(R.wave, 0, sizeof R.wave);
    memo_load();
    if (!R.list_inited) { ui_list_init(&R.list, s_nmemo, UI_ROW_H, 104, memo_row, memo_tap); R.list_inited = 1; }
    else ui_list_set_n(&R.list, s_nmemo);
    R.list.margin = 30;
    R.playing = -1;
}

static void rec_stop(void)
{
    if (!R.rec) return;
    hal_rec_stop();
    R.rec = 0;
    R.t_rec = nos_t;
    if (s_rec_open) rec_finish();
    R.saved_ms = s_saved_ms ? s_saved_ms : R.rec_ms;
    R.t_saved = nos_t;
    nos_feedback(FB_SUCCESS);
}

static void rec_start(void)
{
    hal_datetime dt;
    hal_get_datetime(&dt);
    char path[64];
    snprintf(path, sizeof path, MEMO_DIR "/M_%02d%02d_%02d%02d%02d.wav", dt.month, dt.day, dt.hour, dt.minute, dt.second);
#ifdef ESP_PLATFORM
    mkdir("/sdcard/watch", 0777);
    mkdir(MEMO_DIR, 0777);
#endif
    play_stop();
    if (!hal_rec_start(path)) {
        R.rec_failed = 1;
        R.t_saved = nos_t;
        nos_feedback(FB_ERROR);
        return;
    }
    R.rec_failed = 0;
    R.rec = 1;
    s_rec_open = 1;
    R.rec_ms = 0;
    R.t_rec = nos_t;
    nos_feedback(FB_TOGGLE_ON);           /* the monitored wave carries on, now in white */
}

static void memo_leave(void)
{
    /* a recording keeps going in the background (the REC chip shows it); say so once */
    if (R.rec && svc_memo_active()) nos_toast("RECORDING");
    R.rec = 0;
    play_stop();
}

/* ---------------------------------------------------------------- tick */
static void push_wave(uint8_t lvl)
{
    memmove(R.wave, R.wave + 1, WAVE_COLS - 1);
    R.wave[WAVE_COLS - 1] = lvl;
}

static int band_of(float db) { return db < 40 ? 0 : db < 70 ? 1 : db < 85 ? 2 : 3; }

static void memo_tick(float dt)
{
    int frames = (int)lroundf(dt * NOS_FPS);
    if (frames < 1) frames = 1;
    uint32_t pres = hal_ext_present();
    R.mic = (pres & EXT_MIC) != 0;
    R.spk = (pres & EXT_SPEAKER) != 0;
    R.build = q((nos_t - R.t_enter) / UI_ENTRY_S, 32);
    R.txt = nos_t >= R.t_enter ? 1.0f : 0.0f;

    /* pager */
    if (!R.dragging && msp_step(&R.px, dt)) {}
    int p = (int)floorf(R.px.x / PAGE_W + 0.5f);
    if (p < 0) p = 0;
    if (p >= NPAGES) p = NPAGES - 1;
    if (p != R.page) { R.page = p; nos_feedback(FB_TICK); }

    /* mic: only while a mic page is on the glass or a recording runs (it costs power) */
    if (R.mic && (R.rec || on_page(0) || on_page(2))) {
        float db = hal_mic_db();
        if (db >= 0) R.db += (db - R.db) * m_clamp(dt * 8.0f, 0, 1);
        R.wave_frame += frames;
        if (R.wave_frame >= 3) {                 /* 10 Hz: one column per step */
            R.wave_frame = 0;
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
        /* the number: 1 Hz with a digit morph, but at once when the meter crosses into
         * another band, so the hero, the label and the red dots always agree */
        int bm = band_of(R.meter), bs = band_of(R.db_shown);
        int cross = R.dbs[0] && bm != bs && ((bm == 3) != (bs == 3) || nos_t - R.t_db >= 0.1f);   /* red: at once */
        if (nos_t - R.t_db >= 1.0f || !R.dbs[0] || cross) {
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
        R.db_morph = R.dbs_prev[0] ? q((nos_t - R.t_db) / 0.28f, 16) : 1.0f;
    }

    /* recording clock */
    if (R.rec) {
        if (!hal_rec_active()) rec_stop();          /* card full / error: saved like a stop */
        else {
            R.rec_ms = hal_rec_ms();
            nos_keep_awake();
        }
    }
    char t[12];
    if (R.rec || nos_t - R.t_saved < 1.6f) ui_fmt_dur(t, sizeof t, R.rec ? R.rec_ms : R.saved_ms, 0);
    else snprintf(t, sizeof t, UI_NONE ":" UI_NONE);   /* at rest: no 00:00 */
    if (strcmp(t, R.ts)) {
        if (R.ts[0] && strlen(t) == strlen(R.ts)) { memcpy(R.ts_prev, R.ts, sizeof R.ts); R.t_ts = nos_t; }
        else R.ts_prev[0] = 0;
        memcpy(R.ts, t, sizeof t);
    }
    R.ts_morph = R.ts_prev[0] ? q((nos_t - R.t_ts) / 0.2f, 16) : 1.0f;

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
}

/* ---------------------------------------------------------------- draw */
/* Mirrored dot columns about a centre line: one damage command, dots stamped in raster. */
static void wave_draw(gfx_fb *fb, float x, float cy, uint16_t on, uint16_t line, float build)
{
    const float d = 7.0f, e = d * 0.5f + 1.0f;
    float w = WAVE_COLS * WAVE_PITCH, top = cy - WAVE_HALF * WAVE_PITCH - e, bot = cy + WAVE_HALF * WAVE_PITCH + e;
    int nb = (int)(build * WAVE_COLS + 0.5f);   /* columns built so far, from the centre out */
    if (fb->rec) {
        unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_u(0x3AFEu, on), line), (unsigned)nb);
        for (int c = 0; c < WAVE_COLS; c++) h = gfx_hash_u(h, R.wave[c]);
        gfx_rec_cmd(fb, (int)floorf(x - e), (int)floorf(top), (int)ceilf(x + w + e), (int)ceilf(bot), h);
        return;
    }
    int lo = fb->y_base - fb->oy, hi = lo + fb->h;      /* logical rows in this stripe */
    if (bot < lo || top > hi) return;
    int mid = WAVE_COLS / 2;
    for (int c = 0; c < WAVE_COLS; c++) {
        if (abs(c - mid) * 2 >= nb) continue;
        float cx = x + (c + 0.5f) * WAVE_PITCH;
        int lv = R.wave[c];
        if (cy + e >= lo && cy - e <= hi) dot_disc(fb, cx, cy, d, lv ? on : line);
        for (int k = 1; k <= lv; k++) {
            float yu = cy - k * WAVE_PITCH, yd = cy + k * WAVE_PITCH;
            if (yu + e >= lo && yu - e <= hi) dot_disc(fb, cx, yu, d, on);
            if (yd + e >= lo && yd - e <= hi) dot_disc(fb, cx, yd, d, on);
        }
    }
}

/* Start and stop run on one ~150 ms clock with the button: colours take 4 steps,
 * words leave over 2 steps and the new ones arrive over the next 2. */
#define SWAP_S 0.15f
static float rec_x(void) { return q((nos_t - R.t_rec) / SWAP_S, 4); }           /* 1 = settled */
static float rec_look(void) { float x = rec_x(); return R.rec ? x : 1.0f - x; }  /* 1 = recording */
static float swap_out(float x) { return x < 0.5f ? 1.0f - 2.0f * x : 0.0f; }
static float swap_in(float x) { return x >= 0.5f ? 2.0f * x - 1.0f : 0.0f; }

enum { H_RECORD, H_STOP, H_SAVED, H_NOSD };
static void hint_draw(gfx_fb *fb, int h, float k)
{
    if (k <= 0) return;
    switch (h) {
    case H_NOSD:  ui_state(fb, UI_CX, 298, "NO SD CARD", ui_dim(C_PRI, k)); break;
    case H_SAVED: ui_state(fb, UI_CX, 298, "SAVED", ui_dim(C_PRI, k)); break;
    case H_STOP:  ui_label(fb, UI_CX, 298, "TAP TO STOP", ui_dim(C_SEC, k * R.txt), UI_CENTER); break;
    default:      ui_label(fb, UI_CX, 298, "TAP TO RECORD", ui_dim(C_SEC, k * R.txt), UI_CENTER); break;
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
    if (!R.mic) { absent(fb, "RECORDER", "Add the mic strap to record voice memos", R.build); return; }
    if (nos_t < R.t_enter) { ui_title(fb, "RECORDER", 0); return; }
    float x = rec_x(), rl = rec_look();
    const char *t_new = R.rec ? "RECORDING" : "RECORDER", *t_old = R.rec ? "RECORDER" : "RECORDING";
    if (x >= 1) ui_title(fb, t_new, 0);
    else {
        if (swap_out(x) > 0) ui_label(fb, UI_CX, UI_TITLE_Y, t_old, ui_dim(C_SEC, swap_out(x)), UI_CENTER);
        if (swap_in(x) > 0) ui_label(fb, UI_CX, UI_TITLE_Y, t_new, ui_dim(C_SEC, swap_in(x)), UI_CENTER);
    }
    wave_draw(fb, UI_CX - WAVE_COLS * WAVE_PITCH * 0.5f, WAVE_CY, ui_mix(C_DIS, C_WHITE, rl), C_LINE2, R.build);

    /* the idle state line: SAVED (or NO SD CARD) for a moment, then the hint */
    float since = nos_t - R.t_saved;
    float hold = R.rec_failed ? 2.0f : 1.6f;
    int first = R.rec_failed ? H_NOSD : H_SAVED, ra, rb;
    float rx;
    if (R.rec_failed && since < SWAP_S) { ra = H_RECORD; rb = first; rx = q(since / SWAP_S, 4); }
    else if (since < hold) { ra = rb = first; rx = 1; }
    else { ra = first; rb = H_RECORD; rx = q((since - hold) / SWAP_S, 4); }

    /* elapsed time: white while recording, C_PRI on the saved length, ghosted at rest */
    float sk = R.rec_failed ? 0.0f : 1.0f - q((since - 1.6f) / SWAP_S, 4);
    dot_style st = { DOT_FONT_TALL, 6.0f, 4.8f, ui_mix(ui_mix(C_DIS, C_PRI, sk), C_WHITE, rl), 0, 1, 1 };
    if (R.ts_prev[0] && R.ts_morph < 1) dot_text_morph(fb, &st, UI_CX, TIME_Y, R.ts_prev, R.ts, 1, R.ts_morph);
    else dot_text_anim(fb, &st, UI_CX, TIME_Y, R.ts, 1, q((nos_t - R.t_enter - 0.05f) / 0.3f, 32), DOT_REVEAL_RANDOM, 7);

    /* state line */
    if (x < 1) {
        int rest = rx < 0.5f ? ra : rb;
        hint_swap(fb, R.rec ? rest : H_STOP, R.rec ? H_STOP : rest, x);
    } else if (R.rec) hint_draw(fb, H_STOP, 1);
    else hint_swap(fb, ra, rb, rx);

    /* record button: white ring, red dot <-> rounded square (EASE_MECH, 200 ms) */
    const float b = 1.0f;
    int pr = R.pressed || nos_t - R.t_down < HOLD_S;
    ui_circle(fb, UI_CX, REC_CY, REC_R, pr ? C_LINE2 : C_SURF2);
    ui_circle_line(fb, UI_CX, REC_CY, REC_R, 3, C_LINE2);
    float m = bezier_ease(&EASE_MECH, q((nos_t - R.t_rec) / 0.2f, 16));
    float k = R.rec ? m : 1.0f - m;                    /* 1 = square */
    float side = m_lerp(52.0f, 34.0f, k) * b, rad = m_lerp(26.0f, 8.0f, k) * b;
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
    uint16_t c1 = ui_dim(C_PRI, R.txt * k);
    if (band == 3) ui_state(fb, UI_CX, 318, lab[band], c1);      /* red stays on the meter */
    else ui_label(fb, UI_CX, 318, lab[band], c1, UI_CENTER);
    body_line(fb, 356, line[band], ui_dim(C_SEC, R.txt * k));
}

static void draw_noise(gfx_fb *fb)
{
    if (!R.mic) { absent(fb, "NOISE", "Add the mic strap to measure noise levels", R.build); return; }
    ui_title(fb, "NOISE", 0);
    dot_style st = { DOT_FONT_TALL, 11.0f, 8.8f, C_WHITE, 0, 1, 1 };
    const char *s = R.dbs[0] ? R.dbs : "--";
    if (R.dbs_prev[0] && R.db_morph < 1) dot_text_morph(fb, &st, UI_CX, 104, R.dbs_prev, s, 1, R.db_morph);
    else dot_text(fb, &st, UI_CX - dot_text_width(&st, s) * 0.5f, 104, s);
    ui_text(fb, UI_F_LABEL, UI_CX, 236, "dB", ui_dim(C_SEC, R.txt), UI_CENTER, 2);

    /* meter: 24 dots x 5 dB; white to 85, red beyond (over-limit: the screen's one red) */
    const int n = 24, nsafe = 17;
    const float pitch = 12.0f, d = 8.0f;
    float x0 = UI_CX - pitch * (n - 1) * 0.5f;
    /* dot i lights at 5 i dB, so the first red dot and HEARING RISK both start at 85 */
    float lit = (R.meter > 0 ? floorf(m_clamp(R.meter, 0, 119) / 5.0f) + 1.0f : 0.0f) * q(R.build, 16);
    dot_bar(fb, x0, 272, nsafe, pitch, d, lit < nsafe ? lit : nsafe, C_PRI, C_LINE2);
    /* the red dots belong to HEARING RISK: they light with the hero's band and take the
     * label's 2-step ramp, so the number, the words and the red never disagree */
    float x = q((nos_t - R.t_band) / SWAP_S, 4);
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
        if (pg == 0) draw_recorder(&sub);
        else if (pg == 1) draw_memos(&sub);
        else draw_noise(&sub);
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
    int settled = !R.dragging && fabsf(R.px.x - R.page * PAGE_W) < 2.0f;
    switch (in->type) {
    case IN_DOWN:
        if (R.px.moving) { R.px.v = 0; R.px.target = R.px.x; }     /* catch the pager */
        R.pressed = settled && R.page == 0 && R.mic && in_rec_button(in->x, in->y);
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
        if (R.rec_lock) { R.rec_lock = 0; msp_kick(&R.px, R.page * PAGE_W, 0); }
        if (R.page == 1 && s_nmemo) ui_list_input(&R.list, in);
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_memo = { "RECORDER", memo_enter, memo_leave, memo_tick, memo_draw, memo_input,
                              SCR_HDRAG };
