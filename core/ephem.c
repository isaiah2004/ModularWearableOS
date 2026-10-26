/* ephem.c - see ephem.h. Port of docs/assets/solar/ephem.js (validated against published
 * opposition / conjunction dates and a JPL Horizons vector). */
#include "ephem.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_cost.h"
/* Count work omitted by the face's old flat astronomy/layout allowances.
 * This is an aggregate software-double proxy, not a libm microbenchmark.
 * The simulator enables it only around the ORRERY tick. */
#ifdef GFX_COST_MODEL
#define sin(x) (gfx_cost_orr_math(4),sin(x))
#define cos(x) (gfx_cost_orr_math(4),cos(x))
#define atan2(x,y) (gfx_cost_orr_math(4),atan2(x,y))
#define asin(x) (gfx_cost_orr_math(4),asin(x))
#define acos(x) (gfx_cost_orr_math(4),acos(x))
#define sqrt(x) (gfx_cost_orr_math(1),sqrt(x))
#define fmod(x,y) (gfx_cost_orr_math(1),fmod(x,y))
#define pow(x,y) (gfx_cost_orr_math(4),pow(x,y))
#define cbrt(x) (gfx_cost_orr_math(4),cbrt(x))
#define asinh(x) (gfx_cost_orr_math(4),asinh(x))
#define sinh(x) (gfx_cost_orr_math(4),sinh(x))
#define cosh(x) (gfx_cost_orr_math(4),cosh(x))
#endif

#define D2R (3.14159265358979323846 / 180.0)
#define R2D (180.0 / 3.14159265358979323846)

const char *const ep_name[EP_N] = { "MERCURY", "VENUS", "EARTH", "MARS", "JUPITER", "SATURN", "URANUS", "NEPTUNE" };
const double ep_period_days[EP_N] = { 87.969, 224.701, 365.256, 686.98, 4332.59, 10759.2, 30688.5, 60182.0 };

/* a, e, I, L, long.peri, long.node, then their rates per Julian century (J2000 ecliptic) */
static const double EL[EP_N][12] = {
    { 0.38709927, 0.20563593, 7.00497902, 252.25032350, 77.45779628, 48.33076593,
      0.00000037, 0.00001906, -0.00594749, 149472.67411175, 0.16047689, -0.12534081 },
    { 0.72333566, 0.00677672, 3.39467605, 181.97909950, 131.60246718, 76.67984255,
      0.00000390, -0.00004107, -0.00078890, 58517.81538729, 0.00268329, -0.27769418 },
    { 1.00000261, 0.01671123, -0.00001531, 100.46457166, 102.93768193, 0.0,
      0.00000562, -0.00004392, -0.01294668, 35999.37244981, 0.32327364, 0.0 },
    { 1.52371034, 0.09339410, 1.84969142, -4.55343205, -23.94362959, 49.55953891,
      0.00001847, 0.00007882, -0.00813131, 19140.30268499, 0.44441088, -0.29257343 },
    { 5.20288700, 0.04838624, 1.30439695, 34.39644051, 14.72847983, 100.47390909,
      -0.00011607, -0.00013253, -0.00183714, 3034.74612775, 0.21252668, 0.20469106 },
    { 9.53667594, 0.05386179, 2.48599187, 49.95424423, 92.59887831, 113.66242448,
      -0.00125060, -0.00050991, 0.00193609, 1222.49362201, -0.41897216, -0.28867794 },
    { 19.18916464, 0.04725744, 0.77263783, 313.23810451, 170.95427630, 74.01692503,
      -0.00196176, -0.00004397, -0.00242939, 428.48202785, 0.40805281, 0.04240589 },
    { 30.06992276, 0.00859048, 1.77004347, -55.12002969, 44.96476227, 131.78422574,
      0.00026291, 0.00005105, 0.00035372, 218.45945325, -0.32241464, -0.00508664 },
};

static double norm360(double a) { a = fmod(a, 360.0); return a < 0 ? a + 360.0 : a; }
static double norm180(double a) { a = norm360(a); return a > 180.0 ? a - 360.0 : a; }

double ep_jd_unix(double unix_s) { return unix_s / 86400.0 + 2440587.5; }

double ep_kepler(double Mdeg, double e)
{
    if (!isfinite(Mdeg) || !isfinite(e) || e < 0 || e >= 1) return NAN;
    double M = norm180(Mdeg) * D2R, E = M + e * sin(M);
    double lo = -3.14159265358979323846, hi = -lo;
    /* E-e*sin(E) is monotone for e<1. Newton may overshoot at high e, so retain
     * its root bracket and bisect whenever the candidate leaves it. */
    for (int i = 0; i < 64; i++) {
        double f = E - e * sin(E) - M;
        if (fabs(f) < 2e-14 && fabs(f) <= 2e-13 * (1.0 - e * cos(E))) return E;
        if (f < 0) lo = E; else hi = E;
        double next = E - f / (1.0 - e * cos(E));
        if (!isfinite(next) || next <= lo || next >= hi) next = (lo + hi) * 0.5;
        E = next;
        if (hi - lo < 2e-14) break;
    }
    return E;
}

typedef struct { double a, e, cw, sw, cO, sO, cI, sI, wb; double L; } elems;
static void elems_at(int p, double jd, elems *o)
{
    const double *k = EL[p];
    double T = (jd - 2451545.0) / 36525.0;
    o->a = k[0] + k[6] * T; o->e = k[1] + k[7] * T;
    double I = k[2] + k[8] * T; o->L = k[3] + k[9] * T; o->wb = k[4] + k[10] * T;
    double O = k[5] + k[11] * T, w = o->wb - O;
    o->cw = cos(w * D2R); o->sw = sin(w * D2R); o->cO = cos(O * D2R); o->sO = sin(O * D2R);
    o->cI = cos(I * D2R); o->sI = sin(I * D2R);
}
static ep_vec rot(const elems *o, double xp, double yp)
{
    ep_vec v = {
        (o->cw * o->cO - o->sw * o->sO * o->cI) * xp + (-o->sw * o->cO - o->cw * o->sO * o->cI) * yp,
        (o->cw * o->sO + o->sw * o->cO * o->cI) * xp + (-o->sw * o->sO + o->cw * o->cO * o->cI) * yp,
        (o->sw * o->sI) * xp + (o->cw * o->sI) * yp };
    return v;
}

ep_vec ep_helio_anomaly(int p, double jd, double *anomaly)
{
    elems o; elems_at(p, jd, &o);
    double E = ep_kepler(o.L - o.wb, o.e);
    if(anomaly) {
        double a=fmod(E,2.0*3.14159265358979323846);
        *anomaly=a<0?a+2.0*3.14159265358979323846:a;
    }
    return rot(&o, o.a * (cos(E) - o.e), o.a * sqrt(1.0 - o.e * o.e) * sin(E));
}
ep_vec ep_helio(int p,double jd) { return ep_helio_anomaly(p,jd,0); }

/* Phase reduction stays double: narrowing a Julian date or a many-turn
 * longitude first loses visible precision. All bounded kernels use the S3's
 * native float arithmetic. Conservative provisional 1.5 us per float trig;
 * shared board calibration retains the unseparated memory/arithmetic residual. */
static float preview_sin(float x) { GFX_COST_PX(GFX_COST_ORR_PRE_CPU,1500);return sinf(x); }
static float preview_cos(float x) { GFX_COST_PX(GFX_COST_ORR_PRE_CPU,1500);return cosf(x); }
static float preview_kepler(double degrees,float e)
{
    if(e>.8f)return (float)ep_kepler(degrees,e);
    float M=(float)(norm180(degrees)*D2R),E=M+e*preview_sin(M);
    for(int i=0;i<8;i++) {
        float d=(M-(E-e*preview_sin(E)))/(1-e*preview_cos(E));
        E+=d;if(fabsf(d)<2e-7f)break;
    }
    if(!isfinite(E) || fabsf(E-e*sinf(E)-(float)(norm180(degrees)*D2R))>2e-6f)
        return (float)ep_kepler(degrees,e);
    return E;
}
ep_vec ep_helio_preview(int p,double jd,double *anomaly)
{
    const double *k=EL[p];double T=(jd-2451545.0)/36525.0;
    float a=(float)(k[0]+k[6]*T),e=(float)(k[1]+k[7]*T);
    double L=k[3]+k[9]*T,wb=k[4]+k[10]*T,O=k[5]+k[11]*T;
    float w=(float)((wb-O)*D2R),node=(float)(O*D2R),inc=(float)((k[2]+k[8]*T)*D2R);
    float cw=preview_cos(w),sw=preview_sin(w),cO=preview_cos(node),sO=preview_sin(node),cI=preview_cos(inc),sI=preview_sin(inc);
    float E=preview_kepler(L-wb,e);
    if(anomaly)*anomaly=E<0?(double)E+2*3.14159265358979323846:E;
    float xp=a*(preview_cos(E)-e),yp=a*sqrtf(1-e*e)*preview_sin(E);
    ep_vec v={(cw*cO-sw*sO*cI)*xp+(-sw*cO-cw*sO*cI)*yp,
              (cw*sO+sw*cO*cI)*xp+(-sw*sO+cw*cO*cI)*yp,sw*sI*xp+cw*sI*yp};
    return v;
}

double ep_ecc_anomaly(int p, double jd)
{
    /* An anomaly needs no orbital-plane rotation. elems_at also evaluates
     * six double sin/cos calls; doing that twice per visible trail per frame
     * is particularly expensive on the S3's software double math. Keep the
     * original arithmetic/order for the three elements actually consumed. */
    const double *k=EL[p];
    double T=(jd-2451545.0)/36525.0;
    double e=k[1]+k[7]*T,L=k[3]+k[9]*T,wb=k[4]+k[10]*T;
    double E = ep_kepler(L - wb, e);
    E = fmod(E, 2.0 * 3.14159265358979323846);
    return E < 0 ? E + 2.0 * 3.14159265358979323846 : E;
}

void ep_orbit(int p, double jd, int n, float *xyz)
{
    elems o; elems_at(p, jd, &o);
    double b = o.a * sqrt(1.0 - o.e * o.e);
    for (int i = 0; i < n; i++) {
        double E = (double)i / n * 2.0 * 3.14159265358979323846;
        ep_vec v = rot(&o, o.a * (cos(E) - o.e), b * sin(E));
        xyz[i * 3] = (float)v.x; xyz[i * 3 + 1] = (float)v.y; xyz[i * 3 + 2] = (float)v.z;
    }
}

static double len(ep_vec v) { return sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }
static ep_vec sub(ep_vec a, ep_vec b) { ep_vec v = { a.x - b.x, a.y - b.y, a.z - b.z }; return v; }
static double angle(ep_vec a, ep_vec b)
{
    double c = (a.x * b.x + a.y * b.y + a.z * b.z) / (len(a) * len(b));
    return acos(c < -1 ? -1 : c > 1 ? 1 : c) * R2D;
}

void ep_sky_at(double jd, ep_sky *s)
{
    memset(s, 0, sizeof *s);
    s->jd = jd;
    ep_vec E = ep_helio(EP_EARTH, jd), sun = { -E.x, -E.y, -E.z };
    s->earth = E;
    s->sun_lon = norm360(atan2(sun.y, sun.x) * R2D);
    for (int p = 0; p < EP_N; p++) {
        ep_body *b = &s->b[p];
        b->helio = p == EP_EARTH ? E : ep_helio(p, jd);
        if (p == EP_EARTH) continue;
        ep_vec g = sub(b->helio, E);
        b->dist = len(g);
        b->lon = norm360(atan2(g.y, g.x) * R2D);
        b->lat = asin(g.z / b->dist) * R2D;
        b->elong = angle(g, sun);
        b->evening = norm180(b->lon - s->sun_lon) > 0;
        ep_vec toE = sub(E, b->helio), toS = { -b->helio.x, -b->helio.y, -b->helio.z };
        b->lit = (1.0 + cos(angle(toE, toS) * D2R)) / 2.0;
    }
}

/* ---- events: one sky a day, extrema found on three samples and refined with a parabola ---- */
static int bright(int p) { return p == EP_MERCURY || p == EP_VENUS || p == EP_MARS || p == EP_JUPITER || p == EP_SATURN; }
static int outer(int p) { return p >= EP_MARS; }

static double refine(double t0, double f0, double f1, double f2, double *v)
{
    double den = f0 - 2 * f1 + f2, off = den == 0 ? 0 : 0.5 * (f0 - f2) / den;
    off = off < -1 ? -1 : off > 1 ? 1 : off;
    if (v) *v = f1 - 0.25 * (f0 - f2) * off;
    return t0 + 1 + off;
}

static int push(ep_event *out, int *n, int cap, const ep_event *e, int min_rank)
{
    if (e->rank < min_rank || *n >= cap) return 0;
    out[(*n)++] = *e;
    return 1;
}

static int cmp_jd(const void *a, const void *b)
{
    double d = ((const ep_event *)a)->jd - ((const ep_event *)b)->jd;
    return d < 0 ? -1 : d > 0;
}

void ep_scan_begin(ep_scan *sc, double jd0, int days, int min_rank, ep_event *out, int cap)
{
    memset(sc, 0, sizeof *sc);
    sc->jd0 = jd0; sc->days = days; sc->min_rank = min_rank; sc->out = out; sc->cap = cap;
    ep_sky_at(jd0 - 1, &sc->S[0]);
    ep_sky_at(jd0, &sc->S[1]);
}

int ep_scan_step(ep_scan *sc, int max_days)
{
    ep_event *out = sc->out;
    int cap = sc->cap, min_rank = sc->min_rank;
    for (int step = 0; step < max_days && sc->i <= sc->days; step++, sc->i++) {
        int i = sc->i;
        ep_sky_at(sc->jd0 + i + 1, &sc->S[(i + 2) % 3]);
        const ep_sky *a = &sc->S[i % 3], *b = &sc->S[(i + 1) % 3], *c = &sc->S[(i + 2) % 3];
        double t0 = a->jd;
        for (int p = 0; p < EP_N; p++) {
            if (p == EP_EARTH) continue;
            double e0 = a->b[p].elong, e1 = b->b[p].elong, e2 = c->b[p].elong;
            ep_event ev; memset(&ev, 0, sizeof ev); ev.a = (uint8_t)p; ev.b = -1;
            if (e1 >= e0 && e1 > e2) {
                if (outer(p) && e1 > 170) {
                    ev.kind = EV_OPPOSITION; ev.jd = refine(t0, e0, e1, e2, 0); ev.value = (float)e1;
                    ev.rank = (p == EP_MARS || p == EP_JUPITER || p == EP_SATURN) ? 3 : 2;
                    push(out, &sc->n, cap, &ev, min_rank);
                } else if (!outer(p)) {
                    double v; ev.kind = EV_ELONGATION; ev.jd = refine(t0, e0, e1, e2, &v); ev.value = (float)v;
                    ev.evening = (uint8_t)b->b[p].evening; ev.rank = p == EP_VENUS ? 3 : 2;
                    push(out, &sc->n, cap, &ev, min_rank);
                }
            }
            if (e1 <= e0 && e1 < e2 && e1 < 12) {
                ev.kind = EV_SOLAR; ev.jd = refine(t0, e0, e1, e2, 0); ev.value = (float)e1;
                ev.inferior = (uint8_t)(!outer(p) && b->b[p].dist < 1); ev.rank = 1;
                push(out, &sc->n, cap, &ev, min_rank);
            }
        }
        for (int p = 0; p < EP_N; p++) for (int q = p + 1; q < EP_N; q++) {
            if (p == EP_EARTH || q == EP_EARTH) continue;
            double lim = bright(p) && bright(q) ? 3.0 : 1.5;
            double s0 = angle(sub(a->b[p].helio, a->earth), sub(a->b[q].helio, a->earth));
            double s1 = angle(sub(b->b[p].helio, b->earth), sub(b->b[q].helio, b->earth));
            double s2 = angle(sub(c->b[p].helio, c->earth), sub(c->b[q].helio, c->earth));
            if (s1 <= s0 && s1 < s2 && s1 < lim) {
                ep_event ev; memset(&ev, 0, sizeof ev);
                double v, vis = b->b[p].elong < b->b[q].elong ? b->b[p].elong : b->b[q].elong;
                ev.kind = EV_CONJUNCTION; ev.a = (uint8_t)p; ev.b = (int8_t)q; ev.jd = refine(t0, s0, s1, s2, &v);
                ev.value = (float)(v < 0.05 ? 0.05 : v); ev.evening = (uint8_t)b->b[p].evening;
                ev.rank = vis < 15 ? 1 : (bright(p) && bright(q) ? 3 : 2);
                push(out, &sc->n, cap, &ev, min_rank);
            }
        }
        for (int side = 0; side < 2; side++) {
            uint16_t m = 0; int cnt = 0, br = 0;
            for (int p = 0; p < EP_N; p++) {
                if (p == EP_EARTH || b->b[p].elong <= 18 || b->b[p].evening != (side == 0)) continue;
                m |= (uint16_t)(1u << p); cnt++; br += bright(p);
            }
            if (cnt >= 4 && br >= 3) {
                if (sc->run_on[side]) continue;          /* still the same line-up (members may come and go) */
                ep_event *r = &sc->run_ev[side];
                memset(r, 0, sizeof *r);
                r->kind = EV_LINEUP; r->jd = b->jd; r->mask = m; r->n = (uint8_t)cnt;
                r->evening = (uint8_t)(side == 0); r->b = -1; r->rank = cnt >= 5 ? 3 : 2;
                sc->run_on[side] = 1;
            } else if (sc->run_on[side]) { push(out, &sc->n, cap, &sc->run_ev[side], min_rank); sc->run_on[side] = 0; }
        }
    }
    if (sc->i <= sc->days) return 0;
    if (!sc->done) {
        for (int side = 0; side < 2; side++) if (sc->run_on[side]) push(out, &sc->n, cap, &sc->run_ev[side], min_rank);
        qsort(out, (size_t)sc->n, sizeof *out, cmp_jd);
        sc->done = 1;
    }
    return 1;
}

int ep_events(double jd0, int days, int min_rank, ep_event *out, int cap)
{
    ep_scan sc;
    ep_scan_begin(&sc, jd0, days, min_rank, out, cap);
    while (!ep_scan_step(&sc, 64)) { }
    return sc.n;
}

void ep_event_title(const ep_event *e, char *out, int n)
{
    switch (e->kind) {
    case EV_OPPOSITION:  snprintf(out, (size_t)n, "%s OPPOSITION", ep_name[e->a]); break;
    case EV_CONJUNCTION: snprintf(out, (size_t)n, "%s + %s", ep_name[e->a], ep_name[e->b]); break;
    case EV_ELONGATION:  snprintf(out, (size_t)n, "%s MAX ELONG.", ep_name[e->a]); break;
    case EV_SOLAR:       snprintf(out, (size_t)n, "%s %s CONJ.", ep_name[e->a], e->inferior ? "INF." : "SUP."); break;
    default:             snprintf(out, (size_t)n, "%d-PLANET LINE-UP", e->n); break;
    }
}

/* ===================== beyond the planets ===================== */
ep_body ep_observe(ep_vec h, ep_vec E)
{
    ep_body b; memset(&b, 0, sizeof b);
    ep_vec sun = { -E.x, -E.y, -E.z }, g = sub(h, E);
    double sun_lon = norm360(atan2(sun.y, sun.x) * R2D);
    b.helio = h; b.dist = len(g);
    b.lon = norm360(atan2(g.y, g.x) * R2D); b.lat = asin(g.z / b.dist) * R2D;
    b.elong = angle(g, sun);
    b.evening = norm180(b.lon - sun_lon) > 0;
    ep_vec toE = sub(E, h), toS = { -h.x, -h.y, -h.z };
    b.lit = (1.0 + cos(angle(toE, toS) * D2R)) / 2.0;
    return b;
}

static ep_vec rot_iow(double xp, double yp, double i, double node, double w)
{
    double cw = cos(w * D2R), sw = sin(w * D2R), cO = cos(node * D2R), sO = sin(node * D2R), cI = cos(i * D2R), sI = sin(i * D2R);
    ep_vec v = { (cw * cO - sw * sO * cI) * xp + (-sw * cO - cw * sO * cI) * yp,
                 (cw * sO + sw * cO * cI) * xp + (-sw * sO + cw * cO * cI) * yp,
                 (sw * sI) * xp + (cw * sI) * yp };
    return v;
}

#define GK 0.01720209895
void ep_conic_prepare(ep_conic_cache *c,const ep_conic *o)
{
    double cw=cos(o->w*D2R),sw=sin(o->w*D2R),cO=cos(o->node*D2R),sO=sin(o->node*D2R),cI=cos(o->i*D2R),sI=sin(o->i*D2R);
    c->el=o;
    c->px[0]=cw*cO-sw*sO*cI;c->py[0]=-sw*cO-cw*sO*cI;
    c->px[1]=cw*sO+sw*cO*cI;c->py[1]=-sw*sO+cw*cO*cI;
    c->px[2]=sw*sI;c->py[2]=cw*sI;
    if(fabs(o->e-1)<1e-3) { c->a=c->b=0;c->n=sqrt(2.0)*pow(o->q,1.5); }
    else {
        c->a=o->q/(o->e<1?1-o->e:o->e-1);
        c->b=c->a*sqrt(o->e<1?1-o->e*o->e:o->e*o->e-1);
        c->n=GK/pow(c->a,1.5);
    }
}
ep_vec ep_conic_cached(const ep_conic_cache *c,double jd)
{
    const ep_conic *o=c->el;
    double t=jd-o->tp,e=o->e,xp,yp;
    if(fabs(e-1)<1e-3) {
        double W=3*GK*t/c->n,Y=cbrt(W/2+sqrt(W*W/4+1));
        double sv=Y-1/Y,r=o->q*(1+sv*sv),nu=2*atan(sv);
        xp=r*cos(nu);yp=r*sin(nu);
    } else if(e<1) {
        double E=ep_kepler(c->n*t*R2D,e);
        xp=c->a*(cos(E)-e);yp=c->b*sin(E);
    } else {
        double M=c->n*t,H=asinh(M/e);
        for(int k=0;k<30;k++) { double d=(e*sinh(H)-H-M)/(e*cosh(H)-1);H-=d;if(fabs(d)<1e-10)break; }
        xp=c->a*(e-cosh(H));yp=c->b*sinh(H);
    }
    ep_vec v={c->px[0]*xp+c->py[0]*yp,c->px[1]*xp+c->py[1]*yp,c->px[2]*xp+c->py[2]*yp};
    return v;
}
ep_vec ep_conic_preview(const ep_conic_cache *c,double jd)
{
    float e=(float)c->el->e,xp,yp;
    double t=jd-c->el->tp;
    if(fabs(c->el->e-1)<1e-3) {
        /* Barker's equation; double cbrt retains accuracy in Y-1/Y. The
         * half-angle identities avoid atan + two double trig evaluations. */
        double W=3*GK*t/c->n,Y=cbrt(W/2+sqrt(W*W/4+1)),s=Y-1/Y;
        xp=(float)(c->el->q*(1-s*s));yp=(float)(2*c->el->q*s);
    } else if(e<1) {
        double phase=c->n*t*R2D;
        /* Ill-conditioned near-perihelion cases keep the exact reference. */
        if(e>.98f && fabs(norm180(phase))<.00001)return ep_conic_cached(c,jd);
        float E=preview_kepler(phase,e);
        if(e>.98f && fabsf(E-e*preview_sin(E)-(float)(norm180(phase)*D2R))>2e-6f)
            return ep_conic_cached(c,jd);
        xp=(float)c->a*(preview_cos(E)-e);yp=(float)c->b*preview_sin(E);
    } else {
        float M=(float)(c->n*t),H=asinhf(M/e);
        GFX_COST_PX(GFX_COST_ORR_PRE_CPU,3000);
        if(fabsf(M)<.1f || fabsf(M)>100000)return ep_conic_cached(c,jd);
        int converged=0;
        for(int k=0;k<20;k++) {
            float sh=sinhf(H),ch=coshf(H),d=(e*sh-H-M)/(e*ch-1);
            GFX_COST_PX(GFX_COST_ORR_PRE_CPU,6000);
            H-=d;if(fabsf(d)<1e-6f) { converged=1;break; }
        }
        if(!converged)return ep_conic_cached(c,jd);
        xp=(float)c->a*(e-coshf(H));yp=(float)c->b*sinhf(H);
        GFX_COST_PX(GFX_COST_ORR_PRE_CPU,6000);
    }
    ep_vec v={(float)c->px[0]*xp+(float)c->py[0]*yp,
              (float)c->px[1]*xp+(float)c->py[1]*yp,
              (float)c->px[2]*xp+(float)c->py[2]*yp};
    return v;
}
ep_vec ep_conic_pos(const ep_conic *o, double jd)
{
    double t = jd - o->tp, e = o->e, q = o->q, xp, yp;
    if (fabs(e - 1.0) < 1e-3) {                       /* near-parabolic: Barker's equation */
        double W = 3 * GK * t / (sqrt(2.0) * pow(q, 1.5)), Y = cbrt(W / 2 + sqrt(W * W / 4 + 1));
        double sv = Y - 1 / Y, r = q * (1 + sv * sv), nu = 2 * atan(sv);
        xp = r * cos(nu); yp = r * sin(nu);
    } else if (e < 1) {
        double a = q / (1 - e), E = ep_kepler(GK / pow(a, 1.5) * t * R2D, e);
        xp = a * (cos(E) - e); yp = a * sqrt(1 - e * e) * sin(E);
    } else {
        double a = q / (e - 1), M = GK / pow(a, 1.5) * t, H = asinh(M / e);
        for (int k = 0; k < 30; k++) { double d = (e * sinh(H) - H - M) / (e * cosh(H) - 1); H -= d; if (fabs(d) < 1e-10) break; }
        xp = a * (e - cosh(H)); yp = a * sqrt(e * e - 1) * sinh(H);
    }
    return rot_iow(xp, yp, o->i, o->node, o->w);
}

ep_vec ep_sat_pos(const ep_sat *o, double jd)
{
    double e = o->e, E = ep_kepler(o->M + 360.0 * (jd - o->epoch) / o->P, e);
    return rot_iow(o->a * (cos(E) - e), o->a * sqrt(1 - e * e) * sin(E), o->i, o->node, o->w);
}

static double sd(double x) { return sin(x * D2R); }
static double cd(double x) { return cos(x * D2R); }
void ep_moon(double jd, double *lon, double *lat, double *km)
{
    double d = jd - 2451545.0;
    double L = 218.316 + 13.176396 * d, M = 134.963 + 13.064993 * d, F = 93.272 + 13.229350 * d;
    double D = 297.850 + 12.190749 * d, Ms = 357.529 + 0.98560028 * d;
    *lon = norm360(L + 6.289 * sd(M) + 1.274 * sd(2 * D - M) + 0.658 * sd(2 * D) + 0.214 * sd(2 * M) - 0.186 * sd(Ms) - 0.114 * sd(2 * F));
    *lat = 5.128 * sd(F) + 0.281 * sd(M + F) - 0.278 * sd(F - M) - 0.173 * sd(F - 2 * D);
    *km = 385001 - 20905 * cd(M) - 3699 * cd(2 * D - M) - 2956 * cd(2 * D);
}
double ep_moon_lit(double jd, int *waxing)
{
    double lon, lat, km;
    ep_moon(jd, &lon, &lat, &km);
    ep_vec E = ep_helio(EP_EARTH, jd);
    double sl = norm360(atan2(-E.y, -E.x) * R2D);
    double el = acos(cd(lat) * cd(lon - sl)) * R2D;
    if (waxing) *waxing = norm180(lon - sl) > 0;
    return (1 - cd(el)) / 2;
}

#define EPS_OBL 23.43928
ep_vec ep_ecl2eq(ep_vec v)
{
    ep_vec q = { v.x, cd(EPS_OBL) * v.y - sd(EPS_OBL) * v.z, sd(EPS_OBL) * v.y + cd(EPS_OBL) * v.z };
    return q;
}

/* IAU WGCCRE pole (RA, Dec) and prime meridian W0 + rate deg/day; pole drift ignored */
static const double IAU[EPB_N][4] = {
    { 281.0103, 61.4155, 329.5988, 6.1385108 }, { 272.76, 67.16, 160.20, -1.4813688 },
    { 0, 90, 190.147, 360.9856235 }, { 317.269202, 54.432516, 176.049863, 350.891982443297 },
    { 268.056595, 64.495303, 284.95, 870.5360000 }, { 40.589, 83.537, 38.90, 810.7939024 },
    { 257.311, -15.175, 203.81, -501.1600928 }, { 299.36, 43.46, 249.978, 541.1397757 },
    { 286.13, 63.87, 84.176, 14.1844000 },                                   /* Sun */
    { 269.9949, 66.5392, 38.3213, 13.17635815 },                             /* Moon */
    { 132.993, -6.163, 302.695, 56.3625225 },                                /* Pluto */
};
static void pole_rot(int body, ep_vec q, double *x, double *y, double *z)
{
    double ra = IAU[body][0], de = IAU[body][1];
    double a1 = (90 + ra) * D2R, c1 = cos(a1), s1 = sin(a1);
    double x1 = c1 * q.x + s1 * q.y, y1 = -s1 * q.x + c1 * q.y, z1 = q.z;
    double a2 = (90 - de) * D2R, c2 = cos(a2), s2 = sin(a2);
    *x = x1; *y = c2 * y1 + s2 * z1; *z = -s2 * y1 + c2 * z1;
}
static void body_fixed(int body, double jd, ep_vec veq, float *lat, float *lon)
{
    double x, y, z;
    pole_rot(body, veq, &x, &y, &z);
    double W = (IAU[body][2] + IAU[body][3] * (jd - 2451545.0)) * D2R, c3 = cos(W), s3 = sin(W);
    double x3 = c3 * x + s3 * y, y3 = -s3 * x + c3 * y, n = sqrt(x3 * x3 + y3 * y3 + z * z);
    *lat = (float)(asin(z / n) * R2D); *lon = (float)norm360(atan2(y3, x3) * R2D);
}
void ep_pole_frame(int body, ep_vec v, float *px, float *py)
{
    double x, y, z;
    pole_rot(body, ep_ecl2eq(v), &x, &y, &z);
    *px = (float)x; *py = (float)y;
}
void ep_geometry(int body, double jd, ep_vec pluto_helio, float *obs_lat, float *obs_lon, float *sun_lat, float *sun_lon)
{
    ep_vec E = ep_helio(EP_EARTH, jd), P;
    if (body == EP_EARTH) P = E;
    else if (body == EPB_MOON) {
        double lon, lat, km; ep_moon(jd, &lon, &lat, &km);
        double au = km / 149597870.7;
        P.x = E.x + au * cd(lat) * cd(lon); P.y = E.y + au * cd(lat) * sd(lon); P.z = E.z + au * sd(lat);
    } else if (body == EPB_SUN) { P.x = P.y = P.z = 0; }
    else if (body == EPB_PLUTO) P = pluto_helio;
    else P = ep_helio(body, jd);
    if (body == EPB_SUN) { *sun_lat = 0; *sun_lon = 0; }
    else { ep_vec ts = { -P.x, -P.y, -P.z }; body_fixed(body, jd, ep_ecl2eq(ts), sun_lat, sun_lon); }
    if (body == EP_EARTH) { *obs_lat = 18; *obs_lon = (float)norm360(*sun_lon + 40); }   /* no location yet: just past the day side */
    else body_fixed(body, jd, ep_ecl2eq(sub(E, P)), obs_lat, obs_lon);
}
