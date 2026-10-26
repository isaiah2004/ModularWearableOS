/* apps.h — every NOS screen, widget and face, and the launcher registry.
 * Owners are noted per group; each owner edits only their own files. */
#ifndef NOS_APPS_H
#define NOS_APPS_H
#include "nos.h"

/* ---- system (sys_*.c) ---- */
extern const nos_screen scr_boot;        /* boot animation, then replaces itself with home */
extern const nos_screen scr_home;        /* root: face + widget pager */
extern const nos_screen scr_developer;
extern const nos_screen scr_background;
extern const nos_screen scr_chat;
extern const nos_screen scr_control;     /* control centre (TX_DROP) */
extern const nos_screen scr_notifs;      /* notification list (TX_RISE) */
extern const nos_screen scr_notif_detail;
extern const nos_screen scr_launcher;    /* app list (TX_IRIS) */
extern const nos_screen scr_faces;       /* face picker */
extern const nos_screen scr_faces_home;  /* the same picker opened in place from home (no back swipe) */
extern const nos_screen scr_power;       /* power sheet: sleep / restart / power off */
void sys_notif_open(int i);              /* push the detail view for notification i */
void sys_home_build_in(void);            /* restart the face build-in (boot, wake, new face) */
void sys_home_open_after(const nos_screen *s);  /* open s once home is back on top */
int sys_home_aod_entering(void);         /* AOD entry ramp running: host keeps 30 Hz frames */
int sys_home_aod_dimming(void);          /* ...and in its first half (the face dimming out) */
/* Recording chip (home + control centre): 0 none, 1 track, 2 memo, 3 IMU, 4 workout. */
int sys_rec_state(char *label, int n);
const nos_screen *sys_rec_screen(int kind);
int sys_rec_width(const char *label);
void sys_rec_draw(gfx_fb *fb, int x0, int y, const char *label, uint16_t dot, uint16_t ink);
const struct nos_screen *sys_launcher_last(void);   /* last app opened from the launcher, or 0 */
/* Bounded resumable slices, called from home idle and launcher tick. */
void sys_launcher_prewarm(void);
extern unsigned sys_launcher_prewarm_budget_us, sys_launcher_prewarm_max_steps;
void sys_notifs_prewarm(void);           /* bake a new notification card's blur (idle frames) */
extern unsigned set_settings_bake_budget_us, set_settings_bake_max_steps;
/* Exact optional carry of rolling-control sums; no new allocation owner. */
void set_settings_carry_set(int enabled);
int set_settings_carry_get(void);
void set_settings_carry_report(void);
void set_settings_prewarm(void);         /* first screenful, shared with the opened list */
float set_settings_scroll_offset(void); /* read-only actual list pose for diagnostics */

/* ---- health (fit_*.c) ---- */
extern const nos_screen scr_activity, scr_heart, scr_breathe;
extern const nos_screen scr_workout;
extern const nos_screen scr_energy, scr_battery_history, scr_sports_logs, scr_quick_report, scr_wrist_tuning;
/* ---- time (time_*.c) ---- */
extern const nos_screen scr_timer, scr_stopwatch, scr_alarm, scr_worldclock;
/* scr_timer_ring, scr_alarm_ring are declared in svc.h */
/* ---- outdoor (out_*.c) ---- */
extern const nos_screen scr_compass, scr_baro, scr_imu, scr_nav, scr_weather;
/* ---- connect (con_*.c) ---- */
extern const nos_screen scr_music, scr_cell, scr_find, scr_memo, scr_torch;
/* scr_call, scr_find_watch are declared in svc.h */
/* ---- settings & system (set_*.c) ---- */
extern const nos_screen scr_settings, scr_modules, scr_about, scr_diag;
extern const nos_screen scr_battery;      /* set_battery.c: Settings > Battery, control header */

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
int sys_control_prewarm(void);

/* ---- launcher registry (registry.c) ---- */
typedef struct { const char *name; int icon; const nos_screen *scr; } nos_app;
extern const nos_app nos_app_list[];
extern const int nos_app_count;
/* Open an app from a launcher/complication/widget point with the iris transition. */
void nos_open(const nos_screen *s, int from_x, int from_y);
/* nos_open from a tapped icon (DOT_ICON_* on a C_SURF2 disc of radius r): the iris
 * grows out of it (see nos_iris_seed) */
const nos_screen *nos_last_app(void);   /* last app nos_open()ed from anywhere, or 0 */
void nos_open_icon(const nos_screen *s, int from_x, int from_y, int icon, float r);

/* Boot: services, prefs, root screen. Called once by the host after nos_init(). */
void nos_apps_boot(void);
#endif
