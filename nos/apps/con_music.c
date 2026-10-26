/* con_music.c — now-playing remote for the phone's player, and its home widget.
 *
 * Layout (466 round):
 *   - a 40-tick progress ring hugging the bezel (3 x 8 px radial ticks at r 219):
 *     played ticks lit, the leading one stepping its ink; no playhead (the elapsed
 *     time is the precision). It sweeps in with the entry and across a jump;
 *   - a small dot-matrix equaliser at the top that dances while playing and lies
 *     flat when paused (deterministic hashed levels, 10 Hz, one damage box);
 *   - track title (marquee at exactly 1 px per frame when it doesn't fit, with
 *     stepped edge fades), artist in secondary grey;
 *   - prev / play-pause / next: the big white circle is the focal control;
 *   - elapsed / remaining in mono; a volume pill replaces them when the volume
 *     changes (air gesture up/down over the APDS-9960, or a vertical drag).
 * Empty: "[ NOTHING PLAYING ]" / "Play music on your phone"; no phone link:
 * "[ NOT CONNECTED ]". The widget is a compact glance whose centre toggles
 * play/pause directly; anywhere else opens the app. */
#include "nos.h"
#include "pblur.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "prog.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

static unsigned str_hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

/* ------------------------------------------------- shared player state */
static struct {
    int   have, playing, connected;
    int   waiting;           /* a fresh link: musicinfo / musicstate not settled yet */
    char  track[64], artist[48];
    uint32_t pos_ms, dur_ms;
    int   want;              /* optimistic play state after a tap, -1 none */
    nos_time_t t_want;
} P = { .want = -1 };

static void player_poll(void)
{
    const svc_music_t *m = svc_music();
    P.connected = svc_sys()->phone_connected || hal_phone_connected();
    /* A fresh link sends musicinfo, then musicstate a moment later. For its first second
     * nothing is decided: the page on the glass holds (no "Nothing playing" flash), and a
     * title that arrives must stand 0.2 s so its play state is in before the first frame
     * that shows it (no play -> empty -> pause swap). */
    static int s_seen, s_was_conn, s_was_title;
    static nos_time_t s_t_link = -10, s_t_title = -10;
    int title = m->have && m->track[0];
    if (s_seen && P.connected && !s_was_conn) s_t_link = nos_t;
    if (title && !s_was_title) s_t_title = nos_t;
    s_seen = 1; s_was_conn = P.connected; s_was_title = title;
    int fresh = nos_t - s_t_link < 1.0f;
    P.waiting = P.connected && fresh && (!title || nos_t - s_t_title < 0.2f);
    /* the player waits for a title (never "Unknown track 0:00"); without one the last
     * words stay as they were, for the page that is fading out */
    P.have = P.connected && title && !P.waiting;
    if (P.have) {
        snprintf(P.track, sizeof P.track, "%s", m->track);
        snprintf(P.artist, sizeof P.artist, "%s", m->artist);
        P.pos_ms = m->pos_ms;
        P.dur_ms = m->dur_ms;
        if (P.dur_ms && P.pos_ms > P.dur_ms) P.pos_ms = P.dur_ms;
        /* the tap shows at once; the phone's echo (or 2 s of silence) settles it */
        if (P.want >= 0 && (m->playing == P.want || nos_t - P.t_want > 2.0f)) P.want = -1;
        P.playing = P.want >= 0 ? P.want : m->playing;
    } else P.want = -1;
}

static void player_toggle(void)
{
    P.want = !P.playing;
    P.t_want = nos_t;
    P.playing = P.want;
    svc_music_cmd(MUSIC_PLAYPAUSE);
}

static void fmt_ms(char *b, int n, uint32_t ms, int neg)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) snprintf(b, (size_t)n, "%s%u:%02u:%02u", neg ? "-" : "", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(b, (size_t)n, "%s%u:%02u", neg ? "-" : "", (unsigned)(s / 60), (unsigned)(s % 60));
}

/* ====================================================== now playing ===== */
#define RING_R    219.0f
#define RING_N    40
#define TICK_W    3.0f        /* a progress tick: 3 px across ... */
#define TICK_L    8.0f        /* ... 8 px radial */
#define EQ_COLS   11
#define EQ_ROWS   5
#define EQ_PITCH  10.0f
#define EQ_Y      96.0f
#define TITLE_Y   188
#define ARTIST_Y  224
#define CTRL_Y    308.0f
#define PLAY_R    54.0f
#define SIDE_R    40.0f
#define SIDE_DX   106.0f
#define TIME_Y    392
#define MQ_X0     83
#define MQ_X1     383
#define MQ_GAP    56
#define MQ_HOLD   60          /* frames at rest before each lap */
#define VOL_SHOW  1.8f
#define WG_CY_SHARED 212.0f   /* the widget's play disc (see wg_music below) */
#define WG_R_SHARED  60.0f
#define WG_TITLE_Y   344      /* the widget's title and artist baselines */
#define WG_ARTIST_Y  380
#define TRK_OUT_F    3        /* next track: the old words leave over 3 frames ... */
#define TRK_IN_F     4        /* ... then the new ones arrive over 4 (a shared axis, R4) */
#define HOLD_S       (2.5f / NOS_FPS)   /* a quick tap shows its pressed fill >= 3 frames */
#define RING_ARC     (6.2831853f * RING_R / RING_N)   /* px of arc per ring dot */
#define VOL_IN_F     6        /* the volume pill grows on EASE_STEADY ... */
#define VOL_OUT_F    5        /* ... and folds at a constant speed */

enum { B_NONE = 0, B_PREV, B_PLAY, B_NEXT };

/* ---------------------------------------------------------------- tick ring
 * Playback progress is a ring of thin radial ticks hugging the bezel (no playhead: the
 * elapsed time beside the controls is the precision). Tick i is lit by k = clamp(fill -
 * i, 0, 1), the leading one stepping its ink in 8 steps (prog.h's rule), so a playing
 * track repaints one 3 x 8 px tick at a time. Each tick is one polygon (prog_poly):
 * only the ticks whose ink changed are repainted. Corners are computed once. */
typedef struct { int n; float xy[RING_N][8]; } tickset;
static void ticks_init(tickset *t, float cx, float cy, float r, int n, float w, float len)
{
    t->n = n;
    for (int i = 0; i < n; i++) {
        float a = 6.2831853f * ((float)i + 0.5f) / (float)n, s = sinf(a), c = cosf(a);
        float r0 = r - len * 0.5f, r1 = r + len * 0.5f, hx = c * w * 0.5f, hy = s * w * 0.5f;
        float *p = t->xy[i];
        p[0] = cx + r0 * s - hx; p[1] = cy - r0 * c - hy;
        p[2] = cx + r1 * s - hx; p[3] = cy - r1 * c - hy;
        p[4] = cx + r1 * s + hx; p[5] = cy - r1 * c + hy;
        p[6] = cx + r0 * s + hx; p[7] = cy - r0 * c + hy;
    }
}
static void ticks_draw(gfx_fb *fb, const tickset *t, float fill, uint16_t on, uint16_t off)
{
    for (int i = 0; i < t->n; i++) {
        float k = m_clamp(fill - (float)i, 0, 1);
        uint16_t c;
        if (k >= 1) c = on;
        else if (k <= 0) c = off;
        else {
            k = floorf(k * 8.0f + 0.5f) / 8.0f;           /* 8 perceptible steps */
            c = off ? ui_mix(off, on, k) : ui_dim(on, k);
        }
        if (c) prog_poly(fb, t->xy[i], 4, c);
    }
}
static const tickset *app_ticks(void)
{
    static tickset t;
    if (!t.n) ticks_init(&t, UI_CX, UI_CY, RING_R, RING_N, TICK_W, TICK_L);
    return &t;
}

/* A fill that sweeps (the owner: fills must visibly sweep in): from `from` toward the
 * live target on EASE_STEADY in whole frames, over `dur`; then it follows the target. */
typedef struct { nos_time_t t0; float from, dur; } fsweep;
static void fs_start(fsweep *s, float from, nos_time_t t0, float dur) { s->from = from; s->t0 = t0; s->dur = dur; }
static float fs_value(const fsweep *s, float target)
{
    if (nos_t + 0.5f / NOS_FPS < s->t0) return s->from;
    float u = m_clamp((float)(m_frame_k(s->t0) + 1) * (1.0f / NOS_FPS) / s->dur, 0, 1);
    if (u >= 1) return target;
    /* in the ticks' 8 ink steps: nothing creeps below what the glass can show */
    return floorf((s->from + (target - s->from) * bezier_ease(&EASE_STEADY, u)) * 8.0f + 0.5f) / 8.0f;
}
static int fs_running(const fsweep *s) { return nos_t + 0.5f / NOS_FPS >= s->t0 && (float)(m_frame_k(s->t0) + 1) * (1.0f / NOS_FPS) < s->dur; }
/* the entry sweep: 0.45 s plus 0.55 s a lap, so a full ring takes a second */
static float fs_entry_s(float frac) { return 0.45f + 0.55f * m_clamp(frac, 0, 1); }
/* a jump (next, previous, seek): 0.25 s plus 0.5 s a lap of travel */
static float fs_jump_s(float d) { return 0.25f + 0.5f * m_clamp(d, 0, 1); }

/* ---------------------------------------------------------- widget -> app
 * Opening the app from its widget, on the widget's tap clock in whole frames (the first
 * shown frame already moves, with the iris): the play disc travels to its place on
 * EASE_STEADY, and the words change places by a shared axis (R4: no 156 px slide of
 * text): the widget's words rise and fade out over 3 frames, then the app's rise from
 * 20 px below and fade in over 5. Both sides of the iris edge draw the same frame. */
#define WGO_F   8
static int   wgo_k(nos_time_t t_open) { return m_frame_k(t_open) + 1; }
static float wgo_e(int k) { return k >= WGO_F ? 1.0f : k <= 0 ? 0.0f : bezier_ease(&EASE_STEADY, (float)k / WGO_F); }
static int   wgo_out_dy(int k) { return -(k * 4 + (k > 2)); }                       /* -4 -8 -13 */
static int   wgo_in_dy(int k) { static const unsigned char d[5] = { 20, 14, 8, 4, 1 }; return k < 4 ? 20 : k >= WGO_F ? 0 : d[k - 4]; }
static float wgo_out_ink(int k) { return k >= 4 ? 0.0f : m_ink(1.0f - (float)k / 4.0f); }
static float wgo_in_ink(int k) { return k < 4 ? 0.0f : m_ink(m_clamp((float)(k - 3) / 5.0f, 0, 1)); }

static struct {
    nos_time_t t_enter, t_open;
    float build, txt;
    int   bk;                /* whole frames into the entry build (< 0 before it) */
    int   pressed;
    int   held;              /* last pressed control, shown until t_down + HOLD_S */
    nos_time_t t_down;
    unsigned track_h;
    nos_time_t t_track;           /* last track change: old words out, then the new in */
    int   trk_dir;           /* its direction: +1 the words rise (next), -1 they fall (prev) */
    int   swap_dir;          /* the direction the next change will take (the last tap) */
    char  cur_track[64], cur_artist[48];
    char  old_track[64], old_artist[48];
    int   old_w, old_off;    /* the outgoing title's width and marquee offset */
    float txt_old;           /* outgoing words' ink 0..1 (0 = gone) */
    int   dy_old, dy_new;    /* their vertical offsets, px */
    float ink;               /* the times' ink: the build only (a track change keeps them) */
    float tk;                /* the times' ink after the volume pill (back over 3 frames) */
    nos_time_t t_pill;            /* last frame the volume pill was on the glass */
    /* marquee */
    int   mq_w;              /* title width, px */
    int   mq_off;            /* px scrolled */
    int   mq_hold;           /* frames left at rest */
    int   mq_lap;            /* laps run, px (the motion audit's unwrapped position) */
    /* equaliser */
    uint8_t eq[EQ_COLS];
    uint8_t eq_target[EQ_COLS];
    int   eq_step, eq_frame;
    /* ring */
    float ring_tgt;          /* the playback position in ticks (fractional) */
    float ring_show;         /* ticks lit on the glass (sweeps in, and across a jump) */
    fsweep ring_sw;
    /* times: the strings on the glass, and the outgoing pair during a track change */
    char  last_e[16], last_r[16], old_e[16], old_r[16];
    pswap sw;                /* player (0) / nothing playing (1) / phone not connected (2) */
    /* volume */
    int   vol;               /* 0-100 */
    nos_time_t t_vol;             /* last change */
    mtween vol_w;            /* pill width 0..1 */
    int   vol_shown;
    int   drag_vol0, dragging;
    /* next / prev nudge */
    int   nudge;             /* -1 prev, +1 next */
    nos_time_t t_nudge;
    int   icon_playing;      /* icon state for the swap build */
    nos_time_t t_icon;
    int   from_wg;
} M = { .swap_dir = 1, .trk_dir = 1 };

static int s_from_widget;          /* opened by a widget tap: the play disc travels from it */

/* 0 player, 1 nothing playing, 2 phone not connected; a fresh link keeps `held` */
static int kind_of(int held) { return P.have ? 0 : P.waiting ? held : P.connected ? 1 : 2; }
static int music_kind(void) { return kind_of(M.sw.want); }
/* the playback position in ticks of an n-tick ring, quantised to its 8 ink steps */
static float ring_target(int n)
{
    if (!P.dur_ms) return 0;
    float f = (float)((double)P.pos_ms / (double)P.dur_ms);
    return floorf(m_clamp(f, 0, 1) * (float)n * 8.0f) / 8.0f;
}
static void fmt_times(char *e, char *r, int n)
{
    fmt_ms(e, n, P.pos_ms, 0);
    if (P.dur_ms) fmt_ms(r, n, P.dur_ms - P.pos_ms, 1);
    else snprintf(r, (size_t)n, UI_NONE ":" UI_NONE);
}
/* the player starts over (entry, or back from an absent page): words, ring, icon */
static void player_reset(void)
{
    M.track_h = str_hash(P.track);
    M.t_track = -10;
    snprintf(M.cur_track, sizeof M.cur_track, "%s", P.track);
    snprintf(M.cur_artist, sizeof M.cur_artist, "%s", P.artist);
    M.old_track[0] = M.old_artist[0] = 0;
    M.txt_old = 0;
    M.dy_old = M.dy_new = 0;
    M.mq_off = 0; M.mq_hold = MQ_HOLD; M.mq_lap = 0;
    M.mq_w = ui_text_width(UI_F_TITLE, P.track, 0);
    /* the played ticks sweep in from 12 o'clock with the entry build */
    M.ring_tgt = ring_target(RING_N);
    M.ring_show = 0;
    fs_start(&M.ring_sw, 0, M.t_enter, fs_entry_s(M.ring_tgt / RING_N));
    fmt_times(M.last_e, M.last_r, sizeof M.last_e);
    M.icon_playing = P.playing;
    M.t_icon = -10;
}

static nos_time_t wg_t_open(void);

static void music_enter(void)
{
    player_poll();
    /* the build starts at 40% of the opening iris (UI standard) */
    M.t_enter = nos_enter_t();
    M.from_wg = s_from_widget;
    s_from_widget = 0;
    M.t_open = M.from_wg ? wg_t_open() : nos_t;    /* one clock for both sides of the iris */
    M.build = M.txt = 0;
    M.bk = -1;
    M.pressed = M.held = B_NONE;
    M.t_down = -10;
    player_reset();
    M.sw.want = 2;                     /* opened on a fresh link: hold "not connected" */
    ps_init(&M.sw, music_kind());
    memset(M.eq, 0, sizeof M.eq);
    M.eq_step = 0; M.eq_frame = 0;
    int v = svc_music()->volume;
    M.vol = v >= 0 ? v : (M.vol > 0 ? M.vol : 50);
    M.t_vol = -10;
    mtw_start(&M.vol_w, 0, 0, M_FRAMES(1), &EASE_LINEAR);
    M.vol_shown = 0;
    M.t_pill = -10;
    M.tk = 1;
    M.dragging = 0;
    M.nudge = 0;
    M.swap_dir = M.trk_dir = 1;
    hal_light_gesture_enable(1);
}

static void eq_tick(int frames)
{
    M.eq_frame += frames;
    if (M.eq_frame < 3) return;                 /* 10 Hz: dot-matrix, not video */
    M.eq_frame = 0;
    M.eq_step++;
    for (int c = 0; c < EQ_COLS; c++) {
        int lvl;
        if (P.playing) {
            /* lows dance higher than highs; a slow "beat" lifts every column together */
            float shape = 1.0f - 0.45f * (float)abs(c - 3) / (EQ_COLS - 1);
            float beat = (M.eq_step % 5 == 0) ? 0.25f : 0.0f;
            float r = m_hash01((unsigned)c * 7919u + 13u, (unsigned)M.eq_step);
            r *= r;                             /* mostly mid/low, the odd peak */
            lvl = 1 + (int)((EQ_ROWS - 1) * m_clamp(r * shape * 1.3f + beat, 0, 1) + 0.5f);
        } else lvl = 1;                         /* paused: a flat line of dots */
        M.eq_target[c] = (uint8_t)(lvl * 255 / EQ_ROWS);
        /* one row per step toward the target: mechanical, never a jump */
        int cur = M.eq[c] * EQ_ROWS / 255, tgt = lvl;
        if (cur < tgt) cur++;
        else if (cur > tgt) cur--;
        M.eq[c] = (uint8_t)(cur * 255 / EQ_ROWS);
    }
}

static float vol_k(void) { return mtw_value(&M.vol_w); }
static float vol_half(float k) { return floorf((44.0f + 180.0f * k) * 0.5f); }   /* even widths */

/* Motion audit (sim only, docs/NOS_SIM.md): what draw() puts on the glass this frame. */
static void music_probes(void)
{
#ifdef GFX_COST_MODEL
    nos_motion_probe("music.swap.ink", MC_FADE, 100.0f * ps_ink(&M.sw));
    if (M.sw.shown) return;
    if (M.bk >= 0) nos_motion_probe("music.ring.arc", MC_FILL, M.ring_show * RING_ARC);
    if (M.bk < 0 && !M.from_wg) return;
    if (M.from_wg) {
        int k = wgo_k(M.t_open);
        nos_motion_probe("music.disc.y", MC_COMPONENT, m_lerp(WG_CY_SHARED, CTRL_Y, wgo_e(k)));
        if (k < WGO_F) {
            if (k < 4) nos_motion_probe("music.words.out.dy", MC_MICRO, (float)wgo_out_dy(k));
            else nos_motion_probe("music.words.in.dy", MC_MICRO, (float)wgo_in_dy(k));
            nos_motion_probe(k < 4 ? "music.words.out.ink" : "music.words.in.ink", MC_FADE,
                             100.0f * (k < 4 ? wgo_out_ink(k) : wgo_in_ink(k)));
        }
    }
    if (M.txt_old > 0) {
        nos_motion_probe("music.trk.old.ink", MC_FADE, 100.0f * M.txt_old);
        nos_motion_probe("music.trk.old.dy", MC_MICRO, (float)M.dy_old);
    } else if (M.t_track > -5 && m_frame_k(M.t_track) <= TRK_OUT_F + TRK_IN_F) {
        nos_motion_probe("music.trk.new.ink", MC_FADE, 100.0f * M.txt);
        nos_motion_probe("music.trk.new.dy", MC_MICRO, (float)M.dy_new);
    }
    if (M.mq_w > MQ_X1 - MQ_X0 && M.txt > 0 && M.txt_old <= 0)
        nos_motion_probe("music.marquee", MC_TRACKED, (float)(M.mq_lap + M.mq_off));
    nos_motion_probe("music.nudge", MC_MICRO, (float)(M.nudge * 4));
    if (vol_k() > 0.01f) nos_motion_probe("music.vol.half", MC_COMPONENT, vol_half(vol_k()));
    nos_motion_probe("music.times.ink", MC_FADE, 100.0f * M.tk * M.ink);
    int rows = M.eq[3] * EQ_ROWS / 255;
    nos_motion_probe("music.eq3", MC_DECORATIVE, (float)(rows < 1 ? 1 : rows) * EQ_PITCH);
#endif
}

static void music_gestures(void)
{
    /* Only the foreground consumer owns the APDS queue and renews its lease. */
    if (nos_top() == &scr_music && !nos_transitioning()) {
        hal_light_gesture_enable(P.have);
        if (!P.have) return;
        for (int i = 0; i < 4; i++) {
            int gesture = hal_light_gesture();
            if (gesture == GEST_NONE) break;
            nos_air(gesture);
        }
    }
}

static void music_tick(float dt)
{
    music_gestures();
    int frames = (int)lroundf(dt * NOS_FPS);
    if (frames < 1) frames = 1;
    player_poll();
    /* phone or player gone (or back): the page leaves, then the other one builds */
    if (ps_tick(&M.sw, music_kind())) {
        M.t_enter = M.sw.t_in;
        M.from_wg = 0;
        player_reset();
    }
    M.build = q((nos_t - M.t_enter) / UI_ENTRY_S, 32);
    M.bk = nos_t + 0.5f / NOS_FPS < M.t_enter ? -1 : m_frame_k(M.t_enter);
    /* a new track is detected before the fade is computed, so it is never drawn at full ink */
    unsigned h = str_hash(P.track);
    if (h != M.track_h) {
        /* keep the outgoing words, times (and where the marquee had them) for their step-down */
        if (M.t_track < -5 || m_frame_k(M.t_track) >= TRK_OUT_F) {   /* not mid-change already */
            memcpy(M.old_track, M.cur_track, sizeof M.old_track);
            memcpy(M.old_artist, M.cur_artist, sizeof M.old_artist);
            memcpy(M.old_e, M.last_e, sizeof M.old_e);
            memcpy(M.old_r, M.last_r, sizeof M.old_r);
            M.old_w = M.mq_w; M.old_off = M.mq_off;
            M.trk_dir = M.swap_dir;
        }
        M.swap_dir = 1;                          /* the phone's own changes go forward */
        M.track_h = h;
        M.t_track = nos_t;
        M.mq_w = ui_text_width(UI_F_TITLE, P.track, 0);
        M.mq_off = 0; M.mq_hold = MQ_HOLD; M.mq_lap = 0;
    }
    snprintf(M.cur_track, sizeof M.cur_track, "%s", P.track);
    snprintf(M.cur_artist, sizeof M.cur_artist, "%s", P.artist);
    /* words arrive with the build at full ink (from the widget they are already there).
     * A new track is a shared axis in whole frames: the old words rise 4 px a frame as
     * they fade (62 / 38 / 12 %), then the new rise from 10 px below into place as they
     * fade in (12 / 38 / 62 / 100 %); a "previous" mirrors it downwards. */
    float tf = M.bk >= 0 || M.from_wg ? 1.0f : 0.0f;
    /* the times step in with the second half of the build (4 perceptual steps): the
     * position a reconnect sends just after the title is there before they show */
    M.ink = M.bk < 0 ? 0.0f : m_ink(m_clamp((float)(M.bk - 3) / 4.0f, 0, 1));
    int f = M.t_track < -5 ? 99 : m_frame_k(M.t_track);
    static const unsigned char in_dy[TRK_IN_F] = { 10, 6, 3, 1 };
    M.txt_old = f < TRK_OUT_F && M.old_track[0] ? m_ink(1.0f - (float)(f + 1) / (TRK_OUT_F + 1)) : 0;
    M.dy_old = f < TRK_OUT_F ? -M.trk_dir * 4 * (f + 1) : 0;
    float tt = f < TRK_OUT_F ? 0 : m_ink(m_clamp((float)(f - TRK_OUT_F + 1) / TRK_IN_F, 0, 1));
    M.dy_new = f >= TRK_OUT_F && f - TRK_OUT_F < TRK_IN_F ? M.trk_dir * in_dy[f - TRK_OUT_F] : 0;
    M.txt = tf < tt ? tf : tt;

    /* marquee: whole pixels per frame so the scroll is perfectly even */
    if (M.mq_w > MQ_X1 - MQ_X0) {
        if (M.mq_hold > 0) M.mq_hold -= frames;
        else {
            M.mq_off += frames;
            if (M.mq_off >= M.mq_w + MQ_GAP) { M.mq_lap += M.mq_off; M.mq_off = 0; M.mq_hold = MQ_HOLD; }
        }
    } else { M.mq_off = 0; M.mq_hold = MQ_HOLD; }

    eq_tick(frames);

    /* ring: playing, the leading tick steps its ink (one small repaint every few
     * seconds); a jump (next, previous, seek, reconnect) sweeps the lit arc across to the
     * new place on EASE_STEADY, from the frame it is seen, never a cut */
    float tgt = ring_target(RING_N);
    if (M.bk >= 0 && (fs_running(&M.ring_sw) ? fabsf(tgt - M.ring_tgt) > 1.0f : fabsf(tgt - M.ring_show) > 1.0f))
        fs_start(&M.ring_sw, M.ring_show, nos_t, fs_jump_s(fabsf(tgt - M.ring_show) / RING_N));
    M.ring_tgt = tgt;
    M.ring_show = fs_value(&M.ring_sw, tgt);
    fmt_times(M.last_e, M.last_r, sizeof M.last_e);

    /* the svc volume wins once the phone reports one */
    int sv = svc_music()->volume;
    if (sv >= 0 && nos_t - M.t_vol > 0.6f) M.vol = sv;
    int show = nos_t - M.t_vol < VOL_SHOW || M.dragging;
    if (show != M.vol_shown) {
        M.vol_shown = show;
        float cur = mtw_value(&M.vol_w);
        /* a pill: grows on EASE_STEADY (showing this frame), folds at a constant speed */
        if (show) { mtw_start(&M.vol_w, cur, 1.0f, M_FRAMES(VOL_IN_F), &EASE_STEADY); mtw_lead(&M.vol_w); }
        else mtw_start(&M.vol_w, cur, 0.0f, M_FRAMES(VOL_OUT_F) * cur, &EASE_LINEAR);
    }
    /* the times come back over 3 perceptual steps once the pill has folded */
    if (vol_k() > 0.01f) M.t_pill = nos_t;
    M.tk = m_ink(m_clamp((float)m_frame_k(M.t_pill) / 3.0f, 0, 1));
    if (P.playing != M.icon_playing) { M.icon_playing = P.playing; M.t_icon = nos_t; }
    if (M.nudge && m_frame_k(M.t_nudge) >= 4) M.nudge = 0;
    if (P.playing) nos_keep_awake();
    music_probes();
}

/* ------------------------------------------------------------- drawing */
/* The progress ring: the unplayed ticks arrive with the build (4 perceptual steps), the
 * played ones sweep round from 12 o'clock over them (0.45 s + 0.55 s a lap). */
static float track_ink(int bk) { return bk < 0 ? 0.0f : m_ink(m_clamp((float)(bk + 1) / 4.0f, 0, 1)); }
static void draw_ring(gfx_fb *fb)
{
    float tk = track_ink(M.bk);
    if (tk <= 0) return;
    ticks_draw(fb, app_ticks(), M.bk < 0 ? 0 : M.ring_show, C_PRI, tk < 1 ? ui_dim(C_LINE2, tk) : C_LINE2);
}

/* Title clipped to [MQ_X0, MQ_X1) with stepped fades at the edges it scrolls under.
 * w = its width, off = px scrolled. */
static void marquee(gfx_fb *fb, const char *s, int y, uint16_t col, int w, int off)
{
    const font_t *f = UI_F_TITLE;
    int win = MQ_X1 - MQ_X0;
    int top = y - f->ascent - 3, bot = y + (f->line_height - f->ascent) + 3;
    if (w <= win) { ui_text(fb, f, UI_CX, y, s, col, UI_CENTER, 0); return; }
    if (fb->rec) {
        unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_u(gfx_hash_u(str_hash(s), (unsigned)off), col), 0x3A11u), (unsigned)y);
        gfx_rec_cmd(fb, MQ_X0, top, MQ_X1, bot, h);
        return;
    }
    /* bands: [edge 14 px at 1/3][next 14 px at 2/3][middle full] on each side; the
     * left fade only while text is scrolling under it */
    const int B = 14;
    int lf = off > 0;
    struct { int x0, x1; float k; } band[5] = {
        { MQ_X0,         MQ_X0 + B,     lf ? 0.25f : 1.0f },
        { MQ_X0 + B,     MQ_X0 + 2 * B, lf ? 0.6f : 1.0f },
        { MQ_X0 + 2 * B, MQ_X1 - 2 * B, 1.0f },
        { MQ_X1 - 2 * B, MQ_X1 - B,     0.6f },
        { MQ_X1 - B,     MQ_X1,         0.25f },
    };
    int x = MQ_X0 - off;
    for (int i = 0; i < 5; i++) {
        gfx_fb sub;
        if (!nos_subview(fb, &sub, band[i].x0, top, band[i].x1, bot, 0, 0)) continue;
        uint16_t c = band[i].k >= 1 ? col : ui_dim(col, band[i].k);
        ui_text(&sub, f, x, y, s, c, UI_LEFT, 0);
        ui_text(&sub, f, x + w + MQ_GAP, y, s, c, UI_LEFT, 0);
    }
}

/* The words while the app opens from its widget (k = wgo_k < WGO_F): the widget's layout
 * leaving, then the app's arriving. The widget and the app draw the same thing, so the
 * iris edge shows no seam. tw = the title's width. */
static void wgo_words(gfx_fb *fb, int k, int tw)
{
    if (k < 4) {
        float ink = wgo_out_ink(k);
        int dy = wgo_out_dy(k), x0, x1;
        ui_row_span(340, 44, &x0, &x1);
        ui_text_fit(fb, UI_F_TITLE, UI_CX, WG_TITLE_Y + dy, P.track, ui_dim(C_WHITE, ink), UI_CENTER, x1 - x0);
        ui_row_span(380, 56, &x0, &x1);
        if (P.artist[0]) ui_text_fit(fb, UI_F_BODY, UI_CX, WG_ARTIST_Y + dy, P.artist, ui_dim(C_SEC, ink), UI_CENTER, x1 - x0);
        return;
    }
    float ink = wgo_in_ink(k);
    int dy = wgo_in_dy(k);
    marquee(fb, P.track, TITLE_Y + dy, ui_dim(C_WHITE, ink), tw, 0);
    if (P.artist[0]) ui_text_fit(fb, UI_F_BODY, UI_CX, ARTIST_Y + dy, P.artist, ui_dim(C_SEC, ink), UI_CENTER, MQ_X1 - MQ_X0);
}

static void side_button(gfx_fb *fb, float cx, float r, int icon, int pressed, int nudge, float p)
{
    ui_circle(fb, cx, CTRL_Y, r, pressed ? C_LINE2 : C_SURF2);
    float pitch = 3.4f * (r / SIDE_R);
    dot_icon_anim(fb, icon, cx + (float)(nudge * 4), CTRL_Y, pitch, pitch * 0.8f, C_PRI, p, DOT_REVEAL_CENTER);
}

static void draw_volume(gfx_fb *fb, float k)
{
    const float full = 224, h = 44;
    float w = 2.0f * vol_half(k);               /* even widths: symmetric about UI_CX */
    float y = TIME_Y - 26;
    ui_rrect(fb, UI_CX - w / 2, y, w, h, h / 2, C_SURF2);
    if (k < 0.92f) {
        dot_icon(fb, DOT_ICON_SPEAKER, UI_CX, y + h / 2, 2.2f, 1.8f, C_SEC);
        return;
    }
    float x0 = UI_CX - full / 2;
    dot_icon(fb, DOT_ICON_SPEAKER, x0 + 28, y + h / 2, 2.2f, 1.8f, C_PRI);
    /* the level: ten solid segments (the control centre's volume and brightness read alike) */
    prog_bar(fb, PROG_SEGMENTS, x0 + 52, y + h / 2 - 7, 156, 14, 10, (float)M.vol / 100.0f, C_WHITE, C_LINE2);
}

static void draw_empty(gfx_fb *fb, float build, int connected)
{
    if (!connected)
        ui_absent(fb, DOT_ICON_BT, "Phone not connected", "Pair in Gadgetbridge as Bangle.js NOS", NULL, build);
    else
        ui_absent(fb, DOT_ICON_MUSIC, "Nothing playing", "Play music on your phone", NULL, build);
}

static int shown_pressed(int b) { return M.pressed == b || (M.held == b && nos_t - M.t_down < HOLD_S); }

static void draw_times(gfx_fb *fb, const char *e, const char *r, float k)
{
    if (k <= 0) return;
    ui_text(fb, UI_F_LABEL, UI_CX - 14, TIME_Y, e, ui_dim(C_WHITE, k), UI_RIGHT, 1);
    dot_disc(fb, UI_CX, TIME_Y - 6, 4, ui_dim(C_DIS, k));
    ui_text(fb, UI_F_LABEL, UI_CX + 14, TIME_Y, r, ui_dim(C_DIS, k), UI_LEFT, 1);
}

static void draw_player(gfx_fb *fb);

static void music_draw(gfx_fb *fb)
{
    ui_title(fb, "MUSIC", 0);
    int d0 = ps_dim(fb, &M.sw);
    if (M.sw.shown) {
        /* the empty ring arrives with the absent page's text (4 perceptual steps) */
        float tk = track_ink(M.bk);
        if (tk > 0) ticks_draw(fb, app_ticks(), 0, C_PRI, ui_dim(C_LINE, tk));
        draw_empty(fb, M.build, M.sw.shown == 1);
    } else draw_player(fb);
    fb->dim = d0;
}

static void draw_player(gfx_fb *fb)
{
    draw_ring(fb);
    if (M.bk < 0 && !M.from_wg) return;            /* the iris shows black until 40% */

    /* equaliser: columns rise one row a frame with the build */
    uint8_t ev[EQ_COLS];
    int cap = M.bk < 0 ? 0 : M.bk + 1;
    for (int c = 0; c < EQ_COLS; c++) {
        int rows = M.eq[c] * EQ_ROWS / 255;
        if (rows < 1) rows = 1;
        ev[c] = (uint8_t)((rows < cap ? rows : cap) * 255 / EQ_ROWS);
    }
    dot_columns(fb, UI_CX - EQ_COLS * EQ_PITCH * 0.5f, EQ_Y, EQ_COLS, EQ_ROWS, EQ_PITCH, 7.0f, ev,
                P.playing ? C_PRI : C_DIS, 0);

    /* words: opening from the widget they change places by a shared axis; a new track
     * swaps them by a short one (old rise out, new rise in; "previous" falls) */
    int wk = M.from_wg ? wgo_k(M.t_open) : WGO_F;
    if (wk < WGO_F) wgo_words(fb, wk, M.mq_w);
    else if (M.txt_old > 0) {
        marquee(fb, M.old_track, TITLE_Y + M.dy_old, ui_dim(C_WHITE, M.txt_old), M.old_w, M.old_off);
        if (M.old_artist[0]) ui_text_fit(fb, UI_F_BODY, UI_CX, ARTIST_Y + M.dy_old, M.old_artist, ui_dim(C_SEC, M.txt_old), UI_CENTER, MQ_X1 - MQ_X0);
    } else if (M.txt > 0) {
        marquee(fb, P.track, TITLE_Y + M.dy_new, ui_dim(C_WHITE, M.txt), M.mq_w, M.mq_off);
        if (P.artist[0]) ui_text_fit(fb, UI_F_BODY, UI_CX, ARTIST_Y + M.dy_new, P.artist, ui_dim(C_SEC, M.txt), UI_CENTER, MQ_X1 - MQ_X0);
    }

    /* controls are there at full size; their icons build from the centre */
    float ib = q((nos_t - M.t_enter) / UI_ENTRY_S, 32);
    side_button(fb, UI_CX - SIDE_DX, SIDE_R, DOT_ICON_PREV, shown_pressed(B_PREV), M.nudge < 0 ? -1 : 0, ib);
    side_button(fb, UI_CX + SIDE_DX, SIDE_R, DOT_ICON_NEXT, shown_pressed(B_NEXT), M.nudge > 0 ? 1 : 0, ib);
    /* from the widget the play disc travels from the widget's place and size into its own */
    float e = wgo_e(wk);
    float cy = m_lerp(WG_CY_SHARED, CTRL_Y, e), pr = m_lerp(WG_R_SHARED, PLAY_R, e);
    ui_circle(fb, UI_CX, cy, pr, shown_pressed(B_PLAY) ? C_SEC : C_WHITE);
    int icon = P.playing ? DOT_ICON_PAUSE : DOT_ICON_PLAY;
    float ip = m_clamp((float)(m_frame_k(M.t_icon) + 1) / 4.0f, 0, 1);   /* 4 frames, the tap's frame shows */
    float pitch = 4.4f * (pr / PLAY_R);
    /* the play triangle sits right of its cell centre; nudge it back to optical centre */
    float ox = icon == DOT_ICON_PLAY ? -pitch * 0.9f : 0;
    float ipb = M.from_wg ? 1.0f : ib;
    dot_icon_anim(fb, icon, UI_CX + ox, cy, pitch, pitch * 0.8f, C_BG, ip < ipb ? ip : ipb, DOT_REVEAL_CENTER);

    float vk = vol_k();
    if (vk > 0.01f) { draw_volume(fb, vk); return; }
    /* the times change with the title: the old pair steps down with the old words,
     * the new pair steps up with the new ones */
    if (M.txt_old > 0) draw_times(fb, M.old_e, M.old_r, M.ink * M.tk * M.txt_old);
    else draw_times(fb, M.last_e, M.last_r, M.ink * M.tk * M.txt);
}

/* --------------------------------------------------------------- input */
static int ctrl_at(int x, int y)
{
    float dy = y - CTRL_Y, dx;
    dx = x - UI_CX;
    if (dx * dx + dy * dy <= (PLAY_R + 10) * (PLAY_R + 10)) return B_PLAY;
    dx = x - (UI_CX - SIDE_DX);
    if (dx * dx + dy * dy <= 48 * 48) return B_PREV;
    dx = x - (UI_CX + SIDE_DX);
    if (dx * dx + dy * dy <= 48 * 48) return B_NEXT;
    return B_NONE;
}

static void set_volume(int v, int send_steps)
{
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    int steps = (v - M.vol) / 10;
    M.t_vol = nos_t;
    if (v == M.vol) { nos_feedback(FB_LIMIT); return; }
    if (send_steps)
        for (int i = 0; i < (steps < 0 ? -steps : steps); i++) svc_music_cmd(steps > 0 ? MUSIC_VOLUP : MUSIC_VOLDOWN);
    M.vol = v;
    nos_feedback((v == 0 || v == 100) ? FB_LIMIT : FB_TICK);
}

/* next (+1) / previous (-1): the glyph nudges 4 px for 4 frames, and the coming track
 * change takes the direction (the new words rise for next, fall for previous) */
static void skip(int dir)
{
    svc_music_cmd(dir > 0 ? MUSIC_NEXT : MUSIC_PREV);
    M.nudge = dir;
    M.t_nudge = nos_t;
    M.swap_dir = dir;
    nos_feedback(FB_TAP);
}

static int music_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { M.pressed = M.held = B_NONE; M.dragging = 0; M.t_down = -10; return 1; }

    switch (in->type) {
    case IN_DOWN:
        M.pressed = M.held = P.have ? ctrl_at(in->x, in->y) : B_NONE;
        M.t_down = nos_t;
        return 1;
    case IN_TAP: {
        int b = P.have ? ctrl_at(in->x, in->y) : B_NONE;
        M.pressed = B_NONE;
        if (b == B_PLAY) { player_toggle(); nos_feedback(FB_TAP); }
        else if (b == B_NEXT) skip(1);
        else if (b == B_PREV) skip(-1);
        return 1;
    }
    case IN_DRAG_START:
        M.pressed = M.held = B_NONE;
        if (in->axis != AXIS_Y || !P.have) return 0;
        M.dragging = 1;
        M.drag_vol0 = M.vol;
        M.t_vol = nos_t;
        return 1;
    case IN_DRAG: {
        if (!M.dragging) return 0;
        /* 26 px per 10%: up = louder */
        int v = M.drag_vol0 + 10 * (int)lroundf(-in->dy / 26.0f);
        v = v < 0 ? 0 : v > 100 ? 100 : v;
        if (v != M.vol) set_volume(v, 1);
        M.t_vol = nos_t;
        return 1;
    }
    case IN_DRAG_END:
        if (!M.dragging) return 0;
        M.dragging = 0;
        M.t_vol = nos_t;
        return 1;
    case IN_UP:
        M.pressed = B_NONE;
        if (M.dragging) { M.dragging = 0; M.t_vol = nos_t; }
        return 1;
    case IN_AIR:
        if (!P.have) return 0;
        if (in->x == GEST_UP) { set_volume(M.vol + 10, 1); return 1; }
        if (in->x == GEST_DOWN) { set_volume(M.vol - 10, 1); return 1; }
        if (in->x == GEST_LEFT) { skip(-1); return 1; }
        if (in->x == GEST_RIGHT) { skip(1); return 1; }
        return 0;
    default:
        return 0;
    }
}

static void music_leave(void) { hal_light_gesture_enable(0); }
const nos_screen scr_music = { "MUSIC", music_enter, music_leave, music_tick, music_draw, music_input, 0 };

/* ============================================================ widget ===== */
#define WG_CY     WG_CY_SHARED
#define WG_R      WG_R_SHARED
#define WG_RING_R 88.0f
#define WG_RING_N 36

static struct {
    nos_time_t t_shown;
    float build;
    float fill, tgt;         /* ticks lit on the glass, the playback position in ticks */
    fsweep fsw;
    nos_time_t fs_shown;          /* the build the sweep belongs to (t_shown) */
    int   down;              /* a finger rests on the play disc */
    nos_time_t t_down;            /* since when */
    nos_time_t t_tap;             /* last play/pause tap (the fill holds 3 frames) */
    nos_time_t t_open;            /* opened the app (its disc and words move with it) */
    int   bk;                /* whole frames into the ring build (< 0 before it) */
    int   tw;                /* the title's width while the app opens from here */
    int   icon_playing;
    nos_time_t t_icon;
    pswap sw;                /* the app's page swap, on the widget too (kinds as music_kind) */
    int   sw_inited;
} G = { .t_shown = -10, .t_down = -10, .t_tap = -10, .t_open = -10, .t_icon = -10 };

/* the app's clock for an open from here (music_enter reads it) */
static nos_time_t wg_t_open(void) { return G.t_open; }
/* whole frames into an open from this widget (wgo_k, held at WGO_F once the app covers
 * it); 0 at rest */
static int wg_open_k(void)
{
    if (G.t_open < -5 || nos_t < G.t_open) return 0;
    int k = wgo_k(G.t_open);
    return k > WGO_F + 15 ? 0 : k < WGO_F ? k : WGO_F;
}

static int wg_on_disc(int x, int y)
{
    float dx = x - UI_CX, dy = y - WG_CY;
    return dx * dx + dy * dy <= (WG_R + 16) * (WG_R + 16);
}
/* Pressed as soon as home reports the finger (home already waits out a page drag's
 * slop), and for at least 3 frames on a quick tap. */
static int wg_pressed(void)
{
    return G.down || nos_t - G.t_tap < 2.5f / NOS_FPS;
}

static void wg_tick(float dt, int visible)
{
    /* the player state is read off-glass too: the page slides in with the right glyph
     * (no play -> empty -> pause swap); the ring empties, to fill once the page settles */
    player_poll();
    if (!G.sw_inited) { ps_init(&G.sw, kind_of(2)); G.sw_inited = 1; }
    if (!visible) {
        ps_init(&G.sw, kind_of(G.sw.want));         /* off-glass: no swap to watch */
        G.t_shown = 1e9f; G.icon_playing = P.playing; G.t_icon = -10;
        return;
    }
    /* phone or player gone (or back): the page leaves, then the other one builds */
    if (ps_tick(&G.sw, kind_of(G.sw.want))) {
        G.t_shown = G.sw.t_in;
        G.icon_playing = P.playing; G.t_icon = -10;  /* the glyph builds with the page */
    }
    G.build = nos_t < G.t_shown ? 0 : q((nos_t - G.t_shown) / UI_ENTRY_S, 32);
    G.bk = nos_t + 0.5f / NOS_FPS < G.t_shown ? -1 : m_frame_k(G.t_shown);
    if (wg_open_k()) G.tw = ui_text_width(UI_F_TITLE, P.track, 0);
    {   /* the ring: swept in from the page's build, a jump swept across (as the app's) */
        float tgt = ring_target(WG_RING_N);
        if (G.bk < 0 || G.fs_shown != G.t_shown) {     /* a fresh build: from empty */
            G.fill = 0; G.fs_shown = G.t_shown;
            fs_start(&G.fsw, 0, G.t_shown, fs_entry_s(tgt / WG_RING_N));
        }
        else if (fs_running(&G.fsw) ? fabsf(tgt - G.tgt) > 1.0f : fabsf(tgt - G.fill) > 1.0f)
            fs_start(&G.fsw, G.fill, nos_t, fs_jump_s(fabsf(tgt - G.fill) / WG_RING_N));
        G.tgt = tgt;
        if (G.bk >= 0) G.fill = fs_value(&G.fsw, tgt);
    }
    if (P.playing != G.icon_playing) { G.icon_playing = P.playing; G.t_icon = nos_t; }
#ifdef GFX_COST_MODEL
    nos_motion_probe("wg.music.swap.ink", MC_FADE, 100.0f * ps_ink(&G.sw));
    if (!G.sw.shown) {
        int k = wg_open_k();
        nos_motion_probe("wg.music.disc.y", MC_COMPONENT, m_lerp(WG_CY, CTRL_Y, k ? wgo_e(k) : 0.0f));
        if (G.bk >= 0) nos_motion_probe("wg.music.ring.arc", MC_FILL, G.fill * 6.2831853f * WG_RING_R / WG_RING_N);
        if (k && k < WGO_F)
            nos_motion_probe(k < 4 ? "wg.music.words.out.dy" : "wg.music.words.in.dy", MC_MICRO,
                             (float)(k < 4 ? wgo_out_dy(k) : wgo_in_dy(k)));
    }
#endif
}

static void wg_shown(void) { G.t_shown = nos_t; G.t_open = -10; }

static void wg_player(gfx_fb *fb);
static void wg_draw(gfx_fb *fb)
{
    __typeof__(G) saved = G;
    __typeof__(P) player = P;
    if (pblur_baking) {
        /* Progress advances even off screen. One latched source for the whole
         * striped bake, so it cannot mix two playback positions. */
        static __typeof__(P) *snapshot;
        if (!snapshot) snapshot = hal_big_alloc(sizeof *snapshot);
        if (!snapshot) { if (fb->rec) fb->rec->overflow = 1; return; }
        if (!pblur_busy(4)) *snapshot = P;
        P = *snapshot;
        ps_init(&G.sw, kind_of(G.sw.want));
        G.build = 1; G.bk = 1000; G.fill = ring_target(WG_RING_N);
        G.t_shown = G.t_icon = G.t_open = G.t_tap = -10; G.down = 0;
        G.icon_playing = P.playing;
    }
    ui_title(fb, "MUSIC", 0);
    int d0 = ps_dim(fb, &G.sw);
    if (G.sw.shown) {
        /* at rest the page is simply there; after a swap it builds */
        float p = G.sw.t_in > -5 && nos_t - G.sw.t_in < UI_ENTRY_S ? q((nos_t - G.sw.t_in) / UI_ENTRY_S, 32) : 1.0f;
        draw_empty(fb, p, G.sw.shown == 1);
    } else wg_player(fb);
    fb->dim = d0;
    if (pblur_baking) { G = saved; P = player; }
}

static void wg_player(gfx_fb *fb)
{
    /* the app's tick ring in small: the track arrives, the played ticks sweep in */
    float tk = track_ink(G.bk);
    if (tk > 0) {
        static tickset t;
        if (!t.n) ticks_init(&t, UI_CX, WG_CY, WG_RING_R, WG_RING_N, 3.0f, 7.0f);
        ticks_draw(fb, &t, G.fill, C_SEC, tk < 1 ? ui_dim(C_LINE2, tk) : C_LINE2);
    }
    /* opening the app: the disc and the words move into the app's places here too, so
     * both sides of the iris edge show the same frame (no ghost of the old layout) */
    int k = wg_open_k();
    float e = k ? wgo_e(k) : 0.0f;
    if (k) wgo_words(fb, k, G.tw);
    else {
        int x0, x1;
        ui_row_span(340, 44, &x0, &x1);
        ui_text_fit(fb, UI_F_TITLE, UI_CX, WG_TITLE_Y, P.track, C_WHITE, UI_CENTER, x1 - x0);
        ui_row_span(380, 56, &x0, &x1);
        if (P.artist[0]) ui_text_fit(fb, UI_F_BODY, UI_CX, WG_ARTIST_Y, P.artist, C_SEC, UI_CENTER, x1 - x0);
    }
    float dcy = m_lerp(WG_CY, CTRL_Y, e), dr = m_lerp(WG_R, PLAY_R, e);
    ui_circle(fb, UI_CX, dcy, dr, wg_pressed() ? C_SEC : C_WHITE);
    int icon = P.playing ? DOT_ICON_PAUSE : DOT_ICON_PLAY;
    float pitch = 4.4f * (dr / PLAY_R);            /* the app's glyph scale */
    float ox = icon == DOT_ICON_PLAY ? -pitch * 0.9f : 0;
    dot_icon_anim(fb, icon, UI_CX + ox, dcy, pitch, pitch * 0.8f, C_BG,
                  m_clamp((float)(m_frame_k(G.t_icon) + 1) / 4.0f, 0, 1), DOT_REVEAL_CENTER);
}

static void wg_press(int x, int y, int down)
{
    if (down && P.have && wg_on_disc(x, y)) { G.down = 1; G.t_down = nos_t; }
    else G.down = 0;
}

static void wg_tap(int x, int y)
{
    if (P.have && wg_on_disc(x, y)) {
        G.t_tap = nos_t;
        G.down = 0;
        player_toggle();
        nos_feedback(FB_TAP);
        return;
    }
    G.down = 0; G.t_tap = -10;                      /* nothing stays pressed behind the app */
    G.t_open = nos_t;
    G.tw = ui_text_width(UI_F_TITLE, P.track, 0);
    s_from_widget = 1;
    nos_open(&scr_music, UI_CX, (int)WG_CY);       /* the iris grows from the play disc */
}

const nos_widget wg_music = { "MUSIC", wg_tick, wg_draw, wg_tap, wg_shown, wg_press };
