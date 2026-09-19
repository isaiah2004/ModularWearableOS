/* Live cached Wi-Fi weather. Current conditions and four local-day forecasts. */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <stdio.h>

/* ---- data model ---------------------------------------------------------- */

enum wx_cond { WX_CLEAR = 0, WX_PARTLY, WX_CLOUDY, WX_RAIN, WX_SNOW, WX_STORM };

typedef struct {
    char         day[4];   /* 3-letter abbreviation */
    int          cond;
    int          hi, lo;   /* degrees, current unit (C) */
} wx_day;

typedef struct {
    int          cond;
    int          temp;
    int          hi, lo;
    const char  *location;
} wx_now;

/* Snapshot copied once per frame; all stripes see the same values. */
static wx_now current;

static wx_day forecast[WEATHER_DAYS];
#define NFC ((int)(sizeof forecast / sizeof forecast[0]))

/* ---- pure helpers (also unit-tested under SELFTEST) ---------------------- */

/* condition -> display word */
static const char *cond_word(int c)
{
    switch (c) {
    case WX_CLEAR:  return "Clear";
    case WX_PARTLY: return "Partly Cloudy";
    case WX_CLOUDY: return "Cloudy";
    case WX_RAIN:   return "Rain";
    case WX_SNOW:   return "Snow";
    case WX_STORM:  return "Thunderstorm";
    default:        return "--";
    }
}

/* "24\xB0" — temperature with a trailing degree glyph (small/medium fonts) */
static void temp_str(char *buf, int n, int t)
{
    snprintf(buf, n, "%d\xB0", t);
}

/* "H:33\xB0  L:25\xB0" — warmer value always shown as H (swaps if reversed) */
static void hilo_str(char *buf, int n, int hi, int lo)
{
    if (lo > hi) { int t = hi; hi = lo; lo = t; }
    snprintf(buf, n, "H:%d\xB0  L:%d\xB0", hi, lo);
}

/* ========================================================================== */
#ifndef SELFTEST
/* ---- procedural weather icons ------------------------------------------- */

/* sun: filled disc + 8 rays. r is the overall glyph radius. */
static void draw_sun(gfx_fb *fb, float cx, float cy, float r, uint16_t body,
                     uint16_t rays)
{
    float disc = r * 0.52f;
    /* rays: 8 spokes at 45 deg, sitting just outside the disc */
    static const float dir[8][2] = {
        { 0, -1 }, { 0.707f, -0.707f }, { 1, 0 }, { 0.707f, 0.707f },
        { 0, 1 }, { -0.707f, 0.707f }, { -1, 0 }, { -0.707f, -0.707f },
    };
    float r0 = r * 0.66f, r1 = r * 1.02f, w = r * 0.13f;
    for (int i = 0; i < 8; i++)
        gfx_line(fb, cx + dir[i][0] * r0, cy + dir[i][1] * r0,
                     cx + dir[i][0] * r1, cy + dir[i][1] * r1, w, rays);
    gfx_fill_circle(fb, cx, cy, disc, body);
}

/* cloud: overlapping puff circles + a flat base slab, spanning ~2*r wide.
 * (cx,cy) is the visual centre of the cloud mass. */
static void draw_cloud(gfx_fb *fb, float cx, float cy, float r, uint16_t col)
{
    float top   = cy - r * 0.18f;
    gfx_fill_circle(fb, cx - r * 0.42f, cy + r * 0.10f, r * 0.42f, col); /* left  */
    gfx_fill_circle(fb, cx + r * 0.50f, cy + r * 0.14f, r * 0.40f, col); /* right */
    gfx_fill_circle(fb, cx,             top,            r * 0.50f, col); /* crown */
    /* flat bottom slab tying the puffs together */
    int bx = (int)(cx - r * 0.84f), bw = (int)(r * 1.74f);
    int by = (int)(cy + r * 0.12f), bh = (int)(r * 0.40f);
    gfx_fill_rect(fb, bx, by, bw, bh, col);
}

/* three short slanted streaks under a cloud */
static void draw_rain_streaks(gfx_fb *fb, float cx, float cy, float r,
                              uint16_t col)
{
    float y0 = cy + r * 0.50f, len = r * 0.42f, w = r * 0.12f;
    for (int i = -1; i <= 1; i++) {
        float x = cx + i * r * 0.46f;
        gfx_line(fb, x + r * 0.10f, y0, x - r * 0.10f, y0 + len, w, col);
    }
}

/* dispatch: draw the icon for `cond` centred at (cx,cy) fitting radius r */
static void draw_wx_icon(gfx_fb *fb, int cond, float cx, float cy, float r)
{
    switch (cond) {
    case WX_CLEAR:
        draw_sun(fb, cx, cy, r, GFX_YELLOW, GFX_ORANGE);
        break;
    case WX_PARTLY:
        /* small sun peeking up-left, cloud in front lower-right */
        draw_sun(fb, cx - r * 0.34f, cy - r * 0.34f, r * 0.55f,
                 GFX_YELLOW, GFX_ORANGE);
        draw_cloud(fb, cx + r * 0.14f, cy + r * 0.20f, r * 0.72f, GFX_WHITE);
        break;
    case WX_CLOUDY:
        draw_cloud(fb, cx, cy, r * 0.94f, GFX_RGB(210, 214, 220));
        break;
    case WX_RAIN:
    case WX_STORM:
        draw_cloud(fb, cx, cy - r * 0.10f, r * 0.90f, GFX_RGB(150, 158, 168));
        draw_rain_streaks(fb, cx, cy - r * 0.10f, r * 0.90f, GFX_ACCENT);
        break;
    case WX_SNOW:
        draw_cloud(fb,cx,cy-r*.15f,r*.8f,GFX_WHITE);
        for(int i=-1;i<=1;i++) gfx_fill_circle(fb,cx+i*r*.5f,cy+r*.7f,r*.09f,GFX_WHITE);
        break;
    default:
        break;
    }
}

/* ---- views --------------------------------------------------------------- */

enum { V_CURRENT = 0, V_FORECAST };
static int s_view;
static hal_weather s_weather;
static uint32_t s_frame=UINT32_MAX;

static void on_start(void) { s_view = V_CURRENT; s_frame=UINT32_MAX; }

static void draw_current(gfx_fb *fb)
{
    char buf[24];

    /* location header */
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(38),
                       current.location, GFX_GREY);

    /* big hero icon */
    draw_wx_icon(fb, current.cond, GFX_CX, GFX_S(80), GFX_S(30));

    /* temperature with a real degree glyph (now baked into the fonts) */
    temp_str(buf, sizeof buf, current.temp);
    font_draw_centered(fb, &font_big, (int)GFX_CX, (int)GFX_S(150), buf, GFX_WHITE);

    /* condition word + hi/lo */
    font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_S(176),
                       cond_word(current.cond), GFX_WHITE);
    hilo_str(buf, sizeof buf, current.hi, current.lo);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(200),
                       buf, GFX_GREY);

    /* affordance */
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(222),
                       s_weather.stale ? "Last update over 1h ago" : "Open-Meteo \xB7 Celsius", GFX_DGREY);
}

static void draw_forecast(gfx_fb *fb)
{
    char buf[16];

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(34),
                       "FORECAST", GFX_GREY);

    /* a centred row of tiles across the widest (vertical-centre) band */
    float step = GFX_S(52);
    float cy   = GFX_CY - GFX_S(4);
    float x0   = GFX_CX - step * (NFC - 1) * 0.5f;

    for (int i = 0; i < NFC; i++) {
        const wx_day *d = &forecast[i];
        float tx = x0 + i * step;

        font_draw_centered(fb, &font_small, (int)tx, (int)(cy - GFX_S(30)),
                           d->day, GFX_GREY);
        draw_wx_icon(fb, d->cond, tx, cy, GFX_S(14));
        temp_str(buf, sizeof buf, d->hi);
        font_draw_centered(fb, &font_small, (int)tx, (int)(cy + GFX_S(32)),
                           buf, GFX_WHITE);
        temp_str(buf, sizeof buf, d->lo);
        font_draw_centered(fb, &font_small, (int)tx, (int)(cy + GFX_S(50)),
                           buf, GFX_GREY);
    }

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(224),
                       "swipe down \xB7 now", GFX_DGREY);
}

static void on_draw(gfx_fb *fb)
{
    if(s_frame!=wos_frame_id) {
        s_frame=wos_frame_id;
        hal_weather_get(&s_weather);
        current=(wx_now){weather_condition(s_weather.code),s_weather.temp,
            s_weather.days[0].hi,s_weather.days[0].lo,s_weather.city};
        for(int i=0;i<NFC;i++) {
            const weather_day *d=&s_weather.days[i];
            forecast[i].cond=weather_condition(d->code);forecast[i].hi=d->hi;forecast[i].lo=d->lo;
            static const char *days[]={"SUN","MON","TUE","WED","THU","FRI","SAT"};
            int weekday=(int)(((d->epoch+s_weather.utc_offset_seconds)/86400+4)%7);
            snprintf(forecast[i].day,sizeof forecast[i].day,"%s",days[weekday]);
        }
    }
    gfx_clear(fb, GFX_BLACK);
    if(!s_weather.valid) {
        font_draw_centered(fb,&font_medium,(int)GFX_CX,(int)GFX_S(85),"Weather",GFX_WHITE);
        font_draw_centered(fb,&font_small,(int)GFX_CX,(int)GFX_S(112),s_weather.city,GFX_GREY);
        const char *msg=hal_status(HAL_FEAT_WIFI)!=HAL_CONN_UP ? "Waiting for Wi-Fi" : s_weather.status;
        font_draw_centered(fb,&font_small,(int)GFX_CX,(int)GFX_S(140),msg,GFX_GREY);
        font_draw_centered(fb,&font_small,(int)GFX_CX,(int)GFX_S(178),"Hold to refresh",GFX_DGREY);
        return;
    }
    if (s_view == V_FORECAST) draw_forecast(fb);
    else                      draw_current(fb);
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_LONG_PRESS: hal_weather_refresh(); return 1;
    case WOS_EV_SWIPE_UP:   s_view = V_FORECAST; return 1;
    case WOS_EV_SWIPE_DOWN: s_view = V_CURRENT;  return 1;
    case WOS_EV_TAP:
        s_view = (s_view == V_CURRENT) ? V_FORECAST : V_CURRENT;
        return 1;
    default:
        return 0;
    }
}

const wos_app app_weather = {
    "Weather", on_start, on_event, on_draw, GFX_ACCENT, ICON_WEATHER
};

/* ========================================================================== */
#else  /* SELFTEST: exercise the pure formatting / hi-lo logic */
#include <string.h>
#include <assert.h>

int main(void)
{
    char b[24];

    temp_str(b, sizeof b, 24);
    assert(strcmp(b, "24\xB0") == 0);
    temp_str(b, sizeof b, 7);
    assert(strcmp(b, "7\xB0") == 0);

    /* already-ordered */
    hilo_str(b, sizeof b, 33, 25);
    assert(strcmp(b, "H:33\xB0  L:25\xB0") == 0);
    /* reversed input must be normalised (warmer -> H) */
    hilo_str(b, sizeof b, 25, 33);
    assert(strcmp(b, "H:33\xB0  L:25\xB0") == 0);
    /* equal */
    hilo_str(b, sizeof b, 30, 30);
    assert(strcmp(b, "H:30\xB0  L:30\xB0") == 0);

    assert(strcmp(cond_word(WX_CLEAR),  "Clear")  == 0);
    assert(strcmp(cond_word(WX_RAIN),   "Rain")   == 0);
    assert(strcmp(cond_word(WX_PARTLY), "Partly Cloudy") == 0);

    printf("SELFTEST OK (%d forecast days)\n", NFC);
    return 0;
}
#endif
