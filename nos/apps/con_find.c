/* con_find.c — Find my phone, and the screen shown when the phone looks for the watch.
 *
 * scr_find: one big round button. Tapping it rings the phone (svc_find_phone) and
 * sends dot ripples outward from the button: rings of dots at growing radii whose
 * colour steps down the grey ladder as they travel. Tap again (anywhere) to stop.
 *
 * scr_find_watch: pushed by the services when the phone rings the watch. Bright
 * white dot ripples across the whole face at full panel brightness until a tap (the
 * services' ringer loops the buzz and chime meanwhile).
 *
 * Ripple motion blur (ui_ripple_blur): each ripple dot drags a tail of overlapping,
 * dimmer, slightly smaller dots back towards the centre, so a ring reads as a smeared
 * wavefront. Mode 2 also launches each ring fast and lets it glide out (ease-out), the
 * tail shrinking with its speed. The dots of a frame are built once in tick() into a
 * PSRAM list (tails farthest layer first, then the dots) and stamped per stripe. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>

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

static uint16_t grey_step(float k)        /* k 0 (bright) .. 1 (gone): 5 discrete steps */
{
    static const uint16_t ladder[] = { C_PRI, C_SEC, C_DIS, C_LINE2, C_LINE };
    int i = (int)(k * 5.0f);
    if (i < 0) i = 0;
    if (i > 4) i = 4;
    return ladder[i];
}

/* ============================================================ ripples ===== */
typedef struct { float x, y, d; uint16_t c; } rip_dot;
#define RIP_MAXD  2048          /* 4 rings x 56 dots x (dot + 8 tail dots) */
#define RIP_TAIL  8             /* tail dots at most: spaced <= 0.45 of a dot, they merge */
#define RIP_CHUNK 8             /* dots per damage command */
typedef struct {
    float cx, cy, r0, r1, per, d0;
    int rings, ndots;
    uint16_t (*col)(float ph);                 /* a ring's colour by its age 0..1 */
    int (*clear)(float x, float y, float r);   /* 0: a dot that must not be drawn there */
} rip_spec;
typedef struct { rip_dot *dot; int n; } rip_list;

/* a ring's radius at age ph (0..1): even, or (burst) fast out of the button, gliding on;
 * *tail = its tail length as a multiple of the dot */
static float rip_radius(const rip_spec *sp, float ph, float *tail)
{
    if (ui_ripple_blur == 2) {
        float u = 1.0f - ph, su = gfx_fast_sqrt(u);
        if (tail) *tail = 3.5f * u * su;      /* the speed of r below: (1 - ph)^1.5 */
        return sp->r0 + (sp->r1 - sp->r0) * (1.0f - u * u * su);
    }
    if (tail) *tail = ui_ripple_blur ? 2.2f : 0.0f;
    return sp->r0 + (sp->r1 - sp->r0) * ph;
}

static void rip_build(rip_list *L, const rip_spec *sp, float t)
{
    if (!L->dot) L->dot = (rip_dot *)hal_big_alloc(sizeof(rip_dot) * RIP_MAXD);
    L->n = 0;
    if (!L->dot) return;
    float r[8], d[8], len[8];
    int g[8], live[8], gmax = 0;
    uint16_t col[8], gc[8][RIP_TAIL + 1];
    int nr = sp->rings < 8 ? sp->rings : 8;
    for (int i = 0; i < nr; i++) {
        float ph = t / sp->per - (float)i / (float)sp->rings, k;
        live[i] = ph >= 0;                         /* not emitted yet */
        if (!live[i]) continue;
        ph -= floorf(ph);
        r[i] = rip_radius(sp, ph, &k);
        d[i] = sp->d0 * (1.0f - 0.45f * ph);
        col[i] = sp->col(ph);
        len[i] = k * d[i];
        g[i] = len[i] > 1.0f ? (int)ceilf(len[i] / (0.45f * d[i])) : 0;
        if (g[i] > RIP_TAIL) g[i] = RIP_TAIL;
        if (g[i] > gmax) gmax = g[i];
        /* tail ink: 62 % next to the dot down to the AMOLED floor (UI_GHOST2) */
        for (int j = 1; j <= g[i]; j++) {
            float ink = 0.62f * (1.0f - (float)(j - 1) / (float)g[i]);
            gc[i][j] = ui_dim(col[i], ink > UI_GHOST2 ? ink : UI_GHOST2);
        }
    }
    /* the tails, farthest layer first (a nearer, brighter one draws over it), then dots */
    for (int j = gmax; j >= 0; j--)
        for (int i = 0; i < nr; i++) {
            if (!live[i] || j > g[i]) continue;
            float rr = j ? r[i] - len[i] * (float)j / (float)g[i] : r[i];
            float dd = j ? d[i] * (1.0f - 0.3f * (float)j / (float)g[i]) : d[i];
            uint16_t c = j ? gc[i][j] : col[i];
            for (int k = 0; k < sp->ndots && L->n < RIP_MAXD; k++) {
                float a = 6.2831853f * (float)k / (float)sp->ndots;
                rip_dot *o = &L->dot[L->n++];
                o->x = sp->cx + rr * gfx_fast_sin(a);
                o->y = sp->cy - rr * gfx_fast_cos(a);
                o->d = !sp->clear || sp->clear(o->x, o->y, dd * 0.5f) ? dd : 0;
                o->c = c;
            }
        }
}

/* Recorded per chunk of dots (box + hash), stamped only where they meet the stripe. */
static void rip_draw(gfx_fb *fb, const rip_list *L)
{
    int lo = fb->y_base - fb->oy, hi = lo + fb->h;
    for (int k0 = 0; k0 < L->n; k0 += RIP_CHUNK) {
        const rip_dot *o = &L->dot[k0];
        int n = L->n - k0 < RIP_CHUNK ? L->n - k0 : RIP_CHUNK;
        if (fb->rec) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            unsigned h = 0xF1D0u;
            for (int j = 0; j < n; j++) {
                if (o[j].d <= 0) continue;
                float e = o[j].d * 0.5f + 1.0f;
                if (o[j].x - e < x0) x0 = o[j].x - e;
                if (o[j].y - e < y0) y0 = o[j].y - e;
                if (o[j].x + e > x1) x1 = o[j].x + e;
                if (o[j].y + e > y1) y1 = o[j].y + e;
                h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(h, o[j].c), o[j].x), o[j].y), o[j].d);
            }
            if (x1 > x0) gfx_rec_cmd(fb, (int)floorf(x0), (int)floorf(y0), (int)ceilf(x1), (int)ceilf(y1), h);
            continue;
        }
        for (int j = 0; j < n; j++) {
            float e = o[j].d * 0.5f + 1.0f;
            if (o[j].d > 0 && o[j].y + e >= lo && o[j].y - e <= hi) dot_disc(fb, o[j].x, o[j].y, o[j].d, o[j].c);
        }
    }
}

/* ======================================================= find my phone ===== */
#define FB_CY     206
#define FB_R      72.0f
#define RIP_R0    (FB_R + 10.0f)
#define RIP_R1    120.0f          /* outer ring stays clear of the title */
#define RIP_N     3
#define RIP_PER   1.8f
#define RIP_DOTS  40
#define STATE_Y   346             /* [ RINGING ] */
#define HINT_Y    382             /* the hint keeps its place in both states */
#define SWAP_F    7               /* state words: old out over 3 frames, then new in over 4 */
#define HOLD_S    (2.5f / NOS_FPS)   /* a quick tap shows its pressed fill >= 3 frames */

/* A toggle's white disc grows from the centre (or folds back) over 6 whole frames, the
 * tap's own frame first; a near-linear table (22 43 62 79 92 100 %) keeps the 72 px edge
 * under the 16 px micro budget (the same table as Cellular's radio). */
static const mframes TOGGLE6 = { 6, { 22, 43, 62, 79, 92, 100 } };

static struct {
    nos_time_t t_enter;
    float build;           /* entry 0..1 */
    int   connected;
    int   active;          /* phone ringing (as shown) */
    nos_time_t t_active;        /* when ringing started */
    mtween fill;           /* white disc growth 0..1 */
    int   pressed;
    nos_time_t t_down;          /* press start (the fill holds >= 3 frames) */
    nos_time_t t_swap;          /* last ringing on/off: the state words change in sequence */
    nos_time_t err_t;           /* last refused tap (not connected) */
    float txt;             /* text alpha, quantised */
    pswap sw;              /* 1 = the find page, 0 = phone not connected */
    float wout, win;       /* state words: the old ones' ink, the new ones' ink */
    float rip0;            /* the newest ripple's radius last frame (the audit sees each emission) */
    rip_list rip;          /* this frame's ripple dots */
} F;

static const rip_spec FIND_RIP = { UI_CX, FB_CY, RIP_R0, RIP_R1, RIP_PER, 7.0f, RIP_N, RIP_DOTS, grey_step, 0 };

static void find_enter(void)
{
    F.t_enter = nos_enter_t();            /* build from 40% of the opening iris */
    F.build = 0;
    F.pressed = 0;
    F.t_down = -10;
    F.t_swap = -10;
    F.err_t = -10;
    F.active = svc_find_phone_active();
    F.t_active = nos_t;
    F.connected = svc_sys()->phone_connected || hal_phone_connected();
    ps_init(&F.sw, F.connected);
    mtw_start(&F.fill, F.active ? 1 : 0, F.active ? 1 : 0, M_FRAMES(1), &EASE_LINEAR);
    F.wout = 0; F.win = 1;
    F.rip0 = 0;
    F.rip.n = 0;
}

static void find_leave(void)
{
    if (F.active) { svc_find_phone(0); F.active = 0; }
}

static void set_active(int on)
{
    if (on == F.active) return;
    F.active = on;
    F.t_swap = nos_t;
    if (on) F.t_active = nos_t;
    float cur = mtw_value(&F.fill);
    mtw_start_tab(&F.fill, cur, on ? 1.0f : 0.0f, &TOGGLE6);
    mtw_lead(&F.fill);                    /* the tap's frame already moves */
}

static void find_tick(float dt)
{
    F.connected = svc_sys()->phone_connected || hal_phone_connected();
    if (F.active && !F.connected) { svc_find_phone(0); set_active(0); }
    if (ps_tick(&F.sw, F.connected)) F.t_enter = F.sw.t_in;    /* the other page builds now */
    F.build = q((nos_t - F.t_enter) / UI_ENTRY_S, 32);
    /* the phone may stop ringing on its own (found, or its timeout) */
    if (F.active && !svc_find_phone_active() && nos_t - F.t_active > 1.0f) set_active(0);
    F.txt = nos_t >= F.t_enter ? 1.0f : 0.0f;
    if (F.active) nos_keep_awake();
    /* the state words change in sequence on whole frames, the tap's frame first:
     * the old fade out (62 38 12 %), then the new fade in (12 38 62 100 %) */
    int k = m_frame_k(F.t_swap) + 1;
    F.wout = k < 4 ? m_ink(1.0f - (float)k / 4.0f) : 0.0f;
    F.win = k < 4 ? 0.0f : m_ink(m_clamp((float)(k - 3) / 4.0f, 0, 1));
    F.rip.n = 0;
    if (F.sw.shown && nos_t >= F.t_enter && (F.active || mtw_value(&F.fill) > 0.01f))
        rip_build(&F.rip, &FIND_RIP, nos_t - F.t_active);
#ifdef GFX_COST_MODEL
    nos_motion_probe("find.swap.ink", MC_FADE, 100.0f * ps_ink(&F.sw));
    if (F.sw.shown && nos_t >= F.t_enter) {
        float fillp = mtw_value(&F.fill);
        nos_motion_probe("find.fill.r", MC_MICRO, FB_R * fillp);
        float t = nos_t - F.t_active, ph = t / RIP_PER;
        if ((F.active || fillp > 0.01f) && ph >= 0) {
            float r = rip_radius(&FIND_RIP, ph - floorf(ph), 0);
            if (r < F.rip0) nos_motion_probe("find.ripple0.r", MC_DECORATIVE, r);   /* a new ripple */
            nos_motion_probe("find.ripple0.r", MC_DECORATIVE, r);
            F.rip0 = r;
        }
        if (k < SWAP_F) nos_motion_probe(k < 4 ? "find.words.out" : "find.words.in", MC_FADE, 100.0f * (k < 4 ? F.wout : F.win));
    }
#endif
}

static void find_page(gfx_fb *fb);

static void find_draw(gfx_fb *fb)
{
    ui_title(fb, "FIND PHONE", 0);
    int d0 = ps_dim(fb, &F.sw);
    find_page(fb);
    fb->dim = d0;
}

static void find_page(gfx_fb *fb)
{
    if (!F.sw.shown) {
        ui_absent(fb, DOT_ICON_BT, "Phone not connected", "Pair in Gadgetbridge as Bangle.js NOS", NULL, F.build);
        return;
    }
    if (nos_t < F.t_enter) return;
    float fillp = mtw_value(&F.fill);
    rip_draw(fb, &F.rip);
    /* the button is there at full size; its glyph builds; ringing fills it white */
    int pr = F.pressed || nos_t - F.t_down < HOLD_S;
    ui_circle(fb, UI_CX, FB_CY, FB_R, pr && fillp < 0.5f ? C_LINE2 : C_SURF2);
    float wr = FB_R * fillp;                     /* float radius: the AA edge moves evenly */
    if (wr > 1) ui_circle(fb, UI_CX, FB_CY, wr, pr && fillp >= 0.5f ? C_SEC : C_WHITE);
    uint16_t ink = fillp > 0.5f ? C_BG : C_PRI;
    dot_icon_anim(fb, DOT_ICON_PHONE, UI_CX, FB_CY, 7.0f, 5.6f, ink, F.build, DOT_REVEAL_CENTER);
    /* the hint stays where it is; the words change in sequence (old out, then new in) */
    float fout = F.wout, fin = F.win;
    const char *h_new = F.active ? "TAP TO STOP" : "TAP TO RING", *h_old = F.active ? "TAP TO RING" : "TAP TO STOP";
    if (fout <= 0 && fin >= 1) {
        if (F.active) ui_state(fb, UI_CX, STATE_Y, "RINGING", C_PRI);
        ui_label(fb, UI_CX, HINT_Y, h_new, C_SEC, UI_CENTER);
        return;
    }
    if (fout > 0) {
        if (!F.active) ui_state(fb, UI_CX, STATE_Y, "RINGING", ui_dim(C_PRI, fout));
        ui_label(fb, UI_CX, HINT_Y, h_old, ui_dim(C_SEC, fout), UI_CENTER);
    }
    if (fin > 0) {
        if (F.active) ui_state(fb, UI_CX, STATE_Y, "RINGING", ui_dim(C_PRI, fin));
        ui_label(fb, UI_CX, HINT_Y, h_new, ui_dim(C_SEC, fin), UI_CENTER);
    }
}

static int in_button(int x, int y)
{
    float dx = x - UI_CX, dy = y - FB_CY;
    return dx * dx + dy * dy <= (FB_R + 20) * (FB_R + 20);
}

static int find_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { F.pressed = 0; F.t_down = -10; return 1; }

    switch (in->type) {
    case IN_DOWN:
        F.pressed = F.connected && (in_button(in->x, in->y) || F.active);
        if (F.pressed) F.t_down = nos_t;
        return 1;
    case IN_TAP:
        F.pressed = 0;
        if (F.active) {
            svc_find_phone(0);
            set_active(0);
            nos_feedback(FB_TOGGLE_OFF);
        } else if (in_button(in->x, in->y)) {
            if (!F.connected) { nos_feedback(FB_ERROR); return 1; }
            svc_find_phone(1);
            set_active(1);
            nos_feedback(FB_TOGGLE_ON);
        }
        return 1;
    case IN_DRAG_START:
        F.t_down = -10;
        /* fallthrough */
    case IN_UP:
        F.pressed = 0;
        return 0;
    default:
        return 0;
    }
}

const nos_screen scr_find = { "FIND PHONE", find_enter, find_leave, find_tick, find_draw, find_input, 0 };

/* ====================================================== find my watch ===== */
#define FW_R      84.0f
#define FW_RINGS  4
#define FW_DOTS   56
#define FW_PER    1.2f
#define FW_BEAT_F 36           /* FW_PER in frames: the disc's pulse */
#define FW_R0     (FW_R + 14.0f)
#define FW_R1     224.0f
#define FW_D0     11.0f
#define FW_TITLE_Y 70          /* the words the ripples keep clear of (baselines) */
#define FW_HINT_Y  402

static struct {
    nos_time_t t_enter;
    float build;
    int   was_active;
    int   stopped;
    float pulse;           /* disc breathing 0..1 (whole frames, float radius) */
    rip_list rip;          /* this frame's ripple dots (d 0 = skipped) */
    int   tw, hw;          /* title / hint label widths */
} W;

/* A dot keeps out of a word's band: whole dots go, never a clipped half-dot. */
static int fw_clear(float x, float y, float r)
{
    const float cap = 13.0f, pad = 8.0f;      /* label cap height, air around the words */
    if (y + r > FW_TITLE_Y - cap - pad && y - r < FW_TITLE_Y + pad && fabsf(x - UI_CX) - r < W.tw * 0.5f + pad + 6)
        return 0;
    if (y + r > FW_HINT_Y - cap - pad && y - r < FW_HINT_Y + pad && fabsf(x - UI_CX) - r < W.hw * 0.5f + pad + 6)
        return 0;
    return 1;
}

static uint16_t fw_col(float ph)
{
    return ph < 0.45f ? C_WHITE : ph < 0.7f ? C_PRI : ph < 0.85f ? C_SEC : C_DIS;
}
static const rip_spec FW_RIP = { UI_CX, UI_CY, FW_R0, FW_R1, FW_PER, FW_D0, FW_RINGS, FW_DOTS, fw_col, fw_clear };

static void fw_restore(void)
{
    hal_haptic_stop();
    hal_sfx_stop();
    nos_brightness_override(-1);
}

static void fw_enter(void)
{
    W.t_enter = nos_t;
    W.build = 0;
    W.was_active = svc_find_watch_active();
    W.stopped = 0;
    W.tw = ui_label_width("FIND WATCH");
    W.hw = ui_label_width("TAP TO STOP");
    rip_build(&W.rip, &FW_RIP, 0);
    if (W.was_active) nos_brightness_override(255);
}

static void fw_leave(void)
{
    fw_restore();
}

static void fw_stop(void)
{
    if (W.stopped) return;
    W.stopped = 1;
    svc_find_watch_stop();
    fw_restore();
    nos_feedback(FB_SUCCESS);
    nos_pop();
}

static void fw_tick(float dt)
{
    nos_keep_awake();
    W.build = q((nos_t - W.t_enter) / UI_ENTRY_S, 32);
    if (W.stopped) return;
    if (svc_find_watch_active()) W.was_active = 1;
    else {
        if (!nos_transitioning() && nos_top() == &scr_find_watch) { W.stopped = 1; fw_restore(); nos_pop(); }
        return;
    }
    /* the disc swells as each ripple leaves it: up 8 px over 5 frames, back over 12,
     * at rest for the other 19 of the 36-frame (1.2 s) beat; a float radius, so the AA
     * edge moves evenly (R6) instead of 1 px hops in a 1-0-1-1-0 rhythm */
    int f = m_frame_k(W.t_enter) % FW_BEAT_F;
    W.pulse = f < 5 ? (float)(f + 1) / 5.0f : f < 17 ? 1.0f - (float)(f - 4) / 12.0f : 0.0f;
    rip_build(&W.rip, &FW_RIP, nos_t - W.t_enter);
#ifdef GFX_COST_MODEL
    nos_motion_probe("fw.disc.r", MC_DECORATIVE, FW_R + 8.0f * W.pulse);
    float rp = (nos_t - W.t_enter) / FW_PER, r = rip_radius(&FW_RIP, rp - floorf(rp), 0);
    static float s_r0;
    if (r < s_r0) nos_motion_probe("fw.ripple0.r", MC_DECORATIVE, r);       /* a new ripple */
    nos_motion_probe("fw.ripple0.r", MC_DECORATIVE, r);
    s_r0 = r;
#endif
}

static void fw_draw(gfx_fb *fb)
{
    rip_draw(fb, &W.rip);
    ui_circle(fb, UI_CX, UI_CY, FW_R + 8.0f * W.pulse, C_WHITE);
    dot_icon_anim(fb, DOT_ICON_BELL, UI_CX, UI_CY, 8.0f, 6.4f, C_BG, W.build, DOT_REVEAL_CENTER);
    /* the ripples leave whole-dot gaps around the words */
    ui_title(fb, "FIND WATCH", 0);
    ui_label(fb, UI_CX, FW_HINT_Y, "TAP TO STOP", C_PRI, UI_CENTER);
}

static int fw_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { /* A lost touch does not dismiss the alert. */ return 1; }

    switch (in->type) {
    case IN_TAP:
    case IN_BTN:
    case IN_LONG:
        fw_stop();
        return 1;
    case IN_DOWN: case IN_UP: case IN_DRAG_START: case IN_DRAG: case IN_DRAG_END:
        return 1;              /* nothing else to do here: swallow everything */
    default:
        return 0;
    }
}

const nos_screen scr_find_watch = { "FIND WATCH", fw_enter, fw_leave, fw_tick, fw_draw, fw_input,
                                    SCR_NOBACK | SCR_KEEPAWAKE };
