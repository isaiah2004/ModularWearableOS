/* fit_heart.c — Heart rate app and home widget (health team).
 *
 * Opening the app starts the MAX30102 stream (hal_hr_enable(1)); leaving stops it and
 * records the reading if one locked. States, page 0:
 *   ABSENT     module not connected (designed state, no sensor calls beyond the probe)
 *   NOCONTACT  "Place the watch snugly" with a dot illustration of the watch settling
 *   MEASURING  live PPG trace of dots scrolling right to left, a white heart pulsing on
 *              each beat as the beat's peak reaches the pen, "[ MEASURING ]"
 *   LOCKED     big dot-matrix BPM (morphs on change), red heart (the screen's one red
 *              element), SpO2 when available, signal quality; the trace shrinks below.
 * Page 1 (scroll down): today's resting / min / max and the 24 h history in dot columns.
 *
 * The live trace: hal_hr_get() gives the last 48 samples at 20 Hz (the device appends
 * them in 120 ms bursts; the simulator resamples continuously). Samples go into a ring
 * buffer and a playhead runs a few samples behind the newest at exactly 20 samples/s,
 * so the trace moves by equal steps every frame no matter how the samples arrive.
 * The dots are laid out in tick() into a PSRAM list and drawn as ONE damage command. */
#include "nos.h"
#include "pblur.h"
#include "ui.h"
#include "dot.h"
#include "prog.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "scroll_diag.h"
#include <math.h>
#include <string.h>
#include <stdio.h>

#define PAGE_H   466.0f
#define NPAGES   2
#define WAVE_HZ  20.0f
#define HN       128          /* sample ring */
#define WIN      80           /* samples across the trace: 4 s */
#define LAG      4.0f         /* playhead lag behind the newest sample */
#define TRACE_MAX 420
#define ILLU_MAX  64
#define LOCK_Q   0.6f
#define REARM_MAX 4.0f        /* back from a cover: hold a locked reading while the sensor wakes */
#define XOUT     M_FRAMES(4)  /* state change: outgoing content fades out (4 frames) ... */
#define XIN      M_FRAMES(6)  /* ... then the incoming content fades in (6), perceptual steps */
/* The lock (MEASURING -> LOCKED) is a fade-through, not a slide (MOTION30 R4): the big
 * trace, [ MEASURING ] and the meter fade out over frames 1-4, the small trace and the
 * labels fade in at their own places over frames 4-9, the number builds from frame 3,
 * and the heart steps up 22 px on the 6-frame table (6-6-4-3-2-1 px) as it turns red. */
#define LOCK_OUT 4
#define LOCK_IN  6
#define LOCK_NUM 3

static float qz(float v, float steps) { return floorf(v * steps + 0.5f) / steps; }


/* ------------------------------------------------------------ dot lists
 * A list of dots prepared in tick and drawn as one recorded command. */
typedef struct { float x, y, d; uint16_t c; } hdot;
#define DL_GROUP 8
typedef struct { float lo, hi; } dl_band;
typedef struct { hdot *d; dl_band *band; int n, cap; int x0, y0, x1, y1; unsigned hash; } dlist;

static void dl_alloc(dlist *l, int cap)
{
    if (l->d) return;
    l->d = (hdot *)hal_big_alloc((uint32_t)(sizeof(hdot) * (unsigned)cap));
    l->cap = l->d ? cap : 0;
    if (l->d) l->band = hal_big_alloc((uint32_t)(sizeof(dl_band) * (unsigned)((cap + DL_GROUP - 1) / DL_GROUP)));
    l->n = 0;
}
static void dl_reset(dlist *l)
{
    l->n = 0; l->x0 = l->y0 = 100000; l->x1 = l->y1 = -100000; l->hash = 0x4EA27u;
}
static void dl_add(dlist *l, float x, float y, float d, uint16_t c)
{
    if (!l->d || l->n >= l->cap) return;
    d = qz(d, 4.0f);
    if (d < 0.5f) return;
    x = qz(x, 4.0f); y = qz(y, 4.0f);
    hdot *o = &l->d[l->n++];
    o->x = x; o->y = y; o->d = d; o->c = c;
    float e = d * 0.5f + 1.0f;
    if (l->band) {
        dl_band *b = &l->band[(l->n - 1) / DL_GROUP];
        if ((l->n - 1) % DL_GROUP == 0) { b->lo = y - e; b->hi = y + e; }
        else { if (y - e < b->lo) b->lo = y - e; if (y + e > b->hi) b->hi = y + e; }
    }
    int a = (int)floorf(x - e), b = (int)floorf(y - e), cc = (int)ceilf(x + e), dd = (int)ceilf(y + e);
    if (a < l->x0) l->x0 = a;
    if (b < l->y0) l->y0 = b;
    if (cc > l->x1) l->x1 = cc;
    if (dd > l->y1) l->y1 = dd;
    l->hash = gfx_hash_u(l->hash, (unsigned)(int)(x * 4) * 2654435761u ^ (unsigned)(int)(y * 4));
    l->hash = gfx_hash_u(l->hash, (unsigned)(int)(d * 4) << 16 | c);
}
static void dl_draw(gfx_fb *fb, const dlist *l, float oy)
{
    if (!l->d || !l->n) return;
    int o = (int)oy;
    if (fb->rec) { gfx_rec_cmd(fb, l->x0, l->y0 + o, l->x1, l->y1 + o, gfx_hash_u(l->hash, (unsigned)o)); return; }
    float ylo = (float)(fb->y_base - fb->oy), yhi = ylo + (float)fb->h;
    if ((float)(l->y1 + o) <= ylo || (float)(l->y0 + o) >= yhi) return;
    for (int g = 0; g < l->n; g += DL_GROUP) {
        if (l->band && (l->band[g / DL_GROUP].hi + (float)o <= ylo ||
                        l->band[g / DL_GROUP].lo + (float)o >= yhi)) continue;
        int end = g + DL_GROUP < l->n ? g + DL_GROUP : l->n;
        for (int i = g; i < end; i++) {
            const hdot *p = &l->d[i];
            float y = p->y + (float)o, e = p->d * 0.5f + 1.0f;
            if (y + e <= ylo || y - e >= yhi) continue;
            dot_disc(fb, p->x, y, p->d, p->c);
        }
    }
}

/* -------------------------------------------------------- vertical pager */
typedef struct {
    mspring sp;
    int page, n, drag;
    float grab, dy0;
    float bar_k; nos_time_t bar_t;
} vpager;

static void vp_init(vpager *v, int n)
{
    msp_init_px(&v->sp, 0, SPRING_PAGE);       /* pixels: ends with a 1 px snap (R5) */
    v->page = 0; v->n = n; v->drag = 0; v->bar_k = 0; v->bar_t = -10;
}
static int vp_input(vpager *v, const nos_input *in)
{
    if(in->type==IN_CANCEL) {v->drag=0;msp_kick(&v->sp,(float)v->page*PAGE_H,0);return 1;}
    float max = (float)(v->n - 1) * PAGE_H;
    switch (in->type) {
    case IN_DRAG_START:
        if (in->axis != AXIS_Y) return 0;
        v->drag = 1; v->grab = v->sp.x; v->dy0 = in->dy;
        /* fallthrough */
    case IN_DRAG: {
        if (!v->drag) return 0;
        float x = v->grab - (in->dy - v->dy0);
        if (x < 0) x = -rubber_band(-x, PAGE_H);
        if (x > max) x = max + rubber_band(x - max, PAGE_H);
        msp_set(&v->sp, x);
        v->bar_t = nos_t;
        return 1;
    }
    case IN_DRAG_END: {
        if (!v->drag) return 0;
        v->drag = 0;
        float vel = -in->vy, x = v->sp.x;
        int t;
        if (vel > 350) t = (int)floorf(x / PAGE_H) + 1;
        else if (vel < -350) t = (int)ceilf(x / PAGE_H) - 1;
        else t = (int)floorf(x / PAGE_H + 0.5f);
        if (t > v->page + 1) t = v->page + 1;
        if (t < v->page - 1) t = v->page - 1;
        if (t < 0) t = 0;
        if (t > v->n - 1) t = v->n - 1;
        if (t != v->page) nos_feedback(FB_TICK);
        v->page = t;
        msp_kick(&v->sp, (float)t * PAGE_H, vel);
        v->bar_t = nos_t;
        return 1;
    }
    default:
        return 0;
    }
}
static void vp_tick(vpager *v, float dt)
{
    if (!v->drag && msp_step(&v->sp, dt)) v->bar_t = nos_t;
    /* linear ramp at a fixed rate (in 4 frames, out 8); quantised only when drawn,
     * so it always reaches 0 instead of rounding back up */
    float target = nos_t - v->bar_t < 0.7f ? 1.0f : 0.0f;
    float step = dt * (target > v->bar_k ? 7.5f : 3.75f);
    if (v->bar_k < target) { v->bar_k += step; if (v->bar_k > target) v->bar_k = target; }
    else { v->bar_k -= step; if (v->bar_k < target) v->bar_k = target; }
}
/* Show the scrollbar once (0.7 s, then the normal fade): there is a page below. */
static void vp_flash(vpager *v) { v->bar_t = nos_t; }
/* perceptual ink steps (R9), drawn down to the last one (no pop at the end) */
static float vp_bar_alpha(const vpager *v) { return m_ink(v->bar_k); }

/* ---------------------------------------------------------- history data */
typedef struct {
    uint8_t hist[144];
    int nhist;
    int resting, mn, mx, avg, last, spo2;
    uint32_t last_epoch;
} hdata;

/* Only the service bins producer timestamps. A timeless byte-ring fallback
 * cannot establish a rolling 24-hour axis or distinguish acquisition gaps. */
static void hdata_read(hdata *h, int allow_hal)
{
    (void)allow_hal;
    const svc_heart_t *s = svc_heart();
    memset(h, 0, sizeof *h);
    if (s) {
        h->nhist = s->nhist > 144 ? 144 : s->nhist;
        if (h->nhist > 0) memcpy(h->hist, s->hist, (size_t)h->nhist);
        h->resting = s->resting; h->mn = s->min_today; h->mx = s->max_today;
        h->last = s->bpm_last; h->last_epoch = s->bpm_last_epoch; h->spo2 = s->spo2_last;
    }
    if (h->nhist < 0) h->nhist = 0;
    /* derive what the service didn't give */
    int lo = 255, hi = 0, cnt = 0, sum_low = 0, sum = 0;
    for (int i = 0; i < h->nhist; i++) {
        int v = h->hist[i];
        if (!v) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        sum += v;
        cnt++;
    }
    if (cnt) {
        /* the page shows the same rolling 24 h as its chart: min / avg / max of it */
        h->mn = lo;
        h->mx = hi;
        h->avg = (sum + cnt / 2) / cnt;
        if (!h->resting) {
            /* resting ~ mean of the lowest quarter of the readings */
            int want = cnt / 4 > 0 ? cnt / 4 : 1, got = 0;
            for (int v = lo; v <= hi && got < want; v++)
                for (int i = 0; i < h->nhist && got < want; i++)
                    if (h->hist[i] == v) { sum_low += v; got++; }
            h->resting = got ? (sum_low + got / 2) / got : lo;
        }
        if (!h->last) for (int i = h->nhist - 1; i >= 0; i--) if (h->hist[i]) { h->last = h->hist[i]; break; }
    }
}

/* Column growth: 8 groups one frame apart from frame 1, each EASE_STEADY over 7 frames
 * (R9 whole-frame stagger, R3 even spacing). k = whole frames since the build started. */
#define COL_F 7
static float col_grow(int k, int c, int cols)
{
    k = m_stagger_k(k - 1, c * 8 / cols, 1);
    return k <= 0 ? 0.0f : k >= COL_F ? 1.0f : bezier_ease(&EASE_STEADY, (float)k / COL_F);
}
/* Columns: average of the non-zero samples in each bucket, scaled into lo..hi; k = build
 * frames (from tick only: it eases every column). */
static int hdata_columns(const hdata *h, uint8_t *out, int cols, int rows, int k)
{
    int lo = 255, hi = 0, any = 0;
    for (int i = 0; i < h->nhist; i++) if (h->hist[i]) { any = 1; if (h->hist[i] < lo) lo = h->hist[i]; if (h->hist[i] > hi) hi = h->hist[i]; }
    if (!any) { memset(out, 0, (size_t)cols); return 0; }
    float flo = (float)lo - 3.0f, fhi = (float)hi + 4.0f;
    if (fhi - flo < 30.0f) { float m = (flo + fhi) * 0.5f; flo = m - 15.0f; fhi = m + 15.0f; }
    for (int c = 0; c < cols; c++) {
        int a = c * h->nhist / cols, b = (c + 1) * h->nhist / cols, n = 0, s = 0;
        for (int i = a; i < b; i++) if (h->hist[i]) { s += h->hist[i]; n++; }
        float g = col_grow(k, c, cols);
        float f = n ? ((float)s / (float)n - flo) / (fhi - flo) : 0;
        if (n) { f = floorf(m_clamp(f, 0, 1) * (float)rows + 0.5f) / (float)rows; if (f < 1.0f / (float)rows) f = 1.0f / (float)rows; }
        /* at rest: whole dots, rounded down so no sub-pixel speck grows above a column */
        out[c] = (uint8_t)(g >= 1.0f ? floorf(f * 255.0f) : lroundf(m_clamp(f * g, 0, 1) * 255.0f));
    }
    return 1;
}

static void fmt_bpm(char *b, int n, int v) { if (v > 0 && v < 1000) snprintf(b, (size_t)n, "%d", v); else snprintf(b, (size_t)n, "--"); }

/* ================================================================= APP */
enum { M_ABSENT = 0, M_NOCONTACT, M_MEASURING, M_LOCKED };

static struct {
    /* samples */
    uint8_t v[HN];
    uint32_t n;                 /* samples received */
    uint8_t last[48];
    int have_last;
    float acc;                  /* continuous-source sample clock */
    float ph;                   /* playhead, fractional sample index */
    int ph_ok;
    float lo, hi;               /* smoothed trace range */
    uint32_t pk[8]; int npk;    /* detected beat peaks (sample index), oldest first */
    uint32_t last_peak;
    float beat_t;               /* since the last beat reached the pen */
    /* sensor */
    hal_hr r;
    uint32_t last_sample_ms, accepted_epoch, entry_ms;
    int have_sample, last_lockable;
    int present;
    int mode;
    float mode_t;
    float off_t;                /* time continuously off the wrist */
    float good_t;               /* time continuously lockable */
    float contact_t;            /* time continuously on the wrist */
    float rearm_t;              /* >= 0: back from a cover, sensor waking, reading held */
    int locked;
    float lock_t;
    int flashed;                /* the scrollbar showed once after the entry build */
    float now_a, last_a;        /* history footer: NOW (live) / LAST (stored) crossfade */
    int foot_last;              /* the LAST value, changed only while its row is hidden */
    /* the number */
    int bpm_shown, bpm_prev;
    float bpm_morph;            /* 0..1, 1 = settled */
    float bpm_change_t;
    int spo2;
    float t;
    /* pages */
    vpager vp;
    hdata hd;
    float hist_t;               /* s since the history page started building */
    int hist_started;
    uint8_t hcols[48];          /* history columns, prepared in tick */
    int hany;
    float blk_p[3];
    /* prepared */
    dlist trace, illu;
    float u;                    /* layout: 0 measuring, 1 locked (switches mid fade-through) */
    float trace_k;              /* the trace's own ink through the lock fade-through */
    float lock_out, lock_in;    /* lock fade-through: outgoing / incoming ink */
    float hy;                   /* heart icon centre y (clicks up at the lock) */
    float scroll;               /* audit: trace travel, px */
    float heart_scale;
    float now_h;                /* local clock, hours */
    /* state transitions: the outgoing mode fades out (XOUT), then the new one in (XIN) */
    int prev_mode;              /* -1 = none (entry build) */
    int prev_locked, prev_bpm, prev_spo2;
    float prev_u;
    float kin, kout;            /* quantised alphas for this frame */
} H;

static float trace_y(float v, float yc, float h)
{
    float lo = H.lo, hi = H.hi;
    if (hi - lo < 40) { float m = (lo + hi) * 0.5f; lo = m - 20; hi = m + 20; }
    return m_clamp(yc + h * 0.5f - (v - lo) / (hi - lo) * h, yc - h * 0.5f, yc + h * 0.5f);
}

static int sample_at(uint32_t k, float *out)
{
    if (k >= H.n || H.n - k > HN) return 0;
    *out = (float)H.v[k % HN];
    return 1;
}

static void push_sample(uint8_t s)
{
    H.v[H.n % HN] = s;
    H.n++;
    /* peak test on the previous sample: local max above 60% of the current range */
    if (H.n >= 3) {
        uint32_t j = H.n - 2;
        float a = H.v[(j - 1) % HN], b = H.v[j % HN], c = H.v[(j + 1) % HN];
        float thr = H.lo + 0.6f * (H.hi - H.lo);
        if (b > a && b >= c && b > thr && H.hi - H.lo > 30 && (!H.last_peak || j - H.last_peak >= 7)) {
            if (H.npk == 8) { memmove(H.pk, H.pk + 1, 7 * sizeof H.pk[0]); H.npk = 7; }
            H.pk[H.npk++] = j;
            H.last_peak = j;
        }
    }
}

static void ingest(const uint8_t *w, float dt)
{
    int k = -1;
    if (H.have_last)
        for (int s = 0; s <= 12; s++)
            if (memcmp(w, H.last + s, (size_t)(48 - s)) == 0) { k = s; break; }
    if (k < 0) {
        /* continuously resampled source (or a reset): take samples by time */
        if (!H.have_last) { k = 48; H.acc = 0; }
        else { H.acc += dt * WAVE_HZ; k = (int)H.acc; H.acc -= (float)k; }
    } else H.acc = 0;
    if (k > 48) k = 48;
    for (int j = 48 - k; j < 48; j++) push_sample(w[j]);
    memcpy(H.last, w, 48);
    H.have_last = 1;
}

static void playhead_tick(float dt)
{
    if (H.n < 8) return;
    float newest = (float)(H.n - 1);
    if (!H.ph_ok || H.ph < newest - 24.0f || H.ph > newest) {
        if (!H.ph_ok || H.ph < newest - 24.0f) H.ph = newest - LAG;
        else H.ph = newest;              /* starved: hold at the newest sample */
        H.ph_ok = 1;
        return;
    }
    float err = (newest - LAG) - H.ph;  /* > 0: falling behind */
    float corr = m_clamp(err / LAG, -1, 1) * 0.06f;
    float prev = H.ph;
    H.ph += WAVE_HZ * dt * (1.0f + corr);
    if (H.ph > newest) H.ph = newest;
    /* a beat reaches the pen: pulse the heart */
    while (H.npk && (float)H.pk[0] <= H.ph) {
        if ((float)H.pk[0] > prev) H.beat_t = 0;
        memmove(H.pk, H.pk + 1, (size_t)(H.npk - 1) * sizeof H.pk[0]);
        H.npk--;
    }
    /* trace range follows the visible window, smoothly */
    float lo = 255, hi = 0, v;
    uint32_t k0 = H.ph > WIN ? (uint32_t)(H.ph - WIN) : 0;
    for (uint32_t k = k0; k <= (uint32_t)H.ph; k++)
        if (sample_at(k, &v)) { if (v < lo) lo = v; if (v > hi) hi = v; }
    if (hi >= lo) {
        /* widen at once (a bigger beat never escapes the box), narrow smoothly */
        float a = dt * 2.5f;
        if (lo < H.lo) H.lo = lo; else H.lo += (lo - H.lo) * a;
        if (hi > H.hi) H.hi = hi; else H.hi += (hi - H.hi) * a;
    }
}

/* Lay out the trace: rigid chain of dots, samples joined by interpolated dots. */
static void build_trace(float xl, float xr, float yc, float h, uint16_t ink, uint16_t pen, float dsz, int step)
{
    /* step: use every `step`-th sample (absolute index, so the choice never changes as
     * the trace scrolls). The chain scrolls 2/3 of a sample a frame (20 Hz at 30 fps):
     * dots one sample apart would step 2/3 of their own pitch and alias backwards on the
     * flat stretches (the wagon wheel, MOTION30 R2/R6), so dots sit >= 2 samples apart
     * and interpolated dots >= 2.1 frames of scroll apart: every dot pattern moves less
     * than half its pitch a frame. */
    dlist *l = &H.trace;
    dl_reset(l);
    if (!l->d || !H.ph_ok) return;
    float pitch = (xr - xl) / (float)WIN;
    uint32_t kn = (uint32_t)H.ph;
    float frac = H.ph - (float)kn;
    float px = 0, py = 0;
    int have = 0;
    float spacing = dsz * 1.35f;                /* separate dots: the trace reads as dots */
    if (spacing < 2.1f * pitch * WAVE_HZ / M_FPS) spacing = 2.1f * pitch * WAVE_HZ / M_FPS;
    for (int i = WIN + 2; i >= -1; i--) {
        float v, x, y;
        if (i >= 0) {
            if ((uint32_t)i > kn) continue;
            uint32_t k = kn - (uint32_t)i;
            if (k % (uint32_t)step) continue;
            if (!sample_at(k, &v)) continue;
            x = xr - ((float)i + frac) * pitch;
        } else {
            /* the pen: the playhead's own interpolated position */
            float a, b;
            if (!sample_at(kn, &a)) continue;
            if (!sample_at(kn + 1, &b)) b = a;
            v = a + (b - a) * frac;
            x = xr;
        }
        y = trace_y(v, yc, h);
        if (x < xl - pitch * step) { px = x; py = y; have = 1; continue; }
        if (have) {
            float dx = x - px, dy = y - py, dist = gfx_fast_sqrt(dx * dx + dy * dy);
            int nf = (int)(dist / spacing) - 1;
            for (int f = 1; f <= nf; f++) {
                float t = (float)f / (float)(nf + 1);
                float fx = px + dx * t, fy = py + dy * t;
                if (fx < xl) continue;                 /* clipped at the left edge, no fade */
                dl_add(l, fx, fy, dsz, ink);
            }
        }
        if (x >= xl && i >= 0) dl_add(l, x, y, dsz, ink);
        px = x; py = y; have = 1;
    }
    /* pen dot */
    if (have) dl_add(l, xr, py, dsz * 1.8f, pen);
}

/* Not on the wrist: the watch (a dot pill) settles onto the wrist (a dot arc). */
static void build_illu(float t, float k)
{
    dlist *l = &H.illu;
    dl_reset(l);
    if (!l->d) return;
    const float cx = UI_CX, base = 210;          /* wrist crown */
    /* settle cycle: 2.2 s, lifted -> pressed (eased) -> hold -> lift */
    float c = fmodf(t, 2.2f) / 2.2f;
    float press = c < 0.45f ? bezier_ease(&EASE_INOUT, c / 0.45f) : c < 0.75f ? 1.0f : 1.0f - bezier_ease(&EASE_INOUT, (c - 0.75f) / 0.25f);
    float lift = (1.0f - press) * 16.0f;
    int touch = press > 0.95f;
    uint16_t dim = ui_dim(C_DIS, k), mid = ui_dim(C_SEC, k), lit = ui_dim(C_PRI, k);
    /* wrist: an arc of a big circle */
    for (int i = -7; i <= 7; i++) {
        float a = (float)i * 0.07f;
        float x = cx + 230.0f * gfx_fast_sin(a), y = base + 230.0f * (1.0f - gfx_fast_cos(a));
        dl_add(l, x, y, 7.0f, touch && i >= -1 && i <= 1 ? lit : dim);
    }
    /* strap: follows the wrist a little above it, lifting with the body */
    for (int s = -1; s <= 1; s += 2)
        for (int j = 0; j < 5; j++) {
            float a = (float)s * (0.25f + (float)j * 0.07f);
            float x = cx + 230.0f * gfx_fast_sin(a), y = base - 12.0f + 230.0f * (1.0f - gfx_fast_cos(a));
            dl_add(l, x, y - lift * (1.0f - (float)j / 5.0f), 6.0f, mid);
        }
    /* watch body: 9 x 3 dot pill outline, sensor bump underneath */
    float wy = base - 41 - lift;
    for (int i = 0; i < 9; i++) {
        float x = cx + ((float)i - 4.0f) * 11.0f;
        dl_add(l, x, wy, 7.0f, lit);
        dl_add(l, x, wy + 22, 7.0f, lit);
    }
    dl_add(l, cx - 51, wy + 11, 7.0f, lit);
    dl_add(l, cx + 51, wy + 11, 7.0f, lit);
    for (int i = -1; i <= 1; i++) dl_add(l, cx + (float)i * 11.0f, wy + 33, 5.0f, touch ? lit : mid);
}

static int is_live(int m) { return m == M_MEASURING || m == M_LOCKED; }
static void set_mode(int m);

static void heart_enter(void)
{
    if (nos_revealed()) {
        /* Back from a cover (power sheet, notification): keep the reading, the trace, the
         * mode and the page. leave() kept the stream running (covered), but a long cover
         * lets the HAL idle-stop it: enable again, and hold a locked reading (trace, meter
         * frozen) until the sensor reads a pulse, so it never drops back to MEASURING. */
        hal_hr r;
        int pres = (hal_ext_present() & EXT_HR) != 0 && hal_hr_get(&r);
        if (pres) {
            hal_hr_request(HAL_HR_OWNER_HEART,1);
            if (!H.present) { H.present = 1; set_mode(M_MEASURING); }   /* plugged in meanwhile */
            H.rearm_t = H.locked ? 0.0f : -1.0f;
        }
        return;
    }
    memset(H.v, 128, sizeof H.v);
    H.entry_ms=hal_millis();H.have_sample=H.last_lockable=0;H.accepted_epoch=0;
    H.n = WIN + 8;   /* a flat baseline the live signal scrolls in over */
    H.have_last = 0; H.acc = 0; H.ph = 0; H.ph_ok = 0;
    H.lo = 100; H.hi = 156; H.npk = 0; H.last_peak = 0; H.beat_t = 10;
    H.off_t = 0; H.good_t = 0; H.contact_t = 0; H.rearm_t = -1; H.locked = 0; H.lock_t = 0;
    H.bpm_shown = 0; H.bpm_prev = 0; H.bpm_morph = 1; H.bpm_change_t = 0; H.spo2 = 0;
    /* the entry build runs from nos_enter_t() (40% of the iris); a finger pull skips it */
    H.t = nos_entered_by_finger() ? 10.0f : nos_t - nos_enter_t();
    H.flashed = 0;
    H.now_a = 0; H.last_a = 1; H.foot_last = 0;
    H.u = 0; H.heart_scale = 1;
    dl_alloc(&H.trace, TRACE_MAX);
    dl_alloc(&H.illu, ILLU_MAX);
    dl_reset(&H.trace); dl_reset(&H.illu);
    memset(&H.r, 0, sizeof H.r);
    H.present = (hal_ext_present() & EXT_HR) != 0;
    if (H.present) hal_hr_request(HAL_HR_OWNER_HEART,1);
    H.mode = H.present ? M_MEASURING : M_ABSENT;
    H.mode_t = 0;
    H.prev_mode = -1; H.prev_locked = 0; H.prev_bpm = 0; H.prev_spo2 = 0; H.prev_u = 0;
    H.kin = H.kout = 0;
    vp_init(&H.vp, NPAGES);
    hdata_read(&H.hd, 1);
    H.hist_t = 0; H.hist_started = 0; H.hany = 0;
    H.trace_k = 1; H.lock_out = 1; H.lock_in = 0; H.hy = 134; H.scroll = 0;
}

static void heart_leave(void)
{
    /* Covered by a push (sheet, notification): keep the stream and the reading, they are
     * revealed again (the HAL idle-stops the sensor if the cover stays NOS_EXT_IDLE_MS). */
    if (nos_leaving_covered()) return;
    hal_hr_request(HAL_HR_OWNER_HEART,0);
    if (H.locked && H.r.bpm > 0) svc_heart_record_at(H.r.bpm, H.spo2, H.accepted_epoch);
}

static void set_mode(int m)
{
    if (H.mode == m) return;
    /* measuring -> locked is carried by the layout tween, not a crossfade */
    if (is_live(H.mode) && is_live(m)) { H.mode = m; return; }
    H.prev_mode = H.mode; H.prev_locked = H.locked; H.prev_u = H.u;
    H.prev_bpm = H.bpm_shown; H.prev_spo2 = H.spo2;
    H.mode = m; H.mode_t = M_FRAMES(1);   /* the change shows its first step this frame */
}

/* A state swap: the old state fades out over frames 1-4, the new one in over frames 4-9
 * (no all-black frame between), in perceptual ink steps (m_ink), whole frames. */
static float mode_in_p(void) { return m_clamp((H.mode_t - XOUT + M_FRAMES(1)) / XIN, 0, 1); }
static float mode_out_p(void) { return m_clamp(1.0f - H.mode_t / XOUT, 0, 1); }
static float mode_in(void)
{
    if (H.prev_mode < 0) return m_ink(H.t / UI_ENTRY_S);
    return m_ink(mode_in_p());
}
static float mode_out(void)
{
    if (H.prev_mode < 0 || H.mode_t >= XOUT - 0.002f) return 0;
    return m_ink(mode_out_p());
}
/* the beat pulse, per frame from the beat: it swells at once (the beat frame already
 * shows it), peaks on the next and settles in 5 even-ish steps: ends crisply (R5) */
static const float k_pulse[7] = { 0.6f, 1.0f, 0.75f, 0.5f, 0.3f, 0.15f, 0.05f };

static void trace_for(float u, int locked, float k)
{
    if (k <= 0.01f) { dl_reset(&H.trace); return; }
    float xl = m_lerp(58, 118, u), xr = m_lerp(408, 348, u);
    float yc = m_lerp(238, 324, u), h = m_lerp(100, 38, u);
    build_trace(xl, xr, yc, h, ui_dim(locked ? C_SEC : C_PRI, k), ui_dim(C_WHITE, k),
                m_lerp(3.4f, 2.8f, u), 2);
}

static void heart_tick(float dt)
{
    H.t += dt;
    H.mode_t += dt;
    H.beat_t += dt;
    vp_tick(&H.vp, dt);
    {
        hal_datetime now;
        hal_get_datetime(&now);
        H.now_h = (float)now.hour + (float)now.minute / 60.0f;
    }
    int fresh = 0;
    float acquired_dt=0;
    uint32_t sample_now=hal_millis();
    if (!H.present && (hal_ext_present() & EXT_HR)) {
        /* the strap came back while open: start measuring (absent fades out, then the
         * live state fades in, like every other state change) on a fresh flat trace */
        H.present = 1;
        hal_hr_request(HAL_HR_OWNER_HEART,1);
        memset(H.v, 128, sizeof H.v);
        H.n = WIN + 8; H.have_last = 0; H.acc = 0; H.ph = 0; H.ph_ok = 0;
        H.lo = 100; H.hi = 156; H.npk = 0; H.last_peak = 0; H.beat_t = 10;
        H.off_t = 0; H.good_t = 0; H.contact_t = 0; H.rearm_t = -1; H.locked = 0; H.lock_t = 0;
        memset(&H.r, 0, sizeof H.r);
        set_mode(M_MEASURING);
    }
    if (H.present) {
        hal_hr r;
        H.present = hal_hr_get(&r);
        if (!H.present) { set_mode(M_ABSENT); H.locked = 0; H.rearm_t = -1; }
        int recent=H.present && r.sample_ms && sample_now-r.sample_ms<=3000u &&
                   sample_now-r.sample_ms<=sample_now-H.entry_ms;
        if(recent && (!H.have_sample || (int32_t)(r.sample_ms-H.last_sample_ms)>0)) {
            uint32_t gap=r.sample_ms-H.last_sample_ms;
            acquired_dt=H.have_sample && H.last_lockable && gap<=3000u?gap/1000.0f:0;
            H.have_sample=1;H.last_sample_ms=r.sample_ms;H.r=r;fresh=1;H.rearm_t=-1;
        }
        if(!recent || (!r.on_wrist || r.bpm<35 || r.bpm>230 ||
           !isfinite(r.quality) || r.quality<LOCK_Q || r.quality>1.0f)) {
            H.good_t=H.contact_t=0;H.locked=0;H.bpm_shown=H.spo2=0;
            H.last_lockable=0;
            H.have_sample=recent?H.have_sample:0;
            if(H.present)set_mode(recent && !r.on_wrist?M_NOCONTACT:M_MEASURING);
        }
    }
    if (fresh) {
        ingest(H.r.wave, dt);
        if (!H.r.on_wrist) { H.off_t += acquired_dt; H.contact_t = 0; } else { H.off_t = 0; H.contact_t += acquired_dt; }
        int lockable = H.r.on_wrist && H.r.bpm>=35 && H.r.bpm<=230 && isfinite(H.r.quality) && H.r.quality >= LOCK_Q && H.r.quality<=1.0f;
        H.last_lockable=lockable;
        H.good_t = lockable ? H.good_t + acquired_dt : 0;
        if (H.off_t > 1.0f) {
            if (H.mode != M_NOCONTACT) { set_mode(M_NOCONTACT); H.locked = 0; H.good_t = 0; }
        } else if (H.r.on_wrist || H.mode != M_NOCONTACT) {
            /* MEASURING reads as a state, not a flash: a lock also needs 2 s of contact
             * (on rewrist the signal can be good at once) */
            if (!H.locked && H.good_t >= 0.5f && H.contact_t >= 2.0f) {
                H.locked = 1; H.lock_t = 0;
                H.bpm_shown = H.r.bpm; H.bpm_prev = H.r.bpm; H.bpm_morph = 1; H.bpm_change_t = 0;
                if (nos_prefs.haptics) hal_haptic(HAPTIC_HEARTBEAT);
            }
            set_mode(H.locked ? M_LOCKED : M_MEASURING);
        }
        if (H.locked) {
            if (H.r.bpm > 0 && H.r.bpm != H.bpm_shown && H.bpm_morph >= 1 && H.bpm_change_t > 1.2f) {
                H.bpm_prev = H.bpm_shown; H.bpm_shown = H.r.bpm; H.bpm_morph = 0; H.bpm_change_t = 0;
            }
            H.spo2 = H.r.spo2>=70 && H.r.spo2<=100?H.r.spo2:0;
            H.accepted_epoch=svc_heart_sample_epoch(H.r.sample_ms);
            svc_heart_record_at(H.r.bpm,H.spo2,H.accepted_epoch);
        }
    }
    if(H.locked) {
        H.lock_t+=dt;H.bpm_change_t+=dt;
        if(H.bpm_morph<1)H.bpm_morph=m_clamp(H.bpm_morph+dt/M_FRAMES(8),0,1);
    }
    if(H.present && H.have_sample)playhead_tick(dt);
    if (H.present && (H.mode == M_MEASURING || (H.mode == M_LOCKED && H.lock_t < 20.0f))) nos_keep_awake();
    /* the entry build is done: show the scrollbar once (there is a page below) */
    if (!H.flashed && H.t >= UI_ENTRY_S) { H.flashed = 1; vp_flash(&H.vp); }
    /* history footer: LAST (stored) out, then NOW (live, locked) in, and back */
    {
        float st = dt / M_FRAMES(4);
        if (H.locked) { if (H.last_a > 0) H.last_a = m_clamp(H.last_a - st, 0, 1); else H.now_a = m_clamp(H.now_a + st, 0, 1); }
        else {
            if (H.now_a > 0) H.now_a = m_clamp(H.now_a - st, 0, 1);
            else { if (H.bpm_shown > 0) H.foot_last = H.bpm_shown; H.last_a = m_clamp(H.last_a + st, 0, 1); }
        }
    }
    /* layout: the lock is a fade-through (see LOCK_*) and the heart's 22 px step up */
    {
        int lk = H.locked ? (int)floorf(H.lock_t * M_FPS + 0.5f) : 0;
        H.lock_out = !H.locked ? 1.0f : m_ink(1.0f - (float)lk / LOCK_OUT);
        H.lock_in = !H.locked ? 0.0f : m_ink((float)(lk - (LOCK_OUT - 1)) / LOCK_IN);
        H.u = H.locked && lk >= LOCK_OUT ? 1.0f : 0.0f;
        H.trace_k = !H.locked ? 1.0f : lk < LOCK_OUT ? H.lock_out : H.lock_in;
        H.hy = H.locked ? floorf(m_lerp(134, 112, mf_at(&MF_MICRO6, lk)) + 0.5f) : 134.0f;
    }
    {
        int bk = (int)floorf(H.beat_t * M_FPS + 0.5f);
        float pulse = bk >= 0 && bk < 7 ? k_pulse[bk] : 0.0f;
        H.heart_scale = 1.0f + 0.55f * qz(pulse, 16.0f);   /* the outline swells bold on each beat */
    }
    H.kin = mode_in();
    H.kout = mode_out();
    if (is_live(H.mode)) trace_for(H.u, H.locked, H.kin * H.trace_k);
    else if (H.kout > 0 && is_live(H.prev_mode)) trace_for(H.prev_u, H.prev_locked, H.kout);
    else dl_reset(&H.trace);
    if (H.mode == M_NOCONTACT && H.kin > 0) build_illu(H.t, H.kin);
    else if (H.kout > 0 && H.prev_mode == M_NOCONTACT) build_illu(H.t, H.kout);
    else dl_reset(&H.illu);
    /* the history page builds from its first revealed pixel */
    if (!H.hist_started && H.vp.sp.x > PAGE_H * 0.05f) { H.hist_started = 1; hdata_read(&H.hd, 1); }
    if (H.hist_started && H.hist_t < 2.0f) H.hist_t += dt;
    {   /* the history page: values reveal 2 frames apart, columns grow in 8 groups */
        int hk = (int)floorf(H.hist_t * M_FPS + 0.5f);
        for (int i = 0; i < 3; i++) H.blk_p[i] = m_clamp((float)(hk - 2 * i) / 8.0f, 0, 1);
        H.hany = hdata_columns(&H.hd, H.hcols, 48, 8, hk);
    }
    /* motion audit (sim only): what draw() shows */
    {
        float off = floorf(H.vp.sp.x + 0.5f);
        nos_motion_probe("heart.page", H.vp.drag ? MC_FINGER : MC_MOMENTUM, off);
        nos_motion_probe("heart.bar.ink", MC_FADE, vp_bar_alpha(&H.vp) * 100.0f);
        if (off < PAGE_H) {
            nos_motion_probe("heart.mode.in", MC_FADE, H.kin * 100.0f);
            nos_motion_probe("heart.mode.out", MC_FADE, H.kout * 100.0f);
            if (is_live(H.mode)) {
                /* the trace's scroll as travelled (its pitch differs per layout) */
                float u = H.u, xl = m_lerp(58, 118, u), xr = m_lerp(408, 348, u);
                static float s_ph;
                float d = H.ph - s_ph;
                s_ph = H.ph;
                if (d > 0 && d < 4) H.scroll += d * (xr - xl) / (float)WIN;
                nos_motion_probe("heart.trace.scroll", MC_TRACKED, qz(H.scroll, 4.0f));
                nos_motion_probe("heart.trace.yc", MC_COMPONENT, m_lerp(238, 324, u));
                nos_motion_probe("heart.trace.ink", MC_FADE, H.trace_k * 100.0f);
                nos_motion_probe("heart.icon.y", MC_COMPONENT, H.hy);
                nos_motion_probe("heart.pulse.d", MC_DECORATIVE, qz(4.0f * 0.8f * H.heart_scale, 4.0f));
                nos_motion_probe("heart.measuring.ink", MC_FADE, H.lock_out * 100.0f);
                if (H.locked) nos_motion_probe("heart.labels.ink", MC_FADE, H.lock_in * 100.0f);
            }
            if (H.mode == M_NOCONTACT) {
                float c = fmodf(H.t, 2.2f) / 2.2f;
                float press = c < 0.45f ? bezier_ease(&EASE_INOUT, c / 0.45f) : c < 0.75f ? 1.0f : 1.0f - bezier_ease(&EASE_INOUT, (c - 0.75f) / 0.25f);
                nos_motion_probe("heart.illu.lift", MC_DECORATIVE, qz((1.0f - press) * 16.0f, 4.0f));
            }
        }
        if (off > 0) {
            int hk = (int)floorf(H.hist_t * M_FPS + 0.5f);
            nos_motion_probe("heart.hist.col0", MC_COMPONENT, 8 * 7.0f * col_grow(hk, 0, 48));
            nos_motion_probe("heart.hist.col47", MC_COMPONENT, 8 * 7.0f * col_grow(hk, 47, 48));
            nos_motion_probe("heart.foot.last", MC_FADE, m_ink(H.last_a) * 100.0f);
            nos_motion_probe("heart.foot.now", MC_FADE, m_ink(H.now_a) * 100.0f);
        }
    }
}

/* ---- page 0 ---- */
/* Signal quality as a labelled meter of five 10 x 10 rounded squares, 5 px apart (prog.h),
 * whole squares only (the sensor's quality wobbles; a half-lit square would flicker).
 * No square lit: the label reads NO SIGNAL, right-aligned to the same meter, so the
 * squares never move when it changes. */
#define SIG_N    5
#define SIG_SQ   10.0f
#define SIG_GAP  5.0f
static void draw_signal(gfx_fb *fb, float y, float q, float k)
{
    int lw = ui_text_width(UI_F_LABEL, "SIGNAL", 2);
    const float bw = SIG_N * (SIG_SQ + SIG_GAP), gap = 14 - SIG_GAP * 0.5f;
    float mx = floorf(UI_CX - (lw + gap + bw) * 0.5f + (float)lw + gap + 0.5f);   /* meter left */
    int lit = (int)floorf(m_clamp(q, 0, 1) * SIG_N + 0.5f);
    ui_label(fb, (int)(mx - gap), (int)y, lit ? "SIGNAL" : "NO SIGNAL", ui_dim(lit ? C_DIS : C_SEC, k), UI_RIGHT);
    float by = floorf(y - (float)UI_F_LABEL->ascent * 0.5f - SIG_SQ * 0.5f - 1.0f + 0.5f);
    prog_bar(fb, PROG_SQUARES, mx, by, bw, SIG_SQ, SIG_N, (float)lit / SIG_N, ui_dim(C_PRI, k), ui_dim(C_LINE2, k));
}

static void draw_absent(gfx_fb *fb, float oy, int out)
{
    gfx_fb sub;
    if (!nos_subview(fb, &sub, 0, 0, GFX_W, GFX_H, 0, (int)oy)) return;
    /* ui_absent takes a build progress (it builds its icon and text in steps), so it gets
     * the linear progress of the swap, not its perceptual ink */
    float p = H.prev_mode < 0 ? m_clamp(H.t / 0.4f, 0, 1) : out ? mode_out_p() : mode_in_p();
    ui_absent(&sub, DOT_ICON_HEART, "Not connected", "Add the strap to measure BPM and SpO2",
              "HR SENSOR MODULE", p);
}

static void draw_nocontact(gfx_fb *fb, float oy, float k)
{
    ui_text(fb, UI_F_BODY, UI_CX, (int)(304 + oy), "Place the watch snugly", ui_dim(C_PRI, k), UI_CENTER, 0);
    ui_state(fb, UI_CX, (int)(346 + oy), "NO CONTACT", ui_dim(C_DIS, k));
}

/* out = 1: the outgoing copy of a live state (static, faded by k) */
static void draw_live(gfx_fb *fb, float oy, float k, int out)
{
    int locked = out ? H.prev_locked : H.locked;
    float u = out ? H.prev_u : H.u;
    /* heart: white while measuring, red (hard switch) once locked, stepping up as it turns */
    float hy = (out ? m_lerp(134, 112, u) : H.hy) + oy;
    const float pitch = 4.0f;
    uint16_t hc = ui_dim(locked ? C_RED : C_PRI, k);
    float hb = out ? 1.0f : m_clamp(H.t / UI_ENTRY_S, 0, 1);
    dot_icon_anim(fb, DOT_ICON_HEART, UI_CX, hy, pitch, qz(pitch * 0.8f * (out ? 1.0f : H.heart_scale), 4.0f), hc,
                  hb, DOT_REVEAL_CENTER);
    /* [ MEASURING ] + signal: fade out with the big trace (LOCK_OUT frames) */
    float km = locked ? (out ? 0.0f : H.lock_out) : 1.0f;
    km *= k;
    if (km > 0.01f) {
        ui_state(fb, UI_CX, (int)(340 + oy), "MEASURING", ui_dim(C_SEC, km));
        draw_signal(fb, 382 + oy, H.r.quality / LOCK_Q, km);
    }
    if (!locked) return;
    /* the number */
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, ui_dim(C_WHITE, k), 0, 1, 1 };
    char a[8], b[8];
    fmt_bpm(b, sizeof b, out ? H.prev_bpm : H.bpm_shown);
    float bp = out ? 1.0f : m_clamp((H.lock_t - M_FRAMES(LOCK_NUM)) / UI_ENTRY_S, 0, 1);
    if (bp < 1) dot_text_anim(fb, &st, UI_CX, 150 + oy, b, 1, bp, DOT_REVEAL_RANDOM, 11);
    else if (!out && H.bpm_morph < 1) { fmt_bpm(a, sizeof a, H.bpm_prev); dot_text_morph(fb, &st, UI_CX, 150 + oy, a, b, 1, qz(H.bpm_morph, 16.0f)); }
    else dot_text_a(fb, &st, UI_CX, 150 + oy, b, 1);
    /* labels arrive with the small trace, once [ MEASURING ] and its meter have gone */
    float kl = out ? k : H.lock_in * k;
    ui_label(fb, UI_CX, (int)(266 + oy), "BPM", ui_dim(C_SEC, kl), UI_CENTER);
    int spo2 = out ? H.prev_spo2 : H.spo2;
    if (spo2 > 0) {
        char s[8];
        snprintf(s, sizeof s, "%d%%", spo2 > 100 ? 100 : spo2);
        ui_label_unit(fb, UI_CX, (int)(376 + oy), "SPO2", s, ui_dim(C_SEC, kl), UI_CENTER);
    } else ui_label_unit(fb, UI_CX, (int)(376 + oy), "SPO2", UI_NONE, ui_dim(C_DIS, kl), UI_CENTER);
    draw_signal(fb, 406 + oy, (H.r.quality - 0.4f) / 0.5f, kl);
}

static void draw_mode(gfx_fb *fb, float oy, int mode, float k, int out)
{
    if (mode == M_ABSENT) draw_absent(fb, oy, out);
    else if (mode == M_NOCONTACT) draw_nocontact(fb, oy, k);
    else if (is_live(mode)) draw_live(fb, oy, k, out);
}

/* ---- page 1: the last 24 h ---- */
static void stat_block(gfx_fb *fb, float cx, float oy, int v, const char *label, float p)
{
    char b[8];
    fmt_bpm(b, sizeof b, v);
    dot_style st = { DOT_FONT_TALL, UI_LIST_PITCH, UI_LIST_PITCH * 0.8f, v ? C_WHITE : C_DIS, v ? 0 : C_GHOST, 1, 1 };
    dot_text_anim(fb, &st, cx, 116 + oy, b, 1, p, DOT_REVEAL_RANDOM, 9);
    ui_label(fb, (int)cx, (int)(184 + oy), label, ui_dim(C_SEC, m_ink(p * 1.4f)), UI_CENTER);
}

/* "NOW 73 BPM" with the number in small dots, morphing in step with the hero. */
static void hist_now(gfx_fb *fb, float y, float k)
{
    dot_style st = { DOT_FONT_5X7, UI_SUB_PITCH, UI_SUB_PITCH * 0.8f, ui_dim(C_SEC, k), 0, 1, 1 };
    char a[8], b[8];
    fmt_bpm(b, sizeof b, H.bpm_shown);
    const float gap = 12;
    float w1 = (float)ui_label_width("NOW"), w2 = (float)ui_label_width("BPM");
    float wn = (float)dot_text_width(&st, b) - st.pitch;      /* drop the trailing gap cell */
    float x = UI_CX - (w1 + gap + wn + gap + w2) * 0.5f;
    ui_label(fb, (int)x, (int)y, "NOW", ui_dim(C_DIS, k), UI_LEFT);
    float ty = y - (float)UI_F_LABEL->ascent * 0.5f - (float)dot_text_height(&st) * 0.5f;
    float nx = x + w1 + gap + wn * 0.5f;
    if (H.bpm_morph < 1) {
        fmt_bpm(a, sizeof a, H.bpm_prev);
        dot_text_morph(fb, &st, nx, ty, a, b, 1, qz(H.bpm_morph, 16.0f));
    } else dot_text_a(fb, &st, nx, ty, b, 1);
    ui_label(fb, (int)(x + w1 + gap + wn + gap), (int)y, "BPM", ui_dim(C_DIS, k), UI_LEFT);
}

static void draw_history(gfx_fb *fb, float oy)
{
    const hdata *h = &H.hd;
    ui_label(fb, UI_CX, (int)(UI_TITLE_Y + oy), "LAST 24 H", C_SEC, UI_CENTER);
    stat_block(fb, UI_CX - 120, oy, h->mn, "MIN", H.blk_p[0]);
    stat_block(fb, UI_CX, oy, h->avg, "AVG", H.blk_p[1]);
    stat_block(fb, UI_CX + 120, oy, h->mx, "MAX", H.blk_p[2]);
    const int cols = 48, rows = 8;
    const float pitch = 7.0f, d = 5.4f;
    float x0 = UI_CX - cols * pitch * 0.5f, y0 = 226 + oy;
    const uint8_t *vals = H.hcols;
    int any = H.hany;
    dot_columns(fb, x0, y0, cols, rows, pitch, d, vals, C_SEC, C_GHOST);
    if (any) {
        /* the newest column is now: white */
        dot_columns(fb, x0 + (cols - 1) * pitch, y0, 1, rows, pitch, d, &vals[cols - 1], C_WHITE, C_GHOST);
    } else {
        ui_state(fb, UI_CX, (int)(y0 + rows * pitch * 0.5f + 6), "NO READINGS YET", C_DIS);
    }
    /* time axis: clock hours 00/06/12/18 where they fall in the rolling 24 h, and NOW */
    float ay = y0 + rows * pitch + 30;
    float w = cols * pitch;
    float now_x = x0 + w - pitch * 0.5f;
    ui_label(fb, (int)now_x - 8, (int)ay, "NOW", C_SEC, UI_CENTER);
    static const char *k_ax[4] = { "00", "06", "12", "18" };
    for (int i = 0; i < 4; i++) {
        float ago = fmodf(H.now_h - (float)(i * 6) + 48.0f, 24.0f);
        float x = x0 + w * (1.0f - ago / 24.0f);
        if (now_x - x < 40.0f || x - x0 < 12.0f) continue;
        ui_label(fb, (int)x, (int)ay, k_ax[i], C_DIS, UI_CENTER);
    }
    /* footer: LAST + the stored reading; while locked, NOW + the live number, which
     * morphs with the hero. The two crossfade through black (4 frames each way). */
    float la = m_ink(H.last_a), na = m_ink(H.now_a);
    int last = H.foot_last ? H.foot_last : h->last;
    if (la > 0 && any && last) {
        char b[32];
        snprintf(b, sizeof b, "LAST %d BPM", last);
        ui_label(fb, UI_CX, (int)(ay + 40), b, ui_dim(C_DIS, la), UI_CENTER);
    }
    if (na > 0 && H.bpm_shown > 0) hist_now(fb, ay + 40, na);
}

static void heart_draw(gfx_fb *fb)
{
    float off = floorf(H.vp.sp.x + 0.5f);
    for (int pg = 0; pg < NPAGES; pg++) {
        float oy = pg * PAGE_H - off;
        if (oy <= -PAGE_H || oy >= PAGE_H) continue;
        /* Every page's ink is inside its viewport, with two conservative AA
         * rows on either side. Recording retains the original command stream. */
        if (!fb->rec) {
            int first = fb->y_base - fb->oy;
            if (oy + PAGE_H + 2 <= first || oy - 2 >= first + fb->h) continue;
        }
        if (pg == 0) {
            ui_label(fb, UI_CX, (int)(UI_TITLE_Y + oy), "HEART RATE", C_SEC, UI_CENTER);
            dl_draw(fb, &H.trace, oy);              /* the lists carry their own alpha */
            dl_draw(fb, &H.illu, oy);
            if (H.kout > 0.01f) draw_mode(fb, oy, H.prev_mode, H.kout, 1);
            if (H.kin > 0.01f) draw_mode(fb, oy, H.mode, H.kin, 0);
        } else draw_history(fb, oy);
    }
    /* motion blur on the pages as they slide (nos.h), not on the scroll bar */
    static nos_mb_track mb;
    float mbs = nos_mblur_step(nos_mb_track_step(&mb, off), H.vp.drag);
    if (mbs > 0 && mbs < PAGE_H * 0.5f) nos_mblur_v(fb, 0, GFX_H, mbs);
    float ba = vp_bar_alpha(&H.vp);
    if (ba > 0) ui_scrollbar(fb, H.vp.sp.x, PAGE_H * NPAGES, PAGE_H, ba);
}

static int heart_input(const nos_input *in) { return vp_input(&H.vp, in); }

const nos_screen scr_heart = { "HEART RATE", heart_enter, heart_leave, heart_tick, heart_draw, heart_input, 0 };

/* ============================================================== WIDGET
 * Last reading + a 24-column mini history + "TAP TO MEASURE". The number builds in
 * (dot sparkle, 0.30 s) as soon as the page slides into view. Without the HR module:
 * "--" on a ghost grid, [ NO HR SENSOR ], no tap hint. */
static struct {
    hdata hd;
    float t;
    int started;
    int present;
    float hist_age;        /* s since hdata was refreshed */
    char ago[24];
    uint8_t cols[24];
    int any;
} WH;

static void wgh_refresh(void)
{
    hdata_read(&WH.hd, 1);
    WH.present = (hal_ext_present() & EXT_HR) != 0;
    WH.hist_age = 0;
    WH.ago[0] = 0;
    if (WH.hd.last_epoch) {
        uint64_t now = hal_epoch_seconds();
        long d = now > WH.hd.last_epoch ? (long)(now - WH.hd.last_epoch) : 0;
        if(now<WH.hd.last_epoch)snprintf(WH.ago,sizeof WH.ago,"TIME CHANGED");
        else if (d < 90) snprintf(WH.ago, sizeof WH.ago, "JUST NOW");
        else if (d < 3600) snprintf(WH.ago, sizeof WH.ago, "%ld MIN AGO", d / 60);
        else if (d < 86400) snprintf(WH.ago, sizeof WH.ago, "%ld H AGO", d / 3600);
        else snprintf(WH.ago, sizeof WH.ago, "%ld D AGO", d / 86400);
    } else if (WH.hd.last) snprintf(WH.ago, sizeof WH.ago, "LAST READING");
}

static void wgh_shown(void)
{
    if (WH.started) return;            /* already building since the page slid in */
    wgh_refresh();
    WH.t = 0; WH.started = 1;
}

static void wgh_tick(float dt, int visible)
{
    WH.hist_age += dt;
    if (!visible) {
        if (WH.started) WH.started = 0;
        WH.t = 0;
        if (WH.hist_age > 60.0f || (!WH.hd.nhist && WH.hist_age > 5.0f)) wgh_refresh();
    } else {
        if (WH.hist_age > 60.0f) wgh_refresh();
        if (!WH.started) { wgh_refresh(); WH.t = 0; WH.started = 1; }
        else WH.t += dt;
    }
    int k = WH.started ? (int)floorf(WH.t * M_FPS + 0.5f) : 0;
    WH.any = hdata_columns(&WH.hd, WH.cols, 24, 5, k);
    if (visible) {
        nos_motion_probe("wg.heart.col0", MC_COMPONENT, 5 * 9.0f * col_grow(k, 0, 24));
        nos_motion_probe("wg.heart.col23", MC_COMPONENT, 5 * 9.0f * col_grow(k, 23, 24));
    }
}

static void wgh_draw(gfx_fb *fb)
{
    float p = pblur_baking ? 1 : WH.started ? m_clamp(WH.t / UI_ENTRY_S, 0, 1) : 0;
    int val = WH.present ? WH.hd.last : 0;
    ui_label(fb, UI_CX, UI_TITLE_Y, "HEART RATE", C_SEC, UI_CENTER);
    dot_icon_anim(fb, DOT_ICON_HEART, UI_CX, 112, 3.0f, 2.4f, WH.present ? C_PRI : C_DIS, p, DOT_REVEAL_CENTER);
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, val ? C_WHITE : C_DIS, val ? 0 : C_GHOST, 1, 1 };
    char b[8];
    fmt_bpm(b, sizeof b, val);
    dot_text_anim(fb, &st, UI_CX, 146, b, 1, val ? p : 1, DOT_REVEAL_RANDOM, 5);
    ui_label(fb, UI_CX, 260, "BPM", C_SEC, UI_CENTER);
    if (!WH.present) ui_state(fb, UI_CX, 288, "NO HR SENSOR", C_DIS);
    else if (WH.ago[0]) ui_label(fb, UI_CX, 288, WH.ago, C_DIS, UI_CENTER);
    else ui_label(fb, UI_CX, 288, "NO READINGS YET", C_DIS, UI_CENTER);
    const float pitch = 9.0f;
    float x0 = UI_CX - 12 * pitch;
    /* no module: an empty grid (ghost cells only), no lit history under [ NO HR SENSOR ] */
    static const uint8_t k_none[24];
    uint8_t cols[24];
    if (pblur_baking) hdata_columns(&WH.hd, cols, 24, 5, 1000);
    dot_columns(fb, x0, 308, 24, 5, pitch, 6.6f, WH.present ? (pblur_baking ? cols : WH.cols) : k_none, C_SEC, C_GHOST);
    if (WH.present) ui_label(fb, UI_CX, 394, "TAP TO MEASURE", C_DIS, UI_CENTER);
}

static void wgh_tap(int x, int y) { nos_open(&scr_heart, x, y); }

const nos_widget wg_heart = { "HEART", wgh_tick, wgh_draw, wgh_tap, wgh_shown, 0 };

/* UI-task diagnostic: return the actual pager state without ticking/drawing. */
void fit_heart_scroll_snapshot(nos_scroll_snapshot *s)
{
    *s = (nos_scroll_snapshot){ 2, 2, H.vp.page, NPAGES, H.vp.drag, H.vp.sp.x, (NPAGES - 1) * PAGE_H };
}
