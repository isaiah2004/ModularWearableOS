/* out_nav.c — Navigate (u-blox M10 GPS on the strap), outdoor team.
 *
 * Recording lives in the track service (nos/svc_track.h): a track keeps going after
 * the app is left, and this screen only renders it.
 *
 * States: module absent; searching (sky plot: satellites at their real azimuth /
 * elevation, size by SNR, white once used or locked); fixed: three pages in a
 * horizontal pager
 *   1 NOW     speed, coordinates, altitude, accuracy, Start / hold-to-Stop
 *   2 TRACK   the recorded path as a dot polyline scaled to fit, you = red dot
 *   3 BACK    an arrow to the track's start (rotated by the compass when calibrated,
 *             else by the course over ground) and the distance.
 * A lost fix keeps the last values (dimmed, "[ NO FIX ] 0:12 AGO") for 30 s before the
 * sky plot returns. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "svc_track.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define PI_F    3.14159265f
#define D2R     0.017453292519943295
#define EARTH_R 6371000.0
#define NPAGE   3
#define PAGE_W  466.0f
#define MAXDOTS 360
#define SKY_CY  226.0f
#define SKY_R   124.0f
#define IRIS_LAG 0.17f
#define STALE_S 30.0f        /* lost fix: keep the last values this long */
#define HOLD_S  0.6f         /* hold to stop a track */
#define SUMMARY_S 3.0f

/* ------------------------------------------------------------- pager */
typedef struct { mspring sp; int n, page, dragging; float x0; } pager;
static void pg_init(pager *p, int n, int page)
{
    memset(p, 0, sizeof *p);
    p->n = n; p->page = page;
    msp_init(&p->sp, page * PAGE_W, SPRING_PAGE);
}
static int pg_input(pager *p, const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN: if (p->sp.moving) { p->sp.v = 0; p->sp.moving = 0; } p->x0 = p->sp.x; return 0;
    case IN_DRAG_START:
        if (in->axis != AXIS_X) return 0;
        p->dragging = 1; p->x0 = p->sp.x;
        /* fall through */
    case IN_DRAG: {
        if (!p->dragging) return 0;
        float x = p->x0 - in->dx, max = (p->n - 1) * PAGE_W;
        if (x < 0) x = -rubber_band(-x, PAGE_W);
        if (x > max) x = max + rubber_band(x - max, PAGE_W);
        msp_set(&p->sp, x);
        return 1;
    }
    case IN_DRAG_END: {
        if (!p->dragging) return 0;
        p->dragging = 0;
        float v = -in->vx;
        float proj = p->sp.x + v * 0.18f;
        int t = (int)floorf(proj / PAGE_W + 0.5f);
        int cur = (int)floorf(p->x0 / PAGE_W + 0.5f);
        if (t > cur + 1) t = cur + 1;
        if (t < cur - 1) t = cur - 1;
        if (t < 0) t = 0;
        if (t > p->n - 1) t = p->n - 1;
        msp_kick(&p->sp, t * PAGE_W, v);
        return 1;
    }
    default: return 0;
    }
}
static int pg_tick(pager *p, float dt)
{
    msp_step(&p->sp, dt);
    int pg = (int)floorf(p->sp.x / PAGE_W + 0.5f);
    if (pg < 0) pg = 0;
    if (pg > p->n - 1) pg = p->n - 1;
    if (pg != p->page) { p->page = pg; nos_feedback(FB_TICK); return 1; }
    return 0;
}
static float pg_pos(const pager *p) { return floorf(p->sp.x + 0.5f); }

/* ------------------------------------------------------------- morphing value */
typedef struct { char cur[16], prev[16]; float t0; } mval;
static void mv_set(mval *m, const char *s)
{
    if (!strcmp(m->cur, s)) return;
    if (strlen(m->cur) == strlen(s) && m->cur[0]) { strcpy(m->prev, m->cur); m->t0 = nos_t; }
    else { strcpy(m->prev, s); m->t0 = -10; }
    snprintf(m->cur, sizeof m->cur, "%s", s);
}
static void mv_draw(gfx_fb *fb, const dot_style *st, float x, float y, const mval *m, float build, unsigned seed)
{
    if (build < 1.0f) { dot_text_anim(fb, st, x, y, m->cur, 1, build, DOT_REVEAL_RANDOM, seed); return; }
    float p = (nos_t - m->t0) / 0.28f;
    if (p < 1.0f) dot_text_morph(fb, st, x, y, m->prev, m->cur, 1, p);
    else dot_text_a(fb, st, x, y, m->cur, 1);
}

/* ------------------------------------------------------------- state */
static struct {
    int present;
    hal_gps g;               /* this frame's reading */
    hal_gps last;            /* last valid fix */
    int have_last;
    float last_fix_t;        /* nos_t of the last valid fix */
    int fixed;               /* g.valid now */
    int show_now;            /* NOW page instead of the sky (fixed or recently fixed) */
    int stale;               /* showing the last values without a fix */
    float fix_t;             /* when the current fix arrived */
    pager pg;
    float t0;                /* entry build */
    float sat_d[16];         /* displayed satellite dot size (grows in) */
    int pressed;
    float press_until;
    /* stop: hold */
    int holding; float hold_t0; float hold_p;
    float hint_t;            /* "HOLD TO STOP" after a short tap */
    /* summary after stop */
    int saved_state;         /* 0 none, 1 saved, 2 no card */
    float saved_t; double sum_m; uint32_t sum_ms;
    /* values (refreshed 1 Hz) */
    float val_t;
    mval m_speed, m_dist;
    /* track view */
    float (*dots)[2]; int ndots;
    unsigned track_ver, view_ver;
    float view_t;
    float you_x, you_y; int have_you;
    float start_x, start_y;
    float scale_m;           /* metres per 100 px */
    /* back to start */
    double home_dist, home_brg;
    mspring arrow;           /* unwrapped arrow angle, degrees on screen */
    float arrow_q;
    int head_src;            /* 0 north up, 1 compass, 2 course */
} N;

enum { P_NONE = 0, P_REC };

static int rows_hit(const gfx_fb *fb, float y0, float y1)
{
    if (fb->rec) return 1;
    float lo = (float)(fb->y_base - fb->oy), hi = lo + (float)fb->h;
    return y1 >= lo && y0 < hi;
}
static float wrap180(float a)
{
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}
static double hav_m(double la1, double lo1, double la2, double lo2)
{
    double dla = (la2 - la1) * D2R, dlo = (lo2 - lo1) * D2R;
    double a = sin(dla / 2) * sin(dla / 2) + cos(la1 * D2R) * cos(la2 * D2R) * sin(dlo / 2) * sin(dlo / 2);
    return 2 * EARTH_R * atan2(sqrt(a), sqrt(1 - a));
}
static double bearing_deg(double la1, double lo1, double la2, double lo2)
{
    double y = sin((lo2 - lo1) * D2R) * cos(la2 * D2R);
    double x = cos(la1 * D2R) * sin(la2 * D2R) - sin(la1 * D2R) * cos(la2 * D2R) * cos((lo2 - lo1) * D2R);
    double b = atan2(y, x) / D2R;
    return b < 0 ? b + 360 : b;
}
static void fmt_dist(char *o, int n, double m)
{
    if (m < 1000) snprintf(o, (size_t)n, "%d M", (int)m);
    else snprintf(o, (size_t)n, "%.2f KM", m / 1000.0);
}

/* ------------------------------------------------------------- track view */
static void rebuild_track_view(void)
{
    const svc_track_pt *P;
    int np = svc_track_points(&P);
    N.ndots = 0; N.have_you = 0;
    N.view_ver++;
    if (np < 1) return;
    if (!N.dots) N.dots = (float (*)[2])hal_big_alloc(MAXDOTS * 2 * sizeof(float));
    if (!N.dots) return;
    const hal_gps *g = &N.last;
    /* equirectangular metres around the first point */
    double la0 = P[0].lat, lo0 = P[0].lon, k = cos(la0 * D2R) * D2R * EARTH_R, kl = D2R * EARTH_R;
    double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
    for (int i = 0; i < np; i++) {
        double x = (P[i].lon - lo0) * k, y = (P[i].lat - la0) * kl;
        minx = x < minx ? x : minx; maxx = x > maxx ? x : maxx;
        miny = y < miny ? y : miny; maxy = y > maxy ? y : maxy;
    }
    if (N.have_last) {
        double x = (g->lon - lo0) * k, y = (g->lat - la0) * kl;
        minx = x < minx ? x : minx; maxx = x > maxx ? x : maxx;
        miny = y < miny ? y : miny; maxy = y > maxy ? y : maxy;
    }
    double span = maxx - minx > maxy - miny ? maxx - minx : maxy - miny;
    if (span < 60) span = 60;
    const double box = 236.0;                       /* px, fits the circle with margin */
    double s = box / span, cx = (minx + maxx) / 2, cy = (miny + maxy) / 2;
    N.scale_m = (float)(100.0 / s);
    const float sx = UI_CX, sy = UI_CY - 8;
    double len = 0;
    for (int i = 1; i < np; i++) {
        double ax = (P[i - 1].lon - lo0) * k, ay = (P[i - 1].lat - la0) * kl;
        double bx = (P[i].lon - lo0) * k, by = (P[i].lat - la0) * kl;
        len += sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)) * s;
    }
    float step = 9.0f;
    if (len / step > MAXDOTS - 2) step = (float)(len / (MAXDOTS - 2));
    float carry = 0;
    for (int i = 0; i < np; i++) {
        float bx = (float)(sx + ((P[i].lon - lo0) * k - cx) * s);
        float by = (float)(sy - ((P[i].lat - la0) * kl - cy) * s);
        if (i == 0) {
            N.start_x = bx; N.start_y = by;
            N.dots[N.ndots][0] = bx; N.dots[N.ndots][1] = by; N.ndots++;
            continue;
        }
        float ax = (float)(sx + ((P[i - 1].lon - lo0) * k - cx) * s);
        float ay = (float)(sy - ((P[i - 1].lat - la0) * kl - cy) * s);
        float dx = bx - ax, dy = by - ay, sl = sqrtf(dx * dx + dy * dy);
        float t = step - carry;
        while (t <= sl && N.ndots < MAXDOTS) {
            N.dots[N.ndots][0] = floorf((ax + dx * t / sl) * 2 + 0.5f) / 2;
            N.dots[N.ndots][1] = floorf((ay + dy * t / sl) * 2 + 0.5f) / 2;
            N.ndots++;
            t += step;
        }
        carry = sl - (t - step);
    }
    if (N.have_last) {
        N.you_x = floorf((float)(sx + ((g->lon - lo0) * k - cx) * s) + 0.5f);
        N.you_y = floorf((float)(sy - ((g->lat - la0) * kl - cy) * s) + 0.5f);
        N.have_you = 1;
    }
}

/* ------------------------------------------------------------- lifecycle */
static void nav_enter(void)
{
    if (nos_revealed()) return;
    int page = N.pg.n ? N.pg.page : 0;
    float (*dots)[2] = N.dots;
    memset(&N, 0, sizeof N);
    N.dots = dots;
    pg_init(&N.pg, NPAGE, page);
    N.t0 = nos_t + IRIS_LAG;
    N.fix_t = -10; N.last_fix_t = -100; N.val_t = -100; N.saved_t = -100; N.hint_t = -100;
    N.track_ver = svc_track_version() + 1;          /* force a first build of the view */
    msp_init(&N.arrow, 0, 0.32f, 0.9f);
    hal_gps_enable(1);
}
static void nav_leave(void)
{
    N.holding = 0;
    if (!svc_track_active()) hal_gps_enable(0);
}

static void stop_track(void)
{
    N.sum_m = svc_track_distance();
    N.sum_ms = svc_track_elapsed_ms();
    int r = svc_track_stop();
    N.saved_state = r > 0 ? 1 : 2;
    N.saved_t = nos_t;
    nos_feedback(r > 0 ? FB_SUCCESS : FB_ERROR);
}

static void nav_tick(float dt)
{
    svc_track_tick(dt);                 /* no-op once svc_tick drives it this frame */
    N.present = hal_gps_get(&N.g) && (hal_ext_present() & EXT_GPS);
    if (!N.present) { N.t0 = N.t0 < nos_t ? N.t0 : nos_t; return; }
    hal_gps_enable(1);
    int was = N.fixed;
    N.fixed = N.g.valid;
    if (N.fixed) { N.last = N.g; N.have_last = 1; N.last_fix_t = nos_t; }
    if (N.fixed && !was) {
        if (!N.show_now) { N.fix_t = nos_t; nos_feedback(FB_SUCCESS); }   /* sky -> NOW */
        N.val_t = -100;
    }
    int prev_now = N.show_now;
    N.stale = !N.fixed && N.have_last && nos_t - N.last_fix_t < STALE_S;
    N.show_now = N.fixed || N.stale;
    if (prev_now && !N.show_now) N.t0 = nos_t;      /* back to the sky after 30 s: rebuild it */
    /* satellites grow in / out */
    for (int i = 0; i < 16; i++) {
        float t = i < N.g.nsnr && N.g.snr[i] ? 5.0f + N.g.snr[i] * 0.28f : 0.0f;
        N.sat_d[i] += (t - N.sat_d[i]) * m_clamp(dt * 6.0f, 0, 1);
        if (fabsf(t - N.sat_d[i]) < 0.15f) N.sat_d[i] = t;
        N.sat_d[i] = floorf(N.sat_d[i] * 4 + 0.5f) / 4;
    }
    /* values at 1 Hz */
    if (N.have_last && nos_t - N.val_t >= 1.0f) {
        N.val_t = nos_t;
        char t[16];
        float v = N.last.speed_mps * (nos_prefs.units_metric ? 3.6f : 2.23694f);
        snprintf(t, sizeof t, "%.1f", (double)(floorf(v * 10 + 0.5f) / 10));
        mv_set(&N.m_speed, t);
    }
    /* track view: rebuild when a point lands, and ~1 Hz for the "you" dot */
    unsigned tv = svc_track_version();
    if (tv != N.track_ver || (svc_track_points(0) && nos_t - N.view_t > 1.0f)) {
        N.track_ver = tv; N.view_t = nos_t;
        rebuild_track_view();
    }
    if (pg_tick(&N.pg, dt)) N.hint_t = -100;
    /* hold to stop */
    if (N.holding) {
        N.hold_p = m_clamp((nos_t - N.hold_t0) / HOLD_S, 0, 1);
        N.hold_p = floorf(N.hold_p * 40.0f) / 40.0f;
        if (N.hold_p >= 1.0f) { N.holding = 0; N.pressed = 0; stop_track(); }
    }
    if (N.pressed && N.press_until > 0 && nos_t > N.press_until && !N.holding) { N.pressed = 0; N.press_until = 0; }
    /* back to start */
    const svc_track_pt *P;
    int np = svc_track_points(&P);
    if (N.have_last && np) {
        N.home_dist = hav_m(N.last.lat, N.last.lon, P[0].lat, P[0].lon);
        N.home_brg = bearing_deg(N.last.lat, N.last.lon, P[0].lat, P[0].lon);
        float head = 0;
        hal_mag m;
        if (hal_mag_get(&m) && m.valid && m.calibrated) { head = m.heading_deg; N.head_src = 1; }
        else if (N.last.speed_mps > 0.8f) { head = N.last.course_deg; N.head_src = 2; }
        else N.head_src = 0;
        float want = (float)N.home_brg - head;
        float d = wrap180(want - N.arrow.target);
        if (fabsf(d) > 0.5f) msp_to(&N.arrow, N.arrow.target + d);
        msp_step(&N.arrow, dt);
        N.arrow_q = floorf(N.arrow.x * 2 + 0.5f) / 2;
        if (nos_t - N.val_t < dt * 1.5f) {        /* same 1 Hz beat as the speed */
            char t[16];
            if (N.home_dist < 1000) snprintf(t, sizeof t, "%d", (int)(lround(N.home_dist / 5) * 5));
            else snprintf(t, sizeof t, "%.1f", N.home_dist / 1000.0);
            mv_set(&N.m_dist, t);
        }
    }
    nos_keep_awake();
}

/* ------------------------------------------------------------- shared chrome */
static float build_at(float delay) { return m_clamp((nos_t - N.t0 - delay) / UI_ENTRY_S, 0, 1); }

/* The header slot (title line): recording state, fix state or the screen title. The
 * recording dot is red only where nothing else claims the screen's one red. */
static void header(gfx_fb *fb, int red, const char *title)
{
    char t[48];
    if (svc_track_active()) {
        char d[16], m[16];
        ui_fmt_dur(d, sizeof d, svc_track_elapsed_ms(), 0);
        fmt_dist(m, sizeof m, svc_track_distance());
        snprintf(t, sizeof t, "REC %s  %s", d, m);
        int w = ui_text_width(UI_F_LABEL, t, 2) + 18;
        dot_disc(fb, UI_CX - w / 2 + 4, UI_TITLE_Y - 6, 8, red ? C_RED : C_PRI);
        ui_label(fb, UI_CX - w / 2 + 18, UI_TITLE_Y, t, C_SEC, UI_LEFT);
    } else if (title) ui_title(fb, title, 0);
}

/* ------------------------------------------------------------- sky plot */
static void sky_draw(gfx_fb *fb)
{
    float b = build_at(0);
    /* leaving: on a new fix the sky builds out for 0.25 s while the NOW page waits */
    float out = N.fixed ? 1.0f - m_clamp((nos_t - N.fix_t) / 0.25f, 0, 1) : 1.0f;
    out = floorf(out * 8 + 0.5f) / 8;
    float k = b * out;
    header(fb, 1, "NAVIGATE");
    /* horizon / 30 / 60 deg rings, built dot by dot */
    dot_ring(fb, UI_CX, SKY_CY, SKY_R, 72, 2.6f * out, 0, 72 * b, C_LINE2, 0);
    dot_ring(fb, UI_CX, SKY_CY, SKY_R * 2 / 3, 48, 2.4f * out, 0, 48 * b, C_LINE2, 0);
    dot_ring(fb, UI_CX, SKY_CY, SKY_R / 3, 24, 2.2f * out, 0, 24 * b, C_LINE2, 0);
    if (b >= 1) ui_text(fb, UI_F_SMALL, UI_CX, (int)(SKY_CY - SKY_R + 22), "N", ui_dim(C_DIS, out), UI_CENTER, 0);
    dot_disc(fb, UI_CX, SKY_CY, 9.0f * k, C_WHITE);              /* you */
    for (int i = 0; i < 16; i++) {
        float d = N.sat_d[i] * k;
        if (d <= 0.2f) continue;
        /* real sky position, north up: horizon on the outer ring, zenith at the centre */
        float az = N.g.az[i] * (PI_F / 180.0f);
        float rr = (90.0f - m_clamp(N.g.el[i], 0, 90)) / 90.0f * SKY_R;
        float x = floorf((UI_CX + gfx_fast_sin(az) * rr) * 2 + 0.5f) / 2;
        float y = floorf((SKY_CY - gfx_fast_cos(az) * rr) * 2 + 0.5f) / 2;
        /* white once used (or locked strongly); others ramp from grey with SNR */
        int lit = i < N.g.sats_used || N.g.snr[i] >= 35;
        float q = m_clamp((N.g.snr[i] - 12) / 30.0f, 0, 1);
        q = floorf(q * 8 + 0.5f) / 8;
        dot_disc(fb, x, y, d, lit ? C_WHITE : ui_mix(C_LINE2, C_SEC, q));
    }
    char t[32];
    if (N.g.sats_used > 0) snprintf(t, sizeof t, "SEARCHING %d/%d", N.g.sats_used, N.g.sats_view);
    else if (N.g.sats_view > 0) snprintf(t, sizeof t, "SEARCHING  %d SATS", N.g.sats_view);
    else snprintf(t, sizeof t, "SEARCHING");
    ui_state(fb, UI_CX, 392, t, ui_dim(C_SEC, out));
}

/* ------------------------------------------------------------- pages */
static float now_t0(void) { return (N.t0 > N.fix_t + 0.25f ? N.t0 : N.fix_t + 0.25f); }
static float now_build(float delay) { return m_clamp((nos_t - now_t0() - delay) / UI_ENTRY_S, 0, 1); }
static int row_on(int i) { return nos_t - now_t0() >= i * 0.034f; }

/* dots around the Stop pill, filling clockwise from 12 o'clock while it is held */
static void hold_ring(gfx_fb *fb, float cx, float cy, float w, float h, float p)
{
    const int n = 40;
    const float r = h / 2 + 9, half = w / 2 - h / 2;
    const float straight = 2 * half, arc = PI_F * r, per = 2 * straight + 2 * arc;
    if (fb->rec) {
        gfx_rec_cmd(fb, (int)(cx - half - r - 4), (int)(cy - r - 4), (int)(cx + half + r + 4), (int)(cy + r + 4),
                    gfx_hash_f(0x401Du, p));
        return;
    }
    for (int i = 0; i < n; i++) {
        float s = (i + 0.5f) / n * per, x, y;
        /* start at top centre, go right */
        if (s < half) { x = cx + s; y = cy - r; }
        else if ((s -= half) < arc) { float a = s / r; x = cx + half + r * gfx_fast_sin(a); y = cy - r * gfx_fast_cos(a); }
        else if ((s -= arc) < straight) { x = cx + half - s; y = cy + r; }
        else if ((s -= straight) < arc) { float a = s / r; x = cx - half - r * gfx_fast_sin(a); y = cy + r * gfx_fast_cos(a); }
        else { s -= arc; x = cx - half + s; y = cy - r; }
        if (!rows_hit(fb, y - 4, y + 4)) continue;
        dot_disc(fb, x, y, 5.0f, (float)i < p * n ? C_WHITE : C_LINE2);
    }
}

static void page_now(gfx_fb *fb)
{
    char t[48];
    const hal_gps *g = &N.last;
    uint16_t ink = N.stale ? C_DIS : C_PRI;
    if (N.stale) header(fb, 1, "NAVIGATE");
    else if (svc_track_active()) header(fb, 1, 0);
    else {
        snprintf(t, sizeof t, "%s FIX  %d SATS", g->fix3d ? "3D" : "2D", g->sats_used);
        ui_label(fb, UI_CX, UI_TITLE_Y, t, C_SEC, UI_CENTER);
    }
    int metric = nos_prefs.units_metric;
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, N.stale ? C_DIS : C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 110, &N.m_speed, now_build(0), 5u);
    if (row_on(1)) ui_label(fb, UI_CX, 214, metric ? "KM/H" : "MPH", C_SEC, UI_CENTER);
    if (row_on(2)) {
        snprintf(t, sizeof t, "%.5f %c", fabs(g->lat), g->lat >= 0 ? 'N' : 'S');
        ui_text(fb, UI_F_LABEL, UI_CX, 250, t, ink, UI_CENTER, 2);
    }
    if (row_on(3)) {
        snprintf(t, sizeof t, "%.5f %c", fabs(g->lon), g->lon >= 0 ? 'E' : 'W');
        ui_text(fb, UI_F_LABEL, UI_CX, 276, t, ink, UI_CENTER, 2);
    }
    if (row_on(4)) {
        if (N.stale) {                          /* the fix is lost: say when it was last seen */
            int ago = (int)(nos_t - N.last_fix_t);
            snprintf(t, sizeof t, "[ NO FIX ]  %d:%02d AGO", ago / 60, ago % 60);
            ui_label(fb, UI_CX, 304, t, C_SEC, UI_CENTER);
        } else {
            snprintf(t, sizeof t, "ALT %d M   HDOP %.1f", (int)lroundf(g->alt_m), (double)g->hdop);
            ui_label(fb, UI_CX, 304, t, C_DIS, UI_CENTER);
        }
    }
    /* action row (last, >= 150 ms after the hero) */
    if (nos_t - now_t0() < 0.15f) return;
    if (!svc_track_active() && N.saved_state && nos_t - N.saved_t < SUMMARY_S) {
        char d[16], m[16];
        fmt_dist(m, sizeof m, N.sum_m);
        ui_fmt_dur(d, sizeof d, N.sum_ms, 0);
        ui_state(fb, UI_CX, 356, N.saved_state == 1 ? "SAVED" : "NO SD CARD", C_PRI);
        snprintf(t, sizeof t, "%s  %s", m, d);
        ui_label(fb, UI_CX, 392, t, C_SEC, UI_CENTER);
        return;
    }
    if (svc_track_active()) {
        if (N.holding && N.hold_p > 0) hold_ring(fb, UI_CX, UI_BTN_Y, UI_BTN_W, UI_BTN_H, N.hold_p);
        int pr = N.pressed == P_REC;
        ui_rrect(fb, UI_CX - UI_BTN_W / 2, UI_BTN_Y - UI_BTN_H / 2, UI_BTN_W, UI_BTN_H, UI_BTN_H / 2, pr ? C_SEC : C_SURF2);
        ui_label(fb, UI_CX, UI_BTN_Y + UI_F_LABEL->ascent / 2 - 2, nos_t - N.hint_t < 1.5f ? "HOLD TO STOP" : "STOP",
                 pr ? C_BG : C_PRI, UI_CENTER);
    } else {
        int pr = N.pressed == P_REC;
        ui_rrect(fb, UI_CX - UI_BTN_W / 2, UI_BTN_Y - UI_BTN_H / 2, UI_BTN_W, UI_BTN_H, UI_BTN_H / 2, pr ? C_SEC : C_WHITE);
        ui_label(fb, UI_CX, UI_BTN_Y + UI_F_LABEL->ascent / 2 - 2, "START TRACK", C_BG, UI_CENTER);
    }
}

static void page_track(gfx_fb *fb)
{
    float b = build_at(0);
    if (!svc_track_points(0)) {
        ui_title(fb, "TRACK", 0);
        ui_absent(fb, DOT_ICON_ROUTE, "No track", "Start one on the NOW page", 0, b);
        return;
    }
    header(fb, 0, "TRACK");
    /* the path: one damage command keyed by the view version */
    int nd = (int)(N.ndots * b + 0.5f);
    if (fb->rec) {
        gfx_rec_cmd(fb, UI_CX - 132, UI_CY - 140, UI_CX + 132, UI_CY + 124,
                    gfx_hash_u(gfx_hash_u(0x7A4Cu, N.view_ver), (unsigned)nd));
    } else {
        for (int i = 1; i < nd; i++)
            if (rows_hit(fb, N.dots[i][1] - 4, N.dots[i][1] + 4))
                dot_disc(fb, N.dots[i][0], N.dots[i][1], 5.0f, C_PRI);
    }
    ui_circle_line(fb, N.start_x, N.start_y, 8, 2.5f, C_WHITE);       /* start */
    if (N.have_you) dot_disc(fb, N.you_x, N.you_y, 13.0f * b, N.stale ? C_DIS : C_RED);   /* you, now */
    char t[24];
    float m = N.scale_m;
    if (m >= 1000) snprintf(t, sizeof t, "%.1f KM", (double)(m / 1000));
    else snprintf(t, sizeof t, "%d M", (int)(lroundf(m / 10) * 10));
    /* scale bar: 100 px */
    gfx_fill_rect(fb, UI_CX - 50, 380, 100, 4, C_DIS);
    ui_label(fb, UI_CX, 406, t, C_DIS, UI_CENTER);
}

static void arrow_draw(gfx_fb *fb, float cx, float cy, float ang_deg, float b, uint16_t col)
{
    float a = ang_deg * PI_F / 180.0f, s = gfx_fast_sin(a), c = gfx_fast_cos(a);
    if (fb->rec) {
        gfx_rec_cmd(fb, (int)cx - 84, (int)cy - 84, (int)cx + 84, (int)cy + 84,
                    gfx_hash_u(gfx_hash_f(gfx_hash_f(0xA770u, ang_deg), floorf(b * 32)), col));
        return;
    }
    if (!rows_hit(fb, cy - 84, cy + 84)) return;
    /* the tip is the biggest dot; the head's arms run back from it, the shaft tapers */
    static const signed char pts[][3] = {
        { 0, 64, 13 }, { -11, 53, 11 }, { -22, 42, 10 }, { -33, 31, 9 }, { -44, 20, 8 },
        { 11, 53, 11 }, { 22, 42, 10 }, { 33, 31, 9 }, { 44, 20, 8 },
        { 0, 44, 8 }, { 0, 25, 7 }, { 0, 6, 7 }, { 0, -13, 6 }, { 0, -32, 6 }, { 0, -51, 6 },
    };
    int n = (int)(sizeof pts / sizeof pts[0]);
    for (int i = 0; i < n; i++) {
        float u = pts[i][0], v = pts[i][1] - 6.0f;
        float g = m_clamp(b * 2.0f - (float)i / n, 0, 1);
        float x = cx + u * c + v * s, y = cy + u * s - v * c;
        dot_disc(fb, x, y, pts[i][2] * g, col);
    }
}

static void page_back(gfx_fb *fb)
{
    float b = build_at(0);
    if (!svc_track_points(0)) {
        ui_title(fb, "BACK TO START", 0);
        ui_absent(fb, DOT_ICON_PIN, "No start point", "Record a track to find your way back", 0, b);
        return;
    }
    header(fb, 1, "BACK TO START");
    if (N.home_dist < 15) {
        dot_icon_anim(fb, DOT_ICON_PIN, UI_CX, 186, 7.0f, 5.6f, C_WHITE, b, DOT_REVEAL_CENTER);
        ui_state(fb, UI_CX, 300, "YOU ARE AT THE START", C_PRI);
        return;
    }
    arrow_draw(fb, UI_CX, 184, N.arrow_q, b, N.stale ? C_DIS : C_WHITE);
    dot_style st = { DOT_FONT_TALL, 7.0f, 5.6f, N.stale ? C_DIS : C_WHITE, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 282, &N.m_dist, b, 4u);
    ui_label(fb, UI_CX, 370, N.home_dist < 1000 ? "METRES" : "KM", C_SEC, UI_CENTER);
    ui_label(fb, UI_CX, 400, N.head_src == 1 ? "BY COMPASS" : N.head_src == 2 ? "BY COURSE" : "NORTH UP", C_DIS, UI_CENTER);
}

/* ------------------------------------------------------------- draw / input */
static void nav_draw(gfx_fb *fb)
{
    if (!N.present) {
        ui_title(fb, "NAVIGATE", 0);
        ui_absent(fb, DOT_ICON_SAT, "Not connected", "Add the GPS for speed and tracks", "GPS MODULE", build_at(0));
        return;
    }
    float pos = pg_pos(&N.pg);
    int sky = !N.show_now || nos_t - N.fix_t < 0.25f;
    for (int i = 0; i < NPAGE; i++) {
        float off = i * PAGE_W - pos;
        if (off <= -PAGE_W || off >= PAGE_W) continue;
        gfx_fb sub;
        if (!nos_subview(fb, &sub, 0, 0, GFX_W, GFX_H, (int)off, 0)) continue;
        if (i == 0) { if (sky) sky_draw(&sub); else page_now(&sub); }
        else if (i == 1) page_track(&sub);
        else if (N.have_last || !svc_track_points(0)) page_back(&sub);
        else {
            ui_title(&sub, "BACK TO START", 0);
            ui_absent(&sub, DOT_ICON_SAT, 0, 0, 0, 1);
            ui_state(&sub, UI_CX, 340, "WAITING FOR FIX", C_SEC);
        }
    }
    ui_page_dots(fb, UI_CX, UI_DOTS_Y, NPAGE, pos / PAGE_W);
}

static int on_rec_button(int x, int y)
{
    int summary = !svc_track_active() && N.saved_state && nos_t - N.saved_t < SUMMARY_S;
    return N.show_now && (N.fixed || svc_track_active()) && nos_t - now_t0() >= 0.15f && N.pg.page == 0 && !N.pg.sp.moving &&
           !summary && abs(x - UI_CX) < UI_BTN_W / 2 && abs(y - UI_BTN_Y) < UI_BTN_H / 2;
}

static int nav_input(const nos_input *in)
{
    if (!N.present) return 0;
    if (pg_input(&N.pg, in)) { N.pressed = 0; N.holding = 0; return 1; }
    switch (in->type) {
    case IN_DOWN:
        N.pressed = on_rec_button(in->x, in->y) ? P_REC : 0;
        N.press_until = 0;
        if (N.pressed && svc_track_active()) { N.holding = 1; N.hold_t0 = nos_t; N.hold_p = 0; }
        return 1;
    case IN_UP:
        N.holding = 0;
        if (nos_t >= N.press_until) N.pressed = 0;
        return 1;
    case IN_DRAG_START: N.holding = 0; N.pressed = 0; return 0;
    case IN_LONG: return 0;                   /* the hold is timed in tick(); no extra haptic */
    case IN_TAP:
        if (on_rec_button(in->x, in->y)) {
            if (svc_track_active()) { N.hint_t = nos_t; nos_feedback(FB_LIMIT); }   /* teach the hold */
            else {
                svc_track_start();
                N.saved_state = 0;
                nos_feedback(FB_TOGGLE_ON);
                N.press_until = nos_t + 0.1f;
            }
            return 1;
        }
        return 0;
    default: return 0;
    }
}

const nos_screen scr_nav = { "NAVIGATE", nav_enter, nav_leave, nav_tick, nav_draw, nav_input,
                             SCR_HDRAG | SCR_KEEPAWAKE };
