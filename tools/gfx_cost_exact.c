/* gfx_cost_exact.c — proves the GFX_COST instrumentation never changes pixels.
 *
 * Renders thousands of random primitives (every instrumented gfx/font/sprite call,
 * with random clips, translations and 16-row stripes) and writes the raw frames to
 * a file. Build it twice — against the instrumented sources with -DGFX_COST_MODEL
 * and against pristine copies (or without the define) — and compare the outputs:
 *
 *   gcc -O2 -Icore tools/gfx_cost_exact.c core/gfx.c core/font.c core/sprite.c core/fonts/ALL.c -DGFX_COST_MODEL -o a.exe
 *   gcc -O2 -Icore tools/gfx_cost_exact.c ORIG/gfx.c ORIG/font.c ORIG/sprite.c core/fonts/ALL.c -o b.exe
 *   a.exe out_a.bin && b.exe out_b.bin && cmp out_a.bin out_b.bin
 *
 * (ALL.c = every file in core/fonts.) nos_build.ps1 -Verify does this against
 * `git show HEAD:core/...`. With -DGFX_COST_MODEL, `a.exe --accuracy` also prints
 * counted pixels vs pixels that actually changed for the gfx_bench primitives. */
#include "gfx.h"
#include "font.h"
#include "sprite.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef GFX_COST_MODEL
static unsigned long long s_px, s_calls;
void gfx_cost_add(int cls, unsigned px, int calls) { (void)cls; s_px += px; s_calls += (unsigned)calls; }
#endif
void *hal_big_alloc(uint32_t bytes) { return malloc(bytes); }

static uint32_t s_rng = 12345;
static uint32_t rnd(void) { s_rng = s_rng * 1664525u + 1013904223u; return s_rng >> 8; }
static float frand(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() & 0xFFFF) / 65535.0f; }
static int irand(int lo, int hi) { return lo + (int)(rnd() % (unsigned)(hi - lo + 1)); }

static uint16_t s_frame[GFX_W * GFX_H];
static uint16_t s_stripe[GFX_W * 16];
static sprite s_spr;
static sprite8 s_spr8;
static uint16_t s_lut[256];

typedef struct { int kind; float f[8]; int i[4]; uint16_t c; } prim;

static void draw_prim(gfx_fb *fb, const prim *p)
{
    static const font_t *fonts[] = { &font_small, &font_medium, &font_large, &font_big };
    static const char *texts[] = { "Hello 42", "12:34", "NOS", "Quick fox", "09" };
    switch (p->kind) {
    case 0: gfx_fill_rect(fb, p->i[0], p->i[1], p->i[2], p->i[3], p->c); break;
    case 1: gfx_fill_circle(fb, p->f[0], p->f[1], p->f[2], p->c); break;
    case 2: gfx_fill_pill(fb, p->f[0], p->f[1], p->f[3], p->f[2] * 0.4f, p->c); break;
    case 3: gfx_ring(fb, p->f[0], p->f[1], p->f[2], p->f[4], p->c); break;
    case 4: gfx_arc(fb, p->f[0], p->f[1], p->f[2], p->f[4], p->f[5], p->f[5] + p->f[6], p->c); break;
    case 5: gfx_line(fb, p->f[0], p->f[1], p->f[3], p->f[7], p->f[4], p->c); break;
    case 6: gfx_taper(fb, p->f[0], p->f[1], p->f[3], p->f[7], p->f[4], p->f[4] * 0.3f, p->c); break;
    case 7: gfx_sweep(fb, p->f[0], p->f[1], p->f[2], p->f[4], p->f[6], 0.3f, p->c); break;
    case 8: gfx_px_blend(fb, p->i[0], p->i[1], p->c, (uint8_t)p->i[2]); break;
    case 9: font_draw(fb, fonts[p->i[2] & 3], p->i[0], p->i[1], texts[p->i[3] % 5], p->c); break;
    case 10: sprite_blit(fb, &s_spr, p->i[0] - 60, p->i[1] - 40, 1); break;
    case 11: sprite8_blit(fb, &s_spr8, p->i[0] - 60, p->i[1] - 40, s_lut, 1); break;
    case 12: gfx_line(fb, p->f[0], p->f[1], p->f[0], p->f[1], p->f[4], p->c); break;   /* degenerate */
    }
}

#ifdef GFX_COST_MODEL
static void accuracy(void)
{
    static const char *names[] = { "fill_rect", "circle", "pill", "ring", "arc", "line", "text_med", "digits_big", "blit" };
    for (int k = 0; k < 9; k++) {
        const uint16_t sentinel = 0x0841;
        unsigned long long px0 = s_px, c0 = s_calls;
        for (int i = 0; i < GFX_W * GFX_H; i++) s_frame[i] = sentinel;
        for (int y = 0; y < GFX_H; y += 16) {
            int h = GFX_H - y < 16 ? GFX_H - y : 16;
            memcpy(s_stripe, &s_frame[y * GFX_W], (size_t)h * GFX_W * 2);
            gfx_fb fb = { s_stripe, 0, 0, y, h, 0, GFX_W, 0, 0, 0 };
            uint16_t C = GFX_RGB(255, 140, 40);
            switch (k) {
            case 0: gfx_fill_rect(&fb, 83, 83, 300, 300, C); break;
            case 1: gfx_fill_circle(&fb, GFX_CX, GFX_CY, 120, C); break;
            case 2: gfx_fill_pill(&fb, 120, GFX_CY, 346, 34, C); break;
            case 3: gfx_ring(&fb, GFX_CX, GFX_CY, 180, 14, C); break;
            case 4: gfx_arc(&fb, GFX_CX, GFX_CY, 180, 14, 0.3f, 2.8f, C); break;
            case 5: gfx_line(&fb, 110, 110, 356, 356, 8, C); break;
            case 6: font_draw(&fb, &font_medium, 70, 245, "Quick brown fox", C); break;
            case 7: font_draw_centered(&fb, &font_big, (int)GFX_CX, 270, "12:34", C); break;
            case 8: sprite_blit(&fb, &s_spr, 173, 193, 1); break;
            }
            memcpy(&s_frame[y * GFX_W], s_stripe, (size_t)h * GFX_W * 2);
        }
        unsigned changed = 0;
        for (int i = 0; i < GFX_W * GFX_H; i++) changed += s_frame[i] != sentinel;
        printf("%-11s counted=%7llu changed=%7u calls=%llu\n", names[k], s_px - px0, changed, s_calls - c0);
    }
}
#endif

int main(int argc, char **argv)
{
#ifdef GFX_COST_MODEL
    if (argc > 1 && !strcmp(argv[1], "--accuracy")) {
        sprite_alloc(&s_spr, 120, 80);
        for (int i = 0; i < 120 * 80; i++) s_spr.px[i] = (uint16_t)(i * 2654435761u >> 13) | 1;
        accuracy();
        return 0;
    }
#endif
    if (argc < 2) { fprintf(stderr, "usage: %s OUT.bin\n", argv[0]); return 2; }
    FILE *out = fopen(argv[1], "wb");
    if (!out) return 1;
    sprite_alloc(&s_spr, 120, 80);
    sprite8_alloc(&s_spr8, 120, 80);
    for (int i = 0; i < 120 * 80; i++) { s_spr.px[i] = (uint16_t)(i * 2654435761u >> 13); s_spr8.px[i] = (uint8_t)(i * 37); }
    sprite8_lut_identity(s_lut);
    prim ps[24];
    for (int scene = 0; scene < 400; scene++) {
        int n = irand(1, 24);
        for (int k = 0; k < n; k++) {
            prim *p = &ps[k];
            p->kind = irand(0, 12);
            p->f[0] = frand(-40, GFX_W + 40); p->f[1] = frand(-40, GFX_H + 40);
            p->f[2] = frand(0.5f, 200); p->f[3] = frand(-40, GFX_W + 40);
            p->f[4] = frand(0.5f, 30); p->f[5] = frand(-7, 7); p->f[6] = frand(0, 6.6f);
            p->f[7] = frand(-40, GFX_H + 40);
            p->i[0] = irand(-60, GFX_W + 20); p->i[1] = irand(-60, GFX_H + 20);
            p->i[2] = irand(0, 255); p->i[3] = irand(0, 300);
            p->c = (uint16_t)rnd();
        }
        int ox = irand(0, 3) ? 0 : irand(-200, 200), oy = irand(0, 3) ? 0 : irand(-200, 200);
        int cx0 = 0, cx1 = GFX_W;
        if (irand(0, 1)) { cx0 = irand(0, GFX_W - 2) & ~1; cx1 = cx0 + 2 + (irand(0, GFX_W - cx0 - 2) & ~1); }
        int mode = irand(0, 2);      /* 0 full frame, 1 16-row stripes, 2 stripes + clear first */
        for (int i = 0; i < GFX_W * GFX_H; i++) s_frame[i] = (uint16_t)(i * 7 + scene);
        if (mode == 0) {
            gfx_fb fb = { s_frame, ox, oy, 0, GFX_H, cx0, cx1, 0, 0, 0 };
            if (irand(0, 1)) gfx_clear(&fb, (uint16_t)rnd());
            for (int k = 0; k < n; k++) draw_prim(&fb, &ps[k]);
        } else {
            uint16_t bg = (uint16_t)rnd();
            for (int y = 0; y < GFX_H; y += 16) {
                int h = GFX_H - y < 16 ? GFX_H - y : 16;
                memcpy(s_stripe, &s_frame[y * GFX_W], (size_t)h * GFX_W * 2);
                gfx_fb fb = { s_stripe, ox, oy, y, h, cx0, cx1, 0, 0, 0 };
                if (mode == 2) gfx_clear(&fb, bg);
                for (int k = 0; k < n; k++) draw_prim(&fb, &ps[k]);
                memcpy(&s_frame[y * GFX_W], s_stripe, (size_t)h * GFX_W * 2);
            }
        }
        fwrite(s_frame, sizeof s_frame, 1, out);
    }
    fclose(out);
#ifdef GFX_COST_MODEL
    printf("cost model: %llu px, %llu calls counted\n", s_px, s_calls);
#endif
    return 0;
}
