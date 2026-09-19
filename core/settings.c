/* settings.c — pack the scattered setting globals into one NVS blob. */
#include "settings.h"
#include "hal.h"
#include "wake_service.h"
#include <stdint.h>

/* live settings owned elsewhere */
extern int g_setting_24h;        /* app_settings.c */
extern int g_setting_tilt_wake;  /* app_settings.c */
extern int wos_debug_hud;        /* wos.c */

int g_brightness = 200;
int g_volume = 160;
int g_muted = 1;                  /* ships MUTED — speaker only after explicit unmute */
int g_saved_face = 0;
int g_tz_offset_min = 330;        /* default IST; minutes east of UTC, persisted */
uint32_t g_time_last_set = 0;     /* unix epoch of last manual set, 0 = never */

/* "WTC5": grew a muted field. A "WTC4" blob is still accepted (its fields are a
 * prefix of this layout) and muted falls back to the default (1) — so existing
 * watches keep their settings but come up silent after this update. */
#define CFG_MAGIC    0x57544335u   /* "WTC5" — bump if the layout changes */
#define CFG_MAGIC_V4 0x57544334u   /* previous layout, no muted field */

typedef struct {
    uint32_t magic;
    int32_t brightness, fmt24, tilt, hud, face, tz_min, volume;
    int32_t last_set;           /* unix epoch (local, best-effort) of last set */
    int32_t muted;              /* WTC5+ */
} cfg_blob;

void settings_load(void)
{
    cfg_blob b;
    b.muted = 1;
    /* hal_store_load wants an exact size, so a pre-muted (WTC4) blob needs its
     * own, shorter read — its fields are a prefix of the current layout. */
    int ok = hal_store_load("cfg", &b, sizeof b);
    if (!ok) ok = hal_store_load("cfg", &b, (int)(sizeof b - sizeof(int32_t)));
    if (ok && (b.magic == CFG_MAGIC || b.magic == CFG_MAGIC_V4)) {
        g_brightness        = b.brightness;
        g_setting_24h       = b.fmt24;
        g_setting_tilt_wake = b.tilt;
        wos_debug_hud       = b.hud;
        g_saved_face        = b.face;
        g_volume            = b.volume;
        g_tz_offset_min     = b.tz_min;
        g_time_last_set     = (uint32_t)b.last_set;
        g_muted             = (b.magic == CFG_MAGIC) ? (b.muted != 0) : 1;
    }
    if (g_brightness < 5) g_brightness = 5;
    if (g_volume < 0 || g_volume > 255) g_volume = 160;
    if (g_tz_offset_min < -720 || g_tz_offset_min > 840) g_tz_offset_min = 0;
    hal_set_backlight((uint8_t)g_brightness);
    wake_service_init(g_setting_tilt_wake);
}

void settings_save(void)
{
    cfg_blob b = {
        CFG_MAGIC, g_brightness, g_setting_24h, g_setting_tilt_wake,
        wos_debug_hud, g_saved_face, g_tz_offset_min, g_volume,
        (int32_t)g_time_last_set, g_muted
    };
    hal_store_save("cfg", &b, sizeof b);
}
