/* sys_notifs.c — notification list (pulled up from the face, TX_RISE) and detail.
 *
 * List: newest first, one flat card per notification (dot app icon, app name, time ago,
 * title, two lines of body). Momentum scroll; a downward drag at the top belongs to the
 * engine and closes the sheet with the finger. Swipe a card left to dismiss: it follows
 * the finger, flies out with the release velocity, and the cards below close the gap on
 * a critically damped spring. "CLEAR ALL" ends the list; the cards (and the button) fly
 * out top first. Empty: the standard ui_absent ("No notifications", [ ALL CAUGHT UP ], or
 * "Phone not connected" with how to pair). Everything is marked read on entry.
 *
 * Detail (TX_PUSH): full wrapped title and body, scrolling, with the OPEN / DISMISS pair
 * in the thumb zone (or after the body when it scrolls). The detail tracks its
 * notification by id, so new arrivals shifting indexes are fine. */
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
#define CARD_R   40.0f           /* section 8 cards: r 40, C_SURF2 */
#define GAP      12.0f
#define TOP_Y    100.0f
#define CLEAR_H  ((float)UI_BTN_H)

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

/* Section 8 pressed state: hidden for the first 100 ms (a scroll or swipe passes the slop
 * first and cancels it silently), then on until release; a quick tap still shows it for
 * 3 frames. */
typedef struct { int id, down; float t0, until; } press_t;
static void press_down(press_t *p, int id) { p->id = id; p->down = id >= 0; p->t0 = nos_t; p->until = 0; }
static void press_cancel(press_t *p) { p->id = -1; p->down = 0; p->until = 0; }
static void press_up(press_t *p)
{
    if (!p->down) return;
    p->down = 0;
    float vis = p->t0 + 0.09f;                   /* when it became visible */
    p->until = nos_t < vis ? nos_t + 0.09f : vis + 0.10f;
}
static int press_on(const press_t *p, int id)
{
    if (id < 0 || p->id != id) return 0;
    return p->down ? nos_t - p->t0 >= 0.09f : nos_t < p->until;
}
#define ID_CLEAR 1000

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
    return lines;
}

#define TEXT_W ((int)CARD_W - 52)
/* card height for n body lines (0-2): ~22 px above the first ink, ~20 below the last */
static float card_h_lines(int lines) { return 52.0f + UI_F_BODY->ascent + lines * body_lh() + 26.0f; }
static float card_h_of(const svc_notif *n)
{
    int l = n && n->body[0] ? count_lines(UI_F_BODY, n->body, TEXT_W) : 0;
    return card_h_lines(l > 2 ? 2 : l);
}

/* ===================================================================== list */
static struct {
    mscroll sc;
    float grab_dy;
    int n;
    press_t pr;               /* card index, or ID_CLEAR */
    int was_moving, dragging;
    int sw, sw_live, sw_armed; /* card being swiped, finger on it, past the threshold */
    float sw_x, sw_dx0;
    mspring out;              /* swiped card x while flying out / returning */
    int out_i;                /* index flying (still in the list), -1 */
    int fly;                  /* a dismissed card still crossing the glass (drawn from s_fly) */
    float fly_y;              /* its content y */
    mspring gap;              /* rows from gap_i down: extra y offset closing to 0 */
    int gap_i;
    float t_enter, t_empty;
    int clearing; float t_clear; int clear_n;   /* CLEAR ALL: cards leave top first */
    int clear_k;              /* the button's place in that stagger, -1 off the glass */
    uint64_t now;
    int phone;
    unsigned sig;             /* ids signature the heights were computed for */
    int cnt_from, cnt_to; float cnt_t;   /* header count morph */
} N;
/* card heights and content y (prefix sums) live in PSRAM: internal DRAM is full */
static struct { float h[SVC_NOTIF_MAX]; float y[SVC_NOTIF_MAX + 1]; } *NL;
static svc_notif *s_fly;      /* PSRAM copy of the card flying out after its dismissal */

static float card_hi(int i) { return NL ? NL->h[i] : card_h_lines(2); }
static float card_yi(int i) { return NL ? NL->y[i] : i * (card_h_lines(2) + GAP); }

static void layout_cards(int force)
{
    if (!NL && !(NL = hal_big_alloc(sizeof *NL))) return;
    unsigned sig = (unsigned)N.n * 2654435761u;
    for (int i = 0; i < N.n; i++) { const svc_notif *n = svc_notif_get(i); if (n) sig = sig * 31u + (unsigned)n->id; }
    if (!force && sig == N.sig) return;
    N.sig = sig;
    float y = 0;
    int n = N.n < SVC_NOTIF_MAX ? N.n : SVC_NOTIF_MAX;
    for (int i = 0; i < n; i++) {
        NL->y[i] = y;
        NL->h[i] = card_h_of(svc_notif_get(i));
        y += NL->h[i] + GAP;
    }
    NL->y[n] = y;
}
static float list_h(void) { int n = N.n < SVC_NOTIF_MAX ? N.n : SVC_NOTIF_MAX; return card_yi(n); }
static float content_h(void) { return TOP_Y + list_h() + (N.n ? CLEAR_H + 40 : 0) + 84.0f; }
static void update_bounds(void)
{
    layout_cards(0);
    float mx = content_h() - GFX_H;
    msc_bounds(&N.sc, 0, mx > 0 ? mx : 0);
}

static void gap_start(int i, float h)
{
    msp_init(&N.gap, h, SPRING_PAGE);
    msp_to(&N.gap, 0);
    N.gap_i = i;
}

/* the swiped card is half off the glass: it leaves the data now (drawn from a copy) so
 * the rows below start closing the gap while it is still flying */
static void dismiss_flying(void)
{
    int i = N.out_i;
    const svc_notif *n = svc_notif_get(i);
    if (!s_fly) s_fly = (svc_notif *)hal_big_alloc(sizeof *s_fly);
    if (n && s_fly) { *s_fly = *n; N.fly = 1; N.fly_y = card_yi(i); }
    float h = card_hi(i) + GAP;
    N.out_i = -1; N.sw = -1;
    svc_notif_dismiss(i);
    N.n = svc_notif_count();
    update_bounds();
    gap_start(i, h);
    if (!N.n) N.t_empty = nos_t + 0.15f;
}

static void n_enter(void)
{
    if (nos_revealed()) {                  /* back from the detail: keep everything */
        N.n = svc_notif_count();
        update_bounds();
        press_cancel(&N.pr); N.sw = -1; N.sw_live = 0; N.dragging = 0;
        svc_notif_mark_all_read();
        return;
    }
    N.n = svc_notif_count();
    msc_init(&N.sc, 0, 0, 0);
    layout_cards(1);
    update_bounds();
    press_cancel(&N.pr); N.sw = -1; N.out_i = -1; N.gap_i = -1; N.fly = 0; N.clearing = 0;
    N.sw_live = 0; N.dragging = 0;
    msp_init(&N.out, 0, SPRING_SNAPPY);
    msp_init(&N.gap, 0, SPRING_PAGE);
    /* pulled in by the finger: the sheet reveals its content as it moves, no build-in */
    N.t_enter = N.t_empty = nos_entered_by_finger() ? nos_t - 2.0f : nos_enter_t();
    N.cnt_from = N.cnt_to = N.n; N.cnt_t = -10;
    svc_notif_mark_all_read();
}

#define FLY_V 900.0f
#define FLY_A 5200.0f
static float clear_x(int k)              /* x offset of the k-th visible card while clearing */
{
    float t = nos_t - N.t_clear - k * (2.0f / NOS_FPS);
    if (t <= 0) return 0;
    return -(FLY_V * t + 0.5f * FLY_A * t * t);
}

static void n_tick(float dt)
{
    int n = svc_notif_count();
    if (n != N.n) {
        if (n == 0 && N.n) N.t_empty = nos_t;
        if (n > N.n && N.out_i < 0) N.gap_i = -1;      /* arrivals: no stale gap */
        N.n = n;
        if (N.pr.id >= n && N.pr.id != ID_CLEAR) press_cancel(&N.pr);
    }
    update_bounds();
    if (N.cnt_to != N.n) { N.cnt_from = N.cnt_to; N.cnt_to = N.n; N.cnt_t = nos_t; }
    msc_step(&N.sc, dt);
    if (N.out_i >= 0) {
        msp_step(&N.out, dt);
        if (N.out.target < 0 && N.out.x <= -CARD_W * 0.5f) dismiss_flying();
        else if (N.out.target == 0 && !N.out.moving) { N.out_i = -1; N.sw = -1; }
    } else if (N.fly) {
        msp_step(&N.out, dt);
        if (N.out.x <= -CARD_W - CARD_X - 4) N.fly = 0;
    }
    if (N.gap_i >= 0 && !msp_step(&N.gap, dt)) N.gap_i = -1;
    if (N.clearing && clear_x(N.clear_n - 1) <= -CARD_W - CARD_X - 4) {
        N.clearing = 0;
        svc_notif_clear();
        N.n = 0;
        update_bounds();
        msc_jump(&N.sc, 0);
        N.t_empty = nos_t;
    }
    N.now = hal_epoch_seconds();
    N.phone = svc_sys()->phone_connected || hal_phone_connected();
}

static float card_y(int i, float off)
{
    float y = TOP_Y + card_yi(i) - off;
    if (N.gap_i >= 0 && i >= N.gap_i) y += N.gap.x;
    /* build-in: cards rise 24 px into place, 2 frames apart */
    float e = (nos_t - N.t_enter - i * (2.0f / NOS_FPS)) / 0.26f;
    e = bezier_ease(&EASE_DECEL, m_clamp(e, 0, 1));
    return y + (1 - e) * 24.0f;
}

static float card_fade(int i, float y, float h)
{
    float e = m_clamp((nos_t - N.t_enter - i * (2.0f / NOS_FPS)) / 0.20f, 0, 1);
    float yc = y + h * 0.5f, d = fabsf(yc - UI_CY);
    float f = (1.0f - 0.65f * m_smoothstep(120.0f, 240.0f, d)) * e;
    return floorf(f * 16.0f + 0.5f) / 16.0f;
}

static void draw_card(gfx_fb *fb, const svc_notif *n, float x, float y, float h, float fade, int pressed)
{
    if (!n || fade <= 0) return;
    ui_rrect(fb, x, y, CARD_W, h, CARD_R, gdim(pressed ? C_LINE2 : C_SURF2, fade));
    int ix = (int)x + 36, hy = (int)y + 38;
    dot_icon(fb, n->icon ? n->icon : DOT_ICON_BELL, (float)ix, (float)(hy - 6), 2.3f, 1.85f, gdim(C_WHITE, fade));
    ui_label(fb, ix + 22, hy, n->app[0] ? n->app : "Phone", gdim(C_SEC, fade), UI_LEFT);
    char a[16];
    ago(a, sizeof a, n->t_epoch, N.now);
    if (a[0]) ui_label(fb, (int)(x + CARD_W) - 28, hy, a, gdim(C_DIS, fade), UI_RIGHT);
    int tx = (int)x + 26;
    int tb = (int)y + 52 + UI_F_BODY->ascent;
    ui_text_fit(fb, UI_F_BODY, tx, tb, n->title[0] ? n->title : n->app, gdim(C_PRI, fade), UI_LEFT, TEXT_W);
    if (n->body[0])
        ui_text_wrap(fb, UI_F_BODY, tx, tb + body_lh(), TEXT_W, body_lh(), 2, n->body, gdim(C_SEC, fade), UI_LEFT);
}

/* the standard empty / phone-absent screen (section 8); the state line follows the text */
static void draw_empty(gfx_fb *fb)
{
    float p = m_clamp((nos_t - N.t_empty) / 0.45f, 0, 1);
    if (!N.phone) {
        ui_absent(fb, DOT_ICON_BT, "Phone not connected", "Pair in Gadgetbridge as Bangle.js NOS", NULL, p);
        return;
    }
    ui_absent(fb, DOT_ICON_BELL, "No notifications", NULL, NULL, p);
    float k = floorf(m_clamp((p - 0.3f) / 0.7f, 0, 1) * 8.0f + 0.5f) / 8.0f;
    if (k > 0) ui_state(fb, UI_CX, 340, "ALL CAUGHT UP", gdim(C_SEC, k));
}

/* header: the count in section 8 secondary numerals (5x7, pitch 3, 21 px; they morph
 * when it changes) + NOTIFICATIONS, sitting on the label's baseline */
static void draw_header(gfx_fb *fb, float off)
{
    float k = 1.0f - m_clamp(off / 60.0f, 0, 1);
    if (k <= 0.02f) return;
    k = floorf(k * 16.0f + 0.5f) / 16.0f;
    int y = 70 - (int)lroundf((1 - k) * 18.0f);
    const char *word = N.cnt_to == 1 ? "NOTIFICATION" : "NOTIFICATIONS";
    char a[8], b[8];
    snprintf(a, sizeof a, "%d", N.cnt_from);
    snprintf(b, sizeof b, "%d", N.cnt_to);
    dot_style st = { DOT_FONT_5X7, 3.0f, 2.4f, gdim(C_PRI, k), 0, 1, 1 };
    int dw = dot_text_width(&st, b), lw = ui_text_width(UI_F_LABEL, word, 2);
    int x0 = UI_CX - (dw + 12 + lw) / 2;
    float top = (float)(y - 20);
    float mp = m_clamp((nos_t - N.cnt_t) / 0.25f, 0, 1);
    if (strlen(a) == strlen(b) && mp < 1) dot_text_morph(fb, &st, (float)x0, top, a, b, 0, mp);
    else dot_text(fb, &st, (float)x0, top, b);
    ui_label(fb, x0 + dw + 12, y, word, gdim(C_SEC, k), UI_LEFT);
}

static void n_draw(gfx_fb *fb)
{
    if (N.n == 0 && N.out_i < 0 && !N.fly) { draw_empty(fb); return; }
    float off = floorf(N.sc.x + 0.5f);
    draw_header(fb, off);
    int vis = 0;
    for (int i = 0; i < N.n; i++) {
        float h = card_hi(i);
        float y = floorf(card_y(i, off) + 0.5f);
        if (y + h < 0) continue;
        if (y > GFX_H) break;
        float x = CARD_X;
        float fade = card_fade(i, y, h);
        if (N.clearing) { x += floorf(clear_x(vis) + 0.5f); vis++; }
        if (i == N.sw || i == N.out_i) {
            float sx = N.out_i == i ? N.out.x : N.sw_x;
            if (sx < -30) {                     /* what letting go will do: dismiss */
                float r = 22.0f * m_clamp((-sx - 40) / 70.0f, 0, 1);
                r = floorf(r * 0.5f + 0.5f) * 2.0f;
                int armed = N.sw_armed || (N.out_i == i && N.out.target < 0);   /* not springing back */
                float cy = y + h * 0.5f;
                if (r > 0) {
                    ui_circle(fb, 380, cy, r, armed ? C_WHITE : C_SURF2);
                    if (r >= 14) dot_icon(fb, DOT_ICON_CROSS, 380, cy, 2.0f, 1.6f, armed ? C_BG : C_PRI);
                }
            }
            x += floorf(sx + 0.5f);
            if (sx < 0) fade *= 1.0f - 0.6f * m_clamp(-sx / CARD_W, 0, 1);
            fade = floorf(fade * 16.0f + 0.5f) / 16.0f;
        }
        draw_card(fb, svc_notif_get(i), x, y, h, fade, press_on(&N.pr, i));
    }
    if (N.fly && s_fly) {                      /* the dismissed card finishing its exit */
        float fx = floorf(CARD_X + N.out.x + 0.5f);
        float fy = floorf(TOP_Y + N.fly_y - off + 0.5f);
        float h = card_h_of(s_fly);
        float fade = (1.0f - 0.6f * m_clamp(-N.out.x / CARD_W, 0, 1)) * card_fade(0, fy, h);
        draw_card(fb, s_fly, fx, fy, h, floorf(fade * 16.0f + 0.5f) / 16.0f, 0);
    }
    /* CLEAR ALL closes the list; while clearing it leaves with the cards, last */
    float cy = TOP_Y + list_h() + 20 + CLEAR_H * 0.5f - off;
    if (N.gap_i >= 0) cy += N.gap.x;
    if (N.n && cy > -CLEAR_H && cy < GFX_H + CLEAR_H && (!N.clearing || N.clear_k >= 0)) {
        float e = m_clamp((nos_t - N.t_enter - N.n * (2.0f / NOS_FPS)) / 0.2f, 0, 1);
        float bx = UI_CX + (N.clearing ? floorf(clear_x(N.clear_k) + 0.5f) : 0);
        if (e >= 1 && bx > -UI_BTN_W)
            ui_button(fb, bx, floorf(cy + 0.5f), UI_BTN_W, CLEAR_H, "CLEAR ALL", BTN_SECONDARY,
                      press_on(&N.pr, ID_CLEAR));
    }
}

static int card_at(int x, int y)
{
    float off = floorf(N.sc.x + 0.5f);
    for (int i = 0; i < N.n; i++) {
        float cy = card_y(i, off);
        if (y >= cy && y < cy + card_hi(i) && x >= CARD_X && x < CARD_X + CARD_W) return i;
    }
    return -1;
}

static int clear_at(int x, int y)
{
    if (!N.n || N.clearing) return 0;
    float cy = TOP_Y + list_h() + 20 + CLEAR_H * 0.5f - floorf(N.sc.x + 0.5f);
    return fabsf(y - cy) < CLEAR_H * 0.5f + 4 && fabsf((float)(x - UI_CX)) < UI_BTN_W * 0.5f + 10;
}

static void start_clear(void)
{
    /* the cards on the glass leave left, top first, 2 frames apart */
    float off = floorf(N.sc.x + 0.5f);
    int vis = 0;
    for (int i = 0; i < N.n; i++) {
        float y = card_y(i, off);
        if (y + card_hi(i) < 0) continue;
        if (y > GFX_H) break;
        vis++;
    }
    float by = TOP_Y + list_h() + 20 + CLEAR_H * 0.5f - off;
    N.clear_k = by - CLEAR_H * 0.5f < GFX_H ? vis++ : -1;    /* the button goes last */
    N.clear_n = vis > 0 ? vis : 1;
    N.clearing = 1;
    N.t_clear = nos_t;
}

static int n_input(const nos_input *in)
{
    if (N.clearing) return 1;
    switch (in->type) {
    case IN_DOWN:
        N.was_moving = N.sc.moving && fabsf(N.sc.v) > 60.0f;
        msc_grab(&N.sc);
        press_down(&N.pr, N.was_moving ? -1 : clear_at(in->x, in->y) ? ID_CLEAR : card_at(in->x, in->y));
        return 1;
    case IN_DRAG_START:
        if (in->axis == AXIS_X) {
            int i = N.pr.down && N.pr.id >= 0 && N.pr.id != ID_CLEAR ? N.pr.id : card_at(in->x0, in->y0);
            press_cancel(&N.pr);
            msc_release(&N.sc, 0);
            if (i < 0 || N.out_i >= 0 || N.fly) return 0;
            N.sw = i; N.sw_live = 1; N.sw_x = 0; N.sw_dx0 = in->dx; N.sw_armed = 0;
            return 1;
        }
        press_cancel(&N.pr);
        if (in->dy > 0 && N.sc.x <= 0.5f) { msc_release(&N.sc, 0); return 0; }   /* engine closes */
        N.dragging = 1;
        N.grab_dy = in->dy;
        return 1;
    case IN_DRAG:
        if (N.sw_live) {
            float d = in->dx - N.sw_dx0;
            N.sw_x = d < 0 ? d : rubber_band(d, 160.0f);
            int armed = N.sw_x < -110.0f;
            if (armed != N.sw_armed) { N.sw_armed = armed; nos_feedback(FB_TICK); }
            return 1;
        }
        if (!N.dragging) return 0;
        /* the list reached the top while pulling down: the rest of the gesture is the
         * engine's, which closes (or settles) the sheet from this exact point */
        if (N.sc.drag_off0 - (in->dy - N.grab_dy) < 0 && in->vy >= 0) {
            msc_jump(&N.sc, 0);
            N.sc.dragging = 0;
            N.dragging = 0;
            nos_sheet_take();
            return 1;
        }
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
                if (N.out.v > -FLY_V) N.out.v = -FLY_V;    /* always leaves briskly */
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
        if (N.sc.dragging) msc_release(&N.sc, 0);
        int i = card_at(in->x, in->y), clr = clear_at(in->x, in->y);
        press_up(&N.pr);
        if (was) return 1;
        if (clr) { nos_feedback(FB_SUCCESS); start_clear(); }
        else if (i >= 0) { nos_feedback(FB_TAP); sys_notif_open(i); }
        return 1;
    }
    case IN_UP:
        if (N.sc.dragging) msc_release(&N.sc, 0);   /* a fling was already released */
        press_up(&N.pr);
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
    int dragging, was_moving;
    press_t pr;               /* 1 open, 2 dismiss */
    float t_enter;
    int title_lines, body_lines;
    uint64_t now;
    int phone;
} D;
static svc_notif *s_dn;       /* PSRAM copy: still drawable while the detail pops away */

#define D_HEAD_Y 94.0f        /* header baseline: at x 68 its icon sits 7 px inside the glass */
#define D_TOP 132.0f

static int title_lh(void) { return UI_F_TITLE->line_height; }
static float d_body_y(void) { return D_TOP + UI_F_TITLE->ascent + (D.title_lines - 1) * title_lh() + 22 + UI_F_BODY->ascent; }
/* the action pair's centre: the thumb-zone row (section 8), or after the body when the
 * body runs past it (then the end of the scroll brings it up to the same row) */
static float d_btn_y(void)
{
    float y = d_body_y() + (D.body_lines - 1) * body_lh() + 30 + UI_BTN_H * 0.5f;
    return y < (float)UI_BTN_Y ? (float)UI_BTN_Y : y;
}
static float d_content(void) { return d_btn_y() + (GFX_H - UI_BTN_Y); }

#define D_W 330

static void d_measure(void)
{
    const svc_notif *n = D.idx >= 0 ? svc_notif_get(D.idx) : 0;
    D.title_lines = n ? count_lines(UI_F_TITLE, n->title[0] ? n->title : n->app, D_W) : 1;
    if (D.title_lines < 1) D.title_lines = 1;
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
    nos_push(&scr_notif_detail, TX_PUSH);
}

static void d_enter(void)
{
    msc_init(&D.sc, 0, 0, 0);
    D.dragging = 0; press_cancel(&D.pr);
    D.t_enter = nos_enter_t();
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
    /* header: icon + app + time, as the card had it, on the text column (x 68): low
     * enough that the column clears the bezel there */
    int tx = UI_CX - D_W / 2;
    int hy = (int)(D_HEAD_Y - off);
    char a[16];
    ago(a, sizeof a, n->t_epoch, D.now);
    char head[48];
    snprintf(head, sizeof head, "%s%s%s", n->app[0] ? n->app : "Phone", a[0] ? "  " : "", a);
    dot_icon(fb, n->icon ? n->icon : DOT_ICON_BELL, (float)(tx + 10), (float)(hy - 6), 2.2f, 1.75f, C_WHITE);
    ui_label(fb, tx + 30, hy, head, C_SEC, UI_LEFT);
    float ty = D_TOP + UI_F_TITLE->ascent - off + lift;
    ui_text_wrap(fb, UI_F_TITLE, tx, (int)ty, D_W, title_lh(), 3, n->title[0] ? n->title : n->app, C_PRI, UI_LEFT);
    float by = d_body_y() - off + lift * 1.5f;
    if (n->body[0]) ui_text_wrap(fb, UI_F_BODY, tx, (int)by, D_W, body_lh(), 12, n->body, C_SEC, UI_LEFT);
    float b1 = d_btn_y() - off + lift * 2;
    ui_button(fb, UI_BTN_PAIR_X0, b1, UI_BTN_PAIR_W, UI_BTN_H, "OPEN", BTN_PRIMARY, press_on(&D.pr, 1));
    ui_button(fb, UI_BTN_PAIR_X1, b1, UI_BTN_PAIR_W, UI_BTN_H, "DISMISS", BTN_SECONDARY, press_on(&D.pr, 2));
    float content = d_content();
    ui_scrollbar(fb, D.sc.x, content, GFX_H, D.sc.moving || D.dragging ? 1.0f : 0.0f);
}

static int d_btn_at(int x, int y)
{
    float b1 = d_btn_y() - floorf(D.sc.x + 0.5f);
    if (fabsf(y - b1) > UI_BTN_H * 0.5f + 4) return 0;
    if (x >= UI_BTN_PAIR_X0 - UI_BTN_PAIR_W / 2 - 10 && x < UI_CX) return 1;    /* split at the gap */
    if (x >= UI_CX && x <= UI_BTN_PAIR_X1 + UI_BTN_PAIR_W / 2 + 10) return 2;
    return 0;
}

static int d_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        D.was_moving = D.sc.moving && fabsf(D.sc.v) > 60.0f;
        msc_grab(&D.sc);
        press_down(&D.pr, D.was_moving ? -1 : d_btn_at(in->x, in->y) ? d_btn_at(in->x, in->y) : -1);
        return 1;
    case IN_DRAG_START:
        press_cancel(&D.pr);
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
        press_up(&D.pr);
        if (was || D.idx < 0) return 1;
        if (b == 1) {
            if (D.phone) { nos_feedback(FB_SUCCESS); svc_notif_open_on_phone(D.idx); nos_toast("Opened"); }
            else { nos_feedback(FB_ERROR); nos_toast("No phone"); }
        } else if (b == 2) {
            nos_feedback(FB_TAP);
            /* the list closes the card's slot while the detail slides away */
            if (D.idx < N.n) gap_start(D.idx, card_hi(D.idx) + GAP);
            svc_notif_dismiss(D.idx);
            D.idx = -1;
            nos_pop();
        }
        return 1;
    }
    case IN_UP:
        if (D.sc.dragging) msc_release(&D.sc, 0);   /* a fling was already released */
        press_up(&D.pr);
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_notif_detail = { "NOTIFICATION", d_enter, 0, d_tick, d_draw, d_input, 0 };
