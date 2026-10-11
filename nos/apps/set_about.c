/* set_about.c — About: the NOS dot logo, version, board, memory, uptime, battery rail,
 * and the open-font credits.
 *
 * A ui_list (26 px units) scrolls the page under the ABOUT title: the logo block, then
 * two-line info rows (caps label over a value). Static values are grotesk; the two live
 * numbers (uptime, battery rail) are dot-matrix text that morphs when it changes, and the
 * rail's unit is a caps label beside its digits. The logo is drawn dot by dot from a local
 * bitmap: it sparkles in on entry, the tagline arrives with the first info row, and every
 * few seconds a glint swells a diagonal band of dots by 15 %. */
#include "nos.h"
#include "ui.h"
#include "dot.h"
#include "motion.h"
#include "svc.h"
#include "hal_ext.h"
#include "prefs.h"
#include "apps/nos_apps.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define ABOUT_VERSION NOS_VERSION

/* ---- logo ------------------------------------------------------------------ */
static const char *const k_logo[7] = {
    "#...#..###...####",
    "#...#.#...#.#....",
    "##..#.#...#.#....",
    "#.#.#.#...#..###.",
    "#..##.#...#.....#",
    "#...#.#...#.....#",
    "#...#..###..####.",
};
#define LOGO_COLS 17
#define LOGO_PITCH 12.0f
#define LOGO_DOT 9.4f

/* ---- rows ------------------------------------------------------------------ */
enum { R_LOGO = 0, R_VERSION, R_BOARD, R_DISPLAY, R_PSRAM, R_SRAM, R_UPTIME, R_RAIL, R_FONTS, R_COUNT };
#define AU 26
#define ATOP 96                 /* under the ABOUT title */
#define LOGO_Y 24               /* logo top inside its row */
static const uint8_t k_units[R_COUNT] = { 7, 3, 4, 3, 3, 3, 3, 3, 4 };

typedef struct { char from[24], to[24]; float t0; } amt;
static struct {
    ui_list L;
    int start[R_COUNT];
    int nunits;
    float t0;
    char version[40], psram[16], sram[16];
    amt live[R_COUNT];          /* dot-matrix values that morph */
    float next_upd;
    int inited;
} A;

static void amt_set(amt *m, const char *s, int init)
{
    if (init) { snprintf(m->to, sizeof m->to, "%s", s); memcpy(m->from, m->to, sizeof m->from); m->t0 = -100; return; }
    if (!strcmp(m->to, s)) return;
    memcpy(m->from, m->to, sizeof m->from);
    snprintf(m->to, sizeof m->to, "%s", s);
    m->t0 = nos_t;
}

static void fmt_mem(uint32_t b, char *o, int n)
{
    if (b >= 1024u * 1024u) snprintf(o, (size_t)n, "%.1f MB", b / (1024.0 * 1024.0));
    else snprintf(o, (size_t)n, "%u KB", (unsigned)(b / 1024u));
}

static void about_values(int init)
{
    char b[24];
    uint32_t ps = 0, in = 0;
    hal_mem_info(&ps, &in);
    fmt_mem(ps, A.psram, sizeof A.psram);
    fmt_mem(in, A.sram, sizeof A.sram);
    uint32_t s = hal_millis() / 1000u;
    unsigned h = s / 3600u, m = (s / 60u) % 60u, sec = s % 60u;
    if (h >= 24) snprintf(b, sizeof b, "%uD %02uH %02uM", h / 24u, h % 24u, m);
    else snprintf(b, sizeof b, "%u:%02u:%02u", h, m, sec);
    amt_set(&A.live[R_UPTIME], b, init);
    hal_power pw;
    hal_power_get(&pw);
    int mv = pw.rail_mv > 0 ? pw.rail_mv : hal_battery_mv();
    if (mv > 0) snprintf(b, sizeof b, "%.2f", mv / 1000.0f);     /* the V is a label beside it */
    else snprintf(b, sizeof b, UI_NONE);
    amt_set(&A.live[R_RAIL], b, init);
}

static void about_enter(void)
{
    if (!A.inited || !nos_revealed()) {
        int u = 0;
        for (int i = 0; i < R_COUNT; i++) { A.start[i] = u; u += k_units[i]; }
        A.nunits = u;
        ui_list_init(&A.L, u, AU, ATOP, 0, 0);
        A.L.bottom_pad = GFX_H / 2 - 60;
        ui_list_set_n(&A.L, u);
        A.L.bar_t = -10.0f;
        A.t0 = nos_enter_t();
        A.inited = 1;
    }
    snprintf(A.version, sizeof A.version, "%s  (%s)", ABOUT_VERSION, __DATE__);
    about_values(1);
    A.next_upd = nos_t + 1.0f;
}

static void about_tick(float dt)
{
    ui_list_tick(&A.L, dt);
    if (nos_t >= A.next_upd) { about_values(0); A.next_upd = nos_t + 1.0f; }
}

static float afade(float yc)
{
    float d = fabsf(yc - UI_CY);
    float f = 1.0f - 0.7f * m_smoothstep(140.0f, 230.0f, d);
    return floorf(f * 16.0f + 0.5f) / 16.0f;
}

static void draw_logo(gfx_fb *fb, float cx, float top, float k)
{
    float t = fmaxf(nos_t - A.t0, 0);
    float x0 = cx - (LOGO_COLS * LOGO_PITCH) * 0.5f + LOGO_PITCH * 0.5f;
    /* glint: a diagonal band of dots swelling 15 % as it crosses, every 5 s after the build-in */
    float g = -10.0f;
    if (t > 1.0f) {
        float ph = fmodf(t - 1.0f, 5.0f);
        if (ph < 1.2f) g = ph / 1.2f * (LOGO_COLS + 10.0f) - 5.0f;
    }
    for (int r = 0; r < 7; r++)
        for (int c = 0; c < LOGO_COLS; c++) {
            if (k_logo[r][c] != '#') continue;
            float delay = m_hash01((unsigned)(r * 31 + c), 7u) * 0.28f;
            float p = m_clamp((t - delay) / 0.12f, 0, 1);
            p = floorf(p * 8.0f + 0.5f) / 8.0f;
            if (p <= 0) continue;
            float gd = fabsf((c + r * 0.6f) - g);
            float glow = gd < 2.5f ? 1.0f - gd / 2.5f : 0.0f;
            glow = floorf(glow * 4.0f + 0.5f) / 4.0f;
            float d = LOGO_DOT * p * (1.0f + 0.15f * glow);
            uint16_t col = ui_dim(C_WHITE, k);
            dot_disc(fb, x0 + c * LOGO_PITCH, top + (r + 0.5f) * LOGO_PITCH, d, col);
        }
}

static const char *row_label(int r)
{
    switch (r) {
    case R_VERSION: return "VERSION";
    case R_BOARD: return "BOARD";
    case R_DISPLAY: return "DISPLAY";
    case R_PSRAM: return "PSRAM FREE";
    case R_SRAM: return "INTERNAL RAM FREE";
    case R_UPTIME: return "UPTIME";
    case R_RAIL: return "BATTERY RAIL";
    case R_FONTS: return "TYPE";
    default: return "";
    }
}

static float row_bk(int r)
{
    /* the logo leads; rows follow 34 ms apart and are all in by ~0.37 s */
    float bt = nos_t - A.t0 - (r > 0 ? 0.20f + (r < 5 ? r : 5) * 0.034f : 0);
    float bk = m_clamp(bt / 0.2f, 0, 1);
    return floorf(bezier_ease(&EASE_STANDARD, bk) * 8.0f + 0.5f) / 8.0f;
}

static void about_draw(gfx_fb *fb)
{
    float off = floorf(ui_list_offset(&A.L) + 0.5f);
    ui_title(fb, "ABOUT", off);
    for (int r = 0; r < R_COUNT; r++) {
        float h = (float)(k_units[r] * AU);
        float y = ATOP + A.start[r] * AU - off;
        if (y + h < 0) continue;
        if (y > GFX_H) break;
        float bk = row_bk(r);
        if (bk <= 0) continue;
        float yy = y + floorf((1.0f - bk) * 12.0f + 0.5f);
        float k = afade(yy + h * 0.5f) * (r == R_LOGO ? 1.0f : bk);
        if (r == R_LOGO) {
            draw_logo(fb, UI_CX, yy + LOGO_Y, afade(yy + LOGO_Y + 42));
            float tb = yy + LOGO_Y + 7 * LOGO_PITCH + 38;          /* the tagline comes in with row 1 */
            float tk = afade(tb) * row_bk(R_VERSION);
            if (tk > 0) ui_label(fb, UI_CX, (int)tb, "NOTHING-INSPIRED WATCH OS", ui_dim(C_DIS, tk), UI_CENTER);
            continue;
        }
        ui_label(fb, UI_CX, (int)(yy + 22), row_label(r), ui_dim(C_DIS, k), UI_CENTER);
        int vb = (int)(yy + 54);
        switch (r) {
        case R_VERSION: ui_text(fb, UI_F_BODY, UI_CX, vb, A.version, ui_dim(C_PRI, k), UI_CENTER, 0); break;
        case R_BOARD:     /* the exact part name in two ~200 px halves, clear of the glass */
            ui_text(fb, UI_F_BODY, UI_CX, vb, "ESP32-S3-Touch-", ui_dim(C_PRI, k), UI_CENTER, 0);
            ui_text(fb, UI_F_BODY, UI_CX, vb + 28, "AMOLED-1.43-B", ui_dim(C_PRI, k), UI_CENTER, 0);
            break;
        case R_PSRAM: ui_text(fb, UI_F_BODY, UI_CX, vb, A.psram, ui_dim(C_PRI, k), UI_CENTER, 0); break;
        case R_SRAM: ui_text(fb, UI_F_BODY, UI_CX, vb, A.sram, ui_dim(C_PRI, k), UI_CENTER, 0); break;
        case R_DISPLAY: ui_text(fb, UI_F_BODY, UI_CX, vb, "466 x 466 AMOLED, 30 fps", ui_dim(C_PRI, k), UI_CENTER, 0); break;
        case R_FONTS:
            ui_text(fb, UI_F_BODY, UI_CX, vb, "Space Grotesk, Space Mono", ui_dim(C_PRI, k), UI_CENTER, 0);
            ui_label(fb, UI_CX, vb + 30, "SIL OPEN FONT LICENSE 1.1", ui_dim(C_SEC, k), UI_CENTER);
            break;
        default: {
            const amt *m = &A.live[r];
            dot_style st = { DOT_FONT_5X7, UI_SUB_PITCH, UI_SUB_PITCH * 0.8f, ui_dim(C_PRI, k), 0, 1, 1 };
            float p = (nos_t - m->t0) / 0.28f;
            float ty = yy + 34;
            float cx = UI_CX;
            int unit = r == R_RAIL && strcmp(m->to, UI_NONE);
            if (unit) {                  /* digits in dots, the unit as a caps label beside them */
                int wd = dot_text_width(&st, m->to), wu = ui_label_width("V");
                float x0 = UI_CX - (wd + 8 + wu) * 0.5f;
                cx = floorf(x0 + wd * 0.5f + 0.5f);
                ui_label(fb, (int)(x0 + wd + 8), (int)(ty + dot_text_height(&st)), "V", ui_dim(C_SEC, k), UI_LEFT);
            }
            if (p >= 1) dot_text_a(fb, &st, cx, ty, m->to, 1);
            else dot_text_morph(fb, &st, cx, ty, m->from, m->to, 1, p);
        }
        }
    }
    float content = (float)(ATOP + A.nunits * AU + A.L.bottom_pad);
    ui_scrollbar(fb, A.L.sc.x, content, GFX_H, A.L.bar_alpha);
}

static int about_input(const nos_input *in)
{
    if (in->type == IN_TAP) {           /* nothing to open: a tap only stops a fling */
        nos_input up = *in; up.type = IN_UP;
        return ui_list_input(&A.L, &up);
    }
    return ui_list_input(&A.L, in);
}

const nos_screen scr_about = { "ABOUT", about_enter, 0, about_tick, about_draw, about_input, 0 };
