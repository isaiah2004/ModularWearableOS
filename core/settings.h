/* settings.h — persisted user settings (brightness, toggles, current face),
 * backed by hal_store_* (NVS on device, a file in the sim). Load once at boot;
 * save whenever something changes. */
#ifndef WATCH_SETTINGS_H
#define WATCH_SETTINGS_H

#include <stdint.h>

extern int g_brightness;      /* 0-255, the single source of truth (was
                                 duplicated in Settings and Control) */
extern int g_volume;          /* 0-255 media/speaker volume (ES8311 when wired) */
extern int g_muted;           /* 1 = local speaker silenced (default ON — ships quiet) */
extern int g_saved_face;      /* current watch-face index, restored at boot */

/* Timezone offset, minutes east of UTC (e.g. -300 = UTC-05:00, +330 = IST).
 * Persisted and restored at boot. The B board restores UTC from its PCF85063
 * RTC when valid; this offset converts UTC to the owner's local routine. */
extern int g_tz_offset_min;

/* Unix epoch (seconds, local best-effort) of the last manual time set from the
 * Settings time-setter. 0 = never set. Purely informational until a real time
 * source exists; lets a boot-time sync estimate how stale the clock is. */
extern uint32_t g_time_last_set;

void settings_load(void);     /* read NVS + apply brightness; call once at boot */
void settings_save(void);     /* snapshot the live settings to NVS */

#endif
