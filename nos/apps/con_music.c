/* con_music.c — now-playing remote for the phone's player, and its home widget.
 *
 * Layout (466 round):
 *   - a 60-dot progress ring hugging the bezel: played dots lit, the current
 *     position a bigger white dot (discrete, Nothing-style);
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

static float q(float x, float n) { return floorf(m_clamp(x, 0, 1) * n + 0.5f) / n; }

static unsigned str_hash(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

/* ------------------------------------------------- shared player state */
static struct {
    int   have, playing, connected;
    char  track[64], artist[48];
    uint32_t pos_ms, dur_ms;
    int   want;              /* optimistic play state after a tap, -1 none */
    float t_want;
} P = { .want = -1 };

static void player_poll(void)
{
    const svc_music_t *m = svc_music();
    P.have = m->have && (m->track[0] || m->artist[0]);   /* an empty musicinfo is not a player */
    P.connected = svc_sys()->phone_connected || hal_phone_connected();
    snprintf(P.track, sizeof P.track, "%s", m->track[0] ? m->track : "Unknown track");
    snprintf(P.artist, sizeof P.artist, "%s", m->artist);
    P.pos_ms = m->pos_ms;
    P.dur_ms = m->dur_ms;
    if (P.dur_ms && P.pos_ms > P.dur_ms) P.pos_ms = P.dur_ms;
    /* the tap shows at once; the phone's echo (or 2 s of silence) settles it */
    if (P.want >= 0 && (m->playing == P.want || nos_t - P.t_want > 2.0f)) P.want = -1;
    P.playing = P.want >= 0 ? P.want : m->playing;
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
#define RING_N    60
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

enum { B_NONE = 0, B_PREV, B_PLAY, B_NEXT };

static struct {
    float t_enter;
    float build, txt;
    int   pressed;
    unsigned track_h;
    float t_track;           /* last track change: text fades in */
    /* marquee */
    int   mq_w;              /* title width, px */
    int   mq_off;            /* px scrolled */
    int   mq_hold;           /* frames left at rest */
    /* equaliser */
    uint8_t eq[EQ_COLS];
    uint8_t eq_target[EQ_COLS];
    int   eq_step, eq_frame;
    /* ring */
    int   ring_pos;          /* current dot index */
    /* volume */
    int   vol;               /* 0-100 */
    float t_vol;             /* last change */
    mtween vol_w;            /* pill width 0..1 */
    int   vol_shown;
    int   drag_vol0, dragging;
    /* next / prev nudge */
    int   nudge;             /* -1 prev, +1 next */
    float t_nudge;
    int   icon_playing;      /* icon state for the swap build */
    float t_icon;
    int   from_wg;
} M;

static int s_from_widget;          /* opened by a widget tap: the play disc travels from it */

static void music_enter(void)
{
    player_poll();
    /* the build starts at 40% of the opening iris (UI standard) */
    M.t_enter = nos_t + 0.4f * 0.42f;
    M.from_wg = s_from_widget;
    s_from_widget = 0;
    M.build = M.txt = 0;
    M.pressed = B_NONE;
    M.track_h = str_hash(P.track);
    M.t_track = -10;
    M.mq_off = 0; M.mq_hold = MQ_HOLD;
    M.mq_w = ui_text_width(UI_F_TITLE, P.track, 0);
    memset(M.eq, 0, sizeof M.eq);
    M.eq_step = 0; M.eq_frame = 0;
    int v = svc_music()->volume;
    M.vol = v >= 0 ? v : (M.vol > 0 ? M.vol : 50);
    M.t_vol = -10;
    mtw_start(&M.vol_w, 0, 0, 0.01f, &EASE_STANDARD);
    M.vol_shown = 0;
    M.dragging = 0;
    M.nudge = 0;
    M.icon_playing = P.playing;
    M.t_icon = -10;
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

static void music_tick(float dt)
{
    int frames = (int)lroundf(dt * NOS_FPS);
    if (frames < 1) frames = 1;
    player_poll();
    M.build = q((nos_t - M.t_enter) / UI_ENTRY_S, 32);
    /* a new track is detected before the fade is computed, so it is never drawn at full ink */
    unsigned h = str_hash(P.track);
    if (h != M.track_h) {
        M.track_h = h;
        M.t_track = nos_t;
        M.mq_w = ui_text_width(UI_F_TITLE, P.track, 0);
        M.mq_off = 0; M.mq_hold = MQ_HOLD;
    }
    /* words arrive with the build at full ink; a new track crossfades in 4 steps */
    float tf = nos_t >= M.t_enter ? 1.0f : 0.0f;
    float tt = (nos_t - M.t_track) / 0.2f;
    M.txt = q(tf < tt ? tf : tt, 4);

    /* marquee: whole pixels per frame so the scroll is perfectly even */
    if (M.mq_w > MQ_X1 - MQ_X0) {
        if (M.mq_hold > 0) M.mq_hold -= frames;
        else {
            M.mq_off += frames;
            if (M.mq_off >= M.mq_w + MQ_GAP) { M.mq_off = 0; M.mq_hold = MQ_HOLD; }
        }
    } else { M.mq_off = 0; M.mq_hold = MQ_HOLD; }

    eq_tick(frames);

    M.ring_pos = P.dur_ms ? (int)((uint64_t)P.pos_ms * RING_N / P.dur_ms) : 0;
    if (M.ring_pos >= RING_N) M.ring_pos = RING_N - 1;

    /* the svc volume wins once the phone reports one */
    int sv = svc_music()->volume;
    if (sv >= 0 && nos_t - M.t_vol > 0.6f) M.vol = sv;
    int show = nos_t - M.t_vol < VOL_SHOW || M.dragging;
    if (show != M.vol_shown) {
        M.vol_shown = show;
        float cur = mtw_value(&M.vol_w);
        mtw_start(&M.vol_w, cur, show ? 1.0f : 0.0f, show ? 0.18f : 0.16f, show ? &EASE_STANDARD : &EASE_ACCEL);
    }
    if (P.playing != M.icon_playing) { M.icon_playing = P.playing; M.t_icon = nos_t; }
    if (M.nudge && nos_t - M.t_nudge > 0.14f) M.nudge = 0;
    if (P.playing) nos_keep_awake();
}

/* ------------------------------------------------------------- drawing */
static void draw_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d, float dcur,
                      int pos, float build, int have)
{
    float lit = (float)pos * build;
    if (!have) { dot_ring(fb, cx, cy, r, n, d, 0, 0, 0, C_LINE); return; }
    dot_ring(fb, cx, cy, r, n, d, 0, lit, C_PRI, C_LINE2);
    if (build >= 1) {
        float a = 6.2831853f * (float)pos / (float)n;
        dot_disc(fb, cx + r * gfx_fast_sin(a), cy - r * gfx_fast_cos(a), dcur, C_WHITE);
    }
}

/* Title clipped to [MQ_X0, MQ_X1) with stepped fades at the edges it scrolls under. */
static void marquee(gfx_fb *fb, const char *s, int y, uint16_t col)
{
    const font_t *f = UI_F_TITLE;
    int w = M.mq_w, win = MQ_X1 - MQ_X0;
    int top = y - f->ascent - 3, bot = y + (f->line_height - f->ascent) + 3;
    if (w <= win) { ui_text(fb, f, UI_CX, y, s, col, UI_CENTER, 0); return; }
    if (fb->rec) {
        unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_u(str_hash(s), (unsigned)M.mq_off), col), 0x3A11u);
        gfx_rec_cmd(fb, MQ_X0, top, MQ_X1, bot, h);
        return;
    }
    /* bands: [edge 14 px at 1/3][next 14 px at 2/3][middle full] on each side; the
     * left fade only while text is scrolling under it */
    const int B = 14;
    int lf = M.mq_off > 0;
    struct { int x0, x1; float k; } band[5] = {
        { MQ_X0,         MQ_X0 + B,     lf ? 0.25f : 1.0f },
        { MQ_X0 + B,     MQ_X0 + 2 * B, lf ? 0.6f : 1.0f },
        { MQ_X0 + 2 * B, MQ_X1 - 2 * B, 1.0f },
        { MQ_X1 - 2 * B, MQ_X1 - B,     0.6f },
        { MQ_X1 - B,     MQ_X1,         0.25f },
    };
    int x = MQ_X0 - M.mq_off;
    for (int i = 0; i < 5; i++) {
        gfx_fb sub;
        if (!nos_subview(fb, &sub, band[i].x0, top, band[i].x1, bot, 0, 0)) continue;
        uint16_t c = band[i].k >= 1 ? col : ui_dim(col, band[i].k);
        ui_text(&sub, f, x, y, s, c, UI_LEFT, 0);
        ui_text(&sub, f, x + w + MQ_GAP, y, s, c, UI_LEFT, 0);
    }
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
    float w = h + (full - h) * k;
    w = floorf(w * 0.5f) * 2.0f;                 /* even widths: symmetric about UI_CX */
    float y = TIME_Y - 26;
    ui_rrect(fb, UI_CX - w / 2, y, w, h, h / 2, C_SURF2);
    if (k < 0.92f) {
        dot_icon(fb, DOT_ICON_SPEAKER, UI_CX, y + h / 2, 2.2f, 1.8f, C_SEC);
        return;
    }
    float x0 = UI_CX - full / 2;
    dot_icon(fb, DOT_ICON_SPEAKER, x0 + 28, y + h / 2, 2.2f, 1.8f, C_PRI);
    dot_bar(fb, x0 + 60, y + h / 2, 10, 14.0f, 8.0f, (float)M.vol / 10.0f, C_WHITE, C_LINE2);
}

static void draw_empty(gfx_fb *fb, float build, int connected)
{
    if (!connected)
        ui_absent(fb, DOT_ICON_BT, "Phone not connected", "Pair in Gadgetbridge as Bangle.js NOS", NULL, build);
    else
        ui_absent(fb, DOT_ICON_MUSIC, "Nothing playing", "Play music on your phone", NULL, build);
}

static void music_draw(gfx_fb *fb)
{
    ui_title(fb, "MUSIC", 0);
    float b = bezier_ease(&EASE_STANDARD, M.build);
    if (!P.have) {
        dot_ring(fb, UI_CX, UI_CY, RING_R, RING_N, 4.5f, 0, 0, 0, C_LINE);
        draw_empty(fb, M.build, P.connected);
        return;
    }
    draw_ring(fb, UI_CX, UI_CY, RING_R, RING_N, 4.5f, 11.0f, M.ring_pos, b, P.have);
    if (nos_t < M.t_enter && !M.from_wg) return;          /* the iris shows black until 40% */

    /* equaliser: columns rise row by row with the build */
    uint8_t ev[EQ_COLS];
    for (int c = 0; c < EQ_COLS; c++) {
        int rows = M.eq[c] * EQ_ROWS / 255, cap = (int)(b * EQ_ROWS + 0.5f);
        if (rows < 1) rows = 1;
        ev[c] = (uint8_t)((rows < cap ? rows : cap) * 255 / EQ_ROWS);
    }
    dot_columns(fb, UI_CX - EQ_COLS * EQ_PITCH * 0.5f, EQ_Y, EQ_COLS, EQ_ROWS, EQ_PITCH, 7.0f, ev,
                P.playing ? C_PRI : C_DIS, 0);

    uint16_t c1 = ui_dim(C_WHITE, M.txt), c2 = ui_dim(C_SEC, M.txt);
    marquee(fb, P.track, TITLE_Y, c1);
    if (P.artist[0]) ui_text_fit(fb, UI_F_BODY, UI_CX, ARTIST_Y, P.artist, c2, UI_CENTER, MQ_X1 - MQ_X0);

    /* controls are there at full size; their icons build from the centre */
    float ib = q((nos_t - M.t_enter) / UI_ENTRY_S, 32);
    side_button(fb, UI_CX - SIDE_DX, SIDE_R, DOT_ICON_PREV, M.pressed == B_PREV, M.nudge < 0 ? -1 : 0, ib);
    side_button(fb, UI_CX + SIDE_DX, SIDE_R, DOT_ICON_NEXT, M.pressed == B_NEXT, M.nudge > 0 ? 1 : 0, ib);
    /* from the widget the play disc slides from the widget's place and size into its own */
    float cy = CTRL_Y, pr = PLAY_R;
    if (M.from_wg) {
        float e = bezier_ease(&EASE_STANDARD, q((nos_t - (M.t_enter - 0.168f)) / 0.30f, 32));
        cy = m_lerp(WG_CY_SHARED, CTRL_Y, e);
        pr = m_lerp(WG_R_SHARED, PLAY_R, e);
    }
    ui_circle(fb, UI_CX, cy, pr, M.pressed == B_PLAY ? C_SEC : C_WHITE);
    int icon = P.playing ? DOT_ICON_PAUSE : DOT_ICON_PLAY;
    float ip = q((nos_t - M.t_icon) / 0.15f, 16);
    float pitch = 4.4f * (pr / PLAY_R);
    /* the play triangle sits right of its cell centre; nudge it back to optical centre */
    float ox = icon == DOT_ICON_PLAY ? -pitch * 0.9f : 0;
    float ipb = M.from_wg ? 1.0f : ib;
    dot_icon_anim(fb, icon, UI_CX + ox, cy, pitch, pitch * 0.8f, C_BG, ip < ipb ? ip : ipb, DOT_REVEAL_CENTER);

    float vk = q(mtw_value(&M.vol_w), 32);
    if (vk > 0.01f) { draw_volume(fb, vk); return; }
    char e[16], r[16];
    fmt_ms(e, sizeof e, P.pos_ms, 0);
    ui_text(fb, UI_F_LABEL, UI_CX - 14, TIME_Y, e, c1, UI_RIGHT, 1);
    dot_disc(fb, UI_CX, TIME_Y - 6, 4, ui_dim(C_DIS, M.txt));
    if (P.dur_ms) {
        fmt_ms(r, sizeof r, P.dur_ms - P.pos_ms, 1);
        ui_text(fb, UI_F_LABEL, UI_CX + 14, TIME_Y, r, ui_dim(C_DIS, M.txt), UI_LEFT, 1);
    } else ui_text(fb, UI_F_LABEL, UI_CX + 14, TIME_Y, UI_NONE ":" UI_NONE, ui_dim(C_DIS, M.txt), UI_LEFT, 1);
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

static int music_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        M.pressed = P.have ? ctrl_at(in->x, in->y) : B_NONE;
        return 1;
    case IN_TAP: {
        int b = P.have ? ctrl_at(in->x, in->y) : B_NONE;
        M.pressed = B_NONE;
        if (b == B_PLAY) { player_toggle(); nos_feedback(FB_TAP); }
        else if (b == B_NEXT) { svc_music_cmd(MUSIC_NEXT); M.nudge = 1; M.t_nudge = nos_t; nos_feedback(FB_TAP); }
        else if (b == B_PREV) { svc_music_cmd(MUSIC_PREV); M.nudge = -1; M.t_nudge = nos_t; nos_feedback(FB_TAP); }
        return 1;
    }
    case IN_DRAG_START:
        M.pressed = B_NONE;
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
        if (in->x == GEST_LEFT) { svc_music_cmd(MUSIC_PREV); M.nudge = -1; M.t_nudge = nos_t; nos_feedback(FB_TAP); return 1; }
        if (in->x == GEST_RIGHT) { svc_music_cmd(MUSIC_NEXT); M.nudge = 1; M.t_nudge = nos_t; nos_feedback(FB_TAP); return 1; }
        return 0;
    default:
        return 0;
    }
}

const nos_screen scr_music = { "MUSIC", music_enter, 0, music_tick, music_draw, music_input, 0 };

/* ============================================================ widget ===== */
#define WG_CY     WG_CY_SHARED
#define WG_R      WG_R_SHARED
#define WG_RING_R 88.0f
#define WG_RING_N 36

static struct {
    float t_shown;
    float build;
    int   pos;
    float t_press;
    int   icon_playing;
    float t_icon;
} G = { .t_shown = -10, .t_press = -10, .t_icon = -10 };

static void wg_tick(float dt, int visible)
{
    /* off-glass the ring empties, so it fills dot by dot once the page settles */
    if (!visible) { G.t_shown = 1e9f; return; }
    player_poll();
    G.build = nos_t < G.t_shown ? 0 : q((nos_t - G.t_shown) / UI_ENTRY_S, 32);
    G.pos = P.dur_ms ? (int)((uint64_t)P.pos_ms * WG_RING_N / P.dur_ms) : 0;
    if (G.pos >= WG_RING_N) G.pos = WG_RING_N - 1;
    if (P.playing != G.icon_playing) { G.icon_playing = P.playing; G.t_icon = nos_t; }
}

static void wg_shown(void) { G.t_shown = nos_t; }

static void wg_draw(gfx_fb *fb)
{
    ui_title(fb, "MUSIC", 0);
    float b = bezier_ease(&EASE_STANDARD, G.build);
    if (!P.have) {
        draw_empty(fb, 1.0f, P.connected);
        return;
    }
    dot_ring(fb, UI_CX, WG_CY, WG_RING_R, WG_RING_N, 4.0f, 0, (float)G.pos * b, C_SEC, C_LINE2);
    if (G.build >= 1) {
        float a = 6.2831853f * (float)G.pos / WG_RING_N;
        dot_disc(fb, UI_CX + WG_RING_R * gfx_fast_sin(a), WG_CY - WG_RING_R * gfx_fast_cos(a), 9.0f, C_WHITE);
    }
    int pressed = nos_t - G.t_press < 0.12f;
    ui_circle(fb, UI_CX, WG_CY, WG_R, pressed ? C_SEC : C_WHITE);
    int icon = P.playing ? DOT_ICON_PAUSE : DOT_ICON_PLAY;
    float pitch = 4.8f;
    float ox = icon == DOT_ICON_PLAY ? -pitch * 0.9f : 0;
    dot_icon_anim(fb, icon, UI_CX + ox, WG_CY, pitch, pitch * 0.8f, C_BG, q((nos_t - G.t_icon) / 0.15f, 16), DOT_REVEAL_CENTER);
    int x0, x1;
    ui_row_span(340, 44, &x0, &x1);
    ui_text_fit(fb, UI_F_TITLE, UI_CX, 344, P.track, C_WHITE, UI_CENTER, x1 - x0);
    ui_row_span(380, 56, &x0, &x1);
    if (P.artist[0]) ui_text_fit(fb, UI_F_BODY, UI_CX, 380, P.artist, C_SEC, UI_CENTER, x1 - x0);
}

static void wg_tap(int x, int y)
{
    float dx = x - UI_CX, dy = y - WG_CY;
    if (P.have && dx * dx + dy * dy <= (WG_R + 16) * (WG_R + 16)) {
        G.t_press = nos_t;
        player_toggle();
        nos_feedback(FB_TAP);
        return;
    }
    s_from_widget = 1;
    nos_open(&scr_music, UI_CX, (int)WG_CY);       /* the iris grows from the play disc */
}

const nos_widget wg_music = { "MUSIC", wg_tick, wg_draw, wg_tap, wg_shown };
