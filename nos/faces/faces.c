/* faces.c — face registry (best first: index 0 is the default face), burn-in drift
 * and the small helpers the faces share (face_kit.h). Owned by the faces team. */
#include "faces/face.h"
#include "faces/face_kit.h"
#include "faces/face_ext.h"
#include "nos.h"
#include "prefs.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include "gfx_cost.h"

extern const nos_face face_dot, face_glyph, face_life, face_segment, face_inverted,
                      face_matrix, face_split, face_tape;
const nos_face *const nos_faces[] = { &face_dot, &face_matrix, &face_glyph, &face_split,
                                      &face_life, &face_segment, &face_inverted, &face_tape };
const int nos_face_count = sizeof nos_faces / sizeof nos_faces[0];

/* a slow Lissajous walk inside +-4 px, one step per minute */
static void shift_for(int m, int *dx, int *dy)
{
    static const signed char wx[8] = { 0, 2, 4, 2, 0, -2, -4, -2 };
    static const signed char wy[7] = { 0, 3, -1, 4, -3, 1, -4 };
    *dx = wx[m % 8];
    *dy = wy[m % 7];
}

void face_aod_shift(int *dx, int *dy)
{
    hal_datetime dt; hal_get_datetime(&dt);
    shift_for(dt.hour * 60 + dt.minute, dx, dy);
}

int fk_aod_dx, fk_aod_dy;
static unsigned s_dc_epoch;                           /* bumped by every face tick: dot lists go stale */
void fk_aod_update(const face_ctx *c)
{
    s_dc_epoch++;
    if (!c->aod && c->enter >= 1.0f) face_segment_warm();   /* ~1 ms a frame until done */
    if (!c->aod) { fk_aod_dx = fk_aod_dy = 0; return; }
    shift_for(c->dt.hour * 60 + c->dt.minute, &fk_aod_dx, &fk_aod_dy);
}

float fk_ease(float p) { return bezier_ease(&EASE_STANDARD, m_clamp(p, 0, 1)); }
float fk_mech(float p) { return bezier_ease(&EASE_MECH, m_clamp(p, 0, 1)); }
float fk_stagger(float enter, float start, float span)
{
    if (span <= 0) return enter >= start ? 1.0f : 0.0f;
    return fk_ease((enter - start) / span);
}

int fk_hour_disp(int hour)
{
    if (nos_prefs.hour24) return hour;
    hour %= 12;
    return hour ? hour : 12;
}
void fk_hh(char out[3], int hour)
{
    int h = fk_hour_disp(hour);
    out[0] = (char)('0' + h / 10); out[1] = (char)('0' + h % 10); out[2] = 0;
    if (!nos_prefs.hour24 && h < 10) out[0] = ' ';     /* 12 h: no leading zero */
}
void fk_mm(char out[3], int minute)
{
    out[0] = (char)('0' + minute / 10); out[1] = (char)('0' + minute % 10); out[2] = 0;
}
void fk_time(char out[8], int hour, int minute)
{
    char hh[3], mm[3];
    fk_hh(hh, hour); fk_mm(mm, minute);
    snprintf(out, 8, "%s:%s", hh[0] == ' ' ? hh + 1 : hh, mm);
}
void fk_date(char out[16], const face_ctx *c)
{
    snprintf(out, 16, "%s %d", ui_weekday3(c->dt.weekday), c->dt.day);
}

void fk_view_of(const gfx_fb *fb, fk_view *v)
{
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    v->x0 = (float)(c0 - fb->ox); v->x1 = (float)(c1 - fb->ox);
    v->y0 = (float)(fb->y_base - fb->oy); v->y1 = v->y0 + (float)fb->h;
}

void fk_morph_tick(fk_morph *m, const char *now, float t)
{
    if (!m->init) {
        strncpy(m->from, now, 11); m->from[11] = 0;
        strncpy(m->to, now, 11); m->to[11] = 0;
        m->t0 = -100.0f; m->init = 1;
        return;
    }
    if (strcmp(now, m->to) != 0) {
        memcpy(m->from, m->to, sizeof m->from);
        strncpy(m->to, now, 11); m->to[11] = 0;
        m->t0 = t;
    }
}
float fk_morph_p(const fk_morph *m, float t, float dur)
{
    float p = (t - m->t0) / dur;
    return p >= 1.0f ? 1.0f : p <= 0.0f ? 0.0f : p;
}
void fk_morph_settle(fk_morph *m)
{
    memcpy(m->from, m->to, sizeof m->from);
    m->t0 = -100.0f;
}

/* ---- label cross-fade ------------------------------------------------------------------ */
#define XF_HZ 30.0f                         /* one step per frame, like the time morphs */
void fk_xfade_tick(fk_xfade *x, const char *now, float t, int snap)
{
    if (!x->init || snap) {
        if (!x->init || strcmp(now, x->to)) { strncpy(x->to, now, 15); x->to[15] = 0; }
        memcpy(x->from, x->to, sizeof x->from);
        x->t0 = -100.0f; x->init = 1;
        return;
    }
    if (strcmp(now, x->to)) {
        memcpy(x->from, x->to, sizeof x->from);
        strncpy(x->to, now, 15); x->to[15] = 0;
        x->t0 = t;
    }
}
void fk_xfade_k(const fk_xfade *x, float t, float *a_from, float *a_to)
{
    float n = (t - x->t0) * XF_HZ + 0.5f;           /* frames since the change, rounded */
    int st = n < 0.0f ? 0 : n > 5.0f ? 5 : (int)n;
    static const float kf[5] = { 2.0f / 3.0f, 1.0f / 3.0f, 0, 0, 0 };
    static const float kt[5] = { 0, 0, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f };
    if (st >= 5 || !strcmp(x->from, x->to)) { *a_from = 0; *a_to = 1.0f; return; }
    *a_from = kf[st]; *a_to = kt[st];
}
int fk_xfade_label(gfx_fb *fb, const fk_xfade *x, float t, int px, int py, uint16_t col, int align)
{
    float af, at;
    fk_xfade_k(x, t, &af, &at);
    if (af > 0.0f) ui_label(fb, px, py, x->from, ui_dim(col, af), align);
    if (at > 0.0f) return ui_label(fb, px, py, x->to, at >= 1.0f ? col : ui_dim(col, at), align);
    return ui_text_width(UI_F_LABEL, x->to, 2);
}

void fk_text_outline(gfx_fb *fb, const font_t *f, int x, int y, const char *s,
                     uint16_t color, int align, int tracking, int w)
{
    /* four axis offsets: a 1-2 px stroke for half the glyph cost of eight */
    static const signed char o[4][2] = { {-1,0},{1,0},{0,-1},{0,1} };
    for (int i = 0; i < 4; i++)
        ui_text(fb, f, x + o[i][0] * w, y + o[i][1] * w, s, color, align, tracking);
    ui_text(fb, f, x, y, s, C_BG, align, tracking);
}

float fk_sin(float x)
{
    float k = x * 0.15915494f + 0.5f;
    int ik = (int)k; if ((float)ik > k) ik--;
    x -= 6.2831853f * (float)ik;                          /* [-pi, pi) */
    if (x > 1.5707963f) x = 3.1415927f - x;               /* fold into [-pi/2, pi/2] */
    else if (x < -1.5707963f) x = -3.1415927f - x;
    float x2 = x * x;
    return x * (0.9999966f + x2 * (-0.16665682f + x2 * (0.00830632f - x2 * 0.00018363f)));
}
float fk_cos(float x) { return fk_sin(x + 1.5707963f); }

static inline int ifl(float v) { int i = (int)v; return i - (v < (float)i); }
static inline int icl(float v) { int i = (int)v; return i + (v > (float)i); }

/* ---- dot lists ------------------------------------------------------------------------
 * A face draws once for the record pass and again for every 16-row stripe (~30 a frame),
 * so a dot list's callback (trig, mixes, branches) would run ~30x per dot per frame. The
 * first pass of a frame evaluates each list once into a PSRAM cache (dots + one bbox per 8
 * dots); later passes re-find the list by call order and only reject and stamp. A frame is
 * (face tick epoch, nos_t); a list is re-found by (fn, n) plus its first and last dot
 * re-evaluated and compared, so a different list or changed state never reuses stale dots
 * (it falls back to evaluating directly). */
#define DC_DOTS  2048                                 /* both faces of a picker slide fit */
#define DC_CALLS 64
#define DC_G     8                                    /* dots per reject box */
typedef struct { float x, y, d; uint16_t col, pad; } dc_dot;
typedef struct { fk_dot_fn fn; int n, off; } dc_call;
typedef struct {
    dc_dot   dot[DC_DOTS];
    float    box[DC_DOTS / DC_G][4];
    dc_call  call[DC_CALLS];
    int      ncall, used, cursor, failed;
    unsigned epoch; float t;
    const void *px; int rec, y_base, h, cx0, cx1;     /* the pass in progress */
} dc_cache;
static dc_cache *s_dc;

static int dc_same(const dc_dot *a, fk_dot_fn fn, int i, const void *ctx)
{
    float x, y, d; uint16_t col;
    fn(i, ctx, &x, &y, &d, &col);
    return x == a->x && y == a->y && d == a->d && col == a->col;
}

/* the cached list for this call, built on first use this frame; 0 = evaluate directly */
static const dc_cache *dc_find(const gfx_fb *fb, int n, fk_dot_fn fn, const void *ctx, int *off)
{
    if (n <= 0 || n > DC_DOTS) return 0;
    if (!s_dc) {
        static int tried;
        if (tried) return 0;
        tried = 1;
        s_dc = (dc_cache *)hal_big_alloc(sizeof *s_dc);
        if (!s_dc) return 0;
        memset(s_dc, 0, sizeof *s_dc);
        s_dc->epoch = s_dc_epoch - 1u;
    }
    dc_cache *C = s_dc;
    if (C->epoch != s_dc_epoch || C->t != nos_t) {     /* a new frame: forget every list */
        C->epoch = s_dc_epoch; C->t = nos_t;
        C->ncall = C->used = C->cursor = 0; C->px = 0;
    }
    int rec = fb->rec != 0;
    if (fb->px != C->px || rec != C->rec || fb->y_base != C->y_base || fb->h != C->h ||
        fb->cx0 != C->cx0 || fb->cx1 != C->cx1) {    /* a new pass: lists come round again */
        C->px = fb->px; C->rec = rec; C->y_base = fb->y_base; C->h = fb->h;
        C->cx0 = fb->cx0; C->cx1 = fb->cx1;
        C->cursor = 0;
    }
    for (int pass = 0; pass < 2; pass++) {
        int a = pass ? 0 : C->cursor, b = pass ? C->cursor : C->ncall;
        for (int k = a; k < b; k++) {
            const dc_call *L = &C->call[k];
            if (L->fn != fn || L->n != n) continue;
            if (!dc_same(&C->dot[L->off], fn, 0, ctx) || !dc_same(&C->dot[L->off + n - 1], fn, n - 1, ctx)) continue;
            C->cursor = k + 1;
            *off = L->off;
            return C;
        }
    }
    /* not seen this frame: evaluate it once */
    int o = C->used;
    if (C->ncall >= DC_CALLS || o + n > DC_DOTS) return 0;
    for (int i = 0; i < n; i++) {
        dc_dot *D = &C->dot[o + i];
        fn(i, ctx, &D->x, &D->y, &D->d, &D->col);
    }
    for (int g = 0; g < n; g += DC_G) {
        float *bx = C->box[(o + g) / DC_G];
        bx[0] = bx[1] = 1e9f; bx[2] = bx[3] = -1e9f;
        for (int i = g; i < g + DC_G && i < n; i++) {
            const dc_dot *D = &C->dot[o + i];
            if (D->d <= 0.0f) continue;
            float e = D->d * 0.5f + 1.0f;
            if (D->x - e < bx[0]) bx[0] = D->x - e;
            if (D->y - e < bx[1]) bx[1] = D->y - e;
            if (D->x + e > bx[2]) bx[2] = D->x + e;
            if (D->y + e > bx[3]) bx[3] = D->y + e;
        }
    }
    C->call[C->ncall].fn = fn; C->call[C->ncall].n = n; C->call[C->ncall].off = o;
    C->ncall++;
    C->cursor = C->ncall;
    C->used = (o + n + DC_G - 1) / DC_G * DC_G;           /* keep lists box-aligned */
    *off = o;
    return C;
}

void fk_dots(gfx_fb *fb, int n, int chunk, fk_dot_fn fn, const void *ctx)
{
    if (chunk < 1) chunk = 1;
    int off = 0;
    const dc_cache *C = dc_find(fb, n, fn, ctx, &off);
    if (fb->rec) {
        for (int i0 = 0; i0 < n; i0 += chunk) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            unsigned h = gfx_hash_u(0xFAC3u, (unsigned)(i0 * 977 + n));
            int any = 0;
            for (int i = i0; i < i0 + chunk && i < n; i++) {
                float x, y, d; uint16_t col;
                if (C) { const dc_dot *D = &C->dot[off + i]; x = D->x; y = D->y; d = D->d; col = D->col; }
                else fn(i, ctx, &x, &y, &d, &col);
                if (d <= 0.0f) { h = gfx_hash_u(h, 0xDEADu); continue; }
                float e = d * 0.5f + 1.0f;
                if (x - e < x0) x0 = x - e;
                if (y - e < y0) y0 = y - e;
                if (x + e > x1) x1 = x + e;
                if (y + e > y1) y1 = y + e;
                h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(h, col), x), y), d);
                any = 1;
            }
            if (any) gfx_rec_cmd(fb, ifl(x0), ifl(y0), icl(x1), icl(y1), h);
            else gfx_rec_cmd(fb, 0, 0, 0, 0, h);            /* keep indices stable */
        }
        return;
    }
    fk_view v;
    fk_view_of(fb, &v);
    if (C) {
        for (int g = 0; g < n; g += DC_G) {
            const float *bx = C->box[(off + g) / DC_G];
            if (bx[2] < bx[0] || !fk_hit(&v, bx[0], bx[1], bx[2], bx[3])) continue;
            for (int i = g; i < g + DC_G && i < n; i++) {
                const dc_dot *D = &C->dot[off + i];
                if (D->d <= 0.0f) continue;
                float e = D->d * 0.5f + 1.0f;
                if (!fk_hit(&v, D->x - e, D->y - e, D->x + e, D->y + e)) continue;
                dot_disc(fb, D->x, D->y, D->d, D->col);
            }
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        float x, y, d; uint16_t col;
        fn(i, ctx, &x, &y, &d, &col);
        if (d <= 0.0f) continue;
        float e = d * 0.5f + 1.0f;
        if (!fk_hit(&v, x - e, y - e, x + e, y + e)) continue;
        dot_disc(fb, x, y, d, col);
    }
}

/* ---- tapered capsule (watch hands) ----------------------------------------------------
 * gfx_taper's pixels exactly (the same per-pixel coverage), but each row only walks the
 * capsule's own x-span instead of its whole bounding box: a hand at 10:08 crosses a box
 * of ~160 x 110 px of which it covers a sliver, and the box was walked once per stripe. */
static uint8_t tp_cov(float d)
{
    float c = d + 0.5f;
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255;
    return (uint8_t)(c * 255.0f);
}
/* clip [*lo, *hi] to the x where k*x + m lies in [a, b] */
static void tp_clip(float k, float m, float a, float b, float *lo, float *hi)
{
    if (k > 1e-6f || k < -1e-6f) {
        float x0 = (a - m) / k, x1 = (b - m) / k;
        if (x0 > x1) { float t = x0; x0 = x1; x1 = t; }
        if (x0 > *lo) *lo = x0;
        if (x1 < *hi) *hi = x1;
    } else if (m < a || m > b) { *lo = 1e9f; *hi = -1e9f; }
}
static void tp_disc(float cx, float cy, float r, float fy, float *lo, float *hi)
{
    float dy = fy - cy, q = r * r - dy * dy;
    if (q <= 0.0f) return;
    float h = gfx_fast_sqrt(q);
    if (cx - h < *lo) *lo = cx - h;
    if (cx + h > *hi) *hi = cx + h;
}
void fk_taper(gfx_fb *fb, float ax, float ay, float bx, float by, float ha, float hb, uint16_t color)
{
    float vx = bx - ax, vy = by - ay;
    float vlen2 = vx * vx + vy * vy;
    if (fb->rec || vlen2 < 1e-6f) { gfx_taper(fb, ax, ay, bx, by, ha, hb, color); return; }
    float hmax = ha > hb ? ha : hb;
    color = gfx_ink(fb, color);
    float inv = 1.0f / vlen2, rmax = hmax + 1.0f, rmax2 = rmax * rmax;
    float len = gfx_fast_sqrt(vlen2), nx = -vy / len, ny = vx / len;
    unsigned n = 0;
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    int px0 = ifl((ax < bx ? ax : bx) - rmax), px1 = icl((ax > bx ? ax : bx) + rmax);
    int py0 = ifl((ay < by ? ay : by) - rmax), py1 = icl((ay > by ? ay : by) + rmax);
    int lo_row = fb->y_base - fb->oy, hi_row = lo_row + fb->h - 1;
    if (py0 < lo_row) py0 = lo_row;
    if (py1 > hi_row) py1 = hi_row;
    for (int py = py0; py <= py1; py++) {
        int byi = py + fb->oy - fb->y_base;
        if ((unsigned)byi >= (unsigned)fb->h) continue;
        float fy = py + 0.5f;
        /* the row's x-span: the end discs, and the band |n.(p - a)| <= rmax with
         * 0 <= t <= 1 (the capsule is convex, so the union is one interval) */
        float lo = 1e9f, hi = -1e9f;
        tp_disc(ax, ay, rmax, fy, &lo, &hi);
        tp_disc(bx, by, rmax, fy, &lo, &hi);
        float blo = -1e9f, bhi = 1e9f;
        tp_clip(nx, ny * (fy - ay) - nx * ax, -rmax, rmax, &blo, &bhi);
        tp_clip(vx * inv, (vy * (fy - ay) - vx * ax) * inv, 0.0f, 1.0f, &blo, &bhi);
        if (blo <= bhi) { if (blo < lo) lo = blo; if (bhi > hi) hi = bhi; }
        if (lo > hi) continue;
        int x0 = ifl(lo - 0.5f) - 1, x1 = icl(hi - 0.5f) + 1;
        if (x0 < px0) x0 = px0;
        if (x1 > px1) x1 = px1;
        uint16_t *row = gfx_row(fb, byi);
        for (int px = x0; px <= x1; px++) {
            unsigned bxi = (unsigned)(px + fb->ox);
            if (bxi < (unsigned)c0 || bxi >= (unsigned)c1) continue;
            float fx = px + 0.5f;
            float t = ((fx - ax) * vx + (fy - ay) * vy) * inv;
            if (t < 0.0f) t = 0.0f; else if (t > 1.0f) t = 1.0f;
            float ddx = fx - (ax + t * vx), ddy = fy - (ay + t * vy);
            float d2 = ddx * ddx + ddy * ddy;
            if (d2 >= rmax2) continue;
            float hw = ha + (hb - ha) * t;
            uint8_t a = tp_cov(hw - gfx_fast_sqrt(d2));
            if (!a) continue;
            row[bxi] = a == 255 ? color : gfx_blend565(row[bxi], color, a);
            n++;
        }
    }
    GFX_COST(GFX_COST_LINE, n);
}

typedef struct { float cx, cy, r, d; int n, lit; uint16_t on, off; } ring_args;
static void ring_dot(int i, const void *vc, float *x, float *y, float *d, uint16_t *col)
{
    const ring_args *a = (const ring_args *)vc;
    float ang = (float)i * 6.2831853f / (float)a->n;
    *x = a->cx + fk_sin(ang) * a->r;
    *y = a->cy - fk_cos(ang) * a->r;
    *col = i < a->lit ? a->on : a->off;
    *d = *col ? a->d : 0.0f;
}
void fk_ring(gfx_fb *fb, float cx, float cy, float r, int n, float d, int lit,
             uint16_t on, uint16_t off)
{
    ring_args a = { cx, cy, r, d, n, lit, on, off };
    fk_dots(fb, n, 8, ring_dot, &a);
}

/* ---- wake from AOD ---------------------------------------------------------------- */
#define WAKE_N 6                                      /* frames: 0.2 s at 30 fps */
int fk_wake_skip(const fk_wake *w) { return w->skip; }
int fk_wake_stale(const fk_wake *w) { return w->stale; }
void fk_wake_tick(fk_wake *w, const face_ctx *c)
{
    uint32_t now = hal_millis();
    w->stale = w->last_ms != 0 && now - w->last_ms > 500u;
    if (!c->aod && !w->last_aod && c->enter < w->last_enter && now - w->last_ms < 200u)
        w->skip = 1;                                   /* rebuilt without ever leaving the glass */
    if (c->enter >= 1.0f || c->aod) w->skip = 0;
    w->last_enter = c->enter;
    w->last_ms = now;
    if (c->aod) {
        shift_for(c->dt.hour * 60 + c->dt.minute, &w->dx, &w->dy);
        w->active = 0;
    } else if (w->last_aod) {
        w->active = 1;
        w->t0 = c->t;
        w->frames = 0;
    } else if (w->active) w->frames++;
    if (w->active && !c->aod && w->frames >= WAKE_N + 6 && c->enter >= 1.0f) w->active = 0;
    w->last_aod = c->aod;
}
float fk_build(const fk_wake *w, const face_ctx *c)
{
    return ((w->active || w->skip) && !c->aod) ? 1.0f : c->enter;
}
float fk_wake_k(const fk_wake *w, const face_ctx *c)
{
    if (c->aod) return 0.0f;
    if (!w->active) return 1.0f;
    /* counted frames, one even step each (1/6 .. 1): no step is ever held twice */
    int f = w->frames + 1;
    return f >= WAKE_N ? 1.0f : (float)f / (float)WAKE_N;
}
void fk_wake_off(const fk_wake *w, const face_ctx *c, float *dx, float *dy)
{
    if (c->aod) { *dx = (float)w->dx; *dy = (float)w->dy; return; }
    float k = fk_wake_k(w, c);
    *dx = w->active ? (float)(int)((float)w->dx * (1.0f - k)) : 0.0f;
    *dy = w->active ? (float)(int)((float)w->dy * (1.0f - k)) : 0.0f;
}

/* ---- unread dot ------------------------------------------------------------------- */
static float s_unread_k;
void fk_unread_tick(const face_ctx *c)
{
    float tgt = c->unread > 0 ? 1.0f : 0.0f;
    if (c->aod || c->enter < 1.0f) { s_unread_k = tgt; return; }
    if (s_unread_k < tgt) s_unread_k += 0.25f;
    else if (s_unread_k > tgt) s_unread_k -= 0.25f;
    if (s_unread_k < 0.0f) s_unread_k = 0.0f;
    if (s_unread_k > 1.0f) s_unread_k = 1.0f;
}
float fk_unread_level(const face_ctx *c)
{
    return c->aod ? (c->unread > 0 ? 1.0f : 0.0f) : s_unread_k;
}
void fk_unread_a(gfx_fb *fb, const face_ctx *c, uint16_t col, uint16_t backing, float dx, float dy, float a)
{
    if (c->aod) {
        if (c->unread > 0) dot_disc(fb, UI_CX + dx, 24.0f + dy, 6.0f, C_SEC);
        return;
    }
    float k = s_unread_k * m_clamp(a, 0, 1);
    if (k <= 0.0f) return;
    if (a >= 1.0f) dot_disc(fb, UI_CX + dx, 24.0f + dy, 20.0f, backing);   /* no moat while fading */
    dot_disc(fb, UI_CX + dx, 24.0f + dy, 8.0f, ui_mix(backing, col, k));   /* 8 px, as on the widget pages */
}
void fk_unread(gfx_fb *fb, const face_ctx *c, uint16_t col, uint16_t backing, float dx, float dy)
{
    fk_unread_a(fb, c, col, backing, dx, dy, 1.0f);
}

/* ---- battery status ----------------------------------------------------------------- */
int fk_status_shown(const face_ctx *c)
{
    if (c->aod) return !c->charging && c->battery > 0 && c->battery < 10;
    return c->charging || (c->battery > 0 && c->battery <= 20);
}
int fk_status(gfx_fb *fb, const face_ctx *c, int x, int y, uint16_t col, int align)
{
    if (!fk_status_shown(c)) return 0;
    char b[8];
    snprintf(b, sizeof b, "%d%%", c->battery);
    int tw = ui_text_width(UI_F_LABEL, b, 2);
    int iw = c->charging ? 18 : 0;                       /* bolt + gap */
    int w = tw + iw;
    int x0 = align == UI_CENTER ? x - w / 2 : align == UI_RIGHT ? x - w : x;
    if (c->charging) dot_icon(fb, DOT_ICON_BOLT, (float)(x0 + 7), (float)(y - 6), 1.45f, 1.7f, col);
    ui_label(fb, x0 + iw, y, b, col, UI_LEFT);
    return w;
}

/* ---- glyph lookup --------------------------------------------------------------------- */
const font_glyph *fk_glyph(const font_t *f, unsigned code)
{
    int lo = 0, hi = f->nglyphs - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (f->glyphs[mid].code == code) return &f->glyphs[mid];
        if (f->glyphs[mid].code < code) lo = mid + 1; else hi = mid - 1;
    }
    return 0;
}

/* ---- scaled font ------------------------------------------------------------------------ */
static int sidx(unsigned char ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch == ':' ? 10 : ch == ' ' ? 11 : -1; }

static const char k_scodes[12] = { '0','1','2','3','4','5','6','7','8','9',':','0' };
/* glyph k of the scaled font; 0 on failure */
static int sglyph_build(fk_sfont *sf, const font_t *src, float s, int k)
{
    const char *codes = k_scodes;
    const font_glyph *g = fk_glyph(src, (unsigned char)codes[k]);
    fk_sglyph *o = &sf->g[k];
    memset(o, 0, sizeof *o);
    if (!g) return 1;
    o->adv = (uint8_t)((float)g->xadv * s + 0.5f);
    if (k == 11) return 1;                             /* space: advance only */
    /* destination pixel (u, v) covers source [u/s, (u+1)/s): 4x4 taps */
    float sx0 = (float)g->xoff, sy0 = (float)g->yoff;
    int ox = (int)(sx0 * s) - 1, oy = (int)(sy0 * s) - 1;
    int w = (int)((sx0 + g->w) * s) + 2 - ox, h = (int)((sy0 + g->h) * s) + 2 - oy;
    if (w > 255 || h > 255) return 0;
    uint8_t *px = (uint8_t *)hal_big_alloc((uint32_t)(w * h));
    if (!px) return 0;
    const uint8_t *bm = src->bitmap + g->bitmap_off;
    /* the taps are separable: source column per (u, i) and row per (v, j), worked out
     * once (the same float expressions as a per-tap evaluation, so the same pixels),
     * then 16 integer adds per pixel. It ran 16 float divides per pixel: ~0.4 s on
     * the watch the first time the face ticked. */
    static int16_t *tx;                               /* 2 x 255 x 4 taps, PSRAM (not the stack) */
    if (!tx) tx = (int16_t *)hal_big_alloc(2u * 255u * 4u * sizeof *tx);
    if (!tx) return 0;
    int16_t *ty = tx + 255 * 4;
    for (int u = 0; u < w; u++)
        for (int i = 0; i < 4; i++) {
            float fx = ((float)(ox + u) + ((float)i + 0.5f) * 0.25f) / s - sx0;
            int ix = (int)(fx + 64.0f) - 64;
            tx[u * 4 + i] = (int16_t)(ix >= 0 && ix < g->w ? ix : -1);
        }
    for (int v = 0; v < h; v++)
        for (int j = 0; j < 4; j++) {
            float fy = ((float)(oy + v) + ((float)j + 0.5f) * 0.25f) / s - sy0;
            int iy = (int)(fy + 64.0f) - 64;
            ty[v * 4 + j] = (int16_t)(iy >= 0 && iy < g->h ? iy * g->w : -1);
        }
    for (int v = 0; v < h; v++)
        for (int u = 0; u < w; u++) {
            int acc = 0;
            for (int j = 0; j < 4; j++) {
                int ro = ty[v * 4 + j];
                if (ro < 0) continue;
                const uint8_t *row = bm + ro;
                for (int i = 0; i < 4; i++) {
                    int ix = tx[u * 4 + i];
                    if (ix >= 0) acc += row[ix];
                }
            }
            px[v * w + u] = (uint8_t)(acc / 16);
        }
    o->w = (uint8_t)w; o->h = (uint8_t)h; o->xoff = (int8_t)ox; o->yoff = (int8_t)oy; o->px = px;
    return 1;
}
static void sfont_finish(fk_sfont *sf, const font_t *src, float s)
{
    sf->ascent = (int)((float)src->ascent * s + 0.5f);
    const font_glyph *z = fk_glyph(src, '0');
    sf->cap = z ? (int)((float)(-z->yoff) * s + 0.5f) : sf->ascent;
    sf->ok = 1;
}
int fk_sfont_build_step(fk_sfont *sf, const font_t *src, float s)
{
    if (sf->ok) return 1;
    if (sf->failed) return 0;
    if (sf->next < 12) {
        if (!sglyph_build(sf, src, s, sf->next)) { sf->failed = 1; return 0; }
        sf->next++;
    }
    if (sf->next >= 12) sfont_finish(sf, src, s);
    return sf->ok;
}
int fk_sfont_build(fk_sfont *sf, const font_t *src, float s)
{
    while (!sf->ok && !sf->failed) fk_sfont_build_step(sf, src, s);
    return sf->ok;
}

int fk_stext_width(const fk_sfont *sf, const char *s)
{
    int w = 0;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        int k = sidx(*c);
        if (k >= 0) w += sf->g[k].adv;
    }
    return w;
}

int fk_stext(gfx_fb *fb, const fk_sfont *sf, int x, int y, const char *s, uint16_t col, int align)
{
    if (!sf->ok || !s || !*s) return 0;
    int w = fk_stext_width(sf, s);
    if (align == UI_CENTER) x -= w / 2;
    else if (align == UI_RIGHT) x -= w;
    if (fb->rec) {
        unsigned h = gfx_hash_u(gfx_hash_u(0x5F07u, col), (unsigned)(x * 4099 + y * 7));
        h = gfx_hash_u(h, (unsigned)(uintptr_t)sf);
        for (const unsigned char *c = (const unsigned char *)s; *c; c++) h = gfx_hash_u(h, *c);
        gfx_rec_cmd(fb, x - 4, y - sf->ascent - 2, x + w + 4, y + sf->ascent / 4 + 2, h);
        return w;
    }
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    col = gfx_ink(fb, col);
    unsigned n = 0;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
        int k = sidx(*c);
        if (k < 0) continue;
        const fk_sglyph *g = &sf->g[k];
        if (g->px) {
            int gx = x + g->xoff + fb->ox, by0 = y + g->yoff + fb->oy - fb->y_base;
            int r0 = by0 < 0 ? -by0 : 0, r1 = by0 + g->h > fb->h ? fb->h - by0 : g->h;
            int i0 = c0 - gx > 0 ? c0 - gx : 0, i1 = c1 - gx < g->w ? c1 - gx : g->w;
            for (int r = r0; r < r1; r++) {
                uint16_t *d = gfx_row(fb, by0 + r) + gx;
                const uint8_t *a = g->px + r * g->w;
                for (int i = i0; i < i1; i++) {
                    if (!a[i]) continue;
                    d[i] = a[i] >= 250 ? col : gfx_blend565(d[i], col, a[i]);
                    n++;
                }
            }
        }
        x += g->adv;
    }
    GFX_COST(GFX_COST_GLYPH, n);
    return w;
}

/* ---- odometer roll ------------------------------------------------------------------------ */
static void roll_draw(gfx_fb *fb, const font_t *f, const fk_sfont *sf, int x, int y, char ch, uint16_t col)
{
    char s[2] = { ch, 0 };
    if (!ch || ch == ' ') return;
    if (sf) fk_stext(fb, sf, x, y, s, col, UI_LEFT);
    else ui_text(fb, f, x, y, s, col, UI_LEFT, 0);
}
void fk_roll(gfx_fb *fb, const font_t *f, const fk_sfont *sf, int x, int base, int cell_w,
             int cap, char from, char to, float p, uint16_t col)
{
    fk_roll2(fb, f, sf, x, base, cell_w, cap, from, to, 0, 0, p, col);
}
void fk_roll2(gfx_fb *fb, const font_t *f, const fk_sfont *sf, int x, int base, int cell_w,
              int cap, char from, char to, int nx_from, int nx_to, float p, uint16_t col)
{
    if (p >= 1.0f || from == to) { roll_draw(fb, f, sf, x + nx_to, base, to, col); return; }
    gfx_fb sub;
    int top = base - cap - 6, bot = base + 6;
    if (!nos_subview(fb, &sub, x - 4 + fb->ox, top + fb->oy, x + cell_w + 4 + fb->ox, bot + fb->oy, 0, 0)) return;
    float e = fk_mech(p);
    int span = bot - top;
    int dy = (int)(e * (float)span + 0.5f);
    roll_draw(&sub, f, sf, x + nx_from, base - dy, from, col);
    roll_draw(&sub, f, sf, x + nx_to, base + span - dy, to, col);
}

float fk_blink(int ms)
{
    if (ms < 100) return (float)(ms / 25 + 1) * 0.25f;
    if (ms < 500) return 1.0f;
    if (ms < 600) return 1.0f - (float)((ms - 500) / 25 + 1) * 0.25f;
    return 0.0f;
}

/* ---- extras for home (face_ext.h) ------------------------------------------------- */
int nos_face_style(const nos_face *f)
{
    if (f == &face_inverted) return FACE_LIGHT | FACE_FULL_BLEED;
    if (f == &face_matrix || f == &face_segment || f == &face_glyph || f == &face_life) return FACE_FULL_BLEED;
    return 0;
}
const struct nos_screen *nos_face_press_rect(const nos_face *f, int x, int y, int r[4])
{
    int tmp[4] = { 0, 0, 0, 0 };
    if (!r) r = tmp;
    r[0] = r[1] = r[2] = r[3] = 0;
    if (f == &face_split) return face_split_hit(x, y, r);
    if (f == &face_segment) return face_segment_hit(x, y, r);
    if (f == &face_inverted) return face_inverted_hit(x, y, r);
    return f && f->tap ? f->tap(x, y) : 0;
}
