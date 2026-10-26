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
 * A lost fix keeps the last values (dimmed and frozen, "[ NO FIX ] 0:12 AGO") for 30 s
 * before the sky plot returns.
 *
 * State changes are never cuts: the sky builds out while the NOW page builds in
 * (~250 ms); the header line and the action row (START / STOP / [ SAVED ]) crossfade
 * through the colour-step ladder; the recorded distance counts over to a new value
 * after a jump (refix).
 *
 * Motion (docs/MOTION30.html): durations are whole frames and fades use the perceptual
 * ink levels; the pager is a pixel spring (1 px snap); satellites sit in four size
 * classes by SNR (signal bars, not a size that creeps with every dB) and step between
 * them over 5 frames (EASE_STEADY); the NOW rows cascade one frame apart; the arrow is tracked rotation capped at
 * 15 deg a frame (its tip steps <= 15 px) with a crisp settle and a 2 deg deadband. The
 * track keeps its scale and centre while the path fits, so new points only append dots
 * (nothing drawn moves); when it outgrows the box the view rebuilds with room to grow
 * and fades through. The "you" dot glides to each 1 Hz fix (EASE_STEADY, ~4 px a frame,
 * 1-8 frames); only a refix jump past 100 px cuts, with a 2-frame ghost. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "svc_track.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include "scroll_diag.h"
#include "diag_log.h"
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
#define STALE_S 30.0f        /* lost fix: keep the last values this long */
#define HOLD_S  0.6f         /* hold to stop a track */
#define SUMMARY_S 3.0f
#define FADE_S  M_FRAMES(4)  /* each half of a state crossfade (out, then in) */
#define HDR_S   M_FRAMES(3)  /* each half of the header crossfade */
#define NOW_LAG M_FRAMES(2)  /* fix arrival: the NOW page starts this long after the sky */
#define ARRIVE_S M_FRAMES(5) /* ... and builds in this long (all in ~230 ms) */
#define DIM_S   M_FRAMES(8)  /* stale values dim in 8 even steps */
#define MORPH_S M_FRAMES(8)  /* live value digit morph */
#define ARROW_CAP  15.0f     /* deg a frame: the tip (58 px out) steps <= 15 px */
#define ARROW_MIN  1.0f      /* deg: the settle's smallest step, closer it snaps */
#define ARROW_DEAD 2.0f      /* deg: heading noise does not wobble the arrow */

/* ------------------------------------------------------------- pager */
typedef struct { mspring sp; int n, page, dragging; float x0; } pager;
static void pg_init(pager *p, int n, int page)
{
    memset(p, 0, sizeof *p);
    p->n = n; p->page = page;
    msp_init_px(&p->sp, page * PAGE_W, SPRING_PAGE);   /* pixels: ends with a 1 px snap */
}
static int pg_input(pager *p, const nos_input *in)
{
    if (in->type == IN_CANCEL) { p->dragging = 0; p->sp.v = 0; msp_to(&p->sp, p->page * PAGE_W); return 1; }

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

/* ------------------------------------------------------------- morphing value
 * A lone change morphs over whole frames, starting one step in (the frame of the change
 * shows it); a value that changes again before its morph ends flips like an odometer. */
typedef struct { char cur[16], prev[16]; nos_time_t t0, tc; } mval;
static void mv_set(mval *m, const char *s)
{
    if (!strcmp(m->cur, s)) return;
    int counting = nos_t - m->tc < MORPH_S;
    if (strlen(m->cur) == strlen(s) && m->cur[0] && !counting) { strcpy(m->prev, m->cur); m->t0 = nos_t - M_FRAMES(1); }
    else { strcpy(m->prev, s); m->t0 = -10; }
    m->tc = nos_t;
    snprintf(m->cur, sizeof m->cur, "%s", s);
}
static void mv_draw(gfx_fb *fb, const dot_style *st, float x, float y, const mval *m, float build, unsigned seed)
{
    if (build < 1.0f) { dot_text_anim(fb, st, x, y, m->cur, 1, build, DOT_REVEAL_RANDOM, seed); return; }
    float p = (nos_t - m->t0) / MORPH_S;
    if (p < 1.0f) dot_text_morph(fb, st, x, y, m->prev, m->cur, 1, p);
    else dot_text_a(fb, st, x, y, m->cur, 1);
}

/* ------------------------------------------------------------- state */
enum { H_NAV = 0, H_FIX, H_REC };                           /* header line kinds */
enum { A_NONE = 0, A_START, A_START_DIS, A_STOP, A_SUMMARY, A_RETRY, A_SAVING };  /* action row kinds */
enum { P_NONE = 0, P_REC };

typedef struct { int shown, init, pend; nos_time_t t; } plug_t;   /* see plug_tick() */

static struct {
    int present;             /* shown: flips after the unplug fade */
    plug_t pl;
    hal_gps g;               /* this frame's reading */
    hal_gps last;            /* last valid fix */
    int have_last;
    nos_time_t last_fix_t;        /* nos_t of the last valid fix */
    int fixed;               /* g.valid now */
    int show_now;            /* NOW page instead of the sky (fixed or recently fixed) */
    int stale;               /* showing the last values without a fix */
    float dim;               /* 0 live .. 1 stale (8-step ramp) */
    nos_time_t fix_t;             /* when the current fix arrived (sky -> NOW) */
    nos_time_t lost_t;            /* when the stale values gave way to the sky */
    int first;               /* first tick after entry */
    pager pg;
    nos_time_t t0;                /* entry build */
    float sat_d[16];         /* displayed satellite dot size (grows in) */
    unsigned char sat_to[16], sat_from[16], sat_k[16];   /* its 5-frame tween (1/4 px units) */
    int pressed;
    nos_time_t press_until;
    /* stop: hold */
    int holding; nos_time_t hold_t0; float hold_p;
    nos_time_t hint_t;            /* "HOLD TO STOP" after a short tap */
    /* summary after stop */
    int saved_state;         /* 0 none, 1 saved, 2 no card, 3 too short */
    int save_wait;           /* queued request not yet acknowledged by this view */
    nos_time_t saved_t; double sum_m; uint32_t sum_ms;
    /* crossfades */
    int hk, hk_prev; nos_time_t hk_t;
    int ak, ak_prev; nos_time_t ak_t; uint16_t fill0; int pressed0;
    /* recorded distance as shown (counts over a jump) */
    double d_from, d_to, d_shown; nos_time_t d_t0; int d_init;
    /* values (refreshed 1 Hz) */
    nos_time_t val_t;
    int ago;                 /* seconds since the fix, frozen once the fix is back */
    char slab[32], slab_prev[32]; nos_time_t sl_t;   /* SEARCHING label: crossfades on a change of form */
    mval m_speed, m_dist;
    /* track view */
    float (*dots)[2]; int ndots;
    unsigned track_ver, view_ver;
    int trace_page;
    nos_time_t view_t;
    float v_s, v_cx, v_cy;   /* local metres -> screen */
    double v_la0, v_lo0;     /* the track the view was made for */
    float you_x, you_y; int have_you;
    float start_x, start_y;
    float scale_m;           /* metres per 100 px */
    ui_trail you_tr;         /* a cut past 16 px leaves a ghost (R7) */
    nos_time_t you_t0; float you_fx, you_fy;   /* the glide to (you_x, you_y) from here */
    int you_n;               /* ... over this many frames */
    float you_dx, you_dy;    /* where the dot is drawn this frame */
    /* the view being faded out after a rescale (PSRAM) */
    float (*odots)[2]; int ondots, ohave_you; unsigned oview_ver;
    float ostart_x, ostart_y, oyou_x, oyou_y, oscale_m;
    nos_time_t vf_t;              /* when the view last changed scale/centre (fade-through) */
    /* back to start */
    double home_dist, home_brg;
    mspring arrow;           /* unwrapped arrow angle, degrees on screen */
    float arrow_q; int arrow_init;
    int head_src;            /* 0 north up, 1 compass, 2 course */
} N;

/* track points in local metres (float), appended as the track grows */
static struct { float (*xy)[2]; int n; double la0, lo0, k, kl; float minx, maxx, miny, maxy; } L;

static float q8(float k) { return floorf(m_clamp(k, 0, 1) * 8.0f + 0.5f) / 8.0f; }
static uint16_t fadec(uint16_t c, float k) { return k >= 1.0f ? c : ui_mix(C_BG, c, k); }
/* a label whose text already has the right case (units keep theirs) */
static int lbl(gfx_fb *fb, int x, int y, const char *s, uint16_t col, int align)
{
    return ui_text(fb, UI_F_LABEL, x, y, s, col, align, 2);
}
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
    if (m < 1000) snprintf(o, (size_t)n, "%d m", (int)m);
    else snprintf(o, (size_t)n, "%.2f km", m / 1000.0);
}

/* ------------------------------------------------------------- track view */
static void loc_sync(void)
{
    const svc_track_pt *P;
    int np = svc_track_points(&P);
    if (np > SVC_TRACK_MAXPTS) np = SVC_TRACK_MAXPTS;
    if (np <= 0) { L.n = 0; return; }
    if (!L.xy) L.xy = (float (*)[2])hal_big_alloc(SVC_TRACK_MAXPTS * 2 * sizeof(float));
    if (!L.xy) { L.n = 0; return; }
    /* a new track (or the service halved a full one): start over */
    if (np < L.n || (L.n && (P[0].lat != L.la0 || P[0].lon != L.lo0))) L.n = 0;
    if (!L.n) {
        L.la0 = P[0].lat; L.lo0 = P[0].lon;
        L.k = cos(L.la0 * D2R) * D2R * EARTH_R; L.kl = D2R * EARTH_R;
        L.minx = L.miny = 1e30f; L.maxx = L.maxy = -1e30f;
    }
    for (int i = L.n; i < np; i++) {             /* equirectangular metres around the first point */
        float x = (float)((P[i].lon - L.lo0) * L.k), y = (float)((P[i].lat - L.la0) * L.kl);
        L.xy[i][0] = x; L.xy[i][1] = y;
        if (x < L.minx) L.minx = x;
        if (x > L.maxx) L.maxx = x;
        if (y < L.miny) L.miny = y;
        if (y > L.maxy) L.maxy = y;
    }
    L.n = np;
}

#define VIEW_BOX 236.0f      /* px: the path fits the circle with margin */
#define VIEW_SX  ((float)UI_CX)
#define VIEW_SY  ((float)(UI_CY - 8))

static void you_local(float *x, float *y)
{
    *x = (float)((N.last.lon - L.lo0) * L.k);
    *y = (float)((N.last.lat - L.la0) * L.kl);
}

/* A new "you" position: the dot glides there from where it is drawn (8 frames, see
 * nav_tick), or cuts: on a new view (it fades in with the view), on the first fix, or on a
 * refix jump past 100 px (then the trail leaves a ghost for 2 frames). */
static void you_set(float x, float y, int had, int cut)
{
    if (had && !cut && x == N.you_x && y == N.you_y) return;   /* no news: let a glide finish */
    float fx = had ? N.you_dx : x, fy = had ? N.you_dy : y;
    float dx = x - fx, dy = y - fy;
    if (!had || cut) { fx = x; fy = y; ui_trail_reset(&N.you_tr, x, y); }
    else if (dx * dx + dy * dy > 100.0f * 100.0f) { fx = x; fy = y; }
    N.you_fx = fx; N.you_fy = fy; N.you_t0 = nos_t - M_FRAMES(1);
    {   /* ~4 px a frame: a short hop is a 1-2 frame step, a long one glides 8 frames */
        float d = gfx_fast_sqrt((x - fx) * (x - fx) + (y - fy) * (y - fy));
        int n = (int)floorf(d / 4.0f + 0.5f);
        N.you_n = n < 1 ? 1 : n > 8 ? 8 : n;
    }
    N.you_x = x; N.you_y = y;
    if (!had || cut) { N.you_dx = x; N.you_dy = y; }
    N.have_you = 1;
}

/* The view keeps its scale and centre while the path (and you) fit the box: a new point
 * only appends dots (the resample from the start is deterministic, so nothing already
 * drawn moves). Outgrowing it makes a new view with 30 % room to grow, and the old one
 * is kept for the fade-through (MOTION30 R4: in-place changes fade, they do not jump). */
static int view_fits(float minx, float maxx, float miny, float maxy)
{
    if (N.v_s <= 0 || L.la0 != N.v_la0 || L.lo0 != N.v_lo0) return 0;
    const float h = VIEW_BOX / 2 + 4;
    return (N.v_cx - minx) * N.v_s <= h && (maxx - N.v_cx) * N.v_s <= h &&
           (N.v_cy - miny) * N.v_s <= h && (maxy - N.v_cy) * N.v_s <= h;
}
static void rebuild_track_view(void)
{
    loc_sync();
    int np = L.n;
    if (np < 1) { N.ndots = 0; N.have_you = 0; N.view_ver++; return; }
    if (!N.dots) N.dots = (float (*)[2])hal_big_alloc(MAXDOTS * 2 * sizeof(float));
    if (!N.dots) { N.ndots = 0; N.have_you = 0; return; }
    float minx = L.minx, maxx = L.maxx, miny = L.miny, maxy = L.maxy;
    if (N.have_last) {
        float x, y;
        you_local(&x, &y);
        minx = x < minx ? x : minx; maxx = x > maxx ? x : maxx;
        miny = y < miny ? y : miny; maxy = y > maxy ? y : maxy;
    }
    float s, cx, cy;
    int had = N.have_you, cut = 0;
    if (view_fits(minx, maxx, miny, maxy)) { s = N.v_s; cx = N.v_cx; cy = N.v_cy; }
    else {
        float span = maxx - minx > maxy - miny ? maxx - minx : maxy - miny;
        span *= 1.3f;                                /* room to grow before the next rescale */
        if (span < 60) span = 60;
        s = VIEW_BOX / span; cx = (minx + maxx) / 2; cy = (miny + maxy) / 2;
        /* a drawn view fades out while the new one fades in */
        if (N.v_s > 0 && N.ndots > 0) {
            if (!N.odots) N.odots = (float (*)[2])hal_big_alloc(MAXDOTS * 2 * sizeof(float));
            if (N.odots) {
                float (*t)[2] = N.odots; N.odots = N.dots; N.dots = t;
                N.ondots = N.ndots; N.ohave_you = N.have_you; N.oview_ver = N.view_ver;
                N.ostart_x = N.start_x; N.ostart_y = N.start_y;
                N.oyou_x = N.you_dx; N.oyou_y = N.you_dy; N.oscale_m = N.scale_m;
                N.vf_t = nos_t;
            }
        }
        N.v_la0 = L.la0; N.v_lo0 = L.lo0;
        cut = 1;
    }
    N.ndots = 0; N.have_you = 0;
    N.view_ver++;
    N.v_s = s; N.v_cx = cx; N.v_cy = cy;
    N.scale_m = 100.0f / s;
    /* resample the path at a fixed screen spacing (floats + fast sqrt: ~1 ms for 4096 points) */
    float len = 0;
    const svc_track_pt *track;
    svc_track_points(&track);
    for (int i = 1; i < np; i++) {
        if (track[i].segment_start) continue;
        float dx = (L.xy[i][0] - L.xy[i - 1][0]) * s, dy = (L.xy[i][1] - L.xy[i - 1][1]) * s;
        len += gfx_fast_sqrt(dx * dx + dy * dy);
    }
    float step = 9.0f;
    if (len / step > MAXDOTS - 2) step = len / (MAXDOTS - 2);
    float carry = 0, ax = 0, ay = 0;
    for (int i = 0; i < np; i++) {
        float bx = VIEW_SX + (L.xy[i][0] - cx) * s;
        float by = VIEW_SY - (L.xy[i][1] - cy) * s;
        if (i == 0 || track[i].segment_start) {
            if (i == 0) { N.start_x = bx; N.start_y = by; }
            if (N.ndots >= MAXDOTS) break;
            N.dots[N.ndots][0] = bx; N.dots[N.ndots][1] = by; N.ndots++;
            ax = bx; ay = by; carry = 0;
            continue;
        }
        float dx = bx - ax, dy = by - ay, sl = gfx_fast_sqrt(dx * dx + dy * dy);
        if (sl > 1e-3f) {
            float t = step - carry, inv = 1.0f / sl;
            while (t <= sl && N.ndots < MAXDOTS) {
                N.dots[N.ndots][0] = floorf((ax + dx * t * inv) * 2 + 0.5f) / 2;
                N.dots[N.ndots][1] = floorf((ay + dy * t * inv) * 2 + 0.5f) / 2;
                N.ndots++;
                t += step;
            }
            carry = sl - (t - step);
        }
        ax = bx; ay = by;
    }
    if (N.have_last) {
        float x, y;
        you_local(&x, &y);
        you_set(floorf(VIEW_SX + (x - cx) * s + 0.5f), floorf(VIEW_SY - (y - cy) * s + 0.5f), had, cut);
    }
}

/* The rescale fade-through: frames 0-1 the old view at 56 / 19 %, frames 2-4 the new
 * one at 19 / 56 / 100 %. Returns the ink; *side = 0 old, 1 new. */
static float view_ink(int *side)
{
    int f = m_frame_k(N.vf_t);
    if (f >= 0 && f < 2 && N.odots) { *side = 0; return m_ink(1.0f - (float)(f + 1) / 3.0f); }
    *side = 1;
    if (f >= 2 && f < 4) return m_ink((float)(f - 1) / 3.0f);
    return 1.0f;
}

/* ~1 Hz: move the "you" dot on the current view; rebuild only if it left the box */
static void you_place(void)
{
    if (!N.have_last || L.n < 1 || N.v_s <= 0) return;
    float x, y;
    you_local(&x, &y);
    float px = VIEW_SX + (x - N.v_cx) * N.v_s, py = VIEW_SY - (y - N.v_cy) * N.v_s;
    if (fabsf(px - VIEW_SX) > VIEW_BOX / 2 + 4 || fabsf(py - VIEW_SY) > VIEW_BOX / 2 + 4) { rebuild_track_view(); return; }
    you_set(floorf(px + 0.5f), floorf(py + 0.5f), N.have_you, 0);
}

/* A module unplugged (or plugged back) while the app is open, the rule shared with the
 * connect team: the old content steps to half, then off (2 frames at 30 fps), then the
 * new page builds in. The shown state only flips after the fade. */
#define PLUG_A 0.025f            /* frame 1 of the fade: old content at half */
#define PLUG_B 0.058f            /* frame 2: off; the flip happens on the next tick */
#define PLUG_STEP (1.0f / 30.0f) /* the new page's build starts one step in */
/* returns 1 on the tick the shown state flips */
static int plug_tick(plug_t *p, int real)
{
    if (!p->init) { p->init = 1; p->shown = real; p->pend = 0; p->t = -10; return 0; }
    if (!p->pend) { if (real != p->shown) { p->pend = 1; p->t = nos_t; } return 0; }
    if (nos_t - p->t < PLUG_B) return 0;
    p->pend = 0;
    if (real == p->shown) return 0;   /* back before the fade ended */
    p->shown = real;
    return 1;
}
/* 0 draw normally, 1 draw at half ink, 2 draw nothing; records the fade's damage */
static int plug_phase(gfx_fb *fb, const plug_t *p)
{
    if (!p->pend) return 0;
    int ph = nos_t - p->t < PLUG_A ? 1 : 2;
    if (fb->rec) gfx_rec_cmd(fb, 0, 0, GFX_W, GFX_H, gfx_hash_u(0x9106u, (unsigned)ph));
    return ph;
}

/* ------------------------------------------------------------- lifecycle */
static void nav_enter(void)
{
    if (nos_revealed()) return;
    int page = N.pg.n ? N.pg.page : 0;
    float (*dots)[2] = N.dots, (*odots)[2] = N.odots;
    memset(&N, 0, sizeof N);
    N.dots = dots; N.odots = odots;
    N.vf_t = -10;
    pg_init(&N.pg, NPAGE, page);
    N.trace_page = page;
    N.t0 = nos_enter_t();
    N.fix_t = -10; N.lost_t = -100; N.last_fix_t = -100; N.val_t = -100; N.saved_t = -100; N.hint_t = -100;
    N.hk = -1; N.hk_t = -10; N.ak_t = -10;
    N.first = 1;
    N.save_wait = svc_track_saving();
    N.track_ver = svc_track_version() + 1;          /* force a first build of the view */
    msp_init(&N.arrow, 0, 0.28f, 1.0f);    /* critically damped; capped and snapped in tick */
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
    N.save_wait = r == -2;
    N.saved_state = r > 0 ? 1 : r == 0 ? 2 : r == -2 ? 0 : 3;
    N.saved_t = nos_t;
    if (r != -2) nos_feedback(r > 0 ? FB_SUCCESS : FB_ERROR);
}

static int is_pill(int a) { return a == A_START || a == A_START_DIS || a == A_STOP || a == A_RETRY; }
static void pill_colors(int a, int pressed, uint16_t *fill, uint16_t *ink)
{
    switch (a) {
    case A_START:     *fill = pressed ? C_SEC : C_WHITE; *ink = C_BG; break;
    case A_START_DIS: *fill = C_SURF; *ink = C_DIS; break;                  /* waits for a fix */
    default:          *fill = pressed ? C_SEC : C_SURF2; *ink = pressed ? C_BG : C_PRI; break;
    }
}

static void tick_distance(void)
{
    double d = svc_track_distance();
    if (!N.d_init) { N.d_from = N.d_to = N.d_shown = d; N.d_t0 = -10; N.d_init = 1; }
    if (d != N.d_to) {
        /* a jump (a refix after walking without one) counts over 0.3 s; 5 m steps just land */
        N.d_from = fabs(d - N.d_shown) > 20.0 ? N.d_shown : d;
        N.d_to = d; N.d_t0 = nos_t;
    }
    float u = m_clamp((nos_t - N.d_t0) / 0.3f, 0, 1);
    u = 1.0f - (1.0f - u) * (1.0f - u);
    N.d_shown = N.d_from + (N.d_to - N.d_from) * u;
}

/* The arrow is tracked rotation: a critically damped spring capped at ARROW_CAP a frame
 * (steady, the tip <= 15 px), whose settle never crawls (>= 1 deg steps) and lands. */
static void arrow_step(float dt)
{
    mspring *s = &N.arrow;
    float x0 = s->x, rem = s->target - x0;
    if (rem == 0.0f && s->v == 0.0f) return;
    float fr = dt * M_FPS < 1.0f ? 1.0f : dt * M_FPS;
    msp_step(s, dt);
    float st = s->x - x0, cap = ARROW_CAP * fr, mn = ARROW_MIN * fr;
    if (fabsf(st) > cap) { st = st < 0 ? -cap : cap; s->x = x0 + st; s->v = st * (M_FPS / fr); }
    else if (fabsf(rem) <= mn) s->x = s->target;
    else if (fabsf(st) < mn && st * rem >= 0.0f) s->x = x0 + (rem < 0 ? -mn : mn);
    if (s->x == s->target) { s->v = 0; s->moving = 0; }
}

static void nav_tick(float dt)
{
    svc_track_tick(dt);                 /* no-op once svc_tick drives it this frame */
    if (N.save_wait && !svc_track_saving()) {
        N.save_wait = 0;
        int result = svc_track_save_result();
        N.saved_state = result > 0 ? 1 : 2; N.saved_t = nos_t;
        nos_feedback(result > 0 ? FB_SUCCESS : FB_ERROR);
    }
    int real = hal_gps_get(&N.g) && (hal_ext_present() & EXT_GPS);
    if (plug_tick(&N.pl, real)) {
        N.t0 = nos_t - PLUG_STEP; N.first = 1;
        N.hk = N.hk_prev = H_NAV; N.hk_t = -10;     /* the steady NAVIGATE crossfades to the fix header */
    }
    N.present = N.pl.shown;
    if (N.pl.pend) return;                         /* the old content is fading */
    if (!N.present) N.t0 = N.t0 < nos_t ? N.t0 : nos_t;
    if (N.present) hal_gps_enable(1);
    int was = N.fixed;
    N.fixed = real && N.g.valid && N.g.age_ms <= 3000 && isfinite(N.g.lat) &&
              isfinite(N.g.lon) && fabs(N.g.lat) <= 90 && fabs(N.g.lon) <= 180;
    if (N.fixed) { N.last = N.g; N.have_last = 1; N.last_fix_t = nos_t; }
    if (N.first) {                      /* a fix that is already there on entry is not an arrival */
        N.first = 0;
        if (N.fixed) { N.show_now = 1; was = 1; N.val_t = -100; }
    }
    if (N.fixed && !was) {
        if (!N.show_now) { N.fix_t = nos_t - M_FRAMES(1); nos_feedback(FB_SUCCESS); }   /* sky -> NOW, one step in */
        N.val_t = -100;
    }
    int prev_now = N.show_now;
    N.stale = !N.fixed && N.have_last;
    N.show_now = N.fixed || (N.stale && nos_t - N.last_fix_t < STALE_S);
    if (N.stale) N.ago = (int)(nos_t - N.last_fix_t);   /* a refix fades [ NO FIX ] out as it was */
    {
        char t[32];
        /* the state stays [ SEARCHING ]; the count is a plain label under it */
        if (N.g.sats_used > 0) snprintf(t, sizeof t, "%d OF %d", N.g.sats_used, N.g.sats_view);
        else if (N.g.sats_view > 0) snprintf(t, sizeof t, "%d SAT%s", N.g.sats_view, N.g.sats_view == 1 ? "" : "S");
        else snprintf(t, sizeof t, "-");
        if (strcmp(t, N.slab)) {
            /* same form and width (a count ticking): swap; else old out, new in */
            int same = N.slab[0] && strlen(t) == strlen(N.slab) && !strstr(t, " OF ") == !strstr(N.slab, " OF ");
            if (N.slab[0] && !same && nos_t - N.sl_t >= 0.2f) { strcpy(N.slab_prev, N.slab); N.sl_t = nos_t - M_FRAMES(1); }
            else if (!N.slab[0]) N.sl_t = -10;
            strcpy(N.slab, t);
        }
    }
    if (prev_now && !N.show_now) { N.lost_t = nos_t - M_FRAMES(1); N.t0 = nos_t + M_FRAMES(3); }  /* NOW out, sky in */
    /* stale values dim in 8 steps (and back on a refix) */
    float dw = N.stale ? 1.0f : 0.0f;
    if (N.dim < dw) N.dim = fminf(dw, N.dim + dt / DIM_S);
    else if (N.dim > dw) N.dim = fmaxf(dw, N.dim - dt / DIM_S);
    /* satellites: four size classes by SNR, like signal bars (a size that followed every
     * dB would creep for seconds while a lock builds); a class change or an appearance
     * steps over 5 frames (EASE_STEADY), the frame of the change already showing it */
    {
        int fr = (int)floorf(dt * M_FPS + 0.5f);
        if (fr < 1) fr = 1;
        for (int i = 0; i < 16; i++) {
            int snr = i < N.g.nsnr ? N.g.snr[i] : 0;
            float t = !snr ? 0.0f : snr < 20 ? 7.0f : snr < 30 ? 10.0f : snr < 40 ? 13.0f : 16.0f;
            int tq = (int)(t * 4.0f);
            if (tq != N.sat_to[i]) {
                N.sat_from[i] = (unsigned char)floorf(N.sat_d[i] * 4.0f + 0.5f);
                N.sat_k[i] = 0;
                N.sat_to[i] = (unsigned char)tq;
            }
            if (N.sat_k[i] < 5) N.sat_k[i] = (unsigned char)(N.sat_k[i] + fr > 5 ? 5 : N.sat_k[i] + fr);
            float a = N.sat_from[i] * 0.25f, b = N.sat_to[i] * 0.25f;
            N.sat_d[i] = N.sat_k[i] >= 5 ? b : floorf((a + (b - a) * bezier_ease(&EASE_STEADY, N.sat_k[i] / 5.0f)) * 4.0f + 0.5f) / 4.0f;
        }
    }
    /* values at 1 Hz; frozen while the fix is lost */
    if (N.have_last && !N.stale && nos_t - N.val_t >= 1.0f) {
        N.val_t = nos_t;
        char t[16];
        float v = N.last.speed_mps * (nos_prefs.units_metric ? 3.6f : 2.23694f);
        snprintf(t, sizeof t, "%.1f", (double)(floorf(v * 10 + 0.5f) / 10));
        mv_set(&N.m_speed, t);
    }
    tick_distance();
    /* track view: rebuild when a point lands; move the "you" dot ~1 Hz */
    unsigned tv = svc_track_version();
    if (tv != N.track_ver) { N.track_ver = tv; N.view_t = nos_t; rebuild_track_view(); }
    else if (svc_track_points(0) && !N.stale && nos_t - N.view_t > 1.0f) { N.view_t = nos_t; you_place(); }
    if (N.have_you) {
        /* the dot glides to each fix (EASE_STEADY, you_n frames), started one step in */
        float p = m_clamp((nos_t - N.you_t0) / M_FRAMES(N.you_n > 0 ? N.you_n : 1), 0, 1);
        float e = p >= 1.0f ? 1.0f : bezier_ease(&EASE_STEADY, p);
        N.you_dx = floorf((N.you_fx + (N.you_x - N.you_fx) * e) * 4.0f + 0.5f) / 4.0f;   /* AA dot: 1/4 px */
        N.you_dy = floorf((N.you_fy + (N.you_y - N.you_fy) * e) * 4.0f + 0.5f) / 4.0f;
        ui_trail_push(&N.you_tr, N.you_dx, N.you_dy);
    }
    if (pg_tick(&N.pg, dt)) N.hint_t = -100;
    if (!N.pg.dragging && !N.pg.sp.moving && N.trace_page != N.pg.page) {
        uint32_t request = diag_action_request(DIAG_DOMAIN_NAVIGATION, DIAG_ACT_TRANSITION, N.pg.page);
        diag_action_context(request, (int32_t)diag_log_name_id("NAVIGATE.pages"), N.trace_page);
        diag_action_result(request, DIAG_RESULT_SUCCESS, N.pg.page);
        N.trace_page = N.pg.page;
    }
    /* hold to stop; a released hold unwinds */
    int held = 0;                                    /* the hold just completed under the finger */
    if (N.holding) {
        N.hold_p = m_clamp((nos_t - N.hold_t0) / HOLD_S, 0, 1);
        N.hold_p = floorf(N.hold_p * 40.0f) / 40.0f;
        if (N.hold_p >= 1.0f) { N.holding = 0; N.pressed = 0; held = 1; stop_track(); }
    } else if (N.ak == A_STOP && N.hold_p > 0) {
        N.hold_p = floorf(m_clamp(N.hold_p - dt / M_FRAMES(5), 0, 1) * 40.0f) / 40.0f;
    } else if (N.ak != A_STOP && nos_t - N.ak_t > FADE_S) N.hold_p = 0;
    if (N.pressed && N.press_until > 0 && nos_t > N.press_until && !N.holding) { N.pressed = 0; N.press_until = 0; }
    /* header and action row: crossfade on a change of kind */
    int hk = svc_track_active() ? H_REC : (N.show_now && !N.stale) ? H_FIX : H_NAV;
    if (N.hk < 0) N.hk = N.hk_prev = hk;
    else if (hk != N.hk) { N.hk_prev = N.hk; N.hk = hk; N.hk_t = nos_t - M_FRAMES(1); }   /* fades start one step in */
    int ak = svc_track_active() ? A_STOP : (N.save_wait || svc_track_saving()) ? A_SAVING : svc_track_pending() ? A_RETRY :
             N.saved_state && nos_t - N.saved_t < SUMMARY_S ? A_SUMMARY :
             !N.show_now ? A_NONE : N.stale ? A_START_DIS : A_START;
    if (ak != N.ak) {
        uint16_t f, i;
        N.pressed0 = N.pressed == P_REC || held;
        pill_colors(N.ak, N.pressed0, &f, &i);
        N.fill0 = f;                                 /* the pill's colour at the change */
        N.ak_prev = N.ak; N.ak = ak;
        N.ak_t = N.ak_prev == A_NONE ? -10 : nos_t - M_FRAMES(1);  /* arriving with the page: no fade */
    }
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
        if (!N.arrow_init) { N.arrow_init = 1; msp_set(&N.arrow, want); }   /* no swing in from north */
        float d = wrap180(want - N.arrow.target);
        if (fabsf(d) > ARROW_DEAD) msp_to(&N.arrow, N.arrow.target + d);
        arrow_step(dt);
        N.arrow_q = floorf(N.arrow.x * 2 + 0.5f) / 2;
        if (!N.stale && nos_t - N.val_t < dt * 1.5f) {   /* same 1 Hz beat as the speed */
            char t[16];
            if (N.home_dist < 1000) snprintf(t, sizeof t, "%d", (int)(lround(N.home_dist / 5) * 5));
            else snprintf(t, sizeof t, "%.1f", N.home_dist / 1000.0);
            mv_set(&N.m_dist, t);
        }
    }
    nos_keep_awake();
#ifdef GFX_COST_MODEL
    /* motion audit */
    {
        static const char *satn[16] = { "nav.sat0", "nav.sat1", "nav.sat2", "nav.sat3", "nav.sat4", "nav.sat5",
            "nav.sat6", "nav.sat7", "nav.sat8", "nav.sat9", "nav.sat10", "nav.sat11", "nav.sat12", "nav.sat13",
            "nav.sat14", "nav.sat15" };
        nos_motion_probe("nav.pager", N.pg.dragging ? MC_FINGER : MC_MOMENTUM, pg_pos(&N.pg));
        if (np && N.have_last) nos_motion_probe("nav.arrow", MC_TRACKED, N.arrow_q * 0.0174533f * 58.0f);
        if (!N.show_now) for (int i = 0; i < 16; i++) nos_motion_probe(satn[i], MC_MICRO, N.sat_d[i]);
        nos_motion_probe("nav.dim.ink", MC_FADE, q8(N.dim) * 100.0f);
        float u = nos_t - N.hk_t;
        nos_motion_probe("nav.hdr.ink", MC_FADE, (u < HDR_S ? m_ink(1 - u / HDR_S) : m_ink((u - HDR_S) / HDR_S)) * 100.0f);
        float ua = nos_t - N.ak_t;
        if (N.ak != N.ak_prev && ua < 2 * FADE_S)
            nos_motion_probe("nav.act.ink", MC_FADE, (ua < FADE_S ? m_ink(1 - ua / FADE_S) : m_ink((ua - FADE_S) / FADE_S)) * 100.0f);
        if (N.show_now && nos_t - N.fix_t < FADE_S + 0.1f)
            nos_motion_probe("nav.sky.ink", MC_FADE, m_ink(1.0f - (nos_t - N.fix_t) / FADE_S) * 100.0f);
        if (m_frame_k(N.vf_t) >= 0 && m_frame_k(N.vf_t) < 6) {
            int side; float vk = view_ink(&side);
            nos_motion_probe("nav.view.ink", MC_FADE, vk * (side ? 100.0f : -100.0f));
        }
        if (N.have_you) {
            /* a new view is a fade-through, not a move: its first frame is a fresh origin */
            int fresh = m_frame_k(N.vf_t) == 0;
            if (fresh) { nos_motion_probe("nav.you.x", MC_COMPONENT, N.you_dx); nos_motion_probe("nav.you.y", MC_COMPONENT, N.you_dy); }
            nos_motion_probe("nav.you.x", MC_COMPONENT, N.you_dx);
            nos_motion_probe("nav.you.y", MC_COMPONENT, N.you_dy);
            if (fresh) { nos_motion_probe("nav.view.x", MC_TRACKED, N.start_x); nos_motion_probe("nav.view.y", MC_TRACKED, N.start_y); }
            nos_motion_probe("nav.view.x", MC_TRACKED, N.start_x);
            nos_motion_probe("nav.view.y", MC_TRACKED, N.start_y);
        }
    }
#endif
}

/* ------------------------------------------------------------- shared chrome */
static float build_at(float delay) { return m_clamp((nos_t - N.t0 - delay) / UI_ENTRY_S, 0, 1); }

/* The header slot (title line) in one of its kinds at fade k. The recording dot is
 * red only where nothing else claims the screen's one red. */
static int S_NOTITLE;      /* draw: the steady title is drawn outside the fading body */
static void header_kind(gfx_fb *fb, int kind, int red, const char *title, float k)
{
    if (k <= 0) return;
    char t[48];
    if (kind == H_REC) {
        char l[20], m[16];
        svc_track_label(l, sizeof l);
        fmt_dist(m, sizeof m, N.d_shown);
        snprintf(t, sizeof t, "%s  %s", l, m);
        int w = ui_text_width(UI_F_LABEL, t, 2) + 18;
        dot_disc(fb, UI_CX - w / 2 + 4, UI_TITLE_Y - 6, 8, fadec(red ? C_RED : C_PRI, k));
        lbl(fb, UI_CX - w / 2 + 18, UI_TITLE_Y, t, fadec(C_SEC, k), UI_LEFT);
    } else if (kind == H_FIX) {
        snprintf(t, sizeof t, "%s FIX  %d SATS", N.last.fix3d ? "3D" : "2D", N.last.sats_used);
        ui_label(fb, UI_CX, UI_TITLE_Y, t, fadec(C_SEC, k), UI_CENTER);
    } else if (k >= 1) { if (!S_NOTITLE) ui_title(fb, title, 0); }
    else ui_label(fb, UI_CX, UI_TITLE_Y, title, fadec(C_SEC, k), UI_CENTER);
}
static void header(gfx_fb *fb, int red, const char *title)
{
    header_kind(fb, svc_track_active() ? H_REC : H_NAV, red, title, 1);
}
/* page 1's header: the old kind fades out, then the new one in */
static void header0(gfx_fb *fb)
{
    float u = nos_t - N.hk_t;
    if (u < HDR_S) header_kind(fb, N.hk_prev, 1, "NAVIGATE", m_ink(1 - u / HDR_S));
    else header_kind(fb, N.hk, 1, "NAVIGATE", m_ink((u - HDR_S) / HDR_S));
}

/* ------------------------------------------------------------- sky plot */
static void sky_draw(gfx_fb *fb)
{
    float b = build_at(0);
    /* a fix arrived: the sky builds out while the NOW page builds in */
    float out = N.show_now ? m_clamp(1.0f - (nos_t - N.fix_t) / FADE_S, 0, 1) : 1.0f;   /* sizes: even steps */
    float k = b * out, ki = m_ink(k);                                                 /* ink: perceptual */
    if (k <= 0) return;
    /* horizon / 30 / 60 deg rings, built dot by dot */
    dot_ring(fb, UI_CX, SKY_CY, SKY_R, 72, 2.6f * out, 0, 72 * b, C_LINE2, 0);
    dot_ring(fb, UI_CX, SKY_CY, SKY_R * 2 / 3, 48, 2.4f * out, 0, 48 * b, C_LINE2, 0);
    dot_ring(fb, UI_CX, SKY_CY, SKY_R / 3, 24, 2.2f * out, 0, 24 * b, C_LINE2, 0);
    if (b >= 1) ui_text(fb, UI_F_SMALL, UI_CX, (int)(SKY_CY - SKY_R + 22), "N", fadec(C_DIS, m_ink(out)), UI_CENTER, 0);
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
    ui_state(fb, UI_CX, 380, "SEARCHING", fadec(C_SEC, ki));
    float su = (nos_t - N.sl_t) / M_FRAMES(3);
    const char *cnt = su < 1 ? N.slab_prev : N.slab;
    float ck = su < 1 ? 1 - su : m_clamp(su - 1, 0, 1);
    if (strcmp(cnt, "-")) ui_label(fb, UI_CX, 410, cnt, fadec(C_DIS, m_ink(k * ck)), UI_CENTER);
}

/* ------------------------------------------------------------- NOW page */
static int arriving(void) { return N.fix_t + NOW_LAG > N.t0; }
static nos_time_t now_t0(void) { return arriving() ? N.fix_t + NOW_LAG : N.t0; }
static float now_build(float delay)
{
    return m_clamp((nos_t - now_t0() - delay) / (arriving() ? ARRIVE_S : UI_ENTRY_S), 0, 1);
}
static int row_on(int i) { return m_frame_k(now_t0()) >= i; }   /* a whole-frame cascade (R9) */

/* dots around the Stop pill, filling clockwise from 12 o'clock while it is held;
 * k shrinks them away (build-out) */
static void hold_ring(gfx_fb *fb, float cx, float cy, float w, float h, float p, float k)
{
    const int n = 40;
    const float r = h / 2 + 9, half = w / 2 - h / 2;
    const float straight = 2 * half, arc = PI_F * r, per = 2 * straight + 2 * arc;
    if (k <= 0) return;
    if (fb->rec) {
        gfx_rec_cmd(fb, (int)(cx - half - r - 4), (int)(cy - r - 4), (int)(cx + half + r + 4), (int)(cy + r + 4),
                    gfx_hash_f(gfx_hash_f(0x401Du, p), k));
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
        dot_disc(fb, x, y, 5.0f * k, (float)i < p * n ? C_WHITE : C_LINE2);
    }
}

static const char *pill_label(int a)
{
    if (a == A_STOP) return nos_t - N.hint_t < 1.5f ? "HOLD TO STOP" : "STOP";
    if (a == A_RETRY) return "RETRY SAVE";
    return "START TRACK";
}
/* the STOP <-> HOLD TO STOP hint swap ramps its ink up from the fill */
static uint16_t pill_ink(int a, uint16_t fill, uint16_t ink)
{
    if (a != A_STOP || N.hint_t < 0) return ink;
    nos_time_t sw = nos_t - N.hint_t < 1.5f ? N.hint_t : N.hint_t + 1.5f;
    return ui_mix(fill, ink, m_ink((nos_t - sw) / M_FRAMES(3)));
}
static void pill(gfx_fb *fb, uint16_t fill, const char *label, uint16_t ink)
{
    ui_rrect(fb, UI_CX - UI_BTN_W / 2, UI_BTN_Y - UI_BTN_H / 2, UI_BTN_W, UI_BTN_H, UI_BTN_H / 2, fill);
    ui_label(fb, UI_CX, UI_BTN_Y + UI_F_LABEL->ascent / 2 - 2, label, ink, UI_CENTER);
}
static void summary_draw(gfx_fb *fb, float k)
{
    static const char *word[] = { "", "SAVED", "SAVE FAILED", "TOO SHORT" };
    char d[16], m[16], t[40];
    ui_state(fb, UI_CX, 356, word[N.saved_state & 3], fadec(C_PRI, k));
    if (N.saved_state == 3) return;
    fmt_dist(m, sizeof m, N.sum_m);
    ui_fmt_dur(d, sizeof d, N.sum_ms, 0);
    snprintf(t, sizeof t, "%s  %s", m, d);
    lbl(fb, UI_CX, 392, t, fadec(C_SEC, k), UI_CENTER);
}
/* old: the row leaving, drawn in the state it had at the change (a completed hold
 * fades out from its pressed fill, not from a one-frame release) */
static void action_one(gfx_fb *fb, int a, float k, int old)
{
    if (a == A_NONE || k <= 0) return;
    if (a == A_SAVING) { ui_state(fb, UI_CX, UI_BTN_Y, "SAVING", fadec(C_SEC, k)); return; }
    if (a == A_SUMMARY) { summary_draw(fb, k); return; }
    uint16_t fill, ink;
    pill_colors(a, old ? N.pressed0 : a == N.ak && N.pressed == P_REC, &fill, &ink);
    if (a == A_STOP && N.hold_p > 0) hold_ring(fb, UI_CX, UI_BTN_Y, UI_BTN_W, UI_BTN_H, N.hold_p, k);
    pill(fb, fadec(fill, k), pill_label(a), fadec(pill_ink(a, fill, ink), k));
}
/* pill -> pill: the fill crossfades and the label swaps through the fill; anything
 * else: the old row builds out, then the new one builds in */
static void action_draw(gfx_fb *fb, float pk)
{
    float u = nos_t - N.ak_t;
    int a = N.ak, b = N.ak_prev;
    if (a == b || u >= 2 * FADE_S) { action_one(fb, a, pk, 0); return; }
    if (is_pill(a) && is_pill(b)) {
        if (u >= FADE_S) { action_one(fb, a, pk, 0); return; }
        float t = q8(u / FADE_S);
        uint16_t fa, ia, fbb, ib;
        pill_colors(a, 0, &fa, &ia);
        pill_colors(b, 0, &fbb, &ib);
        uint16_t f = ui_mix(N.fill0, fa, t);
        if (b == A_STOP && N.hold_p > 0) hold_ring(fb, UI_CX, UI_BTN_Y, UI_BTN_W, UI_BTN_H, N.hold_p, pk * (1 - t));
        if (t < 0.5f) pill(fb, fadec(f, pk), pill_label(b), fadec(ui_mix(f, ib, 1 - 2 * t), pk));
        else pill(fb, fadec(f, pk), pill_label(a), fadec(ui_mix(f, ia, 2 * t - 1), pk));
        return;
    }
    if (u < FADE_S) action_one(fb, b, pk * m_ink(1 - u / FADE_S), 1);
    else action_one(fb, a, pk * m_ink((u - FADE_S) / FADE_S), 0);
}

/* k < 1: the page builds out (the stale values giving way to the sky) */
static void page_now(gfx_fb *fb, float k)
{
    char t[48];
    const hal_gps *g = &N.last;
    float dim = q8(N.dim);
    uint16_t ink = fadec(ui_mix(C_PRI, C_DIS, dim), k);
    int metric = nos_prefs.units_metric;
    dot_style st = { DOT_FONT_TALL, UI_HERO_PITCH, UI_HERO_PITCH * 0.8f, ui_mix(C_WHITE, C_DIS, dim), 0, 1, 1 };
    float hb = now_build(0);
    mv_draw(fb, &st, UI_CX, 110, &N.m_speed, hb < k ? hb : k, 5u);
    if (row_on(1)) lbl(fb, UI_CX, 214, metric ? "km/h" : "mph", fadec(C_SEC, k), UI_CENTER);
    if (row_on(2)) {
        snprintf(t, sizeof t, "%.5f %c", fabs(g->lat), g->lat >= 0 ? 'N' : 'S');
        ui_text(fb, UI_F_LABEL, UI_CX, 250, t, ink, UI_CENTER, 2);
    }
    if (row_on(3)) {
        snprintf(t, sizeof t, "%.5f %c", fabs(g->lon), g->lon >= 0 ? 'E' : 'W');
        ui_text(fb, UI_F_LABEL, UI_CX, 276, t, ink, UI_CENTER, 2);
    }
    if (row_on(4)) {
        /* altitude/accuracy and "no fix" swap through the dim ramp */
        if (dim >= 0.5f) {
            int ago = N.ago;
            snprintf(t, sizeof t, "[ NO FIX ]  %d:%02d AGO", ago / 60, ago % 60);
            ui_label(fb, UI_CX, 304, t, fadec(C_SEC, k * (dim * 2 - 1)), UI_CENTER);
        } else {
            snprintf(t, sizeof t, "ALTITUDE %d m   HDOP %.1f", (int)lroundf(g->alt_m), (double)g->hdop);
            lbl(fb, UI_CX, 304, t, fadec(C_DIS, k * (1 - dim * 2)), UI_CENTER);
        }
    }
    /* action row last: after the hero, fading in over 3 frames (19-56-100 %) */
    int af = m_frame_k(now_t0() + (arriving() ? M_FRAMES(3) : M_FRAMES(5)));
    if (af < 0) return;
    action_draw(fb, k * m_ink((float)(af + 1) / 3.0f));
}

static void page_track(gfx_fb *fb)
{
    float b = build_at(0);
    if (!svc_track_points(0)) {
        ui_title(fb, "TRACK", 0);
        ui_absent(fb, DOT_ICON_ROUTE, "No track", "Start one on\nthe NOW page", 0, b);
        return;
    }
    header(fb, 0, "TRACK");
    int side;
    float k = view_ink(&side);
    int old = !side;
    float (*dots)[2] = old ? N.odots : N.dots;
    /* the path: one damage command keyed by the view version, the build and the fade */
    int nd = old ? N.ondots : (int)(N.ndots * b + 0.5f);
    if (fb->rec) {
        gfx_rec_cmd(fb, UI_CX - 132, UI_CY - 140, UI_CX + 132, UI_CY + 124,
                    gfx_hash_f(gfx_hash_u(gfx_hash_u(0x7A4Cu, old ? N.oview_ver : N.view_ver), (unsigned)nd), k));
    } else {
        uint16_t pc = fadec(C_PRI, k);
        for (int i = 1; i < nd; i++)
            if (rows_hit(fb, dots[i][1] - 4, dots[i][1] + 4))
                dot_disc(fb, dots[i][0], dots[i][1], 5.0f, pc);
    }
    ui_circle_line(fb, old ? N.ostart_x : N.start_x, old ? N.ostart_y : N.start_y, 8, 2.5f, fadec(C_WHITE, k));   /* start */
    uint16_t yc = fadec(ui_mix(C_RED, C_DIS, q8(N.dim)), k);
    if (old ? N.ohave_you : N.have_you) {                                  /* you, now */
        if (!old && k >= 1.0f) ui_smear_disc(fb, &N.you_tr, 6.5f * b, yc, 16.0f, 2);   /* a refix hop */
        dot_disc(fb, old ? N.oyou_x : N.you_dx, old ? N.oyou_y : N.you_dy, 13.0f * b, yc);
    }
    char t[24];
    float m = old ? N.oscale_m : N.scale_m;
    if (m >= 1000) snprintf(t, sizeof t, "%.1f km", (double)(m / 1000));
    else snprintf(t, sizeof t, "%d m", (int)(lroundf(m / 10) * 10));
    /* scale bar: 100 px (it never changes; its label fades with the view) */
    gfx_fill_rect(fb, UI_CX - 50, 380, 100, 4, C_DIS);
    lbl(fb, UI_CX, 406, t, fadec(C_DIS, k), UI_CENTER);
    if (!N.fixed && N.have_last) ui_label(fb, UI_CX, 354, "NO FIX / LAST LOCATION", C_DIS, UI_CENTER);
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
        ui_absent(fb, DOT_ICON_PIN, "No start point", "Record a track to\nfind your way back", 0, b);
        return;
    }
    header(fb, 1, "BACK TO START");
    if (N.fixed && N.home_dist < 15) {
        dot_icon_anim(fb, DOT_ICON_PIN, UI_CX, 186, 7.0f, 5.6f, C_WHITE, b, DOT_REVEAL_CENTER);
        ui_state(fb, UI_CX, 300, "YOU ARE AT THE START", C_PRI);
        return;
    }
    uint16_t col = ui_mix(C_WHITE, C_DIS, q8(N.dim));
    arrow_draw(fb, UI_CX, 184, N.arrow_q, b, col);
    dot_style st = { DOT_FONT_TALL, 7.0f, 5.6f, col, 0, 1, 1 };
    mv_draw(fb, &st, UI_CX, 282, &N.m_dist, b, 4u);
    lbl(fb, UI_CX, 370, N.home_dist < 1000 ? "m" : "km", C_SEC, UI_CENTER);
    ui_label(fb, UI_CX, 400, !N.fixed ? "NO FIX / LAST LOCATION" :
             N.head_src == 1 ? "BY COMPASS" : N.head_src == 2 ? "BY COURSE" : "NORTH UP", C_DIS, UI_CENTER);
}

/* ------------------------------------------------------------- draw / input */
static void nav_draw_body(gfx_fb *fb);
static void nav_draw(gfx_fb *fb)
{
    int ph = plug_phase(fb, &N.pl);
    /* NAVIGATE on both sides of the swap (page 1 under a settled NAVIGATE header, or
     * the absent page; the header follows the recording on a replug): it stays put */
    int steady = ph && N.pg.page == 0 && !N.pg.sp.moving && !svc_track_active() &&
                 (!N.present || (N.hk == H_NAV && nos_t - N.hk_t >= 2 * HDR_S));
    if (steady) ui_title(fb, "NAVIGATE", 0);
    if (ph == 2) return;
    int d0 = fb->dim;
    if (ph == 1) nos_ink_dim(fb, 0.5f);
    S_NOTITLE = steady;
    nav_draw_body(fb);
    S_NOTITLE = 0;
    fb->dim = d0;
}
static void nav_draw_body(gfx_fb *fb)
{
    if (!N.present) {
        if (!S_NOTITLE) ui_title(fb, "NAVIGATE", 0);
        ui_absent(fb, DOT_ICON_SAT, "Not connected", "Add the GPS for speed and tracks", "GPS MODULE", build_at(0));
        if (svc_track_active() || svc_track_pending()) action_draw(fb, 1);
        return;
    }
    float pos = pg_pos(&N.pg);
    for (int i = 0; i < NPAGE; i++) {
        float off = i * PAGE_W - pos;
        if (off <= -PAGE_W || off >= PAGE_W) continue;
        gfx_fb sub;
        if (!nos_subview(fb, &sub, 0, 0, GFX_W, GFX_H, (int)off, 0)) continue;
        if (i == 0) {
            header0(&sub);
            if (!N.show_now || nos_t - N.fix_t < FADE_S) sky_draw(&sub);
            if (N.show_now) { if (nos_t - N.fix_t >= NOW_LAG) page_now(&sub, 1); }
            else if (nos_t - N.lost_t < FADE_S) page_now(&sub, m_ink(1 - (nos_t - N.lost_t) / FADE_S));
            if (!N.show_now && nos_t - N.lost_t >= FADE_S) action_draw(&sub, 1);
        }
        else if (i == 1) page_track(&sub);
        else if (N.have_last || !svc_track_points(0)) page_back(&sub);
        else {
            ui_title(&sub, "BACK TO START", 0);
            ui_absent(&sub, DOT_ICON_SAT, 0, 0, 0, 1);
            ui_state(&sub, UI_CX, 340, "WAITING FOR FIX", C_SEC);
        }
    }
    /* motion blur on the pages as they slide (nos.h), not on the page dots */
    static nos_mb_track mb;
    float mbs = nos_mblur_step(nos_mb_track_step(&mb, pos), N.pg.dragging);
    if (mbs > 0 && mbs < PAGE_W * 0.5f) nos_mblur_h(fb, 0, GFX_W, mbs);
    ui_page_dots(fb, UI_CX, UI_DOTS_Y, NPAGE, pos / PAGE_W);
}

static int on_rec_button(int x, int y)
{
    int settled = nos_t - N.ak_t >= FADE_S || is_pill(N.ak_prev);
    return (N.ak == A_START || N.ak == A_STOP || N.ak == A_RETRY) && settled && nos_t - N.t0 >= M_FRAMES(5) &&
           (!N.present || (N.pg.page == 0 && !N.pg.sp.moving)) &&
           abs(x - UI_CX) < UI_BTN_W / 2 && abs(y - UI_BTN_Y) < UI_BTN_H / 2;
}

static int nav_input(const nos_input *in)
{
    if (in->type == IN_CANCEL) { N.pressed = N.holding = 0; N.press_until = 0; pg_input(&N.pg, in); return 1; }

    if (!N.present && !svc_track_active() && !svc_track_pending()) return 0;
    if (N.present && pg_input(&N.pg, in)) { N.pressed = 0; N.holding = 0; return 1; }
    switch (in->type) {
    case IN_DOWN:
        N.pressed = on_rec_button(in->x, in->y) ? P_REC : 0;
        N.press_until = 0;
        if (N.pressed && N.ak == A_STOP) { N.holding = 1; N.hold_t0 = nos_t - N.hold_p * HOLD_S; }
        return 1;
    case IN_UP:
        N.holding = 0;
        if (nos_t >= N.press_until) N.pressed = 0;
        return 1;
    case IN_DRAG_START: N.holding = 0; N.pressed = 0; return 0;
    case IN_LONG: return 0;                   /* the hold is timed in tick(); no extra haptic */
    case IN_TAP:
        if (on_rec_button(in->x, in->y)) {
            if (N.ak == A_STOP) { N.hint_t = nos_t; nos_feedback(FB_LIMIT); }   /* teach the hold */
            else if (N.ak == A_RETRY) stop_track();
            else {
                svc_track_start();
                if (!svc_track_active()) {
                    nos_feedback(FB_ERROR);
                    nos_toast("CAN'T START");
                    return 1;
                }
                N.saved_state = 0;
                N.d_init = 0;                 /* the new track's distance starts at 0, no count */
                nos_feedback(FB_TOGGLE_ON);
                N.press_until = nos_t + 0.1f; /* the crossfade to STOP starts from the pressed fill */
            }
            return 1;
        }
        return 0;
    default: return 0;
    }
}

const nos_screen scr_nav = { "NAVIGATE", nav_enter, nav_leave, nav_tick, nav_draw, nav_input,
                             SCR_HDRAG | SCR_KEEPAWAKE };

/* UI-task diagnostic: return the actual pager state without ticking/drawing. */
void out_nav_scroll_snapshot(nos_scroll_snapshot *s)
{
    *s = (nos_scroll_snapshot){ 5, 1, N.pg.page, NPAGE, N.pg.dragging, N.pg.sp.x, (NPAGE - 1) * PAGE_W };
}
