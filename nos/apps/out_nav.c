/* out_nav.c — Navigate (u-blox M10 GPS on the strap), outdoor team.
 *
 * States: module absent; searching (sky plot: satellites as dots on rings, size and
 * brightness by SNR, "[ SEARCHING 4/12 ]"); fixed: three pages in a horizontal pager
 *   1 NOW     speed, coordinates, altitude, accuracy, Start / Stop track recording
 *   2 TRACK   the recorded path as a dot polyline scaled to fit, you = red dot
 *   3 BACK    an arrow pointing to the track's start (rotated by the compass when
 *             present, else by the course over ground) and the distance.
 * Track points live in PSRAM; Stop writes a GPX to the SD card (watch/tracks/NNNN.gpx).
 * The hal_gps struct has no satellite azimuth/elevation, so the sky plot places each
 * satellite at a stable hashed position on its ring (see the team report). */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include <sys/stat.h>
#define TRACK_ROOT "/sdcard/watch"
#define MKDIR(p) mkdir((p), 0777)
#else                                  /* simulator: under the repo's build output */
#include <direct.h>
#define TRACK_ROOT "build/nos_out"
#define MKDIR(p) _mkdir(p)
#endif
#define TRACK_DIR TRACK_ROOT "/tracks"

#define PI_F    3.14159265f
#define D2R     0.017453292519943295
#define EARTH_R 6371000.0
#define NPAGE   3
#define PAGE_W  466.0f
#define MAXPTS  4096
#define MAXDOTS 360
#define SKY_CY  222.0f

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

/* ------------------------------------------------------------- state */
typedef struct { double lat, lon; float alt; uint32_t epoch; } tpt;

static struct {
    int present;
    hal_gps g;
    int fixed;
    pager pg;
    float build_t[NPAGE];
    float fix_t;                 /* when the fix arrived (page build-in) */
    float sat_d[16];             /* displayed satellite dot size (grows in) */
    int pressed;
    /* track (points persist across visits until a new track starts) */
    tpt *pts; int npts;
    int rec;
    uint32_t rec_ms0, rec_ms;
    double dist_m;
    int saved_state;             /* 0 none, 1 saved, 2 failed */
    float saved_t; int saved_no;
    /* track view */
    float (*dots)[2]; int ndots;
    unsigned track_ver;
    float you_x, you_y; int have_you;
    float start_x, start_y;
    float scale_m;               /* metres per 100 px, for the scale label */
    /* back to start */
    double home_dist, home_brg;
    mspring arrow;               /* unwrapped arrow angle, degrees on screen */
    float arrow_q;
    int head_src;                /* 0 north up, 1 compass, 2 course */
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
static void draw_absent(gfx_fb *fb, const char *title, int icon, const char *what, const char *part)
{
    ui_title(fb, title, 0);
    dot_icon(fb, icon, UI_CX, 176, 6.0f, 4.8f, C_DIS);
    ui_text(fb, UI_F_TITLE, UI_CX, 276, "Not connected", C_PRI, UI_CENTER, 0);
    ui_text_wrap(fb, UI_F_BODY, UI_CX, 316, 330, 30, 2, what, C_SEC, UI_CENTER);
    ui_label(fb, UI_CX, 404, part, C_DIS, UI_CENTER);
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

/* ------------------------------------------------------------- GPX */
static void iso_time(char *o, int n, uint32_t ep)
{
    /* civil from days (Howard Hinnant) */
    long z = (long)(ep / 86400) + 719468, era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long y = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153, d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    unsigned s = ep % 86400;
    snprintf(o, (size_t)n, "%04ld-%02u-%02uT%02u:%02u:%02uZ", y, m, d, s / 3600, s / 60 % 60, s % 60);
}

static int write_gpx(void)
{
    if (!N.pts || N.npts < 2) return 0;
    MKDIR(TRACK_ROOT);
    MKDIR(TRACK_DIR);
    char path[64];
    int no = 0;
    for (int i = 1; i < 10000; i++) {
        snprintf(path, sizeof path, TRACK_DIR "/%04d.gpx", i);
        FILE *t = fopen(path, "r");
        if (!t) { no = i; break; }
        fclose(t);
    }
    if (!no) return 0;
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<gpx version=\"1.1\" creator=\"NOS\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
               "<trk><name>Track %04d</name><trkseg>\n", no);
    char ts[40];
    for (int i = 0; i < N.npts; i++) {
        const tpt *p = &N.pts[i];
        fprintf(f, "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.1f</ele>", p->lat, p->lon, (double)p->alt);
        if (p->epoch) { iso_time(ts, sizeof ts, p->epoch); fprintf(f, "<time>%s</time>", ts); }
        fprintf(f, "</trkpt>\n");
    }
    fprintf(f, "</trkseg></trk>\n</gpx>\n");
    int ok = !ferror(f);
    fclose(f);
    N.saved_no = no;
    return ok;
}

/* ------------------------------------------------------------- track */
static void rebuild_track_view(void)
{
    N.ndots = 0; N.have_you = 0;
    if (!N.pts || N.npts < 1) return;
    if (!N.dots) N.dots = (float (*)[2])hal_big_alloc(MAXDOTS * 2 * sizeof(float));
    if (!N.dots) return;
    /* equirectangular metres around the first point */
    double la0 = N.pts[0].lat, lo0 = N.pts[0].lon, k = cos(la0 * D2R) * D2R * EARTH_R, kl = D2R * EARTH_R;
    double minx = 1e18, maxx = -1e18, miny = 1e18, maxy = -1e18;
    for (int i = 0; i < N.npts; i++) {
        double x = (N.pts[i].lon - lo0) * k, y = (N.pts[i].lat - la0) * kl;
        minx = x < minx ? x : minx; maxx = x > maxx ? x : maxx;
        miny = y < miny ? y : miny; maxy = y > maxy ? y : maxy;
    }
    if (N.fixed) {
        double x = (N.g.lon - lo0) * k, y = (N.g.lat - la0) * kl;
        minx = x < minx ? x : minx; maxx = x > maxx ? x : maxx;
        miny = y < miny ? y : miny; maxy = y > maxy ? y : maxy;
    }
    double span = maxx - minx > maxy - miny ? maxx - minx : maxy - miny;
    if (span < 60) span = 60;
    const double box = 236.0;                       /* px, fits the circle with margin */
    double s = box / span, cx = (minx + maxx) / 2, cy = (miny + maxy) / 2;
    N.scale_m = (float)(100.0 / s);
    const float sx = UI_CX, sy = UI_CY - 8;
    /* walk the polyline and drop a dot every `step` px */
    double len = 0;
    for (int i = 1; i < N.npts; i++) {
        double ax = (N.pts[i - 1].lon - lo0) * k, ay = (N.pts[i - 1].lat - la0) * kl;
        double bx = (N.pts[i].lon - lo0) * k, by = (N.pts[i].lat - la0) * kl;
        len += sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay)) * s;
    }
    float step = 9.0f;
    if (len / step > MAXDOTS - 2) step = (float)(len / (MAXDOTS - 2));
    float carry = 0;
    for (int i = 0; i < N.npts; i++) {
        float bx = (float)(sx + ((N.pts[i].lon - lo0) * k - cx) * s);
        float by = (float)(sy - ((N.pts[i].lat - la0) * kl - cy) * s);
        if (i == 0) {
            N.start_x = bx; N.start_y = by;
            N.dots[N.ndots][0] = bx; N.dots[N.ndots][1] = by; N.ndots++;
            continue;
        }
        float ax = (float)(sx + ((N.pts[i - 1].lon - lo0) * k - cx) * s);
        float ay = (float)(sy - ((N.pts[i - 1].lat - la0) * kl - cy) * s);
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
    if (N.fixed) {
        N.you_x = floorf((float)(sx + ((N.g.lon - lo0) * k - cx) * s) + 0.5f);
        N.you_y = floorf((float)(sy - ((N.g.lat - la0) * kl - cy) * s) + 0.5f);
        N.have_you = 1;
    }
    N.track_ver++;
}

static void rec_start(void)
{
    if (!N.pts) N.pts = (tpt *)hal_big_alloc(MAXPTS * sizeof(tpt));
    if (!N.pts) { nos_feedback(FB_ERROR); return; }
    N.npts = 0; N.dist_m = 0;
    N.rec = 1; N.rec_ms0 = hal_millis(); N.rec_ms = 0;
    N.saved_state = 0;
    nos_feedback(FB_TOGGLE_ON);
}
static void rec_stop(void)
{
    N.rec = 0;
    int ok = write_gpx();
    N.saved_state = ok ? 1 : 2;
    N.saved_t = nos_t;
    nos_feedback(ok ? FB_SUCCESS : FB_ERROR);
}
static void rec_add(void)
{
    if (!N.rec || !N.fixed || !N.pts) return;
    if (N.npts > 0) {
        const tpt *l = &N.pts[N.npts - 1];
        double d = hav_m(l->lat, l->lon, N.g.lat, N.g.lon);
        if (d < 5.0 || d > 500.0) return;          /* 5 m spacing; reject wild jumps */
        N.dist_m += d;
    }
    if (N.npts >= MAXPTS) {                        /* full: keep every other point */
        for (int i = 0; i < MAXPTS / 2; i++) N.pts[i] = N.pts[i * 2];
        N.npts = MAXPTS / 2;
    }
    tpt *p = &N.pts[N.npts++];
    p->lat = N.g.lat; p->lon = N.g.lon; p->alt = N.g.alt_m;
    p->epoch = (uint32_t)hal_epoch_seconds();
    rebuild_track_view();
}

/* ------------------------------------------------------------- lifecycle */
static void nav_enter(void)
{
    N.pressed = 0;
    N.fixed = 0;
    N.fix_t = -10;
    int page = N.pg.n ? N.pg.page : 0;
    pg_init(&N.pg, NPAGE, page);
    for (int i = 0; i < NPAGE; i++) N.build_t[i] = nos_t;
    memset(N.sat_d, 0, sizeof N.sat_d);
    msp_init(&N.arrow, 0, 0.32f, 0.9f);
    hal_gps_enable(1);
}
static void nav_leave(void) { hal_gps_enable(0); }

static void nav_tick(float dt)
{
    N.present = hal_gps_get(&N.g) && (hal_ext_present() & EXT_GPS);
    if (!N.present) return;
    hal_gps_enable(1);
    int was = N.fixed;
    N.fixed = N.g.valid;
    if (N.fixed && !was) { N.fix_t = nos_t; N.build_t[N.pg.page] = nos_t; nos_feedback(FB_SUCCESS); }
    if (!N.fixed && was) N.build_t[0] = nos_t;
    /* satellites grow in / out */
    for (int i = 0; i < 16; i++) {
        float t = i < N.g.nsnr && N.g.snr[i] ? 5.0f + N.g.snr[i] * 0.28f : 0.0f;
        N.sat_d[i] += (t - N.sat_d[i]) * m_clamp(dt * 6.0f, 0, 1);
        if (fabsf(t - N.sat_d[i]) < 0.15f) N.sat_d[i] = t;
        N.sat_d[i] = floorf(N.sat_d[i] * 4 + 0.5f) / 4;
    }
    if (N.rec) N.rec_ms = hal_millis() - N.rec_ms0;
    static uint32_t last_view_ms;
    if (N.fixed) {
        rec_add();
        /* the "you" dot follows even when not recording; refresh ~1 Hz */
        if (N.npts && hal_millis() - last_view_ms > 1000) { rebuild_track_view(); last_view_ms = hal_millis(); }
    }
    if (pg_tick(&N.pg, dt)) N.build_t[N.pg.page] = nos_t;
    /* back to start */
    if (N.fixed && N.npts) {
        N.home_dist = hav_m(N.g.lat, N.g.lon, N.pts[0].lat, N.pts[0].lon);
        N.home_brg = bearing_deg(N.g.lat, N.g.lon, N.pts[0].lat, N.pts[0].lon);
        float head = 0;
        hal_mag m;
        if (hal_mag_get(&m) && m.valid && m.calibrated) { head = m.heading_deg; N.head_src = 1; }
        else if (N.g.speed_mps > 0.8f) { head = N.g.course_deg; N.head_src = 2; }
        else N.head_src = 0;
        float want = (float)N.home_brg - head;
        float d = wrap180(want - N.arrow.target);
        if (fabsf(d) > 0.5f) msp_to(&N.arrow, N.arrow.target + d);
        msp_step(&N.arrow, dt);
        N.arrow_q = floorf(N.arrow.x * 2 + 0.5f) / 2;
    }
    nos_keep_awake();
}

/* ------------------------------------------------------------- sky plot */
static void sky_draw(gfx_fb *fb)
{
    ui_title(fb, "NAVIGATE", 0);
    float b = m_clamp((nos_t - N.build_t[0]) / 0.35f, 0, 1);
    ui_circle_line(fb, UI_CX, SKY_CY, 128, 2, C_LINE2);
    ui_circle_line(fb, UI_CX, SKY_CY, 84, 2, C_LINE);
    ui_circle_line(fb, UI_CX, SKY_CY, 40, 2, C_LINE);
    dot_disc(fb, UI_CX, SKY_CY, 9.0f * b, C_WHITE);              /* you */
    for (int i = 0; i < 16; i++) {
        float d = N.sat_d[i] * b;
        if (d <= 0.2f) continue;
        /* stable pseudo sky position per slot: azimuth by hash, ring by elevation hash */
        float az = m_hash01((unsigned)i, 11u) * 2 * PI_F;
        float rr = 30.0f + m_hash01((unsigned)i, 12u) * 92.0f;
        float x = UI_CX + gfx_fast_sin(az) * rr, y = SKY_CY - gfx_fast_cos(az) * rr;
        /* brightness by signal: in use = white, others ramp from grey with SNR */
        int used = i < N.g.sats_used;
        float k = m_clamp((N.g.snr[i] - 12) / 30.0f, 0, 1);
        k = floorf(k * 8 + 0.5f) / 8;
        dot_disc(fb, x, y, d, used ? C_WHITE : ui_mix(C_LINE2, C_SEC, k));
    }
    char t[32];
    snprintf(t, sizeof t, "SEARCHING %d/%d", N.g.sats_used, N.g.sats_view);
    ui_state(fb, UI_CX, 392, t, C_SEC);
    if (N.rec) {
        dot_disc(fb, UI_CX - 44, 98, 8, C_RED);
        ui_label(fb, UI_CX - 32, 104, "REC", C_SEC, UI_LEFT);
    }
}

/* ------------------------------------------------------------- pages */
static float build_of(int i)
{
    float t0 = N.build_t[i] > N.fix_t ? N.build_t[i] : N.fix_t;
    return m_clamp((nos_t - t0) / 0.36f, 0, 1);
}

/* red = recording, but only where nothing else claims the screen's one red */
static void header(gfx_fb *fb, int red)
{
    char t[40];
    if (N.rec) {
        char d[16];
        ui_fmt_dur(d, sizeof d, N.rec_ms, 0);
        if (N.dist_m < 1000) snprintf(t, sizeof t, "REC %s  %d M", d, (int)N.dist_m);
        else snprintf(t, sizeof t, "REC %s  %.2f KM", d, N.dist_m / 1000.0);
        int w = ui_text_width(UI_F_LABEL, t, 2) + 18;
        dot_disc(fb, UI_CX - w / 2 + 4, 64, 8, red ? C_RED : C_PRI);
        ui_label(fb, UI_CX - w / 2 + 18, 70, t, C_SEC, UI_LEFT);
    } else {
        snprintf(t, sizeof t, "%s FIX  %d SATS", N.g.fix3d ? "3D" : "2D", N.g.sats_used);
        ui_label(fb, UI_CX, 70, t, C_SEC, UI_CENTER);
    }
}

static void page_now(gfx_fb *fb)
{
    header(fb, 1);
    float b = build_of(0);
    int metric = nos_prefs.units_metric;
    float v = N.g.speed_mps * (metric ? 3.6f : 2.23694f);
    char t[40];
    snprintf(t, sizeof t, "%.1f", (double)(floorf(v * 10 + 0.5f) / 10));
    dot_style st = { DOT_FONT_TALL, 9.0f, 7.2f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, 100, t, 1, b, DOT_REVEAL_RANDOM, 5u);
    ui_label(fb, UI_CX, 210, metric ? "KM/H" : "MPH", C_SEC, UI_CENTER);
    snprintf(t, sizeof t, "%.5f %c", fabs(N.g.lat), N.g.lat >= 0 ? 'N' : 'S');
    ui_text(fb, UI_F_LABEL, UI_CX, 256, t, C_PRI, UI_CENTER, 1);
    snprintf(t, sizeof t, "%.5f %c", fabs(N.g.lon), N.g.lon >= 0 ? 'E' : 'W');
    ui_text(fb, UI_F_LABEL, UI_CX, 282, t, C_PRI, UI_CENTER, 1);
    snprintf(t, sizeof t, "ALT %d M   HDOP %.1f", (int)lroundf(N.g.alt_m), (double)N.g.hdop);
    ui_label(fb, UI_CX, 318, t, C_DIS, UI_CENTER);
    if (!N.rec && N.saved_state && nos_t - N.saved_t < 2.5f) {
        if (N.saved_state == 1) { snprintf(t, sizeof t, "SAVED %04d", N.saved_no); ui_state(fb, UI_CX, 382, t, C_PRI); }
        else ui_state(fb, UI_CX, 382, "NO SD CARD", C_SEC);
        return;
    }
    ui_button(fb, UI_CX, 374, 176, 60, N.rec ? "Stop" : "Start track", N.rec ? BTN_SECONDARY : BTN_PRIMARY,
              N.pressed == P_REC);
}

static void page_track(gfx_fb *fb)
{
    float b = build_of(1);
    if (!N.npts) {
        ui_title(fb, "TRACK", 0);
        dot_icon(fb, DOT_ICON_ROUTE, UI_CX, 196, 6.0f, 4.8f, C_DIS);
        ui_state(fb, UI_CX, 290, "NO TRACK", C_SEC);
        ui_text_wrap(fb, UI_F_BODY, UI_CX, 330, 280, 30, 2, "Start one on the first page", C_DIS, UI_CENTER);
        return;
    }
    header(fb, 0);
    /* the path: one damage command keyed by the track version */
    int nd = (int)(N.ndots * b + 0.5f);
    if (fb->rec) {
        gfx_rec_cmd(fb, UI_CX - 132, UI_CY - 140, UI_CX + 132, UI_CY + 124,
                    gfx_hash_u(gfx_hash_u(0x7A4Cu, N.track_ver), (unsigned)nd));
    } else {
        for (int i = 1; i < nd; i++)
            if (rows_hit(fb, N.dots[i][1] - 4, N.dots[i][1] + 4))
                dot_disc(fb, N.dots[i][0], N.dots[i][1], 5.0f, C_PRI);
    }
    ui_circle_line(fb, N.start_x, N.start_y, 8, 2.5f, C_WHITE);       /* start */
    if (N.have_you) dot_disc(fb, N.you_x, N.you_y, 13.0f * b, C_RED); /* you, now */
    char t[24];
    float m = N.scale_m;
    if (m >= 1000) snprintf(t, sizeof t, "%.1f KM", (double)(m / 1000));
    else snprintf(t, sizeof t, "%d M", (int)(lroundf(m / 10) * 10));
    /* scale bar: 100 px */
    ui_segments(fb, UI_CX - 50, 380, 100, 4, 1, 1, C_DIS, 0);
    ui_text(fb, UI_F_SMALL, UI_CX, 402, t, C_DIS, UI_CENTER, 1);
}

static void arrow_draw(gfx_fb *fb, float cx, float cy, float ang_deg, float b)
{
    float a = ang_deg * PI_F / 180.0f, s = gfx_fast_sin(a), c = gfx_fast_cos(a);
    if (fb->rec) {
        gfx_rec_cmd(fb, (int)cx - 84, (int)cy - 84, (int)cx + 84, (int)cy + 84,
                    gfx_hash_f(gfx_hash_f(0xA770u, ang_deg), floorf(b * 32)));
        return;
    }
    if (!rows_hit(fb, cy - 84, cy + 84)) return;
    /* local frame: +v = forward. shaft 5 dots, head as a chevron of 2x3 dots */
    /* the tip is the biggest dot; the head's arms run back from it, the shaft tapers */
    static const signed char pts[][3] = {
        { 0, 64, 13 }, { -15, 49, 10 }, { -30, 34, 9 }, { -45, 19, 8 }, { 15, 49, 10 }, { 30, 34, 9 }, { 45, 19, 8 },
        { 0, 44, 9 }, { 0, 25, 9 }, { 0, 6, 8 }, { 0, -13, 8 }, { 0, -32, 7 }, { 0, -51, 7 },
    };
    int n = (int)(sizeof pts / sizeof pts[0]);
    for (int i = 0; i < n; i++) {
        float u = pts[i][0], v = pts[i][1] - 6.0f;
        float g = m_clamp(b * 2.0f - (float)i / n, 0, 1);
        float x = cx + u * c + v * s, y = cy + u * s - v * c;
        dot_disc(fb, x, y, pts[i][2] * g, C_WHITE);
    }
}

static void page_back(gfx_fb *fb)
{
    ui_title(fb, "BACK TO START", 0);
    float b = build_of(2);
    if (!N.npts) {
        dot_icon(fb, DOT_ICON_PIN, UI_CX, 196, 6.0f, 4.8f, C_DIS);
        ui_state(fb, UI_CX, 290, "NO START POINT", C_SEC);
        ui_text_wrap(fb, UI_F_BODY, UI_CX, 330, 280, 30, 2, "Record a track to find your way back", C_DIS, UI_CENTER);
        return;
    }
    char t[24]; const char *unit;
    if (N.home_dist < 15) {
        dot_icon_anim(fb, DOT_ICON_PIN, UI_CX, 186, 7.0f, 5.6f, C_WHITE, b, DOT_REVEAL_CENTER);
        ui_state(fb, UI_CX, 300, "YOU ARE AT THE START", C_PRI);
        return;
    }
    arrow_draw(fb, UI_CX, 184, N.arrow_q, b);
    if (N.home_dist < 1000) { snprintf(t, sizeof t, "%d", (int)(lround(N.home_dist / 5) * 5)); unit = "METRES"; }
    else { snprintf(t, sizeof t, "%.1f", N.home_dist / 1000.0); unit = "KM"; }
    dot_style st = { DOT_FONT_TALL, 7.0f, 5.6f, C_WHITE, 0, 1, 1 };
    dot_text_anim(fb, &st, UI_CX, 282, t, 1, b, DOT_REVEAL_RANDOM, 4u);
    ui_label(fb, UI_CX, 370, unit, C_SEC, UI_CENTER);
    ui_label(fb, UI_CX, 400, N.head_src == 1 ? "BY COMPASS" : N.head_src == 2 ? "BY COURSE" : "NORTH UP", C_DIS, UI_CENTER);
}

/* ------------------------------------------------------------- draw / input */
static void nav_draw(gfx_fb *fb)
{
    if (!N.present) {
        draw_absent(fb, "NAVIGATE", DOT_ICON_SAT, "Add the GPS for speed and tracks", "M10 GPS  STRAP");
        return;
    }
    float pos = pg_pos(&N.pg);
    for (int i = 0; i < NPAGE; i++) {
        float off = i * PAGE_W - pos;
        if (off <= -PAGE_W || off >= PAGE_W) continue;
        gfx_fb sub;
        if (!nos_subview(fb, &sub, 0, 0, GFX_W, GFX_H, (int)off, 0)) continue;
        if (i == 0) { if (N.fixed) page_now(&sub); else sky_draw(&sub); }
        else if (i == 1) page_track(&sub);
        else if (N.fixed || !N.npts) page_back(&sub);
        else {
            ui_title(&sub, "BACK TO START", 0);
            dot_icon(&sub, DOT_ICON_SAT, UI_CX, 196, 6.0f, 4.8f, C_DIS);
            ui_state(&sub, UI_CX, 290, "WAITING FOR FIX", C_SEC);
        }
    }
    ui_page_dots(fb, UI_CX, 438, NPAGE, pos / PAGE_W);
}

static int on_rec_button(int x, int y)
{
    return N.fixed && N.pg.page == 0 && !N.pg.sp.moving && abs(x - UI_CX) < 96 && abs(y - 374) < 44 &&
           !(N.saved_state && !N.rec && nos_t - N.saved_t < 2.5f);
}

static int nav_input(const nos_input *in)
{
    if (!N.present) return 0;
    if (pg_input(&N.pg, in)) { N.pressed = 0; return 1; }
    switch (in->type) {
    case IN_DOWN: N.pressed = on_rec_button(in->x, in->y) ? P_REC : 0; return 1;
    case IN_UP: N.pressed = 0; return 1;
    case IN_TAP:
        if (on_rec_button(in->x, in->y)) {
            if (N.rec) rec_stop(); else { rec_start(); rec_add(); }
            return 1;
        }
        return 0;
    default: return 0;
    }
}

const nos_screen scr_nav = { "NAVIGATE", nav_enter, nav_leave, nav_tick, nav_draw, nav_input,
                             SCR_HDRAG | SCR_KEEPAWAKE };
