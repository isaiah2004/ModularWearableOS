/* Fixed assertions replacing timezone_picker/manual_time_unset_date audit repros.
 * Exercise the real app callbacks against a firmware-style UTC/local host model. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../nos/apps/set_settings.c"

nos_prefs_t nos_prefs;
nos_time_t nos_t = 10;
static uint64_t utc;
static int host_tz, writes, checked_calls, accept_write = 1, pops, feedback;
static char toast[64];
static hal_datetime wall, requested;

uint64_t hal_epoch_seconds(void) { return utc; }
void hal_get_datetime(hal_datetime *out) { *out = wall; }
void hal_set_datetime(const hal_datetime *dt) { requested = *dt; writes++; }
int hal_set_datetime_checked(const hal_datetime *dt)
{
    requested = *dt; checked_calls++;
    if (dt->year < 2000 || dt->year > 2099 || !accept_write) return 0;
    writes++; wall = *dt; return 1;
}
void prefs_save(void) {}
void nos_feedback(int kind) { feedback = kind; }
void nos_toast(const char *s) { snprintf(toast, sizeof toast, "%s", s); }
void nos_pop(void) { pops++; }
nos_time_t nos_enter_t(void) { return nos_t; }
void ui_fmt_time(char *out, int n, int h, int m) { snprintf(out, (size_t)n, "%02d:%02d", h, m); }

static void host_apply_offset(void)
{
    host_tz = nos_prefs.tz_min;
    time_t local = (time_t)((int64_t)utc + host_tz * 60);
    struct tm *t = gmtime(&local);
    assert(t);
    wall = (hal_datetime){t->tm_year+1900,t->tm_mon+1,t->tm_mday,t->tm_wday,
                          t->tm_hour,t->tm_min,t->tm_sec,0};
}

static void timezone_cases(void)
{
    /* UTC 2024-02-29 23:50 and 2026-12-31 23:50. Each choice crosses
     * the date/year boundary as appropriate, including quarter-hour zones. */
    const uint64_t anchors[] = {1709250600ULL, 1798761000ULL};
    unsigned cases = 0;
    nos_prefs.units_metric = 1;
    for (unsigned a = 0; a < sizeof anchors / sizeof anchors[0]; a++) {
        utc = anchors[a]; nos_prefs.tz_min = 330; host_apply_offset();
        P.id = I_TZ; pick_enter();
        for (int i = 0; i < N_TZ; i++) {
            char before[32], after[32], expected[32];
            uint64_t epoch = utc;
            int minute = ((int)(utc / 60 % 1440) + k_tz[i] + 1440) % 1440;
            snprintf(expected, sizeof expected, "%02d:%02d LOCAL", minute/60, minute%60);
            pick_extra(I_TZ, i, before, sizeof before);
            P.p.idx = i; pick_commit();
            assert(nos_prefs.tz_min == k_tz[i] && utc == epoch);
            /* Preview is correct before and after firmware consumes tz_min. */
            pick_extra(I_TZ, i, after, sizeof after);
            assert(!strcmp(before, expected) && !strcmp(after, expected));
            host_apply_offset();
            pick_extra(I_TZ, i, after, sizeof after);
            assert(!strcmp(after, expected));
            hal_datetime same = wall;
            pick_leave(); pick_leave();       /* cover, reveal, true exit */
            assert(!writes && utc == epoch && !memcmp(&wall, &same, sizeof wall));
            cases++;
        }
    }
    utc = 0;
    char extra[24]; pick_extra(I_TZ, 0, extra, sizeof extra);
    assert(!strcmp(extra, "TIME NOT SYNCED") && !writes);
    printf("PASS DEV-001: %u timezone choices preserve UTC/calendar through commit/cover/exit; preview uses UTC, unknown clock is explicit\n", cases);
}

static void press_set(int year, int accepted)
{
    wall = (hal_datetime){.year=year,.month=10,.day=21,.weekday=3,.hour=5,.minute=30};
    T.h.idx = 12; T.m.idx = 34; T.pressed = 1;
    accept_write = accepted;
    nos_input in = {.type=IN_TAP};
    assert(time_input(&in) && !T.pressed);
    assert(requested.year == year && requested.month == 10 && requested.day == 21);
    assert(requested.hour == 12 && requested.minute == 34 && !requested.second && !requested.millis);
}

int main(int argc,char **argv)
{
    if (argc < 2 || strcmp(argv[1], "manual")) timezone_cases();
    if (argc > 1 && !strcmp(argv[1], "timezone")) return 0;
    press_set(1970, 1);
    assert(checked_calls == 1 && !writes && !pops && feedback == FB_ERROR);
    assert(strstr(toast, "DATE NOT SET") && strstr(toast, "PHONE"));
    assert(wall.hour == 5 && wall.minute == 30 && T.h.idx == 12 && T.m.idx == 34);
    press_set(2026, 0);
    assert(checked_calls == 2 && !writes && !pops && feedback == FB_ERROR);
    assert(!strcmp(toast, "TIME NOT SET: TRY AGAIN"));
    press_set(2026, 1);
    assert(checked_calls == 3 && writes == 1 && pops == 1 && feedback == FB_SUCCESS);
    assert(!strcmp(toast, "TIME SET") && wall.hour == 12 && wall.minute == 34);
    puts("PASS DEV-015: unset date and HAL rejection keep the editor/draft open; only checked success reports TIME SET");
    return 0;
}
