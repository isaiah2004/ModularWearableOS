/* registry.c — launcher order, app opening and boot. */
#include "apps/nos_apps.h"
#include "svc.h"
#include "prefs.h"
#include "dot.h"

const nos_app nos_app_list[] = {
    { "Activity",    DOT_ICON_STEPS,     &scr_activity },
    { "Heart rate",  DOT_ICON_HEART,     &scr_heart },
    { "Breathe",     DOT_ICON_BREATHE,   &scr_breathe },
    { "Weather",     DOT_ICON_CLOUD,     &scr_weather },
    { "Music",       DOT_ICON_MUSIC,     &scr_music },
    { "Timer",       DOT_ICON_TIMER,     &scr_timer },
    { "Stopwatch",   DOT_ICON_STOPWATCH, &scr_stopwatch },
    { "Alarms",      DOT_ICON_ALARM,     &scr_alarm },
    { "World clock", DOT_ICON_GLOBE,     &scr_worldclock },
    { "Compass",     DOT_ICON_COMPASS,   &scr_compass },
    { "Barometer",   DOT_ICON_GAUGE,     &scr_baro },
    { "Navigate",    DOT_ICON_ROUTE,     &scr_nav },
    { "Cellular",    DOT_ICON_SIGNAL,    &scr_cell },
    { "Recorder",    DOT_ICON_MIC,       &scr_memo },
    { "Find phone",  DOT_ICON_FIND,      &scr_find },
    { "Torch",       DOT_ICON_TORCH,     &scr_torch },
    { "Settings",    DOT_ICON_SETTINGS,  &scr_settings },
};
const int nos_app_count = sizeof nos_app_list / sizeof nos_app_list[0];

void nos_open(const nos_screen *s, int from_x, int from_y)
{
    nos_iris_x = from_x;
    nos_iris_y = from_y;
    nos_feedback(FB_OPEN);
    nos_push(s, TX_IRIS);
}

void nos_apps_boot(void)
{
    svc_init();
    nos_bg_tick = svc_tick;
    nos_set_root(&scr_home);
    nos_push(&scr_boot, TX_NONE);
}
