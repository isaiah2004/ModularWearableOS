/* app_calendar.c — Month calendar with a "today" marker and month navigation.
 *
 * ============================ ALGORITHM (plain English) ============================
 *
 * (1) LEAP YEAR
 *     A year is a leap year if it is divisible by 4, EXCEPT century years, which
 *     must also be divisible by 400. So 2024 and 2000 are leap; 2025 and 1900 are not.
 *         leap  <=>  (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)
 *
 * (2) DAYS IN A MONTH
 *     Look the month up in the table {31,28,31,30,31,30,31,31,30,31,30,31}.
 *     February is the only special case: it has 29 days in a leap year, else 28.
 *
 * (3) FIRST WEEKDAY OF THE MONTH  (Sakamoto's algorithm)
 *     We need the weekday of the 1st of the month, with 0 = Sunday .. 6 = Saturday,
 *     which matches the HAL's weekday convention. Sakamoto's method gives the weekday
 *     of any date d/m/y:
 *         t[] = {0,3,2,5,0,3,5,1,4,6,2,4}   (per-month magic offsets)
 *         if (m < 3) y -= 1                 (treat Jan/Feb as months of prev. year,
 *                                            so leap-day handling is automatic)
 *         dow = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7
 *     Passing d = 1 yields the weekday of the first of the month = the column in
 *     which day 1 is drawn. Every following day fills the grid left-to-right,
 *     wrapping to the next row after Saturday.
 *     Self-check: 2026-07-01 -> Wednesday (=3); 2024-02-01 -> Thursday (=4).
 *
 * (4) MONTH-OFFSET NAVIGATION
 *     We keep a signed month offset from the *real* current month (0 = this month).
 *     Swipe LEFT = next month (offset+1), swipe RIGHT = previous month (offset-1).
 *     To resolve the offset to a concrete (year, month) we work in "absolute months":
 *         total = year * 12 + (month - 1) + offset
 *         target_year  = total / 12
 *         target_month = total % 12 + 1
 *     Integer division here floors toward zero, but total is always >= 0 for any
 *     realistic date/offset, so the year wraps correctly across December<->January.
 *
 * ==================================================================================
 */

/* ---- Pure date math (compiled in both the app build and the SELFTEST build) ---- */

static int cal_is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int cal_days_in_month(int y, int m)
{
    static const int len[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m < 1 || m > 12) return 30;          /* defensive; never expected */
    if (m == 2 && cal_is_leap(y)) return 29;
    return len[m - 1];
}

/* Sakamoto: weekday of date d/m/y, 0 = Sunday .. 6 = Saturday. */
static int cal_dow(int y, int m, int d)
{
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

static int cal_first_weekday(int y, int m)
{
    return cal_dow(y, m, 1);
}

/* Resolve a signed month offset from (year, month) into concrete year/month. */
static void cal_apply_offset(int year, int month, int offset, int *ty, int *tm)
{
    int total = year * 12 + (month - 1) + offset;
    *ty = total / 12;
    *tm = total % 12 + 1;
}

#ifdef SELFTEST
/* --------------------------- Standalone date-math tests -------------------------- */
#include <assert.h>
#include <stdio.h>

int main(void)
{
    /* days-in-month */
    assert(cal_days_in_month(2024, 2) == 29);   /* leap February  */
    assert(cal_days_in_month(2025, 2) == 28);   /* common February*/
    assert(cal_days_in_month(2026, 4) == 30);   /* April          */
    assert(cal_days_in_month(2026, 1) == 31);   /* January        */
    assert(cal_days_in_month(2000, 2) == 29);   /* 400-divisible  */
    assert(cal_days_in_month(1900, 2) == 28);   /* century, not 400*/

    /* leap-year predicate */
    assert(cal_is_leap(2024) && !cal_is_leap(2025));
    assert(cal_is_leap(2000) && !cal_is_leap(1900));

    /* first weekday (0=Sun..6=Sat) */
    assert(cal_first_weekday(2026, 7) == 3);    /* 2026-07-01 = Wed */
    assert(cal_first_weekday(2024, 2) == 4);    /* 2024-02-01 = Thu */
    assert(cal_first_weekday(2026, 1) == 4);    /* 2026-01-01 = Thu */

    /* month-offset navigation wraps the year correctly */
    int ty, tm;
    cal_apply_offset(2026, 12, 1, &ty, &tm);   /* Dec 2026 +1 -> Jan 2027 */
    assert(ty == 2027 && tm == 1);
    cal_apply_offset(2026, 1, -1, &ty, &tm);   /* Jan 2026 -1 -> Dec 2025 */
    assert(ty == 2025 && tm == 12);
    cal_apply_offset(2026, 7, 0, &ty, &tm);    /* identity */
    assert(ty == 2026 && tm == 7);
    cal_apply_offset(2026, 7, -13, &ty, &tm);  /* back 13 months -> Jun 2025 */
    assert(ty == 2025 && tm == 6);

    printf("app_calendar SELFTEST: all assertions passed\n");
    return 0;
}

#else
/* --------------------------------- The watch app -------------------------------- */
/* =============================== ANIMATION OVERVIEW ==============================
 *
 * The month grid is static by nature, so all motion is intentional and brief —
 * idle cost stays tiny because a settled grid produces frame-invariant draw
 * commands (identical bbox+hash every frame => zero dirty rows), and the only
 * perpetual motion is a gentle glow behind TODAY's cell, which dirties one
 * small box. Three animations, all derived from a per-frame-stable clock so the
 * record pass and the raster pass see identical geometry:
 *
 *   1. DAY-CELL ENTRANCE (on open / on month change) — the day numbers fade and
 *      rise into place in a diagonal cascade, staggered by row + column, each
 *      cell easing (ease-out cubic) over CAL_CELL_MS. TODAY's disc also scales
 *      up from a seed. Once every cell reaches progress 1.0 the grid is exactly
 *      static again (no residual per-frame change => no damage).
 *
 *   2. MONTH TRANSITION (on horizontal swipe) — the outgoing month slides a
 *      short distance in the swipe direction while fading out; the incoming
 *      month slides in from the opposite side while its cells run the entrance
 *      cascade. Ease-out cubic over CAL_TRANS_MS (~0.34s), a brief full redraw.
 *      The S/M/T/W/T/F/S header is fixed chrome and never moves, so the content
 *      transitions cleanly underneath it.
 *
 *   3. TODAY PULSE (continuous, current month only) — a soft red radial glow
 *      breathes behind today's disc on a slow sine (CAL_PULSE_MS). It only
 *      touches a small box around one cell, so idle repaint is cheap; when the
 *      shown month is not the real one there is no today and the screen is fully
 *      static.
 * ================================================================================ */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <stdio.h>

#define CAL_TWO_PI 6.28318531f

/* Layout is authored on the 240px design grid and scaled with GFX_S(); the grid
 * is centered so the four near-bezel corner cells stay inside the round display. */
#define CAL_TITLE_Y   GFX_S(32)   /* "July 2026"                 */
#define CAL_WKDAY_Y   GFX_S(58)   /* S M T W T F S header row    */
#define CAL_GRID_Y0   GFX_S(78)   /* center-y of the first day row */
#define CAL_COL_X0    48          /* center-x of column 0 (design px) */
#define CAL_COL_DX    24          /* column pitch (design px)    */
#define CAL_ROW_DY    22          /* row pitch (design px)       */
#define CAL_MARK_R    GFX_S(10)   /* today / selection marker radius */

/* ---- animation tunables ---- */
#define CAL_TRANS_MS  340         /* month slide/fade duration        */
#define CAL_CELL_MS   260         /* one cell's fade/rise duration    */
#define CAL_ROW_STAG  34          /* per-row entrance stagger (ms)    */
#define CAL_COL_STAG  10          /* per-column entrance stagger (ms) */
#define CAL_RISE      GFX_S(6)    /* how far a cell rises into place  */
#define CAL_SLIDE     GFX_S(40)   /* month slide distance             */
#define CAL_PULSE_MS  2200.0f     /* today-glow breathing period      */

static int s_offset = 0;   /* months from the real current month (0 = this month) */
static int s_sel    = -1;  /* tap-selected day of the shown month, -1 = none      */

/* animation state (all time in ms on a TICK-accumulated clock) */
static uint32_t s_now;          /* monotonic ms while the app is open        */
static uint32_t s_appear;       /* s_now when the shown grid began appearing */
static int      s_trans;        /* 1 while a month slide is playing          */
static uint32_t s_trans_start;  /* s_now at the swipe that began the slide   */
static int      s_tdir = 1;     /* slide direction: +1 next month, -1 prev   */
static int      s_prev_ty  = 2026, s_prev_tm = 7; /* outgoing month for the slide */
static int      s_prev_today = -1;

static const char *const CAL_MONTHS[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

static void on_start(void)
{
    s_offset = 0;
    s_sel    = -1;
    s_now    = 0;
    s_appear = 0;
    s_trans  = 0;
}

/* center-x / center-y for a grid cell (col 0..6, row 0..5) */
static float cal_cx(int col) { return GFX_S(CAL_COL_X0 + col * CAL_COL_DX); }
static float cal_cy(int row) { return CAL_GRID_Y0 + row * (float)GFX_S(CAL_ROW_DY); }

static float cal_clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* ease-out cubic: fast start, gentle settle — the house feel for arrivals */
static float cal_ease_out(float u)
{
    float m = 1.0f - u;
    return 1.0f - m * m * m;
}

/* colour `col` composited on black at brightness a in [0,1] (for fades) */
static uint16_t cal_dim(uint16_t col, float a)
{
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    return gfx_blend565(GFX_BLACK, col, (uint8_t)(a * 255.0f + 0.5f));
}

/* Soft radial-alpha glow (peak opacity `intensity`) composited via px_blend.
 * Emits ONE damage command in record mode (like face/breathe glows) so the
 * recorder never overflows and the pixel loop runs only in the raster pass.
 * Falloff is in squared radius => no per-pixel sqrt on the S3. */
static void cal_glow(gfx_fb *fb, float cx, float cy, float r,
                     uint16_t col, float intensity)
{
    if (r < 1.0f || intensity <= 0.0f) return;
    int x0 = (int)(cx - r), x1 = (int)(cx + r) + 1;
    int y0 = (int)(cy - r), y1 = (int)(cy + r) + 1;
    if (fb->rec) {
        unsigned hh = gfx_hash_f(gfx_hash_f(gfx_hash_f(gfx_hash_f(
                      gfx_hash_u(0x0Bu, col), cx), cy), r), intensity);
        gfx_rec_cmd(fb, x0, y0, x1 + 1, y1 + 1, hh);
        return;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > GFX_W - 1) x1 = GFX_W - 1;
    if (y1 > GFX_H - 1) y1 = GFX_H - 1;
    float inv2 = 1.0f / (r * r);
    for (int y = y0; y <= y1; y++) {
        float dy = (float)y + 0.5f - cy;
        float dy2 = dy * dy;
        for (int x = x0; x <= x1; x++) {
            float dx = (float)x + 0.5f - cx;
            float t2 = (dx * dx + dy2) * inv2;   /* (d/r)^2, edge at 1 */
            if (t2 >= 1.0f) continue;
            float f = 1.0f - t2;
            float a = intensity * f * f;         /* soft rounded dome */
            gfx_px_blend(fb, x, y, col, (uint8_t)(a * 255.0f + 0.5f));
        }
    }
}

/* Draw one month grid. `ox` shifts it horizontally (slide), `galpha` fades the
 * whole grid (0..1), `cascade` runs the staggered per-cell entrance from
 * `appear`, and `pulse` (0..1) drives today's breathing glow. */
static void cal_draw_grid(gfx_fb *fb, int ty, int tm, int today,
                          float ox, float galpha, int cascade,
                          uint32_t appear, float pulse)
{
    if (galpha <= 0.003f) return;

    char hdr[24];
    snprintf(hdr, sizeof hdr, "%s %d", CAL_MONTHS[tm - 1], ty);
    font_draw_centered(fb, &font_medium, (int)(GFX_CX + ox), (int)CAL_TITLE_Y,
                       hdr, cal_dim(GFX_WHITE, galpha));

    int ndays = cal_days_in_month(ty, tm);
    int first = cal_first_weekday(ty, tm);
    int elapsed = (appear <= s_now) ? (int)(s_now - appear) : 0;

    for (int d = 1; d <= ndays; d++) {
        int idx = first + d - 1;
        int row = idx / 7;
        int col = idx % 7;

        float cp = 1.0f;   /* this cell's entrance progress, eased */
        if (cascade) {
            int delay = row * CAL_ROW_STAG + col * CAL_COL_STAG;
            cp = cal_ease_out(cal_clamp01((float)(elapsed - delay) / (float)CAL_CELL_MS));
            if (cp <= 0.0f) continue;   /* not on stage yet */
        }

        float a = galpha * cp;
        if (a <= 0.003f) continue;

        float cx = cal_cx(col) + ox;
        float cy = cal_cy(row) + (1.0f - cp) * CAL_RISE;   /* rise into place */

        uint16_t txt = GFX_WHITE;
        if (d == today) {
            /* breathing glow behind, then the disc scaling up from a seed */
            float ga = (0.16f + 0.14f * pulse) * a;
            cal_glow(fb, cx, cy, CAL_MARK_R * (1.35f + 0.30f * pulse), GFX_RED, ga);
            gfx_fill_circle(fb, cx, cy, CAL_MARK_R * (0.4f + 0.6f * cp),
                            cal_dim(GFX_RED, a));
        } else if (d == s_sel) {
            gfx_ring(fb, cx, cy, CAL_MARK_R, GFX_S(2), cal_dim(GFX_ACCENT, a));
        } else if (col == 0 || col == 6) {
            txt = GFX_GREY;                                 /* dim weekends */
        }

        char num[12];
        snprintf(num, sizeof num, "%d", d);
        font_draw_centered(fb, &font_small, (int)cx,
                           (int)(cy + font_small.ascent * 0.34f),
                           num, cal_dim(txt, a));
    }
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);

    hal_datetime now;
    hal_get_datetime(&now);

    int ty, tm;
    cal_apply_offset(now.year, now.month, s_offset, &ty, &tm);
    int today = (s_offset == 0) ? now.day : -1;

    /* weekday headers: S M T W T F S — fixed chrome, weekend letters tinted */
    static const char *const WK[7] = { "S", "M", "T", "W", "T", "F", "S" };
    for (int c = 0; c < 7; c++) {
        uint16_t col = (c == 0 || c == 6) ? GFX_RED : GFX_GREY;
        font_draw_centered(fb, &font_small, (int)cal_cx(c),
                           (int)(CAL_WKDAY_Y + font_small.ascent * 0.34f), WK[c], col);
    }

    float pulse = 0.5f + 0.5f * gfx_fast_sin((float)s_now * (CAL_TWO_PI / CAL_PULSE_MS));

    if (s_trans) {
        /* directional slide: outgoing block exits + fades, incoming enters + cascades */
        float tp = cal_ease_out(cal_clamp01((float)(s_now - s_trans_start)
                                            / (float)CAL_TRANS_MS));
        float d = (float)s_tdir;
        cal_draw_grid(fb, s_prev_ty, s_prev_tm, s_prev_today,
                      -d * tp * CAL_SLIDE, 1.0f - tp, 0, s_appear, pulse);
        cal_draw_grid(fb, ty, tm, today,
                      d * (1.0f - tp) * CAL_SLIDE, 1.0f, 1, s_appear, pulse);
    } else {
        cal_draw_grid(fb, ty, tm, today, 0.0f, 1.0f, 1, s_appear, pulse);
    }
}

/* Begin a month change: capture the outgoing month for the slide, then advance. */
static void cal_nav(int dir)
{
    hal_datetime now;
    hal_get_datetime(&now);
    cal_apply_offset(now.year, now.month, s_offset, &s_prev_ty, &s_prev_tm);
    s_prev_today  = (s_offset == 0) ? now.day : -1;
    s_tdir        = dir;
    s_trans       = 1;
    s_trans_start = s_now;
    s_offset     += dir;
    s_sel         = -1;
    s_appear      = s_now;   /* retrigger the incoming cascade */
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_TICK:
        s_now += (uint32_t)ev->x;
        if (s_trans && (s_now - s_trans_start) >= (uint32_t)CAL_TRANS_MS) s_trans = 0;
        return 1;
    case WOS_EV_SWIPE_LEFT:                 /* forward to next month */
        cal_nav(1);
        return 1;
    case WOS_EV_SWIPE_RIGHT:                /* back to previous month */
        cal_nav(-1);
        return 1;
    case WOS_EV_TAP: {
        /* hit-test the tap against the day grid; select the day if one is hit */
        hal_datetime now;
        hal_get_datetime(&now);
        int ty, tm;
        cal_apply_offset(now.year, now.month, s_offset, &ty, &tm);
        int ndays = cal_days_in_month(ty, tm);
        int first  = cal_first_weekday(ty, tm);

        float half_dx = GFX_S(CAL_COL_DX) * 0.5f;
        float half_dy = GFX_S(CAL_ROW_DY) * 0.5f;
        for (int d = 1; d <= ndays; d++) {
            int idx = first + d - 1;
            float cx = cal_cx(idx % 7);
            float cy = cal_cy(idx / 7);
            if (ev->x >= cx - half_dx && ev->x < cx + half_dx &&
                ev->y >= cy - half_dy && ev->y < cy + half_dy) {
                s_sel = (s_sel == d) ? -1 : d;   /* tap again to deselect */
                return 1;
            }
        }
        return 1;
    }
    default:
        return 0;
    }
}

const wos_app app_calendar = { "Calendar", on_start, on_event, on_draw, GFX_RED, ICON_CALENDAR };

#endif /* SELFTEST */
