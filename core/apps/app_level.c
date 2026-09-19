/* app_level.c — Spirit level / inclinometer driven by the IMU accelerometer.
 *
 * PLAIN-ENGLISH ALGORITHM
 * -----------------------
 * 1. READ ACCEL: each frame, hal_imu_read() gives the gravity vector in g's
 *    (ax, ay, az). When the watch lies perfectly flat, screen up, gravity
 *    points straight into the screen: (ax, ay, az) ~ (0, 0, 1).
 *
 * 2. PITCH/ROLL MATH: recover the two tilt angles with atan2 (in degrees):
 *        pitch = atan2(ay, az)   -> tilt around the horizontal (X) axis
 *        roll  = atan2(ax, az)   -> tilt around the vertical  (Y) axis
 *    atan2 keeps the sign and full range, so leaning either way is signed.
 *    Sanity: (0,0,1)->0/0, (0,1,0)->pitch 90, (1,0,0)->roll 90.
 *
 * 3. BUBBLE MAPPING: like a real bubble level, the bubble drifts toward the
 *    downhill side. We offset it from screen center by (ax, ay) scaled so a
 *    full 1 g lean pushes it to the outer ring, then clamp its center inside
 *    the ring so it can never escape the target. The position is low-pass
 *    filtered frame-to-frame so the bubble glides instead of jittering.
 *
 * 4. LEVEL TEST: when BOTH |pitch| and |roll| are within LEVEL_DEG (~2 deg)
 *    the surface is flat enough -> bubble turns GFX_GREEN and we show "LEVEL";
 *    otherwise the bubble is GFX_ORANGE.
 */
#include <math.h>
#include <stdio.h>

#ifndef SELFTEST
#include "apps.h"
#include "../font.h"
#endif

#define PI 3.14159265f
#define LEVEL_DEG 2.0f   /* both angles within this -> "LEVEL" */

/* Recover pitch and roll (degrees) from a gravity/accel vector.
 * Pure math, no hardware — shared by the app and the self-test. */
static void accel_angles(float ax, float ay, float az,
                         float *pitch, float *roll)
{
    *pitch = atan2f(ay, az) * 180.0f / PI;
    *roll  = atan2f(ax, az) * 180.0f / PI;
}

#ifndef SELFTEST

/* smoothed bubble center, in screen pixels; sentinel < 0 means "uninitialized" */
static float s_bx = -1.0f, s_by = -1.0f;

static void on_start(void)
{
    s_bx = s_by = -1.0f;   /* snap to the live reading on first draw */
}

static int on_event(const wos_event *ev)
{
    /* Tap re-centers the smoother so it re-settles crisply. */
    if (ev->type == WOS_EV_TAP) {
        s_bx = s_by = -1.0f;
        return 1;
    }
    return 0;
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);

    /* --- read the accelerometer --- */
    hal_imu m = { 0, 0, 1, 0, 0, 0 };
    hal_imu_read(&m);

    float pitch, roll;
    accel_angles(m.ax, m.ay, m.az, &pitch, &roll);
    int level = (fabsf(pitch) <= LEVEL_DEG) && (fabsf(roll) <= LEVEL_DEG);
    uint16_t bubble_col = level ? GFX_GREEN : GFX_ORANGE;

    /* --- target: concentric rings + crosshair --- */
    float r_out = GFX_S(96);
    float bub_r = GFX_S(11);
    float r_max = r_out - bub_r;          /* keep the bubble fully inside */

    gfx_ring(fb, GFX_CX, GFX_CY, r_out,         GFX_S(2.5f), GFX_GREY);
    gfx_ring(fb, GFX_CX, GFX_CY, GFX_S(64),     GFX_S(1.5f), GFX_DGREY);
    /* inner "level" tolerance ring, tinted by current state */
    gfx_ring(fb, GFX_CX, GFX_CY, GFX_S(18),     GFX_S(1.5f),
             level ? GFX_GREEN : GFX_DGREY);

    /* crosshair lines through the center */
    gfx_line(fb, GFX_CX - r_out, GFX_CY, GFX_CX + r_out, GFX_CY,
             GFX_S(1.5f), GFX_DGREY);
    gfx_line(fb, GFX_CX, GFX_CY - r_out, GFX_CX, GFX_CY + r_out,
             GFX_S(1.5f), GFX_DGREY);

    /* --- bubble target position from tilt, clamped inside the ring --- */
    float tx = GFX_CX + m.ax * r_max;
    float ty = GFX_CY + m.ay * r_max;
    float dx = tx - GFX_CX, dy = ty - GFX_CY;
    float d  = sqrtf(dx * dx + dy * dy);
    if (d > r_max) { tx = GFX_CX + dx * r_max / d; ty = GFX_CY + dy * r_max / d; }

    /* smooth toward the target (or snap on first frame / after a tap) */
    if (s_bx < 0.0f) { s_bx = tx; s_by = ty; }
    else { s_bx += (tx - s_bx) * 0.25f; s_by += (ty - s_by) * 0.25f; }

    /* subtle outline so the bubble reads on the dark target */
    gfx_fill_circle(fb, s_bx, s_by, bub_r + GFX_S(1.5f), GFX_DGREY);
    gfx_fill_circle(fb, s_bx, s_by, bub_r, bubble_col);

    /* --- readouts --- */
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(40),
                       "LEVEL", GFX_GREY);

    char buf[24];
    snprintf(buf, sizeof buf, "P %d\xB0  R %d\xB0",
             (int)lroundf(pitch), (int)lroundf(roll));
    font_draw_centered(fb, &font_medium, (int)GFX_CX, (int)GFX_S(202),
                       buf, GFX_WHITE);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(220),
                       level ? "LEVEL" : "TILTED",
                       bubble_col);
}

const wos_app app_level = { "Level", on_start, on_event, on_draw,
                            GFX_GREEN, ICON_LEVEL };

#endif /* !SELFTEST */

#ifdef SELFTEST
#include <assert.h>
int main(void)
{
    float p, r;
    const float EPS = 0.01f;

    accel_angles(0.0f, 0.0f, 1.0f, &p, &r);   /* flat */
    assert(fabsf(p) < EPS && fabsf(r) < EPS);

    accel_angles(0.0f, 1.0f, 0.0f, &p, &r);   /* pitched forward 90 */
    assert(fabsf(p - 90.0f) < EPS);
    assert(fabsf(r) < EPS);

    accel_angles(1.0f, 0.0f, 0.0f, &p, &r);   /* rolled 90 */
    assert(fabsf(r - 90.0f) < EPS);
    assert(fabsf(p) < EPS);

    /* signed: leaning the other way is negative */
    accel_angles(0.0f, -1.0f, 0.0f, &p, &r);
    assert(fabsf(p + 90.0f) < EPS);

    printf("SELFTEST OK\n");
    return 0;
}
#endif
