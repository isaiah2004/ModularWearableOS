/* app_worldclock.c — a world-clock list for the round AMOLED.
 *
 * Shows a scrollable list of cities, each with its current HH:MM, a
 * day/night sun/moon glyph, and a "Yesterday/Tomorrow" tag when the city
 * has rolled over a calendar day relative to the wearer.
 *
 * ---------------------------------------------------------------------------
 * PLAIN-ENGLISH ALGORITHM
 * ---------------------------------------------------------------------------
 * hal_get_datetime() returns the wearer's LOCAL wall-clock time. We treat the
 * watch as living in a fixed HOME time zone (Hyderabad, India = UTC+5:30 =
 * +330 minutes). DST is deliberately ignored for every city — offsets are the
 * standard-time offsets, so during summer some cities will read one hour off.
 *
 * To find a city's time we never need real UTC; we only need the DIFFERENCE
 * between the city's offset and home's offset:
 *
 *     local_minutes = local_hour * 60 + local_minute      (minutes past midnight, home)
 *     city_total    = local_minutes + (city_offset - HOME_OFFSET)
 *
 * (city_offset - HOME_OFFSET is: undo home's offset back to UTC, then apply
 *  the city's offset — the two constant terms just add together.)
 *
 * DAY WRAP: city_total can fall outside [0,1440). We normalise it back into a
 * single day while counting how many midnights we crossed:
 *
 *     while (city_total <  0)     city_total += 1440;  day_delta--   (city is a day behind)
 *     while (city_total >= 1440)  city_total -= 1440;  day_delta++   (city is a day ahead)
 *     city_hour = city_total / 60;   city_minute = city_total % 60;
 *
 * (This loop form sidesteps C's trunc-toward-zero integer division, which
 *  gives the wrong sign of floor() for negative numbers.)
 *
 * DAY/NIGHT TEST: it's "daytime" in a city when its local hour is in [6,18),
 * i.e. 6am up to (not including) 6pm — otherwise it's night.
 * ---------------------------------------------------------------------------
 */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <stdio.h>

/* Home zone: Hyderabad / India Standard Time = UTC+5:30. */
#define HOME_OFFSET 330

/* Core time math (shared by the UI and the SELFTEST harness).
 * Given the wearer's minutes-past-midnight and the two UTC offsets, returns
 * the city's hour, minute, and the calendar-day delta (-1 behind, +1 ahead). */
static void wc_city_time(int local_min, int city_off, int home_off,
                         int *hh, int *mm, int *day_delta)
{
    int total = local_min + (city_off - home_off);
    int dd = 0;
    while (total < 0)      { total += 1440; dd -= 1; }
    while (total >= 1440)  { total -= 1440; dd += 1; }
    *hh = total / 60;
    *mm = total % 60;
    *day_delta = dd;
}

#ifdef SELFTEST
/* ------------------------------------------------------------------ */
/* Standalone self-test: exercises the timezone/day-wrap math only.    */
/* Build:  gcc -DSELFTEST -std=c99 -Wall -Wextra app_worldclock.c -o t */
/* ------------------------------------------------------------------ */
#include <assert.h>

int main(void)
{
    int hh, mm, dd;

    /* Home = Hyderabad (+330). Wearer local time 14:00 -> 840 minutes. */
    wc_city_time(840, 0,   HOME_OFFSET, &hh, &mm, &dd);   /* London (0)   */
    assert(hh == 8  && mm == 30 && dd == 0);
    wc_city_time(840, 540, HOME_OFFSET, &hh, &mm, &dd);   /* Tokyo (+540) */
    assert(hh == 17 && mm == 30 && dd == 0);

    /* Wrap BACKWARD across midnight: local 02:00 -> Los Angeles (-420).
     * 120 + (-420-330) = -630 -> +1440 = 810 (13:30), one day behind. */
    wc_city_time(120, -420, HOME_OFFSET, &hh, &mm, &dd);
    assert(hh == 13 && mm == 30 && dd == -1);

    /* Wrap FORWARD across midnight: local 23:30 -> Tokyo (+540).
     * 1410 + 210 = 1620 -> -1440 = 180 (03:00), one day ahead. */
    wc_city_time(1410, 540, HOME_OFFSET, &hh, &mm, &dd);
    assert(hh == 3 && mm == 0 && dd == 1);

    /* Home city against itself is always identical, same day. */
    wc_city_time(725, HOME_OFFSET, HOME_OFFSET, &hh, &mm, &dd);
    assert(hh == 12 && mm == 5 && dd == 0);

    printf("worldclock SELFTEST: all timezone/day-wrap cases passed\n");
    return 0;
}

#else
/* ------------------------------------------------------------------ */
/* The watch app.                                                      */
/* ------------------------------------------------------------------ */

typedef struct { const char *name; int off; } wc_city;

/* Home first (index 0). Offsets are standard time in minutes; DST ignored. */
static const wc_city CITIES[] = {
    { "Hyderabad",   330 },   /* home, UTC+5:30 */
    { "London",        0 },   /* UTC+0          */
    { "New York",   -300 },   /* UTC-5 (EST)    */
    { "Tokyo",       540 },   /* UTC+9          */
    { "Los Angeles", -480 },  /* UTC-8 (PST); shown short if space is tight */
};
#define NCITY ((int)(sizeof CITIES / sizeof CITIES[0]))

#define ROW_H    GFX_S(50)
#define LIST_TOP GFX_S(56)
#define LIST_BOT (GFX_H - GFX_S(18))

static float scroll;

static void on_start(void)
{
    scroll = 0;
}

static float content_h(void) { return NCITY * ROW_H; }
static float visible_h(void) { return LIST_BOT - LIST_TOP; }
static float max_scroll(void)
{
    float m = content_h() - visible_h();
    return m > 0 ? m : 0;
}

/* Copy `src` into `dst` (size cap), stopping before it would exceed maxw
 * pixels in font `f`. Keeps the list from colliding with the time column. */
static void fit_name(const font_t *f, const char *src, char *dst, int cap, int maxw)
{
    int n = 0;
    dst[0] = 0;
    while (src[n] && n < cap - 1) {
        dst[n] = src[n];
        dst[n + 1] = 0;
        if (font_text_width(f, dst) > maxw) { dst[n] = 0; break; }
        n++;
    }
}

static void draw_row(gfx_fb *fb, int i, float cy)
{
    hal_datetime t;
    hal_get_datetime(&t);
    int local_min = t.hour * 60 + t.minute;

    int hh, mm, dd;
    wc_city_time(local_min, CITIES[i].off, HOME_OFFSET, &hh, &mm, &dd);
    int is_day = (hh >= 6 && hh < 18);
    int is_home = (i == 0);

    /* day/night glyph on the far left: filled sun vs. outline moon */
    float gx = GFX_S(30);
    if (is_day)
        gfx_fill_circle(fb, gx, cy, GFX_S(7), GFX_YELLOW);
    else
        gfx_ring(fb, gx, cy, GFX_S(6), GFX_S(2.2f), GFX_GREY);

    /* time, right-aligned (font_medium so the city name has room — font_big
     * was ~300px wide and buried the names) */
    char tb[8];
    snprintf(tb, sizeof tb, "%02d:%02d", hh, mm);
    float x_right = GFX_W - GFX_S(22);
    int time_w = font_text_width(&font_medium, tb);
    font_draw(fb, &font_medium, (int)(x_right - time_w),
              (int)(cy + font_medium.ascent * 0.34f), tb,
              is_home ? GFX_ACCENT : GFX_WHITE);

    /* city name (upper) + optional day-offset tag (lower), left column */
    float x_name = GFX_S(42);
    int name_max = (int)(x_right - time_w - GFX_S(8) - x_name);
    char nm[20];
    fit_name(&font_medium, CITIES[i].name, nm, (int)sizeof nm, name_max);

    const char *tag = 0;
    if (dd == 1)       tag = "Tomorrow";
    else if (dd == -1) tag = "Yesterday";
    else if (dd > 1)   tag = "+days";
    else if (dd < -1)  tag = "-days";

    if (tag) {
        font_draw(fb, &font_medium, (int)x_name,
                  (int)(cy - GFX_S(5) + font_medium.ascent * 0.34f), nm,
                  is_home ? GFX_ACCENT : GFX_WHITE);
        font_draw(fb, &font_small, (int)x_name,
                  (int)(cy + GFX_S(17)), tag, GFX_ORANGE);
    } else {
        /* single centred name line when there's no day tag */
        font_draw(fb, &font_medium, (int)x_name,
                  (int)(cy + font_medium.ascent * 0.34f), nm,
                  is_home ? GFX_ACCENT : GFX_WHITE);
    }
}

static void chevron(gfx_fb *fb, float cy, int up)
{
    float w = GFX_S(9), dy = up ? -GFX_S(5) : GFX_S(5);
    gfx_line(fb, GFX_CX - w, cy - dy, GFX_CX, cy + dy, GFX_S(2.5f), GFX_DGREY);
    gfx_line(fb, GFX_CX + w, cy - dy, GFX_CX, cy + dy, GFX_S(2.5f), GFX_DGREY);
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(30),
                       "WORLD CLOCK", GFX_GREY);

    /* If everything fits, stack the rows centred; else honour scroll. */
    float base;
    if (content_h() <= visible_h())
        base = LIST_TOP + (visible_h() - content_h()) * 0.5f;
    else
        base = LIST_TOP - scroll;

    for (int i = 0; i < NCITY; i++) {
        float cy = base + i * ROW_H + ROW_H * 0.5f;
        if (cy + ROW_H * 0.5f < LIST_TOP) continue;
        if (cy - ROW_H * 0.5f > LIST_BOT) continue;
        draw_row(fb, i, cy);
    }

    /* scroll affordances */
    if (max_scroll() > 0) {
        if (scroll > 0.5f)             chevron(fb, GFX_S(46), 1);
        if (scroll < max_scroll() - 0.5f) chevron(fb, GFX_H - GFX_S(30), 0);
    }
}

static int on_event(const wos_event *ev)
{
    float step = ROW_H * 1.5f;
    switch (ev->type) {
    case WOS_EV_SWIPE_UP:    /* content moves up -> reveal lower cities */
        scroll += step;
        if (scroll > max_scroll()) scroll = max_scroll();
        return 1;
    case WOS_EV_SWIPE_DOWN:  /* content moves down -> reveal upper cities */
        scroll -= step;
        if (scroll < 0) scroll = 0;
        return 1;
    default:
        return 0;
    }
}

const wos_app app_worldclock = {
    "World", on_start, on_event, on_draw, GFX_ACCENT, ICON_GLOBE
};

#endif /* SELFTEST */
