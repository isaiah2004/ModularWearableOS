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
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
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

static float qz(float v, float steps) { return floorf(v * steps + 0.5f) / steps; }

/* Neutral grey at a fraction of a grey token (ui_dim rounds the 5/6-bit channels
 * separately, which tints dark greys magenta). */
static uint16_t gdim(uint16_t c, float k)
{
    float lvl = (float)((c >> 5) & 63) / 63.0f * m_clamp(k, 0, 1);
    int r = (int)lroundf(lvl * 31.0f), g = (int)lroundf(lvl * 63.0f);
    return (uint16_t)(r << 11 | g << 5 | r);
}

/* ------------------------------------------------------------ dot lists
 * A list of dots prepared in tick and drawn as one recorded command. */
typedef struct { float x, y, d; uint16_t c; } hdot;
typedef struct { hdot *d; int n, cap; int x0, y0, x1, y1; unsigned hash; } dlist;

static void dl_alloc(dlist *l, int cap)
{
    if (l->d) return;
    l->d = (hdot *)hal_big_alloc((uint32_t)(sizeof(hdot) * (unsigned)cap));
    l->cap = l->d ? cap : 0;
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
    for (int i = 0; i < l->n; i++) {
        const hdot *p = &l->d[i];
        float y = p->y + (float)o, e = p->d * 0.5f + 1.0f;
        if (y + e <= ylo || y - e >= yhi) continue;
        dot_disc(fb, p->x, y, p->d, p->c);
    }
}

/* -------------------------------------------------------- vertical pager */
typedef struct {
    mspring sp;
    int page, n, drag;
    float grab, dy0;
    float bar_k, bar_t;
} vpager;

static void vp_init(vpager *v, int n)
{
    msp_init(&v->sp, 0, SPRING_PAGE);
    v->page = 0; v->n = n; v->drag = 0; v->bar_k = 0; v->bar_t = -10;
}
static int vp_input(vpager *v, const nos_input *in)
{
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
    float target = nos_t - v->bar_t < 0.7f ? 1.0f : 0.0f;
    float k = dt * (target > v->bar_k ? 10.0f : 3.5f);
    v->bar_k += (target - v->bar_k) * (k > 1 ? 1 : k);
    if (v->bar_k < 0.04f) v->bar_k = 0;
    v->bar_k = qz(v->bar_k, 16.0f);
}

/* ---------------------------------------------------------- history data */
typedef struct {
    uint8_t hist[144];
    int nhist;
    int resting, mn, mx, last, spo2;
    uint32_t last_epoch;
} hdata;

/* Fill from the heart service; fall back to the sensor's own history while the
 * service has none (svc is the source of truth once it reports). */
static void hdata_read(hdata *h, int allow_hal)
{
    const svc_heart_t *s = svc_heart();
    memset(h, 0, sizeof *h);
    if (s) {
        h->nhist = s->nhist > 144 ? 144 : s->nhist;
        if (h->nhist > 0) memcpy(h->hist, s->hist, (size_t)h->nhist);
        h->resting = s->resting; h->mn = s->min_today; h->mx = s->max_today;
        h->last = s->bpm_last; h->last_epoch = s->bpm_last_epoch; h->spo2 = s->spo2_last;
    }
    if (h->nhist <= 0 && allow_hal && (hal_ext_present() & EXT_HR)) h->nhist = hal_hr_history(h->hist, 144);
    if (h->nhist < 0) h->nhist = 0;
    /* derive what the service didn't give */
    int lo = 255, hi = 0, cnt = 0, sum_low = 0;
    for (int i = 0; i < h->nhist; i++) {
        int v = h->hist[i];
        if (!v) continue;
        if (v < lo) lo = v;
        if (v > hi) hi = v;
        cnt++;
    }
    if (cnt) {
        if (!h->mn) h->mn = lo;
        if (!h->mx) h->mx = hi;
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

/* Columns: average of the non-zero samples in each bucket, scaled into lo..hi. */
static int hdata_columns(const hdata *h, uint8_t *out, int cols, int rows, float grow_p, int stagger)
{
    int lo = 255, hi = 0, any = 0;
    for (int i = 0; i < h->nhist; i++) if (h->hist[i]) { any = 1; if (h->hist[i] < lo) lo = h->hist[i]; if (h->hist[i] > hi) hi = h->hist[i]; }
    if (!any) { memset(out, 0, (size_t)cols); return 0; }
    float flo = (float)lo - 3.0f, fhi = (float)hi + 4.0f;
    if (fhi - flo < 30.0f) { float m = (flo + fhi) * 0.5f; flo = m - 15.0f; fhi = m + 15.0f; }
    for (int c = 0; c < cols; c++) {
        int a = c * h->nhist / cols, b = (c + 1) * h->nhist / cols, n = 0, s = 0;
        for (int i = a; i < b; i++) if (h->hist[i]) { s += h->hist[i]; n++; }
        float g = stagger ? bezier_ease(&EASE_STANDARD, m_stagger(grow_p, c, cols, 0.45f)) : grow_p;
        float f = n ? ((float)s / (float)n - flo) / (fhi - flo) : 0;
        if (n) { f = floorf(m_clamp(f, 0, 1) * (float)rows + 0.5f) / (float)rows; if (f < 1.0f / (float)rows) f = 1.0f / (float)rows; }
        out[c] = (uint8_t)lroundf(m_clamp(f * g, 0, 1) * 255.0f);
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
    int present;
    int mode;
    float mode_t;
    float off_t;                /* time continuously off the wrist */
    float good_t;               /* time continuously lockable */
    int locked;
    float lock_t;
    /* the number */
    int bpm_shown, bpm_prev;
    float bpm_morph;            /* 0..1, 1 = settled */
    float bpm_change_t;
    int spo2;
    float t;
    /* pages */
    vpager vp;
    hdata hd;
    float hist_p;
    int hist_started;
    /* prepared */
    dlist trace, illu;
    float u;                    /* measuring -> locked layout, eased */
    float heart_scale;
    float now_h;                /* local clock, hours */
} H;

static float trace_y(float v, float yc, float h)
{
    float lo = H.lo, hi = H.hi;
    if (hi - lo < 40) { float m = (lo + hi) * 0.5f; lo = m - 20; hi = m + 20; }
    return yc + h * 0.5f - (v - lo) / (hi - lo) * h;
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
        float a = dt * 2.5f;
        H.lo += (lo - H.lo) * a;
        H.hi += (hi - H.hi) * a;
    }
}

/* Lay out the trace: rigid chain of dots, samples joined by interpolated dots. */
static void build_trace(float xl, float xr, float yc, float h, uint16_t ink, float dsz)
{
    dlist *l = &H.trace;
    dl_reset(l);
    if (!l->d || !H.ph_ok) return;
    float pitch = (xr - xl) / (float)WIN;
    uint32_t kn = (uint32_t)H.ph;
    float frac = H.ph - (float)kn;
    float px = 0, py = 0;
    int have = 0;
    const float spacing = dsz * 0.8f;
    for (int i = WIN + 1; i >= -1; i--) {
        float v, x, y;
        if (i >= 0) {
            if ((uint32_t)i > kn) continue;
            uint32_t k = kn - (uint32_t)i;
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
        if (x < xl - pitch) { px = x; py = y; have = 1; continue; }
        if (have) {
            float dx = x - px, dy = y - py, dist = sqrtf(dx * dx + dy * dy);
            int nf = (int)(dist / spacing);
            for (int f = 1; f <= nf; f++) {
                float t = (float)f / (float)(nf + 1);
                float fx = px + dx * t, fy = py + dy * t;
                if (fx < xl) continue;
                float fade = m_clamp((fx - xl) / 70.0f, 0, 1);
                dl_add(l, fx, fy, dsz, gdim(ink, qz(0.2f + 0.8f * fade, 4.0f)));
            }
        }
        if (x >= xl && i >= 0) {
            float fade = m_clamp((x - xl) / 70.0f, 0, 1);
            dl_add(l, x, y, dsz, gdim(ink, qz(0.2f + 0.8f * fade, 4.0f)));
        }
        px = x; py = y; have = 1;
    }
    /* pen dot */
    if (have) dl_add(l, xr, py, dsz * 1.7f, C_WHITE);
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
    uint16_t dim = gdim(C_DIS, k), mid = gdim(C_SEC, k), lit = gdim(C_PRI, k);
    /* wrist: an arc of a big circle */
    for (int i = -7; i <= 7; i++) {
        float a = (float)i * 0.07f;
        float x = cx + 230.0f * sinf(a), y = base + 230.0f * (1.0f - cosf(a));
        dl_add(l, x, y, 7.0f, touch && i >= -1 && i <= 1 ? lit : dim);
    }
    /* strap: follows the wrist a little above it, lifting with the body */
    for (int s = -1; s <= 1; s += 2)
        for (int j = 0; j < 5; j++) {
            float a = (float)s * (0.25f + (float)j * 0.07f);
            float x = cx + 230.0f * sinf(a), y = base - 12.0f + 230.0f * (1.0f - cosf(a));
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

static void heart_enter(void)
{
    memset(H.v, 128, sizeof H.v);
    H.n = WIN + 8;   /* a flat baseline the live signal scrolls in over */ H.have_last = 0; H.acc = 0; H.ph = 0; H.ph_ok = 0;
    H.lo = 100; H.hi = 156; H.npk = 0; H.last_peak = 0; H.beat_t = 10;
    H.off_t = 0; H.good_t = 0; H.locked = 0; H.lock_t = 0;
    H.bpm_shown = 0; H.bpm_prev = 0; H.bpm_morph = 1; H.bpm_change_t = 0; H.spo2 = 0;
    H.t = 0; H.u = 0; H.heart_scale = 1;
    dl_alloc(&H.trace, TRACE_MAX);
    dl_alloc(&H.illu, ILLU_MAX);
    dl_reset(&H.trace); dl_reset(&H.illu);
    memset(&H.r, 0, sizeof H.r);
    H.present = (hal_ext_present() & EXT_HR) != 0;
    if (H.present) hal_hr_enable(1);
    H.mode = H.present ? M_MEASURING : M_ABSENT;
    H.mode_t = 0;
    vp_init(&H.vp, NPAGES);
    hdata_read(&H.hd, 1);
    H.hist_p = 0; H.hist_started = 0;
}

static void heart_leave(void)
{
    if (H.present) hal_hr_enable(0);
    if (H.locked && H.bpm_shown > 0) svc_heart_record(H.bpm_shown, H.spo2);
}

static void set_mode(int m) { if (H.mode != m) { H.mode = m; H.mode_t = 0; } }

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
    if (H.present) {
        H.present = hal_hr_get(&H.r);
        if (!H.present) set_mode(M_ABSENT);
    }
    if (H.present) {
        ingest(H.r.wave, dt);
        playhead_tick(dt);
        if (!H.r.on_wrist) H.off_t += dt; else H.off_t = 0;
        int lockable = H.r.on_wrist && H.r.bpm > 0 && H.r.quality >= LOCK_Q;
        H.good_t = lockable ? H.good_t + dt : 0;
        if (H.off_t > 1.0f) {
            if (H.mode != M_NOCONTACT) { H.locked = 0; H.good_t = 0; }
            set_mode(M_NOCONTACT);
        } else if (H.r.on_wrist || H.mode != M_NOCONTACT) {
            if (!H.locked && H.good_t >= 0.5f) {
                H.locked = 1; H.lock_t = 0;
                H.bpm_shown = H.r.bpm; H.bpm_prev = H.r.bpm; H.bpm_morph = 1; H.bpm_change_t = 0;
                if (nos_prefs.haptics) hal_haptic(HAPTIC_HEARTBEAT);
            }
            set_mode(H.locked ? M_LOCKED : M_MEASURING);
        }
        if (H.locked) {
            H.lock_t += dt;
            H.bpm_change_t += dt;
            if (H.bpm_morph < 1) H.bpm_morph = m_clamp(H.bpm_morph + dt / 0.3f, 0, 1);
            if (H.r.bpm > 0 && H.r.bpm != H.bpm_shown && H.bpm_morph >= 1 && H.bpm_change_t > 1.2f) {
                H.bpm_prev = H.bpm_shown; H.bpm_shown = H.r.bpm; H.bpm_morph = 0; H.bpm_change_t = 0;
            }
            if (H.r.spo2 > 0) H.spo2 = H.r.spo2;
        }
        if (H.mode == M_MEASURING || (H.mode == M_LOCKED && H.lock_t < 20.0f)) nos_keep_awake();
    }
    /* layout */
    H.u = H.locked ? bezier_ease(&EASE_STANDARD, m_clamp(H.lock_t / 0.45f, 0, 1)) : 0;
    float bt = H.beat_t;
    float pulse = bt < 0.067f ? bt / 0.067f : expf(-(bt - 0.067f) / 0.16f);
    H.heart_scale = 1.0f + 0.55f * qz(m_clamp(pulse, 0, 1), 16.0f);   /* the outline swells bold on each beat */
    if (H.mode == M_MEASURING || H.mode == M_LOCKED) {
        float u = H.u;
        float xl = m_lerp(58, 118, u), xr = m_lerp(408, 348, u);
        float yc = m_lerp(238, 324, u), h = m_lerp(100, 38, u);
        build_trace(xl, xr, yc, h, H.locked ? C_SEC : C_PRI, m_lerp(4.5f, 3.5f, u));
    } else dl_reset(&H.trace);
    if (H.mode == M_NOCONTACT) build_illu(H.mode_t, qz(m_clamp(H.mode_t / 0.3f, 0, 1), 6.0f));
    else dl_reset(&H.illu);
    /* history page builds when first seen */
    if (!H.hist_started && H.vp.sp.x > PAGE_H * 0.45f) { H.hist_started = 1; hdata_read(&H.hd, 1); }
    if (H.hist_started && H.hist_p < 1) H.hist_p = m_clamp(H.hist_p + dt / 0.55f, 0, 1);
}

/* ---- page 0 ---- */
static void draw_absent(gfx_fb *fb, float oy)
{
    float k = qz(m_clamp(H.mode_t / 0.3f, 0, 1), 6.0f);
    dot_icon_anim(fb, DOT_ICON_HEART, UI_CX, 170 + oy, 5.0f, 4.0f, C_DIS, m_clamp(H.mode_t / 0.4f, 0, 1), DOT_REVEAL_CENTER);
    ui_text(fb, UI_F_BODY, UI_CX, (int)(268 + oy), "Module not connected", gdim(C_PRI, k), UI_CENTER, 0);
    ui_text_wrap(fb, UI_F_BODY, UI_CX, (int)(306 + oy), 330, 30, 3,
                 "Add the heart rate strap module to measure BPM and SpO2.", gdim(C_SEC, k), UI_CENTER);
}

static void draw_nocontact(gfx_fb *fb, float oy)
{
    float k = qz(m_clamp(H.mode_t / 0.3f, 0, 1), 6.0f);
    dl_draw(fb, &H.illu, oy);
    ui_text(fb, UI_F_BODY, UI_CX, (int)(304 + oy), "Place the watch snugly", gdim(C_PRI, k), UI_CENTER, 0);
    ui_state(fb, UI_CX, (int)(346 + oy), "NO CONTACT", gdim(C_DIS, k));
}

static void draw_live(gfx_fb *fb, float oy)
{
    float u = H.u;
    float k = qz(m_clamp(H.mode_t / 0.25f, 0, 1), 6.0f);
    /* heart: white while measuring, red once locked */
    float hy = m_lerp(134, 112, u) + oy;
    float pitch = m_lerp(3.4f, 3.0f, u);
    uint16_t hc = H.locked ? ui_mix(C_PRI, C_RED, qz(m_clamp(H.lock_t / 0.2f, 0, 1), 4.0f)) : gdim(C_PRI, k);
    dot_icon_anim(fb, DOT_ICON_HEART, UI_CX, hy, pitch, qz(pitch * 0.8f * H.heart_scale, 4.0f), hc,
                  m_clamp(H.t / 0.35f, 0, 1), DOT_REVEAL_CENTER);
    dl_draw(fb, &H.trace, oy);
    /* [ MEASURING ] fades out as the number arrives */
    float km = qz(m_clamp(1.0f - u * 2.0f, 0, 1), 6.0f) * k;
    if (km > 0.01f) {
        ui_state(fb, UI_CX, (int)(340 + oy), "MEASURING", gdim(C_SEC, km));
        float ql = floorf(m_clamp(H.r.quality / LOCK_Q, 0, 1) * 5.0f + 0.5f);
        dot_bar(fb, UI_CX - 2 * 14.0f, 376 + oy, 5, 14.0f, 5.0f, ql,
                gdim(C_SEC, km), gdim(C_LINE2, km));
    }
    if (!H.locked) return;
    /* the number */
    dot_style st = { DOT_FONT_TALL, 10.0f, 8.0f, C_WHITE, 0, 1, 1 };
    char a[8], b[8];
    fmt_bpm(b, sizeof b, H.bpm_shown);
    float bp = m_clamp(H.lock_t / 0.45f, 0, 1);
    if (bp < 1) dot_text_anim(fb, &st, UI_CX, 148 + oy, b, 1, bp, DOT_REVEAL_CENTER, 11);
    else if (H.bpm_morph < 1) { fmt_bpm(a, sizeof a, H.bpm_prev); dot_text_morph(fb, &st, UI_CX, 148 + oy, a, b, 1, qz(H.bpm_morph, 16.0f)); }
    else dot_text_a(fb, &st, UI_CX, 148 + oy, b, 1);
    float kl = qz(m_clamp((H.lock_t - 0.15f) / 0.3f, 0, 1), 6.0f);
    ui_label(fb, UI_CX, (int)(272 + oy), "BPM", gdim(C_SEC, kl), UI_CENTER);
    /* SpO2 + signal quality on one line */
    char s[24];
    if (H.spo2 > 0) snprintf(s, sizeof s, "SPO2 %d%%", H.spo2);
    else snprintf(s, sizeof s, "SPO2 --");
    ui_label(fb, UI_CX, (int)(378 + oy), s, gdim(H.spo2 ? C_SEC : C_DIS, kl), UI_CENTER);
    float q = m_clamp((H.r.quality - 0.4f) / 0.5f, 0, 1) * 5.0f;
    dot_bar(fb, UI_CX - 2 * 12.0f, 404 + oy, 5, 12.0f, 4.5f, floorf(q + 0.5f) < 1 ? 1 : floorf(q + 0.5f),
            gdim(C_SEC, kl), gdim(C_LINE2, kl));
}

/* ---- page 1: today ---- */
static void stat_block(gfx_fb *fb, float cx, float oy, int v, const char *label, float p)
{
    char b[8];
    fmt_bpm(b, sizeof b, v);
    dot_style st = { DOT_FONT_TALL, 5.0f, 4.0f, v ? C_WHITE : C_DIS, 0, 1, 1 };
    dot_text_anim(fb, &st, cx, 112 + oy, b, 1, p, DOT_REVEAL_RAIN, 9);
    ui_label(fb, (int)cx, (int)(184 + oy), label, gdim(C_SEC, qz(m_clamp(p * 1.4f, 0, 1), 6.0f)), UI_CENTER);
}

static void draw_history(gfx_fb *fb, float oy)
{
    const hdata *h = &H.hd;
    float p = H.hist_p;
    ui_label(fb, UI_CX, (int)(78 + oy), "TODAY", C_SEC, UI_CENTER);
    stat_block(fb, UI_CX - 120, oy, h->resting, "RESTING", m_stagger(p, 0, 3, 0.6f));
    stat_block(fb, UI_CX, oy, h->mn, "MIN", m_stagger(p, 1, 3, 0.6f));
    stat_block(fb, UI_CX + 120, oy, h->mx, "MAX", m_stagger(p, 2, 3, 0.6f));
    const int cols = 48, rows = 8;
    const float pitch = 7.0f, d = 5.4f;
    float x0 = UI_CX - cols * pitch * 0.5f, y0 = 226 + oy;
    uint8_t vals[48];
    float pb = bezier_ease(&EASE_STANDARD, m_clamp((p - 0.15f) / 0.85f, 0, 1));
    int any = hdata_columns(h, vals, cols, rows, pb, 1);
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
    int last = H.locked && H.bpm_shown ? H.bpm_shown : h->last;
    if (any && last) {
        char b[32];
        snprintf(b, sizeof b, "LAST %d BPM", last);
        ui_label(fb, UI_CX, (int)(ay + 40), b, C_DIS, UI_CENTER);
    }
}

static void heart_draw(gfx_fb *fb)
{
    float off = floorf(H.vp.sp.x + 0.5f);
    for (int pg = 0; pg < NPAGES; pg++) {
        float oy = pg * PAGE_H - off;
        if (oy <= -PAGE_H || oy >= PAGE_H) continue;
        if (pg == 0) {
            ui_label(fb, UI_CX, (int)(70 + oy), "HEART RATE", C_SEC, UI_CENTER);
            if (H.mode == M_ABSENT) draw_absent(fb, oy);
            else if (H.mode == M_NOCONTACT) draw_nocontact(fb, oy);
            else draw_live(fb, oy);
        } else draw_history(fb, oy);
    }
    ui_scrollbar(fb, H.vp.sp.x, PAGE_H * NPAGES, PAGE_H, H.vp.bar_k);
}

static int heart_input(const nos_input *in) { return vp_input(&H.vp, in); }

const nos_screen scr_heart = { "HEART RATE", heart_enter, heart_leave, heart_tick, heart_draw, heart_input, 0 };

/* ============================================================== WIDGET
 * Last reading + a 24-column mini history + "TAP TO MEASURE". The number builds in
 * with a dot sparkle and the columns grow when the pager settles on the page. */
static struct {
    hdata hd;
    float t;
    int started;
    float hist_age;        /* s since hdata was refreshed */
    char ago[24];
    uint8_t cols[24];
    int any;
} WH;

static void wgh_refresh(void)
{
    hdata_read(&WH.hd, 1);
    WH.hist_age = 0;
    WH.ago[0] = 0;
    if (WH.hd.last_epoch) {
        uint64_t now = hal_epoch_seconds();
        long d = now > WH.hd.last_epoch ? (long)(now - WH.hd.last_epoch) : 0;
        if (d < 90) snprintf(WH.ago, sizeof WH.ago, "JUST NOW");
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
    float p = WH.started ? m_clamp((WH.t - 0.08f) / 0.5f, 0, 1) : 0;
    WH.any = hdata_columns(&WH.hd, WH.cols, 24, 5, p, 1);
}

static void wgh_draw(gfx_fb *fb)
{
    float p = WH.started ? m_clamp(WH.t / 0.4f, 0, 1) : 0;
    float k = qz(m_clamp(WH.t / 0.3f, 0, 1), 6.0f);
    if (!WH.started) k = 0;
    dot_icon(fb, DOT_ICON_HEART, UI_CX, 108, 3.0f, 2.4f, C_PRI);
    dot_style st = { DOT_FONT_TALL, 10.0f, 8.0f, WH.hd.last ? C_WHITE : C_DIS, WH.hd.last ? 0 : C_GHOST, 1, 1 };
    char b[8];
    fmt_bpm(b, sizeof b, WH.hd.last);
    dot_text_anim(fb, &st, UI_CX, 142, b, 1, WH.hd.last ? p : 1, DOT_REVEAL_RANDOM, 5);
    ui_label(fb, UI_CX, 266, "BPM", C_SEC, UI_CENTER);
    if (WH.ago[0]) ui_label(fb, UI_CX, 294, WH.ago, gdim(C_DIS, k), UI_CENTER);
    else ui_label(fb, UI_CX, 294, "NO READINGS YET", C_DIS, UI_CENTER);
    const float pitch = 9.0f;
    float x0 = UI_CX - 12 * pitch;
    dot_columns(fb, x0, 312, 24, 5, pitch, 6.6f, WH.cols, C_SEC, C_GHOST);
    ui_label(fb, UI_CX, 396, "TAP TO MEASURE", C_DIS, UI_CENTER);
}

static void wgh_tap(int x, int y) { nos_open(&scr_heart, x, y); }

const nos_widget wg_heart = { "HEART", wgh_tick, wgh_draw, wgh_tap, wgh_shown };
