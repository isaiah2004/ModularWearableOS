/* sys_launcher.c — the app list: a rotary dial on the round glass.
 *
 * The row nearest the centre is the focus: full white name, the largest icon disc.
 * Rows further away dim (16 quantised levels) and slide right along the bezel curve,
 * so the column follows the glass like a dial. Scrolling is iOS momentum (0.998/ms
 * decay) whose resting point is moved to the nearest row at release, so a fling lands
 * exactly on a row (the last 3 px hand over to a critically damped spring; a slow
 * release that is a placement rather than a fling goes straight to the spring). Every row
 * crossing the centre gives a haptic tick. A tap opens the app through the iris from
 * its icon. The scroll position survives between openings.
 *
 * Cost (docs/FLAT60_DESIGN.md rule 1): each app's icon disc (three sizes) and name are
 * baked once into 8-bit PSRAM sprites. Every pixel in them is a neutral grey, so the
 * index IS the grey level and a 256-entry table turns it into RGB565: dimming a row,
 * or showing it pressed, is a different table, never a re-raster. Motion is only
 * opaque blits (~33 ns/px): ~6 rows = ~90k px = ~3 ms on the watch. Home bakes the
 * sprites one app per idle frame after boot (sys_launcher_prewarm). */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "sprite.h"
#include <math.h>
#include <string.h>

#define ROW_P     92.0f     /* row pitch */
#define ICON_X    96        /* focused icon centre x */
#define NAME_DX   58        /* name starts this far right of the icon centre */
#define CURVE_K   0.78f     /* how closely rows follow the bezel (1 = hug it) */
#define NVAR      3         /* icon disc sizes: far / near / focus */
#define ISPR      76        /* icon sprite side */
#define NLEV      16        /* dim levels */
#define MAXAPP    32
#define TAU_S     0.4995f   /* decay time constant of 0.998/ms */

static const float k_var_r[NVAR] = { 29.0f, 32.0f, 35.0f };

typedef struct {
    sprite8 icon[NVAR];
    sprite8 name;
    int name_w, name_h, name_base;
    int ready;
} row_spr;

static struct {
    row_spr rows[MAXAPP];
    uint16_t lut[NLEV + 1][256];      /* grey index -> RGB565 at level/NLEV brightness */
    uint16_t lut_press[256];          /* pressed: the disc lifts to C_SEC */
    uint16_t scratch[320 * 64];       /* RGB565 canvas sprites are drawn on */
    int16_t  curve[GFX_H + 2 * (int)ROW_P];   /* x shift by |row centre - CY| */
} *L;
static int s_baked, s_failed;

/* scroll state */
static struct {
    float x, v, target;
    int mode;                 /* 0 idle, 1 drag, 2 decay to target, 3 spring */
    mspring sp;
    float grab_x, grab_dy;
    int pressed, was_moving;
    int focus;
    float t_enter;
    int inited;
} S;
enum { M_IDLE = 0, M_DRAG, M_DECAY, M_SPRING };

static const nos_screen *s_last;
const nos_screen *sys_launcher_last(void) { return s_last; }

static int napps(void) { return nos_app_count < MAXAPP ? nos_app_count : MAXAPP; }
static float max_x(void) { return (napps() - 1) * ROW_P; }

/* ---- sprite baking ---------------------------------------------------------------- */
static uint16_t grey565(int v)
{
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    if (v == 255) return C_WHITE;
    int r = (v * 31 + 127) / 255;              /* neutral: G = 2R, B = R (no casts) */
    return (uint16_t)(r << 11 | (r * 2) << 5 | r);
}

static int ensure_tables(void)
{
    if (L) return 1;
    if (s_failed) return 0;
    L = hal_big_alloc(sizeof *L);
    if (!L) { s_failed = 1; return 0; }
    memset(L, 0, sizeof *L);
    for (int l = 0; l <= NLEV; l++)
        for (int i = 0; i < 256; i++) L->lut[l][i] = grey565((i * l + NLEV / 2) / NLEV);
    /* disc fill grey (C_SURF2 = 26) -> C_SEC (153), the house pressed fill; the white
     * ink above it scales into the rest and stays white */
    for (int i = 0; i < 256; i++)
        L->lut_press[i] = grey565(i <= 26 ? i * 153 / 26 : 153 + (i - 26) * (255 - 153) / (255 - 26));
    for (int y = 0; y < (int)(sizeof L->curve / sizeof L->curve[0]); y++) {
        float d = (float)y, r = (float)UI_R;
        L->curve[y] = (int16_t)(CURVE_K * (d < r ? r - sqrtf(r * r - d * d) : r) + 0.5f);
    }
    return 1;
}

/* RGB565 grey canvas -> grey index (green carries the most bits) */
static void to_index(sprite8 *dst, const uint16_t *src)
{
    int n = dst->w * dst->h;
    for (int i = 0; i < n; i++) {
        int g = (src[i] >> 5) & 63;
        dst->px[i] = (uint8_t)((g * 255 + 31) / 63);
    }
}

static int bake_app(int i)
{
    row_spr *R = &L->rows[i];
    const nos_app *a = &nos_app_list[i];
    sprite cv;
    cv.px = L->scratch;
    for (int v = 0; v < NVAR; v++) {
        if (!R->icon[v].px && !sprite8_alloc(&R->icon[v], ISPR, ISPR)) return 0;
        cv.w = cv.h = ISPR;
        gfx_fb fb = sprite_fb(&cv);
        memset(cv.px, 0, (size_t)ISPR * ISPR * 2);
        float c = ISPR * 0.5f, r = k_var_r[v];
        gfx_fill_circle(&fb, c, c, r, C_SURF2);
        float pitch = r * 0.118f;
        dot_icon(&fb, a->icon, c, c, pitch, pitch * 0.8f, C_WHITE);
        to_index(&R->icon[v], cv.px);
    }
    const font_t *f = UI_F_TITLE;
    int w = ui_text_width(f, a->name, 0) + 6;
    if (w > 320) w = 320;
    int h = f->line_height + 6;
    if (h > 64) h = 64;
    if (!R->name.px && !sprite8_alloc(&R->name, w, h)) return 0;
    cv.w = w; cv.h = h;
    gfx_fb fb = sprite_fb(&cv);
    memset(cv.px, 0, (size_t)w * h * 2);
    R->name_base = f->ascent + 3;
    ui_text(&fb, f, 2, R->name_base, a->name, C_WHITE, UI_LEFT, 0);
    to_index(&R->name, cv.px);
    R->name_w = w; R->name_h = h;
    R->ready = 1;
    return 1;
}

void sys_launcher_prewarm(void)
{
    if (s_baked >= napps() || !ensure_tables()) return;
    if (!bake_app(s_baked)) { s_failed = 1; s_baked = napps(); return; }
    s_baked++;
}

static void bake_all(void)
{
    while (s_baked < napps() && !s_failed) sys_launcher_prewarm();
}

/* ---- scrolling ---------------------------------------------------------------------- */
static float snap_row(float x) { return m_clamp(floorf(x / ROW_P + 0.5f) * ROW_P, 0, max_x()); }

static void spring_to(float target, float v)
{
    msp_init(&S.sp, S.x, SPRING_SNAPPY);
    float dist = target - S.x, w0 = 6.2831853f / S.sp.response;
    if (v * dist > 0 && fabsf(v) > w0 * fabsf(dist)) v = (dist > 0 ? 1.0f : -1.0f) * w0 * fabsf(dist);
    S.sp.v = v;
    msp_to(&S.sp, target);
    S.target = target;
    S.mode = M_SPRING;
}

static void release(float v)
{
    float mx = max_x();
    if (S.x < 0 || S.x > mx) {                /* back from the rubber band */
        msp_init(&S.sp, S.x, 0.40f, 1.0f);
        S.sp.v = v;
        msp_to(&S.sp, S.x < 0 ? 0 : mx);
        S.target = S.sp.target;
        S.mode = M_SPRING;
        return;
    }
    /* where the fling would rest, moved to the nearest row: the decay is re-aimed so
     * it lands exactly there (velocity changes by at most half a row / tau) */
    float tgt = snap_row(decay_project(S.x, v));
    S.target = tgt;
    S.v = (tgt - S.x) / TAU_S;
    S.mode = fabsf(tgt - S.x) < 0.5f ? M_IDLE : M_DECAY;
    if (S.mode == M_IDLE) S.x = tgt;
    else if (fabsf(tgt - S.x) < 3.0f || fabsf(v) < 250.0f) spring_to(tgt, v);   /* a placement, not a fling */
}

static void scroll_tick(float dt)
{
    if (S.mode == M_DECAY) {
        float e = expf(-dt / TAU_S);
        S.x = S.target - (S.target - S.x) * e;
        S.v = (S.target - S.x) / TAU_S;
        if (fabsf(S.target - S.x) < 3.0f) spring_to(S.target, S.v);   /* only the last px */
    }
    if (S.mode == M_SPRING) {
        int m = msp_step(&S.sp, dt);
        S.x = S.sp.x; S.v = S.sp.v;
        if (!m) { S.x = S.target; S.v = 0; S.mode = M_IDLE; }
    }
    int f = (int)floorf(S.x / ROW_P + 0.5f);
    if (f < 0) f = 0;
    if (f > napps() - 1) f = napps() - 1;
    if (f != S.focus) { S.focus = f; nos_feedback(FB_TICK); }
}

/* ---- screen ------------------------------------------------------------------------- */
static void l_enter(void)
{
    if (!S.inited) { memset(&S, 0, sizeof S); S.inited = 1; }
    ensure_tables();
    bake_all();
    S.x = snap_row(S.x);                       /* remembered between openings */
    S.mode = M_IDLE; S.v = 0;
    S.focus = (int)(S.x / ROW_P + 0.5f);
    S.pressed = -1;
    S.t_enter = nos_t;
}

static void l_leave(void) { S.pressed = -1; if (S.mode == M_DRAG) release(0); }

static void l_tick(float dt) { scroll_tick(dt); }

/* Layout of row i at the current scroll: centre y, icon x, dim level, icon variant.
 * Returns 0 if the row is off the glass. */
typedef struct { int cy, ix, lev, var; } row_geo;
static int geo(int i, float xr, row_geo *g)
{
    float cy = UI_CY + i * ROW_P - xr;
    if (cy < -ISPR || cy > GFX_H + ISPR) return 0;
    float d = fabsf(cy - UI_CY);
    int di = (int)d;
    if (di >= (int)(sizeof L->curve / sizeof L->curve[0])) di = (int)(sizeof L->curve / sizeof L->curve[0]) - 1;
    g->cy = (int)lroundf(cy);
    g->ix = ICON_X + L->curve[di];
    float u = d / ROW_P;
    g->var = u < 0.25f ? 2 : u < 0.5f ? 1 : 0;
    /* 1.0 at the focus, ~0.6 one row out, ~0.36 two rows out */
    float f = 1.0f - 0.40f * m_smoothstep(0.0f, 1.0f, u) - 0.26f * m_smoothstep(1.0f, 2.0f, u);
    /* build-in: rows arrive in waves outward from the focus, sliding in from the right */
    float e = (nos_t - S.t_enter - fabsf((float)i - xr / ROW_P) * (2.0f / NOS_FPS)) / 0.22f;
    e = bezier_ease(&EASE_DECEL, m_clamp(e, 0, 1));
    /* keep the whole name inside the glass: on outer rows the bezel chord is shorter,
     * so the row follows the curve less rather than cutting its name */
    if (L->rows[i].ready) {
        float ey = cy < UI_CY ? cy - 16 : cy + 16;           /* the name's outer edge */
        int lim = UI_CX + ui_half_width(ey) - 20 - NAME_DX - L->rows[i].name_w;
        if (g->ix > lim) g->ix = lim;
        if (g->ix < ICON_X) g->ix = ICON_X;
    }
    g->ix += (int)lroundf((1.0f - e) * 40.0f);
    f *= e;
    g->lev = (int)(f * NLEV + 0.5f);
    if (g->lev > NLEV) g->lev = NLEV;
    return g->lev > 0;
}

static void l_draw(gfx_fb *fb)
{
    if (!L) return;
    float xr = floorf(S.x + 0.5f);
    int n = napps(), c = (int)floorf(xr / ROW_P + 0.5f);
    /* header one row above the first app, scrolling with the list */
    float hy = UI_CY - ROW_P - xr;
    if (hy > -20) {
        float e = m_clamp((nos_t - S.t_enter) / 0.2f, 0, 1);
        float k = m_smoothstep(30.0f, 110.0f, hy) * e;
        k = floorf(k * 8.0f + 0.5f) / 8.0f;
        if (k > 0) ui_label(fb, UI_CX, (int)hy + 10, "APPS", ui_dim(C_SEC, k), UI_CENTER);
    }
    for (int i = c - 3; i <= c + 3; i++) {
        if (i < 0 || i >= n) continue;
        row_geo g;
        if (!geo(i, xr, &g)) continue;
        row_spr *R = &L->rows[i];
        if (!R->ready) {                        /* sprites unavailable: draw live */
            float r = k_var_r[g.var];
            ui_circle(fb, (float)g.ix, (float)g.cy, r, grey565(26 * g.lev / NLEV));
            dot_icon(fb, nos_app_list[i].icon, (float)g.ix, (float)g.cy, r * 0.118f, r * 0.094f,
                     grey565(255 * g.lev / NLEV));
            ui_text(fb, UI_F_TITLE, g.ix + NAME_DX, g.cy + UI_F_TITLE->ascent / 2 - 2, nos_app_list[i].name,
                    grey565(255 * g.lev / NLEV), UI_LEFT, 0);
            continue;
        }
        int pressed = i == S.pressed;
        const uint16_t *lut = pressed ? L->lut_press : L->lut[g.lev];
        unsigned key = (unsigned)(i * 64 + g.var * 20 + g.lev) | (pressed ? 0x8000u : 0u);
        sprite8_blit(fb, &R->icon[g.var], g.ix - ISPR / 2, g.cy - ISPR / 2, lut, key);
        /* the name's x-height centre sits on the icon centre */
        int ny = g.cy - R->name_base + UI_F_TITLE->ascent / 2 - 1;
        sprite8_blit(fb, &R->name, g.ix + NAME_DX, ny, L->lut[g.lev], key ^ 0x4000u);
    }
}

static int row_at(int y)
{
    float xr = floorf(S.x + 0.5f);
    int i = (int)floorf((y - UI_CY + xr) / ROW_P + 0.5f);
    if (i < 0 || i >= napps()) return -1;
    float cy = UI_CY + i * ROW_P - xr;
    return fabsf(y - cy) <= ROW_P * 0.5f ? i : -1;
}

static int l_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        S.was_moving = S.mode != M_IDLE && fabsf(S.v) > 60.0f;
        S.mode = M_DRAG;                       /* the finger holds the list */
        S.grab_x = S.x;
        S.grab_dy = 0;
        S.pressed = S.was_moving ? -1 : row_at(in->y);
        return 1;
    case IN_DRAG_START:
        S.pressed = -1;
        if (in->axis != AXIS_Y) { release(0); return 0; }
        S.grab_x = S.x;
        S.grab_dy = in->dy;                    /* absorb the slop: no jump when it locks */
        return 1;
    case IN_DRAG: {
        if (S.mode != M_DRAG) return 0;
        float raw = S.grab_x - (in->dy - S.grab_dy), mx = max_x();
        if (raw < 0) raw = rubber_band(raw, (float)GFX_H);
        else if (raw > mx) raw = mx + rubber_band(raw - mx, (float)GFX_H);
        S.x = raw;
        return 1;
    }
    case IN_DRAG_END:
        if (S.mode != M_DRAG) return 0;
        release(-in->vy);
        return 1;
    case IN_TAP: {
        int was = S.was_moving, i = row_at(in->y);
        S.pressed = -1;
        if (S.mode == M_DRAG) release(0);
        if (was || i < 0) return 1;
        row_geo g;
        float xr = floorf(S.x + 0.5f);
        int ix = L && geo(i, xr, &g) ? g.ix : ICON_X, iy = (int)lroundf(UI_CY + i * ROW_P - xr);
        s_last = nos_app_list[i].scr;
        if (L && geo(i, xr, &g))
            nos_open_icon(nos_app_list[i].scr, ix, iy, nos_app_list[i].icon, k_var_r[g.var]);
        else nos_open(nos_app_list[i].scr, ix, iy);
        return 1;
    }
    case IN_UP:
        S.pressed = -1;
        if (S.mode == M_DRAG) release(0);
        return 1;
    case IN_BTN_DOUBLE: {                  /* button twice from the face: the last app */
        const nos_screen *last = sys_launcher_last();
        if (!last) return 0;
        nos_iris_x = GFX_W; nos_iris_y = UI_CY;
        nos_feedback(FB_OPEN);
        nos_replace(last, TX_IRIS);
        return 1;
    }
    default:
        return 0;
    }
}

const nos_screen scr_launcher = { "LAUNCHER", l_enter, l_leave, l_tick, l_draw, l_input, 0 };
