/* nos_sim.c — the NOS simulator: interactive window, headless scripted runs on a
 * virtual clock, the ESP32-S3 frame-cost model and the damage-tracking check.
 * Usage: docs/NOS_SIM.md, or `nos_sim.exe --help`.
 *
 * The engine always renders through the device path: NOS_STRIPE_ROWS stripes, flushed into a
 * panel-RAM buffer (s_panel) exactly like the QSPI flush, so the cost model sees the
 * same per-stripe primitive calls the watch makes. --fullframe uses the engine's
 * full-frame path instead (no call counting fidelity, handy for debugging). */
/* Older MinGW defaults to MSVCRT printf, which cannot read/write %llx hashes. */
#if defined(__MINGW32__) && !defined(__USE_MINGW_ANSI_STDIO)
#define __USE_MINGW_ANSI_STDIO 1
#endif
/* Declare CreateWaitableTimerExW with its HANDLE return type on older MinGW. */
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <assert.h>
#include <stdarg.h>
#include <errno.h>
#include "nos.h"
#include "developer.h"
#include "pblur.h"
#include "prefs.h"
#include "voice_chat.h"
#include "frame_cap.h"
#include "hal_ext.h"
#include "gfx_cost.h"
#include "nos_sim.h"
#include "apps/nos_apps.h"
#include "settings_shutter.h"
#include "native_menu_assets.h"
#include "apps/sys_launcher_page.h"
#include "svc.h"
#include "workout.h"
#include "wrist_detect.h"
#include "dot.h"
#include "ui.h"
#include "prog.h"
#include "faces/orrery.h"
#include "faces/face_ext.h"
#include "faces/orrery_shade_q16.h"
extern void orrery_card_debug_body(int ob);
#include "render_layers.h"
#include "render_history.h"
#include "render_exposure.h"

/* hal_ext_sim_present, hal_ext_init(), nos_ext_sim_set() and nos_ext_sim_tick() are
 * defined in sim/nos_ext_sim.c. No weak symbols anywhere: MinGW/PE weak definitions
 * neither act as cross-object fallbacks nor reliably merge with the strong copy. */
extern int nos_dbg_rect[12][4], nos_dbg_nr;       /* nos.c: last frame's dirty rects */
extern const nos_screen nos_selftest_root;        /* sim/nos_selftest.c */
extern int sim_log_quiet;                         /* nos_hal_sim.c */
extern int sim_imu_override;
extern float sim_imu_a[3];

/* nos_apps_boot() (nos/apps/registry.c) sets the real root; --selftest uses ours. */

/* Every screen a script can `open NAME` (C name without the scr_ prefix). Keep in sync
 * with nos/apps/nos_apps.h and nos/svc.h. */
extern const nos_screen scr_prog_lab;    /* nos_prog_lab.c */
static const struct { const char *name; const nos_screen *s; } SCREENS[] = {
    { "boot", &scr_boot }, { "home", &scr_home }, { "control", &scr_control }, { "notifs", &scr_notifs },
    { "notif_detail", &scr_notif_detail }, { "launcher", &scr_launcher }, { "faces", &scr_faces },
    { "orrery_card", &scr_orrery_card },
    { "power", &scr_power }, { "activity", &scr_activity }, { "heart", &scr_heart }, { "breathe", &scr_breathe },
    { "energy", &scr_energy }, { "battery", &scr_battery_history }, { "logs", &scr_sports_logs },
    { "background", &scr_background },
    { "chat", &scr_chat },
    { "sports", &scr_workout }, { "workout", &scr_workout },
    { "report", &scr_quick_report }, { "tuning", &scr_wrist_tuning },
    { "timer", &scr_timer }, { "stopwatch", &scr_stopwatch }, { "alarm", &scr_alarm },
    { "worldclock", &scr_worldclock }, { "compass", &scr_compass }, { "baro", &scr_baro }, { "imu", &scr_imu }, { "nav", &scr_nav },
    { "weather", &scr_weather }, { "music", &scr_music }, { "cell", &scr_cell }, { "find", &scr_find },
    { "memo", &scr_memo }, { "torch", &scr_torch }, { "settings", &scr_settings }, { "modules", &scr_modules }, { "diag", &scr_diag },
    { "developer", &scr_developer }, { "about", &scr_about }, { "alarm_ring", &scr_alarm_ring }, { "timer_ring", &scr_timer_ring },
    { "call", &scr_call }, { "find_watch", &scr_find_watch }, { "selftest", &nos_selftest_root },
    { "prog_lab", &scr_prog_lab },
};
static const nos_screen *screen_by_name(const char *n)
{
    if (!_strnicmp(n, "scr_", 4)) n += 4;
    for (unsigned i = 0; i < sizeof SCREENS / sizeof SCREENS[0]; i++)
        if (!_stricmp(SCREENS[i].name, n)) return SCREENS[i].s;
    return 0;
}
/* The firmware's host power hook (firmware/main/nos_main.c): 0 sleep, 1 restart, 2 off. */
void nos_host_power(int mode)
{
    static const char *what[3] = { "sleep", "restart", "power off" };
    char t[48];
    snprintf(t, sizeof t, "host power: %s", mode >= 0 && mode < 3 ? what[mode] : "?");
    hal_log(t);
    nos_toast(mode == 0 ? "SLEEP (SIM)" : mode == 1 ? "RESTART (SIM)" : "POWER OFF (SIM)");
}

static void skip_boot(void)
{
    if (nos_top() == &scr_boot && nos_depth() > 1) nos_pop();   /* pushed with TX_NONE: instant */
}

/* ---- buffers ---------------------------------------------------------------------- */
#define STRIPE_ROWS NOS_STRIPE_ROWS
static uint16_t s_panel[GFX_W * GFX_H];          /* panel RAM: what the glass shows */
static uint16_t s_full[GFX_W * GFX_H];           /* engine full frame (--fullframe) */
static uint16_t s_stripes[2][GFX_W * STRIPE_ROWS];
static uint16_t s_scratch[GFX_W * GFX_H];        /* two-layer transitions */
static int s_fullframe_mode;
static uint32_t s_flushed_px;                    /* pixels sent this frame */
static uint32_t s_flushes;
static int s_exposure_worker = 1;
static nos_exposure_job s_exposure_jobs[2];
static int s_exposure_ready[2];
static uint32_t s_worker_history_us;
typedef struct {
    uint16_t *buf;
    int stride, x0, y0, w, rows;
    const nos_exposure_job *exposure;
} sim_flush_job;
static sim_flush_job s_flush_jobs[2];
static unsigned s_flush_head, s_flush_tail;

static void flush_pixels(uint16_t *buf, int stride, int x0, int y0, int w, int rows)
{
    for (int r = 0; r < rows; r++)
        memcpy(&s_panel[(y0 + r) * GFX_W + x0], &buf[r * stride], (size_t)w * 2);
    s_flushed_px += (uint32_t)(w * rows);
    /* One rendered stripe can produce several physical DMA transfers. */
    s_flushes += (uint32_t)((rows + NOS_DMA_ROWS - 1) / NOS_DMA_ROWS);
}
static void flush_one(void)
{
    assert(s_flush_tail != s_flush_head);
    const sim_flush_job *j = &s_flush_jobs[s_flush_tail++ & 1u];
    if (j->exposure) {
        uint32_t start = hal_micros();
        nos_exposure_apply(j->exposure);
        s_worker_history_us += hal_micros() - start;
    }
    flush_pixels(j->buf, j->stride, j->x0, j->y0, j->w, j->rows);
}
static void stripe_wait(void)
{
    /* Delay the consumer until a slot is needed. This exercises metadata and
     * pixel ownership across two producer draws instead of applying inline. */
    if (s_flush_head - s_flush_tail == 2) flush_one();
}
static int stripe_exposure_prepare(const gfx_fb *fb, int x0, int y0, int x1, int y1, int slot)
{
    s_exposure_ready[slot] = s_exposure_worker &&
        nos_exposure_prepare(&s_exposure_jobs[slot], fb, x0, y0, x1, y1, nos_temporal_blur, nos_aod);
    return s_exposure_ready[slot];
}
static uint32_t stripe_exposure_finish(void)
{
    while (s_flush_tail != s_flush_head) flush_one();
    uint32_t us = s_worker_history_us;
    s_worker_history_us = 0;
    return us;
}
static void flush_cb(uint16_t *buf, int stride, int x0, int y0, int w, int rows)
{
    int slot = buf - x0 == s_stripes[0] ? 0 : 1;
    assert(buf - x0 == s_stripes[slot]);
    if (!s_exposure_ready[slot]) {
        /* The engine has fenced any earlier worker jobs before sync exposure. */
        assert(s_flush_tail == s_flush_head);
        flush_pixels(buf, stride, x0, y0, w, rows);
        return;
    }
    assert(s_flush_head - s_flush_tail < 2);
    s_flush_jobs[s_flush_head++ & 1u] = (sim_flush_job) {
        buf, stride, x0, y0, w, rows, &s_exposure_jobs[slot]
    };
}
/* What the glass shows: the panel GRAM, inverted while the light theme is on (the real
 * panel inverts in hardware, DCS INVON; see firmware/main/nos_main.c theme_apply). */
static int s_inverted;
static uint16_t s_glass[GFX_W * GFX_H];
static const uint16_t *panel(void)
{
    const uint16_t *p = s_fullframe_mode ? s_full : s_panel;
    if (!s_inverted) return p;
    for (int i = 0; i < GFX_W * GFX_H; i++) s_glass[i] = (uint16_t)~p[i];
    return s_glass;
}

/* ---- device cost model -------------------------------------------------------------
 * ns per written pixel, measured on the watch (docs/FLAT60_DESIGN.md). */
/* Scrub-3: 8.909 us per weighted software-double unit fits the a684695
 * four linear 1200 ms chords to 47.4 ms NOSF work. This is an aggregate
 * residual (incl. soft-double arithmetic/PSRAM), not isolated libm timing.
 * Baseline and current source share this price; no post-change discount. */
static const double CLASS_NS[GFX_COST_NCLASS] = { 4, 25, 32, 33, 131, 146, 190, 280, 300, 507, 641, 723, 8, 6, 1000, 1000, 250, 56, 744, 1, 625, 5600, 150, 150, 310, 1254, 1, 110, 8909 };
static const char *CLASS_NAME[GFX_COST_NCLASS] = { "fill", "circle", "pill", "blit", "mask", "glyph",
                                                   "blend", "text", "ring", "arc", "icon", "line",
                                                   "dim", "select", "orr_walk", "orr_build", "orr_index", "psram_write", "psram_rmw", "orr_cpu", "orr_pixel", "orr_capture", "orr_bucket", "orr_geometry", "orr_run", "orr_lit", "orr_pre_cpu", "orr_postdim", "orr_math" };
#define CALL_NS       400.0     /* per primitive call, per stripe it runs in */
#define FIXED_MS      1.5       /* record pass, damage diff, flush bookkeeping */
#define PIPE_MS       1.0       /* raster/flush pipeline fill */
#define BUS_BPS       18.6e6    /* effective QSPI bytes/s */
#define FRAME_MS      (1000.0 / NOS_FPS)

static int s_cost_on, s_target_psram;
static double s_psram_base_ns;
void gfx_cost_target(int psram) { s_target_psram=psram; }
/* ORRERY category estimates: stages raster/capture/bucket, same order as ORRP.
 * This attribution uses counted operations, not desktop execution time. */
static double s_orr_ns[3][7];
static int s_orr_stage=-1, s_orr_category;
static int s_orr_rings, s_orr_math;
void gfx_cost_add(int cls,unsigned px,int calls);
void gfx_cost_orr_math_scope(int on) { s_orr_math=on; }
void gfx_cost_orr_math(unsigned units)
{
    /* This residual was fitted with rings open. Do not extrapolate a mixed
     * scrub/PSRAM fit to locked idle event scans or other applications. */
    if(s_orr_math && s_orr_rings)gfx_cost_add(GFX_COST_ORR_MATH,units,0);
}
void gfx_cost_orr_rings(int open) { s_orr_rings=open; }
void gfx_cost_orr_scope(int stage,int category) { s_orr_stage=stage; s_orr_category=category; }
static uint64_t s_cls_px[GFX_COST_NCLASS], s_cls_calls[GFX_COST_NCLASS];
/* Counted device cost, never desktop wall time; also available outside a scope. */
uint32_t gfx_cost_orr_micros(void)
{
    double ns=0;
    for(int i=0;i<GFX_COST_NCLASS;i++) ns+=s_cls_px[i]*CLASS_NS[i]+s_cls_calls[i]*CALL_NS;
    return (uint32_t)(ns/1000.0);
}
void gfx_cost_add(int cls, unsigned px, int calls)
{
    if (!s_cost_on || cls < 0 || cls >= GFX_COST_NCLASS) return;
    /* Round 5: forced-full board ORRP (us): dots 2141, orbits 4306,
     * planets 2668, text 1712, controls 426, other 1877. Explicit stream
     * prices reflect that sample. Scrub-2 astronomy/projection prices live in
     * face_orrery.c and are fitted to the supplied 77ac23a board stages. */
    static const double raster_scale[7]={1.0,4.91,9.32,3.10,1.05,3.50,3.51};
    double base=px*CLASS_NS[cls]+calls*CALL_NS;
    /* Capture ratios: board pan tuple *30/16, divided by the old per-build
     * sim totals. Trails were not supplied; retain their original estimate. */
    static const double capture_scale[7]={1.55,3.0,1.7,13.2,3.0,1.0,11.2};
    double scale=cls==GFX_COST_ORR_CPU || cls==GFX_COST_ORR_PRE_CPU || cls==GFX_COST_ORR_POSTDIM ? 1.0 : s_orr_stage==0 && cls!=GFX_COST_ORR_RUN && cls!=GFX_COST_ORR_PIXEL && cls!=GFX_COST_ORR_LIT
                 ? raster_scale[s_orr_category] :
                 s_orr_stage==1 ? capture_scale[s_orr_category] : 1.0;
    double extra=base*(scale-1);
    /* 1.43-B October 16 profile, not a desktop timing multiplier:
     * opening orbit bucket 7437 us/tick vs the ecc74d9 replay's 1391;
     * controls peak 24250 vs 7487, of which 6496 was six uncached arcs.
     * Charge the actual construction / arc operations, not every moving
     * frame. Cached coverage keeps its existing price. A settled date drag
     * has neither operation; the supplied +10 orbit builds describes opening.
     * The later 77ac23a sample identifies the missing astronomy/projection
     * work; those operation counts are charged separately in face_orrery.c. */
    if(s_orr_rings && s_orr_stage==2 && s_orr_category==1)
        extra=base*(7437.0/1391.0-1);
    if(s_orr_rings && s_orr_stage==0 && s_orr_category==4 && cls==GFX_COST_ARC)
        extra=base*(1.05*(1+(24250.0-7487.0)/6496.0)-1);
    /* Round 6: the legacy retained-face dim pass called gfx_ink at EVERY
     * pixel, including black. It is not the engine's 8 ns packed-lane dim.
     * 160 ns/visit fits the supplied early iris under-layer ~53 ms residual;
     * this is an aggregate calibration, not an isolated kernel measurement.
     * Round 7: the round-6 sparse pass was also underpriced. ORR_POSTDIM is
     * 110 ns/visited pixel plus its existing 120 ns/lit pixel: ~27 ms over
     * normal raster, fitting u=40-44 ms incl. scratch saves. This aggregate
     * includes cache/call overhead, not just the ~17 ms isolated dim kernel.
     * Only the reproducible round-6 baseline emits it; ink replay emits none. */
    if(s_orr_stage==0 && cls==GFX_COST_DIM) extra=px*(160.0-CLASS_NS[cls]);
    s_cls_px[s_orr_stage>=1?GFX_COST_ORR_PRE_CPU:GFX_COST_ORR_CPU]+=(uint64_t)(extra+0.5);
    if(s_orr_stage>=0) s_orr_ns[s_orr_stage][s_orr_category] += base+extra;
    s_cls_px[cls] += px;
    s_cls_calls[cls] += (unsigned)calls;
    if (s_target_psram && px && cls < GFX_COST_DIM) {
        s_cls_px[GFX_COST_PSRAM_WRITE]+=px;
        /* Filled discs/pills count their AA rim exactly in gfx.c. Other AA
         * classes conservatively count every covered pixel as RMW. */
        if(cls!=GFX_COST_FILL && cls!=GFX_COST_BLIT && cls!=GFX_COST_CIRCLE && cls!=GFX_COST_PILL)
            s_cls_px[GFX_COST_PSRAM_RMW]+=px;
        s_psram_base_ns+=px*CLASS_NS[cls]+calls*CALL_NS;
    }
}

void gfx_cost_psram_edge(unsigned px)
{ if(s_cost_on && s_target_psram) s_cls_px[GFX_COST_PSRAM_RMW]+=px; }

typedef struct {
    uint32_t frame, dirty_px, rects, cmds, calls, record_us, raster_us;
    uint32_t blur[10];          /* per-frame H ran/skip/reduced, V ran/skip/reduced */
    int full, late;
    double raster_ms, bus_ms, frame_ms;
    double cls_ms[GFX_COST_NCLASS];
    char section[32], screen[24];
} fcost;

/* Effective model clock, not the host CPU frequency. Bus prices stay fixed. */
static int s_cpu_mhz = 180;
static int s_cpu_pace = 1;
static fcost s_last;                     /* last frame's estimate */
static fcost *s_hist; static int s_nhist, s_cap_hist;   /* scripted runs keep every frame */
static char s_section[32] = "-";

static void cost_estimate(fcost *c, uint32_t px)
{
    double ns = 0;
    const double cpu_scale = (double)NOS_CPU_REFERENCE_MHZ / s_cpu_mhz;
    uint64_t calls = 0;
    for (int k = 0; k < GFX_COST_NCLASS; k++) {
        c->cls_ms[k] = (s_cls_px[k] * CLASS_NS[k] / 1e6 + s_cls_calls[k] * CALL_NS / 1e6) * cpu_scale;
        ns += s_cls_px[k] * CLASS_NS[k];
        calls += s_cls_calls[k];
    }
    c->calls = (uint32_t)calls;
    c->raster_ms = (ns + calls * CALL_NS) / 1e6 * cpu_scale;
    c->bus_ms = px * 2.0 / BUS_BPS * 1000.0;
    /* Tick preparation and offscreen target rendering precede the first flush.
     * They cannot be hidden behind the stripe transfer pipeline. */
    double pre=c->cls_ms[GFX_COST_ORR_BUILD]+c->cls_ms[GFX_COST_ORR_CAPTURE]+
               c->cls_ms[GFX_COST_ORR_BUCKET]+c->cls_ms[GFX_COST_ORR_GEOMETRY]+c->cls_ms[GFX_COST_ORR_PRE_CPU]+c->cls_ms[GFX_COST_PSRAM_WRITE]+
               c->cls_ms[GFX_COST_PSRAM_RMW]+c->cls_ms[GFX_COST_ORR_MATH]+s_psram_base_ns/1e6*cpu_scale;
    c->frame_ms = px ? pre + fmax(c->raster_ms-pre+FIXED_MS,c->bus_ms)+PIPE_MS : FIXED_MS+c->raster_ms;
    double orr_raster=0;
    for(int k=0;k<7;k++)orr_raster+=s_orr_ns[0][k];
    /* Round-4 full repaint work floor 28.5 ms versus this model's 24.35 ms.
     * Reserve that measured engine/transfer tail, scaled by sent area. */
    if(orr_raster>0) {
        if(s_orr_rings && !nos_transitioning()) {
            /* a684695 scrub: NOSF raster (includes pipeline waits) 24.3 ms.
             * Full-glass QSPI is 23.35 ms; retain 0.95 ms pipeline/drain.
             * NOSF work includes transfer, unlike est_raster_ms (CPU only). */
            /* 0555b02 board: 39.9 work, 25.2 raster. Preserve the old
             * operation prices. +0.90 ms is the measured raster correction;
             * +1.94 ms is unassigned input/record/cache/engine overhead, NOT
             * attributed to astronomy or credited to the preview optimization.
             * Apply the same constants to archived and new open-ring frames. */
            c->frame_ms=pre+fmax(c->raster_ms-pre,c->bus_ms)+2.84+0.95*px/(GFX_W*GFX_H);
        } else c->frame_ms+=4.15*px/(GFX_W*GFX_H);
    }
    c->late = c->frame_ms > FRAME_MS;
}

/* ---- run state ----------------------------------------------------------------------- */
static int s_script_mode, s_no_media, s_abort, s_fail;
static int s_pixel_reference;
static char s_out[260] = "build/nos_out";
static FILE *s_csv, *s_logf, *s_hashf;
static int s_dump_frame = -1;
static uint32_t s_frames_run;           /* frames since boot (== nos_frame) */

/* captures */
typedef struct {
    char name[96];
    int left, total, per_frame_png, cols, div;
    uint16_t *frames;          /* film: total * GFX_W*GFX_H */
    fcost *costs;
    gif_w *gif;
    int gif_k;
} capture;
#define MAXCAP 8
static capture s_caps[MAXCAP];

static void ensure_dir(const char *path)
{
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp; *p; p++)
        if (*p == '/' || *p == '\\') { char c = *p; *p = 0; if (tmp[0]) CreateDirectoryA(tmp, NULL); *p = c; }
    CreateDirectoryA(tmp, NULL);
}
static void out_path(char *dst, int n, const char *name, const char *ext)
{
    snprintf(dst, (size_t)n, "%s/%s%s", s_out, name, ext);
}

static uint64_t frame_hash(const uint16_t *fb)
{
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++)
            if (sim_mask(x, y)) { h ^= fb[y * GFX_W + x]; h *= 1099511628211ull; }
    return h;
}

static void save_png565(const char *path, const uint16_t *fb)
{
    sim_img im = img_new(GFX_W, GFX_H, 0, 0, 0);
    img_from_565(&im, 0, 0, fb, 1);
    if (png_write(path, &im)) printf("wrote %s\n", path);
    img_free(&im);
}

static void film_finish(capture *c)
{
    int n = c->total - c->left;
    if (n <= 0) return;
    int cols = c->cols > 0 ? c->cols : (int)ceil(sqrt((double)n));
    int rows = (n + cols - 1) / cols, div = c->div > 0 ? c->div : 2;
    int tw = GFX_W / div, th = GFX_H / div, lab = 16, pad = 6;
    sim_img im = img_new(pad + cols * (tw + pad), pad + rows * (th + lab + pad), 8, 8, 10);
    for (int i = 0; i < n; i++) {
        int cx = pad + (i % cols) * (tw + pad), cy = pad + (i / cols) * (th + lab + pad);
        img_from_565_scaled(&im, cx, cy, c->frames + (size_t)i * GFX_W * GFX_H, 1, div);
        char t[64];
        const fcost *fc = &c->costs[i];
        snprintf(t, sizeof t, "%d f%u %.1fms", i, (unsigned)fc->frame, fc->frame_ms);
        img_text(&im, cx + 2, cy + th + 4, t, 1, fc->late ? 255 : 170, fc->late ? 70 : 170, fc->late ? 60 : 175);
        if (fc->dirty_px == 0) img_text(&im, cx + tw - 6 * 4 - 2, cy + th + 4, "idle", 1, 90, 90, 100);
    }
    char path[300];
    out_path(path, sizeof path, c->name, ".png");
    if (png_write(path, &im)) printf("wrote %s (%d frames)\n", path, n);
    img_free(&im);
    if (c->per_frame_png) {
        for (int i = 0; i < n; i++) {
            char nm[160];
            snprintf(nm, sizeof nm, "%s_%03d", c->name, i);
            out_path(path, sizeof path, nm, ".png");
            sim_img f = img_new(GFX_W, GFX_H, 0, 0, 0);
            img_from_565(&f, 0, 0, c->frames + (size_t)i * GFX_W * GFX_H, 1);
            png_write(path, &f);
            img_free(&f);
        }
        printf("wrote %d frame PNGs %s/%s_NNN.png\n", n, s_out, c->name);
    }
}
static void capture_close(capture *c)
{
    if (!c->total) return;
    if (c->gif) { gif_close(c->gif); printf("wrote %s/%s.gif (%d frames)\n", s_out, c->name, c->total - c->left); }
    else film_finish(c);
    free(c->frames); free(c->costs);
    memset(c, 0, sizeof *c);
}
static void capture_frame(void)
{
    for (int i = 0; i < MAXCAP; i++) {
        capture *c = &s_caps[i];
        if (!c->total || c->left <= 0) continue;
        int k = c->total - c->left;
        if (c->gif) {
            sim_img im = img_new(GFX_W, GFX_H, 0, 0, 0);
            img_from_565(&im, 0, 0, panel(), 1);
            gif_add(c->gif, &im, gif_delay_fps(c->gif_k++, NOS_FPS));
            img_free(&im);
        } else {
            memcpy(c->frames + (size_t)k * GFX_W * GFX_H, panel(), sizeof s_panel);
            c->costs[k] = s_last;
        }
        if (--c->left == 0) capture_close(c);
    }
}
static int captures_pending(void)
{
    for (int i = 0; i < MAXCAP; i++) if (s_caps[i].total && s_caps[i].left > 0) return 1;
    return 0;
}

/* ---- one frame ------------------------------------------------------------------------ */
static void (*s_on_frame)(void);        /* interactive: recorder / HUD history */

static uint32_t sim_frame(float elapsed_ms)
{
    extern void sim_imu_step(void);
    sim_imu_step();
    nos_ext_sim_tick(hal_millis());
    s_target_psram=0; s_psram_base_ns=0;
    memset(s_orr_ns,0,sizeof s_orr_ns);
    s_orr_stage=-1;
    memset(s_cls_px, 0, sizeof s_cls_px);
    memset(s_cls_calls, 0, sizeof s_cls_calls);
    s_flushed_px = 0; s_flushes = 0;
    int inv = nos_prefs.theme && !nos_aod;     /* the light theme: awake only, as the host does */
    if (inv != s_inverted) { s_inverted = inv; ui_theme_inverted(inv); }
    s_cost_on = 1;
    uint32_t px = nos_frame_run(elapsed_ms);
    s_cost_on = 0;
    fcost c;
    memset(&c, 0, sizeof c);
    c.frame = nos_frame;
    c.dirty_px = s_fullframe_mode ? px : s_flushed_px;
    c.rects = nos_last.rects; c.cmds = nos_last.cmds;
    c.record_us = nos_last.record_us; c.raster_us = nos_last.raster_us;
    c.full = nos_last.full;
    static uint32_t blur_prev[10];
    uint32_t blur_now[10] = { nos_mblur_count[0][1], nos_mblur_count[0][0], nos_mblur_reduced[0],
                            nos_mblur_count[1][1], nos_mblur_count[1][0], nos_mblur_reduced[1],
                            pblur_hits[0], pblur_misses[0], pblur_hits[1], pblur_misses[1] };
    for (int i = 0; i < 10; i++) { c.blur[i] = blur_now[i] - blur_prev[i]; blur_prev[i] = blur_now[i]; }
    if (!px) c.rects = 0;
    cost_estimate(&c, c.dirty_px);
#ifdef GFX_COST_MODEL
    /* The next frame's blur gate must see watch CPU time, never host wall time.
     * Keep c.raster_us above as the actual PC profiling field in frames.csv. */
    nos_last.raster_us = (uint32_t)(c.raster_ms * 1000.0);
    nos_last.wait_us = 0;
    nos_last.record_us = c.cmds * 2u; /* modeled recorder CPU, not nondeterministic PC time */
#endif
    snprintf(c.section, sizeof c.section, "%s", s_section);
    const nos_screen *top = nos_top();
    snprintf(c.screen, sizeof c.screen, "%s", top && top->name ? top->name : "?");
    s_last = c;
    s_frames_run++;
    sim_motion_frame(c.frame, s_section);
    if (s_script_mode) {
        if (s_nhist == s_cap_hist) { s_cap_hist = s_cap_hist ? s_cap_hist * 2 : 1024; s_hist = realloc(s_hist, sizeof *s_hist * s_cap_hist); }
        s_hist[s_nhist++] = c;
        if (s_csv) {
            fprintf(s_csv, "%u,%u,%u,%u,%.3f,%.3f,%.3f,%d,%u,%d,%s,%s,%u,%u",
                    (unsigned)c.frame, (unsigned)c.dirty_px, (unsigned)c.rects, (unsigned)c.cmds,
                    c.raster_ms, c.bus_ms, c.frame_ms, c.late, (unsigned)c.calls, c.full,
                    c.section, c.screen, (unsigned)c.record_us, (unsigned)c.raster_us);
            for (int k = 0; k < GFX_COST_NCLASS; k++) fprintf(s_csv, ",%.3f", c.cls_ms[k]);
            for(int st=0;st<3;st++) for(int k=0;k<7;k++) fprintf(s_csv,",%.3f",s_orr_ns[st][k]/1e6);
            fprintf(s_csv, ",%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%u,%u", nos_last.tick_us,
                    nos_last.history_us, nos_last.history_pixels, nos_last.history_blended,
                    nos_last.layer_bands, nos_last.layer_reused, nos_last.cache_bytes, nos_last.allocation_failures,
                    nos_last.history_uniform_pixels,nos_history_uniform,
                    nos_last.history_uniform_read_bytes,nos_last.history_uniform_write_bytes);
            fputc('\n', s_csv);
        }
        if (s_hashf) fprintf(s_hashf, "%u %016llx\n", (unsigned)c.frame, (unsigned long long)frame_hash(panel()));
        if ((int)c.frame == s_dump_frame) {
            char path[300];
            snprintf(path, sizeof path, "%s/frame_%d.bin", s_out, s_dump_frame);
            FILE *f = fopen(path, "wb");
            if (f) { fwrite(panel(), sizeof s_panel, 1, f); fclose(f); }
            snprintf(path, sizeof path, "%s/frame_%d.png", s_out, s_dump_frame);
            save_png565(path, panel());
            s_abort = 1;
        }
    }
    if (!s_no_media) capture_frame();
    if (s_on_frame) s_on_frame();
    /* Host persistence runs between frames; the watch owns an SD worker task.
     * This keeps scripted state changes deterministic and excludes filesystem
     * latency from the raster cost model. */
    workout_storage_service();
    return px;
}

/* ---- scripted runs (virtual clock) ---------------------------------------------------- */
static void vstep(void)
{
    sim_vclock_ms += 1000.0 / NOS_FPS;
    sim_frame(NOS_PERIOD_MS);
}
static void run_frames(int n) { while (n-- > 0 && !s_abort) vstep(); }
static int ms_frames(double ms) { int n = (int)floor(ms / (1000.0 / NOS_FPS) + 0.5); return n < 1 && ms > 0 ? 1 : n; }

/* Touch samples at 60 Hz: two per frame, at the frame's midpoint and its end. */
static void touch_at(double t_ms_in_frame, int x, int y, int down)
{
    double base = sim_vclock_ms;
    sim_vclock_ms = base + t_ms_in_frame;
    nos_touch(x, y, down);
    sim_vclock_ms = base;
}
static double ease_inout(double t) { return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2; }
static double ease_in(double t) { return t * t; }
static void fail(const char *,const char *,const char *);

/* Firmware inj_poll: one linear, integer-truncated sample per loop. The
 * ordinary simulator drag is eased and delivers two samples per frame.
 * Optional period replays the measured board cadence without device I/O. */
static void board_swipe(int x0,int y0,int x1,int y1,int ms,double period)
{
    if(ms<=0 || period<1 || period>1000) { fail("invalid board_swipe%s%s","","");return; }
    for(double elapsed=0;elapsed<ms;elapsed+=period) {
        int t=(int)elapsed;
        nos_touch(x0+(x1-x0)*t/ms,y0+(y1-y0)*t/ms,1);
        sim_vclock_ms+=period;sim_frame((float)period);
    }
    nos_touch(x1,y1,0);
    sim_vclock_ms+=period;sim_frame((float)period);
}

static void gesture(int x0, int y0, int x1, int y1, double ms, int flick)
{
    const double P = 1000.0 / NOS_FPS;
    touch_at(0, x0, y0, 1);
    vstep();
    int n = ms_frames(ms);
    if (n < 1) n = 1;
    for (int k = 1; k <= n && !s_abort; k++) {
        for (int h = 1; h <= 2; h++) {
            double t = (k - 1 + h * 0.5) / n;
            double e = flick ? ease_in(t) : ease_inout(t);
            touch_at(P * h * 0.5, (int)lround(x0 + (x1 - x0) * e), (int)lround(y0 + (y1 - y0) * e), 1);
        }
        vstep();
    }
    touch_at(P * 0.5, x1, y1, 0);
    vstep();
}
/* slide: the finger moves at constant speed, stops dead, holds still for hold_ms (still
 * sampled at 60 Hz), then lifts: the worst case for drag prediction (a sudden stop). */
static void slide(int x0, int y0, int x1, int y1, double ms, double hold_ms)
{
    const double P = 1000.0 / NOS_FPS;
    touch_at(0, x0, y0, 1);
    vstep();
    int n = ms_frames(ms), nh = ms_frames(hold_ms);
    if (n < 1) n = 1;
    for (int k = 1; k <= n + nh && !s_abort; k++) {
        for (int h = 1; h <= 2; h++) {
            double t = (k - 1 + h * 0.5) / n;
            if (t > 1) t = 1;
            touch_at(P * h * 0.5, (int)lround(x0 + (x1 - x0) * t), (int)lround(y0 + (y1 - y0) * t), 1);
        }
        vstep();
    }
    touch_at(P * 0.5, x1, y1, 0);
    vstep();
}
static void button_hold(int frames_down)
{
    sim_button_down = 1; nos_button(1);
    run_frames(frames_down);
    sim_button_down = 0; nos_button(0);
}

static void logline(const char *s)
{
    if (!sim_log_quiet || s_fail) printf("[f%u] %s\n", (unsigned)nos_frame, s);
    if (s_logf) fprintf(s_logf, "%u %s\n", (unsigned)nos_frame, s);
}
static void fail(const char *fmt, const char *a, const char *b)
{
    char t[300];
    snprintf(t, sizeof t, fmt, a, b);
    logline(t);
    s_fail = 1;
}

static int air_code(const char *s)
{
    static const char *n[] = { "none", "up", "down", "left", "right", "near", "far" };
    for (int i = 0; i < 7; i++) if (!_stricmp(s, n[i])) return i;
    return atoi(s);
}

static int section_late(void)
{
    int late = 0;
    for (int i = s_nhist - 1; i >= 0 && !strcmp(s_hist[i].section, s_section); i--) late += s_hist[i].late;
    return late;
}

static int s_workout_result;
static const char *s_workout_number_keys[] = {
    "state", "sport", "profile", "elapsed", "total", "distance", "steps", "hr_ms", "gps_ms",
    "bpm", "avg_bpm", "peak_bpm", "speed", "avg_speed", "pace", "avg_pace", "cadence",
    "hr_valid", "distance_valid", "speed_valid", "pace_valid", "cadence_valid", "gps_status",
    "save", "recovered", "id", "started", "ended", "history", "storage_ready", "storage_dirty",
    "storage_busy", "storage_available", "storage_errors", "storage_corrupt", "storage_foreign", "nlaps", "result",
    "motion_ms", "movement_ms", "bursts", "wrist", "dominant", "playing_hand", "bowling",
    "autoresume", "recoveries", "mass", "met", "kcal", "placement"
};
static int workout_number(const workout_snapshot *w, const char *field, double *out)
{
    workout_storage_status st; workout_storage_get(&st);
    const double values[] = {
        w->state, w->sport, w->sport, w->elapsed_ms, w->total_ms, w->distance_m, w->steps,
        w->hr_active_ms, w->gps_active_ms, w->bpm, w->avg_bpm, w->peak_bpm,
        w->speed_mps, w->avg_speed_mps, w->pace_s_km, w->avg_pace_s_km, w->cadence_spm,
        w->hr_valid, w->distance_valid, w->speed_valid, w->pace_valid, w->cadence_valid, w->gps_status,
        w->save_state, w->recovered, w->id, w->started_epoch, w->ended_epoch,
        workout_history_count(), st.ready, st.dirty, st.busy, st.available, st.errors, st.corrupt, st.foreign,
        w->nlaps, s_workout_result,
        w->motion_valid_ms, w->movement_ms, w->movement_bursts, w->setup.worn_wrist,
        w->setup.dominant_hand, w->setup.playing_hand, w->setup.bowling_hand,
        w->setup.auto_resume, w->recovery_count, w->setup.mass_kg, w->met_tenths/10.0,
        w->energy_kcal_est, w->setup.placement_source
    };
    for (unsigned i = 0; i < sizeof values / sizeof values[0]; i++)
        if (!strcmp(field, s_workout_number_keys[i]) || (!strcmp(field, "laps") && !strcmp(s_workout_number_keys[i], "nlaps"))) {
            *out = values[i]; return 1;
        }
    return 0;
}
static int workout_append(char *line,size_t capacity,int used,const char *format,...)
{
    if(used<0 || (size_t)used>=capacity) { fail("workout JSON overflow%s%s","",""); return -1; }
    va_list args; va_start(args,format);
    int n=vsnprintf(line+used,capacity-(size_t)used,format,args); va_end(args);
    if(n<0 || (size_t)n>=capacity-(size_t)used) { fail("workout JSON overflow%s%s","",""); return -1; }
    return used+n;
}
static void workout_info(const char *label, const workout_snapshot *w)
{
    char line[4096];
    int n = snprintf(line, sizeof line, "WORKOUT %s {", *label ? label : "snapshot");
    for (unsigned i = 0; i < sizeof s_workout_number_keys / sizeof s_workout_number_keys[0]; i++) {
        double value = 0; workout_number(w, s_workout_number_keys[i], &value);
        n = workout_append(line,sizeof line,n, "%s\"%s\":%.6f", i ? "," : "", s_workout_number_keys[i], value);
    }
    n = workout_append(line,sizeof line,n, ",\"laps\":[");
    for (unsigned i = 0; i < w->nlaps && i < WORKOUT_LAPS_MAX; i++)
        n = workout_append(line,sizeof line,n, "%s{\"elapsed\":%u,\"distance\":%.6f}",
                      i ? "," : "", (unsigned)w->laps[i].elapsed_ms, (double)w->laps[i].distance_m);
    n=workout_append(line,sizeof line,n,"]}");
    if(n<0)return;
    /* Structured observations remain visible with --quiet for test runners. */
    puts(line);
    if (s_logf) fprintf(s_logf, "%u %s\n", (unsigned)nos_frame, line);
}
static void workout_name_normalize(const char *source,char *out,size_t capacity)
{
    size_t used=0;
    while(*source && used+1<capacity) {
        unsigned char ch=(unsigned char)*source++;
        if(isalnum(ch)) out[used++]=(char)tolower(ch);
    }
    out[used]=0;
}
static int workout_sport_by_name(const char *name)
{
    static const char *names[] = { "walk", "run", "cycle", "hike", "treadmill", "workout" };
    for (unsigned i=0;i<sizeof names/sizeof names[0];i++) if (!_stricmp(name,names[i])) return (int)i;
    if (!_stricmp(name,"general") || !_stricmp(name,"indoor")) return WORKOUT_GENERAL;
    errno=0; char *end=NULL; long number=strtol(name,&end,10);
    if(name!=end && *end==0 && !errno && number>=0 && number<WORKOUT_SPORT_COUNT) return (int)number;
    char normalized[96],candidate[96]; workout_name_normalize(name,normalized,sizeof normalized);
    if(!*normalized)return -1;
    for(int i=0;i<WORKOUT_SPORT_COUNT;i++) {
        const workout_profile *profile=workout_profile_get(i);
        if(profile && profile->name) {
            workout_name_normalize(profile->name,candidate,sizeof candidate);
            if(!strcmp(normalized,candidate))return i;
        }
    }
    return -1;
}

static int set_key(const char *key, const char *val, const char *rest)
{
    if (!_stricmp(key, "time")) { int h = 0, m = 0, s = 0; sscanf(val, "%d:%d:%d", &h, &m, &s); sim_rtc_set_time(h, m, s); }
    else if (!_stricmp(key, "date")) { int y = 2026, mo = 1, d = 1; sscanf(val, "%d-%d-%d", &y, &mo, &d); sim_rtc_set_date(y, mo, d); }
    else if (!_stricmp(key, "tz")) sim_tz_min = atoi(val);
    else if (!_stricmp(key, "battery")) sim_batt_pct = atoi(val);
    else if (!_stricmp(key, "charging")) sim_charging = atoi(val);
    else if (!_stricmp(key, "steps")) sim_steps = atoi(val) - (int)(hal_millis() / 1500);
    else if (!_stricmp(key, "present")) hal_ext_sim_present = (uint32_t)strtoul(val, 0, 0);
    else if (!_stricmp(key, "full")) nos_debug_force_full = atoi(val);
    else if (!_stricmp(key, "layer_cache")) { nos_layer_cache_enabled = atoi(val); nos_invalidate(); }
    else if (!_stricmp(key,"prog_geometry")) {
        if (strcmp(val,"0") && strcmp(val,"1")) return 0;
        prog_geometry_enabled=val[0]=='1';
        if(!prog_geometry_enabled)prog_geometry_release();
        nos_invalidate();
    }
    else if (!_stricmp(key,"history_cadence")) nos_history_cadence=atoi(val)!=0;
    else if (!_stricmp(key,"history_uniform")) nos_history_uniform=atoi(val)!=0;
    else if (!_stricmp(key,"history_virtual")) {
        if(strcmp(val,"0") && strcmp(val,"1"))return 0;
        /* Keep set's exact-argument contract: reject trailing tokens too. */
        if(strcmp(rest,"0") && strcmp(rest,"1"))return 0;
        nos_history_virtual_set(val[0]=='1');
        const nos_history_virtual_stats *v=nos_history_virtual_get_stats();
        printf("NOSHISTORYV v=1 enabled=%d virtual_pixels=%llu avoided_bytes=%llu materialize_bytes=%llu classify_pixels=%llu\n",
            nos_history_virtual_get(),(unsigned long long)v->virtual_pixels,(unsigned long long)v->avoided_bytes,
            (unsigned long long)v->materialize_bytes,(unsigned long long)v->classify_pixels);
    }
    else if (!_stricmp(key,"history_native_copy")) {
        if (strcmp(val,"0") && strcmp(val,"1")) return 0;
        nos_history_native_copy=val[0]=='1';
        printf("NOSHISTORY v=1 native_copy=%d\n",nos_history_native_copy);
    }
    else if (!_stricmp(key, "damage_bands")) {
        if (strcmp(val, "0") && strcmp(val, "1")) return 0;
        nos_damage_bands_set(val[0] == '1');
    }
    else if (!_stricmp(key,"settings_carry")) {
        if (strcmp(val,"0") && strcmp(val,"1")) return 0;
        set_settings_carry_set(val[0]=='1');
        set_settings_carry_report();
    }
    else if (!_stricmp(key, "settings_shutter")) {
        if (strcmp(val, "0") && strcmp(val, "1")) return 0;
        set_settings_shutter_set(val[0] == '1');
    }
    else if (!_stricmp(key, "native_rom")) {
        if (strcmp(val, "0") && strcmp(val, "1")) return 0;
        extern void sys_launcher_cache_reset(void),set_settings_cache_release(void);
        set_settings_cache_release();sys_launcher_cache_reset();
        nos_native_menu_rom_set(val[0] == '1');
        nos_layers_invalidate();nos_history_reset();nos_invalidate();
        printf("NOSNATIVE v=1 rom=%d compatible=%d asset_bytes=%u source_sha256=%s\n",
               nos_native_menu_rom_enabled,native_menu_assets_compatible(),
               (unsigned)native_menu_asset_bytes(),native_menu_source_sha256());
    }
    else if (!_stricmp(key, "card_pipeline")) orrery_card_progressive = atoi(val) != 0;
    else if (!_stricmp(key, "card_proxy")) orrery_card_motion_proxy = atoi(val) != 0;
    else if (!_stricmp(key, "card_detail")) {
        int detail = atoi(val);
        if (detail != 2 && detail != 4) return 0;
        orrery_card_motion_step = (unsigned)detail;
    }
    else if (!_stricmp(key, "card_quanta")) {
        int count = atoi(val);
        if (count < 1 || count > 512) return 0;
        orrery_card_render_quanta = (unsigned)count;
    }
    else if (!_stricmp(key, "temporal_blur")) { nos_temporal_blur = atoi(val); nos_invalidate(); }
    else if (!_stricmp(key, "postdim")) { extern int nos_debug_post_dim; nos_debug_post_dim = atoi(val); }
    else if (!_stricmp(key, "theme")) { nos_prefs.theme = (uint8_t)atoi(val); }   /* 0 dark, 1 light */
    else if (!_stricmp(key, "units_metric")) {
        if (strcmp(val, "0") && strcmp(val, "1")) return 0;
        nos_prefs.units_metric = (uint8_t)atoi(val); nos_invalidate();
    }
    else if (!_stricmp(key, "aod")) { nos_aod = atoi(val); if (!nos_aod) nos_keep_awake(); }   /* the host wakes like this */
    else if (!_stricmp(key, "selftest_bug")) { extern int nos_selftest_bug; nos_selftest_bug = atoi(val); }
    else if (!_stricmp(key, "predict")) nos_touch_predict = atoi(val);
    else if (!_stricmp(key, "selftest_spin")) { extern float nos_selftest_spin; nos_selftest_spin = (float)atof(val); }
    else if (!_stricmp(key, "selftest_smear")) { extern int nos_selftest_smear; nos_selftest_smear = atoi(val); }
    else if (!_stricmp(key, "ripple_blur")) ui_ripple_blur = atoi(val);   /* 0 off, 1 tails, 2 tails + burst */
    else if (!_stricmp(key, "orrprof")) { extern void orrery_profile(int); orrery_profile(atoi(val)); }
    else if (!_stricmp(key, "iris_blur")) nos_iris_blur = atoi(val);      /* the transition edge's motion blur */
    else if (!_stricmp(key, "mblur")) nos_mblur = atoi(val);
    else if (!_stricmp(key, "sd")) { extern int sim_sd_present; sim_sd_present = atoi(val); }              /* whole-screen moves: 0 off, 1 on, 2 + drags */
    else if (!_stricmp(key, "tx")) {
        /* set tx push|rise|drop open|close|both default|legacy|steady|axis|cut */
        char g[16] = "", w[16] = "", v[16] = "";
        sscanf(rest, "%15s %15s %15s", g, w, v);
        static const char *gn[] = { "", "push", "rise", "drop" }, *vn[] = { "default", "legacy", "steady", "axis", "cut" };
        int gi = -1, vi = -1;
        for (int i = 1; i < 4; i++) if (!_stricmp(g, gn[i])) gi = i;
        for (int i = 0; i < 5; i++) if (!_stricmp(v, vn[i])) vi = i;
        if (gi < 0 || vi < 0 || (_stricmp(w, "open") && _stricmp(w, "close") && _stricmp(w, "both"))) return 0;
        if (_stricmp(w, "close")) nos_debug_tx_variant(gi, 1, vi);
        if (_stricmp(w, "open")) nos_debug_tx_variant(gi, 0, vi);
    }
    else if (!_stricmp(key, "imu")) {
        if (!_stricmp(val, "off")) sim_imu_override = 0;
        else { sim_imu_override = 1; sscanf(rest, "%f %f %f", &sim_imu_a[0], &sim_imu_a[1], &sim_imu_a[2]); }
    } else return nos_ext_sim_set(key, rest);
    return 1;
}

/* Rest of the line after n tokens. */
static const char *rest_after(const char *line, int n)
{
    const char *p = line;
    for (int i = 0; i < n; i++) {
        while (*p && isspace((unsigned char)*p)) p++;
        while (*p && !isspace((unsigned char)*p)) p++;
    }
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int start_capture(const char *name, int n, int gif, int per_frame, int cols, int div)
{
    if (s_no_media || n <= 0) return 1;
    for (int i = 0; i < MAXCAP; i++) {
        capture *c = &s_caps[i];
        if (c->total) continue;
        memset(c, 0, sizeof *c);
        snprintf(c->name, sizeof c->name, "%s", name);
        c->total = c->left = n;
        c->per_frame_png = per_frame; c->cols = cols; c->div = div;
        if (gif) {
            char path[300];
            out_path(path, sizeof path, name, ".gif");
            c->gif = gif_open(path, GFX_W, GFX_H);
            if (!c->gif) { c->total = 0; return 0; }
        } else {
            c->frames = malloc((size_t)n * sizeof s_panel);
            c->costs = calloc((size_t)n, sizeof *c->costs);
            if (!c->frames || !c->costs) { free(c->frames); free(c->costs); c->total = 0; return 0; }
        }
        return 1;
    }
    return 0;
}

static int run_script(const char *path, int depth)
{
    FILE *f = fopen(path, "r");
    if (!f) { fail("cannot open script %s%s", path, ""); s_abort = 1; return 0; }
    char line[512];
    int ln = 0;
    while (!s_abort && fgets(line, sizeof line, f)) {
        ln++;
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = 0;
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        char cmd[32] = "", a[128] = "", b[128] = "", c3[32] = "", d[32] = "", e[32] = "";
        sscanf(p, "%31s %127s %127s %31s %31s %31s", cmd, a, b, c3, d, e);
        double ms = 0;
        if (!strcmp(cmd, "wait")) {
            ms = atof(a); if (strchr(a, 's') && !strstr(a, "ms")) ms *= 1000;
            run_frames(ms_frames(ms));
        }
        else if (!strcmp(cmd, "frames")) run_frames(atoi(a));
        else if (!strcmp(cmd,"board_swipe")) {
            double period=1000.0/NOS_FPS;
            sscanf(p,"%*s %*s %*s %*s %*s %*s %lf",&period);
            board_swipe(atoi(a),atoi(b),atoi(c3),atoi(d),atoi(e),period);
        }
        else if (!strcmp(cmd, "tap")) {
            int x = atoi(a), y = atoi(b);
            touch_at(0, x, y, 1); run_frames(2);
            touch_at(0, x, y, 0); run_frames(1);
        }
        else if (!strcmp(cmd, "hold")) {
            int x = atoi(a), y = atoi(b);
            touch_at(0, x, y, 1); run_frames(ms_frames(*c3 ? atof(c3) : 700));
            touch_at(0, x, y, 0); run_frames(1);
        }
        else if (!strcmp(cmd, "touch")) nos_touch(atoi(a), atoi(b), atoi(c3));
        else if (!strcmp(cmd, "drag") || !strcmp(cmd, "flick") || !strcmp(cmd, "swipe"))
            gesture(atoi(a), atoi(b), atoi(c3), atoi(d), *e ? atof(e) : (strcmp(cmd, "drag") ? 120 : 400), strcmp(cmd, "drag") != 0);
        else if (!strcmp(cmd, "slide")) {
            char f6[32] = "";
            sscanf(p, "%*s %*s %*s %*s %*s %*s %31s", f6);
            slide(atoi(a), atoi(b), atoi(c3), atoi(d), *e ? atof(e) : 300, *f6 ? atof(f6) : 200);
        }
        else if (!strcmp(cmd, "press")) { button_hold(3); run_frames(1); }
        else if (!strcmp(cmd, "knock")) { extern uint32_t nos_sim_knocks; nos_sim_knocks++; run_frames(1); }   /* a double knock */
        else if (!strcmp(cmd, "longpress")) { button_hold(ms_frames(*a ? atof(a) : 800)); run_frames(1); }
        else if (!strcmp(cmd, "doublepress")) { button_hold(3); run_frames(3); button_hold(3); run_frames(1); }
        else if (!strcmp(cmd, "button")) { sim_button_down = atoi(a); nos_button(sim_button_down); }
        else if (!strcmp(cmd, "air")) { nos_air(air_code(a)); run_frames(1); }
        else if (!strcmp(cmd, "shot")) {
            if (!s_no_media) { char op[300]; out_path(op, sizeof op, a, ".png"); save_png565(op, panel()); }
        }
        else if (!strcmp(cmd, "film") || !strcmp(cmd, "film!")) {
            if (!start_capture(a, atoi(b), 0, cmd[4] == '!', atoi(c3), atoi(d))) fail("line %s: film %s failed", "", a);
        }
        else if (!strcmp(cmd, "gif")) {
            if (!start_capture(a, atoi(b), 1, 0, 0, 0)) fail("gif %s%s failed", a, "");
        }
        else if (!strcmp(cmd, "time")) { int h = 0, m = 0, s = 0; sscanf(a, "%d:%d:%d", &h, &m, &s); sim_rtc_set_time(h, m, s); }
        else if (!strcmp(cmd, "date")) set_key("date", a, a);
        else if (!strcmp(cmd, "set")) {
            if (!set_key(a, b, rest_after(p, 2))) fail("unknown set key '%s'%s", a, "");
        }
        else if (!strcmp(cmd, "workout")) {
            int expected = 1, known = 1;
            if (!strcmp(a, "info")) { workout_info(b, workout_get()); continue; }
            if (!strcmp(a, "history")) {
                workout_snapshot w;
                if (!workout_history_get(atoi(b), &w)) fail("workout history missing index %s%s", b, "");
                else workout_info(c3, &w);
                continue;
            }
            if (!strcmp(a, "start")) {
                int sport = workout_sport_by_name(b);
                if (sport < 0) { fail("unknown workout sport %s%s", b, ""); continue; }
                s_workout_result = workout_start(sport);
                if (*c3) expected = atoi(c3);
            } else {
                if (*b) expected = atoi(b);
                if (!strcmp(a, "pause")) s_workout_result = workout_pause();
                else if (!strcmp(a, "resume")) s_workout_result = workout_resume();
                else if (!strcmp(a, "lap")) s_workout_result = workout_lap_add();
                else if (!strcmp(a, "finish")) s_workout_result = workout_finish();
                else if (!strcmp(a, "discard")) s_workout_result = workout_discard();
                else if (!strcmp(a, "retry")) { workout_retry_save(); s_workout_result = 1; }
                else if (!strcmp(a, "service")) { workout_storage_service(); workout_tick(); s_workout_result = 1; }
                else { known = 0; fail("unknown workout command %s%s", a, ""); }
            }
            if (known && s_workout_result != expected) {
                char why[100]; snprintf(why, sizeof why, "%s returned %d, expected %d", a, s_workout_result, expected);
                fail("workout action %s%s", why, "");
            }
        }
        else if (!strcmp(cmd,"network")) {
            extern void set_connectivity_open(int bt);
            set_connectivity_open(!strcmp(a,"ble"));
        }
        else if (!strcmp(cmd, "open") || !strcmp(cmd, "open!")) {
            const nos_screen *sc = screen_by_name(a);
            if (!sc) fail("open: no screen named '%s'%s", a, "");
            else if (cmd[4] == '!') nos_push(sc, TX_NONE);
            else nos_open(sc, *b ? atoi(b) : GFX_W / 2, *c3 ? atoi(c3) : GFX_H / 2);
        }
        else if (!strcmp(cmd, "root")) {
            const nos_screen *sc = screen_by_name(a);
            if (!sc) fail("root: no screen named '%s'%s", a, ""); else nos_set_root(sc);
        }
        else if (!strcmp(cmd, "face")) { nos_prefs.face = (uint8_t)atoi(a); nos_invalidate(); }
        else if (!strcmp(cmd, "ringstyle")) {
            int style = atoi(a);
            if (style != 5) fail("ringstyle: only the final dock (5) exists, got '%s'%s", a, "");
        }
        else if (!strcmp(cmd, "card")) { orrery_card_debug_body(atoi(a)); nos_invalidate(); }
        else if (!strcmp(cmd, "card_work")) orrery_card_render_report();
        else if (!strcmp(cmd, "card_state")) orrery_card_report_state();
        else if (!strcmp(cmd, "ringplate")) {
            s_fail = 1; /* Report the removed control even under --quiet. */
            fail("ringplate A/B removed: final dock only%s%s", "", "");
        }
        else if (!strcmp(cmd, "skipboot")) skip_boot();
        else if (!strcmp(cmd, "home_now")) nos_home_now();          /* the host entering AOD */
        else if (!strcmp(cmd, "notify")) {
            /* notify APP|TITLE|BODY : a local notification plus its banner */
            char tb[256]; snprintf(tb, sizeof tb, "%s", rest_after(p, 1));
            char *f1 = strchr(tb, '|'), *f2 = f1 ? strchr(f1 + 1, '|') : 0;
            const char *app = tb, *title = "", *body = "";
            if (f1) { *f1 = 0; title = f1 + 1; }
            if (f2) { *f2 = 0; body = f2 + 1; }
            while (*title == ' ') title++;
            while (*body == ' ') body++;
            svc_notif_add_local(app, title, body);
            nos_banner(title, body, DOT_ICON_BELL, 0);
        }
        else if (!strcmp(cmd, "companion")) {
            /* companion APP TITLE : through the HAL queue (hal_notification_poll), like the BLE transport */
            const char *title = rest_after(p, 2);
            if (!sim_notify_push(a, *title ? title : "Notification")) fail("companion %s rejected (%s)", a, title);
        }
        else if (!strcmp(cmd, "phone")) svc_phone_inject(rest_after(p, 1));
        else if (!strcmp(cmd, "present")) hal_ext_sim_present = (uint32_t)strtoul(a, 0, 0);
        else if (!strcmp(cmd, "banner")) {
            char tb[160]; snprintf(tb, sizeof tb, "%s", rest_after(p, 1));
            char *bar = strchr(tb, '|'), *body = "";
            if (bar) { *bar = 0; body = bar + 1; while (*body == ' ') body++; }
            nos_banner(tb, body, 0, 0);
        }
        else if (!strcmp(cmd, "toast")) nos_toast(rest_after(p, 1));
        else if (!strcmp(cmd, "memprewarm")) { extern void dot_prebake_all(void); dot_prebake_all(); }
        else if (!strcmp(cmd, "memchunk")) nos_mem_max_request((uint32_t)strtoul(a, 0, 0));
        else if (!strcmp(cmd, "memfail")) nos_mem_fail_after(atoi(a));
        else if (!strcmp(cmd, "imu")) {
            extern int sim_imu_stall, sim_imu_fault;
            extern void sim_imu_step(void);
            if (!strcmp(a,"start")) { if (!hal_imu_record_start((unsigned)atoi(b))) fail("IMU start rejected%s%s","",""); }
            else if (!strcmp(a,"stop")) { if (!hal_imu_record_stop()) fail("IMU stop rejected%s%s","",""); }
            else if (!strcmp(a,"stall")) sim_imu_stall=atoi(b);
            else if (!strcmp(a,"fault")) sim_imu_fault=atoi(b);
            else if (!strcmp(a,"background")) { sim_vclock_ms+=atof(b); sim_imu_step(); }
            else if (!strcmp(a,"files")) hal_imu_files_request(0,0);
            else if (!strcmp(a,"browse")) hal_imu_files_request(atoi(b),atoi(c3));
            else if (!strcmp(a,"delete")) hal_imu_file_delete(atoi(b));
            else if (strcmp(a,"status")) fail("Unknown imu command %s%s",a,"");
            hal_imu_record r; hal_imu_record_get(&r);
            printf("IMU state=%d hz=%u samples=%u elapsed=%u bytes=%llu dropped=%u missed=%u errors=%u file=%s message=%s\n",
                r.state,r.hz,(unsigned)r.samples,(unsigned)r.elapsed_ms,(unsigned long long)r.bytes,
                (unsigned)r.dropped,(unsigned)r.missed,(unsigned)r.errors,r.name,r.message);
        }
        else if (!strcmp(cmd, "mem")) nos_mem_dump(0);
        else if (!strcmp(cmd, "home")) nos_home();
        else if (!strcmp(cmd, "pop")) nos_pop();
        else if (!strcmp(cmd, "invalidate")) nos_invalidate();
        else if (!strcmp(cmd, "log")) logline(rest_after(p, 1));
        else if (!strcmp(cmd, "mark")) { snprintf(s_section, sizeof s_section, "%s", a); logline(p); }
        else if (!strcmp(cmd, "motion")) {
            if (!_stricmp(a, "on")) sim_motion_on(1);
            else if (!_stricmp(a, "off")) sim_motion_on(0);
            else fail("motion: on or off, not '%s'%s", a, "");
        }
        else if (!strcmp(cmd, "expect")) {
            const nos_screen *t = nos_top();
            if (!strcmp(a, "top")) { if (!t || !t->name || strcmp(t->name, rest_after(p, 2))) fail("EXPECT FAIL top: got '%s', want '%s'", t && t->name ? t->name : "(none)", rest_after(p, 2)); }
            else if (!strcmp(a, "workout")) {
                double got = 0, lo = atof(c3), hi = *d ? atof(d) : lo;
                if (!workout_number(workout_get(), b, &got)) fail("unknown workout field %s%s", b, "");
                else if (!isfinite(got) || got < lo || got > hi) {
                    char why[160]; snprintf(why, sizeof why, "%s got %.6f want [%.6f,%.6f]", b, got, lo, hi);
                    fail("EXPECT FAIL workout %s%s", why, "");
                }
            }
            else if(!strcmp(a,"setup") || !strcmp(a,"wrist")) {
                double got=-1,lo=atof(c3),hi=*d?atof(d):lo; int known=1;
                if(!strcmp(a,"setup")) {
                    workout_setup setup; workout_setup_get(&setup);
                    if(!strcmp(b,"wrist"))got=setup.worn_wrist;
                    else if(!strcmp(b,"dominant"))got=setup.dominant_hand;
                    else if(!strcmp(b,"playing_hand"))got=setup.playing_hand;
                    else if(!strcmp(b,"bowling"))got=setup.bowling_hand;
                    else if(!strcmp(b,"autoresume"))got=setup.auto_resume;
                    else if(!strcmp(b,"mass"))got=setup.mass_kg;
                    else if(!strcmp(b,"placement"))got=setup.placement_source;
                    else known=0;
                } else {
                    wrist_detect_status wrist; wrist_detect_get(&wrist);
                    if(!strcmp(b,"state"))got=wrist.state;
                    else if(!strcmp(b,"reason"))got=wrist.reason;
                    else if(!strcmp(b,"candidate"))got=wrist.candidate;
                    else known=0;
                }
                if(!known || got<lo || got>hi) {
                    char why[160];snprintf(why,sizeof why,"%s.%s got %.6f want [%.6f,%.6f]",a,b,got,lo,hi);
                    fail("EXPECT FAIL setup/wrist %s%s",why,"");
                }
            }
            else if (!strcmp(a, "home_page") || !strcmp(a, "home_dragging")) {
                int page = 0, dragging = 0;
                sys_home_nav_snapshot(&page, 0, &dragging);
                int got = !strcmp(a, "home_page") ? page : dragging;
                if (got != atoi(b)) {
                    char msg[80]; snprintf(msg, sizeof msg, "%s got %d want %s", a, got, b);
                    fail("EXPECT FAIL %s%s", msg, "");
                }
            }
            else if (!strcmp(a,"chat")) {
                voice_chat_snapshot v;voice_chat_get(&v);double got=-1;int known=1;
                if(!strcmp(b,"state"))got=v.state;
                else if(!strcmp(b,"busy"))got=v.busy;
                else if(!strcmp(b,"volume"))got=v.volume;
                else if(!strcmp(b,"mic_gain"))got=v.mic_gain;
                else if(!strcmp(b,"speak"))got=v.speak;
                else if(!strcmp(b,"saved"))got=v.saved_count;
                else if(!strcmp(b,"turns"))got=v.turns;
                else if(!strcmp(b,"dirty"))got=v.storage_dirty;
                else if(!strcmp(b,"error"))got=v.storage_error;
                else if(!strcmp(b,"phase"))got=v.progress_phase;
                else known=0;
                double low=atof(c3),high=*d?atof(d):low;
                if(!known || got<low || got>high) {
                    char message[100];snprintf(message,sizeof message,"%s got %.0f want %.0f..%.0f",b,got,low,high);
                    fail("CHAT assertion %s%s",message,"");
                }
            }
            else if (!strcmp(a, "damage_bands")) {
                if (nos_damage_bands_get() != atoi(b)) fail("EXPECT FAIL damage_bands wanted %s%s", b, "");
            }
            else if (!strcmp(a, "home_offset") || !strcmp(a, "launcher_scroll") || !strcmp(a, "settings_scroll")) {
                float got = 0;
                if (!strcmp(a, "home_offset")) sys_home_nav_snapshot(0, &got, 0);
                else if (!strcmp(a, "settings_scroll")) got = set_settings_scroll_offset();
                else got = sys_launcher_page_scroll();
                if (!isfinite(got) || got < (float)atof(b) || got > (float)atof(c3)) {
                    char msg[128]; snprintf(msg, sizeof msg, "%s got %.3f want [%s,%s]", a, got, b, c3);
                    fail("EXPECT FAIL %s%s", msg, "");
                }
            }
            else if (!strcmp(a,"imu")) {
                hal_imu_record r; hal_imu_record_get(&r);
                hal_imu_file f; hal_imu_file_get(&f);
                unsigned got=0, want=(unsigned)atoi(c3);
                if (!strcmp(b,"state")) got=(unsigned)r.state;
                else if (!strcmp(b,"samples")) got=r.samples;
                else if (!strcmp(b,"dropped")) got=r.dropped;
                else if (!strcmp(b,"errors")) got=r.errors;
                else if (!strcmp(b,"file")) got=(unsigned)f.valid;
                else if (!strcmp(b,"id")) got=(unsigned)f.id;
                else if (!strcmp(b,"duration")) got=f.duration_ms;
                else { fail("Unknown IMU assertion %s%s",b,""); }
                if (got!=want) { char msg[80]; snprintf(msg,sizeof msg,"%s got %u want %u",b,got,want); fail("IMU assertion: %s%s",msg,""); }
            }
            else if (!strcmp(a, "memfailures")) {
                if (nos_mem_failures() != (unsigned)atoi(b)) fail("EXPECT FAIL memfailures wanted %s%s", b, "");
            }
            else if (!strcmp(a, "memowner")) {
                nos_mem_stat st; unsigned bytes = 0;
                for (unsigned i = 0; i < nos_mem_stats(&st, i); i++) if (!strcmp(st.owner, b)) bytes = st.bytes;
                if (bytes != (unsigned)atoi(c3)) fail("EXPECT FAIL memowner %s wanted %s", b, c3);
            }
            else if (!strcmp(a, "mempeak")) {
                if (nos_mem_peak() > (unsigned)atoi(b)) fail("EXPECT FAIL mempeak wanted <= %s%s", b, "");
            }
            else if (!strcmp(a, "depth")) { char g[16]; snprintf(g, sizeof g, "%d", nos_depth()); if (nos_depth() != atoi(b)) fail("EXPECT FAIL depth: got %s, want %s", g, b); }
            else if (!strcmp(a, "present")) {
                char g[16]; snprintf(g, sizeof g, "0x%X", (unsigned)hal_ext_present());
                if (hal_ext_present() != (uint32_t)strtoul(b, 0, 0)) fail("EXPECT FAIL present: got %s, want %s", g, b);
            }
            else if (!strcmp(a, "idle")) {
                /* An optional duration checks EVERY frame with the RTC advancing. */
                int n = ms_frames(atof(b)), sent = 0;
                if (!n) sent = s_last.dirty_px != 0;
                while (n-- > 0 && !s_abort) { vstep(); if (s_last.dirty_px) sent++; }
                if (sent && !s_pixel_reference) { char g[24]; snprintf(g, sizeof g, "%d", sent);
                    fail("EXPECT FAIL idle: %s frames sent pixels during interval%s", g, ""); }
            }
            else if (!strcmp(a, "maxdirty")) {
                unsigned limit = (unsigned)strtoul(b, 0, 10), peak = 0;
                int n = ms_frames(atof(c3));
                if (!n) peak = s_last.dirty_px;
                while (n-- > 0 && !s_abort) { vstep(); if (s_last.dirty_px > peak) peak = s_last.dirty_px; }
                if (peak > limit && !s_pixel_reference) { char g[24]; snprintf(g, sizeof g, "%u", peak);
                    fail("EXPECT FAIL maxdirty: peak %s exceeds %s pixels", g, b); }
            }
            else if (!strcmp(a, "nolate")) { char g[16]; snprintf(g, sizeof g, "%d", section_late()); if (section_late() && !s_pixel_reference) fail("EXPECT FAIL nolate: %s late frames in section %s", g, s_section); }
            else if (!strcmp(a, "blurbaked")) {
                unsigned wanted = (unsigned)strtoul(b, 0, 0);
                if ((pblur_baked_mask() & wanted) != wanted) fail("EXPECT FAIL blurbaked %s in section %s", b, s_section);
            }
            else if (!strcmp(a, "bluridle") || !strcmp(a, "blurlive")) {
                uint32_t ran = 0, attempted = 0;
                for (int i = 0; i < s_nhist; i++) if (!strcmp(s_hist[i].section, s_section)) {
                    ran += s_hist[i].blur[0] + s_hist[i].blur[3] + s_hist[i].blur[6] + s_hist[i].blur[8];
                    attempted += s_hist[i].blur[0] + s_hist[i].blur[1] + s_hist[i].blur[3] + s_hist[i].blur[4] + s_hist[i].blur[6] + s_hist[i].blur[7] + s_hist[i].blur[8] + s_hist[i].blur[9];
                }
                if ((!strcmp(a, "bluridle") && attempted) || (!strcmp(a, "blurlive") && !ran))
                    fail("EXPECT FAIL %s in section %s", a, s_section);
            }
            else if (!strcmp(a, "motionok")) {
                /* every finished move begun in this section met its class budget */
                char why[240];
                if (!sim_motion_enabled()) fail("EXPECT motionok needs 'motion on' (or --motion)%s%s", "", "");
                else if (sim_motion_bad(s_section, why, sizeof why)) fail("EXPECT FAIL motionok: %s%s", why, "");
            }
            else fail("unknown expect '%s'%s", a, "");
        }
        else if (!strcmp(cmd, "include")) {
            if (depth < 8) run_script(a, depth + 1);
            else { fail("script include depth exceeded: %s%s", a, ""); s_abort = 1; }
        }
        else if (!strcmp(cmd, "quit")) break;
        else {
            char t[64]; snprintf(t, sizeof t, "%d", ln);
            fail("script line %s: unknown command '%s'", t, cmd);
        }
    }
    if (ferror(f)) { fail("cannot read script %s%s", path, ""); s_abort = 1; }
    fclose(f);
    return !s_abort;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double pct(double *v, int n, double p) { if (!n) return 0; int i = (int)ceil(p / 100.0 * n) - 1; return v[i < 0 ? 0 : i >= n ? n - 1 : i]; }

static void summarize(FILE *o, const char *section)
{
    double *v = malloc(sizeof(double) * (s_nhist + 1)), *va = malloc(sizeof(double) * (s_nhist + 1));
    int n = 0, na = 0, late = 0;
    uint32_t blur[10] = {0};
    double cls[GFX_COST_NCLASS] = { 0 }, tot = 0, walk_max = 0, build_max = 0;
    for (int i = 0; i < s_nhist; i++) {
        const fcost *c = &s_hist[i];
        if (section && strcmp(c->section, section)) continue;
        v[n++] = c->frame_ms;
        if (c->dirty_px) va[na++] = c->frame_ms;
        late += c->late;
        for (int k = 0; k < 10; k++) blur[k] += c->blur[k];
        if (c->cls_ms[GFX_COST_ORR_WALK] > walk_max) walk_max = c->cls_ms[GFX_COST_ORR_WALK];
        double prep=c->cls_ms[GFX_COST_ORR_BUILD]+c->cls_ms[GFX_COST_ORR_CAPTURE]+
                    c->cls_ms[GFX_COST_ORR_BUCKET]+c->cls_ms[GFX_COST_ORR_GEOMETRY]+c->cls_ms[GFX_COST_ORR_PRE_CPU];
        if (prep > build_max) build_max = prep;
        for (int k = 0; k < GFX_COST_NCLASS; k++) { cls[k] += c->cls_ms[k]; tot += c->cls_ms[k]; }
    }
    qsort(v, n, sizeof *v, cmp_d);
    qsort(va, na, sizeof *va, cmp_d);
    fprintf(o, "%-14s frames %5d  sent %5d  est ms p50 %5.1f  p95 %5.1f  max %5.1f  | sending p50 %5.1f p95 %5.1f  | late %d (%.1f%%)\n",
            section ? section : "ALL", n, na, pct(v, n, 50), pct(v, n, 95), n ? v[n - 1] : 0,
            pct(va, na, 50), pct(va, na, 95), late, n ? 100.0 * late / n : 0);
    fprintf(o, "  live H ran/skip/reduced=%u/%u/%u V=%u/%u/%u\n",
            (unsigned)blur[0], (unsigned)blur[1], (unsigned)blur[2],
            (unsigned)blur[3], (unsigned)blur[4], (unsigned)blur[5]);
    fprintf(o, "  page cache H hit/miss=%u/%u V=%u/%u\n",
            (unsigned)blur[6], (unsigned)blur[7], (unsigned)blur[8], (unsigned)blur[9]);
    if (!section && tot > 0) {
        fprintf(o, "raster time by class:");
        for (int k = 0; k < GFX_COST_NCLASS; k++)
            if (cls[k] > 0.0005 * tot) fprintf(o, "  %s %.0f%%", CLASS_NAME[k], 100 * cls[k] / tot);
        fprintf(o, "\n");
    }
    if (!section && (walk_max || build_max))
        fprintf(o, "ORRERY scrub 3: a684695 open-ring work calibration (47.4 ms); software-double residual plus 24.3 ms raster/transfer floor. CPU-only est_raster_ms is not NOSF work. Legacy visits max %.3f ms; preparation max %.3f ms. New kernels remain predictions; see docs/ORRERY_SCRUB3.md.\n", walk_max, build_max);
    free(v); free(va);
}
static void write_summary(void)
{
    FILE *outs[2] = { sim_log_quiet ? 0 : stdout, 0 };   /* --quiet: file only */
    char path[300];
    out_path(path, sizeof path, "summary", ".txt");
    outs[1] = fopen(path, "w");
    for (int oi = 0; oi < 2; oi++) {
        FILE *o = outs[oi];
        if (!o) continue;
        fprintf(o, "== NOS known-work estimate (%d MHz CPU budget, %.1f ms target) ==\n", s_cpu_mhz, FRAME_MS);
        fprintf(o, "MODEL ONLY: temporal history, signatures and new cache kernels need device calibration. Zero estimated late frames is NOT deadline acceptance.\n");
        summarize(o, 0);
        fprintf(o, "MBLUR h ran=%u skipped=%u reduced=%u  v ran=%u skipped=%u reduced=%u  first=%u afford=%u\n",
                (unsigned)nos_mblur_count[0][1], (unsigned)nos_mblur_count[0][0], (unsigned)nos_mblur_reduced[0],
                (unsigned)nos_mblur_count[1][1], (unsigned)nos_mblur_count[1][0], (unsigned)nos_mblur_reduced[1],
                (unsigned)nos_mblur_gate[0], (unsigned)nos_mblur_gate[1]);
        fprintf(o, "PBLUR h=%u/%u v=%u/%u published=%u discarded=%u ready=%02x bytes=%u\n",
                pblur_hits[0], pblur_misses[0], pblur_hits[1], pblur_misses[1], pblur_published, pblur_discarded, pblur_ready_mask(), pblur_bytes());
        for (int k = 0; k < NOS_BS_N; k++)
            fprintf(o, "BLUR kind=%d moving=%u fast=%u blurred=%u sharp=%u\n", k,
                    (unsigned)nos_blur_stat[k][0], (unsigned)nos_blur_stat[k][1],
                    (unsigned)nos_blur_stat[k][2], (unsigned)nos_blur_stat[k][3]);
        char seen[32][32]; int ns = 0;
        for (int i = 0; i < s_nhist; i++) {
            int found = 0;
            for (int k = 0; k < ns; k++) if (!strcmp(seen[k], s_hist[i].section)) found = 1;
            if (!found && ns < 32) { snprintf(seen[ns], 32, "%s", s_hist[i].section); ns++; }
        }
        if (ns > 1) for (int k = 0; k < ns; k++) summarize(o, seen[k]);
        /* the five worst frames */
        int idx[5] = { -1, -1, -1, -1, -1 };
        for (int i = 0; i < s_nhist; i++)
            for (int k = 0; k < 5; k++)
                if (idx[k] < 0 || s_hist[i].frame_ms > s_hist[idx[k]].frame_ms ||
                    (s_hist[i].frame_ms == s_hist[idx[k]].frame_ms && s_hist[i].raster_ms > s_hist[idx[k]].raster_ms)) {
                    for (int j = 4; j > k; j--) idx[j] = idx[j - 1];
                    idx[k] = i;
                    break;
                }
        fprintf(o, "worst frames:\n");
        for (int k = 0; k < 5 && idx[k] >= 0; k++) {
            const fcost *c = &s_hist[idx[k]];
            int top = 0;
            for (int j = 1; j < GFX_COST_NCLASS; j++) if (c->cls_ms[j] > c->cls_ms[top]) top = j;
            fprintf(o, "  f%-5u %5.1f ms (raster %5.1f bus %5.1f) dirty %6u px rects %u calls %u  top %s %.1f ms  [%s/%s]\n",
                    (unsigned)c->frame, c->frame_ms, c->raster_ms, c->bus_ms, (unsigned)c->dirty_px,
                    (unsigned)c->rects, (unsigned)c->calls, CLASS_NAME[top], c->cls_ms[top], c->section, c->screen);
        }
        if (o != stdout) fclose(o);
    }
}

/* ---- boot -------------------------------------------------------------------------------- */
static int s_selftest, s_keep_boot, s_motion_flag, s_face = -1;
static char s_workout_store[260];
static const nos_screen *s_start_screen;
static void boot(void)
{
    if (!s_workout_store[0]) {
        if (s_script_mode) snprintf(s_workout_store, sizeof s_workout_store, "%s/workouts/run-%lu", s_out, (unsigned long)GetCurrentProcessId());
        else snprintf(s_workout_store, sizeof s_workout_store, "%s/workouts", s_out);
    }
    ensure_dir(s_workout_store);
    workout_storage_set_directory(s_workout_store);
    prefs_load();
    if (s_face >= 0) nos_prefs.face = (uint8_t)s_face;   /* --face N: start on that face */
    hal_ext_init();
    if (!s_fullframe_mode) {
        nos_set_stripes(s_stripes[0], s_stripes[1], STRIPE_ROWS, stripe_wait, flush_cb);
        nos_set_stripe_exposure(stripe_exposure_prepare, stripe_exposure_finish);
        nos_set_scratch(s_scratch);
    }
    nos_init(s_full);
    /* Model the immutable cache after background boot work completes. Its
     * retained PSRAM is charged even in scripts that never open a card. */
    orrery_shade_q16_prewarm();
    if (s_selftest) nos_set_root(&nos_selftest_root);
    else nos_apps_boot();
    if (!s_selftest) { workout_storage_service(); workout_tick(); }
    if (!s_selftest && s_start_screen) { skip_boot(); nos_open(s_start_screen, UI_CX, UI_CY); }
}

static int script_main(const char *script)
{
    s_script_mode = 1;
    sim_virtual = 1;
    sim_store_disabled = 1;
    ensure_dir(s_out);
    char path[300];
    out_path(path, sizeof path, "frames", ".csv");
    s_csv = fopen(path, "w");
    if (s_csv) {
        fprintf(s_csv, "frame,dirty_px,rects,cmds,est_raster_ms,est_bus_ms,est_frame_ms,late,calls,full,section,screen,pc_record_us,pc_raster_us");
        for (int k = 0; k < GFX_COST_NCLASS; k++) fprintf(s_csv, ",ms_%s", CLASS_NAME[k]);
        const char *oname[]={"dots","orbits","planets","text","controls","trails","other"};
        for(int st=0;st<3;st++) for(int k=0;k<7;k++) fprintf(s_csv,",orr_%d_%s",st,oname[k]);
        fprintf(s_csv, ",pc_tick_us,pc_history_us,history_pixels,history_blended,layer_bands,layer_reused,cache_bytes,allocation_failures,history_uniform_pixels,history_uniform_enabled,history_uniform_read_bytes,history_uniform_write_bytes");
        fputc('\n', s_csv);
    }
    out_path(path, sizeof path, "log", ".txt");
    s_logf = fopen(path, "w");
    sim_motion_dir(s_out);
    if (s_motion_flag) sim_motion_on(1);
    boot();
    if (!s_keep_boot) skip_boot();     /* scripts start on home unless --boot */
    run_frames(1);                    /* first frame: full paint of the root */
    run_script(script, 0);
    while (captures_pending() && !s_abort) vstep();   /* finish open films/gifs */
    for (int i = 0; i < MAXCAP; i++) capture_close(&s_caps[i]);
    if (s_csv) fclose(s_csv);
    if (s_logf) fclose(s_logf);
    if (s_hashf) fclose(s_hashf);
    if (s_dump_frame < 0) {
        write_summary();
        out_path(path, sizeof path, "summary", ".txt");
        FILE *sf = sim_motion_enabled() || s_motion_flag ? fopen(path, "a") : 0;
        sim_motion_finish(sf);
        if (sf) fclose(sf);
    }
    return s_fail ? 1 : 0;
}

/* ---- damage check: same script with damage tracking vs forced full repaints ------------ */
static int run_child(const char *args)
{
    char exe[MAX_PATH], cmd[2048];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    snprintf(cmd, sizeof cmd, "\"%s\" %s", exe, args);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}
static int read_hashes(const char *path, unsigned **fr, unsigned long long **h)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int n = 0, cap = 0;
    unsigned a; unsigned long long b;
    *fr = 0; *h = 0;
    while (fscanf(f, "%u %llx", &a, &b) == 2) {
        if (n == cap) { cap = cap ? cap * 2 : 1024; *fr = realloc(*fr, cap * sizeof **fr); *h = realloc(*h, cap * sizeof **h); }
        (*fr)[n] = a; (*h)[n] = b; n++;
    }
    fclose(f);
    return n;
}
static int load_raw(const char *path, uint16_t *dst)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int ok = fread(dst, sizeof s_panel, 1, f) == 1;
    fclose(f);
    return ok;
}
#ifdef GFX_COST_MODEL
#include "nos_quality_tape.h"
#endif
static int damagecheck_main(const char *script, const char *extra)
{
    char dd[300], df[300], args[2200], hp1[300], hp2[300];
    char tape[340], record[400]="",replay[400]="";
#ifdef GFX_COST_MODEL
    snprintf(tape,sizeof tape,"%s/quality.txt",s_out);
    snprintf(record,sizeof record,"--quality-record \"%s\"",tape);
    snprintf(replay,sizeof replay,"--quality-replay \"%s\"",tape);
    printf("  pixel reference: adaptive decisions recorded/replayed; idle/maxdirty/nolate work assertions excluded\n");
#else
    (void)tape;
#endif
    snprintf(dd, sizeof dd, "%s/damage", s_out);
    snprintf(df, sizeof df, "%s/full", s_out);
    ensure_dir(dd); ensure_dir(df);
    snprintf(hp1, sizeof hp1, "%s/hashes.txt", dd);
    snprintf(hp2, sizeof hp2, "%s/hashes.txt", df);
    printf("damagecheck: %s\n  run 1: damage tracking\n", script);
    snprintf(args, sizeof args, "--script \"%s\" --out \"%s\" --hashes \"%s\" --no-media --quiet --pixel-reference %s %s", script, dd, hp1, record, extra);
    int r1 = run_child(args);
    printf("  run 2: forced full repaint (nos_debug_force_full)\n");
    snprintf(args, sizeof args, "--script \"%s\" --out \"%s\" --hashes \"%s\" --no-media --quiet --force-full --pixel-reference %s %s", script, df, hp2, replay, extra);
    int r2 = run_child(args);
    if (r1 != 0 || r2 != 0) {
        printf("FAIL simulator launch or abnormal exit (damage=0x%08x, full=0x%08x)\n",
               (unsigned)r1, (unsigned)r2);
        return 2;
    }
    unsigned *fa, *fb; unsigned long long *ha, *hb;
    int na = read_hashes(hp1, &fa, &ha), nb = read_hashes(hp2, &fb, &hb);
    if (na <= 0 || nb <= 0) { printf("FAIL no frame hashes (exit codes %d %d)\n", r1, r2); return 2; }
    if(na!=nb) {printf("FAIL frame counts differ (%d vs %d)\n",na,nb);return 2;}
    for(int i=0;i<na;i++) if(fa[i]!=fb[i]) {
        printf("FAIL frame sequence differs at index %d (%u vs %u)\n",i,fa[i],fb[i]);return 2;
    }
    int n = na < nb ? na : nb, bad = -1, nbad = 0;
    for (int i = 0; i < n; i++) if (ha[i] != hb[i]) { if (bad < 0) bad = i; nbad++; }
    if (bad < 0) {
        printf("PASS damagecheck: %d frames identical (damage-tracked == full repaint)\n", n);
        return 0;
    }
    unsigned frame = fa[bad];
    printf("FAIL damagecheck: %d of %d frames differ; first at frame %u\n", nbad, n, frame);
    snprintf(args, sizeof args, "--script \"%s\" --out \"%s\" --dump-frame %u --no-media --quiet --pixel-reference %s %s", script, dd, frame, replay, extra);
    int dump_damage=run_child(args);
    snprintf(args, sizeof args, "--script \"%s\" --out \"%s\" --dump-frame %u --no-media --quiet --force-full --pixel-reference %s %s", script, df, frame, replay, extra);
    int dump_full=run_child(args);
    if(dump_damage!=0 || dump_full!=0) {
        printf("FAIL diagnostic frame dump (damage=%d full=%d)\n",dump_damage,dump_full);return 2;
    }
    static uint16_t A[GFX_W * GFX_H], B[GFX_W * GFX_H];
    char pa[300], pb[300];
    snprintf(pa, sizeof pa, "%s/frame_%u.bin", dd, frame);
    snprintf(pb, sizeof pb, "%s/frame_%u.bin", df, frame);
    if (!load_raw(pa, A) || !load_raw(pb, B)) { printf("  could not dump frame %u\n", frame); return 1; }
    int x0 = GFX_W, y0 = GFX_H, x1 = -1, y1 = -1, cnt = 0;
    sim_img im = img_new(GFX_W * 3 + 16, GFX_H + 24, 8, 8, 10);
    img_from_565(&im, 0, 24, A, 1);
    img_from_565(&im, GFX_W + 8, 24, B, 1);
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++) {
            int i = y * GFX_W + x, diff = A[i] != B[i] && sim_mask(x, y);
            int g = (((B[i] >> 5) & 63) * 255 / 63) / 4;
            uint8_t *p = &im.rgb[((size_t)(y + 24) * im.w + GFX_W * 2 + 16 + x) * 3];
            p[0] = diff ? 255 : (uint8_t)g; p[1] = diff ? 0 : (uint8_t)g; p[2] = diff ? 200 : (uint8_t)g;
            if (diff) { cnt++; if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
        }
    img_text(&im, 4, 6, "DAMAGE-TRACKED", 2, 230, 230, 230);
    img_text(&im, GFX_W + 12, 6, "FULL REPAINT", 2, 230, 230, 230);
    img_text(&im, GFX_W * 2 + 20, 6, "DIFF (magenta)", 2, 255, 0, 200);
    char out[300];
    snprintf(out, sizeof out, "%s/damagecheck_f%u.png", s_out, frame);
    png_write(out, &im);
    img_free(&im);
    printf("  %d px differ inside the glass, bbox x %d..%d y %d..%d\n", cnt, x0, x1, y0, y1);
    printf("  images: %s (side by side), %s/frame_%u.png, %s/frame_%u.png\n", out, dd, frame, df, frame);
    return 1;
}

/* ---- interactive window -------------------------------------------------------------------- */
#define BEZEL 28
#define WIN_S (GFX_W + 2 * BEZEL)
#define HUD_W 360
#define HIST 120
static HWND s_hwnd;
static float s_scale = 1.0f;
static int s_hud, s_rects, s_paused, s_step;
static uint32_t *s_win;                    /* WIN_S x WIN_S composed RGB32 */
static uint32_t *s_win_base;               /* bezel + background */
static double s_hist_ms[HIST]; static int s_hist_late[HIST], s_hist_pos, s_hist_sent[HIST];
static int s_air_idx;
static gif_w *s_rec; static int s_rec_k, s_rec_n;
static int s_mouse_down;
static int s_cpu_drag, s_cpu_drag_y, s_cpu_drag_start;
static double s_paced_ms = NOS_PERIOD_MS;
static double s_live_fps,s_paint_fps,s_fps_elapsed;
static unsigned s_fps_frames,s_fps_painted;
static uint32_t s_session_frames, s_session_late;
static char s_status[96];
static double s_exit_after;                /* --exit-after S: quit and print pacing stats */

static void cpu_dial_set(int mhz) {
    if(mhz<20)mhz=20;
    if(mhz>480)mhz=480;
    if(mhz==s_cpu_mhz)return;
    s_cpu_mhz=mhz;
    memset(s_hist_ms,0,sizeof s_hist_ms);
    memset(s_hist_sent,0,sizeof s_hist_sent);
    memset(s_hist_late,0,sizeof s_hist_late);
    s_hist_pos=0;s_session_frames=s_session_late=0;
    cost_estimate(&s_last,s_last.dirty_px);
}
static void win_base_build(void)
{
    s_win = malloc(sizeof(uint32_t) * WIN_S * WIN_S);
    s_win_base = malloc(sizeof(uint32_t) * WIN_S * WIN_S);
    float c = WIN_S / 2.0f, rp = GFX_W / 2.0f, rb = rp + 14.0f;
    for (int y = 0; y < WIN_S; y++)
        for (int x = 0; x < WIN_S; x++) {
            float dx = x + 0.5f - c, dy = y + 0.5f - c, d = sqrtf(dx * dx + dy * dy);
            float t = rb + 0.5f - d;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            /* brushed bezel ring: dark steel with a faint top highlight */
            float hl = 0.5f - 0.5f * dy / (d > 1 ? d : 1);
            int br = (int)(30 + 14 * hl), bg = (int)(30 + 14 * hl), bb = (int)(33 + 15 * hl);
            int r = (int)(10 + (br - 10) * t), g = (int)(10 + (bg - 10) * t), b = (int)(12 + (bb - 12) * t);
            s_win_base[y * WIN_S + x] = (uint32_t)(r << 16 | g << 8 | b);
        }
}
static void win_compose(void)
{
    memcpy(s_win, s_win_base, sizeof(uint32_t) * WIN_S * WIN_S);
    const uint16_t *fb = panel();
    for (int y = 0; y < GFX_H; y++)
        for (int x = 0; x < GFX_W; x++) {
            int m = sim_mask(x, y);
            if (!m) continue;
            uint16_t p = fb[y * GFX_W + x];
            int r = ((p >> 11) & 31) * 255 / 31, g = ((p >> 5) & 63) * 255 / 63, b = (p & 31) * 255 / 31;
            uint32_t *o = &s_win[(y + BEZEL) * WIN_S + x + BEZEL];
            if (m < 255) {
                int br = (*o >> 16) & 255, bgc = (*o >> 8) & 255, bb = *o & 255;
                r = (r * m + br * (255 - m)) / 255; g = (g * m + bgc * (255 - m)) / 255; b = (b * m + bb * (255 - m)) / 255;
            }
            *o = (uint32_t)(r << 16 | g << 8 | b);
        }
    if (s_rects)
        for (int i = 0; i < nos_dbg_nr && i < 12; i++) {
            int x0 = nos_dbg_rect[i][0] + BEZEL, y0 = nos_dbg_rect[i][1] + BEZEL;
            int x1 = nos_dbg_rect[i][2] + BEZEL - 1, y1 = nos_dbg_rect[i][3] + BEZEL - 1;
            uint32_t col = s_last.full ? 0xFF5050 : 0x30E080;
            for (int x = x0; x <= x1; x++) { s_win[y0 * WIN_S + x] = col; s_win[y1 * WIN_S + x] = col; }
            for (int y = y0; y <= y1; y++) { s_win[y * WIN_S + x0] = col; s_win[y * WIN_S + x1] = col; }
        }
}
#define PANEL_ROWS 7
#define PANEL_COLS 4
#define BTN_H 30
#define PANEL_H (PANEL_ROWS * (BTN_H + 6) + 8)    /* the button panel under the watch */
static int client_w(void) { return (int)(WIN_S * s_scale + 0.5f) + (s_hud ? HUD_W : 0); }
static int client_h(void) { int h = (int)(WIN_S * s_scale + 0.5f) + PANEL_H; return h < 780 && s_hud ? 780 : h; }

static void hud_draw(HDC dc)
{
    int x0 = (int)(WIN_S * s_scale + 0.5f), h = client_h();
    HDC mdc = CreateCompatibleDC(dc);
    HBITMAP bm = CreateCompatibleBitmap(dc, HUD_W, h);
    HGDIOBJ obm = SelectObject(mdc, bm);
    RECT r = { 0, 0, HUD_W, h };
    HBRUSH bg = CreateSolidBrush(RGB(14, 14, 17));
    FillRect(mdc, &r, bg); DeleteObject(bg);
    static HFONT font;
    if (!font) font = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                                  CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, "Consolas");
    HGDIOBJ of = SelectObject(mdc, font);
    SetBkMode(mdc, TRANSPARENT);
    char t[128];
    int y = 8;
#define LINE(col, ...) do { snprintf(t, sizeof t, __VA_ARGS__); SetTextColor(mdc, col); TextOutA(mdc, 10, y, t, (int)strlen(t)); y += 17; } while (0)
    const fcost *c = &s_last;
    const nos_screen *top = nos_top();
    LINE(RGB(150, 150, 160), "%s  depth %d%s", top && top->name ? top->name : "?", nos_depth(), nos_transitioning() ? "  TX" : "");
    LINE(RGB(150, 150, 160), "frame %u%s", (unsigned)c->frame, s_paused ? "  PAUSED (N=step)" : "");
    y += 4;
    LINE(c->late ? RGB(255, 90, 70) : RGB(120, 240, 150), "ESP32 est %5.1f ms %s", c->frame_ms, c->late ? "LATE" : "");
    LINE(RGB(200, 200, 210), "raster %5.1f  bus %5.1f ms", c->raster_ms, c->bus_ms);
    LINE(RGB(200, 200, 210), "dirty %6u px  rects %u%s", (unsigned)c->dirty_px, (unsigned)c->rects, c->full ? " FULL" : "");
    LINE(RGB(200, 200, 210), "cmds %u  calls %u", (unsigned)c->cmds, (unsigned)c->calls);
    LINE(RGB(110, 110, 120), "PC record %u us raster %u us", (unsigned)c->record_us, (unsigned)c->raster_us);
    y += 4;
    /* top classes this frame */
    int order[GFX_COST_NCLASS];
    for (int k = 0; k < GFX_COST_NCLASS; k++) order[k] = k;
    for (int i = 0; i < GFX_COST_NCLASS; i++)
        for (int j = i + 1; j < GFX_COST_NCLASS; j++)
            if (c->cls_ms[order[j]] > c->cls_ms[order[i]]) { int tmp = order[i]; order[i] = order[j]; order[j] = tmp; }
    for (int i = 0; i < 4 && c->cls_ms[order[i]] > 0.05; i++)
        LINE(RGB(170, 170, 180), "  %-6s %5.2f ms", CLASS_NAME[order[i]], c->cls_ms[order[i]]);
    if(y<218)y=218;
    /* rolling window over frames that sent pixels */
    double v[HIST]; int nv = 0, late = 0;
    for (int i = 0; i < HIST; i++) if (s_hist_sent[i]) { v[nv++] = s_hist_ms[i]; late += s_hist_late[i]; }
    qsort(v, nv, sizeof *v, cmp_d);
    LINE(RGB(200, 200, 210), "last %d sent: p95 %.1f max %.1f", nv, pct(v, nv, 95), nv ? v[nv - 1] : 0.0);
    LINE(late ? RGB(255, 120, 80) : RGB(140, 220, 160), "late %d   session late %u/%u", late, (unsigned)s_session_late, (unsigned)s_session_frames);
    /* graph */
    int gx = 10, gy = y + 6, gw = HIST * 2, gh = 90;
    RECT gr = { gx - 1, gy - 1, gx + gw + 1, gy + gh + 1 };
    HBRUSH gb = CreateSolidBrush(RGB(24, 24, 28)); FillRect(mdc, &gr, gb); DeleteObject(gb);
    const double gmax = 50.0;
    for (int i = 0; i < HIST; i++) {
        int k = (s_hist_pos + i) % HIST;
        if (!s_hist_sent[k] && s_hist_ms[k] <= 0) continue;
        int bh = (int)(fmin(s_hist_ms[k], gmax) / gmax * gh);
        RECT b = { gx + i * 2, gy + gh - bh, gx + i * 2 + 2, gy + gh };
        HBRUSH bb = CreateSolidBrush(s_hist_late[k] ? RGB(255, 80, 60) : s_hist_sent[k] ? RGB(90, 190, 250) : RGB(60, 60, 70));
        FillRect(mdc, &b, bb); DeleteObject(bb);
    }
    int ly = gy + gh - (int)(FRAME_MS / gmax * gh);
    RECT lr = { gx, ly, gx + gw, ly + 1 };
    HBRUSH lb = CreateSolidBrush(RGB(220, 70, 70)); FillRect(mdc, &lr, lb); DeleteObject(lb);
    y = gy + gh + 6;
    LINE(RGB(110, 110, 120), "red line = %.1f ms target", FRAME_MS);
    y += 6;
    LINE(RGB(110, 110, 120), "S=shot R=rec F=HUD D=rects P/N");
    y=412;
    LINE(RGB(220,220,230), "Effective CPU: %d MHz", s_cpu_mhz);
    /* Rotary dial: vertical drag and wheel both give single-MHz adjustment. */
    HBRUSH knob=CreateSolidBrush(RGB(40,45,53));
    HGDIOBJ oldbrush=SelectObject(mdc,knob);
    Ellipse(mdc,16,444,88,516);
    double angle=(135.0+270.0*(s_cpu_mhz-20)/460.0)*3.141592653589793/180.0;
    HPEN needle=CreatePen(PS_SOLID,3,RGB(90,190,250));
    HGDIOBJ oldpen=SelectObject(mdc,needle);
    MoveToEx(mdc,52,480,NULL);
    LineTo(mdc,52+(int)(29*cos(angle)),480+(int)(29*sin(angle)));
    SelectObject(mdc,oldpen);DeleteObject(needle);
    SelectObject(mdc,oldbrush);DeleteObject(knob);
    SetTextColor(mdc,RGB(180,190,205));
    TextOutA(mdc,110,454,"20 - 480 MHz",12);
    TextOutA(mdc,110,478,"[Reset: 180 MHz]",16);
    y=526;
    LINE(RGB(180,190,205), "Drag up/down or wheel: 1 MHz");
    LINE(RGB(150,150,165), "Shift + wheel: 10 MHz");
    LINE(RGB(150,150,165), "Model only; QSPI bus unchanged");
    y=586;
    LINE(RGB(120,220,170), "[Pace to model: %s]",s_cpu_pace?"ON":"OFF");
    LINE(RGB(150,150,165), "Modeled interval: %.1f ms",s_paced_ms);
    LINE(RGB(150,150,165), "%d FPS target / %d FPS floor",NOS_FPS,NOS_MIN_FPS);
    y=651;
    LINE(RGB(120,240,150), "Live loop: %.1f FPS",s_live_fps);
    LINE(RGB(180,190,205), "Painted: %.1f frames/s",s_paint_fps);
    LINE(RGB(150,150,165), "Model: %.1f FPS",1000.0/s_paced_ms);
    y+=6;
    if (s_status[0]) {
        RECT status_rect={10,y,HUD_W-14,h-10};
        SetTextColor(mdc,RGB(240,200,120));
        DrawTextA(mdc,s_status,-1,&status_rect,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
    }
#undef LINE
    BitBlt(dc, x0, 0, HUD_W, h, mdc, 0, 0, SRCCOPY);
    SelectObject(mdc, of);
    SelectObject(mdc, obm);
    DeleteObject(bm);
    DeleteDC(mdc);
}

static void present(HDC dc)
{
    win_compose();
    BITMAPINFO bi;
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = WIN_S; bi.bmiHeader.biHeight = -WIN_S;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    int sz = (int)(WIN_S * s_scale + 0.5f);
    if (sz == WIN_S) SetDIBitsToDevice(dc, 0, 0, WIN_S, WIN_S, 0, 0, 0, WIN_S, s_win, &bi, DIB_RGB_COLORS);
    else {
        SetStretchBltMode(dc, s_scale > 1 ? COLORONCOLOR : HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, 0, 0, sz, sz, 0, 0, WIN_S, WIN_S, s_win, &bi, DIB_RGB_COLORS, SRCCOPY);
    }
    if (client_h() > sz) { RECT r = { 0, sz, sz, client_h() }; FillRect(dc, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); }
    if (s_hud) hud_draw(dc);
}
static void present_now(void) { HDC dc = GetDC(s_hwnd); present(dc); ReleaseDC(s_hwnd, dc); }

static void apply_size(void)
{
    RECT r = { 0, 0, client_w(), client_h() };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    SetWindowPos(s_hwnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
    InvalidateRect(s_hwnd, NULL, FALSE);
}
static void set_title(void)
{
    char t[200];
    const nos_screen *top = nos_top();
    snprintf(t, sizeof t, "NOS sim  %s  %s%s", top && top->name ? top->name : "", s_rec ? "[REC] " : "",
             s_paused ? "[PAUSED] " : "");
    SetWindowTextA(s_hwnd, t);
}

static void window_frame_hook(void)
{
    s_hist_ms[s_hist_pos] = s_last.frame_ms;
    s_hist_late[s_hist_pos] = s_last.late;
    s_hist_sent[s_hist_pos] = s_last.dirty_px > 0;
    s_hist_pos = (s_hist_pos + 1) % HIST;
    if (s_last.dirty_px) { s_session_frames++; s_session_late += s_last.late; }
    if (s_rec) {
        sim_img im = img_new(GFX_W, GFX_H, 0, 0, 0);
        img_from_565(&im, 0, 0, panel(), 1);
        gif_add(s_rec, &im, gif_delay_fps(s_rec_k++, NOS_FPS));
        img_free(&im);
    }
}

static void mouse_touch(LPARAM lp, int down)
{
    int x = (int)floorf(GET_X_LPARAM(lp) / s_scale) - BEZEL, y = (int)floorf(GET_Y_LPARAM(lp) / s_scale) - BEZEL;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= GFX_W) x = GFX_W - 1;
    if (y >= GFX_H) y = GFX_H - 1;
    nos_touch(x, y, down);
}

/* ---- button panel: one-click tests under the watch ----------------------------------------- */
/* Input the panel queues for later frames: touches (a tap, a fling at one sample a frame, as
 * the watch reads them) and side-button presses. */
typedef struct { uint32_t at; int kind, x, y, down; } auto_ev;   /* kind 0 touch, 1 button */
static auto_ev s_auto[96];
static int s_nauto;
static uint32_t s_iframe;                      /* interactive frames run */
static void auto_add(int delay, int kind, int x, int y, int down)
{
    if (s_nauto < (int)(sizeof s_auto / sizeof *s_auto))
        s_auto[s_nauto++] = (auto_ev){ s_iframe + (uint32_t)delay, kind, x, y, down };
}
static void auto_run(void)                     /* before each frame: what is due */
{
    int k = 0;
    for (int i = 0; i < s_nauto; i++) {
        auto_ev *e = &s_auto[i];
        if (e->at > s_iframe) { s_auto[k++] = *e; continue; }
        if (e->kind) { sim_button_down = e->down; nos_button(e->down); }
        else nos_touch(e->x, e->y, e->down);
    }
    s_nauto = k;
}
static void auto_tap(int delay, int x, int y) { auto_add(delay, 0, x, y, 1); auto_add(delay + 2, 0, x, y, 0); }
/* a fling: ease-in over n frames from (x0, y0) to (x1, y1), released at full speed */
static void auto_fling_xy(int x0, int y0, int x1, int y1, int n)
{
    for (int k = 0; k <= n; k++) {
        double t = (double)k / n;
        auto_add(k, 0, (int)lround(x0 + (x1 - x0) * t * t), (int)lround(y0 + (y1 - y0) * t * t), 1);
    }
    auto_add(n + 1, 0, x1, y1, 0);
}
static void auto_fling(int y0, int y1, int n) { auto_fling_xy(GFX_W / 2, y0, GFX_W / 2, y1, n); }

enum { B_IRIS = 100, B_SLOW, B_APPS, B_BACK, B_RIPPLE, B_FIND, B_RING, B_MATRIX,
       B_SETTINGS, B_FLING_FAST, B_FLING_MED, B_FLING_DOWN, B_HOME, B_SIDE, B_HUD, B_NOTIFS,
       B_MBLUR, B_DRAWER, B_PAGER, B_PUSH, B_LAB, B_STEPSTYLE, B_ACTIVITY, B_STEPWG, B_DEV, B_DEV_FPS, B_DEV_STATS, B_DEV_TOUCH };
static const struct { int id; const char *label; } k_btn[PANEL_ROWS * PANEL_COLS] = {
    { B_IRIS, "" },                   { B_SLOW, "" },               { B_APPS, "Open Apps (iris)" }, { B_BACK, "Back (iris close)" },
    { B_RIPPLE, "" },                 { B_FIND, "Find Phone" },     { B_RING, "Ring watch" },      { B_MATRIX, "Matrix wave" },
    { B_SETTINGS, "Settings" },       { B_FLING_FAST, "Fling up fast" }, { B_FLING_MED, "Fling up medium" }, { B_FLING_DOWN, "Fling down fast" },
    { B_HOME, "Home" },               { B_SIDE, "Side button" },    { B_HUD, "Cost HUD" },         { B_NOTIFS, "Notifications" },
    { B_MBLUR, "" },                  { B_DRAWER, "Pull drawer down" }, { B_PAGER, "Swipe home pages" }, { B_PUSH, "Push a screen" },
    { B_LAB, "Progress lab" },        { B_STEPSTYLE, "" },          { B_ACTIVITY, "Steps app" },   { B_STEPWG, "Steps widget" },
    { B_DEV, "Developer tools" }, { B_DEV_FPS, "FPS overlay" }, { B_DEV_STATS, "Frame stats" }, { B_DEV_TOUCH, "Touch overlay" },
};
static int s_slow = 1;                         /* slow motion: each frame held this many periods */
static HWND s_btn[PANEL_ROWS * PANEL_COLS];
static void panel_labels(void)
{
    static const char *mn[3] = { "Ripple blur: OFF", "Ripple blur: TAILS", "Ripple blur: BURST" };
    if (s_btn[4]) SetWindowTextA(s_btn[4], mn[ui_ripple_blur % 3]);
    if (s_btn[0]) SetWindowTextA(s_btn[0], nos_iris_blur ? "Transition blur: ON" : "Transition blur: OFF");
    if (s_btn[1]) SetWindowTextA(s_btn[1], s_slow > 1 ? "Slow motion: 6x" : "Slow motion: OFF");
    static const char *mb[3] = { "Motion blur: OFF", "Blur: AFTER RELEASE", "Motion blur: ALWAYS" };
    if (s_btn[16]) SetWindowTextA(s_btn[16], mb[nos_mblur % 3]);
    { extern int nos_step_style; char t[48]; snprintf(t, sizeof t, "Step ring: %s", prog_name(nos_step_style));
      if (s_btn[21]) SetWindowTextA(s_btn[21], t); }
}
static void panel_layout(void)
{
    int w = (int)(WIN_S * s_scale + 0.5f), y0 = w + 6, bw = (w - 8 - (PANEL_COLS - 1) * 6) / PANEL_COLS;
    for (int i = 0; i < PANEL_ROWS * PANEL_COLS; i++)
        if (s_btn[i]) MoveWindow(s_btn[i], 4 + (i % PANEL_COLS) * (bw + 6), y0 + (i / PANEL_COLS) * (BTN_H + 6), bw, BTN_H, TRUE);
}
static void panel_create(HWND parent)
{
    HFONT f = CreateFontA(15, 0, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    for (int i = 0; i < PANEL_ROWS * PANEL_COLS; i++) {
        s_btn[i] = CreateWindowA("BUTTON", k_btn[i].label, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                 0, 0, 10, 10, parent, (HMENU)(INT_PTR)k_btn[i].id, GetModuleHandleA(0), 0);
        SendMessageA(s_btn[i], WM_SETFONT, (WPARAM)f, TRUE);
    }
    panel_labels();
    panel_layout();
}
static void open_fresh(const nos_screen *sc)   /* from the face, so each test starts the same */
{
    nos_home_now();
    nos_open(sc, GFX_W / 2, GFX_H / 2);
}
static void panel_click(int id)
{
    switch (id) {
    case B_DEV: open_fresh(&scr_developer); break;
    case B_DEV_FPS: nos_dev_overlays^=NOS_DEV_FPS;nos_invalidate();break;
    case B_DEV_STATS: nos_dev_overlays^=NOS_DEV_STATS;nos_invalidate();break;
    case B_DEV_TOUCH: nos_dev_overlays^=NOS_DEV_TOUCH;nos_invalidate();break;
    case B_IRIS: nos_iris_blur = !nos_iris_blur; panel_labels(); break;
    case B_SLOW: s_slow = s_slow > 1 ? 1 : 6; panel_labels(); break;
    case B_BACK: nos_pop(); break;
    case B_MBLUR: nos_mblur = (nos_mblur + 1) % 3; panel_labels(); break;
    case B_LAB: { extern const nos_screen scr_prog_lab; open_fresh(&scr_prog_lab); break; }
    case B_STEPSTYLE: { extern int nos_step_style; nos_step_style = (nos_step_style + 1) % PROG_NSTYLES; panel_labels(); break; }
    case B_ACTIVITY: open_fresh(&scr_activity); break;
    case B_STEPWG:                             /* home, then a flick to the first widget page */
        nos_home_now();
        auto_fling_xy(380, GFX_H / 2, 140, GFX_H / 2, 4);
        break;
    case B_DRAWER:                             /* from the face: a quick pull down, let go */
        nos_home_now();
        auto_fling_xy(GFX_W / 2, 40, GFX_W / 2, 260, 4);
        break;
    case B_PAGER: {                            /* a flick to the next page, or back to the face */
        static int dir;
        dir = !dir;
        if (dir) auto_fling_xy(380, GFX_H / 2, 140, GFX_H / 2, 4);
        else auto_fling_xy(100, GFX_H / 2, 360, GFX_H / 2, 4);
        break;
    }
    case B_PUSH:                               /* Settings > About slides in (TX_PUSH) */
        nos_home_now();
        nos_push(&scr_settings, TX_NONE);
        nos_push(&scr_about, TX_PUSH);
        break;
    case B_NOTIFS:                             /* enough cards to fling through */
        for (int i = svc_notif_count(); i < 14; i++) {
            char t[32], b[96];
            snprintf(t, sizeof t, "Person %d", i + 1);
            snprintf(b, sizeof b, "Message number %d, long enough to wrap onto a second line", i + 1);
            svc_notif_add_local("Messages", t, b);
        }
        open_fresh(&scr_notifs);
        snprintf(s_status, sizeof s_status, "notifications: rest ~3 s so they can pre-blur, then fling");
        break;
    case B_RIPPLE: {
        static const char *mn[3] = { "off", "tails", "tails + burst" };
        ui_ripple_blur = (ui_ripple_blur + 1) % 3;
        snprintf(s_status, sizeof s_status, "ripple blur: %s", mn[ui_ripple_blur]);
        panel_labels();
        break;
    }
    case B_FIND:
        open_fresh(&scr_find);
        auto_tap(24, GFX_W / 2, 206);          /* after the iris: tap the button, it rings */
        snprintf(s_status, sizeof s_status, "%s", hal_phone_connected() ? "find phone: ringing" : "find phone: the sim phone connects 3 s after start");
        break;
    case B_RING:
        svc_phone_inject("sim find");          /* the fake phone rings the watch */
        snprintf(s_status, sizeof s_status, "phone rings the watch (tap the watch to stop)");
        break;
    case B_MATRIX: {
        extern void sys_home_build_in(void);
        nos_prefs.face = 1;                    /* MATRIX */
        nos_home_now();
        sys_home_build_in();
        nos_invalidate();
        snprintf(s_status, sizeof s_status, "matrix face: build-in wave");
        break;
    }
    case B_SETTINGS: open_fresh(&scr_settings); snprintf(s_status, sizeof s_status, "settings: rest ~3 s so it can pre-blur, then fling"); break;
    case B_FLING_FAST: auto_fling(410, 130, 4); break;
    case B_FLING_MED:  auto_fling(380, 250, 4); break;
    case B_FLING_DOWN: auto_fling(130, 410, 4); break;
    case B_HOME: nos_home(); break;
    case B_APPS: open_fresh(&scr_launcher); break;
    case B_SIDE: auto_add(0, 1, 0, 0, 1); auto_add(3, 1, 0, 0, 0); break;
    case B_HUD: s_hud = !s_hud; apply_size(); break;
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        if (HIWORD(wp) == BN_CLICKED) {
            panel_click(LOWORD(wp));
            SetFocus(h);                       /* keys keep working after a click */
            set_title();
            present_now();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt={GET_X_LPARAM(lp),GET_Y_LPARAM(lp)};ScreenToClient(h,&pt);
        int x0=(int)(WIN_S*s_scale+0.5f);
        if(s_hud && pt.x>=x0 && pt.y>=412 && pt.y<=565) {
            cpu_dial_set(s_cpu_mhz+(GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA)*((GET_KEYSTATE_WPARAM(wp)&MK_SHIFT)?10:1));
            present_now();return 0;
        }
        break;
    }
    case WM_CAPTURECHANGED: s_cpu_drag=0;break;
    case WM_LBUTTONDOWN: {
        int x0=(int)(WIN_S*s_scale+0.5f),x=GET_X_LPARAM(lp)-x0,y=GET_Y_LPARAM(lp);
        if(s_hud && x>=0) {
            if(x>=16 && x<=88 && y>=444 && y<=516) {
                s_cpu_drag=1;s_cpu_drag_y=y;s_cpu_drag_start=s_cpu_mhz;SetCapture(h);
            } else if(x>=110 && x<285 && y>=470 && y<=505)cpu_dial_set(180);
            else if(y>=580 && y<=608)s_cpu_pace=!s_cpu_pace;
            present_now();return 0;
        }
        if (GET_Y_LPARAM(lp) >= (int)(WIN_S * s_scale + 0.5f)) return 0;   /* the panel, not the glass */
        SetCapture(h); s_mouse_down = 1; mouse_touch(lp, 1); return 0;
    }
    case WM_MOUSEMOVE:
        if(s_cpu_drag){cpu_dial_set(s_cpu_drag_start+s_cpu_drag_y-GET_Y_LPARAM(lp));present_now();return 0;}
        if (s_mouse_down) mouse_touch(lp, 1);
        return 0;
    case WM_LBUTTONUP:
        if(s_cpu_drag){s_cpu_drag=0;ReleaseCapture();return 0;}
        if(s_mouse_down){ReleaseCapture();s_mouse_down=0;mouse_touch(lp,0);}return 0;
    case WM_KEYDOWN: {
        int repeat = (lp >> 30) & 1;
        if (wp == VK_ESCAPE) DestroyWindow(h);
        else if (wp == VK_SPACE || wp == 'B') { if (!repeat) { sim_button_down = 1; nos_button(1); } }
        else if (repeat) return 0;
        else if (wp == 'G') {
            static const int g[6] = { GEST_LEFT, GEST_RIGHT, GEST_UP, GEST_DOWN, GEST_NEAR, GEST_FAR };
            static const char *gn[6] = { "left", "right", "up", "down", "near", "far" };
            nos_air(g[s_air_idx]);
            snprintf(s_status, sizeof s_status, "air gesture: %s", gn[s_air_idx]);
            s_air_idx = (s_air_idx + 1) % 6;
        } else if (wp == 'M') {
            static const char *mn[3] = { "off", "tails", "tails + burst" };
            ui_ripple_blur = (ui_ripple_blur + 1) % 3;
            snprintf(s_status, sizeof s_status, "ripple blur: %d %s", ui_ripple_blur, mn[ui_ripple_blur]);
            panel_labels();
            printf("%s\n", s_status);
        } else if (wp == 'W') {
            svc_phone_inject("sim find");   /* the fake phone rings the watch */
            snprintf(s_status, sizeof s_status, "phone rings the watch (tap to stop)");
        } else if (wp == 'S') {
            char path[300], name[64];
            SYSTEMTIME st; GetLocalTime(&st);
            ensure_dir(s_out);
            snprintf(name, sizeof name, "shot_%02d%02d%02d_%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            out_path(path, sizeof path, name, ".png");
            save_png565(path, panel());
            snprintf(s_status, sizeof s_status, "saved %s.png", name);
        } else if (wp == 'R') {
            if (s_rec) {
                gif_close(s_rec); s_rec = 0;
                snprintf(s_status, sizeof s_status, "recording saved (rec_%03d.gif, %d frames)", s_rec_n - 1, s_rec_k);
            } else {
                char path[300], name[32];
                ensure_dir(s_out);
                snprintf(name, sizeof name, "rec_%03d", s_rec_n++);
                out_path(path, sizeof path, name, ".gif");
                s_rec = gif_open(path, GFX_W, GFX_H); s_rec_k = 0;
                snprintf(s_status, sizeof s_status, "recording %s.gif", name);
            }
            printf("%s\n", s_status);
        } else if (wp == 'F') { s_hud = !s_hud; apply_size(); }
        else if (wp == 'D') { s_rects = !s_rects; }
        else if (wp == 'P') { s_paused = !s_paused; }
        else if (wp == 'N') { s_step = 1; }
        set_title();
        present_now();
        return 0;
    }
    case WM_KEYUP:
        if (wp == VK_SPACE || wp == 'B') { sim_button_down = 0; nos_button(0); }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        present(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

static int window_main(void)
{
    typedef BOOL(WINAPI * dpi_ctx_fn)(HANDLE);
    dpi_ctx_fn set_ctx = (dpi_ctx_fn)(void *)GetProcAddress(GetModuleHandleA("user32"), "SetProcessDpiAwarenessContext");
    if (!set_ctx || !set_ctx((HANDLE)-4)) SetProcessDPIAware();
    win_base_build();
    snprintf(sim_store_dir, sizeof sim_store_dir, "%s/store", s_out);
    ensure_dir(sim_store_dir);
    boot();
    s_on_frame = window_frame_hook;

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(0);
    wc.hCursor = LoadCursor(0, IDC_ARROW);
    wc.lpszClassName = "nossim";
    RegisterClassA(&wc);
    RECT r = { 0, 0, client_w(), client_h() };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    s_hwnd = CreateWindowA("nossim", "NOS sim", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    panel_create(s_hwnd);
    ShowWindow(s_hwnd, SW_SHOW);
    set_title();

    /* NOS_FPS pacing: absolute deadlines on a high-resolution waitable timer; the thread
     * sleeps in MsgWaitForMultipleObjects so input is handled the moment it arrives. */
    timeBeginPeriod(1);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    LARGE_INTEGER qpf, now, deadline, last;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&now);
    LONGLONG period = (LONGLONG)(nos_frame_cap_period(nos_prefs.fps_cap)*qpf.QuadPart/1000000);
    deadline.QuadPart = now.QuadPart + period;
    last = now;
    LARGE_INTEGER last_fps_tick=now;
    const nos_screen *shown_top = 0;
    for (;;) {
        MSG msg;
        while (PeekMessage(&msg, 0, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { if (s_rec) gif_close(s_rec); timeEndPeriod(1); return 0; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        QueryPerformanceCounter(&now);
        if (now.QuadPart < deadline.QuadPart) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((deadline.QuadPart - now.QuadPart) * 10000000 / qpf.QuadPart);
            if (due.QuadPart > -1) due.QuadPart = -1;
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            MsgWaitForMultipleObjects(1, &timer, FALSE, INFINITE, QS_ALLINPUT);
            continue;                                  /* handle input, re-check the deadline */
        }
        period=(LONGLONG)(nos_frame_cap_period(nos_prefs.fps_cap)*qpf.QuadPart/1000000);
        deadline.QuadPart=now.QuadPart+period;
        float elapsed = (float)((now.QuadPart - last.QuadPart) * 1000.0 / qpf.QuadPart);
        if (s_exit_after > 0) {
            static LARGE_INTEGER t_start; static int nfr; static double sum, sum2, mx;
            if (!nfr++) t_start = now;
            else { sum += elapsed; sum2 += (double)elapsed * elapsed; if (elapsed > mx) mx = elapsed; }
            if ((now.QuadPart - t_start.QuadPart) / (double)qpf.QuadPart >= s_exit_after) {
                int n = nfr - 1;
                double mean = n ? sum / n : 0, sd = n ? sqrt(fmax(0, sum2 / n - mean * mean)) : 0;
                printf("pacing: %d frames in %.2f s = %.2f fps, period mean %.3f ms sd %.3f max %.3f\n",
                       n, sum / 1000.0, n ? 1000.0 * n / sum : 0, mean, sd, mx);
                DestroyWindow(s_hwnd);
            }
        }
        if (s_paused && !s_step) {last_fps_tick=now;s_live_fps=s_paint_fps=0;continue;}
        if (s_paused) elapsed = NOS_PERIOD_MS;
        s_step = 0;
        last = now;
        static int s_hold;                     /* slow motion: the frame stays up */
        if (s_slow > 1 && !s_paused) {
            if (++s_hold < s_slow) continue;
            s_hold = 0;
            elapsed = NOS_PERIOD_MS;           /* the engine still sees 30 fps steps */
        }
        auto_run();
        s_iframe++;
        uint32_t px = sim_frame(elapsed);
        if(!s_paused){
            s_fps_elapsed+=(now.QuadPart-last_fps_tick.QuadPart)*1000.0/qpf.QuadPart;
            s_fps_frames++;s_fps_painted+=px!=0;
            if(s_fps_elapsed>=500){s_live_fps=1000.0*s_fps_frames/s_fps_elapsed;s_paint_fps=1000.0*s_fps_painted/s_fps_elapsed;s_fps_elapsed=0;s_fps_frames=s_fps_painted=0;}
        }
        last_fps_tick=now;
        double cap_ms=nos_frame_cap_period(nos_prefs.fps_cap)/1000.0;
        s_paced_ms=s_cpu_pace?fmax(cap_ms,s_last.frame_ms):cap_ms;
        if(s_cpu_pace && s_slow==1 && !s_paused) {
            /* Schedule from frame start: host work already consumes this interval.
             * Never busy-wait; the existing message-aware timer handles input. */
            deadline.QuadPart=now.QuadPart+(LONGLONG)(s_paced_ms*qpf.QuadPart/1000.0);
        }
        if (nos_top() != shown_top) { shown_top = nos_top(); set_title(); }
        if (px) present_now();
        else if (s_hud && nos_frame % 15 == 0) { HDC dc = GetDC(s_hwnd); hud_draw(dc); ReleaseDC(s_hwnd, dc); }
    }
}

/* ---- main ------------------------------------------------------------------------------------ */
static void usage(void)
{
    printf("nos_sim.exe                         interactive window (%d fps target)\n", NOS_FPS);
    puts("  --cpu-mhz N  effective model clock 20..480; --cost-hud; --no-cpu-pace\n");
    puts(
         "  --scale F                         window scale (e.g. 1.5, 0.75)\n"
         "  --script FILE --out DIR           headless run on a virtual clock\n"
         "  --open NAME                       open a named app at startup (e.g. sports)\n"
         "  --workout-store DIR               preserve workout history at this path\n"
         "  --no-sd                           boot with the simulated SD card absent\n"
         "  --shots-all [--out DIR]           run tools/nos_tour.txt\n"
         "  --damagecheck FILE [--out DIR]    damage-tracked vs full repaint, frame by frame\n"
         "  --selftest                        boot the debug screen instead of the apps\n"
         "  --boot                            scripted runs keep the boot animation (default: skipped)\n"
         "  --fullframe                       engine full-frame path instead of configured stripes\n"
         "  --sync-exposure                   apply blur on the producer for exact worker A/B\n"
         "  --force-full                      every frame a full repaint (nos_debug_force_full)\n"
         "  --motion                          motion audit from the first frame (motion.csv, motion.txt)\n"
         "  --hashes FILE  --dump-frame N  --no-media  --quiet   (damagecheck plumbing)\n"
         "Keys: drag = touch, Space/B = side button (hold = long), G = air gesture,\n"
         "      S = screenshot, R = record GIF, F = cost HUD, D = dirty rects, P = pause, N = step, Esc.\n"
         "See docs/NOS_SIM.md.");
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    /* Headless test failures must return to the runner, never wait on Windows UI. */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    /* Older MinGW links msvcrt, which lacks _set_abort_behavior. Configure every
     * available CRT by export, keeping that toolchain linkable as well as UCRT. */
    const char *crts[] = { "msvcrt.dll", "ucrtbase.dll" };
    for (unsigned i = 0; i < sizeof crts / sizeof crts[0]; i++) {
        HMODULE crt = GetModuleHandleA(crts[i]);
        if (!crt) crt = LoadLibraryA(crts[i]);
        typedef unsigned (__cdecl *abort_behavior_fn)(unsigned, unsigned);
        abort_behavior_fn set_abort_behavior = crt ?
            (abort_behavior_fn)(void *)GetProcAddress(crt, "_set_abort_behavior") : NULL;
        if (set_abort_behavior) set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
#endif
    const char *script = 0, *dcheck = 0;
#ifdef GFX_COST_MODEL
    const char *quality_path=0;int quality_replay=0;
#endif
    int out_set = 0, shots_all = 0;
    char extra[256] = "";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
#ifdef GFX_COST_MODEL
        if(!strcmp(a,"--quality-record") || !strcmp(a,"--quality-replay")) {
            quality_path=v;quality_replay=!strcmp(a,"--quality-replay");i++;
        } else
#endif
        if (!strcmp(a, "--script")) { script = v; i++; }
        else if (!strcmp(a, "--out")) { snprintf(s_out, sizeof s_out, "%s", v); out_set = 1; i++; }
        else if (!strcmp(a, "--open")) {
            s_start_screen = screen_by_name(v);
            if (!s_start_screen) { fprintf(stderr, "unknown startup screen %s\n", v); return 2; }
            i++;
        }
        else if (!strcmp(a, "--workout-store")) {
            if (!*v || strlen(v) >= 224) { fprintf(stderr, "--workout-store requires a directory shorter than 224 bytes\n"); return 2; }
            snprintf(s_workout_store, sizeof s_workout_store, "%s", v); i++;
        }
        else if (!strcmp(a, "--no-sd")) { extern int sim_sd_present; sim_sd_present = 0; strcat(extra, " --no-sd"); }
        else if (!strcmp(a, "--cpu-mhz")) {
            char *end;long mhz=strtol(v,&end,10);
            if(!*v || *end || mhz<20 || mhz>480){fprintf(stderr,"--cpu-mhz requires 20..480\n");return 2;}
            s_cpu_mhz=(int)mhz;i++;
        }
        else if (!strcmp(a, "--cost-hud")) s_hud=1;
        else if (!strcmp(a, "--no-cpu-pace")) s_cpu_pace=0;
        else if (!strcmp(a, "--scale")) { s_scale = (float)atof(v); if (s_scale < 0.25f) s_scale = 1; i++; }
        else if (!strcmp(a, "--damagecheck")) { dcheck = v; i++; }
        else if (!strcmp(a, "--shots-all")) shots_all = 1;
        else if (!strcmp(a, "--selftest")) { s_selftest = 1; strcat(extra, " --selftest"); }
        else if (!strcmp(a, "--boot")) { s_keep_boot = 1; strcat(extra, " --boot"); }
        else if (!strcmp(a, "--face")) { s_face = atoi(v); i++; }
        else if (!strcmp(a, "--fullframe")) { s_fullframe_mode = 1; strcat(extra, " --fullframe"); }
        else if (!strcmp(a, "--sync-exposure")) { s_exposure_worker = 0; strcat(extra, " --sync-exposure"); }
        else if (!strcmp(a, "--force-full")) nos_debug_force_full = 1;
        else if (!strcmp(a, "--hashes")) { s_hashf = fopen(v, "w"); i++; }
        else if (!strcmp(a, "--dump-frame")) { s_dump_frame = atoi(v); i++; }
        else if (!strcmp(a, "--no-media")) s_no_media = 1;
        else if (!strcmp(a, "--pixel-reference")) s_pixel_reference = 1;
        else if (!strcmp(a, "--quiet")) sim_log_quiet = 1;
        else if (!strcmp(a, "--motion")) { s_motion_flag = 1; strcat(extra, " --motion"); }
        else if (!strcmp(a, "--exit-after")) { s_exit_after = atof(v); i++; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else { fprintf(stderr, "unknown option %s\n", a); usage(); return 2; }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    if (shots_all) {
        script = "tools/nos_tour.txt";
        if (!out_set) { snprintf(s_out, sizeof s_out, "build/nos_shots"); out_set = 1; }
    }
    if (dcheck) {
        if (!out_set) snprintf(s_out, sizeof s_out, "build/nos_damagecheck");
        ensure_dir(s_out);
        return damagecheck_main(dcheck, extra);
    }
    if (script) {
        if (!out_set) {
            const char *base = strrchr(script, '/'), *b2 = strrchr(script, '\\');
            if (b2 > base) base = b2;
            base = base ? base + 1 : script;
            char nm[96]; snprintf(nm, sizeof nm, "%s", base);
            char *dot = strrchr(nm, '.'); if (dot) *dot = 0;
            snprintf(s_out, sizeof s_out, "build/nos_out/%s", nm);
        }
#ifdef GFX_COST_MODEL
        if(quality_path) {qt_open(quality_path,quality_replay);nos_quality_decision=qt_decide;}
#endif
        int result=script_main(script);
#ifdef GFX_COST_MODEL
        if(quality_path) qt_close(s_dump_frame>=0?(unsigned)s_dump_frame:~0u);
#endif
        return result;
    }
    return window_main();
}
