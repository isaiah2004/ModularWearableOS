/* sys_faces.c — face picker (long press on the face, TX_DISSOLVE).
 *
 * The faces are shown live and full size (there is no scaling), paged horizontally with
 * the same feel as the home pager: 1:1 tracking, rubber band at both ends, the release
 * velocity picks the page, a critically damped spring that never overshoots, a tick per
 * page. An "edit mode" frame sits on top: a thin rim at the edge of the glass, the face's
 * name in a pill and page dots. Tap or the button picks the face: the frame fades, the
 * picker cuts back to home and a newly chosen face builds in. Left-edge swipe cancels. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "faces/face.h"
#include <math.h>
#include <string.h>

void sys_home_build_in(void);
const face_ctx *sys_home_ctx(void);

#define PAGE_W 466.0f
#define PILL_Y 384.0f
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
    F.t_enter = nos_t;
    F.chosen = 0;
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

static void f_draw(gfx_fb *fb)
{
    int xi = (int)lroundf(F.x);
    int s0 = (int)floorf(F.x / PAGE_W);
    for (int s = s0; s <= s0 + 1; s++) {
        if (s < 0 || s >= nos_face_count) continue;
        int ox = s * (int)PAGE_W - xi;
        if (ox <= -(int)PAGE_W || ox >= GFX_W) continue;
        gfx_fb sub;
        int x0 = ox < 0 ? 0 : ox, x1 = ox + (int)PAGE_W > GFX_W ? GFX_W : ox + (int)PAGE_W;
        if (x1 > x0 && nos_subview(fb, &sub, x0, 0, x1, GFX_H, ox, 0) && nos_faces[s]->draw)
            nos_faces[s]->draw(&sub, &F.c);
    }
    /* edit-mode frame: a thin rim on the very edge of the glass (outside every face's own
     * tick/dot rings), the name pill and page dots; it fades in over the dissolve */
    float e = m_clamp((nos_t - F.t_enter) / 0.30f, 0, 1);
    if (F.chosen) e = m_clamp(e - (nos_t - F.t_chosen) / 0.16f, 0, 1);   /* fading out */
    float k = floorf(e * 8.0f + 0.5f) / 8.0f;
    if (k > 0) gfx_ring(fb, UI_CX, UI_CY, UI_R - 3.5f, 3.0f, ui_dim(C_SEC, k));
    if (e >= 0.5f || (F.chosen && k > 0)) {
        const char *name = nos_faces[F.page]->name ? nos_faces[F.page]->name : "FACE";
        int w = ui_text_width(UI_F_LABEL, name, 2) + 44;
        if (w < 120) w = 120;
        ui_rrect(fb, UI_CX - w / 2.0f, PILL_Y, (float)w, 40, 20, ui_dim(C_WHITE, k));
        ui_label(fb, UI_CX, (int)(PILL_Y + 20 + UI_F_LABEL->ascent * 0.5f - 2), name, C_BG, UI_CENTER);
        /* page dots */
        const float pitch = 14, d = 7;
        int n = nfaces();
        float pos = F.x / PAGE_W, x0 = UI_CX - (n - 1) * pitch * 0.5f;
        for (int i = 0; i < n; i++) {
            float nr = 1.0f - m_clamp(fabsf(pos - i), 0, 1);
            nr = floorf(nr * 8.0f + 0.5f) / 8.0f;
            dot_disc(fb, x0 + i * pitch, (float)DOTS_Y, d + nr * 2.0f, ui_dim(ui_mix(C_LINE2, C_WHITE, nr), k));
        }
    }
}

static void choose(void)
{
    if (F.chosen) return;
    F.chosen = 1;
    F.t_chosen = nos_t;
    nos_feedback(FB_SUCCESS);
}

/* The frame has faded: home shows exactly this face, so the pop is a cut (a dissolve
 * would composite two full faces for nothing: ~31 ms on the heaviest face). A new face
 * then builds in on home as the confirmation. */
static void finish(void)
{
    int p = F.page;
    if (p != nos_prefs.face) {
        nos_prefs.face = (uint8_t)p;
        prefs_save();
        sys_home_build_in();
    }
    nos_pop_with(TX_NONE);
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
