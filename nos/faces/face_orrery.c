/* face_orrery.c — ORRERY: the solar system as it is right now. A port of the reviewed
 * prototype (docs/SOLAR_FACE.html, docs/assets/solar/watch.js).
 *
 * LOCKED (a watch face): the clock over the Sun, the eight planets at their real
 * heliocentric longitudes on rank-spaced orbits, the asteroid belt and the Kuiper belt,
 * comets with tails, the next alignment as the one red element (geometry + chip).
 * Every touch goes to home as on any face, except holds:
 *   release after 0.75 s -> 3D tilt on / off;  after 1.5 s -> unlock / lock;
 *   still held at 3 s -> the face picker.  A segmented line ring round the finger shows the stage.
 * UNLOCKED (the face takes all touch through nos_face_input):
 *   - drag: PAN (default; in 3D too), LOOK (orbit the 3D camera) or TIME (grab a planet and
 *     drag it round the Sun; months tick, events are magnets), chosen in the menu
 *   - time rings: swipe inward from the top-right rim to open three date dials (years / months /
 *     days); drag a ring to travel, tap a value to fly there, NOW in the middle comes home
 *   - zoom dial: bottom rim 4:30-7:30, 0.5-4x about the middle of the glass
 *   - menu: swipe left from the right rim: DRAG, VIEW (overhead /
 *     tilt), FILTER (planets, asteroids, comets, craft, from Earth), LINES; icons only
 *   - zones: tap a band for the inner / outer system or beyond Neptune; rim = out a level,
 *     core = in, the top = home; edge swipe from the left half = back to the face
 *   - tap a planet or the Sun: its card (orrery_card.c); tap the chip: the events list
 *   - motion blur while time moves fast: planets streak along their own orbits
 *   - side button or sleep locks it again and the view eases home.
 *
 * Cost rules (draw runs once per 16-row stripe): every position, label and layout is
 * retained as drawing commands with per-stripe indices, rebuilt in tick only
 * when their inputs change. Pixels go directly into internal stripe buffers;
 * the clock has separate damage commands.
 * Hold/unread overlays are clipped. Float maths with fast sin/cos/sqrt for the 4000
 * asteroids; doubles only for ephemerides. All big storage lives in PSRAM. */
#include "faces/face_kit.h"
#include "faces/face_ext.h"
#include "faces/orrery.h"
#include "nos.h"
#include "ephem.h"
#include "gfx_cost.h"
#include "prefs.h"
#include "prog.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CX UI_CX
#define CY UI_CY
#define PI_F 3.14159265f
#define D2RF (PI_F / 180.0f)
#define NORB 128                     /* resting ellipse; LOOK uses 64 segments, zoom replays scaled coverage */
#define CHIP_Y 380
#define EDGE_R 197.0f
#define CONTROL_EDGE_R (GFX_W * 0.5f - 22.0f)
#define HOLD_TILT 0.75f
#define HOLD_LOCK 1.5f
#define HOLD_PICK 3.0f
#define NEV 64
/* zoom dial: bottom rim, 4:30 -> 7:30 */
#define ZOOM_R 220.0f
#define ZOOM_W 20.0f
#define ZOOM_P0 135.0f
#define ZOOM_P1 225.0f
#define ZB_P0 (ZOOM_P0 + 11)   /* the segmented bar: 0.5x at ZB_P1 (left), 4x at ZB_P0; - and + sit beyond its ends */
#define ZB_P1 (ZOOM_P1 - 11)
#define ZMIN 0.5f
#define ZMAX 4.0f
/* time rings */
#define RING_W 22.0f
#define TAB_PHI 43.0f
#define ORB_SHRINK 0.33f
/* menu */
#define H_P0 70.0f
#define H_P1 110.0f
/* buttons 1.5x the first size (the user: too small to hit); the lowest main button ends near
 * 125 deg, clear of the zoom dial's touch arc (from 131 deg); fans stop at OPT_MAX, above the
 * event chip */
#define MENU_R 190.0f
#define OPT_R 132.0f   /* MENU_R - 25.5 - 24.5 - 8: a gap between the fan and the main buttons */
#define BTN 25.5f
#define OPT_STEP 24.0f
#define OPT_MAX 126.0f

static const float BODY_R[EP_N] = { 3.5f, 5.5f, 5.5f, 4.5f, 10.0f, 8.5f, 6.5f, 6.5f };
/* Six pixels of rim clearance for the index; three pixels between bands. */
static const float RING_R[3] = { 215, 190, 165 }, TAB_R[3] = { 226, 216, 206 };
static const double RING_LAP[3] = { 12 * 365.25, 365.25, 30 };
static const float MENU_PHI[4] = { 63.75f, 81.25f, 98.75f, 116.25f };
static const char *const MON[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
static const char *const DOW[7] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };

enum { V_FACE, V_INNER, V_OUTER, V_BEYOND };
enum { DR_PAN, DR_LOOK, DR_TIME };
enum { M_DRAG, M_VIEW, M_FILTER, M_LINES };

static float orr_root(float a)
{
    if(a<=0) return 0;
    float r=gfx_fast_sqrt(a);
    return 0.5f*(r+a/r);
}
static float orr_hypot(float x,float y) { return orr_root(x*x+y*y); }
static float quad(float t) { t = t < 0 ? 0 : t > 1 ? 1 : t; return t < 0.5f ? 2 * t * t : 1 - 2 * (1 - t) * (1 - t); }
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float n180(float a) { while (a > 180) a -= 360; while (a < -180) a += 360; return a; }
/* cheap log10 for the beyond map (exponent + quadratic mantissa; < 0.5 % error) */
static float fast_log10(float x)
{
    union { float f; uint32_t i; } u = { x };
    float e = (float)(int)((u.i >> 23) & 255) - 127.0f;
    u.i = (u.i & 0x007FFFFF) | 0x3F800000;
    float m = u.f;                                    /* 1..2 */
    float l2 = e + (-0.34484843f * m + 2.02466578f) * m - 0.67487759f;
    return l2 * 0.30103f;
}

/* ---- radial maps: au -> px, one per zone ---- */
static const float FACE_PTS[][2] = { { 0, 60 }, { 0.387f, 82 }, { 0.723f, 99 }, { 1, 116 }, { 1.524f, 133 }, { 2.1f, 141 },
                                     { 3.3f, 154 }, { 5.2f, 167 }, { 9.54f, 185 }, { 19.2f, 202 }, { 30.1f, 217 }, { 45, 226 } };
static float rmap1(int v, float au)
{
    switch (v) {
    case V_INNER: return fminf(224, 116 * gfx_fast_sqrt(au));
    case V_OUTER: return fminf(224, 35.3f * gfx_fast_sqrt(au));
    case V_BEYOND: return clampf(30 + 80 * fast_log10(fmaxf(au, 0.6f)), 4, 226);
    default: {
        int n = (int)(sizeof FACE_PTS / sizeof FACE_PTS[0]);
        for (int i = 1; i < n; i++)
            if (au <= FACE_PTS[i][0]) {
                float a0 = FACE_PTS[i - 1][0], r0 = FACE_PTS[i - 1][1], a1 = FACE_PTS[i][0], r1 = FACE_PTS[i][1];
                return r0 + (r1 - r0) * (au - a0) / (a1 - a0);
            }
        return 226;
    }
    }
}

/* a point kept as its distance and direction (no trig per frame) */
typedef struct { float au, s, c, zr; } pp;   /* s = y/au, c = x/au, zr = z/au */
static pp mkpp(float x, float y, float z)
{
    pp p; p.au = gfx_fast_sqrt(x * x + y * y);
    float inv = p.au > 1e-6f ? 1.0f / p.au : 0;
    p.s = y * inv; p.c = x * inv; p.zr = z * inv;
    return p;
}

/* one belt asteroid, prepared once: in-plane basis (P, Q), shape, motion */
typedef struct { float Px, Py, Pz, Qx, Qy, Qz, a, b, e, n, M0, ep, refresh; } ast_k;
typedef struct { float due; uint16_t body, pad; } belt_due;

typedef struct { float x, y; char s[28]; uint16_t col; uint8_t align, big; } label_t;
#define NLAB 48
typedef struct { float x, y, w; } rect_t;
/* Compact retained primitives, not pixels. Indices are ordered in each bucket. */
#define ORR_STRIPES ((GFX_H + 15) / 16)
#define ORR_COMMANDS 2048
#define ORR_REFS 8192
#define ORR_TEXTS 256
typedef struct {
    float f[6];
    uint16_t ink, aux;
    int16_t x0, x1;
    uint8_t type, first, last, category;
} orr_cmd;
typedef struct { const font_t *font; dot_style dot; char s[64]; int align, tracking; } orr_text;
#define ORR_DOTS 4096
typedef struct { int16_t x, y; uint16_t ink; uint8_t shape, layer; } orr_pixel;
/* Coverage runs retain blend order within each row, including ellipse joins. */
#define ORR_ORBIT_ROWS 4096
#define ORR_ORBIT_BIAS (ORR_ORBIT_ROWS/2)
typedef struct { int16_t x,y; uint8_t alpha,colour; } orr_orbit_pixel;
typedef struct {
    orr_orbit_pixel *px,*scratch;
    /* Precomposed first layer, exact at the current unscaled pose. */
    struct orr_orbit_ink *ink;
    unsigned ink_key;
    uint32_t row[ORR_ORBIT_ROWS+1], next[ORR_ORBIT_ROWS];
    unsigned key, shape_key, builds;
    int n,capacity,valid,origin_x,origin_y;
    float phase_x,phase_y,zoom,shrink;
} orr_orbit_cache;
typedef struct orr_orbit_ink { int16_t x; uint16_t ink; } orr_orbit_ink;
typedef struct { float cx,cy,r_mid,half,e0x,e0y,e1x,e1y,bx,by,cos_hw,cos_hw2,r_out2,r_in2,rs_lo2,rs_hi2,wx0,wx1,y0,y1; } orr_arc_geom;
#define ORR_ARCS 64
/* Small fixed-UI capture cache; pan never changes these commands. */
#define ORR_UI_CMDS 96
#define ORR_UI_TEXTS 8
#define ORR_UI_ARCS 8
typedef struct {
    orr_cmd cmd[ORR_UI_CMDS];
    orr_text text[ORR_UI_TEXTS];
    orr_arc_geom arc[ORR_UI_ARCS];
    unsigned key, builds, hits;
    int n,nt,na,valid;
} orr_ui_cache;
#define ORR_HOT_W 96
#define ORR_HOT_H 48
typedef struct {
    uint8_t alpha[ORR_HOT_H][ORR_HOT_W];
    struct { uint8_t lo,hi,solid_lo,solid_hi; } row[ORR_HOT_H][2];
    float radial[ORR_HOT_H][4][2];
    int radial_ready;
    int x,y,w,h,ready;
    float radius,thickness,phi,half;
} orr_hot_mask;
typedef struct {
    orr_pixel dot_scratch[ORR_DOTS];
    int ndots, dot_quant_valid, dot_nbelt;
    unsigned dot_style_key;
    /* Catalog-indexed row chains: edit only bodies refreshed this tick.
     * Sorted IDs preserve the old stable row sort's opaque overlap order. */
    uint16_t dot_head[ORR_ORBIT_ROWS], dot_link[ORR_DOTS], dot_prev[ORR_DOTS];
    int dot_chain_valid, dot_items;
    unsigned dot_updates;
    float dot_panx, dot_pany, dot_zoom, dot_shrink;
    orr_cmd cmd[ORR_COMMANDS];
    uint16_t refs[ORR_REFS];
    uint32_t start[ORR_STRIPES + 1];
    orr_text text[ORR_TEXTS];
    orr_arc_geom arc[ORR_ARCS];
    ui_arc_text ring_text[120];
    unsigned ring_text_key[120];
    orr_hot_mask ring_hot[3];
    struct { float x0,y0,x1,y1; uint16_t ink; } ring_tick[3][200];
    uint16_t ring_tick_ref[3][ORR_STRIPES][48];
    uint8_t ring_tick_count[3][ORR_STRIPES];
    int n, nt, na, overflow;
} orr_list;

enum { OP_DOTS, OP_ORBITS, OP_PLANETS, OP_TEXT, OP_CONTROLS, OP_TRAILS, OP_OTHER, OP_N };
enum { OP_RASTER, OP_CAPTURE, OP_BUCKET };
typedef struct {
    uint32_t us[3][OP_N], stamp, frame;
    uint32_t frame_us[5], peak_us[5], sum_us[5], peak_total, peak_frame;
    uint32_t detail_us[5], sum_detail[5], peak_detail[5];
    uint32_t frame_cat[OP_N], peak_cat[OP_N];
    int context[8], peak_context[8];
    uint32_t layout_us[6], peak_layout_us[6];
    uint32_t sum_layout[6], arc_us[2], sum_arc[2], sum_record, sum_compare;
    unsigned last_build[4], peak_build[4];
    unsigned frames, rebuilds, raster_frames;
    int enabled, active, stage, drew, scrub;
} orr_profile_state;

typedef struct {
    /* time */
    orr_profile_state profile;
    float orbit_work[NORB * 3];
    double jd_now, off, jd;          /* now, days travelled, the face's time (now + off) */
    double tz_days;                  /* local - UTC */
    double vel;                      /* days per frame of momentum after a scrub / ring flick */
    double ta_from, ta_to; int ta_t, ta_n;   /* an animated jump (NOW, a ring tap, the events list) */
    int snapped;                     /* index of the event time snapped onto (-1 none) */
    int grab;                        /* planet whose pace a TIME drag uses */
    int month_key;
    /* sky */
    double jd_sky, jd_orb, jd_belt, jd_small, jd_prev_frame;
    ep_sky sky;
    pp orb[EP_N][NORB], pl[EP_N], pluto;
    double E_prev[EP_N], E_now[EP_N];
    double anomaly_jd;
    unsigned anomaly_mask;
    double sky_E[EP_N],sky_E_prev[EP_N],sky_E_prev_jd;
    ep_conic_cache conic[64];
    int conic_ready,conic_pluto;
    /* events */
    ep_event ev[NEV]; int nev; ep_scan scan; int scanning; double scan_start;
    unsigned events_generation;
    /* belt (PSRAM) */
    /* Weighted exact preview: 120 solves while moving. Release starts with
     * four 64-body ticks while glyphs settle, then 448 per tick. Initial
     * entry remains exact; the final sweep shares one date. */
    int belt_cursor, belt_left, belt_first, belt_count, belt_partial;
    double belt_target;
    unsigned belt_version, belt_projected, radius_version[4];
    int belt_visible, dot_slice;
    uint16_t belt_changed[448]; belt_due *belt_queue;
    int nbelt; const ast_k *bk; pp *bp; float *bxs, *bys; uint8_t *bh;
    /* small bodies, comets, craft */
    int small_cursor, small_left, sky_rest_ticks, time_touch, sky_preview;
    double small_target; unsigned small_version;
    pp sm[32]; int nsm; pp cm[32]; int ncm; pp cr[24]; int ncr; float cm_prev[32][2];
    /* view */
    int locked; float lockk;
    int tilt; float tiltk, cam_tilt, az, azv;
    float zoom, panx, pany, pvx, pvy;
    /* Lazy radial maps per view; zoom/tilt never changes AU -> map radius. */
    float *belt_radius[4], orbit_radius[4][EP_N][NORB];
    float orbit_shift[EP_N][NORB][3]; /* pre-pan terms, preserving subtraction order */
    double radius_belt_jd[4], radius_orbit_jd[4];
    int view, from; float tx;        /* zone morph */
    int drag_mode, lines, from_earth, show[4];   /* show: planets, asteroids, comets, craft */
    /* rings */
    int rings_open, rings_pull; float ringk, ringe;
    int ring_now_down, ring_warm;
    float platek, platee;          /* the read-out plate: 0 = date only (now), 1 = expanded with NOW (travelled) */
    /* menu */
    int menu_open, menu_pull, menu_sub; float menuk, menue, subk, sube;
    struct { float x, y; int id, key, kind; } mhit[16]; int nmh;
    /* screen cache */
    float ox[EP_N][NORB], oy[EP_N][NORB], px[EP_N], py[EP_N], pr[EP_N], litux[EP_N], lituy[EP_N], plx, ply;
    float sunx, suny, sc, ct, st, ca, sa;   /* sc: ring shrink; tilt and azimuth sin/cos for this frame */
    float smx[32], smy[32], cmx[32], cmy[32], crx[24], cry[24], tailx[32], taily[32];
    int sm_on[32], cm_on[32], cr_on[24], pl_on[EP_N], pluto_on;
    float blur_x[EP_N][NORB + 1], blur_y[EP_N][NORB + 1]; int blur_n[EP_N];
    label_t lab[NLAB]; int nlab; rect_t used[NLAB + 8]; int nused;
    /* rings layout */
    struct { float phi, sn, cs; char s[6]; uint8_t major; int width; } rl[3][40]; int nrl[3], hot[3];
    double ring_base_jd[3]; int ring_calendar[3];
    double ring_exact_jd[3]; int ring_exact_calendar[3], rings_motion;
    float ring_delta[3];
    float rt_basis[3][200][2];
    float rt[3][200]; uint8_t rtm[3][200]; int nrt[3];
    char readout[24], readout2[32];
    char ring_time[16], ring_date[24], ring_state[24], ring_clock[8];
    char ring_return[24]; int ring_return_width, ring_return_dot;
    /* chip / NOW */
    char chip[48]; int chip_ev, now_pill; float chip_w;
    /* touch */
    int hold; nos_time_t hold_t0, down_t; int hx, hy, hold_stage;
    int drag; float d_px, d_py, d_a, d_ring_phi; int d_ring, d_wasopen;
    int home_t; float home_px, home_py, home_z;
    int pulse;
    /* Geometry, coverage streams and retained commands live in PSRAM. */
    orr_list *list;
    orr_orbit_cache *orbit_pixels;
    gfx_rec scene_rec;
    unsigned scene_input, scene_ink, ui_key;
    orr_ui_cache *ui_cache, *plate_capture[2], *ring_capture[4]; /* three dials + grip */
    int scene_valid, clock_overlay, dots_overflow;
    unsigned layout_key, menu_key, label_key, orbit_geom_key;
    int orbit_geom_valid;
    int layout_valid, menu_valid, rings_valid, labels_shifted, dot_preview;
    unsigned dot_geometry_key, dot_builds, label_builds, ring_builds;
    float layout_panx, layout_pany, label_chip_w;
    double rings_jd, rings_off, rings_tz;
} orr_state;
/* ~88 KiB including retained geometry, so it lives in PSRAM: internal DRAM is full on the watch, and a static copy there left
 * the BLE controller no memory to start (boot loop: "BLE assert emi.c 164", interrupt watchdog).
 * Every entry point calls orr_ok() first; S reads the same as a plain struct. */
static orr_state *s_orr;
static nos_time_t orr_last_draw;
static int s_target, s_edge;       /* touch-down owner; edge opener is never re-hit during a drag */
#define S (*s_orr)

/* Read-only: querying home resource priority never creates face state. */
int face_orrery_interactive(void) { return s_orr && !S.locked; }
extern const nos_screen scr_home;
static int orr_ok(void)
{
    if (s_orr) return 1;
    if (!(s_orr = hal_big_alloc(sizeof *s_orr))) return 0;
    memset(s_orr, 0, sizeof *s_orr);
    S.locked = 1; S.lockk = 1; S.cam_tilt = 64; S.zoom = 1; S.lines = 1;
    for (int i = 0; i < 4; i++) S.show[i] = 1;
    S.snapped = -1; S.chip_ev = -2; S.grab = EP_EARTH; S.tx = 1; S.from = -1;
    return 1;
}

static double travel_date(double jd, int notify)
{
    double bounded = !isfinite(jd) ? (jd > 0 ? EP_JD_MAX : EP_JD_MIN) :
                     jd < EP_JD_MIN ? EP_JD_MIN : jd > EP_JD_MAX ? EP_JD_MAX : jd;
    if (notify && jd != bounded && S.jd != bounded) nos_toast("TIME RANGE 1800-2050");
    return bounded;
}
static void travel_limit(void)
{
    double requested = S.jd_now + S.off;
    double bounded = travel_date(requested, 1);
    if (bounded != requested) { S.off = bounded - S.jd_now; S.vel = 0; S.ta_n = 0; S.snapped = -1; }
    S.jd = bounded;
}

/* Half-angle of the dock sector; shared by rendering and ring hit testing. */
static float ring_gap(int ring) { (void)ring; return 56; }

/* ---------------------------------------------------------------------------- calendar */
static long jdn_of(long y, long m, long d)
{
    long a = (14 - m) / 12, yy = y + 4800 - a, mm = m + 12 * a - 3;
    return d + (153 * mm + 2) / 5 + 365 * yy + yy / 4 - yy / 100 + yy / 400 - 32045;
}
typedef struct { int y, m, d, h, mi, wd; } civ;
#include "face_orrery_profile.h"

/* Tick timings belong exclusively to ORRPEAK; nos_dbg_us is the compositor. */
#define ORR_TIME_START(name) uint32_t name=orr_prof_now()
#define ORR_TIME_END(slot,name) do { uint32_t elapsed=orr_prof_now()-(name); \
    if(orr_prof.enabled)orr_prof.frame_us[slot]+=elapsed; } while(0)

static civ civil(double jd)                      /* local civil time of a UTC JD */
{
    double l = jd + S.tz_days + 0.5;
    long z = (long)floor(l);
    double f = l - z;
    long a = z + 32044, b = (4 * a + 3) / 146097, c = a - 146097 * b / 4, d = (4 * c + 3) / 1461, e = c - 1461 * d / 4,
         m = (5 * e + 2) / 153;
    civ r;
    r.d = (int)(e - (153 * m + 2) / 5 + 1); r.m = (int)(m + 3 - 12 * (m / 10)); r.y = (int)(100 * b + d - 4800 + m / 10);
    r.h = (int)(f * 24); r.mi = (int)((f * 24 - r.h) * 60); r.wd = (int)((z + 1) % 7);
    return r;
}
static double jd_local(long y, long m, long d, double hour)   /* UTC JD of a local wall time */
{
    while (m < 1) { m += 12; y--; }
    while (m > 12) { m -= 12; y++; }
    return (double)jdn_of(y, m, 1) + (d - 1) - 0.5 + hour / 24.0 - S.tz_days;
}

/* ---------------------------------------------------------------------------- geometry */
static float view_r(float au)
{
    float a = rmap1(S.view, au);
    if (S.from < 0 || S.tx >= 1) return a;
    float b = rmap1(S.from, au);
    return b + (a - b) * quad(S.tx);
}
static float zone_mix(int v)
{
    if (S.view == v) return S.from >= 0 ? quad(S.tx) : 1;
    return S.from == v ? 1 - quad(S.tx) : 0;
}
/* screen position of a point; returns the unscaled map radius (zone logic) */
static float proj_radius(const pp *p, float r, float *sx, float *sy)
{
    float rs = r * S.sc * S.zoom;
    float sl = p->s * S.ca + p->c * S.sa, cl = p->c * S.ca - p->s * S.sa;
    *sx = CX + S.panx - rs * sl;
    *sy = CY + S.pany - rs * cl * S.ct - p->zr * rs * S.st;
    return r;
}
static float proj(const pp *p, float *sx, float *sy)
{ return proj_radius(p,view_r(p->au),sx,sy); }
static void radius_cache(int view)
{
    if(S.radius_orbit_jd[view]!=S.jd_orb) {
        for(int p=0;p<EP_N;p++) for(int i=0;i<NORB;i++)
            S.orbit_radius[view][p][i]=rmap1(view,S.orb[p][i].au);
        S.radius_orbit_jd[view]=S.jd_orb;
    }
    if(!S.belt_radius[view] && S.nbelt) S.belt_radius[view]=hal_big_alloc((uint32_t)S.nbelt*sizeof(float));
    if(S.belt_radius[view] && (S.radius_belt_jd[view]!=S.jd_belt || S.radius_version[view]!=S.belt_version)) {
        int partial=S.belt_partial && S.radius_version[view]+1==S.belt_version;
        int count=partial?S.belt_count:S.nbelt;
        for(int j=0;j<count;j++) { int i=partial?S.belt_changed[j]:j; S.belt_radius[view][i]=rmap1(view,S.bp[i].au); }
        S.radius_version[view]=S.belt_version;
        S.radius_belt_jd[view]=S.jd_belt;
    }
}
static float cached_radius(float a,float b)
{ return S.from<0 || S.tx>=1 ? a : b+(a-b)*quad(S.tx); }
static void polar_xy(float phi_deg, float r, float *x, float *y) { *x = CX + r * fk_sin(phi_deg * D2RF); *y = CY - r * fk_cos(phi_deg * D2RF); }
static float phi_of(float x, float y) { float a = atan2f(x - CX, -(y - CY)) / D2RF; return a < 0 ? a + 360 : a; }

/* ---------------------------------------------------------------------------- the belt */
/* Weighted fair queue: fast/eccentric bodies are refreshed more often
 * than distant Kuiper dots. Queue order never changes catalog/blend order. */
static void belt_queue_sink(int root)
{
    belt_due value=S.belt_queue[root];
    for(int child=root*2+1;child<S.nbelt;child=root*2+1) {
        if(child+1<S.nbelt && S.belt_queue[child+1].due<S.belt_queue[child].due)child++;
        if(value.due<=S.belt_queue[child].due)break;
        S.belt_queue[root]=S.belt_queue[child];root=child;
    }
    S.belt_queue[root]=value;
}
static void belt_basis_calculate(ast_k *a,const od_ast *o)
{
        float cw = fk_cos(o->w * D2RF), sw = fk_sin(o->w * D2RF), cO = fk_cos(o->node * D2RF), sO = fk_sin(o->node * D2RF);
        float cI = fk_cos(o->i * D2RF), sI = fk_sin(o->i * D2RF);
        a->Px = cw * cO - sw * sO * cI; a->Py = cw * sO + sw * cO * cI; a->Pz = sw * sI;
        a->Qx = -sw * cO - cw * sO * cI; a->Qy = -sw * sO + cw * cO * cI; a->Qz = cw * sI;
        a->a = o->a; a->e = o->e; a->b = o->a * gfx_fast_sqrt(1 - o->e * o->e);
        /* Refine once: a tiny mean-motion error accumulates over pack epochs. */
        float root = gfx_fast_sqrt(o->a); root = 0.5f * (root + o->a / root);
        a->n = 0.9856076686f / (o->a * root); a->M0 = o->ma; a->ep = o->ep;
        a->refresh=(1-a->e)*(1-a->e)/a->n;
}
/* The belt's rows come from <sd>/sky/belt.bin (orrery_sd.c); the basis is derived from them here, sliced over
 * ticks, and S.bk is published only when all of it is done. No card or file: no belt, nothing else changes. */
#define BELT_SLICE 500
static struct { ast_k *k; pp *bp; float *f; uint8_t *bh; belt_due *q; int pos; } BI;
static void belt_init(int aod)
{
    if(S.bk || !od_belt_n || aod)return;
    od_belt_want();od_sd_pump();
    const od_ast *rows=od_belt_rows();
    if(!rows)return;
    int n=od_belt_n;
    if(!BI.k) {
        BI.k=hal_big_alloc((uint32_t)(n*sizeof(ast_k)));
        BI.bp=hal_big_alloc((uint32_t)(n*sizeof(pp)));
        BI.f=hal_big_alloc((uint32_t)(n*2*sizeof(float)));
        BI.bh=hal_big_alloc((uint32_t)n);
        if(!BI.k || !BI.bp || !BI.f || !BI.bh) {
            hal_big_free(BI.k);hal_big_free(BI.bp);hal_big_free(BI.f);hal_big_free(BI.bh);memset(&BI,0,sizeof BI);return;
        }
        BI.q=hal_big_alloc(n*sizeof *BI.q); /* optional */
        BI.pos=0;
    }
    int end=BI.pos+BELT_SLICE<n?BI.pos+BELT_SLICE:n;
    for(int k=BI.pos;k<end;k++) {
        const od_ast *o=&rows[k];
        ast_k *a=&BI.k[k];
        belt_basis_calculate(a,o);
        if(BI.q)BI.q[k]=(belt_due){a->refresh,(uint16_t)k,0};
        BI.bh[k]=(uint8_t)(o->h10<80?0:o->h10<100?1:2);
        BI.f[k]=NAN;BI.f[n+k]=0;
    }
    if((BI.pos=end)<n)return;
    /* UI thread owns these arrays; publishing the basis pointer commits completeness. */
    S.bp=BI.bp;S.bh=BI.bh;S.bxs=BI.f;S.bys=BI.f+n;S.belt_queue=BI.q;S.nbelt=n;
    if(BI.q)for(int k=n/2-1;k>=0;k--)belt_queue_sink(k);
    S.bk=BI.k;
    memset(&BI,0,sizeof BI);
    od_belt_drop();
}
static float belt_anomaly(float M, float e)
{
    /* Preserve the inexpensive ordinary-belt path, but accept it only when its
     * residual agrees with true sine. High-e bodies use the bracketed solve. */
    if (e >= .9f) return (float)ep_kepler((double)M * 57.2957795130823208768, e);
    float E = M + e * fk_sin(M);
    for (int it = 0; it < 3; it++) E -= (E - e * fk_sin(E) - M) / (1 - e * fk_cos(E));
    if (!isfinite(E) || fabsf(E - e * sinf(E) - M) > 2e-6f)
        return (float)ep_kepler((double)M * 57.2957795130823208768, e);
    return E;
}
static void belt_plane(const ast_k *a, float M, float *xp, float *yp)
{
    if (a->e >= .9f) {
        /* Near perihelion cos(E)-e is a small difference; approximate float trig
         * can visibly rotate it even after the anomaly converges. These few
         * catalog records retain double trig/subtraction until plane publish. */
        double E = ep_kepler((double)M * 57.2957795130823208768, a->e);
        *xp = (float)(a->a * (cos(E) - a->e)); *yp = (float)(a->b * sin(E));
    } else {
        float E = belt_anomaly(M, a->e);
        *xp = a->a * (fk_cos(E) - a->e); *yp = a->b * fk_sin(E);
    }
}
static pp belt_exact(int k,double jd)
{
    const ast_k *a = &S.bk[k];
    float t = (float)(jd - 2451545.0);
    float M = a->M0 + a->n * (t - a->ep);
    M -= (int)(M / 360.0f) * 360.0f; if (M < 0) M += 360; M *= D2RF;
    float xp, yp; belt_plane(a, M, &xp, &yp);
    return mkpp(a->Px * xp + a->Qx * yp, a->Py * xp + a->Qy * yp, 0);
}
static void belt_update(double jd)
{
    S.belt_partial=0;S.belt_count=0;
    if (!S.bk || (S.jd_belt && S.ringk>0 && S.ringk<1)) return;
    int moving=S.time_touch || S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0;
    if(!S.jd_belt) {
        S.belt_first=0; S.belt_count=S.nbelt;
        S.belt_target=jd; S.belt_left=0;
    } else {
        /* Don't restart for the RTC's subpixel drift. Motion retargets the
         * next slice; after release one full sweep converges to that date. */
        if(fabs(jd-S.jd_belt)>=.05) {
            S.belt_target=jd; S.belt_left=S.nbelt;
        }
        if(!S.belt_left)return;
        S.belt_first=S.belt_cursor;
        int budget=moving?120:S.sky_rest_ticks<=4?64:448;
        S.belt_count=S.belt_left<budget?S.belt_left:budget;
        S.belt_left-=S.belt_count;
        S.belt_partial=1;
        for(int j=0;j<S.belt_count;j++) {
            if(moving && S.belt_queue) {
                int k=S.belt_queue[0].body;S.belt_changed[j]=(uint16_t)k;
                S.belt_queue[0].due+=S.bk[k].refresh;belt_queue_sink(0);
            } else S.belt_changed[j]=(uint16_t)((S.belt_first+j)%S.nbelt);
        }
        if(moving && S.belt_queue) {
            /* A weighted preview is not a contiguous exact sweep. Release
             * must visit every catalog index once before claiming settled. */
            S.belt_left=S.nbelt;
            GFX_COST_PX(GFX_COST_ORR_PRE_CPU,2500*S.belt_count);
        }
    }
    for(int j=0;j<S.belt_count;j++) {
        int k=S.belt_partial?S.belt_changed[j]:j;
        S.bp[k]=belt_exact(k,S.belt_target);
    }
    S.belt_cursor=(S.belt_first+S.belt_count)%S.nbelt;
    /* 77ac23a board astronomy 40.3 ms: 38.8 ms / 3999 Kepler solves,
     * plus the existing 0.5 ms planets and 1 ms small-body allowance.
     * Aggregate attribution, not separately measured microbenchmarks. */
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,S.belt_count*9700);
    S.jd_belt=S.belt_target; S.belt_version++;
}

/* ---------------------------------------------------------------------------- the sky */
static void sky_update(double jd, int aod)
{
    if(!S.conic_ready) {
        S.conic_pluto=-1;
        int ns=od_small_n<32?od_small_n:32,nc=od_comets_n<32?od_comets_n:32;
        for(int i=0;i<ns+nc;i++) {
            ep_conic_prepare(S.conic+i,i<ns?&od_small_bodies[i].el:&od_comets[i-ns].el);
            if(i<ns && strstr(od_small_bodies[i].name,"Pluto"))S.conic_pluto=i;
        }
        S.conic_ready=1;
    }
    /* Open-ring motion uses bounded previews; release forces an exact refresh
     * even when the date did not change. Ordinary wall-clock motion is subpixel. */
    int preview=S.ringe>0.5f && (S.time_touch || S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0);
    double cadence = S.time_touch || S.drag == 4 || S.drag == 5 || S.ta_n || S.vel != 0 ? 0 : 0.05;
    int exact_refresh=S.sky_preview && !preview;
    int date_moved=fabs(jd-S.jd_sky)>cadence;
    int moved = exact_refresh || date_moved;
    double sky_jd=date_moved?jd:S.jd_sky;
    uint32_t sky_stamp=orr_prof.enabled?orr_prof_now():0;
    if (moved) {
        /* The face consumes helio, earth and distance only. Observer longitude,
         * latitude, elongation and phase belong to the event/card calculators.
         * Settling keeps the original refresh epoch, double ephemerides and
         * distance expression; a later RTC tick must not shift settled pixels. */
        S.sky.jd=sky_jd;
        memcpy(S.sky_E_prev,S.sky_E,sizeof S.sky_E);
        S.sky_E_prev_jd=S.jd_sky;
        S.sky.earth=preview?ep_helio_preview(EP_EARTH,sky_jd,&S.sky_E[EP_EARTH]):ep_helio_anomaly(EP_EARTH,sky_jd,&S.sky_E[EP_EARTH]);
        for (int p = 0; p < EP_N; p++) {
            ep_vec h=p==EP_EARTH?S.sky.earth:preview?ep_helio_preview(p,sky_jd,&S.sky_E[p]):ep_helio_anomaly(p,sky_jd,&S.sky_E[p]);
            S.sky.b[p].helio=h;
            double x=h.x-S.sky.earth.x,y=h.y-S.sky.earth.y,z=h.z-S.sky.earth.z;
            S.sky.b[p].dist=sqrt(x*x+y*y+z*z);
            S.pl[p] = mkpp((float)h.x, (float)h.y, (float)h.z);
        }
        ep_vec pl = S.conic_pluto>=0?(preview?ep_conic_preview(S.conic+S.conic_pluto,sky_jd):ep_conic_cached(S.conic+S.conic_pluto,sky_jd)):orrery_pluto(sky_jd);
        S.pluto = mkpp((float)pl.x, (float)pl.y, (float)pl.z);
        GFX_COST_PX(GFX_COST_ORR_PRE_CPU,500000);
        S.jd_sky = sky_jd;
    }
    if(orr_prof.enabled)orr_prof.detail_us[0]+=orr_prof_now()-sky_stamp;
    S.sky_preview=preview;
    if (aod) return;   /* grey planets need neither orbits, belt nor small bodies */
    /* The displayed Kepler ellipses are fixed at first use. Time travel moves
     * bodies along them, rather than rebuilding secular elements every month. */
    if (S.jd_orb == 0) {
        float *xyz=S.orbit_work;
        for (int p = 0; p < EP_N; p++) {
            ep_orbit(p, jd, NORB, xyz);
            for (int i = 0; i < NORB; i++) S.orb[p][i] = mkpp(xyz[i * 3], xyz[i * 3 + 1], xyz[i * 3 + 2]);
        }
        S.jd_orb = jd;
    }
    int time_moving=S.time_touch || S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0;
    if(time_moving)S.sky_rest_ticks=0;else if(S.sky_rest_ticks<100)S.sky_rest_ticks++;
    int initial=S.jd_small==0;
    S.nsm=od_small_n<32?od_small_n:32; S.ncm=od_comets_n<32?od_comets_n:32;
    int total=S.nsm+S.ncm;
    if(initial || fabs(jd-S.jd_small)>.05) { S.small_target=jd;S.small_left=total; }
    else if(exact_refresh)S.small_left=total;
    uint32_t small_stamp=orr_prof.enabled?orr_prof_now():0;
    if(S.small_left && (initial || S.ringk<=0 || S.ringk>=1)) {
        int moving=S.time_touch || S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0;
        /* Exact release already refreshes all planets and settles arc masks.
         * Two small bodies per early settle tick avoid stacking eight double
         * solves on that frame. The later 8-body sweep finishes before the
         * existing belt sweep, at the same retained epoch. Motion stays at 8. */
        int count=initial?total:moving?8:S.sky_rest_ticks<=4?2:8;
        if(count>S.small_left)count=S.small_left;
        for(int j=0;j<count;j++) {
            int i=(S.small_cursor+j)%total;
            ep_vec v=preview?ep_conic_preview(S.conic+i,S.small_target):ep_conic_cached(S.conic+i,S.small_target);
            pp q=mkpp((float)v.x,(float)v.y,(float)v.z);
            if(i<S.nsm)S.sm[i]=q;else S.cm[i-S.nsm]=q;
        }
        S.small_cursor=(S.small_cursor+count)%total; S.small_left-=count;
        GFX_COST_PX(GFX_COST_ORR_PRE_CPU,total?1000000u*count/total:0);
        S.jd_small=S.small_target;S.small_version++;
    }
    if(orr_prof.enabled)orr_prof.detail_us[1]+=orr_prof_now()-small_stamp;
    if(initial) {
        S.ncr=0;
        for(int i=0;i<od_crafts_n && S.ncr<24;i++) {
            const od_craft *c=&od_crafts[i];
            S.cr[S.ncr++]=mkpp((float)c->xyz[0],(float)c->xyz[1],(float)c->xyz[2]);
        }
    }
    ORR_DETAIL(2,belt_update(jd));
}

/* ---------------------------------------------------------------------------- events */
static int ev_cmp(const void *a, const void *b)
{
    double d = ((const ep_event *)a)->jd - ((const ep_event *)b)->jd;
    return d < 0 ? -1 : d > 0;
}
static int ev_same(const ep_event *a, const ep_event *b)
{
    return a->kind == b->kind && a->a == b->a && a->b == b->b && a->mask == b->mask && fabs(a->jd - b->jd) < 1e-7;
}
static double events_window_start(void) { return fmax(EP_JD_MIN + 1, fmin(EP_JD_MAX - 1, floor(S.jd) - 60)); }
static void events_begin(void)
{
    /* The extrema/refinement scan samples one day either side of its window. */
    S.scan_start = events_window_start();
    int days = (int)fmin(460, floor(EP_JD_MAX - 1 - S.scan_start));
    ep_scan_begin(&S.scan, S.scan_start, days, 2, S.ev, NEV);
    S.scanning = 1; S.nev = 0; S.snapped = -1; S.chip_ev = -2;
    S.events_generation++;
}
static void events_scan_step(void)
{
    if (!S.scanning) return;
    ep_event selected;
    int preserve = S.snapped >= 0 && S.snapped < S.nev;
    if (preserve) selected = S.ev[S.snapped];
    int done = ep_scan_step(&S.scan, 4);
    if (!done && S.scan.n != S.nev) qsort(S.ev, (size_t)S.scan.n, sizeof S.ev[0], ev_cmp);
    if (S.scan.n != S.nev || done) {
        S.nev = S.scan.n; S.chip_ev = -2; S.events_generation++;
        S.snapped = -1;
        if (preserve) for (int i = 0; i < S.nev; i++) if (ev_same(&selected, &S.ev[i])) { S.snapped = i; break; }
    }
    if (done) S.scanning = 0;
}
static void events_update(void)
{
    /* An already-running scan used to ignore the start guard below. It did
     * four sky solves and 252 pair-angle tests per tick while scrubbing.
     * Keep existing magnets, resume the same scan after motion/settling. */
    if(S.time_touch || S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0 ||
       S.belt_left || S.small_left) return;
    /* the window follows the face's time once it settles: [t - 60, t + 400] days */
    int settled = !S.drag && S.vel == 0 && !S.ta_n;
    if (!S.scanning && settled && (S.scan_start == 0 ||
        ((S.jd < S.scan_start + 30 || S.jd > S.scan_start + 400) && S.scan_start != events_window_start()))) {
        events_begin();
    }
    events_scan_step();
}
static int active_event(void)
{
    if (S.snapped >= 0 && S.snapped < S.nev) return S.snapped;
    for (int i = 0; i < S.nev; i++) if (S.ev[i].jd >= S.jd - 1.5 && S.ev[i].jd - S.jd < 60) return i;
    return -1;
}
static int next_event(void)
{
    for (int i = 0; i < S.nev; i++) if (S.ev[i].jd >= S.jd) return i;
    return -1;
}
/* magnets: while time moves, an event within ~2 degrees of the grabbed planet's travel holds it */
static void snap_check(int dragging)
{
    double win = ep_period_days[S.grab] * 2.2 / 360.0;
    int hit = -1;
    for (int i = 0; i < S.nev; i++) if (fabs(S.ev[i].jd - S.jd) < win) { hit = i; break; }
    if (hit >= 0 && hit != S.snapped) {
        S.snapped = hit; S.off = S.ev[hit].jd - S.jd_now; S.jd = S.ev[hit].jd;
        S.pulse = 8; nos_feedback(FB_TOGGLE_ON);
        if (!dragging) S.vel = 0;
    } else if (hit < 0) S.snapped = -1;
}

/* ---------------------------------------------------------------------------- labels */
static int lab_free(float x, float y, float w, float h)
{
    float x0 = x - w / 2, x1 = x + w / 2;
    if (orr_hypot(fmaxf(fabsf(x0 - CX), fabsf(x1 - CX)), fabsf(y - CY) + h) > 226) return 0;
    for (int i = 0; i < S.nused; i++) {
        rect_t *r = &S.used[i];
        if (fabsf(r->x - x) * 2 < r->w + w && fabsf(r->y - y) < h + 2) return 0;
    }
    if (S.nused < NLAB + 8) { S.used[S.nused].x = x; S.used[S.nused].y = y; S.used[S.nused].w = w; S.nused++; }
    return 1;
}
static void lab_add(float x, float y, const char *s, uint16_t col, int big)
{
    if (S.nlab >= NLAB) return;
    float w = (float)ui_text_width(UI_F_SMALL, s, 1) + 4;
    if (!lab_free(x, y, w, 13)) return;
    label_t *l = &S.lab[S.nlab++];
    l->x = x; l->y = y; snprintf(l->s, sizeof l->s, "%s", s); l->col = col; l->align = UI_CENTER; l->big = (uint8_t)big;
}
static void reserve(float x, float y, float w, float h) { if (S.nused < NLAB + 8) { S.used[S.nused].x = x; S.used[S.nused].y = y; S.used[S.nused].w = w; S.nused++; } (void)h; }

/* ---------------------------------------------------------------------------- layout (tick) */
static unsigned orr_orbit_key(void);
static void layout(void)
{
    unsigned gkey=orr_orbit_key();
    int new_orbit=!S.orbit_geom_valid || S.orbit_geom_key!=gkey;
    S.sc = 1 - ORB_SHRINK * S.ringe;
    float tl = S.cam_tilt * quad(S.tiltk) * D2RF;
    S.ct = fk_cos(tl); S.st = fk_sin(tl); S.ca = fk_cos(S.az * D2RF); S.sa = fk_sin(S.az * D2RF);
    float mixI = zone_mix(V_INNER), mixO = zone_mix(V_OUTER), mixB = zone_mix(V_BEYOND), mixF = zone_mix(V_FACE);
    radius_cache(S.view);
    if(S.from>=0) radius_cache(S.from);
    for (int p = 0; p < EP_N; p++) {
        if(new_orbit || S.panx!=S.layout_panx || S.pany!=S.layout_pany)
        for (int i = 0; i < NORB; i++) {
            float *d=S.orbit_shift[p][i];
            if(new_orbit) {
                float r=cached_radius(S.orbit_radius[S.view][p][i],S.from>=0?S.orbit_radius[S.from][p][i]:0);
                const pp *q=&S.orb[p][i]; float rs=r*S.sc*S.zoom;
                float sl=q->s*S.ca+q->c*S.sa, cl=q->c*S.ca-q->s*S.sa;
                d[0]=rs*sl; d[1]=rs*cl*S.ct; d[2]=q->zr*rs*S.st;
            }
            S.ox[p][i]=(CX+S.panx)-d[0];
            S.oy[p][i]=((CY+S.pany)-d[1])-d[2];
        }
        proj(&S.pl[p], &S.px[p], &S.py[p]);
        float base = BODY_R[p];
        float k = p <= EP_MARS ? 1 + 0.45f * mixI - 0.4f * mixO - 0.5f * mixB : 1 + 0.25f * mixO - 0.35f * mixI - 0.3f * mixB;
        S.pr[p] = fmaxf(1.5f, base * k);
        int off = (p > EP_MARS && mixI > 0.5f) || (p <= EP_MARS && mixB > 0.5f);
        S.pl_on[p] = !off && (S.show[0] || p == EP_EARTH);
    }
    S.orbit_geom_key=gkey; S.orbit_geom_valid=1;
    float plr = proj(&S.pluto, &S.plx, &S.ply);
    S.pluto_on = S.show[0] && mixB + mixO > 0.3f && plr < 225;
    S.sunx = CX + S.panx; S.suny = CY + S.pany;
    for (int p = 0; p < EP_N; p++) {
        float dx=S.sunx-S.px[p],dy=S.suny-S.py[p],d=orr_hypot(dx,dy);
        S.litux[p]=d>0?dx/d:0; S.lituy[p]=d>0?dy/d:0;
    }
    /* Only the changed bodies need radius mapping and projection. A geometry,
     * visibility or pan change always projects the complete belt. */
    int partial=S.belt_partial && S.belt_projected+1==S.belt_version &&
        !new_orbit && S.panx==S.layout_panx && S.pany==S.layout_pany &&
        S.list && S.zoom==S.list->dot_zoom && S.sc==S.list->dot_shrink &&
        S.panx==S.list->dot_panx && S.pany==S.list->dot_pany && S.belt_visible==S.show[1] &&
        S.layout_valid;
    S.dot_slice=partial;
    int count=partial?S.belt_count:S.nbelt;
    /* Board slot 0: 10.45 ms for 3999 projected bodies + fixed work. */
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,250000+(S.dot_preview?0:count*2550));
    if(!S.dot_preview) for (int j = 0; j < count; j++) {
        int k=partial?S.belt_changed[j]:j;
        pp q = S.bp[k];   /* normalisation is cached with the Kepler solve */
        float r=S.belt_radius[S.view] && (S.from<0 || S.belt_radius[S.from]) ?
            cached_radius(S.belt_radius[S.view][k],S.from>=0?S.belt_radius[S.from][k]:0) : view_r(q.au);
        proj_radius(&q,r,&S.bxs[k],&S.bys[k]);
        int kbo = q.au > 20;
        if (r > 223 || !S.show[1] || (!kbo && mixB > 0.98f)) S.bxs[k] = NAN;
    }
    if(!S.dot_preview) { S.belt_projected=S.belt_version; S.belt_visible=S.show[1]; }
    for (int i = 0; i < S.nsm; i++) {
        const od_small *o = &od_small_bodies[i];
        int comet = o->kind == OD_COMET || o->kind == OD_INTERSTELLAR, pluto = strstr(o->name, "Pluto") != 0;
        float r = proj(&S.sm[i], &S.smx[i], &S.smy[i]);
        S.sm_on[i] = !pluto && r <= 225 && (comet ? S.show[2] : S.show[1]);
    }
    for (int i = 0; i < S.ncm; i++) {
        float r = proj(&S.cm[i], &S.cmx[i], &S.cmy[i]);
        S.cm_on[i] = S.show[2] && od_comets[i].m1 < 16 && r <= 223;
        float L = fminf(20, 9 / fmaxf(S.cm[i].au, 0.3f)), dx = S.cmx[i] - S.sunx, dy = S.cmy[i] - S.suny, d = orr_hypot(dx, dy);
        S.tailx[i] = S.cmx[i] + (d > 0 ? dx / d * L : 0); S.taily[i] = S.cmy[i] + (d > 0 ? dy / d * L : 0);
    }
    for (int i = 0; i < S.ncr; i++) {
        float r = proj(&S.cr[i], &S.crx[i], &S.cry[i]);
        const od_craft *c = &od_crafts[i];
        float de = orr_hypot((float)(c->xyz[0] - S.sky.earth.x), (float)(c->xyz[1] - S.sky.earth.y));
        S.cr_on[i] = S.show[3] && c->planet < 0 && mixF < 0.5f && r <= 225 && (mixI > 0.5f || S.cr[i].au > 3) && de > 0.03f;
    }
    /* motion blur: each planet's run of orbit points between last frame's anomaly and now */
    /* only while time really moves (> 6 h a frame): the span is this frame's own travel */
    double dj = S.jd_prev_frame ? S.jd - S.jd_prev_frame : 0;
    unsigned anomaly_mask=0;
    uint32_t anomaly_stamp=orr_prof.enabled?orr_prof_now():0;
    for (int p = 0; p < EP_N; p++) {
        S.blur_n[p] = 0;
        if (fabs(dj) < 0.25 || !S.pl_on[p]) continue;
        S.E_prev[p] = S.sky_E_prev_jd==S.jd_prev_frame?S.sky_E_prev[p]:
            S.anomaly_jd==S.jd_prev_frame && (S.anomaly_mask&(1u<<p)) ?
            S.E_now[p] : ep_ecc_anomaly(p,S.jd_prev_frame);
        S.E_now[p] = S.jd_sky==S.jd?S.sky_E[p]:ep_ecc_anomaly(p, S.jd);
        anomaly_mask|=1u<<p;
        double span = S.E_now[p] - S.E_prev[p];
        if (dj > 0 && span < 0) span += 2 * 3.14159265358979;
        if (dj < 0 && span > 0) span -= 2 * 3.14159265358979;
        if (fabs(dj) >= ep_period_days[p]) span = (dj > 0 ? 1 : -1) * 2 * 3.14159265358979 * 0.999;
        float rad = fmaxf(20, orr_hypot(S.px[p] - S.sunx, S.py[p] - S.suny)), arcpx = (float)fabs(span) * rad;
        if (arcpx < 3) continue;
        if (arcpx > 240) { span *= 240 / arcpx; arcpx = 240; }   /* a longer smear reads the same and costs more */
        int steps = (int)fminf(NORB, fmaxf(3, arcpx / 4));
        for (int k = 0; k <= steps; k++) {
            double E = S.E_now[p] - span * (1.0 - (double)k / steps);       /* tail first */
            double fi = E / (2 * 3.14159265358979) * NORB;
            fi -= (long)(fi / NORB) * NORB; if (fi < 0) fi += NORB;
            int i0 = (int)fi, i1 = (i0 + 1) % NORB; float f = (float)(fi - i0);
            S.blur_x[p][k] = S.ox[p][i0] + (S.ox[p][i1] - S.ox[p][i0]) * f;
            S.blur_y[p][k] = S.oy[p][i0] + (S.oy[p][i1] - S.oy[p][i0]) * f;
        }
        S.blur_x[p][steps] = S.px[p]; S.blur_y[p][steps] = S.py[p];
        S.blur_n[p] = steps + 1;
    }
    S.anomaly_jd=S.jd;S.anomaly_mask=anomaly_mask;
    if(orr_prof.enabled)orr_prof.detail_us[4]+=orr_prof_now()-anomaly_stamp;

}
static void labels_layout(void)
{
    S.label_builds++;
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,200000);
    float mixI = zone_mix(V_INNER), mixO = zone_mix(V_OUTER), mixB = zone_mix(V_BEYOND), mixF = zone_mix(V_FACE);
    /* labels: planets first, then small bodies; reserved: the top time, the chip, the read-out */
    S.nlab = 0; S.nused = 0;
    if (mixF < 0.99f) reserve(CX, 22, 100, 14);

    reserve(CX, CHIP_Y, S.chip_w + 10, 16);
    for (int p = 0; p < EP_N; p++) {
        if (!S.pl_on[p]) continue;
        int show = (p <= EP_MARS && mixI > 0.6f) || (p > EP_MARS && (mixO > 0.6f || mixB > 0.6f));
        if (!show) continue;
        float r = orr_hypot(S.px[p] - S.sunx, S.py[p] - S.suny), ux = r > 0 ? (S.px[p] - S.sunx) / r : 0, uy = r > 0 ? (S.py[p] - S.suny) / r : 0;
        float o = r > 170 ? -1.0f : 1.0f, d0 = S.pr[p] + 16, lx = S.px[p] + ux * d0 * o, ly = S.py[p] + uy * d0 * o - (uy * o < 0 ? 6 : 0);
        char nm[16], ds[16];
        snprintf(nm, sizeof nm, "%s", ep_name[p]);
        if (p == EP_EARTH) snprintf(ds, sizeof ds, "YOU");
        else snprintf(ds, sizeof ds, p <= EP_MARS ? "%.2f AU" : "%.1f AU", S.sky.b[p].dist);
        int before = S.nlab;
        lab_add(lx, ly, nm, C_PRI, 1);
        if (S.nlab > before) lab_add(lx, ly + 12, ds, C_DIS, 0);
    }
    if (S.pluto_on && (mixO > 0.6f || mixB > 0.6f)) lab_add(S.plx, S.ply - 14, "PLUTO", C_PRI, 1);
    if (mixI > 0.5f && S.show[0]) for (int p = EP_JUPITER; p < EP_N; p++) {   /* the outer planets' rim ticks */
        float s = S.sc * S.zoom, sl = S.pl[p].s * S.ca + S.pl[p].c * S.sa, cl = S.pl[p].c * S.ca - S.pl[p].s * S.sa;
        char t[4]; snprintf(t, sizeof t, "%.3s", ep_name[p]);
        lab_add(CX + S.panx - 206 * s * sl, CY + S.pany - 206 * s * cl * S.ct, t, C_DIS, 0);
    }
    for (int i = 0; i < S.nsm; i++) {
        if (!S.sm_on[i]) continue;
        const od_small *o = &od_small_bodies[i];
        int big = o->kind == OD_DWARF || strstr(o->name, "Vesta") || strstr(o->name, "Pallas");
        if ((mixI > 0.9f && S.sm[i].au < 3.5f && big) || (mixB > 0.9f && S.sm[i].au > 20)) {
            const char *s = o->short_name; while (*s >= '0' && *s <= '9') s++; while (*s == ' ' || *s == '(') s++;
            char t[20]; snprintf(t, sizeof t, "%s", s);
            for (char *q = t; *q; q++) { if (*q == ')') *q = 0; else if (*q >= 'a' && *q <= 'z') *q -= 32; }
            lab_add(S.smx[i], S.smy[i] - 9, t, C_DIS, 0);
        }
    }
    if (mixB > 0.9f) for (int i = 0; i < S.ncr; i++) if (S.cr_on[i] && S.cr[i].au > 20) {
        char t[20]; snprintf(t, sizeof t, "%s", od_crafts[i].name);
        for (char *q = t; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        lab_add(S.crx[i] + (S.crx[i] > CX ? -30 : 30), S.cry[i] + 1, t, C_DIS, 0);
    }
}

/* time rings: tick angles and labels of the three dials for the face's time */
static double ring_layout_jd;
static void rings_layout(void)
{
    /* Ring labels only need minute resolution while resting. Recomputing their
     * subpixel positions every frame defeated both damage stability and idle. */
    ring_layout_jd = (S.drag == 4 || S.drag == 5) || S.ta_n || S.vel != 0 ? S.jd : floor(S.jd * 1440.0) / 1440.0;
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,80000);
    civ n = civil(ring_layout_jd);
    int keys[3]={n.y,n.y*12+n.m,(int)floor(ring_layout_jd+S.tz_days+0.5)};
    int rebuild[3];
    int moving=S.time_touch || S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0;
    S.rings_motion=moving;
    for(int i=0;i<3;i++) {
        /* Remember the legacy calendar-entry origin even while retaining a
         * larger preview window. Returning to it preserves AA rounding at
         * rest byte-for-byte, rather than choosing a new floating origin. */
        if(!S.nrl[i] || S.ring_exact_calendar[i]!=keys[i] || S.rings_tz!=S.tz_days) {
            S.ring_exact_calendar[i]=keys[i];S.ring_exact_jd[i]=ring_layout_jd;
        }
        /* A finger turns one dial. Preserve the other two until release;
         * still track canonical calendar-entry origins for exact settling. */
        if(S.drag==4 && i!=S.d_ring && S.nrl[i] && S.rings_tz==S.tz_days) {
            rebuild[i]=0;continue;
        }
        rebuild[i]=!S.nrl[i] || S.ring_calendar[i]!=keys[i] || S.rings_tz!=S.tz_days;
        float delta=(float)((S.ring_base_jd[i]-ring_layout_jd)/RING_LAP[i]*360);
        /* A calendar boundary does not change the visible geometry. Keep the
         * polar window until its guard ticks approach the dock cutout. Rebase
         * at rest so canonical dates/hit targets are independent of history. */
        if(moving && S.nrl[i] &&
           S.nrt[i] && S.rings_tz==S.tz_days &&
           S.rt[i][0]+delta < -140 && S.rt[i][S.nrt[i]-1]+delta > 140)
            rebuild[i]=0;
        if(!moving && S.ring_base_jd[i]!=S.ring_exact_jd[i])rebuild[i]=1;
        if(rebuild[i]) { S.ring_builds++; GFX_COST_PX(GFX_COST_ORR_PRE_CPU,120000); S.nrl[i]=S.nrt[i]=0; S.ring_calendar[i]=keys[i]; S.ring_base_jd[i]=S.ring_exact_jd[i]; }
        S.ring_delta[i]=(float)((S.ring_base_jd[i]-ring_layout_jd)/RING_LAP[i]*360);
        S.hot[i]=-1;
    }
    #define AT(i, J) ((float)(((J) - S.ring_base_jd[i]) / RING_LAP[i] * 360.0))
    #define ADDL(i, phi_, str, maj) do { if (S.nrl[i] < 40) { S.rl[i][S.nrl[i]].phi = (phi_); snprintf(S.rl[i][S.nrl[i]].s, 6, "%s", str); \
        S.rl[i][S.nrl[i]].major = (uint8_t)(maj); S.nrl[i]++; } } while (0)
    #define ADDT(i, phi_, maj) do { if (S.nrt[i] < 200) { S.rt[i][S.nrt[i]] = (phi_); S.rtm[i][S.nrt[i]] = (uint8_t)(maj); S.nrt[i]++; } } while (0)
    char t[8];
    if(rebuild[0]) for (int Y = n.y - 7; Y <= n.y + 7; Y++) {
        snprintf(t, sizeof t, "%d", Y);
        ADDT(0, AT(0, jd_local(Y, 1, 1, 0)), 1);
        ADDL(0, AT(0, jd_local(Y, 7, 2, 0)), t, 1);
        for (int m = 2; m <= 12; m++) ADDT(0, AT(0, jd_local(Y, m, 1, 0)), 0);
    }
    if(rebuild[1]) for (int k = -7; k <= 7; k++) {
        int M = n.m + k;
        ADDT(1, AT(1, jd_local(n.y, M, 1, 0)), 1);
        ADDL(1, AT(1, jd_local(n.y, M, 15, 0)), MON[((M - 1) % 12 + 12) % 12], 1);
        ADDT(1, AT(1, jd_local(n.y, M, 8, 0)), 0); ADDT(1, AT(1, jd_local(n.y, M, 22, 0)), 0);
    }
    if(rebuild[2]) for (int k = -17; k <= 17; k++) {
        double j0 = jd_local(n.y, n.m, n.d + k, 0);
        civ c = civil(j0 + 0.5);
        ADDT(2, AT(2, j0), c.d == 1);
        if (c.d == 1) ADDL(2, AT(2, j0 + 0.5), MON[c.m - 1], 1);
        else { snprintf(t, sizeof t, "%d", c.d); ADDL(2, AT(2, j0 + 0.5), t, 0); }
    }
    for(int i=0;i<3;i++) if(rebuild[i]) {
        for(int j=0;j<S.nrl[i];j++) {
            S.rl[i][j].sn=fk_sin(S.rl[i][j].phi*D2RF);
            S.rl[i][j].cs=fk_cos(S.rl[i][j].phi*D2RF);
            S.rl[i][j].width=ui_text_width(UI_F_SMALL,S.rl[i][j].s,1);
        }
        for(int j=0;j<S.nrt[i];j++) {
            S.rt_basis[i][j][0]=fk_sin(S.rt[i][j]*D2RF);
            S.rt_basis[i][j][1]=fk_cos(S.rt[i][j]*D2RF);
        }
    }
    for (int i = 0; i < 3; i++) {
        if(S.drag==4 && i!=S.d_ring)continue;
        for (int j = 0; j < S.nrl[i]; j++) if (S.hot[i] < 0 || fabsf(S.rl[i][j].phi+S.ring_delta[i]) < fabsf(S.rl[i][S.hot[i]].phi+S.ring_delta[i])) S.hot[i] = j;
    }
    #undef AT
    #undef ADDL
    #undef ADDT
    int days = (int)floor(S.off + 0.5);
    snprintf(S.readout, sizeof S.readout, "%02d.%02d.%02d", n.d, n.m, n.y % 100);
    if (fabs(S.off) > 0.02) snprintf(S.readout2, sizeof S.readout2, "%s %02d:%02d · %+d D", DOW[n.wd], n.h, n.mi, days);
    else snprintf(S.readout2, sizeof S.readout2, "%s %02d:%02d · NOW", DOW[n.wd], n.h, n.mi);
    snprintf(S.ring_time, sizeof S.ring_time, "%s %02d:%02d", DOW[n.wd], n.h, n.mi);
    snprintf(S.ring_clock, sizeof S.ring_clock, "%02d:%02d", n.h, n.mi);
    snprintf(S.ring_date, sizeof S.ring_date, "%02d %s %04d", n.d, MON[n.m - 1], n.y);
    if (S.jd <= EP_JD_MIN || S.jd >= EP_JD_MAX) snprintf(S.ring_state, sizeof S.ring_state, "[1800-2050 LIMIT]");
    else if (fabs(S.off) > 0.02) snprintf(S.ring_state, sizeof S.ring_state, "[%+d D]", days);
    else snprintf(S.ring_state, sizeof S.ring_state, "[NOW]");
    snprintf(S.ring_return, sizeof S.ring_return, "%+d D  NOW", days);
    S.ring_return_width=ui_text_width(UI_F_SMALL,S.ring_return,-1);
    S.ring_return_dot=S.ring_return_width-ui_text_width(UI_F_SMALL," NOW",-1)-4;
}

/* menu buttons and their options for this frame's slide (hit list = draw list) */
static int menu_opt_count(int m) { return m == M_DRAG ? 4 : m == M_VIEW ? 2 : m == M_FILTER ? 5 : 0; }
static void menu_layout(void)
{
    S.nmh = 0;
    if (S.menue < 0.01f) return;
    for (int i = 0; i < 4; i++) {
        float k = quad(clampf(S.menuk * 1.25f - i * 0.08f, 0, 1)), x, y;
        polar_xy(MENU_PHI[i], MENU_R + (1 - k) * 70, &x, &y);
        S.mhit[S.nmh].x = x; S.mhit[S.nmh].y = y; S.mhit[S.nmh].id = i; S.mhit[S.nmh].key = -1; S.mhit[S.nmh].kind = k > 0.9f ? 1 : 0; S.nmh++;
        if (S.menu_sub == i && S.sube > 0.01f) {
            int n = menu_opt_count(i);
            float mid = fminf(MENU_PHI[i], OPT_MAX - (n - 1) / 2.0f * OPT_STEP);   /* the fan never reaches the chip */
            for (int j = 0; j < n; j++) {
                float fphi = mid + (j - (n - 1) / 2.0f) * OPT_STEP, op = MENU_PHI[i] + (fphi - MENU_PHI[i]) * S.sube;
                polar_xy(op, MENU_R + (OPT_R - MENU_R) * S.sube, &x, &y);
                S.mhit[S.nmh].x = x; S.mhit[S.nmh].y = y; S.mhit[S.nmh].id = i; S.mhit[S.nmh].key = j; S.mhit[S.nmh].kind = S.sube > 0.9f ? 2 : 0; S.nmh++;
            }
        }
    }
}
static int menu_opt_on(int m, int j)
{
    switch (m) {
    case M_DRAG: return j < 3 && S.drag_mode == (j == 0 ? DR_PAN : j == 1 ? DR_LOOK : DR_TIME);
    case M_VIEW: return (j == 1) == !!S.tilt;
    case M_FILTER: return j < 4 ? S.show[j] : S.from_earth;
    }
    return 0;
}

/* ---------------------------------------------------------------------------- tick */
static double unix_now(const face_ctx *c)
{
    uint64_t e = hal_epoch_seconds();
    if (e) return (double)e + c->dt.millis / 1000.0;
    double loc = (jdn_of(c->dt.year, c->dt.month, c->dt.day) - 2440588.0) * 86400.0 + c->dt.hour * 3600.0 + c->dt.minute * 60.0 + c->dt.second;
    return loc;
}
static void lock_now(int locked)
{
    if (S.locked == locked) return;
    S.locked = locked;
    S.drag = 0; S.time_touch=0; S.pvx = S.pvy = 0; S.vel = 0;
    S.menu_pull = S.rings_pull = 0; s_edge = 0;
    if (locked) {
        S.menu_open = 0; S.rings_open = 0;
        if (S.panx || S.pany || S.zoom != 1) { S.home_t = 1; S.home_px = S.panx; S.home_py = S.pany; S.home_z = S.zoom; }
        if (S.view != V_FACE) { S.from = S.view; S.view = V_FACE; S.tx = 0; }
        if (fabs(S.off) > 0.02) { S.ta_from = S.off; S.ta_to = 0; S.ta_t = 0; S.ta_n = 16; }
    }
    nos_feedback(locked ? FB_TOGGLE_OFF : FB_TOGGLE_ON);
    nos_toast(locked ? "LOCKED" : "UNLOCKED");
}
static void go_view(int v) { if (v == S.view) return; S.from = S.view; S.view = v; S.tx = 0; nos_feedback(FB_TICK); }
static void step(float *k, int target, float frames) { float t = (float)target; if (*k != t) { *k += (t > *k ? 1 : -1) / frames; if (fabsf(*k - t) < 0.5f / frames) *k = t; } }

static void enter(void) { if (orr_ok()) { S.jd_prev_frame = 0; S.time_touch=0; s_edge=0; } }

static void scene_update(const face_ctx *c);
static void orr_snap_tick(const face_ctx *c);
static void orr_band_prepare(void);
static void orr_plate_prepare(void);

static void tick(const face_ctx *c, float dt)
{
    if (!orr_ok()) return;
    orr_prof_frame();
    orr_prof_scrub(S.drag==4);
#ifdef GFX_COST_MODEL
    gfx_cost_orr_math_scope(1);
#endif
    ORR_TIME_START(tupdate);
    (void)dt;
    double un = unix_now(c);
    /* Precise time remains available to the card via orrery_jd()/orrery_now(). */
    S.jd_now = ep_jd_unix(un);
    if (hal_epoch_seconds()) {
        double loc = (jdn_of(c->dt.year, c->dt.month, c->dt.day) - 2440588.0) * 86400.0 + c->dt.hour * 3600.0 + c->dt.minute * 60.0 + c->dt.second;
        S.tz_days = floor((loc - floor(un)) / 60.0 + 0.5) / 1440.0;
    }
    if (c->aod) {   /* sleep locks it, at home, at now */
        S.locked = 1; S.lockk = 1; S.hold = 0; S.drag = 0; S.time_touch=0; S.home_t = 0; S.panx = S.pany = 0; S.zoom = 1; S.pvx = S.pvy = 0;
        S.off = 0; S.vel = 0; S.ta_n = 0; S.view = V_FACE; S.from = -1; S.tx = 1; S.menu_open = S.rings_open = 0;
        S.menuk = S.menue = S.ringk = S.ringe = 0; S.tilt = 0; S.tiltk = 0; S.az = S.azv = 0;
        S.menu_pull = S.rings_pull = 0; s_edge = 0;
        S.subk = S.sube = 0; S.pulse = 0; S.jd_prev_frame = 0;
    }
    belt_init(c->aod);
    fk_aod_update(c);
    fk_unread_tick(c);

    /* time: an animated jump, or momentum, or still */
    if (S.ta_n) {
        S.ta_t++;
        S.off = S.ta_from + (S.ta_to - S.ta_from) * quad((float)S.ta_t / S.ta_n);
        if (S.ta_t >= S.ta_n) { S.off = S.ta_to; S.ta_n = 0; }
    } else if (!S.drag && S.vel != 0) {
        S.off += S.vel; S.vel *= 0.9;
        if (fabs(S.vel) < 1e-4) S.vel = 0;
        travel_limit();
        snap_check(0);
    }
    if (!S.drag && !S.ta_n && S.vel == 0 && fabs(S.off) < 0.02) S.off = 0;
    /* Keep astronomy/card time precise. Stationary cache keys below use minute
     * resolution, while sky positions only refresh at a visible-motion cadence. */
    travel_limit();
    /* Keep the home awake geometry warm at the normal ephemeris cadence. */
    sky_update(S.jd, 0);
    ORR_DETAIL(3,if (!c->aod) events_update());

    /* animations: controls fade 8 f, tilt 12 f, zone morph 10 f, rings 10 f, menu 9 f, options 8 f */
    step(&S.lockk, S.locked, 8);
    step(&S.tiltk, S.tilt, 12);
    if (S.from >= 0) { S.tx += 0.1f; if (S.tx >= 1) { S.tx = 1; S.from = -1; } }
    if (!S.rings_pull) step(&S.ringk, S.rings_open, 10);
    S.ringe = quad(S.ringk);
    step(&S.platek, fabs(S.off) > 0.02, 8);   /* travelled: the plate grows and shows a big NOW */
    S.platee = quad(S.platek);
    if (!S.menu_pull) step(&S.menuk, S.menu_open, 9);
    S.menue = quad(S.menuk);
    step(&S.subk, S.menu_sub >= 0 && S.menu_open, 8);
    S.sube = quad(S.subk);
    if (!S.menu_open && S.menuk == 0) S.menu_sub = -1;
    if (S.drag != 3) {   /* LOOK released: the azimuth coasts, and flat again it returns to 0 */
        S.az += S.azv; S.azv *= 0.9f; if (fabsf(S.azv) < 0.02f) S.azv = 0;
        if (!S.tilt) { S.az = n180(S.az) * 0.8f; if (fabsf(S.az) < 0.05f) S.az = 0; }
    }
    if (S.home_t) {
        float k = quad(S.home_t / 12.0f);
        S.panx = S.home_px * (1 - k); S.pany = S.home_py * (1 - k);
        S.zoom = expf(logf(S.home_z) * (1 - k));
        if (++S.home_t > 12) { S.home_t = 0; S.panx = S.pany = 0; S.zoom = 1; }
    }
    if (S.drag != 1 && (S.pvx != 0 || S.pvy != 0)) {
        float lim = 210 * S.zoom;
        S.panx = clampf(S.panx + S.pvx, -lim, lim); S.pany = clampf(S.pany + S.pvy, -lim, lim);
        S.pvx *= 0.88f; S.pvy *= 0.88f;
        if (fabsf(S.pvx) + fabsf(S.pvy) < 0.3f) S.pvx = S.pvy = 0;
    }
    if (S.hold) {
        /* a resting fingertip rolls: 12 px (the engine's drag slop) until the long press at
         * 0.45 s, then 40 px (after it the engine never makes the touch a drag); 8 px for the
         * whole 1.5 s made the unlock hold hit or miss on the watch */
        if (nos_touch_travel() > (nos_t - S.hold_t0 < 0.45f ? 12 : 40)) S.hold = 0;
        else {
            float h = nos_t - S.hold_t0;
            int st = h >= HOLD_PICK ? 3 : h >= HOLD_LOCK ? 2 : h >= HOLD_TILT ? 1 : 0;
            if (st > S.hold_stage) { S.hold_stage = st; nos_feedback(FB_TICK); }
            /* Warm the unlocked controls while the finger rests (first unlock after boot
             * used to build them on the unlock frame). Both return at once when built. */
            if (h >= 0.1f && S.locked) { orr_band_prepare(); orr_plate_prepare(); }
            if (st == 3) { S.hold = 0; home_open_faces(); }
        }
    }
    if (S.pulse > 0) S.pulse--;

    /* the chip: the active event, or NOW once time has travelled */
    S.now_pill = fabs(S.off) > 0.02 && !(S.drag == 4 || S.drag == 5);
    int ai = active_event();
    if (ai < 0) ai = next_event();
    if (ai != S.chip_ev || ((int)floor(S.jd) != S.month_key)) {
        S.chip_ev = ai; S.chip[0] = 0;
        if (ai >= 0) {
            char t[32]; ep_event_title(&S.ev[ai], t, sizeof t);
            int d = (int)floor(S.ev[ai].jd - S.jd + 0.5);
            if (d == 0) snprintf(S.chip, sizeof S.chip, "%s · TODAY", t);
            else if (d > 0) snprintf(S.chip, sizeof S.chip, "%s · IN %d D", t, d);
            else snprintf(S.chip, sizeof S.chip, "%s · %d D AGO", t, -d);
        }
        S.chip_w = S.chip[0] ? (float)ui_text_width(UI_F_SMALL, S.chip, 1) + 34 : 0;
        S.month_key = (int)floor(S.jd);
    }

    ORR_TIME_END(0,tupdate);
    scene_update(c);
    orr_snap_tick(c);
    orr_prof_context(c);
    S.jd_prev_frame = S.jd;
#ifdef GFX_COST_MODEL
    gfx_cost_orr_math_scope(0);
#endif
}

/* ---------------------------------------------------------------------------- draw */
/* one damage command for a group of marks: record mode records it, raster mode draws */
static int grp(gfx_fb *fb, const fk_view *v, float x0, float y0, float x1, float y1, unsigned h)
{
    if (fb->rec) { gfx_rec_cmd(fb, (int)floorf(x0) - 2, (int)floorf(y0) - 2, (int)ceilf(x1) + 2, (int)ceilf(y1) + 2, h); return 0; }
    return fk_hit(v, x0 - 2, y0 - 2, x1 + 2, y1 + 2);
}
/* A 1 px anti-aliased line (Wu): two blended pixels per step, straight into the stripe. The
 * orbits are ~7500 px of hairline redrawn on every full repaint (time travel, zones); as AA
 * gfx_line segments they cost ~14 ms a frame on the watch, this way ~3 ms. Raster mode only
 * (callers record their own damage); priced as blend pixels for the sim's cost model. */
/* Board-informed proxy: one visited list item/setup costs 1 us (240 cycles).
 * Counts work even when culled; raster pixels/calls retain their existing prices.
 * Only this face opts in. Cache construction switches to the separate build class. */
static int orr_cost_class = GFX_COST_ORR_WALK;
#define ORR_VISIT(n) GFX_COST_PX(orr_cost_class, (n))
#include "face_orrery_runs.h"
#include "face_orrery_draw.h"
#include "face_orrery_snap.h"
static unsigned s_hl_px;
static void hl_plot(gfx_fb *fb, int x, int y, uint16_t col, float a)
{
    int bx = x + fb->ox, by = y + fb->oy - fb->y_base;
    if ((unsigned)bx >= GFX_W || (unsigned)by >= (unsigned)fb->h || a <= 0.02f) return;
    int c0 = fb->cx0, c1 = fb->cx1; if (c1 <= c0) { c0 = 0; c1 = GFX_W; }
    if (bx < c0 || bx >= c1) return;
    uint16_t *p = &gfx_row(fb, by)[bx];
    *p = a >= 0.98f ? col : gfx_blend565(*p, col, (uint8_t)(a * 255));
    s_hl_px++;
}
static void hairline(gfx_fb *fb, const fk_view *v, float x0, float y0, float x1, float y1, uint16_t col)
{
    if (orr_capture) { orr_line_item(B_HAIR, x0, y0, x1, y1, 1, col); return; }
    ORR_VISIT(1);
    if (!fk_hit(v, fminf(x0, x1) - 1, fminf(y0, y1) - 1, fmaxf(x0, x1) + 2, fmaxf(y0, y1) + 2)) return;
    float dx = x1 - x0, dy = y1 - y0;
    if (fabsf(dx) >= fabsf(dy)) {
        if (x0 > x1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dy = -dy; dx = -dx; }
        float g = dx != 0 ? dy / dx : 0;
        for (int x = (int)ceilf(fmaxf(x0,v->x0-1)); x <= (int)floorf(fminf(x1,v->x1)); x++) {
            float y = y0 + g * (x - x0); int yi = (int)floorf(y); float f = y - yi;
            if (yi + 1 < v->y0 || yi > v->y1) continue;
            hl_plot(fb, x, yi, col, 1 - f); hl_plot(fb, x, yi + 1, col, f);
        }
    } else {
        if (y0 > y1) { float t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dy = -dy; dx = -dx; }
        float g = dx / dy;
        int ya = (int)ceilf(fmaxf(y0, v->y0)), yb = (int)floorf(fminf(y1, v->y1));
        for (int y = ya; y <= yb; y++) {
            float x = x0 + g * (y - y0); int xi = (int)floorf(x); float f = x - xi;
            hl_plot(fb, xi, y, col, 1 - f); hl_plot(fb, xi + 1, y, col, f);
        }
    }
}
static void hl_begin(void) { s_hl_px = 0; }
static void hl_end(void) { GFX_COST(GFX_COST_BLEND, s_hl_px); }
static void seg(gfx_fb *fb, const fk_view *v, float ax, float ay, float bx, float by, float w, uint16_t col)
{
    if (!fk_hit(v, fminf(ax, bx) - w, fminf(ay, by) - w, fmaxf(ax, bx) + w, fmaxf(ay, by) + w)) return;
    if (w <= 1.25f && !fb->rec) { hl_begin(); hairline(fb, v, ax, ay, bx, by, gfx_ink(fb, col)); hl_end(); }
    else gfx_line(fb, ax, ay, bx, by, w, col);
}
static void dashed(gfx_fb *fb, const fk_view *v, float ax, float ay, float bx, float by, float w, uint16_t col)
{
    unsigned h = gfx_hash_u(0xDA51u, col);
    h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(h, ax), ay), bx), by), w);
    if (!grp(fb, v, fminf(ax, bx) - w, fminf(ay, by) - w,
             fmaxf(ax, bx) + w, fmaxf(ay, by) + w, h)) return;
    float L = orr_hypot(bx - ax, by - ay); int n = (int)(L / 6);
    for (int i = 0; i < n; i += 2) {
        float t0 = (float)i / n, t1 = (float)(i + 1) / n;
        seg(fb, v, ax + (bx - ax) * t0, ay + (by - ay) * t0, ax + (bx - ax) * t1, ay + (by - ay) * t1, w, col);
    }
}
static void ellipse_line(gfx_fb *fb, const fk_view *v, float cx, float cy, float rx, float ry, uint16_t col, int dash)
{
    /* A guide ellipse is one visual object, not 48 independent damage entries.
     * The beyond-Neptune view otherwise overflowed the recorder even at rest. */
    unsigned h = gfx_hash_u(gfx_hash_u(0xE111u, col), (unsigned)dash);
    h = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(h, cx), cy), rx), ry);
    if (!grp(fb, v, cx - rx, cy - ry, cx + rx, cy + ry, h)) return;
    float px = cx + rx, py = cy;
    for (int i = 1; i <= 48; i++) {
        float a = i / 48.0f * 2 * PI_F, x = cx + rx * fk_cos(a), y = cy + ry * fk_sin(a);
        if (!dash || (i & 1)) seg(fb, v, px, py, x, y, 1, col);
        px = x; py = y;
    }
}
#define OCH 12
static void draw_orbits_direct(gfx_fb *fb, const fk_view *v)
{
    for (int p = 0; p < EP_N; p++) {
        if (!S.pl_on[p] && p != EP_EARTH) continue;
        if ((p > EP_MARS && zone_mix(V_INNER) > 0.5f) || (p <= EP_MARS && zone_mix(V_BEYOND) > 0.5f)) continue;
        int hot = (p <= EP_MARS && zone_mix(V_INNER) > 0.5f) || (p > EP_MARS && zone_mix(V_OUTER) > 0.5f);
        uint16_t col = hot ? C_LINE2 : C_LINE;
        for (int i0 = 0; i0 < NORB; i0 += OCH) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            unsigned h = gfx_hash_u(0x0B17u, (unsigned)(p * 131 + i0 + col));
            for (int i = i0; i <= i0 + OCH && i <= NORB; i++) {
                GFX_COST_PX(orr_capture?GFX_COST_ORR_GEOMETRY:orr_cost_class,1);
                int j = i % NORB; float x = S.ox[p][j], y = S.oy[p][j];
                x0 = fminf(x0, x); y0 = fminf(y0, y); x1 = fmaxf(x1, x); y1 = fmaxf(y1, y);
                if (fb->rec) h = gfx_hash_f(gfx_hash_f(h, x), y);
            }
            if(orr_capture) {
                orr_cmd *q=orr_item(B_ORBIT_SEG,y0-1,y1+1,col);
                orr_xbox(q,x0-1,x1+1);
                if(q) { q->aux=(uint16_t)p; q->f[0]=(float)i0; q->f[1]=(float)(i0+OCH<NORB?i0+OCH:NORB); }
                continue;
            }
            if (!grp(fb, v, x0, y0, x1, y1, h)) continue;
            uint16_t ink = gfx_ink(fb, col);
            hl_begin();
            for (int i = i0; i < i0 + OCH && i < NORB; i++) { int j = (i + 1) % NORB; hairline(fb, v, S.ox[p][i], S.oy[p][i], S.ox[p][j], S.oy[p][j], ink); }
            hl_end();
        }
    }
}
static void draw_orbits(gfx_fb *fb,const fk_view *v)
{
    if(orr_capture && S.orbit_pixels && S.orbit_pixels->valid)
        orr_item(B_ORBIT,0,GFX_H-1,0);
    else if(!fb->rec && S.orbit_pixels && S.orbit_pixels->valid) {
        int lo=fb->y_base-fb->oy,hi=lo+fb->h;
        if(lo<0)lo=0;
        if(hi>GFX_H)hi=GFX_H;
        if(lo<hi)orr_orbits_replay(fb,lo,hi);
    } else draw_orbits_direct(fb,v);
}
static void belt_dot(int i, const void *ctx, float *x, float *y, float *d, uint16_t *col)
{
    (void)ctx;
    ORR_VISIT(1);
    *x = (S.list?CX+S.panx+(S.bxs[i]-CX-S.list->dot_panx)*S.zoom/S.list->dot_zoom:S.bxs[i]); *y = (S.list?CY+S.pany+(S.bys[i]-CY-S.list->dot_pany)*S.zoom/S.list->dot_zoom:S.bys[i]); *d = isnan(S.bxs[i]) ? 0 : 1.4f; *col = orr_belt_ink(i);
}
static void draw_belt(gfx_fb *fb, const fk_view *v)
{
    if (!S.bk || !S.show[1]) return;
    /* The former belt-streak branch never ran: tick had already advanced
     * jd_prev_frame before draw. Preserve those dot-only visuals. The visible
     * planetary motion trails are composed by draw_blur below. */
    if (orr_capture) orr_item(B_BELT,0,GFX_H-1,0);
    else if(fb->rec) fk_dots(fb,S.nbelt,64,belt_dot,0);
    else for(int i=0;i<S.nbelt;i++) {
        orr_pixel d;
        if(orr_pixel_make(&d,(S.list?CX+S.panx+(S.bxs[i]-CX-S.list->dot_panx)*S.zoom/S.list->dot_zoom:S.bxs[i]),(S.list?CY+S.pany+(S.bys[i]-CY-S.list->dot_pany)*S.zoom/S.list->dot_zoom:S.bys[i]),orr_belt_pixel_ink(i),S.bh[i]==0,0)) orr_pixel_draw(fb,&d);
    }
}
/* A planet lit from one side: the half facing the Sun (ux, uy = unit vector towards it) in its
 * tint, the night half dim, a straight terminator through the centre, both edges anti-aliased
 * over a pixel. Straight into the stripe like the hairline (the old version was a gfx_arc band,
 * whose round caps bulged over the terminator into the night side). */
static void lit_disc(gfx_fb *fb, const fk_view *v, float x, float y, float r, float ux, float uy, uint16_t ink)
{
    if (orr_capture) {
        orr_cmd *q = orr_item(B_LIT, y - r - 2, y + r + 2, ink);
        orr_xbox(q,x-r-2,x+r+2);
        if (q) { q->f[0] = x; q->f[1] = y; q->f[2] = r; q->f[3] = ux; q->f[4] = uy; }
        return;
    }
    if (!grp(fb, v, x - r, y - r, x + r, y + r, gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(0x117Du, ink), x), y), r), ux), uy))) return;
    uint16_t day = gfx_ink(fb, ink), night = gfx_ink(fb, ui_dim(ink, 0.3f));
    int ya = (int)fmaxf(floorf(y - r - 1), (float)v->y0), yb = (int)fminf(ceilf(y + r + 1), (float)v->y1);
    int xa = (int)floorf(x - r - 1), xb = (int)ceilf(x + r + 1);
    hl_begin();
    for (int py = ya; py <= yb; py++) {
        float dy = py + 0.5f - y;
        for (int px = xa; px <= xb; px++) {
            float dx = px + 0.5f - x, d2 = dx * dx + dy * dy;
            if (d2 > (r + 0.501f) * (r + 0.501f)) continue;
            /* Only the one-pixel limb needs distance. Keep the existing fast
             * rsqrt coverage there, byte-identical including RGB565 rounding. */
            float cov = d2 < (r-0.5f)*(r-0.5f) ? 1 :
                        fminf(1, r + 0.5f - gfx_fast_sqrt(d2));
            if (cov <= 0) continue;
            float t = fminf(1, fmaxf(0, 0.5f + dx * ux + dy * uy));   /* the terminator */
            uint16_t c = t >= 0.98f ? day : t <= 0.02f ? night : gfx_blend565(night, day, (uint8_t)(t * 255));
            hl_plot(fb, px, py, c, cov);
        }
    }
    GFX_COST(GFX_COST_ORR_LIT,s_hl_px);
}
static void draw_planet(gfx_fb *fb, const fk_view *v, int p)
{
    float r = S.pr[p], x = S.px[p], y = S.py[p];
    if (!fk_hit(v, x - r * 2.2f - 6, y - r - 6, x + r * 2.2f + 6, y + r + 6)) return;
    uint16_t ink = ob_tint[ob_of_planet(p)];
    if (S.tiltk > 0.02f) {   /* a drop line to the ecliptic shows height in 3D */
        pp b = S.pl[p]; b.zr = 0; float bx, by; proj(&b, &bx, &by);
        seg(fb, v, x, y, bx, by, 1, C_LINE2);
    }
    if (p == EP_SATURN && r > 4) gfx_line(fb, x - 0.94f * r * 1.9f, y + 0.34f * r * 1.9f, x + 0.94f * r * 1.9f, y - 0.34f * r * 1.9f, 1.2f, ui_dim(ink, 0.8f));
    if (r < 4.5f) gfx_fill_circle(fb, x, y, r, ink);
    else lit_disc(fb, v, x, y, r, S.litux[p], S.lituy[p], ink);
    if (p == EP_EARTH) gfx_ring(fb, x, y, r + 4, 1.5f, C_PRI);
}
static void draw_blur(gfx_fb *fb, const fk_view *v)
{
    for (int p = 0; p < EP_N; p++) {
        int n = S.blur_n[p];
        if (n < 2) continue;
        float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; unsigned h = gfx_hash_u(0xB10Bu, (unsigned)p);
        for (int k = 0; k < n; k++) {
            ORR_VISIT(1);
            x0 = fminf(x0, S.blur_x[p][k]); x1 = fmaxf(x1, S.blur_x[p][k]); y0 = fminf(y0, S.blur_y[p][k]); y1 = fmaxf(y1, S.blur_y[p][k]);
            if (fb->rec) h = gfx_hash_f(gfx_hash_f(h, S.blur_x[p][k]), S.blur_y[p][k]);
        }
        float r = S.pr[p];
        if (!grp(fb, v, x0 - r, y0 - r, x1 + r, y1 + r, h)) continue;
        uint16_t ink = ob_tint[ob_of_planet(p)];
        for (int k = 0; k < n - 1; k++) {
            ORR_VISIT(1);
            float f = (float)k / (n - 1), d = fmaxf(1.5f, r * 2 * (0.25f + 0.6f * f));
            float x = S.blur_x[p][k], y = S.blur_y[p][k];
            /* the tail fades out and thins: flat colour steps, no alpha */
            if (fk_hit(v, x - d, y - d, x + d, y + d)) dot_disc(fb, x, y, d, ui_dim(ink, 0.06f + 0.4f * f * gfx_fast_sqrt(f)));
        }
    }
}
static void draw_bodies(gfx_fb *fb, const fk_view *v)
{
    if (S.show[2]) for (int i = 0; i < S.ncm; i++) {
        ORR_VISIT(1);
        if (!S.cm_on[i]) continue;
        seg(fb, v, S.cmx[i], S.cmy[i], S.tailx[i], S.taily[i], 1, C_SEC);
        if(!orr_capture) orr_small_pixel(fb,S.cmx[i]-0.5f,S.cmy[i]-0.5f,C_PRI,1);
    }
    orr_cat(OP_DOTS);
    if(orr_capture) { orr_cmd *q=orr_item(B_BELT,0,GFX_H-1,0); if(q)q->aux=1; }
    for (int i = 0; i < S.nsm; i++) {
        ORR_VISIT(1);
        if (!S.sm_on[i]) continue;
        const od_small *o = &od_small_bodies[i];
        int big = o->kind == OD_DWARF || strstr(o->name, "Vesta") || strstr(o->name, "Pallas");
        if(!orr_capture) orr_small_pixel(fb,S.smx[i],S.smy[i],big?C_SEC:C_DIS,big?2:1);
    }
    orr_cat(OP_OTHER);
    for (int i = 0; i < S.ncr; i++) {
        ORR_VISIT(1);
        if (!S.cr_on[i]) continue;
        seg(fb, v, S.crx[i] - 3, S.cry[i], S.crx[i] + 3, S.cry[i], 1, C_SEC);
        seg(fb, v, S.crx[i], S.cry[i] - 3, S.crx[i], S.cry[i] + 3, 1, C_SEC);
    }
    if (S.pluto_on && fk_hit(v, S.plx - 4, S.ply - 4, S.plx + 4, S.ply + 4)) gfx_fill_circle(fb, S.plx, S.ply, 2.5f, ob_tint[OB_PLUTO]);
}
static void draw_zone_guides(gfx_fb *fb, const fk_view *v)
{
    float mixF = zone_mix(V_FACE), mixB = zone_mix(V_BEYOND), ct = S.ct, s = S.sc * S.zoom;
    if (mixF < 1 && S.ringe < 0.5f) ellipse_line(fb, v, S.sunx, S.suny, 224 * s, 224 * s * ct, C_LINE, 1);
    if (mixB > 0.5f) {
        static const float au[3] = { 10, 30, 100 }; static const char *const nm[3] = { "10 AU", "30 AU", "100 AU" };
        for (int i = 0; i < 3; i++) {
            float r = rmap1(V_BEYOND, au[i]) * s;
            ellipse_line(fb, v, S.sunx, S.suny, r, r * ct, C_LINE, 0);
            ui_text(fb, UI_F_SMALL, (int)(S.sunx + r * 0.71f + 4), (int)(S.suny + r * 0.71f * ct - 2), nm[i], C_DIS, UI_LEFT, 1);
        }
        float hp = rmap1(V_BEYOND, 120) * s;
        ellipse_line(fb, v, S.sunx, S.suny, hp, hp * ct, C_LINE2, 1);
        ui_text(fb, UI_F_SMALL, (int)S.sunx, (int)(S.suny - hp * ct - 4), "HELIOPAUSE", C_DIS, UI_CENTER, 1);
    }
    /* planets beyond the inner zone sit on its rim as ticks: tap the rim to go out */
    float mixI = zone_mix(V_INNER);
    if (mixI > 0.5f && S.show[0]) for (int p = EP_JUPITER; p < EP_N; p++) {
        float sl = S.pl[p].s * S.ca + S.pl[p].c * S.sa, cl = S.pl[p].c * S.ca - S.pl[p].s * S.sa;
        float x = CX + S.panx - 224 * s * sl, y = CY + S.pany - 224 * s * cl * ct;
        if (fk_hit(v, x - 3, y - 3, x + 3, y + 3)) gfx_fill_circle(fb, x, y, 2.5f, C_SEC);
    }
}
static void draw_event(gfx_fb *fb, const fk_view *v, const face_ctx *c)
{
    /* In the sector variants the single red accent belongs to the live dot. */
    if (S.ringe > 0.5f) return;
    if (!S.lines || !S.show[0] || S.chip_ev < 0 || S.chip_ev >= S.nev) return;
    const ep_event *e = &S.ev[S.chip_ev];
    double near = fabs(e->jd - S.jd);
    if (e->jd - S.jd > 60 || S.jd - e->jd > 2) return;
    float ex = S.px[EP_EARTH], ey = S.py[EP_EARTH];
    uint16_t red = ui_dim(fk_red(c, C_PRI), near < 2 ? 1.0f : fmaxf(0.4f, 1 - (float)near / 60));
    switch (e->kind) {
    case EV_OPPOSITION: case EV_SOLAR: seg(fb, v, S.sunx, S.suny, S.px[e->a], S.py[e->a], 1.5f, red); seg(fb, v, S.sunx, S.suny, ex, ey, 1.5f, red); break;
    case EV_CONJUNCTION: seg(fb, v, ex, ey, S.px[e->a], S.py[e->a], 1.5f, red); seg(fb, v, ex, ey, S.px[e->b], S.py[e->b], 1.5f, red); break;
    case EV_ELONGATION: seg(fb, v, ex, ey, S.px[e->a], S.py[e->a], 1.5f, red); seg(fb, v, ex, ey, S.sunx, S.suny, 1.5f, red); break;
    case EV_LINEUP: for (int p = 0; p < EP_N; p++) if (e->mask & (1u << p)) dashed(fb, v, ex, ey, S.px[p], S.py[p], 1.5f, red); break;
    }
    if (near < 1.5) {
        if (e->kind == EV_LINEUP) {
            for (int p = 0; p < EP_N; p++) if (e->mask & (1u << p)) gfx_ring(fb, S.px[p], S.py[p], 14, 1.5f, red);
        } else gfx_ring(fb, S.px[e->a], S.py[e->a], 14, 1.5f, red);
    }
}
static void draw_from_earth(gfx_fb *fb, const fk_view *v)
{
    if (!S.from_earth) return;
    struct { float x, y, d; const char *n; } c[64]; int n = 0;
    ep_vec E = S.sky.earth;
    #define ADD(xx, yy, h, nm) do { if (n < 64) { c[n].x = (xx); c[n].y = (yy); c[n].n = (nm); \
        c[n].d = orr_root((float)(((h).x - E.x) * ((h).x - E.x) + ((h).y - E.y) * ((h).y - E.y) + ((h).z - E.z) * ((h).z - E.z))); n++; } } while (0)
    if (S.show[0]) for (int p = 0; p < EP_N; p++) if (p != EP_EARTH && S.pl_on[p]) ADD(S.px[p], S.py[p], S.sky.b[p].helio, ep_name[p]);
    for (int i = 0; i < S.nsm; i++) if (S.sm_on[i]) { ep_vec h = { S.sm[i].c * S.sm[i].au, S.sm[i].s * S.sm[i].au, S.sm[i].zr * S.sm[i].au }; ADD(S.smx[i], S.smy[i], h, od_small_bodies[i].short_name); }
    for (int i = 0; i < S.ncm; i++) if (S.cm_on[i]) { ep_vec h = { S.cm[i].c * S.cm[i].au, S.cm[i].s * S.cm[i].au, S.cm[i].zr * S.cm[i].au }; ADD(S.cmx[i], S.cmy[i], h, od_comets[i].name); }
    for (int i = 0; i < S.ncr; i++) if (S.cr_on[i]) { ep_vec h = { od_crafts[i].xyz[0], od_crafts[i].xyz[1], od_crafts[i].xyz[2] }; ADD(S.crx[i], S.cry[i], h, od_crafts[i].name); }
    #undef ADD
    for (int k = 0; k < 5 && k < n; k++) {   /* the five nearest, selection-sorted */
        int m = k; for (int j = k + 1; j < n; j++) if (c[j].d < c[m].d) m = j;
        if (m != k) { __typeof__(c[0]) t = c[k]; c[k] = c[m]; c[m] = t; }
        dashed(fb, v, S.px[EP_EARTH], S.py[EP_EARTH], c[k].x, c[k].y, 1, GFX_RGB(0x55, 0x55, 0x55));
        char t[40];
        if (c[k].d < 0.05f) snprintf(t, sizeof t, "%.12s %.1f LD", c[k].n, c[k].d / 0.00257f);
        else snprintf(t, sizeof t, "%.12s %.2f AU", c[k].n, c[k].d);
        for (char *q = t; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        ui_text(fb, UI_F_SMALL, (int)c[k].x, (int)c[k].y + (c[k].y > S.py[EP_EARTH] ? 16 : -8), t, C_SEC, UI_CENTER, 1);
    }
}
static void draw_labels(gfx_fb *fb)
{
    ORR_VISIT(S.nlab);
    for (int i = 0; i < S.nlab; i++) ui_text(fb, UI_F_SMALL, (int)S.lab[i].x, (int)S.lab[i].y + 5, S.lab[i].s, S.lab[i].col, S.lab[i].align, 1);
}
static void draw_chip(gfx_fb *fb, const fk_view *v)
{
    if (S.ringe > 0.5f || zone_mix(V_FACE) < 0.5f) return;
    if (S.now_pill) {
        if (!fk_hit(v, CX - 50, CHIP_Y - 18, CX + 50, CHIP_Y + 18)) return;
        gfx_fill_pill(fb, CX - 30, CHIP_Y, CX + 30, 17, C_PRI);
        ui_text(fb, UI_F_SMALL, CX, CHIP_Y + 5, "NOW", C_BG, UI_CENTER, 1);
        return;
    }
    if (!S.chip[0]) return;
    float w = S.chip_w, x0 = CX - w / 2;
    if (!fk_hit(v, x0, CHIP_Y - 16, x0 + w, CHIP_Y + 16)) return;
    gfx_fill_pill(fb, x0 + 15, CHIP_Y, x0 + w - 15, 15, C_SURF2);
    dot_disc(fb, x0 + 14, CHIP_Y, 7, C_RED);
    ui_text(fb, UI_F_SMALL, (int)(x0 + 24), CHIP_Y + 5, S.chip, C_PRI, UI_LEFT, 1);
}
static void draw_zoom_dial(gfx_fb *fb, const fk_view *v)
{
    float k = (1 - S.lockk) * fmaxf(0, 1 - S.ringe * 3);
    if (k <= 0.01f) return;
    if (!grp(fb, v, CX - 170, CY + 140, CX + 170, CY + 233, gfx_hash_f(gfx_hash_f(0x2003u, S.zoom), k))) return;
    /* log positions of the ticks along the bar: 0.5 0.75 1 1.5 2 3 4 (major = the powers of two;
     * no numbers: the read-out under the bar says the value) */
    static const float tf[7] = { 0, 0.19499f, 0.33333f, 0.52832f, 0.66667f, 0.86166f, 1 };
    float zf = clampf(logf(S.zoom / ZMIN) / logf(ZMAX / ZMIN), 0, 1);
    for (int i = 0; i < 20; i++) orr_zoom(fb, i, zf, ui_dim(C_PRI, k), ui_dim(GFX_RGB(0x2a, 0x2a, 0x2a), k));
    for (int i = 0; i < 7; i++) {
        int major = !(i & 1);
        float phi = ZB_P1 - tf[i] * (ZB_P1 - ZB_P0), x0, y0, x1, y1;
        polar_xy(phi, ZOOM_R - 9, &x0, &y0); polar_xy(phi, ZOOM_R - (major ? 16 : 13), &x1, &y1);
        gfx_line(fb, x0, y0, x1, y1, 1.5f, ui_dim(major ? C_SEC : C_DIS, k));
    }
    float mx, my, px, py;
    polar_xy(ZOOM_P1 - 4, ZOOM_R + 1, &mx, &my); polar_xy(ZOOM_P0 + 4, ZOOM_R + 1, &px, &py);
    gfx_line(fb, mx - 4, my, mx + 4, my, 2, ui_dim(C_SEC, k));
    gfx_line(fb, px - 4, py, px + 4, py, 2, ui_dim(C_SEC, k)); gfx_line(fb, px, py - 4, px, py + 4, 2, ui_dim(C_SEC, k));
    char t[8]; snprintf(t, sizeof t, S.zoom < 1 ? "%.2fX" : "%.1fX", S.zoom);
    ui_text(fb, UI_F_SMALL, CX, CY + 195, t, ui_dim(C_PRI, k), UI_CENTER, 1);   /* the value, bottom centre */
}
static int commands_restore(orr_ui_cache *u,unsigned key);
static void commands_save(orr_ui_cache **cache,unsigned key,int n,int nt,int na);
static void draw_rings(gfx_fb *fb, const fk_view *v)
{
    float e = S.ringe, show = 1 - S.lockk;
    if (e < 0.01f) S.ring_warm=0;
    if (show < 0.01f) return;
    if (e < 0.01f) {   /* collapsed: the pull tab, three short arcs at top right (it steps aside for the menu) */
        float a = show * (1 - S.menue);
        if (a < 0.01f) return;
        for (int i = 0; i < 3; i++)
            gfx_arc(fb, CX, CY, TAB_R[i], 3, (TAB_PHI - 14 + i * 2) * D2RF, (TAB_PHI + 14 - i * 2) * D2RF, ui_dim(i == 0 ? C_PRI : C_SEC, a));
        return;
    }
    float span = 28 + 332 * e;
    for (int i = 0; i < 3; i++) {
        float r = TAB_R[i] + (RING_R[i] - TAB_R[i]) * e, w = 3 + (RING_W - 3) * e;
        /* Animated highlights/tick streams replace the shared backing data. */
        if(orr_capture && e!=1 && S.ring_capture[i])S.ring_capture[i]->valid=0;
        float ds=fk_sin(S.ring_delta[i]*D2RF),dc=fk_cos(S.ring_delta[i]*D2RF);
        /* a band is two hairline edges, not a 22 px fill: a filled annulus costs ~10 ms a ring on the watch */
        uint16_t band = GFX_RGB(0x30, 0x30, 0x30);
        if (e < 0.85f) {   /* the sweep out of the tab: thin arcs growing round the glass */
            orr_sweep(fb,i,r,TAB_PHI-span/2,TAB_PHI+span/2,ui_dim(C_SEC,1-e*.6f));
            continue;
        }
        /* The open ring: band, ticks, labels and selected annular segment. */
        union { double jd; uint32_t bits[2]; } ring_key = { ring_layout_jd };
        unsigned h = gfx_hash_u(gfx_hash_u(gfx_hash_u(0x417Bu, (unsigned)i), ring_key.bits[0]), ring_key.bits[1]);
        /* The last opening frames still change the radius and band width even
         * when the date is stationary. Include that geometry in damage. */
        h = gfx_hash_f(h, e);
        if (!grp(fb, v, CX - r - w, CY - r - w, CX + r + w, CY + r + w, h)) continue;
        unsigned capture_key=gfx_hash_f(gfx_hash_f(gfx_hash_u(gfx_hash_u(0xCA47u,C_PRI),C_BG),e),S.ring_delta[i]);
        capture_key=gfx_hash_u(capture_key,C_LINE2);
        uint32_t base_bits[2];memcpy(base_bits,&S.ring_base_jd[i],sizeof base_bits);
        capture_key=gfx_hash_u(gfx_hash_u(capture_key,base_bits[0]),base_bits[1]);
        capture_key=gfx_hash_u(gfx_hash_u(capture_key,S.ring_calendar[i]),S.hot[i]);
        capture_key=gfx_hash_u(capture_key,(S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0) && (S.drag!=4 || i==S.d_ring));
        int cached_capture=orr_capture && e==1;
        if(cached_capture && commands_restore(S.ring_capture[i],capture_key))continue;
        int cn=cached_capture?S.list->n:0,cnt=cached_capture?S.list->nt:0,cna=cached_capture?S.list->na:0;
        float gap = ring_gap(i);
        /* Preview the warm final band once labels appear. This avoids six
         * annular scans; the temporary radius error is at most 6.2 px on
         * the inner ring. At rest these are the exact original edges. */
        if(e>=0.85f && orr_band && orr_band->ready) {
            if(i==0) {
                if(orr_capture) orr_item(B_BANDS,0,GFX_H-1,C_LINE2);
                else orr_band_draw(fb,C_LINE2);
            }
        } else if (gap > 0) {
            gfx_arc(fb, CX, CY, r - w / 2, 1.2f, (-180 + gap) * D2RF, (180 - gap) * D2RF, C_LINE2);
            gfx_arc(fb, CX, CY, r + w / 2, 1.2f, (-180 + gap) * D2RF, (180 - gap) * D2RF, C_LINE2);
        } else { gfx_ring(fb, CX, CY, r - w / 2, 1.2f, band); gfx_ring(fb, CX, CY, r + w / 2, 1.2f, band); }
        if(orr_capture) memset(S.list->ring_tick_count[i],0,sizeof S.list->ring_tick_count[i]);
        int hot=S.hot[i];
        /* Omit ticks entirely covered by the opaque selected segment. Leave a
         * two-pixel angular margin so its antialiased ends retain the backdrop. */
        float hot_span=hot>=0?(S.rl[i][hot].width+10)*0.5f/r/D2RF:0;
        /* Half-degree rotation cache during motion (<=0.94 px at the outer
         * radius); release switches back to the exact legacy coordinates. */
        int tick_motion=(S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0) && (S.drag!=4 || i==S.d_ring);
        float tick_delta=tick_motion?floorf(S.ring_delta[i]*2+.5f)*.5f:S.ring_delta[i];
        float ts=fk_sin(tick_delta*D2RF),tc=fk_cos(tick_delta*D2RF);
        float tick_hot=hot>=0?S.rl[i][hot].phi+tick_delta:0;
        union { double jd; uint32_t bits[2]; } tick_base={S.ring_base_jd[i]};
        unsigned tick_key=gfx_hash_u(gfx_hash_u(0x71C4u,tick_base.bits[0]),tick_base.bits[1]);
        tick_key=gfx_hash_f(gfx_hash_f(gfx_hash_f(tick_key,tick_delta),e),(float)S.tz_days);
        tick_key=gfx_hash_u(gfx_hash_u(tick_key,(unsigned)hot),(unsigned)S.ring_calendar[i]);
        int tick_build=orr_capture?orr_ticks_begin(i,tick_key):-1;
        /* A successful coverage build needs no duplicate endpoint/ref list.
         * Populate that legacy list only on allocation/capacity failure. */
        for(int pass=0;pass<2;pass++) {
        if(orr_capture && tick_build==0)break;
        for (int j = 0; j < S.nrt[i]; j++) {
            ORR_VISIT(1);
            float phi=S.rt[i][j]+tick_delta;
            if(fabsf(phi)>180-gap)continue;
            if(hot>=0 && fabsf(phi-tick_hot)<hot_span)continue;
            float sn=S.rt_basis[i][j][0]*tc+S.rt_basis[i][j][1]*ts;
            float cs=S.rt_basis[i][j][1]*tc-S.rt_basis[i][j][0]*ts;
            float r0=r-w/2+1,r1=S.rtm[i][j]?r+w/2-1:r-w/2+5;
            float x0=CX+sn*r0,y0=CY-cs*r0,x1=CX+sn*r1,y1=CY-cs*r1;
            uint16_t ink=S.rtm[i][j] ? GFX_RGB(0x44,0x44,0x44) : GFX_RGB(0x2c,0x2c,0x2c);
            if(orr_capture && tick_build>0 && !pass) {
                orr_tick_line(i,x0,y0,x1,y1,S.rtm[i][j]);
                GFX_COST_PX(GFX_COST_ORR_PRE_CPU,1000);
            } else if(orr_capture) {
                S.list->ring_tick[i][j].x0=x0; S.list->ring_tick[i][j].y0=y0;
                S.list->ring_tick[i][j].x1=x1; S.list->ring_tick[i][j].y1=y1;
                S.list->ring_tick[i][j].ink=ink;
                int a=(int)fmaxf(0,floorf(fminf(y0,y1)-2))/16;
                int z=(int)fminf(GFX_H-1,ceilf(fmaxf(y0,y1)+2))/16;
                for(int b=a;b<=z;b++) {
                    int n=S.list->ring_tick_count[i][b];
                    if(n==48) { S.list->overflow=1; break; }
                    S.list->ring_tick_ref[i][b][n]=(uint16_t)j;
                    S.list->ring_tick_count[i][b]++;
                }
                GFX_COST_PX(GFX_COST_ORR_PRE_CPU,2000+500*(z-a+1));
            } else seg(fb,v,x0,y0,x1,y1,1,ink);
        }
        if(tick_build>0 && !pass)orr_ticks_finish(i);
        if(!orr_capture || tick_build<0 || (orr_ticks[i] && orr_ticks[i]->valid))break;
        }
        if(orr_capture) { orr_cmd *q=orr_item(B_RTICKS,0,GFX_H-1,0); if(q)q->aux=(uint16_t)i; }
        for (int j = 0; j < S.nrl[i]; j++) {
            ORR_VISIT(1);
            if (j == S.hot[i] || fabsf(S.rl[i][j].phi+S.ring_delta[i])>=172-gap) continue;
            float lx=CX+r*(S.rl[i][j].sn*dc+S.rl[i][j].cs*ds), ly=CY-r*(S.rl[i][j].cs*dc-S.rl[i][j].sn*ds);
            if (fk_hit(v, lx - 20, ly - 8, lx + 20, ly + 8)) orr_ring_text(fb,i*40+j,r,S.rl[i][j].phi+S.ring_delta[i],S.rl[i][j].s,S.rl[i][j].major ? GFX_RGB(0x8a,0x8a,0x8a) : GFX_RGB(0x6a,0x6a,0x6a));
        }
        if (S.hot[i] >= 0) {
            int j=S.hot[i];
            /* Square-ended sector shares the label's centre/rotation and band
             * height. Cache coverage in tick, stamp only this stripe in draw. */
            float phi=(S.rl[i][j].phi+S.ring_delta[i])*D2RF;
            float half=(S.rl[i][j].width+14)*0.5f/r;
            orr_ring_highlight(fb,i,r,w,phi,half,C_PRI);
            orr_ring_text(fb,i*40+j,r,S.rl[i][j].phi+S.ring_delta[i],S.rl[i][j].s,C_BG);
        }
        if(cached_capture)commands_save(&S.ring_capture[i],capture_key,cn,cnt,cna);
    }
    if (e > 0.85f) {
        unsigned grip_key=gfx_hash_u(gfx_hash_u(0x641Fu,C_PRI),C_BG);
        if(orr_capture && commands_restore(S.ring_capture[3],grip_key))return;
        int n=orr_capture?S.list->n:0,nt=orr_capture?S.list->nt:0,na=orr_capture?S.list->na:0;
        /* the index at 12 o'clock, and the grip that folds them away */
        float gx, gy; polar_xy(TAB_PHI, RING_R[0], &gx, &gy);
        if (fk_hit(v, CX - 4, 1, CX + 4, 6)) { gfx_line(fb, CX - 3, 2, CX, 5, 1, C_PRI); gfx_line(fb, CX + 3, 2, CX, 5, 1, C_PRI); }
        if (fk_hit(v, gx - 11, gy - 11, gx + 11, gy + 11)) {
            gfx_fill_circle(fb, gx, gy, 10, C_PRI);
            gfx_line(fb, gx - 4, gy - 4, gx + 4, gy + 4, 2, C_BG); gfx_line(fb, gx + 4, gy - 4, gx - 4, gy + 4, 2, C_BG);
        }
        if(orr_capture)commands_save(&S.ring_capture[3],grip_key,n,nt,na);
    }
}
#include "face_orrery_ringread.h"

/* the menu's line icons on an 18 px grid */
static void icon(gfx_fb *fb, const char *name, float x, float y, uint16_t col, float s)
{
    #define L(ax, ay, bx, by) gfx_line(fb, x + (ax) * s, y + (ay) * s, x + (bx) * s, y + (by) * s, 1.6f + 0.75f * s, col)
    #define RING(cx, cy, r) gfx_ring(fb, x + (cx) * s, y + (cy) * s, (r) * s, 1.6f + 0.75f * s, col)
    #define DOT(cx, cy, r) gfx_fill_circle(fb, x + (cx) * s, y + (cy) * s, (r) * s, col)
    if (!strcmp(name, "pan")) { L(-8, 0, 8, 0); L(0, -8, 0, 8); L(-5, -3, -8, 0); L(-5, 3, -8, 0); L(5, -3, 8, 0); L(5, 3, 8, 0); L(-3, -5, 0, -8); L(3, -5, 0, -8); L(-3, 5, 0, 8); L(3, 5, 0, 8); }
    else if (!strcmp(name, "time")) { RING(0, 0, 8); L(0, -5, 0, 0); L(0, 0, 4, 2); }
    else if (!strcmp(name, "look")) { gfx_arc(fb, x, y, 8 * s, 2, -2.6f + 1.5708f, 1.9f + 1.5708f, col); L(-2.6f, 8.6f, -2.6f, 4.2f); L(-2.6f, 8.6f, 1.6f, 6.4f); DOT(0, 0, 2.4f); }
    else if (!strcmp(name, "centre")) { RING(0, 0, 6); DOT(0, 0, 2); L(0, -10, 0, -8); L(0, 8, 0, 10); L(-10, 0, -8, 0); L(8, 0, 10, 0); }
    else if (!strcmp(name, "overhead")) { RING(0, 0, 8); DOT(0, 0, 2.2f); DOT(5.7f, -5.7f, 2.2f); }
    else if (!strcmp(name, "tilt")) { float px = x + 9 * s, py = y + s; for (int i = 1; i <= 16; i++) { float a = i / 16.0f * 2 * PI_F, qx = x + 9 * s * fk_cos(a), qy = y + s + 3.6f * s * fk_sin(a); gfx_line(fb, px, py, qx, qy, 2, col); px = qx; py = qy; } DOT(0, 1, 2.2f); DOT(6, -1.5f, 2.2f); L(0, -1, 0, -7); }
    else if (!strcmp(name, "filter")) { L(-8, -6, 8, -6); L(8, -6, 2, 1); L(2, 1, 2, 7); L(2, 7, -2, 5); L(-2, 5, -2, 1); L(-2, 1, -8, -6); }
    else if (!strcmp(name, "planets")) { RING(0, 0, 5); L(-9, 3, 9, -3); }
    else if (!strcmp(name, "asteroids")) { DOT(-5, -3, 2.2f); DOT(3, -6, 1.6f); DOT(6, 2, 2.4f); DOT(-2, 6, 1.5f); DOT(1, 0, 1.2f); }
    else if (!strcmp(name, "comets")) { DOT(5, -5, 3); L(2, -2, -8, 8); L(4, 0, -5, 9); L(0, -4, -9, 5); }
    else if (!strcmp(name, "craft")) { L(-3, -3, 3, -3); L(3, -3, 3, 3); L(3, 3, -3, 3); L(-3, 3, -3, -3); L(-10, 0, -3, 0); L(3, 0, 10, 0); L(-10, -3, -10, 3); L(10, -3, 10, 3); }
    else if (!strcmp(name, "fromEarth")) { RING(-5, 5, 3.2f); L(-3, 3, 0, 0); L(2, -2, 7, -7); L(-2, 5, 2, 4); L(5, 3, 8, 2); DOT(7, -7, 1.8f); DOT(8, 2, 1.8f); }
    else if (!strcmp(name, "lines")) { L(-9, 7, -4, 3); L(-1, 1, 4, -3); L(6, -4, 9, -7); DOT(-7, 5.5f, 2.4f); DOT(0, 0, 2.4f); DOT(7, -5.5f, 2.4f); }
    #undef L
    #undef RING
    #undef DOT
}
static const char *main_icon(int m)
{
    switch (m) {
    case M_DRAG: return S.drag_mode == DR_PAN ? "pan" : S.drag_mode == DR_LOOK ? "look" : "time";
    case M_VIEW: return S.tilt ? "tilt" : "overhead";
    case M_FILTER: return "filter";
    default: return "lines";
    }
}
static const char *opt_icon(int m, int j)
{
    static const char *const d[4] = { "pan", "look", "time", "centre" }, *const vw[2] = { "overhead", "tilt" },
                      *const f[5] = { "planets", "asteroids", "comets", "craft", "fromEarth" };
    return m == M_DRAG ? d[j] : m == M_VIEW ? vw[j] : f[j];
}
static void draw_menu(gfx_fb *fb, const fk_view *v)
{
    float show = 1 - S.lockk;
    if (show < 0.01f) return;
    /* The year dial owns this rim while open (including its touch priority).
     * Fade the menu handle with the opening sweep so it cannot strike through
     * tangent year labels at the right-hand flip point. */
    float handle = show * (1 - S.ringe);
    if (handle > 0.01f)
        gfx_arc(fb, CX, CY, 226, 6, H_P0 * D2RF, H_P1 * D2RF, ui_dim(S.menue > 0.5f ? C_PRI : GFX_RGB(0x6a, 0x6a, 0x6a), handle));
    if (S.menue < 0.6f && handle > 0.01f) {
        uint16_t c = ui_dim(C_SEC, handle * (1 - S.menue / 0.6f));
        float cx = CX + 212;
        if (fk_hit(v, cx - 8, CY - 8, cx + 4, CY + 8)) { dot_disc(fb, cx, CY - 6, 3.6f, c); dot_disc(fb, cx - 5, CY, 3.6f, c); dot_disc(fb, cx, CY + 6, 3.6f, c); }
    }
    for (int k = 0; k < S.nmh; k++) {
        ORR_VISIT(1);
        float x = S.mhit[k].x, y = S.mhit[k].y; int m = S.mhit[k].id, j = S.mhit[k].key;
        unsigned h = gfx_hash_f(gfx_hash_f(gfx_hash_u(0x3E7u, (unsigned)(m * 16 + j + 1)), x), y);
        if (j < 0) {
            int on = m == M_LINES ? S.lines : 0, open = S.menu_sub == m;
            h = gfx_hash_u(gfx_hash_u(h, (unsigned)(on * 2 + open)), (unsigned)(uintptr_t)main_icon(m));
            if (!grp(fb, v, x - BTN - 4, y - BTN - 4, x + BTN + 4, y + BTN + 4, h)) continue;
            gfx_fill_circle(fb, x, y, BTN, on ? C_PRI : GFX_RGB(0x1c, 0x1c, 0x1c));
            if (open) gfx_ring(fb, x, y, BTN + 3, 2, C_PRI);
            icon(fb, main_icon(m), x, y, on ? C_BG : m == M_LINES ? GFX_RGB(0x77, 0x77, 0x77) : C_PRI, 1.2f);
        } else {
            int lit = menu_opt_on(m, j);
            h = gfx_hash_u(h, (unsigned)lit);
            if (!grp(fb, v, x - BTN, y - BTN, x + BTN, y + BTN, h)) continue;
            gfx_fill_circle(fb, x, y, BTN - 1, lit ? C_PRI : GFX_RGB(0x26, 0x26, 0x26));
            icon(fb, opt_icon(m, j), x, y, lit ? C_BG : C_SEC, 1.08f);
        }
    }
}
static void draw_hold(gfx_fb *fb)
{
    if (!S.hold) return;
    float h = nos_t - S.hold_t0;
    if (h < 0.15f) return;
    /* Half the previous large loader dimensions. Use the same
     * segmented-line primitive as the zoom dial, including its growing tip. */
    gfx_fill_circle(fb, S.hx, S.hy, 87, C_BG);
    prog_ring(fb, PROG_SEGMENTS, S.hx, S.hy, 69.3f, 6.3f, 32,
              clampf(h / HOLD_LOCK, 0, 1), C_PRI, C_LINE2);
    if (h > HOLD_LOCK) prog_ring(fb, PROG_SEGMENTS, S.hx, S.hy, 81.9f, 4.725f, 32,
              clampf((h - HOLD_LOCK) / (HOLD_PICK - HOLD_LOCK), 0, 1), C_SEC, 0);
    const char *what = h >= HOLD_LOCK ? (h > 2.4f ? "FACES" : S.locked ? "UNLOCK" : "LOCK") : h >= HOLD_TILT ? (S.tilt ? "FLAT" : "3D") : "";
    if (*what) ui_text(fb, UI_F_SMALL, S.hx, S.hy - 98, what, C_PRI, UI_CENTER, 1);
}
static void draw_centre(gfx_fb *fb, const fk_view *v, const face_ctx *c)
{
    float mixF = zone_mix(V_FACE);
    /* the Sun: a plain tinted disc once unlocked (and in the zones); the clock owns the locked face */
    float sa = fmaxf(1 - mixF, 1 - S.lockk);
    orr_cat(OP_PLANETS);
    if (sa > 0.01f) gfx_fill_circle(fb, S.sunx, S.suny, (8 + 3 * zone_mix(V_INNER)) * fminf(1.6f, fmaxf(0.75f, S.zoom)), ui_dim(ob_tint[OB_SUN], sa));
    orr_cat(OP_TEXT);
    if (S.ringe > 0.5f) return; /* the flat dock is drawn after the rings */
    float tA = mixF * S.lockk;
    if (tA > 0.01f && fk_hit(v, CX - 94, CY - 38, CX + 94, CY + 46)) {
        char tm[8], db[16];
        if (fabs(S.off) > 0.02) {   /* travelling while locked (an ease home): days off, and the date */
            civ n = civil(S.jd);
            snprintf(tm, sizeof tm, "%+d", (int)floor(S.off + 0.5));
            snprintf(db, sizeof db, "%02d %s %d", n.d, MON[n.m - 1], n.y);
        } else { fk_time(tm, c->dt.hour, c->dt.minute); fk_date(db, c); }
        /* In the tilted view, compressed orbit lines used to run through the
         * locked digits. A black inset keeps the clock readable over the Sun. */
        if (S.tiltk > 0.02f) gfx_fill_pill(fb, CX - 52, CY + 3, CX + 52, 39, C_BG);
        dot_style st = { DOT_FONT_5X7, 4.6f, 3.6f, ui_dim(C_PRI, tA), 0, 1, 1 };
        dot_text_a(fb, &st, CX, CY - 20, tm, 1);
        ui_label(fb, CX, CY + 34, db, ui_dim(C_SEC, tA), UI_CENTER);
    }
    if (S.lockk < 0.99f && fabs(S.off) > 0.02 && mixF > 0.01f) {   /* unlocked and travelled: the date at the top */
        civ n = civil(S.jd); char t[40];
        snprintf(t, sizeof t, "%02d %s %d · %+d D", n.d, MON[n.m - 1], n.y, (int)floor(S.off + 0.5));
        ui_text(fb, UI_F_SMALL, CX, 37, t, ui_dim(C_SEC, 1 - S.lockk), UI_CENTER, 1);
    }
    if (mixF < 0.99f && S.ringe < 0.05f && (fabs(S.off) > 0.02 || S.lockk > 0.5f)) {
        char t[24];
        if (fabs(S.off) > 0.02) { civ n = civil(S.jd); snprintf(t, sizeof t, "%02d %s %d", n.d, MON[n.m - 1], n.y); }
        else fk_time(t, c->dt.hour, c->dt.minute);
        ui_text(fb, UI_F_SMALL, CX, 27, t, C_SEC, UI_CENTER, 1);
    }
    if (mixF < 0.5f && S.view != V_BEYOND) {
        static const char *const zn[4] = { "", "INNER", "OUTER", "BEYOND" };
        ui_text(fb, UI_F_SMALL, CX, CHIP_Y + 31, zn[S.view], C_DIS, UI_CENTER, 1);
    }
}

/* The fixed controls are the same throughout a pan. Copy their retained
 * commands, text and prepared arcs instead of measuring text/preparing arcs
 * again. Capacity/allocation failure simply takes the normal capture path. */
static int commands_restore(orr_ui_cache *u,unsigned key)
{
    orr_list *l=S.list;
    if(!orr_capture || !u || !u->valid || u->key!=key ||
       l->n+u->n>ORR_COMMANDS || l->nt+u->nt>ORR_TEXTS || l->na+u->na>ORR_ARCS) return 0;
    /* Ring runs live in the list, but their glyph slots may have been evicted
     * or may still be direct motion previews. Validate/re-pin before copying. */
    for(int i=0;i<u->n;i++)if(u->cmd[i].type==B_ATEXT) {
        int category=orr_category;orr_cat(OP_TEXT);
        uint32_t stamp=orr_prof.enabled?orr_prof_now():0;
        int retained=ui_text_arc_retain(&l->ring_text[u->cmd[i].aux]);
        if(orr_prof.enabled)orr_prof.arc_us[1]+=orr_prof_now()-stamp;
        orr_cat(category);
        if(!retained)return 0;
    }
    memcpy(l->text+l->nt,u->text,u->nt*sizeof *u->text);
    memcpy(l->arc+l->na,u->arc,u->na*sizeof *u->arc);
    for(int i=0;i<u->n;i++) {
        orr_cmd q=u->cmd[i];
        if(q.type==B_TEXT || q.type==B_LABEL || q.type==B_DTEXT) q.aux+=(uint16_t)l->nt;
        else if(q.type==B_ARC && q.aux) q.aux+=(uint16_t)l->na;
        l->cmd[l->n++]=q;
    }
    l->nt+=u->nt; l->na+=u->na; u->hits++;
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,1000*u->n+2000*(u->nt+u->na));
    return 1;
}
static void commands_save(orr_ui_cache **cache,unsigned key,int n,int nt,int na)
{
    orr_list *l=S.list;
    if(!*cache) { *cache=hal_big_alloc(sizeof **cache); if(*cache)memset(*cache,0,sizeof **cache); }
    orr_ui_cache *u=*cache;
    if(!u) return;
    u->valid=0;
    if(l->overflow || l->n-n>ORR_UI_CMDS || l->nt-nt>ORR_UI_TEXTS || l->na-na>ORR_UI_ARCS) return;
    u->n=l->n-n; u->nt=l->nt-nt; u->na=l->na-na;
    memcpy(u->text,l->text+nt,u->nt*sizeof *u->text);
    memcpy(u->arc,l->arc+na,u->na*sizeof *u->arc);
    for(int i=0;i<u->n;i++) {
        orr_cmd q=l->cmd[n+i];
        if(q.type==B_TEXT || q.type==B_LABEL || q.type==B_DTEXT) q.aux-=(uint16_t)nt;
        else if(q.type==B_ARC && q.aux) q.aux-=(uint16_t)na;
        u->cmd[i]=q;
    }
    u->key=key; u->valid=1; u->builds++;
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,1000*u->n+2000*(u->nt+u->na));
}
static int controls_restore(void) { return commands_restore(S.ui_cache,S.ui_key); }
static void controls_save(int n,int nt,int na) { commands_save(&S.ui_cache,S.ui_key,n,nt,na); }
static void draw_scene(gfx_fb *fb, const face_ctx *c)
{
    if (!orr_ok()) return;
    fk_view v; fk_view_of(fb, &v);
    if (c->aod) {
        orr_cat(OP_PLANETS);
        int dx = fk_aod_dx, dy = fk_aod_dy;
        for (int p = 0; p < EP_N; p++) dot_disc(fb, S.px[p] + dx, S.py[p] + dy, fmaxf(3.0f, BODY_R[p] * 1.2f), C_DIS);
        char tm[8]; fk_time(tm, c->dt.hour, c->dt.minute);
        dot_style st = { DOT_FONT_5X7, 4.6f, 3.0f, C_SEC, 0, 1, 1 };
        dot_text_a(fb, &st, CX + dx, CY - 16 + dy, tm, 1);
        return;
    }
    orr_cat(OP_ORBITS);
    draw_orbits(fb, &v);
    draw_zone_guides(fb, &v);
    orr_cat(OP_DOTS);
    draw_belt(fb, &v);
    orr_cat(OP_TRAILS);
    draw_blur(fb, &v);
    draw_bodies(fb, &v);
    orr_cat(OP_OTHER);
    draw_event(fb, &v, c);
    orr_cat(OP_PLANETS);
    for (int p = 0; p < EP_N; p++) if (S.pl_on[p]) draw_planet(fb, &v, p);
    orr_cat(OP_OTHER);
    draw_from_earth(fb, &v);
    orr_cat(OP_TEXT);
    draw_labels(fb);
    if (!S.clock_overlay) draw_centre(fb, &v, c);
    orr_cat(OP_CONTROLS);
    int open=S.ringe>0.5f;
    int restored=controls_restore();
    if(restored && !open)return;
    int cn=orr_capture?S.list->n:0,ct=orr_capture?S.list->nt:0,ca=orr_capture?S.list->na:0;
    if(!restored) {
        if (S.pulse > 0) gfx_ring(fb, CX, CY, 231, 2, ui_dim(C_PRI, S.pulse / 8.0f));
        draw_chip(fb, &v);
        draw_zoom_dial(fb, &v);
        if(orr_capture && open)controls_save(cn,ct,ca);
    }
    draw_rings(fb, &v);
    draw_ring_sector(fb);
    draw_menu(fb, &v);
    if(orr_capture && !open) controls_save(cn,ct,ca);
}

/* Explicit input key: no frame time, fractional RTC second, pointer or padding.
 * Include the previous time only when a trail must be drawn/cleared. */
static unsigned scene_key(const face_ctx *c, int clock, int camera)
{
    unsigned h = gfx_hash_u(gfx_hash_u(2166136261u,S.belt_version),S.small_version);
    float f[] = { S.lockk, S.tiltk, S.cam_tilt, S.az, S.zoom, camera?S.panx:0, camera?S.pany:0,
        S.tx, S.ringe, S.menue, S.sube, c->rec, S.platee };
    int v[] = { S.view, S.from, S.drag_mode, S.lines, S.from_earth,
        S.show[0], S.show[1], S.show[2], S.show[3], S.menu_sub, S.tilt,
        S.now_pill, S.chip_ev, S.pulse, S.ring_now_down, c->aod, c->aod ? fk_aod_dx : 0, c->aod ? fk_aod_dy : 0,
        C_BG, C_PRI, C_SEC, nos_prefs.hour24,
        camera && (S.drag==1 || S.pvx!=0 || S.pvy!=0 || S.home_t), S.drag==2 || S.drag==3 };
    for (unsigned i = 0; i < sizeof f / sizeof f[0]; i++) h = gfx_hash_f(h, f[i]);
    for (unsigned i = 0; i < sizeof v / sizeof v[0]; i++) h = gfx_hash_u(h, (unsigned)v[i]);
    if (clock) {
        int date[] = { c->dt.year, c->dt.month, c->dt.day, c->dt.weekday, c->dt.hour, c->dt.minute };
        for (unsigned i = 0; i < sizeof date / sizeof date[0]; i++) h = gfx_hash_u(h, (unsigned)date[i]);
    }
    /* The event's fade is quantised by RGB565, not by fractional days. */
    if (S.chip_ev >= 0 && S.chip_ev < S.nev) {
        double d = S.ev[S.chip_ev].jd - S.jd, near = fabs(d);
        h = gfx_hash_u(h, (unsigned)(d <= 60 && d >= -2));
        h = gfx_hash_u(h, (unsigned)(near < 1.5));
        h = gfx_hash_u(h, ui_dim(fk_red(c, C_PRI), near < 2 ? 1.0f : fmaxf(0.4f, 1 - (float)near / 60)));
    }
    double render_jd = S.drag == 4 || S.drag == 5 || S.ta_n || S.vel != 0 ?
                       S.jd : floor(S.jd * 1440.0) / 1440.0;
    double t[] = { clock || S.ringe > 0.01f || fabs(S.off) > 0.02 ? render_jd : 0, S.jd_sky, S.jd_orb, S.jd_small, S.jd_belt,
        S.jd_prev_frame && fabs(S.jd - S.jd_prev_frame) >= 0.25 ? S.jd_prev_frame : 0 };
    for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++) {
        uint32_t b[2]; memcpy(b, &t[i], sizeof b);
        h = gfx_hash_u(gfx_hash_u(h, b[0]), b[1]);
    }
    for (const char *p = S.chip; *p; p++) h = gfx_hash_u(h, (unsigned char)*p);
    return h;
}
/* Geometry key excludes pan and wall-clock labels; include dot/label theme ink. */
static unsigned layout_key(int with_zoom)
{
    unsigned h=gfx_hash_u(gfx_hash_u(2166136261u,S.belt_version),S.small_version);
    float f[]={S.tiltk,S.cam_tilt,S.az,with_zoom?S.zoom:0,S.tx,S.ringe};
    int v[]={S.view,S.from,S.show[0],S.show[1],S.show[2],S.show[3],C_PRI,C_SEC,C_DIS};
    double t[]={S.jd_sky,S.jd_orb,S.jd_small,S.jd_belt,
        S.jd_prev_frame && fabs(S.jd-S.jd_prev_frame)>=0.25 ? S.jd_prev_frame : 0};
    for(unsigned i=0;i<sizeof f/sizeof f[0];i++) h=gfx_hash_f(h,f[i]);
    for(unsigned i=0;i<sizeof v/sizeof v[0];i++) h=gfx_hash_u(h,(unsigned)v[i]);
    for(unsigned i=0;i<sizeof t/sizeof t[0];i++) { uint32_t b[2]; memcpy(b,&t[i],sizeof b); h=gfx_hash_u(gfx_hash_u(h,b[0]),b[1]); }
    return h;
}
/* Dots depend on belt/small-body positions, never on planet ephemerides or
 * the previous frame's trail. Ring opening and zoom can replay scaled dots. */
static unsigned dots_key(void)
{
    unsigned h=gfx_hash_u(gfx_hash_u(0xD075u,S.belt_version),S.small_version);
    float f[]={S.tiltk,S.cam_tilt,S.az,S.tx};
    int v[]={S.view,S.from,S.show[1],S.show[2],C_PRI,C_SEC,C_DIS};
    double t[]={S.jd_belt,S.jd_small};
    for(unsigned i=0;i<sizeof f/sizeof *f;i++)h=gfx_hash_f(h,f[i]);
    for(unsigned i=0;i<sizeof v/sizeof *v;i++)h=gfx_hash_u(h,(unsigned)v[i]);
    for(unsigned i=0;i<sizeof t/sizeof *t;i++) { uint32_t b[2];memcpy(b,t+i,sizeof b);h=gfx_hash_u(gfx_hash_u(h,b[0]),b[1]); }
    return h;
}
static void layout_shift(float dx,float dy)
{
    GFX_COST_PX(GFX_COST_ORR_PRE_CPU,240000);
    /* Visibility flags/radial cutoffs are relative to the Sun, not the glass.
     * Keep off-screen geometry, so later pans can expose it without stale edges. */
    for(int p=0;p<EP_N;p++) {
        S.px[p]+=dx; S.py[p]+=dy;
        if(!S.orbit_pixels || !S.orbit_pixels->valid) for(int i=0;i<NORB;i++) {
            const float *d=S.orbit_shift[p][i];
            S.ox[p][i]=(CX+S.panx)-d[0]; S.oy[p][i]=((CY+S.pany)-d[1])-d[2];
        }
        for(int i=0;i<S.blur_n[p];i++) { S.blur_x[p][i]+=dx; S.blur_y[p][i]+=dy; }
    }
    S.plx+=dx; S.ply+=dy; S.sunx+=dx; S.suny+=dy;
    /* Belt positions and sorted dots stay at their build origin. */
    for(int i=0;i<S.nsm;i++) { S.smx[i]+=dx; S.smy[i]+=dy; }
    for(int i=0;i<S.ncm;i++) { S.cmx[i]+=dx; S.cmy[i]+=dy; S.tailx[i]+=dx; S.taily[i]+=dy; }
    for(int i=0;i<S.ncr;i++) { S.crx[i]+=dx; S.cry[i]+=dy; }
}
static void scene_update(const face_ctx *c)
{
#ifdef GFX_COST_MODEL
    extern void gfx_cost_orr_rings(int);
    gfx_cost_orr_rings(S.ringe>0.01f);
#endif
    /* AOD renders its tiny scene directly. The retained list belongs to the
     * next awake frame, including dots, labels, commands and stripe indices. */
    if(c->aod) { face_ctx awake=*c; awake.aod=0; scene_update(&awake); return; }
    S.clock_overlay = !c->aod && S.lockk == 1 && S.view == V_FACE && S.from < 0 &&
                      S.ringe == 0 && fabs(S.off) <= 0.02;
    if (S.lockk < 1) { orr_band_prepare();orr_plate_prepare(); }
    unsigned key = gfx_hash_u(scene_key(c, !S.clock_overlay, 1), (unsigned)S.clock_overlay);
    int arc_motion=S.drag==4 || S.drag==5 || S.drag==6 || S.ta_n || S.vel!=0 ||
                   S.rings_pull || (S.ringk>0 && S.ringk<1);
    key=gfx_hash_u(key,(unsigned)arc_motion);
    if (S.scene_valid && key == S.scene_input && !ui_text_arc_pending()) return;
    ui_text_arc_motion(arc_motion);
    if(!S.list) S.list=hal_big_alloc(sizeof *S.list);
    ORR_TIME_START(tlayout);
    {
        unsigned lk=layout_key(1),dk=dots_key();
        int scaling=S.drag==2 || S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0 || S.belt_left || S.small_left || (S.ringk>0 && S.ringk<1);
        S.dot_preview=S.layout_valid && S.list && dk==S.dot_geometry_key &&
            (scaling || (S.zoom==S.list->dot_zoom && 1-ORB_SHRINK*S.ringe==S.list->dot_shrink));
        int changed=!S.layout_valid || lk!=S.layout_key || !S.dot_preview;
        float dx=S.panx-S.layout_panx, dy=S.pany-S.layout_pany;
        ORR_LAYOUT(0,if(changed) layout(); else if(dx!=0 || dy!=0) layout_shift(dx,dy));
        ORR_LAYOUT(1,orr_orbits_build());
        /* Outgoing home still ticks during card transitions. Keep its coverage
         * replay exact without regrowing optional ink ahead of card textures. */
        if (nos_top() == &scr_home && !nos_transitioning()) ORR_LAYOUT(1,orr_orbits_ink());
        if(S.list && changed && !S.dot_preview) {
            orr_cat(OP_DOTS); orr_prof_start(OP_BUCKET);
            ORR_LAYOUT(2,orr_pixels_build()); S.dot_geometry_key=dk; orr_prof_stop();
        }
        unsigned labkey=gfx_hash_f(gfx_hash_f(gfx_hash_f(lk,S.panx),S.pany),S.chip_w);
        int moving=S.drag==1 || S.pvx!=0 || S.pvy!=0 || S.home_t;
        /* The overview has no body labels. Its changing ephemeris/chip never
         * changes that empty label layout. Other zones retain moving labels. */
        int empty_labels=S.ringe>0.5f && S.view==V_FACE && S.from<0;
        if((changed && (!empty_labels || S.nlab)) || (!empty_labels &&
           (S.chip_w!=S.label_chip_w || (!moving && S.labels_shifted)))) {
            ORR_LAYOUT(3,labels_layout()); S.labels_shifted=0;
        } else if(dx!=0 || dy!=0) {
            for(int i=0;i<S.nlab;i++) { S.lab[i].x+=dx; S.lab[i].y+=dy; }
            S.labels_shifted=1;
        }
        S.label_chip_w=S.chip_w;
        S.label_key=labkey; S.layout_key=lk; S.layout_valid=1;
        S.layout_panx=S.panx; S.layout_pany=S.pany;
        double rjd=S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0 ? S.jd : floor(S.jd*1440.0)/1440.0;
        int time_motion=S.drag==4 || S.drag==5 || S.ta_n || S.vel!=0;
        if(S.ringe>0.01f && (!S.rings_valid || S.rings_jd!=rjd || S.rings_off!=S.off || S.rings_tz!=S.tz_days || S.rings_motion!=time_motion)) {
            ORR_LAYOUT(4,rings_layout()); S.rings_jd=rjd; S.rings_off=S.off; S.rings_tz=S.tz_days; S.rings_valid=1;
        }
        unsigned mk=gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_u(0,(unsigned)S.menu_sub),S.menuk),S.menue),S.sube);
        if(!S.menu_valid || mk!=S.menu_key) { ORR_LAYOUT(5,menu_layout()); S.menu_key=mk; S.menu_valid=1; }
    }
    ORR_TIME_END(1,tlayout);
    if(!S.list) { S.layout_valid=0; S.scene_valid=0; return; }
    S.scene_ink=scene_key(c,0,1);
    S.ui_key=scene_key(c,!S.clock_overlay,0);
    if(S.ringe>0.5f) {
        /* The open-ring prefix is just the optional pulse: the chip and zoom
         * dial are hidden. Date/ring text belong to the live suffix. */
        S.ui_key=gfx_hash_u(gfx_hash_u(gfx_hash_u(0x0F3Au,S.pulse),C_PRI),C_BG);
    }
    ORR_TIME_START(tcommands);
    S.list->n=S.list->nt=S.list->na=0; S.list->overflow=!c->aod && S.dots_overflow;
    gfx_fb fb={0}; fb.h=GFX_H; fb.cx1=GFX_W;
    orr_cost_class=GFX_COST_ORR_BUILD;
    ORR_REBUILD();
    orr_cat(OP_OTHER); orr_prof_start(OP_CAPTURE);
    ui_text_arc_begin();
    orr_capture=1;
    draw_scene(&fb,c);
    orr_capture=0;
    /* Exact masks replace preview pixels during otherwise idle settle ticks. */
    for(int i=0;i<S.list->n;i++) if(S.list->cmd[i].type==B_ATEXT)
        S.scene_ink=gfx_hash_u(S.scene_ink,S.list->ring_text[S.list->cmd[i].aux].hash);
    orr_prof_stop();
    orr_cost_class=GFX_COST_ORR_WALK;
    ORR_TIME_END(2,tcommands);
    ORR_TIME_START(tbuckets);
    orr_cat(OP_OTHER); orr_prof_start(OP_BUCKET);
    orr_bucket();
    orr_prof_stop();
    S.scene_rec.n=S.scene_rec.overflow=0;
    fb.rec=&S.scene_rec;
    if(c->aod) draw_scene(&fb,c);
    else {
        gfx_rec_cmd(&fb,0,0,GFX_W,GFX_H,S.scene_ink);
        if(!S.clock_overlay) draw_centre(&fb,&(fk_view){0,0,GFX_W,GFX_H},c);
    }
    S.scene_input=key; S.scene_valid=!S.list->overflow;
    ORR_TIME_END(3,tbuckets);
}
static void draw(gfx_fb *fb, const face_ctx *c)
{
    orr_last_draw = nos_t;
    if (!orr_ok()) { fk_clock_fallback(fb, c, UI_CX, UI_CY + 24); return; }
    if(!fb->rec) { orr_cat(OP_OTHER); orr_prof_start(OP_RASTER); }
    if(c->aod) { draw_scene(fb,c); }
    else if (!S.scene_valid) {
        /* Allocation failure, capacity overflow or unticked preview: correct
         * direct rendering remains available (without the performance guarantee). */
        draw_scene(fb, c);
    } else if (fb->rec) {
        /* Preserve small clock damage; sky/controls share a retained key.
         * Replay once per frame, never once per raster stripe. */
        int n = S.scene_rec.n < GFX_MAX_CMDS ? S.scene_rec.n : GFX_MAX_CMDS;
        for (int i = 0; i < n; i++) {
            const gfx_cmd *q = &S.scene_rec.c[i];
            gfx_rec_cmd(fb, q->x0, q->y0, q->x1, q->y1, q->hash);
        }
        if (S.scene_rec.overflow) fb->rec->overflow = 1;
    } else if (!orr_snap_draw(fb)) {
        orr_baked_background=1;
        orr_replay(fb);
        orr_baked_background=0;
    }
    if (!c->aod) {
        fk_view v; fk_view_of(fb, &v);
        orr_cat(OP_TEXT);
        if (S.clock_overlay) draw_centre(fb, &v, c);
        orr_cat(OP_CONTROLS);
        if (S.hold && fk_hit(&v, S.hx - 90, S.hy - 122, S.hx + 90, S.hy + 90)) draw_hold(fb);
        /* The normal unread dot's 20px backing erased selected ring text.
         * Preserve its fade as a short rim tick while the dials are open. */
        if(S.ringe>0.85f && S.lockk<0.5f) {
            float unread=fk_unread_level(c);
            if(unread>0) gfx_line(fb,CX+12,3,CX+12,5,2,ui_mix(C_BG,C_PRI,unread));
        } else fk_unread(fb, c, C_PRI, C_BG, 0, 0);
    }
    if(!fb->rec) orr_prof_stop();
}

/* ---------------------------------------------------------------------------- touch */
static int on_zoom(int x, int y)
{
    float r = orr_hypot(x - CX, y - CY), f = phi_of((float)x, (float)y);
    return (1 - S.lockk) * fmaxf(0, 1 - S.ringe * 3) > 0.5f && r >= ZOOM_R - ZOOM_W / 2 - 10 && f >= ZOOM_P0 - 4 && f <= ZOOM_P1 + 4;
}
static int on_handle(int x, int y)
{
    float r = orr_hypot(x - CX, y - CY), f = phi_of((float)x, (float)y);
    return r >= 186 && f >= H_P0 - 8 && f <= H_P1 + 8;
}
/* An opener must start on the physical rim, beside its visible hint. The wider
 * handle hit area remains useful for closing an already open menu. */
static int on_control_edge(int x, int y)
{
    float dx = x - CX, dy = y - CY, r2 = dx * dx + dy * dy;
    return r2 >= CONTROL_EDGE_R * CONTROL_EDGE_R && r2 <= GFX_W * GFX_W * 0.25f;
}
static int controls_closed(void)
{
    /* Keep ownership through closing animations as well: a hidden handle must
     * never receive a touch intended for a still-visible time ring. */
    return !S.rings_open && !S.rings_pull && S.ringk == 0 && S.ringe == 0 &&
           !S.menu_open && !S.menu_pull && S.menuk == 0 && S.menue == 0;
}
static int menu_hit(int x, int y)
{
    int best = -1; float bd = 1e9f;
    for (int k = 0; k < S.nmh; k++) {
        if (!S.mhit[k].kind) continue;
        float d = orr_hypot(x - S.mhit[k].x, y - S.mhit[k].y), lim = S.mhit[k].key < 0 ? 36.0f : 34.0f;
        if (d < lim && d < bd) { bd = d; best = k; }
    }
    return best;
}
static void menu_action(int k)
{
    int m = S.mhit[k].id, j = S.mhit[k].key;
    static const char *const names[4] = { "DRAG", "VIEW", "FILTER", "LINES" };
    char t[40];
    if (j < 0) {
        if (m == M_LINES) { S.lines = !S.lines; S.menu_sub = -1; nos_toast(S.lines ? "LINES ON" : "LINES OFF"); }
        else { S.menu_sub = S.menu_sub == m ? -1 : m; S.subk = 0; if (S.menu_sub >= 0) nos_toast(names[m]); }
        nos_feedback(FB_TICK);
        return;
    }
    static const char *const dn[4] = { "PAN", "LOOK", "TIME", "RECENTRE" }, *const vn[2] = { "OVERHEAD", "TILT" },
                      *const fn[5] = { "PLANETS", "ASTEROIDS", "COMETS", "CRAFT", "FROM EARTH" };
    if (m == M_DRAG) {
        if (j == 3) { S.home_t = 1; S.home_px = S.panx; S.home_py = S.pany; S.home_z = S.zoom; S.zoom = S.zoom; }
        else S.drag_mode = j == 0 ? DR_PAN : j == 1 ? DR_LOOK : DR_TIME;
        snprintf(t, sizeof t, "%s", dn[j]);
    } else if (m == M_VIEW) { S.tilt = j == 1; snprintf(t, sizeof t, "%s", vn[j]); }
    else {
        int on;
        if (j < 4) on = S.show[j] = !S.show[j]; else on = S.from_earth = !S.from_earth;
        snprintf(t, sizeof t, "%s %s", j == 4 ? "NEAREST" : fn[j], on ? "ON" : "OFF");   /* short: the toast holds ~9 caps */
    }
    nos_toast(t); nos_feedback(FB_TICK);
}
static int nearest_planet(int x, int y, float lim)
{
    int best = -1; float bd = lim;
    for (int p = 0; p < EP_N; p++) {
        if (!S.pl_on[p]) continue;
        float d = orr_hypot(x - S.px[p], y - S.py[p]), r = fmaxf(24, S.pr[p] + 12);
        if (d < r && d < bd) { bd = d; best = p; }
    }
    return best;
}
static void tap_unlocked(int x, int y, int target)
{
    float r = orr_hypot(x - S.panx - CX, (y - S.pany - CY) / fmaxf(0.3f, S.ct)) / (S.sc * S.zoom);
    if (target == 1) { int k = menu_hit(x, y); if (k >= 0) menu_action(k); return; }
    if (S.menu_open) { S.menu_open = 0; nos_feedback(FB_TICK); return; }   /* a tap anywhere else folds the menu */
    if (target == 2) {
        if (S.now_pill || S.ringe > 0.5f) {
            S.ta_from = S.off; S.ta_to = 0; S.ta_t = 0; S.ta_n = (int)fminf(24, 10 + (float)fabs(S.off) / 60); S.snapped = -1; S.vel = 0;
        } else orrery_open_events(x, y);
        nos_feedback(FB_TICK);
        return;
    }
    if (target == 3 || target == 7) return;   /* rim hints open only with an inward swipe */
    if (target == 4) { S.rings_open = 0; nos_feedback(FB_TICK); return; }
    if (target == 5) {   /* a spot on the zoom dial: that zoom (near 1x lands on 1x) */
        float f = clampf((ZB_P1 - phi_of((float)x, (float)y)) / (ZB_P1 - ZB_P0), 0, 1), z = ZMIN * powf(ZMAX / ZMIN, f);
        if (fabsf(logf(z)) < 0.12f) z = 1;
        S.home_t = 0;
        float fz = z / S.zoom; S.panx *= fz; S.pany *= fz; S.zoom = z;
        nos_feedback(FB_TICK);
        return;
    }
    if (target == 6) {   /* a value on a ring: fly to it */
        int ri = S.d_ring; float f = n180(phi_of((float)x, (float)y));
        double to = travel_date(S.jd_now + S.off + f / 360.0 * RING_LAP[ri], 1) - S.jd_now;
        S.ta_from = S.off; S.ta_to = to; S.ta_t = 0; S.ta_n = 14; S.snapped = -1; S.vel = 0;
        nos_feedback(FB_TICK);
        return;
    }
    int p = nearest_planet(x, y, 1e9f);
    if (p >= 0) { orrery_open_card(ob_of_planet(p), (int)S.px[p], (int)S.py[p]); return; }
    if (S.pluto_on && orr_hypot(x - S.plx, y - S.ply) < 18) { orrery_open_card(OB_PLUTO, (int)S.plx, (int)S.ply); return; }
    if (zone_mix(V_FACE) < 0.99f && orr_hypot(x - S.sunx, y - S.suny) < 20) { orrery_open_card(OB_SUN, (int)S.sunx, (int)S.suny); return; }
    /* small bodies, comets, craft: a read-out */
    float bd = 12; int kind = 0, idx = -1;
    for (int i = 0; i < S.nsm; i++) if (S.sm_on[i] && orr_hypot(x - S.smx[i], y - S.smy[i]) < bd) { bd = orr_hypot(x - S.smx[i], y - S.smy[i]); kind = 1; idx = i; }
    for (int i = 0; i < S.ncm; i++) if (S.cm_on[i] && orr_hypot(x - S.cmx[i], y - S.cmy[i]) < bd) { bd = orr_hypot(x - S.cmx[i], y - S.cmy[i]); kind = 2; idx = i; }
    for (int i = 0; i < S.ncr; i++) if (S.cr_on[i] && orr_hypot(x - S.crx[i], y - S.cry[i]) < bd) { bd = orr_hypot(x - S.crx[i], y - S.cry[i]); kind = 3; idx = i; }
    if (idx >= 0) {
        char t[64];
        if (kind == 1) {
            const char *nm = od_small_bodies[idx].short_name; while (*nm >= '0' && *nm <= '9') nm++; while (*nm == ' ' || *nm == '(') nm++;
            snprintf(t, sizeof t, "%.14s · %.2f AU", nm, S.sm[idx].au);
            char *q = strchr(t, ')'); if (q) memmove(q, q + 1, strlen(q));
        } else if (kind == 2) {
            char nm[20]; snprintf(nm, sizeof nm, "%s", od_comets[idx].name); char *q = strchr(nm, '('); if (q && q > nm) q[-1] = 0;
            snprintf(t, sizeof t, "%.16s · %.1f AU", nm, S.cm[idx].au);
        } else snprintf(t, sizeof t, "%.14s · %.1f AU", od_crafts[idx].name, S.cr[idx].au);
        for (char *q = t; *q; q++) if (*q >= 'a' && *q <= 'z') *q -= 32;
        nos_toast(t); nos_feedback(FB_TAP);
        return;
    }
    if (S.ringe > 0.5f) return;      /* with the rings open, empty taps don't change zones */
    if (S.view == V_FACE) {
        if (r < 62) orrery_open_card(OB_SUN, CX, CY);
        else if (r < 158) go_view(V_INNER); else if (r < 222) go_view(V_OUTER); else go_view(V_BEYOND);
        return;
    }
    if (y < 44 && abs(x - CX) < 70) { go_view(V_FACE); return; }
    float core = S.view == V_OUTER ? 50 : 30;
    if (r < core) go_view(S.view == V_INNER ? V_FACE : S.view == V_OUTER ? V_INNER : V_OUTER);
    else if (r > 214) go_view(S.view == V_INNER ? V_OUTER : S.view == V_OUTER ? V_BEYOND : V_FACE);
}

/* Hit the drawn band, including the sweep while opening/closing. Keep
 * grip tap ownership separate from the direction chosen at drag start. */
static int ring_band_hit(int x,int y)
{
    if(S.ringe<=.01f)return -1;
    float r=orr_hypot(x-CX,y-CY), phi=phi_of((float)x,(float)y);
    int hit=-1; float best=1e9f;
    for(int i=0;i<3;i++) {
        float e=S.ringe, radius=TAB_R[i]+(RING_R[i]-TAB_R[i])*e;
        float half=(3+(RING_W-3)*e)/2+1;
        int angle=e<.85f ? fabsf(n180(phi-TAB_PHI))<=(28+332*e)/2 :
                             fabsf(n180(phi))<=180-ring_gap(i);
        float d=fabsf(r-radius);
        if(angle && d<=half && d<best) { hit=i;best=d; }
    }
    return hit;
}

/* targets claimed at touch-down: 1 menu, 2 chip / NOW, 3 time tab, 4 grip, 5 zoom dial, 6 ring, 7 handle */
int face_orrery_input(const nos_input *in)
{
    if (!orr_ok()) return 0;
    switch (in->type) {
    case IN_CANCEL:
        S.hold=S.drag=S.time_touch=S.menu_pull=S.rings_pull=S.ring_now_down=0;
        S.d_ring=-1;s_target=s_edge=0;S.pvx=S.pvy=0;S.vel=0;
        return 1;
    case IN_DOWN: {
        int x = in->x, y = in->y;
        S.pvx = S.pvy = 0; S.drag = 0; S.time_touch=0; s_target = s_edge = 0; S.d_ring = -1; S.down_t = nos_t;
        S.menu_pull = S.rings_pull = 0;
        if (!S.locked) {
            float tx, ty, gx, gy;
            polar_xy(TAB_PHI, TAB_R[1], &tx, &ty); polar_xy(TAB_PHI, RING_R[0], &gx, &gy);
            int ring_hit=ring_band_hit(x,y);
            S.d_ring=ring_hit;
            int mh = S.menue > 0.5f && !S.rings_open && S.ringk == 0 && S.ringe == 0 ? menu_hit(x, y) : -1;
            int on_chip = (S.now_pill || S.chip[0]) && S.ringe < 0.5f && zone_mix(V_FACE) > 0.5f &&
                          fabsf((float)x - CX) < fmaxf(60, S.chip_w / 2) && fabsf((float)y - CHIP_Y) < 25;
            int on_now_r = S.ringe > 0.5f && fabs(S.off) > 0.02 && fabsf((float)x - CX) < 40 && fabsf((float)y - (CY + 42)) < 20;
            if (S.ringe > 0.9f) on_now_r = ring_sector_hit(x, y);
            S.ring_now_down = on_now_r;
            if (mh >= 0) s_target = 1;
            else if (on_chip || on_now_r) s_target = 2;
            else if (ring_hit<0 && controls_closed() &&
                     (orr_hypot(x - tx, y - ty) < 34 ||
                      (on_control_edge(x, y) && fabsf(n180(phi_of((float)x, (float)y) - TAB_PHI)) <= 16))) s_target = 3;
            /* Visible bands own the grip's touch slop. The actual 10 px
             * grip retains taps; a tangential drag there still scrubs. */
            else if (S.ringe>.85f && (ring_hit<0 || orr_hypot(x-gx,y-gy)<=10) && orr_hypot(x - gx, y - gy) < 22) s_target = 4;
            else if (on_zoom(x, y)) s_target = 5;
            else if (ring_hit >= 0) { s_target = 6; S.d_ring = ring_hit; S.d_ring_phi = phi_of((float)x, (float)y); }
            else if (!S.rings_open && S.ringk == 0 && S.ringe == 0 && on_handle(x, y)) s_target = 7;
            if ((s_target == 3 || s_target == 7) && controls_closed() && on_control_edge(x, y)) s_edge = s_target;
            S.time_touch=s_target==6 || (s_target==4 && S.d_ring>=0);
            int p = nearest_planet(x, y, 1e9f);
            S.grab = p >= 0 ? p : S.view == V_OUTER ? EP_JUPITER : S.view == V_BEYOND ? EP_NEPTUNE : EP_EARTH;
            S.ta_n = 0; S.vel = 0;
        }
        if (s_target == 0 || s_target == 7) { S.hold = s_target == 0; S.hold_t0 = nos_t; S.hx = in->x; S.hy = in->y; S.hold_stage = 0; }
        else S.hold = 0;
        return 1;
    }
    case IN_LONG:
        return 1;                       /* the picker waits for 3 s, not 0.45 */
    case IN_UP: {
        S.time_touch=0;
        S.ring_now_down = 0;
        s_edge = 0;
        int was = S.hold; S.hold = 0;
        if (was) {
            float h = nos_t - S.hold_t0;
            if (h >= HOLD_LOCK) lock_now(!S.locked);
            else if (h >= HOLD_TILT) { S.tilt = !S.tilt; nos_feedback(FB_TICK); nos_toast(S.tilt ? "3D" : "FLAT"); }
        }
        /* Normal releases commit in DRAG_END. A lone UP cancels ownership and
         * must not activate a half-pulled control (e.g. when another screen wins). */
        if (S.drag == 7) { S.menu_pull = 0; S.menu_open = 0; }
        if (S.drag == 8) { S.menu_pull = 0; S.menu_open = 1; }
        if (S.drag == 6) { S.rings_pull = 0; S.rings_open = S.d_wasopen; }
        S.drag = 0;
        return !S.locked;
    }
    case IN_TAP:
        S.hold = 0;
        if (S.locked) return 0;         /* home calls the face's tap(): the unlock hint */
        if (nos_t - S.down_t > 0.45f) return 1;    /* a long press is never a late tap */
        tap_unlocked(in->x, in->y, s_target);
        return 1;
    case IN_DRAG_START: {
        S.hold = 0;
        if (S.locked) return 0;         /* locked: swipes page and pull the sheets as on any face */
        float mx = in->dx, my = in->dy, r0 = orr_hypot(in->x0 - CX, in->y0 - CY);
        float ux = r0 > 0 ? (in->x0 - CX) / r0 : 0, uy = r0 > 0 ? (in->y0 - CY) / r0 : 0;
        float radial = mx * ux + my * uy, tang = fabsf(mx * uy - my * ux);
        int inward = radial < 0 && -radial > tang, hx = fabsf(mx) > fabsf(my);
        int edge_back = r0 >= EDGE_R && inward && mx > 0 && in->x0 < CX;
        int handle = on_handle(in->x0, in->y0);
        S.d_px = S.panx; S.d_py = S.pany; S.d_a = phi_of((float)in->x, (float)in->y);
        S.hx = in->x; S.hy = in->y;
        if(s_target==6 || (s_target==4 && S.d_ring>=0 && tang>=fabsf(radial))) {
            S.drag=4; S.rings_open=1; S.rings_pull=0;
        }
        else if (s_edge == 7 && controls_closed() && inward && mx < 0 && hx) { S.drag = 7; S.menu_pull = 1; }
        else if (s_edge == 3 && controls_closed() && inward && mx < 0 && my > 0) {
            S.drag = 6; S.rings_pull = 1; S.d_wasopen = 0;
            S.d_px = ux; S.d_py = uy;   /* opening progress follows the inward pull, not total travel */
        }
        else if (S.menu_open && mx > 0 && hx && (s_target == 1 || handle)) { S.drag = 8; S.menu_pull = 1; }
        else if (s_target == 4) { S.drag = 6; S.rings_pull = 1; S.d_wasopen = 1; }
        else if (s_target == 5) S.drag = 2;
        else if (s_target) S.drag = 9;   /* claimed by a button: the drag goes nowhere */
        else if (edge_back && S.view != V_FACE) S.drag = 10;
        else if (S.drag_mode == DR_TIME) S.drag = 5;
        else if (S.drag_mode == DR_LOOK && S.tilt) S.drag = 3;
        else S.drag = 1;
        if (S.drag == 1) S.home_t = 0;
        return 1;
    }
    case IN_DRAG: {
        if (S.locked || !S.drag) return 0;
        float a = phi_of((float)in->x, (float)in->y), da = n180(a - S.d_a);
        S.d_a = a;
        switch (S.drag) {
        case 1: {
            float lim = 210 * S.zoom;
            S.panx = clampf(S.d_px + in->dx, -lim, lim); S.pany = clampf(S.d_py + in->dy, -lim, lim);
            break;
        }
        case 2: {   /* the zoom dial: toward 4:30 zooms in; its 90 degrees span 0.5x to 4x */
            float z = clampf(S.zoom * powf(ZMAX / ZMIN, -da / (ZB_P1 - ZB_P0)), ZMIN, ZMAX);
            if (fabsf(logf(z)) < 0.035f) z = 1;
            static const float marks[7] = { 0.5f, 0.75f, 1, 1.5f, 2, 3, 4 };   /* a tick under the finger at every mark */
            for (int i = 0; i < 7; i++) if ((S.zoom - marks[i]) * (z - marks[i]) < 0) nos_feedback(FB_TICK);
            float fz = z / S.zoom; S.panx *= fz; S.pany *= fz; S.zoom = z;
            break;
        }
        case 3: S.az += (float)(in->x - S.hx) * 0.4f; S.azv = (float)(in->x - S.hx) * 0.4f; S.cam_tilt = clampf(S.cam_tilt - (in->y - S.hy) * 0.35f, 10, 85); break;
        case 4: case 5: {   /* time: a ring (its lap) or the face (the grabbed planet's pace) */
            if (S.drag == 5 && orr_hypot(in->x - CX, in->y - CY) < 36) break;   /* near the centre a twitch is a huge angle */
            double lap = S.drag == 4 ? RING_LAP[S.d_ring] : ep_period_days[S.grab];
            double days = -da / 360.0 * lap;   /* counter-clockwise = forward: the planets' own direction */
            civ before = civil(S.jd);
            S.off += days; S.vel = days; travel_limit();
            civ after = civil(S.jd);
            int unit_changed = S.drag == 4 ? (S.d_ring == 0 ? before.y != after.y : S.d_ring == 1 ? before.m != after.m : before.d != after.d) : before.m != after.m;
            if (unit_changed) nos_feedback(FB_TICK);
            if (S.drag == 5 || S.d_ring == 2) snap_check(1);
            break;
        }
        case 6: {
            float distance = S.d_wasopen ? orr_hypot((float)in->dx, (float)in->dy) :
                             -(in->dx * S.d_px + in->dy * S.d_py);
            float k = clampf(distance / 150.0f, 0, 1);
            S.ringk = S.d_wasopen ? 1 - k : k;
            break;
        }
        case 7: S.menuk = clampf(-(float)in->dx / 140.0f, 0, 1); break;
        case 8: S.menuk = clampf(1 - (float)in->dx / 140.0f, 0, 1); break;
        }
        S.hx = in->x; S.hy = in->y;
        return 1;
    }
    case IN_DRAG_END:
        S.time_touch=0;
        s_edge = 0;
        if (S.locked || !S.drag) return 0;
        if (S.drag == 1) { S.pvx = in->vx / NOS_FPS; S.pvy = in->vy / NOS_FPS; }
        if (S.drag == 4 || S.drag == 5) { if (S.snapped >= 0) S.vel = 0; else S.vel *= 0.8; }
        if (S.drag == 6) { S.rings_pull = 0; S.rings_open = S.ringk > 0.4f; nos_feedback(FB_TICK); }
        if (S.drag == 7) { S.menu_pull = 0; S.menu_open = S.menuk > 0.4f || -in->vx > 600; nos_feedback(FB_TICK); }
        if (S.drag == 8) { S.menu_pull = 0; S.menu_open = S.menuk > 0.75f && in->vx < 600; nos_feedback(FB_TICK); }
        if (S.drag == 10 && (in->dx > 90 || in->vx > 600)) go_view(V_FACE);
        S.drag = 0;
        return 1;
    case IN_BTN:
        if (S.locked) return 0;
        lock_now(1);                    /* unlocked: the button locks before anything else */
        return 1;
    default:
        return 0;
    }
}

static const struct nos_screen *tap(int x, int y)
{
    (void)x; (void)y;
    if (orr_ok() && S.locked) nos_toast("HOLD 1.5 S TO UNLOCK");
    return 0;
}

const nos_face face_orrery = { "ORRERY", enter, tick, draw, tap };
void orrery_cache_trim(void)
{
    if (!s_orr) return;
    /* Keep published coverage and scene commands: returning from events must not
     * rebuild the sky on its first moving frame. Only sorting scratch is idle. */
    if (S.orbit_pixels) {
        hal_big_free(S.orbit_pixels->scratch); S.orbit_pixels->scratch = 0;
    }
}
void orrery_cache_card_prepare(void)
{
    orrery_cache_trim();
    if (!s_orr || !S.orbit_pixels) return;
    orr_snap_free();
    /* Coverage/scene stay published for outgoing home; ink is only an accelerator. */
    hal_big_free(S.orbit_pixels->ink);
    S.orbit_pixels->ink = 0;
    S.orbit_pixels->ink_key = 0;
}
void orrery_cache_idle(void)
{
    if (!nos_transitioning() && nos_t - orr_last_draw >= 5.0f) orrery_cache_trim();
}

/* ---- orrery.h: shared with the planet card and the events list ---------------------------- */
const char *const ob_name[OB_N] = { "Sun", "Mercury", "Venus", "Earth", "Moon", "Mars", "Jupiter", "Saturn", "Uranus", "Neptune", "Pluto" };
const uint16_t ob_tint[OB_N] = {
    GFX_RGB(0xff, 0xd2, 0x7a), GFX_RGB(0xb5, 0xae, 0xa4), GFX_RGB(0xe8, 0xd6, 0xa8), GFX_RGB(0x7f, 0xb2, 0xff), GFX_RGB(0xcf, 0xcf, 0xcf),
    GFX_RGB(0xe0, 0x7a, 0x4f), GFX_RGB(0xe3, 0xc4, 0x9b), GFX_RGB(0xe8, 0xd4, 0x9c), GFX_RGB(0xa8, 0xe3, 0xe8), GFX_RGB(0x6f, 0x8c, 0xff),
    GFX_RGB(0xd9, 0xc2, 0xa5) };
int ob_ephem(int ob)
{
    static const int map[OB_N] = { EPB_SUN, EP_MERCURY, EP_VENUS, EP_EARTH, EPB_MOON, EP_MARS, EP_JUPITER, EP_SATURN, EP_URANUS, EP_NEPTUNE, EPB_PLUTO };
    return ob >= 0 && ob < OB_N ? map[ob] : EPB_SUN;
}
int ob_of_planet(int ep)
{
    static const int map[EP_N] = { OB_MERCURY, OB_VENUS, OB_EARTH, OB_MARS, OB_JUPITER, OB_SATURN, OB_URANUS, OB_NEPTUNE };
    return ep >= 0 && ep < EP_N ? map[ep] : OB_SUN;
}
void orrery_report_state(void)
{
    /* Query must not call orr_ok(): that initializer allocates the face. */
    if (!s_orr) { printf("ORRERY_STATE initialized=0\n"); return; }
    printf("ORRERY_STATE initialized=1 locked=%d view=%d pan_x=%.3f pan_y=%.3f zoom=%.3f rings=%d menu=%d offset_days=%.6f\n",
           S.locked, S.view, S.panx, S.pany, S.zoom, S.rings_open, S.menu_open, S.off);
}
double orrery_jd(void) { if (!orr_ok()) return od_pack_jd; return travel_date((S.jd_now ? S.jd_now : od_pack_jd) + S.off, 0); }
double orrery_now(void) { if (!orr_ok()) return od_pack_jd; return S.jd_now ? S.jd_now : od_pack_jd; }
void orrery_jump(double jd)
{
    if (!orr_ok()) return;
    S.ta_from = S.off; S.ta_to = travel_date(jd, 1) - S.jd_now; S.ta_t = 0; S.vel = 0;
    S.ta_n = (int)fminf(30, 14 + (float)fabs(S.ta_to - S.off) / 40.0f);
    S.snapped = -1;
}
ep_vec orrery_pluto(double jd)
{
    for (int i = 0; i < od_small_n; i++)
        if (strstr(od_small_bodies[i].name, "Pluto")) return ep_conic_pos(&od_small_bodies[i].el, jd);
    ep_vec z = { 0, 0, 0 };
    return z;
}
const ep_event *orrery_events(int *n) { if (!orr_ok()) { *n = 0; return 0; } *n = S.nev; return S.ev; }
void orrery_events_step(void)
{
    if (!orr_ok()) return;
    if (!S.scan_start) { S.jd = orrery_jd(); events_begin(); }
    events_scan_step();
}
unsigned orrery_events_generation(void) { return s_orr ? S.events_generation : 0; }
int orrery_events_progress(void)
{
    if (!s_orr || !S.scan_start) return 0;
    return !S.scanning ? 100 : S.scan.i * 100 / (S.scan.days + 1);
}
