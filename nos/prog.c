#include "hal.h"
#include <string.h>
#include <stddef.h>
/* prog.c — progress indicators beyond dots (prog.h). */
#include "prog.h"
#include "ui.h"
#include "dot.h"
#include "gfx_cost.h"
#include <math.h>

const char *prog_name(int style)
{
    static const char *n[PROG_NSTYLES] = { "SQUARES", "SEGMENTS", "SLANTS", "TRIANGLES", "CHEVRONS", "DOTS" };
    return style >= 0 && style < PROG_NSTYLES ? n[style] : "?";
}

/* ---- convex polygon: per pixel, coverage = the nearest edge's signed distance + 0.5 ---- */
void prog_poly(gfx_fb *fb, const float *xy, int n, uint16_t color)
{
    if (n < 3 || n > 16) return;
    float x0 = xy[0], x1 = xy[0], y0 = xy[1], y1 = xy[1];
    for (int i = 1; i < n; i++) {
        float x = xy[2 * i], y = xy[2 * i + 1];
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
    }
    int bx0 = (int)floorf(x0) - 1, by0 = (int)floorf(y0) - 1, bx1 = (int)ceilf(x1) + 1, by1 = (int)ceilf(y1) + 1;
    if (fb->rec) {
        unsigned h = gfx_hash_u(0x9017u, color);
        for (int i = 0; i < 2 * n; i++) h = gfx_hash_f(h, xy[i]);
        gfx_rec_cmd(fb, bx0, by0, bx1, by1, h);
        return;
    }
    int lo = fb->y_base - fb->oy, hi = lo + fb->h;     /* logical rows held */
    if (by0 < lo) by0 = lo;
    if (by1 > hi) by1 = hi;
    int c0 = fb->cx0, c1 = fb->cx1;
    if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (c0 < 0) c0 = 0;
    if (c1 > GFX_W) c1 = GFX_W;
    c0 -= fb->ox; c1 -= fb->ox;                         /* to logical columns */
    if (bx0 < c0) bx0 = c0;
    if (bx1 > c1) bx1 = c1;
    if (by1 <= by0 || bx1 <= bx0) return;            /* not in this stripe: no maths */
    color = gfx_ink(fb, color);
    /* edges as a x + b y + c >= 0 inside, |(a, b)| = 1 */
    float area = 0, ea[16], eb[16], ec[16];
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
    }
    float sg = area > 0 ? 1.0f : -1.0f;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        float dx = xy[2 * j] - xy[2 * i], dy = xy[2 * j + 1] - xy[2 * i + 1];
        float l = gfx_fast_sqrt(dx * dx + dy * dy);
        if (l < 1e-4f) { ea[i] = eb[i] = 0; ec[i] = 1e6f; continue; }
        ea[i] = -dy / l * sg; eb[i] = dx / l * sg;
        ec[i] = -(ea[i] * xy[2 * i] + eb[i] * xy[2 * i + 1]);
    }
    unsigned np = 0;                                    /* sim cost model only */
    for (int py = by0; py < by1; py++) {
        uint16_t *row = gfx_row(fb, py + fb->oy - fb->y_base) + fb->ox;
        float fy = (float)py + 0.5f;
        int left = bx0, right = bx1 - 1;
        for (; left <= right; left++) {
            float fx = (float)left + 0.5f, d = 1e6f;
            for (int i = 0; i < n; i++) {
                float e = ea[i] * fx + eb[i] * fy + ec[i];
                if (e < d) d = e;
            }
            float cov = d + 0.5f;
            if (cov >= 1) break;
            if (cov <= 0) continue;
            np++;
            row[left] = gfx_blend565(row[left], color, (uint8_t)(cov * 255.0f));
        }
        if (left > right) continue;
        for (; right > left; right--) {
            float fx = (float)right + 0.5f, d = 1e6f;
            for (int i = 0; i < n; i++) {
                float e = ea[i] * fx + eb[i] * fy + ec[i];
                if (e < d) d = e;
            }
            float cov = d + 0.5f;
            if (cov >= 1) break;
            if (cov <= 0) continue;
            np++;
            row[right] = gfx_blend565(row[right], color, (uint8_t)(cov * 255.0f));
        }
        /* For finite raster inputs each original rounded edge expression is
         * monotone in x. Its solid half-interval intersects the other edges in
         * one interval, so two solid endpoints prove the interior solid too.
         * Keep the original cov>=1 test (rounding differs from d>=0.5). The
         * left scan owns no-solid rows; the right scan never rewrites left. */
        np += (unsigned)(right - left + 1);
        for (int px = left; px <= right; px++) row[px] = color;
    }
    GFX_COST(GFX_COST_BLEND, np);
}

/* ---- one element in a local frame: centre (px, py), u along the bar, v across it ---- */
typedef struct { float px, py, ux, uy, vx, vy, slot, th; } frame_t;
static void poly_local(gfx_fb *fb, const frame_t *f, const float *ab, int n, uint16_t c)
{
    float xy[32];
    for (int i = 0; i < n; i++) {
        xy[2 * i] = f->px + f->ux * ab[2 * i] + f->vx * ab[2 * i + 1];
        xy[2 * i + 1] = f->py + f->uy * ab[2 * i] + f->vy * ab[2 * i + 1];
    }
    prog_poly(fb, xy, n, c);
}
/* a box in the local frame [a0, a1] x [-b, b] with rounded corners of radius rr (each
 * corner three points on its arc: 12 edges read as round at these sizes) */
static void box_local(gfx_fb *fb, const frame_t *f, float a0, float a1, float b, float rr, uint16_t c)
{
    if (a1 - a0 < 0.5f) return;
    if (rr > (a1 - a0) * 0.5f) rr = (a1 - a0) * 0.5f;
    if (rr > b) rr = b;
    static const float k[3][2] = { { 1.0f, 0.0f }, { 0.7071f, 0.7071f }, { 0.0f, 1.0f } };
    const float cx[4] = { a1 - rr, a0 + rr, a0 + rr, a1 - rr }, cy[4] = { b - rr, b - rr, -b + rr, -b + rr };
    const float sx[4] = { 1, -1, -1, 1 }, sy[4] = { 1, 1, -1, -1 };
    float ab[24];
    int m = 0;
    for (int q = 0; q < 4; q++)                         /* corners in order around the box */
        for (int j = 0; j < 3; j++) {
            int jj = (q & 1) ? 2 - j : j;               /* keep walking the same way round */
            ab[m++] = cx[q] + sx[q] * rr * k[jj][0];
            ab[m++] = cy[q] + sy[q] * rr * k[jj][1];
        }
    poly_local(fb, f, ab, 12, c);
}
static uint16_t lerp_ink(uint16_t off, uint16_t on, float k)
{
    k = floorf(k * 8.0f + 0.5f) / 8.0f;               /* 8 perceptible steps */
    return off ? ui_mix(off, on, k) : ui_dim(on, k);
}
static void element(gfx_fb *fb, int style, const frame_t *f, int i, float k, uint16_t on, uint16_t off)
{
    if (k <= 0 && !off) return;
    uint16_t c = k >= 1 ? on : k <= 0 ? off : lerp_ink(off, on, k);
    float s = f->slot, h2 = f->th * 0.5f;
    switch (style) {
    case PROG_SQUARES: {
        float side = s * 0.74f < f->th ? s * 0.74f : f->th;
        box_local(fb, f, -side * 0.5f, side * 0.5f, side * 0.5f, side * 0.28f, c);
        break;
    }
    case PROG_SEGMENTS: {                               /* the leading one grows along its length */
        float L = s * 0.80f, a0 = -L * 0.5f;
        if (k > 0 && k < 1) {
            if (off) box_local(fb, f, a0, a0 + L, h2, 2.5f, off);
            box_local(fb, f, a0, a0 + L * (floorf(k * 8.0f + 0.5f) / 8.0f), h2, 2.5f, on);
        } else box_local(fb, f, a0, a0 + L, h2, 2.5f, c);
        break;
    }
    case PROG_SLANT: {                                  /* "/" dashes */
        float dw = s * 0.40f, sk = f->th * 0.30f;
        float ab[8] = { -dw * 0.5f + sk, -h2, dw * 0.5f + sk, -h2, dw * 0.5f - sk, h2, -dw * 0.5f - sk, h2 };
        poly_local(fb, f, ab, 4, c);
        break;
    }
    case PROG_TRIANGLES: {                              /* stacked up / down, tessellating */
        float bw = s * 0.80f, apex = (i & 1) ? h2 : -h2;
        float ab[6] = { -bw, -apex, bw, -apex, 0, apex };
        poly_local(fb, f, ab, 3, c);
        break;
    }
    case PROG_CHEVRONS: {                               /* ">" pointing along the bar */
        float st = s * 0.26f, d = (s * 0.72f - st) * 0.5f, o = -st * 0.5f;
        float up[8] = { o - d, -h2, o - d + st, -h2, o + d + st, 0, o + d, 0 };
        float dn[8] = { o + d, 0, o + d + st, 0, o - d + st, h2, o - d, h2 };
        poly_local(fb, f, up, 4, c);
        poly_local(fb, f, dn, 4, c);
        break;
    }
    default: {
        float d = s * 0.72f < f->th ? s * 0.72f : f->th;
        dot_disc(fb, f->px, f->py, d, c);
        break;
    }
    }
}

void prog_bar(gfx_fb *fb, int style, float x, float y, float w, float h, int n, float frac,
              uint16_t on, uint16_t off)
{
    if (n <= 0 || w <= 0) return;
    frame_t f = { 0, y + h * 0.5f, 1, 0, 0, 1, w / (float)n, h };
    float t = frac * (float)n;
    for (int i = 0; i < n; i++) {
        f.px = x + f.slot * ((float)i + 0.5f);
        float k = t - (float)i;
        element(fb, style, &f, i, k < 0 ? 0 : k > 1 ? 1 : k, on, off);
    }
}


/* Bounded immutable layout metadata, never rendered app pixels. Cache only
 * original angle results and conservative extents: storing r*sin/r*cos would
 * split the target's fused multiply-add and change translated AA pixels. */
#define PROG_GEOMETRY_MAX 64
#define PROG_GEOMETRY_SLOTS 2
typedef struct { float sa,ca,ex,ey; } ring_geom;
typedef struct { int n,style; float r,t; unsigned age; ring_geom g[PROG_GEOMETRY_MAX]; } ring_geometry;
static unsigned geometry_age,geometry_bytes;
static ring_geometry *geometry;
static int geometry_blocked;
int prog_geometry_enabled=1;
unsigned prog_geometry_bytes(void){return geometry_bytes;}
void prog_geometry_release(void) {
    hal_big_free(geometry);geometry=NULL;geometry_blocked=0;geometry_age=0;geometry_bytes=0;
}

static const ring_geom *ring_geometry_get(int n,int style,float r,float t) {
    if(!prog_geometry_enabled || n>PROG_GEOMETRY_MAX || t<=0 || geometry_blocked) return NULL;
    if(!geometry) {
        const unsigned bytes=sizeof(ring_geometry)*PROG_GEOMETRY_SLOTS;
#ifdef NOS_PSRAM_TRACKING
        unsigned available=nos_mem_available();
        if(available<512u*1024u || bytes>available-512u*1024u) return NULL;
#endif
        geometry=hal_big_alloc(bytes);
        if(!geometry){geometry_blocked=1;return NULL;}
        memset(geometry,0,bytes);geometry_bytes=bytes;
    }
    ring_geometry *v=&geometry[0];
    for(int j=0;j<PROG_GEOMETRY_SLOTS;j++) {
        ring_geometry *g=&geometry[j];
        if(g->n==n && g->style==style && g->r==r && g->t==t) {
            g->age=++geometry_age;return g->g;
        }
        if(g->age<v->age) v=g;
    }

    float slot=6.2831853f*r/(float)n, a,b=t*.5f;
    switch(style) {
    case PROG_SQUARES: {float side=slot*.74f<t?slot*.74f:t;a=b=side*.5f;break;}
    case PROG_SEGMENTS:a=slot*.40f;break;
    case PROG_SLANT:a=slot*.20f+t*.30f;break;
    case PROG_TRIANGLES:a=slot*.80f;break;
    case PROG_CHEVRONS:a=slot*.36f;break;
    default:{float d=slot*.72f<t?slot*.72f:t;a=b=d*.5f;break;}
    }
    for(int i=0;i<n;i++) {
        float angle=6.2831853f*((float)i+.5f)/(float)n;
        float sa=gfx_fast_sin(angle),ca=gfx_fast_cos(angle);
        ring_geom *g=&v->g[i];
        g->sa=sa;g->ca=ca;
        g->ex=fabsf(ca)*a+fabsf(sa)*b+2.f;
        g->ey=fabsf(sa)*a+fabsf(ca)*b+2.f;
    }
    v->n=n;v->r=r;v->t=t;v->style=style;v->age=++geometry_age;
    return v->g;
}

void prog_ring(gfx_fb *fb,int style,float cx,float cy,float r,float t,int n,float frac,uint16_t on,uint16_t off)
{
    if(n<=0 || r<=0) return;
    float tf=frac*(float)n;
    frame_t f;f.slot=6.2831853f*r/(float)n;f.th=t;
    const ring_geom *g=ring_geometry_get(n,style,r,t);
    int lo=fb->y_base-fb->oy,hi=lo+fb->h,c0=fb->cx0,c1=fb->cx1;
    if(c1<=c0){c0=0;c1=GFX_W;}if(c0<0)c0=0;if(c1>GFX_W)c1=GFX_W;
    c0-=fb->ox;c1-=fb->ox;
    const float xlo=(float)c0,xhi=(float)c1,ylo=(float)lo,yhi=(float)hi;
    for(int i=0;i<n;i++) {
        if(g) {
            float sa=g[i].sa,ca=g[i].ca;
            float px=cx+r*sa,py=cy-r*ca;
            /* Two extra pixels cover the original integer bbox's AA padding
             * and conservative float rounding. Rejected segments never build
             * local polygon vertices, edge equations or interpolated ink. */
            if(!fb->rec && (py+g[i].ey<ylo || py-g[i].ey>yhi ||
                px+g[i].ex<xlo || px-g[i].ex>xhi)) continue;
            f.px=px;f.py=py;
            f.ux=ca;f.uy=sa;f.vx=-sa;f.vy=ca;
        } else {
            float a=6.2831853f*((float)i+.5f)/(float)n,sa=gfx_fast_sin(a),ca=gfx_fast_cos(a);
            f.px=cx+r*sa;f.py=cy-r*ca;f.ux=ca;f.uy=sa;f.vx=-sa;f.vy=ca;
        }
        float k=tf-(float)i;
        element(fb,style,&f,i,k<0?0:k>1?1:k,on,off);
    }
}

void prog_arc(gfx_fb *fb, int style, float cx, float cy, float r, float t, float a0, float a1, int n,
              float frac, uint16_t on, uint16_t off)
{
    if (n <= 0 || r <= 0 || a0 == a1) return;
    float tf = frac * (float)n, dir = a1 > a0 ? 1.0f : -1.0f;
    frame_t f;
    f.slot = (a1 - a0) * dir * r / (float)n;
    f.th = t;
    for (int i = 0; i < n; i++) {
        float a = a0 + (a1 - a0) * ((float)i + 0.5f) / (float)n, sa = gfx_fast_sin(a), ca = gfx_fast_cos(a);
        f.px = cx + r * sa; f.py = cy - r * ca;
        f.ux = dir * ca; f.uy = dir * sa;               /* the way it fills */
        f.vx = -sa; f.vy = ca;                          /* inward */
        float k = tf - (float)i;
        element(fb, style, &f, i, k < 0 ? 0 : k > 1 ? 1 : k, on, off);
    }
}

/* One arc element, with exactly prog_arc's arithmetic and element index.
 * Retained callers can bucket each segment without replaying its neighbours. */
void prog_arc_segment(gfx_fb *fb, int style, float cx, float cy, float r, float t,
                      float a0, float a1, int n, float frac, uint16_t on, uint16_t off, int i)
{
    if (n <= 0 || r <= 0 || a0 == a1 || i < 0 || i >= n) return;
    float dir = a1 > a0 ? 1.0f : -1.0f;
    frame_t f;
    f.slot = (a1 - a0) * dir * r / (float)n; f.th = t;
    float a = a0 + (a1 - a0) * ((float)i + 0.5f) / (float)n;
    float sa = gfx_fast_sin(a), ca = gfx_fast_cos(a);
    f.px = cx + r * sa; f.py = cy - r * ca;
    f.ux = dir * ca; f.uy = dir * sa; f.vx = -sa; f.vy = ca;
    float k = frac * (float)n - (float)i;
    element(fb, style, &f, i, k < 0 ? 0 : k > 1 ? 1 : k, on, off);
}
