/* motion.c — see motion.h */
#include "motion.h"
#include "nos.h"
#include <math.h>
#include <limits.h>

#define TWO_PI 6.28318530718f

float m_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
float m_lerp(float a, float b, float t) { return a + (b - a) * t; }
float m_unlerp(float a, float b, float x) { return b != a ? (x - a) / (b - a) : 0.0f; }
float m_smoothstep(float e0, float e1, float x)
{
    float t = m_clamp(m_unlerp(e0, e1, x), 0, 1);
    return t * t * (3 - 2 * t);
}
float m_stagger(float p, int i, int n, float span)
{
    if (n <= 1) return m_clamp(p / (span > 0 ? span : 1), 0, 1);
    float start = (1.0f - span) * (float)i / (float)(n - 1);
    return m_clamp((p - start) / span, 0, 1);
}
float m_hash01(unsigned a, unsigned b)
{
    unsigned h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (float)(h >> 8) / 16777216.0f;
}

/* ---------------- whole frames ---------------- */
int m_frame_k(nos_time_t t0)
{
    double frames=floor((nos_t-t0)*M_FPS+0.5);
    return frames>=INT_MAX?INT_MAX:frames<=INT_MIN?INT_MIN:(int)frames;
}
int m_stagger_k(int k, int i, int step)
{
    if (i < 0) i = 0;
    if (i > 7) i = 7;                          /* at most 8 distinct starts (R9) */
    return k - i * step;
}
float m_ink(float t)
{
    t = m_clamp(t, 0, 1);
    float k = t * gfx_fast_sqrt(t);            /* ~t^1.5: even steps of perceived brightness */
    return floorf(k * 16.0f + 0.5f) / 16.0f;
}

const mframes MF_SNAP3  = { 3, { 60, 90, 100 } };
const mframes MF_MICRO4 = { 4, { 45, 78, 95, 100 } };
const mframes MF_MICRO6 = { 6, { 28, 53, 73, 87, 96, 100 } };
float mf_at(const mframes *tab, int k)
{
    if (!tab || k >= tab->n) return 1.0f;
    if (k <= 0) return 0.0f;
    return tab->pct[k - 1] * 0.01f;
}

float m_cut(float from, float to, int k)
{
    if (k <= 0) return from;
    if (k >= M_CUT_FRAMES) return to;
    float d = to - from, ad = fabsf(d), sg = d < 0 ? -1.0f : 1.0f;
    float lead = ad * 0.2f < 20.0f ? ad * 0.2f : 20.0f;
    float over = ad * 0.1f < 4.0f ? ad * 0.1f : 4.0f;   /* 4 px past, then 2, 1, 0 */
    switch (k) {
    case 1:  return from + sg * (ad >= 4.0f ? 1.0f : 0.0f);
    case 2:  return from + sg * lead;
    case 3:  return to + sg * over;
    case 4:  return to + sg * over * 0.5f;
    default: return to + sg * over * 0.25f;
    }
}

/* ---------------- springs ----------------
 * Exact solution of x'' = -k (x - target) - c x' over each step, so the result does not
 * depend on the frame rate (a 2-frame catch-up equals two 1-frame steps). */
void msp_init(mspring *s, float x, float response, float damping)
{
    s->x = s->target = x;
    s->v = 0;
    s->response = response > 0.02f ? response : 0.02f;
    s->damping = damping;
    s->eps = MSP_EPS;
    s->snap = 0;
    s->moving = 0;
}
void msp_init_px(mspring *s, float x, float response, float damping)
{
    msp_init(s, x, response, damping);
    s->snap = MSP_SNAP_PX;
}
void msp_snap(mspring *s, float snap) { s->snap = snap > 0 ? snap : 0; }
static void eps_for(mspring *s, float target)
{
    float d = fabsf(target - s->x) * 0.01f;
    if (d <= 0) return;                      /* no distance: keep the last threshold */
    s->eps = d > MSP_EPS ? MSP_EPS : d < 0.0005f ? 0.0005f : d;
}
void msp_to(mspring *s, float target) { eps_for(s, target); s->target = target; s->moving = 1; }
void msp_set(mspring *s, float x) { s->x = s->target = x; s->v = 0; s->moving = 0; }
void msp_kick(mspring *s, float target, float v) { eps_for(s, target); s->target = target; s->v = v; s->moving = 1; }

int msp_step(mspring *s, float dt)
{
    if (!s->moving && s->x == s->target && s->v == 0) return 0;
    float w0 = TWO_PI / s->response;          /* undamped angular frequency */
    float z = s->damping;
    float x0 = s->x - s->target, v0 = s->v;
    float x, v;
    if (z < 0.999f) {                          /* under-damped */
        float wd = w0 * sqrtf(1 - z * z);
        float e = expf(-z * w0 * dt);
        float c = cosf(wd * dt), sn = sinf(wd * dt);
        float B = (v0 + z * w0 * x0) / wd;
        x = e * (x0 * c + B * sn);
        /* derivative of e*(x0 c + B s): -z w0 x + e (-x0 wd s + B wd c) */
        v = -z * w0 * x + e * (-x0 * wd * sn + B * wd * c);
    } else {                                   /* critically damped */
        float e = expf(-w0 * dt);
        float B = v0 + w0 * x0;
        x = (x0 + B * dt) * e;
        v = (B - w0 * (x0 + B * dt)) * e;
    }
    float step = fabsf(x - x0);
    s->x = s->target + x;
    s->v = v;
    int settle;
    if (s->snap > 0) {
        /* unit-aware crisp ending (R5): within one snap unit, or within 1.5 while already
         * creeping under 1.5 a frame, and slow (not an underdamped pass through the target).
         * Per frame, not per dt: a late frame's bigger step must not hide the creep. */
        float sn = s->snap, ax = fabsf(x), fr = dt * M_FPS;
        settle = (ax <= sn || (ax <= 1.5f * sn && step < 1.5f * sn * (fr > 1 ? fr : 1))) &&
                 fabsf(v) <= 90.0f * sn;
    } else settle = fabsf(x) < s->eps && fabsf(v) < s->eps * 20.0f;
    if (settle) {
        s->x = s->target;
        s->v = 0;
        s->moving = 0;
        return 0;
    }
    s->moving = 1;
    return 1;
}

/* ---------------- momentum scroll ---------------- */
#define MSC_TAIL_V 105.0f    /* px/s: below 3.5 px a frame the fling steps whole pixels */
#define MSC_STOP_V 45.0f     /* px/s: below 1.5 px a frame it stops (no creeping tail, R5) */
static float s_decay_tau(void) { return -1.0f / (1000.0f * logf(DECAY_RATE)); } /* seconds */

float decay_project(float x, float v) { return x + v * s_decay_tau(); }

float rubber_band(float over, float dim)
{
    /* iOS: f(x) = (1 - 1 / (x * c / d + 1)) * d, c = 0.55 */
    float c = 0.55f, a = fabsf(over);
    float r = (1.0f - 1.0f / (a * c / dim + 1.0f)) * dim;
    return over < 0 ? -r : r;
}

void msc_init(mscroll *s, float min, float max, float snap)
{
    s->x = min; s->v = 0;
    s->min = min; s->max = max > min ? max : min;
    s->snap = snap;
    s->snap_free = 0;
    s->dragging = s->moving = s->springing = 0;
    s->drag_x0 = s->drag_off0 = 0;
    msp_init_px(&s->sp, s->x, SPRING_PAGE);     /* content px: snaps within 1 px */
}
void msc_bounds(mscroll *s, float min, float max)
{
    s->min = min; s->max = max > min ? max : min;
    if (!s->dragging && (s->x < s->min || s->x > s->max)) msc_animate_to(s, m_clamp(s->x, s->min, s->max));
}
void msc_grab(mscroll *s)
{
    s->dragging = 1; s->moving = 0; s->springing = 0; s->v = 0;
    s->drag_off0 = s->x;
}
void msc_drag(mscroll *s, float d)
{
    /* content follows the finger 1:1 inside bounds, with rubber band beyond */
    float raw = s->drag_off0 - d;
    float dim = 466.0f;
    if (raw < s->min) raw = s->min + rubber_band(raw - s->min, dim);
    else if (raw > s->max) raw = s->max + rubber_band(raw - s->max, dim);
    s->x = raw;
}
static float snap_of(const mscroll *s, float x)
{
    if (s->snap <= 0) return x;
    float k = floorf((x - s->min) / s->snap + 0.5f);
    return m_clamp(s->min + k * s->snap, s->min, s->max);
}
void msc_release(mscroll *s, float v)
{
    s->dragging = 0;
    if (s->x < s->min || s->x > s->max) {            /* return from rubber band */
        s->sp.response = 0.45f; s->sp.damping = 1.0f;
        s->sp.x = s->x; s->sp.v = v;
        msp_to(&s->sp, s->x < s->min ? s->min : s->max);
        s->springing = 1; s->moving = 1;
        return;
    }
    if (s->snap > 0) {                               /* paging: project, then spring */
        float rest = decay_project(s->x, v);
        float tgt = snap_of(s, rest);
        /* never skip more than one page per flick for pagers with page-size snaps */
        float cur = snap_of(s, s->x);
        if (!s->snap_free) {
            if (tgt > cur + s->snap) tgt = cur + s->snap;
            if (tgt < cur - s->snap) tgt = cur - s->snap;
            if (fabsf(v) > 300.0f && tgt == cur) tgt = m_clamp(cur + (v > 0 ? s->snap : -s->snap), s->min, s->max);
        }
        s->sp.response = 0.42f; s->sp.damping = 0.95f;
        s->sp.x = s->x; s->sp.v = v;
        msp_to(&s->sp, tgt);
        s->springing = 1; s->moving = 1;
        return;
    }
    s->v = v; s->moving = fabsf(v) > MSC_STOP_V; s->springing = 0;
}
void msc_jump(mscroll *s, float x) { s->x = x; s->v = 0; s->moving = s->springing = 0; }
void msc_animate_to(mscroll *s, float x)
{
    s->sp.response = 0.45f; s->sp.damping = 1.0f;
    s->sp.x = s->x; s->sp.v = s->v;
    msp_to(&s->sp, m_clamp(x, s->min, s->max));
    s->springing = 1; s->moving = 1;
}
int msc_step(mscroll *s, float dt)
{
    if (s->dragging) return 1;
    if (!s->moving) return 0;
    if (s->springing) {
        int m = msp_step(&s->sp, dt);
        s->x = s->sp.x; s->v = s->sp.v;
        if (!m) { s->moving = 0; s->springing = 0; }
        return m;
    }
    /* free decay, analytic over dt */
    float tau = s_decay_tau();
    float e = expf(-dt / tau);
    float d = s->v * tau * (1.0f - e);
    if (fabsf(s->v) < MSC_TAIL_V) {
        /* the slow tail in whole pixels on a whole-pixel position (3, then 2 px a frame):
         * a 1.33 px/frame drift steps 1, 1, 2 and shimmers on thin strokes (R6) */
        s->x = floorf(s->x + 0.5f);
        d = floorf(d + 0.5f);
    }
    s->v *= e;
    s->x += d;
    if (s->x < s->min || s->x > s->max) {
        /* hit an edge mid-fling: bounce back with a spring carrying the velocity,
         * the iOS feel (a soft overshoot proportional to speed) */
        float edge = s->x < s->min ? s->min : s->max;
        s->sp.response = 0.40f; s->sp.damping = 1.0f;
        s->sp.x = s->x; s->sp.v = s->v * 0.6f;
        msp_to(&s->sp, edge);
        s->springing = 1;
        return 1;
    }
    if (fabsf(s->v) < MSC_STOP_V) { s->v = 0; s->moving = 0; return 0; }
    return 1;
}

/* ---------------- cubic bezier ---------------- */
const bezier EASE_STEADY   = { 0.61f, 1.0f, 0.88f, 1.0f };
const bezier EASE_LINEAR   = { 0.0f, 0.0f, 1.0f, 1.0f };
const bezier EASE_STANDARD = { 0.2f, 0.0f, 0.0f, 1.0f };
const bezier EASE_DECEL    = { 0.05f, 0.7f, 0.1f, 1.0f };
const bezier EASE_ACCEL    = { 0.3f, 0.0f, 0.8f, 0.15f };
const bezier EASE_INOUT    = { 0.65f, 0.0f, 0.35f, 1.0f };
const bezier EASE_MECH     = { 0.7f, 0.0f, 0.2f, 1.0f };

static float bz(float a, float b, float t)       /* 1D bezier with p0=0, p3=1 */
{
    float u = 1 - t;
    return 3 * u * u * t * a + 3 * u * t * t * b + t * t * t;
}
static float bzd(float a, float b, float t)
{
    float u = 1 - t;
    return 3 * u * u * a + 6 * u * t * (b - a) + 3 * t * t * (1 - b);
}
float bezier_ease(const bezier *b, float x)
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    if (b->x1 == b->y1 && b->x2 == b->y2) return x;   /* the identity (EASE_LINEAR) */
    float t = x;
    for (int i = 0; i < 6; i++) {              /* Newton on x(t) = x */
        float d = bzd(b->x1, b->x2, t);
        if (fabsf(d) < 1e-5f) break;
        t -= (bz(b->x1, b->x2, t) - x) / d;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
    }
    float lo = 0, hi = 1;                      /* bisection polish for flat regions */
    for (int i = 0; i < 8; i++) {
        float xt = bz(b->x1, b->x2, t);
        if (fabsf(xt - x) < 1e-4f) break;
        if (xt < x) lo = t; else hi = t;
        t = (lo + hi) * 0.5f;
    }
    return bz(b->y1, b->y2, t);
}

/* Authored durations land exactly on `to`. The 2 ms boundary tolerance is an
 * intentional end snap; timestamp subtraction happens before float conversion. */
#define MTW_TOL 0.002f
void mtw_start(mtween *tw, float from, float to, float dur, const bezier *curve)
{
    float fr = floorf(dur * M_FPS + 0.5f);
    if (fr < 1) fr = 1;
    tw->from = from; tw->to = to; tw->t0 = nos_t; tw->dur = fr / M_FPS;
    tw->curve = curve ? curve : &EASE_STANDARD;
    tw->tab = 0;
}
void mtw_start_tab(mtween *tw, float from, float to, const mframes *tab)
{
    tw->from = from; tw->to = to; tw->t0 = nos_t;
    tw->dur = (float)(tab && tab->n ? tab->n : 1) / M_FPS;
    tw->curve = &EASE_LINEAR;
    tw->tab = tab;
}
void mtw_lead(mtween *tw) { tw->t0 -= 1.0f / M_FPS; }
float mtw_progress(const mtween *tw)
{
    float e = (float)(nos_t - tw->t0);
    if (e >= tw->dur - MTW_TOL) return 1.0f;
    return m_clamp(e / tw->dur, 0, 1);
}
float mtw_value(const mtween *tw)
{
    if (tw->tab) return m_lerp(tw->from, tw->to, mf_at(tw->tab, m_frame_k(tw->t0)));
    float p = mtw_progress(tw);
    return p >= 1.0f ? tw->to : m_lerp(tw->from, tw->to, bezier_ease(tw->curve, p));
}
int mtw_done(const mtween *tw) { return nos_t - tw->t0 >= tw->dur - MTW_TOL; }
