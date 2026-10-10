#ifndef WATCH_APPS_H
#define WATCH_APPS_H

#include "../wos.h"
#include "../icons.h"

/* openable apps (appear in launcher / list) */
extern const wos_app app_watchface;
extern const wos_app app_stopwatch;
extern const wos_app app_wifi;
extern const wos_app app_settings;
extern const wos_app app_timer;
extern const wos_app app_alarm;
extern const wos_app app_calc;
extern const wos_app app_music;
extern const wos_app app_recorder;
extern const wos_app app_ble;
extern const wos_app app_level;
extern const wos_app app_flashlight;
extern const wos_app app_weather;
extern const wos_app app_activity;
extern const wos_app app_voice;
extern const wos_app app_netmon;
extern const wos_app app_calendar;
extern const wos_app app_sensors;
extern const wos_app app_worldclock;
extern const wos_app app_breathe;
extern const wos_app app_findphone;
extern const wos_app app_2048;
extern const wos_app app_battery;
extern const wos_app app_pomodoro;
extern const wos_app app_pet_care;

/* Background alarm scheduler — call once per main-loop iteration on any platform. Self-throttles
 * (checks ~1/min, tightening to ~1/s within a few seconds of an alarm) and rings SND_ALARM +
 * a callout when an enabled alarm's minute arrives, regardless of the foreground app. */
void alarm_service_tick(void);

/* Pip: expressive eye face with touch, motion, circadian rhythm and memory. */
void face_pip_draw(gfx_fb *fb, const hal_datetime *t);
int  face_pip_event(const wos_event *ev);
const char *face_pip_debug(char *buf, int n);   /* serial harness dump */
void face_pip_service(int save_now);          /* outside rendering; periodic persistence */
int face_pip_command(const char *command);    /* center, save, live, demo ACTION */
void face_pip_display(int on);
void face_pip_alarm(void);

/* standalone watch faces (registered in the app_watchface.c gallery) */
void face_dense_draw(gfx_fb *fb, const hal_datetime *t);
void face_nothing_draw(gfx_fb *fb, const hal_datetime *t);
void face_samsung_draw(gfx_fb *fb, const hal_datetime *t);
void face_ultra_draw(gfx_fb *fb, const hal_datetime *t);
void face_pixel_draw(gfx_fb *fb, const hal_datetime *t);

/* edge-menu overlays */
extern const wos_app app_launcher;      /* LEFT   */
extern const wos_app app_applist;       /* RIGHT  */
extern const wos_app app_stack;         /* RIGHT, Flat60 style */
extern const wos_app app_notifications; /* TOP    */
extern const wos_app app_control;       /* BOTTOM */

extern const wos_app app_storage;

void app_diagnostics_touch_test(void);
#endif
