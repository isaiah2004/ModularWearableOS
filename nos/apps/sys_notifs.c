/* sys_notifs.c — notification list (pulled up from the face, TX_RISE) and detail.
 *
 * List: newest first, one flat card per notification (dot app icon, app name, time ago,
 * title, two lines of body). Momentum scroll; a downward drag at the top belongs to the
 * engine and closes the sheet with the finger. Swipe a card left to dismiss: it follows
 * the finger, flies out with the release velocity, and the cards below close the gap on
 * a critically damped spring. "CLEAR ALL" ends the list. Empty: a calm dot bell inside a
 * ring of dots, "No notifications", [ ALL CAUGHT UP ]. Everything is marked read on entry.
 *
 * Detail (TX_PUSH): full wrapped title and body, scrolling, with OPEN ON PHONE / DISMISS.
 * The detail tracks its notification by id, so new arrivals shifting indexes are fine. */
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

#define CARD_W   344.0f
#define CARD_X   (UI_CX - CARD_W / 2)
#define CARD_R   30.0f
#define GAP      12.0f
#define TOP_Y    100.0f
#define CLEAR_H  64.0f

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

static void ago(char *b, int n, uint32_t t, uint64_t now)
{
    if (!t || !now || now < t) { b[0] = 0; return; }
    uint32_t s = (uint32_t)(now - t);
    if (s < 60) snprintf(b, (size_t)n, "NOW");
    else if (s < 3600) snprintf(b, (size_t)n, "%uM AGO", (unsigned)(s / 60));
    else if (s < 86400) snprintf(b, (size_t)n, "%uH AGO", (unsigned)(s / 3600));
    else snprintf(b, (size_t)n, "%uD AGO", (unsigned)(s / 86400));
}

static int body_lh(void) { return UI_F_BODY->line_height + 2; }
static float card_h(void) { return 52.0f + UI_F_BODY->ascent + 2 * body_lh() + 16.0f; }
static float pitch(void) { return card_h() + GAP; }

/* ===================================================================== list */
static struct {
    mscroll sc;
    float grab_dy;
    int n;
    int pressed, was_moving, dragging;
    int sw, sw_live;          /* card being swiped, finger on it */
    float sw_x, sw_dx0;
    mspring out;              /* swiped card x while flying out / returning */
    int out_i;
    mspring gap;              /* rows below a dismissed card: extra y offset */
    int gap_i;
    float t_enter, t_empty;
    uint64_t now;
    int phone;
    int clear_pressed;
} N;

static float content_h(void) { return TOP_Y + N.n * pitch() + (N.n ? CLEAR_H + 40 : 0) + 84.0f; }
static void update_bounds(void)
{
    float mx = content_h() - GFX_H;
    msc_bounds(&N.sc, 0, mx > 0 ? mx : 0);
}

static int s_from_detail;      /* returning from the detail: keep scroll, no build-in */

static void n_enter(void)
{
    if (s_from_detail) {
        s_from_detail = 0;
        N.n = svc_notif_count();
        update_bounds();
        N.pressed = -1; N.sw = -1; N.sw_live = 0; N.dragging = 0; N.clear_pressed = 0;
        return;
    }
    N.n = svc_notif_count();
    msc_init(&N.sc, 0, 0, 0);
    update_bounds();
    N.pressed = -1; N.sw = -1; N.out_i = -1; N.gap_i = -1;
    N.sw_live = 0; N.dragging = 0; N.clear_pressed = 0;
    msp_init(&N.out, 0, SPRING_SNAPPY);
    msp_init(&N.gap, 0, SPRING_PAGE);
    N.t_enter = N.t_empty = nos_t;
    svc_notif_mark_all_read();
}

static void n_tick(float dt)
{
    int n = svc_notif_count();
    if (n != N.n) {
        if (n == 0 && N.n) N.t_empty = nos_t;
        if (n > N.n && N.out_i < 0) N.gap_i = -1;      /* arrivals: no stale gap */
        N.n = n;
        update_bounds();
        if (N.pressed >= n) N.pressed = -1;
    }
    msc_step(&N.sc, dt);
    if (N.out_i >= 0) {
        msp_step(&N.out, dt);
        if (N.out.target < 0 && N.out.x <= -CARD_W - CARD_X + 2) {   /* off the glass: drop it */
            int i = N.out_i;
            N.out_i = -1; N.sw = -1;
            svc_notif_dismiss(i);
            N.n = svc_notif_count();
            msp_init(&N.gap, pitch(), SPRING_PAGE);
            msp_to(&N.gap, 0);
            N.gap_i = i;
            update_bounds();
            if (!N.n) N.t_empty = nos_t;
        } else if (N.out.target == 0 && !N.out.moving) { N.out_i = -1; N.sw = -1; }
    }
    if (N.gap_i >= 0 && !msp_step(&N.gap, dt)) N.gap_i = -1;
    N.now = hal_epoch_seconds();
    N.phone = svc_sys()->phone_connected || hal_phone_connected();
}

static float card_y(int i, float off)
{
    float y = TOP_Y + i * pitch() - off;
    if (N.gap_i >= 0 && i >= N.gap_i) y += N.gap.x;
    /* build-in: cards rise 24 px into place, 2 frames apart */
    float e = (nos_t - N.t_enter - i * (2.0f / NOS_FPS)) / 0.26f;
    e = bezier_ease(&EASE_DECEL, m_clamp(e, 0, 1));
    return y + (1 - e) * 24.0f;
}

static float card_fade(int i, float y)
{
    float e = m_clamp((nos_t - N.t_enter - i * (2.0f / NOS_FPS)) / 0.20f, 0, 1);
    float yc = y + card_h() * 0.5f, d = fabsf(yc - UI_CY);
    float f = (1.0f - 0.65f * m_smoothstep(120.0f, 240.0f, d)) * e;
    return floorf(f * 16.0f + 0.5f) / 16.0f;
}

static void draw_card(gfx_fb *fb, int i, float x, float y, float fade, int pressed)
{
    const svc_notif *n = svc_notif_get(i);
    if (!n || fade <= 0) return;
    float h = card_h();
    ui_rrect(fb, x, y, CARD_W, h, CARD_R, gdim(pressed ? C_SURF2 : C_SURF, fade));
    int ix = (int)x + 36, hy = (int)y + 38;
    dot_icon(fb, n->icon ? n->icon : DOT_ICON_BELL, (float)ix, (float)(hy - 6), 2.3f, 1.85f, gdim(C_WHITE, fade));
    ui_label(fb, ix + 22, hy, n->app[0] ? n->app : "Phone", gdim(C_SEC, fade), UI_LEFT);
    char a[16];
    ago(a, sizeof a, n->t_epoch, N.now);
    if (a[0]) ui_label(fb, (int)(x + CARD_W) - 28, hy, a, gdim(C_DIS, fade), UI_RIGHT);
    int tx = (int)x + 26, tw = (int)CARD_W - 52;
    int tb = (int)y + 52 + UI_F_BODY->ascent;
    ui_text_fit(fb, UI_F_BODY, tx, tb, n->title[0] ? n->title : n->app, gdim(C_PRI, fade), UI_LEFT, tw);
    if (n->body[0])
        ui_text_wrap(fb, UI_F_BODY, tx, tb + body_lh(), tw, body_lh(), 2, n->body, gdim(C_SEC, fade), UI_LEFT);
}

static void draw_empty(gfx_fb *fb)
{
    float p = m_clamp((nos_t - N.t_empty) / 0.45f, 0, 1);
    float q = floorf(p * 32.0f + 0.5f) / 32.0f;
    dot_ring(fb, UI_CX, 178, 74, 28, 5.0f, 0, 28.0f * q, C_LINE2, 0);
    dot_icon_anim(fb, DOT_ICON_BELL, UI_CX, 178, 6.0f, 4.8f, C_PRI, p, DOT_REVEAL_CENTER);
    ui_text(fb, UI_F_BODY, UI_CX, 296, "No notifications", C_PRI, UI_CENTER, 0);
    ui_state(fb, UI_CX, 336, N.phone ? "ALL CAUGHT UP" : "PHONE NOT CONNECTED", C_SEC);
}

static void n_draw(gfx_fb *fb)
{
    if (N.n == 0 && N.out_i < 0) { draw_empty(fb); return; }
    float off = floorf(N.sc.x + 0.5f);
    char t[32];
    snprintf(t, sizeof t, N.n == 1 ? "%d NOTIFICATION" : "%d NOTIFICATIONS", N.n);
    ui_title(fb, t, off);
    float h = card_h();
    for (int i = 0; i < N.n; i++) {
        float y = floorf(card_y(i, off) + 0.5f);
        if (y + h < 0) continue;
        if (y > GFX_H) break;
        float x = CARD_X;
        float fade = card_fade(i, y);
        if (i == N.sw || i == N.out_i) {
            float sx = N.out_i == i ? N.out.x : N.sw_x;
            x += floorf(sx + 0.5f);
            if (sx < 0) fade *= 1.0f - 0.6f * m_clamp(-sx / CARD_W, 0, 1);
            fade = floorf(fade * 16.0f + 0.5f) / 16.0f;
        }
        draw_card(fb, i, x, y, fade, i == N.pressed);
    }
    /* CLEAR ALL closes the list */
    float cy = TOP_Y + N.n * pitch() + 20 + CLEAR_H * 0.5f - off;
    if (N.gap_i >= 0) cy += N.gap.x;
    if (cy > -CLEAR_H && cy < GFX_H + CLEAR_H) {
        float e = m_clamp((nos_t - N.t_enter - N.n * (2.0f / NOS_FPS)) / 0.2f, 0, 1);
        if (e >= 1) ui_button(fb, UI_CX, floorf(cy + 0.5f), 200, CLEAR_H, "CLEAR ALL", BTN_SECONDARY, N.clear_pressed);
    }
}

static int card_at(int x, int y)
{
    float off = floorf(N.sc.x + 0.5f);
    for (int i = 0; i < N.n; i++) {
        float cy = card_y(i, off);
        if (y >= cy && y < cy + card_h() && x >= CARD_X && x < CARD_X + CARD_W) return i;
    }
    return -1;
}

static int clear_at(int x, int y)
{
    if (!N.n) return 0;
    float cy = TOP_Y + N.n * pitch() + 20 + CLEAR_H * 0.5f - floorf(N.sc.x + 0.5f);
    return fabsf(y - cy) < CLEAR_H * 0.5f + 8 && fabsf((float)(x - UI_CX)) < 110;
}

static int n_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        N.was_moving = N.sc.moving && fabsf(N.sc.v) > 60.0f;
        msc_grab(&N.sc);
        N.pressed = N.was_moving ? -1 : card_at(in->x, in->y);
        N.clear_pressed = !N.was_moving && clear_at(in->x, in->y);
        return 1;
    case IN_DRAG_START:
        N.clear_pressed = 0;
        if (in->axis == AXIS_X) {
            int i = N.pressed >= 0 ? N.pressed : card_at(in->x0, in->y0);
            N.pressed = -1;
            msc_release(&N.sc, 0);
            if (i < 0 || N.out_i >= 0) return 0;
            N.sw = i; N.sw_live = 1; N.sw_x = 0; N.sw_dx0 = in->dx;
            return 1;
        }
        N.pressed = -1;
        if (in->dy > 0 && N.sc.x <= 0.5f) { msc_release(&N.sc, 0); return 0; }   /* engine closes */
        N.dragging = 1;
        N.grab_dy = in->dy;
        return 1;
    case IN_DRAG:
        if (N.sw_live) {
            float d = in->dx - N.sw_dx0;
            N.sw_x = d < 0 ? d : rubber_band(d, 160.0f);
            return 1;
        }
        if (!N.dragging) return 0;
        msc_drag(&N.sc, in->dy - N.grab_dy);
        return 1;
    case IN_DRAG_END:
        if (N.sw_live) {
            N.sw_live = 0;
            msp_init(&N.out, N.sw_x, 0.30f, 1.0f);
            N.out.v = in->vx;
            N.out_i = N.sw;
            if (N.sw_x < -110.0f || in->vx < -600.0f) {
                msp_to(&N.out, -CARD_W - CARD_X - 20);
                if (N.out.v > -900.0f) N.out.v = -900.0f;   /* always leaves briskly */
                nos_feedback(FB_TAP);
            } else msp_to(&N.out, 0);
            return 1;
        }
        if (!N.dragging) return 0;
        N.dragging = 0;
        msc_release(&N.sc, -in->vy);
        return 1;
    case IN_TAP: {
        int was = N.was_moving;
        msc_release(&N.sc, 0);
        int i = card_at(in->x, in->y), clr = clear_at(in->x, in->y);
        N.pressed = -1; N.clear_pressed = 0;
        if (was) return 1;
        if (clr) { nos_feedback(FB_SUCCESS); svc_notif_clear(); N.n = 0; N.t_empty = nos_t; update_bounds(); msc_jump(&N.sc, 0); }
        else if (i >= 0) { nos_feedback(FB_TAP); sys_notif_open(i); }
        return 1;
    }
    case IN_UP:
        if (!N.dragging) msc_release(&N.sc, 0);
        N.pressed = -1; N.clear_pressed = 0;
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_notifs = { "NOTIFICATIONS", n_enter, 0, n_tick, n_draw, n_input, SCR_HDRAG };

/* ===================================================================== detail */
static struct {
    int id;                   /* notification id being shown */
    int idx;                  /* its current index, -1 gone */
    mscroll sc;
    float grab_dy;
    int dragging, was_moving, pressed;   /* pressed: 1 open, 2 dismiss */
    float t_enter;
    int title_lines, body_lines;
    uint64_t now;
    int phone;
} D;
static svc_notif *s_dn;       /* PSRAM copy: still drawable while the detail pops away */

#define D_TOP 104.0f

static int title_lh(void) { return UI_F_TITLE->line_height; }
static float d_body_y(void) { return D_TOP + UI_F_TITLE->ascent + (D.title_lines - 1) * title_lh() + 22 + UI_F_BODY->ascent; }
static float d_btn_y(void)
{
    float y = d_body_y() + (D.body_lines - 1) * body_lh() + 64;
    return y < 300.0f ? 300.0f : y;               /* short messages: actions in the thumb zone */
}
static float d_content(void) { return d_btn_y() + 84 + 34 + 48; }

/* count wrapped lines without drawing: same breaking as ui_text_wrap */
static int count_lines(const font_t *f, const char *s, int max_w)
{
    int lines = 0;
    const char *p = s;
    char buf[128];
    while (*p && lines < 12) {
        while (*p == ' ') p++;
        if (!*p) break;
        int best = 0, n = 0;
        while (p[n] && p[n] != '\n' && n < 120) {
            n++;
            if (p[n] == ' ' || p[n] == 0 || p[n] == '\n') {
                memcpy(buf, p, (size_t)n); buf[n] = 0;
                if (ui_text_width(f, buf, 0) <= max_w) best = n; else break;
            }
        }
        if (!best) {
            best = n;
            while (best > 1) { memcpy(buf, p, (size_t)best); buf[best] = 0; if (ui_text_width(f, buf, 0) <= max_w) break; best--; }
        }
        lines++;
        p += best;
        if (*p == '\n') p++;
    }
    return lines ? lines : 1;
}

#define D_W 330

static void d_measure(void)
{
    const svc_notif *n = D.idx >= 0 ? svc_notif_get(D.idx) : 0;
    D.title_lines = n ? count_lines(UI_F_TITLE, n->title[0] ? n->title : n->app, D_W) : 1;
    if (D.title_lines > 3) D.title_lines = 3;
    D.body_lines = n && n->body[0] ? count_lines(UI_F_BODY, n->body, D_W) : 0;
    if (D.body_lines > 12) D.body_lines = 12;
    if (D.body_lines == 0) D.body_lines = 1;
    float content = d_content();
    msc_bounds(&D.sc, 0, content - GFX_H > 0 ? content - GFX_H : 0);
}

void sys_notif_open(int i)
{
    const svc_notif *n = svc_notif_get(i);
    if (!n) return;
    D.id = n->id;
    D.idx = i;
    if (!s_dn) s_dn = (svc_notif *)hal_big_alloc(sizeof *s_dn);
    if (s_dn) *s_dn = *n;
    s_from_detail = 1;
    nos_push(&scr_notif_detail, TX_PUSH);
}

static void d_enter(void)
{
    msc_init(&D.sc, 0, 0, 0);
    D.dragging = 0; D.pressed = 0;
    D.t_enter = nos_t;
    d_measure();
}

static void d_tick(float dt)
{
    /* follow the notification by id */
    const svc_notif *n = D.idx >= 0 ? svc_notif_get(D.idx) : 0;
    if (!n || n->id != D.id) {
        int c = svc_notif_count(), f = -1;
        for (int i = 0; i < c; i++) { const svc_notif *m = svc_notif_get(i); if (m && m->id == D.id) { f = i; break; } }
        if (f != D.idx) { D.idx = f; d_measure(); }
    }
    if (D.idx >= 0 && s_dn && (n = svc_notif_get(D.idx)) != 0) *s_dn = *n;
    msc_step(&D.sc, dt);
    D.now = hal_epoch_seconds();
    D.phone = svc_sys()->phone_connected || hal_phone_connected();
}

static void d_draw(gfx_fb *fb)
{
    const svc_notif *n = s_dn;
    if (!n) return;
    float off = floorf(D.sc.x + 0.5f);
    float e = bezier_ease(&EASE_DECEL, m_clamp((nos_t - D.t_enter) / 0.3f, 0, 1));
    float lift = floorf((1 - e) * 16.0f + 0.5f);
    /* header: icon + app + time, as the card had it */
    int hy = (int)(64 - off);
    char a[16];
    ago(a, sizeof a, n->t_epoch, D.now);
    char head[48];
    snprintf(head, sizeof head, "%s%s%s", n->app[0] ? n->app : "Phone", a[0] ? "  " : "", a);
    int lw = ui_text_width(UI_F_LABEL, head, 2);
    int x0 = UI_CX - (lw + 30) / 2;
    dot_icon(fb, n->icon ? n->icon : DOT_ICON_BELL, (float)(x0 + 10), (float)(hy - 6), 2.2f, 1.75f, C_WHITE);
    ui_label(fb, x0 + 30, hy, head, C_SEC, UI_LEFT);
    int tx = UI_CX - D_W / 2;
    float ty = D_TOP + UI_F_TITLE->ascent - off + lift;
    ui_text_wrap(fb, UI_F_TITLE, tx, (int)ty, D_W, title_lh(), 3, n->title[0] ? n->title : n->app, C_PRI, UI_LEFT);
    float by = d_body_y() - off + lift * 1.5f;
    if (n->body[0]) ui_text_wrap(fb, UI_F_BODY, tx, (int)by, D_W, body_lh(), 12, n->body, C_SEC, UI_LEFT);
    float b1 = d_btn_y() - off + lift * 2;
    ui_button(fb, UI_CX, b1, 260, 68, "OPEN ON PHONE", BTN_SECONDARY, D.pressed == 1);
    ui_button(fb, UI_CX, b1 + 84, 260, 68, "DISMISS", BTN_GHOST, D.pressed == 2);
    float content = d_content();
    ui_scrollbar(fb, D.sc.x, content, GFX_H, D.sc.moving || D.dragging ? 1.0f : 0.0f);
}

static int d_btn_at(int x, int y)
{
    float b1 = d_btn_y() - floorf(D.sc.x + 0.5f);
    if (fabsf((float)(x - UI_CX)) > 140) return 0;
    if (fabsf(y - b1) < 40) return 1;
    if (fabsf(y - (b1 + 84)) < 40) return 2;
    return 0;
}

static int d_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        D.was_moving = D.sc.moving && fabsf(D.sc.v) > 60.0f;
        msc_grab(&D.sc);
        D.pressed = D.was_moving ? 0 : d_btn_at(in->x, in->y);
        return 1;
    case IN_DRAG_START:
        D.pressed = 0;
        if (in->axis != AXIS_Y) { msc_release(&D.sc, 0); return 0; }
        D.dragging = 1;
        D.grab_dy = in->dy;
        return 1;
    case IN_DRAG:
        if (!D.dragging) return 0;
        msc_drag(&D.sc, in->dy - D.grab_dy);
        return 1;
    case IN_DRAG_END:
        if (!D.dragging) return 0;
        D.dragging = 0;
        msc_release(&D.sc, -in->vy);
        return 1;
    case IN_TAP: {
        int b = d_btn_at(in->x, in->y), was = D.was_moving;
        msc_release(&D.sc, 0);
        D.pressed = 0;
        if (was || D.idx < 0) return 1;
        if (b == 1) {
            if (D.phone) { nos_feedback(FB_SUCCESS); svc_notif_open_on_phone(D.idx); nos_toast("Opened on phone"); }
            else { nos_feedback(FB_ERROR); nos_toast("Phone not connected"); }
        } else if (b == 2) {
            nos_feedback(FB_TAP);
            svc_notif_dismiss(D.idx);
            D.idx = -1;
            nos_pop();
        }
        return 1;
    }
    case IN_UP:
        if (!D.dragging) msc_release(&D.sc, 0);
        D.pressed = 0;
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_notif_detail = { "NOTIFICATION", d_enter, 0, d_tick, d_draw, d_input, 0 };
