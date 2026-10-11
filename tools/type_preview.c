/* type_preview.c — NOS proportional type specimen (nos/fonts) -> build/nos/type_specimen.png
 *
 *   gcc -O2 -std=c99 -Wall -Wextra -Icore -Inos -Inos/fonts tools/type_preview.c \
 *       core/font.c core/gfx.c nos/dot.c nos/dot_glyphs.c nos/fonts/nf_*.c -lm \
 *       -o build/nos/type_preview.exe
 *   build/nos/type_preview.exe       (run from the repo root)
 *
 * Panels: three 466x466 watch mock-ups (round mask shown) and the character sets of
 * every face, all rendered with the real font_draw() / dot_text() paths. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx.h"
#include "font.h"
#include "dot.h"
#include "nos_fonts.h"
#include "../tools/png_write.h"

void *hal_big_alloc(uint32_t bytes) { return malloc(bytes); }

#define SHEET_W (GFX_W * 3 + 40)
#define SHEET_H (GFX_H * 2 + 60)
static uint16_t sheet[SHEET_W * SHEET_H];
static uint16_t panel[GFX_W * GFX_H];

static const uint16_t WHITE = GFX_WHITE, GREY = GFX_RGB(140, 140, 140);
static const uint16_t RED = GFX_RGB(215, 25, 33), CARD = GFX_RGB(24, 24, 24);

static gfx_fb begin(void)
{
    memset(panel, 0, sizeof panel);
    gfx_fb fb;
    memset(&fb, 0, sizeof fb);
    fb.px = panel; fb.h = GFX_H; fb.cx0 = 0; fb.cx1 = GFX_W;
    return fb;
}

/* copy the panel to the sheet; round = shade what the round glass hides */
static void put(int x, int y, int round)
{
    for (int r = 0; r < GFX_H; r++)
        for (int k = 0; k < GFX_W; k++) {
            uint16_t c = panel[r * GFX_W + k];
            if (round) {
                float dx = (float)k + 0.5f - GFX_CX, dy = (float)r + 0.5f - GFX_CY;
                float d2 = dx * dx + dy * dy;
                if (d2 > 233.0f * 233.0f) c = gfx_blend565(c, GFX_RGB(30, 30, 46), 200);
            }
            sheet[(y + r) * SHEET_W + x + k] = c;
        }
}

static void center(gfx_fb *fb, const font_t *f, int y, const char *t, uint16_t c)
{
    font_draw_centered(fb, f, GFX_W / 2, y, t, c);
}

/* greedy word wrap, centred lines; returns the y after the last line */
static int wrap(gfx_fb *fb, const font_t *f, int y, int maxw, const char *t, uint16_t c, int lead)
{
    char line[160] = "", word[64];
    const char *p = t;
    while (*p) {
        int n = 0;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && n < 63) word[n++] = *p++;
        word[n] = 0;
        if (!n) break;
        char trial[160];
        snprintf(trial, sizeof trial, "%s%s%s", line, line[0] ? " " : "", word);
        if (font_text_width(f, trial) > maxw && line[0]) {
            center(fb, f, y, line, c);
            y += lead;
            snprintf(line, sizeof line, "%s", word);
        } else {
            snprintf(line, sizeof line, "%s", trial);
        }
    }
    if (line[0]) { center(fb, f, y, line, c); y += lead; }
    return y;
}

static void mock_clock(void)
{
    gfx_fb fb = begin();
    center(&fb, &nf_label, 98, "MON 12 OCT", GREY);
    center(&fb, &nf_hero, 292, "10:09", WHITE);
    center(&fb, &nf_body, 350, "Partly cloudy  18\xB0", WHITE);
    font_draw_centered(&fb, &nf_small, GFX_W / 2, 400, "BAT 87%  \xB7  8,421 STEPS", GREY);
    dot_disc(&fb, GFX_W / 2.0f, 128, 8, RED);
    put(10, 10, 1);
}

static void mock_activity(void)
{
    gfx_fb fb = begin();
    dot_ring(&fb, GFX_CX, GFX_CY, 214, 72, 7, 0, 51.6f, WHITE, GFX_RGB(45, 45, 45));
    center(&fb, &nf_label, 128, "ACTIVITY", GREY);
    center(&fb, &nf_display, 236, "8,421", WHITE);
    center(&fb, &nf_label, 272, "STEPS  72% OF GOAL", GREY);
    center(&fb, &nf_title, 334, "412 kcal", WHITE);
    center(&fb, &nf_small, 366, "ACTIVE 48 MIN", GREY);
    put(10 + GFX_W + 10, 10, 1);
}

static void mock_notify(void)
{
    gfx_fb fb = begin();
    gfx_fill_rect(&fb, 58, 92, 350, 290, CARD);
    dot_icon(&fb, DOT_ICON_MSG, GFX_CX, 132, 3, 2.34f, WHITE);
    center(&fb, &nf_label, 186, "MESSAGES  \xB7  2 MIN", GREY);
    center(&fb, &nf_title, 228, "Maya Lindqvist", WHITE);
    wrap(&fb, &nf_body, 268, 300, "Are we still on for the climb tomorrow? Bring the spare rope.", WHITE, 32);
    gfx_fill_rect(&fb, 133, 392, 200, 44, GFX_RGB(40, 40, 40));
    center(&fb, &nf_label, 420, "REPLY", WHITE);
    put(10 + 2 * (GFX_W + 10), 10, 1);
}

static void mock_dotclock(void)
{
    gfx_fb fb = begin();
    dot_style s;
    memset(&s, 0, sizeof s);
    s.font = DOT_FONT_TALL; s.pitch = 13; s.dot = 13 * 0.78f; s.color = WHITE; s.mono = 1;
    center(&fb, &nf_label, 120, "TUE 13 OCT", GREY);
    dot_text_a(&fb, &s, GFX_CX, 160, "21:36", 1);
    center(&fb, &nf_body, 340, "Next: Standup  9:30", WHITE);
    center(&fb, &nf_small, 372, "HR 64  \xB7  SPO2 98%", GREY);
    put(10, 20 + GFX_H, 1);
}

static void charsets(void)
{
    gfx_fb fb = begin();
    int y = 30;
    struct { const font_t *f; const char *name; const char *t[3]; } rows[] = {
        { &nf_title,  "NF_TITLE  SPACE GROTESK MEDIUM 32", { "ABCDEFGHIJKLM abcdefghijklm", "NOPQRSTUVWXYZ nopqrstuvwxyz", "" } },
        { &nf_body,   "NF_BODY  SPACE GROTESK 26", { "The quick brown fox jumps over", "the lazy dog. 0123456789 !?&@#", "(a) [b] {c} 18\xB0 3/4 +5% $9.99" } },
        { &nf_label,  "NF_LABEL  SPACE MONO 18 +1PX", { "ABCDEFGHIJKLMNOPQRSTUVWXYZ", "0123456789 %:/-+.,\xB0 BPM KCAL", "abcdefghijklmnopqrstuvwxyz" } },
        { &nf_small,  "NF_SMALL  SPACE MONO 15 +1PX", { "ABCDEFGHIJKLMNOPQRSTUVWXYZ 0123456789", "BAT 87%  12:45  2.4 KM  SPO2 98%", "" } },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        font_draw(&fb, &nf_small, 16, y, rows[i].name, RED);
        y += 10 + rows[i].f->ascent;
        for (int k = 0; k < 3; k++) {
            if (!rows[i].t[k][0]) continue;
            font_draw(&fb, rows[i].f, 16, y, rows[i].t[k], WHITE);
            y += rows[i].f->line_height - (rows[i].f->line_height > 30 ? 8 : 4);
        }
        y += 8;
    }
    put(10 + GFX_W + 10, 20 + GFX_H, 0);

    fb = begin();
    y = 24;
    font_draw(&fb, &nf_small, 16, y, "NF_DISPLAY  SPACE GROTESK MEDIUM 64", RED);
    y += 8 + nf_display.ascent - 10;
    font_draw(&fb, &nf_display, 16, y, "0123456789", WHITE); y += 62;
    font_draw(&fb, &nf_display, 16, y, "ABCDEFGHIJ", WHITE); y += 62;
    font_draw(&fb, &nf_display, 16, y, "Rag 72% 18\xB0", WHITE); y += 34;
    font_draw(&fb, &nf_small, 16, y, "NF_HERO  SPACE GROTESK 160 (DIGITS + :)", RED);
    y += nf_hero.ascent - 34;
    font_draw(&fb, &nf_hero, 6, y, "0123", WHITE);
    put(10 + 2 * (GFX_W + 10), 20 + GFX_H, 0);
}

int main(void)
{
    for (int i = 0; i < SHEET_W * SHEET_H; i++) sheet[i] = GFX_RGB(12, 12, 12);
    mock_clock();
    mock_activity();
    mock_notify();
    mock_dotclock();
    charsets();
    if (png_write_565("build/nos/type_specimen.png", SHEET_W, SHEET_H, sheet, SHEET_W)) {
        printf("PNG write failed\n");
        return 1;
    }
    const font_t *all[] = { &nf_hero, &nf_display, &nf_title, &nf_body, &nf_label, &nf_small };
    const char *nm[] = { "hero", "display", "title", "body", "label", "small" };
    for (int i = 0; i < 6; i++)
        printf("%-8s line %3d ascent %3d glyphs %2d  width('10:09')=%d\n", nm[i], all[i]->line_height,
               all[i]->ascent, all[i]->nglyphs, font_text_width(all[i], "10:09"));
    printf("wrote build/nos/type_specimen.png\n");
    return 0;
}
