/* apps.h — every NOS screen, widget and face, and the launcher registry.
 * Owners are noted per group; each owner edits only their own files. */
#ifndef NOS_APPS_H
#define NOS_APPS_H
#include "nos.h"

/* ---- system (sys_*.c) ---- */
extern const nos_screen scr_boot;        /* boot animation, then replaces itself with home */
extern const nos_screen scr_home;        /* root: face + widget pager */
extern const nos_screen scr_control;     /* control centre (TX_DROP) */
extern const nos_screen scr_notifs;      /* notification list (TX_RISE) */
extern const nos_screen scr_notif_detail;
extern const nos_screen scr_launcher;    /* app list (TX_IRIS) */
extern const nos_screen scr_faces;       /* face picker */
extern const nos_screen scr_power;       /* power sheet: sleep / restart / power off */
void sys_notif_open(int i);              /* push the detail view for notification i */
void sys_home_build_in(void);            /* restart the face build-in (boot, wake, new face) */
const struct nos_screen *sys_launcher_last(void);   /* last app opened from the launcher, or 0 */
void sys_launcher_prewarm(void);         /* bake one launcher row sprite (idle frames) */

/* ---- health (fit_*.c) ---- */
extern const nos_screen scr_activity, scr_heart, scr_breathe;
/* ---- time (time_*.c) ---- */
extern const nos_screen scr_timer, scr_stopwatch, scr_alarm, scr_worldclock;
/* scr_timer_ring, scr_alarm_ring are declared in svc.h */
/* ---- outdoor (out_*.c) ---- */
extern const nos_screen scr_compass, scr_baro, scr_nav, scr_weather;
/* ---- connect (con_*.c) ---- */
extern const nos_screen scr_music, scr_cell, scr_find, scr_memo, scr_torch;
/* scr_call, scr_find_watch are declared in svc.h */
/* ---- settings & system (set_*.c) ---- */
extern const nos_screen scr_settings, scr_modules, scr_about;

/* ---- home widgets (one full-screen glance page each, owned by the app group) ----
 * Drawn by the home pager at a horizontal offset (fb is already translated). */
typedef struct {
    const char *name;
    void (*tick)(float dt, int visible);    /* visible = this page is on screen */
    void (*draw)(gfx_fb *fb);
    void (*tap)(int x, int y);              /* typically pushes the app (set nos_iris_x/y) */
    void (*shown)(void);                    /* page settled into view: start build-in */
    void (*press)(int x, int y, int down);  /* optional: finger down (1) / released or cancelled (0), for pressed states */
} nos_widget;
extern const nos_widget wg_activity, wg_heart, wg_weather, wg_music, wg_timer;

/* ---- launcher registry (registry.c) ---- */
typedef struct { const char *name; int icon; const nos_screen *scr; } nos_app;
extern const nos_app nos_app_list[];
extern const int nos_app_count;
/* Open an app from a launcher/complication/widget point with the iris transition. */
void nos_open(const nos_screen *s, int from_x, int from_y);
/* nos_open from a tapped icon (DOT_ICON_* on a C_SURF2 disc of radius r): the iris
 * grows out of it (see nos_iris_seed) */
void nos_open_icon(const nos_screen *s, int from_x, int from_y, int icon, float r);

/* Boot: services, prefs, root screen. Called once by the host after nos_init(). */
void nos_apps_boot(void);
#endif
