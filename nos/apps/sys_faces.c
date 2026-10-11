/* sys_faces.c — face picker (long press on the face, TX_DISSOLVE).
 *
 * The faces are shown live and full size (there is no scaling), paged horizontally with
 * the same feel as the home pager: 1:1 tracking, rubber band at both ends, the release
 * velocity picks the page, a critically damped spring that never overshoots, a tick per
 * page. An "edit mode" frame sits on top: a thin rim at the edge of the glass, the face's
 * name in a pill and page dots. Tap or the button picks the face: the frame fades, the
 * picker cuts back to home (the face stays live, no rebuild). Opened from Settings, the
 * face stays dimmed while it confirms and the picker goes back the way it came (the
 * row's value shows the choice). Left-edge swipe cancels. */
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

#define PAGE_W 466.0f
#define PILL_Y 396.0f    /* the name pill's top: clear of DOT's minute digits (to y 392) */
#define PILL_H 34.0f
#define DOTS_Y 440

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
}

static void finish(void);
static void f_tick(float dt)
{
    if (F.chosen == 1 && nos_t - F.t_chosen >= 0.17f) { F.chosen = 2; finish(); }
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
    int s0 = (int)floorf(F.x / PAGE_W);
    for (int s = s0; s <= s0 + 1; s++) {
        if (s < 0 || s >= nos_face_count) continue;
        if (nos_faces[s]->tick) nos_faces[s]->tick(&F.c, dt);
    }
}

#define EDIT_DIM (18.0f / 32.0f)    /* the live face recedes while it is being edited */

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
    /* dim level: constant in edit mode (the dissolve in covers the change), back to full
     * brightness in 4 steps as the frame fades on confirm. nos_dim records nothing, so the
     * level is recorded as a full-screen command: a change repaints everything. */
    float dk = EDIT_DIM;
    if (F.chosen && F.on_home)            /* over home: it ends exactly as home shows it */
        dk = EDIT_DIM + (1.0f - EDIT_DIM) * floorf(m_clamp((nos_t - F.t_chosen) / 0.16f, 0, 1) * 4.0f) / 4.0f;
    if (fb->rec) gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H, gfx_hash_f(0xd17u, dk));

    /* edit-mode frame: a thin rim on the very edge of the glass (outside every face's own
     * tick/dot rings), the name pill and page dots; it fades in over the transition in */
    float e = m_clamp((nos_t - F.t_enter) / 0.30f, 0, 1);
    if (F.chosen) e = m_clamp(e - (nos_t - F.t_chosen) / 0.16f, 0, 1);   /* fading out */
    float fa = floorf(e * 8.0f + 0.5f) / 8.0f;
    const char *name = nos_faces[F.page]->name ? nos_faces[F.page]->name : "FACE";
    int w = ui_text_width(UI_F_LABEL, name, 2) + 44;
    if (w < 120) w = 120;
    const float pcy = PILL_Y + PILL_H * 0.5f;
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
    /* the rim carries on from the 3/8 the long press charged it to on home */
    float k = F.chosen || !F.on_home ? fa : (3.0f + floorf(e * 5.0f + 0.5f)) / 8.0f;
    if (k > 0) gfx_ring(fb, UI_CX, UI_CY, UI_R - 3.5f, 3.0f, ui_dim(C_SEC, k));
    if (fa <= 0.0f) return;
    /* the pill fades up from the face's ground: no dark flash on a white face */
    ui_rrect(fb, UI_CX - w / 2.0f, PILL_Y, (float)w, PILL_H, PILL_H * 0.5f, ui_mix(disc, C_WHITE, fa));
    ui_label(fb, UI_CX, (int)(pcy + UI_F_LABEL->ascent * 0.5f - 2), name, disc ? ui_mix(disc, C_BG, fa) : C_BG,
             UI_CENTER);
    /* page dots: light on a dark face; on a white face ink-coloured, as home draws them */
    const float pitch = 14, d = 7;
    int n = nfaces();
    float pos = F.x / PAGE_W, x0 = UI_CX - (n - 1) * pitch * 0.5f;
    uint16_t g = ui_dim(C_WHITE, dk);         /* a white face's ground under the dots */
    for (int i = 0; i < n; i++) {
        float nr = 1.0f - m_clamp(fabsf(pos - i), 0, 1);
        nr = floorf(nr * 8.0f + 0.5f) / 8.0f;
        float dx = x0 + i * pitch;
        int under = (int)floorf((dx + (float)xi) / PAGE_W);   /* the face this dot sits over */
        uint16_t col;
        if (under >= 0 && under < nos_face_count && (nos_face_style(nos_faces[under]) & FACE_LIGHT))
            col = ui_mix(g, ui_dim(g, 0.55f * (1.0f - nr)), fa);
        else col = ui_dim(ui_mix(C_LINE2, C_WHITE, nr), fa);
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
    int p = F.page;
    if (p != nos_prefs.face) {
        nos_prefs.face = (uint8_t)p;
        prefs_save();
    }
    if (F.on_home) nos_pop_with(TX_NONE);
    else nos_pop();
}

static int f_input(const nos_input *in)
{
    if (F.chosen) return 1;                     /* closing */
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
    case IN_BTN:
        if (!F.settling && !F.dragging) choose();
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_faces = { "FACES", f_enter, 0, f_tick, f_draw, f_input, SCR_HDRAG };
