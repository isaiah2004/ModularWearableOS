/* face_segment.c — SEGMENT: activity rings in discrete segments.
 *
 * Three concentric rings of 60 square-ended segments (6 px thick, 3 px parallel
 * gaps) at r = 215 / 196 / 177: steps, kcal and active minutes against their goals.
 * Lit segments white, empty #222, a second lap past the goal continues in red. In
 * the centre the time in the grotesk display face, the date above, and the three
 * values below as mono caps.
 *
 * The rings are cached: they live in four quadrant sprites (PSRAM) and a segment is
 * re-rasterised into its quadrant only when its colour changes (a few per hour), so
 * a full redraw (pager slide) is four blits, and the build-in — segments lighting one
 * by one clockwise — costs a handful of segment rasters per frame plus the blit of the
 * quadrant that changed. Without PSRAM it falls back to drawing segments directly.
 * Tap anywhere on the rings or values: the Activity app. */
#include "faces/face_kit.h"
#include "nos.h"
#include "svc.h"
#include "sprite.h"
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

static uint16_t seg_color(int ring, int i, float fill)
{
    if (fill >= (float)NSEG + (float)i + 1.0f) return C_RED;   /* second lap */
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

static void enter(void) { for (int r = 0; r < NRING; r++) s_shown[r] = 0; }

static void tick(const face_ctx *c, float dt)
{
    fk_aod_update(c);
    cache_init();
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
        if (f > 2 * NSEG) f = 2 * NSEG;
        s_target[r] = (float)(int)f;                          /* whole segments */
    }
    if (c->aod) return;
    float e = c->enter;
    for (int r = 0; r < NRING; r++) {
        if (e < 1.0f) s_shown[r] = (float)(int)(s_target[r] * fk_ease(m_clamp((e - 0.08f * (float)r) / 0.75f, 0, 1)));
        else if (s_shown[r] < s_target[r]) {                  /* live gains: one segment per frame */
            s_shown[r] += 1.0f;
            if (s_shown[r] > s_target[r]) s_shown[r] = s_target[r];
        } else s_shown[r] = s_target[r];
    }
    cache_update();
}

static void fmt_k(char *b, int n, int v)
{
    if (v >= 10000) snprintf(b, (size_t)n, "%d.%dK", v / 1000, (v % 1000) / 100);
    else snprintf(b, (size_t)n, "%d", v);
}

static void centre(gfx_fb *fb, const face_ctx *c, uint16_t tcol, uint16_t lcol, int dx, int dy, int aod)
{
    char b[24];
    ui_fmt_time(b, sizeof b, c->dt.hour, c->dt.minute);
    int asc = UI_F_DISPLAY->ascent;
    int ty = UI_CY + (int)(asc * 0.36f) - 14 + dy;            /* optical centre, slightly high */
    if (aod) fk_text_outline(fb, UI_F_DISPLAY, UI_CX + dx, ty, b, tcol, UI_CENTER, 0, 1);
    else ui_text(fb, UI_F_DISPLAY, UI_CX + dx, ty, b, tcol, UI_CENTER, 0);
    snprintf(b, sizeof b, "%s %d", ui_weekday3(c->dt.weekday), c->dt.day);
    ui_label(fb, UI_CX + dx, ty - asc - 26, b, lcol, UI_CENTER);
    if (aod) {
        char v[16]; fmt_k(v, sizeof v, s_val[0]);
        snprintf(b, sizeof b, "%s STEPS", v);
        ui_label(fb, UI_CX + dx, ty + 54, b, lcol, UI_CENTER);
        return;
    }
    /* three values: STP / KCAL / MIN */
    static const char *lab[3] = { "STP", "KCAL", "MIN" };
    const int xs[3] = { UI_CX - 84, UI_CX, UI_CX + 84 };
    for (int k = 0; k < 3; k++) {
        char v[16]; fmt_k(v, sizeof v, s_val[k]);
        ui_text(fb, UI_F_BODY, xs[k], ty + 58, v, C_PRI, UI_CENTER, 0);
        ui_text(fb, UI_F_SMALL, xs[k], ty + 82, lab[k], C_DIS, UI_CENTER, 0);
    }
}

static void draw(gfx_fb *fb, const face_ctx *c)
{
    if (c->aod) {
        /* the steps ring as small dots for the lit segments only, time outlined */
        int dx = fk_aod_dx, dy = fk_aod_dy;
        int lit = (int)s_target[0];
        if (lit > NSEG) lit = NSEG;
        if (lit > 0) fk_ring(fb, UI_CX + dx, UI_CY + dy, k_r[0], NSEG, 3.0f, lit, C_DIS, 0);
        centre(fb, c, C_SEC, C_DIS, dx, dy, 1);
        return;
    }
    if (G && G->ok) {
        for (int q = 0; q < 4; q++) sprite_blit(fb, &G->q[q], qx(q), qy(q), G->key[q]);
    } else {
        /* no PSRAM: segments as dots (cheap, still discrete) */
        for (int r = 0; r < NRING; r++) {
            float sh = s_shown[r];
            float lap1 = sh > NSEG ? NSEG : sh;
            fk_ring(fb, UI_CX, UI_CY, k_r[r], NSEG, 6.0f, (int)lap1, C_WHITE, COL_OFF);
            if (sh > NSEG) fk_ring(fb, UI_CX, UI_CY, k_r[r], NSEG, 6.0f, (int)(sh - NSEG), C_RED, 0);
        }
    }
    centre(fb, c, C_WHITE, C_SEC, 0, 0, 0);
}

static const struct nos_screen *tap(int x, int y)
{
    float dx = (float)x - UI_CX, dy = (float)y - UI_CY, d2 = dx * dx + dy * dy;
    if (d2 >= 160.0f * 160.0f || (y > UI_CY + 30 && y < UI_CY + 140)) return &scr_activity;
    return 0;
}

const nos_face face_segment = { "SEGMENT", enter, tick, draw, tap };
