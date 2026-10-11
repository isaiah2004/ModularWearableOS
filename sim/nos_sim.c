/* nos_sim.c — the NOS simulator: interactive window, headless scripted runs on a
 * virtual clock, the ESP32-S3 frame-cost model and the damage-tracking check.
 * Usage: docs/NOS_SIM.md, or `nos_sim.exe --help`.
 *
 * The engine always renders through the device path: 16-row stripes, flushed into a
 * panel-RAM buffer (s_panel) exactly like the QSPI flush, so the cost model sees the
 * same per-stripe primitive calls the watch makes. --fullframe uses the engine's
 * full-frame path instead (no call counting fidelity, handy for debugging). */
#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "nos.h"
#include "prefs.h"
#include "hal_ext.h"
#include "gfx_cost.h"
#include "nos_sim.h"
#include "apps/nos_apps.h"
#include "svc.h"
#include "dot.h"

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
static const struct { const char *name; const nos_screen *s; } SCREENS[] = {
    { "boot", &scr_boot }, { "home", &scr_home }, { "control", &scr_control }, { "notifs", &scr_notifs },
    { "notif_detail", &scr_notif_detail }, { "launcher", &scr_launcher }, { "faces", &scr_faces },
    { "power", &scr_power }, { "activity", &scr_activity }, { "heart", &scr_heart }, { "breathe", &scr_breathe },
    { "timer", &scr_timer }, { "stopwatch", &scr_stopwatch }, { "alarm", &scr_alarm },
    { "worldclock", &scr_worldclock }, { "compass", &scr_compass }, { "baro", &scr_baro }, { "nav", &scr_nav },
    { "weather", &scr_weather }, { "music", &scr_music }, { "cell", &scr_cell }, { "find", &scr_find },
    { "memo", &scr_memo }, { "torch", &scr_torch }, { "settings", &scr_settings }, { "modules", &scr_modules },
    { "about", &scr_about }, { "alarm_ring", &scr_alarm_ring }, { "timer_ring", &scr_timer_ring },
    { "call", &scr_call }, { "find_watch", &scr_find_watch }, { "selftest", &nos_selftest_root },
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
#define STRIPE_ROWS 16
static uint16_t s_panel[GFX_W * GFX_H];          /* panel RAM: what the glass shows */
static uint16_t s_full[GFX_W * GFX_H];           /* engine full frame (--fullframe) */
static uint16_t s_stripes[2][GFX_W * STRIPE_ROWS];
static uint16_t s_scratch[GFX_W * GFX_H];        /* two-layer transitions */
static int s_fullframe_mode;
static uint32_t s_flushed_px;                    /* pixels sent this frame */
static uint32_t s_flushes;

static void flush_cb(uint16_t *buf, int stride, int x0, int y0, int w, int rows)
{
    for (int r = 0; r < rows; r++)
        memcpy(&s_panel[(y0 + r) * GFX_W + x0], &buf[r * stride], (size_t)w * 2);
    s_flushed_px += (uint32_t)(w * rows);
    s_flushes++;
}
static const uint16_t *panel(void) { return s_fullframe_mode ? s_full : s_panel; }

/* ---- device cost model -------------------------------------------------------------
 * ns per written pixel, measured on the watch (docs/FLAT60_DESIGN.md). */
static const double CLASS_NS[GFX_COST_NCLASS] = { 4, 25, 32, 33, 131, 146, 190, 280, 300, 507, 641, 723, 8, 6 };
static const char *CLASS_NAME[GFX_COST_NCLASS] = { "fill", "circle", "pill", "blit", "mask", "glyph",
                                                   "blend", "text", "ring", "arc", "icon", "line",
                                                   "dim", "select" };
#define CALL_NS       400.0     /* per primitive call, per stripe it runs in */
#define FIXED_MS      1.5       /* record pass, damage diff, flush bookkeeping */
#define PIPE_MS       1.0       /* raster/flush pipeline fill */
#define BUS_BPS       18.6e6    /* effective QSPI bytes/s */
#define FRAME_MS      (1000.0 / NOS_FPS)

static int s_cost_on;
static uint64_t s_cls_px[GFX_COST_NCLASS], s_cls_calls[GFX_COST_NCLASS];
void gfx_cost_add(int cls, unsigned px, int calls)
{
    if (!s_cost_on || cls < 0 || cls >= GFX_COST_NCLASS) return;
    s_cls_px[cls] += px;
    s_cls_calls[cls] += (unsigned)calls;
}

typedef struct {
    uint32_t frame, dirty_px, rects, cmds, calls, record_us, raster_us;
    int full, late;
    double raster_ms, bus_ms, frame_ms;
    double cls_ms[GFX_COST_NCLASS];
    char section[32], screen[24];
} fcost;

static fcost s_last;                     /* last frame's estimate */
static fcost *s_hist; static int s_nhist, s_cap_hist;   /* scripted runs keep every frame */
static char s_section[32] = "-";

static void cost_estimate(fcost *c, uint32_t px)
{
    double ns = 0;
    uint64_t calls = 0;
    for (int k = 0; k < GFX_COST_NCLASS; k++) {
        c->cls_ms[k] = s_cls_px[k] * CLASS_NS[k] / 1e6 + s_cls_calls[k] * CALL_NS / 1e6;
        ns += s_cls_px[k] * CLASS_NS[k];
        calls += s_cls_calls[k];
    }
    c->calls = (uint32_t)calls;
    c->raster_ms = (ns + calls * CALL_NS) / 1e6;
    c->bus_ms = px * 2.0 / BUS_BPS * 1000.0;
    /* flush runs on the other core in parallel with raster */
    c->frame_ms = px ? fmax(c->raster_ms + FIXED_MS, c->bus_ms) + PIPE_MS : FIXED_MS + c->raster_ms;
    c->late = c->frame_ms > FRAME_MS;
}

/* ---- run state ----------------------------------------------------------------------- */
static int s_script_mode, s_no_media, s_abort, s_fail;
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
            gif_add(c->gif, &im, gif_delay_30fps(c->gif_k++));
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
    nos_ext_sim_tick(hal_millis());
    memset(s_cls_px, 0, sizeof s_cls_px);
    memset(s_cls_calls, 0, sizeof s_cls_calls);
    s_flushed_px = 0; s_flushes = 0;
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
    if (!px) c.rects = 0;
    cost_estimate(&c, c.dirty_px);
    snprintf(c.section, sizeof c.section, "%s", s_section);
    const nos_screen *top = nos_top();
    snprintf(c.screen, sizeof c.screen, "%s", top && top->name ? top->name : "?");
    s_last = c;
    s_frames_run++;
    if (s_script_mode) {
        if (s_nhist == s_cap_hist) { s_cap_hist = s_cap_hist ? s_cap_hist * 2 : 1024; s_hist = realloc(s_hist, sizeof *s_hist * s_cap_hist); }
        s_hist[s_nhist++] = c;
        if (s_csv) {
            fprintf(s_csv, "%u,%u,%u,%u,%.3f,%.3f,%.3f,%d,%u,%d,%s,%s,%u,%u",
                    (unsigned)c.frame, (unsigned)c.dirty_px, (unsigned)c.rects, (unsigned)c.cmds,
                    c.raster_ms, c.bus_ms, c.frame_ms, c.late, (unsigned)c.calls, c.full,
                    c.section, c.screen, (unsigned)c.record_us, (unsigned)c.raster_us);
            for (int k = 0; k < GFX_COST_NCLASS; k++) fprintf(s_csv, ",%.3f", c.cls_ms[k]);
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
    else if (!_stricmp(key, "postdim")) { extern int nos_debug_post_dim; nos_debug_post_dim = atoi(val); }
    else if (!_stricmp(key, "aod")) nos_aod = atoi(val);
    else if (!_stricmp(key, "selftest_bug")) { extern int nos_selftest_bug; nos_selftest_bug = atoi(val); }
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
    if (!f) { fprintf(stderr, "cannot open script %s\n", path); return 0; }
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
        else if (!strcmp(cmd, "press")) { button_hold(3); run_frames(1); }
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
        else if (!strcmp(cmd, "skipboot")) skip_boot();
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
        else if (!strcmp(cmd, "home")) nos_home();
        else if (!strcmp(cmd, "pop")) nos_pop();
        else if (!strcmp(cmd, "invalidate")) nos_invalidate();
        else if (!strcmp(cmd, "log")) logline(rest_after(p, 1));
        else if (!strcmp(cmd, "mark")) { snprintf(s_section, sizeof s_section, "%s", a); logline(p); }
        else if (!strcmp(cmd, "expect")) {
            const nos_screen *t = nos_top();
            if (!strcmp(a, "top")) { if (!t || !t->name || strcmp(t->name, rest_after(p, 2))) fail("EXPECT FAIL top: got '%s', want '%s'", t && t->name ? t->name : "(none)", rest_after(p, 2)); }
            else if (!strcmp(a, "depth")) { char g[16]; snprintf(g, sizeof g, "%d", nos_depth()); if (nos_depth() != atoi(b)) fail("EXPECT FAIL depth: got %s, want %s", g, b); }
            else if (!strcmp(a, "present")) {
                char g[16]; snprintf(g, sizeof g, "0x%X", (unsigned)hal_ext_present());
                if (hal_ext_present() != (uint32_t)strtoul(b, 0, 0)) fail("EXPECT FAIL present: got %s, want %s", g, b);
            }
            else if (!strcmp(a, "idle")) { if (s_last.dirty_px) fail("EXPECT FAIL idle: last frame still sent pixels%s%s", "", ""); }
            else if (!strcmp(a, "nolate")) { char g[16]; snprintf(g, sizeof g, "%d", section_late()); if (section_late()) fail("EXPECT FAIL nolate: %s late frames in section %s", g, s_section); }
            else fail("unknown expect '%s'%s", a, "");
        }
        else if (!strcmp(cmd, "include")) {
            if (depth < 8) run_script(a, depth + 1);
        }
        else if (!strcmp(cmd, "quit")) break;
        else {
            char t[64]; snprintf(t, sizeof t, "%d", ln);
            fail("script line %s: unknown command '%s'", t, cmd);
        }
    }
    fclose(f);
    return 1;
}

static int cmp_d(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double pct(double *v, int n, double p) { if (!n) return 0; int i = (int)ceil(p / 100.0 * n) - 1; return v[i < 0 ? 0 : i >= n ? n - 1 : i]; }

static void summarize(FILE *o, const char *section)
{
    double *v = malloc(sizeof(double) * (s_nhist + 1)), *va = malloc(sizeof(double) * (s_nhist + 1));
    int n = 0, na = 0, late = 0;
    double cls[GFX_COST_NCLASS] = { 0 }, tot = 0;
    for (int i = 0; i < s_nhist; i++) {
        const fcost *c = &s_hist[i];
        if (section && strcmp(c->section, section)) continue;
        v[n++] = c->frame_ms;
        if (c->dirty_px) va[na++] = c->frame_ms;
        late += c->late;
        for (int k = 0; k < GFX_COST_NCLASS; k++) { cls[k] += c->cls_ms[k]; tot += c->cls_ms[k]; }
    }
    qsort(v, n, sizeof *v, cmp_d);
    qsort(va, na, sizeof *va, cmp_d);
    fprintf(o, "%-14s frames %5d  sent %5d  est ms p50 %5.1f  p95 %5.1f  max %5.1f  | sending p50 %5.1f p95 %5.1f  | late %d (%.1f%%)\n",
            section ? section : "ALL", n, na, pct(v, n, 50), pct(v, n, 95), n ? v[n - 1] : 0,
            pct(va, na, 50), pct(va, na, 95), late, n ? 100.0 * late / n : 0);
    if (!section && tot > 0) {
        fprintf(o, "raster time by class:");
        for (int k = 0; k < GFX_COST_NCLASS; k++)
            if (cls[k] > 0.0005 * tot) fprintf(o, "  %s %.0f%%", CLASS_NAME[k], 100 * cls[k] / tot);
        fprintf(o, "\n");
    }
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
        fprintf(o, "== NOS device cost estimate (ESP32-S3 model, budget %.1f ms) ==\n", FRAME_MS);
        summarize(o, 0);
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
static int s_selftest, s_keep_boot;
static void boot(void)
{
    prefs_load();
    hal_ext_init();
    if (!s_fullframe_mode) {
        nos_set_stripes(s_stripes[0], s_stripes[1], STRIPE_ROWS, NULL, flush_cb);
        nos_set_scratch(s_scratch);
    }
    nos_init(s_full);
    if (s_selftest) nos_set_root(&nos_selftest_root);
    else nos_apps_boot();
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
        fputc('\n', s_csv);
    }
    out_path(path, sizeof path, "log", ".txt");
    s_logf = fopen(path, "w");
    boot();
    if (!s_keep_boot) skip_boot();     /* scripts start on home unless --boot */
    run_frames(1);                    /* first frame: full paint of the root */
    run_script(script, 0);
    while (captures_pending() && !s_abort) vstep();   /* finish open films/gifs */
    for (int i = 0; i < MAXCAP; i++) capture_close(&s_caps[i]);
    if (s_csv) fclose(s_csv);
    if (s_logf) fclose(s_logf);
    if (s_hashf) fclose(s_hashf);
    if (s_dump_frame < 0) write_summary();
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
static int damagecheck_main(const char *script, const char *extra)
{
    char dd[300], df[300], args[1800], hp1[300], hp2[300];
    snprintf(dd, sizeof dd, "%s/damage", s_out);
    snprintf(df, sizeof df, "%s/full", s_out);
    ensure_dir(dd); ensure_dir(df);
    snprintf(hp1, sizeof hp1, "%s/hashes.txt", dd);
    snprintf(hp2, sizeof hp2, "%s/hashes.txt", df);
    printf("damagecheck: %s\n  run 1: damage tracking\n", script);
    snprintf(args, sizeof args, "--script \"%s\" --out \"%s\" --hashes \"%s\" --no-media --quiet %s", script, dd, hp1, extra);
    int r1 = run_child(args);
    printf("  run 2: forced full repaint (nos_debug_force_full)\n");
    snprintf(args, sizeof args, "--script \"%s\" --out \"%s\" --hashes \"%s\" --no-media --quiet --force-full %s", script, df, hp2, extra);
    int r2 = run_child(args);
    if (r1 < 0 || r2 < 0) { printf("FAIL could not run the simulator\n"); return 2; }
    unsigned *fa, *fb; unsigned long long *ha, *hb;
    int na = read_hashes(hp1, &fa, &ha), nb = read_hashes(hp2, &fb, &hb);
    if (na <= 0 || nb <= 0) { printf("FAIL no frame hashes (exit codes %d %d)\n", r1, r2); return 2; }
    if (na != nb) printf("  note: frame counts differ (%d vs %d) — the runs diverged\n", na, nb);
    int n = na < nb ? na : nb, bad = -1, nbad = 0;
    for (int i = 0; i < n; i++) if (ha[i] != hb[i]) { if (bad < 0) bad = i; nbad++; }
    if (bad < 0) {
        printf("PASS damagecheck: %d frames identical (damage-tracked == full repaint)\n", n);
        return 0;
    }
    unsigned frame = fa[bad];
    printf("FAIL damagecheck: %d of %d frames differ; first at frame %u\n", nbad, n, frame);
    snprintf(args, sizeof args, "--script \"%s\" --out \"%s\" --dump-frame %u --no-media --quiet %s", script, dd, frame, extra);
    run_child(args);
    snprintf(args, sizeof args, "--script \"%s\" --out \"%s\" --dump-frame %u --no-media --quiet --force-full %s", script, df, frame, extra);
    run_child(args);
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
#define HUD_W 300
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
static uint32_t s_session_frames, s_session_late;
static char s_status[96];
static double s_exit_after;                /* --exit-after S: quit and print pacing stats */

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
static int client_w(void) { return (int)(WIN_S * s_scale + 0.5f) + (s_hud ? HUD_W : 0); }
static int client_h(void) { int h = (int)(WIN_S * s_scale + 0.5f); return h < 400 && s_hud ? 400 : h; }

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
    y = 200;
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
    LINE(RGB(110, 110, 120), "red line = 33.3 ms budget");
    y += 6;
    LINE(RGB(110, 110, 120), "drag=touch  Space/B=button  G=air");
    LINE(RGB(110, 110, 120), "S=shot R=rec F=HUD D=rects P/N");
    if (s_status[0]) LINE(RGB(240, 200, 120), "%s", s_status);
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
        gif_add(s_rec, &im, gif_delay_30fps(s_rec_k++));
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

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_LBUTTONDOWN: SetCapture(h); s_mouse_down = 1; mouse_touch(lp, 1); return 0;
    case WM_MOUSEMOVE: if (s_mouse_down) mouse_touch(lp, 1); return 0;
    case WM_LBUTTONUP: ReleaseCapture(); s_mouse_down = 0; mouse_touch(lp, 0); return 0;
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
    s_hwnd = CreateWindowA("nossim", "NOS sim", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, 0, 0, wc.hInstance, 0);
    ShowWindow(s_hwnd, SW_SHOW);
    set_title();

    /* 30 Hz pacing: absolute deadlines on a high-resolution waitable timer; the thread
     * sleeps in MsgWaitForMultipleObjects so input is handled the moment it arrives. */
    timeBeginPeriod(1);
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    LARGE_INTEGER qpf, now, deadline, last;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&now);
    const LONGLONG period = qpf.QuadPart / NOS_FPS;
    deadline.QuadPart = now.QuadPart + period;
    last = now;
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
        deadline.QuadPart += period;
        if (now.QuadPart - deadline.QuadPart > 4 * period) deadline.QuadPart = now.QuadPart + period;
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
        if (s_paused && !s_step) continue;
        if (s_paused) elapsed = NOS_PERIOD_MS;
        s_step = 0;
        last = now;
        uint32_t px = sim_frame(elapsed);
        if (nos_top() != shown_top) { shown_top = nos_top(); set_title(); }
        if (px) present_now();
        else if (s_hud && nos_frame % 15 == 0) { HDC dc = GetDC(s_hwnd); hud_draw(dc); ReleaseDC(s_hwnd, dc); }
    }
}

/* ---- main ------------------------------------------------------------------------------------ */
static void usage(void)
{
    puts("nos_sim.exe                         interactive window (30 fps)\n"
         "  --scale F                         window scale (e.g. 1.5, 0.75)\n"
         "  --script FILE --out DIR           headless run on a virtual clock\n"
         "  --shots-all [--out DIR]           run tools/nos_tour.txt\n"
         "  --damagecheck FILE [--out DIR]    damage-tracked vs full repaint, frame by frame\n"
         "  --selftest                        boot the debug screen instead of the apps\n"
         "  --boot                            scripted runs keep the boot animation (default: skipped)\n"
         "  --fullframe                       engine full-frame path instead of 16-row stripes\n"
         "  --force-full                      every frame a full repaint (nos_debug_force_full)\n"
         "  --hashes FILE  --dump-frame N  --no-media  --quiet   (damagecheck plumbing)\n"
         "Keys: drag = touch, Space/B = side button (hold = long), G = air gesture,\n"
         "      S = screenshot, R = record GIF, F = cost HUD, D = dirty rects, P = pause, N = step, Esc.\n"
         "See docs/NOS_SIM.md.");
}

int main(int argc, char **argv)
{
    const char *script = 0, *dcheck = 0;
    int out_set = 0, shots_all = 0;
    char extra[256] = "";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--script")) { script = v; i++; }
        else if (!strcmp(a, "--out")) { snprintf(s_out, sizeof s_out, "%s", v); out_set = 1; i++; }
        else if (!strcmp(a, "--scale")) { s_scale = (float)atof(v); if (s_scale < 0.25f) s_scale = 1; i++; }
        else if (!strcmp(a, "--damagecheck")) { dcheck = v; i++; }
        else if (!strcmp(a, "--shots-all")) shots_all = 1;
        else if (!strcmp(a, "--selftest")) { s_selftest = 1; strcat(extra, " --selftest"); }
        else if (!strcmp(a, "--boot")) { s_keep_boot = 1; strcat(extra, " --boot"); }
        else if (!strcmp(a, "--fullframe")) { s_fullframe_mode = 1; strcat(extra, " --fullframe"); }
        else if (!strcmp(a, "--force-full")) nos_debug_force_full = 1;
        else if (!strcmp(a, "--hashes")) { s_hashf = fopen(v, "w"); i++; }
        else if (!strcmp(a, "--dump-frame")) { s_dump_frame = atoi(v); i++; }
        else if (!strcmp(a, "--no-media")) s_no_media = 1;
        else if (!strcmp(a, "--quiet")) sim_log_quiet = 1;
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
        return script_main(script);
    }
    return window_main();
}
