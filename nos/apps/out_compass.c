/* out_compass.c — Compass (QMC5883L) with a level bubble (QMI8658), outdoor team.
 *
 * Dial: 72 tick dots around the bezel, N/E/S/W in dot-matrix caps and degree numerals
 * every 30 deg, rotating with the heading under a fixed red lubber mark at 12 o'clock.
 * The heading is low-passed by a damped spring on the unwrapped angle (always the short
 * way round), so the dial moves like a damped needle (~0.3 s) and settles exactly,
 * after which the screen costs nothing.
 *
 * Cost: the whole dial is ONE damage command (bbox + hash of the quantised heading and
 * the build-in progress). In raster mode every dot is rejected against the stripe
 * before it is stamped, and positions come from an angle-addition recurrence (no trig
 * per dot). A moving dial repaints the dial's box (bus-bound ~20 ms).
 *
 * States: module absent; needs calibration (figure-8 prompt + dot progress ring);
 * dial (tap: field strength + Calibrate + the Level hint); level (long press toggles). */
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

#define PI_F 3.14159265f
#define DEG2RAD (PI_F / 180.0f)

/* dial geometry: the ring sits 33 px inside the glass so the lubber mark has its own band */
#define R_TICK   200.0f
#define R_NUM    170.0f
#define R_CARD   168.0f
#define N_TICK   72
#define IRIS_LAG 0.17f      /* iris entries start their build at 40% of the iris */

/* details: Calibrate button */
#define CAL_W    150
#define CAL_Y    336

enum { M_ABSENT = 0, M_CAL, M_CAL_DONE, M_DIAL, M_LEVEL };

static struct {
    int mode;
    hal_mag mag;
    mspring hd;            /* displayed heading, unwrapped degrees */
    float target;          /* unwrapped target */
    float shown;           /* quantised display heading 0..360 (what draw uses) */
    float build;           /* build-in 0..1 */
    float t_mode;          /* nos_t when the mode began */
    int details;           /* tap: field strength + recalibrate */
    int pressed;           /* recalibrate pill pressed */
    float cal_shown;       /* progress ring value (smoothed) */
    float fig_t;           /* figure-8 animation phase 0..1 (quantised) */
    int recal, recal_low;  /* manual recalibration: wait for progress to restart */
    /* heading readout morph */
    char ro_cur[8], ro_prev[8];
    float ro_t0, ro_changed;
    /* level */
    mspring bx, by;
    float tilt_x, tilt_y;  /* degrees */
    int level_ok;
    float bub_x, bub_y;    /* quantised bubble offset */
} C;

/* ------------------------------------------------------------- helpers */
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
static float wrap360(float a)
{
    a = fmodf(a, 360.0f);
    return a < 0 ? a + 360.0f : a;
}

static const char *cardinal8(float h)
{
    static const char *n[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
    int i = (int)floorf(wrap360(h + 22.5f) / 45.0f) & 7;
    return n[i];
}

static void set_mode(int m)
{
    if (C.mode == m) return;
    C.mode = m;
    C.t_mode = nos_t;
    C.build = 0;
    C.details = 0;
    C.pressed = 0;
    nos_invalidate();
}

static void start_cal(int manual)
{
    C.cal_shown = 0;
    C.recal = manual; C.recal_low = 0;
    set_mode(M_CAL);
    hal_mag_calibrate(1);
}

/* ------------------------------------------------------------- lifecycle */
static void compass_enter(void)
{
    if (nos_revealed() && C.mode >= 0 && C.mode != M_ABSENT) return;   /* back: keep state, no rebuild */
    memset(&C, 0, sizeof C);
    C.mode = -1;
    hal_mag_get(&C.mag);
    float h = C.mag.valid ? C.mag.heading_deg : 0.0f;
    msp_init(&C.hd, h, 0.32f, 0.86f);    /* a damped needle: ~0.3 s, a breath of settle */
    C.target = h;
    C.shown = wrap360(h);
    msp_init(&C.bx, 0, SPRING_SNAPPY);
    msp_init(&C.by, 0, SPRING_SNAPPY);
    if (!(hal_ext_present() & EXT_MAG)) set_mode(M_ABSENT);
    else if (!C.mag.calibrated) start_cal(0);
    else set_mode(M_DIAL);
    C.t_mode = nos_t + IRIS_LAG;
}

static void compass_leave(void)
{
    if (C.mode == M_CAL) hal_mag_calibrate(0);
}

static void readout_set(void)
{
    char t[8];
    snprintf(t, sizeof t, "%d\xB0", (int)floorf(C.shown + 0.5f) % 360);
    if (!strcmp(t, C.ro_cur)) return;
    /* morph a slow tick (the needle settling by a degree); while the dial spins the
     * digits change every frame and a morph would never finish, so cut cleanly */
    if (C.ro_cur[0] && strlen(t) == strlen(C.ro_cur) && nos_t - C.ro_changed > 0.2f) {
        strcpy(C.ro_prev, C.ro_cur); C.ro_t0 = nos_t;
    } else { strcpy(C.ro_prev, t); C.ro_t0 = -10; }
    strcpy(C.ro_cur, t);
    C.ro_changed = nos_t;
}

static void tick_heading(float dt)
{
    float raw = C.mag.heading_deg;
    /* retarget the short way round; a small deadband keeps a still wrist still */
    float d = wrap180(raw - wrap360(C.target));
    if (fabsf(d) > 0.6f) C.target += d;
    msp_to(&C.hd, C.target);
    msp_step(&C.hd, dt);
    if (fabsf(C.hd.x - C.target) < 0.05f && fabsf(C.hd.v) < 0.5f) { C.hd.x = C.target; C.hd.v = 0; }
    /* keep the unwrapped values bounded */
    if (C.hd.x > 720.0f || C.hd.x < -720.0f) {
        float k = 360.0f * floorf(C.hd.x / 360.0f);
        C.hd.x -= k; C.target -= k; C.hd.target -= k;
    }
    /* 0.5 deg steps: the settle tail stops repainting once nothing visibly moves */
    C.shown = wrap360(floorf(C.hd.x * 2.0f + 0.5f) / 2.0f);
    readout_set();
}

static void tick_level(float dt)
{
    hal_imu imu;
    float ax = 0, ay = 0, az = 1;
    if (hal_imu_read(&imu)) { ax = imu.ax; ay = imu.ay; az = imu.az; }
    float tx = atan2f(ax, fabsf(az) + 1e-3f) / DEG2RAD;
    float ty = atan2f(ay, fabsf(az) + 1e-3f) / DEG2RAD;
    C.tilt_x = tx; C.tilt_y = ty;
    /* the bubble floats uphill: opposite the gravity vector's tilt; 1 deg = 6 px */
    float px = m_clamp(-tx * 6.0f, -122.0f, 122.0f), py = m_clamp(ty * 6.0f, -122.0f, 122.0f);
    float r = sqrtf(px * px + py * py);
    if (r > 112.0f) { px *= 112.0f / r; py *= 112.0f / r; }
    msp_to(&C.bx, px); msp_to(&C.by, py);
    msp_step(&C.bx, dt); msp_step(&C.by, dt);
    C.bub_x = floorf(C.bx.x * 2.0f + 0.5f) / 2.0f;
    C.bub_y = floorf(C.by.x * 2.0f + 0.5f) / 2.0f;
    /* FLAT follows the bubble the user sees, not the raw tilt */
    int ok = sqrtf(C.bub_x * C.bub_x + C.bub_y * C.bub_y) < 6.0f;
    if (ok && !C.level_ok) nos_feedback(FB_TICK);
    C.level_ok = ok;
}

static void compass_tick(float dt)
{
    int present = hal_mag_get(&C.mag) && (hal_ext_present() & EXT_MAG);
    if (!present) {
        if (C.mode != M_ABSENT) { set_mode(M_ABSENT); }
        C.build = m_clamp((nos_t - C.t_mode) / UI_ENTRY_S, 0, 1);
        return;
    }
    if (C.mode == M_ABSENT) {             /* module plugged in while open */
        if (!C.mag.calibrated) start_cal(0);
        else set_mode(M_DIAL);
    }
    float age = nos_t - C.t_mode;
    switch (C.mode) {
    case M_CAL: {
        float p = m_clamp(C.mag.cal_progress, 0, 1);
        int done;
        if (C.recal) {                    /* the old calibration still says "calibrated" */
            if (p < 0.1f) C.recal_low = 1;
            done = C.recal_low && p >= 0.95f;
            if (!C.recal_low) p = 0;
        } else done = p >= 0.95f || C.mag.calibrated;
        C.cal_shown += (p - C.cal_shown) * m_clamp(dt * 8.0f, 0, 1);
        if (fabsf(p - C.cal_shown) < 0.002f) C.cal_shown = p;
        C.fig_t = floorf(fmodf((age > 0 ? age : 0) / 2.4f, 1.0f) * 72.0f) / 72.0f;   /* 2.4 s loop */
        C.build = m_clamp(age / UI_ENTRY_S, 0, 1);
        if (done) {
            hal_mag_calibrate(0);
            nos_feedback(FB_SUCCESS);
            set_mode(M_CAL_DONE);
        }
        nos_keep_awake();
        break;
    }
    case M_CAL_DONE:
        C.build = m_clamp(age / 0.3f, 0, 1);
        tick_heading(dt);
        if (age > 1.1f) { set_mode(M_DIAL); C.t_mode -= 0.12f; }   /* the ring hands over mid-sweep */
        break;
    case M_DIAL:
        C.build = m_clamp(age / 0.38f, 0, 1);
        tick_heading(dt);
        break;
    case M_LEVEL:
        C.build = m_clamp(age / UI_ENTRY_S, 0, 1);
        tick_level(dt);
        break;
    }
}

/* ------------------------------------------------------------- dial */
static void dial_draw(gfx_fb *fb)
{
    const float H = C.shown, b = C.build;
    if (fb->rec) {
        unsigned h = gfx_hash_f(gfx_hash_f(0xC0A5u, H), floorf(b * 64.0f));
        int e = (int)(R_TICK + 6.0f);
        gfx_rec_cmd(fb, UI_CX - e, UI_CY - e, UI_CX + e, UI_CY + e, h);
        return;
    }
    /* angle of tick 0 on screen is -H; step +5 deg via rotation recurrence */
    float a0 = -H * DEG2RAD;
    float s = gfx_fast_sin(a0), c = gfx_fast_cos(a0);
    const float ss = 0.0871557427f, cs = 0.9961946981f;   /* sin/cos 5 deg */
    for (int i = 0; i < N_TICK; i++) {
        /* build-in: ticks grow in a sweep clockwise from north over the first 70% */
        float g = m_clamp((b * 1.4f - (float)i / N_TICK * 0.9f) * 3.0f, 0, 1);
        if (b >= 1.0f) g = 1;
        if (g > 0) {
            float x = UI_CX + s * R_TICK, y = UI_CY - c * R_TICK;
            float d; uint16_t col;
            if (i == 0) { d = 9.0f; col = C_WHITE; }
            else if (i % 6 == 0) { d = 7.0f; col = C_PRI; }
            else if (i % 2 == 0) { d = 4.0f; col = C_SEC; }
            else { d = 3.2f; col = C_DIS; }
            d *= g;
            if (rows_hit(fb, y - d, y + d)) dot_disc(fb, x, y, d, col);
        }
        float ns = s * cs + c * ss, nc = c * cs - s * ss;
        s = ns; c = nc;
    }
    /* labels every 30 deg: cardinals in dot caps, numerals in mono; each appears as
     * the build sweep passes its tick, and all of them are on once the build ends */
    dot_style cs_st = { DOT_FONT_5X7, 4.2f, 3.4f, C_PRI, 0, 1, 0 };
    for (int k = 0; k < 12; k++) {
        float g = m_clamp((b * 1.75f - (float)(k * 6) / N_TICK * 0.9f) * 3.0f - 1.0f, 0, 1);
        if (b >= 1.0f) g = 1;
        if (g <= 0) continue;
        float a = ((float)(k * 30) - H) * DEG2RAD;
        float sn = gfx_fast_sin(a), cn = gfx_fast_cos(a);
        if (k % 3 == 0) {
            static const char *card[] = { "N", "E", "S", "W" };
            float x = UI_CX + sn * R_CARD, y = UI_CY - cn * R_CARD;
            if (!rows_hit(fb, y - 16, y + 16)) continue;
            cs_st.color = k == 0 ? C_WHITE : C_PRI;
            dot_text_anim(fb, &cs_st, x, y - 14.7f, card[k / 3], 1, g, DOT_REVEAL_CENTER, 1u);
        } else {
            float x = UI_CX + sn * R_NUM, y = UI_CY - cn * R_NUM;
            if (g < 1 || !rows_hit(fb, y - 10, y + 10)) continue;
            char t[4];
            snprintf(t, sizeof t, "%d", k * 30);
            ui_text(fb, UI_F_SMALL, (int)lroundf(x), (int)lroundf(y + 5), t, C_SEC, UI_CENTER, 0);
        }
    }
}

static void lubber_draw(gfx_fb *fb)
{
    /* a triangle of red dots pointing at the reading: 3-2-1, 12 px inside the glass */
    const float p = 5.6f, d = 4.4f;
    for (int r = 0; r < 3; r++)
        for (int k = 0; k <= 2 - r; k++)
            dot_disc(fb, UI_CX + (k - (2 - r) * 0.5f) * p, 12.0f + r * p * 0.87f, d, C_RED);
}

static void readout_draw(gfx_fb *fb)
{
    dot_style st = { DOT_FONT_TALL, UI_HEROW_PITCH, UI_HEROW_PITCH * 0.8f, C_WHITE, 0, 1, 1 };
    float p = m_clamp((C.build - 0.2f) / 0.8f, 0, 1);
    float mp = (nos_t - C.ro_t0) / 0.28f;
    /* the number and its degree mark are centred as one group */
    if (p < 1) dot_text_anim(fb, &st, UI_CX, UI_CY - 52, C.ro_cur, 1, p, DOT_REVEAL_RANDOM, 7u);
    else if (mp < 1) dot_text_morph(fb, &st, UI_CX, UI_CY - 52, C.ro_prev, C.ro_cur, 1, mp);
    else dot_text_a(fb, &st, UI_CX, UI_CY - 52, C.ro_cur, 1);
    if (C.details) {
        ui_label(fb, UI_CX, 150, "HOLD FOR LEVEL", C_DIS, UI_CENTER);
        char v[12];
        snprintf(v, sizeof v, "FIELD %d", (int)lroundf(C.mag.field_ut));
        ui_label_unit(fb, UI_CX, UI_CY + 52, v, "uT", C_SEC, UI_CENTER);
        ui_button(fb, UI_CX, CAL_Y, CAL_W, UI_BTN_H, "Calibrate", BTN_SECONDARY, C.pressed);
    } else if (p > 0) {
        ui_label(fb, UI_CX, UI_CY + 60, cardinal8(C.shown), C_SEC, UI_CENTER);
    }
}

/* ------------------------------------------------------------- calibration */
static void cal_draw(gfx_fb *fb)
{
    ui_title(fb, "CALIBRATE", 0);
    float b = C.build;
    /* progress: 60 dots around the bezel fill one by one */
    dot_ring(fb, UI_CX, UI_CY, R_TICK, 60, 6.0f, 0, C.cal_shown * 60.0f * b, C_WHITE, b > 0.5f ? C_LINE2 : 0);
    /* figure-8 (lemniscate of Gerono) as faint dots, a bright dot travelling it */
    const float ax = 64, ay = 34, cy = UI_CY - 34;
    if (fb->rec) {
        unsigned h = gfx_hash_f(gfx_hash_f(0xF16u, C.fig_t), floorf(b * 16));
        gfx_rec_cmd(fb, (int)(UI_CX - ax - 10), (int)(cy - ay - 10), (int)(UI_CX + ax + 10), (int)(cy + ay + 10), h);
    } else if (rows_hit(fb, cy - ay - 10, cy + ay + 10)) {
        for (int i = 0; i < 36; i++) {
            float t = i / 36.0f * 2 * PI_F;
            float x = UI_CX + ax * gfx_fast_sin(t), y = cy + ay * gfx_fast_sin(t) * gfx_fast_cos(t) * 2.0f;
            dot_disc(fb, x, y, 3.0f * b, C_DIS);
        }
        for (int k = 0; k < 5; k++) {
            float t = (C.fig_t - k * 0.018f) * 2 * PI_F;
            float x = UI_CX + ax * gfx_fast_sin(t), y = cy + ay * gfx_fast_sin(t) * gfx_fast_cos(t) * 2.0f;
            dot_disc(fb, x, y, (10.0f - k * 1.6f) * b, k == 0 ? C_WHITE : ui_dim(C_PRI, 0.75f - k * 0.13f));
        }
    }
    ui_text_wrap(fb, UI_F_BODY, UI_CX, 290, 290, 30, 2, "Move your wrist\nin a figure 8", C_PRI, UI_CENTER);
    char pc[12];
    snprintf(pc, sizeof pc, "%d%%", (int)(C.cal_shown * 100.0f + 0.5f));
    ui_label(fb, UI_CX, 372, pc, C_SEC, UI_CENTER);
}

static void cal_done_draw(gfx_fb *fb)
{
    dot_ring(fb, UI_CX, UI_CY, R_TICK, 60, 6.0f, 0, 60, C_WHITE, 0);
    dot_icon_anim(fb, DOT_ICON_CHECK, UI_CX, UI_CY - 30, 7.0f, 5.6f, C_WHITE, C.build, DOT_REVEAL_CENTER);
    ui_state(fb, UI_CX, UI_CY + 70, "CALIBRATED", C_PRI);
}

/* ------------------------------------------------------------- level */
static void level_draw(gfx_fb *fb)
{
    ui_title(fb, "LEVEL", 0);
    float b = C.build;
    const float cy = UI_CY - 10;
    /* fixed scale: outer ring, 1 deg target ring, crosshair dots */
    ui_circle_line(fb, UI_CX, cy, 140, 2, C_LINE2);
    ui_circle_line(fb, UI_CX, cy, 38, 2, C.level_ok ? C_WHITE : C_DIS);
    for (int k = 1; k <= 4; k++) {
        float o = 38 + k * 20.4f;
        uint16_t col = C_LINE2;
        dot_disc(fb, UI_CX - o, cy, 4, col); dot_disc(fb, UI_CX + o, cy, 4, col);
        dot_disc(fb, UI_CX, cy - o, 4, col); dot_disc(fb, UI_CX, cy + o, 4, col);
    }
    /* the bubble */
    ui_circle(fb, UI_CX + C.bub_x, cy + C.bub_y, 26.0f * b, C.level_ok ? C_WHITE : C_PRI);
    /* readout, clear of the ring (ring bottom y 363) */
    float tilt = sqrtf(C.tilt_x * C.tilt_x + C.tilt_y * C.tilt_y);
    if (C.level_ok) ui_state(fb, UI_CX, 398, "FLAT", C_PRI);
    else {
        char t[16];
        snprintf(t, sizeof t, "%.1f\xB0", (double)(floorf(tilt * 10 + 0.5f) / 10));
        dot_style st = { DOT_FONT_5X7, 4.0f, 3.3f, C_PRI, 0, 1, 1 };
        dot_text_a(fb, &st, UI_CX, 380, t, 1);
    }
}

/* ------------------------------------------------------------- draw / input */
static void compass_draw(gfx_fb *fb)
{
    switch (C.mode) {
    case M_ABSENT:
        ui_title(fb, "COMPASS", 0);
        ui_absent(fb, DOT_ICON_COMPASS, "Not connected", "Add the compass to see your heading",
                  "COMPASS MODULE", C.build);
        break;
    case M_CAL: cal_draw(fb); break;
    case M_CAL_DONE: cal_done_draw(fb); break;
    case M_DIAL:
        dial_draw(fb);
        lubber_draw(fb);
        readout_draw(fb);
        break;
    case M_LEVEL: level_draw(fb); break;
    }
}

static int in_cal_button(int x, int y)
{
    return C.details && abs(x - UI_CX) < CAL_W / 2 + 4 && abs(y - CAL_Y) < UI_BTN_H / 2;
}

static int compass_input(const nos_input *in)
{
    switch (in->type) {
    case IN_DOWN:
        C.pressed = C.mode == M_DIAL && in_cal_button(in->x, in->y);
        return 1;
    case IN_UP:
        C.pressed = 0;
        return 1;
    case IN_TAP:
        if (C.mode == M_DIAL) {
            if (in_cal_button(in->x, in->y)) {
                nos_feedback(FB_TAP);
                start_cal(1);
            } else {
                C.details = !C.details;
                nos_feedback(FB_TAP);
            }
            return 1;
        }
        return 0;
    case IN_LONG:
        if (C.mode == M_DIAL) { set_mode(M_LEVEL); return 1; }
        if (C.mode == M_LEVEL) { set_mode(M_DIAL); return 1; }
        return 0;
    default:
        return 0;
    }
}

const nos_screen scr_compass = { "COMPASS", compass_enter, compass_leave, compass_tick, compass_draw,
                                 compass_input, SCR_KEEPAWAKE };
