/* app_control.c — control center (BOTTOM edge overlay). Header shows the IP-derived location
 * (coords + battery, city below), a brightness slider, a 2x3 feature-status grid (BLE / WiFi /
 * Net / Sync / AI / GPS — pills: white bg = on, dark = off; dot = red down / blue trying /
 * green up; NOT clickable), and three action buttons: Music, Settings, AOD. Empty tap or swipe
 * down dismisses. */
#include "apps.h"
#include "../icons.h"
#include "../font.h"
#include "../battery.h"
#include "../settings.h"
#include "../hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* geometry is in the 240-unit design space (GFX_S scales x466/240 to pixels) */
/* brightness + volume sliders */
#define TRK_X0  GFX_S(50)
#define TRK_X1  (GFX_W - GFX_S(34))
#define TRK_TH  GFX_S(3.5f)
#define BRI_Y   GFX_S(58)
#define VOL_Y   GFX_S(84)
#define HIT_H   GFX_S(9)            /* TIGHT vertical hit band per slider (was too loose) */

/* feature grid */
#define GRID_Y0 GFX_S(117)          /* row centres */
#define GRID_DY GFX_S(25)
#define PILL_W  GFX_S(68)
#define PILL_H  GFX_S(18)
static const struct { const char *label; int feat; } FEAT[6] = {
    { "BLE",  HAL_FEAT_BLE  }, { "WiFi", HAL_FEAT_WIFI }, { "Net",  HAL_FEAT_NET },
    { "Sync", HAL_FEAT_SYNC }, { "AI",   HAL_FEAT_AI   }, { "GPS",  HAL_FEAT_GPS },
};

/* action buttons (icon-only, no labels — nudged down to give the grid room) */
#define ACT_Y   GFX_S(174)
#define ACT_R   GFX_S(15)
static struct { float cx; uint16_t col; int icon; } act[4];

static int s_started;
static int s_drag = -1;              /* slider being dragged: 0 = brightness, 1 = volume, -1 none */
static uint32_t s_vol_sent;          /* throttle host-volume BLE writes */
static int s_app_music = 9, s_app_settings = 23;

static int find_app(const char *name)
{
    for (int i = 0, n = wos_app_count(); i < n; i++)
        if (strcmp(wos_app_name(i), name) == 0) return i;
    return -1;
}

static void layout(void)
{
    int m = find_app("Music");    if (m >= 0) s_app_music = m;
    int s = find_app("Settings"); if (s >= 0) s_app_settings = s;
    const uint16_t col[4] = { GFX_PURPLE, GFX_GREY, GFX_ACCENT, GFX_GREEN };
    const int      ic[4]  = { ICON_MUSIC, ICON_SETTINGS, ICON_CLOCK, ICON_SYNC };  /* Music/Settings/AOD/Resync */
    float gx = GFX_S(44);
    for (int i = 0; i < 4; i++) {
        act[i].cx = GFX_CX + ((float)i - 1.5f) * gx;
        act[i].col = col[i];
        act[i].icon = ic[i];
    }
    s_started = 1;
}

static void on_start(void)
{
    if (!s_started) layout();
    s_drag = -1;
}

/* --- brightness + volume sliders --- */
static int val_from_x(float x)
{
    float t = (x - TRK_X0) / (float)(TRK_X1 - TRK_X0);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return (int)(t * 255.0f + 0.5f);
}
static void draw_slider(gfx_fb *fb, float y, int icon, uint16_t col, int val)
{
    float th = TRK_TH;
    float kx = TRK_X0 + (TRK_X1 - TRK_X0) * (val / 255.0f);
    uint16_t trk = GFX_RGB(58, 60, 68);
    gfx_fill_circle(fb, TRK_X0, y, th, trk);
    gfx_fill_circle(fb, TRK_X1, y, th, trk);
    gfx_fill_rect(fb, TRK_X0, y - th, TRK_X1 - TRK_X0, 2 * th, trk);
    gfx_fill_circle(fb, TRK_X0, y, th, col);
    if (kx > TRK_X0) gfx_fill_rect(fb, TRK_X0, y - th, kx - TRK_X0, 2 * th, col);
    gfx_fill_circle(fb, kx, y, th + GFX_S(5), GFX_WHITE);
    icon_glyph(fb, icon, TRK_X0 - GFX_S(26), y, GFX_S(11), GFX_WHITE);
}
static void apply_slider(int sld, float x)
{
    int v = val_from_x(x);
    if (sld == 0) {
        if (v < 5) v = 5;
        if(v==g_brightness)return;
        g_brightness = v;
        hal_set_backlight((uint8_t)g_brightness);
    } else {
        g_volume = v;
        g_muted = 0;                                       /* reaching for volume = unmute */
        uint32_t now = hal_millis();                       /* stream to the host, throttled */
        if (now - s_vol_sent > 120) { hal_media_vol((int)(v * 100 / 255)); s_vol_sent = now; }
    }
}
/* which slider a touch lands on: TIGHT y band AND on the track horizontally (so a stray touch
 * elsewhere no longer grabs a slider). Returns 0 = brightness, 1 = volume, -1 = neither.
 * The icon column left of the tracks is EXCLUDED — the volume icon there is the mute toggle. */
static int hit_slider(float x, float y)
{
    if (x < TRK_X0 - GFX_S(8) || x > TRK_X1 + GFX_S(10)) return -1;
    if (y > BRI_Y - HIT_H && y < BRI_Y + HIT_H) return 0;
    if (y > VOL_Y - HIT_H && y < VOL_Y + HIT_H) return 1;
    return -1;
}
/* the speaker icon left of the volume track = mute toggle */
static int hit_mute(float x, float y)
{
    float dx = x - (TRK_X0 - GFX_S(26)), dy = y - VOL_Y;
    return dx * dx + dy * dy <= GFX_S(15) * GFX_S(15);
}

/* --- feature pill --- */
static void feat_colors(hal_conn s, uint16_t *bg, uint16_t *dot, uint16_t *txt)
{
    switch (s) {
    case HAL_CONN_UP:     *bg = GFX_WHITE; *dot = GFX_GREEN;            *txt = GFX_BLACK; break;
    case HAL_CONN_TRYING: *bg = GFX_WHITE; *dot = GFX_RGB(64,140,255);  *txt = GFX_BLACK; break;
    case HAL_CONN_DOWN:   *bg = GFX_WHITE; *dot = GFX_RGB(240,64,64);   *txt = GFX_BLACK; break;
    default:              *bg = GFX_RGB(46,48,54); *dot = GFX_RGB(96,98,106); *txt = GFX_GREY; break;
    }
}
static void draw_pill(gfx_fb *fb, int cx, int cy, uint16_t bg, uint16_t dot, const char *label, uint16_t txt)
{
    int w = (int)PILL_W, h = (int)PILL_H, r = h / 2;
    int x0 = cx - w / 2, x1 = cx + w / 2;
    gfx_fill_circle(fb, x0 + r, cy, r, bg);
    gfx_fill_circle(fb, x1 - r, cy, r, bg);
    gfx_fill_rect(fb, x0 + r, cy - r, w - 2 * r, h, bg);
    gfx_fill_circle(fb, x0 + r + GFX_S(3), cy, GFX_S(5), dot);
    font_draw_centered(fb, &font_small, cx + GFX_S(9), cy + (int)(font_small.ascent * 0.34f), label, txt);
}

static void on_draw(gfx_fb *fb)
{
    gfx_clear(fb, GFX_RGB(5, 6, 8));        /* wet black */
    char b[16];
    if(battery_valid()) snprintf(b, sizeof b, "CONTROL  ~%d%%", battery_pct());
    else snprintf(b,sizeof b,"CONTROL  %s",battery_state()==BATT_EXTERNAL ? "USB" : "--");
    font_draw_centered(fb, &font_small, (int)GFX_CX, (int)GFX_S(30), b, GFX_GREY);

    draw_slider(fb, BRI_Y, ICON_FLASH, GFX_ORANGE, g_brightness);      /* brightness */
    /* volume: grey track + red slash over the icon while muted (icon = mute toggle) */
    draw_slider(fb, VOL_Y, ICON_WAVEFORM,
                g_muted ? GFX_RGB(90, 92, 100) : GFX_ACCENT, g_volume);
    if (g_muted) {
        float ix = TRK_X0 - GFX_S(26);
        gfx_line(fb, ix - GFX_S(12), VOL_Y + GFX_S(12),
                     ix + GFX_S(12), VOL_Y - GFX_S(12), GFX_S(2), GFX_RGB(240, 64, 64));
    }

    /* 2x3 feature-status grid */
    int cols[3] = { (int)GFX_CX - (int)PILL_W - (int)GFX_S(3),
                    (int)GFX_CX,
                    (int)GFX_CX + (int)PILL_W + (int)GFX_S(3) };
    for (int i = 0; i < 6; i++) {
        int cx = cols[i % 3], cy = (int)GRID_Y0 + (i / 3) * (int)GRID_DY;
        uint16_t bg, dot, txt;
        feat_colors(hal_status(FEAT[i].feat), &bg, &dot, &txt);
        draw_pill(fb, cx, cy, bg, dot, FEAT[i].label, txt);
    }

    /* action buttons — icon only */
    for (int i = 0; i < 4; i++)
        icon_badge(fb, act[i].cx, ACT_Y, ACT_R, act[i].col, act[i].icon, 0);
}

static int on_event(const wos_event *ev)
{
    switch (ev->type) {
    case WOS_EV_TOUCH_DOWN:
        s_drag=-1;
        if (hit_mute(ev->x, ev->y)) return 1;   /* toggle fires on TAP, not here */
        s_drag = hit_slider(ev->x, ev->y);
        if (s_drag >= 0) apply_slider(s_drag, ev->x);
        return 1;                       /* keep the panel open on any press inside it */
    case WOS_EV_TOUCH_MOVE:
        if (s_drag >= 0) apply_slider(s_drag, ev->x);
        return 1;
    case WOS_EV_TOUCH_UP:
        if (s_drag >= 0) {
            apply_slider(s_drag,ev->x);
            if (s_drag == 1) hal_media_vol((int)(g_volume * 100 / 255));   /* land the final level */
            settings_save(); s_drag = -1;
        }
        return 1;
    case WOS_EV_TAP: {
        if (hit_mute(ev->x, ev->y)) {
            g_muted = !g_muted;
            settings_save();
            if (!g_muted) hal_play(SND_SELECT);    /* audible proof it's back on */
            return 1;
        }
        int sld = hit_slider(ev->x, ev->y);
        if (sld >= 0) { apply_slider(sld, ev->x); settings_save(); return 1; }
        for (int i = 0; i < 4; i++) {              /* action buttons */
            float dx = ev->x - act[i].cx, dy = ev->y - ACT_Y;
            if (dx * dx + dy * dy <= (ACT_R + GFX_S(6)) * (ACT_R + GFX_S(6))) {
                if (i == 0)      wos_open_app(s_app_music);
                else if (i == 1) wos_open_app(s_app_settings);
                else if (i == 2) hal_set_aod(1);
                else             hal_time_resync();       /* re-sync clock + location */
                return 1;
            }
        }
        return 0;                       /* empty tap -> dismiss */
    }
    default:
        return 0;
    }
}

const wos_app app_control = { "control", on_start, on_event, on_draw, GFX_GREY, ICON_NONE };
