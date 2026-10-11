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
 *             that turns amber past 85 dB, and a hearing-risk label.
 * Module-absent states for the mic (record, noise) and the speaker (playback). */
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

static void absent(gfx_fb *fb, const char *title, int icon, const char *what, float build, float txt)
{
    ui_title(fb, title, 0);
    dot_icon_anim(fb, icon, UI_CX, 176, 7.0f, 5.6f, C_DIS, build, DOT_REVEAL_RAIN);
    body_line(fb, 288, "Module not connected", ui_dim(C_PRI, txt));
    body_line(fb, 322, what, ui_dim(C_SEC, txt));
}

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
#define DOTS_Y     446

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
    int   meter_frame;
    float meter;                 /* dB on the meter (10 Hz) */
    /* recording */
    int   rec;
    int   pressed;
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
} R = { .playing = -1 };

static int on_page(int p) { return fabsf(R.px.x - p * PAGE_W) < PAGE_W; }

/* ----------------------------------------------------------- memos list */
static void play_stop(void)
{
    if (R.playing < 0) return;
    if (R.real_play) hal_audio_play_stop();
    R.playing = -1;
    R.real_play = 0;
}

static void memo_row(gfx_fb *fb, int i, int x0, int y, int x1, int h, float fade, int pressed)
{
    if (!s_memos || i >= s_nmemo) return;
    const memo *m = &s_memos[i];
    float st = q((nos_t - R.t_enter - 0.066f * i) / 0.2f, 4);
    float k = fade * st;
    if (k <= 0) return;
    int cx0 = x0 + 4, cx1 = x1 - 4;
    ui_rrect(fb, (float)cx0, (float)(y + 6), (float)(cx1 - cx0), (float)(h - 12), 32, ui_dim(pressed ? C_SURF2 : C_SURF, k));
    int playing = R.playing == i;
    float bx = (float)cx0 + 42, by = (float)y + h * 0.5f;
    ui_circle(fb, bx, by, 24, ui_dim(playing ? C_WHITE : C_SURF3, k));
    int icon = playing ? DOT_ICON_PAUSE : DOT_ICON_PLAY;
    dot_icon(fb, icon, bx - (playing ? 0 : 2.2f), by, 2.4f, 1.9f, playing ? C_BG : ui_dim(C_PRI, k));
    int tx = cx0 + 80;
    ui_text_fit(fb, UI_F_BODY, tx, y + 44, m->name, ui_dim(C_PRI, k), UI_LEFT, cx1 - 24 - tx);
    char d[16];
    ui_fmt_dur(d, sizeof d, m->dur_ms, 0);
    if (playing && m->dur_ms) {
        /* progress: 12 dots, filling one by one */
        float p = m_clamp((float)R.play_ms / (float)m->dur_ms, 0, 1);
        dot_bar(fb, (float)tx + 3, (float)(y + 68), 12, 11.0f, 6.0f, q(p, 12) * 12.0f, C_WHITE, C_LINE2);
    } else {
        /* length, then the time if it was today, else the day */
        char b[40], w[12];
        hal_datetime now;
        hal_get_datetime(&now);
        if (now.month == m->mo && now.day == m->d) ui_fmt_time(w, sizeof w, m->h, m->mi);
        else snprintf(w, sizeof w, "%d %s", m->d, ui_month3(m->mo));
        snprintf(b, sizeof b, "%s  %s", d, w);
        ui_text_fit(fb, UI_F_LABEL, tx, y + 74, b, ui_dim(C_DIS, k), UI_LEFT, cx1 - 24 - tx);
    }
}

static void memo_tap(int i)
{
    if (!s_memos || i < 0 || i >= s_nmemo) return;
    if (R.playing == i) { play_stop(); return; }
    play_stop();
    if (!R.spk) { nos_feedback(FB_ERROR); nos_toast("Speaker not connected"); return; }
    int ok = hal_audio_play_file(s_memos[i].path);
#ifdef ESP_PLATFORM
    if (!ok) { nos_feedback(FB_ERROR); nos_toast("Can't play this memo"); return; }
#endif
    R.real_play = ok;
    R.playing = i;
    R.t_play = nos_t;
    R.play_ms = 0;
}

/* --------------------------------------------------------------- enter */
static void memo_enter(void)
{
    R.t_enter = nos_t;
    R.build = R.txt = 0;
    msp_init(&R.px, 0, SPRING_PAGE);
    R.page = 0;
    R.dragging = 0;
    R.pressed = 0;
    R.rec = hal_rec_active();
    R.t_rec = -10;
    R.t_saved = -10;
    R.rec_failed = 0;
    R.ts[0] = R.ts_prev[0] = 0;
    R.dbs[0] = R.dbs_prev[0] = 0;
    R.db = R.db_shown = 0;
    R.meter = 0;
    memset(R.wave, 0, sizeof R.wave);
    memo_load();
    if (!R.list_inited) { ui_list_init(&R.list, s_nmemo, 100, 104, memo_row, memo_tap); R.list_inited = 1; }
    else ui_list_set_n(&R.list, s_nmemo);
    R.list.margin = 30;
    R.playing = -1;
}

static void rec_stop(void)
{
    if (!R.rec) return;
    hal_rec_stop();
    uint32_t ms = hal_rec_ms();
    if (!ms) ms = R.rec_ms;
    R.rec = 0;
    R.t_rec = nos_t;
    R.saved_ms = ms;
    R.t_saved = nos_t;
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
    ui_list_set_n(&R.list, s_nmemo);
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
    R.rec_ms = 0;
    R.t_rec = nos_t;
    R.t_saved = -10;
    memset(R.wave, 0, sizeof R.wave);
    nos_feedback(FB_TOGGLE_ON);
}

static void memo_leave(void)
{
    rec_stop();
    play_stop();
}

/* ---------------------------------------------------------------- tick */
static void push_wave(uint8_t lvl)
{
    memmove(R.wave, R.wave + 1, WAVE_COLS - 1);
    R.wave[WAVE_COLS - 1] = lvl;
}

static void memo_tick(float dt)
{
    int frames = (int)lroundf(dt * NOS_FPS);
    if (frames < 1) frames = 1;
    uint32_t pres = hal_ext_present();
    R.mic = (pres & EXT_MIC) != 0;
    R.spk = (pres & EXT_SPEAKER) != 0;
    R.build = q((nos_t - R.t_enter) / 0.36f, 32);
    R.txt = q((nos_t - R.t_enter - 0.08f) / 0.2f, 4);

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
        /* the number: 4 Hz, with a digit morph */
        if (nos_t - R.t_db >= 0.25f) {
            char b[8];
            int v = (int)lroundf(R.db);
            if (v < 0) v = 0;
            if (v > 140) v = 140;
            snprintf(b, sizeof b, "%d", v);
            if (strcmp(b, R.dbs)) {
                if (R.dbs[0] && strlen(b) == strlen(R.dbs)) memcpy(R.dbs_prev, R.dbs, sizeof R.dbs);
                else R.dbs_prev[0] = 0;
                memcpy(R.dbs, b, sizeof b);
                R.t_db = nos_t;
            } else R.t_db = nos_t - 0.2f;          /* re-check soon */
            R.db_shown = (float)v;
        }
        R.db_morph = R.dbs_prev[0] ? q((nos_t - R.t_db) / 0.2f, 16) : 1.0f;
    }

    /* recording clock */
    if (R.rec) {
        if (!hal_rec_active()) rec_stop();          /* card full / error */
        else {
            R.rec_ms = hal_rec_ms();
            nos_keep_awake();
        }
    }
    char t[12];
    ui_fmt_dur(t, sizeof t, R.rec ? R.rec_ms : (nos_t - R.t_saved < 1.6f ? R.saved_ms : 0), 0);
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

static void draw_recorder(gfx_fb *fb)
{
    if (!R.mic) { absent(fb, "RECORDER", DOT_ICON_MIC, "Add the mic strap to record", R.build, R.txt); return; }
    ui_title(fb, R.rec ? "RECORDING" : "RECORDER", 0);
    wave_draw(fb, UI_CX - WAVE_COLS * WAVE_PITCH * 0.5f, WAVE_CY, R.rec ? C_WHITE : C_DIS, C_LINE2, R.build);

    /* elapsed time: ghosted until recording */
    dot_style st = { DOT_FONT_TALL, 6.0f, 4.8f, R.rec ? C_WHITE : (nos_t - R.t_saved < 1.6f && !R.rec_failed ? C_PRI : C_LINE2), 0, 1, 1 };
    if (R.ts_prev[0] && R.ts_morph < 1) dot_text_morph(fb, &st, UI_CX, TIME_Y, R.ts_prev, R.ts, 1, R.ts_morph);
    else dot_text_anim(fb, &st, UI_CX, TIME_Y, R.ts, 1, q((nos_t - R.t_enter - 0.05f) / 0.3f, 32), DOT_REVEAL_RANDOM, 7);

    /* state line */
    float since = nos_t - R.t_saved;
    uint16_t c2 = ui_dim(C_SEC, R.txt);
    if (R.rec_failed && since < 2.0f) ui_state(fb, UI_CX, 298, "NO SD CARD", C_PRI);
    else if (!R.rec && since < 1.6f) ui_state(fb, UI_CX, 298, "SAVED", C_PRI);
    else if (!R.rec) ui_label(fb, UI_CX, 298, "TAP TO RECORD", c2, UI_CENTER);
    else ui_label(fb, UI_CX, 298, "TAP TO STOP", c2, UI_CENTER);

    /* record button: white ring, red dot <-> rounded square (EASE_MECH, 200 ms) */
    float b = bezier_ease(&EASE_STANDARD, q((nos_t - R.t_enter - 0.06f) / 0.26f, 32));
    if (b <= 0.01f) return;
    ui_circle(fb, UI_CX, REC_CY, REC_R * b, R.pressed ? C_SURF3 : C_SURF2);
    ui_circle_line(fb, UI_CX, REC_CY, REC_R * b, 3, C_LINE2);
    float m = bezier_ease(&EASE_MECH, q((nos_t - R.t_rec) / 0.2f, 16));
    float k = R.rec ? m : 1.0f - m;                    /* 1 = square */
    float side = m_lerp(52.0f, 34.0f, k) * b, rad = m_lerp(26.0f, 8.0f, k) * b;
    uint16_t red = R.pressed ? C_RED_D : C_RED;
    ui_rrect(fb, UI_CX - side / 2, REC_CY - side / 2, side, side, rad, red);
}

static void draw_memos(gfx_fb *fb)
{
    if (!s_nmemo) {
        ui_title(fb, "MEMOS", 0);
        dot_icon_anim(fb, DOT_ICON_MIC, UI_CX, 196, 7.0f, 5.6f, C_DIS, R.build, DOT_REVEAL_RAIN);
        ui_state(fb, UI_CX, 300, "NO MEMOS", ui_dim(C_PRI, R.txt));
        body_line(fb, 336, "Your recordings land here", ui_dim(C_SEC, R.txt));
        return;
    }
    ui_list_draw(fb, &R.list);
    ui_title(fb, R.spk ? "MEMOS" : "MEMOS  /  NO SPEAKER", ui_list_offset(&R.list));
}

static void draw_noise(gfx_fb *fb)
{
    if (!R.mic) { absent(fb, "NOISE", DOT_ICON_SPEAKER, "Add the mic strap to measure", R.build, R.txt); return; }
    ui_title(fb, "NOISE", 0);
    int risk = R.db_shown >= 85.0f;
    dot_style st = { DOT_FONT_TALL, 11.0f, 8.8f, C_WHITE, 0, 1, 1 };
    const char *s = R.dbs[0] ? R.dbs : "--";
    if (R.dbs_prev[0] && R.db_morph < 1) dot_text_morph(fb, &st, UI_CX, 104, R.dbs_prev, s, 1, R.db_morph);
    else dot_text(fb, &st, UI_CX - dot_text_width(&st, s) * 0.5f, 104, s);
    ui_label(fb, UI_CX, 236, "DB", ui_dim(C_SEC, R.txt), UI_CENTER);

    /* meter: 24 dots x 5 dB; white to 85, amber beyond */
    const int n = 24, nsafe = 17;
    const float pitch = 12.0f, d = 8.0f;
    float x0 = UI_CX - pitch * (n - 1) * 0.5f;
    float lit = floorf(m_clamp(R.meter, 0, 120) / 5.0f + 0.5f) * q(R.build, 16);
    dot_bar(fb, x0, 272, nsafe, pitch, d, lit < nsafe ? lit : nsafe, C_PRI, C_LINE2);
    dot_bar(fb, x0 + nsafe * pitch, 272, n - nsafe, pitch, d, lit > nsafe ? lit - nsafe : 0, C_AMBER, C_LINE2);

    const char *lab, *line;
    int v = (int)R.db_shown;
    if (v < 40)      { lab = "QUIET";        line = "Like a library"; }
    else if (v < 70) { lab = "MODERATE";     line = "Normal conversation"; }
    else if (v < 85) { lab = "LOUD";         line = "Busy street traffic"; }
    else             { lab = "HEARING RISK"; line = "Limit your exposure"; }
    if (risk) ui_state(fb, UI_CX, 318, lab, C_AMBER);
    else ui_label(fb, UI_CX, 318, lab, ui_dim(C_PRI, R.txt), UI_CENTER);
    body_line(fb, 356, line, ui_dim(C_SEC, R.txt));
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
    if (!R.rec) ui_page_dots(fb, UI_CX, DOTS_Y, NPAGES, R.px.x / PAGE_W);
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
        if (settled && R.page == 1 && s_nmemo) ui_list_input(&R.list, in);
        return 1;
    case IN_DRAG_START:
        R.pressed = 0;
        if (in->axis == AXIS_X) {
            if (R.page == 1) ui_list_input(&R.list, in);            /* cancels its press */
            if (R.rec) return 1;                                   /* stay put while recording */
            R.dragging = 1;
            R.grab_x = R.px.x;
            /* fallthrough into the first move */
        } else {
            if (R.page == 1 && settled && s_nmemo) return ui_list_input(&R.list, in);
            return 0;
        }
        /* fallthrough */
    case IN_DRAG:
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
        if (R.page == 1 && s_nmemo) ui_list_input(&R.list, in);
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_memo = { "RECORDER", memo_enter, memo_leave, memo_tick, memo_draw, memo_input,
                              SCR_HDRAG };
