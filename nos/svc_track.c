/* svc_track.c — GPS track recorder service. See svc_track.h. */
#include "svc_track.h"
#include "nos.h"
#include "hal.h"
#include "hal_ext.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
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

#define MAXPTS  SVC_TRACK_MAXPTS       /* 96 KB of PSRAM, ~20 km at 5 m spacing */
#define D2R     0.017453292519943295
#define EARTH_R 6371000.0

static struct {
    svc_track_pt *pts;                 /* PSRAM, allocated on the first start */
    int n;
    int active;
    uint32_t t0_ms, elapsed_ms;
    double dist;
    unsigned ver;
    uint32_t last_frame;
} T;

static double hav_m(double la1, double lo1, double la2, double lo2)
{
    double dla = (la2 - la1) * D2R, dlo = (lo2 - lo1) * D2R;
    double a = sin(dla / 2) * sin(dla / 2) + cos(la1 * D2R) * cos(la2 * D2R) * sin(dlo / 2) * sin(dlo / 2);
    return 2 * EARTH_R * atan2(sqrt(a), sqrt(1 - a));
}

void svc_track_tick(float dt)
{
    (void)dt;
    if (!T.active) return;
    if (T.last_frame == nos_frame + 1u) return;     /* once per frame */
    T.last_frame = nos_frame + 1u;
    T.elapsed_ms = hal_millis() - T.t0_ms;
    hal_gps_enable(1);                              /* idempotent; keeps the stream alive */
    hal_gps g;
    if (!hal_gps_get(&g) || !g.valid || !T.pts) return;
    if (T.n > 0) {
        const svc_track_pt *l = &T.pts[T.n - 1];
        double d = hav_m(l->lat, l->lon, g.lat, g.lon);
        if (d < 5.0 || d > 500.0) return;          /* 5 m spacing; reject wild jumps */
        T.dist += d;
    }
    if (T.n >= MAXPTS) {                            /* full: keep every other point */
        for (int i = 0; i < MAXPTS / 2; i++) T.pts[i] = T.pts[i * 2];
        T.n = MAXPTS / 2;
    }
    svc_track_pt *p = &T.pts[T.n++];
    p->lat = g.lat; p->lon = g.lon; p->alt = g.alt_m;
    p->epoch = (uint32_t)hal_epoch_seconds();
    T.ver++;
}

void svc_track_start(void)
{
    if (!T.pts) T.pts = (svc_track_pt *)hal_big_alloc(MAXPTS * sizeof(svc_track_pt));
    T.n = 0; T.dist = 0;
    T.active = T.pts != 0;
    T.t0_ms = hal_millis(); T.elapsed_ms = 0;
    T.last_frame = 0;
    T.ver++;
    if (T.active) { hal_gps_enable(1); svc_track_tick(0); }
}

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
    MKDIR(TRACK_ROOT);
    MKDIR(TRACK_DIR);
    char path[64];
    int no = 0;
#ifdef ESP_PLATFORM
    for (int i = 1; i < 10000; i++) {               /* next free number on the card */
        snprintf(path, sizeof path, TRACK_DIR "/%04d.gpx", i);
        FILE *t = fopen(path, "r");
        if (!t) { no = i; break; }
        fclose(t);
    }
#else
    /* sim: numbered per run from 1 (overwriting), so scripted runs are deterministic */
    static int s_sim_no;
    no = ++s_sim_no;
    snprintf(path, sizeof path, TRACK_DIR "/%04d.gpx", no);
#endif
    if (!no) return 0;
    FILE *f = fopen(path, "w");
    if (!f) return 0;
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<gpx version=\"1.1\" creator=\"NOS\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
               "<trk><name>Track %04d</name><trkseg>\n", no);
    char ts[40];
    for (int i = 0; i < T.n; i++) {
        const svc_track_pt *p = &T.pts[i];
        fprintf(f, "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.1f</ele>", p->lat, p->lon, (double)p->alt);
        if (p->epoch) { iso_time(ts, sizeof ts, p->epoch); fprintf(f, "<time>%s</time>", ts); }
        fprintf(f, "</trkpt>\n");
    }
    fprintf(f, "</trkseg></trk>\n</gpx>\n");
    int ok = !ferror(f);
    fclose(f);
    return ok ? no : 0;
}

int svc_track_stop(void)
{
    if (!T.active) return -1;
    T.elapsed_ms = hal_millis() - T.t0_ms;
    T.active = 0;
    if (T.n < 2) return -1;
    return write_gpx();
}

int svc_track_active(void) { return T.active; }
int svc_track_points(const svc_track_pt **out) { if (out) *out = T.pts; return T.pts ? T.n : 0; }
double svc_track_distance(void) { return T.dist; }
uint32_t svc_track_elapsed_ms(void) { return T.active ? hal_millis() - T.t0_ms : T.elapsed_ms; }
unsigned svc_track_version(void) { return T.ver; }
void svc_track_label(char *out, int n)
{
    uint32_t s = svc_track_elapsed_ms() / 1000;
    if (s >= 3600) snprintf(out, (size_t)n, "REC %u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    else snprintf(out, (size_t)n, "REC %02u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}
