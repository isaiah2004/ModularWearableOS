/* svc_track.c — GPS track recorder service. See svc_track.h. */
#include "svc_track.h"
#include "sports.h"
#include "runtime_admission.h"
#include "diag_log.h"
#include "nos.h"
#include "hal.h"
#include "hal_ext.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
extern int board_sd_asset_acquire(void);
extern void board_sd_asset_release(void);
#ifndef TRACK_ROOT
#define TRACK_ROOT "/sdcard/watch"
#endif
#define MKDIR(p) mkdir((p), 0777)
#else                                  /* simulator: under the repo's build output */
#include <direct.h>
#include <io.h>
#define fsync _commit
#define close _close
#define fdopen _fdopen
#ifndef TRACK_ROOT
#define TRACK_ROOT "build/nos_out"
#endif
#define MKDIR(p) _mkdir(p)
#endif
#define TRACK_DIR TRACK_ROOT "/tracks"
#ifndef O_BINARY
#define O_BINARY 0
#endif

#define MAXPTS  SVC_TRACK_MAXPTS       /* bounded PSRAM points, ~20 km at 5 m spacing */
#define D2R     0.017453292519943295
#define EARTH_R 6371000.0

static struct {
    svc_track_pt *pts;                 /* PSRAM, allocated on the first start */
    int n;
    int active, gap, candidate;
    volatile int pending, saving, save_result;
    hal_gps reacquire;
    uint32_t candidate_ms, candidate_sample_ms, last_point_ms;
    uint32_t t0_ms, elapsed_ms;
    double dist;
    unsigned ver;
    uint32_t last_frame;
    uint32_t session, save_request;
    uint32_t rejected_count, rejected_mask, rejected_age, rejected_report_ms;
} T;

/* UI-owned traces outlive Navigate. Workers publish only their checked result. */
static void track_save_trace(void)
{
    if (!T.save_request || svc_track_saving()) return;
    int result = svc_track_save_result();
    diag_action_result(T.save_request, result > 0 ? DIAG_RESULT_SUCCESS : DIAG_RESULT_FAILED,
                       result > 0 ? result : -EIO);
    T.save_request = 0;
}

static void track_rejected_fix(uint32_t reason, uint32_t age)
{
    if (T.rejected_count < INT32_MAX) T.rejected_count++;
    T.rejected_mask |= reason;
    T.rejected_age = age > INT32_MAX ? INT32_MAX : age;
}

static void track_quality_trace(uint32_t now)
{
    if (!T.rejected_count || now - T.rejected_report_ms < 30000u) return;
    /* GNSS domain + bit mask: unavailable1, invalid2, stale4, range8,
     * quality16, outlier32, unstable reacquisition64. No coordinates. */
    diag_log_event(DIAG_SENSOR_QUALITY, (DIAG_DOMAIN_GNSS << 16) | (int32_t)T.rejected_mask,
                   (int32_t)T.rejected_count, (int32_t)T.rejected_age);
    T.rejected_count = T.rejected_mask = 0;
    T.rejected_report_ms = now;
}

static double hav_m(double la1, double lo1, double la2, double lo2)
{
    double dla = (la2 - la1) * D2R, dlo = (lo2 - lo1) * D2R;
    double a = sin(dla / 2) * sin(dla / 2) + cos(la1 * D2R) * cos(la2 * D2R) * sin(dlo / 2) * sin(dlo / 2);
    a = fmax(0.0, fmin(1.0, a));
    return 2 * EARTH_R * atan2(sqrt(a), sqrt(1 - a));
}

void svc_track_tick(float dt)
{
    (void)dt;
    track_save_trace();
    track_quality_trace(hal_millis());
    if (!T.active) return;
    if (T.last_frame == nos_frame + 1u) return;     /* once per frame */
    T.last_frame = nos_frame + 1u;
    T.elapsed_ms = hal_millis() - T.t0_ms;
    hal_gps_enable(1);                              /* idempotent; keeps the stream alive */
    hal_gps g = {0};
    uint32_t now = hal_millis();
    int available = hal_gps_get(&g);
    uint32_t rejected = !available ? 1u : !g.valid || !T.pts ? 2u : g.age_ms > 3000 ? 4u :
        !isfinite(g.lat) || !isfinite(g.lon) || !isfinite(g.alt_m) ||
        fabs(g.lat) > 90 || fabs(g.lon) > 180 ? 8u :
        !isfinite(g.hdop) || g.hdop < 0 || g.hdop > 10 ? 16u : 0u;
    if (rejected) {
        track_rejected_fix(rejected, available ? g.age_ms : UINT32_MAX);
        T.gap = T.n > 0; T.candidate = 0;
        return;
    }
    int segment = T.n == 0;
    if (T.n && now - T.last_point_ms > 15000) T.gap = 1;
    if (T.gap) {
        if (!T.candidate) { T.reacquire = g; T.candidate_ms = now; T.candidate_sample_ms = now - g.age_ms; T.candidate = 1; return; }
        uint32_t elapsed = now - T.candidate_ms;
        double jump = hav_m(T.reacquire.lat, T.reacquire.lon, g.lat, g.lon);
        if (jump > fmax(35.0, elapsed * 0.08)) {
            track_rejected_fix(64u, g.age_ms);
            T.reacquire = g; T.candidate_ms = now;
            T.candidate_sample_ms = now - g.age_ms;
            return;
        }
        if (elapsed < 1000 || now - g.age_ms == T.candidate_sample_ms) return;
        /* A new segment recovers the anchor without inventing distance in the gap. */
        segment = 1; T.gap = T.candidate = 0;
    }
    if (T.n > 0) {
        const svc_track_pt *l = &T.pts[T.n - 1];
        double d = hav_m(l->lat, l->lon, g.lat, g.lon);
        if (!segment) {
            if (d > 500.0 || d > fmax(35.0, (now - T.last_point_ms) * 0.08)) {
                track_rejected_fix(32u, g.age_ms);
                T.gap = 1; T.reacquire = g; T.candidate_ms = now; T.candidate = 1;
                T.candidate_sample_ms = now - g.age_ms;
                return;
            }
            if (d < 5.0) { T.last_point_ms = now; return; }
            T.dist += d;
        }
    }
    if (T.n >= MAXPTS) {                            /* full: keep every other point */
        if (T.pts[MAXPTS - 1].segment_start) segment = 1;
        for (int i = 0; i < MAXPTS / 2; i++) {
            svc_track_pt p = T.pts[i * 2];
            if (i && T.pts[i * 2 - 1].segment_start) p.segment_start = 1;
            T.pts[i] = p;
        }
        T.n = MAXPTS / 2;
    }
    svc_track_pt *p = &T.pts[T.n++];
    p->lat = g.lat; p->lon = g.lon; p->alt = g.alt_m;
    p->epoch = (uint32_t)hal_epoch_seconds();
    p->segment_start = (uint8_t)segment;
    T.last_point_ms = now;
    T.ver++;
}

void svc_track_start(void)
{
    track_save_trace();
    int blocked = !nos_runtime_recording_allowed() || T.active || svc_track_pending() || svc_track_saving();
    uint32_t session = blocked ? T.session : T.session + 1u;
    if (!blocked && !session) session = 1;
    uint32_t request = diag_action_request(DIAG_DOMAIN_GNSS, DIAG_ACT_START, (int32_t)session);
    if (blocked) { diag_action_result(request, DIAG_RESULT_REJECTED, -EBUSY); return; }
    if (!T.pts) T.pts = (svc_track_pt *)hal_big_alloc(MAXPTS * sizeof(svc_track_pt));
    T.n = 0; T.dist = 0;
    T.gap = T.candidate = 0;
    T.active = T.pts != 0;
    T.session = session;
    T.t0_ms = hal_millis(); T.elapsed_ms = 0;
    T.last_frame = 0;
    T.ver++;
    if (T.active) {
        float event[]={1,1};sports_emit(SP_ROUTINE,event,2);
        hal_gps_enable(1); svc_track_tick(0);
    }
    /* Success means collection is armed, never a claim of a satellite fix. */
    diag_action_result(request, T.active ? DIAG_RESULT_SUCCESS : DIAG_RESULT_FAILED,
                       T.active ? 1 : -ENOMEM);
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
    char path[80], temp[88];
    int no = 0;
    int fd = -1;
    for (int i = 1; i < 10000; i++) {               /* next free number on the card */
        snprintf(path, sizeof path, TRACK_DIR "/%04d.gpx", i);
        struct stat st;
        if (!stat(path, &st)) continue;
        if (errno != ENOENT) return 0;
        snprintf(temp, sizeof temp, "%s.part", path);
        fd = open(temp, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0666);
        if (fd >= 0) { no = i; break; }
        if (errno != EEXIST) return 0;
    }
    if (!no) return 0;
    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); remove(temp); return 0; }
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<gpx version=\"1.1\" creator=\"NOS\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n"
               "<trk><name>Track %04d</name><trkseg>\n", no);
    char ts[40];
    for (int i = 0; i < T.n; i++) {
        const svc_track_pt *p = &T.pts[i];
        if (i && p->segment_start) fprintf(f, "</trkseg><trkseg>\n");
        fprintf(f, "<trkpt lat=\"%.7f\" lon=\"%.7f\"><ele>%.1f</ele>", p->lat, p->lon, (double)p->alt);
        if (p->epoch) { iso_time(ts, sizeof ts, p->epoch); fprintf(f, "<time>%s</time>", ts); }
        fprintf(f, "</trkpt>\n");
    }
    fprintf(f, "</trkseg></trk>\n</gpx>\n");
    int ok = !ferror(f);
    if (fflush(f)) ok = 0;
    if (fsync(fileno(f))) ok = 0;
    if (fclose(f)) ok = 0;
    struct stat st;
    if (ok && (!stat(path, &st) || errno != ENOENT)) ok = 0;
    if (ok && rename(temp, path)) ok = 0;
    if (!ok) remove(temp);
    return ok ? no : 0;
}

static void track_save(void)
{
    int no = 0;
#ifdef ESP_PLATFORM
    int leased = board_sd_asset_acquire();
    if (leased) {
#endif
        if (hal_sports_sd_lock()) { no = write_gpx(); hal_sports_sd_unlock(); }
#ifdef ESP_PLATFORM
        board_sd_asset_release();
    }
#endif
    __atomic_store_n(&T.save_result, no, __ATOMIC_RELAXED);
    if (no > 0) __atomic_store_n(&T.pending, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&T.saving, 0, __ATOMIC_RELEASE); /* publish completion last */
}
#ifdef ESP_PLATFORM
static void track_save_task(void *arg)
{
    (void)arg;
    track_save();
    vTaskDelete(NULL);
}
#endif

int svc_track_stop(void)
{
    track_save_trace();
    uint32_t request = diag_action_request(DIAG_DOMAIN_GNSS,
        T.active ? DIAG_ACT_STOP : DIAG_ACT_SAVE, (int32_t)T.session);
    if (!T.active && !svc_track_pending()) {
        diag_action_result(request, DIAG_RESULT_REJECTED, -ENOENT); return -1;
    }
    if (svc_track_saving()) {
        diag_action_result(request, DIAG_RESULT_REJECTED, -EBUSY); return -2;
    }
    if (T.active) {
        T.elapsed_ms = hal_millis() - T.t0_ms; T.active = 0;
        float event[]={1,0};sports_emit(SP_ROUTINE,event,2);
    }
    if (T.n < 2) {
        diag_action_result(request, DIAG_RESULT_CANCELLED, T.n); return -1;
    }
    __atomic_store_n(&T.pending, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&T.save_result, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&T.saving, 1, __ATOMIC_RELEASE);
    T.save_request = request;
    diag_action_context(request, 1, T.n);
    diag_action_result(request, DIAG_RESULT_PENDING, T.n);
#ifdef ESP_PLATFORM
    /* Frozen points remain owned until completion. SD I/O never runs on the UI task. */
    if (xTaskCreate(track_save_task, "track_save", 4096, NULL, 1, NULL) != pdPASS) {
        __atomic_store_n(&T.saving, 0, __ATOMIC_RELEASE);
        diag_action_result(T.save_request, DIAG_RESULT_FAILED, -ENOMEM);
        T.save_request = 0; return 0;
    }
    return -2;
#else
    track_save();
    track_save_trace();
    return svc_track_save_result();
#endif
}

int svc_track_active(void) { return T.active; }
int svc_track_pending(void) { return __atomic_load_n(&T.pending, __ATOMIC_ACQUIRE); }
int svc_track_saving(void) { return __atomic_load_n(&T.saving, __ATOMIC_ACQUIRE); }
int svc_track_save_result(void) { return __atomic_load_n(&T.save_result, __ATOMIC_ACQUIRE); }
void svc_track_discard(void)
{
    if (!T.active && !svc_track_saving()) {
        track_save_trace();
        if (svc_track_pending()) {
            uint32_t request = diag_action_request(DIAG_DOMAIN_GNSS, DIAG_ACT_CANCEL, (int32_t)T.session);
            diag_action_result(request, DIAG_RESULT_CANCELLED, T.n);
        }
        T.n = 0; T.dist = 0; T.ver++;
        __atomic_store_n(&T.pending, 0, __ATOMIC_RELEASE);
    }
}
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
