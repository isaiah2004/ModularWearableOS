/* sys_home.c — the root screen: the watch face plus a horizontal pager of widgets.
 *
 *   page 0      the current face (nos_faces[nos_prefs.face]); the faces team draws it
 *   pages 1..5  wg_activity, wg_heart, wg_weather, wg_music, wg_timer
 *
 * Paging feel (the iOS pager): the content follows the finger 1:1 (the touch slop is
 * absorbed so nothing jumps when the drag locks), rubber-bands past both ends, and on
 * release the finger's velocity picks the page (a flick moves one page, a slow drag
 * goes to the nearest) and hands over to a critically damped spring launched with that
 * velocity, clamped so it can never overshoot. Touching a moving pager stops it dead.
 * A haptic tick marks every page crossing.
 *
 * The button from a widget page returns to the face as ONE page of motion: the current
 * widget is drawn in slot 1 for the trip ("alias"), so the user sees it slide away and
 * the face slide in rather than five pages whipping past.
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
#include <math.h>
#include <string.h>

void sys_launcher_prewarm(void);              /* sys_launcher.c: bake one row sprite */
const nos_screen *sys_launcher_last(void);    /* sys_launcher.c: last app opened, or 0 */

#define PAGE_W   466.0f
#define NWG      5
#define NPAGE    (NWG + 1)
#define FLICK_V  350.0f      /* px/s: faster releases always change page */
#define ENTER_S  0.40f       /* face build-in duration */
#define DOTS_Y   448

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

static const nos_widget *const k_wg[NWG] = { &wg_activity, &wg_heart, &wg_weather, &wg_music, &wg_timer };

static struct {
    float x;                 /* pager offset in slot space, px (0 = face) */
    mspring sp;
    int settling;
    int dragging, grabbed;
    float grab_x, grab_dx;   /* pager x and finger dx when the drag locked */
    int grab_page;           /* page under the finger when it went down */
    int was_moving;          /* the press stopped a moving pager: no tap */
    int alias;               /* >0: widget page drawn in slot 1 (return-to-face trip) */
    int page;                /* nearest real page (haptic detents) */
    int shown_page;          /* page whose shown() ran last */
    float dots_a;            /* page-dot opacity, quantised to 8 levels */
    float dots_raw, dots_hold;
    face_ctx c;
    float enter_t0;
    int face_idx;
    int last_aod;
    uint32_t last_tick_ms;
    int prev_awake_ms;
    int covered;             /* another screen is on top */
    int holding; float hold_t;   /* finger resting on the face: the edit rim charges */
    int prs;                 /* face complication pressed: 1 finger down, 2 kept through its opening */
    int pr[4];               /* its press rect (screen x0 y0 x1 y1) */
    float pr_t;
    int inited;
} H;

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
static float pager_max(void) { return H.alias ? PAGE_W : PAGE_W * NWG; }
/* page shown in slot s */
static int slot_page(int s) { return (H.alias && s == 1) ? H.alias : s; }
/* fractional real page position (for dots) */
static float real_pos(void)
{
    float s = floorf(H.x + 0.5f) / PAGE_W;      /* drawn position: no repaint on sub-px tails */
    return H.alias ? s * (float)H.alias : s;
}
static int nearest_page(void) { return slot_page((int)floorf(H.x / PAGE_W + 0.5f)); }
static int at_face(void) { return !H.dragging && !H.settling && H.x == 0.0f; }

/* widget pressed states: the page under a resting finger hears down / up (or cancel) */
static int s_press_pg;
static void widget_press(int page, int x, int y, int down)
{
    if (!down) { if (s_press_pg >= 1 && k_wg[s_press_pg - 1]->press) k_wg[s_press_pg - 1]->press(x, y, 0); s_press_pg = 0; return; }
    if (page >= 1 && k_wg[page - 1]->press) { k_wg[page - 1]->press(x, y, 1); s_press_pg = page; }
}

static void settle_to(float target, float v)
{
    if (fabsf(target - H.x) < 0.01f && fabsf(v) < 1.0f) { H.x = target; H.settling = 0; return; }
    msp_init(&H.sp, H.x, SPRING_PAGE);
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
    H.dragging = 0;
    H.grabbed = 0;
    float pos = H.x / PAGE_W;
    int n = H.alias ? 1 : NWG;
    int tgt;
    if (v > FLICK_V) tgt = (int)floorf(pos) + 1;
    else if (v < -FLICK_V) tgt = (int)ceilf(pos) - 1;
    else tgt = (int)floorf(pos + 0.5f);
    int from = H.grab_page;
    if (tgt > from + 1) tgt = from + 1;
    if (tgt < from - 1) tgt = from - 1;
    if (tgt < 0) tgt = 0;
    if (tgt > n) tgt = n;
    settle_to(tgt * PAGE_W, v);
}

static void go_face(void)
{
    if (H.x <= 0.0f && !H.dragging) return;
    int p = nearest_page();
    if (!H.alias && p >= 2) {               /* one page of motion, whatever the distance */
        H.alias = p;
        H.x -= (float)(p - 1) * PAGE_W;
    }
    H.dragging = H.grabbed = 0;
    settle_to(0, H.settling ? H.sp.v : 0);
}

static void go_page(int p)                 /* air gestures */
{
    if (H.alias) return;
    if (p < 0 || p > NWG) { nos_feedback(FB_LIMIT); return; }
    settle_to(p * PAGE_W, 0);
}

/* ---- lifecycle ---------------------------------------------------------------------- */
static void home_enter(void)
{
    if (!H.inited) {
        memset(&H, 0, sizeof H);
        H.inited = 1;
        H.face_idx = nos_prefs.face;
        H.shown_page = 0;
        H.last_tick_ms = hal_millis();
        sys_home_build_in();
    }
    /* time spent under another screen (the face picker, an app) is not a sleep: the wake
     * test below only measures gaps while home itself is on the glass */
    H.covered = 0;
    H.last_tick_ms = hal_millis();
    H.prev_awake_ms = nos_awake_ms();
    H.prs = 0;
}

static void home_leave(void)
{
    H.covered = 1;
    H.holding = 0;
    if (H.prs == 1) H.prs = 0;           /* a latched press stays through its opening */
    widget_press(0, 0, 0, 0);
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
    c->aod = nos_aod;
    c->enter = m_clamp((nos_t - H.enter_t0) / ENTER_S, 0, 1);
    (void)dt;
}

/* A fresh context for screens that draw faces themselves (the face picker). */
const face_ctx *sys_home_ctx(void)
{
    build_ctx(0);
    return &H.c;
}

static void home_tick(float dt)
{
    uint32_t now = hal_millis();
    int awake = nos_awake_ms();
    /* waking: the frame loop paused, AOD ended, or the screen-off timeout had passed */
    int woke = (!H.covered && now - H.last_tick_ms > 1500u) ||
               (H.prev_awake_ms > nos_prefs.sleep_s * 1000 + 500 && awake < 500);
    /* out of AOD the time was on the glass a frame ago: no build-in from nothing */
    if (H.last_aod && !nos_aod) woke = 0;
    if (nos_aod && !H.last_aod) {              /* AOD shows the face only */
        H.alias = 0; H.x = 0; H.settling = H.dragging = H.grabbed = 0;
        H.shown_page = 0; H.page = 0;
    }
    if (woke) sys_home_build_in();
    H.last_aod = nos_aod;
    H.last_tick_ms = now;
    H.prev_awake_ms = awake;
    /* a face picked in the picker was already shown live and full size: no rebuild */
    if (nos_prefs.face != H.face_idx) H.face_idx = nos_prefs.face;
    if (H.prs == 2 && nos_t - H.pr_t > 0.6f) H.prs = 0;   /* the opening has covered it */

    /* pager */
    if (H.settling) {
        int m = msp_step(&H.sp, dt);
        /* the last sub-pixel of a settle would cost a whole-page repaint for nothing */
        if (m && fabsf(H.sp.x - H.sp.target) < 0.6f && fabsf(H.sp.v) < 40.0f) m = 0;
        H.x = H.sp.x;
        if (!m) {
            H.settling = 0;
            H.x = H.sp.target;
            if (H.alias && H.x <= 0.0f) H.alias = 0;
            else if (H.alias && H.x >= PAGE_W) { H.x = H.alias * PAGE_W; H.alias = 0; }
            int p = nearest_page();
            if (p != H.shown_page) {
                H.shown_page = p;
                if (p >= 1 && k_wg[p - 1]->shown) k_wg[p - 1]->shown();
            }
        }
    }
    int p = nearest_page();
    if (p != H.page) { H.page = p; nos_feedback(FB_TICK); }

    /* page dots: on widget pages and while the pager moves. A linear ramp on the raw
     * value (quantised only when drawn): in over 0.12 s; out over 0.3 s after a 0.2 s
     * hold once the pager has settled on the face (gone within ~0.5 s). */
    int want = !nos_aod && (H.dragging || H.settling || H.x > 0.5f);
    if (want) H.dots_hold = nos_t;
    if (want) H.dots_raw += dt / 0.12f;
    else if (nos_t - H.dots_hold > 0.2f) H.dots_raw -= dt / 0.30f;
    H.dots_raw = m_clamp(H.dots_raw, 0, 1);
    H.dots_a = floorf(H.dots_raw * 8.0f + 0.5f) / 8.0f;

    /* face + widgets */
    build_ctx(dt);
    const nos_face *f = cur_face();
    if (f && f->tick) f->tick(&H.c, dt);
    float rx = real_pos() * PAGE_W;
    for (int i = 0; i < NWG; i++) {
        if (!k_wg[i]->tick) continue;
        int slot_vis = 0;
        if (nos_aod) slot_vis = 0;
        else if (H.alias) slot_vis = (i + 1 == H.alias) && H.x > 0.0f;
        else slot_vis = fabsf((i + 1) * PAGE_W - rx) < PAGE_W;
        k_wg[i]->tick(dt, slot_vis);
    }

    if (!H.dragging && !H.settling && !nos_transitioning()) sys_launcher_prewarm();
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
    int clear = xi >= UI_CX + (NPAGE - 1) * pitch * 0.5f + 16;
    if (clear || ((st & FACE_FULL_BLEED) && !light)) {
        float ca = m_clamp(H.dots_a * 2.0f, 0, 1);
        int cx0 = (int)x0 - 13;
        rr_pass(fb, cx0, DOTS_Y - 12, cx0 + (int)((NPAGE - 1) * pitch) + 26, DOTS_Y + 12, 12.0f,
                RR_FADE, 0, ca);
    }
    for (int i = 0; i < NPAGE; i++) {
        float near = 1.0f - m_clamp(fabsf(pos - i), 0, 1);
        near = floorf(near * 8.0f + 0.5f) / 8.0f;
        float dx = x0 + i * pitch;
        uint16_t col;
        if (light && !clear && dx < (float)(GFX_W - xi) && dx >= (float)-xi) {     /* this dot is over the white face */
            float ink = 204.0f * (1.0f - near);                 /* #CCCCCC .. black: C_LINE2..white inverted */
            col = grey565((int)lroundf(255.0f + (ink - 255.0f) * H.dots_a));
        } else col = gdim(ui_mix(C_LINE2, C_WHITE, near), H.dots_a);
        dot_disc(fb, dx, (float)DOTS_Y, d + near * 2.0f, col);
    }
}

static void draw_page(gfx_fb *fb, int page, int ox)
{
    gfx_fb sub;
    int x0 = ox < 0 ? 0 : ox, x1 = ox + (int)PAGE_W > GFX_W ? GFX_W : ox + (int)PAGE_W;
    if (x1 <= x0 || !nos_subview(fb, &sub, x0, 0, x1, GFX_H, ox, 0)) return;
    if (page == 0) {
        const nos_face *f = cur_face();
        if (f && f->draw) f->draw(&sub, &H.c);
    } else if (k_wg[page - 1]->draw) k_wg[page - 1]->draw(&sub);
}

static void home_draw(gfx_fb *fb)
{
    if (nos_aod) {
        const nos_face *f = cur_face();
        if (f && f->draw) f->draw(fb, &H.c);
        return;
    }
    int xi = (int)lroundf(H.x);
    int s0 = (int)floorf(H.x / PAGE_W);
    for (int s = s0; s <= s0 + 1; s++) {
        if (s < 0 || s > (H.alias ? 1 : NWG)) continue;
        int ox = s * (int)PAGE_W - xi;
        if (ox <= -(int)PAGE_W || ox >= GFX_W) continue;
        draw_page(fb, slot_page(s), ox);
    }
    int light = (nos_face_style(cur_face()) & FACE_LIGHT) != 0;
    /* a pressed complication: C_SURF2 under the face's art (#E0E0E0 under ink on white) */
    if (H.prs && xi == 0)
        rr_pass(fb, H.pr[0], H.pr[1], H.pr[2], H.pr[3], 16.0f, light ? RR_DARKEN : RR_LIGHTEN,
                light ? grey565(0xE0) : C_SURF2, 1.0f);
    draw_dots(fb);
    /* the unread dot: on page 0 the face draws its own (face_ctx.unread); widget pages get
     * this red one, fading in with the slot offset as the face slides away */
    if (H.c.unread > 0 && xi > 0) {
        float k = floorf(m_clamp(H.x / PAGE_W, 0, 1) * 8.0f + 0.5f) / 8.0f;
        if (k > 0) {
            if (xi >= UI_CX + 14) dot_disc(fb, UI_CX, 24, 20, C_BG);   /* face clear: backdrop */
            dot_disc(fb, UI_CX, 24, 9, ui_dim(C_RED, k));
        }
    }
    /* a resting finger on the face charges the edit rim (the picker continues it) */
    if (H.holding && xi == 0) {
        float k = m_clamp((nos_t - H.hold_t - 0.15f) / 0.30f, 0, 1) * 3.0f;
        k = floorf(k + 0.5f) / 8.0f;
        /* on a white face the rim darkens by the same amount instead */
        if (k > 0) gfx_ring(fb, UI_CX, UI_CY, UI_R - 3.5f, 3.0f,
                            light ? grey565((int)lroundf(255.0f - 153.0f * k)) : gdim(C_SEC, k));
    }
}

/* ---- input -------------------------------------------------------------------------- */
static int home_input(const nos_input *in)
{
    if (nos_aod) return 0;
    switch (in->type) {
    case IN_DOWN:
        H.was_moving = H.settling && fabsf(H.sp.v) > 60.0f;
        if (H.settling) { H.settling = 0; H.sp.v = 0; }   /* catch the page mid-flight */
        H.grabbed = 1;
        H.grab_x = H.x;
        H.grab_page = (int)floorf(H.x / PAGE_W + 0.5f);
        H.holding = !H.was_moving && H.x == 0.0f;
        H.hold_t = nos_t;
        /* a complication under the finger shows pressed until release (all-zero rect: a
         * target with nothing to light, e.g. a bezel ring) */
        H.prs = 0;
        if (H.holding && !H.alias) {
            int r[4] = { 0, 0, 0, 0 };
            if (nos_face_press_rect(cur_face(), in->x, in->y, r) && r[2] > r[0] && r[3] > r[1]) {
                memcpy(H.pr, r, sizeof r);
                H.prs = 1;
            }
        }
        /* a settled widget page shows its pressed state */
        if (!H.was_moving && !H.alias && H.x == (float)H.grab_page * PAGE_W)
            widget_press(H.grab_page, in->x, in->y, 1);
        return 1;
    case IN_DRAG_START:
        H.holding = 0;
        H.prs = 0;
        widget_press(0, in->x, in->y, 0);           /* paging or a sheet: cancel */
        if (in->axis == AXIS_X) {
            H.dragging = 1;
            H.grab_x = H.x;
            H.grab_dx = in->dx;
            return 1;
        }
        /* vertical: the sheets, from the face only */
        if (nearest_page() == 0 && H.x < 40.0f) {
            H.x = 0; H.settling = 0; H.grabbed = 0; H.alias = 0;
            if (in->dy > 0) nos_pull(&scr_control, TX_DROP, in);
            else nos_pull(&scr_notifs, TX_RISE, in);
            return 1;
        }
        if (H.grabbed) pager_release(0);
        return 0;
    case IN_DRAG: {
        if (!H.dragging) return 0;
        float raw = H.grab_x - (in->dx - H.grab_dx), mx = pager_max();
        if (raw < 0) raw = rubber_band(raw, PAGE_W);
        else if (raw > mx) raw = mx + rubber_band(raw - mx, PAGE_W);
        H.x = raw;
        return 1;
    }
    case IN_DRAG_END:
        if (!H.dragging) return 0;
        pager_release(-in->vx);
        return 1;
    case IN_TAP: {
        int was = H.was_moving;
        H.holding = 0;
        widget_press(0, in->x, in->y, 0);
        if (H.grabbed) pager_release(0);
        if (was || H.x != (float)((int)floorf(H.x / PAGE_W + 0.5f)) * PAGE_W) return 1;
        int p = nearest_page();
        if (p == 0) {
            const nos_face *f = cur_face();
            const nos_screen *s = f && f->tap ? f->tap(in->x, in->y) : 0;
            if (s) {
                if (H.prs) { H.prs = 2; H.pr_t = nos_t; }   /* stays lit while the iris opens */
                nos_open(s, in->x, in->y);
            }
        } else if (k_wg[p - 1]->tap) {
            k_wg[p - 1]->tap(in->x, in->y);         /* widgets open with nos_open (FB_OPEN) */
        }
        return 1;
    }
    case IN_LONG:
        H.holding = 0;
        H.prs = 0;
        if (nearest_page() == 0 && H.x < 1.0f && !H.was_moving) {
            H.grabbed = 0;
            nos_push(&scr_faces, TX_DISSOLVE);
            return 1;
        }
        return 0;
    case IN_UP:
        if (H.grabbed && !H.dragging) pager_release(0);
        H.grabbed = 0;
        H.holding = 0;
        if (H.prs == 1) H.prs = 0;
        widget_press(0, in->x, in->y, 0);
        return 1;
    case IN_BTN:                           /* dispatched on release, at once */
        H.holding = 0;
        H.prs = 0;
        if (at_face()) nos_open(&scr_launcher, GFX_W, UI_CY);
        else { nos_feedback(FB_BACK); go_face(); }
        return 1;
    case IN_BTN_DOUBLE: {                  /* e.g. pressed twice in an app: the first press
                                              came home, the second reopens the last app */
        const nos_screen *s = sys_launcher_last();
        if (s) { nos_open(s, GFX_W, UI_CY); return 1; }
        return 0;
    }
    case IN_AIR:
        if (in->x == GEST_LEFT) { go_page(nearest_page() + 1); return 1; }
        if (in->x == GEST_RIGHT) { go_page(nearest_page() - 1); return 1; }
        return 0;
    default:
        return 0;
    }
}

const nos_screen scr_home = { "HOME", home_enter, home_leave, home_tick, home_draw, home_input,
                              SCR_NOBACK | SCR_HDRAG };
