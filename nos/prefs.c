/* prefs.c — see prefs.h */
#include "prefs.h"
#include "hal.h"
#include <string.h>

#define PREFS_VERSION 3
nos_prefs_t nos_prefs;
static int s_dirty;
static uint32_t s_dirty_ms;

void prefs_defaults(void)
{
    memset(&nos_prefs, 0, sizeof nos_prefs);
    nos_prefs.version = PREFS_VERSION;
    nos_prefs.haptics = 1;
    nos_prefs.haptic_strength = 70;
    nos_prefs.sound = 1;
    nos_prefs.volume = 60;
    nos_prefs.brightness = 70;
    nos_prefs.auto_bright = 1;
    nos_prefs.aod = 0;
    nos_prefs.raise_wake = 1;
    nos_prefs.wrist_left = 1;
    nos_prefs.hour24 = 1;
    nos_prefs.face = 0;
    nos_prefs.sleep_s = 15;
    nos_prefs.wifi = 1;
    nos_prefs.ble = 1;
    nos_prefs.lte = 0;
    nos_prefs.gps_auto = 1;
    nos_prefs.tz_min = 330;          /* IST, matching the existing firmware default */
    nos_prefs.step_goal_100 = 80;    /* 8000 steps */
    nos_prefs.units_metric = 1;
}

void prefs_load(void)
{
    nos_prefs_t p;
    if (hal_store_load("nosprefs", &p, sizeof p) && p.version == PREFS_VERSION) nos_prefs = p;
    else prefs_defaults();
    if (nos_prefs.sleep_s < 5) nos_prefs.sleep_s = 5;
}

void prefs_save(void) { s_dirty = 1; s_dirty_ms = hal_millis(); }

void prefs_tick(void)
{
    if (s_dirty && hal_millis() - s_dirty_ms > 2000) {
        s_dirty = 0;
        hal_store_save("nosprefs", &nos_prefs, sizeof nos_prefs);
    }
}
