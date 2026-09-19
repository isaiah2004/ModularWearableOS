/* app_sensors.c — live Sensors dashboard: a maker/debug instrument panel that
 * shows the watch's real sensors (IMU, battery, steps, uptime) as numbers,
 * signed bar meters, and a scrolling plot.
 *
 * PLAIN-ENGLISH ALGORITHM
 * -----------------------
 * Two pages, toggled by TAP (or SWIPE_UP -> SYSTEM, SWIPE_DOWN -> IMU):
 *
 * PAGE 1 "IMU":
 *   1. READ each frame: hal_imu_read() gives accelerometer (ax,ay,az in g)
 *      and gyroscope (gx,gy,gz in deg/s).
 *   2. NUMBERS: print each of the 6 axes as an aligned label + value.
 *   3. SIGNED BAR METERS: each axis also gets a small horizontal bar centered
 *      at zero. We map the reading to a fraction in [-1,+1] via bar_frac():
 *          frac = clamp(value / full_scale, -1, +1)
 *      Accel full scale = 2 g, gyro full scale = 250 deg/s. The bar fills from
 *      the center line rightward for positive, leftward for negative, so you
 *      can watch it swing as the watch moves.
 *   4. SCROLLING PLOT: we keep a small static ring buffer of the most recent
 *      accel-magnitude samples ( |a| = sqrt(ax^2+ay^2+az^2) ). Each frame we
 *      push one sample; we draw the buffer oldest->newest as vertical bars,
 *      so the trace scrolls left and the height also demonstrates frame rate.
 *
 * PAGE 2 "SYSTEM":
 *   - Battery %: hal_battery_percent(), shown as a ring arc + number.
 *   - Steps:     hal_step_count().
 *   - Uptime:    hal_millis() formatted H:MM:SS by format_uptime().
 *
 * The two pure helpers (bar_frac mapping, format_uptime) are covered by a
 * standalone self-test under SELFTEST.
 */
#include <math.h>
#include <stdio.h>

#ifndef SELFTEST
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include "../hal.h"
#endif

#include <stdint.h>
#include <stddef.h>

/* Map a signed reading to a bar fraction in [-1, +1] about zero.
 * Pure math, shared by the app and the self-test. */
static float bar_frac(float value, float full_scale)
{
    if (full_scale <= 0.0f) return 0.0f;
    float f = value / full_scale;
    if (f >  1.0f) f =  1.0f;
    if (f < -1.0f) f = -1.0f;
    return f;
}

/* Format milliseconds-since-boot as "H:MM:SS" into buf. */
static void format_uptime(uint32_t ms, char *buf, size_t n)
{
    uint32_t total = ms / 1000u;
    unsigned h = (unsigned)(total / 3600u);
    unsigned m = (unsigned)((total / 60u) % 60u);
    unsigned s = (unsigned)(total % 60u);
    snprintf(buf, n, "%u:%02u:%02u", h, m, s);
}

#ifndef SELFTEST

#define ACCEL_FS   2.0f      /* accel bar full scale, g      */
#define GYRO_FS  250.0f      /* gyro  bar full scale, deg/s  */
#define PLOTN     64         /* scrolling-plot ring-buffer length */
#define PLOT_FS    3.0f      /* accel magnitude full scale for the plot, g */

/* diagnostics pages — swipe left/right to move; IMU last (no IMU chip wired yet) */
enum { PG_SYSTEM = 0, PG_TESTS, PG_LOG, PG_IMU, PG_TOUCH, PG_N };
#define CLR_CX (GFX_CX + GFX_S(78))     /* clear-logs button (LOG page) */
#define CLR_CY (GFX_CY - GFX_S(94))
#define CLR_R  GFX_S(12)
#define LOG_SHOW 10

/* --- static state (no malloc) --- */
static int    s_page;
void app_diagnostics_imu_test(void) { s_page=PG_IMU; }
static int s_touch_x, s_touch_y, s_touch_down, s_touch_seen, s_touch_taps;
void app_diagnostics_touch_test(void)
{
    s_page = PG_TOUCH;
    s_touch_seen = s_touch_down = s_touch_taps = 0;
}
static float  s_plot[PLOTN];          /* ring buffer of accel magnitudes */
static int    s_head;                 /* next write index (oldest sample) */
static int    s_filled;               /* samples written, capped at PLOTN */
static uint32_t s_beep_ms;            /* flash the beep button briefly after a tap */
static int    s_log_scroll;           /* LOG page: lines scrolled back from newest */

static void on_start(void)
{
    s_page = 0;
    s_head = 0;
    s_filled = 0;
    for (int i = 0; i < PLOTN; i++) s_plot[i] = 0.0f;
}

static int on_event(const wos_event *ev)
{
    if (s_page == PG_TOUCH) {
        if (ev->type == WOS_EV_TOUCH_DOWN || ev->type == WOS_EV_TOUCH_MOVE || ev->type == WOS_EV_TOUCH_UP) {
            s_touch_x = ev->x; s_touch_y = ev->y;
            s_touch_down = ev->type != WOS_EV_TOUCH_UP;
            s_touch_seen = 1;
        }
        if (ev->type == WOS_EV_TAP) s_touch_taps++;
        return 1; /* Dragging tests coordinates; BOOT/home exits this page. */
    }
    switch (ev->type) {
    case WOS_EV_SWIPE_LEFT:  s_page = (s_page + 1) % PG_N;  s_log_scroll = 0;  return 1;
    case WOS_EV_SWIPE_RIGHT: s_page = (s_page + PG_N - 1) % PG_N; s_log_scroll = 0; return 1;
    case WOS_EV_SWIPE_DOWN:  if (s_page == PG_LOG) { s_log_scroll++; return 1; } return 0;  /* older */
    case WOS_EV_SWIPE_UP:    if (s_page == PG_LOG) { if (s_log_scroll > 0) s_log_scroll--; return 1; } return 0;
    case WOS_EV_TAP:
        if (s_page == PG_TESTS) { hal_play(SND_COIN); s_beep_ms = hal_millis(); return 1; }  /* speaker test */
        if (s_page == PG_LOG) {
            float dx = ev->x - CLR_CX, dy = ev->y - CLR_CY;
            if (dx * dx + dy * dy <= (CLR_R + GFX_S(6)) * (CLR_R + GFX_S(6))) {
                hal_log_clear(); s_log_scroll = 0;                     /* clear-logs button */
            }
        }
        return 1;
    default:
        return 0;
    }
}

/* One signed bar meter: dim track, a zero tick, and a fill that grows from the
 * center line (right for +, left for -). cx,cy,hw are device pixels. */
static void draw_bar(gfx_fb *fb, float cx, float cy, float hw,
                     float frac, uint16_t col)
{
    float th = GFX_S(6);                 /* bar thickness */
    int   y  = (int)(cy - th * 0.5f);
    int   ih = (int)th;
    /* track */
    gfx_fill_rect(fb, (int)(cx - hw), y, (int)(2.0f * hw), ih, GFX_DGREY);
    /* signed fill */
    float w = frac * hw;
    if (w >= 0.0f) gfx_fill_rect(fb, (int)cx,       y, (int)w,  ih, col);
    else           gfx_fill_rect(fb, (int)(cx + w), y, (int)(-w), ih, col);
    /* zero tick on top */
    gfx_fill_rect(fb, (int)(cx - GFX_S(0.5f)), y - 1, (int)GFX_S(1), ih + 2, GFX_GREY);
}

/* Right-align text so a column of numbers lines up. right_x = device px. */
static void draw_value(gfx_fb *fb, int right_x, int y,
                       const char *s, uint16_t col)
{
    int w = font_text_width(&font_small, s);
    font_draw(fb, &font_small, right_x - w, y, s, col);
}

/* One IMU row: label, signed bar, numeric value. dy = design offset from
 * screen center; val is the reading; fs its bar full scale. */
static void draw_imu_row(gfx_fb *fb, float dy, const char *label,
                         float val, float fs, const char *fmt, uint16_t col)
{
    float cy = GFX_CY + GFX_S(dy);
    font_draw(fb, &font_small, (int)(GFX_CX + GFX_S(-72)),
              (int)(cy + GFX_S(4)), label, GFX_GREY);
    draw_bar(fb, GFX_CX + GFX_S(2), cy, GFX_S(30), bar_frac(val, fs), col);
    char b[16];
    snprintf(b, sizeof b, fmt, (double)val);
    draw_value(fb, (int)(GFX_CX + GFX_S(78)), (int)(cy + GFX_S(4)), b, GFX_WHITE);
}

static void draw_imu_page(gfx_fb *fb, const hal_imu *m)
{
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(-96)),
                       "IMU", GFX_ORANGE);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(-82)),
                       hal_imu_read(&(hal_imu){0}) ? "QMI8658 live" : "IMU unavailable", GFX_DGREY);

    font_draw(fb, &font_small, (int)(GFX_CX + GFX_S(-72)),
              (int)(GFX_CY + GFX_S(-70)), "ACCEL g + GRAVITY", GFX_GREEN);
    draw_imu_row(fb, -54, "aX", m->ax, ACCEL_FS, "%+5.2f", GFX_GREEN);
    draw_imu_row(fb, -38, "aY", m->ay, ACCEL_FS, "%+5.2f", GFX_GREEN);
    draw_imu_row(fb, -22, "aZ", m->az, ACCEL_FS, "%+5.2f", GFX_GREEN);

    font_draw(fb, &font_small, (int)(GFX_CX + GFX_S(-72)),
              (int)(GFX_CY + GFX_S(-2)), "GYRO \xB0/s", GFX_ACCENT);
    draw_imu_row(fb,  14, "gX", m->gx, GYRO_FS, "%+6.1f", GFX_ACCENT);
    draw_imu_row(fb,  30, "gY", m->gy, GYRO_FS, "%+6.1f", GFX_ACCENT);
    draw_imu_row(fb,  46, "gZ", m->gz, GYRO_FS, "%+6.1f", GFX_ACCENT);

    /* scrolling accel-magnitude plot */
    float px0 = GFX_CX + GFX_S(-52);
    float py1 = GFX_CY + GFX_S(78);      /* baseline */
    float ph  = GFX_S(22);               /* max height */
    float pw  = GFX_S(104);
    float bw  = pw / PLOTN;
    gfx_fill_rect(fb, (int)px0, (int)(py1 - ph), (int)pw, (int)ph, GFX_DGREY);
    int n = s_filled;
    for (int i = 0; i < n; i++) {
        /* oldest first: ring index starting at s_head (== oldest when full) */
        int idx = (s_head + i) % PLOTN;
        float f = s_plot[idx] / PLOT_FS;
        if (f > 1.0f) f = 1.0f;
        float h = f * ph;
        uint16_t c = f > 0.66f ? GFX_RED : f > 0.4f ? GFX_ORANGE : GFX_GREEN;
        gfx_fill_rect(fb, (int)(px0 + i * bw), (int)(py1 - h),
                      (int)(bw + 1.0f), (int)h, c);
    }
    char magnitude[36];snprintf(magnitude,sizeof magnitude,"|a| %.2fg  REST ~1g",
        (double)sqrtf(m->ax*m->ax+m->ay*m->ay+m->az*m->az));
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(90)),magnitude,GFX_GREY);
}

static void draw_system_page(gfx_fb *fb)
{
    char b[24];

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(-96)),
                       "SYSTEM", GFX_ORANGE);

    /* battery ring (left) */
    int bat = hal_battery_percent();
    if (bat < 0) bat = 0;
    if (bat > 100) bat = 100;
    float bcx = GFX_CX + GFX_S(-52), bcy = GFX_CY + GFX_S(-30);
    float br = GFX_S(30);
    uint16_t bcol = bat <= 20 ? GFX_RED : bat <= 50 ? GFX_ORANGE : GFX_GREEN;
    gfx_ring(fb, bcx, bcy, br, GFX_S(5), GFX_DGREY);
    if (bat > 0)
        gfx_arc(fb, bcx, bcy, br, GFX_S(5), 0.0f,
                (float)bat / 100.0f * 6.2831853f, bcol);
    snprintf(b, sizeof b, "%d%%", bat);
    font_draw_centered(fb, &font_medium, (int)bcx, (int)(bcy + GFX_S(5)), b, GFX_WHITE);
    font_draw_centered(fb, &font_small, (int)bcx, (int)(bcy + GFX_S(48)),
                       "BATT", GFX_GREY);

    /* steps (right) */
    float scx = GFX_CX + GFX_S(52), scy = GFX_CY + GFX_S(-30);
    snprintf(b, sizeof b, "%d", hal_step_count());
    font_draw_centered(fb, &font_medium, (int)scx, (int)(scy + GFX_S(0)), b, GFX_ACCENT);
    font_draw_centered(fb, &font_small, (int)scx, (int)(scy + GFX_S(48)),
                       "STEPS", GFX_GREY);

    /* uptime (bottom, wide) */
    format_uptime(hal_millis(), b, sizeof b);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(50)),
                       "UPTIME", GFX_GREY);
    font_draw_centered(fb, &font_large, (int)GFX_CX, (int)(GFX_CY + GFX_S(78)),
                       b, GFX_WHITE);
}

/* one status row: coloured dot + label + short verdict */
static void draw_status_row(gfx_fb *fb, float dy, const char *label, hal_conn st)
{
    float cy = GFX_CY + GFX_S(dy);
    uint16_t dot = st == HAL_CONN_UP ? GFX_GREEN : st == HAL_CONN_TRYING ? GFX_ACCENT
                 : st == HAL_CONN_DOWN ? GFX_RED : GFX_DGREY;
    const char *sw = st == HAL_CONN_UP ? "OK" : st == HAL_CONN_TRYING ? "..."
                   : st == HAL_CONN_DOWN ? "DOWN" : "OFF";
    gfx_fill_circle(fb, GFX_CX + GFX_S(-70), cy, GFX_S(3.5f), dot);
    font_draw(fb, &font_small, (int)(GFX_CX + GFX_S(-58)), (int)(cy + GFX_S(4)), label, GFX_WHITE);
    draw_value(fb, (int)(GFX_CX + GFX_S(74)), (int)(cy + GFX_S(4)), sw, GFX_GREY);
}

static void draw_tests_page(gfx_fb *fb)
{
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(-96)), "TESTS", GFX_ORANGE);
    draw_status_row(fb, -70, "WiFi",      hal_status(HAL_FEAT_WIFI));
    draw_status_row(fb, -50, "Bluetooth", hal_status(HAL_FEAT_BLE));
    draw_status_row(fb, -30, "Internet",  hal_status(HAL_FEAT_NET));
    draw_status_row(fb, -10, "Time sync", hal_status(HAL_FEAT_SYNC));
    hal_imu imu; hal_storage sd;
    hal_storage_get(&sd);
    draw_status_row(fb,  10, "IMU", hal_imu_read(&imu) ? HAL_CONN_UP : HAL_CONN_DOWN);
    draw_status_row(fb,  30, "SD card", sd.mounted ? HAL_CONN_UP : HAL_CONN_DOWN);
    float by = GFX_CY + GFX_S(70);                        /* speaker beep test */
    int lit = (hal_millis() - s_beep_ms) < 220u;
    gfx_fill_circle(fb, (int)GFX_CX, (int)by, (int)GFX_S(17), lit ? GFX_GREEN : GFX_PURPLE);
    icon_glyph(fb, ICON_WAVEFORM, GFX_CX, by, GFX_S(10), GFX_WHITE);
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(by + GFX_S(30)), hal_audio_available() ? "TAP = BEEP" : "NO ONBOARD AUDIO", GFX_GREY);
}

static void draw_log_page(gfx_fb *fb)
{
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(-96)), "LOG", GFX_ORANGE);
    /* clear-logs button (trash) top-right */
    gfx_fill_circle(fb, (int)CLR_CX, (int)CLR_CY, (int)CLR_R, GFX_RGB(72, 40, 40));
    icon_glyph(fb, ICON_TRASH, CLR_CX, CLR_CY, GFX_S(8), GFX_RGB(240, 130, 130));

    int n = hal_log_count();
    if (n == 0) {
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_CY, "(no events yet)", GFX_DGREY);
        return;
    }
    int maxsc = n > LOG_SHOW ? n - LOG_SHOW : 0;        /* swipe up/down scrolls history */
    if (s_log_scroll > maxsc) s_log_scroll = maxsc;
    int start = n - LOG_SHOW - s_log_scroll; if (start < 0) start = 0;
    int end = start + LOG_SHOW; if (end > n) end = n;
    float y = GFX_CY + GFX_S(-72);
    for (int i = start; i < end; i++) {
        font_draw(fb, &font_small, (int)(GFX_CX + GFX_S(-98)), (int)y, hal_log_get(i), GFX_GREY);
        y += GFX_S(15);
    }
    if (maxsc > 0) {                                    /* position indicator */
        char sc[32]; snprintf(sc, sizeof sc, "%d-%d / %d", start + 1, end, n);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_CY + GFX_S(86)), sc, GFX_DGREY);
    }
}

static void draw_touch_page(gfx_fb *fb)
{
    char label[48];
    font_draw_centered(fb, &font_small, GFX_CX, GFX_S(35), "TOUCH TEST", GFX_ORANGE);
    font_draw_centered(fb, &font_small, GFX_CX, GFX_S(53), "Touch the five targets", GFX_GREY);
    const int target[5][2] = {{120,120},{60,120},{180,120},{120,80},{120,170}};
    for (int i = 0; i < 5; i++) {
        float x = GFX_S(target[i][0]), y = GFX_S(target[i][1]);
        gfx_ring(fb, x, y, GFX_S(9), GFX_S(1), GFX_DGREY);
        gfx_line(fb, x-GFX_S(5), y, x+GFX_S(5), y, GFX_S(1), GFX_GREY);
        gfx_line(fb, x, y-GFX_S(5), x, y+GFX_S(5), GFX_S(1), GFX_GREY);
    }
    if (s_touch_seen) {
        uint16_t color = s_touch_down ? GFX_GREEN : GFX_ORANGE;
        gfx_ring(fb, s_touch_x, s_touch_y, GFX_S(12), GFX_S(2), color);
        gfx_line(fb, s_touch_x-GFX_S(16), s_touch_y, s_touch_x+GFX_S(16), s_touch_y, GFX_S(1), color);
        gfx_line(fb, s_touch_x, s_touch_y-GFX_S(16), s_touch_x, s_touch_y+GFX_S(16), GFX_S(1), color);
    }
    snprintf(label, sizeof label, "%s X:%d Y:%d", s_touch_down ? "DOWN" : "UP", s_touch_x, s_touch_y);
    font_draw_centered(fb, &font_small, GFX_CX, GFX_S(194), label, GFX_WHITE);
    snprintf(label, sizeof label, "Taps: %d", s_touch_taps);
    font_draw_centered(fb, &font_small, GFX_CX, GFX_S(211), label, GFX_GREY);
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);

    /* read the (absent) IMU every frame and feed the scrolling plot */
    hal_imu m = { 0, 0, 1, 0, 0, 0 };
    hal_imu_read(&m);
    float mag = sqrtf(m.ax * m.ax + m.ay * m.ay + m.az * m.az);
    s_plot[s_head] = mag;
    s_head = (s_head + 1) % PLOTN;
    if (s_filled < PLOTN) s_filled++;

    switch (s_page) {
    case PG_SYSTEM: draw_system_page(fb); break;
    case PG_TESTS:  draw_tests_page(fb);  break;
    case PG_LOG:    draw_log_page(fb);    break;
    case PG_TOUCH:  draw_touch_page(fb);  break;
    default:        draw_imu_page(fb, &m); break;
    }

    /* page dots */
    for (int i = 0; i < PG_N; i++)
        gfx_fill_circle(fb, GFX_CX + GFX_S(((float)i - (PG_N - 1) / 2.0f) * 8.0f),
                        GFX_CY + GFX_S(104), GFX_S(2.2f),
                        i == s_page ? GFX_WHITE : GFX_DGREY);
}

const wos_app app_sensors = { "Diagnostics", on_start, on_event, on_draw,
                              GFX_ORANGE, ICON_GAUGE };

#endif /* !SELFTEST */

#ifdef SELFTEST
#include <assert.h>
#include <string.h>
int main(void)
{
    const float EPS = 1e-4f;

    /* bar_frac: zero-centered, clamped to [-1,1] */
    assert(fabsf(bar_frac(0.0f, 2.0f) - 0.0f)  < EPS);
    assert(fabsf(bar_frac(1.0f, 2.0f) - 0.5f)  < EPS);
    assert(fabsf(bar_frac(2.0f, 2.0f) - 1.0f)  < EPS);
    assert(fabsf(bar_frac(4.0f, 2.0f) - 1.0f)  < EPS);   /* clamp + */
    assert(fabsf(bar_frac(-4.0f, 2.0f) + 1.0f) < EPS);   /* clamp - */
    assert(fabsf(bar_frac(-125.0f, 250.0f) + 0.5f) < EPS);
    assert(fabsf(bar_frac(5.0f, 0.0f) - 0.0f)  < EPS);   /* guard fs<=0 */

    /* format_uptime: H:MM:SS */
    char b[24];
    format_uptime(0u, b, sizeof b);            assert(strcmp(b, "0:00:00") == 0);
    format_uptime(1000u, b, sizeof b);         assert(strcmp(b, "0:00:01") == 0);
    format_uptime(59000u, b, sizeof b);        assert(strcmp(b, "0:00:59") == 0);
    format_uptime(60000u, b, sizeof b);        assert(strcmp(b, "0:01:00") == 0);
    format_uptime(3599000u, b, sizeof b);      assert(strcmp(b, "0:59:59") == 0);
    format_uptime(3600000u, b, sizeof b);      assert(strcmp(b, "1:00:00") == 0);
    format_uptime(45296000u, b, sizeof b);     assert(strcmp(b, "12:34:56") == 0);

    printf("SELFTEST OK\n");
    return 0;
}
#endif
