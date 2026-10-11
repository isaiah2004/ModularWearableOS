/* face_segment.c — SEGMENT: activity rings in discrete segments.
 *
 * Three concentric rings of 60 square-ended segments (6 px thick, 3 px parallel
 * gaps) at r = 215 / 196 / 177: steps, kcal and active minutes against their goals.
 * Lit segments white, empty #2A2A2A; past its goal the steps ring continues in red
 * (the other rings stay full white, so there is one red element). In the centre the
 * time in the grotesk hero face at ~74 px cap, the date (or the battery while low /
 * charging) above, and the three values below with mono caps labels.
 * Build-in: from the fill shown last time to now (never a stale replay). Wake from
 * AOD (6 frames): the step dots grow and the outlined time fills in, then the rings,
 * values and labels fade up over the last 3 frames (no frame adds over ~25 % light).
 *
 * The rings are cached: they live in four quadrant sprites (PSRAM) and a segment is
 * re-rasterised into its quadrant only when its colour changes (a few per hour), so
 * a full redraw (pager slide) is four blits, and the build-in — segments lighting one
 * by one clockwise — costs a handful of segment rasters per frame plus the blit of the
 * quadrant that changed. Without PSRAM it falls back to drawing segments directly.
 * Tap anywhere on the rings or values: the Activity app. The time is nf_hero scaled to
 * 0.66 once into PSRAM masks (fk_sfont); changed digits roll at the minute. */
#include "faces/face_kit.h"
#include "nos.h"
#include "svc.h"
#include "sprite.h"
#include "gfx_cost.h"
#include "apps/nos_apps.h"
#include <stdio.h>
#include <string.h>

#define NSEG     60
#define NRING    3
#define SEG_TH   6.0f
#define SEG_GAP  3.0f
#define Q        233                    /* quadrant size */
static const float k_r[NRING] = { 215.0f, 196.0f, 177.0f };   /* ring mid radii */

#define COL_OFF  C_SURF3
#define RASTER_BUDGET 36                 /* segment rasters per frame (~0.15 ms each) */

typedef struct {
    sprite   q[4];
    unsigned key[4];
    uint16_t drawn[NRING][NSEG];          /* colour currently in the sprite, 0 = none */
    int16_t  span[GFX_H][2];              /* per row: the ring band's left span [x0, x1) */
    int      ok;
} seg_cache;

static seg_cache *G;
static int   s_alloc_failed;
static float s_shown[NRING];              /* displayed fill in segments (0..120) */
static float s_target[NRING];
static int   s_val[NRING];                /* steps, kcal, active minutes */

/* ---- one segment, rasterised exactly (coverage over black) ---- */
typedef struct { float ri, ro, nx0, ny0, nx1, ny1; } seg_geo;

static void seg_geom(int ring, int i, seg_geo *g)
{
    float a0 = (float)i * (6.2831853f / NSEG), a1 = (float)(i + 1) * (6.2831853f / NSEG);
    g->ri = k_r[ring] - SEG_TH * 0.5f;
    g->ro = k_r[ring] + SEG_TH * 0.5f;
    /* boundary lines through the centre at a0 / a1; inward normals (into the segment) */
    g->nx0 = fk_cos(a0); g->ny0 = fk_sin(a0);       /* clockwise side of a0 */
    g->nx1 = -fk_cos(a1); g->ny1 = -fk_sin(a1);
}

static uint16_t s_red;                      /* C_RED, easing to C_PRI while recording (tick) */

static uint16_t seg_color(int ring, int i, float fill)
{
    /* a second lap continues in red on the steps ring only (one red element); the
     * other rings simply stay full white past their goal */
    if (ring == 0 && fill >= (float)NSEG + (float)i + 1.0f) return s_red;
    if (fill >= (float)i + 1.0f) return C_WHITE;
    return COL_OFF;
}

/* raster the segment into a 16-bit buffer (stride, origin ox/oy in screen coords) */
static void seg_raster(uint16_t *px, int stride, int ox, int oy, int w, int h,
                       int ring, int i, uint16_t col)
{
    seg_geo g;
    seg_geom(ring, i, &g);
    float am = ((float)i + 0.5f) * (6.2831853f / NSEG);
    float hw = 3.1415927f / NSEG + 0.02f;
    /* bbox from the four corners and the outer mid point */
    float xs[5], ys[5];
    float aa[3] = { am - hw, am, am + hw };
    int n = 0;
    for (int k = 0; k < 3; k++) {
        xs[n] = UI_CX + fk_sin(aa[k]) * (g.ro + 1); ys[n] = UI_CY - fk_cos(aa[k]) * (g.ro + 1); n++;
        if (k != 1) { xs[n] = UI_CX + fk_sin(aa[k]) * (g.ri - 1); ys[n] = UI_CY - fk_cos(aa[k]) * (g.ri - 1); n++; }
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int k = 0; k < n; k++) {
        if (xs[k] < x0) x0 = xs[k];
        if (xs[k] > x1) x1 = xs[k];
        if (ys[k] < y0) y0 = ys[k];
        if (ys[k] > y1) y1 = ys[k];
    }
    int ix0 = (int)x0 - 1, iy0 = (int)y0 - 1, ix1 = (int)x1 + 2, iy1 = (int)y1 + 2;
    if (ix0 < ox) ix0 = ox;
    if (iy0 < oy) iy0 = oy;
    if (ix1 > ox + w) ix1 = ox + w;
    if (iy1 > oy + h) iy1 = oy + h;
    for (int y = iy0; y < iy1; y++) {
        uint16_t *row = px + (y - oy) * stride - ox;
        float dy = (float)y + 0.5f - UI_CY;
        for (int x = ix0; x < ix1; x++) {
            float dx = (float)x + 0.5f - UI_CX;
            /* screen y grows down: clock angle a has direction (sin a, -cos a); the
             * clockwise normal of that line is (cos a, sin a) */
            float e0 = dx * g.nx0 + dy * g.ny0 - SEG_GAP * 0.5f;
            float e1 = dx * g.nx1 + dy * g.ny1 - SEG_GAP * 0.5f;
            if (e0 < -1.0f || e1 < -1.0f) continue;
            float r = gfx_fast_sqrt(dx * dx + dy * dy);
            float m = r - g.ri;
            if (g.ro - r < m) m = g.ro - r;
            if (e0 < m) m = e0;
            if (e1 < m) m = e1;
            float cov = m + 0.5f;
            if (cov <= 0.0f) continue;
            row[x] = cov >= 1.0f ? col : gfx_blend565(0, col, (uint8_t)(cov * 255.0f));
        }
    }
}

static int quad_of(int i) { return i < 15 ? 1 : i < 30 ? 3 : i < 45 ? 2 : 0; }
static int qx(int q) { return (q & 1) ? Q : 0; }
static int qy(int q) { return (q & 2) ? Q : 0; }

static void cache_init(void)
{
    if (G || s_alloc_failed) return;
    G = (seg_cache *)hal_big_alloc(sizeof *G);
    if (!G) { s_alloc_failed = 1; return; }
    memset(G, 0, sizeof *G);
    for (int q = 0; q < 4; q++)
        if (!sprite_alloc(&G->q[q], q & 1 ? GFX_W - Q : Q, q & 2 ? GFX_H - Q : Q)) { s_alloc_failed = 1; G = 0; return; }
    for (int q = 0; q < 4; q++) {
        memset(G->q[q].px, 0, (size_t)G->q[q].w * (size_t)G->q[q].h * 2);
        G->key[q] = 0x5E600000u + (unsigned)q * 4096u;
    }
    /* the band the segments can touch (2 px clear of their AA), as spans per row: the
     * sprites are black everywhere else, so the blit copies only these */
    const float ri = k_r[NRING - 1] - SEG_TH * 0.5f - 2.0f, ro = k_r[0] + SEG_TH * 0.5f + 2.0f;
    for (int y = 0; y < GFX_H; y++) {
        float dy = (float)y + 0.5f - UI_CY, ady = dy < 0 ? -dy : dy;
        int x0 = 0, x1 = 0;
        if (ady < ro) {
            x0 = (int)(UI_CX - gfx_fast_sqrt(ro * ro - dy * dy)) - 1;
            x1 = ady < ri ? (int)(UI_CX - gfx_fast_sqrt(ri * ri - dy * dy)) + 2 : UI_CX;
            if (x0 < 0) x0 = 0;
            if (x1 > UI_CX) x1 = UI_CX;
        }
        G->span[y][0] = (int16_t)x0; G->span[y][1] = (int16_t)x1;
    }
    G->ok = 1;
}

/* bring the sprites up to date with the displayed fills */
static void cache_update(void)
{
    if (!G || !G->ok) return;
    int budget = RASTER_BUDGET;
    for (int i = 0; i < NSEG; i++)                 /* clockwise, all rings together */
        for (int r = 0; r < NRING; r++) {
            uint16_t col = seg_color(r, i, s_shown[r]);
            if (G->drawn[r][i] == col) continue;
            if (budget-- <= 0) return;
            int q = quad_of(i);
            sprite *s = &G->q[q];
            seg_raster(s->px, s->w, qx(q), qy(q), s->w, s->h, r, i, col);
            G->drawn[r][i] = col;
            G->key[q]++;
        }
}

static fk_sfont s_font;                     /* nf_hero at 0.66: an ~74 px cap time */
static fk_wake  s_w;
static fk_morph s_tm;
static fk_xfade s_date;
static int      s_cold = 1;                 /* sprites hold nothing yet */
static float    s_from[NRING];

#define T_SCALE 0.66f
#define Y_T     (UI_CY + 21)                /* time baseline: the date-time-values group centres on UI_CY */
#define Y_DATE  (Y_T - 74 - 24)
#define Y_VAL   (Y_T + 46)
#define Y_LAB   (Y_T + 70)

/* the empty track, `n` segments on from where it got to; done = sprites ready */
static int s_cold_i;
#define WARM_SEGS 4
static void cold_track(int n)
{
    for (; n > 0 && s_cold_i < NRING * NSEG; n--, s_cold_i++) {
        int r = s_cold_i / NSEG, i = s_cold_i % NSEG, q = quad_of(i);
        seg_raster(G->q[q].px, G->q[q].w, qx(q), qy(q), G->q[q].w, G->q[q].h, r, i, COL_OFF);
        G->drawn[r][i] = COL_OFF;
    }
    if (s_cold_i >= NRING * NSEG) {
        for (int q = 0; q < 4; q++) G->key[q] += 97;
        s_cold = 0;
    }
}

static void enter(void)
{
    /* the sprites still show the last visit's fill: animate from there, never from 0
     * over stale arcs ("what you walked since you last looked") */
    for (int r = 0; r < NRING; r++) s_from[r] = s_shown[r];
    s_tm.init = 0;
}

static void tick(const face_ctx *c, float dt)
{
    s_red = fk_red(c, C_PRI);
    fk_aod_update(c);
    fk_wake_tick(&s_w, c);
    fk_unread_tick(c);
    cache_init();
    fk_sfont_build(&s_font, UI_F_HERO, T_SCALE);
    int stale = fk_wake_stale(&s_w);
    char tb[8], db[16];
    fk_time(tb, c->dt.hour, c->dt.minute);
    fk_morph_tick(&s_tm, tb, c->t);
    if (stale) fk_morph_settle(&s_tm);
    float e = fk_build(&s_w, c);
    fk_date(db, c);
    fk_xfade_tick(&s_date, db, c->t, c->aod || stale || e < 1.0f);
    const svc_activity_t *a = svc_activity();
    int steps = c->steps > 0 ? c->steps : a->steps;
    int goal = c->step_goal > 0 ? c->step_goal : a->goal > 0 ? a->goal : 8000;
    float kcal = a->kcal > 0 ? a->kcal : (float)steps * 0.04f;
    int act = a->active_min > 0 ? a->active_min : steps / 110;
    s_val[0] = steps; s_val[1] = (int)(kcal + 0.5f); s_val[2] = act;
    float goals[3] = { (float)goal, 400.0f, 30.0f };
    float vals[3] = { (float)steps, kcal, (float)act };
    for (int r = 0; r < NRING; r++) {
        float f = vals[r] / goals[r] * NSEG;
        float cap = r == 0 ? 2.0f * NSEG : (float)NSEG;
        if (f > cap) f = cap;
        s_target[r] = (float)(int)f;                          /* whole segments */
    }
    if (G && G->ok && s_cold) {
        /* first use: the rest of the empty track (the warm-up may have done most of
         * it), once, before anything is blitted */
        for (int r = 0; r < NRING; r++) s_shown[r] = s_from[r] = 0;
        cold_track(NRING * NSEG);
    }
    /* AOD shows the steps as dots at their target, so the rings wake at the fill on
     * screen; a face that was off the glass shows its fill as it is now */
    if (c->aod || (stale && e >= 1.0f))
        for (int r = 0; r < NRING; r++) s_shown[r] = s_from[r] = s_target[r];
    if (c->aod) { cache_update(); return; }
    for (int r = 0; r < NRING; r++) {
        if (e < 1.0f) {
            float q = fk_ease(m_clamp((e - 0.08f * (float)r) / 0.75f, 0, 1));
            s_shown[r] = (float)(int)(s_from[r] + (s_target[r] - s_from[r]) * q + 0.5f);
        } else if (s_shown[r] != s_target[r]) {               /* live changes land within 0.4 s */
            float d = s_target[r] - s_shown[r];
            float ad = d < 0 ? -d : d;
            float step = (float)(int)(ad / 12.0f + 0.999f);
            if (step < 1.0f) step = 1.0f;
            if (step > ad) step = ad;
            s_shown[r] += d > 0 ? step : -step;
        }
        s_from[r] = e < 1.0f ? s_from[r] : s_shown[r];
    }
    cache_update();
}

/* background warm-up, a little each frame from whichever face is on (faces.c), so that
 * SEGMENT's first frame (it slides in on the picker or the pager) builds nothing: the
 * scaled font a glyph a frame, then the ring sprites, then their empty track */
void face_segment_warm(void)
{
    if (!s_cold || s_alloc_failed) return;
    if (!s_font.ok && !s_font.failed) { fk_sfont_build_step(&s_font, UI_F_HERO, T_SCALE); return; }
    if (!G) { cache_init(); return; }
    if (G->ok) cold_track(WARM_SEGS);
}

static void fmt_k(char *b, int n, int v)
{
    if (v >= 10000) snprintf(b, (size_t)n, "%d.%dK", v / 1000, (v % 1000) / 100);
    else snprintf(b, (size_t)n, "%d", v);
}

static int red_used(void) { return s_shown[0] > NSEG; }

/* the time in the scaled hero face; changed digits roll (k < 1: outline filling in) */
static void draw_time(gfx_fb *fb, const face_ctx *c, float k, float dx, float dy)
{
    if (!s_font.ok) return;
    int w = fk_stext_width(&s_font, s_tm.to);
    int x = UI_CX - w / 2 + (int)dx, y = Y_T + (int)dy;
    if (k < 1.0f) {
        static const signed char o[4][2] = { {-1,0},{1,0},{0,-1},{0,1} };
        uint16_t line = ui_mix(C_SEC, C_WHITE, k);
        for (int j = 0; j < 4; j++) fk_stext(fb, &s_font, x + o[j][0], y + o[j][1], s_tm.to, line, UI_LEFT);
        fk_stext(fb, &s_font, x, y, s_tm.to, ui_mix(C_BG, C_WHITE, k), UI_LEFT);
        return;
    }
    float p = fk_morph_p(&s_tm, c->t, 0.30f);
    if (p >= 1.0f || strlen(s_tm.from) != strlen(s_tm.to)) {
        fk_stext(fb, &s_font, x, y, s_tm.to, C_WHITE, UI_LEFT);
        return;
    }
    for (int i = 0; s_tm.to[i]; i++) {
        char one[2] = { s_tm.to[i], 0 };
        int cw = fk_stext_width(&s_font, one);
        fk_roll(fb, 0, &s_font, x, y, cw, s_font.cap, s_tm.from[i], s_tm.to[i], p, C_WHITE);
        x += cw;
    }
}

/* AOD step dots: one per lit segment, centred on it (so they sink into the white
 * segments as the rings fade in on a wake); the second lap reddens with the rings */
typedef struct { float dx, dy, d; int lit, lap2; uint16_t on, red; } aod_ctx;
static void step_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const aod_ctx *A = (const aod_ctx *)vc;
    if (i >= A->lit) { *d = 0; *col = 0; return; }
    float a = ((float)i + 0.5f) * (6.2831853f / NSEG);
    *x = UI_CX + A->dx + fk_sin(a) * k_r[0];
    *y = UI_CY + A->dy - fk_cos(a) * k_r[0];
    *d = A->d;
    *col = i < A->lap2 ? A->red : A->on;
}

/* the value columns, nudged so the values and labels rows balance together (a wide
 * step count on the left would otherwise pull the row left); counted in digit cells
 * so it only moves when a value gains or loses a digit */
static int values_shift(char v[3][16])
{
    static const char *lab[3] = { "STP", "KCAL", "MIN" };
    const int xs[3] = { UI_CX - 88, UI_CX, UI_CX + 80 };
    int dw = ui_text_width(UI_F_BODY, "0", 0);
    float vl = (float)xs[0] - (float)((int)strlen(v[0]) * dw) * 0.5f;
    float vr = (float)xs[2] + (float)((int)strlen(v[2]) * dw) * 0.5f;
    float ll = (float)xs[0] - (float)ui_text_width(UI_F_LABEL, lab[0], 2) * 0.5f;
    float lr = (float)xs[2] + (float)ui_text_width(UI_F_LABEL, lab[2], 2) * 0.5f;
    float mid = ((vl + vr) * 0.5f + (ll + lr) * 0.5f) * 0.5f;
    return (int)((float)UI_CX - mid + (mid < UI_CX ? 0.5f : -0.5f));
}

static void draw_values(gfx_fb *fb, float a)
{
    static const char *lab[3] = { "STP", "KCAL", "MIN" };
    char v[3][16];
    for (int i = 0; i < 3; i++) fmt_k(v[i], sizeof v[i], s_val[i]);
    int sh = values_shift(v);
    const int xs[3] = { UI_CX - 88 + sh, UI_CX + sh, UI_CX + 80 + sh };
    for (int i = 0; i < 3; i++) {
        ui_text(fb, UI_F_BODY, xs[i], Y_VAL, v[i], ui_dim(C_PRI, a), UI_CENTER, 0);
        ui_label(fb, xs[i], Y_LAB, lab[i], ui_dim(C_DIS, a), UI_CENTER);
    }
}

/* sprite_blit for a ring quadrant, copying only the ring band's spans (the rest of the
 * sprite is black over a cleared frame): ~3.7x fewer pixels, which matters most under
 * an ink dim (wake, picker), where every pixel goes through gfx_ink */
static void blit_quad(gfx_fb *fb, const sprite *s, int x, int y, unsigned key)
{
    if (fb->rec || !s->px) { sprite_blit(fb, s, x, y, key); return; }
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    unsigned n = 0;
    for (int r = 0; r < s->h; r++) {
        int ly = y + r, by = ly + fb->oy - fb->y_base;
        if ((unsigned)by >= (unsigned)fb->h || ly < 0 || ly >= GFX_H) continue;
        int a0 = G->span[ly][0], a1 = G->span[ly][1];
        if (a1 <= a0) continue;
        int sp[2][2] = { { a0, a1 }, { GFX_W - a1, GFX_W - a0 } };
        uint16_t *row = gfx_row(fb, by);
        const uint16_t *src = &s->px[r * s->w];
        for (int k = 0; k < 2; k++) {
            int l0 = sp[k][0] > x ? sp[k][0] : x, l1 = sp[k][1] < x + s->w ? sp[k][1] : x + s->w;
            if (l0 + fb->ox < c0) l0 = c0 - fb->ox;
            if (l1 + fb->ox > c1) l1 = c1 - fb->ox;
            if (l1 <= l0) continue;
            uint16_t *d = row + l0 + fb->ox;
            const uint16_t *p = src + (l0 - x);
            int m = l1 - l0;
            if (fb->dim) for (int i = 0; i < m; i++) d[i] = gfx_ink(fb, p[i]);
            else memcpy(d, p, (size_t)m * 2);
            n += (unsigned)m;
        }
    }
    GFX_COST(GFX_COST_BLIT, n);
}

static void blit_rings(gfx_fb *fb, float a)
{
    if (G && G->ok) {
        gfx_fb d = *fb;
        if (a < 1.0f) nos_ink_dim(&d, a);
        unsigned dk = (unsigned)d.dim * 0x9E37u;            /* the dim is part of the look */
        for (int q = 0; q < 4; q++) blit_quad(&d, &G->q[q], qx(q), qy(q), G->key[q] + dk);
        return;
    }
    /* no PSRAM: segments as dots (cheap, still discrete) */
    for (int r = 0; r < NRING; r++) {
        float sh = s_shown[r];
        float lap1 = sh > NSEG ? NSEG : sh;
        fk_ring(fb, UI_CX, UI_CY, k_r[r], NSEG, 6.0f, (int)lap1, ui_dim(C_WHITE, a), ui_dim(COL_OFF, a));
        if (sh > NSEG) fk_ring(fb, UI_CX, UI_CY, k_r[r], NSEG, 6.0f, (int)(sh - NSEG), ui_dim(s_red, a), 0);
    }
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    char b[24];
    float dx, dy;
    fk_wake_off(&s_w, c, &dx, &dy);
    float k = fk_wake_k(&s_w, c);
    float e = fk_build(&s_w, c);
    uint16_t unread_rest = red_used() ? C_PRI : fk_red(c, C_PRI);
    if (c->aod || k < 1.0f) {
        /* AOD: the steps ring as small dots for the lit segments only, time outlined.
         * Wake (6 frames): the dots grow and the time fills in; from the half-way frame
         * the rings, values and labels fade up in 3 even steps while "STEPS" has gone,
         * so no single frame adds more than about a quarter of the light */
        float kr = c->aod ? 0.0f : m_clamp((k - 0.5f) / 0.5f, 0, 1);
        float kd = m_clamp(k / 0.5f, 0, 1);
        if (kr > 0.0f) blit_rings(fb, kr);
        int lit = (int)s_target[0];
        if (lit > NSEG) lit = NSEG;
        aod_ctx A = { dx, dy, 3.0f + 3.0f * kd, lit, kr > 0.0f ? (int)s_target[0] - NSEG : 0,
                      ui_mix(C_DIS, C_WHITE, kd), ui_mix(C_WHITE, s_red, kr) };
        if (lit > 0) fk_dots(fb, NSEG, 8, step_dot, &A);
        draw_time(fb, c, k, dx, dy);
        /* the date row: the battery holds it in AOD only when critical, awake from 20 % */
        face_ctx ca = *c; ca.aod = 1;
        face_ctx cw = *c; cw.aod = 0;
        int st_a = fk_status_shown(&ca), st_w = fk_status_shown(&cw);
        int xd = UI_CX + (int)dx, yd = Y_DATE + (int)dy;
        uint16_t lc = ui_mix(C_DIS, C_SEC, k);
        if (c->aod || st_a == st_w) {
            if (st_a) fk_status(fb, c->aod ? c : &cw, xd, yd, lc, UI_CENTER);
            else ui_label(fb, xd, yd, s_date.to, lc, UI_CENTER);
        } else if (k < 0.5f) {
            float o = 1.0f - k / 0.5f;
            if (st_a) fk_status(fb, &ca, xd, yd, ui_dim(C_DIS, o), UI_CENTER);
            else ui_label(fb, xd, yd, s_date.to, ui_dim(C_DIS, o), UI_CENTER);
        } else {
            if (st_w) fk_status(fb, &cw, xd, yd, ui_dim(C_SEC, kr), UI_CENTER);
            else ui_label(fb, xd, yd, s_date.to, ui_dim(C_SEC, kr), UI_CENTER);
        }
        /* "2871 STEPS" leaves over the first half; the values row arrives with the rings */
        if (k < 0.5f) {
            char v[16]; fmt_k(v, sizeof v, s_val[0]);
            snprintf(b, sizeof b, "%s STEPS", v);
            ui_label(fb, UI_CX + (int)dx, Y_VAL + (int)dy, b, ui_dim(C_DIS, 1.0f - k / 0.5f), UI_CENTER);
        }
        if (kr > 0.0f) draw_values(fb, kr);
        fk_unread(fb, c, ui_mix(C_SEC, unread_rest, k), C_BG, dx, dy + 74.0f);
        return;
    }
    blit_rings(fb, 1.0f);
    float la = e >= 1.0f ? 1.0f : (float)(int)(fk_ease(e * 1.4f) * 4.0f) / 4.0f;
    draw_time(fb, c, 1.0f, 0, 0);
    /* the date row; the battery takes it over while low or charging */
    if (fk_status_shown(c)) fk_status(fb, c, UI_CX, Y_DATE, ui_dim(C_SEC, la), UI_CENTER);
    else fk_xfade_label(fb, &s_date, c->t, UI_CX, Y_DATE, ui_dim(C_SEC, la), UI_CENTER);
    draw_values(fb, la);
    fk_unread(fb, c, unread_rest, C_BG, 0, 74.0f);       /* y 98: inside the rings */
}

/* press rect of the complication under (x, y). The rings light a square that hugs the
 * three tracks where the finger is (home lightens it under the segments). */
const struct nos_screen *face_segment_hit(int x, int y, int r[4])
{
    float dx = (float)x - UI_CX, dy = (float)y - UI_CY, d2 = dx * dx + dy * dy;
    if (d2 >= 160.0f * 160.0f) {
        if (r) {
            float d = gfx_fast_sqrt(d2), rm = (k_r[0] + k_r[2]) * 0.5f;
            int px = (int)(UI_CX + dx / d * rm), py = (int)(UI_CY + dy / d * rm);
            r[0] = px - 30; r[1] = py - 30; r[2] = px + 30; r[3] = py + 30;
        }
        return &scr_activity;
    }
    if (y > Y_VAL - 30 && y < Y_LAB + 16) {
        if (r) { r[0] = UI_CX - 140; r[1] = Y_VAL - 30; r[2] = UI_CX + 140; r[3] = Y_LAB + 14; }
        return &scr_activity;
    }
    return 0;
}

static const struct nos_screen *tap(int x, int y) { return face_segment_hit(x, y, 0); }

const nos_face face_segment = { "SEGMENT", enter, tick, draw, tap };
