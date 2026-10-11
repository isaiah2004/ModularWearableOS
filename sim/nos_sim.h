/* nos_sim.h — internal interface between the NOS simulator's parts:
 *   nos_sim.c      main, window, scripting, cost model, damage check
 *   nos_hal_sim.c  core/hal.h on the PC (clock, RTC, battery, store, fake radios)
 *   nos_img.c      PNG / GIF writers, contact sheets, a tiny label font
 *   nos_ext_sim.c  nos/hal_ext.h on the PC (owned by the modules agent), plus the
 *                  sim hooks below (hal_ext_init, nos_ext_sim_set/tick, hal_ext_sim_present)
 * Nothing in nos/ or core/ may include this. */
#ifndef NOS_SIM_H
#define NOS_SIM_H
#include <stdint.h>
#include <stdio.h>

/* ---- clock --------------------------------------------------------------------
 * Interactive mode: hal_millis() is real time (QueryPerformanceCounter).
 * Scripted mode: sim_virtual = 1 and hal_millis() is a virtual clock that only the
 * script runner advances (exactly 33.333 ms per frame), so runs are deterministic. */
extern int    sim_virtual;
extern double sim_vclock_ms;          /* virtual ms since boot (scripted mode) */
uint32_t sim_real_ms(void);           /* real ms since start, never virtual */

/* ---- simulated RTC ------------------------------------------------------------
 * hal_get_datetime() = RTC base + time elapsed on the active clock since it was set.
 * Scripted mode boots at 2026-10-11 10:09:30 (deterministic); interactive at the PC
 * local time. hal_epoch_seconds() = local time - sim_tz_min minutes. */
void sim_rtc_set_time(int h, int m, int s);
void sim_rtc_set_date(int y, int mo, int d);
void sim_rtc_from_pc(void);
extern int sim_tz_min;                /* minutes east of UTC used for the epoch, default 330 */

/* ---- board state the script / keys can change --------------------------------- */
extern int sim_batt_pct;              /* hal_battery_percent */
extern int sim_charging;              /* 1 = external power observed */
extern int sim_button_down;           /* side button as the sim drives it (hal_button_down) */
extern int sim_store_disabled;        /* 1 = hal_store_* is a RAM-only no-op (scripted runs) */
extern char sim_store_dir[260];       /* where store_*.dat files live */
/* Queue a companion notification: delivered by hal_notification_poll(). 1 = queued. */
int  sim_notify_push(const char *app, const char *title);
extern int sim_steps;                 /* hal_step_count base value */

/* Optional extra `set KEY VALUE` handling by the hal_ext simulation (nos_ext_sim.c):
 * return 1 if the key was handled. Keys handled by the sim itself: time, date, tz,
 * battery, charging, present, steps. Everything else is offered here first. */
int  nos_ext_sim_set(const char *key, const char *value);
/* Called once per frame (before nos_frame_run) so synthetic module data can advance
 * on the active clock. */
void nos_ext_sim_tick(uint32_t now_ms);
void hal_ext_init(void);              /* called once at boot */

/* ---- images (nos_img.c) -------------------------------------------------------- */
/* 24-bit RGB image, row-major, 3 bytes per pixel. */
typedef struct { int w, h; uint8_t *rgb; } sim_img;
sim_img img_new(int w, int h, uint8_t r, uint8_t g, uint8_t b);
void    img_free(sim_img *im);
/* Copy an RGB565 GFX_W x GFX_H frame with the round panel mask (outside = bezel colour). */
void    img_from_565(sim_img *im, int dx, int dy, const uint16_t *fb, int masked);
void    img_from_565_scaled(sim_img *im, int dx, int dy, const uint16_t *fb, int masked, int div);
void    img_rect(sim_img *im, int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
void    img_frame(sim_img *im, int x, int y, int w, int h, int t, uint8_t r, uint8_t g, uint8_t b);
/* 5x7 label text (ASCII 32-126), scale = integer pixel size. Returns width. */
int     img_text(sim_img *im, int x, int y, const char *s, int scale, uint8_t r, uint8_t g, uint8_t b);
int     img_text_width(const char *s, int scale);
int     png_write(const char *path, const sim_img *im);
/* 5x7 glyph rows for ASCII c (bit 4 = leftmost column); used by the window HUD too. */
const uint8_t *img_glyph(int c);

/* Streaming animated GIF with a fixed 256-colour palette tuned for NOS (black, greys,
 * white, Nothing red and red tints, plus a colour cube for the rest). Each frame only
 * encodes the bounding box that changed since the previous frame. */
typedef struct gif_w gif_w;
gif_w  *gif_open(const char *path, int w, int h);
/* Add a frame given as an RGB image of the GIF's size. delay in 1/100 s. */
void    gif_add(gif_w *g, const sim_img *im, int delay_cs);
void    gif_close(gif_w *g);
/* Exact 30 fps pacing in centiseconds: 3,3,4,3,3,4... (frame index -> delay). */
int     gif_delay_30fps(int frame_index);

/* round-mask coverage 0-255 of panel pixel (x, y) */
uint8_t sim_mask(int x, int y);

/* sim/nos_phone_sim.c: fake phone link */
void nos_phone_sim_set(int connected);
int  nos_phone_sim_cmd(const char *s);
#endif
