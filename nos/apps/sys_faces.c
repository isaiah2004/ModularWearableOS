/* sys_faces.c — face picker (long press on the face: opens in place, TX_NONE).
 *
 * The faces are shown live and full size (there is no scaling), paged horizontally with
 * the same feel as the home pager: 1:1 tracking, rubber band at both ends, the release
 * velocity picks the page, a critically damped spring that never overshoots, a tick per
 * page. An "edit mode" frame sits on top: a thin rim at the edge of the glass, the face's
 * name in a pill and page dots. A tap picks the face: the frame fades, the picker cuts
 * back to home (the face stays live, no rebuild). The button cancels: the pager springs
 * back to the face it opened on, then the frame fades the same way, nothing saved.
 * From home it opens in place: its first frame is home's (the face, the 3/8 rim, the REC
 * chip), then the face recedes and the frame builds in over it, one screen's draw a
 * frame. Opened from Settings, the face stays dimmed while it confirms and the picker
 * goes back the way it came (the row's value shows the choice); there the edge swipe
 * also goes back. */
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

const face_ctx *sys_home_ctx(void);
void sys_home_draw_rec(gfx_fb *fb, float a, int face);   /* sys_home.c: the REC chip on a face */
void sys_home_rec_tick(float dt);                         /* sys_home.c: its clock, under the picker */

#define PAGE_W 466.0f
#define PILL_Y 396.0f    /* the name pill's top: clear of DOT's minute digits (to y 392) */
#define PILL_H 34.0f
#define DOTS_Y 440
#define OPEN_S   0.25f              /* in place from home: face recedes, frame builds in */
#define CLOSE_S  0.18f              /* on choose: frame out (and the face back up), then pop */
#define SET_CLOSE_S 0.12f           /* from Settings: the frame out, then slide back at once */
#define CANCEL_S 0.30f              /* the button: back to the face it opened on, frame out with it */

static struct {
    float x;
    mspring sp;
    int settling, dragging, grabbed, was_moving;
    float grab_x, grab_dx;
    int grab_page, page;
    float t_enter;
    int chosen;
    float t_chosen;
    int on_home;             /* pushed over home (long press): it is the face underneath */
    int orig;                /* the face it opened on (home's) */
    int cancel;              /* the button: back to orig, nothing saved */
    mtween cancel_tw;        /* the pager's way back (a tween: no spring creep) */
    face_ctx c;
} F;

static int nfaces(void) { return nos_face_count > 0 ? nos_face_count : 1; }
static float maxx(void) { return (nfaces() - 1) * PAGE_W; }

static void settle_to(float target, float v)
{
    msp_init(&F.sp, F.x, SPRING_PAGE);
    float dist = target - F.x, w0 = 6.2831853f / F.sp.response;
    if (v * dist > 0 && fabsf(v) > w0 * fabsf(dist)) v = (dist > 0 ? 1.0f : -1.0f) * w0 * fabsf(dist);
    if (fabsf(dist) < 0.01f && fabsf(v) < 1.0f) { F.x = target; F.settling = 0; return; }
    F.sp.v = v;
    msp_to(&F.sp, target);
    F.settling = 1;
}

static void release(float v)
{
    F.dragging = F.grabbed = 0;
    float pos = F.x / PAGE_W;
    int t = v > 350.0f ? (int)floorf(pos) + 1 : v < -350.0f ? (int)ceilf(pos) - 1 : (int)floorf(pos + 0.5f);
    if (t > F.grab_page + 1) t = F.grab_page + 1;
    if (t < F.grab_page - 1) t = F.grab_page - 1;
    if (t < 0) t = 0;
    if (t > nfaces() - 1) t = nfaces() - 1;
    settle_to(t * PAGE_W, v);
}

static void f_enter(void)
{
    memset(&F, 0, sizeof F);
    int cur = nos_prefs.face < nos_face_count ? nos_prefs.face : 0;
    F.x = cur * PAGE_W;
    F.page = cur;
    F.t_enter = nos_enter_t();
    F.chosen = 0;
    F.on_home = nos_depth() == 2;
    F.orig = cur;
}

static void finish(void);
static float close_s(void) { return F.cancel ? CANCEL_S : F.on_home ? CLOSE_S : SET_CLOSE_S; }
/* how far into the edit dim the face is, linear in time, 8 steps (0 = home's face, 1 =
 * edit mode); from Settings it is dimmed throughout */
static float dim_lin(void)
{
    if (!F.on_home) return 1.0f;
    float lin = m_clamp((nos_t - F.t_enter) / OPEN_S, 0, 1);
    if (F.chosen) {
        float c = 1.0f - m_clamp((nos_t - F.t_chosen) / close_s(), 0, 1);
        if (c < lin) lin = c;
    }
    return floorf(lin * 8.0f + 0.5f) / 8.0f;
}
static void f_tick(float dt)
{
    if (F.on_home) sys_home_rec_tick(dt);
    if (F.cancel) F.x = mtw_value(&F.cancel_tw);           /* back over CANCEL_S, fading as it goes */
    /* pop on the frame the fade (8 steps) reaches 0: no idle frame before it */
    if (F.chosen == 1 && nos_t - F.t_chosen >= close_s() * (15.0f / 16.0f) - 0.001f) { F.chosen = 2; finish(); }
    if (F.settling) {
        int m = msp_step(&F.sp, dt);
        /* the last sub-pixel of a settle would cost a whole-page repaint for nothing */
        if (m && fabsf(F.sp.x - F.sp.target) < 0.6f && fabsf(F.sp.v) < 40.0f) m = 0;
        F.x = F.sp.x;
        if (!m) { F.settling = 0; F.x = F.sp.target; }
    }
    int p = (int)floorf(F.x / PAGE_W + 0.5f);
    if (p < 0) p = 0;
    if (p > nfaces() - 1) p = nfaces() - 1;
    if (p != F.page) { F.page = p; nos_feedback(FB_TICK); }
    const face_ctx *hc = sys_home_ctx();
    if (hc) F.c = *hc;
    F.c.enter = 1.0f;
    F.c.aod = 0;
    /* lite draw: the faces drop their near-invisible ghost/unlit detail as the face dims
     * into edit mode (and back with it), and whenever two faces are on the glass (a
     * slide draws both) */
    float pos = F.x / PAGE_W;
    int sliding = fabsf(pos - floorf(pos + 0.5f)) * PAGE_W > 0.5f;
    if (F.cancel)                         /* back with the fade: the ghosts return in steps */
        F.c.lite = floorf(fmaxf(dim_lin(), 1.0f - mtw_progress(&F.cancel_tw)) * 8.0f + 0.5f) / 8.0f;
    else F.c.lite = sliding ? 1.0f : dim_lin();
    int s0 = (int)floorf(F.x / PAGE_W);
    for (int s = s0; s <= s0 + 1; s++) {
        if (s < 0 || s >= nos_face_count) continue;
        if (nos_faces[s]->tick) nos_faces[s]->tick(&F.c, dt);
    }
}

#define EDIT_DIM (18.0f / 32.0f)    /* the live face recedes while it is being edited */

/* a neutral grey dimmed without RGB565 rounding casts, as home dims its rim */
static uint16_t gdim(uint16_t c, float k)
{
    if (k >= 1.0f) return c;
    if (k <= 0.0f) return 0;
    int r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    if (b != r || (g >> 1) != r) return ui_dim(c, k);
    int v = (int)(r * k + 0.5f);
    return (uint16_t)(v << 11 | (v * 2) << 5 | v);
}
static uint16_t grey8(int v)
{
    if (v >= 255) return C_WHITE;
    if (v <= 0) return 0;
    int r = (v * 31 + 127) / 255, g = (v * 63 + 127) / 255;
    return (uint16_t)(r << 11 | g << 5 | r);
}

/* A feathered moat around the name pill, applied over the face already drawn: the face's
 * own ground (`to`: black, or a white face's dimmed disc) within `in` px of the pill's
 * rounded rect, fading back to the face over `feather` px, at strength `a`. A hard keyline
 * sliced the face's dots into crescents; this lets them sink away (and a white face keeps
 * no dark halo). Raster only; records one command so it repaints with the pill. */
static void moat(gfx_fb *fb, float cx, float cy, float hw, float hh, float rad, float in, float feather,
                 uint16_t to, float a)
{
    if (a <= 0.0f) return;
    float ext = in + feather;
    int x0 = (int)floorf(cx - hw - ext), x1 = (int)ceilf(cx + hw + ext);
    int y0 = (int)floorf(cy - hh - ext), y1 = (int)ceilf(cy + hh + ext);
    unsigned h = gfx_hash_u(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(0x30a7u, hw), in), feather), a), to);
    if (gfx_rec_cmd(fb, x0, y0, x1, y1, h)) return;
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    int bx0 = x0 + fb->ox, bx1 = x1 + fb->ox;
    if (bx0 < c0) bx0 = c0;
    if (bx1 > c1) bx1 = c1;
    int r0 = y0 + fb->oy - fb->y_base, r1 = y1 + fb->oy - fb->y_base;
    if (r0 < 0) r0 = 0;
    if (r1 > fb->h) r1 = fb->h;
    unsigned n = 0;
    for (int r = r0; r < r1; r++) {
        uint16_t *p = gfx_row(fb, r);
        float py = (float)(r - fb->oy + fb->y_base) + 0.5f;
        float dy = fabsf(py - cy) - (hh - rad);
        if (dy < 0) dy = 0;
        for (int bx = bx0; bx < bx1; bx++) {
            uint32_t c = p[bx];
            if (c == to) continue;
            float dx = fabsf((float)(bx - fb->ox) + 0.5f - cx) - (hw - rad);
            if (dx < 0) dx = 0;
            float d = (dx > 0 || dy > 0 ? gfx_fast_sqrt(dx * dx + dy * dy) : 0) - rad;   /* outside the pill */
            float k = m_clamp((d - in) / feather, 0, 1);                              /* kept face */
            k = 1.0f - (1.0f - k) * a;
            if (k >= 1.0f) continue;
            int kk = (int)(floorf(k * 8.0f + 0.5f) * 4.0f);                          /* 8 steps, of 32 */
            n++;
            if (kk <= 0) { p[bx] = to; continue; }
            if (to) { p[bx] = ui_mix(to, (uint16_t)c, kk / 32.0f); continue; }
            int rr = c >> 11, g = (c >> 5) & 63, b = c & 31;
            int nr = (rr * kk + 16) >> 5, nb = (b * kk + 16) >> 5;
            int ng = (rr == b && (g >> 1) == rr) ? nr * 2 : (g * kk + 16) >> 5;   /* greys stay neutral */
            p[bx] = (uint16_t)(nr << 11 | ng << 5 | nb);
        }
    }
    GFX_COST(GFX_COST_BLEND, n);
}

static void f_draw(gfx_fb *fb)
{
    /* b: the edit frame's build, 0 = home exactly as it was, 1 = edit mode.
     * From home: in over OPEN_S (EASE_DECEL) from the frame home last showed, out over
     * CLOSE_S on choose, so the pop lands on an identical home. From Settings (pushed
     * with a slide) the face is dimmed from the start and stays so; the frame fades. */
    /* lin: the same build linear in time; the face's dim follows it, so no one frame
     * carries a large share of the light change (a white face is most of the glass) */
    float b, lin = 1.0f;
    if (F.on_home) {
        lin = m_clamp((nos_t - F.t_enter) / OPEN_S, 0, 1);
        b = bezier_ease(&EASE_DECEL, lin);
        if (F.chosen) {
            float c = 1.0f - m_clamp((nos_t - F.t_chosen) / close_s(), 0, 1);
            if (c < lin) lin = c;
            if (c < b) b = c;
        }
    } else {
        b = m_clamp((nos_t - F.t_enter) / 0.30f, 0, 1);
        if (F.chosen) b = m_clamp(b - (nos_t - F.t_chosen) / close_s(), 0, 1);
    }
    float fa = floorf(b * 8.0f + 0.5f) / 8.0f;
    /* the face recedes with the build (ink dim: no pixel pass). nos_dim records nothing,
     * so the level is recorded as a full-screen command: a change repaints everything. */
    float dk = 1.0f - (1.0f - EDIT_DIM) * dim_lin();
    if (fb->rec) gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H, gfx_hash_f(0xd17u, dk));
    const char *name = nos_faces[F.page]->name ? nos_faces[F.page]->name : "FACE";
    int w = ui_text_width(UI_F_LABEL, name, 2) + 44;
    if (w < 120) w = 120;
    /* the pill rises 12 px into place as it fades up */
    const float py = PILL_Y + floorf((1.0f - b) * 12.0f + 0.5f), pcy = py + PILL_H * 0.5f;
    uint16_t disc = 0;                    /* a white face's ground, as dimmed (0: a dark face) */

    int xi = (int)lroundf(F.x);
    int s0 = (int)floorf(F.x / PAGE_W);
    for (int s = s0; s <= s0 + 1; s++) {
        if (s < 0 || s >= nos_face_count) continue;
        int ox = s * (int)PAGE_W - xi;
        if (ox <= -(int)PAGE_W || ox >= GFX_W) continue;
        gfx_fb sub;
        int x0 = ox < 0 ? 0 : ox, x1 = ox + (int)PAGE_W > GFX_W ? GFX_W : ox + (int)PAGE_W;
        if (x1 > x0 && nos_subview(fb, &sub, x0, 0, x1, GFX_H, ox, 0) && nos_faces[s]->draw) {
            if (dk < 1.0f) nos_ink_dim(&sub, dk);     /* drawn dimmed: no pixel pass */
            nos_faces[s]->draw(&sub, &F.c);
            /* home's REC chip on the face it opened on: it gives way as the frame builds */
            /* home's REC chip: it gives way as the frame builds, and comes back with the
             * frame-out on the face home will show (a new one on choose) */
            int rf = F.chosen && !F.cancel ? F.page : F.orig;
            if (F.on_home && s == rf && fa < 1.0f) sys_home_draw_rec(&sub, 1.0f - fa, rf);
            /* the face sinks away around the pill, into its own ground: 6 px, so DOT's
             * last row of minute dots keeps its shape */
            int light = (nos_face_style(nos_faces[s]) & FACE_LIGHT) != 0;
            uint16_t to = light ? gfx_ink(&sub, C_WHITE) : 0;
            if (light && s == F.page) disc = to;
            moat(&sub, UI_CX - ox, pcy, w / 2.0f, PILL_H * 0.5f, PILL_H * 0.5f, 2, 4, to, fa);
            /* art under the page dots: a soft capsule (a white face takes ink dots instead) */
            if ((nos_face_style(nos_faces[s]) & FACE_FULL_BLEED) && !light)
                moat(&sub, UI_CX - ox, (float)DOTS_Y, (nfaces() - 1) * 7.0f + 6.0f, 6, 6, 2, 8, 0, fa);
        }
    }
    /* the edit-mode frame: a thin rim on the very edge of the glass (outside every face's
     * own tick/dot rings), the name pill and page dots. The rim carries on from the 3/8 the
     * long press charged it to on home (drawn as home draws it: on a white face it darkens
     * the ground), and goes out with the frame on choose. */
    /* from home it carries on from home's 3/8 at home's rate, an eighth per 0.1 s */
    float k = F.chosen || !F.on_home ? fa : fminf(1.0f, (3.0f + floorf((nos_t - F.t_enter) / 0.1f)) / 8.0f);
    if (k > 0) {
        int lightp = (nos_face_style(nos_faces[F.page]) & FACE_LIGHT) != 0;
        gfx_ring(fb, UI_CX, UI_CY, UI_R - 3.5f, 3.0f,
                 lightp ? grey8((int)lroundf(255.0f * dk * (1.0f - 0.6f * k))) : gdim(C_SEC, k));
    }
    if (fa <= 0.0f) return;
    /* the pill fades up from the face's ground: no dark flash on a white face */
    ui_rrect(fb, UI_CX - w / 2.0f, py, (float)w, PILL_H, PILL_H * 0.5f, ui_mix(disc, C_WHITE, fa));
    ui_label(fb, UI_CX, (int)(pcy + UI_F_LABEL->ascent * 0.5f - 2), name,
             disc ? ui_mix(disc, C_BG, fa) : C_BG, UI_CENTER);
    /* page dots: light on a dark face; on a white face ink-coloured, as home draws them */
    const float pitch = 14, d = 7;
    int n = nfaces();
    float pos = F.x / PAGE_W, x0 = UI_CX - (n - 1) * pitch * 0.5f;
    uint16_t g = ui_dim(C_WHITE, dk);         /* a white face's ground under the dots */
    for (int i = 0; i < n; i++) {
        float nr = 1.0f - m_clamp(fabsf(pos - i), 0, 1);
        nr = floorf(nr * 8.0f + 0.5f) / 8.0f;
        float dx = x0 + i * pitch;
        /* the dots step in outward from the current face's, 4 levels each */
        float di = fabsf((float)i - pos) / (float)(n > 1 ? n - 1 : 1);
        float fd = floorf(m_clamp((b - 0.35f * di) / 0.65f, 0, 1) * 4.0f + 0.5f) / 4.0f;
        if (fd <= 0.0f) continue;
        int under = (int)floorf((dx + (float)xi) / PAGE_W);   /* the face this dot sits over */
        uint16_t col;
        if (under >= 0 && under < nos_face_count && (nos_face_style(nos_faces[under]) & FACE_LIGHT))
            col = ui_mix(g, ui_dim(g, 0.55f * (1.0f - nr)), fd);
        else col = ui_dim(ui_mix(C_LINE2, C_WHITE, nr), fd);
        dot_disc(fb, dx, (float)DOTS_Y, d + nr * 2.0f, col);
    }
}

static void choose(void)
{
    if (F.chosen) return;
    F.chosen = 1;
    F.t_chosen = nos_t;
    nos_feedback(FB_SUCCESS);
}

/* Over home the frame has faded and the face is back at full brightness: home shows
 * exactly this face, so the pop is an invisible cut (a dissolve would composite two full
 * faces for nothing: ~31 ms on the heaviest face). The face is already live: no build-in.
 * Over anything else (Settings) the face stayed dimmed: back the way it came. */
static void finish(void)
{
    int p = F.cancel ? F.orig : F.page;
    if (p != nos_prefs.face) {
        nos_prefs.face = (uint8_t)p;
        prefs_save();
    }
    if (F.on_home) nos_pop_with(TX_NONE);
    else nos_pop();
}

static int f_input(const nos_input *in)
{
    if (F.chosen || F.cancel) return 1;       /* closing, or cancelling back */
    switch (in->type) {
    case IN_DOWN:
        F.was_moving = F.settling && fabsf(F.sp.v) > 60.0f;
        if (F.settling) { F.settling = 0; F.sp.v = 0; }
        F.grabbed = 1;
        F.grab_page = (int)floorf(F.x / PAGE_W + 0.5f);
        return 1;
    case IN_DRAG_START:
        if (in->axis != AXIS_X) { if (F.grabbed) release(0); return 0; }
        F.dragging = 1;
        F.grab_x = F.x;
        F.grab_dx = in->dx;
        return 1;
    case IN_DRAG: {
        if (!F.dragging) return 0;
        float raw = F.grab_x - (in->dx - F.grab_dx), mx = maxx();
        if (raw < 0) raw = rubber_band(raw, PAGE_W);
        else if (raw > mx) raw = mx + rubber_band(raw - mx, PAGE_W);
        F.x = raw;
        return 1;
    }
    case IN_DRAG_END:
        if (!F.dragging) return 0;
        release(-in->vx);
        return 1;
    case IN_TAP:
        if (F.grabbed) release(0);
        if (!F.was_moving && !F.settling) choose();
        return 1;
    case IN_UP:
        if (F.grabbed && !F.dragging) release(0);
        F.grabbed = 0;
        return 1;
    case IN_BTN:                                /* cancel: back to the face it opened on */
        if (F.cancel) return 1;
        F.cancel = 1;
        nos_feedback(FB_BACK);
        if (F.grabbed) F.grabbed = 0;
        F.dragging = 0;
        F.settling = 0;
        mtw_start(&F.cancel_tw, F.x, F.orig * PAGE_W, CANCEL_S, &EASE_STANDARD);
        F.chosen = 1;                               /* the frame fades with the way back */
        F.t_chosen = nos_t;
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_faces = { "FACES", f_enter, 0, f_tick, f_draw, f_input, SCR_HDRAG };
/* The same picker opened in place over home (TX_NONE): no swipe-back, which would slide
 * the face off an identical face (the engine kept that off for the old dissolve too).
 * The button or a tap chooses. (sys_home.c; asked for in nos_apps.h) */
const nos_screen scr_faces_home = { "FACES", f_enter, 0, f_tick, f_draw, f_input, SCR_HDRAG | SCR_NOBACK };
