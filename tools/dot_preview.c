/* dot_preview.c — specimen sheet + self-check for nos/dot.c.
 *
 *   gcc -O2 -std=c99 -Wall -Wextra -DDOT_STATS -Icore -Inos tools/dot_preview.c \
 *       nos/dot.c nos/dot_glyphs.c core/gfx.c -lm -o build/nos/dot_preview.exe
 *   build/nos/dot_preview.exe          (run from the repo root)
 *
 * Writes build/nos/dot_specimen.png (the sheet) and build/nos/dot_review_*.png
 * (zoomed glyph / icon tables for design review), then checks:
 *   - every glyph / icon art string has a whole number of rows of its width;
 *   - record mode: no pixel outside the recorded damage boxes changes;
 *   - 16-row stripes, a column clip window, a column band buffer (stride/x_base)
 *     and an ox/oy content offset all reproduce the full-frame render exactly.
 * Exit code 0 = all checks passed. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gfx.h"
#include "dot.h"
#include "../tools/png_write.h"

void *hal_big_alloc(uint32_t bytes) { return malloc(bytes); }

#ifdef DOT_STATS
extern unsigned long dot_stat_solid, dot_stat_blend, dot_stat_stamps;
#endif

/* ---- canvas: a big RGB565 sheet composed from 466-wide panels ----------- */
typedef struct { int w, h; uint16_t *px; } canvas;

static canvas cv_new(int w, int h, uint16_t bg)
{
    canvas c = { w, h, malloc(sizeof(uint16_t) * (size_t)w * h) };
    for (int i = 0; i < w * h; i++) c.px[i] = bg;
    return c;
}

static uint16_t g_panel[GFX_W * 1400];

static gfx_fb panel_begin(int h, uint16_t bg)
{
    for (int i = 0; i < GFX_W * h; i++) g_panel[i] = bg;
    gfx_fb fb;
    memset(&fb, 0, sizeof fb);
    fb.px = g_panel; fb.h = h; fb.cx0 = 0; fb.cx1 = GFX_W;
    return fb;
}

static void panel_put(canvas *c, int x, int y, int h)
{
    for (int r = 0; r < h && y + r < c->h; r++)
        for (int k = 0; k < GFX_W && x + k < c->w; k++)
            c->px[(y + r) * c->w + x + k] = g_panel[r * GFX_W + k];
}

static const uint16_t W = GFX_WHITE, GREY = GFX_RGB(60, 60, 60), DIM = GFX_RGB(110, 110, 110);
static const uint16_t RED = GFX_RGB(215, 25, 33);

static dot_style sty(int font, float pitch, uint16_t col)
{
    dot_style s;
    memset(&s, 0, sizeof s);
    s.font = (uint8_t)font; s.pitch = pitch; s.dot = pitch * 0.78f; s.color = col;
    return s;
}

static void label(gfx_fb *fb, float x, float y, const char *t)
{
    dot_style s = sty(DOT_FONT_5X7, 2.0f, DIM);
    dot_text(fb, &s, x, y, t);
}

/* ---- art validation --------------------------------------------------------- */
typedef struct { uint8_t w; const char *art; } dot_art;
extern const dot_art dot_art_5x7[96], dot_art_tall[96], dot_art_3x5[96];
extern const char *const dot_art_icons[DOT_ICON_COUNT];

static int check_art(void)
{
    int bad = 0;
    const dot_art *fonts[3] = { dot_art_5x7, dot_art_tall, dot_art_3x5 };
    const int rows[3] = { 7, 9, 5 };
    for (int f = 0; f < 3; f++)
        for (int i = 0; i < 96; i++) {
            if (!fonts[f][i].art) continue;
            size_t n = strlen(fonts[f][i].art);
            int w = fonts[f][i].w;
            if (!w || n % (size_t)w || (int)(n / (size_t)w) < rows[f] || (int)(n / (size_t)w) > rows[f] + 2) {
                printf("ART ERROR font %d glyph '%c' (slot %d): len %zu width %d\n", f, i + 32, i, n, w);
                bad++;
            }
        }
    for (int i = 1; i < DOT_ICON_COUNT; i++) {
        if (!dot_art_icons[i]) { printf("ART ERROR icon %d missing\n", i); bad++; continue; }
        size_t n = strlen(dot_art_icons[i]);
        if (n != 121) { printf("ART ERROR icon %d: len %zu\n", i, n); bad++; }
    }
    return bad;
}

static const char *icon_names[DOT_ICON_COUNT] = {
    "", "BELL", "HEART", "STEPS", "FLAME", "SUN", "MOON", "CLOUD", "RAIN", "SNOW", "WIND", "THERMO", "DROP",
    "COMPASS", "PIN", "ROUTE", "SAT", "SIGNAL", "MSG", "MUSIC", "PLAY", "PAUSE", "NEXT", "PREV",
    "TIMER", "STOPWTCH", "ALARM", "MIC", "SPEAKER", "TORCH", "SETTINGS", "BATTERY", "BOLT", "BT", "WIFI",
    "PHONE", "FIND", "DND", "BRIGHT", "LOCK", "CHECK", "CROSS", "PLUS", "MINUS", "UP", "DOWN",
    "LEFT", "RIGHT", "GAUGE", "MOUNTAIN", "LEAF", "BREATHE", "CALENDAR", "GRID", "SD", "INFO", "LEVEL",
    "HAND", "EYE", "GLOBE", "CAMERA", "FACE", "SMILE",
};

static const char *ASCII1 = " !\"#$%&'()*+,-./0123456789:;<=>?@";
static const char *ASCII2 = "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`";
static const char *ASCII3 = "abcdefghijklmnopqrstuvwxyz{|}~\xC2\xB0";

/* ---- review sheets (zoomed) --------------------------------------------- */
/* Glyph table of one font: every slot in a grid, pitch `pitch`, faint cell box. */
static void review_grid(int font, float pitch, const char *chars, int per_row, const char *path)
{
    int fw = font == DOT_FONT_3X5 ? 5 : 5, fh = font == DOT_FONT_TALL ? 9 : font == DOT_FONT_3X5 ? 5 : 7;
    int cw = (int)(pitch * (float)(fw + 2)), ch = (int)(pitch * (float)(fh + 4));
    int n = 0;
    const unsigned char *q = (const unsigned char *)chars;
    while (*q) { if (*q != 0xC2) n++; q++; }
    int rows = (n + per_row - 1) / per_row;
    int pw = cw * per_row + 20, ph = ch * rows + 20;
    int npan = (pw + GFX_W - 1) / GFX_W;
    canvas c = cv_new(npan * GFX_W, ph, GFX_BLACK);
    for (int pnl = 0; pnl < npan; pnl++) {
        gfx_fb fb = panel_begin(ph, GFX_BLACK);
        fb.ox = -pnl * GFX_W;
        int k = 0;
        for (const unsigned char *p = (const unsigned char *)chars; *p; p++) {
            char one[3] = { (char)*p, 0, 0 };
            if (*p == 0xC2) { one[1] = (char)p[1]; p++; }
            int cx = 10 + (k % per_row) * cw, cy = 10 + (k / per_row) * ch;
            /* baseline guide + cap box */
            gfx_fill_rect(&fb, cx, cy + (int)(pitch * (float)fh), cw - (int)pitch, 1, GFX_RGB(70, 20, 20));
            dot_style s = sty(font, pitch, W);
            s.mono = 1;
            dot_text(&fb, &s, (float)cx, (float)cy, one);
            k++;
        }
        panel_put(&c, pnl * GFX_W, 0, ph);
    }
    png_write_565(path, c.w, c.h, c.px, c.w);
    free(c.px);
}

static void review_fonts(void)
{
    canvas c = cv_new(GFX_W * 3, 1300, GFX_BLACK);
    /* 5x7 in three panels at pitch 7 */
    const char *chunks[] = { " !\"#$%&'()*+,-./", "0123456789:;<=>?@", "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ",
                             "[\\]^_`{|}~\xC2\xB0", "abcdefghijklm", "nopqrstuvwxyz",
                             "The quick brown", "fox jumps over", "lazy dog. 23\xC2\xB0", "Hello, Nothing!",
                             "gyp jq 12:45" };
    gfx_fb fb = panel_begin(1300, GFX_BLACK);
    dot_style s = sty(DOT_FONT_5X7, 6.0f, W);
    s.ghost = GREY;
    float y = 8;
    for (int i = 0; i < 12; i++) { dot_text(&fb, &s, 8, y, chunks[i]); y += 6 * 10.5f; }
    panel_put(&c, 0, 0, 1300);

    fb = panel_begin(1300, GFX_BLACK);
    s = sty(DOT_FONT_TALL, 7.0f, W);
    s.ghost = GREY;
    const char *tchunks[] = { "0123456789", "+-%/.,:\xC2\xB0", "ABCDEFGHI", "JKLMNOPQR", "STUVWXYZ" };
    y = 8;
    for (int i = 0; i < 5; i++) { dot_text(&fb, &s, 8, y, tchunks[i]); y += 7 * 11; }
    s = sty(DOT_FONT_TALL, 4.0f, W);
    for (int i = 0; i < 5; i++) { dot_text(&fb, &s, 8, y, tchunks[i]); y += 4 * 11; }
    s = sty(DOT_FONT_3X5, 6.0f, W);
    s.ghost = GREY;
    const char *schunks[] = { "0123456789", "ABCDEFGHIJKLM", "NOPQRSTUVWXYZ", "-.:%/+\xC2\xB0" };
    for (int i = 0; i < 4; i++) { dot_text(&fb, &s, 8, y, schunks[i]); y += 6 * 7; }
    panel_put(&c, GFX_W, 0, 1300);

    fb = panel_begin(1300, GFX_BLACK);
    s = sty(DOT_FONT_3X5, 3.0f, W);
    y = 8;
    for (int i = 0; i < 4; i++) { dot_text(&fb, &s, 8, y, schunks[i]); y += 3 * 7; }
    dot_text(&fb, &s, 8, y, "BPM 72  STEPS 8421  BAT 87%"); y += 3 * 8;
    s = sty(DOT_FONT_5X7, 3.0f, W);
    for (int i = 0; i < 12; i++) { dot_text(&fb, &s, 8, y, chunks[i]); y += 3 * 10; }
    s = sty(DOT_FONT_5X7, 2.4f, W);
    dot_text(&fb, &s, 8, y, "Small 5x7 at 2.4 px: Monday 12 Oct"); y += 2.4f * 10;
    s = sty(DOT_FONT_TALL, 12.0f, W);
    s.mono = 1;
    dot_text(&fb, &s, 8, y, "0123"); y += 12 * 10;
    dot_text(&fb, &s, 8, y, "4567"); y += 12 * 10;
    dot_text(&fb, &s, 8, y, "89:1");
    panel_put(&c, GFX_W * 2, 0, 1300);
    png_write_565("build/nos/dot_review_fonts.png", c.w, c.h, c.px, c.w);
    free(c.px);
}

static void review_clock(void)
{
    canvas c = cv_new(GFX_W * 2, 760, GFX_BLACK);
    const char *a[] = { "12:45", "09:30", "67:18", "20:16" };
    for (int pnl = 0; pnl < 2; pnl++) {
        gfx_fb fb = panel_begin(760, GFX_BLACK);
        dot_style s = sty(DOT_FONT_TALL, 10, W);
        s.mono = 1;
        if (pnl) s.ghost = GFX_RGB(40, 40, 40);
        for (int i = 0; i < 4; i++) dot_text_a(&fb, &s, 233, 10.0f + (float)i * 105.0f, a[i], 1);
        dot_style t = sty(DOT_FONT_3X5, 7, W);
        if (pnl == 0) {
            dot_text(&fb, &t, 10, 440, "0123456789:.");
            dot_text(&fb, &t, 10, 490, "ABCDEFGHIJKLM");
            dot_text(&fb, &t, 10, 540, "NOPQRSTUVWXYZ");
            dot_text(&fb, &t, 10, 590, "-%+/Â° BAT 87%");
        } else {
            dot_style u = sty(DOT_FONT_5X7, 5, W);
            dot_text(&fb, &u, 10, 440, "@&$#*? Hello");
            dot_text(&fb, &u, 10, 490, "Monday 12 Oct");
            dot_text(&fb, &u, 10, 540, "Heart 72 bpm");
            dot_text(&fb, &u, 10, 590, "jog: 4.2 km/h");
        }
        panel_put(&c, pnl * GFX_W, 0, 760);
    }
    png_write_565("build/nos/dot_review_clock.png", c.w, c.h, c.px, c.w);
    free(c.px);
}

static void review_anim(void)
{
    canvas c = cv_new(GFX_W * 2, 900, GFX_BLACK);
    for (int pnl = 0; pnl < 2; pnl++) {
        gfx_fb fb = panel_begin(900, GFX_BLACK);
        float y = 8;
        if (pnl == 0) {
            for (int m = 1; m <= 4; m++) {
                for (int k = 0; k < 4; k++) {
                    dot_style s = sty(DOT_FONT_TALL, 4, W);
                    s.ghost = GFX_RGB(40, 40, 40); s.mono = 1;
                    dot_text_anim(&fb, &s, 8.0f + (float)k * 115.0f, y, "12:45", 0, 0.15f + 0.23f * (float)k, m, 11);
                }
                y += 48;
                for (int k = 0; k < 4; k++)
                    dot_icon_anim(&fb, DOT_ICON_HEART, 40.0f + (float)k * 115.0f, y + 30, 5, 3.9f, RED, 0.15f + 0.23f * (float)k, m);
                y += 75;
            }
        } else {
            for (int k = 0; k <= 6; k++) {
                dot_style s = sty(DOT_FONT_TALL, 5, W);
                s.mono = 1; s.ghost = GFX_RGB(40, 40, 40);
                dot_text_morph(&fb, &s, 8.0f + (float)(k % 2) * 230.0f, y, "12:59", "13:00", 0, (float)k / 6.0f);
                if (k % 2) y += 54;
            }
            y += 60;
            for (int k = 0; k <= 5; k++) {
                dot_style s = sty(DOT_FONT_5X7, 4, W);
                dot_text_morph(&fb, &s, 8, y, "Mon", "Tue", 0, (float)k / 5.0f);
                dot_text_morph(&fb, &s, 120, y, "72", "105", 0, (float)k / 5.0f);
                y += 34;
            }
        }
        panel_put(&c, pnl * GFX_W, 0, 900);
    }
    png_write_565("build/nos/dot_review_anim.png", c.w, c.h, c.px, c.w);
    free(c.px);
}

static void review_icons(int pitch, const char *path)
{
    int cols = pitch > 4 ? 6 : 10;
    int cell = pitch * 11 + (pitch > 4 ? 20 : 12);
    int rows = (DOT_ICON_COUNT - 1 + cols - 1) / cols;
    int ph = rows * (cell + 16) + 10;
    canvas c = cv_new(GFX_W, ph, GFX_BLACK);
    gfx_fb fb = panel_begin(ph, GFX_BLACK);
    for (int i = 1; i < DOT_ICON_COUNT; i++) {
        int k = i - 1, cx = (k % cols) * cell + cell / 2 + 4, cy = (k / cols) * (cell + 16) + cell / 2 + 4;
        dot_icon(&fb, i, (float)cx, (float)cy, (float)pitch, pitch * 0.78f, W);
        dot_style s = sty(DOT_FONT_3X5, 1.6f, DIM);
        dot_text_a(&fb, &s, (float)cx, (float)(cy + cell / 2 + 2), icon_names[i], 1);
    }
    panel_put(&c, 0, 0, ph);
    png_write_565(path, c.w, c.h, c.px, c.w);
    free(c.px);
}

/* ---- the specimen sheet ------------------------------------------------- */
static void specimen(void)
{
    const int PH = 1200;
    canvas c = cv_new(GFX_W * 3, PH, GFX_BLACK);

    /* panel 1: fonts */
    gfx_fb fb = panel_begin(PH, GFX_BLACK);
    float y = 10;
    label(&fb, 10, y, "5X7  PITCH 3 / 4 / 5"); y += 22;
    float ps[3] = { 3, 4, 5 };
    for (int k = 0; k < 3; k++) {
        dot_style s = sty(DOT_FONT_5X7, ps[k], W);
        const char *lines[3] = { ASCII1, ASCII2, ASCII3 };
        int nl = k == 0 ? 3 : 2;
        for (int i = 0; i < nl; i++) {
            const char *t = lines[i];
            char buf[64];
            if (k > 0) {  /* larger pitch: split lines to fit */
                snprintf(buf, sizeof buf, "%.*s", k == 1 ? 24 : 18, t);
                t = buf;
            }
            dot_text(&fb, &s, 10, y, t);
            y += ps[k] * 10;
        }
    }
    dot_style s = sty(DOT_FONT_5X7, 4, W);
    dot_text(&fb, &s, 10, y, "Quick brown fox jumps"); y += 40;
    label(&fb, 10, y, "TALL 5X9  PITCH 4 / 6"); y += 22;
    s = sty(DOT_FONT_TALL, 4, W);
    dot_text(&fb, &s, 10, y, "0123456789 +-%/.,:\xC2\xB0"); y += 44;
    dot_text(&fb, &s, 10, y, "ABCDEFGHIJKLMNOPQRS"); y += 44;
    dot_text(&fb, &s, 10, y, "TUVWXYZ 72 BPM"); y += 44;
    s = sty(DOT_FONT_TALL, 6, W);
    s.ghost = GREY;
    dot_text(&fb, &s, 10, y, "8421 24\xC2\xB0"); y += 66;
    label(&fb, 10, y, "3X5  PITCH 3 / 5"); y += 22;
    s = sty(DOT_FONT_3X5, 3, W);
    dot_text(&fb, &s, 10, y, "0123456789 ABCDEFGHIJKLMNOPQRSTUVWXYZ :.-%"); y += 24;
    s = sty(DOT_FONT_3X5, 5, W);
    dot_text(&fb, &s, 10, y, "0123456789 :.-%"); y += 34;
    dot_text(&fb, &s, 10, y, "ABCDEFGHIJKLM"); y += 34;
    dot_text(&fb, &s, 10, y, "NOPQRSTUVWXYZ"); y += 40;
    label(&fb, 10, y, "MONO + GHOST"); y += 22;
    s = sty(DOT_FONT_5X7, 5, W);
    s.mono = 1; s.ghost = GREY;
    dot_text(&fb, &s, 10, y, "10:41 PM"); y += 50;
    panel_put(&c, 0, 0, PH);

    /* panel 2: icons + clock */
    fb = panel_begin(PH, GFX_BLACK);
    y = 10;
    label(&fb, 10, y, "ICONS  PITCH 3"); y += 20;
    for (int i = 1; i < DOT_ICON_COUNT; i++) {
        int k = i - 1;
        dot_icon(&fb, i, 30.0f + (float)(k % 11) * 40.0f, y + 18 + (float)(k / 11) * 42.0f, 3, 2.34f, W);
    }
    y += 6 * 42 + 10;
    label(&fb, 10, y, "ICONS  PITCH 5"); y += 20;
    for (int i = 1; i < DOT_ICON_COUNT; i++) {
        int k = i - 1;
        dot_icon(&fb, i, 38.0f + (float)(k % 7) * 65.0f, y + 30 + (float)(k / 7) * 64.0f, 5, 3.9f, W);
    }
    panel_put(&c, GFX_W, 0, PH);

    /* panel 3: clock, reveals, morph, rings */
    fb = panel_begin(PH, GFX_BLACK);
    y = 10;
    label(&fb, 10, y, "TALL PITCH 14  MONO"); y += 22;
    s = sty(DOT_FONT_TALL, 14, W);
    s.mono = 1;
    dot_text_a(&fb, &s, GFX_W / 2.0f, y, "12:45", 1); y += 14 * 9 + 14;
    label(&fb, 10, y, "REVEAL SCAN/RANDOM/RAIN/CENTER P.25 .5 .75"); y += 20;
    for (int m = 1; m <= 4; m++) {
        for (int k = 0; k < 3; k++) {
            dot_style r = sty(DOT_FONT_TALL, 4, W);
            r.mono = 1; r.ghost = GREY;
            dot_text_anim(&fb, &r, 10.0f + (float)k * 150.0f, y, "12:45", 0, 0.25f * (float)(k + 1), m, 7);
        }
        y += 44;
    }
    label(&fb, 10, y, "MORPH 8 TO 9   P 0 .2 .4 .6 .8 1"); y += 20;
    for (int k = 0; k <= 5; k++) {
        dot_style r = sty(DOT_FONT_TALL, 7, W);
        r.ghost = GREY;
        dot_text_morph(&fb, &r, 14.0f + (float)k * 74.0f, y, "8", "9", 0, (float)k * 0.2f);
    }
    y += 7 * 9 + 14;
    label(&fb, 10, y, "MORPH 12:59 TO 13:00  P .3  /  13:00"); y += 20;
    {
        dot_style r = sty(DOT_FONT_TALL, 5, W);
        r.mono = 1;
        dot_text_morph(&fb, &r, 10, y, "12:59", "13:00", 0, 0.3f);
        dot_text(&fb, &r, 250, y, "13:00");
    }
    y += 5 * 9 + 14;
    label(&fb, 10, y, "RINGS / BAR / COLUMNS"); y += 20;
    float ry = y + 110;
    dot_ring(&fb, 120, ry, 100, 60, 6.0f, 0.0f, 37.5f, W, GREY);
    dot_ring(&fb, 120, ry, 82, 40, 5.0f, 5.25f, 22.6f, RED, 0);
    dot_ring(&fb, 120, ry, 64, 24, 7.0f, 0.0f, 9.5f, W, GREY);
    {
        dot_style r = sty(DOT_FONT_TALL, 4, W);
        dot_text_a(&fb, &r, 120, ry - 18, "62%", 1);
    }
    dot_ring(&fb, 340, ry, 90, 12, 14.0f, 0.0f, 7.33f, W, GREY);
    y = ry + 120;
    dot_bar(&fb, 20, y, 30, 8, 6, 17.4f, W, GREY);
    y += 20;
    dot_bar(&fb, 20, y, 30, 8, 6, 8.75f, RED, GREY);
    y += 20;
    uint8_t vals[36];
    for (int i = 0; i < 36; i++) vals[i] = (uint8_t)(127 + 120 * gfx_fast_sin((float)i * 0.45f) * gfx_fast_cos((float)i * 0.13f));
    dot_columns(&fb, 20, y, 36, 10, 7, 5.2f, vals, W, GREY);
    y += 80;
    for (int i = 1; i <= 6; i++) dot_disc(&fb, 20.0f + (float)i * (float)i * 4.0f, y + 20, (float)i * 3.0f, W);
    panel_put(&c, GFX_W * 2, 0, PH);

    if (png_write_565("build/nos/dot_specimen.png", c.w, c.h, c.px, c.w)) printf("PNG write failed\n");
    free(c.px);
}

/* ---- record / clip / stripe self-check ----------------------------------- */
#define NCASE 26
static void draw_case(gfx_fb *fb, int i)
{
    dot_style s = sty(DOT_FONT_5X7, 4, W);
    switch (i) {
    case 0: dot_text(fb, &s, 20, 30, "Hello, gyp!"); break;
    case 1: s.ghost = GREY; s.mono = 1; dot_text_a(fb, &s, 233, 200, "12:45 PM", 1); break;
    case 2: s = sty(DOT_FONT_TALL, 14, W); s.mono = 1; dot_text_a(fb, &s, 233, 170, "12:45", 1); break;
    case 3: s = sty(DOT_FONT_TALL, 9.5f, W); dot_text_a(fb, &s, 460, 100, "98%", 2); break;
    case 4: s = sty(DOT_FONT_3X5, 3.3f, W); dot_text(fb, &s, -7.5f, 455.2f, "BAT 87% ABC"); break;
    case 5: s.ghost = GREY; dot_text_anim(fb, &s, 30, 80, "NOTHING", 0, 0.37f, DOT_REVEAL_SCAN, 0); break;
    case 6: dot_text_anim(fb, &s, 30, 80, "Random", 0, 0.5f, DOT_REVEAL_RANDOM, 3); break;
    case 7: s = sty(DOT_FONT_TALL, 7, W); dot_text_anim(fb, &s, 233, 300, "07:30", 1, 0.61f, DOT_REVEAL_RAIN, 0); break;
    case 8: s = sty(DOT_FONT_TALL, 7, W); s.ghost = GREY; dot_text_anim(fb, &s, 233, 300, "-12\xC2\xB0", 1, 0.44f, DOT_REVEAL_CENTER, 0); break;
    case 9: s = sty(DOT_FONT_TALL, 10, W); s.mono = 1; dot_text_morph(fb, &s, 233, 120, "12:59", "13:00", 1, 0.45f); break;
    case 10: s = sty(DOT_FONT_5X7, 6, W); dot_text_morph(fb, &s, 100, 400, "Mon", "Tuesday", 0, 0.3f); break;
    case 11: dot_ring(fb, 233, 233, 215, 60, 6, 0, 41.3f, W, GREY); break;
    case 12: dot_ring(fb, 233, 233, 120, 37, 9.3f, 30.5f, 44.2f, RED, 0); break;
    case 13: dot_ring(fb, 100, 100, 40, 8, 12, -2.5f, 3.75f, W, GREY); break;
    case 14: dot_bar(fb, 40.3f, 440.7f, 25, 15.2f, 11, 13.6f, W, GREY); break;
    case 15: { uint8_t v[20]; for (int k = 0; k < 20; k++) v[k] = (uint8_t)(k * 13); dot_columns(fb, 50, 250, 20, 8, 9, 7, v, W, GREY); } break;
    case 16: dot_icon(fb, DOT_ICON_SETTINGS, 233.4f, 233.7f, 5, 3.9f, W); break;
    case 17: dot_icon_anim(fb, DOT_ICON_HEART, 300, 300, 4.5f, 3.5f, RED, 0.5f, DOT_REVEAL_RANDOM); break;
    case 18: dot_icon_anim(fb, DOT_ICON_GLOBE, 5, 233, 6, 6.5f, W, 0.8f, DOT_REVEAL_CENTER); break;
    case 19: dot_disc(fb, 233.3f, 15.6f, 37.3f, W); break;
    case 20: dot_disc(fb, 400.9f, 400.1f, 1.6f, W); break;
    case 21: dot_disc(fb, 233, 233, 55, GFX_GREEN); break;   /* > 40 px fallback */
    case 22: s = sty(DOT_FONT_5X7, 3, W); s.gap = 2; dot_text(fb, &s, 200, 50, "wide gap ~|{}"); break;
    case 23: s = sty(DOT_FONT_5X7, 12, W); s.dot = 15; dot_text(fb, &s, 10, 340, "Big"); break;
    case 24: s = sty(DOT_FONT_TALL, 16, W); dot_text_anim(fb, &s, 233, 160, "9", 1, 0.2f, DOT_REVEAL_NONE, 0); break;
    case 25: dot_bar(fb, 460, 10, 12, -9, 6, 4.5f, W, 0); break;
    }
}

static uint16_t pat(int x, int y) { return (uint16_t)(((x * 37) ^ (y * 101)) & 0x7BEF); }

static uint16_t g_full[GFX_W * GFX_H], g_tmp[GFX_W * GFX_H], g_stripe[GFX_W * 16];

static void fill_pat(uint16_t *b, int y0, int rows, int ox, int oy, int flat)
{
    (void)ox; (void)oy;
    for (int r = 0; r < rows; r++)
        for (int x = 0; x < GFX_W; x++) b[r * GFX_W + x] = flat ? 0x1082 : pat(x, y0 + r);
}

static gfx_fb fb_full(uint16_t *b)
{
    gfx_fb fb;
    memset(&fb, 0, sizeof fb);
    fb.px = b; fb.h = GFX_H; fb.cx0 = 0; fb.cx1 = GFX_W;
    return fb;
}

static int self_check(void)
{
    int fails = 0;
    static gfx_rec rec;
    for (int i = 0; i < NCASE; i++) {
        /* record */
        gfx_fb fb = fb_full(g_full);
        rec.n = 0; rec.overflow = 0;
        fb.rec = &rec;
        draw_case(&fb, i);
        fb.rec = NULL;
        int ncmd = rec.n;
        if (ncmd == 0) { printf("case %d: no damage recorded\n", i); fails++; }
        /* full raster on a pattern */
        fill_pat(g_full, 0, GFX_H, 0, 0, 0);
        draw_case(&fb, i);
        long changed = 0, outside = 0;
        for (int y = 0; y < GFX_H; y++)
            for (int x = 0; x < GFX_W; x++) {
                if (g_full[y * GFX_W + x] == pat(x, y)) continue;
                changed++;
                int in = 0;
                for (int k = 0; k < ncmd && !in; k++)
                    in = x >= rec.c[k].x0 && x < rec.c[k].x1 && y >= rec.c[k].y0 && y < rec.c[k].y1;
                if (!in) { if (outside < 3) printf("case %d: pixel (%d,%d) changed outside damage\n", i, x, y); outside++; }
            }
        if (outside) fails++;
        if (!changed) { printf("case %d: nothing drawn\n", i); fails++; }
        /* stripes of 16 rows */
        long diff = 0;
        for (int y0 = 0; y0 < GFX_H; y0 += 16) {
            int h = GFX_H - y0 < 16 ? GFX_H - y0 : 16;
            fill_pat(g_stripe, y0, h, 0, 0, 0);
            gfx_fb sf;
            memset(&sf, 0, sizeof sf);
            sf.px = g_stripe; sf.h = h; sf.y_base = y0; sf.cx0 = 0; sf.cx1 = GFX_W;
            draw_case(&sf, i);
            for (int r = 0; r < h; r++)
                for (int x = 0; x < GFX_W; x++) diff += g_stripe[r * GFX_W + x] != g_full[(y0 + r) * GFX_W + x];
        }
        if (diff) { printf("case %d: stripes differ from full frame in %ld px\n", i, diff); fails++; }
        /* column clip window */
        fill_pat(g_tmp, 0, GFX_H, 0, 0, 0);
        gfx_fb cf = fb_full(g_tmp);
        cf.cx0 = 101; cf.cx1 = 297;
        draw_case(&cf, i);
        diff = 0;
        for (int y = 0; y < GFX_H; y++)
            for (int x = 0; x < GFX_W; x++) {
                uint16_t want = (x >= 101 && x < 297) ? g_full[y * GFX_W + x] : pat(x, y);
                diff += g_tmp[y * GFX_W + x] != want;
            }
        if (diff) { printf("case %d: clip window wrong in %ld px\n", i, diff); fails++; }
        /* column band buffer: columns [120, 320) packed 200 wide, 16-row stripes */
        diff = 0;
        for (int y0 = 0; y0 < GFX_H; y0 += 16) {
            int h = GFX_H - y0 < 16 ? GFX_H - y0 : 16;
            for (int r = 0; r < h; r++)
                for (int x = 0; x < 200; x++) g_stripe[r * 200 + x] = pat(120 + x, y0 + r);
            gfx_fb bf;
            memset(&bf, 0, sizeof bf);
            bf.px = g_stripe; bf.h = h; bf.y_base = y0; bf.cx0 = 120; bf.cx1 = 320;
            bf.stride = 200; bf.x_base = 120;
            draw_case(&bf, i);
            for (int r = 0; r < h; r++)
                for (int x = 0; x < 200; x++) diff += g_stripe[r * 200 + x] != g_full[(y0 + r) * GFX_W + 120 + x];
        }
        if (diff) { printf("case %d: column band differs in %ld px\n", i, diff); fails++; }
        /* content offset ox/oy on a flat background */
        fill_pat(g_full, 0, GFX_H, 0, 0, 1);
        gfx_fb ff = fb_full(g_full);
        draw_case(&ff, i);
        fill_pat(g_tmp, 0, GFX_H, 0, 0, 1);
        gfx_fb of = fb_full(g_tmp);
        of.ox = 13; of.oy = -9;
        draw_case(&of, i);
        diff = 0;
        for (int y = 0; y < GFX_H; y++)
            for (int x = 0; x < GFX_W; x++) {
                int sx = x - 13, sy = y + 9;
                uint16_t want = (sx >= 0 && sy < GFX_H) ? g_full[sy * GFX_W + sx] : 0x1082;
                if (sx >= 0 && sy < GFX_H) diff += g_tmp[y * GFX_W + x] != want;
            }
        if (diff) { printf("case %d: ox/oy offset differs in %ld px\n", i, diff); fails++; }
    }
    printf("self-check: %d cases, %d failures\n", NCASE, fails);
    return fails;
}

/* ---- performance notes ----------------------------------------------------- */
static void perf(void)
{
    gfx_fb fb = fb_full(g_full);
    dot_style s = sty(DOT_FONT_TALL, 14, W);
    s.mono = 1;
    static gfx_rec rec;
    rec.n = 0;
    fb.rec = &rec;
    dot_text_a(&fb, &s, 233, 170, "12:45", 1);
    fb.rec = NULL;
    long box = (long)(rec.c[0].x1 - rec.c[0].x0) * (rec.c[0].y1 - rec.c[0].y0);
#ifdef DOT_STATS
    dot_stat_solid = dot_stat_blend = dot_stat_stamps = 0;
#endif
    memset(g_full, 0, sizeof g_full);
    /* as the engine runs it: one draw per 16-row stripe */
    clock_t t0 = clock();
    int reps = 2000;
    for (int rep = 0; rep < reps; rep++)
        for (int y0 = 0; y0 < GFX_H; y0 += 16) {
            gfx_fb sf;
            memset(&sf, 0, sizeof sf);
            sf.px = g_stripe; sf.h = 16; sf.y_base = y0; sf.cx0 = 0; sf.cx1 = GFX_W;
            dot_text_a(&sf, &s, 233, 170, "12:45", 1);
        }
    double us = (double)(clock() - t0) / CLOCKS_PER_SEC * 1e6 / reps;
#ifdef DOT_STATS
    printf("perf: TALL p14 \"12:45\": bbox %d x %d = %ld px; per frame %lu stamps, %lu solid px, %lu blended px; host %.1f us/frame (30 stripe calls)\n",
           rec.c[0].x1 - rec.c[0].x0, rec.c[0].y1 - rec.c[0].y0, box,
           dot_stat_stamps / reps, dot_stat_solid / reps, dot_stat_blend / reps, us);
#else
    printf("perf: bbox %ld px, host %.1f us/frame\n", box, us);
#endif
    /* rejection cost: 30 stripe calls of a label that is in none of them */
    dot_style l = sty(DOT_FONT_5X7, 3, W);
    t0 = clock();
    reps = 200000;
    for (int rep = 0; rep < reps; rep++) {
        gfx_fb sf;
        memset(&sf, 0, sizeof sf);
        sf.px = g_stripe; sf.h = 16; sf.y_base = 0; sf.cx0 = 0; sf.cx1 = GFX_W;
        dot_text(&sf, &l, 100, 300, "STEPS 8421");
    }
    printf("perf: rejected label call: host %.0f ns/call\n", (double)(clock() - t0) / CLOCKS_PER_SEC * 1e9 / reps);
}

int main(void)
{
    int bad = check_art();
    specimen();
    review_fonts();
    review_grid(DOT_FONT_5X7, 9, " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`", 16, "build/nos/dot_review_57a.png");
    review_grid(DOT_FONT_5X7, 9, "abcdefghijklmnopqrstuvwxyz{|}~\xC2\xB0", 16, "build/nos/dot_review_57b.png");
    review_grid(DOT_FONT_TALL, 9, "0123456789:.,-+%/\xC2\xB0 ABCDEFGHIJKLMNOPQRSTUVWXYZ", 14, "build/nos/dot_review_tall.png");
    review_grid(DOT_FONT_3X5, 10, "0123456789:.-%+/\xC2\xB0" "ABCDEFGHIJKLMNOPQRSTUVWXYZ", 18, "build/nos/dot_review_35.png");
    review_clock();
    review_anim();
    review_icons(5, "build/nos/dot_review_icons5.png");
    review_icons(3, "build/nos/dot_review_icons3.png");
    bad += self_check();
    perf();
    printf(bad ? "FAILED\n" : "OK\n");
    return bad ? 1 : 0;
}
