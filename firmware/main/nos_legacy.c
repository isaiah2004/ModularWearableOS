/* nos_legacy.c — globals the retained core/settings.c and core/wake_service.c still
 * reference from the retired UI (wos, Flat60, old settings app). NOS keeps its own
 * preferences in nos/prefs.c; these only keep the legacy NVS blob loadable. */
int g_setting_24h = 1;
int g_setting_tilt_wake = 1;
int wos_debug_hud, wos_debug_rects, wos_fps_cap48, wos_fast_display, wos_ui_style;
