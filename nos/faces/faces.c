/* faces.c — face registry (best first: index 0 is the default face), burn-in drift
 * and the small helpers the faces share (face_kit.h). Owned by the faces team. */
#include "faces/face.h"
#include "faces/face_kit.h"
#include "nos.h"
#include "prefs.h"
#include <string.h>

extern const nos_face face_dot, face_glyph, face_life, face_segment, face_inverted,
                      face_matrix, face_split, face_tape;
const nos_face *const nos_faces[] = { &face_dot, &face_glyph, &face_matrix, &face_life,
                                      &face_segment, &face_split, &face_inverted, &face_tape };
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
void fk_aod_update(const face_ctx *c)
{
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
}
void fk_mm(char out[3], int minute)
{
    out[0] = (char)('0' + minute / 10); out[1] = (char)('0' + minute % 10); out[2] = 0;
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
    if (!m->init || strlen(now) != strlen(m->to)) {
        strncpy(m->from, now, 7); m->from[7] = 0;
        strncpy(m->to, now, 7); m->to[7] = 0;
        m->t0 = -100.0f; m->init = 1;
        return;
    }
    if (strcmp(now, m->to) != 0) {
        memcpy(m->from, m->to, sizeof m->from);
        strncpy(m->to, now, 7); m->to[7] = 0;
        m->t0 = t;
    }
}
float fk_morph_p(const fk_morph *m, float t, float dur)
{
    float p = (t - m->t0) / dur;
    return p >= 1.0f ? 1.0f : p <= 0.0f ? 0.0f : p;
}

void fk_text_outline(gfx_fb *fb, const font_t *f, int x, int y, const char *s,
                     uint16_t color, int align, int tracking, int w)
{
    static const signed char o[8][2] = { {-1,0},{1,0},{0,-1},{0,1},{-1,-1},{1,-1},{-1,1},{1,1} };
    for (int i = 0; i < 8; i++)
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

void fk_dots(gfx_fb *fb, int n, int chunk, fk_dot_fn fn, const void *ctx)
{
    if (chunk < 1) chunk = 1;
    if (fb->rec) {
        for (int i0 = 0; i0 < n; i0 += chunk) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            unsigned h = gfx_hash_u(0xFAC3u, (unsigned)(i0 * 977 + n));
            int any = 0;
            for (int i = i0; i < i0 + chunk && i < n; i++) {
                float x, y, d; uint16_t col;
                fn(i, ctx, &x, &y, &d, &col);
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
    for (int i = 0; i < n; i++) {
        float x, y, d; uint16_t col;
        fn(i, ctx, &x, &y, &d, &col);
        if (d <= 0.0f) continue;
        float e = d * 0.5f + 1.0f;
        if (!fk_hit(&v, x - e, y - e, x + e, y + e)) continue;
        dot_disc(fb, x, y, d, col);
    }
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
