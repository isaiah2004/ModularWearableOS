/* app_alarm.c — Alarm list.
 *
 * PLAIN-ENGLISH ALGORITHM
 * -----------------------
 * STATE: a fixed array of 3 alarms, each {hour, minute, enabled}. No malloc,
 * no growth — the array IS the model. TAP on a row flips that alarm's
 * `enabled` flag in place; nothing else is stored.
 *
 * "MINUTES UNTIL NEXT" (mins_until):
 *   Convert both the current time and the alarm time to minutes-past-midnight
 *   (h*60 + m). Subtract: raw = alarmMinutes - nowMinutes.
 *   If raw is negative the alarm already passed today, so it fires tomorrow —
 *   add 1440 (a full day) to wrap past midnight. raw == 0 means "right now".
 *   Result is always in [0, 1439].
 *     e.g. now 06:00, alarm 06:30 -> 30
 *          now 23:00, alarm 06:30 -> (390-1380) = -990, +1440 = 450
 *
 * "NEXT ENABLED ALARM" (next_alarm_index):
 *   Walk every enabled alarm, compute its mins_until, keep the smallest.
 *   The soonest future enabled alarm wins (ties keep the earliest index).
 *   Returns -1 when nothing is enabled.
 *
 * DRAW: header shows the next enabled alarm's time (font_big) with a bell
 * accent and an "in Xh Ym" countdown. Below, one row per alarm: the time in
 * font_large plus an on/off toggle pill mirroring app_settings.c. The next
 * alarm's row gets a small accent dot.
 */
#include <stdio.h>

typedef struct { int hour, minute, enabled; } alarm_t;

/* pure: minutes from now until this alarm, wrapping past midnight. */
static int mins_until(int now_h, int now_m, int a_h, int a_m)
{
    int raw = (a_h * 60 + a_m) - (now_h * 60 + now_m);
    if (raw < 0) raw += 1440;
    return raw;
}

/* pure: index of the soonest enabled alarm, or -1 if none are enabled. */
static int next_alarm_index(const alarm_t *al, int n, int now_h, int now_m)
{
    int best = -1, best_m = 0;
    for (int i = 0; i < n; i++) {
        if (!al[i].enabled) continue;
        int m = mins_until(now_h, now_m, al[i].hour, al[i].minute);
        if (best < 0 || m < best_m) { best = i; best_m = m; }
    }
    return best;
}

#ifdef SELFTEST
/* ---- standalone logic self-test (compile with -DSELFTEST and run) ---- */
#include <assert.h>

int main(void)
{
    /* mins_until: basic, exact-now, and midnight wrap */
    assert(mins_until(6, 0, 6, 30) == 30);      /* 06:00 -> 06:30  */
    assert(mins_until(22, 0, 22, 0) == 0);      /* alarm == now    */
    assert(mins_until(23, 0, 6, 30) == 450);    /* wrap past 00:00 */
    assert(mins_until(7, 0, 6, 30) == 1410);    /* just missed it  */

    alarm_t seed[3] = { {6, 30, 1}, {7, 0, 0}, {22, 0, 1} };

    /* at 06:00 the soonest enabled alarm is 06:30 (index 0), 30 min out */
    int i = next_alarm_index(seed, 3, 6, 0);
    assert(i == 0);
    assert(mins_until(6, 0, seed[i].hour, seed[i].minute) == 30);

    /* at 23:00, 07:00 is disabled so 06:30 (index 0) wins, 450 min out */
    i = next_alarm_index(seed, 3, 23, 0);
    assert(i == 0);
    assert(mins_until(23, 0, seed[i].hour, seed[i].minute) == 450);

    /* at 06:45 (past 06:30), the next enabled is 22:00 (index 2) */
    i = next_alarm_index(seed, 3, 6, 45);
    assert(i == 2);
    assert(mins_until(6, 45, seed[i].hour, seed[i].minute) == 915);

    /* nothing enabled -> -1 */
    alarm_t off[2] = { {8, 0, 0}, {9, 0, 0} };
    assert(next_alarm_index(off, 2, 12, 0) == -1);

    printf("app_alarm self-test: all asserts passed\n");
    return 0;
}

#else
/* -------------------------- watch app -------------------------- */
#include "apps.h"
#include "../font.h"
#include "../icons.h"
#include <math.h>

/* Seeded alarms; edits persist to NVS ("alarms"). Six slots -> the list scrolls. */
#define NALARMS 6
static alarm_t s_alarms[NALARMS] = {
    {6, 30, 1}, {7, 0, 0}, {8, 15, 0}, {12, 30, 0}, {18, 0, 0}, {22, 0, 1},
};
static int s_loaded = 0;
static void alarms_load(void)
{
    if (s_loaded) return;
    s_loaded = 1;
    hal_store_load("alarms", s_alarms, sizeof s_alarms);   /* keeps seed if none saved */
}
static void alarms_save(void) { hal_store_save("alarms", s_alarms, sizeof s_alarms); }

/* ---- background firing service (runs in ANY app; see apps.h) ---------------- */
static int s_fired_key = -1;         /* h*60+m already rung this minute (fire once) */

void alarm_service_tick(void)
{
    static uint32_t last = 0;
    uint32_t now = hal_millis();
    alarms_load();

    hal_datetime t;
    hal_get_datetime(&t);

    /* seconds until the soonest enabled alarm (wraps past midnight) */
    int soonest = -1, nowsec = t.hour * 3600 + t.minute * 60 + t.second;
    for (int i = 0; i < NALARMS; i++) {
        if (!s_alarms[i].enabled) continue;
        int s = (s_alarms[i].hour * 3600 + s_alarms[i].minute * 60) - nowsec;
        if (s < 0) s += 86400;
        if (soonest < 0 || s < soonest) soonest = s;
    }

    /* Cheap when far off (check ~1/min), precise when close (check ~1/s in the
     * last few seconds) so we ring within a second of the exact minute. */
    uint32_t interval = (soonest >= 0 && soonest <= 6) ? 900 : 59000;
    if (last && now - last < interval) return;
    last = now;

    int key = t.hour * 60 + t.minute;
    if (key == s_fired_key) return;                 /* already rung this minute */
    for (int i = 0; i < NALARMS; i++) {
        if (s_alarms[i].enabled &&
            s_alarms[i].hour == t.hour && s_alarms[i].minute == t.minute) {
            s_fired_key = key;
            hal_play(SND_ALARM);
            face_pip_alarm();
            char msg[24];
            snprintf(msg, sizeof msg, "ALARM  %02d:%02d", t.hour, t.minute);
            wos_callout(msg, 4500);
            break;
        }
    }
}

/* ---- view state ------------------------------------------------------------ */
static int   s_edit = -1;            /* -1 = list; else index being edited */
static alarm_t s_edit_orig;          /* snapshot for cancel */
static float s_scroll = 0, s_scroll_to = 0;   /* list scroll, in row units */
static float s_ah = 0, s_am = 0;     /* drum roll offset (slots), eases to 0 */
static float s_enter = 0;            /* edit-panel slide-in 0..1 */

/* list geometry (240 grid) */
#define ROW_PITCH GFX_S(48)
#define ROW_TOP   GFX_S(98)          /* center of the first slot */
#define LIST_VIS  3                  /* rows fully visible before scrolling */
#define TOGGLE_X  (GFX_W - GFX_S(88))/* taps right of here hit the on/off pill */

static void al_start(void)
{
    alarms_load();
    s_edit = -1; s_scroll = s_scroll_to = 0; s_ah = s_am = 0; s_enter = 0;
}

static float clampf(float v, float lo, float hi)
{ return v < lo ? lo : v > hi ? hi : v; }

/* frame-rate-independent ease toward a target */
static float approach(float cur, float tgt, int dt_ms)
{
    float k = clampf(dt_ms / 90.0f, 0.0f, 1.0f);
    return cur + (tgt - cur) * k;
}

static void draw_num_c(gfx_fb *fb, const font_t *f, float cx, float cy,
                       int val, uint16_t col)
{
    char b[4];
    snprintf(b, sizeof b, "%02d", val);
    font_draw_centered(fb, f, (int)cx, (int)(cy + f->ascent * 0.34f), b, col);
}

/* ---- list view ------------------------------------------------------------- */
static void draw_row(gfx_fb *fb, float y, const alarm_t *a, int is_next)
{
    if (is_next)
        gfx_fill_circle(fb, GFX_S(24), y, GFX_S(5), GFX_ACCENT);

    char tb[8];
    snprintf(tb, sizeof tb, "%02d:%02d", a->hour, a->minute);
    uint16_t tc = a->enabled ? GFX_WHITE : GFX_GREY;
    font_draw(fb, &font_medium, (int)GFX_S(42),
              (int)(y + font_medium.ascent * 0.34f), tb, tc);

    /* on/off toggle pill (mirrors app_settings) */
    float kw = GFX_S(42), kh = GFX_S(24);
    float kx = GFX_W - GFX_S(34) - kw, ky = y - kh / 2;
    gfx_fill_rect(fb, (int)kx, (int)ky, (int)kw, (int)kh,
                  a->enabled ? GFX_GREEN : GFX_DGREY);
    float knob = kh / 2;
    gfx_fill_circle(fb, a->enabled ? kx + kw - knob : kx + knob, y,
                    knob - GFX_S(2), GFX_WHITE);
}

static void draw_list(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    hal_datetime t;
    hal_get_datetime(&t);
    int ni = next_alarm_index(s_alarms, NALARMS, t.hour, t.minute);

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(30),
                       "ALARMS", GFX_GREY);
    if (ni >= 0) {
        int m = mins_until(t.hour, t.minute, s_alarms[ni].hour, s_alarms[ni].minute);
        char sub[28];
        if (m == 0)            snprintf(sub, sizeof sub, "next in 0m");
        else if (m >= 60)      snprintf(sub, sizeof sub, "next in %dh %02dm", m / 60, m % 60);
        else                   snprintf(sub, sizeof sub, "next in %dm", m);
        font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(54), sub, GFX_ACCENT);
    }

    float top = GFX_S(70), bot = GFX_H - GFX_S(26);
    for (int i = 0; i < NALARMS; i++) {
        float y = ROW_TOP + (i - s_scroll) * ROW_PITCH;
        if (y < top || y > bot) continue;       /* clip to viewport */
        draw_row(fb, y, &s_alarms[i], i == ni);
    }

    /* scrollbar */
    int max_scroll = NALARMS - LIST_VIS;
    if (max_scroll > 0) {
        float tx = GFX_W - GFX_S(9), t0 = GFX_S(74), t1 = GFX_H - GFX_S(28);
        gfx_fill_rect(fb, (int)tx, (int)t0, (int)GFX_S(3), (int)(t1 - t0), GFX_DGREY);
        float th = (t1 - t0) * (float)LIST_VIS / NALARMS;
        float ty = t0 + (t1 - t0 - th) * clampf(s_scroll / max_scroll, 0, 1);
        gfx_fill_rect(fb, (int)tx, (int)ty, (int)GFX_S(3), (int)th, GFX_GREY);
    }
}

/* ---- edit view: two roll-drums (HH : MM) ----------------------------------- */
#define DRUM_CY  GFX_S(126)
#define DRUM_SLOT GFX_S(40)
#define HH_CX   (GFX_CX - GFX_S(42))
#define MM_CX   (GFX_CX + GFX_S(42))
#define BTN_CY  GFX_S(196)
#define BTN_R   GFX_S(19)
#define OK_CX   (GFX_CX + GFX_S(44))
#define NO_CX   (GFX_CX - GFX_S(44))

static void draw_drum(gfx_fb *fb, float cx, int val, int mod, float roll, float yoff)
{
    /* roll in slots: 0 settled; the strip slides so val stays centered */
    for (int d = -1; d <= 1; d++) {
        int v = ((val + d) % mod + mod) % mod;
        float y = DRUM_CY + (d + roll) * DRUM_SLOT + yoff;
        if (d == 0) draw_num_c(fb, &font_big, cx, y, v, GFX_WHITE);
        else        draw_num_c(fb, &font_medium, cx, y, v, GFX_DGREY);
    }
}

static void draw_edit(gfx_fb *fb)
{
    gfx_clear(fb, GFX_BLACK);
    float yoff = (1.0f - s_enter) * GFX_S(26);   /* slide-in */

    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)(GFX_S(52) + yoff),
                       "SET ALARM", GFX_GREY);

    /* faint up/down hint chevrons */
    for (int col = 0; col < 2; col++) {
        float cx = col ? MM_CX : HH_CX;
        float uy = DRUM_CY - GFX_S(52) + yoff, dy = DRUM_CY + GFX_S(52) + yoff;
        gfx_line(fb, cx - GFX_S(8), uy + GFX_S(5), cx, uy, GFX_S(2), GFX_DGREY);
        gfx_line(fb, cx + GFX_S(8), uy + GFX_S(5), cx, uy, GFX_S(2), GFX_DGREY);
        gfx_line(fb, cx - GFX_S(8), dy - GFX_S(5), cx, dy, GFX_S(2), GFX_DGREY);
        gfx_line(fb, cx + GFX_S(8), dy - GFX_S(5), cx, dy, GFX_S(2), GFX_DGREY);
    }

    draw_drum(fb, HH_CX, s_alarms[s_edit].hour, 24, s_ah, yoff);
    /* colon between the drums (two dots) */
    gfx_fill_circle(fb, GFX_CX, DRUM_CY - GFX_S(8) + yoff, GFX_S(3), GFX_WHITE);
    gfx_fill_circle(fb, GFX_CX, DRUM_CY + GFX_S(8) + yoff, GFX_S(3), GFX_WHITE);
    draw_drum(fb, MM_CX, s_alarms[s_edit].minute, 60, s_am, yoff);

    /* cancel (grey X) + confirm (green check) */
    float by = BTN_CY + yoff;
    gfx_fill_circle(fb, NO_CX, by, BTN_R, GFX_DGREY);
    gfx_line(fb, NO_CX - GFX_S(6), by - GFX_S(6), NO_CX + GFX_S(6), by + GFX_S(6), GFX_S(3), GFX_WHITE);
    gfx_line(fb, NO_CX - GFX_S(6), by + GFX_S(6), NO_CX + GFX_S(6), by - GFX_S(6), GFX_S(3), GFX_WHITE);
    gfx_fill_circle(fb, OK_CX, by, BTN_R, GFX_GREEN);
    gfx_line(fb, OK_CX - GFX_S(7), by, OK_CX - GFX_S(1), by + GFX_S(6), GFX_S(3), GFX_WHITE);
    gfx_line(fb, OK_CX - GFX_S(1), by + GFX_S(6), OK_CX + GFX_S(8), by - GFX_S(6), GFX_S(3), GFX_WHITE);
}

static void on_draw(gfx_fb *fb)
{
    alarms_load();
    if (s_edit >= 0) draw_edit(fb);
    else             draw_list(fb);
}

/* ---- events ---------------------------------------------------------------- */
static void bump(int col, int delta)          /* col 0=hour 1=min */
{
    if (col == 0) {
        s_alarms[s_edit].hour = (s_alarms[s_edit].hour + delta + 24) % 24;
        s_ah += (float)delta;                  /* kick the roll, eases back to 0 */
    } else {
        s_alarms[s_edit].minute = (s_alarms[s_edit].minute + delta + 60) % 60;
        s_am += (float)delta;
    }
    hal_haptic(6);
}

static int on_event(const wos_event *ev)
{
    if (ev->type == WOS_EV_TICK) {
        int dt = ev->x;
        s_scroll = approach(s_scroll, s_scroll_to, dt);
        s_ah = approach(s_ah, 0, dt);
        s_am = approach(s_am, 0, dt);
        if (s_edit >= 0) s_enter = approach(s_enter, 1, dt);
        return 1;
    }

    if (s_edit >= 0) {                          /* -------- edit view -------- */
        int col = ev->x < (int)GFX_CX ? 0 : 1;
        switch (ev->type) {
        case WOS_EV_SWIPE_UP:   bump(col, +1); return 1;   /* wheel rolls forward */
        case WOS_EV_SWIPE_DOWN: bump(col, -1); return 1;
        case WOS_EV_TAP: {
            float dxo = ev->x - OK_CX, dyo = ev->y - BTN_CY;
            float dxn = ev->x - NO_CX, dyn = ev->y - BTN_CY;
            if (dxo * dxo + dyo * dyo <= (BTN_R + GFX_S(6)) * (BTN_R + GFX_S(6))) {
                alarms_save(); hal_haptic(10); s_edit = -1;   /* confirm */
                return 1;
            }
            if (dxn * dxn + dyn * dyn <= (BTN_R + GFX_S(6)) * (BTN_R + GFX_S(6))) {
                s_alarms[s_edit] = s_edit_orig; hal_haptic(10); s_edit = -1;  /* cancel */
                return 1;
            }
            /* tap upper/lower half of a column = fine +/-1 */
            if (ev->y > (int)(DRUM_CY - GFX_S(50)) && ev->y < (int)(DRUM_CY + GFX_S(50)))
                bump(col, ev->y < (int)DRUM_CY ? +1 : -1);
            return 1;
        }
        default: return 1;                      /* swallow everything in edit */
        }
    }

    /* -------- list view -------- */
    int max_scroll = NALARMS - LIST_VIS;
    switch (ev->type) {
    case WOS_EV_SWIPE_UP:   s_scroll_to = clampf(s_scroll_to + 1, 0, (float)max_scroll); return 1;
    case WOS_EV_SWIPE_DOWN: s_scroll_to = clampf(s_scroll_to - 1, 0, (float)max_scroll); return 1;
    case WOS_EV_TAP:
        for (int i = 0; i < NALARMS; i++) {
            float y = ROW_TOP + (i - s_scroll) * ROW_PITCH;
            if (ev->y < y - ROW_PITCH / 2 || ev->y > y + ROW_PITCH / 2) continue;
            if (ev->x >= (int)TOGGLE_X) {       /* hit the pill -> toggle */
                s_alarms[i].enabled = !s_alarms[i].enabled;
                alarms_save(); hal_haptic(8);
            } else {                            /* hit the time -> edit */
                s_edit = i; s_edit_orig = s_alarms[i];
                s_ah = s_am = 0; s_enter = 0;
            }
            return 1;
        }
        return 0;
    default: return 0;
    }
}

const wos_app app_alarm = { "Alarm", al_start, on_event, on_draw, GFX_RED, ICON_ALARM };

#endif /* SELFTEST */
