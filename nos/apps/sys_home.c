/* sys_home.c — the root screen: the watch face plus a horizontal pager of widgets.
 *
 *   page 0      the current face (nos_faces[nos_prefs.face]); the faces team draws it
 *   page 1      embedded Apps list
 *   pages 2..6  wg_activity, wg_heart, wg_weather, wg_music, wg_timer
 * Physical left-to-right order: Activity, Heart, Weather, Music, Timer, Face, Apps.
 * Logical IDs stay stable; signed slots put the face at 0 and Apps at +1.
 *
 * Paging feel (the iOS pager): the content follows the finger 1:1 (the touch slop is
 * absorbed so nothing jumps when the drag locks), rubber-bands past both ends, and on
 * release the finger's velocity picks the page (a flick moves one page, a slow drag
 * goes to the nearest) and hands over to a critically damped pixel spring launched with
 * that velocity, clamped so it can never overshoot, ending with a 1 px snap (MOTION30
 * R5). Touching a moving pager stops it dead. A haptic tick marks every page crossing.
 *
 * Programmatic page moves (the button back to the face, air gestures) are mechanical
 * cuts (MOTION30 §5, m_cut): a 1 px nudge, a lead-in, one jump frame to 4 px past the
 * page, then 2, 1, 0. No frame sits at a mid-speed position for the eye to track, and a
 * horizontal tear lasts one frame. The button from a widget page returns to the face as
 * ONE page of motion: the current widget is drawn in slot -1 for the trip ("alias"), so
 * the user sees it jump away and the face click in rather than five pages whipping past.
 *
 * Only the pages intersecting the glass are drawn, through nos_subview() with the
 * page's x offset; the recorder lands their damage where they are drawn. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "faces/face.h"
#include "faces/face_ext.h"
#include "gfx_cost.h"
#include "svc_track.h"
#include "workout.h"
#include "pblur.h"
#include "apps/sys_launcher_page.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void sys_launcher_prewarm(void);              /* sys_launcher.c: bake one row sprite */
const nos_screen *sys_launcher_last(void);    /* sys_launcher.c: last app opened, or 0 */

#define PAGE_W   466.0f
#define NWG      5
#define NPAGE    (NWG + 2)
#define LAST_PAGE (NPAGE - 1)
#define FIRST_SLOT (-NWG)
#define LAST_SLOT 1
#define FLICK_V  350.0f      /* px/s: faster releases always change page */
#define ENTER_S  0.40f       /* face build-in duration */
#define DOTS_Y   UI_DOTS_Y

/* Dim a neutral grey without the RGB565 rounding casts (keeps G = 2R, B = R). */
static uint16_t gdim(uint16_t c, float k)
{
    if (k >= 1.0f) return c;
    if (k <= 0.0f) return 0;
    int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    if (b != r || (g >> 1) != r) return ui_dim(c, k);       /* not a grey */
    int v = (int)(r * k + 0.5f);
    return (uint16_t)(v << 11 | (v * 2) << 5 | v);
}

/* neutral grey from an 8-bit level: green rounded on its own 6 bits, so light greys
 * do not turn lavender (G = 2R is a shade short up there) and 255 is C_WHITE */
static uint16_t grey565(int v)
{
    if (v >= 255) return C_WHITE;
    if (v <= 0) return 0;
    int r = (v * 31 + 127) / 255, g = (v * 63 + 127) / 255;
    return (uint16_t)(r << 11 | g << 5 | r);
}

/* A pass over what is already drawn in a rounded rect [x0,x1) x [y0,y1) (radius rad, AA
 * corners), so it can sit "under" art the screen does not own (the face):
 *   RR_FADE    darken towards black by a (the page dots' capsule fading in),
 *   RR_LIGHTEN raise every channel to at least col (a pressed fill under light-on-dark art),
 *   RR_DARKEN  lower every channel to at most col (a pressed fill under ink on a white face).
 * Raster only; records one command (box + params) so a change repaints just the box. */
enum { RR_FADE, RR_LIGHTEN, RR_DARKEN };
static uint16_t rr_px(uint16_t p, int op, int cr, int cg, int cb, int w)   /* w: 0..256 */
{
    int r = p >> 11, g = (p >> 5) & 63, b = p & 31, tr, tg, tb;
    if (op == RR_FADE) {
        int k = 256 - w;
        tr = (r * k + 128) >> 8; tb = (b * k + 128) >> 8;
        tg = (r == b && (g >> 1) == r) ? tr * 2 : (g * k + 128) >> 8;   /* greys stay neutral */
        return (uint16_t)(tr << 11 | tg << 5 | tb);
    }
    if (op == RR_LIGHTEN) { tr = r > cr ? r : cr; tg = g > cg ? g : cg; tb = b > cb ? b : cb; }
    else                  { tr = r < cr ? r : cr; tg = g < cg ? g : cg; tb = b < cb ? b : cb; }
    if (w < 256) {
        tr = r + (((tr - r) * w) >> 8); tg = g + (((tg - g) * w) >> 8); tb = b + (((tb - b) * w) >> 8);
    }
    return (uint16_t)(tr << 11 | tg << 5 | tb);
}
static void rr_pass(gfx_fb *fb, int x0, int y0, int x1, int y1, float rad, int op, uint16_t col, float a)
{
    unsigned h = gfx_hash_u(gfx_hash_u(0x5e11u, (unsigned)op), col);
    if (gfx_rec_cmd(fb, x0, y0, x1, y1, gfx_hash_f(gfx_hash_f(h, a), rad))) return;
    col = gfx_ink(fb, col);
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    int bx0 = x0 + fb->ox, bx1 = x1 + fb->ox;
    if (bx0 < c0) bx0 = c0;
    if (bx1 > c1) bx1 = c1;
    int r0 = y0 + fb->oy - fb->y_base, r1 = y1 + fb->oy - fb->y_base;
    if (r0 < 0) r0 = 0;
    if (r1 > fb->h) r1 = fb->h;
    int ia = (int)(m_clamp(a, 0, 1) * 256.0f + 0.5f);
    if (ia <= 0 || bx1 <= bx0 || r1 <= r0) { GFX_COST(GFX_COST_DIM, 0); return; }
    int cr = col >> 11, cg = (col >> 5) & 63, cb = col & 31;
    unsigned n = 0, nb = 0;
    for (int r = r0; r < r1; r++) {
        uint16_t *p = gfx_row(fb, r);
        float py = (float)(r - fb->oy + fb->y_base) + 0.5f;    /* logical y, pixel centre */
        float dy = py < y0 + rad ? y0 + rad - py : py > y1 - rad ? py - (y1 - rad) : 0.0f;
        for (int bx = bx0; bx < bx1; bx++) {
            int w = ia;
            if (dy > 0.0f) {
                float px = (float)(bx - fb->ox) + 0.5f;
                float dx = px < x0 + rad ? x0 + rad - px : px > x1 - rad ? px - (x1 - rad) : 0.0f;
                if (dx > 0.0f) {
                    float cov = m_clamp(rad + 0.5f - gfx_fast_sqrt(dx * dx + dy * dy), 0, 1);
                    if (cov <= 0.0f) continue;
                    w = (int)(cov * (float)ia + 0.5f);
                    nb++;
                }
            }
            p[bx] = rr_px(p[bx], op, cr, cg, cb, w);
            n++;
        }
    }
    GFX_COST(GFX_COST_DIM, n);
    GFX_COST_PX(GFX_COST_BLEND, nb);
}

/* ---- the system REC chip ------------------------------------------------------------
 * While a GPS track (svc_track) or a voice memo (svc_memo) records in the background, a
 * red dot + "REC 12:34" (Navigate's header words; a memo is counted the same way) on the
 * face page and in the control centre header. A tap opens the recorder. With both
 * running the memo shows: the microphone is the one to know about. */
#define REC_DOT 8
#define REC_GAP 8
int sys_rec_state(char *label, int n)
{
    if (svc_memo_active()) {
        unsigned t = (unsigned)(svc_memo_elapsed_ms() / 1000u);
        if (t >= 3600) snprintf(label, (size_t)n, "REC %u:%02u:%02u", t / 3600, t / 60 % 60, t % 60);
        else snprintf(label, (size_t)n, "REC %02u:%02u", t / 60, t % 60);
        return 2;
    }
    hal_imu_record rec; hal_imu_record_get(&rec);
    if (rec.state == HAL_IMU_RECORDING || rec.state == HAL_IMU_STARTING || rec.state == HAL_IMU_STOPPING) {
        unsigned t=rec.elapsed_ms/1000;
        snprintf(label,(size_t)n,"IMU %02u:%02u",t/60,t%60); return 3;
    }
    const workout_snapshot *w = workout_get();
    if (w->state == WORKOUT_RUNNING || w->state == WORKOUT_PAUSED) {
        unsigned t = w->elapsed_ms / 1000u;
        const char *tag = w->state == WORKOUT_PAUSED ? "PAUSE" : "SPORT";
        if (t >= 3600) snprintf(label, (size_t)n, "%s %u:%02u:%02u", tag, t/3600, t/60%60, t%60);
        else snprintf(label, (size_t)n, "%s %02u:%02u", tag, t/60, t%60);
        return 4;
    }
    if (svc_track_active()) { svc_track_label(label, n); return 1; }
    return 0;
}
const nos_screen *sys_rec_screen(int kind) { return kind == 4 ? &scr_workout : kind == 3 ? &scr_imu : kind == 2 ? &scr_memo : &scr_nav; }
int sys_rec_width(const char *label) { return REC_DOT + REC_GAP + ui_label_width(label); }
/* the chip from its left edge, label baseline y (the dot sits on the middle of the caps) */
void sys_rec_draw(gfx_fb *fb, int x0, int y, const char *label, uint16_t dot, uint16_t ink)
{
    dot_disc(fb, x0 + REC_DOT * 0.5f, y - 6.5f, REC_DOT, dot);
    ui_label(fb, x0 + REC_DOT + REC_GAP, y, label, ink, UI_LEFT);
}

/* Where each face has a free line for it (label baseline, centred), clear of its date,
 * complications and the page dots. DOT, LIFE and TAPE keep their battery status on that
 * very line: there the chip takes the line and carries the status after it. */
/* no_dot: the face's own red is the recording light (TAPE's lamp), the chip is words
 * only. rmax: the backing stays inside this radius (LIFE's day ring sits at r 212). */
typedef struct { const char *face; short y; char shares_status, no_dot; short rmax; } rec_slot;
static const rec_slot k_rec_slot[] = {
    { "DOT", 428, 1, 0, 0 }, { "MATRIX", 388, 0, 0, 0 }, { "GLYPH", 340, 0, 0, 0 },
    { "SPLIT", 404, 0, 0, 0 }, { "LIFE", 408, 1, 0, 200 }, { "SEGMENT", 362, 0, 0, 0 },
    { "INVERTED", 396, 0, 0, 0 }, { "TAPE", 406, 1, 1, 0 },
    { "ORRERY", 434, 0, 0, 0 }, /* clear the Solar face's event pill */
};
static const rec_slot *rec_slot_of(const nos_face *f)
{
    static const rec_slot other = { "", 404, 0, 0, 0 };
    for (unsigned i = 0; i < sizeof k_rec_slot / sizeof k_rec_slot[0]; i++)
        if (f && f->name && !strcmp(f->name, k_rec_slot[i].face)) return &k_rec_slot[i];
    return &other;
}

static const nos_widget *const k_wg[NWG] = { &wg_activity, &wg_heart, &wg_weather, &wg_music, &wg_timer };

static struct {
    float x;                 /* pager offset in slot space, px (0 = face) */
    nos_mb_track mb;
    float mb_step;            /* tick-owned displacement; draw/record never advances it */
    int mb_moving;
    mspring sp;
    int settling;            /* 1 spring (after a finger), 2 cut (programmatic) */
    float cut_from, cut_to; nos_time_t cut_t0;
    int dragging, grabbed;
    int apps_visible, apps_touch, apps_y;
    float grab_x, grab_dx;   /* pager x and finger dx when the drag locked */
    int grab_page;           /* physical slot under the finger when it went down */
    int was_moving;          /* the press stopped a moving pager: no tap */
    int alias;               /* >0: widget logical page drawn in slot -1 (return trip) */
    int page;                /* nearest real page (haptic detents) */
    unsigned char wvis[NWG]; /* widget page on the glass last tick (shown() on the rising edge) */
    float dots_a;            /* page-dot opacity, quantised to 8 levels */
    float dots_raw; nos_time_t dots_hold;
    face_ctx c;
    nos_time_t enter_t0;
    int face_idx;
    int last_aod;
    uint32_t last_tick_ms;
    int prev_awake_ms;
    int covered;             /* another screen is on top */
    int holding; nos_time_t hold_t;   /* finger resting on the face: the edit rim charges */
    nos_time_t rim_t;             /* the long press fired: hold the charged rim through the dissolve */
    float rec_raw;           /* REC chip opacity, linear 0..1 (drawn in 4 steps) */
    int rec_kind;            /* 1 track, 2 memo (kept while it fades out) */
    char rec_lbl[16];
    nos_time_t aod_t0;            /* AOD began (the entry ramp runs from here) */
    nos_time_t wake_t;            /* AOD ended: the chip comes up with the face's wake */
    nos_time_t rec_open_t;        /* a tap on the chip: when its app opens (after 3 lit frames) */
    int rec_ox, rec_oy;
    int aod_out;             /* the face was on the glass: it dims out before the AOD face */
    int aod_cut;             /* AOD collapsed another screen onto home: no face to dim */
    int prog;                /* the pager is on a programmatic move (button, air gesture) */
    float probe_off;         /* motion audit: H.x re-based by the alias trip, kept continuous */
    int inited;
} H;

/* AOD entry, in frames: the face dims out in 3 (only if it was on the glass), then the AOD
 * face comes up in 3 more; a 1 Hz AOD frame lands past it. The host keeps 30 Hz frames
 * (and the user's brightness) while sys_home_aod_entering() says so. */
#define AOD_OUT_N 3
#define AOD_IN_N 3
static int aod_frame(void) { return (int)floorf((nos_t - H.aod_t0) * NOS_FPS + 0.5f); }
static int aod_dimming_out(void) { return nos_aod && H.aod_out && aod_frame() < AOD_OUT_N; }
/* the page's ink level during the entry ramp (1 = as is): perceptual levels (m_ink),
 * 62 / 38 / 12 % out, 12 / 38 / 62 % in */
static float aod_ramp_k(void)
{
    if (!nos_aod) return 1.0f;
    int n = aod_frame();
    if (H.aod_out) {
        if (n < AOD_OUT_N) return m_ink((float)(AOD_OUT_N - n) / (AOD_OUT_N + 1));
        n -= AOD_OUT_N;
    }
    return n < AOD_IN_N ? m_ink((float)(n + 1) / (AOD_IN_N + 1)) : 1.0f;
}
int sys_home_aod_dimming(void) { return aod_dimming_out(); }
int sys_home_aod_entering(void)
{
    return nos_aod && aod_frame() < (H.aod_out ? AOD_OUT_N : 0) + AOD_IN_N;
}

static const nos_face *cur_face(void)
{
    int i = nos_prefs.face;
    if (i < 0 || i >= nos_face_count) i = 0;
    return nos_faces[i];
}

void sys_home_build_in(void)
{
    H.enter_t0 = nos_t;
    H.c.enter = 0;
    const nos_face *f = cur_face();
    if (f && f->enter) f->enter();
}

/* ---- pager geometry --------------------------------------------------------------- */
static int first_slot(void) { return H.alias ? -1 : FIRST_SLOT; }
static int last_slot(void) { return H.alias ? 0 : LAST_SLOT; }
static int page_slot(int p) { return p >= 2 ? p - NWG - 2 : p; }
/* page shown in slot s */
static int slot_page(int s) { return H.alias && s == -1 ? H.alias : s < 0 ? s + NWG + 2 : s; }
/* fractional real page position (for dots) */
static float real_pos(void)
{
    float s = floorf(H.x + 0.5f) / PAGE_W;      /* drawn position: no repaint on sub-px tails */
    return H.alias ? -s * (float)page_slot(H.alias) : s;
}
static int nearest_page(void)
{
    int s = (int)floorf(H.x / PAGE_W + 0.5f);
    if (s < first_slot()) s = first_slot();
    if (s > last_slot()) s = last_slot();
    return slot_page(s);
}
static int at_face(void) { return !H.dragging && !H.settling && H.x == 0.0f; }
static float q4(float x) { return floorf(m_clamp(x, 0, 1) * 4.0f + 0.5f) / 4.0f; }

/* §8 pressed state, for a face complication (its press rect) or a widget page (its
 * press hook): it lights once 60 ms have passed with the finger still (<= 3 px), then
 * stays on until release unless the finger passes 8 px (a page drag or a sheet starting:
 * dropped silently). A quick tap still shows it for 3 frames,
 * and a tap that opens something keeps it lit while the iris grows over it. */
static struct {
    int kind;                /* 0 none, 1 face rect, 2 widget page */
    int page, x, y;          /* widget: page and finger */
    int r[4];                /* face: press rect (screen x0 y0 x1 y1) */
    int down, lit, shown;    /* finger still down; widget has heard press(1); lit while down */
    nos_time_t t0, until;
} PR;
#define PR_SHOW_S 0.06f      /* a held, still press shows from the 2nd frame */
static int pr_visible(void)
{
    if (!PR.kind) return 0;
    if (!PR.down) return nos_t < PR.until;
    return PR.shown;
}
static void pr_cancel(void)
{
    if (PR.kind == 2 && PR.lit && k_wg[PR.page - 2]->press) k_wg[PR.page - 2]->press(PR.x, PR.y, 0);
    PR.kind = 0; PR.down = PR.lit = 0;
}
static void pr_release(float hold)
{
    if (!PR.kind || !PR.down) return;
    PR.down = 0;
    nos_time_t vis = PR.t0 + PR_SHOW_S;                   /* when it became visible */
    PR.until = nos_t < vis ? nos_t + 0.09f : vis + 0.10f;
    if (hold > 0 && PR.until < nos_t + hold) PR.until = nos_t + hold;
}
static void pr_tick(void)
{
    if (PR.kind && PR.down && !PR.shown && nos_t - PR.t0 >= PR_SHOW_S && nos_touch_travel() <= 3) PR.shown = 1;
    int on = pr_visible();
    if (PR.kind == 2 && k_wg[PR.page - 2]->press && on != PR.lit) {
        k_wg[PR.page - 2]->press(PR.x, PR.y, on);
        PR.lit = on;
    }
    if (PR.kind && !on && !PR.down) pr_cancel();
    if (PR.kind && PR.down && nos_touch_travel() > 8) pr_cancel();   /* a drag is starting */
}

static void settle_to(float target, float v)
{
    if (fabsf(target - H.x) < 0.01f && fabsf(v) < 1.0f) { H.x = target; H.settling = 0; return; }
    msp_init_px(&H.sp, H.x, SPRING_PAGE);      /* pixels: ends with a 1 px snap, no creep */
    /* a critically damped spring overshoots only if launched towards its target faster
     * than w0 * distance: cap the handed-over velocity there (exactly no overshoot) */
    float dist = target - H.x, w0 = 6.2831853f / H.sp.response;
    if (v * dist > 0 && fabsf(v) > w0 * fabsf(dist)) v = (dist > 0 ? 1.0f : -1.0f) * w0 * fabsf(dist);
    H.sp.v = v;
    msp_to(&H.sp, target);
    H.settling = 1;
}

static void pager_release(float v)
{
    v = -v;                 /* finger left moves toward the page on the right */
    H.dragging = 0;
    H.grabbed = 0;
    H.prog = 0;
    float pos = H.x / PAGE_W;
    int tgt;
    if (v > FLICK_V) tgt = (int)floorf(pos) + 1;
    else if (v < -FLICK_V) tgt = (int)ceilf(pos) - 1;
    else tgt = (int)floorf(pos + 0.5f);
    int from = H.grab_page;
    if (tgt > from + 1) tgt = from + 1;
    if (tgt < from - 1) tgt = from - 1;
    if (tgt < first_slot()) tgt = first_slot();
    if (tgt > last_slot()) tgt = last_slot();
    settle_to(tgt * PAGE_W, v);
}

/* A programmatic page move: a mechanical cut, started one frame in so the press moves
 * this very frame (mtw_lead's rule). */
static void cut_to(float target)
{
    if (fabsf(target - H.x) < 0.5f) { H.x = target; H.settling = 0; return; }
    H.cut_from = H.x;
    H.cut_to = target;
    H.cut_t0 = nos_t - 1.0f / NOS_FPS;
    H.sp.v = 0;
    H.settling = 2;
}

static void go_face(void)
{
    if (H.apps_touch) sys_launcher_page_cancel();
    H.apps_touch = H.apps_y = 0;
    if (H.x == 0.0f && !H.dragging && !H.settling) return;
    int p = nearest_page();
    if (!H.alias && p >= 2) {               /* one page of motion, whatever the distance */
        H.alias = p;
        float shift = (float)(page_slot(p) + 1) * PAGE_W;
        H.x -= shift;
        H.probe_off += shift;
    }
    H.dragging = H.grabbed = 0;
    cut_to(0);
    H.prog = 1;
}

static void go_page(int p)                 /* air gestures and side button */
{
    if (H.apps_touch) sys_launcher_page_cancel();
    H.apps_touch = H.apps_y = 0;
    if (H.alias) return;
    if (p < 0 || p > LAST_PAGE) { nos_feedback(FB_LIMIT); return; }
    cut_to(page_slot(p) * PAGE_W);
    H.prog = 1;
}

/* ---- lifecycle ---------------------------------------------------------------------- */
/* An app to open once home is back on top (a double press caught the launcher still
 * opening: its iris closes back into the button first, then the app irises from home). */
static const nos_screen *s_open_next;

void sys_home_open_after(const struct nos_screen *s) { s_open_next = s; }

static void home_enter(void)
{
    if (!H.inited) {
        memset(&H, 0, sizeof H);
        H.inited = 1;
        H.face_idx = nos_prefs.face;
        H.last_tick_ms = hal_millis();
        sys_home_build_in();
    }
    /* time spent under another screen (the face picker, an app) is not a sleep: the wake
     * test below only measures gaps while home itself is on the glass */
    H.covered = 0;
    /* Reveal precedes input delivery; do not defer list enter until after a new
     * DOWN, which would erase its freshly established press/drag state. */
    if (!nos_aod && !H.apps_visible && !H.alias && fabsf(H.x - PAGE_W) < PAGE_W) {
        sys_launcher_page_enter(); H.apps_visible = 1;
    }
    if (nos_aod) H.aod_cut = 1;          /* nos_home_now() for AOD: the face was not showing */
    H.last_tick_ms = hal_millis();
    H.prev_awake_ms = nos_awake_ms();
    pr_cancel();
    H.rim_t = 0;
    H.rec_open_t = 0;
}

static void home_leave(void)
{
    H.covered = 1;
    if (H.apps_visible) sys_launcher_page_leave();
    H.apps_visible = H.apps_touch = H.apps_y = 0;
    H.holding = 0;
    if (PR.down) pr_cancel();            /* a released press stays lit through its opening */
    if (H.dragging || H.grabbed) pager_release(0);
}

static void build_ctx(float dt)
{
    face_ctx *c = &H.c;
    hal_get_datetime(&c->dt);
    const svc_activity_t *a = svc_activity();
    const svc_sys_t *sy = svc_sys();
    c->steps = a->steps > 0 ? a->steps : sy->steps_today;
    c->step_goal = a->goal > 0 ? a->goal : nos_prefs.step_goal_100 * 100;
    c->bpm = svc_heart()->bpm_last;
    c->battery = sy->battery_pct > 0 ? sy->battery_pct : hal_battery_percent();
    c->charging = sy->charging;
    c->unread = svc_notif_unread();
    const svc_weather_t *w = svc_weather();
    c->weather_valid = w->valid;
    c->temp_c = w->temp_c;
    c->wx_cond = w->cond;
    int ah, am, ain;
    c->next_alarm_min = svc_alarm_next(&ah, &am, &ain) ? ain : -1;
    c->t = nos_t;
    c->aod = nos_aod && !aod_dimming_out();   /* the face dims out as itself first */
    c->enter = m_clamp((nos_t - H.enter_t0) / ENTER_S, 0, 1);
    /* recording: the faces' red accents yield to the chip (4 steps as it fades; kept
     * through AOD and the wake, so a wake never re-tints them) */
    c->rec = floorf(H.rec_raw * 4.0f + 0.5f) / 4.0f;
    (void)dt;
}

/* A fresh context for screens that draw faces themselves (the face picker). */
const face_ctx *sys_home_ctx(void)
{
    build_ctx(0);
    return &H.c;
}

static void rec_step(float dt, int snap)
{
    char lbl[16];
    int rk = sys_rec_state(lbl, sizeof lbl);
    if (rk) { H.rec_kind = rk; memcpy(H.rec_lbl, lbl, sizeof lbl); }
    if (snap) H.rec_raw = rk ? 1.0f : 0.0f;
    else H.rec_raw = m_clamp(H.rec_raw + (rk ? 1.0f : -1.0f) * dt / (4.0f / NOS_FPS), 0, 1);
}
/* the face picker over home: home doesn't tick under it, so the picker keeps the chip's
 * clock (and its fade) running (sys_faces.c) */
void sys_home_rec_tick(float dt) { rec_step(dt, 0); }

static void rec_probe(void);
static int home_blur_tick(void);
static void home_blur_check(void);
static void home_tick(float dt)
{
    if (H.rec_open_t > 0 && nos_t >= H.rec_open_t) {   /* the chip was lit 3 frames */
        H.rec_open_t = 0;
        if (nos_top() == &scr_home && !nos_aod)
            nos_open(sys_rec_screen(H.rec_kind), H.rec_kind == 4 ? UI_CX : H.rec_ox,
                     H.rec_kind == 4 ? UI_CY : H.rec_oy);
    }
    if (s_open_next && nos_top() == &scr_home && !nos_transitioning()) {
        const nos_screen *s = s_open_next;
        s_open_next = 0;
        nos_open(s, GFX_W, UI_CY);
    }
    uint32_t now = hal_millis();
    int awake = nos_awake_ms();
    /* waking: the frame loop paused, AOD ended, or the screen-off timeout had passed */
    int woke = (!H.covered && now - H.last_tick_ms > 1500u) ||
               (H.prev_awake_ms > nos_prefs.sleep_s * 1000 + 500 && awake < 500);
    /* out of AOD the time was on the glass a frame ago: no build-in from nothing */
    if (H.last_aod && !nos_aod) {
        woke = 0;
        awake = 0;              /* the waking touch resets the awake clock: not a new wake */
        H.wake_t = nos_t;
    }
    if (nos_aod && !H.last_aod) {              /* AOD shows the face only */
        if (H.apps_visible || H.apps_touch) sys_launcher_page_cancel();
        H.aod_t0 = nos_t;
        H.aod_out = !H.aod_cut && !H.covered && fabsf(H.x) < 0.5f && !H.alias;
        H.aod_cut = 0;
        H.alias = 0; H.x = 0; H.settling = H.dragging = H.grabbed = 0;
        H.page = 0;
        pr_cancel();
    }
    if (woke) sys_home_build_in();
    H.last_aod = nos_aod;
    H.last_tick_ms = now;
    H.prev_awake_ms = awake;
    /* a face picked in the picker was already shown live and full size: no rebuild */
    if (nos_prefs.face != H.face_idx) { H.face_idx = nos_prefs.face; pblur_invalidate(0); }
    pr_tick();

    /* pager */
    if (H.settling) {
        int m;
        float tgt;
        if (H.settling == 2) {                  /* the cut: whole frames since the press */
            int k = m_frame_k(H.cut_t0);
            H.x = m_cut(H.cut_from, H.cut_to, k);
            m = k < M_CUT_FRAMES;
            tgt = H.cut_to;
        } else {                                /* the pixel spring snaps its last pixel */
            m = msp_step(&H.sp, dt);
            H.x = H.sp.x;
            tgt = H.sp.target;
        }
        if (!m) {
            H.settling = 0;
            H.x = tgt;
            if (H.alias && H.x == 0.0f) H.alias = 0;
            else if (H.alias && H.x <= -PAGE_W) {
                H.probe_off -= (float)(page_slot(H.alias) + 1) * PAGE_W;
                H.x = page_slot(H.alias) * PAGE_W; H.alias = 0;
            }
            H.prog = 0;
        }
    }
    int moving = H.dragging || H.settling;
    float delta = nos_mb_track_step(&H.mb, (float)lroundf(H.x));
    H.mb_step = (moving || H.mb_moving) && fabsf(delta) < PAGE_W * 0.5f ? -delta : 0.0f;
    H.mb_moving = moving;
    int p = nearest_page();
    if (p != H.page) { H.page = p; nos_feedback(FB_TICK); }

    /* page dots: on widget pages and while the pager moves. A linear ramp in whole frames
     * on the raw value, drawn at perceptual ink levels (m_ink): in over 4 frames; out over
     * 9 after a 6-frame hold once the pager has settled on the face (gone within ~0.5 s). */
    int want = !nos_aod && (H.dragging || H.settling || fabsf(H.x) > 0.5f);
    if (want) H.dots_hold = nos_t;
    if (want) H.dots_raw += dt / M_FRAMES(4);
    else if (nos_t - H.dots_hold > M_FRAMES(6) - 0.001f) H.dots_raw -= dt / M_FRAMES(9);
    H.dots_raw = m_clamp(H.dots_raw, 0, 1);
    H.dots_a = m_ink(H.dots_raw);

    /* REC chip: in and out over 4 frames on a settled home; while home is coming back
     * (an iris closing over it) it is already as it is now, no stale chip fading out */
    rec_step(dt, nos_top() != &scr_home || nos_transitioning());

    /* Embedded Apps has a single lifecycle owner, independent of stack enters. */
    int apps_visible = !nos_aod && !H.covered && nos_top() == &scr_home &&
        (H.alias ? H.alias == 1 && H.x > 0 : fabsf(H.x - PAGE_W) < PAGE_W);
    if (apps_visible && !H.apps_visible) sys_launcher_page_enter();
    if (!apps_visible && H.apps_visible) {
        sys_launcher_page_leave(); H.apps_touch = H.apps_y = 0;
    }
    H.apps_visible = apps_visible;
    if (apps_visible) sys_launcher_page_tick(dt);

    /* face + widgets */
    build_ctx(dt);
    const nos_face *f = cur_face();
    int face_interactive = nos_face_interactive(f);
    /* Tick/input safe point: outgoing sheets and pager views cannot reference the
     * inactive list caches here. Keep live page/face blur and history resident. */
    if (face_interactive && !H.dragging && !H.settling && at_face() && !H.alias &&
        nos_top() == &scr_home && !nos_transitioning()) nos_mem_face_prepare();
    /* A standalone launcher never ticks its hidden face. Keep the same bound
     * when Apps owns the full glass; resume before its first returning pixel.
     * Widget-only idle still ticks faces for existing face-source prewarming. */
    int drawn_x = (int)lroundf(H.x);
    int face_hidden_by_apps = !nos_aod && drawn_x >= (int)PAGE_W &&
        (H.alias ? H.alias == 1 : drawn_x < 2 * (int)PAGE_W);
    if (f && f->tick && !face_hidden_by_apps) f->tick(&H.c, dt);
    for (int i = 0; i < NWG; i++) {
        int slot_vis = 0;
        if (nos_aod) slot_vis = 0;
        else if (H.alias) slot_vis = (i + 2 == H.alias) && H.x < 0.0f;
        else slot_vis = fabsf(page_slot(i + 2) * PAGE_W - H.x) < PAGE_W;
        /* shown() (the widget's entry build) runs on the first frame any of the page is on
         * the glass, so it builds as it slides in and never blanks once it has arrived */
        if (slot_vis && !H.wvis[i] && k_wg[i]->shown) k_wg[i]->shown();
        H.wvis[i] = (unsigned char)slot_vis;
        if (k_wg[i]->tick) k_wg[i]->tick(dt, slot_vis);
    }

    if (!H.apps_visible && !nos_aod && !H.dragging && !H.settling && !nos_transitioning() && nos_top() == &scr_home) {
        /* Page jobs and list prewarmers never spend budgets on the same frame. */
        if (!(nos_frame & 1u)) home_blur_tick();
        else if (!face_interactive) {
            /* Spread the independent prewarmers: don't stack three bake budgets. */
            switch (nos_frame % 3u) {
            case 0: sys_launcher_prewarm(); break;
            case 1: sys_notifs_prewarm(); break;
            default: set_settings_prewarm(); break;
            }
        }
    }

    if (H.dragging || H.settling) home_blur_check();

    /* motion audit (sim only): the pager as drawn, the dots' and chip's ink, the AOD ramp */
    if (!nos_aod) {
        nos_motion_probe("home.pager", H.dragging ? MC_FINGER : H.prog ? MC_TRANSITION : MC_MOMENTUM,
                         (float)lroundf(H.x) + H.probe_off);
        nos_motion_probe("home.dots.ink", MC_FADE, H.dots_a * 100.0f);
        nos_motion_probe("home.rec.ink", MC_FADE, m_ink(H.rec_raw) * 100.0f);
    } else nos_motion_probe("home.aod.ink", MC_FADE, aod_ramp_k() * 100.0f);
    rec_probe();
}

/* ---- draw --------------------------------------------------------------------------- */
static void draw_dots(gfx_fb *fb)
{
    if (H.dots_a <= 0.0f) return;
    const float pitch = 14.0f, d = 7.0f;
    float pos = real_pos();
    float x0 = UI_CX - (NPAGE - 1) * pitch * 0.5f;
    int xi = (int)floorf(H.x + 0.5f);
    int st = nos_face_style(cur_face());
    int light = (st & FACE_LIGHT) != 0;
    /* a black capsule keeps the dots legible over art: over widget pages once the face has
     * slid clear, and from the first drag frame over a face with art under the dot row.
     * It fades with the dots, twice as fast, so the dots never sit on bare art (and the art
     * never cuts back in). A white face gets ink-coloured dots instead, no capsule. */
    int clear = abs(xi) >= UI_CX + (NPAGE - 1) * pitch * 0.5f + 16;
    if (clear || ((st & FACE_FULL_BLEED) && !light)) {
        float ca = m_clamp(H.dots_a * 2.0f, 0, 1);
        int cx0 = (int)x0 - 13;
        rr_pass(fb, cx0, DOTS_Y - 12, cx0 + (int)((NPAGE - 1) * pitch) + 26, DOTS_Y + 12, 12.0f,
                RR_FADE, 0, ca);
    }
    for (int i = 0; i < NPAGE; i++) {
        float near = 1.0f - m_clamp(fabsf(pos - page_slot(i)), 0, 1);
        near = floorf(near * 8.0f + 0.5f) / 8.0f;
        float dx = x0 + (page_slot(i) - FIRST_SLOT) * pitch;
        uint16_t col;
        if (light && !clear && dx < (float)(GFX_W - xi) && dx >= (float)-xi) {     /* this dot is over the white face */
            float ink = 204.0f * (1.0f - near);                 /* #CCCCCC .. black: C_LINE2..white inverted */
            col = grey565((int)lroundf(255.0f + (ink - 255.0f) * H.dots_a));
        } else col = gdim(ui_mix(C_LINE2, C_WHITE, near), H.dots_a);
        dot_disc(fb, dx, (float)DOTS_Y, d + near * 2.0f, col);
    }
}

/* the face's battery status line (as faces.c fk_status: bolt + "15%"), when the chip takes it */
static int status_shown(void) { return H.c.charging || (H.c.battery > 0 && H.c.battery <= 20); }
static int status_w(char *b, int n)
{
    snprintf(b, (size_t)n, "%d%%", H.c.battery);
    return ui_text_width(UI_F_LABEL, b, 2) + (H.c.charging ? 18 : 0);
}

/* the chip line on the current face: x0, baseline, total width; 0 = not shown */
static int rec_line_f(const nos_face *f, int *x0, int *y, int *w)
{
    if (H.rec_raw <= 0.0f || !H.rec_kind || H.c.aod) return 0;   /* AOD: draw_rec_aod */
    const rec_slot *sl = rec_slot_of(f);
    char b[8];
    *w = sl->no_dot ? ui_label_width(H.rec_lbl) : sys_rec_width(H.rec_lbl);
    if (sl->shares_status && status_shown()) *w += 16 + status_w(b, sizeof b);
    *x0 = UI_CX - *w / 2;
    *y = sl->y;
    return 1;
}
static int rec_line(int *x0, int *y, int *w) { return rec_line_f(cur_face(), x0, y, w); }
/* its backing / pressed rect (screen), 30 px tall around the caps */
static void rec_rect(int x0, int y, int w, int r[4])
{
    r[0] = x0 - 12; r[1] = y - 21; r[2] = x0 + w + 12; r[3] = y + 9;
}

/* The chip's backing: the face's own ground (black, or a white face's white) inside its
 * rect + 2 px, feathered back to the face over 4 px in 8 steps (a hard edge sliced ghost
 * cells and ring dots into half-moons), at strength a; pixels past rmax (0 = none) keep
 * the face. Raster only; records one command. */
static void rec_back(gfx_fb *fb, const int rr[4], uint16_t to, float a, float rmax)
{
    if (a <= 0.0f) return;
    const float in = 2.0f, feather = 4.0f, ext = in + feather;
    float cx = (rr[0] + rr[2]) * 0.5f, cy = (rr[1] + rr[3]) * 0.5f;
    float hw = (rr[2] - rr[0]) * 0.5f, hh = (rr[3] - rr[1]) * 0.5f, rad = hh;
    int x0 = (int)floorf(cx - hw - ext), x1 = (int)ceilf(cx + hw + ext);
    int y0 = (int)floorf(cy - hh - ext), y1 = (int)ceilf(cy + hh + ext);
    unsigned h = gfx_hash_u(gfx_hash_f(gfx_hash_f(gfx_hash_f(0x2ecbu, hw), a), rmax), to);
    if (gfx_rec_cmd(fb, x0, y0, x1, y1, h)) return;
    to = gfx_ink(fb, to);
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    int bx0 = x0 + fb->ox, bx1 = x1 + fb->ox;
    if (bx0 < c0) bx0 = c0;
    if (bx1 > c1) bx1 = c1;
    int r0 = y0 + fb->oy - fb->y_base, r1 = y1 + fb->oy - fb->y_base;
    if (r0 < 0) r0 = 0;
    if (r1 > fb->h) r1 = fb->h;
    float rm2 = rmax > 0 ? rmax * rmax : 1e9f;
    unsigned n = 0;
    for (int r = r0; r < r1; r++) {
        uint16_t *p = gfx_row(fb, r);
        float py = (float)(r - fb->oy + fb->y_base) + 0.5f;
        float dy = fabsf(py - cy) - (hh - rad);
        if (dy < 0) dy = 0;
        float gy = py - UI_CY;
        for (int bx = bx0; bx < bx1; bx++) {
            uint32_t c = p[bx];
            if (c == to) continue;
            float lx = (float)(bx - fb->ox) + 0.5f;
            if ((lx - UI_CX) * (lx - UI_CX) + gy * gy > rm2) continue;
            float dx = fabsf(lx - cx) - (hw - rad);
            if (dx < 0) dx = 0;
            float d = (dx > 0 || dy > 0 ? gfx_fast_sqrt(dx * dx + dy * dy) : 0) - rad;
            float k = m_clamp((d - in) / feather, 0, 1);              /* kept face */
            k = 1.0f - (1.0f - k) * a;
            if (k >= 1.0f) continue;
            int kk = (int)(floorf(k * 8.0f + 0.5f) * 4.0f);           /* 8 steps, of 32 */
            n++;
            if (kk <= 0) { p[bx] = to; continue; }
            if (to) { p[bx] = ui_mix(to, (uint16_t)c, kk / 32.0f); continue; }
            int cr = c >> 11, cg = (c >> 5) & 63, cb = c & 31;
            int nr = (cr * kk + 16) >> 5, nb = (cb * kk + 16) >> 5;
            int ng = (cr == cb && (cg >> 1) == cr) ? nr * 2 : (cg * kk + 16) >> 5;   /* greys stay neutral */
            p[bx] = (uint16_t)(nr << 11 | ng << 5 | nb);
        }
    }
    GFX_COST(GFX_COST_BLEND, n);
}

static int s_rec_wy;     /* baseline of the AOD word as last drawn (the timer rides it) */
static int draw_rec_aod_to(gfx_fb *fb, const nos_face *f, float a, int tx, int ty, float e, float m);
static void draw_rec_a(gfx_fb *fb, float a, const nos_face *f)
{
    int x0, y, w;
    if (!rec_line_f(f, &x0, &y, &w)) return;
    int light = (nos_face_style(f) & FACE_LIGHT) != 0;
    /* fades with the face build-in, and comes up with the face out of AOD (6 frames, at
     * perceptual ink levels), so neither a wake nor a build ever pops it in at full; out
     * of AOD the grey AOD chip hands over to it (it never blinks out between) */
    int kf = m_frame_k(H.wake_t) + 1;            /* the first awake frame already moves */
    float kr = m_clamp((float)kf / 6.0f, 0, 1), kw = m_ink(kr);
    const rec_slot *sl = rec_slot_of(f);
    /* out of AOD the grey AOD "REC" is the chip's word: it moves onto the chip's place on
     * a 6-frame table (28/53/73/87/96/100 %, no frame over ~10 px) and takes its colours,
     * while the backing and the timer fade in around it */
    int handoff = kw < 1.0f && !light && !strncmp(H.rec_lbl, "REC ", 4);
    float he = mf_at(&MF_MICRO6, kf), ha = m_ink(H.rec_raw) * a;
    float k = m_ink(H.rec_raw) * q4(H.c.enter) * kw * a;
    if (k <= 0.0f) {
        if (handoff) draw_rec_aod_to(fb, f, ha, x0, y, he, kw);
        return;
    }
    int r[4];
    rec_rect(x0, y, w, r);
    /* a backing that clears the face art under it (ghost dots, hands, its own status) */
    rec_back(fb, r, light ? C_WHITE : 0, k, (float)sl->rmax);
    uint16_t dot = light ? ui_mix(C_WHITE, C_RED, k) : ui_dim(C_RED, k);
    uint16_t ink = light ? grey565((int)lroundf(255.0f * (1.0f - k))) : gdim(C_PRI, k);
    if (handoff) {                                /* the word is the moving AOD one, the timer rides it */
        int wx = draw_rec_aod_to(fb, f, ha, x0, y, he, kw);
        ui_label(fb, wx + ui_label_width("REC "), s_rec_wy, H.rec_lbl + 4, ink, UI_LEFT);
    }
    else if (sl->no_dot) ui_label(fb, x0, y, H.rec_lbl, ink, UI_LEFT);
    else sys_rec_draw(fb, x0, y, H.rec_lbl, dot, ink);
    if (sl->shares_status && status_shown()) {
        char b[8];
        int sx = x0 + (sl->no_dot ? ui_label_width(H.rec_lbl) : sys_rec_width(H.rec_lbl)) + 16;
        status_w(b, sizeof b);
        uint16_t col = light ? grey565((int)lroundf(255.0f - (255.0f - 102.0f) * k)) : gdim(C_SEC, k);
        if (H.c.charging) {
            dot_icon(fb, DOT_ICON_BOLT, (float)(sx + 7), (float)(y - 6), 1.45f, 1.7f, col);
            sx += 18;
        }
        ui_label(fb, sx, y, b, col, UI_LEFT);
    }
}

static void draw_rec(gfx_fb *fb) { draw_rec_a(fb, 1.0f, cur_face()); }
/* the face picker over home: the chip as home would show it on face `face`, at a
 * (sys_faces.c) */
void sys_home_draw_rec(gfx_fb *fb, float a, int face)
{
    if (face >= 0 && face < nos_face_count) draw_rec_a(fb, a, nos_faces[face]);
}

/* In AOD: "REC" in C_DIS after a 6 px C_SEC dot (no red, no backing; ~0.1 % lit) on the
 * face's slot, drifting a few px with the minute like the AOD faces. */
/* ...and on a wake it moves by e (0..1) onto the awake chip at (tx, ty): its dot onto
 * the chip's dot, its word onto the chip's "REC", its greys toward the chip's red and
 * white by mix */
static int draw_rec_aod_to(gfx_fb *fb, const nos_face *f, float a, int tx, int ty, float e, float mix)
{
    if (!H.rec_kind || H.rec_raw <= 0.0f || a <= 0.0f) return tx;
    const rec_slot *sl = rec_slot_of(f);
    const char *t = H.rec_kind == 4 ? (!strncmp(H.rec_lbl, "PAUSE", 5) ? "PAUSE" : "SPORT") : "REC";
    int lw = ui_label_width(t), w = sl->no_dot ? lw : 6 + 8 + lw;
    int m = H.c.dt.minute;
    float x0 = (float)(UI_CX - w / 2 + (m * 7) % 5 - 2), y = (float)(sl->y + (m * 3) % 5 - 2);
    float dx = x0 + 3.0f, lx = sl->no_dot ? x0 : x0 + 14.0f, d = 6.0f;
    if (e > 0.0f) {
        float tdx = tx + REC_DOT * 0.5f, tlx = sl->no_dot ? (float)tx : (float)(tx + REC_DOT + REC_GAP);
        dx += (tdx - dx) * e; lx += (tlx - lx) * e; y += (ty - y) * e; d += (REC_DOT - d) * e;
    }
    uint16_t dc = mix > 0 ? ui_mix(C_SEC, C_RED, mix) : C_SEC, lc = mix > 0 ? ui_mix(C_DIS, C_PRI, mix) : C_DIS;
    if (!sl->no_dot) dot_disc(fb, dx, y - 6.5f, d, a < 1.0f ? ui_dim(dc, a) : dc);
    ui_label(fb, (int)lroundf(lx), (int)lroundf(y), t, a < 1.0f ? gdim(lc, a) : lc, UI_LEFT);
    s_rec_wy = (int)lroundf(y);
    return (int)lroundf(lx);
}
static void draw_rec_aod_a(gfx_fb *fb, const nos_face *f, float a) { draw_rec_aod_to(fb, f, a, 0, 0, 0.0f, 0.0f); }

/* motion audit (sim only): the REC word's x as drawn: the AOD word, its hand-off onto the
 * awake chip (the same arithmetic as draw_rec_aod_to / draw_rec_a), then the chip's */
static void rec_probe(void)
{
    if (!H.rec_kind || H.rec_raw <= 0.0f) return;
    const nos_face *f = cur_face();
    const rec_slot *sl = rec_slot_of(f);
    const char *label = H.rec_kind == 4 ? (!strncmp(H.rec_lbl, "PAUSE", 5) ? "PAUSE" : "SPORT") : "REC";
    int lw = ui_label_width(label), w = sl->no_dot ? lw : 6 + 8 + lw, m = H.c.dt.minute;
    float x0 = (float)(UI_CX - w / 2 + (m * 7) % 5 - 2), lx = sl->no_dot ? x0 : x0 + 14.0f;
    if (!nos_aod) {
        int tx, ty, tw;
        if (!rec_line_f(f, &tx, &ty, &tw)) return;
        float tlx = sl->no_dot ? (float)tx : (float)(tx + REC_DOT + REC_GAP);
        int kf = m_frame_k(H.wake_t) + 1;
        int light = (nos_face_style(f) & FACE_LIGHT) != 0;
        int handoff = m_ink(m_clamp((float)kf / 6.0f, 0, 1)) < 1.0f && !light && !strncmp(H.rec_lbl, "REC ", 4);
        lx = handoff ? lx + (tlx - lx) * mf_at(&MF_MICRO6, kf) : tlx;
    }
    nos_motion_probe("home.rec.word", MC_MICRO, (float)lroundf(lx));
}
static void draw_rec_aod(gfx_fb *fb) { draw_rec_aod_a(fb, cur_face(), 1.0f); }

/* the finger is on the chip: its line, grown to the 88 px touch target */
static int rec_hit(int x, int y, int r[4])
{
    int x0, by, w;
    if (H.rec_raw < 0.5f || !rec_line(&x0, &by, &w)) return 0;
    rec_rect(x0, by, w, r);
    int cy = (r[1] + r[3]) / 2;
    return x >= r[0] - 8 && x < r[2] + 8 && y >= cy - 44 && y < cy + 44;
}

static void draw_page(gfx_fb *fb, int page, int ox)
{
    gfx_fb sub;
    int x0 = ox < 0 ? 0 : ox, x1 = ox + (int)PAGE_W > GFX_W ? GFX_W : ox + (int)PAGE_W;
    if (x1 <= x0 || !nos_subview(fb, &sub, x0, 0, x1, GFX_H, ox, 0)) return;
    if (page == 0) {
        const nos_face *f = cur_face();
        if (f && f->draw) f->draw(&sub, &H.c);
        draw_rec(&sub);
    } else if (page == 1) sys_launcher_page_draw(&sub);
    else if (k_wg[page - 2]->draw) k_wg[page - 2]->draw(&sub);
}

static int s_bake_page;
static face_ctx s_bake_face;
static void home_blur_source(gfx_fb *fb)
{
    if (!s_bake_page) {
        const nos_face *f = cur_face();
        if (f && f->draw) f->draw(fb, &s_bake_face);
        draw_rec(fb);
    } else if (k_wg[s_bake_page - 1]->draw) k_wg[s_bake_page - 1]->draw(fb);
}
static void home_blur_check(void)
{
    int first = (int)floorf(H.x / PAGE_W);
    for (int slot = first; slot <= first + 1; slot++) {
        if (slot < first_slot() || slot > last_slot()) continue;
        s_bake_page = slot_page(slot);
        if (s_bake_page == 1) continue;
        if (s_bake_page >= 2) --s_bake_page;
        /* A partial bake owns its captured face context until it completes. */
        if (!pblur_busy(0)) { s_bake_face = H.c; s_bake_face.enter = 1; }
        pblur_check(s_bake_page, home_blur_source);
    }
}
static int home_blur_tick(void)
{
    static int next;
    if (next == NWG + 1) {
        if (!sys_control_prewarm()) next = 0;
    } else {
        s_bake_page = next;
        if (!pblur_busy(next)) { s_bake_face = H.c; s_bake_face.enter = 1; }
        if (!pblur_tick(next, 0, home_blur_source)) next++;
    }
    return 1;
}

static void home_draw(gfx_fb *fb)
{
    if (nos_aod) {
        const nos_face *f = cur_face();
        /* the entry ramp: ink-dimmed (recorded, so each step repaints) */
        float k = aod_ramp_k();
        gfx_fb sub;
        if (k < 1.0f && fb->rec) gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H, gfx_hash_f(0xa0du, k));
        gfx_fb *t = fb;
        if (k < 1.0f && nos_subview(fb, &sub, 0, 0, GFX_W, GFX_H, 0, 0)) { nos_ink_dim(&sub, k); t = &sub; }
        if (f && f->draw) f->draw(t, &H.c);
        if (H.c.aod) draw_rec_aod(t);
        else draw_rec(t);                   /* the face dimming out as itself */
        return;
    }
    int xi = (int)lroundf(H.x);
    int s0 = (int)floorf(H.x / PAGE_W);
    float mbs = H.mb.frame == nos_frame ? nos_mblur_step(H.mb_step, H.dragging) : 0;
    for (int s = s0; s <= s0 + 1; s++) {
        if (s < first_slot() || s > last_slot()) continue;
        int ox = s * (int)PAGE_W - xi;
        if (ox <= -(int)PAGE_W || ox >= GFX_W) continue;
        int page = slot_page(s);
        if (page == 1 || !pblur_draw(fb, page ? page - 1 : 0, ox, 0, mbs)) {
            draw_page(fb, page, ox);
            if (mbs >= 8) {
                int x0 = ox < 0 ? 0 : ox, x1 = ox + GFX_W;
                if (x1 > GFX_W) x1 = GFX_W;
                nos_mblur_h_reduced(fb, x0, x1, mbs);
            }
        }
    }
    /* motion blur on the pages (not the dots): once released, or always in A/B mode 2 */
    /* A resting face can redraw at 30 Hz; that is not pager motion. Only the
     * tick-owned displacement (including the final snap) can request this pass. */
    int light = (nos_face_style(cur_face()) & FACE_LIGHT) != 0;
    /* a pressed complication: C_SURF2 under the face's art (#E0E0E0 under ink on white) */
    if (PR.kind == 1 && xi == 0 && pr_visible())
        rr_pass(fb, PR.r[0], PR.r[1], PR.r[2], PR.r[3], 16.0f, light ? RR_DARKEN : RR_LIGHTEN,
                light ? grey565(0xE0) : C_SURF2, 1.0f);
    draw_dots(fb);
    /* the unread dot: on page 0 the face draws its own (face_ctx.unread); widget pages get
     * this red one, fading in with the slot offset as the face slides away */
    if (H.c.unread > 0 && xi != 0) {
        float k = floorf(m_clamp(fabsf(H.x) / PAGE_W, 0, 1) * 8.0f + 0.5f) / 8.0f;
        if (k > 0) {
            if (abs(xi) >= UI_CX + 14) dot_disc(fb, UI_CX, 24, 20, C_BG);   /* face clear: backdrop */
            dot_disc(fb, UI_CX, 24, 8, ui_dim(C_RED, k));   /* one dot: red, 8 px */
        }
    }
    /* a resting finger on the face charges the edit rim; once the long press fires it holds
     * at its 3/8 under the picker's dissolve, so both layers carry the same rim and the
     * picker continues it from there with no gap */
    float k = 0;
    if (H.holding && xi == 0) {
        k = m_clamp((nos_t - H.hold_t - 0.15f) / 0.30f, 0, 1) * 3.0f;
        k = floorf(k + 0.5f) / 8.0f;
    } else if (H.rim_t > 0 && nos_t - H.rim_t < 0.45f && xi == 0) k = 3.0f / 8.0f;
    /* on a white face the rim darkens by the same amount instead */
    if (k > 0) gfx_ring(fb, UI_CX, UI_CY, UI_R - 3.5f, 3.0f,
                        light ? grey565((int)lroundf(255.0f - 153.0f * k)) : gdim(C_SEC, k));
}

/* ---- input -------------------------------------------------------------------------- */
/* the face picker, opened by a face's own gesture (ORRERY: a 3 s hold) */
void home_open_faces(void)
{
    H.holding = 0; H.grabbed = 0;
    pr_cancel();
    nos_push(&scr_faces_home, TX_NONE);
    H.rim_t = nos_t;
}

static int home_input(const nos_input *in)
{
    if(in->type==IN_CANCEL) {
        if(H.apps_touch)sys_launcher_page_input(in);
        else if(nearest_page()==0 && at_face() && !H.alias)nos_face_input(cur_face(),in);
        pr_cancel();H.holding=0;H.apps_touch=H.apps_y=0;
        if(H.dragging || H.grabbed)settle_to((float)H.grab_page*PAGE_W,0);
        H.dragging=H.grabbed=0;return 1;
    }
    if (nos_aod) return 0;
    /* a face that takes touch (face_ext.h) hears it first while it fills the glass */
    if (in->type != IN_BTN_LONG && in->type != IN_BTN_DOUBLE && in->type != IN_AIR &&
        nearest_page() == 0 && at_face() && !H.alias && nos_face_input(cur_face(), in)) return 1;
    if (in->type == IN_DOWN && !H.alias && !H.dragging && !H.settling && H.x == PAGE_W) {
        H.apps_touch = 1; H.apps_y = 0;
        H.grabbed = 1; H.grab_x = H.x; H.grab_page = 1; H.was_moving = 0;
        return sys_launcher_page_input(in);
    }
    if (H.apps_touch) {
        if (in->type == IN_DRAG_START && in->axis == AXIS_X) {
            sys_launcher_page_cancel(); H.apps_touch = H.apps_y = 0;
        } else if (in->type == IN_DRAG_START) {
            H.apps_y = 1; H.grabbed = 0;
            return sys_launcher_page_input(in);
        } else if (in->type == IN_DRAG || in->type == IN_DRAG_END ||
                   in->type == IN_TAP || in->type == IN_LONG || in->type == IN_UP) {
            int handled = sys_launcher_page_input(in);
            if (in->type == IN_UP) H.apps_touch = H.apps_y = H.grabbed = 0;
            return handled;
        }
    }
    switch (in->type) {
    case IN_DOWN:
        H.was_moving = H.settling == 2 || (H.settling && fabsf(H.sp.v) > 60.0f);   /* a cut is always fast */
        if (H.settling) { H.settling = 0; H.sp.v = 0; }   /* catch the page mid-flight */
        H.grabbed = 1;
        H.grab_x = H.x;
        H.grab_page = (int)floorf(H.x / PAGE_W + 0.5f);
        H.holding = !H.was_moving && H.x == 0.0f;
        H.hold_t = nos_t;
        /* pressed states (pr_*): a complication under the finger lights its press rect
         * (all-zero rect: a target with nothing to light, e.g. a bezel ring); a settled
         * widget page hears its press hook */
        pr_cancel();
        if (H.holding && !H.alias) {
            int r[4] = { 0, 0, 0, 0 };
            if (rec_hit(in->x, in->y, r) ||                  /* the REC chip comes first */
                (nos_face_press_rect(cur_face(), in->x, in->y, r) && r[2] > r[0] && r[3] > r[1])) {
                memcpy(PR.r, r, sizeof r);
                PR.kind = 1;
            }
        } else if (!H.was_moving && !H.alias && nearest_page() >= 2 && H.x == (float)H.grab_page * PAGE_W &&
                   k_wg[nearest_page() - 2]->press) {
            PR.kind = 2; PR.page = nearest_page(); PR.x = in->x; PR.y = in->y;
        }
        if (PR.kind) { PR.down = 1; PR.lit = PR.shown = 0; PR.t0 = nos_t; PR.until = 0; }
        return 1;
    case IN_DRAG_START:
        H.holding = 0;
        pr_cancel();                                /* paging or a sheet: silently, if still hidden */
        if (in->axis == AXIS_X) {
            /* A locked interactive face may consume DOWN and decline this drag. */
            H.grabbed = 1;
            H.grab_page = (int)floorf(H.x / PAGE_W + 0.5f);
            H.settling = 0; H.sp.v = 0;
            H.dragging = 1;
            H.prog = 0;
            H.grab_x = H.x;
            H.grab_dx = in->dx;
            return 1;
        }
        /* vertical: the sheets, from the face only */
        if (nearest_page() == 0 && fabsf(H.x) < 40.0f) {
            H.x = 0; H.settling = 0; H.grabbed = 0; H.alias = 0;
            if (in->dy > 0) nos_pull(&scr_control, TX_DROP, in);
            else nos_pull(&scr_notifs, TX_RISE, in);
            return 1;
        }
        if (H.grabbed) pager_release(0);
        return 0;
    case IN_DRAG: {
        if (!H.dragging) return 0;
        float raw = H.grab_x - (in->dx - H.grab_dx);
        float mn = first_slot() * PAGE_W, mx = last_slot() * PAGE_W;
        if (raw < mn) raw = mn + rubber_band(raw - mn, PAGE_W);
        else if (raw > mx) raw = mx + rubber_band(raw - mx, PAGE_W);
        H.x = raw;
        return 1;
    }
    case IN_DRAG_END:
        if (!H.dragging) return 0;
        pager_release(in->vx);
        return 1;
    case IN_TAP: {
        int was = H.was_moving;
        H.holding = 0;
        if (H.grabbed) pager_release(0);
        if (was || H.x != (float)((int)floorf(H.x / PAGE_W + 0.5f)) * PAGE_W) { pr_cancel(); return 1; }
        pr_release(0);                              /* a quick tap still shows 3 frames */
        int p = nearest_page();
        if (p == 0) {
            const nos_face *f = cur_face();
            int r[4];
            if (rec_hit(in->x, in->y, r)) {         /* lit 3 frames, then its app opens */
                memcpy(PR.r, r, sizeof r);
                PR.kind = 1; PR.down = 0; PR.until = nos_t + 0.5f;
                H.rec_open_t = nos_t + 3.0f / NOS_FPS - 0.001f;
                H.rec_ox = in->x; H.rec_oy = in->y;
                return 1;
            }
            const nos_screen *s = f && f->tap ? f->tap(in->x, in->y) : 0;
            if (s) nos_open(s, in->x, in->y);
        } else if (p >= 2 && k_wg[p - 2]->tap) {
            k_wg[p - 2]->tap(in->x, in->y);         /* widgets open with nos_open (FB_OPEN) */
        }
        /* it opened something: stay lit while the iris grows over it */
        if (PR.kind && nos_top() != &scr_home && PR.until < nos_t + 0.4f) PR.until = nos_t + 0.4f;
        return 1;
    }
    case IN_LONG:
        H.holding = 0;
        pr_cancel();
        if (nearest_page() == 0 && fabsf(H.x) < 1.0f && !H.was_moving) {
            H.grabbed = 0;
            nos_push(&scr_faces_home, TX_NONE);     /* in place: it builds its frame in */
            H.rim_t = nos_t;                        /* after leave(): the rim holds */
            return 1;
        }
        return 0;
    case IN_UP:
        if (H.grabbed && !H.dragging) pager_release(0);
        H.grabbed = 0;
        H.holding = 0;
        pr_release(0);
        return 1;
    case IN_BTN:                           /* dispatched on release, at once */
        H.holding = 0;
        pr_cancel();
        if (at_face()) { nos_feedback(FB_OPEN); go_page(1); }
        else { nos_feedback(FB_BACK); go_face(); }
        return 1;
    case IN_BTN_DOUBLE: {                  /* e.g. pressed twice in an app: the first press
                                              came home, the second reopens the last app */
        const nos_screen *s = sys_launcher_last();
        if (s) { nos_open(s, GFX_W, UI_CY); return 1; }
        return 0;
    }
    case IN_AIR:
        if (in->x == GEST_LEFT || in->x == GEST_RIGHT) {
            int slot = page_slot(nearest_page()) + (in->x == GEST_LEFT ? -1 : 1);
            if (slot < FIRST_SLOT || slot > LAST_SLOT) nos_feedback(FB_LIMIT);
            else go_page(slot_page(slot));
            return 1;
        }
        return 0;
    default:
        return 0;
    }
}

const nos_screen scr_home = { "HOME", home_enter, home_leave, home_tick, home_draw, home_input,
                              SCR_NOBACK | SCR_HDRAG | SCR_NATIVE_CACHE };

void sys_home_nav_snapshot(int *page, float *offset, int *dragging)
{
    if (page) *page = nearest_page();
    if (offset) *offset = H.x;
    if (dragging) *dragging = H.dragging || H.apps_y;
}
