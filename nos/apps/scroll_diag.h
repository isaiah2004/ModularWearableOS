/* Read-only UI-thread scroll poses for serial validation; no state changes. */
#ifndef NOS_SCROLL_DIAG_H
#define NOS_SCROLL_DIAG_H
typedef struct {
    int app, axis, page, pages, dragging; /* apps 1..8; 6..8 continuous, page0/pages1; axis 1 horizontal, 2 vertical */
    float offset, limit;
} nos_scroll_snapshot;
void fit_activity_scroll_snapshot(nos_scroll_snapshot *s);
void fit_heart_scroll_snapshot(nos_scroll_snapshot *s);
void out_weather_scroll_snapshot(nos_scroll_snapshot *s);
void out_baro_scroll_snapshot(nos_scroll_snapshot *s);
void out_nav_scroll_snapshot(nos_scroll_snapshot *s);
void set_about_scroll_snapshot(nos_scroll_snapshot *s);
void sys_launcher_scroll_snapshot(nos_scroll_snapshot *s);
void set_settings_scroll_snapshot(nos_scroll_snapshot *s);
#endif
