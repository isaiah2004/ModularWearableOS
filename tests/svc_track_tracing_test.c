#include <assert.h>
#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "app_observability_test_log.h"

static int lock_ok = 1, create_ok = 1, update_busy, shutdown_busy;
static void (*worker)(void *);
#define ESP_PLATFORM 1
#define TRACK_ROOT "track_data"
#define mkdir(p,m) _mkdir(p)
#define fsync _commit
#include "../nos/svc_track.c"

uint32_t nos_frame;
static uint32_t clock_ms;
static hal_gps fix;
static int routine_starts;
void sports_emit(int type, const float *v, int n)
{ if (type == SP_ROUTINE && n == 2 && v[1] == 1) routine_starts++; }
void *hal_big_alloc(uint32_t n) { return calloc(1, n); }
uint32_t hal_millis(void) { return clock_ms; }
uint64_t hal_epoch_seconds(void) { return 1792569600u + clock_ms / 1000; }
void hal_gps_enable(int on) { (void)on; }
int hal_gps_get(hal_gps *out) { *out = fix; return 1; }
int hal_sports_sd_lock(void) { return lock_ok; }
void hal_sports_sd_unlock(void) {}
int board_sd_asset_acquire(void) { return 1; }
void board_sd_asset_release(void) {}
int ota_busy(void) { return update_busy; }
int nos_host_shutdown_pending(void) { return shutdown_busy; }
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle)
{
    (void)name; (void)stack; (void)arg; (void)priority; (void)handle;
    if (!create_ok) return 0;
    assert(!worker); worker = fn; return pdPASS;
}
void vTaskDelete(void *handle) { (void)handle; }
static void tick(uint32_t ms)
{ clock_ms = ms; nos_frame++; svc_track_tick(.0333f); }
static void finish_worker(void)
{
    assert(worker);
    void (*fn)(void *) = worker;
    worker = NULL; fn(NULL);
}
static unsigned quality_events(void)
{
    unsigned n = 0;
    for (unsigned i = 0; i < app_test_event_count; i++)
        n += app_test_events[i].code == DIAG_SENSOR_QUALITY;
    return n;
}

int main(void)
{
    fix = (hal_gps){.valid=1, .lat=12.1234567, .lon=77.7654321, .hdop=1};
    svc_track_start();
    uint32_t start = app_test_request(DIAG_DOMAIN_GNSS, DIAG_ACT_START);
    assert(start && app_test_results(start, DIAG_RESULT_SUCCESS) == 1);
    assert(T.n == 1 && routine_starts == 1);
    fix.lon += .0001; tick(1000); assert(T.n == 2);
    assert(svc_track_stop() == -2 && svc_track_saving());
    uint32_t stop = app_test_request(DIAG_DOMAIN_GNSS, DIAG_ACT_STOP);
    assert(stop && app_test_results(stop, DIAG_RESULT_PENDING) == 1);
    assert(!app_test_results(stop, DIAG_RESULT_SUCCESS));
    svc_track_start();
    uint32_t rejected = app_test_request(DIAG_DOMAIN_GNSS, DIAG_ACT_START);
    assert(app_test_results(rejected, DIAG_RESULT_REJECTED) == 1 && T.n == 2);
    unsigned before = app_test_event_count;
    lock_ok = 0; finish_worker();
    assert(app_test_event_count == before && svc_track_pending());
    /* Only the background service ticks: Navigate is closed. */
    tick(1100);
    assert(app_test_results(stop, DIAG_RESULT_FAILED) == 1);
    for (int i = 0; i < 100; i++) tick(1101u + (unsigned)i);
    assert(app_test_results(stop, DIAG_RESULT_FAILED) == 1);
    lock_ok = 1; assert(svc_track_stop() == -2);
    uint32_t retry = app_test_request(DIAG_DOMAIN_GNSS, DIAG_ACT_SAVE);
    assert(retry != stop && app_test_results(retry, DIAG_RESULT_PENDING) == 1);
    finish_worker(); assert(!svc_track_pending() && !app_test_results(retry, DIAG_RESULT_SUCCESS));
    int file = svc_track_save_result(); assert(file > 0);
    tick(2000); assert(app_test_results(retry, DIAG_RESULT_SUCCESS) == 1);
    tick(2100); assert(app_test_results(retry, DIAG_RESULT_SUCCESS) == 1);
    char path[100]; snprintf(path, sizeof path, TRACK_DIR "/%04d.gpx", file);
    FILE *f = fopen(path, "rb"); assert(f);
    char xml[4096]; size_t size = fread(xml, 1, sizeof xml - 1, f); xml[size] = 0;
    assert(!fclose(f) && strstr(xml, "</trkseg></trk>"));

    svc_track_start(); fix.lon += .0001; tick(3000); assert(T.n == 2);
    create_ok = 0; assert(svc_track_stop() == 0 && svc_track_pending());
    stop = app_test_request(DIAG_DOMAIN_GNSS, DIAG_ACT_STOP);
    assert(app_test_results(stop, DIAG_RESULT_FAILED) == 1);
    svc_track_discard();
    uint32_t discard = app_test_request(DIAG_DOMAIN_GNSS, DIAG_ACT_CANCEL);
    assert(app_test_results(discard, DIAG_RESULT_CANCELLED) == 1 && !svc_track_pending());
    update_busy = 1; svc_track_start();
    rejected = app_test_request(DIAG_DOMAIN_GNSS, DIAG_ACT_START);
    assert(!T.active && app_test_results(rejected, DIAG_RESULT_REJECTED) == 1);
    update_busy = 0; shutdown_busy = 1; svc_track_start();
    rejected = app_test_request(DIAG_DOMAIN_GNSS, DIAG_ACT_START);
    assert(!T.active && app_test_results(rejected, DIAG_RESULT_REJECTED) == 1);

    shutdown_busy = 0; svc_track_start(); fix.valid = 0;
    for (uint32_t t = 3010; t < 30000; t += 10) tick(t);
    assert(!quality_events());
    tick(30000); assert(quality_events() == 1);
    for (uint32_t t = 30010; t < 60000; t += 10) tick(t);
    assert(quality_events() == 1);
    tick(60000); assert(quality_events() == 2);
    app_test_event q = app_test_events[app_test_event_count - 1];
    assert(q.code == DIAG_SENSOR_QUALITY && q.a == ((DIAG_DOMAIN_GNSS << 16) | 2));
    assert(q.b > 2500 && q.c == 0);
    /* No sampled latitude/longitude or path enters any diagnostic argument. */
    assert(app_test_event_count < 50);
    puts("PASS: closed-app checked save/retry, one terminal per request, task failure/discard, admission, sparse private GNSS quality");
    free(T.pts);
    return 0;
}
