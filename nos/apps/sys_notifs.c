/* sys_notifs.c — notification list (pulled up from the face, TX_RISE) and detail.
 *
 * List: newest first, one flat card per notification (dot app icon, app name, time ago,
 * title, two lines of body). Momentum scroll; a downward drag at the top belongs to the
 * engine and closes the sheet with the finger. Swipe a card left to dismiss: it follows
 * the finger and leaves at the release speed (at least 30 px a frame, gathering to 60;
 * never slowing on its way out), and the cards below close the gap on a 9-frame
 * EASE_STEADY tween. "CLEAR ALL" ends the list; the cards (and the button) step 49 px left
 * at 7 px a frame while their ink fades out, top first, 2 frames apart (MOTION30 R4:
 * move less, fade more). Empty: the standard ui_absent ("No notifications",
 * [ ALL CAUGHT UP ], or "Phone not connected" with how to pair). Everything is marked
 * read on entry.
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
#include "kblur.h"
#include "diag_log.h"
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
typedef struct { int id, down, shown; nos_time_t t0, until; } press_t;
static void press_down(press_t *p, int id) { p->id = id; p->down = id >= 0; p->shown = 0; p->t0 = nos_t; p->until = 0; }
static void press_cancel(press_t *p) { p->id = -1; p->down = 0; p->until = 0; }
static void press_up(press_t *p)
{
    if (!p->down) return;
    p->down = 0;
    nos_time_t vis = p->t0 + 0.09f;                   /* when it became visible */
    p->until = nos_t < vis ? nos_t + 0.09f : vis + 0.10f;
}
static int press_on(const press_t *p, int id)
{
    if (id < 0 || p->id != id) return 0;
    if (p->down) return p->shown && nos_touch_travel() <= 8;
    return nos_t < p->until;
}
/* a held press lights once 90 ms have passed with the finger still (<= 3 px), then
 * stays lit until release unless the finger passes 8 px (a drag starting) */
static void press_tick(press_t *p)
{
    if (p->down && !p->shown && nos_t - p->t0 >= 0.09f && nos_touch_travel() <= 3) p->shown = 1;
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
    int press_id,press_valid; /* stable store identity captured on touch down */
    uint32_t press_instance;
    int was_moving, dragging;
    int sw, sw_live, sw_armed; /* card being swiped, finger on it, past the threshold */
    int sw_id,out_id;         /* swipe and fly-out owners survive index changes */
    uint32_t sw_instance,out_instance;
    uint32_t dismiss_request,clear_request;
    float sw_x, sw_dx0;
    mspring out;              /* swiped card x while flying out / returning */
    float fly_vmax;           /* its exit speed cap, px/s */
    int out_i;                /* index flying (still in the list), -1 */
    int fly;                  /* a dismissed card still crossing the glass (drawn from s_fly) */
    float fly_y;              /* its content y */
    mtween gap;               /* rows from gap_i down: extra y offset closing to 0 (0.25 s) */
    int gap_i;
    nos_time_t t_enter, t_empty;
    int clearing; nos_time_t t_clear; int clear_n;   /* CLEAR ALL: cards leave top first */
    int clear_k;              /* the button's place in that stagger, -1 off the glass */
    uint64_t now;
    int phone;
    unsigned sig;             /* ids signature the heights were computed for */
    uint32_t revision;        /* content/order revision used for layout and owners */
    int cnt_from, cnt_to; nos_time_t cnt_t;   /* header count morph */
    float band_prev;          /* last frame's drawn offset */
    float band_step;          /* live fallback exposure, pixels per reference 30Hz frame */
    int band_lv;              /* fling blur: this frame's blur level (0 = draw live) */
} N;
/* card heights and content y (prefix sums) live in PSRAM: internal DRAM is full */
static struct { float h[SVC_NOTIF_MAX]; float y[SVC_NOTIF_MAX + 1]; unsigned sig[SVC_NOTIF_MAX]; } *NL;
static svc_notif *s_fly;      /* PSRAM copy of the card flying out after its dismissal */

static float card_hi(int i) { return NL ? NL->h[i] : card_h_lines(2); }
static float card_yi(int i) { return NL ? NL->y[i] : i * (card_h_lines(2) + GAP); }
static int index_of_id(int id)
{
    for(int i=0;i<svc_notif_count();i++) {const svc_notif *n=svc_notif_get(i);if(n && n->id==id)return i;}
    return -1;
}
static int index_of_identity(int id,uint32_t instance)
{
    int i=index_of_id(id);
    const svc_notif *n=i>=0?svc_notif_get(i):NULL;
    return n && n->instance==instance?i:-1;
}

static void layout_cards(int force)
{
    uint32_t revision=svc_notif_revision();
    if(!force && NL && revision==N.revision)return;
    if (!NL && !(NL = hal_big_alloc(sizeof *NL))) return;
    unsigned sig = (unsigned)N.n * 2654435761u;
    for (int i = 0; i < N.n; i++) { const svc_notif *n = svc_notif_get(i); if (n) sig = sig * 31u + (unsigned)n->id; }
    N.sig = sig;N.revision=revision;
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
/* only on a change: msc_bounds re-aims a list that is past an edge, and calling it every
 * frame restarted the edge-bounce spring each frame (a slower, creeping return) */
static void update_bounds(void)
{
    layout_cards(0);
    float mx = content_h() - GFX_H;
    if (mx < 0) mx = 0;
    if (N.sc.min != 0 || N.sc.max != mx) msc_bounds(&N.sc, 0, mx);
}
static void cancel_swipe(void)
{
    if(N.dismiss_request) {
        diag_action_result(N.dismiss_request,DIAG_RESULT_CANCELLED,N.out_id);
        N.dismiss_request=0;
    }
    N.sw=N.out_i=-1;N.sw_live=N.sw_armed=0;N.sw_x=0;
    msp_set(&N.out,0);
}
static void sync_cards(void)
{
    uint32_t revision=svc_notif_revision();
    if(revision==N.revision && svc_notif_count()==N.n)return;
    int count=svc_notif_count();
    if(!count && N.n)N.t_empty=nos_t;
    /* A store mutation invalidates index-based gap geometry, even when count is
     * unchanged. The flying copy retains its own content position. */
    N.gap_i=-1;N.n=count;
    if(N.press_valid) {
        int index=index_of_identity(N.press_id,N.press_instance);
        if(index<0) {press_cancel(&N.pr);N.press_valid=0;}
        else if(N.pr.id!=ID_CLEAR)N.pr.id=index;
    }
    if(N.sw>=0) {
        int index=index_of_identity(N.sw_id,N.sw_instance);
        if(index<0)cancel_swipe();else N.sw=index;
    }
    if(N.out_i>=0) {
        int index=index_of_identity(N.out_id,N.out_instance);
        if(index<0)cancel_swipe();else N.out_i=index;
    }
    update_bounds();
}

/* the rows from i down close a gap of h px: EASE_STEADY over 9 frames (a component move,
 * peak <= 1.6x the average, ~33 px a frame for the tallest card), after `delay` frames */
static void gap_start(int i, float h, int delay)
{
    mtw_start(&N.gap, h, 0, M_FRAMES(9), &EASE_STEADY);
    N.gap.t0 += M_FRAMES(delay);
    N.gap_i = i;
}

/* the swiped card is half off the glass: it leaves the data now (drawn from a copy) so
 * the rows below start closing the gap while it is still flying */
static void dismiss_flying(void)
{
    int i = index_of_identity(N.out_id,N.out_instance);
    if(i<0) {cancel_swipe();return;}
    const svc_notif *n = svc_notif_get(i);
    if (!s_fly) s_fly = (svc_notif *)hal_big_alloc(sizeof *s_fly);
    if (n && s_fly) { *s_fly = *n; N.fly = 1; N.fly_y = card_yi(i); }
    float h = card_hi(i) + GAP;
    N.out_i = -1; N.sw = -1;
    svc_notif_dismiss(i);
    diag_action_result(N.dismiss_request,index_of_identity(N.out_id,N.out_instance)<0?
                       DIAG_RESULT_SUCCESS:DIAG_RESULT_FAILED,N.out_id);
    N.dismiss_request=0;
    N.n = svc_notif_count();
    update_bounds();
    gap_start(i, h, 0);
    if (!N.n) N.t_empty = nos_t + M_FRAMES(5);
}

static void n_enter(void)
{
    if (nos_revealed()) {                  /* back from the detail: keep everything */
        N.n = svc_notif_count();
        update_bounds();
        press_cancel(&N.pr); N.sw = -1; N.sw_live = 0; N.dragging = 0;
        N.press_valid=0;
        N.band_prev = floorf(N.sc.x + 0.5f); N.band_lv = 0;
        svc_notif_mark_all_read();
        return;
    }
    N.n = svc_notif_count();
    msc_init(&N.sc, 0, 0, 0);
    N.band_prev = 0; N.band_lv = 0;
    layout_cards(1);
    update_bounds();
    press_cancel(&N.pr); N.sw = -1; N.out_i = -1; N.gap_i = -1; N.fly = 0; N.clearing = 0;
    N.press_valid=0;
    N.sw_live = 0; N.dragging = 0;
    msp_init(&N.out, 0, SPRING_SNAPPY);
    mtw_start(&N.gap, 0, 0, 0.0f, &EASE_STANDARD);
    /* pulled in by the finger: the sheet reveals its content as it moves, no build-in */
    N.t_enter = N.t_empty = nos_entered_by_finger() ? nos_t - 2.0f : nos_enter_t();
    N.cnt_from = N.cnt_to = N.n; N.cnt_t = -10;
    svc_notif_mark_all_read();
}
static void n_leave(void)
{
    cancel_swipe();
    if(N.clear_request)diag_action_result(N.clear_request,DIAG_RESULT_CANCELLED,svc_notif_count());
    N.clear_request=0;N.clearing=0;
}

/* a swiped card leaves at its release speed, at least FLY_V, gathering by FLY_A up to
 * FLY_VMAX (60 px a frame) unless the finger was faster: an exit never slows down */
#define FLY_V 900.0f
#define FLY_A 5200.0f
#define FLY_VMAX 1800.0f
/* CLEAR ALL: each item steps CLEAR_DX left at a whole 7 px a frame over CLEAR_N frames
 * while its ink fades out (m_ink), 2 frames after the one above it */
#define CLEAR_N 7
#define CLEAR_DX 49.0f
static float clear_p(int k)              /* 0..1 progress of the k-th visible item */
{
    return m_clamp((float)m_stagger_k(m_frame_k(N.t_clear), k, 2) / CLEAR_N, 0, 1);
}
static float clear_x(int k) { return -CLEAR_DX * clear_p(k); }
static float clear_ink(int k) { return m_ink(1.0f - clear_p(k)); }

static void n_probe(void);
static void band_tick(float dt);

static void n_tick(float dt)
{
    press_tick(&N.pr);
    sync_cards();
    update_bounds();
    if (N.cnt_to != N.n) { N.cnt_from = N.cnt_to; N.cnt_to = N.n; N.cnt_t = nos_t; }
    msc_step(&N.sc, dt);
    if (N.out_i >= 0 || N.fly) {
        if (N.out.target < 0) {                /* leaving: never slower than it was let go */
            N.out.v = fmaxf(N.out.v - FLY_A * dt, -N.fly_vmax);
            N.out.x += N.out.v * dt;
        } else msp_step(&N.out, dt);           /* back into place: a pixel spring */
        if (N.out_i >= 0) {
            if (N.out.target < 0 && N.out.x <= -CARD_W * 0.5f) dismiss_flying();
            else if (N.out.target == 0 && !N.out.moving) { N.out_i = -1; N.sw = -1; }
        } else if (N.out.x <= -CARD_W - CARD_X - 4) N.fly = 0;
    }
    if (N.gap_i >= 0 && mtw_done(&N.gap)) N.gap_i = -1;
    if (N.clearing && clear_p(N.clear_n - 1) >= 1.0f) {
        N.clearing = 0;
        svc_notif_clear();
        diag_action_result(N.clear_request,svc_notif_count()?DIAG_RESULT_FAILED:DIAG_RESULT_SUCCESS,svc_notif_count());
        N.clear_request=0;
        N.n = 0;
        update_bounds();
        msc_jump(&N.sc, 0);              /* the empty state is already building (start_clear) */
    }
    N.now = hal_epoch_seconds();
    N.phone = svc_sys()->phone_connected || hal_phone_connected();
    band_tick(dt);
    n_probe();
}

/* build-in of card i (and of CLEAR ALL as item n): the 6-frame table (28/53/73/87/96/100 %)
 * from a whole-frame start 2 frames after the one above, at most 8 starts (R9) */
static float card_in(int i) { return mf_at(&MF_MICRO6, m_stagger_k(m_frame_k(N.t_enter), i, 2)); }

static float card_y(int i, float off)
{
    float y = TOP_Y + card_yi(i) - off;
    if (N.gap_i >= 0 && i >= N.gap_i) y += floorf(mtw_value(&N.gap) + 0.5f);
    /* build-in: cards rise 24 px into place (7 6 5 3 2 1 px), 2 frames apart */
    return y + (1 - card_in(i)) * 24.0f;
}

static float card_fade(int i, float y, float h)
{
    float e = m_ink(card_in(i));               /* ink comes up in perceptual steps */
    float yc = y + h * 0.5f, d = fabsf(yc - UI_CY);
    float f = (1.0f - 0.65f * m_smoothstep(120.0f, 240.0f, d)) * e;
    return floorf(f * 16.0f + 0.5f) / 16.0f;
}

static void draw_card(gfx_fb *fb, const svc_notif *n, float x, float y, float h, float fade, int pressed)
{
    if (!n || fade <= 0) return;
    /* The caller's card height need not bound two wrapped body lines. Keep
     * the union of background and worst-case text ink, with AA/truncation guard.
     * Current fonts fit their declared ascent/line height; icon ink starts at y+18. */
    if(!fb->rec) {
        float ink_bottom=(float)(int)y+52+UI_F_BODY->ascent+
            (n->body[0]?2*body_lh():0)+UI_F_BODY->line_height;
        if((int)ceilf(fmaxf(y+h,ink_bottom))+4+fb->oy<=fb->y_base ||
           (int)floorf(y)-4+fb->oy>=fb->y_base+fb->h) return;
    }

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
    float p = m_clamp((nos_t - N.t_empty) / M_FRAMES(14), 0, 1);   /* the empty build, 14 frames */
    if (!N.phone) {
        ui_absent(fb, DOT_ICON_BT, "Phone not connected", "Pair in Gadgetbridge as Bangle.js NOS", NULL, p);
        return;
    }
    ui_absent(fb, DOT_ICON_BELL, "No notifications", NULL, NULL, p);
    float k = m_ink((p - 0.3f) / 0.7f);
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
    float mp = m_clamp((nos_t - N.cnt_t) / M_FRAMES(8), 0, 1);   /* the dot morph, 8 whole frames */
    if (strlen(a) == strlen(b) && mp < 1) dot_text_morph(fb, &st, (float)x0, top, a, b, 0, mp);
    else dot_text(fb, &st, (float)x0, top, b);
    int label_w = ui_label(fb, x0 + dw + 12, y, word, gdim(C_SEC, k), UI_LEFT);
    int text_top = y - UI_F_LABEL->ascent - 2;
    if (text_top > y - 23) text_top = y - 23;
    ui_fblur_coverage_exclude(fb, x0 - 3, text_top, x0 + dw + 12 + label_w + 3,
                            y + UI_F_LABEL->line_height - UI_F_LABEL->ascent + 3);
}

/* ---- fling blur (ui.h): the cards and CLEAR ALL in 90 px content bands (from TOP_Y),
 * baked with 24 px of context. Only a free glide blurs: a held list, a swipe, a card
 * flying out, a closing gap, CLEAR ALL and the entry build all stay live. */
#define BAND_X0 ((int)CARD_X - 1)
#define BAND_X1 ((int)(CARD_X + CARD_W) + 2)
static float clear_cy(void) { return list_h() + 20 + CLEAR_H * 0.5f; }   /* content y, from TOP_Y */
/* ---- fling blur: one blurred copy per card (kblur.h), made once while idle ----
 * Cards keep their look once they arrive, so each card's band (the card and half the
 * gap above and below it) is baked when the card comes in, in idle frames (on the face
 * too: sys_notifs_prewarm), and kept in its own cache; only a new card, or one whose
 * look changed (its age label: NOW, 1 MIN ...), is baked again. CLEAR ALL is an item too,
 * so a glide never shows one sharp thing among smeared ones. */
#define KB_W     (BAND_X1 - BAND_X0)
#define KB_MAXH  ((int)(card_h_lines(2) + GAP + 1))
#define KB_CLEAR 0xC1EA0001u
#define KB_CLR_H ((int)CLEAR_H + 20)
static kblur *s_kb;
static nos_time_t s_kb_retry_at;
static uint32_t s_kb_retry_room;
void sys_notifs_blur_release(void)
{
    hal_big_free(s_kb); s_kb = 0;
    s_kb_retry_at = 0; s_kb_retry_room = 0;
}
void sys_notifs_blur_mem(void)
{
    printf("MEM_CACHE notifications slots=%d width=%d max_height=%d\n", kblur_slots(s_kb), KB_W, KB_MAXH);
}
static unsigned card_sig(const svc_notif *n, float h, uint64_t now)
{
    char a[16];
    ago(a, sizeof a, n->t_epoch, now);
    unsigned g = gfx_hash_u(0x9E3779B9u, (unsigned)h);
    g = gfx_hash_u(g, (unsigned)n->icon);
    for (const char *t = n->app; *t; t++) g = g * 31u + (unsigned char)*t;
    for (const char *t = n->title; *t; t++) g = g * 31u + (unsigned char)*t;
    for (const char *t = n->body; *t; t++) g = g * 31u + (unsigned char)*t;
    for (const char *t = a; *t; t++) g = g * 31u + (unsigned char)*t;
    return g;
}
/* the bake view's column 0 is BAND_X0 on the glass; an item's top is at y */
static void kb_card(gfx_fb *fb, int i, int x0, int y, int x1, int h, void *ctx)
{
    (void)i; (void)x1;
    const svc_notif *n = (const svc_notif *)ctx;
    draw_card(fb, n, (float)(x0 + (int)CARD_X - BAND_X0), (float)y + GAP * 0.5f, (float)h - GAP, 1.0f, 0);
}
static void kb_clear(gfx_fb *fb, int i, int x0, int y, int x1, int h, void *ctx)
{
    (void)i; (void)x1; (void)h; (void)ctx;
    ui_button(fb, x0 + UI_CX - BAND_X0, y + 10.0f + CLEAR_H * 0.5f, UI_BTN_W, CLEAR_H, "CLEAR ALL", BTN_SECONDARY, 0);
}
/* idle frames: bake whatever is missing, newest first (as many as the cache holds) */
void sys_notifs_prewarm(void)
{
    if (nos_transitioning()) return;
    int n = svc_notif_count();
    if (n <= 0) return;
    if (!s_kb) {
        uint32_t room = 0;
#ifdef NOS_PSRAM_TRACKING
        /* Keep mandatory UI state/geometry ahead of an optional prewarmed arena. */
        room = nos_mem_available();
        if (room < (896u + 512u) * 1024u) return;
#endif
        if (nos_t < s_kb_retry_at && room <= s_kb_retry_room) return;
        if (!(s_kb = kblur_new(KB_W, KB_MAXH, 24, 896u * 1024u))) {
            s_kb_retry_at = nos_t + 1.0f;
            s_kb_retry_room = room;
            return;
        }
        s_kb_retry_at = 0; s_kb_retry_room = 0;
    }
    if (n > SVC_NOTIF_MAX) n = SVC_NOTIF_MAX;
    uint64_t now = hal_epoch_seconds();
    unsigned keys[SVC_NOTIF_MAX + 1], idsig = 0;
    int nk = 0;
    for (int i = 0; i < n; i++) {
        const svc_notif *v = svc_notif_get(i);
        if (v) { keys[nk++] = (unsigned)v->id; idsig = idsig * 31u + (unsigned)v->id; }
    }
    keys[nk++] = KB_CLEAR;
    static unsigned s_idsig;
    if (idsig != s_idsig) { s_idsig = idsig; kblur_keep_only(s_kb, keys, nk); }   /* dismissed: free */
    int room = kblur_slots(s_kb) - 1;            /* the button keeps a slot */
    /* Follow the viewport, including older notifications after a long scroll. */
    int first = 0;
    if (nos_top() == &scr_notifs) {
        while (first + 1 < n && card_y(first, N.sc.x) + card_hi(first) < 0) first++;
        if (first > 0) first--;
    }
    if (first + room > n) first = n > room ? n - room : 0;
    unsigned keep[KB_NLEV + SVC_NOTIF_MAX + 1]; int nkeep = 0;
    for (int i = first; i < n && i < first + room; i++) {
        const svc_notif *v = svc_notif_get(i); if (v) keep[nkeep++] = (unsigned)v->id;
    }
    keep[nkeep++] = KB_CLEAR;
    kblur_keep_only(s_kb, keep, nkeep);
    for (int i = first; i < n && i < first + room; i++) {
        const svc_notif *v = svc_notif_get(i);
        if (!v) continue;
        float h = card_h_of(v) + GAP;
        /* One changed card per frame, even when its final slice finishes early. */
        unsigned sig = card_sig(v, h, now);
        if (!kblur_ready(s_kb, (unsigned)v->id, sig, KB_NLEV)) {
            kblur_bake(s_kb, (unsigned)v->id, sig, (int)h, kb_card, i, (void *)v, 2000u);
            return;
        }
    }
    kblur_bake(s_kb, KB_CLEAR, 1u, KB_CLR_H, kb_clear, 0, 0, 2000u);
}
static void band_tick(float dt)
{
    float off = floorf(N.sc.x + 0.5f), fr = dt * M_FPS, st = (off - N.band_prev) / (fr > 0.5f ? fr : 1.0f);
    N.band_prev = off;
    /* Cached ladders take NOS_FPS-normalized steps; live kernels take the
     * authored 1/30-second exposure. Convert once for both cold-card paths. */
    N.band_step = fabsf(st) * M_FPS / 30.0f;
    N.band_lv = 0;
    if (N.n <= 0) return;
    int free_glide = !N.clearing && N.sw < 0 && N.out_i < 0 && !N.fly && N.gap_i < 0 && card_in(N.n) >= 1;
    N.band_lv = !free_glide || NOS_HELD_SHARP(N.dragging || N.sc.dragging) ? 0
              : ui_fblur_level_early(fabsf(st));          /* nobody reads a card above 150 px/s */
    if (!N.band_lv) nos_blur_note(NOS_BS_NOTIFS, N.dragging || N.sc.dragging || st != 0.0f, 0, 0);   /* too slow (the draw notes fast frames) */
    /* this frame's looks (the glide checks them), and idle frames bake */
    if (NL) for (int i = 0; i < N.n && i < SVC_NOTIF_MAX; i++) {
        const svc_notif *v = svc_notif_get(i);
        NL->sig[i] = v ? card_sig(v, card_hi(i) + GAP, N.now) : 0;
    }
    if (st == 0.0f && !N.dragging && !N.sc.dragging) sys_notifs_prewarm();
}
/* a glide: every card on the glass (and CLEAR ALL) from its blurred copy, if all are
 * ready at this level; else the caller draws live */
static int notif_blur_draw(gfx_fb *fb, unsigned key, int level, int y, int height, float fade)
{
    int drawn = kblur_draw(fb, s_kb, key, level, BAND_X0, y, fade);
    if (drawn) ui_fblur_coverage_claim(fb, BAND_X0, y, BAND_X1, y + height);
    return drawn;
}
static int draw_blurred_(gfx_fb *fb, float off);
static int draw_blurred(gfx_fb *fb, float off)
{
    int r = draw_blurred_(fb, off);
    if (r) nos_blur_note(NOS_BS_NOTIFS, 1, 1, 1);
    return r;
}
static int draw_blurred_(gfx_fb *fb, float off)
{
    int lv = N.band_lv;
    if (!s_kb || !NL || lv <= 0) return 0;
    int i0 = -1, i1 = -1;
    for (int i = 0; i < N.n && i < SVC_NOTIF_MAX; i++) {
        float y = floorf(card_y(i, off) + 0.5f), h = card_hi(i);
        if (y + h < 0) continue;
        if (y > GFX_H) break;
        const svc_notif *v = svc_notif_get(i);
        if (!v || !kblur_ready(s_kb, (unsigned)v->id, NL->sig[i], lv)) return 0;
        if (i0 < 0) i0 = i;
        i1 = i;
    }
    float by = TOP_Y + clear_cy() - off;
    int clr = by - CLEAR_H * 0.5f - 10 < GFX_H && by + CLEAR_H * 0.5f + 10 > 0;
    if (clr && !kblur_ready(s_kb, KB_CLEAR, 1u, lv)) return 0;
    for (int i = i0; i >= 0 && i <= i1; i++) {
        float y = floorf(card_y(i, off) + 0.5f), h = card_hi(i);
        notif_blur_draw(fb, (unsigned)svc_notif_get(i)->id, lv, (int)(y - GAP * 0.5f), (int)(h + GAP), card_fade(i, y, h));
    }
    if (clr) notif_blur_draw(fb, KB_CLEAR, lv, (int)floorf(by - CLEAR_H * 0.5f - 10 + 0.5f),
                             KB_CLR_H, card_fade(N.n, by - CLEAR_H * 0.5f, CLEAR_H));
    draw_header(fb, off);
    return 1;
}

static void n_draw(gfx_fb *fb)
{
    if (N.n == 0 && N.out_i < 0 && !N.fly) { draw_empty(fb); return; }
    if (N.clearing && nos_t >= N.t_empty) draw_empty(fb);   /* under the last cards out */
    float off = floorf(N.sc.x + 0.5f);
    if (N.band_lv && draw_blurred(fb, off)) return;   /* a glide: the cards' blurred copies */
    int vis = 0, all_blurred = 1;
    for (int i = 0; i < N.n; i++) {
        float h = card_hi(i);
        float y = floorf(card_y(i, off) + 0.5f);
        if (y + h < 0) continue;
        if (y > GFX_H) break;
        const svc_notif *v = svc_notif_get(i);
        if (N.band_lv && v && kblur_ready(s_kb, (unsigned)v->id, NL->sig[i], N.band_lv)) {
            notif_blur_draw(fb, (unsigned)v->id, N.band_lv, (int)(y - GAP * .5f), (int)(h + GAP), card_fade(i, y, h));
            continue;
        }
        float x = CARD_X;
        float fade = card_fade(i, y, h);
        if (N.clearing) {
            x += floorf(clear_x(vis) + 0.5f);
            fade = floorf(fade * clear_ink(vis) * 16.0f + 0.5f) / 16.0f;
            vis++;
        }
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
        if (N.band_lv) {
            int lo = y < 0 ? 0 : (int)y, hi = y + h > GFX_H ? GFX_H : (int)(y + h);
            if (!nos_mblur_v_fallback(fb, (int)CARD_X, lo, (int)(CARD_X + CARD_W), hi, fmaxf(8, N.band_step))) all_blurred = 0;
        }
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
    if (N.gap_i >= 0) cy += floorf(mtw_value(&N.gap) + 0.5f);
    if (N.n && cy > -CLEAR_H && cy < GFX_H + CLEAR_H && (!N.clearing || N.clear_k >= 0)) {
        /* it arrives at full colour once the build reaches it (no grey fade-up) */
        float bx = UI_CX + (N.clearing ? floorf(clear_x(N.clear_k) + 0.5f) : 0);
        float ink = N.clearing ? clear_ink(N.clear_k) : 1.0f;
        gfx_fb sub;
        if (card_in(N.n) >= 1 && ink > 0) {
            if (ink >= 1.0f)
                ui_button(fb, bx, floorf(cy + 0.5f), UI_BTN_W, CLEAR_H, "CLEAR ALL", BTN_SECONDARY,
                          press_on(&N.pr, ID_CLEAR));
            else if (nos_subview(fb, &sub, 0, 0, GFX_W, GFX_H, 0, 0)) {
                /* the recorder hashes colours, not the ink dim: record the level too */
                if (fb->rec) gfx_rec_cmd(fb, (int)bx - UI_BTN_W / 2, (int)cy - UI_BTN_H / 2,
                                         (int)bx + UI_BTN_W / 2 + 1, (int)cy + UI_BTN_H / 2 + 1,
                                         gfx_hash_f(0xc1ea7u, ink));
                nos_ink_dim(&sub, ink);
                ui_button(&sub, bx, floorf(cy + 0.5f), UI_BTN_W, CLEAR_H, "CLEAR ALL", BTN_SECONDARY, 0);
            }
        }
    }
    if (N.band_lv) {
        if (N.n && cy + CLEAR_H * .5f > 0 && cy - CLEAR_H * .5f < GFX_H) {
            int lo = (int)fmaxf(0, cy - CLEAR_H * .5f), hi = (int)fminf(GFX_H, cy + CLEAR_H * .5f);
            if (!nos_mblur_v_fallback(fb, UI_CX - UI_BTN_W / 2, lo, UI_CX + UI_BTN_W / 2, hi, fmaxf(8, N.band_step))) all_blurred = 0;
        }
        nos_blur_note(NOS_BS_NOTIFS, 1, 1, all_blurred);
    }
    draw_header(fb, off);
}

/* motion audit (sim only): every animated value as drawn */
static void n_probe(void)
{
    nos_motion_probe("notifs.list", N.dragging ? MC_FINGER : MC_MOMENTUM, -floorf(N.sc.x + 0.5f));
    if (N.sw >= 0 || N.out_i >= 0 || N.fly) {
        float sx = N.out_i >= 0 || N.fly ? N.out.x : N.sw_x;
        nos_motion_probe("notifs.swipe", N.sw_live ? MC_FINGER : MC_MOMENTUM, floorf(sx + 0.5f));
    }
    if (N.gap_i >= 0) nos_motion_probe("notifs.gap", MC_COMPONENT, floorf(mtw_value(&N.gap) + 0.5f));
    static const char *const k_y[3] = { "notifs.card0.y", "notifs.card1.y", "notifs.card2.y" };
    for (int i = 0; i < 3 && i < N.n; i++)
        nos_motion_probe(k_y[i], MC_MICRO, floorf((1 - card_in(i)) * 24.0f + 0.5f));
    if (N.n) nos_motion_probe("notifs.card0.ink", MC_FADE, m_ink(card_in(0)) * 100.0f);
    if (N.clearing) {
        static const char *const k_c[4] = { "notifs.clear0", "notifs.clear1", "notifs.clear2", "notifs.clear3" };
        static const char *const k_i[4] = { "notifs.clear0.ink", "notifs.clear1.ink", "notifs.clear2.ink", "notifs.clear3.ink" };
        for (int k = 0; k < 4 && k < N.clear_n; k++) {
            nos_motion_probe(k_c[k], MC_COMPONENT, floorf(clear_x(k) + 0.5f));
            nos_motion_probe(k_i[k], MC_FADE, clear_ink(k) * 100.0f);
        }
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
    N.clear_request=diag_action_request(DIAG_DOMAIN_NOTIFICATION,DIAG_ACT_DISMISS,0);
    diag_action_context(N.clear_request,(int32_t)diag_log_name_id("NOTIFICATIONS"),svc_notif_count());
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
    N.t_clear = nos_t - 1.0f / NOS_FPS;        /* the tap's frame already moves */
    /* the empty state builds while the last item exits: no black glass between */
    int last = m_stagger_k(0, N.clear_n - 1, 2);
    N.t_empty = N.t_clear + M_FRAMES(-last + 3);
}

static int n_input(const nos_input *in)
{
    sync_cards();
    if (N.clearing) return 1;
    switch (in->type) {
    case IN_CANCEL:
        press_cancel(&N.pr);N.press_valid=0;cancel_swipe();N.dragging=0;
        msc_release(&N.sc,0);return 1;
    case IN_DOWN:
        N.was_moving = N.sc.moving && fabsf(N.sc.v) > 60.0f;
        msc_grab(&N.sc);
        press_down(&N.pr, N.was_moving ? -1 : clear_at(in->x, in->y) ? ID_CLEAR : card_at(in->x, in->y));
        N.press_valid=N.pr.id>=0 && N.pr.id<N.n;
        if(N.press_valid) {
            const svc_notif *n=svc_notif_get(N.pr.id);
            N.press_id=n->id;N.press_instance=n->instance;
        }
        return 1;
    case IN_DRAG_START:
        if (in->axis == AXIS_X) {
            int i = N.press_valid ? index_of_identity(N.press_id,N.press_instance) : -1;
            press_cancel(&N.pr);
            msc_release(&N.sc, 0);
            if (i < 0 || N.out_i >= 0 || N.fly) return 0;
            N.sw = i; N.sw_live = 1; N.sw_x = 0; N.sw_dx0 = in->dx; N.sw_armed = 0;
            N.sw_id=svc_notif_get(i)->id;N.sw_instance=svc_notif_get(i)->instance;N.press_valid=0;
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
            msp_init_px(&N.out, N.sw_x, 0.30f, 1.0f);   /* back into place ends with a 1 px snap */
            N.out.v = in->vx;
            N.out_i = N.sw;
            N.out_id=N.sw_id;N.out_instance=N.sw_instance;
            if (N.sw_x < -110.0f || in->vx < -600.0f) {
                N.dismiss_request=diag_action_request(DIAG_DOMAIN_NOTIFICATION,DIAG_ACT_DISMISS,N.out_id);
                diag_action_context(N.dismiss_request,(int32_t)diag_log_name_id("NOTIFICATIONS"),(int32_t)N.out_instance);
                N.out.target = -CARD_W - CARD_X - 20;      /* leaving (integrated in tick) */
                if (N.out.v > -FLY_V) N.out.v = -FLY_V;    /* always leaves briskly */
                N.fly_vmax = fmaxf(-N.out.v, FLY_VMAX);
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
        int i=N.press_valid?index_of_identity(N.press_id,N.press_instance):-1;
        int clr=N.pr.id==ID_CLEAR && clear_at(in->x,in->y);
        press_up(&N.pr);
        if (was) return 1;
        if (clr) { nos_feedback(FB_SUCCESS); start_clear(); }
        else if (i >= 0) { nos_feedback(FB_TAP); sys_notif_open(i); }
        return 1;
    }
    case IN_UP:
        if (N.sc.dragging) msc_release(&N.sc, 0);   /* a fling was already released */
        press_up(&N.pr);
        N.press_valid=0;
        return 1;
    default:
        return 0;
    }
}

const nos_screen scr_notifs = { "NOTIFICATIONS", n_enter, n_leave, n_tick, n_draw, n_input, SCR_HDRAG | SCR_NATIVE_CACHE };

/* ===================================================================== detail */
static struct {
    int id;                   /* notification id being shown */
    uint32_t instance;        /* a removed/reused phone id is a different card */
    uint32_t open_request;
    int idx;                  /* its current index, -1 gone */
    mscroll sc;
    float grab_dy;
    int dragging, was_moving;
    press_t pr;               /* 1 open, 2 dismiss */
    nos_time_t t_enter;
    int title_lines, body_lines;
    uint64_t now;
    int phone;
    uint32_t revision;
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
    if(D.open_request)diag_action_result(D.open_request,DIAG_RESULT_CANCELLED,D.id);
    D.open_request=diag_action_request(DIAG_DOMAIN_NOTIFICATION,DIAG_ACT_OPEN,n->id);
    diag_action_context(D.open_request,(int32_t)diag_log_name_id("NOTIFICATIONS"),(int32_t)n->instance);
    D.id = n->id;D.instance=n->instance;
    D.idx = i;
    if (!s_dn) s_dn = (svc_notif *)hal_big_alloc(sizeof *s_dn);
    if (s_dn) *s_dn = *n;
    nos_push(&scr_notif_detail, TX_PUSH);
}

/* the entry build: title, body and buttons rise 16 / 24 / 32 px on the 6-frame table
 * (whole frames from the first visible frame of the push), the rows below further, as
 * the shared-axis fade brings the screen in. Returns the title's lift (whole px). */
static float d_lift(void)
{
    return floorf((1.0f - mf_at(&MF_MICRO6, m_frame_k(D.t_enter))) * 16.0f + 0.5f);
}

static void d_enter(void)
{
    msc_init(&D.sc, 0, 0, 0);
    D.dragging = 0; press_cancel(&D.pr);
    D.t_enter = nos_enter_t();
    D.revision=svc_notif_revision();
    d_measure();
}

static void d_tick(float dt)
{
    press_tick(&D.pr);
    /* follow the notification by id */
    const svc_notif *n;
    uint32_t revision=svc_notif_revision();
    if(revision!=D.revision) {D.idx=index_of_identity(D.id,D.instance);D.revision=revision;d_measure();}
    if(D.open_request && (!nos_contains(&scr_notif_detail) || D.idx<0 || !nos_transitioning())) {
        diag_action_result(D.open_request,D.idx<0?DIAG_RESULT_STALE:
                           nos_top()==&scr_notif_detail?DIAG_RESULT_SUCCESS:DIAG_RESULT_CANCELLED,D.id);
        D.open_request=0;
    }
    if (D.idx >= 0 && s_dn && (n = svc_notif_get(D.idx)) != 0) *s_dn = *n;
    msc_step(&D.sc, dt);
    D.now = hal_epoch_seconds();
    D.phone = svc_sys()->phone_connected || hal_phone_connected();
    /* motion audit (sim only) */
    nos_motion_probe("detail.list", D.dragging ? MC_FINGER : MC_MOMENTUM, -floorf(D.sc.x + 0.5f));
    float lift = d_lift();
    nos_motion_probe("detail.lift.title", MC_MICRO, lift);
    nos_motion_probe("detail.lift.btn", MC_MICRO, lift * 2);
}
static void d_leave(void)
{
    if(D.open_request)diag_action_result(D.open_request,DIAG_RESULT_CANCELLED,D.id);
    D.open_request=0;
}

static void d_draw(gfx_fb *fb)
{
    const svc_notif *n = s_dn;
    if (!n) return;
    float off = floorf(D.sc.x + 0.5f);
    float lift = d_lift();
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
    float by = d_body_y() - off + floorf(lift * 1.5f + 0.5f);
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
    case IN_CANCEL: press_cancel(&D.pr);D.dragging=0;msc_release(&D.sc,0);return 1;
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
        /* Input runs before the service/tick phase. Resolve immediately, even if
         * this frame's store mutation has not yet reached d_tick(). */
        D.idx=index_of_identity(D.id,D.instance);
        int b = d_btn_at(in->x, in->y), was = D.was_moving;
        msc_release(&D.sc, 0);
        press_up(&D.pr);
        if (was) return 1;
        uint32_t request=0;
        if(b==1 || b==2) {
            request=diag_action_request(DIAG_DOMAIN_NOTIFICATION,b==1?DIAG_ACT_SEND:DIAG_ACT_DISMISS,D.id);
            diag_action_context(request,(int32_t)diag_log_name_id("NOTIFICATION"),(int32_t)D.instance);
        }
        if(D.idx<0) {diag_action_result(request,DIAG_RESULT_STALE,D.id);return 1;}
        if (b == 1) {
            diag_action_result(request,D.phone?DIAG_RESULT_PENDING:DIAG_RESULT_REJECTED,D.id);
            if (D.phone) { nos_feedback(FB_SUCCESS); svc_notif_open_on_phone(D.idx); nos_toast("Opened"); }
            else { nos_feedback(FB_ERROR); nos_toast("No phone"); }
        } else if (b == 2) {
            nos_feedback(FB_TAP);
            /* the list closes the card's slot while the detail slides away */
            /* ...once the list shows again (the shared-axis close fades the detail out
             * over its first 3 frames), so the slot is seen closing */
            if (D.idx < N.n) gap_start(D.idx, card_hi(D.idx) + GAP, 3);
            svc_notif_dismiss(D.idx);
            diag_action_result(request,index_of_identity(D.id,D.instance)<0?DIAG_RESULT_SUCCESS:DIAG_RESULT_FAILED,D.id);
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

const nos_screen scr_notif_detail = { "NOTIFICATION", d_enter, d_leave, d_tick, d_draw, d_input, 0 };
