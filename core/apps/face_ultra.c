/* face_ultra.c — "Ultra": a Galaxy-Watch-Ultra-style rugged instrument face.
 *
 * Design rules (from a multi-critic pass against the reference):
 *   - reference orange identifies gauges, their small source icons, seconds
 *     and the noon index. Curved weather/health values remain grey/white.
 *   - complications follow inset circular sections: upper forecast fan,
 *     lower-right health bands, lower-left score, central instrument hub.
 *   - the big 12/9/6 cardinals and the hands dominate; complications recede.
 *   - chamfered vector cardinals + Saira values and condensed bezel numerals.
 *     Type, gauges and minute marks occupy separate radial lanes.
 *
 * Everything static is rasterised once into a cached layer (re-rendered only
 * when weather/battery/steps/day change); only the three hands draw live per frame.
 *
 * DATA HONESTY (plain 1.43-B board):
 *   REAL      : time/date (RTC), battery (ADC), steps (QMI8658 cadence).
 *   PLACEHOLDER: heart rate/SpO2 unavailable; compass rose decorative. */
#include "apps.h"
#include "../icons.h"
#include "../layer.h"
#include "../pathtext.h"
#include "../settings.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI 3.14159265f
#define LAY_ULTRA 10

/* palette */
#define U_BG      GFX_RGB(24, 24, 24)     /* neutral charcoal after RGB565 quantization */
#define U_WELL    GFX_RGB(0, 0, 0)        /* recessed curved sections */
#define U_SEAM    GFX_RGB(43, 47, 53)
#define U_TICK    GFX_RGB(66, 69, 78)      /* minor ticks / unlit dashes */
#define U_TICKHR  GFX_RGB(200, 205, 214)   /* hour ticks */
#define U_NUM     GFX_RGB(150, 156, 168)   /* small ring numerals */
#define U_NUMHI   GFX_RGB(240, 244, 240)   /* neutral white in RGB565 */
#define U_FAN     GFX_RGB(16, 16, 16)      /* charcoal grid inside the black label band */
#define U_ROSE    GFX_RGB(48, 48, 48)      /* fine forecast grid */
#define U_ROSEHI  GFX_RGB(88, 92, 88)
#define U_TRACK   GFX_RGB(34, 37, 44)      /* arc/tick track */
#define U_ORANGE  GFX_RGB(255, 102, 0)     /* the reference's safety orange */
#define U_ICON    GFX_RGB(183, 188, 196)   /* weather/health symbols */
#define U_LABEL   GFX_RGB(177, 182, 191)   /* curved complication text */
#define U_EDGE    GFX_RGB(8, 8, 8)        /* contact shadows and hub bore */
#define U_BEVEL   GFX_RGB(104, 108, 104)  /* shaded side of the raised hand rails */

/* Frame-latched sensor values used to compose the static dial. */
static hal_weather s_weather;
static int s_brightness=-1;

static const char *HN[12] = { "12","01","02","03","04","05",
                              "06","07","08","09","10","11" };

static void polar(float ang, float r, float *x, float *y)
{
    *x = GFX_CX + sinf(ang) * GFX_S(r);
    *y = GFX_CY - cosf(ang) * GFX_S(r);
}


/* smooth arc gauge: grey track + orange fill */
static void gauge_arc(gfx_fb *fb, float a0, float a1, float frac, float r, float th)
{
    gfx_arc(fb, GFX_CX, GFX_CY + GFX_S(0.6f), GFX_S(r), GFX_S(th + 1.8f), a0, a1, U_EDGE);
    gfx_arc(fb, GFX_CX, GFX_CY, GFX_S(r), GFX_S(th), a0, a1, U_TRACK);
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    if (frac > 0.004f)
        gfx_arc(fb, GFX_CX, GFX_CY, GFX_S(r), GFX_S(th), a0, a0 + (a1 - a0) * frac, U_ORANGE);
}

/* radial tick-dash bank: unlit = neutral grey, lit = orange */
static void gauge_ticks(gfx_fb *fb, float a0, float a1, int n, float frac)
{
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    gfx_arc(fb, GFX_CX, GFX_CY, GFX_S(90.5f), GFX_S(9), a0, a1, U_WELL);
    int lit = (int)(frac * n + 0.5f);
    for (int i = 0; i < n; i++) {
        float a = a0 + (a1 - a0) * (i / (float)(n - 1));
        float x0, y0, x1, y1;
        polar(a, 87, &x0, &y0);
        polar(a, 94, &x1, &y1);
        gfx_line(fb, x0, y0, x1, y1, GFX_S(1.5f), i < lit ? U_ORANGE : U_TRACK);
    }
}

static void cloud(gfx_fb *fb, float cx, float cy, float s, uint16_t col)
{
    float S = GFX_S(s);
    gfx_fill_circle(fb, cx - S * 0.62f, cy + S * 0.1f, S * 0.32f, col);
    gfx_fill_circle(fb, cx + S * 0.42f, cy - S * 0.02f, S * 0.42f, col);
    gfx_fill_circle(fb, cx - S * 0.15f, cy - S * 0.15f, S * 0.46f, col);
    gfx_fill_rect(fb, (int)(cx - S * 0.68f), (int)(cy + S * 0.02f),
                  (int)(S * 1.36f), (int)(S * 0.48f), col);
}

static void heart(gfx_fb *fb, float cx, float cy, float s, uint16_t col)
{
    float S = GFX_S(s);
    gfx_line(fb, cx - S * 0.42f, cy - S * 0.26f, cx, cy + S * 0.48f, S * 0.90f, col);
    gfx_line(fb, cx + S * 0.42f, cy - S * 0.26f, cx, cy + S * 0.48f, S * 0.90f, col);
}

static void sun(gfx_fb *fb, float cx, float cy)
{
    gfx_fill_circle(fb, cx, cy, GFX_S(1.8f), U_ORANGE);
    for (int i = 0; i < 8; i++) {
        float a = i * PI / 4, sn = sinf(a), cs = cosf(a);
        gfx_line(fb, cx + sn*GFX_S(3), cy - cs*GFX_S(3),
                 cx + sn*GFX_S(4.2f), cy - cs*GFX_S(4.2f), GFX_S(0.75f), U_ORANGE);
    }
}

/* A side-view walking shoe, with heel, ankle opening, toe and a separate sole.
 * The local x axis follows its curved step readout; not a tiny toe-dot cluster. */
static void shoe(gfx_fb *fb, float cx, float cy, float angle, uint16_t col)
{
    const float strokes[][5] = {
        { -3.5f, -1.9f, -3.5f, 1.0f, 2.0f },
        { -3.5f,  1.0f,  3.5f, 1.0f, 2.2f },
        { -1.5f, -0.8f,  2.4f, 0.7f, 2.0f },
        { -4.0f,  3.2f,  4.5f, 3.2f, 0.9f },
    };
    float cs = cosf(angle), sn = sinf(angle);
    for (int i = 0; i < 4; i++) {
        const float *p = strokes[i];
        gfx_line(fb, cx + GFX_S(p[0]*cs - p[1]*sn), cy + GFX_S(p[0]*sn + p[1]*cs),
                 cx + GFX_S(p[2]*cs - p[3]*sn), cy + GFX_S(p[2]*sn + p[3]*cs),
                 GFX_S(p[4]), col);
    }
}

/* Open instrument frame: narrow heel, flared shoulders, tapered rails and a
 * clipped tip. Draw ONLY the rails so the forecast, gauges and other hand are
 * visible through the aperture. The slightly offset white face leaves a fine
 * shaded bevel, without painting a dark channel over the dial underneath. */
static void hand(gfx_fb *fb, float ang, float root, float tip, float width)
{
    float sn = sinf(ang), cs = cosf(ang), half = width * 0.5f;
    float corners[10][2] = {
        {-half * 0.58f, root}, {-half, root + 2.6f},
        {-half * 1.08f, root + 7}, {-half * 0.86f, tip - 3.5f},
        {-half * 0.40f, tip}, {half * 0.40f, tip},
        {half * 0.86f, tip - 3.5f}, {half * 1.08f, root + 7},
        {half, root + 2.6f}, {half * 0.58f, root}
    };
    float x[10], y[10];
    for (int i = 0; i < 10; i++) {
        x[i] = GFX_CX + GFX_S(corners[i][0] * cs + corners[i][1] * sn);
        y[i] = GFX_CY + GFX_S(corners[i][0] * sn - corners[i][1] * cs);
    }
    float rx, ry;
    polar(ang, root, &rx, &ry);
    gfx_line(fb, GFX_CX, GFX_CY, rx, ry, GFX_S(3.8f), U_BEVEL);
    gfx_line(fb, GFX_CX, GFX_CY, rx, ry, GFX_S(2.8f), U_NUMHI);
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 10; i++) {
            int j = (i + 1) % 10;
            float offset = pass ? -GFX_S(0.18f) : 0;
            gfx_line(fb, x[i] + offset, y[i] + offset, x[j] + offset, y[j] + offset,
                     GFX_S(pass ? 2.6f : 3.3f), pass ? U_NUMHI : U_BEVEL);
        }
}

static void commafmt(char *out, int n)
{
    if (n < 0) n = 0;
    if (n < 1000) snprintf(out, 12, "%d", n);
    else          snprintf(out, 12, "%d,%03d", n / 1000, n % 1000);
}

/* Small, cached vector outlines for the reference's chamfered instrument
 * numerals. Complication typography remains Saira. No new font dependency. */
static int in_polygon(const float (*p)[2], int n, float x, float y)
{
    int inside = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((p[i][1] > y) != (p[j][1] > y) &&
            x < (p[j][0] - p[i][0]) * (y - p[i][1]) / (p[j][1] - p[i][1]) + p[i][0])
            inside = !inside;
    return inside;
}

static void dial_shape(gfx_fb *bg, const float (*p)[2], int n,
                       const float (*hole)[2], int nh, float x, float y,
                       float width, float height, float sx, float sy, uint16_t col)
{
    /* Only called while composing the full cached dial, never in record mode. */
    if (bg->rec) return;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    int x1 = (int)ceilf(x + width * sx), y1 = (int)ceilf(y + height * sy);
    for (int py = y0; py < y1; py++)
        for (int px = x0; px < x1; px++) {
            int coverage = 0;
            for (int yy = 0; yy < 2; yy++)
                for (int xx = 0; xx < 2; xx++) {
                    float lx = (px + 0.25f + xx * 0.5f - x) / sx;
                    float ly = (py + 0.25f + yy * 0.5f - y) / sy;
                    coverage += in_polygon(p, n, lx, ly) &&
                                (!nh || !in_polygon(hole, nh, lx, ly));
                }
            if (coverage) gfx_px_blend(bg, px, py, col, (uint8_t)(coverage * 255 / 4));
        }
}

/* Small cached shapes, rotated around their anchor. Local y points down.
 * Shared by square-ended dial batons and the battery's stepped silhouette. */
static void dial_polygon(gfx_fb *bg, const float (*p)[2], int n,
                         float cx, float cy, float angle, uint16_t col)
{
    float q[16][2], cs = cosf(angle), sn = sinf(angle);
    float x0 = GFX_W, y0 = GFX_H, x1 = 0, y1 = 0;
    for (int i = 0; i < n; i++) {
        q[i][0] = cx + GFX_S(p[i][0] * cs - p[i][1] * sn);
        q[i][1] = cy + GFX_S(p[i][0] * sn + p[i][1] * cs);
        x0 = fminf(x0, q[i][0]); x1 = fmaxf(x1, q[i][0]);
        y0 = fminf(y0, q[i][1]); y1 = fmaxf(y1, q[i][1]);
    }
    for (int i = 0; i < n; i++) { q[i][0] -= x0; q[i][1] -= y0; }
    dial_shape(bg, q, n, NULL, 0, x0, y0, x1 - x0, y1 - y0, 1, 1, col);
}

static void dial_marker(gfx_fb *bg, float angle, float outer, float length,
                        float width, uint16_t col)
{
    float x, y; polar(angle, outer, &x, &y);
    const float p[][2] = {{-width/2,0},{width/2,0},{width/2,length},{-width/2,length}};
    dial_polygon(bg, p, 4, x + GFX_S(0.35f), y + GFX_S(0.65f), angle, U_EDGE);
    dial_polygon(bg, p, 4, x, y, angle, col);
}

static void battery_icon(gfx_fb *bg, float angle, float radius)
{
    static const float body[][2] = {
        {-1,-5.4f},{1,-5.4f},{1,-4.4f},{2,-4.4f},{2.6f,-3.8f},
        {2.6f,3.8f},{2,4.4f},{-2,4.4f},{-2.6f,3.8f},{-2.6f,-3.8f},
        {-2,-4.4f},{-1,-4.4f}
    };
    float x, y; polar(angle, radius, &x, &y);
    dial_polygon(bg, body, 12, x + GFX_S(0.45f), y + GFX_S(0.65f), angle, U_EDGE);
    dial_polygon(bg, body, 12, x, y, angle, U_ORANGE);
    const float shine[][2] = {{-1.9f,-3.2f},{-1.25f,-3.2f},{-1.25f,2.9f},{-1.9f,2.9f}};
    dial_polygon(bg, shine, 4, x, y, angle, GFX_RGB(255, 140, 48));
}

static void bignum(gfx_fb *bg, const char *s, float cx, float cy, float size, uint16_t col)
{
    static const float one[][2] = {{0,4},{6,0},{9,0},{9,28},{5,28},{5,6},{0,9}};
    static const float two[][2] = {{0,3},{3,0},{18,0},{21,3},{21,11},{4,21},
        {4,24},{21,24},{21,28},{0,28},{0,19},{17,9},{17,4},{4,4},{4,8},{0,8}};
    static const float nine[][2] = {{2,0},{18,0},{20,2},{20,26},{18,28},{2,28},
        {0,26},{0,22},{4,22},{4,24},{16,24},{16,16},{2,16},{0,14},{0,2}};
    static const float six[][2] = {{2,0},{18,0},{20,2},{20,6},{16,6},{16,4},
        {4,4},{4,12},{18,12},{20,14},{20,26},{18,28},{2,28},{0,26},{0,2}};
    static const float hole9[][2] = {{4,4},{16,4},{16,12},{4,12}};
    static const float hole6[][2] = {{4,16},{16,16},{16,24},{4,24}};
    float sy = size / 28, sx = sy * 1.13f, y = cy - size * 0.5f;
    if (s[0] == '1') {
        float x = cx - 34 * sx * 0.5f;
        dial_shape(bg, one, 7, NULL, 0, x, y, 9, 28, sx, sy, col);
        dial_shape(bg, two, 16, NULL, 0, x + 13*sx, y, 21, 28, sx, sy, col);
    } else {
        dial_shape(bg, s[0] == '9' ? nine : six, 15, s[0] == '9' ? hole9 : hole6, 4,
                   cx - 10*sx, y, 20, 28, sx, sy, col);
    }
}

/* Cached-base-only annular section. Clipping a ring gives square radial ends,
 * unlike the round caps of gfx_arc. 0 = upper semicircle; 1 = lower-right. */
static void section(gfx_fb *bg, float inner, float outer, int lower)
{
    gfx_fb clip = *bg;
    if (lower) {
        clip.px += (GFX_H / 2) * GFX_W;
        clip.y_base = GFX_H / 2;
        clip.h = GFX_H - GFX_H / 2;
        clip.cx0 = GFX_W / 2;
    } else {
        clip.h = GFX_H / 2;
    }
    gfx_ring(&clip, GFX_CX, GFX_CY, GFX_S((inner + outer) * 0.5f),
             GFX_S(outer - inner), U_WELL);
    /* Dark inner lip and a thin lower highlight suggest a shallow recess. */
    gfx_ring(&clip, GFX_CX, GFX_CY, GFX_S(outer - 0.65f), GFX_S(1.5f), U_EDGE);
    gfx_ring(&clip, GFX_CX, GFX_CY, GFX_S(outer), GFX_S(0.65f), U_SEAM);
    gfx_ring(&clip, GFX_CX, GFX_CY, GFX_S(inner), GFX_S(0.65f), U_SEAM);
    float a0 = lower ? PI * 0.5f : -PI * 0.5f;
    float a1 = lower ? PI : PI * 0.5f;
    for (int i = 0; i < 2; i++) {
        float x0, y0, x1, y1, a = i ? a1 : a0;
        polar(a, inner, &x0, &y0); polar(a, outer, &x1, &y1);
        gfx_line(bg, x0, y0, x1, y1, GFX_S(0.65f), U_SEAM);
    }
}

/* Upper forecast sector: five tangent labels over a concentric fan grid.
 * Five live two-hour forecast slots from the Wi-Fi weather cache. */
static void forecast(gfx_fb *bg)
{
    section(bg, 30, 70, 0);
    gfx_fb fan = *bg;
    fan.h = GFX_H / 2;
    gfx_ring(&fan, GFX_CX, GFX_CY, GFX_S(44), GFX_S(28), U_FAN);
    for (int i = 0; i < 5; i++)
        gfx_arc(bg, GFX_CX, GFX_CY, GFX_S(30 + i * 7), GFX_S(0.55f),
                -PI * 0.5f, PI * 0.5f, U_ROSE);
    for (int i = 0; i <= 24; i++) {
        float a = -PI * 0.5f + i * (PI / 24), x0, y0, x1, y1;
        polar(a, 30, &x0, &y0); polar(a, 58, &x1, &y1);
        gfx_line(bg, x0, y0, x1, y1, GFX_S(0.5f), U_ROSE);
    }
    for (int i = 0; i < 5; i++) {
        float a = (i - 2) * (PI / 5.0f), x, y;
        char hour[12]="--";
        int available=s_weather.valid && !s_weather.stale;
        if(available) {
            int h=(int)((s_weather.hours[i].epoch+s_weather.utc_offset_seconds)/3600%24);
            snprintf(hour,sizeof hour,"%d %s",h%12 ? h%12 : 12,h>=12 ? "PM" : "AM");
        }
        pathtext_arc(bg, PATHTEXT_CONDENSED, hour, GFX_S(60), a,
                     GFX_S(8), U_LABEL);
        polar(a, 46, &x, &y);
        if(!available) continue;
        int condition=weather_condition(s_weather.hours[i].code);
        if(condition==0) {sun(bg,x,y);continue;}
        if (condition >= 3) {
            /* A small rain glyph breaks the row of cloud silhouettes. */
            for (int k = -2; k <= 2; k++) {
                float length = abs(k) == 2 ? 2.2f : 4;
                gfx_line(bg, x + GFX_S(k * 2 + 1), y - GFX_S(length * 0.5f),
                         x + GFX_S(k * 2 - 1), y + GFX_S(length * 0.5f), GFX_S(0.9f), U_ICON);
            }
        } else {
            cloud(bg, x + GFX_S(0.35f), y + GFX_S(0.65f), 5.8f, U_EDGE);
            cloud(bg, x, y, 5.2f, U_ICON);
        }
    }
}

static void instrument_hub(gfx_fb *bg)
{
    /* Decorative instrument rose; this board has no compass heading sensor. */
    gfx_fill_circle(bg, GFX_CX, GFX_CY, GFX_S(29), U_BG);
    gfx_ring(bg, GFX_CX, GFX_CY, GFX_S(29), GFX_S(0.8f), U_SEAM);
    for (int i = 0; i < 60; i++) {
        /* The letters replace three ticks at each cardinal, in the same lane. */
        int cardinal = i % 15;
        if (cardinal <= 1 || cardinal >= 14) continue;
        float a = i * (2 * PI / 60), x0, y0, x1, y1;
        polar(a, i % 5 == 0 ? 22.2f : (i % 5 == 2 || i % 5 == 3 ? 24.2f : 23.3f), &x0, &y0);
        polar(a, 27.5f, &x1, &y1);
        gfx_line(bg, x0, y0, x1, y1, GFX_S(i % 5 == 0 ? 0.9f : 0.65f), U_ROSEHI);
    }
    const char *card[4] = { "N", "E", "S", "W" };
    for (int i = 0; i < 4; i++) {
        pathtext_arc_inward(bg, PATHTEXT_CONDENSED, card[i], GFX_S(28.3f),
                            i * (PI * 0.5f), GFX_S(6.4f), U_LABEL);
    }
}

static void health_bands(gfx_fb *bg)
{
    /* Independent slots for later live sources, including IMU data.
     * Feels-like comes from Wi-Fi; no optical heart-rate sensor is fitted. */
    section(bg, 32, 47, 1);
    section(bg, 50, 68, 1);
    char rate[16], feels[28];
    snprintf(rate, sizeof rate, "-- bpm");
    if(s_weather.valid && !s_weather.stale) snprintf(feels, sizeof feels, "Feels like temp. %d", s_weather.feels);
    else snprintf(feels,sizeof feels,"Feels like temp. --");
    pathtext_arc_inward(bg, PATHTEXT_CONDENSED, rate, GFX_S(43), 2.22f,
                        GFX_S(7.5f), U_LABEL);
    float x, y;
    polar(2.82f, 39, &x, &y);
    heart(bg, x, y, 4.2f, U_ICON);
    pathtext_arc_inward(bg, PATHTEXT_CONDENSED, feels, GFX_S(64), 2.34f,
                        GFX_S(7), U_LABEL);
    /* Degree mark at the text's endpoint, with the same inward orientation. */
    float end = 2.34f - pathtext_width(PATHTEXT_CONDENSED, feels, GFX_S(7)) / GFX_S(128);
    polar(end - 0.05f, 58.5f, &x, &y);
    gfx_ring(bg, x, y, GFX_S(1.1f), GFX_S(0.65f), U_LABEL);
}

/* ---- cached static furniture ---- */
static void ultra_base(gfx_fb *bg, int batt, int steps,
                       const char *sb, const char *bb, const char *db)
{
    gfx_clear(bg, U_BG);
    gfx_ring(bg, GFX_CX, GFX_CY, GFX_S(97), GFX_S(0.6f), GFX_RGB(35, 38, 43));
    gfx_ring(bg, GFX_CX, GFX_CY, GFX_S(119), GFX_S(0.6f), GFX_RGB(24, 27, 33));

    forecast(bg);
    health_bands(bg);
    instrument_hub(bg);

    /* Minute markers, rotated bezel numbers and gauges retain separate lanes. */
    for (int i = 0; i < 60; i++) {
        float a = i * (2 * PI / 60.0f);
        int beat = i % 5, cardinal = (i % 15) == 0;
        /* Bold quarter-hours, broad hour batons, medium flanking marks and
         * shorter fine marks: a repeating five-minute rhythm like the photo. */
        float length = cardinal ? 7.2f : !beat ? 5.8f : (beat == 1 || beat == 4) ? 4.6f : 3.5f;
        float width = cardinal ? 3.0f : !beat ? 2.4f : (beat == 1 || beat == 4) ? 1.15f : 0.85f;
        dial_marker(bg, a, 118, length, width, !beat ? U_NUMHI : U_TICKHR);
    }
    static const float index[][2] = {{0,0},{7,0},{3.5f,4.5f}};
    dial_shape(bg, index, 3, NULL, 0, GFX_CX - GFX_S(3.5f), GFX_CY - GFX_S(106),
               7, 4.5f, GFX_S(1), GFX_S(1), U_ORANGE);
    for (int h = 0; h < 12; h++) {
        if (h == 0 || h == 3 || h == 6 || h == 9) continue;
        pathtext_arc(bg, PATHTEXT_CONDENSED, HN[h], GFX_S(101),
                     h * (2 * PI / 12.0f), GFX_S(8.5f), U_NUM);
    }
    bignum(bg, "12", GFX_CX + GFX_S(0.6f), GFX_CY - GFX_S(83.1f), GFX_S(25), U_EDGE);
    bignum(bg, "9", GFX_CX - GFX_S(82.4f), GFX_CY + GFX_S(0.9f), GFX_S(27), U_EDGE);
    bignum(bg, "6", GFX_CX + GFX_S(0.6f), GFX_CY + GFX_S(84.9f), GFX_S(27), U_EDGE);
    bignum(bg, "12", GFX_CX,             GFX_CY - GFX_S(84), GFX_S(25), U_NUMHI);
    bignum(bg, "9",  GFX_CX - GFX_S(83), GFX_CY,             GFX_S(27), U_NUMHI);
    bignum(bg, "6",  GFX_CX,             GFX_CY + GFX_S(84), GFX_S(27), U_NUMHI);

    /* Live steps and battery follow the curvature of their outer gauges. */
    gauge_arc(bg, -1.18f, -0.42f, steps / 10000.0f, 90.5f, 6.0f);
    float step_size = GFX_S(8);
    float step_width = pathtext_width(PATHTEXT_CONDENSED, sb, step_size);
    if (step_width > GFX_S(43)) step_size *= GFX_S(43) / step_width;
    pathtext_arc(bg, PATHTEXT_CONDENSED, sb, GFX_S(78.5f), -0.70f, step_size, U_LABEL);
    float x, y;
    polar(-1.30f, 90.5f, &x, &y);
    shoe(bg, x, y, -1.30f, U_ORANGE);
    gauge_ticks(bg, 0.52f, 1.30f, 15, batt / 100.0f);
    pathtext_arc(bg, PATHTEXT_CONDENSED, bb, GFX_S(77), 1.06f, GFX_S(8), U_LABEL);
    battery_icon(bg, 0.34f, 90.0f);
    gauge_ticks(bg, 1.96f, 2.84f, 19, s_brightness / 255.0f);
    char brightness[16];
    snprintf(brightness,sizeof brightness,"%d%%",(s_brightness*100+127)/255);
    pathtext_arc_inward(bg,PATHTEXT_CONDENSED,brightness,GFX_S(82),2.40f,GFX_S(8),U_LABEL);
    polar(1.80f, 90.5f, &x, &y);
    sun(bg, x, y);

    /* Small date window leaves the upper/lower arc boundary unobstructed. */
    float dx, dy; polar(PI * 0.5f, 84, &dx, &dy);
    static const float date_case[][2] = {{3,0},{21,0},{24,3},{24,19},{21,22},{3,22},{0,19},{0,3}};
    static const float date_inset[][2] = {{2.5f,0},{19.5f,0},{22,2.5f},{22,17.5f},
        {19.5f,20},{2.5f,20},{0,17.5f},{0,2.5f}};
    dial_shape(bg, date_case, 8, NULL, 0, dx-GFX_S(12), dy-GFX_S(11),
               24, 22, GFX_S(1), GFX_S(1), U_SEAM);
    dial_shape(bg, date_inset, 8, NULL, 0, dx-GFX_S(11), dy-GFX_S(10),
               22, 20, GFX_S(1), GFX_S(1), U_WELL);
    pathtext_draw(bg, PATHTEXT_BOLD, db, (int)dx, (int)(dy + GFX_S(5.75f)),
                  GFX_S(11.5f), U_NUMHI, 1);

    /* A self-contained score well fills the remaining lower-left quadrant. */
    float scx = GFX_CX - GFX_S(49), scy = GFX_CY + GFX_S(48);
    gfx_fill_circle(bg, scx + GFX_S(0.5f), scy + GFX_S(1.0f), GFX_S(29), U_EDGE);
    gfx_fill_circle(bg, scx, scy, GFX_S(28), U_WELL);
    gfx_ring(bg, scx, scy, GFX_S(28), GFX_S(0.8f), U_SEAM);
    /* No optical SpO2 sensor is fitted. Keep the reference's square tip at
     * the empty track's start; no fabricated saturation or health verdict. */
    float start = -2.42f, end = start;
    gfx_arc(bg, scx, scy, GFX_S(23.5f), GFX_S(4.5f), start, 2.42f, GFX_RGB(88, 40, 8));
    const float tip[][2]={{-2.6f,-2.6f},{2.6f,-2.6f},{2.6f,2.6f},{-2.6f,2.6f}};
    dial_polygon(bg,tip,4,scx+sinf(end)*GFX_S(23.5f),scy-cosf(end)*GFX_S(23.5f),end,U_NUMHI);
    const char *score="--%";
    pathtext_draw(bg, PATHTEXT_BOLD, score, (int)scx, (int)(scy + GFX_S(1)),
                  GFX_S(11), U_NUMHI, 1);
    pathtext_draw(bg, PATHTEXT_CONDENSED, "SpO2", (int)scx,
                  (int)(scy + GFX_S(11)), GFX_S(7.5f), U_LABEL, 1);
    float icy = scy + GFX_S(19);
    gfx_arc(bg, scx, icy, GFX_S(3.4f), GFX_S(1.2f), 0.25f, 5.95f, U_ORANGE);
    gfx_line(bg, scx, icy, scx + GFX_S(2.1f), icy - GFX_S(2.4f), GFX_S(1.2f), U_ORANGE);
}

void face_ultra_draw(gfx_fb *fb, const hal_datetime *t)
{
    static uint32_t s_fid = 0xFFFFFFFFu;
    static int s_batt = -1, s_steps = -1, s_day = -1;
    static char s_sb[12], s_bb[16], s_db[4];
    if (wos_frame_id != s_fid) {
        s_fid = wos_frame_id;
        hal_power power; hal_power_get(&power);
        hal_activity activity;hal_activity_get(&activity);
        hal_weather weather;hal_weather_get(&weather);
        int batt=power.battery_valid && power.age_ms<=3000 ? power.battery_percent :
            (power.external_power==1 && power.age_ms<=3000 ? -2 : -1);
        int steps=activity.steps>2147483647u ? 2147483647 : (int)activity.steps;
        if (batt > 100) batt = 100;
        if (steps < 0) steps = 0;
        int brightness=g_brightness<0 ? 0 : g_brightness>255 ? 255 : g_brightness;
        if (batt != s_batt || steps != s_steps || t->day != s_day ||
            brightness!=s_brightness || weather.generation!=s_weather.generation ||
            weather.valid!=s_weather.valid || weather.stale!=s_weather.stale) {
            s_weather=weather;s_brightness=brightness;
            s_batt = batt;
            s_steps = steps;
            s_day = t->day;
            commafmt(s_sb, s_steps);
            if(s_batt>=0) snprintf(s_bb, sizeof s_bb, "%d%%", s_batt);
            else snprintf(s_bb,sizeof s_bb,"%s",s_batt==-2 ? "USB" : "--");
            snprintf(s_db, sizeof s_db, "%02d", s_day);
            layer_invalidate(LAY_ULTRA);
        }
    }

    gfx_fb *bg = layer_need_render(LAY_ULTRA);
    if (bg) ultra_base(bg, s_batt, s_steps, s_sb, s_bb, s_db);
    layer_blit(fb, LAY_ULTRA);

    /* ---- hands (live) ---- */
    float mn  = (t->minute + t->second / 60.0f) * (2 * PI / 60.0f);
    float hrA = (t->hour % 12 + t->minute / 60.0f) * (2 * PI / 12.0f);
    float sc  = (t->second + t->millis / 1000.0f) * (2 * PI / 60.0f);
    hand(fb, hrA, 12, 59, 10.2f);
    hand(fb, mn,  12, 99, 9.3f);

    /* Fine second needle and a pointed orange counterbalance, like the reference. */
    float sx2, sy2, tx, ty;
    polar(sc, 106, &sx2, &sy2);
    polar(sc + PI, 29, &tx, &ty);
    gfx_line(fb, GFX_CX, GFX_CY, sx2, sy2, GFX_S(0.8f), U_ORANGE);
    gfx_taper(fb, GFX_CX, GFX_CY, tx, ty, GFX_S(1.8f), GFX_S(1.2f), U_ORANGE);
    float tipx, tipy; polar(sc + PI, 32, &tipx, &tipy);
    gfx_taper(fb, tx, ty, tipx, tipy, GFX_S(1.2f), 0, U_ORANGE);
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(5.2f), U_ORANGE);
    gfx_fill_circle(fb, GFX_CX, GFX_CY, GFX_S(2.3f), U_EDGE);
}
